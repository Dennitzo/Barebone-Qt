#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "BimTools.h"

#include "arxHeaders.h"
#include "AcCm/AcCmColor.h"
#include "BrxSpecific/BrxGenericPropertiesAccess.h"
#include "BrxSpecific/bim/AnchorFeature.h"
#include "BrxSpecific/bim/BuildingElements.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace Barebone::Brx::BimTools {
namespace {

constexpr double kTolerance = 1.0e-9;

std::string wideToUtf8(const wchar_t* value)
{
    if (value == nullptr || *value == L'\0') {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string nativeToUtf8(const ACHAR* value)
{
#ifdef UNICODE
    return wideToUtf8(value);
#else
    return value == nullptr ? std::string() : std::string(value);
#endif
}

std::basic_string<ACHAR> utf8ToNative(const std::string& value)
{
#ifdef UNICODE
    return utf8ToWide(value);
#else
    return value;
#endif
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

std::string jsonStringOrNull(const std::string& value)
{
    return value.empty() ? "null" : std::string("\"") + jsonEscape(value) + "\"";
}

std::string upperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string trim(std::string value)
{
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string normalizedHandle(const std::string& value)
{
    return upperAscii(trim(value));
}

std::string handleText(const AcDbObjectId& id)
{
    if (id.isNull()) {
        return {};
    }
    ACHAR buffer[AcDbHandle::kStrSiz]{};
    return id.handle().getIntoAsciiBuffer(buffer) ? nativeToUtf8(buffer) : std::string();
}

std::string errorStatusText(Acad::ErrorStatus status)
{
    std::ostringstream out;
    out << nativeToUtf8(acadErrorStatusText(status)) << " (" << static_cast<int>(status) << ')';
    return out.str();
}

std::string bimStatusText(BimApi::ResultStatus status)
{
    return "BimApi status " + std::to_string(static_cast<int>(status));
}

void addError(std::vector<TargetError>& errors, std::string target, std::string code, std::string message)
{
    errors.push_back(TargetError{std::move(target), std::move(code), std::move(message)});
}

bool finitePoint(const AcGePoint3d& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool finiteVector(const AcGeVector3d& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

bool objectIdFromHandle(AcDbDatabase* database, const std::string& text, AcDbObjectId& id)
{
    id.setNull();
    const std::string value = normalizedHandle(text);
    if (database == nullptr || value.empty()
        || !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
        return false;
    }
    const std::basic_string<ACHAR> native = utf8ToNative(value);
    const AcDbHandle handle(native.c_str());
    return !handle.isNull()
        && database->getAcDbObjectId(id, false, handle) == Acad::eOk
        && !id.isNull()
        && id.database() == database
        && id.isValid()
        && !id.isEffectivelyErased();
}

std::string classificationOf(const AcDbObjectId& id)
{
    AcString typeName;
    return BimClassification::getClassification(typeName, id, false) == BimApi::eOk
        ? nativeToUtf8(typeName.constPtr())
        : std::string();
}

Fingerprint fingerprintOf(const AcDbObjectId& id)
{
    Fingerprint value;
    value.handle = handleText(id);
    value.guid = nativeToUtf8(BimClassification::getGUID(id));
    value.bimType = classificationOf(id);
    return value;
}

std::string classificationKey(std::string value)
{
    value = upperAscii(trim(std::move(value)));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0 || ch == '_' || ch == '-';
    }), value.end());
    if (value.rfind("BIM", 0) == 0) {
        value.erase(0, 3);
    }
    return value;
}

std::vector<std::string> canonicalClassifications(
    const std::vector<std::string>& requested,
    std::vector<TargetError>& errors)
{
    if (requested.empty()) {
        return {};
    }

    AcStringArray nativeNames;
    const BimApi::ResultStatus status = BimClassification::getBimTypeNames(nativeNames, false);
    if (status != BimApi::eOk) {
        addError(errors, "classification", "classificationCatalogUnavailable", bimStatusText(status));
        return {};
    }

    std::vector<std::string> available;
    available.reserve(static_cast<std::size_t>(nativeNames.length()));
    for (int i = 0; i < nativeNames.length(); ++i) {
        available.push_back(nativeToUtf8(nativeNames.at(i).constPtr()));
    }

    std::vector<std::string> result;
    for (const std::string& input : requested) {
        const std::string wanted = trim(input);
        auto exact = std::find(available.begin(), available.end(), wanted);
        if (exact == available.end()) {
            exact = std::find_if(available.begin(), available.end(), [&](const std::string& candidate) {
                return classificationKey(candidate) == classificationKey(wanted);
            });
        }
        if (exact == available.end()) {
            addError(errors, input, "invalidClassification", "Unbekannte BRX-BIM-Klassifikation '" + input + "'");
            continue;
        }
        if (std::find(result.begin(), result.end(), *exact) == result.end()) {
            result.push_back(*exact);
        }
    }
    return result;
}

bool classificationAllowed(const std::string& value, const std::vector<std::string>& allowed)
{
    return allowed.empty() || std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

AcDbObjectIdArray pickfirstIds(std::vector<std::string>& debug)
{
    AcDbObjectIdArray result;
    ads_name selectionSet{};
    const int getStatus = acedSSGet(_T("_I"), nullptr, nullptr, nullptr, selectionSet);
    debug.push_back("acedSSGet(_I) status=" + std::to_string(getStatus));
    if (getStatus != RTNORM) {
        return result;
    }

    Adesk::Int32 count = 0;
    if (acedSSLength(selectionSet, &count) == RTNORM) {
        for (Adesk::Int32 i = 0; i < count; ++i) {
            ads_name entityName{};
            AcDbObjectId id;
            if (acedSSName(selectionSet, static_cast<int>(i), entityName) == RTNORM
                && acdbGetObjectId(id, entityName) == Acad::eOk
                && !id.isNull()) {
                result.append(id);
            }
        }
    }
    acedSSFree(selectionSet);
    return result;
}

std::string entityTypeName(const AcDbEntity* entity)
{
    return entity != nullptr && entity->isA() != nullptr
        ? nativeToUtf8(entity->isA()->name())
        : std::string("AcDbEntity");
}

std::string entityLayerName(const AcDbEntity* entity)
{
    if (entity == nullptr || entity->layerId().isNull()) {
        return {};
    }
    AcDbLayerTableRecord* layer = nullptr;
    if (acdbOpenObject(layer, entity->layerId(), AcDb::kForRead) != Acad::eOk || layer == nullptr) {
        return {};
    }
    const ACHAR* name = nullptr;
    const std::string result = layer->getName(name) == Acad::eOk ? nativeToUtf8(name) : std::string();
    layer->close();
    return result;
}

std::string blockReferenceName(const AcDbEntity* entity)
{
    const AcDbBlockReference* reference = AcDbBlockReference::cast(entity);
    if (reference == nullptr) {
        return {};
    }
    AcDbBlockTableRecord* record = nullptr;
    if (acdbOpenObject(record, reference->blockTableRecord(), AcDb::kForRead) != Acad::eOk || record == nullptr) {
        return {};
    }
    const ACHAR* name = nullptr;
    const std::string result = record->getName(name) == Acad::eOk ? nativeToUtf8(name) : std::string();
    record->close();
    return result;
}

std::string valueDataTypeName(AcValue::DataType type, bool isBool)
{
    if (isBool) return "boolean";
    switch (type) {
    case AcValue::kLong: return "integer";
    case AcValue::kDouble: return "number";
    case AcValue::kString: return "string";
    case AcValue::kDate: return "date";
    case AcValue::kPoint: return "point2d";
    case AcValue::k3dPoint: return "point3d";
    case AcValue::kObjectId: return "objectId";
    case AcValue::kBuffer: return "buffer";
    case AcValue::kResbuf: return "resbuf";
    case AcValue::kGeneral: return "general";
    case AcValue::kColor: return "color";
    default: return "unknown";
    }
}

std::string valueUnitTypeName(AcValue::UnitType type)
{
    switch (type) {
    case AcValue::kUnitless: return "unitless";
    case AcValue::kDistance: return "distance";
    case AcValue::kAngle: return "angle";
    case AcValue::kArea: return "area";
    case AcValue::kVolume: return "volume";
    case AcValue::kCurrency: return "currency";
    case AcValue::kPercentage: return "percentage";
    case AcValue::kAngleNotTransformed: return "angleNotTransformed";
    default: return "unknown";
    }
}

std::string valueJson(const AcValue& value)
{
    if (value.isBool()) {
        bool booleanValue = false;
        return value.get(booleanValue) ? (booleanValue ? "true" : "false") : "null";
    }
    switch (value.dataType()) {
    case AcValue::kLong: {
        Adesk::Int32 integerValue = 0;
        return value.get(integerValue) ? std::to_string(integerValue) : "null";
    }
    case AcValue::kDouble: {
        double numberValue = 0.0;
        if (!value.get(numberValue) || !std::isfinite(numberValue)) return "null";
        std::ostringstream out;
        out << std::setprecision(17) << numberValue;
        return out.str();
    }
    case AcValue::kString:
    case AcValue::kGeneral: {
        AcString stringValue;
        return value.get(stringValue) ? jsonStringOrNull(nativeToUtf8(stringValue.constPtr())) : "null";
    }
    case AcValue::kDate: {
        __time64_t dateValue = 0;
        std::tm date{};
        if (!value.get(dateValue) || _gmtime64_s(&date, &dateValue) != 0) return "null";
        std::ostringstream out;
        out << '"' << std::setfill('0') << std::setw(4) << date.tm_year + 1900 << '-'
            << std::setw(2) << date.tm_mon + 1 << '-' << std::setw(2) << date.tm_mday << 'T'
            << std::setw(2) << date.tm_hour << ':' << std::setw(2) << date.tm_min << ':'
            << std::setw(2) << date.tm_sec << "Z\"";
        return out.str();
    }
    case AcValue::kPoint: {
        AcGePoint2d point;
        if (!value.get(point)) return "null";
        std::ostringstream out;
        out << '[' << point.x << ',' << point.y << ']';
        return out.str();
    }
    case AcValue::k3dPoint: {
        AcGePoint3d point;
        if (!value.get(point)) return "null";
        std::ostringstream out;
        out << '[' << point.x << ',' << point.y << ',' << point.z << ']';
        return out.str();
    }
    case AcValue::kObjectId: {
        AcDbObjectId objectId;
        return value.get(objectId) ? jsonStringOrNull(handleText(objectId)) : "null";
    }
    case AcValue::kColor: {
        AcCmColor color;
        if (!value.get(color)) return "null";
        std::ostringstream out;
        out << "{\"method\":" << static_cast<int>(color.colorMethod())
            << ",\"index\":" << color.colorIndex()
            << ",\"red\":" << static_cast<int>(color.red())
            << ",\"green\":" << static_cast<int>(color.green())
            << ",\"blue\":" << static_cast<int>(color.blue()) << '}';
        return out.str();
    }
    case AcValue::kBuffer: {
        VARIANT nativeValue;
        VariantInit(&nativeValue);
        if (!value.get(nativeValue)
            || (V_VT(&nativeValue) & VT_ARRAY) == 0
            || (V_VT(&nativeValue) & VT_TYPEMASK) != VT_UI1
            || V_ARRAY(&nativeValue) == nullptr
            || SafeArrayGetDim(V_ARRAY(&nativeValue)) != 1) {
            VariantClear(&nativeValue);
            return "null";
        }
        LONG lower = 0;
        LONG upper = -1;
        if (SafeArrayGetLBound(V_ARRAY(&nativeValue), 1, &lower) != S_OK
            || SafeArrayGetUBound(V_ARRAY(&nativeValue), 1, &upper) != S_OK) {
            VariantClear(&nativeValue);
            return "null";
        }
        void* buffer = nullptr;
        if (SafeArrayAccessData(V_ARRAY(&nativeValue), &buffer) != S_OK) {
            VariantClear(&nativeValue);
            return "null";
        }
        const std::size_t size = upper >= lower ? static_cast<std::size_t>(upper - lower + 1) : 0;
        const auto* bytes = static_cast<const unsigned char*>(buffer);
        std::ostringstream out;
        out << "{\"encoding\":\"hex\",\"size\":" << size << ",\"data\":\""
            << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < size; ++i) out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        out << "\"}";
        SafeArrayUnaccessData(V_ARRAY(&nativeValue));
        VariantClear(&nativeValue);
        return out.str();
    }
    default:
        return "null";
    }
}

std::vector<Property> readProperties(const AcDbObjectId& id, std::string& componentType, bool includeAll)
{
    std::vector<Property> result;
    AcStringArray propertyNames;
    if (!BrxDbProperties::listAll(id, propertyNames)) {
        return result;
    }

    result.reserve(static_cast<std::size_t>(propertyNames.length()));
    for (int i = 0; i < propertyNames.length(); ++i) {
        const AcString& nativeName = propertyNames.at(i);
        const std::string qualifiedName = nativeToUtf8(nativeName.constPtr());
        std::string unqualified = qualifiedName;
        const std::size_t separator = unqualified.find('~');
        if (separator != std::string::npos) unqualified.resize(separator);
        std::string propertyKey = upperAscii(unqualified);
        propertyKey.erase(std::remove_if(propertyKey.begin(), propertyKey.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0 || ch == '_' || ch == '-';
        }), propertyKey.end());
        const bool componentTypeProperty = propertyKey == "COMPONENTTYPE";
        if (!includeAll && !componentTypeProperty) continue;

        AcValue value;
        if (!BrxDbProperties::getValue(id, nativeName.constPtr(), value)) {
            continue;
        }

        Property property;
        property.qualifiedName = qualifiedName;
        property.dataType = valueDataTypeName(value.dataType(), value.isBool());
        property.unitType = valueUnitTypeName(value.unitType());
        AcString formatted;
        if (value.format(formatted)) {
            property.formattedValue = nativeToUtf8(formatted.constPtr());
        }
        property.valueJson = valueJson(value);
        if (property.valueJson == "null" && !property.formattedValue.empty()) {
            property.valueJson = jsonStringOrNull(property.formattedValue);
        }
        if (includeAll) {
            AcString readOnlyName(nativeName);
            property.readOnlyKnown = BrxDbProperties::isReadOnly(id, readOnlyName, property.readOnly);
        }

        if (componentTypeProperty) {
            if (!property.formattedValue.empty()) {
                componentType = property.formattedValue;
            } else if (property.valueJson.size() >= 2 && property.valueJson.front() == '"' && property.valueJson.back() == '"') {
                componentType = property.valueJson.substr(1, property.valueJson.size() - 2);
            }
        }
        if (includeAll) result.push_back(std::move(property));
    }
    return result;
}

bool populateObject(
    const AcDbObjectId& id,
    const AcDbEntity* entity,
    bool includeGeometry,
    bool includeProperties,
    ObjectData& object,
    std::string& error)
{
    object = ObjectData{};
    object.id = id;
    object.handle = handleText(id);

    AcString typeName;
    const BimApi::ResultStatus classificationStatus = BimClassification::getClassification(typeName, id, false);
    if (classificationStatus != BimApi::eOk) {
        error = "Objekt ist nicht BIM-klassifiziert: " + bimStatusText(classificationStatus);
        return false;
    }

    object.bimType = nativeToUtf8(typeName.constPtr());
    object.name = nativeToUtf8(BimClassification::getName(id));
    object.description = nativeToUtf8(BimClassification::getDescription(id));
    object.guid = nativeToUtf8(BimClassification::getGUID(id));
    object.entityType = entityTypeName(entity);
    object.layer = entityLayerName(entity);
    object.blockName = blockReferenceName(entity);

    if (includeGeometry && entity != nullptr) {
        AcDbExtents extents;
        if (entity->getGeomExtents(extents) == Acad::eOk) {
            object.bounds.valid = true;
            object.bounds.minPoint = extents.minPoint();
            object.bounds.maxPoint = extents.maxPoint();
        }
    }

    std::vector<Property> properties = readProperties(id, object.componentType, includeProperties);
    if (includeProperties) {
        object.properties = std::move(properties);
    }
    return true;
}

bool isEntityOnLockedLayer(const AcDbEntity* entity)
{
    if (entity == nullptr || entity->layerId().isNull()) return false;
    AcDbLayerTableRecord* layer = nullptr;
    if (acdbOpenObject(layer, entity->layerId(), AcDb::kForRead) != Acad::eOk || layer == nullptr) return true;
    const bool locked = layer->isLocked();
    layer->close();
    return locked;
}

bool isEntityFromXref(const AcDbEntity* entity)
{
    if (entity == nullptr || entity->ownerId().isNull()) return false;
    AcDbBlockTableRecord* owner = nullptr;
    if (acdbOpenObject(owner, entity->ownerId(), AcDb::kForRead) != Acad::eOk || owner == nullptr) return true;
    bool xref = owner->isFromExternalReference() || owner->isFromOverlayReference();
    owner->close();

    const AcDbBlockReference* reference = AcDbBlockReference::cast(entity);
    if (!xref && reference != nullptr && !reference->blockTableRecord().isNull()) {
        AcDbBlockTableRecord* definition = nullptr;
        if (acdbOpenObject(definition, reference->blockTableRecord(), AcDb::kForRead) != Acad::eOk || definition == nullptr) {
            return true;
        }
        xref = definition->isFromExternalReference() || definition->isFromOverlayReference();
        definition->close();
    }

    if (!xref && !entity->layerId().isNull()) {
        AcDbLayerTableRecord* layer = nullptr;
        if (acdbOpenObject(layer, entity->layerId(), AcDb::kForRead) != Acad::eOk || layer == nullptr) {
            return true;
        }
        xref = layer->isDependent();
        layer->close();
    }
    return xref;
}

bool isAnchoredOrAnchorHost(AcDbDatabase* database, const AcDbObjectId& id, std::string& reason)
{
    if (BimApi::isAnchoredBlockRef(id)) {
        reason = "anchoredBlockReference";
        return true;
    }

    AcArray<AcDbObjectId> anchoredIds;
    if (BimApi::getAnchoredBlockReferences(database, anchoredIds) != Acad::eOk) {
        reason = "anchorStateUnavailable";
        return true;
    }
    for (int i = 0; i < anchoredIds.length(); ++i) {
        AcDbFullSubentPath hostFace;
        if (BimApi::getAnchorFace(anchoredIds.at(i), hostFace) != Acad::eOk) {
            reason = "anchorStateUnavailable";
            return true;
        }
        const AcDbObjectIdArray& pathIds = hostFace.objectIds();
        for (int pathIndex = 0; pathIndex < pathIds.length(); ++pathIndex) {
            if (pathIds.at(pathIndex) == id) {
                reason = "hostsAnchoredElement";
                return true;
            }
        }
    }
    return false;
}

bool inputScale(AcDbDatabase* database, const std::string& requestedUnits, double& scale, std::string& error)
{
    const std::string units = upperAscii(trim(requestedUnits));
    if (units == "DRAWING") {
        scale = 1.0;
        return true;
    }
    if (!units.empty() && units != "MM" && units != "MILLIMETER" && units != "MILLIMETERS") {
        error = "units muss 'mm' oder 'drawing' sein";
        return false;
    }
    if (database == nullptr || database->insunits() == AcDb::kUnitsUndefined) {
        error = "INSUNITS ist undefiniert; fuer mm-Eingaben muss die Zeichnungseinheit gesetzt oder units='drawing' verwendet werden";
        return false;
    }
    const Acad::ErrorStatus status = acdbGetUnitsConversion(AcDb::kUnitsMillimeters, database->insunits(), scale);
    if (status != Acad::eOk || !std::isfinite(scale) || scale <= 0.0) {
        error = "Millimeter konnten nicht in Zeichnungseinheiten konvertiert werden: " + errorStatusText(status);
        return false;
    }
    return true;
}

AcGePoint3d boundsCenter(const Bounds& bounds)
{
    return AcGePoint3d(
        (bounds.minPoint.x + bounds.maxPoint.x) * 0.5,
        (bounds.minPoint.y + bounds.maxPoint.y) * 0.5,
        (bounds.minPoint.z + bounds.maxPoint.z) * 0.5);
}

std::string targetErrorsJson(const std::vector<TargetError>& errors)
{
    std::ostringstream json;
    json << '[';
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i > 0) json << ',';
        json << "{\"target\":" << jsonStringOrNull(errors[i].target)
            << ",\"code\":\"" << jsonEscape(errors[i].code) << "\""
            << ",\"message\":\"" << jsonEscape(errors[i].message) << "\"}";
    }
    json << ']';
    return json.str();
}

std::string handlesJson(const AcDbObjectIdArray& ids)
{
    std::ostringstream json;
    json << '[';
    for (int i = 0; i < ids.length(); ++i) {
        if (i > 0) json << ',';
        json << '"' << jsonEscape(handleText(ids.at(i))) << '"';
    }
    json << ']';
    return json.str();
}

std::string propertiesJson(const std::vector<Property>& properties)
{
    std::ostringstream json;
    json << '[';
    for (std::size_t i = 0; i < properties.size(); ++i) {
        if (i > 0) json << ',';
        const Property& property = properties[i];
            json << "{\"qualifiedName\":\"" << jsonEscape(property.qualifiedName) << "\""
                << ",\"dataType\":\"" << jsonEscape(property.dataType) << "\""
                << ",\"unitType\":\"" << jsonEscape(property.unitType) << "\""
                << ",\"value\":" << property.valueJson
                << ",\"formattedValue\":" << jsonStringOrNull(property.formattedValue)
                << ",\"readOnly\":";
            if (property.readOnlyKnown) json << (property.readOnly ? "true" : "false");
            else json << "null";
            json << '}';
    }
    json << ']';
    return json.str();
}

std::string objectsJson(const std::vector<ObjectData>& objects, bool includeProperties)
{
    std::ostringstream json;
    json << '[';
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (i > 0) json << ',';
        json << objectJson(objects[i], includeProperties);
    }
    json << ']';
    return json.str();
}

} // namespace

