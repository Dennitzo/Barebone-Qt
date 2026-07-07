# BRX Capabilities Uebersicht

Stand: 2026-06-24

Quelle:
- `src/bricscad/BareboneBrxPlugin.cpp`: `kBridgeMethods`, `kBridgeCommands`
- `src/ui/BricsCadPage.cpp`: `BricsCadPage::availableAgentTools()`

Aktuelle Qt-Log-Zeile:

```text
BRX-Capabilities geladen: 30 Methoden, 22 Commands, 31 Agent-Tools
```

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

## Methoden (30)

| # | Methode | Art | Risiko | Modul/Zweck |
|---:|---|---|---|---|
| 1 | `capabilities.list` | query | readOnly | Vollstaendige BRX-Capability-Liste laden. |
| 2 | `actions.list` | query | readOnly | Dokumentierte API-Aktionen mit Schemas, Pflichtfeldern und Beispielen laden. |
| 3 | `actions.validate` | query | readOnly | Agent-Vorschlaege trocken gegen Syntax, Pflichtdaten und Zeichnungszustand pruefen. |
| 4 | `commands.list` | query | readOnly | Native BricsCAD-Commands als Kontextliste laden. |
| 5 | `layers.list` | query | readOnly | Layer der aktiven Zeichnung lesen. |
| 6 | `geometry.query` | query | readOnly | Geometrien ueber Selector und Filter abfragen; liefert `bounds` und `dimensions.widthX/depthY/heightZ` in mm, wenn Extents verfuegbar sind. |
| 7 | `selection.describe` | query | readOnly | Aktuelle BricsCAD-Auswahl beschreiben. |
| 8 | `entity.describe` | query | readOnly | Konkrete Entities per Handle beschreiben. |
| 9 | `geometry.create` | action | modifiesDrawing | Grundgeometrie erzeugen: Punkt, Linie, Rechteck, Polyline, Kreis, Bogen, Box. |
| 10 | `rectangles.extrude` | action | modifiesDrawing | Geschlossene Rechteck-Polylinien extrudieren. |
| 11 | `undo.last` | action | modifiesDrawing | Letzte Aktionen rueckgaengig machen. |
| 12 | `bim.classify` | action | modifiesDrawing | 3D-Solids als BIM-Element klassifizieren. |
| 13 | `profile.extrude` | action | modifiesDrawing | Geschlossene 2D-Profile extrudieren. |
| 14 | `geometry.move` | action | modifiesDrawing | Entities ueber Selector verschieben. |
| 15 | `geometry.copy` | action | modifiesDrawing | Entities ueber Selector kopieren. |
| 16 | `geometry.rotate` | action | modifiesDrawing | Entities ueber Selector rotieren. |
| 17 | `geometry.scale` | action | modifiesDrawing | Entities ueber Selector uniform skalieren. |
| 18 | `geometry.delete` | action | modifiesDrawing | Entities ueber Selector loeschen; `confirm=true` ist Pflicht. |
| 19 | `selection.set` | action | modifiesEditorState | Aktuelle Auswahl in BricsCAD setzen. |
| 20 | `entity.setLayer` | action | modifiesDrawing | Vorhandene Entities per Selector einem Ziel-Layer zuweisen. |
| 21 | `layers.create` | action | modifiesDrawing | Layer ueber BricsCADs nativen `-LAYER`-Command im Command Context anlegen, optional mit ACI-Farbe. |
| 22 | `layers.rename` | action | modifiesDrawing | Vorhandenen Layer ueber BricsCADs nativen `-LAYER`-Command im Command Context umbenennen. |
| 23 | `layers.setColor` | action | modifiesDrawing | ACI-Farbe eines Layers ueber BricsCADs nativen `-LAYER`-Command im Command Context setzen. |
| 24 | `layers.batch` | action | modifiesDrawing | BRX-Layer-Batch-Methode fuer create/rename/setColor; native Ausfuehrung nur im Command Context. |
| 25 | `command.execute` | action | modifiesDrawing | Validierte einzelne native BricsCAD-Kommandozeile aus `commands.list` posten. |
| 26 | `document.save` | action | modifiesDocument | Aktive Zeichnung explizit speichern. |
| 27 | `measurement.bbox` | query | readOnly | Bounding-Boxes und daraus abgeleitete Dimensionen fuer Entities berechnen. |
| 28 | `measurement.length` | query | readOnly | Kurvenlaengen fuer Entities berechnen. |
| 29 | `measurement.area` | query | readOnly | Flaechen geschlossener Kurven und Kreise berechnen. |
| 30 | `undo.redo` | action | modifiesDrawing | Redo ausfuehren, sofern BricsCAD es erlaubt. |

## BRX-Action-Methoden (19, alle direkte AI-Tools)

Diese 19 Methoden sind im BRX als `kind=action` vorhanden und werden dem AI-Agenten direkt angeboten. Zusaetzlich bietet Qt das virtuelle Agent-Tool `layers.ensureMany` an. Read-only BRX-Methoden wie `capabilities.list`, `actions.list`, `actions.validate`, `geometry.query` und Messfunktionen sind ebenfalls freigegebene Agent-Tools fuer Kontext, Diagnose und Try-before-fail. Read-only Tools duerfen bei Tabellen-/Datenfragen automatisch laufen; mutierende AI-Tools laufen vor der Anzeige zur Nutzerbestaetigung durch lokale Qt-Validierung und BRX-Preflight via `actions.validate`.

