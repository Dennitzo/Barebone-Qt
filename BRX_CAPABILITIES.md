# BRX Capabilities Uebersicht

Stand: 2026-06-24

Quelle:
- `src/bricscad/BareboneBrxPlugin.cpp`: `kBridgeMethods`, `kBridgeCommands`
- `src/ui/BricsCadPage.cpp`: `BricsCadPage::availableAgentTools()`

Aktuelle Qt-Log-Zeile:

```text
BRX-Capabilities geladen: 29 Methoden, 22 Commands, 17 Action-Tools
```

## Begriffe

- Methoden: alle Bridge-Methoden, die das BRX-Plugin ueber `capabilities.list` meldet.
- BRX-Action-Methoden: Methoden mit `kind=action` in `capabilities.list`.
- Action-Tools: BRX-Action-Methoden, die Barebone-Qt dem AI-Agenten als ausfuehrbare Tools gibt. `layers.batch` ist BRX-intern vorhanden, aber fuer die AI ausgeblendet.
- Qt-virtuelle Agent-Tools: zusaetzliche Tools, die nur Barebone-Qt dem Agenten anbietet und vor BRX-Preflight/Ausfuehrung in echte BRX-Aktionen expandiert. Aktuell: `layers.ensureMany`.
- Commands: native BricsCAD-Kommandos als Low-Level-Referenz. Die AI kann je nach Aufgabe zwischen spezialisierten Bridge-Tools und validiertem `command.execute` entscheiden.
- Preflight: Vor der Nutzerbestaetigung prueft Qt jeden Vorschlag ueber `actions.validate` im BRX-Plugin.
- Hinweis: `layers.batch` bleibt eine BRX-Methode, wird dem AI-Agenten aber nicht mehr direkt als Tool angeboten. Die AI soll Barebone-Agent-JSON v2 mit `proposal.actions[]` oder fuer mehrere Layer `layers.ensureMany` liefern; Qt fuehrt diese interne Batch einzeln gegen BRX aus.
- Direkte BricsCAD-DB-Schreibvorgaenge sind fuer die AI verboten. Vorschlaege duerfen nur die von Qt angebotenen `tools[].name` verwenden; keine AcDb-/LayerTable-/EntityTable-Mutationen und keine Pseudo-Tools fuer Datenbankwrites.
- Native Commands mit `acedCommand` duerfen im BRX nur im BricsCAD Command Context laufen. Barebone-Qt/BRX plant Layer, Extrude, BIMClassify, Undo und Redo deshalb ueber `beginExecuteInCommandContext`; ein Guard blockiert versehentliche Native-Command-Ausfuehrung im Application Context.

## Methoden (29)

| # | Methode | Art | Risiko | Modul/Zweck |
|---:|---|---|---|---|
| 1 | `capabilities.list` | query | readOnly | Vollstaendige BRX-Capability-Liste laden. |
| 2 | `actions.list` | query | readOnly | Dokumentierte API-Aktionen mit Schemas, Pflichtfeldern und Beispielen laden. |
| 3 | `actions.validate` | query | readOnly | Agent-Vorschlaege trocken gegen Syntax, Pflichtdaten und Zeichnungszustand pruefen. |
| 4 | `commands.list` | query | readOnly | Native BricsCAD-Commands als Kontextliste laden. |
| 5 | `layers.list` | query | readOnly | Layer der aktiven Zeichnung lesen. |
| 6 | `geometry.query` | query | readOnly | Geometrien ueber Selector und Filter abfragen. |
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
| 20 | `layers.create` | action | modifiesDrawing | Layer ueber BricsCADs nativen `-LAYER`-Command im Command Context anlegen, optional mit ACI-Farbe. |
| 21 | `layers.rename` | action | modifiesDrawing | Vorhandenen Layer ueber BricsCADs nativen `-LAYER`-Command im Command Context umbenennen. |
| 22 | `layers.setColor` | action | modifiesDrawing | ACI-Farbe eines Layers ueber BricsCADs nativen `-LAYER`-Command im Command Context setzen. |
| 23 | `layers.batch` | action | modifiesDrawing | BRX-interne Layer-Batch-Methode; fuer AI-Batches nicht direkt angeboten; native Ausfuehrung nur im Command Context. |
| 24 | `command.execute` | action | modifiesDrawing | Validierte einzelne native BricsCAD-Kommandozeile aus `commands.list` posten. |
| 25 | `document.save` | action | modifiesDocument | Aktive Zeichnung explizit speichern. |
| 26 | `measurement.bbox` | query | readOnly | Bounding-Boxes fuer Entities berechnen. |
| 27 | `measurement.length` | query | readOnly | Kurvenlaengen fuer Entities berechnen. |
| 28 | `measurement.area` | query | readOnly | Flaechen geschlossener Kurven und Kreise berechnen. |
| 29 | `undo.redo` | action | modifiesDrawing | Redo ausfuehren, sofern BricsCAD es erlaubt. |