Availability availability()
{
    Availability value;
    value.apiAvailable = BimApi::isBimAvailable();
    value.licenseAvailable = isLicenseAvailable(BricsCAD::eBim);
    value.available = value.apiAvailable && value.licenseAvailable;
    if (!value.apiAvailable && !value.licenseAvailable) {
        value.reason = "BRX BIM API und BIM-Lizenz sind nicht verfuegbar (RUNASLEVEL/Lizenz pruefen)";
    } else if (!value.apiAvailable) {
        value.reason = "BRX BIM API ist nicht verfuegbar (RUNASLEVEL 3 oder 5 pruefen)";
    } else if (!value.licenseAvailable) {
        value.reason = "BricsCAD BIM-Lizenz ist nicht verfuegbar";
    }
    return value;
}

std::string availabilityJson(const Availability& value)
{
    std::ostringstream json;
    json << "{\"available\":" << (value.available ? "true" : "false")
        << ",\"apiAvailable\":" << (value.apiAvailable ? "true" : "false")
        << ",\"licenseAvailable\":" << (value.licenseAvailable ? "true" : "false")
        << ",\"reason\":" << jsonStringOrNull(value.reason) << '}';
    return json.str();
}

ResolveResult resolve(AcDbDatabase* database, const Selector& selector, ResolvePurpose purpose)
{
    ResolveResult result;
    const Availability state = availability();
    if (!state.available) {
        addError(result.errors, "bim", "bimUnavailable", state.reason);
        return result;
    }
    if (database == nullptr) {
        addError(result.errors, "database", "noDatabase", "Keine aktive BricsCAD-Zeichnung gefunden");
        return result;
    }

    const std::vector<std::string> canonicalFilters = canonicalClassifications(selector.classifications, result.errors);
    if (!selector.classifications.empty() && canonicalFilters.empty()) {
        return result;
    }

    AcDbObjectIdArray candidates;
    if (selector.scope == SelectorScope::Handles) {
        if (selector.handles.empty()) {
            addError(result.errors, "handles", "emptySelector", "Der BIM-Selector enthaelt keine Handles");
        }
        for (const std::string& handle : selector.handles) {
            AcDbObjectId id;
            if (!objectIdFromHandle(database, handle, id)) {
                addError(result.errors, handle, "unknownHandle", "Unbekannter Datenbank-Handle '" + handle + "'");
                continue;
            }
            candidates.append(id);
        }
    } else if (selector.scope == SelectorScope::Selection) {
        candidates = pickfirstIds(result.debug);
    } else {
        AcDbObjectIdArray classified;
        const BimApi::ResultStatus status = BimClassification::getAllClassified(classified, database);
        result.debug.push_back("getAllClassified count=" + std::to_string(classified.length())
            + " status=" + std::to_string(static_cast<int>(status)));
        if (status != BimApi::eOk) {
            addError(result.errors, "database", "classificationQueryFailed", bimStatusText(status));
            return result;
        }

        if (selector.scope == SelectorScope::Names) {
            if (selector.names.empty()) {
                addError(result.errors, "names", "emptySelector", "Der BIM-Selector enthaelt keine Namen");
            }
            for (const std::string& requestedName : selector.names) {
                AcDbObjectIdArray matches;
                for (int i = 0; i < classified.length(); ++i) {
                    const AcDbObjectId id = classified.at(i);
                    if (requestedName == nativeToUtf8(BimClassification::getName(id))) {
                        matches.append(id);
                    }
                }
                if (matches.isEmpty()) {
                    addError(result.errors, requestedName, "unknownName", "Kein BIM-Objekt mit exakt diesem Namen gefunden");
                    continue;
                }
                if (matches.length() > 1
                    && (purpose == ResolvePurpose::DrawingMutation
                        || (purpose == ResolvePurpose::SelectionMutation && !selector.allMatches))) {
                    addError(result.errors, requestedName, "ambiguousName",
                        "Der BIM-Name ist mehrdeutig; explizite Handles verwenden"
                        + std::string(purpose == ResolvePurpose::SelectionMutation ? " oder allMatches=true setzen" : ""));
                    continue;
                }
                for (int i = 0; i < matches.length(); ++i) candidates.append(matches.at(i));
            }
        } else {
            const AcDbObjectId currentSpaceId = database->currentSpaceId();
            for (int i = 0; i < classified.length(); ++i) {
                AcDbEntity* entity = nullptr;
                if (acdbOpenObject(entity, classified.at(i), AcDb::kForRead) != Acad::eOk || entity == nullptr) {
                    continue;
                }
                const bool currentSpace = entity->ownerId() == currentSpaceId;
                entity->close();
                if (currentSpace) candidates.append(classified.at(i));
            }
        }
    }

    std::set<std::string> seenHandles;
    for (int i = 0; i < candidates.length(); ++i) {
        const AcDbObjectId id = candidates.at(i);
        const std::string handle = normalizedHandle(handleText(id));
        if (handle.empty() || !seenHandles.insert(handle).second) {
            continue;
        }
        if (!id.isValid() || id.isEffectivelyErased()) {
            addError(result.errors, handle, "erasedTarget", "BIM-Ziel ist geloescht oder nicht mehr gueltig");
            continue;
        }
        const std::string typeName = classificationOf(id);
        if (typeName.empty()) {
            addError(result.errors, handle, "notBimClassified", "Das Ziel ist kein klassifiziertes BIM-Objekt");
            continue;
        }
        if (!classificationAllowed(typeName, canonicalFilters)) {
            continue;
        }
        if (purpose == ResolvePurpose::DrawingMutation) {
            AcDbEntity* entity = nullptr;
            const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForRead);
            if (openStatus != Acad::eOk || entity == nullptr) {
                addError(result.errors, handle, "openFailed", errorStatusText(openStatus));
                continue;
            }
            const bool xref = isEntityFromXref(entity);
            const bool lockedLayer = isEntityOnLockedLayer(entity);
            entity->close();
            if (xref) {
                addError(result.errors, handle, "xrefTarget", "XRef-abhaengige BIM-Objekte koennen nicht transformiert werden");
                continue;
            }
            if (lockedLayer) {
                addError(result.errors, handle, "lockedLayer", "BIM-Objekt liegt auf einem gesperrten Layer");
                continue;
            }
            std::string anchorReason;
            if (isAnchoredOrAnchorHost(database, id, anchorReason)) {
                addError(result.errors, handle, "anchoredTarget",
                    "Geankerte BIM-Objekte oder Hosts mit geankerten Elementen werden in v1 sicher abgelehnt (" + anchorReason + ")");
                continue;
            }
        }
        result.ids.append(id);
        result.fingerprints.push_back(fingerprintOf(id));
    }

    if (purpose != ResolvePurpose::Query && result.ids.isEmpty() && result.errors.empty()) {
        addError(result.errors, "selector", "emptySelector", "Der BIM-Selector findet keine Objekte");
    }

    if (!selector.expectedResolvedHandles.empty()) {
        std::set<std::string> expected;
        std::set<std::string> actual;
        for (const std::string& handle : selector.expectedResolvedHandles) expected.insert(normalizedHandle(handle));
        for (const Fingerprint& fingerprint : result.fingerprints) actual.insert(normalizedHandle(fingerprint.handle));
        if (expected != actual) {
            addError(result.errors, "resolvedHandles", "targetSetChanged",
                "Die erneut aufgeloesten BIM-Ziele stimmen nicht mit actions.validate.resolvedHandles ueberein");
        }
    }

    for (const Fingerprint& expected : selector.expectedFingerprints) {
        const auto current = std::find_if(result.fingerprints.begin(), result.fingerprints.end(), [&](const Fingerprint& value) {
            return normalizedHandle(value.handle) == normalizedHandle(expected.handle);
        });
        if (current == result.fingerprints.end()
            || current->guid != expected.guid
            || current->bimType != expected.bimType) {
            addError(result.errors, expected.handle, "fingerprintChanged",
                "BIM-Zielfingerprint hat sich seit actions.validate geaendert");
        }
    }
    if (!selector.expectedResolvedHandles.empty() || !selector.expectedFingerprints.empty()) {
        std::set<std::string> expectedHandles;
        std::set<std::string> fingerprintHandles;
        for (const std::string& handle : selector.expectedResolvedHandles) {
            expectedHandles.insert(normalizedHandle(handle));
        }
        for (const Fingerprint& fingerprint : selector.expectedFingerprints) {
            fingerprintHandles.insert(normalizedHandle(fingerprint.handle));
        }
        if (expectedHandles != fingerprintHandles) {
            addError(result.errors, "targetFingerprints", "incompleteFingerprintSet",
                "targetFingerprints muessen exakt dieselben Handles wie resolvedHandles enthalten");
        }
    }

    result.success = result.errors.empty();
    return result;
}

