#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "arxHeaders.h"
#include "AcDb/AcDbSymbolUtilities_Services.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kQtBridgeHost = "127.0.0.1";
constexpr unsigned short kQtBridgePort = 47626;
constexpr double kRectangleTolerance = 1.0e-6;
constexpr const char* kBridgeTokenFileName = "BareboneQtBridge.token";
constexpr const char* kBridgeBuildId = "bridge-json-v2";
constexpr const wchar_t* kBridgeLogWindowClassName = L"BareboneQtBrxBridgeLogWindow";
constexpr UINT kBridgeLogAppendMessage = WM_APP + 101;

std::atomic_bool g_bridgeClientRunning{false};
std::atomic_bool g_bridgeClientConnected{false};
std::atomic_bool g_pluginLoaded{false};
std::thread g_bridgeClientThread;
std::mutex g_bridgeClientSocketMutex;
std::mutex g_bridgeClientSendMutex;
SOCKET g_bridgeClientSocket = INVALID_SOCKET;
HWND g_bridgeLogWindow = nullptr;
HWND g_bridgeLogEdit = nullptr;

struct BridgeJob {
    explicit BridgeJob(std::string value)
        : request(std::move(value))
    {
    }

    std::string request;
    std::string response;
    std::mutex mutex;
    std::condition_variable doneEvent;
    bool done = false;
};

struct BridgeMethodDescriptor {
    const char* name;
    const char* kind;
    const char* risk;
    const char* description;
    const char* paramsSchema;   // JSON object fragment or nullptr
    const char* apiPost;        // JSON object fragment or nullptr
};

struct BridgeCommandDescriptor {
    const char* name;
    const char* description;
    const char* json;           // JSON object fragment or nullptr
};

std::string jsonEscape(const std::string& value);

constexpr const char* kSelectorSchema = R"({
"type":"object",
"properties":{
  "scope":{"type":"string","enum":["currentSpace","selection","handles","lastResult","lastExtruded"]},
  "layer":{"type":"string"},
  "handles":{"type":"array","items":{"type":"string"}},
  "kind":{"type":"string","enum":["polyline","solid","entity"]},
  "shape":{"type":"string","enum":["rectangle"]}
}
})";

const BridgeMethodDescriptor kBridgeMethods[] = {
    {"capabilities.list", "query", "readOnly", "Liefert die verfuegbaren BRX-Bridge-Methoden und Schemas.", nullptr, nullptr},
    {"actions.list", "query", "readOnly", "Liefert die dokumentierten Barebone-BRX API-Aktionen mit POST-Optionen, Pflichtfeldern und Beispielen.", nullptr, nullptr},
    {
        "actions.validate",
        "query",
        "readOnly",
        "Prueft einen Agent-Action-Vorschlag trocken gegen BRX-Syntax, Pflichtdaten und aktuellen Zeichnungszustand.",
        R"JSON({
"type":"object",
"properties":{
  "actions":{"type":"array","items":{"type":"object"}},
  "tool":{"type":"string"},
  "method":{"type":"string"},
  "params":{"type":"object"}
}
})JSON",
        nullptr,
    },
    {
        "pipes.validateNetwork", "query", "readOnly",
        "Validiert Konnektivitaet und offene Enden eines Rohrnetzes aus offenen Polylinien.",
        R"({"type":"object","required":["system"],"properties":{"system":{"type":"string"},"selector":{"type":"object"},"startNode":{"type":"object"},"endNodes":{"type":"array"},"teeNodes":{"type":"array"},"minimumClearanceMm":{"type":"number","minimum":0},"avoidLayers":{"type":"array","items":{"type":"string"}}}})", nullptr,
    },
    {
        "pipes.createNetworkSolids", "action", "modifiesDrawing",
        "Erzeugt zylindrische Rohrsegmente entlang validierter offener Polylinien.",
        R"({"type":"object","required":["system","selector","diameterMm","targetLayer"],"properties":{"system":{"type":"string"},"selector":{"type":"object"},"diameterMm":{"type":"number","exclusiveMinimum":0},"targetLayer":{"type":"string"},"connectionMode":{"enum":["elbows_and_tees"]},"saveBefore":{"type":"boolean"}}})",
        R"({"method":"pipes.createNetworkSolids","required":["system","selector","diameterMm","targetLayer"]})",
    },
    {
        "annotations.createRoomDimensions", "action", "modifiesDrawing",
        "Erzeugt Laengen- und Breitenbemassung sowie einen Raumstempel fuer rechteckige Raumkonturen.",
        R"({"type":"object","required":["selector","roomHeightMm","dimensionLayer","labelLayer"],"properties":{"selector":{"type":"object"},"roomNames":{"type":"array","items":{"type":"string"}},"roomHeightMm":{"type":"number","exclusiveMinimum":0},"dimensionLayer":{"type":"string"},"labelLayer":{"type":"string"},"textHeightMm":{"type":"number","exclusiveMinimum":0},"offsetMm":{"type":"number","minimum":0},"saveBefore":{"type":"boolean"}}})",
        R"({"method":"annotations.createRoomDimensions","required":["selector","roomHeightMm","dimensionLayer","labelLayer"]})",
    },
    {"commands.list", "query", "readOnly", "Liefert eine Low-Level-Startliste nativer BricsCAD-Kommandos und zugehoeriger Bridge-Tools.", nullptr, nullptr},
    {"layers.list", "query", "readOnly", "Listet Layer der aktiven Zeichnung.", nullptr, nullptr},
    {
        "geometry.query",
        "query",
        "readOnly",
        "Liest aktuelle Geometrien aus der Zeichnung anhand eines Selectors und optionaler Filter. Liefert bounds und daraus abgeleitete dimensions(widthX, depthY, heightZ, unit=mm), sofern getGeomExtents verfuegbar ist.",
        R"JSON({
"type":"object",
"properties":{
  "selector":{"type":"object","properties":{
    "scope":{"type":"string","enum":["currentSpace","selection","handles","lastResult","lastExtruded"]},
    "layer":{"type":"string"},
    "handles":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["polyline","solid","entity"]},
    "shape":{"type":"string","enum":["rectangle"]}
  }},
  "filters":{"type":"object","properties":{
    "layer":{"type":"string"},
    "layers":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["polyline","solid","entity"]},
    "kinds":{"type":"array","items":{"type":"string"}},
    "shape":{"type":"string","enum":["rectangle"]},
    "shapes":{"type":"array","items":{"type":"string"}},
    "types":{"type":"array","items":{"type":"string"}},
    "typeContains":{"type":"string"},
    "isClosed":{"type":"boolean"},
    "is3D":{"type":"boolean"},
    "minArea":{"type":"number"},
    "maxArea":{"type":"number"}
  }},
  "include":{"type":"array","items":{"type":"string"}},
  "limit":{"type":"number","minimum":1}
}
})JSON",
        nullptr,
    },
    {
        "selection.describe",
        "query",
        "readOnly",
        "Beschreibt die aktuelle BricsCAD-Auswahl mit Handles, Typen, Layern und optionaler Geometrie.",
        R"JSON({
"type":"object",
"properties":{
  "include":{"type":"array","items":{"type":"string"}},
  "limit":{"type":"number","minimum":1}
}
})JSON",
        nullptr,
    },
    {
        "entity.describe",
        "query",
        "readOnly",
        "Beschreibt konkrete Entities per Handle-Liste.",
        R"JSON({
"type":"object",
"required":["handles"],
"properties":{
  "handle":{"type":"string"},
  "handles":{"type":"array","items":{"type":"string"}},
  "include":{"type":"array","items":{"type":"string"}},
  "limit":{"type":"number","minimum":1}
},
"oneOfRequired":[["handle"],["handles"]]
})JSON",
        nullptr,
    },
    {
        "geometry.create",
        "action",
        "modifiesDrawing",
        "Erzeugt neue Grundgeometrie direkt in der Zeichnung.",
        R"JSON({
"type":"object",
"required":["geometry"],
"properties":{
  "geometry":{"type":"string","enum":["point","rectangle","line","polyline","circle","arc","box"]},
  "layer":{"type":"string"},
  "position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "origin":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "center":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "radius":{"type":"number"},
  "radiusMm":{"type":"number"},
  "startAngle":{"type":"number"},
  "endAngle":{"type":"number"},
  "startAngleDeg":{"type":"number"},
  "endAngleDeg":{"type":"number"},
  "width":{"type":"number"},
  "widthMm":{"type":"number"},
  "x":{"type":"number"},
  "depth":{"type":"number"},
  "length":{"type":"number"},
  "lengthMm":{"type":"number"},
  "depthMm":{"type":"number"},
  "y":{"type":"number"},
  "height":{"type":"number"},
  "heightMm":{"type":"number"},
  "z":{"type":"number"},
  "start":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "end":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "points":{"type":"array","items":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}},
  "closed":{"type":"boolean"},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"geometry.create",
"required":["geometry"],
"bodySchema":{"type":"object","properties":{
  "geometry":{"type":"string","enum":["point","rectangle","line","polyline","circle","arc","box"]},
  "layer":{"type":"string"},
  "position":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "origin":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "center":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "radius":{"type":"number"},
  "radiusMm":{"type":"number"},
  "startAngle":{"type":"number"},
  "endAngle":{"type":"number"},
  "startAngleDeg":{"type":"number"},
  "endAngleDeg":{"type":"number"},
  "width":{"type":"number"},
  "widthMm":{"type":"number"},
  "x":{"type":"number"},
  "depth":{"type":"number"},
  "length":{"type":"number"},
  "lengthMm":{"type":"number"},
  "depthMm":{"type":"number"},
  "y":{"type":"number"},
  "height":{"type":"number"},
  "heightMm":{"type":"number"},
  "z":{"type":"number"},
  "start":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "end":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "points":{"type":"array","items":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}},
  "closed":{"type":"boolean"},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"examples":[
  {"geometry":"box","origin":{"x":0,"y":0,"z":0},"width":100,"depth":100,"height":100,"layer":"0","saveBefore":true},
  {"geometry":"circle","center":{"x":0,"y":0,"z":0},"radius":50,"layer":"0"},
  {"geometry":"arc","center":{"x":0,"y":0,"z":0},"radius":50,"startAngleDeg":0,"endAngleDeg":90},
  {"geometry":"rectangle","origin":{"x":0,"y":0,"z":0},"width":100,"depth":100,"layer":"0","saveBefore":true},
  {"geometry":"line","start":{"x":0,"y":0,"z":0},"end":{"x":100,"y":0,"z":0},"layer":"0"},
  {"geometry":"polyline","points":[{"x":0,"y":0,"z":0},{"x":100,"y":0,"z":0},{"x":100,"y":50,"z":0}],"closed":false},
  {"geometry":"point","position":{"x":0,"y":0,"z":0}}
]}
})JSON",
    },
    {
        "rectangles.extrude",
        "action",
        "modifiesDrawing",
        "Extrudiert geschlossene Rechteck-Polylinien ueber die BRX API.",
        R"JSON({
"type":"object",
"required":["heightMm"],
"properties":{
  "layer":{"type":"string","description":"Optionaler Layername, wenn kein selector verwendet wird."},
  "selector":{"type":"object","description":"Optionaler Selector fuer Auswahl, Handles oder aktuellen Raum.","properties":{
    "scope":{"type":"string","enum":["currentSpace","selection","handles","lastResult","lastExtruded"]},
    "layer":{"type":"string"},
    "handles":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["rectangle"]},
    "shape":{"type":"string","enum":["rectangle"]}
  }},
  "heightMm":{"type":"number","minimum":0.1,"description":"Extrusionshoehe in Millimetern."},
  "detail":{"type":"string","enum":["summary","element","geometry"]},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"oneOfRequired":[["layer"],["selector"]]
})JSON",
        R"JSON({
"method":"rectangles.extrude",
"required":["heightMm"],
"oneOfRequired":[["layer"],["selector"]],
"bodySchema":{"type":"object","properties":{
  "layer":{"type":"string"},
  "selector":{"type":"object","properties":{
    "scope":{"type":"string","enum":["currentSpace","selection","handles","lastResult","lastExtruded"]},
    "handles":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["rectangle"]},
    "shape":{"type":"string","enum":["rectangle"]}
  }},
  "heightMm":{"type":"number","minimum":0.1},
  "detail":{"type":"string","enum":["summary","element","geometry"]},
  "saveBefore":{"type":"boolean"}
}},
"required":["heightMm"],
"examples":[{"heightMm":1000,"selector":{"scope":"selection","kind":"rectangle"},"saveBefore":true},{"heightMm":3000,"layer":"0","detail":"element","saveBefore":true}]
})JSON",
    },
    {
        "undo.last",
        "action",
        "modifiesDrawing",
        "Macht bis zu mehreren letzten Aktionen rueckgaengig.",
        R"JSON({
"type":"object",
"properties":{
  "steps":{"type":"number","minimum":1,"description":"Anzahl der Schritte fuer UNDO (Standard: 1)."},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"undo.last",
"required":[],
"bodySchema":{"type":"object","properties":{
  "steps":{"type":"number","minimum":1,"description":"Anzahl der Schritte fuer UNDO"},
  "saveBefore":{"type":"boolean","description":"Default: true"},
  "reason":{"type":"string"}
}},
"examples":[{"steps":1,"saveBefore":true}]
})JSON",
    },
    {
        "bim.classify",
        "action",
        "modifiesDrawing",
        "Klassifiziert 3D-Solids als BIM-Element.",
        R"JSON({
"type":"object",
"required":["classification"],
"properties":{
  "target":{"type":"string","enum":["lastExtruded","selection","handles"]},
  "selector":{"type":"object","description":"Optionaler Selector fuer Auswahl, Handles, lastResult, lastExtruded oder aktuellen Raum.","properties":{
    "scope":{"type":"string","enum":["selection","handles","lastResult","lastExtruded","currentSpace"]},
    "handles":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["solid"]}
  }},
  "classification":{"type":"string","enum":["BIMWall"]},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"oneOfRequired":[["target"],["selector"]]
})JSON",
        R"JSON({
"method":"bim.classify",
"required":["classification"],
"oneOfRequired":[["target"],["selector"]],
"bodySchema":{"type":"object","properties":{
  "target":{"type":"string","enum":["lastExtruded","selection","handles"]},
  "selector":{"type":"object","properties":{
    "scope":{"type":"string","enum":["selection","handles","lastResult","lastExtruded","currentSpace"]},
    "handles":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["solid"]}
  }},
  "classification":{"type":"string","enum":["BIMWall"]},
  "saveBefore":{"type":"boolean"}
}},
"examples":[{"classification":"BIMWall","target":"lastExtruded","saveBefore":true},{"classification":"BIMWall","selector":{"scope":"selection","kind":"solid"},"saveBefore":true}]
})JSON",
    },
    {
        "profile.extrude",
        "action",
        "modifiesDrawing",
        "Extrudiert beliebige geschlossene 2D-Profile ueber Selector oder Handles.",
        R"JSON({
"type":"object",
"required":["heightMm"],
"properties":{
  "selector":{"type":"object","properties":{
    "scope":{"type":"string","enum":["currentSpace","selection","handles","lastResult"]},
    "layer":{"type":"string"},
    "handles":{"type":"array","items":{"type":"string"}},
    "kind":{"type":"string","enum":["polyline","entity","profile"]},
    "shape":{"type":"string"}
  }},
  "heightMm":{"type":"number","minimum":0.1},
  "direction":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "taperAngleDeg":{"type":"number"},
  "detail":{"type":"string","enum":["summary","element","geometry"]},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"oneOfRequired":[["selector"],["layer"]]
})JSON",
        R"JSON({
"method":"profile.extrude",
"required":["heightMm"],
"oneOfRequired":[["selector"],["layer"]],
"bodySchema":{"type":"object","properties":{
  "selector":"Selector for closed 2D profiles; prefer {scope:'selection'} for current selection",
  "layer":"optional layer fallback",
  "heightMm":"positive extrusion height in mm",
  "direction":"optional extrusion direction vector; z-axis is default",
  "taperAngleDeg":"optional taper angle; native command support can depend on BricsCAD",
  "detail":"summary|element|geometry",
  "saveBefore":"boolean default true"
}},
"examples":[{"heightMm":3000,"selector":{"scope":"selection","kind":"profile"},"saveBefore":true},{"heightMm":1000,"layer":"0","detail":"element"}]
})JSON",
    },
    {
        "geometry.move",
        "action",
        "modifiesDrawing",
        "Verschiebt Entities per Selector um einen Vektor oder von einem Punkt zu einem Zielpunkt.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "vector":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "offset":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "fromPoint":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "toPoint":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"oneOfRequired":[["vector"],["offset"],["fromPoint","toPoint"]]
})JSON",
        R"JSON({
"method":"geometry.move",
"required":["selector"],
"oneOfRequired":[["vector"],["offset"],["fromPoint","toPoint"]],
"bodySchema":{"type":"object","properties":{"selector":"Selector","vector":"{x,y,z} offset in drawing units/mm","fromPoint":"{x,y,z}","toPoint":"{x,y,z}","saveBefore":"boolean default true"}},
"examples":[{"selector":{"scope":"selection"},"vector":{"x":100,"y":0,"z":0}},{"selector":{"scope":"handles","handles":["A7"]},"fromPoint":{"x":0,"y":0,"z":0},"toPoint":{"x":100,"y":0,"z":0}}]
})JSON",
    },
    {
        "geometry.copy",
        "action",
        "modifiesDrawing",
        "Kopiert Entities per Selector um einen Offset, optional mehrfach mit Abstand.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "vector":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "offset":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "count":{"type":"number","minimum":1},
  "spacing":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"oneOfRequired":[["vector"],["offset"],["spacing"]]
})JSON",
        R"JSON({
"method":"geometry.copy",
"required":["selector"],
"oneOfRequired":[["vector"],["offset"],["spacing"]],
"bodySchema":{"type":"object","properties":{"selector":"Selector","vector":"single copy offset","count":"number of copies; default 1","spacing":"repeated-copy offset","saveBefore":"boolean default true"}},
"examples":[{"selector":{"scope":"selection"},"vector":{"x":500,"y":0,"z":0},"count":2}]
})JSON",
    },
    {
        "geometry.rotate",
        "action",
        "modifiesDrawing",
        "Rotiert Entities per Selector um einen Basispunkt und eine Achse.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "basePoint":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "basePointMode":{"type":"string","enum":["entityCenter","eachEntityCenter","selectionCenter"]},
  "axis":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "angleDeg":{"type":"number"},
  "angleRad":{"type":"number"},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
},
"oneOfRequired":[["angleDeg"],["angleRad"]]
})JSON",
        R"JSON({
"method":"geometry.rotate",
"required":["selector"],
"oneOfRequired":[["angleDeg"],["angleRad"]],
"bodySchema":{"type":"object","properties":{"selector":"Selector; after selection.set prefer scope=handles with affectedHandles instead of scope=selection","basePoint":"rotation center","basePointMode":"entityCenter/eachEntityCenter rotates every entity around its own extents center; selectionCenter rotates around the combined selection extents center","axis":"rotation axis; Z default","angleDeg":"user-provided degrees; no default","angleRad":"user-provided radians; no default","saveBefore":"boolean default true"}},
"examples":[{"selector":{"scope":"selection"},"basePoint":{"x":0,"y":0,"z":0},"angleDeg":90}]
})JSON",
    },
    {
        "geometry.scale",
        "action",
        "modifiesDrawing",
        "Skaliert Entities per Selector. Uniforme Skalierung ist stabil freigegeben.",
R"JSON({
"type":"object",
"required":["selector","factor"],
"properties":{
  "selector":{"type":"object"},
  "basePoint":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},
  "factor":{"type":"number","minimum":0.000001},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"geometry.scale",
"required":["selector","factor"],
"bodySchema":{"type":"object","properties":{"selector":"Selector","basePoint":"scale origin; default 0,0,0","factor":"uniform scale factor only; do not use for one-axis extension or lengthening","saveBefore":"boolean default true"}},
"examples":[{"selector":{"scope":"selection"},"basePoint":{"x":0,"y":0,"z":0},"factor":2}]
})JSON",
    },
    {
        "geometry.delete",
        "action",
        "modifiesDrawing",
        "Loescht Entities per Selector oder Handles. confirm=true ist Pflicht.",
        R"JSON({
"type":"object",
"required":["selector","confirm"],
"properties":{
  "selector":{"type":"object"},
  "confirm":{"type":"boolean","const":true},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"geometry.delete",
"required":["selector","confirm"],
"bodySchema":{"type":"object","properties":{"selector":"Selector","confirm":"must be true","saveBefore":"boolean default true"}},
"examples":[{"selector":{"scope":"handles","handles":["A7"]},"confirm":true}]
})JSON",
    },
    {
        "selection.set",
        "action",
        "modifiesEditorState",
        "Setzt die aktuelle BricsCAD-Auswahl aus einem Selector oder einer Handle-Liste.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"selection.set",
"required":["selector"],
"bodySchema":{"type":"object","properties":{"selector":"Selector resolving to selectable entities"}},
"examples":[{"selector":{"scope":"handles","handles":["A7","A8"]}}]
})JSON",
    },
    {
        "entity.setLayer",
        "action",
        "modifiesDrawing",
        "Weist vorhandenen Entities per Selector einen Ziel-Layer zu.",
        R"JSON({
"type":"object",
"required":["selector","layer"],
"properties":{
  "selector":{"type":"object"},
  "layer":{"type":"string"},
  "targetLayer":{"type":"string"},
  "createIfMissing":{"type":"boolean"},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"entity.setLayer",
"required":["selector","layer"],
"bodySchema":{"type":"object","properties":{"selector":"Selector for existing entities; use scope=handles for explicit handles like A8","layer":"target layer name","targetLayer":"alias for layer","createIfMissing":"optional boolean; creates target layer before assignment when missing","saveBefore":"boolean default true"}},
"examples":[{"selector":{"scope":"handles","handles":["A8"]},"layer":"Haus - Wände","createIfMissing":true,"saveBefore":true}]
})JSON",
    },
    {
        "layers.create",
        "action",
        "modifiesDrawing",
        "Legt einen Layer ueber BricsCADs nativen -LAYER-Command im Command Context an, optional mit Farbe.",
        R"JSON({
"type":"object",
"required":["name"],
"properties":{
  "name":{"type":"string"},
  "colorIndex":{"type":"number","minimum":1},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"layers.create",
"required":["name"],
"bodySchema":{"type":"object","properties":{"name":"layer name","colorIndex":"ACI color index 1..255 optional","saveBefore":"boolean default true"}},
"examples":[{"name":"AI-Walls","colorIndex":1}]
})JSON",
    },
    {
        "layers.rename",
        "action",
        "modifiesDrawing",
        "Benennt einen vorhandenen Layer ueber BricsCADs nativen -LAYER-Command im Command Context um.",
        R"JSON({
"type":"object",
"required":["oldName","newName"],
"properties":{
  "oldName":{"type":"string"},
  "newName":{"type":"string"},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"layers.rename",
"required":["oldName","newName"],
"bodySchema":{"type":"object","properties":{"oldName":"existing layer name","newName":"new layer name","saveBefore":"boolean default true"}},
"examples":[{"oldName":"Layer1","newName":"AI-Walls"}]
})JSON",
    },
    {
        "layers.setColor",
        "action",
        "modifiesDrawing",
        "Setzt die ACI-Farbe eines Layers ueber BricsCADs nativen -LAYER-Command im Command Context.",
        R"JSON({
"type":"object",
"required":["name","colorIndex"],
"properties":{
  "name":{"type":"string"},
  "colorIndex":{"type":"number","minimum":1},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"layers.setColor",
"required":["name","colorIndex"],
"bodySchema":{"type":"object","properties":{"name":"layer name","colorIndex":"ACI color index 1..255","saveBefore":"boolean default true"}},
"examples":[{"name":"AI-Walls","colorIndex":3}]
})JSON",
    },
    {
        "layers.batch",
        "action",
        "modifiesDrawing",
        "Fuehrt mehrere Layer-Aktionen seriell ueber BricsCADs nativen -LAYER-Command im Command Context aus.",
        R"JSON({
"type":"object",
"required":["actions"],
"properties":{
  "actions":{"type":"array","items":{"type":"object"}},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"layers.batch",
"required":["actions"],
"bodySchema":{"type":"object","properties":{"actions":"array of {tool|method|operation, params}; supported operations: layers.create, layers.rename, layers.setColor","saveBefore":"boolean default true"}},
"examples":[{"actions":[{"tool":"layers.create","params":{"name":"Layer1"}},{"tool":"layers.setColor","params":{"name":"Layer1","colorIndex":3}}],"saveBefore":true}]
})JSON",
    },
    {
        "command.execute",
        "action",
        "modifiesDrawing",
        "Sendet eine vorvalidierte native BricsCAD-Kommandozeile an das aktive Dokument.",
        R"JSON({
"type":"object",
"required":["commandLine"],
"properties":{
  "commandLine":{"type":"string"},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"command.execute",
"required":["commandLine"],
"bodySchema":{"type":"object","properties":{"commandLine":"single native BricsCAD command line from commands.list; no scripts or multi-command lines","saveBefore":"boolean default true"}},
"examples":[{"commandLine":"_.-LAYER _New AI-Walls","saveBefore":true}]
})JSON",
    },
    {
        "document.save",
        "action",
        "modifiesDocument",
        "Speichert die aktive Zeichnung explizit.",
        R"JSON({
"type":"object",
"properties":{
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"document.save",
"required":[],
"bodySchema":{"type":"object","properties":{"reason":"optional reason shown in logs"}},
"examples":[{"reason":"checkpoint before AI action"}]
})JSON",
    },
    {
        "measurement.bbox",
        "query",
        "readOnly",
        "Berechnet Bounding-Boxes fuer Entities per Selector. Liefert pro Objekt bounds, dimensions(widthX, depthY, heightZ, unit=mm), success und optional error.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "includeObjects":{"type":"boolean"}
}
})JSON",
        nullptr,
    },
    {
        "measurement.length",
        "query",
        "readOnly",
        "Berechnet Kurvenlaengen fuer Entities per Selector.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "includeObjects":{"type":"boolean"}
}
})JSON",
        nullptr,
    },
    {
        "measurement.area",
        "query",
        "readOnly",
        "Berechnet Flaechenwerte fuer geschlossene Kurven und Kreise per Selector.",
        R"JSON({
"type":"object",
"required":["selector"],
"properties":{
  "selector":{"type":"object"},
  "includeObjects":{"type":"boolean"}
}
})JSON",
        nullptr,
    },
    {
        "undo.redo",
        "action",
        "modifiesDrawing",
        "Fuehrt REDO aus, wenn BricsCAD in der aktuellen Sitzung Redo erlaubt.",
        R"JSON({
"type":"object",
"properties":{
  "steps":{"type":"number","minimum":1},
  "saveBefore":{"type":"boolean"},
  "reason":{"type":"string"}
}
})JSON",
        R"JSON({
"method":"undo.redo",
"required":[],
"bodySchema":{"type":"object","properties":{"steps":"number >= 1; default 1","saveBefore":"boolean default true"}},
"examples":[{"steps":1}]
})JSON",
    },
};

const BridgeCommandDescriptor kBridgeCommands[] = {
    {"RECTANGLE", "Zeichnet ein Rechteck ueber zwei Eckpunkte oder Optionen.",
     R"({"commandLine":true})"},
    {"EXTRUDE", "Extrudiert 2D-Profile zu 3D-Solids.",
     R"({"selectionSet":true,"commandLine":true,"params":{"heightMm":{"type":"number","unit":"mm","required":true}}})"},
    {"MOVE", "Verschiebt Entities.",
     R"({"selectionSet":true,"commandLine":true,"bridgeTool":"geometry.move"})"},
    {"COPY", "Kopiert Entities.",
     R"({"selectionSet":true,"commandLine":true,"bridgeTool":"geometry.copy"})"},
    {"ROTATE", "Rotiert Entities.",
     R"({"selectionSet":true,"commandLine":true,"bridgeTool":"geometry.rotate"})"},
    {"SCALE", "Skaliert Entities uniform. Nicht fuer einachsiges Verlaengern verwenden.",
     R"({"selectionSet":true,"commandLine":true,"bridgeTool":"geometry.scale","limits":"uniform factor only; not for one-axis extension"})"},
    {"ERASE", "Loescht Entities.",
     R"({"selectionSet":true,"commandLine":true,"bridgeTool":"geometry.delete"})"},
    {"LAYER", "Verwaltet Layer.",
     R"({"commandLine":true,"bridgeTool":"layers.create|layers.rename|layers.setColor"})"},
    {"SAVE", "Speichert die Zeichnung.",
     R"({"commandLine":true,"bridgeTool":"document.save"})"},
    {"UNDO", "Macht zuletzt ausgefuehrte Aktionen rueckgaengig.",
     R"({"selectionSet":false,"commandLine":true,"params":{"steps":{"type":"number","minimum":1,"required":false}}})"},
    {"REDO", "Stellt rueckgaengig gemachte Aktion wieder her.",
     R"({"selectionSet":false,"commandLine":true,"bridgeTool":"undo.redo"})"},
    {"BIMCLASSIFY", "Klassifiziert Entities als BIM-Elemente.",
     R"({"selectionSet":true,"commandLine":true,"params":{"classification":{"enum":["BIMWall"],"required":true}},"options":[{"option":"Wall","classification":"BIMWall"},{"option":"Column","classification":"BIMColumn"},{"option":"Slab","classification":"BIMSlab"},{"option":"Beam","classification":"BIMBeam"}]})"},
    {"UNION", "Vereint 3D-Solids.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"SUBTRACT", "Subtrahiert 3D-Solids.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"INTERSECT", "Bildet Schnittmenge von 3D-Solids.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"SWEEP", "Swept Profil entlang eines Pfads.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"LOFT", "Erzeugt Flaechen/Solids aus Profilen.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"OFFSET", "Versetzt Kurven/Polylinien.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"TRIM", "Trimmt Geometrie.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"EXTEND", "Verlaengert Geometrie.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"FILLET", "Erzeugt Rundungen.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
    {"CHAMFER", "Erzeugt Fasen.",
     R"({"selectionSet":true,"commandLine":true,"status":"contextOnly"})"},
};

std::size_t bridgeMethodCount()
{
    return sizeof(kBridgeMethods) / sizeof(kBridgeMethods[0]);
}

std::size_t bridgeCommandCount()
{
    return sizeof(kBridgeCommands) / sizeof(kBridgeCommands[0]);
}

const char* methodCategory(const char* name)
{
    const std::string method = name == nullptr ? std::string() : std::string(name);
    if (method.rfind("geometry.", 0) == 0 || method.rfind("profile.", 0) == 0) {
        return "geometry";
    }
    if (method.rfind("selection.", 0) == 0) {
        return "selection";
    }
    if (method.rfind("layers.", 0) == 0) {
        return "layer";
    }
    if (method.rfind("measurement.", 0) == 0) {
        return "analysis";
    }
    if (method.rfind("document.", 0) == 0) {
        return "document";
    }
    if (method.rfind("undo.", 0) == 0) {
        return "undo";
    }
    if (method.rfind("bim.", 0) == 0) {
        return "bim";
    }
    return "bridge";
}

void appendMethodDescriptor(std::ostringstream& out, const BridgeMethodDescriptor& method, bool isLast)
{
    out << "{\"name\":\"" << jsonEscape(method.name)
        << "\",\"kind\":\"" << jsonEscape(method.kind)
        << "\",\"risk\":\"" << jsonEscape(method.risk)
        << "\",\"category\":\"" << jsonEscape(methodCategory(method.name))
        << "\",\"description\":\"" << jsonEscape(method.description) << "\"";
    if (method.paramsSchema != nullptr) {
        out << ",\"paramsSchema\":" << method.paramsSchema;
    }
    if (method.apiPost != nullptr) {
        out << ",\"apiDoc\":{\"transport\":\"bridge-json\",\"post\":" << method.apiPost << "}";
    }
    out << ",\"resultSchema\":\"barebone.bricscad." << jsonEscape(method.name) << ".result.v1\"";
    if (std::strcmp(method.risk, "modifiesDrawing") == 0) {
        out << ",\"failureReasons\":[\"invalidParams\",\"emptySelector\",\"documentLockFailed\",\"brxApiError\"]";
    }
    out << "}" << (isLast ? "" : ",");
}

void appendCommandDescriptor(std::ostringstream& out, const BridgeCommandDescriptor& command, bool isLast)
{
    out << "{\"name\":\"" << jsonEscape(command.name)
        << "\",\"description\":\"" << jsonEscape(command.description) << "\"";
    if (command.json != nullptr) {
        const std::string fragment = command.json;
        const std::size_t first = fragment.find_first_not_of(" \t\r\n");
        const std::size_t last = fragment.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && fragment.at(first) == '{' && fragment.at(last) == '}') {
            out << "," << fragment.substr(first + 1, last - first - 1);
        } else {
            out << "," << fragment;
        }
    }
    out << "}" << (isLast ? "" : ",");
}

struct SelectionEntitySnapshot {
    std::string handle;
    std::string type;
    std::string layer;
};

struct JsonPoint3d;

void captureCurrentSelection(const char* reason);
void rememberSelectionSnapshot(const AcDbObjectIdArray& ids, const char* reason, std::vector<std::string>* debugLines = nullptr);
void sendSelectionSnapshotEvent(const std::vector<SelectionEntitySnapshot>& snapshot);
AcDbObjectIdArray lastExtrudedSolidIds();
AcDbObjectIdArray lastResultObjectIds();
int runNativeExtrudeCommand(const ads_name selectionSet, double heightMm, std::vector<std::string>& debugLines);
int runNativeMoveCommand(const ads_name selectionSet, const JsonPoint3d& vector, std::vector<std::string>& debugLines);

class BridgeSelectionReactor final : public AcEditorReactor {
public:
    void pickfirstModified() override
    {
        captureCurrentSelection("pickfirstModified");
    }
};

