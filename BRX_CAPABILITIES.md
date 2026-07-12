# BRX Capabilities Uebersicht

Stand: 2026-07-12

Quelle:
- `src/bricscad/BareboneBrxPlugin.cpp`: `kBridgeMethods`, `kBridgeCommands`
- `src/ui/BricsCadPage.cpp`: `BricsCadPage::availableAgentTools()`

Aktuelle Qt-Log-Zeile:

```text
BRX-Capabilities geladen: 38 Methoden, 22 Commands, 39 Agent-Tools
```

Die 39 Agent-Tools gelten bei verfuegbarer BIM-API und BIM-Lizenz: 38 BRX-Methoden plus `layers.ensureMany`. Sind die BIM-API oder Lizenz nicht verfuegbar, bleiben die vier neuen BIM-Methoden und das kompatibel erhaltene `bim.classify` mit `available=false` in `capabilities.list`, werden aber nicht an die AI angeboten; dann sind es 34 Agent-Tools.

## Begriffe

- Methoden: alle Bridge-Methoden, die das BRX-Plugin ueber `capabilities.list` meldet.
- BRX-Action-Methoden: Methoden mit `kind=action` in `capabilities.list`.
- Agent-Tools: alle BRX-Methoden aus `capabilities.list`, die Barebone-Qt dem AI-Agenten als freigegebene Tools gibt, inklusive read-only Diagnose-/Kontextmethoden, Preflight und Action-Methoden. Zusaetzlich gibt es Qt-virtuelle Agent-Tools.
- Qt-virtuelle Agent-Tools: zusaetzliche Tools, die nur Barebone-Qt dem Agenten anbietet und vor BRX-Preflight/Ausfuehrung in echte BRX-Aktionen expandiert. Aktuell: `layers.ensureMany`.
- Commands: native BricsCAD-Kommandos als Low-Level-Referenz. Die AI kann je nach Aufgabe zwischen spezialisierten Bridge-Tools und validiertem `command.execute` entscheiden.
- Preflight: Vor der Nutzerbestaetigung prueft Qt jeden Vorschlag ueber `actions.validate` im BRX-Plugin.
- Hinweis: `layers.batch` ist als BRX-Agent-Tool freigegeben. Fuer einfache Layer-Anlage bleibt `layers.ensureMany` oft kompakter; fuer gemischte Layer-Batches kann `layers.batch` direkt genutzt werden.
- Direkte BricsCAD-DB-Schreibvorgaenge sind fuer die AI verboten. Vorschlaege duerfen nur die von Qt angebotenen `tools[].name` verwenden; keine AcDb-/LayerTable-/EntityTable-Mutationen und keine Pseudo-Tools fuer Datenbankwrites.
- Native Commands mit `acedCommand` duerfen im BRX nur im BricsCAD Command Context laufen. Barebone-Qt/BRX plant Layer, Extrude, BIMClassify, Undo und Redo deshalb ueber `beginExecuteInCommandContext`; ein Guard blockiert versehentliche Native-Command-Ausfuehrung im Application Context.

## Methoden (38)