bool readObject(const AcDbObjectId& id, bool includeGeometry, bool includeProperties, ObjectData& object, std::string& error)
{
    const Availability state = availability();
    if (!state.available) {
        error = state.reason;
        return false;
    }
    AcDbEntity* entity = nullptr;
    const Acad::ErrorStatus status = acdbOpenObject(entity, id, AcDb::kForRead);
    if (status != Acad::eOk || entity == nullptr) {
        error = "BIM-Entity konnte nicht gelesen werden: " + errorStatusText(status);
        return false;
    }
    const bool ok = populateObject(id, entity, includeGeometry, includeProperties, object, error);
    entity->close();
    return ok;
}

QueryResult query(AcDbDatabase* database, const QueryRequest& request)
{
    QueryResult result;
    result.availability = availability();
    result.offset = request.offset;
    result.limit = std::clamp<std::size_t>(request.limit, 1, 500);
    if (!result.availability.available) {
        addError(result.errors, "bim", "bimUnavailable", result.availability.reason);
        return result;
    }

    ResolveResult resolved = resolve(database, request.selector, ResolvePurpose::Query);
    result.debug = std::move(resolved.debug);
    result.errors = std::move(resolved.errors);
    result.total = static_cast<std::size_t>(resolved.ids.length());

    std::map<std::string, std::size_t> counts;
    for (int i = 0; i < resolved.ids.length(); ++i) {
        ++counts[classificationOf(resolved.ids.at(i))];
    }
    result.classifications.assign(counts.begin(), counts.end());

    const std::size_t begin = std::min(result.offset, result.total);
    const std::size_t end = std::min(result.total, begin + result.limit);
    result.truncated = begin > 0 || end < result.total;
    result.objects.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        ObjectData object;
        std::string error;
        const AcDbObjectId id = resolved.ids.at(static_cast<int>(i));
        if (!readObject(id, request.includeGeometry, request.includeProperties, object, error)) {
            addError(result.errors, handleText(id), "objectReadFailed", error);
            continue;
        }
        result.objects.push_back(std::move(object));
    }
    result.success = resolved.success && result.errors.empty();
    return result;
}

