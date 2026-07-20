using Autodesk.Revit.DB;
using Autodesk.Revit.UI;
using System.Collections.Concurrent;
using System.Text.Json;

namespace Barebone.RevitBridge;

internal sealed class BridgeExternalEventHandler : IExternalEventHandler
{
    private readonly ConcurrentQueue<BridgeRequest> _requests = new();
    private BridgeClient? _client;

    public void AttachClient(BridgeClient client)
    {
        _client = client;
    }

    public void Enqueue(BridgeRequest request)
    {
        _requests.Enqueue(request);
    }

    public void Execute(UIApplication app)
    {
        while (_requests.TryDequeue(out var request)) {
            try {
                var result = ExecuteRequest(app, request);
                _client?.SendResponse(request.Id, ok: true, result: result);
            } catch (Exception ex) {
                _client?.SendResponse(request.Id, ok: false, error: ex.Message);
            }
        }
    }

    public string GetName()
    {
        return "Barebone Revit Bridge External Event";
    }

    private static object ExecuteRequest(UIApplication app, BridgeRequest request)
    {
        return request.Method switch {
            "revit.status" => Status(app),
            "revit.document.summary" => DocumentSummary(app),
            "revit.selection.describe" => SelectionDescribe(app),
            "revit.levels.list" => LevelsList(app),
            "revit.views.list" => ViewsList(app, request.Params),
            "revit.categories.summary" => CategoriesSummary(app),
            "revit.elements.list" => ElementsList(app, request.Params),
            "revit.selection.set" => SelectionSet(app, request.Params),
            "revit.textNote.create" => TextNoteCreate(app, request.Params),
            "revit.parameter.setStringOnSelection" => SetStringParameterOnSelection(app, request.Params),
            _ => throw new InvalidOperationException($"Unsupported method: {request.Method}")
        };
    }

    private static object Status(UIApplication app)
    {
        var uidoc = app.ActiveUIDocument;
        var doc = uidoc?.Document;
        return new {
            message = doc is null ? "Revit verbunden, kein aktives Dokument." : $"Revit verbunden: {doc.Title}",
            revitVersion = app.Application.VersionNumber,
            activeDocument = doc is null ? null : DocumentInfo(doc),
            activeView = doc?.ActiveView is null ? null : ViewInfo(doc.ActiveView)
        };
    }

    private static object DocumentSummary(UIApplication app)
    {
        var doc = RequireDocument(app);
        return new {
            document = DocumentInfo(doc),
            activeView = ViewInfo(doc.ActiveView),
            counts = new {
                levels = new FilteredElementCollector(doc).OfClass(typeof(Level)).GetElementCount(),
                views = new FilteredElementCollector(doc).OfClass(typeof(View)).GetElementCount(),
                walls = new FilteredElementCollector(doc).OfClass(typeof(Wall)).WhereElementIsNotElementType().GetElementCount(),
                rooms = new FilteredElementCollector(doc).OfCategory(BuiltInCategory.OST_Rooms).WhereElementIsNotElementType().GetElementCount()
            }
        };
    }

    private static object SelectionDescribe(UIApplication app)
    {
        var uidoc = RequireUiDocument(app);
        var doc = uidoc.Document;
        var elementIds = uidoc.Selection.GetElementIds();
        var elements = elementIds
            .Select(id => doc.GetElement(id))
            .Where(element => element is not null)
            .Take(50)
            .Select(element => new {
                id = ElementIdValue(element!.Id),
                name = element.Name,
                category = element.Category?.Name,
                type = element.GetType().Name
            })
            .ToArray();
        return new {
            count = elementIds.Count,
            elements
        };
    }

    private static object LevelsList(UIApplication app)
    {
        var doc = RequireDocument(app);
        return new FilteredElementCollector(doc)
            .OfClass(typeof(Level))
            .Cast<Level>()
            .OrderBy(level => level.Elevation)
            .Select(level => new {
                id = ElementIdValue(level.Id),
                name = level.Name,
                elevation = level.Elevation
            })
            .ToArray();
    }

    private static object ViewsList(UIApplication app, JsonElement parameters)
    {
        var doc = RequireDocument(app);
        var limit = Math.Clamp(ReadInt(parameters, "limit", 200), 1, 10000);
        return new FilteredElementCollector(doc)
            .OfClass(typeof(View))
            .Cast<View>()
            .Where(view => !view.IsTemplate)
            .OrderBy(view => view.ViewType.ToString())
            .ThenBy(view => view.Name)
            .Take(limit)
            .Select(view => new {
                id = ElementIdValue(view.Id),
                name = view.Name,
                viewType = view.ViewType.ToString()
            })
            .ToArray();
    }