| # | BRX-Action | Kategorie | Sicherheitslogik |
|---:|---|---|---|
| 1 | `geometry.create` | Geometrie | Schema pruefen, Layer und numerische Parameter validieren. |
| 2 | `rectangles.extrude` | Geometrie/Extrusion | Selector oder Layer plus positive Hoehe pruefen. |
| 3 | `undo.last` | Undo | Schritte begrenzen und Zeichnungszustand schuetzen; BRX nutzt numerisches `UNDO <steps>` statt Back/All, damit keine Yes/No-Rueckfrage haengen bleibt. |
| 4 | `bim.classify` | BIM | Ziel-Solids und erlaubte Klassifikation pruefen. |
| 5 | `profile.extrude` | Geometrie/Extrusion | Geschlossene Profile, Selector und Hoehe pruefen. |
| 6 | `geometry.move` | Transformation | Selector und Vektor bzw. Punktpaar pruefen. |
| 7 | `geometry.copy` | Transformation | Selector, Offset/Spacing und Kopienanzahl pruefen. |
| 8 | `geometry.rotate` | Transformation | Selector, Winkel und Rotationsbasis pruefen. |
| 9 | `geometry.scale` | Transformation | Selector und uniformen Faktor pruefen. |
| 10 | `geometry.delete` | Loeschen | Selector pruefen; `confirm=true` erzwingen. |
| 11 | `selection.set` | Auswahl | Selector/Handles auf vorhandene Entities pruefen. |
| 12 | `entity.setLayer` | Entity/Layer | Selector/Handles und Ziellayer pruefen; optional fehlenden Ziellayer anlegen. |
| 13 | `layers.create` | Layer | Layername und optionale Farbe pruefen; Ausfuehrung ueber nativen `-LAYER`-Command im Command Context. |
| 14 | `layers.rename` | Layer | Existenz des alten und Gueltigkeit des neuen Namens pruefen; Ausfuehrung ueber nativen `-LAYER`-Command im Command Context. |
| 15 | `layers.setColor` | Layer | Layerexistenz und ACI-Farbe pruefen; Ausfuehrung ueber nativen `-LAYER`-Command im Command Context. |
| 16 | `layers.batch` | Layer | Layer-Batch fuer create/rename/setColor; BRX validiert die einzelnen Batch-Aktionen. |
| 17 | `command.execute` | Native Command | Einzelne native Kommandozeile aus `commands.list` pruefen; keine Mehrfachbefehle, Semikolons oder Zeilenumbrueche. |
| 18 | `document.save` | Dokument | Zeichnung explizit speichern. |
| 19 | `undo.redo` | Undo | Redo-Schritte pruefen. |

## Qt-virtuelle Agent-Tools (1)

Diese Tools existieren nicht als BRX-Methode. Qt bietet sie dem Agenten zusaetzlich zu allen BRX-Methoden an und expandiert sie vor Preflight/Ausfuehrung in echte BRX-Aktionen.

| # | Tool | Expandiert zu | Zweck |
|---:|---|---|---|
| 1 | `layers.ensureMany` | `layers.create[]` | Kompakte Layer-Batches mit `params.layers:[{name,colorIndex}]`; reduziert JSON-Laenge und vermeidet unnoetige AI-Retry-Schleifen. |

## Read-only Methoden (11)

Diese Methoden veraendern die Zeichnung nicht und dienen als Kontext, Diagnose oder Preflight-Unterstuetzung. Sie sind ebenfalls freigegebene Agent-Tools und koennen im BricsCAD-Modus als Kontext-/Diagnoseaktionen genutzt werden:

| # | Methode | Zweck |
|---:|---|---|
| 1 | `capabilities.list` | Geladene Methoden, Commands und Schemas lesen. |
| 2 | `actions.list` | API-Dokumentation fuer den Agenten lesen. |
| 3 | `actions.validate` | Vorschlag trocken validieren. |
| 4 | `commands.list` | Native BricsCAD-Commands lesen. |
| 5 | `layers.list` | Layerzustand lesen. |
| 6 | `geometry.query` | Zeichnungsgeometrie lesen; fuer alle Objekte `selector.scope=currentSpace` ohne Layerfilter nutzen. |
| 7 | `selection.describe` | Aktuelle Auswahl lesen. |
| 8 | `entity.describe` | Entities per Handle lesen. |
| 9 | `measurement.bbox` | Bounding-Boxes und `dimensions.widthX/depthY/heightZ` berechnen. |
| 10 | `measurement.length` | Laengen berechnen. |
| 11 | `measurement.area` | Flaechen berechnen. |

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

## Aktueller Sicherheitsfluss

1. Der Nutzer schreibt einen Prompt im BricsCAD Modus.
2. Die AI erzeugt einen strukturierten Vorschlag als Barebone-Agent-JSON v2, bevorzugt mit `schema="barebone.agent.response.v2"` und `proposal.actions[]`.
3. Qt prueft lokal, ob Toolname, JSON-Struktur und Basisschema plausibel sind.
4. Qt sendet denselben Vorschlag an BRX `actions.validate`.
5. BRX prueft Syntax, Pflichtdaten, Layer, Handles, Selector-Ziele und zeichnungsabhaengige Bedingungen.
6. Bei Fehlern wird der Vorschlag nicht zur Bestaetigung angezeigt. Qt schickt die Fehler zur Nachbesserung zur AI oder fragt den Nutzer nach fehlenden Daten.
7. Gueltige mutierende Vorschlaege werden als bestaetigungspflichtige Aktion angezeigt. Reine read-only Vorschlaege werden bei Daten-/Tabellenfragen automatisch ausgefuehrt.
8. Nach Nutzerbestaetigung bzw. bei read-only automatisch wird seriell ausgefuehrt. Bei internen Qt-Batches wird jede mutierende Aktion einzeln per `actions.validate` geprueft, einzeln an BRX gesendet, und erst nach erfolgreicher BRX-Antwort fortgesetzt. `layers.ensureMany` wird vorher in einzelne `layers.create`-Aktionen expandiert.
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