OperationResult setSelection(AcDbDatabase* database, const Selector& selector)
{
    OperationResult result;
    result.operation = "selection.set";
    result.availability = availability();
    if (!result.availability.available) {
        addError(result.errors, "bim", "bimUnavailable", result.availability.reason);
        return result;
    }

    ResolveResult resolved = resolve(database, selector, ResolvePurpose::SelectionMutation);
    result.debug = std::move(resolved.debug);
    result.errors = std::move(resolved.errors);
    result.fingerprints = std::move(resolved.fingerprints);
    if (!resolved.success) {
        return result;
    }

    ads_name selectionSet{};
    int status = acedSSAdd(nullptr, nullptr, selectionSet);
    result.debug.push_back("acedSSAdd create status=" + std::to_string(status));
    if (status != RTNORM) {
        addError(result.errors, "selection", "selectionSetCreateFailed", "Pickfirst-Auswahl konnte nicht erstellt werden");
        return result;
    }
    for (int i = 0; i < resolved.ids.length(); ++i) {
        ads_name entityName{};
        const Acad::ErrorStatus nameStatus = acdbGetAdsName(entityName, resolved.ids.at(i));
        if (nameStatus != Acad::eOk || acedSSAdd(entityName, selectionSet, selectionSet) != RTNORM) {
            addError(result.errors, handleText(resolved.ids.at(i)), "selectionAddFailed", "BIM-Objekt konnte der Pickfirst-Auswahl nicht hinzugefuegt werden");
        }
    }
    if (result.errors.empty()) {
        status = acedSSSetFirst(nullptr, selectionSet);
        result.debug.push_back("acedSSSetFirst status=" + std::to_string(status));
        if (status != RTNORM) {
            addError(result.errors, "selection", "selectionSetFirstFailed", "Pickfirst-Auswahl konnte nicht gesetzt werden");
        }
    }
    acedSSFree(selectionSet);
    if (result.errors.empty()) {
        result.affectedIds = resolved.ids;
        result.success = true;
    }
    return result;
}