    private static object CategoriesSummary(UIApplication app)
    {
        var doc = RequireDocument(app);
        var categories = new FilteredElementCollector(doc)
            .WhereElementIsNotElementType()
            .ToElements()
            .GroupBy(element => element.Category?.Name ?? "Ohne Kategorie")
            .OrderByDescending(group => group.Count())
            .ThenBy(group => group.Key)
            .Select(group => new {
                category = group.Key,
                count = group.Count()
            })
            .ToArray();

        return new {
            document = DocumentInfo(doc),
            totalCount = categories.Sum(category => category.count),
            categories
        };
    }

    private static object ElementsList(UIApplication app, JsonElement parameters)
    {
        var doc = RequireDocument(app);
        var scope = ReadString(parameters, "scope", "document");
        if (scope != "document" && scope != "activeView") {
            scope = "document";
        }
        var limit = Math.Clamp(ReadInt(parameters, "limit", 10000), 1, 10000);
        var includeElementTypes = ReadBool(parameters, "includeElementTypes", false);

        var totalCount = ElementCollector(doc, scope, includeElementTypes).GetElementCount();
        var elements = ElementCollector(doc, scope, includeElementTypes)
            .ToElements()
            .OrderBy(element => element.Category?.Name ?? "")
            .ThenBy(element => element.Name ?? "")
            .ThenBy(element => ElementIdValue(element.Id))
            .Take(limit)
            .Select(element => ElementInfo(doc, element))
            .ToArray();

        return new {
            document = DocumentInfo(doc),
            scope,
            includeElementTypes,
            totalCount,
            returnedCount = elements.Length,
            truncated = totalCount > elements.Length,
            elements
        };
    }

    private static object SelectionSet(UIApplication app, JsonElement parameters)
    {
        var uidoc = RequireUiDocument(app);
        var ids = ReadLongArray(parameters, "elementIds").Select(id => new ElementId(id)).ToList();
        uidoc.Selection.SetElementIds(ids);
        return new {
            message = $"{ids.Count} Revit-Element(e) ausgewaehlt.",
            selected = ids.Select(ElementIdValue).ToArray()
        };
    }

    private static object TextNoteCreate(UIApplication app, JsonElement parameters)
    {
        var doc = RequireDocument(app);
        var view = doc.ActiveView ?? throw new InvalidOperationException("Kein aktiver View vorhanden.");
        var text = ReadString(parameters, "text");
        if (string.IsNullOrWhiteSpace(text)) {
            throw new InvalidOperationException("TextNote benoetigt params.text.");
        }

        var x = ReadDouble(parameters, "x", 0.0);
        var y = ReadDouble(parameters, "y", 0.0);
        var z = ReadDouble(parameters, "z", 0.0);
        var textType = new FilteredElementCollector(doc)
            .OfClass(typeof(TextNoteType))
            .FirstElement()
            ?? throw new InvalidOperationException("Kein TextNoteType im Dokument gefunden.");

        using var transaction = new Transaction(doc, "Barebone Revit TextNote");
        transaction.Start();
        var note = TextNote.Create(doc, view.Id, new XYZ(x, y, z), text, new TextNoteOptions(textType.Id));
        transaction.Commit();

        return new {
            message = $"Textnotiz erstellt: {ElementIdValue(note.Id)}",
            elementId = ElementIdValue(note.Id)
        };
    }

    private static object SetStringParameterOnSelection(UIApplication app, JsonElement parameters)
    {
        var uidoc = RequireUiDocument(app);
        var doc = uidoc.Document;
        var name = ReadString(parameters, "parameterName");
        var value = ReadString(parameters, "value");
        if (string.IsNullOrWhiteSpace(name)) {
            throw new InvalidOperationException("Parameteraktion benoetigt params.parameterName.");
        }

        var selectedIds = uidoc.Selection.GetElementIds().ToArray();
        if (selectedIds.Length == 0) {
            throw new InvalidOperationException("Keine Auswahl vorhanden.");
        }