std::mutex g_selectionSnapshotMutex;
std::vector<SelectionEntitySnapshot> g_lastSelectionSnapshot;
std::string g_lastSelectionSignature;
std::unique_ptr<BridgeSelectionReactor> g_selectionReactor;
std::mutex g_lastExtrudedSolidsMutex;
std::vector<AcDbObjectId> g_lastExtrudedSolidIds;
std::string g_lastExtrudedLayer;
std::mutex g_lastResultMutex;
std::vector<AcDbObjectId> g_lastResultIds;
std::string g_lastResultLabel;

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string toUpperAscii(std::string value)
{
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::vector<std::string> splitTabs(const std::string& value)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('\t', start);
        if (end == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

int hexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(const std::string& value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::string percentEncode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        flags = 0;
        size = MultiByteToWideChar(CP_UTF8, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const wchar_t* value)
{
    if (value == nullptr || value[0] == L'\0') {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring asciiToWide(const std::string& value)
{
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char c : value) {
        result.push_back(c < 128 ? static_cast<wchar_t>(c) : L'?');
    }
    return result;
}

std::wstring bridgeLogTimestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

void appendTextToEdit(HWND edit, const std::wstring& text)
{
    if (edit == nullptr) {
        return;
    }

    const int textLength = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(textLength), static_cast<LPARAM>(textLength));
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

LRESULT CALLBACK bridgeLogWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_bridgeLogEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0,
            0,
            0,
            0,
            window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (g_bridgeLogEdit != nullptr) {
            SendMessageW(g_bridgeLogEdit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        return 0;
    }
    case WM_SIZE:
        if (g_bridgeLogEdit != nullptr) {
            MoveWindow(g_bridgeLogEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case kBridgeLogAppendMessage: {
        auto* line = reinterpret_cast<std::wstring*>(lParam);
        if (line != nullptr) {
            appendTextToEdit(g_bridgeLogEdit, *line);
            delete line;
        }
        return 0;
    }
    case WM_DESTROY:
        if (window == g_bridgeLogWindow) {
            g_bridgeLogWindow = nullptr;
            g_bridgeLogEdit = nullptr;
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool registerBridgeLogWindowClass()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = bridgeLogWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kBridgeLogWindowClassName;

    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void appendBridgeUiLog(const std::string& message)
{
    HWND logWindow = g_bridgeLogWindow;
    if (logWindow == nullptr) {
        return;
    }

    std::wstring line = L"[";
    line += bridgeLogTimestamp();
    line += L"] ";
    line += utf8ToWide(message);
    line += L"\r\n";

    auto* postedLine = new std::wstring(std::move(line));
    if (!PostMessageW(logWindow, kBridgeLogAppendMessage, 0, reinterpret_cast<LPARAM>(postedLine))) {
        delete postedLine;
    }
}

void showBridgeLogWindow()
{
    if (g_bridgeLogWindow == nullptr) {
        if (!registerBridgeLogWindowClass()) {
            return;
        }

        HWND owner = adsw_acadMainWnd();
        g_bridgeLogWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kBridgeLogWindowClassName,
            L"Barebone-Qt Bridge Log",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            620,
            360,
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
    }

    if (g_bridgeLogWindow != nullptr) {
        ShowWindow(g_bridgeLogWindow, SW_SHOWNORMAL);
        UpdateWindow(g_bridgeLogWindow);
    }
}

void destroyBridgeLogWindow()
{
    if (g_bridgeLogWindow != nullptr) {
        DestroyWindow(g_bridgeLogWindow);
        g_bridgeLogWindow = nullptr;
        g_bridgeLogEdit = nullptr;
    }
}

std::string acharToUtf8(const ACHAR* value)
{
#ifdef UNICODE
    return wideToUtf8(value);
#else
    return value == nullptr ? std::string() : std::string(value);
#endif
}

std::basic_string<ACHAR> utf8ToAchar(const std::string& value)
{
#ifdef UNICODE
    return utf8ToWide(value);
#else
    return value;
#endif
}

std::string errorStatusText(Acad::ErrorStatus status)
{
    std::ostringstream result;
    result << acharToUtf8(acadErrorStatusText(status)) << " (" << static_cast<int>(status) << ")";
    return result.str();
}

std::string objectHandleText(const AcDbObjectId& objectId)
{
    if (objectId.isNull()) {
        return "<null>";
    }

    ACHAR handleBuffer[AcDbHandle::kStrSiz]{};
    if (!objectId.handle().getIntoAsciiBuffer(handleBuffer)) {
        return "<handle unavailable>";
    }
    return acharToUtf8(handleBuffer);
}

std::string entityTypeName(const AcDbEntity* entity)
{
    if (entity == nullptr || entity->isA() == nullptr || entity->isA()->name() == nullptr) {
        return "AcDbEntity";
    }
    return acharToUtf8(entity->isA()->name());
}

std::string entityLayerName(const AcDbEntity* entity)
{
    if (entity == nullptr || entity->layerId().isNull()) {
        return "<kein Layer>";
    }

    AcDbLayerTableRecord* layer = nullptr;
    const Acad::ErrorStatus status = acdbOpenObject(layer, entity->layerId(), AcDb::kForRead);
    if (status != Acad::eOk || layer == nullptr) {
        return "<Layer nicht lesbar>";
    }

    const ACHAR* name = nullptr;
    std::string result = "<Layer ohne Name>";
    if (layer->getName(name) == Acad::eOk && name != nullptr) {
        result = acharToUtf8(name);
    }
    layer->close();
    return result;
}

void appendDebug(std::vector<std::string>& debugLines, const std::string& message);

bool setEntityLayerByName(const AcDbObjectId& entityId, const std::string& layerName, std::vector<std::string>& debugLines)
{
    if (entityId.isNull() || layerName.empty()) {
        return true;
    }

    AcDbEntity* entity = nullptr;
    const Acad::ErrorStatus openStatus = acdbOpenObject(entity, entityId, AcDb::kForWrite);
    if (openStatus != Acad::eOk || entity == nullptr) {
        appendDebug(debugLines, "setCreatedEntityLayer open handle=" + objectHandleText(entityId) + ": " + errorStatusText(openStatus));
        return false;
    }

    const std::basic_string<ACHAR> nativeLayer = utf8ToAchar(layerName);
    const Acad::ErrorStatus layerStatus = entity->setLayer(nativeLayer.c_str());
    appendDebug(debugLines, "setCreatedEntityLayer handle=" + objectHandleText(entityId)
        + " layer='" + layerName + "': " + errorStatusText(layerStatus));
    entity->close();
    return layerStatus == Acad::eOk;
}

bool setEntitiesLayerByName(const AcDbObjectIdArray& entityIds, const std::string& layerName, std::vector<std::string>& debugLines)
{
    if (layerName.empty()) {
        return true;
    }

    bool ok = true;
    for (int i = 0; i < entityIds.length(); ++i) {
        ok = setEntityLayerByName(entityIds.at(i), layerName, debugLines) && ok;
    }
    return ok;
}

std::vector<SelectionEntitySnapshot> readPickfirstSelectionSnapshot()
{
    std::vector<SelectionEntitySnapshot> snapshot;

    ads_name selectionSet{};
    const int getStatus = acedSSGet(_T("_I"), nullptr, nullptr, nullptr, selectionSet);
    if (getStatus != RTNORM) {
        return snapshot;
    }

    Adesk::Int32 selectionLength = 0;
    if (acedSSLength(selectionSet, &selectionLength) != RTNORM || selectionLength <= 0) {
        acedSSFree(selectionSet);
        return snapshot;
    }

    snapshot.reserve(static_cast<std::size_t>(selectionLength));
    for (Adesk::Int32 i = 0; i < selectionLength; ++i) {
        ads_name entityName{};
        if (acedSSName(selectionSet, static_cast<int>(i), entityName) != RTNORM) {
            continue;
        }

        AcDbObjectId entityId;
        if (acdbGetObjectId(entityId, entityName) != Acad::eOk || entityId.isNull()) {
            continue;
        }

        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
        if (openStatus != Acad::eOk || entity == nullptr) {
            continue;
        }

        SelectionEntitySnapshot item;
        item.handle = objectHandleText(entityId);
        item.type = entityTypeName(entity);
        item.layer = entityLayerName(entity);
        snapshot.push_back(std::move(item));
        entity->close();
    }

    acedSSFree(selectionSet);
    return snapshot;
}

std::string selectionSignature(const std::vector<SelectionEntitySnapshot>& snapshot)
{
    std::ostringstream signature;
    for (const SelectionEntitySnapshot& item : snapshot) {
        signature << item.handle << '|' << item.type << '|' << item.layer << '\n';
    }
    return signature.str();
}

void rememberSelectionSnapshot(const AcDbObjectIdArray& ids, const char* reason, std::vector<std::string>* debugLines)
{
    std::vector<SelectionEntitySnapshot> snapshot;
    snapshot.reserve(static_cast<std::size_t>(ids.length()));
    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId entityId = ids.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
        if (openStatus != Acad::eOk || entity == nullptr) {
            if (debugLines != nullptr) {
                appendDebug(*debugLines, "selection cache open failed handle=" + objectHandleText(entityId) + ": " + errorStatusText(openStatus));
            }
            continue;
        }

        SelectionEntitySnapshot item;
        item.handle = objectHandleText(entityId);
        item.type = entityTypeName(entity);
        item.layer = entityLayerName(entity);
        snapshot.push_back(std::move(item));
        entity->close();
    }

    {
        std::lock_guard<std::mutex> lock(g_selectionSnapshotMutex);
        g_lastSelectionSignature = selectionSignature(snapshot);
        g_lastSelectionSnapshot = snapshot;
    }

    if (debugLines != nullptr) {
        appendDebug(*debugLines, "Cached BBTRACK selection snapshot updated from "
            + std::string(reason != nullptr ? reason : "unknown")
            + " size=" + std::to_string(snapshot.size()));
    }
    sendSelectionSnapshotEvent(snapshot);
}

void captureCurrentSelection(const char* reason)
{
    std::vector<SelectionEntitySnapshot> snapshot = readPickfirstSelectionSnapshot();
    const std::string signature = selectionSignature(snapshot);

    {
        std::lock_guard<std::mutex> lock(g_selectionSnapshotMutex);
        if (signature == g_lastSelectionSignature) {
            return;
        }
        g_lastSelectionSignature = signature;
        g_lastSelectionSnapshot = snapshot;
    }

    std::ostringstream logLine;
    logLine << "BricsCAD Auswahl: " << snapshot.size() << " Geometrien";
    if (reason != nullptr && reason[0] != '\0') {
        logLine << " (" << reason << ")";
    }
    if (!snapshot.empty()) {
        logLine << ": ";
        const std::size_t previewCount = std::min<std::size_t>(snapshot.size(), 5);
        for (std::size_t i = 0; i < previewCount; ++i) {
            if (i > 0) {
                logLine << "; ";
            }
            logLine << snapshot[i].type
                << " handle=" << snapshot[i].handle
                << " layer=" << snapshot[i].layer;
        }
        if (snapshot.size() > previewCount) {
            logLine << "; +" << (snapshot.size() - previewCount) << " weitere";
        }
    }
    appendBridgeUiLog(logLine.str());
    sendSelectionSnapshotEvent(snapshot);
}

void startSelectionTracking()
{
    AcEditor* editor = acedEditor;
    if (g_selectionReactor != nullptr || editor == nullptr) {
        return;
    }

    g_selectionReactor = std::make_unique<BridgeSelectionReactor>();
    editor->addReactor(g_selectionReactor.get());
    appendBridgeUiLog("BricsCAD Auswahltracking aktiv, wartet auf Auswahlinteraktion");
}

void stopSelectionTracking()
{
    AcEditor* editor = acedEditor;
    if (g_selectionReactor != nullptr && editor != nullptr) {
        editor->removeReactor(g_selectionReactor.get());
    }
    g_selectionReactor.reset();

    std::lock_guard<std::mutex> lock(g_selectionSnapshotMutex);
    g_lastSelectionSnapshot.clear();
    g_lastSelectionSignature.clear();
}

void printBrxDebug(const std::string& message)
{
#ifdef UNICODE
    const std::wstring wideMessage = utf8ToWide(message);
    acutPrintf(_T("\nBarebone-Qt BRX Debug: %ls"), wideMessage.c_str());
#else
    acutPrintf("\nBarebone-Qt BRX Debug: %s", message.c_str());
#endif
}

std::string brxDebugLogPath()
{
    char tempPath[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) {
        return "BareboneQtBrx.log";
    }
    return std::string(tempPath) + "BareboneQtBrx.log";
}

void resetBrxDebugLog()
{
    std::ofstream logFile(brxDebugLogPath(), std::ios::out | std::ios::trunc);
    if (logFile) {
        logFile << "Barebone-Qt BRX Debug Log\n";
    }
}

void writeBrxDebugLog(const std::string& message)
{
    std::ofstream logFile(brxDebugLogPath(), std::ios::out | std::ios::app);
    if (logFile) {
        logFile << message << '\n';
    }
}

void appendDebug(std::vector<std::string>& debugLines, const std::string& message)
{
    debugLines.push_back(message);
    printBrxDebug(message);
    writeBrxDebugLog(message);
}

void appendDebugResponse(std::ostringstream& response, const std::vector<std::string>& debugLines)
{
    for (const std::string& line : debugLines) {
        response << "DEBUG\t" << percentEncode(line) << "\n";
    }
}

void finishBridgeJob(BridgeJob* job, std::string response)
{
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->response = std::move(response);
        job->done = true;
    }
    job->doneEvent.notify_one();
}

bool commandNameInList(const std::string& command, const char* const* commands, std::size_t commandCount)
{
    for (std::size_t i = 0; i < commandCount; ++i) {
        if (command == commands[i]) {
            return true;
        }
    }
    return false;
}

bool requiresCommandContext(const std::string& request)
{
    const std::vector<std::string> parts = splitTabs(request);
    if (parts.empty()) {
        return false;
    }
    const std::string command = toUpperAscii(parts.front());
    static constexpr const char* kCommandContextRequests[] = {
        "POSTCOMMAND",
        "GEOMETRYCREATE",
        "GEOMETRYMOVE",
        "GEOMETRYCOPY",
        "GEOMETRYROTATE",
        "GEOMETRYSCALE",
        "GEOMETRYDELETE",
        "SELECTIONSET",
        "ENTITYSETLAYER",
        "LAYERCREATE",
        "LAYERRENAME",
        "LAYERSETCOLOR",
        "LAYERBATCH",
        "DOCUMENTSAVE",
        "PROFILEEXTRUDE",
        "EXTRUDE",
        "EXTRUDESELECTOR",
        "BIMCLASSIFY",
        "BIMCLASSIFYSELECTOR",
        "UNDO",
        "REDO",
        "PIPESCREATESOLIDS",
        "ROOMDIMENSIONS",
    };
    static constexpr const char* kApplicationContextRequests[] = {
        "LAYERS",
        "ACTIONVALIDATE",
        "GEOMETRYQUERY",
        "SELECTIONDESCRIBE",
        "ENTITYDESCRIBE",
        "MEASUREBBOX",
        "MEASURELENGTH",
        "MEASUREAREA",
        "PIPESVALIDATE",
    };
    if (commandNameInList(command, kCommandContextRequests, sizeof(kCommandContextRequests) / sizeof(kCommandContextRequests[0]))) {
        return true;
    }
    if (commandNameInList(command, kApplicationContextRequests, sizeof(kApplicationContextRequests) / sizeof(kApplicationContextRequests[0]))) {
        return false;
    }
    return false;
}

void appendUtf8(std::string& output, unsigned int codepoint)
{
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream result;
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            result << "\\\"";
            break;
        case '\\':
            result << "\\\\";
            break;
        case '\b':
            result << "\\b";
            break;
        case '\f':
            result << "\\f";
            break;
        case '\n':
            result << "\\n";
            break;
        case '\r':
            result << "\\r";
            break;
        case '\t':
            result << "\\t";
            break;
        default:
            if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                result << "\\u00" << kHex[c >> 4] << kHex[c & 0x0F];
            } else {
                result << static_cast<char>(c);
            }
            break;
        }
    }
    return result.str();
}

void skipJsonWhitespace(const std::string& text, std::size_t& pos)
{
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
}

int jsonHexValue(char value)
{
    return hexValue(value);
}

bool parseJsonStringAt(const std::string& text, std::size_t& pos, std::string& output)
{
    skipJsonWhitespace(text, pos);
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;

    output.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            output.push_back(c);
            continue;
        }
        if (pos >= text.size()) {
            return false;
        }

        const char escaped = text[pos++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
            break;
        case 'b':
            output.push_back('\b');
            break;
        case 'f':
            output.push_back('\f');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        case 'u': {
            if (pos + 4 > text.size()) {
                return false;
            }
            unsigned int codepoint = 0;
            for (int i = 0; i < 4; ++i) {
                const int digit = jsonHexValue(text[pos++]);
                if (digit < 0) {
                    return false;
                }
                codepoint = (codepoint << 4) | static_cast<unsigned int>(digit);
            }
            appendUtf8(output, codepoint);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

bool skipJsonValue(const std::string& text, std::size_t& pos)
{
    skipJsonWhitespace(text, pos);
    if (pos >= text.size()) {
        return false;
    }

    if (text[pos] == '"') {
        std::string ignored;
        return parseJsonStringAt(text, pos, ignored);
    }

    if (text[pos] == '{' || text[pos] == '[') {
        const char open = text[pos];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        while (pos < text.size()) {
            if (text[pos] == '"') {
                std::string ignored;
                if (!parseJsonStringAt(text, pos, ignored)) {
                    return false;
                }
                continue;
            }
            if (text[pos] == open) {
                ++depth;
            } else if (text[pos] == close) {
                --depth;
                ++pos;
                if (depth == 0) {
                    return true;
                }
                continue;
            }
            ++pos;
        }
        return false;
    }

    while (pos < text.size()
        && text[pos] != ','
        && text[pos] != '}'
        && text[pos] != ']'
        && std::isspace(static_cast<unsigned char>(text[pos])) == 0) {
        ++pos;
    }
    return true;
}

std::optional<std::size_t> findJsonPropertyValue(const std::string& object, const std::string& key)
{
    std::size_t pos = 0;
    skipJsonWhitespace(object, pos);
    if (pos >= object.size() || object[pos] != '{') {
        return std::nullopt;
    }
    ++pos;

    while (pos < object.size()) {
        skipJsonWhitespace(object, pos);
        if (pos < object.size() && object[pos] == '}') {
            return std::nullopt;
        }

        std::string currentKey;
        if (!parseJsonStringAt(object, pos, currentKey)) {
            return std::nullopt;
        }
        skipJsonWhitespace(object, pos);
        if (pos >= object.size() || object[pos] != ':') {
            return std::nullopt;
        }
        ++pos;
        skipJsonWhitespace(object, pos);

        if (currentKey == key) {
            return pos;
        }
        if (!skipJsonValue(object, pos)) {
            return std::nullopt;
        }
        skipJsonWhitespace(object, pos);
        if (pos < object.size() && object[pos] == ',') {
            ++pos;
        }
    }

    return std::nullopt;
}

std::optional<std::string> jsonStringProperty(const std::string& object, const std::string& key)
{
    const std::optional<std::size_t> valuePos = findJsonPropertyValue(object, key);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }

    std::size_t pos = *valuePos;
    std::string value;
    if (!parseJsonStringAt(object, pos, value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> jsonDoubleProperty(const std::string& object, const std::string& key)
{
    const std::optional<std::size_t> valuePos = findJsonPropertyValue(object, key);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }

    const char* begin = object.c_str() + *valuePos;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> jsonBoolProperty(const std::string& object, const std::string& key)
{
    const std::optional<std::size_t> valuePos = findJsonPropertyValue(object, key);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }

    std::size_t pos = *valuePos;
    skipJsonWhitespace(object, pos);
    if (object.compare(pos, 4, "true") == 0) {
        return true;
    }
    if (object.compare(pos, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<int> jsonIntProperty(const std::string& object, const std::string& key)
{
    const std::optional<double> value = jsonDoubleProperty(object, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

std::optional<std::string> jsonObjectProperty(const std::string& object, const std::string& key)
{
    const std::optional<std::size_t> valuePos = findJsonPropertyValue(object, key);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }

    std::size_t end = *valuePos;
    if (!skipJsonValue(object, end)) {
        return std::nullopt;
    }
    return object.substr(*valuePos, end - *valuePos);
}

std::optional<std::string> jsonArrayProperty(const std::string& object, const std::string& key)
{
    const std::optional<std::size_t> valuePos = findJsonPropertyValue(object, key);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }

    std::size_t pos = *valuePos;
    skipJsonWhitespace(object, pos);
    if (pos >= object.size() || object[pos] != '[') {
        return std::nullopt;
    }

    std::size_t end = pos;
    if (!skipJsonValue(object, end)) {
        return std::nullopt;
    }
    return object.substr(pos, end - pos);
}

std::vector<std::string> jsonStringArrayValues(const std::string& arrayText)
{
    std::vector<std::string> values;
    std::size_t pos = 0;
    skipJsonWhitespace(arrayText, pos);
    if (pos >= arrayText.size() || arrayText[pos] != '[') {
        return values;
    }
    ++pos;

    while (pos < arrayText.size()) {
        skipJsonWhitespace(arrayText, pos);
        if (pos < arrayText.size() && arrayText[pos] == ']') {
            break;
        }

        std::string value;
        if (!parseJsonStringAt(arrayText, pos, value)) {
            break;
        }
        values.push_back(value);
        skipJsonWhitespace(arrayText, pos);
        if (pos < arrayText.size() && arrayText[pos] == ',') {
            ++pos;
        }
    }
    return values;
}

std::vector<double> jsonNumberArrayValues(const std::string& arrayText)
{
    std::vector<double> values;
    std::size_t pos = 0;
    skipJsonWhitespace(arrayText, pos);
    if (pos >= arrayText.size() || arrayText[pos] != '[') {
        return values;
    }
    ++pos;

    while (pos < arrayText.size()) {
        skipJsonWhitespace(arrayText, pos);
        if (pos < arrayText.size() && arrayText[pos] == ']') {
            break;
        }

        const char* begin = arrayText.c_str() + pos;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin) {
            break;
        }
        values.push_back(value);
        pos = static_cast<std::size_t>(end - arrayText.c_str());
        skipJsonWhitespace(arrayText, pos);
        if (pos < arrayText.size() && arrayText[pos] == ',') {
            ++pos;
        }
    }
    return values;
}

std::vector<std::string> jsonObjectArrayValues(const std::string& arrayText)
{
    std::vector<std::string> values;
    std::size_t pos = 0;
    skipJsonWhitespace(arrayText, pos);
    if (pos >= arrayText.size() || arrayText[pos] != '[') {
        return values;
    }
    ++pos;

    while (pos < arrayText.size()) {
        skipJsonWhitespace(arrayText, pos);
        if (pos < arrayText.size() && arrayText[pos] == ']') {
            break;
        }
        if (pos >= arrayText.size() || arrayText[pos] != '{') {
            break;
        }
        const std::size_t begin = pos;
        if (!skipJsonValue(arrayText, pos)) {
            break;
        }
        values.push_back(arrayText.substr(begin, pos - begin));
        skipJsonWhitespace(arrayText, pos);
        if (pos < arrayText.size() && arrayText[pos] == ',') {
            ++pos;
        }
    }
    return values;
}

std::vector<std::string> splitLines(const std::string& value)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < value.size()) {
        std::size_t end = value.find('\n', start);
        if (end == std::string::npos) {
            end = value.size();
        }
        std::string line = trim(value.substr(start, end - start));
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        start = end + 1;
    }
    return lines;
}

std::string jsonDebugArray(const std::vector<std::string>& lines)
{
    std::ostringstream result;
    result << '[';
    bool first = true;
    for (const std::string& line : lines) {
        if (!first) {
            result << ',';
        }
        first = false;
        result << '"' << jsonEscape(line) << '"';
    }
    result << ']';
    return result.str();
}

std::vector<std::string> protocolDebugLines(const std::vector<std::string>& lines)
{
    std::vector<std::string> debugLines;
    for (const std::string& line : lines) {
        if (line.rfind("DEBUG\t", 0) == 0) {
            debugLines.push_back(percentDecode(line.substr(6)));
        }
    }
    return debugLines;
}

std::string jsonErrorResponse(int id, const std::string& message, const std::vector<std::string>& debugLines = {})
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":false"
        << ",\"error\":\"" << jsonEscape(message) << "\""
        << ",\"debug\":" << jsonDebugArray(debugLines)
        << "}\n";
    return response.str();
}

std::string jsonCapabilitiesResponse(int id)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":true"
        << ",\"result\":{"
        << "\"schema\":\"barebone.bricscad.capabilities.v1\","
        << "\"methods\":["
        << "{\"name\":\"capabilities.list\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Liefert die verfuegbaren BRX-Bridge-Methoden und Schemas.\"},"
        << "{\"name\":\"actions.list\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Liefert die dokumentierten Barebone-BRX API-Aktionen mit POST-Optionen, Pflichtfeldern und Beispielen.\"},"
        << "{\"name\":\"commands.list\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Liefert eine Low-Level-Startliste nativer BricsCAD-Kommandos und zugehoeriger Bridge-Tools.\"},"
        << "{\"name\":\"layers.list\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Listet Layer der aktiven Zeichnung.\"},"
        << "{\"name\":\"geometry.query\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Liest aktuelle Geometrien aus der Zeichnung anhand eines Selectors und optionaler Filter. Liefert bounds und dimensions(widthX, depthY, heightZ, unit=mm), sofern getGeomExtents verfuegbar ist.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"properties\":{\"selector\":{\"type\":\"object\",\"properties\":{\"scope\":{\"enum\":[\"currentSpace\",\"selection\",\"handles\",\"lastResult\",\"lastExtruded\"]},\"layer\":{\"type\":\"string\"},\"handles\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"kind\":{\"enum\":[\"polyline\",\"solid\",\"entity\"]},\"shape\":{\"enum\":[\"rectangle\"]}}},\"filters\":{\"type\":\"object\",\"properties\":{\"layer\":{\"type\":\"string\"},\"layers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"kind\":{\"enum\":[\"polyline\",\"solid\",\"entity\"]},\"kinds\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"shape\":{\"enum\":[\"rectangle\"]},\"shapes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"types\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}},\"include\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"limit\":{\"type\":\"number\",\"minimum\":1}}}},"
        << "{\"name\":\"selection.describe\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Beschreibt die aktuelle BricsCAD-Auswahl mit Handles, Typen, Layern und optionaler Geometrie.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"properties\":{\"include\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"limit\":{\"type\":\"number\",\"minimum\":1}}}},"
        << "{\"name\":\"entity.describe\",\"kind\":\"query\",\"risk\":\"readOnly\",\"description\":\"Beschreibt konkrete Entities per Handle-Liste.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"properties\":{\"handle\":{\"type\":\"string\"},\"handles\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"include\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"limit\":{\"type\":\"number\",\"minimum\":1}}}},"
        << "{\"name\":\"geometry.create\",\"kind\":\"action\",\"risk\":\"modifiesDrawing\",\"description\":\"Erzeugt neue Grundgeometrie direkt ueber die BRX API. Die Aktion ist atomar und benoetigt alle Zeichenoptionen im POST-Body.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"required\":[\"geometry\"],\"properties\":{\"geometry\":{\"enum\":[\"point\",\"rectangle\",\"line\",\"polyline\",\"circle\",\"arc\",\"box\"]},\"layer\":{\"type\":\"string\"},\"position\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},\"point\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},\"origin\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},\"center\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},\"radius\":{\"type\":\"number\"},\"radiusMm\":{\"type\":\"number\"},\"startAngle\":{\"type\":\"number\"},\"endAngle\":{\"type\":\"number\"},\"startAngleDeg\":{\"type\":\"number\"},\"endAngleDeg\":{\"type\":\"number\"},\"width\":{\"type\":\"number\"},\"widthMm\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"depth\":{\"type\":\"number\"},\"length\":{\"type\":\"number\"},\"lengthMm\":{\"type\":\"number\"},\"depthMm\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},\"heightMm\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"start\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},\"end\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},\"points\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},\"closed\":{\"type\":\"boolean\"},\"saveBefore\":{\"type\":\"boolean\"},\"reason\":{\"type\":\"string\"}}},"
        << "\"apiDoc\":{\"transport\":\"bridge-json\",\"post\":{\"method\":\"geometry.create\",\"required\":[\"geometry\"],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"geometry\":\"point|rectangle|line|polyline|circle|arc|box\",\"layer\":\"string optional\",\"position/point\":\"{x,y,z} for point\",\"origin\":\"{x,y,z} for rectangle/box\",\"center\":\"{x,y,z} for circle/arc\",\"radius\":\"circle/arc radius\",\"startAngleDeg/endAngleDeg\":\"arc angles in degrees\",\"width/x\":\"rectangle or box width\",\"depth/y\":\"rectangle or box depth/length\",\"height/z\":\"box height\",\"start\":\"{x,y,z} for line\",\"end\":\"{x,y,z} for line\",\"points\":\"array of {x,y,z} for polyline\",\"closed\":\"boolean for polyline\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"geometry\":\"box\",\"origin\":{\"x\":0,\"y\":0,\"z\":0},\"width\":100,\"depth\":100,\"height\":100,\"layer\":\"0\",\"saveBefore\":true},{\"geometry\":\"circle\",\"center\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":50,\"layer\":\"0\"},{\"geometry\":\"arc\",\"center\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":50,\"startAngleDeg\":0,\"endAngleDeg\":90},{\"geometry\":\"rectangle\",\"origin\":{\"x\":0,\"y\":0,\"z\":0},\"width\":100,\"depth\":100,\"layer\":\"0\",\"saveBefore\":true},{\"geometry\":\"line\",\"start\":{\"x\":0,\"y\":0,\"z\":0},\"end\":{\"x\":100,\"y\":0,\"z\":0},\"layer\":\"0\"},{\"geometry\":\"polyline\",\"points\":[{\"x\":0,\"y\":0,\"z\":0},{\"x\":100,\"y\":0,\"z\":0},{\"x\":100,\"y\":50,\"z\":0}],\"closed\":false},{\"geometry\":\"point\",\"position\":{\"x\":0,\"y\":0,\"z\":0}}]}}},"
        << "{\"name\":\"rectangles.extrude\",\"kind\":\"action\",\"risk\":\"modifiesDrawing\",\"description\":\"Extrudiert geschlossene Rechteck-Polylinien ueber die BRX API. Die Aktion ist atomar: Alle benoetigten Optionen stehen im POST-Body.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"required\":[\"heightMm\"],\"properties\":{\"layer\":{\"type\":\"string\",\"description\":\"Optionaler Layername, wenn kein selector verwendet wird.\"},\"selector\":{\"type\":\"object\",\"description\":\"Optionaler Selector fuer Auswahl, Handles oder aktuellen Raum.\",\"properties\":{\"scope\":{\"enum\":[\"currentSpace\",\"selection\",\"handles\"]},\"layer\":{\"type\":\"string\"},\"handles\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"kind\":{\"enum\":[\"rectangle\"]},\"shape\":{\"enum\":[\"rectangle\"]}}},\"heightMm\":{\"type\":\"number\",\"minimum\":0.1,\"description\":\"Extrusionshoehe in Millimetern.\"},\"detail\":{\"enum\":[\"summary\",\"element\",\"geometry\"]},\"saveBefore\":{\"type\":\"boolean\"},\"reason\":{\"type\":\"string\"}}},"
        << "\"apiDoc\":{\"transport\":\"bridge-json\",\"post\":{\"method\":\"rectangles.extrude\",\"required\":[\"heightMm\"],\"oneOfRequired\":[[\"layer\"],[\"selector\"]],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"layer\":\"string\",\"selector\":\"Selector\",\"heightMm\":\"number mm > 0\",\"detail\":\"summary|element|geometry\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"heightMm\":1000,\"selector\":{\"scope\":\"selection\",\"kind\":\"rectangle\"},\"saveBefore\":true},{\"heightMm\":3000,\"layer\":\"0\",\"detail\":\"element\",\"saveBefore\":true}]}}},"
        << "{\"name\":\"undo.last\",\"kind\":\"action\",\"risk\":\"modifiesDrawing\",\"description\":\"Macht bis zu mehreren letzten Aktionen rueckgaengig. BRX nutzt numerisches UNDO <steps> und keine Back/All-Variante mit Yes/No-Rueckfrage.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"properties\":{\"steps\":{\"type\":\"number\",\"minimum\":1,\"description\":\"Anzahl der Schritte fuer UNDO (Standard: 1).\"},\"saveBefore\":{\"type\":\"boolean\"},\"reason\":{\"type\":\"string\"}}},"
        << "\"apiDoc\":{\"transport\":\"bridge-json\",\"post\":{\"method\":\"undo.last\",\"required\":[],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"steps\":\"number >= 1 (default: 1)\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"steps\":1,\"saveBefore\":true}]}}},"
        << "{\"name\":\"bim.classify\",\"kind\":\"action\",\"risk\":\"modifiesDrawing\",\"description\":\"Klassifiziert 3D-Solids ueber die BRX API als BIM-Element. Die Aktion ist atomar und nutzt eine Zielauswahl oder zuletzt erzeugte Solids.\","
        << "\"paramsSchema\":{\"type\":\"object\",\"required\":[\"classification\"],\"properties\":{\"target\":{\"enum\":[\"lastExtruded\",\"selection\",\"handles\"]},\"selector\":{\"type\":\"object\",\"description\":\"Optionaler Selector fuer Auswahl, Handles, lastResult, lastExtruded oder aktuellen Raum.\",\"properties\":{\"scope\":{\"enum\":[\"selection\",\"handles\",\"lastResult\",\"lastExtruded\",\"currentSpace\"]},\"handles\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"kind\":{\"enum\":[\"solid\"]}}},\"classification\":{\"enum\":[\"BIMWall\"]},\"saveBefore\":{\"type\":\"boolean\"},\"reason\":{\"type\":\"string\"}}},"
        << "\"apiDoc\":{\"transport\":\"bridge-json\",\"post\":{\"method\":\"bim.classify\",\"required\":[\"classification\"],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"target\":\"lastExtruded|selection|handles\",\"selector\":\"Selector optional\",\"classification\":\"BIMWall\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"classification\":\"BIMWall\",\"target\":\"lastExtruded\",\"saveBefore\":true},{\"classification\":\"BIMWall\",\"selector\":{\"scope\":\"selection\",\"kind\":\"solid\"},\"saveBefore\":true}]}}}"
        << "],"
        << "\"commands\":["
        << "{\"name\":\"RECTANGLE\",\"description\":\"Zeichnet ein Rechteck ueber zwei Eckpunkte oder Optionen.\",\"commandLine\":true},"
        << "{\"name\":\"EXTRUDE\",\"description\":\"Extrudiert 2D-Profile zu 3D-Solids.\",\"selectionSet\":true,\"commandLine\":true,\"params\":{\"heightMm\":{\"type\":\"number\",\"unit\":\"mm\",\"required\":true}}},"
        << "{\"name\":\"UNDO\",\"description\":\"Macht zuletzt ausgefuehrte Aktionen rueckgaengig.\",\"selectionSet\":false,\"commandLine\":true,\"params\":{\"steps\":{\"type\":\"number\",\"minimum\":1,\"required\":false}}},"
        << "{\"name\":\"BIMCLASSIFY\",\"description\":\"Klassifiziert Entities als BIM-Elemente.\",\"selectionSet\":true,\"commandLine\":true,\"params\":{\"classification\":{\"enum\":[\"BIMWall\"],\"required\":true}},\"options\":[{\"option\":\"Wall\",\"classification\":\"BIMWall\"},{\"option\":\"Column\",\"classification\":\"BIMColumn\"},{\"option\":\"Slab\",\"classification\":\"BIMSlab\"},{\"option\":\"Beam\",\"classification\":\"BIMBeam\"}]}"
        << "],"
        << "\"selectorSchema\":{\"type\":\"object\",\"properties\":{\"scope\":{\"enum\":[\"currentSpace\",\"selection\",\"handles\",\"lastResult\",\"lastExtruded\"]},\"layer\":{\"type\":\"string\"},\"handles\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"kind\":{\"enum\":[\"polyline\",\"solid\",\"entity\"]},\"shape\":{\"enum\":[\"rectangle\"]}}}"
        << "},\"debug\":[]}\n";
    return response.str();
}

std::string jsonActionsResponse(int id)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":true"
        << ",\"result\":{\"schema\":\"barebone.bricscad.actions.v1\",\"description\":\"Dokumentierte Barebone-BRX API-Aktionen. Jede Aktion beschreibt den POST-Body vollstaendig, damit der Agent fehlende Optionen gezielt erfragen kann.\",\"actions\":["
        << "{\"name\":\"geometry.create\",\"description\":\"Erzeugt neue Grundgeometrie direkt in der Zeichnung.\",\"post\":{\"method\":\"geometry.create\",\"required\":[\"geometry\"],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"geometry\":\"point|rectangle|line|polyline|circle|arc|box\",\"layer\":\"string optional\",\"position/point\":\"{x,y,z} for point\",\"origin\":\"{x,y,z} for rectangle/box\",\"center\":\"{x,y,z} for circle/arc\",\"radius\":\"circle/arc radius\",\"startAngleDeg/endAngleDeg\":\"arc angles in degrees\",\"width/x\":\"rectangle or box width\",\"depth/y\":\"rectangle or box depth/length\",\"height/z\":\"box height\",\"start\":\"{x,y,z} for line\",\"end\":\"{x,y,z} for line\",\"points\":\"array of {x,y,z} for polyline\",\"closed\":\"boolean for polyline\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"geometry\":\"box\",\"origin\":{\"x\":0,\"y\":0,\"z\":0},\"width\":100,\"depth\":100,\"height\":100,\"layer\":\"0\",\"saveBefore\":true},{\"geometry\":\"circle\",\"center\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":50,\"layer\":\"0\"},{\"geometry\":\"arc\",\"center\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":50,\"startAngleDeg\":0,\"endAngleDeg\":90},{\"geometry\":\"rectangle\",\"origin\":{\"x\":0,\"y\":0,\"z\":0},\"width\":100,\"depth\":100,\"layer\":\"0\",\"saveBefore\":true},{\"geometry\":\"line\",\"start\":{\"x\":0,\"y\":0,\"z\":0},\"end\":{\"x\":100,\"y\":0,\"z\":0},\"layer\":\"0\"},{\"geometry\":\"polyline\",\"points\":[{\"x\":0,\"y\":0,\"z\":0},{\"x\":100,\"y\":0,\"z\":0},{\"x\":100,\"y\":50,\"z\":0}],\"closed\":false},{\"geometry\":\"point\",\"position\":{\"x\":0,\"y\":0,\"z\":0}}]}},"
        << "{\"name\":\"rectangles.extrude\",\"description\":\"Extrudiert geschlossene Rechteck-Polylinien.\",\"post\":{\"method\":\"rectangles.extrude\",\"required\":[\"heightMm\"],\"oneOfRequired\":[[\"layer\"],[\"selector\"]],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"layer\":\"string optional\",\"selector\":\"Selector optional\",\"heightMm\":\"number mm > 0\",\"detail\":\"summary|element|geometry\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"heightMm\":1000,\"selector\":{\"scope\":\"selection\",\"kind\":\"rectangle\"},\"saveBefore\":true},{\"heightMm\":3000,\"layer\":\"0\",\"detail\":\"element\",\"saveBefore\":true}]}},"
        << "{\"name\":\"undo.last\",\"description\":\"Macht bis zu mehreren letzten Aktionen rueckgaengig; nutzt numerisches UNDO <steps> ohne Back/All-Yes-No-Rueckfrage.\",\"post\":{\"method\":\"undo.last\",\"required\":[],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"steps\":\"number >=1\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"steps\":1,\"saveBefore\":true}]}},"
        << "{\"name\":\"bim.classify\",\"description\":\"Klassifiziert 3D-Solids als BIM-Element.\",\"post\":{\"method\":\"bim.classify\",\"required\":[\"classification\"],\"bodySchema\":{\"type\":\"object\",\"properties\":{\"target\":\"lastExtruded|selection|handles\",\"selector\":\"Selector optional\",\"classification\":\"BIMWall\",\"saveBefore\":\"boolean default true\"}},\"examples\":[{\"classification\":\"BIMWall\",\"target\":\"lastExtruded\",\"saveBefore\":true},{\"classification\":\"BIMWall\",\"selector\":{\"scope\":\"selection\",\"kind\":\"solid\"},\"saveBefore\":true}]}}"
        << "]},\"debug\":[]}\n";
    return response.str();
}