std::string fingerprintsJson(const std::vector<Fingerprint>& fingerprints)
{
    std::ostringstream json;
    json << '[';
    for (std::size_t i = 0; i < fingerprints.size(); ++i) {
        if (i > 0) json << ',';
        json << "{\"handle\":\"" << jsonEscape(fingerprints[i].handle) << "\""
            << ",\"guid\":" << jsonStringOrNull(fingerprints[i].guid)
            << ",\"bimType\":\"" << jsonEscape(fingerprints[i].bimType) << "\""
            << ",\"classification\":\"" << jsonEscape(fingerprints[i].bimType) << "\"}";
    }
    json << ']';
    return json.str();
}

std::string bimDataJson(const ObjectData& object, bool includeProperties)
{
    std::ostringstream json;
    json << "{\"classified\":true"
        << ",\"type\":\"" << jsonEscape(object.bimType) << "\""
        << ",\"name\":" << jsonStringOrNull(object.name)
        << ",\"description\":" << jsonStringOrNull(object.description)
        << ",\"guid\":" << jsonStringOrNull(object.guid)
        << ",\"blockName\":" << jsonStringOrNull(object.blockName)
        << ",\"componentType\":" << jsonStringOrNull(object.componentType);
    if (includeProperties) {
        json << ",\"properties\":" << propertiesJson(object.properties);
    }
    json << '}';
    return json.str();
}

