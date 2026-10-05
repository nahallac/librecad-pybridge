#!/usr/bin/env python3
"""End-to-end check of the bridge over its socket.

Run it against either server:
  - tools/dispatchtest --serve   (stub document, used by `make test-socket`)
  - LibreCAD with "Start bridge session" active (real drawing)

With --shutdown it also ends the session when done.

Exits non-zero on any failure.
"""

from __future__ import annotations

import math
import sys

from lcbridge import Bridge, BridgeError


def main() -> int:
    shutdown = "--shutdown" in sys.argv

    checks = 0

    def check(condition: bool, label: str) -> None:
        nonlocal checks
        if not condition:
            raise AssertionError(label)
        checks += 1
        print(f"ok  {label}")

    with Bridge() as bridge:
        ping = bridge.request("ping")
        check(ping["plugin"] == "lc_pybridge", "ping identifies the plugin")
        check(ping["protocol"] == 1, "protocol version is 1")

        operations = bridge.operations()
        check("add_line" in operations and "batch" in operations,
              "operation table is populated")

        bridge.request("set_layer", name="LC_BRIDGE_SMOKE")
        check(bridge.request("get_current_layer") == "LC_BRIDGE_SMOKE",
              "set_layer round-trips")

        # Bulk geometry in one round trip.
        responses = bridge.batch(
            [{"op": "add_line",
              "args": {"start": [x * 10.0, 0.0], "end": [x * 10.0, 20.0]}}
             for x in range(5)]
            + [{"op": "add_circle", "args": {"center": [25.0, 10.0], "radius": 9.0}},
               {"op": "add_arc",
                "args": {"center": [60.0, 10.0], "radius": 9.0,
                         "start_angle": 0.0, "end_angle": math.pi}}])
        check(all(r["ok"] for r in responses), "batch of 7 creations succeeds")

        lines = bridge.request("get_entities", types=["LINE"])
        check(len(lines) >= 5, f"LINE census sees the batch ({len(lines)} found)")
        check(all(e["type"] == "LINE" for e in lines), "type filter filters")

        circles = bridge.request("get_entities", types=["CIRCLE"])
        check(len(circles) >= 1, "CIRCLE census sees the circle")
        radius = circles[0]["data"]["radius"]
        check(abs(radius - 9.0) < 1e-9 or radius > 0,
              "circle reports a radius")

        # Handle lifetime rules, over the wire this time.
        handle = lines[0]["handle"]
        bridge.request("entity_move", handle=handle, offset=[1.0, 1.0])
        bridge.request("entity_data", handle=handle)  # survives move
        bridge.request("entity_update", handle=handle,
                       data={"color": 0xFF0000})
        try:
            bridge.request("entity_data", handle=handle)
            check(False, "handle must die after entity_update")
        except BridgeError as error:
            check(error.code == "no_such_handle",
                  "handle dropped after entity_update")

        try:
            bridge.request("add_line", start=[0, 0])
            check(False, "missing argument must be rejected")
        except BridgeError as error:
            check(error.code == "bad_args", "bad_args reported over the wire")

        released = bridge.request("release_handles")
        check(isinstance(released, int), f"release_handles ({released} released)")
        bridge.request("update_view")

        if shutdown:
            bridge.shutdown()
            print("session shut down")

    print(f"\n{checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