std::string jsonCommandsResponse(int id)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":true"
        << ",\"result\":{\"schema\":\"barebone.bricscad.commands.v1\",\"description\":\"Freigegebene native BricsCAD-Kommandos fuer validiertes command.execute. command.execute akzeptiert nur einzelne vollstaendige Kommandozeilen aus dieser Liste.\",\"commands\":["
        << "{\"name\":\"RECTANGLE\",\"description\":\"Zeichnet ein Rechteck ueber zwei Eckpunkte oder Optionen.\",\"commandLine\":true},"
        << "{\"name\":\"EXTRUDE\",\"description\":\"Extrudiert 2D-Profile zu 3D-Solids.\",\"selectionSet\":true,\"commandLine\":true,\"params\":{\"heightMm\":{\"type\":\"number\",\"unit\":\"mm\",\"required\":true}}},"
        << "{\"name\":\"UNDO\",\"description\":\"Macht zuletzt ausgefuehrte Aktionen rueckgaengig.\",\"selectionSet\":false,\"commandLine\":true,\"params\":{\"steps\":{\"type\":\"number\",\"minimum\":1,\"required\":false}}},"
        << "{\"name\":\"BIMCLASSIFY\",\"description\":\"Klassifiziert Entities als BIM-Elemente.\",\"selectionSet\":true,\"commandLine\":true,\"params\":{\"classification\":{\"enum\":[\"BIMWall\"],\"required\":true}},\"options\":[{\"option\":\"Wall\",\"classification\":\"BIMWall\"},{\"option\":\"Column\",\"classification\":\"BIMColumn\"},{\"option\":\"Slab\",\"classification\":\"BIMSlab\"},{\"option\":\"Beam\",\"classification\":\"BIMBeam\"}]}"
        << "]},\"debug\":[]}\n";
    return response.str();
}

std::string jsonCapabilitiesResponseFromDescriptors(int id)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":true"
        << ",\"result\":{"
        << "\"schema\":\"barebone.bricscad.capabilities.v1\","
        << "\"protocol\":\"bridge-json\","
        << "\"units\":\"mm\","
        << "\"coordinateSystem\":\"WCS\","
        << "\"toolCategories\":[\"geometry\",\"selection\",\"layer\",\"analysis\",\"document\",\"undo\",\"bim\",\"bridge\"],"
        << "\"resultStandard\":{\"fields\":[\"schema\",\"summary\",\"affectedHandles\",\"failedHandles\",\"warnings\",\"timeMs\"]},"
        << "\"methods\":[";
    for (std::size_t i = 0; i < bridgeMethodCount(); ++i) {
        appendMethodDescriptor(response, kBridgeMethods[i], i + 1 == bridgeMethodCount());
    }
    response << "],\"commands\":[";
    for (std::size_t i = 0; i < bridgeCommandCount(); ++i) {
        appendCommandDescriptor(response, kBridgeCommands[i], i + 1 == bridgeCommandCount());
    }
    response << "],\"selectorSchema\":" << kSelectorSchema
        << "},\"debug\":[]}\n";
    return response.str();
}

std::string jsonActionsResponseFromDescriptors(int id)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":true"
        << ",\"result\":{\"schema\":\"barebone.bricscad.actions.v1\","
        << "\"description\":\"Dokumentierte Barebone-BRX API-Aktionen. Jede Aktion beschreibt den POST-Body, Risiken und Ergebnisstandard.\","
        << "\"actions\":[";
    bool first = true;
    for (std::size_t i = 0; i < bridgeMethodCount(); ++i) {
        const BridgeMethodDescriptor& method = kBridgeMethods[i];
        if (std::strcmp(method.kind, "action") != 0 || method.apiPost == nullptr) {
            continue;
        }
        if (!first) {
            response << ',';
        }
        first = false;
        response << "{\"name\":\"" << jsonEscape(method.name)
            << "\",\"category\":\"" << jsonEscape(methodCategory(method.name))
            << "\",\"risk\":\"" << jsonEscape(method.risk)
            << "\",\"description\":\"" << jsonEscape(method.description)
            << "\",\"resultSchema\":\"barebone.bricscad." << jsonEscape(method.name) << ".result.v1\"";
        if (method.paramsSchema != nullptr) {
            response << ",\"paramsSchema\":" << method.paramsSchema;
        }
        response << ",\"post\":" << method.apiPost << "}";
    }
    response << "]},\"debug\":[]}\n";
    return response.str();
}

std::string jsonCommandsResponseFromDescriptors(int id)
{
    std::ostringstream response;
    response << "{\"id\":" << id
        << ",\"type\":\"response\""
        << ",\"ok\":true"
        << ",\"result\":{\"schema\":\"barebone.bricscad.commands.v1\","
        << "\"description\":\"Startliste bekannter nativer BricsCAD-Kommandos mit zugehoerigen Bridge-Tools. Die AI kann je nach Aufgabe zwischen freigegebenen Tools und validierter nativer command.execute-Ausfuehrung waehlen.\","
        << "\"commands\":[";
    for (std::size_t i = 0; i < bridgeCommandCount(); ++i) {
        appendCommandDescriptor(response, kBridgeCommands[i], i + 1 == bridgeCommandCount());
    }
    response << "]},\"debug\":[]}\n";
    return response.str();
}

int protocolIntValue(const std::vector<std::string>& parts, const std::string& key)
{
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == key) {
            return std::atoi(parts[i + 1].c_str());
        }
    }
    return 0;
}

std::string protocolStringValue(const std::vector<std::string>& parts, const std::string& key)
{
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == key) {
            return parts[i + 1];
        }
    }
    return {};
}

double protocolDoubleValue(const std::vector<std::string>& parts, const std::string& key)
{
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == key) {
            return std::strtod(parts[i + 1].c_str(), nullptr);
        }
    }
    return 0.0;
}

std::string protocolStringValueDecoded(const std::vector<std::string>& parts, const std::string& key)
{
    return percentDecode(protocolStringValue(parts, key));
}

std::string normalizeDetail(std::string detail)
{
    detail = toUpperAscii(trim(std::move(detail)));
    if (detail == "SUMMARY") {
        return "summary";
    }
    if (detail == "GEOMETRY") {
        return "geometry";
    }
    return "element";
}

std::string normalizeBimClass(std::string value)
{
    value = toUpperAscii(trim(std::move(value)));
    if (value == "BIMWALL" || value == "WALL" || value == "BIM WALL") {
        return "BIMWall";
    }
    return {};
}

std::string bimClassCommandOption(const std::string& bimClass)
{
    if (bimClass == "BIMWall") {
        return "_Wall";
    }
    return {};
}

std::string jsonResponseFromProtocol(int id, const std::string& method, const std::string& protocolResponse, const std::string& detail = "summary")
{
    const std::vector<std::string> lines = splitLines(protocolResponse);
    const std::vector<std::string> debugLines = protocolDebugLines(lines);
    const std::string summary = lines.empty() ? std::string() : lines.front();

    if (summary.rfind("ERROR", 0) == 0) {
        const std::vector<std::string> parts = splitTabs(summary);
        const std::string message = parts.size() > 1 ? parts[1] : "BRX Request fehlgeschlagen";
        return jsonErrorResponse(id, message, debugLines);
    }

    for (const std::string& line : lines) {
        if (line.rfind("RESULT\t", 0) != 0) {
            continue;
        }
        const std::string resultJson = line.substr(7);
        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":" << resultJson
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "layers.list") {
        std::ostringstream layers;
        layers << '[';
        bool first = true;
        for (const std::string& line : lines) {
            if (line.rfind("LAYER\t", 0) != 0) {
                continue;
            }
            if (!first) {
                layers << ',';
            }
            first = false;
            layers << '"' << jsonEscape(percentDecode(line.substr(6))) << '"';
        }
        layers << ']';

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"layers\":" << layers.str() << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "rectangles.extrude") {
        const std::vector<std::string> parts = splitTabs(summary);
        const int found = protocolIntValue(parts, "FOUND");
        const int extruded = protocolIntValue(parts, "EXTRUDED");
        const int skipped = protocolIntValue(parts, "SKIPPED");
        const int errors = protocolIntValue(parts, "ERRORS");
        const std::string layer = percentDecode(protocolStringValue(parts, "LAYER"));
        const double heightMm = protocolDoubleValue(parts, "HEIGHT_MM");
        const bool saveBefore = protocolIntValue(parts, "SAVE_BEFORE") != 0;
        const bool savedBefore = protocolIntValue(parts, "SAVED_BEFORE") != 0;

        std::ostringstream elements;
        elements << '[';
        bool firstElement = true;
        for (const std::string& line : lines) {
            if (line.rfind("ELEMENT\t", 0) != 0) {
                continue;
            }
            if (!firstElement) {
                elements << ',';
            }
            firstElement = false;
            elements << line.substr(8);
        }
        elements << ']';

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"schema\":\"barebone.bricscad.rectangles.extrude.result.v1\""
            << ",\"detail\":\"" << jsonEscape(detail) << "\""
            << ",\"units\":\"mm\""
            << ",\"coordinateSystem\":\"WCS\""
            << ",\"layer\":\"" << jsonEscape(layer) << "\""
            << ",\"heightMm\":" << heightMm
            << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
            << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
            << ",\"found\":" << found
            << ",\"extruded\":" << extruded
            << ",\"skipped\":" << skipped
            << ",\"errors\":" << errors
            << ",\"elements\":" << elements.str()
            << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "bim.classify") {
        const std::vector<std::string> parts = splitTabs(summary);
        const int found = protocolIntValue(parts, "FOUND");
        const int classified = protocolIntValue(parts, "CLASSIFIED");
        const int errors = protocolIntValue(parts, "ERRORS");
        const std::string target = percentDecode(protocolStringValue(parts, "TARGET"));
        const std::string bimClass = percentDecode(protocolStringValue(parts, "CLASS"));
        const bool saveBefore = protocolIntValue(parts, "SAVE_BEFORE") != 0;
        const bool savedBefore = protocolIntValue(parts, "SAVED_BEFORE") != 0;

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"schema\":\"barebone.bricscad.bim.classify.result.v1\""
            << ",\"target\":\"" << jsonEscape(target) << "\""
            << ",\"class\":\"" << jsonEscape(bimClass) << "\""
            << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
            << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
            << ",\"found\":" << found
            << ",\"classified\":" << classified
            << ",\"errors\":" << errors
            << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "geometry.create") {
        const std::vector<std::string> parts = splitTabs(summary);
        const std::string geometryType = percentDecode(protocolStringValue(parts, "GEOMETRY"));
        const std::string layer = percentDecode(protocolStringValue(parts, "LAYER"));
        const std::string handle = protocolStringValue(parts, "HANDLE");
        const bool saveBefore = protocolIntValue(parts, "SAVE_BEFORE") != 0;
        const bool savedBefore = protocolIntValue(parts, "SAVED_BEFORE") != 0;

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"schema\":\"barebone.bricscad.geometry.create.result.v1\""
            << ",\"geometry\":\"" << jsonEscape(geometryType) << "\""
            << ",\"created\":" << protocolIntValue(parts, "CREATED")
            << ",\"handle\":\"" << jsonEscape(handle) << "\""
            << ",\"layer\":\"" << jsonEscape(layer) << "\""
            << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
            << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
            << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "command.execute") {
        const std::vector<std::string> parts = splitTabs(summary);
        const std::string command = percentDecode(protocolStringValue(parts, "COMMAND"));
        const int posted = protocolIntValue(parts, "POSTED");
        const std::string commandLine = percentDecode(protocolStringValue(parts, "COMMAND_LINE"));
        const bool saveBefore = protocolIntValue(parts, "SAVE_BEFORE") != 0;
        const bool savedBefore = protocolIntValue(parts, "SAVED_BEFORE") != 0;

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"schema\":\"barebone.bricscad.command.execute.result.v1\""
            << ",\"command\":\"" << jsonEscape(command) << "\""
            << ",\"posted\":" << posted
            << ",\"commandLine\":\"" << jsonEscape(commandLine) << "\""
            << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
            << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
            << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "undo.last") {
        const std::vector<std::string> parts = splitTabs(summary);
        const int requested = protocolIntValue(parts, "REQUESTED");
        const int undone = protocolIntValue(parts, "UNDONE");
        const bool saveBefore = protocolIntValue(parts, "SAVE_BEFORE") != 0;
        const bool savedBefore = protocolIntValue(parts, "SAVED_BEFORE") != 0;
        const bool succeeded = protocolIntValue(parts, "SUCCEEDED") != 0;
        const int errors = protocolIntValue(parts, "ERRORS");

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"schema\":\"barebone.bricscad.undo.last.result.v1\""
            << ",\"command\":\"UNDO\""
            << ",\"requested\":" << requested
            << ",\"undone\":" << undone
            << ",\"errors\":" << errors
            << ",\"succeeded\":" << (succeeded ? "true" : "false")
            << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
            << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
            << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    if (method == "geometry.query" || method == "selection.describe" || method == "entity.describe") {
        const std::vector<std::string> parts = splitTabs(summary);
        const int count = protocolIntValue(parts, "COUNT");
        std::ostringstream objects;
        objects << '[';
        bool firstObject = true;
        for (const std::string& line : lines) {
            if (line.rfind("OBJECT\t", 0) != 0) {
                continue;
            }
            if (!firstObject) {
                objects << ',';
            }
            firstObject = false;
            objects << line.substr(7);
        }
        objects << ']';

        std::ostringstream response;
        response << "{\"id\":" << id
            << ",\"type\":\"response\""
            << ",\"ok\":true"
            << ",\"result\":{\"schema\":\"barebone.bricscad." << jsonEscape(method) << ".result.v1\""
            << ",\"count\":" << count
            << ",\"objects\":" << objects.str()
            << "}"
            << ",\"debug\":" << jsonDebugArray(debugLines)
            << "}\n";
        return response.str();
    }

    return jsonErrorResponse(id, "Unbekannte BRX Methode");
}

bool sendAll(SOCKET socketHandle, const std::string& response)
{
    const char* cursor = response.data();
    int remaining = static_cast<int>(response.size());
    while (remaining > 0) {
        const int sent = send(socketHandle, cursor, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        cursor += sent;
        remaining -= sent;
    }
    return true;
}

std::string compactBridgeJsonLine(const std::string& line)
{
    std::string compacted;
    compacted.reserve(line.size());

    bool inString = false;
    bool escaped = false;
    for (char ch : line) {
        if (inString) {
            compacted.push_back(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            compacted.push_back(ch);
            inString = true;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }

        compacted.push_back(ch);
    }

    compacted.push_back('\n');
    return compacted;
}

bool sendBridgeClientLine(const std::string& line)
{
    std::lock_guard<std::mutex> socketLock(g_bridgeClientSocketMutex);
    if (g_bridgeClientSocket == INVALID_SOCKET || !g_bridgeClientConnected.load()) {
        return false;
    }

    std::lock_guard<std::mutex> sendLock(g_bridgeClientSendMutex);
    return sendAll(g_bridgeClientSocket, compactBridgeJsonLine(line));
}

void sendSelectionSnapshotEvent(const std::vector<SelectionEntitySnapshot>& snapshot)
{
    std::ostringstream message;
    message << "{\"type\":\"event\",\"event\":\"selection.changed\",\"selection\":[";
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (i > 0) {
            message << ',';
        }
        message << "{\"handle\":\"" << jsonEscape(snapshot[i].handle)
            << "\",\"type\":\"" << jsonEscape(snapshot[i].type)
            << "\",\"layer\":\"" << jsonEscape(snapshot[i].layer)
            << "\"}";
    }
    message << "]}\n";
    sendBridgeClientLine(message.str());
}

std::string errorResponse(const std::string& message)
{
    return "ERROR\t" + message + "\n";
}

AcDbDatabase* workingDatabase()
{
    return acdbHostApplicationServices() == nullptr
        ? nullptr
        : acdbHostApplicationServices()->workingDatabase();
}

std::string listLayersInApplicationContext()
{
    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return errorResponse("Keine aktive BricsCAD Zeichnung gefunden");
    }

    AcAxDocLock lock(database);
    if (lock.lockStatus() != Acad::eOk) {
        return errorResponse("Aktive Zeichnung konnte nicht gesperrt werden");
    }

    AcDbLayerTable* layerTable = nullptr;
    if (database->getLayerTable(layerTable, AcDb::kForRead) != Acad::eOk || layerTable == nullptr) {
        return errorResponse("Layer-Tabelle konnte nicht gelesen werden");
    }

    AcDbLayerTableIterator* iterator = nullptr;
    if (layerTable->newIterator(iterator) != Acad::eOk || iterator == nullptr) {
        layerTable->close();
        return errorResponse("Layer-Iterator konnte nicht erstellt werden");
    }

    std::vector<std::string> layerNames;
    for (; !iterator->done(); iterator->step()) {
        AcDbLayerTableRecord* record = nullptr;
        if (iterator->getRecord(record, AcDb::kForRead) != Acad::eOk || record == nullptr) {
            continue;
        }

        const ACHAR* name = nullptr;
        if (record->getName(name) == Acad::eOk && name != nullptr) {
            layerNames.push_back(acharToUtf8(name));
        }
        record->close();
    }

    delete iterator;
    layerTable->close();

    std::sort(layerNames.begin(), layerNames.end());
    layerNames.erase(std::unique(layerNames.begin(), layerNames.end()), layerNames.end());

    std::ostringstream response;
    response << "OK\n";
    for (const std::string& layerName : layerNames) {
        response << "LAYER\t" << percentEncode(layerName) << "\n";
    }
    response << "END\n";
    return response.str();
}

struct BoundsData {
    bool valid = false;
    AcGePoint3d minPoint;
    AcGePoint3d maxPoint;
};

struct RectangleElementData {
    AcDbObjectId sourceId;
    AcDbObjectId solidId;
    BoundsData profileBounds;
    BoundsData solidBounds;
    AcGePoint3d profilePoints[4];
    double lengthMm = 0.0;
    double thicknessMm = 0.0;
    double heightMm = 0.0;
};

BoundsData boundsFromPoints(const AcGePoint3d points[4])
{
    BoundsData bounds;
    bounds.valid = true;
    bounds.minPoint = points[0];
    bounds.maxPoint = points[0];

    for (int i = 1; i < 4; ++i) {
        bounds.minPoint.x = std::min(bounds.minPoint.x, points[i].x);
        bounds.minPoint.y = std::min(bounds.minPoint.y, points[i].y);
        bounds.minPoint.z = std::min(bounds.minPoint.z, points[i].z);
        bounds.maxPoint.x = std::max(bounds.maxPoint.x, points[i].x);
        bounds.maxPoint.y = std::max(bounds.maxPoint.y, points[i].y);
        bounds.maxPoint.z = std::max(bounds.maxPoint.z, points[i].z);
    }

    return bounds;
}

void appendPointJson(std::ostringstream& json, const AcGePoint3d& point)
{
    json << '[' << point.x << ',' << point.y << ',' << point.z << ']';
}

void appendBoundsJson(std::ostringstream& json, const BoundsData& bounds)
{
    if (!bounds.valid) {
        json << "null";
        return;
    }

    json << "{\"min\":";
    appendPointJson(json, bounds.minPoint);
    json << ",\"max\":";
    appendPointJson(json, bounds.maxPoint);
    json << '}';
}

void appendDimensionsJson(std::ostringstream& json, const BoundsData& bounds)
{
    if (!bounds.valid) {
        json << "null";
        return;
    }

    json << "{\"widthX\":" << std::abs(bounds.maxPoint.x - bounds.minPoint.x)
        << ",\"depthY\":" << std::abs(bounds.maxPoint.y - bounds.minPoint.y)
        << ",\"heightZ\":" << std::abs(bounds.maxPoint.z - bounds.minPoint.z)
        << ",\"unit\":\"mm\""
        << ",\"source\":\"bounds\""
        << '}';
}

double boundsCenterDistance2d(const BoundsData& a, const BoundsData& b)
{
    if (!a.valid || !b.valid) {
        return 0.0;
    }

    const double ax = (a.minPoint.x + a.maxPoint.x) * 0.5;
    const double ay = (a.minPoint.y + a.maxPoint.y) * 0.5;
    const double bx = (b.minPoint.x + b.maxPoint.x) * 0.5;
    const double by = (b.minPoint.y + b.maxPoint.y) * 0.5;
    const double dx = ax - bx;
    const double dy = ay - by;
    return dx * dx + dy * dy;
}

bool objectIdArrayContains(const AcDbObjectIdArray& ids, const AcDbObjectId& objectId)
{
    for (int i = 0; i < ids.length(); ++i) {
        if (ids.at(i) == objectId) {
            return true;
        }
    }
    return false;
}

bool getEntityBounds(const AcDbObjectId& entityId, BoundsData& bounds, std::vector<std::string>& debugLines)
{
    bounds = {};
    AcDbEntity* entity = nullptr;
    const Acad::ErrorStatus openStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
    if (openStatus != Acad::eOk || entity == nullptr) {
        appendDebug(debugLines, "Bounds open failed handle=" + objectHandleText(entityId) + ": " + errorStatusText(openStatus));
        return false;
    }

    AcDbExtents extents;
    const Acad::ErrorStatus extentsStatus = entity->getGeomExtents(extents);
    entity->close();
    if (extentsStatus != Acad::eOk) {
        appendDebug(debugLines, "Bounds unavailable handle=" + objectHandleText(entityId) + ": " + errorStatusText(extentsStatus));
        return false;
    }

    bounds.valid = true;
    bounds.minPoint = extents.minPoint();
    bounds.maxPoint = extents.maxPoint();
    return true;
}

bool saveWorkingDatabaseBeforeAction(AcDbDatabase* database, std::vector<std::string>& debugLines, std::string& errorMessage)
{
    if (database == nullptr) {
        errorMessage = "Keine aktive BricsCAD Zeichnung gefunden";
        return false;
    }

    const ACHAR* filename = nullptr;
    const Acad::ErrorStatus filenameStatus = database->getFilename(filename);
    appendDebug(debugLines, "Pre-action save filename lookup: " + errorStatusText(filenameStatus));
    if (filenameStatus != Acad::eOk || filename == nullptr || filename[0] == 0) {
        errorMessage = "Zeichnung wurde noch nie gespeichert; bitte einmal manuell speichern, damit Auto-Speichern moeglich ist";
        appendDebug(debugLines, "Pre-action save skipped: drawing has no filename");
        return false;
    }

    appendDebug(debugLines, "Pre-action save target=" + acharToUtf8(filename));
    const Acad::ErrorStatus saveStatus = database->save();
    appendDebug(debugLines, "Pre-action save status: " + errorStatusText(saveStatus));
    if (saveStatus != Acad::eOk) {
        errorMessage = "Auto-Speichern vor Aktion fehlgeschlagen: " + errorStatusText(saveStatus);
        return false;
    }

    return true;
}

bool extractRectangleElementData(
    const AcDbPolyline* polyline,
    const AcDbObjectId& sourceId,
    double heightMm,
    RectangleElementData& element)
{
    if (polyline == nullptr
        || polyline->isClosed() == Adesk::kFalse
        || polyline->isOnlyLines() == Adesk::kFalse
        || polyline->numVerts() != 4) {
        return false;
    }

    AcGePoint3d points[4];
    for (unsigned int i = 0; i < 4; ++i) {
        if (polyline->getPointAt(i, points[i]) != Acad::eOk) {
            return false;
        }
    }

    AcGeVector3d edges[4] = {
        points[1] - points[0],
        points[2] - points[1],
        points[3] - points[2],
        points[0] - points[3],
    };

    for (const AcGeVector3d& edge : edges) {
        if (edge.length() <= kRectangleTolerance) {
            return false;
        }
    }

    const double len0 = edges[0].length();
    const double len1 = edges[1].length();
    const double len2 = edges[2].length();
    const double len3 = edges[3].length();
    const bool oppositeLengthsMatch = std::abs(len0 - len2) <= kRectangleTolerance
        && std::abs(len1 - len3) <= kRectangleTolerance;
    const bool rightAngles = std::abs(edges[0].dotProduct(edges[1])) <= kRectangleTolerance * len0 * len1
        && std::abs(edges[1].dotProduct(edges[2])) <= kRectangleTolerance * len1 * len2
        && std::abs(edges[2].dotProduct(edges[3])) <= kRectangleTolerance * len2 * len3
        && std::abs(edges[3].dotProduct(edges[0])) <= kRectangleTolerance * len3 * len0;

    if (!oppositeLengthsMatch || !rightAngles) {
        return false;
    }

    element = {};
    element.sourceId = sourceId;
    element.heightMm = heightMm;
    element.lengthMm = std::max(len0, len1);
    element.thicknessMm = std::min(len0, len1);
    element.profileBounds = boundsFromPoints(points);
    for (int i = 0; i < 4; ++i) {
        element.profilePoints[i] = points[i];
    }
    return true;
}

AcDbObjectIdArray collectCurrentSpaceSolidIds(AcDbDatabase* database, std::vector<std::string>& debugLines)
{
    AcDbObjectIdArray solidIds;
    if (database == nullptr) {
        return solidIds;
    }

    AcDbBlockTableRecord* space = nullptr;
    const AcDbObjectId currentSpaceId = database->currentSpaceId();
    const Acad::ErrorStatus openStatus = acdbOpenObject(space, currentSpaceId, AcDb::kForRead);
    if (openStatus != Acad::eOk || space == nullptr) {
        appendDebug(debugLines, "Collect solids open current space failed: " + errorStatusText(openStatus));
        return solidIds;
    }

    AcDbBlockTableRecordIterator* iterator = nullptr;
    const Acad::ErrorStatus iteratorStatus = space->newIterator(iterator);
    if (iteratorStatus != Acad::eOk || iterator == nullptr) {
        space->close();
        appendDebug(debugLines, "Collect solids iterator failed: " + errorStatusText(iteratorStatus));
        return solidIds;
    }

    for (; !iterator->done(); iterator->step()) {
        AcDbObjectId entityId;
        if (iterator->getEntityId(entityId) != Acad::eOk || entityId.isNull()) {
            continue;
        }

        AcDbEntity* entity = nullptr;
        if (acdbOpenObject(entity, entityId, AcDb::kForRead) != Acad::eOk || entity == nullptr) {
            continue;
        }
        if (AcDb3dSolid::cast(entity) != nullptr) {
            solidIds.append(entityId);
        }
        entity->close();
    }

    delete iterator;
    space->close();
    return solidIds;
}

AcDbObjectIdArray newObjectIds(const AcDbObjectIdArray& before, const AcDbObjectIdArray& after)
{
    AcDbObjectIdArray result;
    for (int i = 0; i < after.length(); ++i) {
        const AcDbObjectId id = after.at(i);
        if (!objectIdArrayContains(before, id)) {
            result.append(id);
        }
    }
    return result;
}

void rememberLastExtrudedSolids(const AcDbObjectIdArray& ids, const std::string& layerName, std::vector<std::string>& debugLines)
{
    std::lock_guard<std::mutex> lock(g_lastExtrudedSolidsMutex);
    g_lastExtrudedSolidIds.clear();
    g_lastExtrudedLayer = layerName;
    g_lastExtrudedSolidIds.reserve(static_cast<std::size_t>(ids.length()));
    for (int i = 0; i < ids.length(); ++i) {
        g_lastExtrudedSolidIds.push_back(ids.at(i));
    }
    appendDebug(debugLines, "Remember last extruded solids count=" + std::to_string(g_lastExtrudedSolidIds.size())
        + " layer='" + layerName + "'");
}

void rememberLastResult(const AcDbObjectIdArray& ids, const std::string& label, std::vector<std::string>& debugLines)
{
    std::lock_guard<std::mutex> lock(g_lastResultMutex);
    g_lastResultIds.clear();
    g_lastResultLabel = label;
    g_lastResultIds.reserve(static_cast<std::size_t>(ids.length()));
    for (int i = 0; i < ids.length(); ++i) {
        g_lastResultIds.push_back(ids.at(i));
    }
    appendDebug(debugLines, "Remember lastResult count=" + std::to_string(g_lastResultIds.size())
        + " label='" + label + "'");
}

void rememberLastResult(const AcDbObjectId& id, const std::string& label, std::vector<std::string>& debugLines)
{
    AcDbObjectIdArray ids;
    if (!id.isNull()) {
        ids.append(id);
    }
    rememberLastResult(ids, label, debugLines);
}

AcDbObjectIdArray lastExtrudedSolidIds()
{
    std::lock_guard<std::mutex> lock(g_lastExtrudedSolidsMutex);
    AcDbObjectIdArray ids;
    for (const AcDbObjectId& id : g_lastExtrudedSolidIds) {
        ids.append(id);
    }
    return ids;
}

AcDbObjectIdArray lastResultObjectIds()
{
    std::lock_guard<std::mutex> lock(g_lastResultMutex);
    AcDbObjectIdArray ids;
    for (const AcDbObjectId& id : g_lastResultIds) {
        ids.append(id);
    }
    return ids;
}

void matchCreatedSolids(
    std::vector<RectangleElementData>& elements,
    const AcDbObjectIdArray& createdSolidIds,
    std::vector<std::string>& debugLines)
{
    struct SolidCandidate {
        AcDbObjectId id;
        BoundsData bounds;
        bool used = false;
    };

    std::vector<SolidCandidate> candidates;
    candidates.reserve(static_cast<std::size_t>(createdSolidIds.length()));
    for (int i = 0; i < createdSolidIds.length(); ++i) {
        SolidCandidate candidate;
        candidate.id = createdSolidIds.at(i);
        getEntityBounds(candidate.id, candidate.bounds, debugLines);
        candidates.push_back(candidate);
        appendDebug(debugLines, "Created solid candidate handle=" + objectHandleText(candidate.id));
    }

    for (RectangleElementData& element : elements) {
        int bestIndex = -1;
        double bestDistance = 0.0;
        for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
            if (candidates[static_cast<std::size_t>(i)].used) {
                continue;
            }

            const double distance = boundsCenterDistance2d(element.profileBounds, candidates[static_cast<std::size_t>(i)].bounds);
            if (bestIndex < 0 || distance < bestDistance) {
                bestIndex = i;
                bestDistance = distance;
            }
        }

        if (bestIndex < 0) {
            continue;
        }

        SolidCandidate& candidate = candidates[static_cast<std::size_t>(bestIndex)];
        candidate.used = true;
        element.solidId = candidate.id;
        element.solidBounds = candidate.bounds;
        appendDebug(debugLines, "Matched source handle=" + objectHandleText(element.sourceId)
            + " to solid handle=" + objectHandleText(element.solidId));
    }
}

std::string rectangleElementJson(
    const RectangleElementData& element,
    const std::string& layerNameUtf8,
    const std::string& detail,
    bool commandSucceeded)
{
    const bool hasSolid = !element.solidId.isNull();
    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.element.v1\""
        << ",\"operation\":\"" << (commandSucceeded ? (hasSolid ? "extruded" : "extruded_unresolved_solid") : "failed") << "\""
        << ",\"sourceHandle\":\"" << jsonEscape(objectHandleText(element.sourceId)) << "\""
        << ",\"solidHandle\":";
    if (hasSolid) {
        json << "\"" << jsonEscape(objectHandleText(element.solidId)) << "\"";
    } else {
        json << "null";
    }

    json << ",\"sourceEntityKind\":\"polyline\""
        << ",\"entityKind\":" << (hasSolid ? "\"3dSolid\"" : "null")
        << ",\"bimClass\":null"
        << ",\"layer\":\"" << jsonEscape(layerNameUtf8) << "\""
        << ",\"heightMm\":" << element.heightMm
        << ",\"lengthMm\":" << element.lengthMm
        << ",\"thicknessMm\":" << element.thicknessMm
        << ",\"boundingBoxStatus\":\"" << (element.solidBounds.valid ? "actual" : "unavailable") << "\""
        << ",\"boundingBox\":";
    appendBoundsJson(json, element.solidBounds);
    json << ",\"sourceProfile\":{\"boundingBox\":";
    appendBoundsJson(json, element.profileBounds);

    if (detail == "geometry") {
        json << ",\"points\":[";
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                json << ',';
            }
            appendPointJson(json, element.profilePoints[i]);
        }
        json << ']';
    }

    json << "}}";
    return json.str();
}

bool isRectanglePolyline(const AcDbPolyline* polyline)
{
    if (polyline == nullptr
        || polyline->isClosed() == Adesk::kFalse
        || polyline->isOnlyLines() == Adesk::kFalse
        || polyline->numVerts() != 4) {
        return false;
    }

    AcGePoint3d points[4];
    for (unsigned int i = 0; i < 4; ++i) {
        if (polyline->getPointAt(i, points[i]) != Acad::eOk) {
            return false;
        }
    }

    AcGeVector3d edges[4] = {
        points[1] - points[0],
        points[2] - points[1],
        points[3] - points[2],
        points[0] - points[3],
    };

    for (const AcGeVector3d& edge : edges) {
        if (edge.length() <= kRectangleTolerance) {
            return false;
        }
    }

    const double len0 = edges[0].length();
    const double len1 = edges[1].length();
    const double len2 = edges[2].length();
    const double len3 = edges[3].length();
    const bool oppositeLengthsMatch = std::abs(len0 - len2) <= kRectangleTolerance
        && std::abs(len1 - len3) <= kRectangleTolerance;
    const bool rightAngles = std::abs(edges[0].dotProduct(edges[1])) <= kRectangleTolerance * len0 * len1
        && std::abs(edges[1].dotProduct(edges[2])) <= kRectangleTolerance * len1 * len2
        && std::abs(edges[2].dotProduct(edges[3])) <= kRectangleTolerance * len2 * len3
        && std::abs(edges[3].dotProduct(edges[0])) <= kRectangleTolerance * len3 * len0;

    return oppositeLengthsMatch && rightAngles;
}

std::string entityKind(const AcDbEntity* entity)
{
    if (AcDbPolyline::cast(entity) != nullptr) {
        return "polyline";
    }
    if (AcDb3dSolid::cast(entity) != nullptr) {
        return "solid";
    }
    return "entity";
}

std::string entityShape(const AcDbEntity* entity)
{
    const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
    if (polyline != nullptr && isRectanglePolyline(polyline)) {
        return "rectangle";
    }
    return {};
}

bool objectIdFromHandleText(const std::string& handleText, AcDbObjectId& objectId)
{
    objectId = AcDbObjectId();
    const std::basic_string<ACHAR> handle = utf8ToAchar(handleText);
    ads_name entityName{};
    if (acdbHandEnt(handle.c_str(), entityName) != RTNORM) {
        return false;
    }
    return acdbGetObjectId(objectId, entityName) == Acad::eOk && !objectId.isNull();
}

AcDbObjectIdArray selectionSetObjectIds(const ACHAR* mode, const char* label, std::vector<std::string>* debugLines)
{
    AcDbObjectIdArray ids;
    ads_name selectionSet{};
    const int getStatus = acedSSGet(mode, nullptr, nullptr, nullptr, selectionSet);
    if (debugLines != nullptr) {
        appendDebug(*debugLines, std::string("acedSSGet ") + label + " status=" + std::to_string(getStatus));
    }
    if (getStatus != RTNORM) {
        return ids;
    }

    Adesk::Int32 selectionLength = 0;
    if (acedSSLength(selectionSet, &selectionLength) != RTNORM || selectionLength <= 0) {
        acedSSFree(selectionSet);
        if (debugLines != nullptr) {
            appendDebug(*debugLines, std::string("Selection ") + label + " empty");
        }
        return ids;
    }
    if (debugLines != nullptr) {
        appendDebug(*debugLines, std::string("Selection ") + label + " length=" + std::to_string(selectionLength));
    }

    for (Adesk::Int32 i = 0; i < selectionLength; ++i) {
        ads_name entityName{};
        if (acedSSName(selectionSet, static_cast<int>(i), entityName) != RTNORM) {
            continue;
        }
        AcDbObjectId entityId;
        if (acdbGetObjectId(entityId, entityName) == Acad::eOk && !entityId.isNull()) {
            ids.append(entityId);
        }
    }
    acedSSFree(selectionSet);
    return ids;
}

AcDbObjectIdArray cachedSelectionObjectIds(std::vector<std::string>* debugLines)
{
    std::vector<SelectionEntitySnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_selectionSnapshotMutex);
        snapshot = g_lastSelectionSnapshot;
    }

    AcDbObjectIdArray ids;
    if (debugLines != nullptr) {
        appendDebug(*debugLines, "Cached BBTRACK selection snapshot size=" + std::to_string(snapshot.size()));
    }
    for (const SelectionEntitySnapshot& item : snapshot) {
        AcDbObjectId id;
        if (objectIdFromHandleText(item.handle, id)) {
            ids.append(id);
        } else if (debugLines != nullptr) {
            appendDebug(*debugLines, "Cached selection handle not found: " + item.handle);
        }
    }
    return ids;
}

AcDbObjectIdArray pickfirstObjectIds(std::vector<std::string>* debugLines = nullptr)
{
    AcDbObjectIdArray ids = selectionSetObjectIds(_T("_I"), "implied/_I", debugLines);
    if (!ids.isEmpty()) {
        return ids;
    }

    ids = cachedSelectionObjectIds(debugLines);
    if (!ids.isEmpty()) {
        return ids;
    }

    ids = selectionSetObjectIds(_T("_P"), "previous/_P", debugLines);
    if (!ids.isEmpty()) {
        return ids;
    }

    if (debugLines != nullptr) {
        appendDebug(*debugLines, "Selection fallback result ids=" + std::to_string(ids.length()));
    }
    return ids;
}