std::string objectJson(const ObjectData& object, bool includeProperties)
{
    std::ostringstream json;
    json << "{\"handle\":\"" << jsonEscape(object.handle) << "\""
        << ",\"name\":" << jsonStringOrNull(object.name)
        << ",\"description\":" << jsonStringOrNull(object.description)
        << ",\"guid\":" << jsonStringOrNull(object.guid)
        << ",\"bimType\":\"" << jsonEscape(object.bimType) << "\""
        << ",\"entityType\":\"" << jsonEscape(object.entityType) << "\""
        << ",\"layer\":\"" << jsonEscape(object.layer) << "\""
        << ",\"blockName\":" << jsonStringOrNull(object.blockName)
        << ",\"componentType\":" << jsonStringOrNull(object.componentType)
        << ",\"bounds\":";
    if (!object.bounds.valid) {
        json << "null,\"dimensions\":null";
    } else {
        const double width = std::abs(object.bounds.maxPoint.x - object.bounds.minPoint.x);
        const double depth = std::abs(object.bounds.maxPoint.y - object.bounds.minPoint.y);
        const double height = std::abs(object.bounds.maxPoint.z - object.bounds.minPoint.z);
        json << "{\"min\":[" << object.bounds.minPoint.x << ',' << object.bounds.minPoint.y << ',' << object.bounds.minPoint.z
            << "],\"max\":[" << object.bounds.maxPoint.x << ',' << object.bounds.maxPoint.y << ',' << object.bounds.maxPoint.z << "]}"
            << ",\"dimensions\":{\"widthX\":" << width << ",\"depthY\":" << depth
            << ",\"heightZ\":" << height << ",\"unit\":\"drawing\"}";
    }
    if (includeProperties) json << ",\"properties\":" << propertiesJson(object.properties);
    json << ",\"bim\":" << bimDataJson(object, includeProperties) << '}';
    return json.str();
}