        var changed = new List<long>();
        var skipped = new List<long>();
        using var transaction = new Transaction(doc, "Barebone Revit Parameter");
        transaction.Start();
        foreach (var id in selectedIds) {
            var element = doc.GetElement(id);
            var parameter = element?.LookupParameter(name);
            if (parameter is null || parameter.IsReadOnly || parameter.StorageType != StorageType.String) {
                skipped.Add(ElementIdValue(id));
                continue;
            }
            parameter.Set(value);
            changed.Add(ElementIdValue(id));
        }
        transaction.Commit();

        return new {
            message = $"{changed.Count} Parameter aktualisiert, {skipped.Count} uebersprungen.",
            changed,
            skipped
        };
    }

    private static UIDocument RequireUiDocument(UIApplication app)
    {
        return app.ActiveUIDocument ?? throw new InvalidOperationException("Kein aktives Revit-Dokument vorhanden.");
    }

    private static Document RequireDocument(UIApplication app)
    {
        return RequireUiDocument(app).Document;
    }

    private static object DocumentInfo(Document doc)
    {
        return new {
            title = doc.Title,
            path = doc.PathName,
            isFamilyDocument = doc.IsFamilyDocument,
            isWorkshared = doc.IsWorkshared,
            isModified = doc.IsModified
        };
    }

    private static object ViewInfo(View view)
    {
        return new {
            id = ElementIdValue(view.Id),
            name = view.Name,
            viewType = view.ViewType.ToString()
        };
    }

    private static long ElementIdValue(ElementId id)
    {
        return id.Value;
    }

    private static FilteredElementCollector ElementCollector(Document doc, string scope, bool includeElementTypes)
    {
        var collector = scope == "activeView" && doc.ActiveView is not null
            ? new FilteredElementCollector(doc, doc.ActiveView.Id)
            : new FilteredElementCollector(doc);
        return includeElementTypes ? collector : collector.WhereElementIsNotElementType();
    }

    private static object ElementInfo(Document doc, Element element)
    {
        var typeId = element.GetTypeId();
        var typeElement = IsValidElementId(typeId) ? doc.GetElement(typeId) as ElementType : null;
        var level = IsValidElementId(element.LevelId) ? doc.GetElement(element.LevelId) as Level : null;
        var ownerView = IsValidElementId(element.OwnerViewId) ? doc.GetElement(element.OwnerViewId) as View : null;
        return new {
            id = ElementIdValue(element.Id),
            uniqueId = element.UniqueId,
            name = element.Name,
            category = element.Category?.Name,
            className = element.GetType().Name,
            familyName = typeElement?.FamilyName,
            typeName = typeElement?.Name,
            typeId = IsValidElementId(typeId) ? ElementIdValue(typeId) : (long?)null,
            levelName = level?.Name,
            levelId = level is null ? (long?)null : ElementIdValue(level.Id),
            ownerViewName = ownerView?.Name,
            ownerViewId = ownerView is null ? (long?)null : ElementIdValue(ownerView.Id),
            viewSpecific = element.ViewSpecific,
            pinned = element.Pinned,
            isElementType = element is ElementType
        };
    }

    private static bool IsValidElementId(ElementId id)
    {
        return id != ElementId.InvalidElementId && id.Value > 0;
    }

    private static string ReadString(JsonElement element, string propertyName, string defaultValue = "")
    {
        return element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(propertyName, out var value)
            && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? defaultValue
            : defaultValue;
    }

    private static double ReadDouble(JsonElement element, string propertyName, double defaultValue)
    {
        if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(propertyName, out var value)) {
            return defaultValue;
        }
        return value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var number) ? number : defaultValue;
    }

    private static int ReadInt(JsonElement element, string propertyName, int defaultValue)
    {
        if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(propertyName, out var value)) {
            return defaultValue;
        }
        return value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number) ? number : defaultValue;
    }

    private static bool ReadBool(JsonElement element, string propertyName, bool defaultValue)
    {
        if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(propertyName, out var value)) {
            return defaultValue;
        }
        return value.ValueKind == JsonValueKind.True || (value.ValueKind != JsonValueKind.False && defaultValue);
    }

    private static IEnumerable<long> ReadLongArray(JsonElement element, string propertyName)
    {
        if (element.ValueKind != JsonValueKind.Object
            || !element.TryGetProperty(propertyName, out var value)
            || value.ValueKind != JsonValueKind.Array) {
            return [];
        }
        return value.EnumerateArray()
            .Where(item => item.ValueKind == JsonValueKind.Number && item.TryGetInt64(out _))
            .Select(item => item.GetInt64());
    }
}