bool curveLength(AcDbEntity* entity, double& length);
bool entityArea(AcDbEntity* entity, double& area);

void appendEntityJson(std::ostringstream& json, const AcDbObjectId& entityId, bool includeGeometry, bool includeMetrics, std::vector<std::string>& debugLines)
{
    AcDbEntity* entity = nullptr;
    const Acad::ErrorStatus openStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
    if (openStatus != Acad::eOk || entity == nullptr) {
        json << "{\"handle\":\"" << jsonEscape(objectHandleText(entityId))
            << "\",\"error\":\"" << jsonEscape(errorStatusText(openStatus)) << "\"}";
        return;
    }

    const std::string type = entityTypeName(entity);
    const std::string layer = entityLayerName(entity);
    const std::string kind = entityKind(entity);
    const std::string shape = entityShape(entity);
    BoundsData bounds;
    AcDbExtents extents;
    if (entity->getGeomExtents(extents) == Acad::eOk) {
        bounds.valid = true;
        bounds.minPoint = extents.minPoint();
        bounds.maxPoint = extents.maxPoint();
    }

    json << "{\"handle\":\"" << jsonEscape(objectHandleText(entityId))
        << "\",\"type\":\"" << jsonEscape(type)
        << "\",\"layer\":\"" << jsonEscape(layer)
        << "\",\"kind\":\"" << jsonEscape(kind)
        << "\",\"shape\":";
    if (shape.empty()) {
        json << "null";
    } else {
        json << "\"" << jsonEscape(shape) << "\"";
    }
    json << ",\"bounds\":";
    appendBoundsJson(json, bounds);
    json << ",\"dimensions\":";
    appendDimensionsJson(json, bounds);

    if (includeMetrics) {
        double length = 0.0;
        double area = 0.0;
        const bool hasLength = curveLength(entity, length);
        const bool hasArea = entityArea(entity, area);
        const double height = bounds.valid ? std::abs(bounds.maxPoint.z - bounds.minPoint.z) : 0.0;
        json << ",\"metrics\":{"
            << "\"length\":";
        if (hasLength) {
            json << length;
        } else {
            json << "null";
        }
        json << ",\"perimeter\":";
        if (hasLength && !shape.empty()) {
            json << length;
        } else {
            json << "null";
        }
        json << ",\"area\":";
        if (hasArea) {
            json << area;
        } else {
            json << "null";
        }
        json << ",\"height\":" << height
            << ",\"volume\":null"
            << "}";
    }

    if (includeGeometry) {
        const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
        if (polyline != nullptr) {
            json << ",\"geometry\":{\"closed\":" << (polyline->isClosed() ? "true" : "false")
                << ",\"vertices\":[";
            for (unsigned int i = 0; i < polyline->numVerts(); ++i) {
                AcGePoint3d point;
                if (polyline->getPointAt(i, point) != Acad::eOk) {
                    continue;
                }
                if (i > 0) {
                    json << ',';
                }
                appendPointJson(json, point);
            }
            json << "]}";
        }
    }

    json << "}";
    entity->close();
}

bool stringVectorContains(const std::vector<std::string>& values, const std::string& candidate)
{
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

AcDbObjectIdArray collectCurrentSpaceEntityIds(AcDbDatabase* database, std::vector<std::string>& debugLines)
{
    AcDbObjectIdArray ids;
    if (database == nullptr) {
        return ids;
    }

    AcDbBlockTableRecord* space = nullptr;
    const AcDbObjectId currentSpaceId = database->currentSpaceId();
    const Acad::ErrorStatus openStatus = acdbOpenObject(space, currentSpaceId, AcDb::kForRead);
    if (openStatus != Acad::eOk || space == nullptr) {
        appendDebug(debugLines, "collectCurrentSpaceEntityIds open failed: " + errorStatusText(openStatus));
        return ids;
    }

    AcDbBlockTableRecordIterator* iterator = nullptr;
    const Acad::ErrorStatus iteratorStatus = space->newIterator(iterator);
    if (iteratorStatus != Acad::eOk || iterator == nullptr) {
        space->close();
        appendDebug(debugLines, "collectCurrentSpaceEntityIds iterator failed: " + errorStatusText(iteratorStatus));
        return ids;
    }

    for (; !iterator->done(); iterator->step()) {
        AcDbObjectId entityId;
        if (iterator->getEntityId(entityId) == Acad::eOk && !entityId.isNull()) {
            ids.append(entityId);
        }
    }

    delete iterator;
    space->close();
    return ids;
}

bool entityMatchesQueryFilters(const AcDbObjectId& entityId, const std::string& filtersText, std::vector<std::string>& debugLines);

AcDbObjectIdArray selectorObjectIds(const std::string& selectorText, AcDbDatabase* database, std::vector<std::string>& debugLines)
{
    AcDbObjectIdArray ids;
    const std::string scope = jsonStringProperty(selectorText, "scope").value_or("currentSpace");
    if (scope == "selection") {
        ids = pickfirstObjectIds(&debugLines);
    } else if (scope == "lastExtruded") {
        ids = lastExtrudedSolidIds();
    } else if (scope == "lastResult") {
        ids = lastResultObjectIds();
    } else if (scope == "handles") {
        const std::optional<std::string> handlesArray = jsonArrayProperty(selectorText, "handles");
        for (const std::string& handle : handlesArray.has_value() ? jsonStringArrayValues(*handlesArray) : std::vector<std::string>{}) {
            AcDbObjectId id;
            if (objectIdFromHandleText(handle, id)) {
                ids.append(id);
            } else {
                appendDebug(debugLines, "Selector handle not found: " + handle);
            }
        }
    } else {
        ids = collectCurrentSpaceEntityIds(database, debugLines);
    }

    const bool hasInlineFilters = jsonStringProperty(selectorText, "layer").has_value()
        || jsonStringProperty(selectorText, "kind").has_value()
        || jsonStringProperty(selectorText, "shape").has_value()
        || jsonArrayProperty(selectorText, "layers").has_value()
        || jsonArrayProperty(selectorText, "kinds").has_value()
        || jsonArrayProperty(selectorText, "shapes").has_value()
        || jsonArrayProperty(selectorText, "types").has_value()
        || jsonStringProperty(selectorText, "typeContains").has_value()
        || jsonBoolProperty(selectorText, "isClosed").has_value()
        || jsonBoolProperty(selectorText, "is3D").has_value()
        || jsonDoubleProperty(selectorText, "minArea").has_value()
        || jsonDoubleProperty(selectorText, "maxArea").has_value();
    if (!hasInlineFilters) {
        appendDebug(debugLines, "Selector scope=" + scope + " ids=" + std::to_string(ids.length()));
        return ids;
    }

    AcDbObjectIdArray filteredIds;
    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId id = ids.at(i);
        if (entityMatchesQueryFilters(id, selectorText, debugLines)) {
            filteredIds.append(id);
        }
    }
    appendDebug(debugLines, "Selector scope=" + scope
        + " ids=" + std::to_string(ids.length())
        + " filtered=" + std::to_string(filteredIds.length()));
    return filteredIds;
}

bool entityMatchesQueryFilters(const AcDbObjectId& entityId, const std::string& filtersText, std::vector<std::string>& debugLines)
{
    AcDbEntity* entity = nullptr;
    const Acad::ErrorStatus openStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
    if (openStatus != Acad::eOk || entity == nullptr) {
        appendDebug(debugLines, "Filter open failed handle=" + objectHandleText(entityId) + ": " + errorStatusText(openStatus));
        return false;
    }

    const std::string layer = entityLayerName(entity);
    const std::string type = entityTypeName(entity);
    const std::string kind = entityKind(entity);
    const std::string shape = entityShape(entity);
    const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
    const bool isClosed = polyline != nullptr && polyline->isClosed() == Adesk::kTrue;
    BoundsData bounds;
    AcDbExtents extents;
    if (entity->getGeomExtents(extents) == Acad::eOk) {
        bounds.valid = true;
        bounds.minPoint = extents.minPoint();
        bounds.maxPoint = extents.maxPoint();
    }
    const bool is3D = AcDb3dSolid::cast(entity) != nullptr
        || (bounds.valid && std::abs(bounds.maxPoint.z - bounds.minPoint.z) > kRectangleTolerance);
    double area = 0.0;
    const bool hasArea = entityArea(entity, area);
    auto reject = [&entity]() {
        entity->close();
        return false;
    };

    if (!filtersText.empty()) {
        if (const std::optional<std::string> layers = jsonArrayProperty(filtersText, "layers"); layers.has_value()) {
            const std::vector<std::string> values = jsonStringArrayValues(*layers);
            if (!values.empty() && !stringVectorContains(values, layer)) {
                return reject();
            }
        }
        if (const std::optional<std::string> types = jsonArrayProperty(filtersText, "types"); types.has_value()) {
            const std::vector<std::string> values = jsonStringArrayValues(*types);
            if (!values.empty() && !stringVectorContains(values, type)) {
                return reject();
            }
        }
        if (const std::optional<std::string> kinds = jsonArrayProperty(filtersText, "kinds"); kinds.has_value()) {
            const std::vector<std::string> values = jsonStringArrayValues(*kinds);
            if (!values.empty() && !stringVectorContains(values, kind) && !stringVectorContains(values, shape)) {
                return reject();
            }
        }
        if (const std::optional<std::string> shapes = jsonArrayProperty(filtersText, "shapes"); shapes.has_value()) {
            const std::vector<std::string> values = jsonStringArrayValues(*shapes);
            if (!values.empty() && !stringVectorContains(values, shape)) {
                return reject();
            }
        }
        const std::string singleLayer = jsonStringProperty(filtersText, "layer").value_or("");
        if (!singleLayer.empty() && singleLayer != layer) {
            return reject();
        }
        const std::string singleKind = jsonStringProperty(filtersText, "kind").value_or("");
        if (!singleKind.empty() && singleKind != kind && singleKind != shape) {
            return reject();
        }
        const std::string singleShape = jsonStringProperty(filtersText, "shape").value_or("");
        if (!singleShape.empty() && singleShape != shape) {
            return reject();
        }
        const std::string typeContains = jsonStringProperty(filtersText, "typeContains").value_or("");
        if (!typeContains.empty() && toUpperAscii(type).find(toUpperAscii(typeContains)) == std::string::npos) {
            return reject();
        }
        if (const std::optional<bool> requiredClosed = jsonBoolProperty(filtersText, "isClosed"); requiredClosed.has_value() && *requiredClosed != isClosed) {
            return reject();
        }
        if (const std::optional<bool> required3d = jsonBoolProperty(filtersText, "is3D"); required3d.has_value() && *required3d != is3D) {
            return reject();
        }
        if (const std::optional<double> minArea = jsonDoubleProperty(filtersText, "minArea"); minArea.has_value() && (!hasArea || area < *minArea)) {
            return reject();
        }
        if (const std::optional<double> maxArea = jsonDoubleProperty(filtersText, "maxArea"); maxArea.has_value() && (!hasArea || area > *maxArea)) {
            return reject();
        }
    }
    entity->close();
    return true;
}

bool createSelectionSet(const AcDbObjectIdArray& objectIds, ads_name selectionSet, std::vector<std::string>& debugLines)
{
    int status = acedSSAdd(nullptr, nullptr, selectionSet);
    appendDebug(debugLines, "acedSSAdd create selection set: " + std::to_string(status));
    if (status != RTNORM) {
        return false;
    }

    for (int i = 0; i < objectIds.length(); ++i) {
        ads_name entityName{};
        const Acad::ErrorStatus nameStatus = acdbGetAdsName(entityName, objectIds.at(i));
        appendDebug(debugLines, "acdbGetAdsName handle=" + objectHandleText(objectIds.at(i)) + ": " + errorStatusText(nameStatus));
        if (nameStatus != Acad::eOk) {
            continue;
        }

        status = acedSSAdd(entityName, selectionSet, selectionSet);
        appendDebug(debugLines, "acedSSAdd entity handle=" + objectHandleText(objectIds.at(i)) + ": " + std::to_string(status));
    }

    Adesk::Int32 selectionLength = 0;
    status = acedSSLength(selectionSet, &selectionLength);
    appendDebug(debugLines, "acedSSLength: status=" + std::to_string(status) + ", length=" + std::to_string(selectionLength));
    return status == RTNORM && selectionLength == objectIds.length();
}

std::string objectsProtocolResponse(
    const AcDbObjectIdArray& ids,
    const std::string& paramsJson,
    const std::string& defaultFiltersJson,
    bool includeGeometryDefault,
    const std::vector<std::string>& initialDebugLines = {})
{
    std::vector<std::string> debugLines = initialDebugLines;
    const std::string filtersText = jsonObjectProperty(paramsJson, "filters").value_or(defaultFiltersJson);
    const std::optional<std::string> includeArray = jsonArrayProperty(paramsJson, "include");
    const std::vector<std::string> include = includeArray.has_value() ? jsonStringArrayValues(*includeArray) : std::vector<std::string>{};
    const bool includeGeometry = includeGeometryDefault
        || stringVectorContains(include, "geometry")
        || stringVectorContains(include, "vertices")
        || stringVectorContains(include, "polyline");
    const bool includeMetrics = stringVectorContains(include, "metrics")
        || stringVectorContains(include, "length")
        || stringVectorContains(include, "area")
        || stringVectorContains(include, "perimeter")
        || stringVectorContains(include, "height")
        || stringVectorContains(include, "volume");
    const int limit = std::max(1, jsonIntProperty(paramsJson, "limit").value_or(500));

    std::ostringstream response;
    int count = 0;
    for (int i = 0; i < ids.length() && count < limit; ++i) {
        const AcDbObjectId id = ids.at(i);
        if (!entityMatchesQueryFilters(id, filtersText, debugLines)) {
            continue;
        }

        std::ostringstream objectJson;
        appendEntityJson(objectJson, id, includeGeometry, includeMetrics, debugLines);
        response << "OBJECT\t" << objectJson.str() << "\n";
        ++count;
    }

    std::ostringstream result;
    result << "OK\tCOUNT\t" << count << "\n" << response.str();
    appendDebugResponse(result, debugLines);
    return result.str();
}

std::string geometryQueryInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return errorResponse("Keine aktive BricsCAD Zeichnung gefunden");
    }

    const std::string selectorText = jsonObjectProperty(paramsJson, "selector").value_or("{}");
    AcDbObjectIdArray ids = selectorObjectIds(selectorText, database, debugLines);
    appendDebug(debugLines, "geometry.query candidate ids=" + std::to_string(ids.length()));

    std::ostringstream result;
    const std::string protocol = objectsProtocolResponse(ids, paramsJson, "{}", false, debugLines);
    result << protocol;
    return result.str();
}

std::string selectionDescribeInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    const AcDbObjectIdArray ids = pickfirstObjectIds(&debugLines);
    return objectsProtocolResponse(ids, paramsJson, "{}", true, debugLines);
}

std::string entityDescribeInApplicationContext(const std::string& paramsJson)
{
    AcDbObjectIdArray ids;
    const std::optional<std::string> handlesArray = jsonArrayProperty(paramsJson, "handles");
    for (const std::string& handle : handlesArray.has_value() ? jsonStringArrayValues(*handlesArray) : std::vector<std::string>{}) {
        AcDbObjectId id;
        if (objectIdFromHandleText(handle, id)) {
            ids.append(id);
        }
    }
    if (ids.isEmpty()) {
        const std::string handle = jsonStringProperty(paramsJson, "handle").value_or("");
        AcDbObjectId id;
        if (!handle.empty() && objectIdFromHandleText(handle, id)) {
            ids.append(id);
        }
    }
    return objectsProtocolResponse(ids, paramsJson, "{}", true);
}

struct JsonPoint3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

JsonPoint3d jsonPointProperty(const std::string& object, const std::string& key, const JsonPoint3d& fallback = {})
{
    const std::optional<std::string> pointObject = jsonObjectProperty(object, key);
    if (!pointObject.has_value()) {
        const std::optional<std::string> pointArray = jsonArrayProperty(object, key);
        if (!pointArray.has_value()) {
            return fallback;
        }
        const std::vector<double> values = jsonNumberArrayValues(*pointArray);
        if (values.size() < 2) {
            return fallback;
        }
        return JsonPoint3d{values[0], values[1], values.size() >= 3 ? values[2] : fallback.z};
    }

    return JsonPoint3d{
        jsonDoubleProperty(*pointObject, "x").value_or(fallback.x),
        jsonDoubleProperty(*pointObject, "y").value_or(fallback.y),
        jsonDoubleProperty(*pointObject, "z").value_or(fallback.z),
    };
}

std::vector<JsonPoint3d> jsonPointArrayProperty(const std::string& object, const std::string& key)
{
    std::vector<JsonPoint3d> points;
    const std::optional<std::string> arrayText = jsonArrayProperty(object, key);
    if (!arrayText.has_value()) {
        return points;
    }

    for (const std::string& pointObject : jsonObjectArrayValues(*arrayText)) {
        points.push_back(JsonPoint3d{
            jsonDoubleProperty(pointObject, "x").value_or(0.0),
            jsonDoubleProperty(pointObject, "y").value_or(0.0),
            jsonDoubleProperty(pointObject, "z").value_or(0.0),
        });
    }
    return points;
}

double jsonAngleRadians(const std::string& object, const std::string& radKey, const std::string& degKey, double fallback)
{
    if (const std::optional<double> radians = jsonDoubleProperty(object, radKey); radians.has_value()) {
        return *radians;
    }
    if (const std::optional<double> degrees = jsonDoubleProperty(object, degKey); degrees.has_value()) {
        return (*degrees) * 3.14159265358979323846 / 180.0;
    }
    return fallback;
}

std::string selectorJsonFromParams(const std::string& paramsJson)
{
    if (const std::optional<std::string> selector = jsonObjectProperty(paramsJson, "selector"); selector.has_value()) {
        return *selector;
    }
    if (const std::optional<std::string> handles = jsonArrayProperty(paramsJson, "handles"); handles.has_value()) {
        return std::string("{\"scope\":\"handles\",\"handles\":") + *handles + "}";
    }
    if (const std::optional<std::string> handle = jsonStringProperty(paramsJson, "handle"); handle.has_value() && !handle->empty()) {
        return std::string("{\"scope\":\"handles\",\"handles\":[\"") + jsonEscape(*handle) + "\"]}";
    }
    if (const std::optional<std::string> layer = jsonStringProperty(paramsJson, "layer"); layer.has_value() && !layer->empty()) {
        return std::string("{\"scope\":\"currentSpace\",\"layer\":\"") + jsonEscape(*layer) + "\"}";
    }
    if (const std::optional<std::string> target = jsonStringProperty(paramsJson, "target"); target.has_value() && !target->empty()) {
        if (*target == "selection" || *target == "lastResult" || *target == "lastExtruded" || *target == "currentSpace") {
            return std::string("{\"scope\":\"") + jsonEscape(*target) + "\"}";
        }
    }
    return {};
}

std::string handlesJsonArray(const AcDbObjectIdArray& ids)
{
    std::ostringstream json;
    json << '[';
    for (int i = 0; i < ids.length(); ++i) {
        if (i > 0) {
            json << ',';
        }
        json << '"' << jsonEscape(objectHandleText(ids.at(i))) << '"';
    }
    json << ']';
    return json.str();
}

std::string okJsonResultResponse(const std::string& resultJson, const std::vector<std::string>& debugLines)
{
    std::ostringstream response;
    response << "OK\n"
        << "RESULT\t" << resultJson << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string genericActionResultJson(
    const std::string& schema,
    const std::string& summary,
    const AcDbObjectIdArray& affectedIds,
    const AcDbObjectIdArray& failedIds,
    bool saveBefore,
    bool savedBefore)
{
    std::ostringstream json;
    json << "{\"schema\":\"" << jsonEscape(schema) << "\""
        << ",\"summary\":\"" << jsonEscape(summary) << "\""
        << ",\"affected\":" << affectedIds.length()
        << ",\"failed\":" << failedIds.length()
        << ",\"affectedHandles\":" << handlesJsonArray(affectedIds)
        << ",\"failedHandles\":" << handlesJsonArray(failedIds)
        << ",\"warnings\":[]"
        << ",\"timeMs\":0"
        << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
        << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
        << '}';
    return json.str();
}

bool prepareMutation(
    AcDbDatabase*& database,
    bool saveBefore,
    bool& savedBefore,
    std::unique_ptr<AcAxDocLock>& docLock,
    std::vector<std::string>& debugLines,
    std::string& errorMessage)
{
    database = workingDatabase();
    if (database == nullptr) {
        errorMessage = "Keine aktive BricsCAD Zeichnung gefunden";
        return false;
    }

    savedBefore = false;
    if (saveBefore) {
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, errorMessage)) {
            return false;
        }
        savedBefore = true;
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            errorMessage = "Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus());
            return false;
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    } else {
        appendDebug(debugLines, "Command context active; explicit document lock skipped");
    }
    return true;
}

bool ensureNativeCommandContext(std::vector<std::string>& debugLines, const std::string& commandName)
{
    if (acDocManager == nullptr) {
        appendDebug(debugLines, "Native command blocked for " + commandName + ": document manager unavailable");
        return false;
    }

    const bool applicationContext = acDocManager->isApplicationContext();
    appendDebug(debugLines, "Native command context for " + commandName + ": "
        + std::string(applicationContext ? "application" : "command"));
    if (applicationContext) {
        appendDebug(debugLines, "Native command blocked for " + commandName + ": acedCommand requires BricsCAD command context");
        return false;
    }
    return true;
}

int normalizedLayerColorIndex(int requestedColorIndex)
{
    if (requestedColorIndex >= 1 && requestedColorIndex <= 255) {
        return requestedColorIndex;
    }
    return 7;
}

std::size_t utf8CodepointLength(unsigned char ch)
{
    if ((ch & 0x80) == 0) {
        return 1;
    }
    if ((ch & 0xE0) == 0xC0) {
        return 2;
    }
    if ((ch & 0xF0) == 0xE0) {
        return 3;
    }
    if ((ch & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

std::string normalizedBrxLayerName(const std::string& value)
{
    const std::string input = trim(value);
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size();) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch < 128) {
            if (std::isalnum(ch) != 0) {
                out.push_back(static_cast<char>(ch));
            } else if (std::isspace(ch) != 0) {
                out.push_back(' ');
            } else {
                out.push_back('-');
            }
            ++i;
            continue;
        }

        if (i + 1 < input.size() && ch == 0xC3) {
            const unsigned char next = static_cast<unsigned char>(input[i + 1]);
            if (next == 0xA4 || next == 0xB6 || next == 0xBC
                || next == 0x84 || next == 0x96 || next == 0x9C) {
                out.push_back(input[i]);
                out.push_back(input[i + 1]);
                i += 2;
                continue;
            }
            if (next == 0x9F) {
                out += "ss";
                i += 2;
                continue;
            }
        }

        out.push_back('-');
        i += std::min<std::size_t>(utf8CodepointLength(ch), input.size() - i);
    }

    std::string collapsed;
    collapsed.reserve(out.size());
    bool previousSpace = false;
    bool previousHyphen = false;
    for (char ch : out) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!previousSpace && !previousHyphen) {
                collapsed.push_back(' ');
            }
            previousSpace = true;
            continue;
        }
        if (ch == '-') {
            if (!previousHyphen) {
                if (!collapsed.empty() && collapsed.back() == ' ') {
                    collapsed.pop_back();
                }
                collapsed.push_back('-');
            }
            previousHyphen = true;
            previousSpace = false;
            continue;
        }
        if (previousHyphen) {
            collapsed.push_back(' ');
        }
        collapsed.push_back(ch);
        previousSpace = false;
        previousHyphen = false;
    }

    std::string result = trim(collapsed);
    while (!result.empty() && result.front() == '-') {
        result.erase(result.begin());
        result = trim(result);
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
        result = trim(result);
    }
    return result;
}

AcDbObjectId defaultLayerLinetypeId(AcDbDatabase* database, AcDbLayerTable* layerTable)
{
    AcDbObjectId linetypeId;

    if (layerTable != nullptr) {
        AcDbLayerTableRecord* layerZero = nullptr;
        if (layerTable->getAt(_ACRX_T("0"), layerZero, AcDb::kForRead) == Acad::eOk && layerZero != nullptr) {
            linetypeId = layerZero->linetypeObjectId();
            layerZero->close();
        }
    }

    if (linetypeId.isNull() && database != nullptr && acdbSymUtil() != nullptr) {
        linetypeId = acdbSymUtil()->linetypeContinuousId(database);
    }

    if (linetypeId.isNull() && database != nullptr) {
        AcDbLinetypeTable* linetypeTable = nullptr;
        if (database->getLinetypeTable(linetypeTable, AcDb::kForRead) == Acad::eOk && linetypeTable != nullptr) {
            AcDbLinetypeTableRecord* record = nullptr;
            if (linetypeTable->getAt(_ACRX_T("Continuous"), record, AcDb::kForRead) == Acad::eOk && record != nullptr) {
                linetypeId = record->objectId();
                record->close();
            } else if (linetypeTable->getAt(_ACRX_T("CONTINUOUS"), record, AcDb::kForRead) == Acad::eOk && record != nullptr) {
                linetypeId = record->objectId();
                record->close();
            }
            linetypeTable->close();
        }
    }

    return linetypeId;
}

Acad::ErrorStatus initializeNewLayerRecord(
    AcDbLayerTableRecord* record,
    const std::basic_string<ACHAR>& nativeName)
{
    if (record == nullptr) {
        return Acad::eNullObjectPointer;
    }

    return record->setName(nativeName.c_str());
}

Acad::ErrorStatus stabilizeExistingLayerRecord(
    AcDbLayerTableRecord* record)
{
    if (record == nullptr) {
        return Acad::eNullObjectPointer;
    }

    return Acad::eOk;
}

Acad::ErrorStatus addInitializedLayerRecord(
    AcDbDatabase* database,
    AcDbLayerTable* layerTable,
    const std::basic_string<ACHAR>& nativeName,
    int requestedColorIndex)
{
    if (layerTable == nullptr) {
        return Acad::eNullObjectPointer;
    }

    auto* record = new AcDbLayerTableRecord();
    Acad::ErrorStatus status = initializeNewLayerRecord(record, nativeName);
    if (status != Acad::eOk) {
        delete record;
        return status;
    }

    AcDbObjectId layerId;
    status = layerTable->add(layerId, record);
    if (status == Acad::eOk) {
        if (requestedColorIndex >= 1 && requestedColorIndex <= 255) {
            AcCmColor color;
            color.setColorIndex(static_cast<Adesk::UInt16>(normalizedLayerColorIndex(requestedColorIndex)));
            (void)record->setColor(color);
        }
        record->close();
    } else {
        delete record;
    }
    return status;
}

std::string validationLayerKey(const std::string& layerName)
{
    return toUpperAscii(trim(layerName));
}

void addUniqueMessage(std::vector<std::string>& target, const std::string& message)
{
    if (!message.empty() && std::find(target.begin(), target.end(), message) == target.end()) {
        target.push_back(message);
    }
}

struct ActionValidationState {
    AcDbDatabase* database = nullptr;
    std::set<std::string> plannedLayers;
    std::set<std::string> removedLayers;
    bool plannedLastResultAvailable = false;
    std::string plannedLastResultKind;
    std::string plannedLastResultShape;
    bool plannedLastExtrudedSolids = false;
};

struct ActionValidationResult {
    std::string tool;
    std::vector<std::string> errors;
    std::vector<std::string> missing;
    std::vector<std::string> warnings;
    std::vector<std::string> hints;

    bool valid() const
    {
        return errors.empty() && missing.empty();
    }
};

bool layerExistsInDatabase(AcDbDatabase* database, const std::string& layerName)
{
    if (database == nullptr || trim(layerName).empty()) {
        return false;
    }

    AcDbLayerTable* layerTable = nullptr;
    if (database->getLayerTable(layerTable, AcDb::kForRead) != Acad::eOk || layerTable == nullptr) {
        return false;
    }
    const bool exists = layerTable->has(utf8ToAchar(layerName).c_str());
    layerTable->close();
    return exists;
}

bool layerExistsForValidation(const ActionValidationState& state, const std::string& layerName)
{
    const std::string key = validationLayerKey(layerName);
    if (key.empty() || state.removedLayers.find(key) != state.removedLayers.end()) {
        return false;
    }
    if (state.plannedLayers.find(key) != state.plannedLayers.end()) {
        return true;
    }
    return layerExistsInDatabase(state.database, layerName);
}

void markLayerPlanned(ActionValidationState& state, const std::string& layerName)
{
    const std::string key = validationLayerKey(layerName);
    if (!key.empty()) {
        state.plannedLayers.insert(key);
        state.removedLayers.erase(key);
    }
}

void markLayerRemoved(ActionValidationState& state, const std::string& layerName)
{
    const std::string key = validationLayerKey(layerName);
    if (!key.empty()) {
        state.removedLayers.insert(key);
        state.plannedLayers.erase(key);
    }
}

bool validateLayerReference(
    const ActionValidationState& state,
    const std::string& layerName,
    const std::string& path,
    std::vector<std::string>& errors,
    std::vector<std::string>& missing)
{
    const std::string trimmedLayer = trim(layerName);
    if (trimmedLayer.empty()) {
        addUniqueMessage(missing, path);
        return false;
    }
    if (!layerExistsForValidation(state, trimmedLayer)) {
        addUniqueMessage(errors, path + " verweist auf nicht vorhandenen Layer '" + trimmedLayer + "'");
        return false;
    }
    return true;
}