std::string queryResultJson(const QueryResult& result)
{
    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.bim.objects.query.result.v1\""
        << ",\"success\":" << (result.success ? "true" : "false")
        << ",\"availability\":" << availabilityJson(result.availability)
        << ",\"total\":" << result.total
        << ",\"offset\":" << result.offset
        << ",\"limit\":" << result.limit
        << ",\"count\":" << result.objects.size()
        << ",\"truncated\":" << (result.truncated ? "true" : "false")
        << ",\"coordinateSystem\":\"WCS\""
        << ",\"classifications\":{";
    for (std::size_t i = 0; i < result.classifications.size(); ++i) {
        if (i > 0) json << ',';
        json << '"' << jsonEscape(result.classifications[i].first) << "\":" << result.classifications[i].second;
    }
    json << "},\"objects\":" << objectsJson(result.objects, true)
        << ",\"errors\":" << targetErrorsJson(result.errors)
        << ",\"objectTable\":{\"columns\":[\"Handle\",\"Name\",\"BIM-Typ\",\"Component Type\",\"Layer\",\"Abmessungen\",\"GUID\"],\"rows\":[";
    for (std::size_t i = 0; i < result.objects.size(); ++i) {
        if (i > 0) json << ',';
        const ObjectData& object = result.objects[i];
        std::ostringstream dimensions;
        if (object.bounds.valid) {
            dimensions << std::abs(object.bounds.maxPoint.x - object.bounds.minPoint.x) << " x "
                << std::abs(object.bounds.maxPoint.y - object.bounds.minPoint.y) << " x "
                << std::abs(object.bounds.maxPoint.z - object.bounds.minPoint.z) << " drawing";
        }
        json << "[\"" << jsonEscape(object.handle) << "\"," << jsonStringOrNull(object.name)
            << ",\"" << jsonEscape(object.bimType) << "\"," << jsonStringOrNull(object.componentType)
            << ",\"" << jsonEscape(object.layer) << "\"," << jsonStringOrNull(dimensions.str())
            << ',' << jsonStringOrNull(object.guid) << ']';
    }
    json << "]}";

    bool hasProperties = false;
    for (const ObjectData& object : result.objects) hasProperties = hasProperties || !object.properties.empty();
    if (hasProperties) {
        json << ",\"propertyTable\":{\"columns\":[\"Handle\",\"Name\",\"Eigenschaft\",\"Datentyp\",\"Wert\",\"Einheit\"],\"rows\":[";
        bool first = true;
        for (const ObjectData& object : result.objects) {
            for (const Property& property : object.properties) {
                if (!first) json << ',';
                first = false;
                json << "[\"" << jsonEscape(object.handle) << "\"," << jsonStringOrNull(object.name)
                    << ",\"" << jsonEscape(property.qualifiedName) << "\",\"" << jsonEscape(property.dataType) << "\",";
                if (!property.formattedValue.empty()) json << '"' << jsonEscape(property.formattedValue) << '"';
                else json << property.valueJson;
                json << ",\"" << jsonEscape(property.unitType) << "\"]";
            }
        }
        json << "]}";
    }
    json << '}';
    return json.str();
}

