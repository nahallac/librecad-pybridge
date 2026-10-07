#!/usr/bin/env python3
"""Checks for the ergonomic Document API, run over a live socket.

Works against tools/dispatchtest --serve (what scripts/test_socket.sh does) or
against a real LibreCAD bridge session. With --shutdown it ends the session
when done. Exits non-zero on any failure.
"""

from __future__ import annotations

import math
import sys

from lcbridge import BridgeError, DimStyle, Document, StaleEntityError

checks = 0


def check(condition: bool, label: str) -> None:
    global checks
    if not condition:
        raise AssertionError(label)
    checks += 1
    print(f"ok  {label}")


def main() -> int:
    shutdown = "--shutdown" in sys.argv

    with Document.connect() as doc:
        # -- layers ---------------------------------------------------------
        base = doc.current_layer
        doc.set_layer("LC_API_TEST")
        check(doc.current_layer == "LC_API_TEST", "set_layer switches")
        check("LC_API_TEST" in doc.layers, "layers lists the new layer")

        with doc.layer("LC_API_INNER"):
            check(doc.current_layer == "LC_API_INNER", "layer() switches in")
        check(doc.current_layer == "LC_API_TEST", "layer() restores on exit")

        properties = doc.layer_properties
        check({"color", "lineweight", "linetype"} <= set(properties),
              "layer_properties shape")
        doc.set_layer_properties(color=0x0000FF)
        check(doc.layer_properties["color"] == 0x0000FF,
              "set_layer_properties(color=...) round-trips")

        # -- creation, tuples as points --------------------------------------
        doc.add_line((0, 0), (100, 0))
        doc.add_circle((50, 50), 25)
        doc.add_arc((150, 50), 25, 0.0, math.pi / 2)
        doc.add_polyline([(200, 0), (250, 0), (250, 50, 0.5), (200, 50)],
                         closed=True)
        doc.add_text("api test", (0, 120), height=5)

        lines = doc.entities(types=["line"])     # lowercase on purpose
        check(len(lines) >= 1, "entities() filter accepts lowercase type names")

        # -- batch ----------------------------------------------------------
        before = len(doc.entities(types=["POINT"]))
        with doc.batch():
            for i in range(10):
                doc.add_point((i * 5.0, -20.0))
            # A query inside the batch must see everything queued so far.
            inside = len(doc.entities(types=["POINT"]))
            check(inside == before + 10,
                  "query inside batch() flushes the queue first")
        after = len(doc.entities(types=["POINT"]))
        check(after == before + 10, "batch() created everything exactly once")

        try:
            with doc.batch():
                doc.add_point((0, 0))
                raise RuntimeError("abort")
        except RuntimeError:
            pass
        check(len(doc.entities(types=["POINT"])) == after,
              "batch() aborted by an exception sends nothing")

        # -- entity objects ---------------------------------------------------
        # Filter to this test's layer: on the stub server the smoke test's
        # entities share the drawing.
        def api_circles():
            return [c for c in doc.entities(types=["CIRCLE"])
                    if c["layer"] == "LC_API_TEST"]

        circle = api_circles()[0]
        check(circle.type == "CIRCLE", "Entity.type")
        check(circle["radius"] > 0, "subscription reads attributes")

        circle.move((10, 0))
        circle.refresh()
        check(not math.isnan(circle["center_x"]), "handle survives move")
        circle.rotate((0, 0), math.pi).scale((0, 0), (2, 2))
        check(circle.refresh()["radius"] > 0, "handle survives rotate+scale")

        circle.update(color=0x00FF00)
        try:
            circle.refresh()
            check(False, "stale entity must refuse refresh")
        except StaleEntityError:
            check(True, "update() makes the entity stale")

        fresh = api_circles()[0]
        check(fresh["color"] == 0x00FF00, "update() wrote the attribute")

        try:
            fresh.update(radius="not a number works, but...",
                         no_such_attribute=1)
            check(False, "unknown attribute must be rejected")
        except BridgeError as error:
            check(error.code == "bad_args", "unknown attribute raises BridgeError")
        check(not fresh._stale, "failed update leaves the entity usable")

        # -- polyline vertices -------------------------------------------------
        polyline = [p for p in doc.entities(types=["POLYLINE"])
                    if p["layer"] == "LC_API_TEST"][0]
        vertices = polyline.vertices()
        check(len(vertices) == 4 and len(vertices[0]) == 3,
              "vertices() returns [x, y, bulge] rows")
        polyline.set_vertices([(0, 0), (10, 0), (10, 10, 0.3)])
        check(len(polyline.vertices()) == 3, "set_vertices() rewrites")

        # -- removal -----------------------------------------------------------
        point = doc.entities(types=["POINT"])[0]
        point.remove()
        try:
            point.refresh()
            check(False, "removed entity must be stale")
        except StaleEntityError:
            check(True, "remove() makes the entity stale")

        # -- dimensions ---------------------------------------------------------
        with doc.layer("LC_API_DIMS"):
            doc.dim_aligned((0, 0), (100, 0), offset=-15)
            doc.dim_radius((50, 40), 20)
            doc.dim_diameter((50, 40), 20, style=DimStyle(terminator="arrow"))
            with doc.batch():                     # dims nest inside a batch
                doc.dim_vertical((10, 5), (90, 25), x=120)

        dim_entities = [e for e in doc.entities()
                        if e["layer"] == "LC_API_DIMS"]
        dim_texts = sorted(e["text"] for e in dim_entities if e.type == "TEXT")
        check(dim_texts == ["100", "20", "R20", "Ø40"],
              f"dimension labels measure correctly ({dim_texts})")
        check(sum(1 for e in dim_entities if e.type == "LINE") == 17,
              "dimension line work drawn (ticks, arrows, extensions)")

        # -- misc ---------------------------------------------------------------
        doc.set_variable("$LC_API_PROBE", 3, type="int")
        check(doc.get_variable("$LC_API_PROBE", type="int") == 3,
              "variables round-trip")
        check(doc.get_variable("$LC_API_MISSING") is None,
              "unset variable reads as None")
        check(isinstance(doc.real_to_string(1.5, units=2, precision=1), str),
              "real_to_string formats")

        # -- native operations --------------------------------------------------
        status = doc.native_status()
        check({"commands", "selection", "reason"} <= set(status),
              "native_status shape")
        if not status["commands"]:
            probe = doc.entities(types=["CIRCLE"])[0]
            for call in (lambda: doc.exec_command("zoom"),
                         lambda: doc.cad_dim_aligned((0, 0), (10, 0), (5, 5)),
                         lambda: doc.cad_hatch([]),
                         lambda: probe.selected,
                         lambda: probe.bbox(),
                         lambda: doc.bbox(),
                         lambda: doc.entities(selected_only=True),
                         lambda: doc.offset([probe], 5, (0, 0)),
                         lambda: doc.mirror([probe], (0, 0), (1, 0)),
                         lambda: doc.explode([probe]),
                         lambda: doc.trim(probe, (0, 0), probe, (1, 1)),
                         lambda: doc.file_info(),
                         lambda: doc.save(),
                         lambda: doc.save_as("/tmp/never-written.dxf"),
                         lambda: doc.undo_checkpoint(),
                         lambda: doc.undo(),
                         lambda: doc.open("/nonexistent.dxf"),
                         lambda: doc.new(),
                         lambda: doc.layer_state("0"),
                         lambda: doc.layer_states(),
                         lambda: doc.set_layer_state("0", locked=True),
                         lambda: doc.rename_layer("0", "ZERO"),
                         lambda: doc.define_block("B", (0, 0), [probe]),
                         lambda: doc.rename_block("B", "C"),
                         lambda: doc.remove_block("B"),
                         lambda: doc.block_entities("B"),
                         lambda: probe.length(),
                         lambda: probe.area(),
                         lambda: probe.intersections(probe),
                         lambda: doc.nearest_entity((0, 0)),
                         lambda: probe.nearest_point((0, 0)),
                         lambda: probe.contains((0, 0)),
                         lambda: doc.find_entity(1),
                         lambda: doc.view(),
                         lambda: doc.zoom_auto(),
                         lambda: doc.zoom_window((0, 0), (10, 10)),
                         lambda: doc.zoom_in(),
                         lambda: doc.zoom_out(2.0, center=(0, 0)),
                         lambda: doc.zoom_pan(10, 10),
                         lambda: doc.zoom_previous(),
                         lambda: doc.zoom_page(),
                         lambda: doc.set_view(factor=1.0),
                         lambda: doc.documents(),
                         lambda: doc.activate_document(0),
                         lambda: doc.close_document(discard=True),
                         lambda: doc.export_image("/tmp/never.png", 10, 10),
                         lambda: doc.export_pdf("/tmp/never.pdf"),
                         lambda: doc.move([probe], (1, 0)),
                         lambda: doc.rotate([probe], (0, 0), 0.5, copies=2),
                         lambda: doc.scale([probe], (0, 0), 2),
                         lambda: doc.move_rotate([probe], (1, 0), (0, 0), 0.5),
                         lambda: doc.rotate2([probe], (0, 0), (1, 0), 0.5, 0.5),
                         lambda: doc.stretch((0, 0), (1, 1), (1, 0)),
                         lambda: doc.fillet(probe, (0, 0), probe, (1, 1), 1),
                         lambda: doc.chamfer(probe, (0, 0), probe, (1, 1), 1),
                         lambda: doc.cut(probe, (0, 0)),
                         lambda: doc.change_attributes([probe], color="bylayer"),
                         lambda: doc.revert_direction([probe])):
                try:
                    call()
                    check(False, "native op must be unavailable on the stub")
                except BridgeError as error:
                    check(error.code == "unavailable",
                          f"native op reports unavailable ({error.code})")

        released = doc.release()
        check(isinstance(released, int), f"release() ({released} handles)")
        doc.update_view()
        doc.set_layer(base)

        if shutdown:
            doc.shutdown()
            print("session shut down")

    print(f"\n{checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