std::optional<double> finiteJsonNumberProperty(const std::string& object, const std::string& key)
{
    const std::optional<double> value = jsonDoubleProperty(object, key);
    if (!value.has_value() || !std::isfinite(*value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> firstFiniteNumberProperty(const std::string& object, const std::vector<std::string>& keys)
{
    for (const std::string& key : keys) {
        const std::optional<double> value = finiteJsonNumberProperty(object, key);
        if (value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool hasJsonPointValue(
    const std::string& object,
    const std::string& key,
    const std::string& path,
    std::vector<std::string>& errors,
    std::vector<std::string>& missing)
{
    if (const std::optional<std::string> point = jsonObjectProperty(object, key); point.has_value()) {
        const bool hasX = finiteJsonNumberProperty(*point, "x").has_value();
        const bool hasY = finiteJsonNumberProperty(*point, "y").has_value();
        if (!hasX) {
            addUniqueMessage(missing, path + ".x");
        }
        if (!hasY) {
            addUniqueMessage(missing, path + ".y");
        }
        if (jsonDoubleProperty(*point, "z").has_value() && !finiteJsonNumberProperty(*point, "z").has_value()) {
            addUniqueMessage(errors, path + ".z muss eine gueltige Zahl sein");
        }
        return hasX && hasY;
    }

    if (const std::optional<std::string> pointArray = jsonArrayProperty(object, key); pointArray.has_value()) {
        const std::vector<double> values = jsonNumberArrayValues(*pointArray);
        if (values.size() < 2) {
            addUniqueMessage(missing, path + " braucht mindestens [x,y]");
            return false;
        }
        for (double value : values) {
            if (!std::isfinite(value)) {
                addUniqueMessage(errors, path + " enthaelt keine gueltige Zahl");
                return false;
            }
        }
        return true;
    }

    addUniqueMessage(missing, path);
    return false;
}

bool hasAnyJsonPointValue(
    const std::string& object,
    const std::vector<std::string>& keys,
    const std::string& path,
    std::vector<std::string>& errors,
    std::vector<std::string>& missing)
{
    for (const std::string& key : keys) {
        if (jsonObjectProperty(object, key).has_value() || jsonArrayProperty(object, key).has_value()) {
            return hasJsonPointValue(object, key, path + "." + key, errors, missing);
        }
    }
    addUniqueMessage(missing, path + "." + keys.front());
    return false;
}

bool validateNonZeroVector(
    const std::string& paramsJson,
    const std::string& tool,
    std::vector<std::string>& errors,
    std::vector<std::string>& missing)
{
    bool hasVector = false;
    JsonPoint3d vector;
    if (jsonObjectProperty(paramsJson, "vector").has_value() || jsonArrayProperty(paramsJson, "vector").has_value()) {
        hasVector = hasJsonPointValue(paramsJson, "vector", tool + ".params.vector", errors, missing);
        vector = jsonPointProperty(paramsJson, "vector");
    } else if (jsonObjectProperty(paramsJson, "offset").has_value() || jsonArrayProperty(paramsJson, "offset").has_value()) {
        hasVector = hasJsonPointValue(paramsJson, "offset", tool + ".params.offset", errors, missing);
        vector = jsonPointProperty(paramsJson, "offset");
    } else if ((jsonObjectProperty(paramsJson, "fromPoint").has_value() || jsonArrayProperty(paramsJson, "fromPoint").has_value())
        || (jsonObjectProperty(paramsJson, "toPoint").has_value() || jsonArrayProperty(paramsJson, "toPoint").has_value())) {
        const bool fromOk = hasJsonPointValue(paramsJson, "fromPoint", tool + ".params.fromPoint", errors, missing);
        const bool toOk = hasJsonPointValue(paramsJson, "toPoint", tool + ".params.toPoint", errors, missing);
        if (fromOk && toOk) {
            const JsonPoint3d from = jsonPointProperty(paramsJson, "fromPoint");
            const JsonPoint3d to = jsonPointProperty(paramsJson, "toPoint");
            vector = JsonPoint3d{to.x - from.x, to.y - from.y, to.z - from.z};
            hasVector = true;
        }
    }

    if (!hasVector) {
        addUniqueMessage(missing, tool + ".params.vector oder offset");
        return false;
    }

    if (std::abs(vector.x) <= kRectangleTolerance
        && std::abs(vector.y) <= kRectangleTolerance
        && std::abs(vector.z) <= kRectangleTolerance) {
        addUniqueMessage(errors, tool + " braucht einen nicht-leeren Vektor/Offset");
        return false;
    }
    return true;
}

bool plannedTargetMatchesSelector(const std::string& selectorJson, const std::string& plannedKind, const std::string& plannedShape)
{
    if (plannedKind.empty()) {
        return false;
    }

    const std::string requestedKind = toUpperAscii(trim(jsonStringProperty(selectorJson, "kind").value_or("")));
    if (!requestedKind.empty()
        && requestedKind != "ENTITY"
        && requestedKind != plannedKind
        && !(requestedKind == "RECTANGLE" && plannedShape == "RECTANGLE")) {
        return false;
    }

    const std::string requestedShape = toUpperAscii(trim(jsonStringProperty(selectorJson, "shape").value_or("")));
    if (!requestedShape.empty() && requestedShape != plannedShape) {
        return false;
    }

    return true;
}

bool validateSelectorForAction(
    const ActionValidationState& state,
    const std::string& paramsJson,
    const std::string& tool,
    bool requireObjects,
    AcDbObjectIdArray* resolvedIds,
    std::vector<std::string>& errors,
    std::vector<std::string>& missing,
    std::vector<std::string>& debugLines)
{
    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        addUniqueMessage(missing, tool + ".params.selector, handles, handle, layer oder target");
        return false;
    }

    const std::string scope = jsonStringProperty(selectorJson, "scope").value_or("currentSpace");
    const std::vector<std::string> allowedScopes{"currentSpace", "selection", "handles", "lastResult", "lastExtruded"};
    if (std::find(allowedScopes.begin(), allowedScopes.end(), scope) == allowedScopes.end()) {
        addUniqueMessage(errors, tool + ".params.selector.scope ist nicht erlaubt: " + scope);
        return false;
    }

    if (const std::optional<std::string> layer = jsonStringProperty(selectorJson, "layer"); layer.has_value() && !layer->empty()) {
        validateLayerReference(state, *layer, tool + ".params.selector.layer", errors, missing);
    }

    if (scope == "handles") {
        const std::optional<std::string> handlesArray = jsonArrayProperty(selectorJson, "handles");
        const std::vector<std::string> handles = handlesArray.has_value() ? jsonStringArrayValues(*handlesArray) : std::vector<std::string>{};
        if (handles.empty()) {
            addUniqueMessage(missing, tool + ".params.selector.handles");
        }
        for (const std::string& handle : handles) {
            AcDbObjectId id;
            if (!objectIdFromHandleText(handle, id)) {
                addUniqueMessage(errors, tool + ".params.selector.handles enthaelt unbekannten Handle '" + handle + "'");
            }
        }
    }

    if (!errors.empty() || !missing.empty()) {
        return false;
    }

    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, state.database, debugLines);
    if (resolvedIds != nullptr) {
        *resolvedIds = ids;
    }

    if (requireObjects && ids.isEmpty()) {
        if (scope == "lastResult"
            && state.plannedLastResultAvailable
            && plannedTargetMatchesSelector(selectorJson, state.plannedLastResultKind, state.plannedLastResultShape)) {
            appendDebug(debugLines, "Validation accepts planned lastResult target");
            return true;
        }
        if (scope == "lastExtruded"
            && state.plannedLastExtrudedSolids
            && plannedTargetMatchesSelector(selectorJson, "SOLID", "")) {
            appendDebug(debugLines, "Validation accepts planned lastExtruded target");
            return true;
        }
        addUniqueMessage(errors, tool + ".params.selector findet keine Objekte");
        return false;
    }
    return true;
}

int countRectangleProfiles(const AcDbObjectIdArray& ids, std::vector<std::string>& debugLines)
{
    int count = 0;
    for (int i = 0; i < ids.length(); ++i) {
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus status = acdbOpenObject(entity, ids.at(i), AcDb::kForRead);
        if (status != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "validate rectangle open failed handle=" + objectHandleText(ids.at(i)) + ": " + errorStatusText(status));
            continue;
        }
        const bool isRectangle = entityShape(entity) == "rectangle";
        entity->close();
        if (isRectangle) {
            ++count;
        }
    }
    return count;
}

int countExtrudableProfiles(const AcDbObjectIdArray& ids, std::vector<std::string>& debugLines)
{
    int count = 0;
    for (int i = 0; i < ids.length(); ++i) {
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus status = acdbOpenObject(entity, ids.at(i), AcDb::kForRead);
        if (status != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "validate profile open failed handle=" + objectHandleText(ids.at(i)) + ": " + errorStatusText(status));
            continue;
        }
        const AcDbCurve* curve = AcDbCurve::cast(entity);
        bool accept = curve != nullptr;
        const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
        if (polyline != nullptr && polyline->isClosed() == Adesk::kFalse) {
            accept = false;
        }
        entity->close();
        if (accept) {
            ++count;
        }
    }
    return count;
}

int countSolids(const AcDbObjectIdArray& ids, std::vector<std::string>& debugLines)
{
    int count = 0;
    for (int i = 0; i < ids.length(); ++i) {
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus status = acdbOpenObject(entity, ids.at(i), AcDb::kForRead);
        if (status != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "validate solid open failed handle=" + objectHandleText(ids.at(i)) + ": " + errorStatusText(status));
            continue;
        }
        const bool solid = AcDb3dSolid::cast(entity) != nullptr;
        entity->close();
        if (solid) {
            ++count;
        }
    }
    return count;
}

bool validateGeometryCreateParams(
    const ActionValidationState& state,
    const std::string& paramsJson,
    ActionValidationResult& result)
{
    const std::string geometryType = toUpperAscii(trim(jsonStringProperty(paramsJson, "geometry").value_or(
        jsonStringProperty(paramsJson, "type").value_or(""))));
    if (geometryType.empty()) {
        addUniqueMessage(result.missing, "geometry.create.params.geometry");
        return false;
    }

    if (const std::optional<std::string> layer = jsonStringProperty(paramsJson, "layer"); layer.has_value() && !layer->empty()) {
        validateLayerReference(state, *layer, "geometry.create.params.layer", result.errors, result.missing);
    }

    if (geometryType == "POINT") {
        hasAnyJsonPointValue(paramsJson, {"position", "point"}, "geometry.create.params", result.errors, result.missing);
    } else if (geometryType == "RECTANGLE") {
        hasJsonPointValue(paramsJson, "origin", "geometry.create.params.origin", result.errors, result.missing);
        const std::optional<double> width = firstFiniteNumberProperty(paramsJson, {"width", "widthMm", "x"});
        const std::optional<double> depth = firstFiniteNumberProperty(paramsJson, {"depth", "depthMm", "length", "lengthMm", "y"});
        if (!width.has_value() || std::abs(*width) <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.width");
        }
        if (!depth.has_value() || std::abs(*depth) <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.depth oder length");
        }
    } else if (geometryType == "LINE") {
        const bool startOk = hasJsonPointValue(paramsJson, "start", "geometry.create.params.start", result.errors, result.missing);
        const bool endOk = hasJsonPointValue(paramsJson, "end", "geometry.create.params.end", result.errors, result.missing);
        if (startOk && endOk) {
            const JsonPoint3d start = jsonPointProperty(paramsJson, "start");
            const JsonPoint3d end = jsonPointProperty(paramsJson, "end");
            if (std::abs(start.x - end.x) <= kRectangleTolerance
                && std::abs(start.y - end.y) <= kRectangleTolerance
                && std::abs(start.z - end.z) <= kRectangleTolerance) {
                addUniqueMessage(result.errors, "geometry.create LINE braucht unterschiedliche start/end Punkte");
            }
        }
    } else if (geometryType == "CIRCLE") {
        hasAnyJsonPointValue(paramsJson, {"center", "origin"}, "geometry.create.params", result.errors, result.missing);
        const std::optional<double> radius = firstFiniteNumberProperty(paramsJson, {"radius", "radiusMm"});
        if (!radius.has_value() || *radius <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.radius");
        }
    } else if (geometryType == "ARC") {
        hasAnyJsonPointValue(paramsJson, {"center", "origin"}, "geometry.create.params", result.errors, result.missing);
        const std::optional<double> radius = firstFiniteNumberProperty(paramsJson, {"radius", "radiusMm"});
        if (!radius.has_value() || *radius <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.radius");
        }
        const double startAngle = jsonAngleRadians(paramsJson, "startAngle", "startAngleDeg", 0.0);
        const double endAngle = jsonAngleRadians(paramsJson, "endAngle", "endAngleDeg", 3.14159265358979323846 / 2.0);
        if (std::abs(startAngle - endAngle) <= kRectangleTolerance) {
            addUniqueMessage(result.errors, "geometry.create ARC braucht unterschiedliche start/end Winkel");
        }
    } else if (geometryType == "POLYLINE") {
        const std::optional<std::string> pointsArray = jsonArrayProperty(paramsJson, "points");
        const std::vector<std::string> points = pointsArray.has_value() ? jsonObjectArrayValues(*pointsArray) : std::vector<std::string>{};
        if (points.size() < 2) {
            addUniqueMessage(result.missing, "geometry.create.params.points mit mindestens zwei Punkten");
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            hasJsonPointValue(std::string("{\"point\":") + points[i] + "}", "point", "geometry.create.params.points[" + std::to_string(i) + "]", result.errors, result.missing);
        }
    } else if (geometryType == "BOX" || geometryType == "CUBOID" || geometryType == "QUADER") {
        hasJsonPointValue(paramsJson, "origin", "geometry.create.params.origin", result.errors, result.missing);
        const std::optional<double> width = firstFiniteNumberProperty(paramsJson, {"width", "widthMm", "x"});
        const std::optional<double> depth = firstFiniteNumberProperty(paramsJson, {"depth", "depthMm", "length", "lengthMm", "y"});
        const std::optional<double> height = firstFiniteNumberProperty(paramsJson, {"height", "heightMm", "z"});
        if (!width.has_value() || *width <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.width");
        }
        if (!depth.has_value() || *depth <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.depth oder length");
        }
        if (!height.has_value() || *height <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.create.params.height");
        }
    } else {
        addUniqueMessage(result.errors, "geometry.create unterstuetzt nur point, line, polyline, rectangle, circle, arc oder box");
    }
    return result.valid();
}

void rememberPlannedActionResult(ActionValidationState& state, const std::string& tool, const std::string& paramsJson)
{
    if (tool == "geometry.create") {
        const std::string geometryType = toUpperAscii(trim(jsonStringProperty(paramsJson, "geometry").value_or(
            jsonStringProperty(paramsJson, "type").value_or(""))));
        state.plannedLastResultAvailable = true;
        state.plannedLastResultShape.clear();
        if (geometryType == "RECTANGLE") {
            state.plannedLastResultKind = "POLYLINE";
            state.plannedLastResultShape = "RECTANGLE";
        } else if (geometryType == "BOX" || geometryType == "CUBOID" || geometryType == "QUADER") {
            state.plannedLastResultKind = "SOLID";
            state.plannedLastExtrudedSolids = true;
        } else {
            state.plannedLastResultKind = "ENTITY";
        }
        return;
    }

    if (tool == "rectangles.extrude" || tool == "profile.extrude") {
        state.plannedLastResultAvailable = true;
        state.plannedLastResultKind = "SOLID";
        state.plannedLastResultShape.clear();
        state.plannedLastExtrudedSolids = true;
    }
}

bool validateActionObject(
    const std::string& actionJson,
    ActionValidationState& state,
    ActionValidationResult& result,
    std::vector<std::string>& debugLines);

void validateLayerBatchParams(
    const std::string& paramsJson,
    ActionValidationState& state,
    ActionValidationResult& result,
    std::vector<std::string>& debugLines)
{
    const std::optional<std::string> actionsArray = jsonArrayProperty(paramsJson, "actions");
    if (!actionsArray.has_value()) {
        addUniqueMessage(result.missing, "layers.batch.params.actions");
        return;
    }
    const std::vector<std::string> actions = jsonObjectArrayValues(*actionsArray);
    if (actions.empty()) {
        addUniqueMessage(result.missing, "layers.batch.params.actions");
        return;
    }
    if (actions.size() > 50) {
        addUniqueMessage(result.errors, "layers.batch erlaubt maximal 50 Aktionen");
        return;
    }
    for (std::size_t i = 0; i < actions.size(); ++i) {
        ActionValidationResult nested;
        validateActionObject(actions[i], state, nested, debugLines);
        for (const std::string& message : nested.errors) {
            addUniqueMessage(result.errors, "layers.batch.actions[" + std::to_string(i) + "]: " + message);
        }
        for (const std::string& message : nested.missing) {
            addUniqueMessage(result.missing, "layers.batch.actions[" + std::to_string(i) + "]: " + message);
        }
        for (const std::string& message : nested.warnings) {
            addUniqueMessage(result.warnings, "layers.batch.actions[" + std::to_string(i) + "]: " + message);
        }
        for (const std::string& message : nested.hints) {
            addUniqueMessage(result.hints, message);
        }
    }
}

std::string stripCommandPrefixes(std::string token)
{
    token = trim(token);
    while (!token.empty() && (token.front() == '_' || token.front() == '.' || token.front() == '-')) {
        token.erase(token.begin());
    }
    return toUpperAscii(token);
}

std::string nativeCommandNameFromLine(const std::string& commandLine)
{
    std::string text = trim(commandLine);
    while (text.size() >= 2 && text[0] == '^' && (text[1] == 'C' || text[1] == 'c')) {
        text = trim(text.substr(2));
    }
    const std::size_t separator = text.find_first_of(" \t");
    const std::string token = separator == std::string::npos ? text : text.substr(0, separator);
    return stripCommandPrefixes(token);
}

bool nativeCommandLineHasArguments(const std::string& commandLine)
{
    std::string text = trim(commandLine);
    while (text.size() >= 2 && text[0] == '^' && (text[1] == 'C' || text[1] == 'c')) {
        text = trim(text.substr(2));
    }
    const std::size_t separator = text.find_first_of(" \t");
    if (separator == std::string::npos) {
        return false;
    }
    return !trim(text.substr(separator + 1)).empty();
}

bool isKnownBridgeCommand(const std::string& commandName)
{
    const std::string normalized = toUpperAscii(commandName);
    for (const BridgeCommandDescriptor& command : kBridgeCommands) {
        if (toUpperAscii(command.name) == normalized) {
            return true;
        }
    }
    return false;
}

bool commandExecuteAllowsNoArguments(const std::string& commandName)
{
    static const std::set<std::string> allowedWithoutArguments{
        "REDO",
        "SAVE",
    };
    return allowedWithoutArguments.find(toUpperAscii(commandName)) != allowedWithoutArguments.end();
}

void validateCommandExecuteParams(const std::string& paramsJson, ActionValidationResult& result)
{
    const std::string commandLine = trim(jsonStringProperty(paramsJson, "commandLine").value_or(""));
    if (commandLine.empty()) {
        addUniqueMessage(result.missing, "command.execute.params.commandLine");
        return;
    }
    if (commandLine.size() > 500) {
        addUniqueMessage(result.errors, "command.execute.params.commandLine darf maximal 500 Zeichen haben");
    }
    if (commandLine.find('\n') != std::string::npos
        || commandLine.find('\r') != std::string::npos
        || commandLine.find(';') != std::string::npos) {
        addUniqueMessage(result.errors, "command.execute erlaubt nur eine einzelne native Kommandozeile ohne Semikolon oder Zeilenumbruch");
    }

    const std::string commandName = nativeCommandNameFromLine(commandLine);
    if (commandName.empty()) {
        addUniqueMessage(result.missing, "command.execute.params.commandLine.command");
        return;
    }
    if (!isKnownBridgeCommand(commandName)) {
        addUniqueMessage(result.errors, "command.execute command '" + commandName + "' ist nicht in commands.list freigegeben");
        return;
    }
    if (!nativeCommandLineHasArguments(commandLine) && !commandExecuteAllowsNoArguments(commandName)) {
        addUniqueMessage(result.missing, "command.execute.params.commandLine braucht vollstaendige Argumente fuer " + commandName);
    }
    addUniqueMessage(result.hints, "command.execute wird vor Nutzerbestaetigung validiert und als einzelne native BricsCAD-Kommandozeile gepostet");
}

bool validateActionObject(
    const std::string& actionJson,
    ActionValidationState& state,
    ActionValidationResult& result,
    std::vector<std::string>& debugLines)
{
    result.tool = jsonStringProperty(actionJson, "tool").value_or(
        jsonStringProperty(actionJson, "method").value_or(
            jsonStringProperty(actionJson, "operation").value_or("")));
    if (result.tool == "create" || result.tool == "rename" || result.tool == "setColor") {
        result.tool = "layers." + result.tool;
    }
    if (result.tool.empty()) {
        addUniqueMessage(result.missing, "action.tool");
        return false;
    }

    const std::optional<std::string> params = jsonObjectProperty(actionJson, "params");
    std::string paramsJson = params.value_or("{}");
    if (!params.has_value() && result.tool != "document.save") {
        addUniqueMessage(result.missing, result.tool + ".params");
    }

    if (result.tool == "layers.create") {
        const std::string requestedName = trim(jsonStringProperty(paramsJson, "name").value_or(""));
        const std::string name = normalizedBrxLayerName(requestedName);
        bool planCreatedLayer = false;
        if (name.empty()) {
            addUniqueMessage(result.missing, "layers.create.params.name");
        } else {
            if (name != requestedName) {
                addUniqueMessage(result.hints, "layers.create.params.name wird vor Ausfuehrung zu '" + name + "' normalisiert");
            }
            if (validationLayerKey(name) == "0") {
                addUniqueMessage(result.errors, "Layer 0 darf nicht neu angelegt werden");
            }
            if (layerExistsForValidation(state, name)) {
                addUniqueMessage(result.warnings, "Layer '" + name + "' existiert bereits; layers.create wuerde ueberspringen");
            } else {
                planCreatedLayer = true;
            }
        }
        if (const std::optional<int> colorIndex = jsonIntProperty(paramsJson, "colorIndex"); colorIndex.has_value()
            && (*colorIndex < 1 || *colorIndex > 255)) {
            addUniqueMessage(result.errors, "layers.create.params.colorIndex muss 1..255 sein");
        }
        if (planCreatedLayer && result.valid()) {
            markLayerPlanned(state, name);
        }
    } else if (result.tool == "layers.rename") {
        const std::string oldName = trim(jsonStringProperty(paramsJson, "oldName").value_or(""));
        const std::string newName = trim(jsonStringProperty(paramsJson, "newName").value_or(""));
        if (oldName.empty()) {
            addUniqueMessage(result.missing, "layers.rename.params.oldName");
        }
        if (newName.empty()) {
            addUniqueMessage(result.missing, "layers.rename.params.newName");
        }
        if (!oldName.empty() && (validationLayerKey(oldName) == "0" || validationLayerKey(oldName) == "DEFPOINTS")) {
            addUniqueMessage(result.errors, "Layer '" + oldName + "' darf nicht umbenannt werden");
        }
        if (!oldName.empty() && !layerExistsForValidation(state, oldName)) {
            addUniqueMessage(result.errors, "layers.rename.params.oldName verweist auf nicht vorhandenen Layer '" + oldName + "'");
        }
        if (!newName.empty() && layerExistsForValidation(state, newName)) {
            addUniqueMessage(result.errors, "layers.rename.params.newName existiert bereits: '" + newName + "'");
        }
        if (result.valid()) {
            markLayerRemoved(state, oldName);
            markLayerPlanned(state, newName);
        }
    } else if (result.tool == "layers.setColor") {
        const std::string name = trim(jsonStringProperty(paramsJson, "name").value_or(""));
        validateLayerReference(state, name, "layers.setColor.params.name", result.errors, result.missing);
        const int colorIndex = jsonIntProperty(paramsJson, "colorIndex").value_or(0);
        if (colorIndex < 1 || colorIndex > 255) {
            addUniqueMessage(result.errors, "layers.setColor.params.colorIndex muss 1..255 sein");
        }
    } else if (result.tool == "layers.batch") {
        validateLayerBatchParams(paramsJson, state, result, debugLines);
    } else if (result.tool == "geometry.create") {
        validateGeometryCreateParams(state, paramsJson, result);
    } else if (result.tool == "geometry.move") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        validateNonZeroVector(paramsJson, result.tool, result.errors, result.missing);
    } else if (result.tool == "geometry.copy") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        validateNonZeroVector(paramsJson, result.tool, result.errors, result.missing);
        const int count = jsonIntProperty(paramsJson, "count").value_or(1);
        if (count < 1 || count > 100) {
            addUniqueMessage(result.errors, "geometry.copy.params.count muss 1..100 sein");
        }
    } else if (result.tool == "geometry.rotate") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        const std::string basePointMode = toUpperAscii(trim(jsonStringProperty(paramsJson, "basePointMode").value_or("")));
        const bool dynamicBasePoint = basePointMode == "ENTITYCENTER"
            || basePointMode == "EACHENTITYCENTER"
            || basePointMode == "OWNCENTER"
            || basePointMode == "SOLIDCENTER"
            || basePointMode == "BOUNDSCENTER"
            || basePointMode == "SELECTIONCENTER";
        if (basePointMode.empty()) {
            hasJsonPointValue(paramsJson, "basePoint", "geometry.rotate.params.basePoint", result.errors, result.missing);
        } else if (!dynamicBasePoint) {
            addUniqueMessage(result.errors, "geometry.rotate.params.basePointMode muss entityCenter, eachEntityCenter oder selectionCenter sein");
        }
        const double angle = jsonAngleRadians(paramsJson, "angleRad", "angleDeg", 0.0);
        if (!std::isfinite(angle) || std::abs(angle) <= kRectangleTolerance) {
            addUniqueMessage(result.missing, "geometry.rotate.params.angleDeg oder angleRad");
        }
        if ((jsonObjectProperty(paramsJson, "axis").has_value() || jsonArrayProperty(paramsJson, "axis").has_value())
            && hasJsonPointValue(paramsJson, "axis", "geometry.rotate.params.axis", result.errors, result.missing)) {
            const JsonPoint3d axis = jsonPointProperty(paramsJson, "axis");
            if (std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z) <= kRectangleTolerance) {
                addUniqueMessage(result.errors, "geometry.rotate.params.axis darf kein Nullvektor sein");
            }
        }
    } else if (result.tool == "geometry.scale") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        hasJsonPointValue(paramsJson, "basePoint", "geometry.scale.params.basePoint", result.errors, result.missing);
        const double factor = jsonDoubleProperty(paramsJson, "factor").value_or(0.0);
        if (!std::isfinite(factor) || factor <= kRectangleTolerance) {
            addUniqueMessage(result.errors, "geometry.scale.params.factor muss > 0 sein");
        }
        if (jsonDoubleProperty(paramsJson, "xFactor").has_value()
            || jsonDoubleProperty(paramsJson, "yFactor").has_value()
            || jsonDoubleProperty(paramsJson, "zFactor").has_value()) {
            addUniqueMessage(result.errors, "geometry.scale erlaubt nur uniformen factor, keine xFactor/yFactor/zFactor");
        }
    } else if (result.tool == "geometry.delete") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        if (!jsonBoolProperty(paramsJson, "confirm").value_or(false)) {
            addUniqueMessage(result.missing, "geometry.delete.params.confirm=true");
        }
    } else if (result.tool == "selection.set") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
    } else if (result.tool == "entity.setLayer") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        const std::string requestedLayer = trim(jsonStringProperty(paramsJson, "layer").value_or(
            jsonStringProperty(paramsJson, "targetLayer").value_or("")));
        const std::string layerName = normalizedBrxLayerName(requestedLayer);
        if (layerName.empty()) {
            addUniqueMessage(result.missing, "entity.setLayer.params.layer");
        } else {
            if (layerName != requestedLayer) {
                addUniqueMessage(result.hints, "entity.setLayer.params.layer wird vor Ausfuehrung zu '" + layerName + "' normalisiert");
            }
            const bool createIfMissing = jsonBoolProperty(paramsJson, "createIfMissing").value_or(false);
            if (!layerExistsForValidation(state, layerName)) {
                if (createIfMissing) {
                    markLayerPlanned(state, layerName);
                    addUniqueMessage(result.hints, "entity.setLayer legt den fehlenden Ziellayer '" + layerName + "' vor der Zuweisung an");
                } else {
                    addUniqueMessage(result.errors, "entity.setLayer.params.layer verweist auf nicht vorhandenen Layer '" + layerName + "'. Lege ihn vorher mit layers.create an oder setze createIfMissing=true");
                }
            }
        }
    } else if (result.tool == "rectangles.extrude") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        const double heightMm = jsonDoubleProperty(paramsJson, "heightMm").value_or(0.0);
        if (!std::isfinite(heightMm) || heightMm <= kRectangleTolerance) {
            addUniqueMessage(result.errors, "rectangles.extrude.params.heightMm muss > 0 sein");
        }
        if (!ids.isEmpty() && countRectangleProfiles(ids, debugLines) <= 0) {
            addUniqueMessage(result.errors, "rectangles.extrude Selector enthaelt keine geschlossenen Rechteck-Polylinien");
        }
    } else if (result.tool == "profile.extrude") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        const double heightMm = jsonDoubleProperty(paramsJson, "heightMm").value_or(
            jsonDoubleProperty(paramsJson, "height").value_or(0.0));
        if (!std::isfinite(heightMm) || heightMm <= kRectangleTolerance) {
            addUniqueMessage(result.errors, "profile.extrude.params.heightMm muss > 0 sein");
        }
        if (!ids.isEmpty() && countExtrudableProfiles(ids, debugLines) <= 0) {
            addUniqueMessage(result.errors, "profile.extrude Selector enthaelt keine extrudierbaren Profile");
        }
    } else if (result.tool == "bim.classify") {
        const std::string requestedClass = jsonStringProperty(paramsJson, "classification").value_or(
            jsonStringProperty(paramsJson, "class").value_or(""));
        const std::string bimClass = normalizeBimClass(requestedClass);
        if (bimClass.empty()) {
            addUniqueMessage(result.errors, "bim.classify.params.classification erlaubt aktuell nur BIMWall");
        }
        AcDbObjectIdArray ids;
        if (jsonObjectProperty(paramsJson, "selector").has_value()
            || jsonArrayProperty(paramsJson, "handles").has_value()
            || jsonStringProperty(paramsJson, "handle").has_value()
            || !jsonStringProperty(paramsJson, "target").value_or("").empty()) {
            validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        } else {
            ids = lastExtrudedSolidIds();
            if (ids.isEmpty()) {
                addUniqueMessage(result.errors, "bim.classify braucht selector/target oder zuletzt extrudierte Solids");
            }
        }
        if (!ids.isEmpty() && countSolids(ids, debugLines) <= 0) {
            addUniqueMessage(result.errors, "bim.classify Ziel enthaelt keine 3D-Solids");
        }
    } else if (result.tool == "document.save") {
        if (state.database == nullptr) {
            addUniqueMessage(result.errors, "Keine aktive BricsCAD Zeichnung gefunden");
        }
    } else if (result.tool == "undo.last" || result.tool == "undo.redo") {
        const int steps = jsonIntProperty(paramsJson, "steps").value_or(1);
        if (steps < 1 || steps > 50) {
            addUniqueMessage(result.errors, result.tool + ".params.steps muss 1..50 sein");
        }
    } else if (result.tool == "command.execute") {
        validateCommandExecuteParams(paramsJson, result);
    } else if (result.tool == "pipes.createNetworkSolids") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        if (jsonStringProperty(paramsJson, "system").value_or("").empty()) addUniqueMessage(result.missing, "pipes.createNetworkSolids.params.system");
        if (jsonStringProperty(paramsJson, "targetLayer").value_or("").empty()) addUniqueMessage(result.missing, "pipes.createNetworkSolids.params.targetLayer");
        if (jsonDoubleProperty(paramsJson, "diameterMm").value_or(0.0) <= 0.0) addUniqueMessage(result.errors, "pipes.createNetworkSolids.params.diameterMm muss > 0 sein");
    } else if (result.tool == "annotations.createRoomDimensions") {
        AcDbObjectIdArray ids;
        validateSelectorForAction(state, paramsJson, result.tool, true, &ids, result.errors, result.missing, debugLines);
        if (jsonStringProperty(paramsJson, "dimensionLayer").value_or("").empty()) addUniqueMessage(result.missing, "annotations.createRoomDimensions.params.dimensionLayer");
        if (jsonStringProperty(paramsJson, "labelLayer").value_or("").empty()) addUniqueMessage(result.missing, "annotations.createRoomDimensions.params.labelLayer");
        if (jsonDoubleProperty(paramsJson, "roomHeightMm").value_or(0.0) <= 0.0) addUniqueMessage(result.errors, "annotations.createRoomDimensions.params.roomHeightMm muss > 0 sein");
    } else if (result.tool == "capabilities.list"
        || result.tool == "actions.list"
        || result.tool == "actions.validate"
        || result.tool == "commands.list"
        || result.tool == "layers.list"
        || result.tool == "geometry.query"
        || result.tool == "selection.describe"
        || result.tool == "entity.describe"
        || result.tool == "measurement.bbox"
        || result.tool == "measurement.length"
        || result.tool == "measurement.area"
        || result.tool == "pipes.validateNetwork") {
        addUniqueMessage(result.hints, result.tool + " ist read-only und veraendert die Zeichnung nicht");
    } else {
        addUniqueMessage(result.errors, "Unbekannte oder nicht validierbare Action: " + result.tool);
    }

    if (result.valid()) {
        rememberPlannedActionResult(state, result.tool, paramsJson);
    }
    return result.valid();
}

std::string actionValidationResultJson(const ActionValidationResult& result, int index)
{
    std::ostringstream json;
    json << "{\"index\":" << index
        << ",\"tool\":\"" << jsonEscape(result.tool) << "\""
        << ",\"valid\":" << (result.valid() ? "true" : "false")
        << ",\"errors\":" << jsonDebugArray(result.errors)
        << ",\"missing\":" << jsonDebugArray(result.missing)
        << ",\"warnings\":" << jsonDebugArray(result.warnings)
        << ",\"hints\":" << jsonDebugArray(result.hints)
        << '}';
    return json.str();
}

std::string validateActionsInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Validation document lock: " + errorStatusText(docLock->lockStatus()));
    }

    std::vector<std::string> actions;
    if (const std::optional<std::string> actionsArray = jsonArrayProperty(paramsJson, "actions"); actionsArray.has_value()) {
        actions = jsonObjectArrayValues(*actionsArray);
    } else if (jsonStringProperty(paramsJson, "tool").has_value() || jsonStringProperty(paramsJson, "method").has_value()) {
        actions.push_back(paramsJson);
    }

    if (actions.empty()) {
        return fail("actions.validate braucht actions[] oder tool+params");
    }
    if (actions.size() > 50) {
        return fail("actions.validate erlaubt maximal 50 Aktionen");
    }

    ActionValidationState state;
    state.database = database;
    std::vector<std::string> errors;
    std::vector<std::string> missing;
    std::vector<std::string> warnings;
    std::vector<std::string> hints;

    std::ostringstream actionResults;
    actionResults << '[';
    for (std::size_t i = 0; i < actions.size(); ++i) {
        ActionValidationResult result;
        validateActionObject(actions[i], state, result, debugLines);
        if (i > 0) {
            actionResults << ',';
        }
        actionResults << actionValidationResultJson(result, static_cast<int>(i + 1));
        for (const std::string& message : result.errors) {
            addUniqueMessage(errors, "Aktion " + std::to_string(i + 1) + " (" + result.tool + "): " + message);
        }
        for (const std::string& message : result.missing) {
            addUniqueMessage(missing, "Aktion " + std::to_string(i + 1) + " (" + result.tool + "): " + message);
        }
        for (const std::string& message : result.warnings) {
            addUniqueMessage(warnings, "Aktion " + std::to_string(i + 1) + " (" + result.tool + "): " + message);
        }
        for (const std::string& message : result.hints) {
            addUniqueMessage(hints, message);
        }
    }
    actionResults << ']';

    if (!missing.empty()) {
        addUniqueMessage(hints, "Fehlende Werte ueber ask_user erfragen oder mit nachvollziehbaren Default-Werten explizit in params setzen.");
    }
    if (!errors.empty()) {
        addUniqueMessage(hints, "Korrigiere tool/params anhand der BRX-Capabilities, bevor der Nutzer eine Ausfuehrung bestaetigt.");
    }

    const bool valid = errors.empty() && missing.empty();
    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.actions.validate.result.v1\""
        << ",\"valid\":" << (valid ? "true" : "false")
        << ",\"actionsRequested\":" << actions.size()
        << ",\"errors\":" << jsonDebugArray(errors)
        << ",\"missing\":" << jsonDebugArray(missing)
        << ",\"warnings\":" << jsonDebugArray(warnings)
        << ",\"hints\":" << jsonDebugArray(hints)
        << ",\"actions\":" << actionResults.str()
        << '}';
    return okJsonResultResponse(json.str(), debugLines);
}

JsonPoint3d actionVectorFromParams(const std::string& paramsJson)
{
    if (jsonObjectProperty(paramsJson, "vector").has_value()) {
        return jsonPointProperty(paramsJson, "vector");
    }
    if (jsonObjectProperty(paramsJson, "offset").has_value()) {
        return jsonPointProperty(paramsJson, "offset");
    }
    if (jsonObjectProperty(paramsJson, "fromPoint").has_value() && jsonObjectProperty(paramsJson, "toPoint").has_value()) {
        const JsonPoint3d from = jsonPointProperty(paramsJson, "fromPoint");
        const JsonPoint3d to = jsonPointProperty(paramsJson, "toPoint");
        return JsonPoint3d{to.x - from.x, to.y - from.y, to.z - from.z};
    }
    return {};
}

std::string transformEntitiesInApplicationContext(const std::string& paramsJson, const std::string& operation)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail(operation + " braucht selector, handles, handle oder layer");
    }

    AcGeMatrix3d matrix;
    JsonPoint3d moveVector{};
    const bool useNativeMove = operation == "move";
    bool useEntityCenterBasePoint = false;
    bool useSelectionCenterBasePoint = false;
    double rotateAngle = 0.0;
    AcGeVector3d rotateAxis;
    if (operation == "move") {
        moveVector = actionVectorFromParams(paramsJson);
        if (std::abs(moveVector.x) <= kRectangleTolerance && std::abs(moveVector.y) <= kRectangleTolerance && std::abs(moveVector.z) <= kRectangleTolerance) {
            return fail("geometry.move braucht einen nicht-leeren vector/offset oder fromPoint/toPoint");
        }
    } else if (operation == "rotate") {
        const JsonPoint3d base = jsonPointProperty(paramsJson, "basePoint");
        const std::string basePointMode = toUpperAscii(trim(jsonStringProperty(paramsJson, "basePointMode").value_or("")));
        useEntityCenterBasePoint = basePointMode == "ENTITYCENTER"
            || basePointMode == "EACHENTITYCENTER"
            || basePointMode == "OWNCENTER"
            || basePointMode == "SOLIDCENTER"
            || basePointMode == "BOUNDSCENTER";
        useSelectionCenterBasePoint = basePointMode == "SELECTIONCENTER";
        const JsonPoint3d axisValue = jsonPointProperty(paramsJson, "axis", JsonPoint3d{0.0, 0.0, 1.0});
        rotateAngle = jsonAngleRadians(paramsJson, "angleRad", "angleDeg", 0.0);
        if (!std::isfinite(rotateAngle) || std::abs(rotateAngle) <= kRectangleTolerance) {
            return fail("geometry.rotate braucht angleDeg oder angleRad != 0");
        }
        rotateAxis = AcGeVector3d(axisValue.x, axisValue.y, axisValue.z);
        if (rotateAxis.length() <= kRectangleTolerance) {
            return fail("geometry.rotate braucht eine gueltige Achse");
        }
        if (!useEntityCenterBasePoint && !useSelectionCenterBasePoint) {
            matrix = AcGeMatrix3d::rotation(rotateAngle, rotateAxis, AcGePoint3d(base.x, base.y, base.z));
        }
    } else if (operation == "scale") {
        const JsonPoint3d base = jsonPointProperty(paramsJson, "basePoint");
        const double factor = jsonDoubleProperty(paramsJson, "factor").value_or(0.0);
        if (!std::isfinite(factor) || factor <= kRectangleTolerance) {
            return fail("geometry.scale unterstuetzt aktuell nur uniforme Skalierung mit factor > 0");
        }
        matrix = AcGeMatrix3d::scaling(factor, AcGePoint3d(base.x, base.y, base.z));
    } else {
        return fail("Unbekannte Transform-Operation: " + operation);
    }

    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, database, debugLines);
    AcGePoint3d selectionCenter;
    if (useSelectionCenterBasePoint) {
        bool hasSelectionExtents = false;
        AcDbExtents selectionExtents;
        for (int i = 0; i < ids.length(); ++i) {
            AcDbEntity* entity = nullptr;
            const Acad::ErrorStatus openStatus = acdbOpenObject(entity, ids.at(i), AcDb::kForRead);
            if (openStatus != Acad::eOk || entity == nullptr) {
                continue;
            }
            AcDbExtents entityExtents;
            const Acad::ErrorStatus extentsStatus = entity->getGeomExtents(entityExtents);
            entity->close();
            if (extentsStatus != Acad::eOk) {
                continue;
            }
            if (!hasSelectionExtents) {
                selectionExtents = entityExtents;
                hasSelectionExtents = true;
            } else {
                selectionExtents.addExt(entityExtents);
            }
        }
        if (!hasSelectionExtents) {
            return fail("geometry.rotate basePointMode=selectionCenter konnte keinen Auswahl-Mittelpunkt berechnen");
        }
        const AcGePoint3d minPoint = selectionExtents.minPoint();
        const AcGePoint3d maxPoint = selectionExtents.maxPoint();
        selectionCenter = AcGePoint3d(
            (minPoint.x + maxPoint.x) * 0.5,
            (minPoint.y + maxPoint.y) * 0.5,
            (minPoint.z + maxPoint.z) * 0.5);
        matrix = AcGeMatrix3d::rotation(rotateAngle, rotateAxis, selectionCenter);
        appendDebug(debugLines, "geometry.rotate basePointMode=selectionCenter center="
            + std::to_string(selectionCenter.x) + ","
            + std::to_string(selectionCenter.y) + ","
            + std::to_string(selectionCenter.z));
    }
    if (useNativeMove) {
        AcDbObjectIdArray affectedIds;
        AcDbObjectIdArray failedIds;
        if (ids.isEmpty()) {
            return fail("geometry.move selector findet keine Objekte");
        }

        if (docLock != nullptr) {
            docLock.reset();
            appendDebug(debugLines, "Document lock released before native MOVE");
        }

        ads_name selectionSet{};
        if (!createSelectionSet(ids, selectionSet, debugLines)) {
            for (int i = 0; i < ids.length(); ++i) {
                failedIds.append(ids.at(i));
            }
        } else {
            const int commandStatus = runNativeMoveCommand(selectionSet, moveVector, debugLines);
            if (commandStatus == RTNORM) {
                affectedIds = ids;
                rememberLastResult(affectedIds, "geometry.move", debugLines);
            } else {
                for (int i = 0; i < ids.length(); ++i) {
                    failedIds.append(ids.at(i));
                }
            }
            const int freeStatus = acedSSFree(selectionSet);
            appendDebug(debugLines, "acedSSFree geometry.move status=" + std::to_string(freeStatus));
        }

        const std::string schema = "barebone.bricscad.geometry.move.result.v1";
        const std::string summary = "geometry.move affected=" + std::to_string(affectedIds.length())
            + " failed=" + std::to_string(failedIds.length());
        return okJsonResultResponse(genericActionResultJson(schema, summary, affectedIds, failedIds, saveBefore, savedBefore), debugLines);
    }

    AcDbObjectIdArray affectedIds;
    AcDbObjectIdArray failedIds;
    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId id = ids.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForWrite);
        if (openStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, operation + " open failed handle=" + objectHandleText(id) + ": " + errorStatusText(openStatus));
            failedIds.append(id);
            continue;
        }
        AcGeMatrix3d entityMatrix = matrix;
        if (useEntityCenterBasePoint) {
            AcDbExtents extents;
            const Acad::ErrorStatus extentsStatus = entity->getGeomExtents(extents);
            if (extentsStatus != Acad::eOk) {
                appendDebug(debugLines, operation + " extents failed handle=" + objectHandleText(id) + ": " + errorStatusText(extentsStatus));
                entity->close();
                failedIds.append(id);
                continue;
            }
            const AcGePoint3d minPoint = extents.minPoint();
            const AcGePoint3d maxPoint = extents.maxPoint();
            const AcGePoint3d entityCenter(
                (minPoint.x + maxPoint.x) * 0.5,
                (minPoint.y + maxPoint.y) * 0.5,
                (minPoint.z + maxPoint.z) * 0.5);
            entityMatrix = AcGeMatrix3d::rotation(rotateAngle, rotateAxis, entityCenter);
            appendDebug(debugLines, operation + " entityCenter handle=" + objectHandleText(id)
                + " center=" + std::to_string(entityCenter.x)
                + "," + std::to_string(entityCenter.y)
                + "," + std::to_string(entityCenter.z));
        }
        const Acad::ErrorStatus transformStatus = entity->transformBy(entityMatrix);
        entity->close();
        appendDebug(debugLines, operation + " transform handle=" + objectHandleText(id) + ": " + errorStatusText(transformStatus));
        if (transformStatus == Acad::eOk) {
            affectedIds.append(id);
        } else {
            failedIds.append(id);
        }
    }

    const std::string schema = "barebone.bricscad.geometry." + operation + ".result.v1";
    const std::string summary = "geometry." + operation + " affected=" + std::to_string(affectedIds.length())
        + " failed=" + std::to_string(failedIds.length());
    return okJsonResultResponse(genericActionResultJson(schema, summary, affectedIds, failedIds, saveBefore, savedBefore), debugLines);
}