std::string operationResultJson(const OperationResult& result, bool saveBefore, bool savedBefore)
{
    const std::string schemaOperation = result.operation == "selection.set" ? "selection.set" : result.operation;
    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.bim." << jsonEscape(schemaOperation) << ".result.v1\""
        << ",\"success\":" << (result.success ? "true" : "false")
        << ",\"summary\":\"bim." << jsonEscape(schemaOperation) << " affected=" << result.affectedIds.length()
        << " failed=" << result.errors.size() << "\""
        << ",\"availability\":" << availabilityJson(result.availability)
        << ",\"operation\":\"" << jsonEscape(result.operation) << "\""
        << ",\"coordinateSystem\":\"WCS\""
        << ",\"inputUnits\":" << jsonStringOrNull(result.inputUnits)
        << ",\"affected\":" << result.affectedIds.length()
        << ",\"failed\":" << result.errors.size()
        << ",\"affectedHandles\":" << handlesJson(result.affectedIds)
        << ",\"targetFingerprints\":" << fingerprintsJson(result.fingerprints)
        << ",\"failedTargets\":" << targetErrorsJson(result.errors)
        << ",\"before\":" << objectsJson(result.before, false)
        << ",\"after\":" << objectsJson(result.after, false)
        << ",\"warnings\":[],\"timeMs\":0"
        << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
        << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
        << '}';
    return json.str();
}

} // namespace Barebone::Brx::BimTools