| # | Methode | Art | Risiko | Modul/Zweck |
|---:|---|---|---|---|
| 1 | `capabilities.list` | query | readOnly | Vollstaendige BRX-Capability-Liste laden. |
| 2 | `actions.list` | query | readOnly | Dokumentierte API-Aktionen mit Schemas, Pflichtfeldern und Beispielen laden. |
| 3 | `actions.validate` | query | readOnly | Agent-Vorschlaege trocken gegen Syntax, Pflichtdaten und Zeichnungszustand pruefen. |
| 4 | `pipes.validateNetwork` | query | readOnly | Rohrnetz, Grundrissbounds und Technikraumstart validieren. |
| 5 | `pipes.createNetworkSolids` | action | modifiesDrawing | Rohrsegmente aus validierten Konturen erzeugen. |
| 6 | `annotations.createRoomDimensions` | action | modifiesDrawing | Raumdimensionen und Beschriftungen erzeugen. |
| 7 | `commands.list` | query | readOnly | Native BricsCAD-Commands als Kontextliste laden. |
| 8 | `layers.list` | query | readOnly | Layer der aktiven Zeichnung lesen. |
| 9 | `geometry.query` | query | readOnly | Geometrien ueber Selector und Filter abfragen. |
| 10 | `selection.describe` | query | readOnly | Aktuelle BricsCAD-Auswahl beschreiben. |
| 11 | `entity.describe` | query | readOnly | Entities per Handle oder exaktem BIM-Namen inklusive BIM-Daten beschreiben. |
| 12 | `bim.objects.query` | query | readOnly | Klassifizierte BIM-Objekte, Bounds und optional typisierte Generic Properties tabellarisch lesen. |
| 13 | `bim.selection.set` | action | modifiesEditorState | BIM-Objekte per exaktem Namen oder Handle als Pickfirst-Auswahl setzen. |
| 14 | `bim.move` | action | modifiesDrawing | Klassifizierte BIM-Entities atomar mit einem WCS-Vektor verschieben. |
| 15 | `bim.rotate` | action | modifiesDrawing | Klassifizierte BIM-Entities atomar um eine WCS-Achse rotieren. |
| 16 | `geometry.create` | action | modifiesDrawing | Grundgeometrie erzeugen. |
| 17 | `rectangles.extrude` | action | modifiesDrawing | Geschlossene Rechteck-Polylinien extrudieren. |
| 18 | `undo.last` | action | modifiesDrawing | Letzte Aktionen rueckgaengig machen. |
| 19 | `bim.classify` | action | modifiesDrawing | 3D-Solids als BIM-Element klassifizieren. |
| 20 | `profile.extrude` | action | modifiesDrawing | Geschlossene 2D-Profile extrudieren. |
| 21 | `geometry.move` | action | modifiesDrawing | Entities ueber Selector verschieben. |
| 22 | `geometry.copy` | action | modifiesDrawing | Entities ueber Selector kopieren. |
| 23 | `geometry.rotate` | action | modifiesDrawing | Entities ueber Selector rotieren. |
| 24 | `geometry.scale` | action | modifiesDrawing | Entities ueber Selector uniform skalieren. |
| 25 | `geometry.delete` | action | modifiesDrawing | Entities ueber Selector loeschen. |
| 26 | `selection.set` | action | modifiesEditorState | Aktuelle Auswahl in BricsCAD setzen. |
| 27 | `entity.setLayer` | action | modifiesDrawing | Vorhandene Entities einem Ziel-Layer zuweisen. |
| 28 | `entity.setName` | action | modifiesDrawing | Namen vorhandener Entities setzen. |
| 29 | `layers.create` | action | modifiesDrawing | Layer anlegen. |
| 30 | `layers.rename` | action | modifiesDrawing | Vorhandenen Layer umbenennen. |
| 31 | `layers.setColor` | action | modifiesDrawing | ACI-Farbe eines Layers setzen. |
| 32 | `layers.batch` | action | modifiesDrawing | Layer-Aktionen als Batch ausfuehren. |
| 33 | `command.execute` | action | modifiesDrawing | Eine validierte Kommandozeile aus `commands.list` posten. |
| 34 | `document.save` | action | modifiesDocument | Aktive Zeichnung explizit speichern. |
| 35 | `measurement.bbox` | query | readOnly | Bounding-Boxes und Dimensionen berechnen. |
| 36 | `measurement.length` | query | readOnly | Kurvenlaengen berechnen. |
| 37 | `measurement.area` | query | readOnly | Flaechen berechnen. |
| 38 | `undo.redo` | action | modifiesDrawing | Redo ausfuehren. |

## BRX-Action-Methoden (25, verfuegbare Methoden sind direkte AI-Tools)

Diese 25 Methoden sind im BRX als `kind=action` vorhanden. Zusaetzlich bietet Qt das virtuelle Agent-Tool `layers.ensureMany` an. Read-only BRX-Methoden sind ebenfalls fuer Kontext, Diagnose und Try-before-fail freigegeben. Read-only Tools duerfen bei Tabellen-/Datenfragen automatisch laufen; mutierende Tools laufen vor der Nutzerbestaetigung durch lokale Qt-Validierung und BRX-Preflight via `actions.validate`. Die drei neuen BIM-Actions und `bim.classify` werden bei fehlender BIM-Verfuegbarkeit nicht an die AI angeboten.