std::string copyEntitiesInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail("geometry.copy braucht selector, handles, handle oder layer");
    }

    JsonPoint3d vector = actionVectorFromParams(paramsJson);
    if (std::abs(vector.x) <= kRectangleTolerance && std::abs(vector.y) <= kRectangleTolerance && std::abs(vector.z) <= kRectangleTolerance
        && jsonObjectProperty(paramsJson, "spacing").has_value()) {
        vector = jsonPointProperty(paramsJson, "spacing");
    }
    if (std::abs(vector.x) <= kRectangleTolerance && std::abs(vector.y) <= kRectangleTolerance && std::abs(vector.z) <= kRectangleTolerance) {
        return fail("geometry.copy braucht vector, offset oder spacing");
    }

    const int count = std::max(1, jsonIntProperty(paramsJson, "count").value_or(1));
    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    AcDbBlockTableRecord* space = nullptr;
    const Acad::ErrorStatus openSpaceStatus = acdbOpenObject(space, database->currentSpaceId(), AcDb::kForWrite);
    if (openSpaceStatus != Acad::eOk || space == nullptr) {
        return fail("Aktueller Zeichenbereich konnte nicht geoeffnet werden: " + errorStatusText(openSpaceStatus));
    }

    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, database, debugLines);
    AcDbObjectIdArray affectedIds;
    AcDbObjectIdArray failedIds;
    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId id = ids.at(i);
        AcDbEntity* source = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(source, id, AcDb::kForRead);
        if (openStatus != Acad::eOk || source == nullptr) {
            failedIds.append(id);
            appendDebug(debugLines, "copy open failed handle=" + objectHandleText(id) + ": " + errorStatusText(openStatus));
            continue;
        }

        for (int copyIndex = 1; copyIndex <= count; ++copyIndex) {
            AcRxObject* cloneObject = source->clone();
            AcDbEntity* cloneEntity = AcDbEntity::cast(cloneObject);
            if (cloneEntity == nullptr) {
                delete cloneObject;
                failedIds.append(id);
                appendDebug(debugLines, "copy clone failed handle=" + objectHandleText(id));
                continue;
            }
            const AcGeMatrix3d matrix = AcGeMatrix3d::translation(AcGeVector3d(vector.x * copyIndex, vector.y * copyIndex, vector.z * copyIndex));
            const Acad::ErrorStatus transformStatus = cloneEntity->transformBy(matrix);
            if (transformStatus != Acad::eOk) {
                delete cloneEntity;
                failedIds.append(id);
                appendDebug(debugLines, "copy transform failed handle=" + objectHandleText(id) + ": " + errorStatusText(transformStatus));
                continue;
            }

            AcDbObjectId newId;
            const Acad::ErrorStatus appendStatus = space->appendAcDbEntity(newId, cloneEntity);
            if (appendStatus == Acad::eOk) {
                cloneEntity->close();
                affectedIds.append(newId);
                appendDebug(debugLines, "copy created source=" + objectHandleText(id) + " handle=" + objectHandleText(newId));
            } else {
                delete cloneEntity;
                failedIds.append(id);
                appendDebug(debugLines, "copy append failed source=" + objectHandleText(id) + ": " + errorStatusText(appendStatus));
            }
        }
        source->close();
    }

    space->close();
    const std::string summary = "geometry.copy created=" + std::to_string(affectedIds.length())
        + " failed=" + std::to_string(failedIds.length());
    return okJsonResultResponse(genericActionResultJson("barebone.bricscad.geometry.copy.result.v1", summary, affectedIds, failedIds, saveBefore, savedBefore), debugLines);
}

std::string deleteEntitiesInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    if (!jsonBoolProperty(paramsJson, "confirm").value_or(false)) {
        return fail("geometry.delete braucht confirm=true");
    }
    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail("geometry.delete braucht selector, handles, handle oder layer");
    }

    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, database, debugLines);
    AcDbObjectIdArray affectedIds;
    AcDbObjectIdArray failedIds;
    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId id = ids.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForWrite);
        if (openStatus != Acad::eOk || entity == nullptr) {
            failedIds.append(id);
            appendDebug(debugLines, "delete open failed handle=" + objectHandleText(id) + ": " + errorStatusText(openStatus));
            continue;
        }
        const Acad::ErrorStatus eraseStatus = entity->erase();
        entity->close();
        appendDebug(debugLines, "delete erase handle=" + objectHandleText(id) + ": " + errorStatusText(eraseStatus));
        if (eraseStatus == Acad::eOk) {
            affectedIds.append(id);
        } else {
            failedIds.append(id);
        }
    }

    const std::string summary = "geometry.delete affected=" + std::to_string(affectedIds.length())
        + " failed=" + std::to_string(failedIds.length());
    return okJsonResultResponse(genericActionResultJson("barebone.bricscad.geometry.delete.result.v1", summary, affectedIds, failedIds, saveBefore, savedBefore), debugLines);
}

std::string setSelectionInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }
    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail("selection.set braucht selector, handles, handle oder layer");
    }
    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, database, debugLines);

    ads_name selectionSet{};
    if (!createSelectionSet(ids, selectionSet, debugLines)) {
        return fail("SelectionSet konnte nicht aus Selector erstellt werden");
    }
    const int status = acedSSSetFirst(nullptr, selectionSet);
    appendDebug(debugLines, "acedSSSetFirst status=" + std::to_string(status));
    const int freeStatus = acedSSFree(selectionSet);
    appendDebug(debugLines, "acedSSFree selection.set status=" + std::to_string(freeStatus));
    if (status == RTNORM) {
        rememberSelectionSnapshot(ids, "selection.set", &debugLines);
    }

    AcDbObjectIdArray failedIds;
    const std::string summary = "selection.set selected=" + std::to_string(status == RTNORM ? ids.length() : 0);
    return okJsonResultResponse(genericActionResultJson("barebone.bricscad.selection.set.result.v1", summary, status == RTNORM ? ids : failedIds, status == RTNORM ? failedIds : ids, false, false), debugLines);
}

std::string saveDocumentInApplicationContext(const std::string& paramsJson)
{
    (void)paramsJson;
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    AcDbDatabase* database = workingDatabase();
    std::string errorMessage;
    if (!saveWorkingDatabaseBeforeAction(database, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    AcDbObjectIdArray affected;
    AcDbObjectIdArray failed;
    std::ostringstream json;
    json << genericActionResultJson("barebone.bricscad.document.save.result.v1", "document.save saved active drawing", affected, failed, false, true);
    return okJsonResultResponse(json.str(), debugLines);
}

Acad::ErrorStatus applyLayerMutation(
    AcDbDatabase* database,
    const std::string& paramsJson,
    const std::string& operation,
    std::string& summary,
    bool& skipped,
    std::vector<std::string>& debugLines);

std::string setEntityLayerInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    const std::string requestedLayer = trim(jsonStringProperty(paramsJson, "layer").value_or(
        jsonStringProperty(paramsJson, "targetLayer").value_or("")));
    const std::string layerName = normalizedBrxLayerName(requestedLayer);
    if (layerName.empty()) {
        return fail("entity.setLayer braucht layer");
    }
    if (layerName != requestedLayer) {
        appendDebug(debugLines, "entity.setLayer normalized layer '" + requestedLayer + "' -> '" + layerName + "'");
    }

    const bool createIfMissing = jsonBoolProperty(paramsJson, "createIfMissing").value_or(false);
    if (!layerExistsInDatabase(database, layerName)) {
        if (!createIfMissing) {
            return fail("entity.setLayer Ziellayer nicht gefunden: " + layerName);
        }
        std::string createSummary;
        bool skipped = false;
        const std::string createParams = std::string("{\"name\":\"") + jsonEscape(layerName) + "\"}";
        const Acad::ErrorStatus createStatus = applyLayerMutation(database, createParams, "create", createSummary, skipped, debugLines);
        appendDebug(debugLines, "entity.setLayer createIfMissing: " + createSummary + " status=" + errorStatusText(createStatus));
        if (createStatus != Acad::eOk || !layerExistsInDatabase(database, layerName)) {
            return fail("entity.setLayer konnte Ziellayer nicht anlegen: " + createSummary);
        }
    }

    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail("entity.setLayer braucht selector, handles, handle oder layer");
    }

    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, database, debugLines);
    if (ids.isEmpty()) {
        return fail("entity.setLayer selector findet keine Objekte");
    }

    AcDbObjectIdArray affectedIds;
    AcDbObjectIdArray failedIds;
    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId id = ids.at(i);
        if (setEntityLayerByName(id, layerName, debugLines)) {
            affectedIds.append(id);
        } else {
            failedIds.append(id);
        }
    }

    if (!affectedIds.isEmpty()) {
        rememberLastResult(affectedIds, "entity.setLayer", debugLines);
    }

    const std::string summary = "entity.setLayer affected=" + std::to_string(affectedIds.length())
        + " failed=" + std::to_string(failedIds.length())
        + " layer=" + layerName;
    return okJsonResultResponse(genericActionResultJson("barebone.bricscad.entity.setLayer.result.v1", summary, affectedIds, failedIds, saveBefore, savedBefore), debugLines);
}

std::string mutateLayerInApplicationContext(const std::string& paramsJson, const std::string& operation)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    std::string summary;
    bool skipped = false;
    const Acad::ErrorStatus status = applyLayerMutation(database, paramsJson, operation, summary, skipped, debugLines);
    appendDebug(debugLines, "Layer mutation executed via native BricsCAD -LAYER command path");
    if (status != Acad::eOk) {
        return fail(summary + " fehlgeschlagen: " + errorStatusText(status));
    }

    AcDbObjectIdArray affected;
    AcDbObjectIdArray failed;
    const std::string schema = "barebone.bricscad.layers." + operation + ".result.v1";
    return okJsonResultResponse(genericActionResultJson(schema, summary, affected, failed, saveBefore, savedBefore), debugLines);
}

std::string layerOperationFromAction(const std::string& actionJson)
{
    std::string operation = jsonStringProperty(actionJson, "operation").value_or("");
    if (operation.empty()) {
        operation = jsonStringProperty(actionJson, "tool").value_or(
            jsonStringProperty(actionJson, "method").value_or(""));
    }
    if (operation == "layers.create" || operation == "create") {
        return "create";
    }
    if (operation == "layers.rename" || operation == "rename") {
        return "rename";
    }
    if (operation == "layers.setColor" || operation == "setColor") {
        return "setColor";
    }
    return {};
}

std::string layerParamsFromAction(const std::string& actionJson)
{
    if (const std::optional<std::string> params = jsonObjectProperty(actionJson, "params"); params.has_value()) {
        return *params;
    }
    return actionJson;
}

Acad::ErrorStatus layerCommandStatus(int commandStatus)
{
    return commandStatus == RTNORM ? Acad::eOk : Acad::eInvalidInput;
}

int runNativeLayerNewCommand(const std::string& name, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.-LAYER _New")) {
        return RTERROR;
    }
    appendDebug(debugLines, "Calling acedCommand _.-LAYER _New name='" + name + "'");
    const std::basic_string<ACHAR> nativeName = utf8ToAchar(name);
    const int commandStatus = acedCommand(
        RTSTR, _T("_.-LAYER"),
        RTSTR, _T("_New"),
        RTSTR, nativeName.c_str(),
        RTSTR, _T(""),
        RTNONE);
    appendDebug(debugLines, "acedCommand _.-LAYER _New status=" + std::to_string(commandStatus));
    return commandStatus;
}

int runNativeLayerRenameCommand(const std::string& oldName, const std::string& newName, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.-LAYER _Rename")) {
        return RTERROR;
    }
    appendDebug(debugLines, "Calling acedCommand _.-LAYER _Rename old='" + oldName + "' new='" + newName + "'");
    const std::basic_string<ACHAR> nativeOldName = utf8ToAchar(oldName);
    const std::basic_string<ACHAR> nativeNewName = utf8ToAchar(newName);
    const int commandStatus = acedCommand(
        RTSTR, _T("_.-LAYER"),
        RTSTR, _T("_Rename"),
        RTSTR, nativeOldName.c_str(),
        RTSTR, nativeNewName.c_str(),
        RTSTR, _T(""),
        RTNONE);
    appendDebug(debugLines, "acedCommand _.-LAYER _Rename status=" + std::to_string(commandStatus));
    return commandStatus;
}

int runNativeLayerColorCommand(const std::string& name, int colorIndex, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.-LAYER _Color")) {
        return RTERROR;
    }
    appendDebug(debugLines, "Calling acedCommand _.-LAYER _Color name='" + name + "' colorIndex=" + std::to_string(colorIndex));
    const std::basic_string<ACHAR> nativeName = utf8ToAchar(name);
    const std::basic_string<ACHAR> nativeColor = utf8ToAchar(std::to_string(colorIndex));
    const int commandStatus = acedCommand(
        RTSTR, _T("_.-LAYER"),
        RTSTR, _T("_Color"),
        RTSTR, nativeColor.c_str(),
        RTSTR, nativeName.c_str(),
        RTSTR, _T(""),
        RTNONE);
    appendDebug(debugLines, "acedCommand _.-LAYER _Color status=" + std::to_string(commandStatus));
    return commandStatus;
}

Acad::ErrorStatus applyLayerMutation(
    AcDbDatabase* database,
    const std::string& paramsJson,
    const std::string& operation,
    std::string& summary,
    bool& skipped,
    std::vector<std::string>& debugLines)
{
    skipped = false;
    if (database == nullptr) {
        summary = "Keine aktive Zeichnung";
        return Acad::eNullObjectPointer;
    }

    if (operation == "create") {
        const std::string requestedName = trim(jsonStringProperty(paramsJson, "name").value_or(""));
        const std::string name = normalizedBrxLayerName(requestedName);
        if (name.empty()) {
            summary = "layers.create braucht name";
            return Acad::eInvalidInput;
        }
        if (name != requestedName) {
            appendDebug(debugLines, "layers.create normalized name '" + requestedName + "' -> '" + name + "'");
        }
        if (layerExistsInDatabase(database, name)) {
            skipped = true;
            summary = "layers.create skipped existing layer " + name;
            return Acad::eOk;
        }

        const int colorIndex = jsonIntProperty(paramsJson, "colorIndex").value_or(0);
        Acad::ErrorStatus status = layerCommandStatus(runNativeLayerNewCommand(name, debugLines));
        if (status == Acad::eOk && !layerExistsInDatabase(database, name)) {
            summary = "layers.create " + name + " command returned success but layer was not found after creation";
            return Acad::eInvalidInput;
        }
        if (status == Acad::eOk && colorIndex >= 1 && colorIndex <= 255) {
            status = layerCommandStatus(runNativeLayerColorCommand(name, colorIndex, debugLines));
        }
        summary = "layers.create " + name;
        return status;
    }

    if (operation == "rename") {
        const std::string oldName = trim(jsonStringProperty(paramsJson, "oldName").value_or(""));
        const std::string newName = trim(jsonStringProperty(paramsJson, "newName").value_or(""));
        if (oldName.empty() || newName.empty()) {
            summary = "layers.rename braucht oldName und newName";
            return Acad::eInvalidInput;
        }
        if (oldName == newName) {
            skipped = true;
            summary = "layers.rename skipped unchanged layer " + oldName;
            return Acad::eOk;
        }
        if (!layerExistsInDatabase(database, oldName)) {
            summary = "layers.rename oldName nicht gefunden: " + oldName;
            return Acad::eInvalidInput;
        }
        if (layerExistsInDatabase(database, newName)) {
            summary = "layers.rename newName existiert bereits: " + newName;
            return Acad::eInvalidInput;
        }
        Acad::ErrorStatus status = layerCommandStatus(runNativeLayerRenameCommand(oldName, newName, debugLines));
        if (status == Acad::eOk && !layerExistsInDatabase(database, newName)) {
            summary = "layers.rename command returned success but new layer was not found: " + newName;
            return Acad::eInvalidInput;
        }
        summary = "layers.rename " + oldName + " -> " + newName;
        return status;
    }

    if (operation == "setColor") {
        const std::string name = trim(jsonStringProperty(paramsJson, "name").value_or(""));
        const int colorIndex = jsonIntProperty(paramsJson, "colorIndex").value_or(0);
        if (name.empty() || colorIndex < 1 || colorIndex > 255) {
            summary = "layers.setColor braucht name und colorIndex 1..255";
            return Acad::eInvalidInput;
        }
        if (!layerExistsInDatabase(database, name)) {
            summary = "layers.setColor Layer nicht gefunden: " + name;
            return Acad::eInvalidInput;
        }
        const Acad::ErrorStatus status = layerCommandStatus(runNativeLayerColorCommand(name, colorIndex, debugLines));
        summary = "layers.setColor " + name + " colorIndex=" + std::to_string(colorIndex);
        return status;
    }

    summary = "Unbekannte Layer-Operation: " + operation;
    return Acad::eInvalidInput;
}

std::string mutateLayerBatchInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::optional<std::string> actionsArray = jsonArrayProperty(paramsJson, "actions");
    if (!actionsArray.has_value()) {
        return fail("layers.batch braucht actions[]");
    }

    const std::vector<std::string> actions = jsonObjectArrayValues(*actionsArray);
    if (actions.empty()) {
        return fail("layers.batch actions[] ist leer");
    }
    if (actions.size() > 50) {
        return fail("layers.batch erlaubt maximal 50 Aktionen");
    }

    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    int completed = 0;
    int skipped = 0;
    int failed = 0;
    std::ostringstream actionResults;
    actionResults << '[';
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const std::string operation = layerOperationFromAction(actions[i]);
        const std::string actionParams = layerParamsFromAction(actions[i]);
        std::string summary;
        bool wasSkipped = false;
        Acad::ErrorStatus status = Acad::eInvalidInput;
        if (operation.empty()) {
            summary = "layers.batch Aktion " + std::to_string(i + 1) + " hat keine unterstuetzte Layer-Operation";
        } else {
            status = applyLayerMutation(database, actionParams, operation, summary, wasSkipped, debugLines);
        }

        if (i > 0) {
            actionResults << ',';
        }
        actionResults << "{\"index\":" << (i + 1)
            << ",\"operation\":\"" << jsonEscape(operation.empty() ? std::string("<unbekannt>") : operation) << "\""
            << ",\"ok\":" << (status == Acad::eOk ? "true" : "false")
            << ",\"skipped\":" << (wasSkipped ? "true" : "false")
            << ",\"summary\":\"" << jsonEscape(summary) << "\"";

        if (status == Acad::eOk) {
            ++completed;
            if (wasSkipped) {
                ++skipped;
            }
        } else {
            ++failed;
            actionResults << ",\"error\":\"" << jsonEscape(errorStatusText(status)) << "\"";
        }
        actionResults << '}';
        appendDebug(debugLines,
            "layers.batch action " + std::to_string(i + 1)
            + " operation=" + (operation.empty() ? std::string("<unbekannt>") : operation)
            + " status=" + errorStatusText(status)
            + " summary=" + summary);
    }
    actionResults << ']';

    appendDebug(debugLines, "Layer batch executed via native BricsCAD -LAYER command path");
    appendDebug(debugLines, "layers.batch action results=" + actionResults.str());
    if (failed > 0) {
        std::ostringstream response;
        response << errorResponse("layers.batch fehlgeschlagen: " + std::to_string(failed) + " Fehler");
        appendDebugResponse(response, debugLines);
        return response.str();
    }

    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.layers.batch.result.v1\""
        << ",\"summary\":\"layers.batch completed=" << completed << " skipped=" << skipped << "\""
        << ",\"actionsRequested\":" << actions.size()
        << ",\"completed\":" << completed
        << ",\"skipped\":" << skipped
        << ",\"failed\":" << failed
        << ",\"results\":" << actionResults.str()
        << ",\"warnings\":[]"
        << ",\"timeMs\":0"
        << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
        << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
        << '}';
    return okJsonResultResponse(json.str(), debugLines);
}

bool curveLength(AcDbEntity* entity, double& length)
{
    length = 0.0;
    AcDbCurve* curve = AcDbCurve::cast(entity);
    if (curve == nullptr) {
        return false;
    }

    double endParam = 0.0;
    if (curve->getEndParam(endParam) != Acad::eOk) {
        return false;
    }
    return curve->getDistAtParam(endParam, length) == Acad::eOk;
}

bool entityArea(AcDbEntity* entity, double& area)
{
    area = 0.0;
    AcDbPolyline* polyline = AcDbPolyline::cast(entity);
    if (polyline != nullptr && polyline->getArea(area) == Acad::eOk) {
        return true;
    }

    AcDbCircle* circle = AcDbCircle::cast(entity);
    if (circle != nullptr) {
        area = 3.14159265358979323846 * circle->radius() * circle->radius();
        return true;
    }
    return false;
}

std::string measureEntitiesInApplicationContext(const std::string& paramsJson, const std::string& operation)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    const std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail("measurement." + operation + " braucht selector, handles, handle oder layer");
    }
    const bool includeObjects = jsonBoolProperty(paramsJson, "includeObjects").value_or(true);
    const AcDbObjectIdArray ids = selectorObjectIds(selectorJson, database, debugLines);

    double total = 0.0;
    int measured = 0;
    int failed = 0;
    std::ostringstream objects;
    objects << '[';
    bool firstObject = true;
    BoundsData aggregateBounds;

    for (int i = 0; i < ids.length(); ++i) {
        const AcDbObjectId id = ids.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForRead);
        if (openStatus != Acad::eOk || entity == nullptr) {
            ++failed;
            appendDebug(debugLines, "measure open failed handle=" + objectHandleText(id) + ": " + errorStatusText(openStatus));
            continue;
        }

        BoundsData bounds;
        AcDbExtents extents;
        if (entity->getGeomExtents(extents) == Acad::eOk) {
            bounds.valid = true;
            bounds.minPoint = extents.minPoint();
            bounds.maxPoint = extents.maxPoint();
            if (!aggregateBounds.valid) {
                aggregateBounds = bounds;
            } else {
                aggregateBounds.minPoint.x = std::min(aggregateBounds.minPoint.x, bounds.minPoint.x);
                aggregateBounds.minPoint.y = std::min(aggregateBounds.minPoint.y, bounds.minPoint.y);
                aggregateBounds.minPoint.z = std::min(aggregateBounds.minPoint.z, bounds.minPoint.z);
                aggregateBounds.maxPoint.x = std::max(aggregateBounds.maxPoint.x, bounds.maxPoint.x);
                aggregateBounds.maxPoint.y = std::max(aggregateBounds.maxPoint.y, bounds.maxPoint.y);
                aggregateBounds.maxPoint.z = std::max(aggregateBounds.maxPoint.z, bounds.maxPoint.z);
            }
        }

        double value = 0.0;
        bool ok = operation == "bbox" && bounds.valid;
        if (operation == "length") {
            ok = curveLength(entity, value);
        } else if (operation == "area") {
            ok = entityArea(entity, value);
        }

        if (ok) {
            ++measured;
            total += value;
        } else {
            ++failed;
        }

        if (includeObjects) {
            if (!firstObject) {
                objects << ',';
            }
            firstObject = false;
            objects << "{\"handle\":\"" << jsonEscape(objectHandleText(id))
                << "\",\"type\":\"" << jsonEscape(entityTypeName(entity))
                << "\",\"layer\":\"" << jsonEscape(entityLayerName(entity))
                << "\",\"ok\":" << (ok ? "true" : "false")
                << ",\"success\":" << (ok ? "true" : "false")
                << ",\"bounds\":";
            appendBoundsJson(objects, bounds);
            objects << ",\"dimensions\":";
            appendDimensionsJson(objects, bounds);
            if (!ok) {
                objects << ",\"error\":\"" << jsonEscape(operation + "_unavailable") << "\"";
            }
            if (operation == "length") {
                objects << ",\"length\":" << value;
            } else if (operation == "area") {
                objects << ",\"area\":" << value;
            }
            objects << '}';
        }
        entity->close();
    }
    objects << ']';

    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.measurement." << jsonEscape(operation) << ".result.v1\""
        << ",\"summary\":\"measurement." << jsonEscape(operation) << " measured=" << measured << " failed=" << failed << "\""
        << ",\"count\":" << ids.length()
        << ",\"measured\":" << measured
        << ",\"failed\":" << failed
        << ",\"units\":\"mm\""
        << ",\"coordinateSystem\":\"WCS\"";
    if (operation == "length") {
        json << ",\"totalLength\":" << total;
    } else if (operation == "area") {
        json << ",\"totalArea\":" << total;
    }
    json << ",\"bounds\":";
    appendBoundsJson(json, aggregateBounds);
    json << ",\"dimensions\":";
    appendDimensionsJson(json, aggregateBounds);
    json << ",\"objects\":" << objects.str()
        << ",\"warnings\":[]"
        << ",\"timeMs\":0"
        << '}';
    return okJsonResultResponse(json.str(), debugLines);
}

std::string extrudeProfilesInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    std::string selectorJson = selectorJsonFromParams(paramsJson);
    if (selectorJson.empty()) {
        return fail("profile.extrude braucht selector, handles, handle oder layer");
    }

    const double heightMm = jsonDoubleProperty(paramsJson, "heightMm").value_or(
        jsonDoubleProperty(paramsJson, "height").value_or(0.0));
    if (!std::isfinite(heightMm) || heightMm <= kRectangleTolerance) {
        return fail("profile.extrude braucht heightMm > 0");
    }

    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    AcDbDatabase* database = nullptr;
    bool savedBefore = false;
    std::unique_ptr<AcAxDocLock> docLock;
    std::string errorMessage;
    if (!prepareMutation(database, saveBefore, savedBefore, docLock, debugLines, errorMessage)) {
        return fail(errorMessage);
    }

    const AcDbObjectIdArray candidateIds = selectorObjectIds(selectorJson, database, debugLines);
    AcDbObjectIdArray profileIds;
    for (int i = 0; i < candidateIds.length(); ++i) {
        const AcDbObjectId id = candidateIds.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForRead);
        if (openStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "profile.extrude open failed handle=" + objectHandleText(id) + ": " + errorStatusText(openStatus));
            continue;
        }
        const AcDbCurve* curve = AcDbCurve::cast(entity);
        bool accept = curve != nullptr;
        const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
        if (polyline != nullptr && polyline->isClosed() == Adesk::kFalse) {
            accept = false;
        }
        if (accept) {
            profileIds.append(id);
            appendDebug(debugLines, "profile.extrude candidate handle=" + objectHandleText(id));
        } else {
            appendDebug(debugLines, "profile.extrude skip handle=" + objectHandleText(id)
                + " type=" + entityTypeName(entity));
        }
        entity->close();
    }

    if (profileIds.isEmpty()) {
        return fail("Selector enthaelt keine extrudierbaren Profile");
    }

    const AcDbObjectIdArray solidIdsBefore = collectCurrentSpaceSolidIds(database, debugLines);
    if (docLock != nullptr) {
        docLock.reset();
        appendDebug(debugLines, "Document lock released before native profile EXTRUDE");
    }

    int extruded = 0;
    int errors = 0;
    ads_name selectionSet{};
    if (!createSelectionSet(profileIds, selectionSet, debugLines)) {
        errors = profileIds.length();
    } else {
        const int commandStatus = runNativeExtrudeCommand(selectionSet, heightMm, debugLines);
        if (commandStatus == RTNORM) {
            extruded = profileIds.length();
        } else {
            errors = profileIds.length();
        }
        const int freeStatus = acedSSFree(selectionSet);
        appendDebug(debugLines, "acedSSFree profile.extrude status=" + std::to_string(freeStatus));
    }

    AcDbObjectIdArray createdSolidIds;
    if (extruded > 0) {
        const AcDbObjectIdArray solidIdsAfter = collectCurrentSpaceSolidIds(database, debugLines);
        createdSolidIds = newObjectIds(solidIdsBefore, solidIdsAfter);
        rememberLastExtrudedSolids(createdSolidIds, jsonStringProperty(paramsJson, "layer").value_or("selector"), debugLines);
        rememberLastResult(createdSolidIds, "profile.extrude", debugLines);
    }

    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.profile.extrude.result.v1\""
        << ",\"summary\":\"profile.extrude extruded=" << extruded << " found=" << profileIds.length() << " errors=" << errors << "\""
        << ",\"units\":\"mm\""
        << ",\"heightMm\":" << heightMm
        << ",\"found\":" << profileIds.length()
        << ",\"extruded\":" << extruded
        << ",\"errors\":" << errors
        << ",\"affectedHandles\":" << handlesJsonArray(createdSolidIds)
        << ",\"sourceHandles\":" << handlesJsonArray(profileIds)
        << ",\"failedHandles\":[]"
        << ",\"warnings\":[]"
        << ",\"timeMs\":0"
        << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
        << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
        << '}';
    return okJsonResultResponse(json.str(), debugLines);
}

std::string createGeometryInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::string geometryType = toUpperAscii(jsonStringProperty(paramsJson, "geometry").value_or(
        jsonStringProperty(paramsJson, "type").value_or("")));
    const std::string layerName = jsonStringProperty(paramsJson, "layer").value_or("");
    const bool saveBefore = jsonBoolProperty(paramsJson, "saveBefore").value_or(true);
    appendDebug(debugLines, "GEOMETRYCREATE request geometry='" + geometryType + "' layer='" + layerName + "'");

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    }

    AcDbEntity* entity = nullptr;
    std::string normalizedType;
    bool extrudeCreatedProfile = false;
    double createdProfileHeight = 0.0;
    if (geometryType == "POINT") {
        const JsonPoint3d position = jsonPointProperty(paramsJson, "position", jsonPointProperty(paramsJson, "point"));
        entity = new AcDbPoint(AcGePoint3d(position.x, position.y, position.z));
        normalizedType = "point";
    } else if (geometryType == "RECTANGLE") {
        const JsonPoint3d origin = jsonPointProperty(paramsJson, "origin");
        const double width = jsonDoubleProperty(paramsJson, "width").value_or(
            jsonDoubleProperty(paramsJson, "widthMm").value_or(jsonDoubleProperty(paramsJson, "x").value_or(0.0)));
        const double depth = jsonDoubleProperty(paramsJson, "depth").value_or(
            jsonDoubleProperty(paramsJson, "depthMm").value_or(
                jsonDoubleProperty(paramsJson, "length").value_or(
                    jsonDoubleProperty(paramsJson, "lengthMm").value_or(jsonDoubleProperty(paramsJson, "y").value_or(0.0)))));
        if (!std::isfinite(width) || !std::isfinite(depth) || std::abs(width) <= kRectangleTolerance || std::abs(depth) <= kRectangleTolerance) {
            return fail("geometry.create RECTANGLE braucht width und depth/length > 0");
        }

        auto* polyline = new AcDbPolyline(4);
        polyline->addVertexAt(0, AcGePoint2d(origin.x, origin.y));
        polyline->addVertexAt(1, AcGePoint2d(origin.x + width, origin.y));
        polyline->addVertexAt(2, AcGePoint2d(origin.x + width, origin.y + depth));
        polyline->addVertexAt(3, AcGePoint2d(origin.x, origin.y + depth));
        polyline->setClosed(Adesk::kTrue);
        if (std::abs(origin.z) > kRectangleTolerance) {
            polyline->setElevation(origin.z);
        }
        entity = polyline;
        normalizedType = "rectangle";
    } else if (geometryType == "LINE") {
        const JsonPoint3d start = jsonPointProperty(paramsJson, "start");
        const JsonPoint3d end = jsonPointProperty(paramsJson, "end");
        if (std::abs(start.x - end.x) <= kRectangleTolerance
            && std::abs(start.y - end.y) <= kRectangleTolerance
            && std::abs(start.z - end.z) <= kRectangleTolerance) {
            return fail("geometry.create LINE braucht unterschiedliche start/end Punkte");
        }
        entity = new AcDbLine(AcGePoint3d(start.x, start.y, start.z), AcGePoint3d(end.x, end.y, end.z));
        normalizedType = "line";
    } else if (geometryType == "CIRCLE") {
        const JsonPoint3d center = jsonPointProperty(paramsJson, "center", jsonPointProperty(paramsJson, "origin"));
        const double radius = jsonDoubleProperty(paramsJson, "radius").value_or(jsonDoubleProperty(paramsJson, "radiusMm").value_or(0.0));
        if (!std::isfinite(radius) || radius <= kRectangleTolerance) {
            return fail("geometry.create CIRCLE braucht radius > 0");
        }
        entity = new AcDbCircle(AcGePoint3d(center.x, center.y, center.z), AcGeVector3d::kZAxis, radius);
        normalizedType = "circle";
    } else if (geometryType == "ARC") {
        const JsonPoint3d center = jsonPointProperty(paramsJson, "center", jsonPointProperty(paramsJson, "origin"));
        const double radius = jsonDoubleProperty(paramsJson, "radius").value_or(jsonDoubleProperty(paramsJson, "radiusMm").value_or(0.0));
        const double startAngle = jsonAngleRadians(paramsJson, "startAngle", "startAngleDeg", 0.0);
        const double endAngle = jsonAngleRadians(paramsJson, "endAngle", "endAngleDeg", 3.14159265358979323846 / 2.0);
        if (!std::isfinite(radius) || radius <= kRectangleTolerance) {
            return fail("geometry.create ARC braucht radius > 0");
        }
        if (std::abs(startAngle - endAngle) <= kRectangleTolerance) {
            return fail("geometry.create ARC braucht unterschiedliche start/end Winkel");
        }
        entity = new AcDbArc(AcGePoint3d(center.x, center.y, center.z), AcGeVector3d::kZAxis, radius, startAngle, endAngle);
        normalizedType = "arc";
    } else if (geometryType == "POLYLINE") {
        const std::vector<JsonPoint3d> points = jsonPointArrayProperty(paramsJson, "points");
        if (points.size() < 2) {
            return fail("geometry.create POLYLINE braucht mindestens zwei points");
        }
        auto* polyline = new AcDbPolyline(static_cast<unsigned int>(points.size()));
        for (std::size_t i = 0; i < points.size(); ++i) {
            polyline->addVertexAt(static_cast<unsigned int>(i), AcGePoint2d(points[i].x, points[i].y));
        }
        polyline->setClosed(jsonBoolProperty(paramsJson, "closed").value_or(false) ? Adesk::kTrue : Adesk::kFalse);
        if (!points.empty() && std::abs(points.front().z) > kRectangleTolerance) {
            polyline->setElevation(points.front().z);
        }
        entity = polyline;
        normalizedType = "polyline";
    } else if (geometryType == "BOX" || geometryType == "CUBOID" || geometryType == "QUADER") {
        const JsonPoint3d origin = jsonPointProperty(paramsJson, "origin");
        const double width = jsonDoubleProperty(paramsJson, "width").value_or(
            jsonDoubleProperty(paramsJson, "widthMm").value_or(jsonDoubleProperty(paramsJson, "x").value_or(0.0)));
        const double depth = jsonDoubleProperty(paramsJson, "depth").value_or(
            jsonDoubleProperty(paramsJson, "depthMm").value_or(
                jsonDoubleProperty(paramsJson, "length").value_or(
                    jsonDoubleProperty(paramsJson, "lengthMm").value_or(jsonDoubleProperty(paramsJson, "y").value_or(0.0)))));
        const double height = jsonDoubleProperty(paramsJson, "height").value_or(
            jsonDoubleProperty(paramsJson, "heightMm").value_or(jsonDoubleProperty(paramsJson, "z").value_or(0.0)));
        if (!std::isfinite(width) || !std::isfinite(depth) || !std::isfinite(height)
            || width <= kRectangleTolerance || depth <= kRectangleTolerance || height <= kRectangleTolerance) {
            return fail("geometry.create BOX braucht width/x, depth/y und height/z > 0");
        }

        auto* polyline = new AcDbPolyline(4);
        polyline->addVertexAt(0, AcGePoint2d(origin.x, origin.y));
        polyline->addVertexAt(1, AcGePoint2d(origin.x + width, origin.y));
        polyline->addVertexAt(2, AcGePoint2d(origin.x + width, origin.y + depth));
        polyline->addVertexAt(3, AcGePoint2d(origin.x, origin.y + depth));
        polyline->setClosed(Adesk::kTrue);
        if (std::abs(origin.z) > kRectangleTolerance) {
            polyline->setElevation(origin.z);
        }
        entity = polyline;
        normalizedType = "box";
        extrudeCreatedProfile = true;
        createdProfileHeight = height;
        appendDebug(debugLines, "Box will be created via rectangle profile + native EXTRUDE");
    } else {
        return fail("geometry.create unterstuetzt aktuell geometry=point, line, polyline, rectangle, circle, arc oder box");
    }

    entity->setDatabaseDefaults(database);
    appendDebug(debugLines, "setDatabaseDefaults done");

    if (!layerName.empty()) {
        const std::basic_string<ACHAR> nativeLayer = utf8ToAchar(layerName);
        const Acad::ErrorStatus layerStatus = entity->setLayer(nativeLayer.c_str());
        appendDebug(debugLines, "setLayer '" + layerName + "': " + errorStatusText(layerStatus));
        if (layerStatus != Acad::eOk) {
            delete entity;
            return fail("Layer konnte nicht gesetzt werden: " + errorStatusText(layerStatus));
        }
    }

    AcDbBlockTableRecord* space = nullptr;
    const Acad::ErrorStatus openStatus = acdbOpenObject(space, database->currentSpaceId(), AcDb::kForWrite);
    appendDebug(debugLines, "Open current space for write: " + errorStatusText(openStatus));
    if (openStatus != Acad::eOk || space == nullptr) {
        delete entity;
        return fail("Aktueller Modellbereich konnte nicht geoeffnet werden: " + errorStatusText(openStatus));
    }

    AcDbObjectId entityId;
    const Acad::ErrorStatus appendStatus = space->appendAcDbEntity(entityId, entity);
    space->close();
    if (appendStatus != Acad::eOk) {
        delete entity;
        return fail("Geometrie konnte nicht angelegt werden: " + errorStatusText(appendStatus));
    }
    entity->close();
    appendDebug(debugLines, "Created " + normalizedType + " handle=" + objectHandleText(entityId));
    AcDbObjectId resultEntityId = entityId;
    int createdCount = 1;

    if (extrudeCreatedProfile) {
        if (docLock != nullptr) {
            docLock.reset();
            appendDebug(debugLines, "Document lock released before native EXTRUDE for box");
        }

        const AcDbObjectIdArray solidIdsBefore = collectCurrentSpaceSolidIds(database, debugLines);
        AcDbObjectIdArray profileIds;
        profileIds.append(entityId);
        ads_name selectionSet{};
        if (!createSelectionSet(profileIds, selectionSet, debugLines)) {
            return fail("Box-Profil konnte nicht fuer EXTRUDE ausgewaehlt werden");
        }

        const int commandStatus = runNativeExtrudeCommand(selectionSet, createdProfileHeight, debugLines);
        const int freeStatus = acedSSFree(selectionSet);
        appendDebug(debugLines, "acedSSFree box profile status=" + std::to_string(freeStatus));
        if (commandStatus != RTNORM) {
            return fail("Box-Profil konnte nicht extrudiert werden");
        }

        const AcDbObjectIdArray solidIdsAfter = collectCurrentSpaceSolidIds(database, debugLines);
        const AcDbObjectIdArray createdSolidIds = newObjectIds(solidIdsBefore, solidIdsAfter);
        appendDebug(debugLines, "Box created solid scan before=" + std::to_string(solidIdsBefore.length())
            + " after=" + std::to_string(solidIdsAfter.length())
            + " created=" + std::to_string(createdSolidIds.length()));
        if (createdSolidIds.isEmpty()) {
            return fail("Box-Extrusion abgeschlossen, aber kein neues 3D-Solid erkannt");
        }
        std::unique_ptr<AcAxDocLock> postExtrudeDocLock;
        if (applicationContext && !layerName.empty()) {
            postExtrudeDocLock = std::make_unique<AcAxDocLock>(database);
            if (postExtrudeDocLock->lockStatus() != Acad::eOk) {
                return fail("Aktive Zeichnung konnte nach EXTRUDE nicht gesperrt werden: " + errorStatusText(postExtrudeDocLock->lockStatus()));
            }
            appendDebug(debugLines, "Post-EXTRUDE document lock: " + errorStatusText(postExtrudeDocLock->lockStatus()));
        }
        if (!setEntitiesLayerByName(createdSolidIds, layerName, debugLines)) {
            return fail("Layer konnte fuer erzeugte 3D-Solids nicht gesetzt werden");
        }
        resultEntityId = createdSolidIds.at(0);
        createdCount = createdSolidIds.length();
        rememberLastExtrudedSolids(createdSolidIds, layerName, debugLines);
        rememberLastResult(createdSolidIds, "geometry.create.box", debugLines);
    } else {
        rememberLastResult(resultEntityId, "geometry.create." + normalizedType, debugLines);
    }

    std::ostringstream response;
    response << "OK"
        << "\tGEOMETRY\t" << percentEncode(normalizedType)
        << "\tCREATED\t" << createdCount
        << "\tHANDLE\t" << objectHandleText(resultEntityId)
        << "\tLAYER\t" << percentEncode(layerName)
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

