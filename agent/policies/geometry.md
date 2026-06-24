Geometrie-Policy.

Neue Grundgeometrien wie Kreis, Rechteck, Linie, Polyline, Punkt, Bogen oder Box werden mit geometry.create erstellt, wenn dieses Tool vorhanden ist.

Fuer geometry.create rectangle ist die zweite 2D-Abmessung als depth oder length zu senden, nicht als height. Angaben wie y, b, Tiefe oder Rechteck-Hoehe bedeuten depth/length.

Eine Wand mit Laenge, Wandstaerke und Hoehe kann als geometry.create geometry=box erstellt werden: Laenge=width, Wandstaerke=depth, Hoehe=height. Danach optional bim.classify mit target=lastExtruded, wenn vorhanden.

Eine vorhandene ausgewaehlte 3D-Solid-Geometrie kann als BIM-Wand klassifiziert werden, wenn bim.classify in tools vorhanden ist. Bei aktueller Auswahl verwende params {"classification":"BIMWall","selector":{"scope":"selection","kind":"solid"}}.

Nutze geometry.scale nur fuer uniforme Skalierung mit factor. Nutze geometry.scale niemals fuer Verlaengern, Strecken in X/Y/Z oder einseitige Laengenaenderungen.

Face-/Subentity-Bearbeitung ist nur erlaubt, wenn ein ausdrueckliches Tool dafuer in tools vorhanden ist. geometry.move verschiebt ganze Entities und darf nicht fuer einzelne Solid-Faces genutzt werden.

Wenn der Nutzer Ursprung sagt, verwende {"x":0,"y":0,"z":0}. Einheiten sind mm.

Bei Extrusionen bedeuten z, hoehe, height, heightMm oder eine alleinstehende mm-Angabe params.heightMm.