| # | BRX-Action | Kategorie | Sicherheitslogik |
|---:|---|---|---|
| 1 | `pipes.createNetworkSolids` | Rohrnetz | Vorher validierte Konturen und Technikraumbezug verlangen. |
| 2 | `annotations.createRoomDimensions` | Annotation | Raumgeometrie und Beschriftungsparameter pruefen. |
| 3 | `bim.selection.set` | BIM/Auswahl | Exakte Namen oder Handles, Klassifikation und Mehrdeutigkeit pruefen. |
| 4 | `bim.move` | BIM/Transformation | Fingerprints, Einheiten, Anker, XRef, Layerstatus und Vektor pruefen. |
| 5 | `bim.rotate` | BIM/Transformation | Fingerprints, Einheiten, Anker, XRef, Winkel, Achse und Basispunkt pruefen. |
| 6 | `geometry.create` | Geometrie | Schema, Layer und numerische Parameter validieren. |
| 7 | `rectangles.extrude` | Geometrie/Extrusion | Selector oder Layer plus positive Hoehe pruefen. |
| 8 | `undo.last` | Undo | Schritte begrenzen; numerisches `UNDO <steps>` verwenden. |
| 9 | `bim.classify` | BIM | Ziel-Solids und erlaubte Klassifikation pruefen. |
| 10 | `profile.extrude` | Geometrie/Extrusion | Geschlossene Profile, Selector und Hoehe pruefen. |
| 11 | `geometry.move` | Transformation | Selector und Vektor beziehungsweise Punktpaar pruefen. |
| 12 | `geometry.copy` | Transformation | Selector, Offset und Kopienanzahl pruefen. |
| 13 | `geometry.rotate` | Transformation | Selector, Winkel und Rotationsbasis pruefen. |
| 14 | `geometry.scale` | Transformation | Selector und uniformen Faktor pruefen. |
| 15 | `geometry.delete` | Loeschen | Selector pruefen; `confirm=true` erzwingen. |
| 16 | `selection.set` | Auswahl | Selector/Handles auf vorhandene Entities pruefen. |
| 17 | `entity.setLayer` | Entity/Layer | Selector/Handles und Ziellayer pruefen. |
| 18 | `entity.setName` | Entity | Selector und Zielname pruefen. |
| 19 | `layers.create` | Layer | Layername und optionale Farbe pruefen. |
| 20 | `layers.rename` | Layer | Alten und neuen Namen pruefen. |
| 21 | `layers.setColor` | Layer | Layerexistenz und ACI-Farbe pruefen. |
| 22 | `layers.batch` | Layer | Jede enthaltene Layer-Aktion pruefen. |
| 23 | `command.execute` | Native Command | Genau eine bekannte Kommandozeile ohne Semikolon oder Zeilenumbruch pruefen. |
| 24 | `document.save` | Dokument | Zeichnung explizit speichern. |
| 25 | `undo.redo` | Undo | Redo-Schritte pruefen. |

## Qt-virtuelle Agent-Tools (1)

Diese Tools existieren nicht als BRX-Methode. Qt bietet sie dem Agenten zusaetzlich zu allen BRX-Methoden an und expandiert sie vor Preflight/Ausfuehrung in echte BRX-Aktionen.

| # | Tool | Expandiert zu | Zweck |
|---:|---|---|---|
| 1 | `layers.ensureMany` | `layers.create[]` | Kompakte Layer-Batches mit `params.layers:[{name,colorIndex}]`; reduziert JSON-Laenge und vermeidet unnoetige AI-Retry-Schleifen. |

## Read-only Methoden (13)

Diese Methoden veraendern die Zeichnung nicht und dienen als Kontext, Diagnose oder Preflight-Unterstuetzung. Sie sind ebenfalls freigegebene Agent-Tools und koennen im BricsCAD-Modus als Kontext-/Diagnoseaktionen genutzt werden:

| # | Methode | Zweck |
|---:|---|---|
| 1 | `capabilities.list` | Geladene Methoden, Commands und Schemas lesen. |
| 2 | `actions.list` | API-Dokumentation fuer den Agenten lesen. |
| 3 | `actions.validate` | Vorschlag trocken validieren. |
| 4 | `pipes.validateNetwork` | Rohrnetz und Grundrissbezug validieren. |
| 5 | `commands.list` | Native BricsCAD-Commands lesen. |
| 6 | `layers.list` | Layerzustand lesen. |
| 7 | `geometry.query` | Zeichnungsgeometrie lesen. |
| 8 | `selection.describe` | Aktuelle Auswahl lesen. |
| 9 | `entity.describe` | Entities per Handle oder exaktem BIM-Namen inklusive BIM-Metadaten lesen. |
| 10 | `bim.objects.query` | Klassifizierte BIM-Objekte und optional typisierte Properties lesen. |
| 11 | `measurement.bbox` | Bounding-Boxes und Dimensionen berechnen. |
| 12 | `measurement.length` | Laengen berechnen. |
| 13 | `measurement.area` | Flaechen berechnen. |

