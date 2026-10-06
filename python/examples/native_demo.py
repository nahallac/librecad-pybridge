#!/usr/bin/env python3
"""Real DIMENSION and HATCH entities, via the bridge's native operations.

Needs a live LibreCAD bridge session (not the stub): the entities are created
by LibreCAD's own dimension and hatch actions, driven through its command
line. Draws a plate with a hatched square hole, dimensioned.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from lcbridge import BridgeError, Document


def main() -> int:
    with Document.connect() as doc:
        status = doc.native_status()
        if not (status["commands"] and status["selection"]):
            print(f"native access unavailable: {status['reason']}", file=sys.stderr)
            return 1

        # Dimension text sized for a drawing in millimetres.
        doc.set_variable("$DIMTXT", 60.0)
        doc.set_variable("$DIMASZ", 60.0)   # arrow size
        doc.set_variable("$DIMEXO", 15.0)   # extension line offset
        doc.set_variable("$DIMEXE", 30.0)   # extension line extension

        with doc.layer("PLATE"):
            doc.add_polyline([(0, 0), (1200, 0), (1200, 800), (0, 800)],
                             closed=True)
        with doc.layer("HOLE"):
            doc.add_polyline([(450, 250), (750, 250), (750, 550), (450, 550)],
                             closed=True)

        # Real hatch over the hole boundary.
        holes = [e for e in doc.entities(types=["POLYLINE"])
                 if e["layer"] == "HOLE"]
        with doc.layer("HATCH"):
            doc.cad_hatch(holes, pattern="ANSI31", scale=10.0)

        # Real dimensions: these are DIMENSION entities, editable in LibreCAD.
        with doc.layer("DIMS"):
            doc.cad_dim_horizontal((0, 0), (1200, 0), (600, -150))
            doc.cad_dim_vertical((1200, 0), (1200, 800), (1380, 400))
            doc.cad_dim_aligned((450, 250), (750, 550), (380, 480))

        doc.release()
        doc.update_view()

        census: dict[str, int] = {}
        for entity in doc.entities():
            census[entity.type] = census.get(entity.type, 0) + 1
        doc.release()
        print("census:", ", ".join(f"{count} {name}"
                                   for name, count in sorted(census.items())))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BridgeError as error:
        print(f"bridge error: {error}", file=sys.stderr)
        sys.exit(1)