## BRX-Action-Methoden (18, davon 17 direkte AI-Tools)

Diese 18 Methoden sind im BRX als `kind=action` vorhanden. Dem AI-Agenten werden aktuell 17 davon direkt angeboten; `layers.batch` bleibt intern und wird durch Qt-Batches mit `proposal.actions[]` ersetzt. Zusaetzlich bietet Qt ein virtuelles Agent-Tool `layers.ensureMany` an, sodass der Agent insgesamt 18 ausfuehrbare Tools sehen kann. Alle AI-Tools laufen vor der Anzeige zur Nutzerbestaetigung durch lokale Qt-Validierung und BRX-Preflight via `actions.validate`.

| # | BRX-Action | Kategorie | Sicherheitslogik |
|---:|---|---|---|
| 1 | `geometry.create` | Geometrie | Schema pruefen, Layer und numerische Parameter validieren. |
| 2 | `rectangles.extrude` | Geometrie/Extrusion | Selector oder Layer plus positive Hoehe pruefen. |
| 3 | `undo.last` | Undo | Schritte begrenzen und Zeichnungszustand schuetzen. |
| 4 | `bim.classify` | BIM | Ziel-Solids und erlaubte Klassifikation pruefen. |
| 5 | `profile.extrude` | Geometrie/Extrusion | Geschlossene Profile, Selector und Hoehe pruefen. |
| 6 | `geometry.move` | Transformation | Selector und Vektor bzw. Punktpaar pruefen. |
| 7 | `geometry.copy` | Transformation | Selector, Offset/Spacing und Kopienanzahl pruefen. |
| 8 | `geometry.rotate` | Transformation | Selector, Winkel und Rotationsbasis pruefen. |
| 9 | `geometry.scale` | Transformation | Selector und uniformen Faktor pruefen. |
| 10 | `geometry.delete` | Loeschen | Selector pruefen; `confirm=true` erzwingen. |
| 11 | `selection.set` | Auswahl | Selector/Handles auf vorhandene Entities pruefen. |
| 12 | `layers.create` | Layer | Layername und optionale Farbe pruefen; Ausfuehrung ueber nativen `-LAYER`-Command im Command Context. |
| 13 | `layers.rename` | Layer | Existenz des alten und Gueltigkeit des neuen Namens pruefen; Ausfuehrung ueber nativen `-LAYER`-Command im Command Context. |
| 14 | `layers.setColor` | Layer | Layerexistenz und ACI-Farbe pruefen; Ausfuehrung ueber nativen `-LAYER`-Command im Command Context. |
| 15 | `layers.batch` | Layer | BRX-intern verfuegbar, aber fuer AI ausgeblendet; Qt-Batches laufen ueber `actions[]` und einzelne BRX-Requests. |
| 16 | `command.execute` | Native Command | Einzelne native Kommandozeile aus `commands.list` pruefen; keine Mehrfachbefehle, Semikolons oder Zeilenumbrueche. |
| 17 | `document.save` | Dokument | Zeichnung explizit speichern. |
| 18 | `undo.redo` | Undo | Redo-Schritte pruefen. |