## Commands (22)

Diese Commands sind native BricsCAD-Kommandos aus `commands.list`. Sie sind als Kontext und Low-Level-Referenz geladen. Der Agent kann die passende Bridge-Methode oder `command.execute` verwenden; `command.execute` wird vor der Nutzerbestaetigung strikt validiert.

| # | Command | Bridge-Tool / Native-Status |
|---:|---|---|
| 1 | `RECTANGLE` | Bridge-Tool `geometry.create` oder validiertes `command.execute`. |
| 2 | `EXTRUDE` | Bridge-Tools `rectangles.extrude`/`profile.extrude` oder validiertes `command.execute`. |
| 3 | `MOVE` | Bridge-Tool `geometry.move` oder validiertes `command.execute`. |
| 4 | `COPY` | Bridge-Tool `geometry.copy` oder validiertes `command.execute`. |
| 5 | `ROTATE` | Bridge-Tool `geometry.rotate` oder validiertes `command.execute`. |
| 6 | `SCALE` | Bridge-Tool `geometry.scale` oder validiertes `command.execute`; Bridge-Tool nur uniforme Skalierung. |
| 7 | `ERASE` | Bridge-Tool `geometry.delete` oder validiertes `command.execute`. |
| 8 | `LAYER` | Bridge-Tools `layers.create`, `layers.rename`, `layers.setColor`, `entity.setLayer`, virtuelles Qt-Tool `layers.ensureMany` oder validiertes `command.execute`; Batch erfolgt in Qt ueber `proposal.actions[]`. |
| 9 | `SAVE` | Bridge-Tool `document.save` oder validiertes `command.execute`. |
| 10 | `UNDO` | Bridge-Tool `undo.last` oder validiertes `command.execute`. |
| 11 | `REDO` | Bridge-Tool `undo.redo` oder validiertes `command.execute`. |
| 12 | `BIMCLASSIFY` | Bridge-Tool `bim.classify` oder validiertes `command.execute`. |
| 13 | `UNION` | `contextOnly`; noch kein stabiles Action-Tool. |
| 14 | `SUBTRACT` | `contextOnly`; noch kein stabiles Action-Tool. |
| 15 | `INTERSECT` | `contextOnly`; noch kein stabiles Action-Tool. |
| 16 | `SWEEP` | `contextOnly`; noch kein stabiles Action-Tool. |
| 17 | `LOFT` | `contextOnly`; noch kein stabiles Action-Tool. |
| 18 | `OFFSET` | `contextOnly`; noch kein stabiles Action-Tool. |
| 19 | `TRIM` | `contextOnly`; noch kein stabiles Action-Tool. |
| 20 | `EXTEND` | `contextOnly`; noch kein stabiles Action-Tool. |
| 21 | `FILLET` | `contextOnly`; noch kein stabiles Action-Tool. |
| 22 | `CHAMFER` | `contextOnly`; noch kein stabiles Action-Tool. |

## BRX-V26-BIM-Voraussetzungen

- Gepinnte API-Basis: BRX SDK `26.1.05.0`, x64 und Windows SDK `10.0.19041.0` oder neuer.
- Der CMake-Guard verlangt das Visual-Studio-2022-v143-Toolset (`MSVC_VERSION 1930..1949`) und lehnt aeltere sowie neuere Toolset-Generationen ab.
- Gelinkt wird `brx26.lib`. `Ice.lib` ist nicht erforderlich, weil dieses Grundgeruest keinen IFC-Import/-Export implementiert.
- `capabilities.list` prueft jede strukturierte BIM-Methode mit `BimApi::isBimAvailable()` und `isLicenseAvailable(BricsCAD::eBim)` und liefert einen strukturierten Availability-Grund.
- Klassifikationen stammen zur Laufzeit aus `BimClassification::getBimTypeNames`; deprecated BIM-Enums werden fuer die neuen Tools nicht verwendet.
- `actions.validate` liefert kanonische `resolvedHandles` und Fingerprints aus Handle, GUID und Klassifikation. Bei `autoBimHandlesFromLastQuery` materialisiert BRX exakt die vorherige Query-Seite unter Beachtung von `offset` und `limit`. Qt zeigt diese Ziele vor der Bestaetigung und sendet sie fuer die erneute Aufloesung an die Ausfuehrung.
- Millimeter werden ueber `database->insunits()` und `acdbGetUnitsConversion` in Zeichnungseinheiten konvertiert. Undefinierte `INSUNITS` werden fuer `units=mm` abgelehnt; `units=drawing` ist explizit moeglich.
- XRef-Ziele, gesperrte Layer, geloeschte oder unklassifizierte Ziele sowie geankerte Elemente und Hosts mit geankerten Elementen werden im gemeinsamen Preflight abgelehnt.