struct PipeSegment {
    AcGePoint3d start;
    AcGePoint3d end;
};

std::vector<PipeSegment> pipeSegmentsFromParams(
    const std::string& paramsJson,
    AcDbDatabase* database,
    std::vector<std::string>& debugLines)
{
    std::vector<PipeSegment> segments;
    const std::string selector = jsonObjectProperty(paramsJson, "selector").value_or("{}");
    const AcDbObjectIdArray ids = selectorObjectIds(selector, database, debugLines);
    for (int idIndex = 0; idIndex < ids.length(); ++idIndex) {
        AcDbEntity* entity = nullptr;
        if (acdbOpenObject(entity, ids.at(idIndex), AcDb::kForRead) != Acad::eOk || entity == nullptr) {
            continue;
        }
        AcDbPolyline* polyline = AcDbPolyline::cast(entity);
        if (polyline == nullptr || polyline->isClosed() || polyline->numVerts() < 2) {
            entity->close();
            continue;
        }
        for (unsigned int i = 1; i < polyline->numVerts(); ++i) {
            AcGePoint3d start;
            AcGePoint3d end;
            if (polyline->getPointAt(i - 1, start) == Acad::eOk
                && polyline->getPointAt(i, end) == Acad::eOk
                && start.distanceTo(end) > kRectangleTolerance) {
                segments.push_back(PipeSegment{start, end});
            }
        }
        entity->close();
    }
    appendDebug(debugLines, "Pipe segments read=" + std::to_string(segments.size()));
    return segments;
}

bool pipePointsEqual(const AcGePoint3d& a, const AcGePoint3d& b, double tolerance = 1.0e-3)
{
    return a.distanceTo(b) <= tolerance;
}

std::string validatePipeNetworkInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return errorResponse("Keine aktive BricsCAD Zeichnung gefunden");
    }
    const std::vector<PipeSegment> segments = pipeSegmentsFromParams(paramsJson, database, debugLines);
    if (segments.empty()) {
        return errorResponse("pipes.validateNetwork findet keine offenen Polyliniensegmente");
    }

    std::vector<AcGePoint3d> nodes;
    std::vector<int> degrees;
    auto nodeIndex = [&](const AcGePoint3d& point) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (pipePointsEqual(nodes[i], point)) {
                return static_cast<int>(i);
            }
        }
        nodes.push_back(point);
        degrees.push_back(0);
        return static_cast<int>(nodes.size() - 1);
    };
    std::vector<std::pair<int, int>> edges;
    for (const PipeSegment& segment : segments) {
        const int a = nodeIndex(segment.start);
        const int b = nodeIndex(segment.end);
        ++degrees[a];
        ++degrees[b];
        edges.emplace_back(a, b);
    }
    std::vector<bool> reached(nodes.size(), false);
    std::vector<int> pending{0};
    reached[0] = true;
    while (!pending.empty()) {
        const int current = pending.back();
        pending.pop_back();
        for (const auto& edge : edges) {
            const int next = edge.first == current ? edge.second : (edge.second == current ? edge.first : -1);
            if (next >= 0 && !reached[next]) {
                reached[next] = true;
                pending.push_back(next);
            }
        }
    }
    const int components = std::count(reached.cbegin(), reached.cend(), false) + 1;
    const int openEnds = static_cast<int>(std::count(degrees.cbegin(), degrees.cend(), 1));
    const int teeNodes = static_cast<int>(std::count_if(degrees.cbegin(), degrees.cend(), [](int degree) { return degree >= 3; }));
    const bool connected = components == 1;
    std::ostringstream result;
    result << "RESULT\t{\"schema\":\"barebone.bricscad.pipes.validate-network.result.v1\""
        << ",\"valid\":" << (connected ? "true" : "false")
        << ",\"connected\":" << (connected ? "true" : "false")
        << ",\"segments\":" << segments.size()
        << ",\"nodes\":" << nodes.size()
        << ",\"openEnds\":" << openEnds
        << ",\"teeNodes\":" << teeNodes
        << ",\"components\":" << components << "}\n";
    return result.str();
}

std::string createPipeNetworkSolidsInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return errorResponse("Keine aktive BricsCAD Zeichnung gefunden");
    }
    const double diameter = jsonDoubleProperty(paramsJson, "diameterMm").value_or(0.0);
    const std::string layerName = jsonStringProperty(paramsJson, "targetLayer").value_or("");
    if (diameter <= kRectangleTolerance || layerName.empty()) {
        return errorResponse("pipes.createNetworkSolids braucht diameterMm > 0 und targetLayer");
    }
    const std::vector<PipeSegment> segments = pipeSegmentsFromParams(paramsJson, database, debugLines);
    if (segments.empty()) {
        return errorResponse("Keine validierten Polyliniensegmente gefunden");
    }
    AcDbBlockTableRecord* space = nullptr;
    if (acdbOpenObject(space, database->currentSpaceId(), AcDb::kForWrite) != Acad::eOk || space == nullptr) {
        return errorResponse("Modellbereich konnte nicht geoeffnet werden");
    }
    AcDbObjectIdArray createdIds;
    for (const PipeSegment& segment : segments) {
        const AcGeVector3d direction = segment.end - segment.start;
        const double length = direction.length();
        auto* solid = new AcDb3dSolid();
        solid->setDatabaseDefaults(database);
        Acad::ErrorStatus status = solid->createFrustum(length, diameter / 2.0, diameter / 2.0, diameter / 2.0);
        if (status == Acad::eOk) {
            const AcGeVector3d unit = direction.normal();
            const double dot = std::clamp(AcGeVector3d::kZAxis.dotProduct(unit), -1.0, 1.0);
            const double angle = std::acos(dot);
            AcGeVector3d axis = AcGeVector3d::kZAxis.crossProduct(unit);
            if (axis.length() > kRectangleTolerance) {
                solid->transformBy(AcGeMatrix3d::rotation(angle, axis.normal(), AcGePoint3d::kOrigin));
            } else if (dot < 0.0) {
                solid->transformBy(AcGeMatrix3d::rotation(3.14159265358979323846, AcGeVector3d::kXAxis, AcGePoint3d::kOrigin));
            }
            solid->transformBy(AcGeMatrix3d::translation(segment.start - AcGePoint3d::kOrigin));
            status = solid->setLayer(utf8ToAchar(layerName).c_str());
        }
        AcDbObjectId id;
        if (status == Acad::eOk) {
            status = space->appendAcDbEntity(id, solid);
        }
        if (status == Acad::eOk) {
            solid->close();
            createdIds.append(id);
        } else {
            delete solid;
        }
    }
    std::vector<AcGePoint3d> fittingPoints;
    std::vector<int> fittingDegrees;
    auto addFittingEndpoint = [&](const AcGePoint3d& point) {
        for (std::size_t i = 0; i < fittingPoints.size(); ++i) {
            if (pipePointsEqual(fittingPoints[i], point)) {
                ++fittingDegrees[i];
                return;
            }
        }
        fittingPoints.push_back(point);
        fittingDegrees.push_back(1);
    };
    for (const PipeSegment& segment : segments) {
        addFittingEndpoint(segment.start);
        addFittingEndpoint(segment.end);
    }
    int fittings = 0;
    for (std::size_t i = 0; i < fittingPoints.size(); ++i) {
        if (fittingDegrees[i] < 2) continue;
        auto* fitting = new AcDb3dSolid();
        fitting->setDatabaseDefaults(database);
        Acad::ErrorStatus status = fitting->createSphere(diameter / 2.0);
        if (status == Acad::eOk) status = fitting->transformBy(AcGeMatrix3d::translation(fittingPoints[i] - AcGePoint3d::kOrigin));
        if (status == Acad::eOk) status = fitting->setLayer(utf8ToAchar(layerName).c_str());
        AcDbObjectId id;
        if (status == Acad::eOk) status = space->appendAcDbEntity(id, fitting);
        if (status == Acad::eOk) {
            fitting->close();
            createdIds.append(id);
            ++fittings;
        } else {
            delete fitting;
        }
    }
    space->close();
    rememberLastResult(createdIds, "pipes.createNetworkSolids", debugLines);
    std::ostringstream result;
    result << "RESULT\t{\"schema\":\"barebone.bricscad.pipes.create-network-solids.result.v1\",\"created\":"
        << createdIds.length() << ",\"segments\":" << segments.size() << ",\"fittings\":" << fittings
        << ",\"diameterMm\":" << diameter << ",\"handles\":[";
    for (int i = 0; i < createdIds.length(); ++i) {
        if (i) result << ',';
        result << '\"' << jsonEscape(objectHandleText(createdIds.at(i))) << '\"';
    }
    result << "]}\n";
    return result.str();
}

std::string createRoomDimensionsInApplicationContext(const std::string& paramsJson)
{
    std::vector<std::string> debugLines;
    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) return errorResponse("Keine aktive BricsCAD Zeichnung gefunden");
    const std::string selector = jsonObjectProperty(paramsJson, "selector").value_or("{}");
    const AcDbObjectIdArray roomIds = selectorObjectIds(selector, database, debugLines);
    const std::string dimensionLayer = jsonStringProperty(paramsJson, "dimensionLayer").value_or("");
    const std::string labelLayer = jsonStringProperty(paramsJson, "labelLayer").value_or("");
    const double roomHeight = jsonDoubleProperty(paramsJson, "roomHeightMm").value_or(0.0);
    const double textHeight = jsonDoubleProperty(paramsJson, "textHeightMm").value_or(180.0);
    const double offset = jsonDoubleProperty(paramsJson, "offsetMm").value_or(300.0);
    const std::vector<std::string> roomNames = jsonArrayProperty(paramsJson, "roomNames").has_value()
        ? jsonStringArrayValues(*jsonArrayProperty(paramsJson, "roomNames")) : std::vector<std::string>{};
    if (roomIds.isEmpty() || dimensionLayer.empty() || labelLayer.empty() || roomHeight <= 0.0) {
        return errorResponse("annotations.createRoomDimensions braucht Raumhandles, roomHeightMm und beide Layer");
    }
    AcDbBlockTableRecord* space = nullptr;
    if (acdbOpenObject(space, database->currentSpaceId(), AcDb::kForWrite) != Acad::eOk || space == nullptr) {
        return errorResponse("Modellbereich konnte nicht geoeffnet werden");
    }
    AcDbObjectIdArray createdIds;
    int annotated = 0;
    for (int i = 0; i < roomIds.length(); ++i) {
        AcDbEntity* room = nullptr;
        if (acdbOpenObject(room, roomIds.at(i), AcDb::kForRead) != Acad::eOk || room == nullptr) continue;
        AcDbPolyline* rectangle = AcDbPolyline::cast(room);
        AcDbExtents extents;
        if (rectangle == nullptr || !isRectanglePolyline(rectangle) || room->getGeomExtents(extents) != Acad::eOk) {
            room->close();
            continue;
        }
        room->close();
        const AcGePoint3d min = extents.minPoint();
        const AcGePoint3d max = extents.maxPoint();
        const double length = max.x - min.x;
        const double width = max.y - min.y;
        const std::string name = i < static_cast<int>(roomNames.size()) ? roomNames[i] : "Raum " + std::to_string(i + 1);
        std::vector<AcDbEntity*> entities;
        entities.push_back(new AcDbAlignedDimension(
            AcGePoint3d(min.x, min.y, min.z), AcGePoint3d(max.x, min.y, min.z), AcGePoint3d((min.x + max.x) / 2.0, min.y - offset, min.z)));
        entities.push_back(new AcDbAlignedDimension(
            AcGePoint3d(min.x, min.y, min.z), AcGePoint3d(min.x, max.y, min.z), AcGePoint3d(min.x - offset, (min.y + max.y) / 2.0, min.z)));
        auto* label = new AcDbMText();
        label->setLocation(AcGePoint3d((min.x + max.x) / 2.0, (min.y + max.y) / 2.0, min.z));
        label->setTextHeight(textHeight);
        const std::string labelText = name + "\\PL=" + std::to_string(static_cast<int>(std::round(length)))
            + " mm  B=" + std::to_string(static_cast<int>(std::round(width)))
            + " mm\\PH=" + std::to_string(static_cast<int>(std::round(roomHeight)))
            + " mm  A=" + std::to_string(length * width / 1000000.0) + " m2";
        const std::basic_string<ACHAR> nativeText = utf8ToAchar(labelText);
        label->setContents(nativeText.c_str());
        entities.push_back(label);
        for (std::size_t entityIndex = 0; entityIndex < entities.size(); ++entityIndex) {
            AcDbEntity* entity = entities[entityIndex];
            entity->setDatabaseDefaults(database);
            entity->setLayer(utf8ToAchar(entityIndex < 2 ? dimensionLayer : labelLayer).c_str());
            AcDbObjectId id;
            if (space->appendAcDbEntity(id, entity) == Acad::eOk) {
                entity->close();
                createdIds.append(id);
            } else {
                delete entity;
            }
        }
        ++annotated;
    }
    space->close();
    rememberLastResult(createdIds, "annotations.createRoomDimensions", debugLines);
    std::ostringstream result;
    result << "RESULT\t{\"schema\":\"barebone.bricscad.annotations.room-dimensions.result.v1\",\"rooms\":"
        << annotated << ",\"created\":" << createdIds.length() << "}\n";
    return result.str();
}

std::string postNativeCommandLineInApplicationContext(const std::string& commandLineUtf8, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    std::string commandLine = trim(commandLineUtf8);
    appendDebug(debugLines, "POSTCOMMAND request commandLine='" + commandLine + "' saveBefore=" + (saveBefore ? "true" : "false"));
    if (commandLine.empty()) {
        return fail("commandLine ist leer");
    }
    if (commandLine.size() > 2000) {
        return fail("commandLine ist zu lang");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    if (acDocManager == nullptr) {
        return fail("BricsCAD Document Manager ist nicht verfuegbar");
    }
    AcApDocument* document = acDocManager->curDocument();
    if (document == nullptr) {
        return fail("Kein aktives BricsCAD Dokument gefunden");
    }

    if (commandLine.back() != '\n' && commandLine.back() != '\r') {
        commandLine.push_back('\n');
    }

    const std::basic_string<ACHAR> nativeCommandLine = utf8ToAchar(commandLine);
    const Acad::ErrorStatus status = acDocManager->sendStringToExecute(document, nativeCommandLine.c_str(), true, false, true);
    appendDebug(debugLines, "sendStringToExecute: " + errorStatusText(status));
    if (status != Acad::eOk) {
        return fail("BricsCAD Befehl konnte nicht gepostet werden: " + errorStatusText(status));
    }

    std::ostringstream response;
    response << "OK"
        << "\tCOMMAND\tPOSTCOMMAND"
        << "\tPOSTED\t1"
        << "\tCOMMAND_LINE\t" << percentEncode(commandLine)
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

int runNativeExtrudeCommand(const ads_name selectionSet, double heightMm, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.EXTRUDE")) {
        return RTERROR;
    }
    {
        std::ostringstream line;
        line << "Calling acedCommand _.EXTRUDE heightMm=" << heightMm;
        appendDebug(debugLines, line.str());
    }

    const int commandStatus = acedCommand(
        RTSTR, _T("_.EXTRUDE"),
        RTPICKS, selectionSet,
        RTSTR, _T(""),
        RTREAL, heightMm,
        RTNONE);
    appendDebug(debugLines, "acedCommand _.EXTRUDE status=" + std::to_string(commandStatus));
    acedUpdateDisplay();
    return commandStatus;
}

int runNativeMoveCommand(const ads_name selectionSet, const JsonPoint3d& vector, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.MOVE")) {
        return RTERROR;
    }
    {
        std::ostringstream line;
        line << "Calling acedCommand _.MOVE vector=(" << vector.x << "," << vector.y << "," << vector.z << ")";
        appendDebug(debugLines, line.str());
    }

    ads_point basePoint{};
    ads_point targetPoint{};
    basePoint[0] = 0.0;
    basePoint[1] = 0.0;
    basePoint[2] = 0.0;
    targetPoint[0] = vector.x;
    targetPoint[1] = vector.y;
    targetPoint[2] = vector.z;

    const int commandStatus = acedCommand(
        RTSTR, _T("_.MOVE"),
        RTPICKS, selectionSet,
        RTSTR, _T(""),
        RT3DPOINT, basePoint,
        RT3DPOINT, targetPoint,
        RTNONE);
    appendDebug(debugLines, "acedCommand _.MOVE status=" + std::to_string(commandStatus));
    return commandStatus;
}

int runNativeBimClassifyCommand(const ads_name selectionSet, const std::string& bimClass, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.BIMCLASSIFY")) {
        return RTERROR;
    }
    const std::string option = bimClassCommandOption(bimClass);
    if (option.empty()) {
        appendDebug(debugLines, "BIMCLASSIFY unsupported class=" + bimClass);
        return RTERROR;
    }

    appendDebug(debugLines, "Calling acedCommand _.BIMCLASSIFY class=" + bimClass + " option=" + option);
    const std::basic_string<ACHAR> optionText = utf8ToAchar(option);
    const int commandStatus = acedCommand(
        RTSTR, _T("_.BIMCLASSIFY"),
        RTSTR, optionText.c_str(),
        RTPICKS, selectionSet,
        RTSTR, _T(""),
        RTNONE);
    appendDebug(debugLines, "acedCommand _.BIMCLASSIFY status=" + std::to_string(commandStatus));
    acedUpdateDisplay();
    return commandStatus;
}

int runNativeUndoCommand(int steps, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.UNDO")) {
        return RTERROR;
    }
    {
        std::ostringstream line;
        line << "Calling acedCommand _.UNDO steps=" << steps;
        appendDebug(debugLines, line.str());
    }
    const std::string stepsText = std::to_string(steps);
    const std::basic_string<ACHAR> stepArg = utf8ToAchar(stepsText);
    const int commandStatus = acedCommand(
        RTSTR, _T("_.UNDO"),
        RTSTR, stepArg.c_str(),
        RTNONE);
    appendDebug(debugLines, "acedCommand _.UNDO status=" + std::to_string(commandStatus));
    acedUpdateDisplay();
    return commandStatus;
}

int runNativeRedoCommand(int steps, std::vector<std::string>& debugLines)
{
    if (!ensureNativeCommandContext(debugLines, "_.REDO")) {
        return RTERROR;
    }
    int finalStatus = RTNORM;
    for (int i = 0; i < steps; ++i) {
        appendDebug(debugLines, "Calling acedCommand _.REDO step=" + std::to_string(i + 1));
        const int commandStatus = acedCommand(
            RTSTR, _T("_.REDO"),
            RTNONE);
        appendDebug(debugLines, "acedCommand _.REDO status=" + std::to_string(commandStatus));
        if (commandStatus != RTNORM) {
            finalStatus = commandStatus;
            break;
        }
    }
    acedUpdateDisplay();
    return finalStatus;
}

std::string undoLastActionInApplicationContext(int steps, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    if (steps < 1) {
        return fail("steps muss mindestens 1 sein");
    }

    appendDebug(debugLines, "UNDO request requested=" + std::to_string(steps)
        + " saveBefore=" + (saveBefore ? "true" : "false"));

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const int commandStatus = runNativeUndoCommand(steps, debugLines);
    const bool succeeded = commandStatus == RTNORM;
    const int undone = succeeded ? steps : 0;
    const int errors = succeeded ? 0 : 1;

    std::ostringstream response;
    response << "OK"
        << "\tREQUESTED\t" << steps
        << "\tUNDONE\t" << undone
        << "\tERRORS\t" << errors
        << "\tSUCCEEDED\t" << (succeeded ? 1 : 0)
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string redoLastActionInApplicationContext(int steps, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    if (steps < 1) {
        return fail("steps muss mindestens 1 sein");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const int commandStatus = runNativeRedoCommand(steps, debugLines);
    const bool succeeded = commandStatus == RTNORM;
    const int redone = succeeded ? steps : 0;
    const int errors = succeeded ? 0 : 1;

    AcDbObjectIdArray affected;
    AcDbObjectIdArray failed;
    std::ostringstream json;
    json << "{\"schema\":\"barebone.bricscad.undo.redo.result.v1\""
        << ",\"summary\":\"undo.redo redone=" << redone << " requested=" << steps << "\""
        << ",\"requested\":" << steps
        << ",\"redone\":" << redone
        << ",\"errors\":" << errors
        << ",\"succeeded\":" << (succeeded ? "true" : "false")
        << ",\"affectedHandles\":[]"
        << ",\"failedHandles\":[]"
        << ",\"warnings\":[]"
        << ",\"timeMs\":0"
        << ",\"saveBefore\":" << (saveBefore ? "true" : "false")
        << ",\"savedBefore\":" << (savedBefore ? "true" : "false")
        << '}';
    return okJsonResultResponse(json.str(), debugLines);
}

std::string classifyLastExtrudedSolidsInApplicationContext(const std::string& bimClassUtf8, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::string bimClass = normalizeBimClass(bimClassUtf8);
    appendDebug(debugLines, "BIMCLASSIFY request target=lastExtruded class='" + bimClassUtf8
        + "' normalized='" + bimClass + "' saveBefore=" + (saveBefore ? "true" : "false"));
    if (bimClass.empty()) {
        return fail("BIM-Klasse wird noch nicht unterstuetzt. Aktuell erlaubt: BIMWall");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    appendDebug(debugLines, std::string("Document manager context: ") + (applicationContext ? "application" : "command"));

    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    } else {
        appendDebug(debugLines, "Command context active; explicit document lock skipped");
    }

    const AcDbObjectIdArray rememberedIds = lastExtrudedSolidIds();
    AcDbObjectIdArray validSolidIds;
    for (int i = 0; i < rememberedIds.length(); ++i) {
        const AcDbObjectId id = rememberedIds.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForRead);
        if (openStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "Skip remembered solid handle=" + objectHandleText(id) + " open failed: " + errorStatusText(openStatus));
            continue;
        }
        if (AcDb3dSolid::cast(entity) != nullptr) {
            validSolidIds.append(id);
            appendDebug(debugLines, "BIMCLASSIFY candidate solid handle=" + objectHandleText(id));
        } else {
            appendDebug(debugLines, "Skip remembered object handle=" + objectHandleText(id) + " not AcDb3dSolid");
        }
        entity->close();
    }

    if (validSolidIds.isEmpty()) {
        return fail("Keine zuletzt extrudierten 3D-Solids gefunden. Bitte zuerst Rechtecke extrudieren.");
    }

    if (docLock != nullptr) {
        docLock.reset();
        appendDebug(debugLines, "Document lock released before native BIMCLASSIFY");
    }

    int classified = 0;
    int errors = 0;
    ads_name selectionSet{};
    if (!createSelectionSet(validSolidIds, selectionSet, debugLines)) {
        errors = validSolidIds.length();
        appendDebug(debugLines, "Selection set creation failed; BIMCLASSIFY not executed");
    } else {
        const int commandStatus = runNativeBimClassifyCommand(selectionSet, bimClass, debugLines);
        if (commandStatus == RTNORM) {
            classified = validSolidIds.length();
            appendDebug(debugLines, "Native BIMCLASSIFY completed for selection");
        } else {
            errors = validSolidIds.length();
            appendDebug(debugLines, "Native BIMCLASSIFY failed for selection");
        }

        const int freeStatus = acedSSFree(selectionSet);
        appendDebug(debugLines, "acedSSFree status=" + std::to_string(freeStatus));
    }

    std::ostringstream response;
    response << "OK"
        << "\tTARGET\t" << percentEncode("lastExtruded")
        << "\tCLASS\t" << percentEncode(bimClass)
        << "\tFOUND\t" << validSolidIds.length()
        << "\tCLASSIFIED\t" << classified
        << "\tERRORS\t" << errors
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string classifySelectorSolidsInApplicationContext(const std::string& selectorJson, const std::string& bimClassUtf8, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::string bimClass = normalizeBimClass(bimClassUtf8);
    const std::string target = jsonStringProperty(selectorJson, "scope").value_or("selector");
    appendDebug(debugLines, "BIMCLASSIFY selector request target='" + target
        + "' class='" + bimClassUtf8
        + "' normalized='" + bimClass
        + "' saveBefore=" + (saveBefore ? "true" : "false")
        + " selector=" + selectorJson);
    if (bimClass.empty()) {
        return fail("BIM-Klasse wird noch nicht unterstuetzt. Aktuell erlaubt: BIMWall");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    appendDebug(debugLines, std::string("Document manager context: ") + (applicationContext ? "application" : "command"));

    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    } else {
        appendDebug(debugLines, "Command context active; explicit document lock skipped");
    }

    const AcDbObjectIdArray candidateIds = selectorObjectIds(selectorJson, database, debugLines);
    if (target == "selection" && candidateIds.isEmpty()) {
        return fail("Keine BricsCAD-Auswahl erkannt. Bitte Solids in BricsCAD auswaehlen und danach erneut in Barebone-Qt bestaetigen.");
    }

    AcDbObjectIdArray validSolidIds;
    for (int i = 0; i < candidateIds.length(); ++i) {
        const AcDbObjectId id = candidateIds.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openStatus = acdbOpenObject(entity, id, AcDb::kForRead);
        if (openStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "Skip selector entity handle=" + objectHandleText(id) + " open failed: " + errorStatusText(openStatus));
            continue;
        }
        if (AcDb3dSolid::cast(entity) != nullptr) {
            validSolidIds.append(id);
            appendDebug(debugLines, "BIMCLASSIFY selector candidate solid handle=" + objectHandleText(id));
        } else {
            appendDebug(debugLines, "Skip selector object handle=" + objectHandleText(id) + " not AcDb3dSolid");
        }
        entity->close();
    }

    if (validSolidIds.isEmpty()) {
        return fail("Selector enthaelt keine 3D-Solids fuer BIMCLASSIFY");
    }

    if (docLock != nullptr) {
        docLock.reset();
        appendDebug(debugLines, "Document lock released before native BIMCLASSIFY");
    }

    int classified = 0;
    int errors = 0;
    ads_name selectionSet{};
    if (!createSelectionSet(validSolidIds, selectionSet, debugLines)) {
        errors = validSolidIds.length();
        appendDebug(debugLines, "Selection set creation failed; BIMCLASSIFY not executed");
    } else {
        const int commandStatus = runNativeBimClassifyCommand(selectionSet, bimClass, debugLines);
        if (commandStatus == RTNORM) {
            classified = validSolidIds.length();
            appendDebug(debugLines, "Native BIMCLASSIFY completed for selector selection");
        } else {
            errors = validSolidIds.length();
            appendDebug(debugLines, "Native BIMCLASSIFY failed for selector selection");
        }

        const int freeStatus = acedSSFree(selectionSet);
        appendDebug(debugLines, "acedSSFree status=" + std::to_string(freeStatus));
    }

    std::ostringstream response;
    response << "OK"
        << "\tTARGET\t" << percentEncode(target)
        << "\tCLASS\t" << percentEncode(bimClass)
        << "\tFOUND\t" << validSolidIds.length()
        << "\tCLASSIFIED\t" << classified
        << "\tERRORS\t" << errors
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string extrudeLayerRectanglesInApplicationContext(const std::string& layerNameUtf8, double heightMm, const std::string& detail, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    {
        std::ostringstream line;
        line << "EXTRUDE request layer='" << layerNameUtf8 << "' heightMm=" << heightMm
            << " detail=" << detail
            << " saveBefore=" << (saveBefore ? "true" : "false");
        appendDebug(debugLines, line.str());
    }

    if (layerNameUtf8.empty()) {
        return fail("Layername ist leer");
    }
    if (!std::isfinite(heightMm) || heightMm <= 0.0) {
        return fail("Hoehe muss groesser als 0 mm sein");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }
    appendDebug(debugLines, "Working database gefunden");

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    appendDebug(debugLines, std::string("Document manager context: ") + (applicationContext ? "application" : "command"));

    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    } else {
        appendDebug(debugLines, "Command context active; explicit document lock skipped");
    }

    const std::basic_string<ACHAR> layerName = utf8ToAchar(layerNameUtf8);
    AcDbObjectId layerId;
    AcDbLayerTable* layerTable = nullptr;
    const Acad::ErrorStatus layerTableStatus = database->getLayerTable(layerTable, AcDb::kForRead);
    appendDebug(debugLines, "getLayerTable: " + errorStatusText(layerTableStatus));
    if (layerTableStatus != Acad::eOk || layerTable == nullptr) {
        return fail("Layer-Tabelle konnte nicht gelesen werden");
    }

    const Acad::ErrorStatus layerStatus = layerTable->getAt(layerName.c_str(), layerId);
    layerTable->close();
    appendDebug(debugLines, "Layer lookup: " + errorStatusText(layerStatus));
    if (layerStatus != Acad::eOk || layerId.isNull()) {
        return fail("Layer wurde in der aktiven Zeichnung nicht gefunden");
    }
    appendDebug(debugLines, "Layer id handle=" + objectHandleText(layerId));

    AcDbBlockTableRecord* space = nullptr;
    const AcDbObjectId currentSpaceId = database->currentSpaceId();
    const Acad::ErrorStatus openSpaceReadStatus = acdbOpenObject(space, currentSpaceId, AcDb::kForRead);
    appendDebug(debugLines, "Open current space for read handle=" + objectHandleText(currentSpaceId) + ": " + errorStatusText(openSpaceReadStatus));
    if (openSpaceReadStatus != Acad::eOk || space == nullptr) {
        return fail("Aktueller Zeichenbereich konnte nicht gelesen werden");
    }

    AcDbBlockTableRecordIterator* iterator = nullptr;
    const Acad::ErrorStatus iteratorStatus = space->newIterator(iterator);
    appendDebug(debugLines, "Create block iterator: " + errorStatusText(iteratorStatus));
    if (iteratorStatus != Acad::eOk || iterator == nullptr) {
        space->close();
        return fail("Objekt-Iterator konnte nicht erstellt werden");
    }

    AcDbObjectIdArray rectangleIds;
    AcDbObjectIdArray solidIdsBefore;
    std::vector<RectangleElementData> elements;
    int scannedEntities = 0;
    int layerEntities = 0;
    for (; !iterator->done(); iterator->step()) {
        AcDbObjectId entityId;
        if (iterator->getEntityId(entityId) != Acad::eOk || entityId.isNull()) {
            continue;
        }
        ++scannedEntities;

        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openEntityStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
        if (openEntityStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "Skip entity handle=" + objectHandleText(entityId) + " open failed: " + errorStatusText(openEntityStatus));
            continue;
        }

        if (detail != "summary" && AcDb3dSolid::cast(entity) != nullptr) {
            solidIdsBefore.append(entityId);
        }

        if (entity->layerId() == layerId) {
            ++layerEntities;
            const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
            RectangleElementData element;
            if (extractRectangleElementData(polyline, entityId, heightMm, element)) {
                rectangleIds.append(entityId);
                elements.push_back(element);
                appendDebug(debugLines, "Rectangle candidate handle=" + objectHandleText(entityId));
            }
        }
        entity->close();
    }

    delete iterator;
    space->close();

    {
        std::ostringstream line;
        line << "Scan result scanned=" << scannedEntities
            << " onLayer=" << layerEntities
            << " rectangles=" << rectangleIds.length();
        appendDebug(debugLines, line.str());
    }

    if (docLock != nullptr) {
        docLock.reset();
        appendDebug(debugLines, "Document lock released before native EXTRUDE");
    }

    int extruded = 0;
    int errors = 0;

    if (!rectangleIds.isEmpty()) {
        ads_name selectionSet{};
        if (!createSelectionSet(rectangleIds, selectionSet, debugLines)) {
            errors = rectangleIds.length();
            appendDebug(debugLines, "Selection set creation failed; EXTRUDE not executed");
        } else {
            const int commandStatus = runNativeExtrudeCommand(selectionSet, heightMm, debugLines);
            if (commandStatus == RTNORM) {
                extruded = rectangleIds.length();
                appendDebug(debugLines, "Native EXTRUDE completed for selection");
                if (detail != "summary") {
                    const AcDbObjectIdArray solidIdsAfter = collectCurrentSpaceSolidIds(database, debugLines);
                    const AcDbObjectIdArray createdSolidIds = newObjectIds(solidIdsBefore, solidIdsAfter);
                    appendDebug(debugLines, "Created solid scan before=" + std::to_string(solidIdsBefore.length())
                        + " after=" + std::to_string(solidIdsAfter.length())
                        + " created=" + std::to_string(createdSolidIds.length()));
                    rememberLastExtrudedSolids(createdSolidIds, layerNameUtf8, debugLines);
                    rememberLastResult(createdSolidIds, "rectangles.extrude", debugLines);
                    matchCreatedSolids(elements, createdSolidIds, debugLines);
                }
            } else {
                errors = rectangleIds.length();
                appendDebug(debugLines, "Native EXTRUDE failed for selection");
            }

            const int freeStatus = acedSSFree(selectionSet);
            appendDebug(debugLines, "acedSSFree status=" + std::to_string(freeStatus));
        }
    }

    const int found = rectangleIds.length();
    const int skipped = std::max(0, found - extruded);
    std::ostringstream response;
    response << "OK"
        << "\tFOUND\t" << found
        << "\tEXTRUDED\t" << extruded
        << "\tSKIPPED\t" << skipped
        << "\tERRORS\t" << errors
        << "\tLAYER\t" << percentEncode(layerNameUtf8)
        << "\tHEIGHT_MM\t" << heightMm
        << "\tDETAIL\t" << detail
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    if (detail != "summary") {
        const bool commandSucceeded = extruded > 0 && errors == 0;
        for (const RectangleElementData& element : elements) {
            response << "ELEMENT\t" << rectangleElementJson(element, layerNameUtf8, detail, commandSucceeded) << "\n";
        }
    }
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string extrudeSelectorRectanglesInApplicationContext(const std::string& selectorJson, double heightMm, const std::string& detail, bool saveBefore)
{
    std::vector<std::string> debugLines;
    auto fail = [&debugLines](const std::string& message) {
        appendDebug(debugLines, "ERROR: " + message);
        std::ostringstream response;
        response << errorResponse(message);
        appendDebugResponse(response, debugLines);
        return response.str();
    };

    const std::string selectorLabel = jsonStringProperty(selectorJson, "layer").value_or(
        jsonStringProperty(selectorJson, "scope").value_or("selector"));
    {
        std::ostringstream line;
        line << "EXTRUDE selector request selector=" << selectorJson
            << " heightMm=" << heightMm
            << " detail=" << detail
            << " saveBefore=" << (saveBefore ? "true" : "false");
        appendDebug(debugLines, line.str());
    }

    if (!std::isfinite(heightMm) || heightMm <= 0.0) {
        return fail("Hoehe muss groesser als 0 mm sein");
    }

    AcDbDatabase* database = workingDatabase();
    if (database == nullptr) {
        return fail("Keine aktive BricsCAD Zeichnung gefunden");
    }
    appendDebug(debugLines, "Working database gefunden");

    bool savedBefore = false;
    if (saveBefore) {
        std::string saveError;
        if (!saveWorkingDatabaseBeforeAction(database, debugLines, saveError)) {
            return fail(saveError);
        }
        savedBefore = true;
    }

    const bool applicationContext = acDocManager != nullptr && acDocManager->isApplicationContext();
    appendDebug(debugLines, std::string("Document manager context: ") + (applicationContext ? "application" : "command"));

    std::unique_ptr<AcAxDocLock> docLock;
    if (applicationContext) {
        docLock = std::make_unique<AcAxDocLock>(database);
        if (docLock->lockStatus() != Acad::eOk) {
            return fail("Aktive Zeichnung konnte nicht gesperrt werden: " + errorStatusText(docLock->lockStatus()));
        }
        appendDebug(debugLines, "Document lock: " + errorStatusText(docLock->lockStatus()));
    } else {
        appendDebug(debugLines, "Command context active; explicit document lock skipped");
    }

    const AcDbObjectIdArray candidateIds = selectorObjectIds(selectorJson, database, debugLines);
    const std::string selectorScope = jsonStringProperty(selectorJson, "scope").value_or("currentSpace");
    if (selectorScope == "selection" && candidateIds.isEmpty()) {
        return fail("Keine BricsCAD-Auswahl erkannt. Bitte Rechtecke in BricsCAD auswaehlen und danach erneut in Barebone-Qt bestaetigen.");
    }

    AcDbObjectIdArray solidIdsBefore;
    if (detail != "summary") {
        solidIdsBefore = collectCurrentSpaceSolidIds(database, debugLines);
    }
    AcDbObjectIdArray rectangleIds;
    std::vector<RectangleElementData> elements;

    for (int i = 0; i < candidateIds.length(); ++i) {
        const AcDbObjectId entityId = candidateIds.at(i);
        AcDbEntity* entity = nullptr;
        const Acad::ErrorStatus openEntityStatus = acdbOpenObject(entity, entityId, AcDb::kForRead);
        if (openEntityStatus != Acad::eOk || entity == nullptr) {
            appendDebug(debugLines, "Skip selector entity handle=" + objectHandleText(entityId) + " open failed: " + errorStatusText(openEntityStatus));
            continue;
        }

        const AcDbPolyline* polyline = AcDbPolyline::cast(entity);
        RectangleElementData element;
        if (extractRectangleElementData(polyline, entityId, heightMm, element)) {
            rectangleIds.append(entityId);
            elements.push_back(element);
            appendDebug(debugLines, "Selector rectangle candidate handle=" + objectHandleText(entityId)
                + " layer=" + entityLayerName(entity));
        } else {
            appendDebug(debugLines, "Skip selector entity handle=" + objectHandleText(entityId)
                + " kind=" + entityKind(entity)
                + " type=" + entityTypeName(entity));
        }
        entity->close();
    }

    appendDebug(debugLines, "Selector scan result candidates=" + std::to_string(candidateIds.length())
        + " rectangles=" + std::to_string(rectangleIds.length()));

    if (rectangleIds.isEmpty()) {
        return fail("Selector enthaelt keine geschlossenen Rechteck-Polylinien fuer EXTRUDE");
    }

    if (docLock != nullptr) {
        docLock.reset();
        appendDebug(debugLines, "Document lock released before native EXTRUDE");
    }

    int extruded = 0;
    int errors = 0;

    if (!rectangleIds.isEmpty()) {
        ads_name selectionSet{};
        if (!createSelectionSet(rectangleIds, selectionSet, debugLines)) {
            errors = rectangleIds.length();
            appendDebug(debugLines, "Selection set creation failed; EXTRUDE not executed");
        } else {
            const int commandStatus = runNativeExtrudeCommand(selectionSet, heightMm, debugLines);
            if (commandStatus == RTNORM) {
                extruded = rectangleIds.length();
                appendDebug(debugLines, "Native EXTRUDE completed for selector selection");
                if (detail != "summary") {
                    const AcDbObjectIdArray solidIdsAfter = collectCurrentSpaceSolidIds(database, debugLines);
                    const AcDbObjectIdArray createdSolidIds = newObjectIds(solidIdsBefore, solidIdsAfter);
                    appendDebug(debugLines, "Created solid scan before=" + std::to_string(solidIdsBefore.length())
                        + " after=" + std::to_string(solidIdsAfter.length())
                        + " created=" + std::to_string(createdSolidIds.length()));
                    rememberLastExtrudedSolids(createdSolidIds, selectorLabel, debugLines);
                    rememberLastResult(createdSolidIds, "rectangles.extrude", debugLines);
                    matchCreatedSolids(elements, createdSolidIds, debugLines);
                }
            } else {
                errors = rectangleIds.length();
                appendDebug(debugLines, "Native EXTRUDE failed for selector selection");
            }

            const int freeStatus = acedSSFree(selectionSet);
            appendDebug(debugLines, "acedSSFree status=" + std::to_string(freeStatus));
        }
    }

    const int found = rectangleIds.length();
    const int skipped = std::max(0, found - extruded);
    std::ostringstream response;
    response << "OK"
        << "\tFOUND\t" << found
        << "\tEXTRUDED\t" << extruded
        << "\tSKIPPED\t" << skipped
        << "\tERRORS\t" << errors
        << "\tLAYER\t" << percentEncode(selectorLabel)
        << "\tHEIGHT_MM\t" << heightMm
        << "\tDETAIL\t" << detail
        << "\tSAVE_BEFORE\t" << (saveBefore ? 1 : 0)
        << "\tSAVED_BEFORE\t" << (savedBefore ? 1 : 0)
        << "\n";
    if (detail != "summary") {
        const bool commandSucceeded = extruded > 0 && errors == 0;
        for (const RectangleElementData& element : elements) {
            response << "ELEMENT\t" << rectangleElementJson(element, selectorLabel, detail, commandSucceeded) << "\n";
        }
    }
    appendDebugResponse(response, debugLines);
    return response.str();
}