## Qt-virtuelle Agent-Tools (1)

Diese Tools existieren nicht als BRX-Methode. Qt bietet sie dem Agenten an und expandiert sie vor Preflight/Ausfuehrung in echte BRX-Aktionen.

| # | Tool | Expandiert zu | Zweck |
|---:|---|---|---|
| 1 | `layers.ensureMany` | `layers.create[]` | Kompakte Layer-Batches mit `params.layers:[{name,colorIndex}]`; reduziert JSON-Laenge und vermeidet unnoetige AI-Retry-Schleifen. |

## Read-only Methoden (11)

Diese Methoden veraendern die Zeichnung nicht und dienen als Kontext, Diagnose oder Preflight-Unterstuetzung:

| # | Methode | Zweck |
|---:|---|---|
| 1 | `capabilities.list` | Geladene Methoden, Commands und Schemas lesen. |
| 2 | `actions.list` | API-Dokumentation fuer den Agenten lesen. |
| 3 | `actions.validate` | Vorschlag trocken validieren. |
| 4 | `commands.list` | Native BricsCAD-Commands lesen. |
| 5 | `layers.list` | Layerzustand lesen. |
| 6 | `geometry.query` | Zeichnungsgeometrie lesen. |
| 7 | `selection.describe` | Aktuelle Auswahl lesen. |
| 8 | `entity.describe` | Entities per Handle lesen. |
| 9 | `measurement.bbox` | Bounding-Boxes berechnen. |
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
| 8 | `LAYER` | Bridge-Tools `layers.create`, `layers.rename`, `layers.setColor`, virtuelles Qt-Tool `layers.ensureMany` oder validiertes `command.execute`; Batch erfolgt in Qt ueber `proposal.actions[]`. |
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
7. Nur gueltige Vorschlaege werden als bestaetigungspflichtige Aktion angezeigt.
8. Nach Nutzerbestaetigung wird seriell ausgefuehrt. Bei internen Qt-Batches wird jede Aktion einzeln per `actions.validate` geprueft, einzeln an BRX gesendet, und erst nach erfolgreicher BRX-Antwort fortgesetzt. `layers.ensureMany` wird vorher in einzelne `layers.create`-Aktionen expandiert.
9. Aktionen mit nativen BricsCAD-Commands laufen im BRX Command Context. Falls ein Native-Command-Pfad versehentlich im Application Context landet, wird er blockiert statt `acedCommand` aufzurufen.
10. `command.execute` ist fuer die AI freigegeben, aber nur fuer bekannte Commands aus `commands.list` und nur als einzelne vollstaendige Kommandozeile ohne Semikolon oder Zeilenumbruch.

## Hinweise fuer Erweiterungen

- Neue stabile API-Funktionen gehoeren in `kBridgeMethods`.
- Nur Methoden mit `kind=action` werden Agent-Action-Tools.
- Qt-virtuelle Tools gehoeren in `BricsCadPage::availableAgentTools()` und muessen vor `actions.validate` deterministisch in echte BRX-Aktionen expandieren.
- Read-only Kontextfunktionen sollten `kind=query` und `risk=readOnly` bleiben.
- Native Commands in `kBridgeCommands` sind Referenz und Whitelist fuer `command.execute`; die AI darf zwischen Bridge-Tool und validiertem nativen Command waehlen.
- Jede neue Action braucht eine passende Validierung in `actions.validate`, bevor sie dem Agenten gegeben werden sollte.
- Neue Schreibfunktionen duerfen der AI nur angeboten werden, wenn sie ohne direkte BricsCAD-DB-Schreibmutation stabil laufen oder intern einen stabilen nativen BricsCAD-Command-Pfad im Command Context nutzen.