## Aktueller Sicherheitsfluss

1. Der Nutzer schreibt einen Prompt im BricsCAD Modus.
2. Die AI erzeugt einen strukturierten Vorschlag als Barebone-Agent-JSON v2, bevorzugt mit `schema="barebone.agent.response.v2"` und `proposal.actions[]`.
3. Qt prueft lokal, ob Toolname, JSON-Struktur und Basisschema plausibel sind.
4. Qt sendet denselben Vorschlag mit stabilen `clientActionIndex`-Werten an BRX `actions.validate`.
5. BRX prueft Syntax, Pflichtdaten, Layer, Handles, Selector-Ziele und zeichnungsabhaengige Bedingungen. Abhaengige BIM-Abfragen werden paginiert zu exakten Handles aufgeloest; fuer die nachfolgende BIM-Mutation liefert BRX `resolvedHandles` und `targetFingerprints`.
6. Bei Fehlern wird der Vorschlag nicht zur Bestaetigung angezeigt. Qt schickt die Fehler zur Nachbesserung zur AI oder fragt den Nutzer nach fehlenden Daten.
7. Gueltige mutierende Vorschlaege werden als bestaetigungspflichtige Aktion angezeigt; Qt zeigt BIM-Zielhandles und Fingerprints mit an. Reine read-only Vorschlaege werden bei Daten-/Tabellenfragen automatisch ausgefuehrt.
8. Nach Nutzerbestaetigung beziehungsweise bei read-only automatisch wird seriell ausgefuehrt. BIM-Ziele werden dabei aus den konkreten Query-Handles beziehungsweise dem bestaetigten Selector erneut aufgeloest und gegen die Pflichtartefakte `resolvedHandles`/`targetFingerprints` verglichen. Bei internen Qt-Batches wird jede mutierende Aktion einzeln per `actions.validate` geprueft. `layers.ensureMany` wird vorher in einzelne `layers.create`-Aktionen expandiert.
9. Aktionen mit nativen BricsCAD-Commands laufen im BRX Command Context. Falls ein Native-Command-Pfad versehentlich im Application Context landet, wird er blockiert statt `acedCommand` aufzurufen.
10. `command.execute` ist fuer die AI freigegeben, aber nur fuer bekannte Commands aus `commands.list` und nur als einzelne vollstaendige Kommandozeile ohne Semikolon oder Zeilenumbruch.

## Hinweise fuer Erweiterungen

- Neue stabile API-Funktionen gehoeren in `kBridgeMethods`.
- Nur Methoden mit `kind=action` werden Agent-Action-Tools.
- Qt-virtuelle Tools gehoeren in `BricsCadPage::availableAgentTools()` und muessen vor `actions.validate` deterministisch in echte BRX-Aktionen expandieren.
- Read-only Kontextfunktionen sollten `kind=query` und `risk=readOnly` bleiben.
- Native Commands in `kBridgeCommands` sind Referenz und Whitelist fuer `command.execute`; die AI darf zwischen Bridge-Tool und validiertem nativen Command waehlen.
- Jede neue Action braucht eine passende Validierung in `actions.validate`, bevor sie dem Agenten gegeben werden sollte.
- Neue Schreibfunktionen duerfen der AI nur angeboten werden, wenn sie als explizites BRX-Tool mit Schema, `actions.validate`-Preflight und Fehlerpfad implementiert sind; die AI darf weiterhin keine freien AcDb-/LayerTable-/EntityTable-Mutationen vorschlagen.
