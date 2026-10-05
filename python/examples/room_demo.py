#!/usr/bin/env python3
"""Draw a small furnished room through the bridge.

Start a bridge session in LibreCAD (Plugins -> Python Bridge: Start bridge
session), then:

    python3 python/examples/room_demo.py

Everything it draws is one undo step. Units are whatever the drawing uses;
the shape is a 4000 x 3000 room with a door and a window, which reads
naturally in millimetres.
"""

from __future__ import annotations

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from lcbridge import Document

W, H = 4000.0, 3000.0          # room, inner face
T = 100.0                      # wall thickness
DOOR = 900.0                   # door leaf
WIN = 1200.0                   # window width


def main() -> int:
    with Document.connect() as doc:
        with doc.layer("WALLS"):
            doc.set_layer_properties(color=-1)
            with doc.batch():
                # Inner and outer wall faces as closed polylines.
                doc.add_polyline([(0, 0), (W, 0), (W, H), (0, H)], closed=True)
                doc.add_polyline([(-T, -T), (W + T, -T), (W + T, H + T),
                                  (-T, H + T)], closed=True)

        with doc.layer("OPENINGS"):
            with doc.batch():
                # Door opening in the south wall, hinged at x=600.
                door_x = 600.0
                doc.add_line((door_x, 0), (door_x, -T))
                doc.add_line((door_x + DOOR, 0), (door_x + DOOR, -T))
                # Leaf and swing arc.
                doc.add_line((door_x, 0), (door_x, DOOR))
                doc.add_arc((door_x, 0), DOOR, 0.0, math.pi / 2)

                # Window in the north wall, centred.
                win_x = (W - WIN) / 2
                doc.add_line((win_x, H), (win_x, H + T))
                doc.add_line((win_x + WIN, H), (win_x + WIN, H + T))
                doc.add_line((win_x, H + T / 2), (win_x + WIN, H + T / 2))

        with doc.layer("FURNITURE"):
            with doc.batch():
                # A table with four chairs, mid-room.
                cx, cy = W / 2, H / 2
                doc.add_circle((cx, cy), 450)                 # table
                for i in range(4):
                    a = i * math.pi / 2 + math.pi / 4
                    doc.add_circle((cx + 700 * math.cos(a),
                                    cy + 700 * math.sin(a)), 180)

        with doc.layer("LABELS"):
            doc.add_text("ROOM  4.0 x 3.0", (W / 2, H - 300), height=150,
                         halign="center")

        doc.update_view()
        census: dict[str, int] = {}
        for entity in doc.entities():
            census[entity.type] = census.get(entity.type, 0) + 1
        doc.release()
        print("drawn; census:",
              ", ".join(f"{count} {name}" for name, count in sorted(census.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