std::string handlePluginRequestInApplicationContext(const std::string& request)
{
    const std::vector<std::string> parts = splitTabs(request);
    if (parts.empty() || parts.front().empty()) {
        return errorResponse("Leerer BRX Request");
    }

    const std::string command = toUpperAscii(parts.front());
    if (command == "LAYERS") {
        return listLayersInApplicationContext();
    }

    if (command == "ACTIONVALIDATE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return validateActionsInApplicationContext(paramsJson);
    }

    if (command == "GEOMETRYQUERY") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return geometryQueryInApplicationContext(paramsJson);
    }

    if (command == "SELECTIONDESCRIBE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return selectionDescribeInApplicationContext(paramsJson);
    }

    if (command == "ENTITYDESCRIBE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return entityDescribeInApplicationContext(paramsJson);
    }

    if (command == "GEOMETRYCREATE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return createGeometryInApplicationContext(paramsJson);
    }

    if (command == "GEOMETRYMOVE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return transformEntitiesInApplicationContext(paramsJson, "move");
    }

    if (command == "GEOMETRYCOPY") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return copyEntitiesInApplicationContext(paramsJson);
    }

    if (command == "GEOMETRYROTATE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return transformEntitiesInApplicationContext(paramsJson, "rotate");
    }

    if (command == "GEOMETRYSCALE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return transformEntitiesInApplicationContext(paramsJson, "scale");
    }

    if (command == "GEOMETRYDELETE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return deleteEntitiesInApplicationContext(paramsJson);
    }

    if (command == "SELECTIONSET") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return setSelectionInApplicationContext(paramsJson);
    }

    if (command == "ENTITYSETLAYER") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return setEntityLayerInApplicationContext(paramsJson);
    }

    if (command == "LAYERCREATE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return mutateLayerInApplicationContext(paramsJson, "create");
    }

    if (command == "LAYERRENAME") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return mutateLayerInApplicationContext(paramsJson, "rename");
    }

    if (command == "LAYERSETCOLOR") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return mutateLayerInApplicationContext(paramsJson, "setColor");
    }
    if (command == "LAYERBATCH") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return mutateLayerBatchInApplicationContext(paramsJson);
    }

    if (command == "DOCUMENTSAVE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return saveDocumentInApplicationContext(paramsJson);
    }

    if (command == "MEASUREBBOX") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return measureEntitiesInApplicationContext(paramsJson, "bbox");
    }

    if (command == "MEASURELENGTH") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return measureEntitiesInApplicationContext(paramsJson, "length");
    }

    if (command == "MEASUREAREA") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return measureEntitiesInApplicationContext(paramsJson, "area");
    }

    if (command == "PROFILEEXTRUDE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return extrudeProfilesInApplicationContext(paramsJson);
    }

    if (command == "PIPESVALIDATE") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return validatePipeNetworkInApplicationContext(paramsJson);
    }

    if (command == "PIPESCREATESOLIDS") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return createPipeNetworkSolidsInApplicationContext(paramsJson);
    }

    if (command == "ROOMDIMENSIONS") {
        const std::string paramsJson = parts.size() >= 2 ? percentDecode(parts[1]) : "{}";
        return createRoomDimensionsInApplicationContext(paramsJson);
    }

    if (command == "POSTCOMMAND") {
        if (parts.size() < 2) {
            return errorResponse("POSTCOMMAND erwartet commandLine");
        }

        const std::string commandLine = percentDecode(parts[1]);
        const bool saveBefore = parts.size() >= 3 && parts[2] == "1";
        return postNativeCommandLineInApplicationContext(commandLine, saveBefore);
    }

    if (command == "EXTRUDE") {
        if (parts.size() < 3) {
            return errorResponse("EXTRUDE erwartet Layer und Hoehe");
        }

        const std::string layerName = percentDecode(parts[1]);
        char* parseEnd = nullptr;
        const double height = std::strtod(parts[2].c_str(), &parseEnd);
        if (parseEnd == parts[2].c_str()) {
            return errorResponse("Hoehe konnte nicht gelesen werden");
        }

        const std::string detail = parts.size() >= 4 ? normalizeDetail(parts[3]) : "element";
        const bool saveBefore = parts.size() >= 5 && parts[4] == "1";
        return extrudeLayerRectanglesInApplicationContext(layerName, height, detail, saveBefore);
    }

    if (command == "EXTRUDESELECTOR") {
        if (parts.size() < 3) {
            return errorResponse("EXTRUDESELECTOR erwartet Selector und Hoehe");
        }

        const std::string selectorJson = percentDecode(parts[1]);
        char* parseEnd = nullptr;
        const double height = std::strtod(parts[2].c_str(), &parseEnd);
        if (parseEnd == parts[2].c_str()) {
            return errorResponse("Hoehe konnte nicht gelesen werden");
        }

        const std::string detail = parts.size() >= 4 ? normalizeDetail(parts[3]) : "element";
        const bool saveBefore = parts.size() >= 5 && parts[4] == "1";
        return extrudeSelectorRectanglesInApplicationContext(selectorJson, height, detail, saveBefore);
    }

    if (command == "BIMCLASSIFY") {
        if (parts.size() < 2) {
            return errorResponse("BIMCLASSIFY erwartet eine BIM-Klasse");
        }

        const std::string bimClass = percentDecode(parts[1]);
        const bool saveBefore = parts.size() >= 3 && parts[2] == "1";
        return classifyLastExtrudedSolidsInApplicationContext(bimClass, saveBefore);
    }

    if (command == "BIMCLASSIFYSELECTOR") {
        if (parts.size() < 3) {
            return errorResponse("BIMCLASSIFYSELECTOR erwartet BIM-Klasse und Selector");
        }

        const std::string bimClass = percentDecode(parts[1]);
        const std::string selectorJson = percentDecode(parts[2]);
        const bool saveBefore = parts.size() >= 4 && parts[3] == "1";
        return classifySelectorSolidsInApplicationContext(selectorJson, bimClass, saveBefore);
    }

    if (command == "UNDO") {
        if (parts.size() < 2) {
            return errorResponse("UNDO erwartet Schritte");
        }

        char* parseEnd = nullptr;
        const int steps = static_cast<int>(std::strtol(parts[1].c_str(), &parseEnd, 10));
        if (parseEnd == parts[1].c_str()) {
            return errorResponse("UNDO steps konnte nicht gelesen werden");
        }
        if (steps < 1) {
            return errorResponse("UNDO steps muss mindestens 1 sein");
        }
        const bool saveBefore = parts.size() >= 3 && parts[2] == "1";
        return undoLastActionInApplicationContext(steps, saveBefore);
    }

    if (command == "REDO") {
        const int steps = parts.size() >= 2 ? std::max(1, std::atoi(parts[1].c_str())) : 1;
        const bool saveBefore = parts.size() >= 3 && parts[2] == "1";
        return redoLastActionInApplicationContext(steps, saveBefore);
    }

    return errorResponse("Unbekannter BRX Request");
}

void processBridgeJobInApplicationContext(void* data)
{
    auto* job = static_cast<BridgeJob*>(data);
    finishBridgeJob(job, handlePluginRequestInApplicationContext(job->request));
}

void processBridgeJobInCommandContext(void* data)
{
    auto* job = static_cast<BridgeJob*>(data);
    finishBridgeJob(job, handlePluginRequestInApplicationContext(job->request));
}

std::string dispatchPluginRequest(const std::string& rawRequest)
{
    const std::string request = trim(rawRequest);
    if (request.empty()) {
        return errorResponse("Leerer BRX Request");
    }

    if (toUpperAscii(request) == "PING") {
        return "OK\tBareboneBrx\tready\n";
    }

    if (acDocManager == nullptr) {
        return errorResponse("BricsCAD Document Manager ist nicht verfuegbar");
    }

    BridgeJob job(request);
    if (requiresCommandContext(request)) {
        const Acad::ErrorStatus scheduleStatus = acDocManager->beginExecuteInCommandContext(&processBridgeJobInCommandContext, &job);
        if (scheduleStatus != Acad::eOk) {
            return errorResponse("BricsCAD Command Context konnte nicht gestartet werden: " + errorStatusText(scheduleStatus));
        }
    } else {
        acDocManager->executeInApplicationContext(&processBridgeJobInApplicationContext, &job);
    }

    std::unique_lock<std::mutex> lock(job.mutex);
    job.doneEvent.wait(lock, [&job]() {
        return job.done;
    });
    return job.response.empty() ? errorResponse("Leere Antwort aus BricsCAD Application Context") : job.response;
}

std::string bridgeTokenFilePath()
{
    char tempPath[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) {
        return kBridgeTokenFileName;
    }
    return std::string(tempPath) + kBridgeTokenFileName;
}

std::string readBridgeToken()
{
    std::ifstream tokenFile(bridgeTokenFilePath());
    std::string token;
    std::getline(tokenFile, token);
    return trim(token);
}

std::string jsonResponseForBridgeRequest(const std::string& line)
{
    const int id = jsonIntProperty(line, "id").value_or(0);
    const std::string type = jsonStringProperty(line, "type").value_or("");
    if (type == "event") {
        appendBridgeUiLog("Qt -> BRX event: " + jsonStringProperty(line, "event").value_or("<unbekannt>"));
        return {};
    }
    if (type != "request") {
        appendBridgeUiLog("Qt -> BRX ignoriert: type=" + (type.empty() ? std::string("<leer>") : type));
        return {};
    }

    const std::string method = jsonStringProperty(line, "method").value_or("");
    appendBridgeUiLog("Qt -> BRX request id=" + std::to_string(id) + " method=" + (method.empty() ? std::string("<leer>") : method));
    if (method == "capabilities.list") {
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=capabilities.list");
        return jsonCapabilitiesResponseFromDescriptors(id);
    }

    if (method == "commands.list") {
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=commands.list");
        return jsonCommandsResponseFromDescriptors(id);
    }

    if (method == "actions.list") {
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=actions.list");
        return jsonActionsResponseFromDescriptors(id);
    }

    if (method == "actions.validate") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=actions.validate missing params");
            return jsonErrorResponse(id, "actions.validate erwartet params");
        }
        appendBridgeUiLog("Qt -> BRX actions.validate params=" + *params);
        std::ostringstream request;
        request << "ACTIONVALIDATE\t" << percentEncode(*params);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=actions.validate");
        return response;
    }

    if (method == "layers.list") {
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest("LAYERS"));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=layers.list");
        return response;
    }

    if (method == "geometry.query" || method == "selection.describe" || method == "entity.describe") {
        const std::string params = jsonObjectProperty(line, "params").value_or("{}");
        std::string command = "GEOMETRYQUERY";
        if (method == "selection.describe") {
            command = "SELECTIONDESCRIBE";
        } else if (method == "entity.describe") {
            command = "ENTITYDESCRIBE";
        }

        appendBridgeUiLog("Qt -> BRX " + method + " params=" + params);
        std::ostringstream request;
        request << command << '\t' << percentEncode(params);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=" + method);
        return response;
    }

    if (method == "measurement.bbox" || method == "measurement.length" || method == "measurement.area") {
        const std::string params = jsonObjectProperty(line, "params").value_or("{}");
        std::string command = "MEASUREBBOX";
        if (method == "measurement.length") {
            command = "MEASURELENGTH";
        } else if (method == "measurement.area") {
            command = "MEASUREAREA";
        }
        appendBridgeUiLog("Qt -> BRX " + method + " params=" + params);
        std::ostringstream request;
        request << command << '\t' << percentEncode(params);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=" + method);
        return response;
    }

    auto dispatchJsonParamsMethod = [&](const std::string& nativeCommand) -> std::string {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=" + method + " missing params");
            return jsonErrorResponse(id, method + " erwartet params gemaess apiDoc.post.bodySchema");
        }
        appendBridgeUiLog("Qt -> BRX " + method + " params=" + *params);
        std::ostringstream request;
        request << nativeCommand << '\t' << percentEncode(*params);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=" + method);
        return response;
    };

    if (method == "geometry.move") {
        return dispatchJsonParamsMethod("GEOMETRYMOVE");
    }
    if (method == "pipes.validateNetwork") {
        return dispatchJsonParamsMethod("PIPESVALIDATE");
    }
    if (method == "pipes.createNetworkSolids") {
        return dispatchJsonParamsMethod("PIPESCREATESOLIDS");
    }
    if (method == "annotations.createRoomDimensions") {
        return dispatchJsonParamsMethod("ROOMDIMENSIONS");
    }
    if (method == "geometry.copy") {
        return dispatchJsonParamsMethod("GEOMETRYCOPY");
    }
    if (method == "geometry.rotate") {
        return dispatchJsonParamsMethod("GEOMETRYROTATE");
    }
    if (method == "geometry.scale") {
        return dispatchJsonParamsMethod("GEOMETRYSCALE");
    }
    if (method == "geometry.delete") {
        return dispatchJsonParamsMethod("GEOMETRYDELETE");
    }
    if (method == "selection.set") {
        return dispatchJsonParamsMethod("SELECTIONSET");
    }
    if (method == "entity.setLayer") {
        return dispatchJsonParamsMethod("ENTITYSETLAYER");
    }
    if (method == "layers.create") {
        return dispatchJsonParamsMethod("LAYERCREATE");
    }
    if (method == "layers.rename") {
        return dispatchJsonParamsMethod("LAYERRENAME");
    }
    if (method == "layers.setColor") {
        return dispatchJsonParamsMethod("LAYERSETCOLOR");
    }
    if (method == "layers.batch") {
        return dispatchJsonParamsMethod("LAYERBATCH");
    }
    if (method == "document.save") {
        const std::string params = jsonObjectProperty(line, "params").value_or("{}");
        appendBridgeUiLog("Qt -> BRX document.save params=" + params);
        std::ostringstream request;
        request << "DOCUMENTSAVE\t" << percentEncode(params);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=document.save");
        return response;
    }
    if (method == "profile.extrude") {
        return dispatchJsonParamsMethod("PROFILEEXTRUDE");
    }
    if (method == "undo.redo") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        const int steps = params.has_value() ? static_cast<int>(jsonDoubleProperty(*params, "steps").value_or(1.0)) : 1;
        const bool saveBefore = params.has_value() ? jsonBoolProperty(*params, "saveBefore").value_or(true) : true;
        std::ostringstream request;
        request << "REDO\t" << std::max(1, steps) << '\t' << (saveBefore ? 1 : 0);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=undo.redo");
        return response;
    }

    if (method == "command.execute") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=command.execute missing params");
            return jsonErrorResponse(id, "command.execute erwartet params");
        }

        const std::string commandLine = jsonStringProperty(*params, "commandLine").value_or("");
        const bool saveBefore = jsonBoolProperty(*params, "saveBefore").value_or(true);

        appendBridgeUiLog("Qt -> BRX command.execute commandLine='" + commandLine
            + "' saveBefore=" + (saveBefore ? "true" : "false"));

        if (!commandLine.empty()) {
            std::ostringstream request;
            request << "POSTCOMMAND\t" << percentEncode(commandLine) << '\t' << (saveBefore ? 1 : 0);
            const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
            appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=command.execute commandLine");
            return response;
        }

        appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=command.execute missing commandLine");
        return jsonErrorResponse(id, "command.execute erwartet eine native BricsCAD commandLine");
    }

    if (method == "undo.last") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=undo.last missing params");
            return jsonErrorResponse(id, "undo.last erwartet params");
        }

        const int steps = static_cast<int>(jsonDoubleProperty(*params, "steps").value_or(1.0));
        const bool saveBefore = jsonBoolProperty(*params, "saveBefore").value_or(true);
        appendBridgeUiLog("Qt -> BRX undo.last steps=" + std::to_string(steps)
            + " saveBefore=" + (saveBefore ? "true" : "false"));
        if (steps <= 0) {
            return jsonErrorResponse(id, "undo.last erwartet steps > 0");
        }

        std::ostringstream request;
        request << "UNDO\t" << steps << '\t' << (saveBefore ? 1 : 0);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=undo.last");
        return response;
    }

    if (method == "geometry.create") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=geometry.create missing params");
            return jsonErrorResponse(id, "geometry.create erwartet params gemaess apiDoc.post.bodySchema");
        }

        const std::string geometry = jsonStringProperty(*params, "geometry").value_or(
            jsonStringProperty(*params, "type").value_or(""));
        appendBridgeUiLog("Qt -> BRX geometry.create geometry='" + geometry + "' params=" + *params);
        if (trim(geometry).empty()) {
            return jsonErrorResponse(id, "geometry.create POST braucht geometry=point|rectangle|line|polyline|circle|arc|box");
        }

        std::ostringstream request;
        request << "GEOMETRYCREATE\t" << percentEncode(*params);
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=geometry.create");
        return response;
    }

    if (method == "rectangles.extrude") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=rectangles.extrude missing params");
            return jsonErrorResponse(id, "rectangles.extrude erwartet params gemaess apiDoc.post.bodySchema");
        }

        const std::string layer = jsonStringProperty(*params, "layer").value_or("");
        const std::optional<std::string> selector = jsonObjectProperty(*params, "selector");
        const double heightMm = jsonDoubleProperty(*params, "heightMm").value_or(0.0);
        const std::string detail = normalizeDetail(jsonStringProperty(*params, "detail").value_or("element"));
        const bool saveBefore = jsonBoolProperty(*params, "saveBefore").value_or(true);
        appendBridgeUiLog("Qt -> BRX rectangles.extrude layer='" + layer
            + "' selector=" + (selector.has_value() ? *selector : std::string("<none>"))
            + "' heightMm=" + std::to_string(heightMm)
            + " detail=" + detail
            + " saveBefore=" + (saveBefore ? "true" : "false"));
        if ((layer.empty() && !selector.has_value()) || !std::isfinite(heightMm) || heightMm <= 0.0) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=rectangles.extrude invalid params");
            return jsonErrorResponse(id, "rectangles.extrude POST braucht heightMm > 0 und layer oder selector");
        }

        std::ostringstream request;
        if (selector.has_value()) {
            request << "EXTRUDESELECTOR\t" << percentEncode(*selector) << '\t' << heightMm << '\t' << detail << '\t' << (saveBefore ? 1 : 0);
        } else {
            request << "EXTRUDE\t" << percentEncode(layer) << '\t' << heightMm << '\t' << detail << '\t' << (saveBefore ? 1 : 0);
        }
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()), detail);
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=rectangles.extrude");
        return response;
    }

    if (method == "bim.classify") {
        const std::optional<std::string> params = jsonObjectProperty(line, "params");
        if (!params.has_value()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=bim.classify missing params");
            return jsonErrorResponse(id, "bim.classify erwartet params gemaess apiDoc.post.bodySchema");
        }

        const std::string requestedClass = jsonStringProperty(*params, "classification").value_or(
            jsonStringProperty(*params, "class").value_or(""));
        const std::string bimClass = normalizeBimClass(requestedClass);
        const std::string target = jsonStringProperty(*params, "target").value_or("");
        std::optional<std::string> selector = jsonObjectProperty(*params, "selector");
        const bool saveBefore = jsonBoolProperty(*params, "saveBefore").value_or(true);
        if (!selector.has_value() && target == "selection") {
            selector = std::string("{\"scope\":\"selection\",\"kind\":\"solid\"}");
        }
        appendBridgeUiLog("Qt -> BRX bim.classify class='" + requestedClass
            + "' normalized='" + bimClass
            + "' selector=" + (selector.has_value() ? *selector : std::string("<lastExtruded>"))
            + "' saveBefore=" + (saveBefore ? "true" : "false"));
        if (bimClass.empty()) {
            appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " method=bim.classify invalid class");
            return jsonErrorResponse(id, "bim.classify POST braucht classification=BIMWall");
        }

        std::ostringstream request;
        if (selector.has_value()) {
            request << "BIMCLASSIFYSELECTOR\t" << percentEncode(bimClass) << '\t' << percentEncode(*selector) << '\t' << (saveBefore ? 1 : 0);
        } else {
            request << "BIMCLASSIFY\t" << percentEncode(bimClass) << '\t' << (saveBefore ? 1 : 0);
        }
        const std::string response = jsonResponseFromProtocol(id, method, dispatchPluginRequest(request.str()));
        appendBridgeUiLog("BRX -> Qt response id=" + std::to_string(id) + " method=bim.classify");
        return response;
    }

    appendBridgeUiLog("BRX -> Qt error id=" + std::to_string(id) + " unknown method=" + method);
    return jsonErrorResponse(id, "Unbekannte BRX Methode: " + method);
}

std::string helloMessage(const std::string& token)
{
    std::ostringstream message;
    message << "{\"type\":\"hello\""
        << ",\"role\":\"brx\""
        << ",\"plugin\":\"BareboneBrx\""
        << ",\"protocol\":1"
        << ",\"bridgeBuild\":\"" << kBridgeBuildId << "\""
        << ",\"token\":\"" << jsonEscape(token) << "\""
        << "}\n";
    return message.str();
}

bool connectBridgeSocket(SOCKET socketHandle)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kQtBridgePort);
    if (inet_pton(AF_INET, kQtBridgeHost, &address.sin_addr) != 1) {
        return false;
    }
    return connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR;
}

void closeBridgeClientSocket(SOCKET socketHandle)
{
    bool closeSocket = true;
    {
        std::lock_guard<std::mutex> lock(g_bridgeClientSocketMutex);
        if (g_bridgeClientSocket == socketHandle) {
            g_bridgeClientSocket = INVALID_SOCKET;
        } else {
            closeSocket = false;
        }
    }

    g_bridgeClientConnected.store(false);
    if (closeSocket && socketHandle != INVALID_SOCKET) {
        shutdown(socketHandle, SD_BOTH);
        closesocket(socketHandle);
    }
}

void sleepBeforeBridgeReconnect()
{
    for (int i = 0; i < 10 && g_bridgeClientRunning.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void bridgeClientLoop()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        writeBrxDebugLog("Bridge client: WSAStartup failed");
        return;
    }

    while (g_bridgeClientRunning.load()) {
        const std::string token = readBridgeToken();
        if (token.empty()) {
            writeBrxDebugLog("Bridge client: token file not ready: " + bridgeTokenFilePath());
            appendBridgeUiLog("BRX wartet auf Barebone-Qt Token: " + bridgeTokenFilePath());
            sleepBeforeBridgeReconnect();
            continue;
        }

        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle == INVALID_SOCKET) {
            sleepBeforeBridgeReconnect();
            continue;
        }

        if (!connectBridgeSocket(socketHandle)) {
            closesocket(socketHandle);
            sleepBeforeBridgeReconnect();
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_bridgeClientSocketMutex);
            g_bridgeClientSocket = socketHandle;
        }
        g_bridgeClientConnected.store(true);
        writeBrxDebugLog("Bridge client: connected to Barebone-Qt");
        appendBridgeUiLog("BRX -> Qt TCP verbunden auf 127.0.0.1:" + std::to_string(kQtBridgePort));

        if (!sendBridgeClientLine(helloMessage(token))) {
            appendBridgeUiLog("BRX -> Qt hello konnte nicht gesendet werden");
            closeBridgeClientSocket(socketHandle);
            sleepBeforeBridgeReconnect();
            continue;
        }
        appendBridgeUiLog("BRX -> Qt hello gesendet");

        std::string buffer;
        char chunk[4096]{};
        while (g_bridgeClientRunning.load()) {
            const int received = recv(socketHandle, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                break;
            }

            buffer.append(chunk, received);
            while (true) {
                const std::size_t newline = buffer.find('\n');
                if (newline == std::string::npos) {
                    break;
                }

                const std::string line = trim(buffer.substr(0, newline));
                buffer.erase(0, newline + 1);
                if (line.empty()) {
                    continue;
                }

                const std::string response = jsonResponseForBridgeRequest(line);
                if (!response.empty() && !sendBridgeClientLine(response)) {
                    break;
                }
            }
        }

        writeBrxDebugLog("Bridge client: disconnected from Barebone-Qt");
        appendBridgeUiLog("BRX Verbindung zu Barebone-Qt getrennt");
        closeBridgeClientSocket(socketHandle);
        sleepBeforeBridgeReconnect();
    }

    WSACleanup();
}

void startBridgeClient()
{
    bool expected = false;
    if (!g_bridgeClientRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    g_bridgeClientThread = std::thread(bridgeClientLoop);
}

void stopBridgeClient()
{
    bool expected = true;
    if (!g_bridgeClientRunning.compare_exchange_strong(expected, false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_bridgeClientSocketMutex);
        if (g_bridgeClientSocket != INVALID_SOCKET) {
            shutdown(g_bridgeClientSocket, SD_BOTH);
            closesocket(g_bridgeClientSocket);
            g_bridgeClientSocket = INVALID_SOCKET;
        }
    }

    if (g_bridgeClientThread.joinable()) {
        g_bridgeClientThread.join();
    }
    g_bridgeClientConnected.store(false);
}

void processStartSelectionTrackingInApplicationContext(void*)
{
    if (!g_pluginLoaded.load()) {
        return;
    }
    startSelectionTracking();
    captureCurrentSelection("autoStart");
    writeBrxDebugLog("Selection tracking auto-started after APPLOAD");
}

void startSelectionTrackingDelayed()
{
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (!g_pluginLoaded.load() || acDocManager == nullptr) {
            return;
        }
        acDocManager->executeInApplicationContext(&processStartSelectionTrackingInApplicationContext, nullptr);
    }).detach();
}

void printBridgeStatus()
{
    const std::wstring status = g_bridgeClientConnected.load() ? L"verbunden" : L"nicht verbunden";
    const std::wstring host = asciiToWide(kQtBridgeHost);
    const std::wstring tokenPath = asciiToWide(bridgeTokenFilePath());
    acutPrintf(
        _T("\nBarebone-Qt Bridge: %ls, Ziel %ls:%d, Token-Datei: %ls"),
        status.c_str(),
        host.c_str(),
        kQtBridgePort,
        tokenPath.c_str());
}

} // namespace

class BareboneBrxApp final : public AcRxArxApp {
public:
    BareboneBrxApp()
        : AcRxArxApp()
    {
    }

    void RegisterServerComponents() override
    {
    }

    AcRx::AppRetCode On_kInitAppMsg(void* appData) override
    {
        const AcRx::AppRetCode result = AcRxArxApp::On_kInitAppMsg(appData);
        acrxRegisterAppMDIAware(appData);
        acrxUnlockApplication(appData);
        g_pluginLoaded.store(true);

        resetBrxDebugLog();
        writeBrxDebugLog("Init: debug log reset");
        writeBrxDebugLog("Init: automatic log window skipped during APPLOAD; use BBLOG after load");
        writeBrxDebugLog("Init: automatic selection tracking scheduled after APPLOAD");
        writeBrxDebugLog("Init: before startBridgeClient");
        startBridgeClient();
        writeBrxDebugLog("Init: after startBridgeClient");
        startSelectionTrackingDelayed();

        acutPrintf(_T("\nBarebone-Qt BRX Schnittstelle geladen."));
        acutPrintf(_T("\nDirekte Kommunikation: BRX verbindet sich als Client zu 127.0.0.1:%d"), kQtBridgePort);
        const std::wstring debugLogPath = asciiToWide(brxDebugLogPath());
        acutPrintf(_T("\nDebug Log: %ls"), debugLogPath.c_str());
        const std::wstring tokenPath = asciiToWide(bridgeTokenFilePath());
        acutPrintf(_T("\nToken-Datei: %ls"), tokenPath.c_str());
        acutPrintf(_T("\nVerfuegbare Befehle: BBPING, BBINFO, BBLOG, BBTRACK\n"));
        return result;
    }

    AcRx::AppRetCode On_kUnloadAppMsg(void* appData) override
    {
        g_pluginLoaded.store(false);
        appendBridgeUiLog("Barebone-Qt BRX Plugin wird entladen");
        stopSelectionTracking();
        stopBridgeClient();
        destroyBridgeLogWindow();
        acutPrintf(_T("\nBarebone-Qt BRX Schnittstelle entladen.\n"));
        return AcRxArxApp::On_kUnloadAppMsg(appData);
    }

    static void BareboneQtBBPING()
    {
        printBridgeStatus();
    }

    static void BareboneQtBBINFO()
    {
        printBridgeStatus();
        const std::wstring debugLogPath = asciiToWide(brxDebugLogPath());
        acutPrintf(_T("\nBarebone-Qt BRX: Protocol=NDJSON, Debug Log=%ls"), debugLogPath.c_str());
    }

    static void BareboneQtBBLOG()
    {
        showBridgeLogWindow();
        appendBridgeUiLog("Bridge Logfenster angezeigt");
    }

    static void BareboneQtBBTRACK()
    {
        startSelectionTracking();
        acutPrintf(_T("\nBarebone-Qt BRX: Auswahltracking aktiviert."));
    }
};

IMPLEMENT_ARX_ENTRYPOINT(BareboneBrxApp)

ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneQt, BBPING, BBPING, ACRX_CMD_TRANSPARENT, NULL)
ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneQt, BBINFO, BBINFO, ACRX_CMD_TRANSPARENT, NULL)
ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneQt, BBLOG, BBLOG, ACRX_CMD_TRANSPARENT, NULL)
ACED_ARXCOMMAND_ENTRY_AUTO(BareboneBrxApp, BareboneQt, BBTRACK, BBTRACK, ACRX_CMD_TRANSPARENT, NULL)
