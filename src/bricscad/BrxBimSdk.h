#pragma once

#include "AcDb/AcDbObjectId.h"
#include "AcGe/AcGePoint3d.h"
#include "AcGe/AcGeVector3d.h"

#include <cstddef>
#include <string>
#include <vector>

class AcDbDatabase;
class AcDbEntity;

namespace Barebone::Brx::BrxBimSdk {

struct Availability {
    bool apiAvailable = false;
    bool licenseAvailable = false;
    bool available = false;
    std::string reason;
};

struct Bounds {
    bool valid = false;
    AcGePoint3d minPoint;
    AcGePoint3d maxPoint;
};

struct Fingerprint {
    std::string handle;
    std::string guid;
    std::string bimType;
    std::string name;
    std::string entityType;
    std::string layer;
    bool anchorStateKnown = false;
    bool anchored = false;
    std::string anchorHostHandle;
    std::vector<std::string> hostedAnchoredHandles;
};

struct Property {
    std::string qualifiedName;
    std::string dataType;
    std::string unitType;
    std::string valueJson = "null";
    std::string formattedValue;
    bool readOnly = false;
    bool readOnlyKnown = false;
};

struct ObjectData {
    AcDbObjectId id;
    std::string handle;
    std::string name;
    std::string description;
    std::string guid;
    std::string bimType;
    std::string entityType;
    std::string layer;
    std::string blockName;
    std::string componentType;
    Bounds bounds;
    std::vector<Property> properties;
};

enum class SelectorScope {
    CurrentSpace,
    Selection,
    Handles,
    Names,
};

struct Selector {
    SelectorScope scope = SelectorScope::CurrentSpace;
    std::vector<std::string> handles;
    std::vector<std::string> names;
    std::vector<std::string> classifications;
    bool allMatches = false;
    std::vector<std::string> expectedResolvedHandles;
    std::vector<Fingerprint> expectedFingerprints;
};

enum class ResolvePurpose {
    Query,
    SelectionMutation,
    DrawingMutation,
};

struct TargetError {
    std::string target;
    std::string code;
    std::string message;
};

struct ResolveResult {
    bool success = false;
    AcDbObjectIdArray ids;
    std::vector<Fingerprint> fingerprints;
    std::vector<TargetError> errors;
    std::vector<std::string> debug;
};

struct QueryRequest {
    Selector selector;
    bool includeGeometry = true;
    bool includeProperties = false;
    std::size_t offset = 0;
    std::size_t limit = 100;
};

struct QueryResult {
    Availability availability;
    bool success = false;
    std::size_t total = 0;
    std::size_t offset = 0;
    std::size_t limit = 100;
    bool truncated = false;
    std::vector<ObjectData> objects;
    std::vector<std::pair<std::string, std::size_t>> classifications;
    std::vector<TargetError> errors;
    std::vector<std::string> debug;
};

struct OperationResult {
    Availability availability;
    bool success = false;
    std::string operation;
    std::string inputUnits;
    AcDbObjectIdArray affectedIds;
    std::vector<Fingerprint> fingerprints;
    std::vector<ObjectData> before;
    std::vector<ObjectData> after;
    std::vector<TargetError> errors;
    std::vector<std::string> debug;
};

Availability availability();
std::string availabilityJson(const Availability& value);

ResolveResult resolve(AcDbDatabase* database, const Selector& selector, ResolvePurpose purpose);
bool readObject(const AcDbObjectId& id, bool includeGeometry, bool includeProperties, ObjectData& object, std::string& error);
QueryResult query(AcDbDatabase* database, const QueryRequest& request);
OperationResult setSelection(AcDbDatabase* database, const Selector& selector);

std::string fingerprintsJson(const std::vector<Fingerprint>& fingerprints);
std::string objectJson(const ObjectData& object, bool includeProperties = true);
std::string bimDataJson(const ObjectData& object, bool includeProperties = true);
std::string queryResultJson(const QueryResult& result);
std::string operationResultJson(const OperationResult& result, bool saveBefore, bool savedBefore);

} // namespace Barebone::Brx::BrxBimSdk
