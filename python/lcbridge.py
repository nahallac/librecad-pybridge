"""Thin client for the LibreCAD Python bridge.

Speaks the bridge's line protocol: one JSON object per line over a Unix domain
socket. This module is deliberately a thin mirror of the plugin's operation
table -- the ergonomic wrapper (doc.add_line(...), doc.layers, ...) is a later
milestone and will sit on top of this.

Usage:

    from lcbridge import Bridge

    with Bridge() as b:
        b.request("set_layer", name="FLOORPLAN")
        b.request("add_line", start=[0, 0], end=[5000, 0])
        print(b.request("get_layers"))

Conventions (enforced by the plugin, documented here for convenience):
  - points are [x, y]; polyline vertices are [x, y] or [x, y, bulge]
  - angles are radians everywhere, add_arc included
  - colors are ints: -1 ByLayer, -2 ByBlock, else 24-bit RGB

The socket path is resolved the same way the plugin resolves it: the
LC_PYBRIDGE_SOCKET environment variable if set, otherwise
$TMPDIR/librecad-pybridge (usually /tmp/librecad-pybridge) -- Qt5's
QLocalServer places bare-name sockets in QDir::tempPath(), not in
XDG_RUNTIME_DIR.
"""

from __future__ import annotations

__version__ = "0.1.0"

import json
import os
import time
import socket
from typing import Any, Iterator


def default_socket_path() -> str:
    """The socket path the plugin listens on by default.

    Mirrors Qt5's QLocalServer: a bare listen() name becomes
    QDir::tempPath()/<name>, and QDir::tempPath() is $TMPDIR (or /tmp), not
    XDG_RUNTIME_DIR.
    """
    explicit = os.environ.get("LC_PYBRIDGE_SOCKET")
    if explicit:
        return explicit
    temp_dir = os.environ.get("TMPDIR") or "/tmp"
    return os.path.join(temp_dir.rstrip("/"), "librecad-pybridge")


class BridgeError(Exception):
    """An error response from the bridge."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class ProtocolError(Exception):
    """The connection broke or the server sent something unreadable."""


class Bridge:
    """One connection to a running bridge session.

    Context manager; closing does not end the session in LibreCAD, it only
    disconnects. Call shutdown() to end the session itself.
    """

    def __init__(self, path: str | None = None, timeout: float | None = 30.0):
        self._path = path or default_socket_path()
        self._timeout = timeout
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(self._path)
        self._reader = self._sock.makefile("rb")
        self._next_id = 1

    # -- context manager ----------------------------------------------------

    def __enter__(self) -> "Bridge":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def close(self) -> None:
        """Disconnect. The bridge session in LibreCAD keeps running."""
        try:
            self._reader.close()
        finally:
            self._sock.close()

    # -- protocol -----------------------------------------------------------

    def request(self, op: str, **args: Any) -> Any:
        """Send one request, wait for its response, return its result.

        Raises BridgeError on an error response and ProtocolError if the
        connection fails.
        """
        response = self.request_raw({"op": op, "args": args} if args else {"op": op})
        if not response.get("ok"):
            error = response.get("error") or {}
            raise BridgeError(error.get("code", "unknown"),
                              error.get("message", "no message"))
        return response.get("result")

    def request_raw(self, request: dict[str, Any]) -> dict[str, Any]:
        """Send a prebuilt request object; return the raw response object."""
        request = dict(request)
        request.setdefault("id", self._next_id)
        self._next_id += 1

        line = json.dumps(request, separators=(",", ":")) + "\n"
        try:
            self._sock.sendall(line.encode("utf-8"))
            reply = self._reader.readline()
        except OSError as error:
            raise ProtocolError(f"connection failed: {error}") from error
        if not reply:
            raise ProtocolError("server closed the connection")
        try:
            response = json.loads(reply)
        except json.JSONDecodeError as error:
            raise ProtocolError(f"unreadable response: {error}") from error
        if not isinstance(response, dict):
            raise ProtocolError("response was not a JSON object")
        return response

    def batch(self, requests: list[dict[str, Any]],
              stop_on_error: bool = True) -> list[dict[str, Any]]:
        """Run many requests in one round trip; returns the raw responses.

        Each entry is {"op": ..., "args": {...}}. This is the path to use for
        bulk geometry: one message, not one per entity.
        """
        result = self.request("batch", requests=requests,
                              stop_on_error=stop_on_error)
        return result["responses"]

    def operations(self) -> list[str]:
        """Names of every operation the bridge supports."""
        return self.request("operations")

    def shutdown(self) -> None:
        """End the bridge session in LibreCAD (not only this connection)."""
        self.request("shutdown")


def iter_entities(bridge: Bridge, **args: Any) -> Iterator[dict[str, Any]]:
    """get_entities as an iterator; remember to release handles when done."""
    yield from bridge.request("get_entities", **args)


# ===========================================================================
# Ergonomic layer
# ===========================================================================
#
# Document wraps a Bridge in the API scripts are meant to use:
#
#     with Document.connect() as doc:
#         doc.set_layer("WALLS")
#         doc.add_line((0, 0), (5000, 0))
#         with doc.batch():                      # one wire message
#             for x in range(100):
#                 doc.add_circle((x * 50, 0), 10)
#         for circle in doc.entities(types=["CIRCLE"]):
#             circle.move((0, 100))
#
# Everything still speaks the same protocol; this layer only adds names,
# tuples-as-points, context managers, and entity objects over handles.


class StaleEntityError(Exception):
    """The entity's handle is gone (after update() or remove())."""


def _pt(value: Any) -> list[float]:
    """A point argument: any two-element iterable of numbers."""
    x, y = value
    return [float(x), float(y)]


def _vertex(value: Any) -> list[float]:
    """A polyline vertex: (x, y) or (x, y, bulge)."""
    items = list(value)
    if len(items) == 2:
        return [float(items[0]), float(items[1])]
    x, y, bulge = items
    return [float(x), float(y), float(bulge)]


class Entity:
    """One drawing entity, addressed by a bridge handle.

    Attribute names are the per-type names the bridge reports ("start_x" on a
    LINE, "radius" on a CIRCLE, ...), available by subscription:
    ``entity["radius"]``.

    Lifetime rules, inherited from LibreCAD's plugin API: move/rotate/scale
    keep the entity usable; update() and remove() end it -- afterwards every
    operation raises StaleEntityError, and a fresh object must be fetched with
    Document.entities().
    """

    def __init__(self, doc: "Document", handle: int, etype: str,
                 data: dict[str, Any] | None):
        self._doc = doc
        self._handle = handle
        self._generation = doc._generation
        self.type = etype
        self._data = data
        self._stale = False

    def __repr__(self) -> str:
        state = " (stale)" if self._stale else ""
        return f"<Entity {self.type} #{self._handle}{state}>"

    def __getitem__(self, name: str) -> Any:
        return self.data[name]

    @property
    def data(self) -> dict[str, Any]:
        """The entity's attributes, fetching them on first use."""
        if self._data is None:
            self.refresh()
        return self._data

    # -- plumbing ------------------------------------------------------------

    def _call(self, op: str, **args: Any) -> Any:
        if self._stale or self._generation != self._doc._generation:
            raise StaleEntityError(
                f"{self.type} #{self._handle} is stale; re-fetch it with "
                f"Document.entities()")
        return self._doc._call_now(op, handle=self._handle, **args)

    def refresh(self) -> "Entity":
        """Re-read the attributes from the drawing."""
        result = self._call("entity_data")
        self.type = result["type"]
        self._data = result["data"]
        return self

    # -- native reads (need a real LibreCAD session) ---------------------------

    @property
    def selected(self) -> bool:
        """Whether the entity is currently selected in LibreCAD.

        Read from the engine (RS_Entity::isSelected); the plugin API itself
        only offers a prompt. Raises BridgeError("unavailable") on the stub.
        """
        return bool(self._call("entity_selected"))

    def bbox(self) -> tuple[tuple[float, float], tuple[float, float]]:
        """((xmin, ymin), (xmax, ymax)) as LibreCAD keeps it for the entity."""
        box = self._call("entity_bbox")
        return (tuple(box["min"]), tuple(box["max"]))

    # -- modification (handle survives) ---------------------------------------

    def move(self, offset: Any, keep_original: bool = False) -> "Entity":
        self._call("entity_move", offset=_pt(offset), keep_original=keep_original)
        self._data = None
        return self

    def rotate(self, center: Any, angle: float,
               keep_original: bool = False) -> "Entity":
        """Rotate by ``angle`` radians around ``center``."""
        self._call("entity_rotate", center=_pt(center), angle=float(angle),
                   keep_original=keep_original)
        self._data = None
        return self

    def move_rotate(self, offset: Any, center: Any, angle: float,
                    keep_original: bool = False) -> "Entity":
        self._call("entity_move_rotate", offset=_pt(offset), center=_pt(center),
                   angle=float(angle), keep_original=keep_original)
        self._data = None
        return self

    def scale(self, center: Any, factor: Any,
              keep_original: bool = False) -> "Entity":
        """``factor`` is (fx, fy); pass the same value twice for uniform."""
        self._call("entity_scale", center=_pt(center), factor=_pt(factor),
                   keep_original=keep_original)
        self._data = None
        return self

    # -- polylines -------------------------------------------------------------

    def vertices(self) -> list[list[float]]:
        """POLYLINE only: the vertex list as [x, y, bulge] triples."""
        return self._call("entity_polyline")

    def set_vertices(self, vertices: list[Any]) -> None:
        """POLYLINE only: replace every vertex.

        Not undoable (LibreCAD edits the polyline in place, outside the undo
        system); prefer remove() plus Document.add_polyline() when that
        matters.
        """
        self._call("entity_set_polyline",
                   vertices=[_vertex(v) for v in vertices])
        self._data = None

    # -- end of life (handle dies) ----------------------------------------------

    def update(self, **attrs: Any) -> None:
        """Write attributes, e.g. ``entity.update(color=0xFF0000, end_x=99)``.

        The handle is dead afterwards; re-fetch to keep working with the
        entity.
        """
        self._call("entity_update", data=attrs)
        self._stale = True

    def remove(self) -> None:
        """Delete the entity from the drawing."""
        self._call("entity_remove")
        self._stale = True


class _LayerSwitch:
    def __init__(self, doc: "Document", name: str):
        self._doc = doc
        self._name = name
        self._previous: str | None = None

    def __enter__(self) -> None:
        self._previous = self._doc.current_layer
        self._doc.set_layer(self._name)

    def __exit__(self, *exc_info: object) -> None:
        if self._previous is not None:
            self._doc.set_layer(self._previous)


class _NullScope:
    """No-op context manager: used when a batch is already open."""

    def __enter__(self) -> None:
        return None

    def __exit__(self, *exc_info: object) -> None:
        return None


class _BatchScope:
    def __init__(self, doc: "Document"):
        self._doc = doc

    def __enter__(self) -> None:
        self._doc._begin_batch()

    def __exit__(self, exc_type: object, *exc_info: object) -> None:
        # On an exception nothing queued is sent: the batch dies with the
        # scope rather than half-applying.
        self._doc._end_batch(discard=exc_type is not None)


class DimStyle:
    """Geometry settings for the drawn dimensions.

    LibreCAD's plugin interface cannot create DIMENSION entities (every DIM*
    case is disabled upstream), so the dim_*() methods draw dimensions out of
    lines and text instead. They measure correctly and print correctly, but
    they are plain geometry: not associative, and LibreCAD's dimension tools
    will not edit them.

    Sizes are in drawing units. ``terminator`` is "tick" (an oblique slash,
    the architectural convention) or "arrow" (an open two-line arrowhead).
    ``precision`` is the number of decimals; trailing zeros are trimmed.
    ``scale`` multiplies the measured value before formatting, for drawings
    not in the unit the label should show.
    """

    def __init__(self, text_height: float = 2.5, terminator: str = "tick",
                 terminator_size: float = 1.25, extension_gap: float = 0.625,
                 extension_overshoot: float = 1.25, text_gap: float = 0.625,
                 precision: int = 2, scale: float = 1.0):
        if terminator not in ("tick", "arrow"):
            raise ValueError('terminator must be "tick" or "arrow"')
        self.text_height = float(text_height)
        self.terminator = terminator
        self.terminator_size = float(terminator_size)
        self.extension_gap = float(extension_gap)
        self.extension_overshoot = float(extension_overshoot)
        self.text_gap = float(text_gap)
        self.precision = int(precision)
        self.scale = float(scale)

    def format_value(self, value: float) -> str:
        text = f"{value * self.scale:.{self.precision}f}"
        if "." in text:
            text = text.rstrip("0").rstrip(".")
        return text


class Document:
    """The open LibreCAD drawing, over a bridge session."""

    def __init__(self, bridge: Bridge):
        self._bridge = bridge
        self._queue: list[dict[str, Any]] | None = None
        # Bumped when open()/new() start a fresh session; handles from the
        # old one are meaningless there, so Entity checks it.
        self._generation = 0

    @classmethod
    def connect(cls, path: str | None = None,
                timeout: float | None = 30.0) -> "Document":
        return cls(Bridge(path, timeout))

    def __enter__(self) -> "Document":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def close(self) -> None:
        """Disconnect; the session in LibreCAD keeps running."""
        self._bridge.close()

    def shutdown(self) -> None:
        """End the session in LibreCAD itself."""
        self._flush()
        self._bridge.shutdown()

    @property
    def bridge(self) -> Bridge:
        """The underlying thin client, for anything not wrapped here."""
        return self._bridge

    # -- batching -------------------------------------------------------------

    def batch(self) -> _BatchScope:
        """Queue creation calls and send them as one message on exit.

        Only calls with no return value are queued; anything that returns data
        (queries, entity operations) flushes the queue first and runs
        immediately, so results are always consistent with what was queued
        before them.
        """
        return _BatchScope(self)

    def _begin_batch(self) -> None:
        if self._queue is not None:
            raise RuntimeError("batch() cannot be nested")
        self._queue = []

    def _end_batch(self, discard: bool = False) -> None:
        queue, self._queue = self._queue, None
        if discard or not queue:
            return
        responses = self._bridge.batch(queue)
        for response in responses:
            if not response.get("ok"):
                error = response.get("error") or {}
                raise BridgeError(error.get("code", "unknown"),
                                  error.get("message", "no message"))

    def _flush(self) -> None:
        if self._queue:
            responses = self._bridge.batch(self._queue)
            self._queue = []
            for response in responses:
                if not response.get("ok"):
                    error = response.get("error") or {}
                    raise BridgeError(error.get("code", "unknown"),
                                      error.get("message", "no message"))

    def _call_queued(self, op: str, **args: Any) -> None:
        """A creation call: queued inside batch(), immediate otherwise."""
        if self._queue is not None:
            self._queue.append({"op": op, "args": args})
            return
        self._bridge.request(op, **args)

    def _call_now(self, op: str, **args: Any) -> Any:
        """A call whose result is needed: never queued."""
        self._flush()
        return self._bridge.request(op, **args) if args \
            else self._bridge.request(op)

    # -- layers ---------------------------------------------------------------

    @property
    def layers(self) -> list[str]:
        return self._call_now("get_layers")

    @property
    def current_layer(self) -> str:
        return self._call_now("get_current_layer")

    def set_layer(self, name: str) -> None:
        """Switch to ``name``, creating it if needed (creation is not undoable)."""
        self._call_now("set_layer", name=name)

    def layer(self, name: str) -> _LayerSwitch:
        """``with doc.layer("DOORS"): ...`` -- switch and switch back."""
        return _LayerSwitch(self, name)

    def delete_layer(self, name: str) -> bool:
        return self._call_now("delete_layer", name=name)

    @property
    def layer_properties(self) -> dict[str, Any]:
        """Current layer's {"color", "lineweight", "linetype"}."""
        return self._call_now("get_layer_properties")

    def set_layer_properties(self, **props: Any) -> None:
        """Any of color=, lineweight=, linetype=; omitted ones keep their value."""
        self._call_now("set_layer_properties", **props)

    # -- blocks ---------------------------------------------------------------

    @property
    def blocks(self) -> list[str]:
        return self._call_now("get_blocks")

    def add_block_from_file(self, path: str) -> str:
        """Import a DXF as a block definition; returns the block name.

        Not undoable (the one creation call LibreCAD leaves off the undo
        stack).
        """
        return self._call_now("add_block_from_file", path=str(path))

    # -- creation (all batchable) -----------------------------------------------

    def add_point(self, at: Any) -> None:
        self._call_queued("add_point", at=_pt(at))

    def add_line(self, start: Any, end: Any) -> None:
        self._call_queued("add_line", start=_pt(start), end=_pt(end))

    def add_lines(self, points: list[Any], closed: bool = False) -> None:
        """A chain of line segments (separate LINE entities)."""
        self._call_queued("add_lines", points=[_pt(p) for p in points],
                          closed=closed)

    def add_polyline(self, vertices: list[Any], closed: bool = False) -> None:
        """One POLYLINE entity; vertices are (x, y) or (x, y, bulge)."""
        self._call_queued("add_polyline",
                          vertices=[_vertex(v) for v in vertices],
                          closed=closed)

    def add_spline(self, points: list[Any], closed: bool = False) -> None:
        self._call_queued("add_spline_points", points=[_pt(p) for p in points],
                          closed=closed)

    def add_circle(self, center: Any, radius: float) -> None:
        self._call_queued("add_circle", center=_pt(center), radius=float(radius))

    def add_arc(self, center: Any, radius: float, start_angle: float,
                end_angle: float) -> None:
        """Angles in radians, counterclockwise from +x."""
        self._call_queued("add_arc", center=_pt(center), radius=float(radius),
                          start_angle=float(start_angle),
                          end_angle=float(end_angle))

    def add_ellipse(self, center: Any, major: Any, ratio: float,
                    start_angle: float = 0.0, end_angle: float = 0.0) -> None:
        """``major`` is the major-axis endpoint relative to the center."""
        self._call_queued("add_ellipse", center=_pt(center), major=_pt(major),
                          ratio=float(ratio), start_angle=float(start_angle),
                          end_angle=float(end_angle))

    def add_text(self, text: str, at: Any, height: float, angle: float = 0.0,
                 halign: str = "left", valign: str = "bottom",
                 style: str = "standard") -> None:
        self._call_queued("add_text", text=str(text), at=_pt(at),
                          height=float(height), angle=float(angle),
                          halign=halign, valign=valign, style=style)

    def add_insert(self, block: str, at: Any, scale: Any = (1.0, 1.0),
                   angle: float = 0.0) -> None:
        self._call_queued("add_insert", block=str(block), at=_pt(at),
                          scale=_pt(scale), angle=float(angle))

    # -- query ----------------------------------------------------------------

    def entities(self, types: list[str] | None = None,
                 visible_only: bool = False,
                 selected_only: bool = False) -> list[Entity]:
        """Entities in the drawing, optionally filtered by type name.

        ``selected_only`` keeps only what is currently selected in LibreCAD
        (a native read; unavailable on the stub). Each call hands out fresh
        handles; call release() (or let the session end) when a large result
        set is no longer needed.
        """
        args: dict[str, Any] = {"visible_only": visible_only}
        if types:
            args["types"] = [t.upper() for t in types]
        if selected_only:
            args["selected_only"] = True
        rows = self._call_now("get_entities", **args)
        return [Entity(self, row["handle"], row["type"], row.get("data"))
                for row in rows]

    def release(self) -> int:
        """Drop every entity handle this session holds; returns the count."""
        return self._call_now("release_handles")

    def unselect(self) -> None:
        self._call_now("unselect")

    # -- misc -----------------------------------------------------------------

    def update_view(self) -> None:
        self._call_now("update_view")

    def get_variable(self, key: str, type: str = "double") -> Any:
        """A drawing (DXF header) variable, or None when unset."""
        return self._call_now("get_variable", key=key, type=type)

    def set_variable(self, key: str, value: float, type: str = "double") -> bool:
        return self._call_now("set_variable", key=key, value=value, type=type)

    def real_to_string(self, value: float, units: int = 0,
                       precision: int = 0) -> str:
        """Format a number in the drawing's units (see the bridge docs)."""
        return self._call_now("real_to_string", value=float(value), units=units,
                              precision=precision)

    # -- dimensions (drawn, not DIMENSION entities -- see DimStyle) -------------

    def _dim_terminators(self, at: list[float], direction: list[float],
                         style: DimStyle) -> None:
        """One terminator on the dimension line at ``at``.

        ``direction`` is the unit vector of the dimension line, pointing
        inward (toward the other end).
        """
        import math as _math
        ux, uy = direction
        size = style.terminator_size
        if style.terminator == "tick":
            # Oblique slash at 45 degrees to the dimension line.
            tx = (ux - uy) * size * 0.7071
            ty = (uy + ux) * size * 0.7071
            self.add_line((at[0] - tx, at[1] - ty), (at[0] + tx, at[1] + ty))
        else:
            # Open arrowhead pointing outward, 15 degrees half-angle.
            angle = _math.atan2(uy, ux)
            for wing in (angle + 0.262, angle - 0.262):
                self.add_line((at[0], at[1]),
                              (at[0] + size * 2 * _math.cos(wing),
                               at[1] + size * 2 * _math.sin(wing)))

    def dim_aligned(self, p1: Any, p2: Any, offset: float,
                    text: str | None = None,
                    style: DimStyle | None = None) -> None:
        """Dimension the distance p1-p2, parallel to it.

        ``offset`` places the dimension line to the left of the p1->p2
        direction (negative for the right). ``text`` overrides the measured
        label. Drawn on the current layer; wrap in ``doc.layer(...)`` to
        direct it.
        """
        import math as _math
        style = style or DimStyle()
        x1, y1 = _pt(p1)
        x2, y2 = _pt(p2)
        length = _math.hypot(x2 - x1, y2 - y1)
        if length == 0.0:
            raise ValueError("dim_aligned needs two distinct points")
        ux, uy = (x2 - x1) / length, (y2 - y1) / length
        nx, ny = -uy, ux                       # left normal
        side = 1.0 if offset >= 0 else -1.0
        distance = abs(offset)

        gap = style.extension_gap
        over = style.extension_overshoot
        with self.batch() if self._queue is None else _NullScope():
            for x, y in ((x1, y1), (x2, y2)):
                self.add_line((x + side * nx * gap, y + side * ny * gap),
                              (x + side * nx * (distance + over),
                               y + side * ny * (distance + over)))
            d1 = (x1 + side * nx * distance, y1 + side * ny * distance)
            d2 = (x2 + side * nx * distance, y2 + side * ny * distance)
            self.add_line(d1, d2)
            self._dim_terminators([d1[0], d1[1]], [ux, uy], style)
            self._dim_terminators([d2[0], d2[1]], [-ux, -uy], style)

            angle = _math.atan2(uy, ux)
            if angle > _math.pi / 2 or angle <= -_math.pi / 2:
                angle += _math.pi         # keep the label readable
                tnx, tny = -nx * side, -ny * side
            else:
                tnx, tny = nx * side, ny * side
            label = text if text is not None else style.format_value(length)
            self.add_text(label,
                          ((d1[0] + d2[0]) / 2 + tnx * style.text_gap,
                           (d1[1] + d2[1]) / 2 + tny * style.text_gap),
                          height=style.text_height, angle=angle,
                          halign="center", valign="bottom")

    def dim_horizontal(self, p1: Any, p2: Any, y: float,
                       text: str | None = None,
                       style: DimStyle | None = None) -> None:
        """Horizontal distance between two points, dimension line at ``y``."""
        a, b = _pt(p1), _pt(p2)
        if a[0] > b[0]:
            a, b = b, a
        self._dim_projected(a, b, (a[0], y), (b[0], y), text, style)

    def dim_vertical(self, p1: Any, p2: Any, x: float,
                     text: str | None = None,
                     style: DimStyle | None = None) -> None:
        """Vertical distance between two points, dimension line at ``x``."""
        a, b = _pt(p1), _pt(p2)
        if a[1] > b[1]:
            a, b = b, a
        self._dim_projected(a, b, (x, a[1]), (x, b[1]), text, style)

    def _dim_projected(self, s1: Any, s2: Any, d1: Any, d2: Any,
                       text: str | None, style: DimStyle | None) -> None:
        """Extension lines from s1->d1 and s2->d2, dimension line d1-d2."""
        import math as _math
        style = style or DimStyle()
        s1, s2, d1, d2 = _pt(s1), _pt(s2), _pt(d1), _pt(d2)
        length = _math.hypot(d2[0] - d1[0], d2[1] - d1[1])
        if length == 0.0:
            raise ValueError("dimension has zero length")
        ux, uy = (d2[0] - d1[0]) / length, (d2[1] - d1[1]) / length

        with self.batch() if self._queue is None else _NullScope():
            for source, target in ((s1, d1), (s2, d2)):
                ex, ey = target[0] - source[0], target[1] - source[1]
                elen = _math.hypot(ex, ey)
                if elen > style.extension_gap:
                    ex, ey = ex / elen, ey / elen
                    self.add_line((source[0] + ex * style.extension_gap,
                                   source[1] + ey * style.extension_gap),
                                  (target[0] + ex * style.extension_overshoot,
                                   target[1] + ey * style.extension_overshoot))
            self.add_line(tuple(d1), tuple(d2))
            self._dim_terminators(d1, [ux, uy], style)
            self._dim_terminators(d2, [-ux, -uy], style)

            angle = _math.atan2(uy, ux)
            nx, ny = -uy, ux
            if angle > _math.pi / 2 or angle <= -_math.pi / 2:
                angle += _math.pi
                nx, ny = -nx, -ny
            label = text if text is not None else style.format_value(length)
            self.add_text(label,
                          ((d1[0] + d2[0]) / 2 + nx * style.text_gap,
                           (d1[1] + d2[1]) / 2 + ny * style.text_gap),
                          height=style.text_height, angle=angle,
                          halign="center", valign="bottom")

    def dim_radius(self, center: Any, radius: float, angle: float = 0.785398,
                   text: str | None = None,
                   style: DimStyle | None = None) -> None:
        """Radius leader from the center to the circle edge at ``angle``."""
        import math as _math
        style = style or DimStyle()
        cx, cy = _pt(center)
        ex = cx + radius * _math.cos(angle)
        ey = cy + radius * _math.sin(angle)
        with self.batch() if self._queue is None else _NullScope():
            self.add_line((cx, cy), (ex, ey))
            self._dim_terminators([ex, ey],
                                  [-_math.cos(angle), -_math.sin(angle)], style)
            label = text if text is not None                 else "R" + style.format_value(float(radius))
            tangle = angle if -_math.pi / 2 < angle <= _math.pi / 2                 else angle + _math.pi
            self.add_text(label, ((cx + ex) / 2, (cy + ey) / 2 + style.text_gap),
                          height=style.text_height, angle=tangle,
                          halign="center", valign="bottom")

    def dim_diameter(self, center: Any, radius: float, angle: float = 0.785398,
                     text: str | None = None,
                     style: DimStyle | None = None) -> None:
        """Diameter leader straight through the circle at ``angle``."""
        import math as _math
        style = style or DimStyle()
        cx, cy = _pt(center)
        ux, uy = _math.cos(angle), _math.sin(angle)
        p1 = (cx - radius * ux, cy - radius * uy)
        p2 = (cx + radius * ux, cy + radius * uy)
        with self.batch() if self._queue is None else _NullScope():
            self.add_line(p1, p2)
            self._dim_terminators(list(p1), [ux, uy], style)
            self._dim_terminators(list(p2), [-ux, -uy], style)
            label = text if text is not None                 else "\u00d8" + style.format_value(2.0 * float(radius))
            tangle = angle if -_math.pi / 2 < angle <= _math.pi / 2                 else angle + _math.pi
            self.add_text(label, (cx, cy + style.text_gap),
                          height=style.text_height, angle=tangle,
                          halign="center", valign="bottom")

    # -- native operations (command injection; real DIMENSION / HATCH) ----------
    #
    # These run inside LibreCAD through its command line and internal symbols,
    # because the plugin API cannot create dimension or hatch entities. They
    # need a real LibreCAD session; against the offline stub they raise
    # BridgeError("unavailable"). Check native_status() to probe.

    def native_status(self) -> dict[str, Any]:
        """{"commands": bool, "selection": bool, "reason": str}."""
        return self._call_now("native_status")

    def exec_command(self, command: str) -> None:
        """Feed one line to LibreCAD's command widget, as if typed.

        Escape hatch to every command-line feature the bridge does not wrap.
        Write-only: commands report to the widget's history, not back here.
        """
        self._call_now("exec_command", command=str(command))

    def select(self, entities: list["Entity"],
               deselect_others: bool = True) -> int:
        """Set the drawing selection to ``entities``; returns how many."""
        return self._call_now(
            "select_entities",
            handles=[entity._handle for entity in entities],
            deselect_others=deselect_others)

    def bbox(self, entities: list["Entity"] | None = None
             ) -> tuple[tuple[float, float], tuple[float, float]] | None:
        """Union bounding box of ``entities``, or of the whole drawing.

        ((xmin, ymin), (xmax, ymax)); None when nothing has an extent.
        """
        args: dict[str, Any] = {}
        if entities is not None:
            args["handles"] = [entity._handle for entity in entities]
        box = self._call_now("get_bbox", **args)
        if box is None:
            return None
        return (tuple(box["min"]), tuple(box["max"]))

    # -- file and undo (need a real LibreCAD session) ---------------------------

    def file_info(self) -> dict[str, Any]:
        """{"path": str, "modified": bool} for the current drawing."""
        return self._call_now("file_info")

    def save(self) -> dict[str, Any]:
        """Write the drawing to its own file. BridgeError("no_filename") for
        an unnamed drawing; use save_as()."""
        return self._call_now("file_save")

    def save_as(self, path: str, format: str | None = None) -> dict[str, Any]:
        """Write the drawing to ``path``, which becomes its file name.

        ``format`` is a DXF version ("dxf2007" default, "dxf2004", "dxf2000",
        "dxf14", "dxf12", "dxf1") or None to pick by extension.
        """
        args: dict[str, Any] = {"path": os.fspath(path)}
        if format:
            args["format"] = format
        return self._call_now("file_save_as", **args)

    def undo_checkpoint(self) -> None:
        """Close the current undo step and start a new one.

        A session is one undo step by default; call this between stages
        that should undo separately.
        """
        self._flush()
        self._call_now("undo_checkpoint")

    def undo(self, steps: int = 1) -> int:
        """Checkpoint, then undo ``steps`` steps; returns how many were."""
        self._flush()
        return int(self._call_now("undo", steps=int(steps)))

    def redo(self, steps: int = 1) -> int:
        self._flush()
        return int(self._call_now("redo", steps=int(steps)))

    def open(self, path: str, timeout: float = 60.0) -> dict[str, Any]:
        """Open ``path`` in a new LibreCAD window and continue there.

        A bridge session is bound to one drawing, so this ends the current
        session, has LibreCAD open the file and start a new session on it,
        and reconnects to that. Every Entity from before is stale afterwards.
        Returns file_info() of the new drawing; raises BridgeError if the
        drawing LibreCAD ended up on is not ``path`` (it could not be read,
        for example -- LibreCAD shows its own message then).
        """
        path = os.path.abspath(os.fspath(path))
        self._restart_session("file_open", timeout, path=path)
        info = self.file_info()
        if info.get("path") != path:
            raise BridgeError("open_failed",
                              f"LibreCAD did not open {path!r}; the session is "
                              f"on {info.get('path')!r}")
        return info

    def new(self, timeout: float = 30.0) -> dict[str, Any]:
        """Start a new, unnamed drawing in a new window and continue there.
        Same session mechanics as open()."""
        self._restart_session("file_new", timeout)
        return self.file_info()

    def session_id(self) -> str:
        """Unique id of the bridge session this Document is connected to."""
        return str(self._bridge.request("session")["id"])

    def _restart_session(self, op: str, timeout: float, **args: Any) -> None:
        self._flush()
        old_id = self.session_id()
        self._call_now(op, **args)         # acknowledged, then the session ends
        socket_path = self._bridge._path
        bridge_timeout = self._bridge._timeout
        self._bridge.close()
        self._generation += 1
        deadline = time.monotonic() + timeout
        # The new session listens on the same path, often within milliseconds,
        # so the socket's absence cannot be relied on; connect and ask instead
        # until a session with a different id answers.
        while True:
            try:
                probe = Bridge(socket_path, timeout=2.0)
            except OSError:
                probe = None
            if probe is not None:
                try:
                    if probe.request("session")["id"] != old_id:
                        probe._sock.settimeout(bridge_timeout)
                        self._bridge = probe
                        return
                except (OSError, BridgeError, ProtocolError):
                    pass
                probe.close()
            if time.monotonic() > deadline:
                raise BridgeError(
                    "restart_timeout",
                    "no new bridge session appeared (did LibreCAD show a "
                    "dialog?)")
            time.sleep(0.1)

    # -- engine modifications (the modify tools, via RS_Modification) ---------
    #
    # Each returns the entities it created. Entities the engine replaced are
    # marked stale here, exactly like update()/remove() do.

    def _rows(self, rows: list[dict[str, Any]]) -> list[Entity]:
        return [Entity(self, row["handle"], row["type"], row.get("data"))
                for row in rows]

    @staticmethod
    def _retire(entities: list["Entity"]) -> None:
        for entity in entities:
            entity._stale = True

    def offset(self, entities: list["Entity"], distance: float, side: Any,
               count: int = 1, keep_original: bool = True,
               use_current_layer: bool = False,
               use_current_attributes: bool = False) -> list[Entity]:
        """Parallel copies of ``entities`` at ``distance``, toward ``side``.

        ``side`` is any point on the side to offset toward. ``count`` copies
        are made at 1x, 2x, ... the distance. With ``keep_original`` False
        the originals are replaced by one offset copy (and become stale).
        """
        rows = self._call_now(
            "mod_offset", handles=[e._handle for e in entities],
            distance=float(distance), side=_pt(side), count=int(count),
            keep_original=keep_original, use_current_layer=use_current_layer,
            use_current_attributes=use_current_attributes)
        if not keep_original:
            self._retire(entities)
        return self._rows(rows)

    def mirror(self, entities: list["Entity"], axis_p1: Any, axis_p2: Any,
               copy: bool = False) -> list[Entity]:
        """Mirror ``entities`` across the line axis_p1-axis_p2.

        ``copy`` keeps the originals; otherwise they are replaced (stale).
        """
        rows = self._call_now(
            "mod_mirror", handles=[e._handle for e in entities],
            axis_p1=_pt(axis_p1), axis_p2=_pt(axis_p2), copy=copy)
        if not copy:
            self._retire(entities)
        return self._rows(rows)

    def explode(self, entities: list["Entity"],
                remove: bool = True) -> list[Entity]:
        """Replace containers (polylines, inserts, dimensions, hatches, text)
        with their member entities. ``remove`` drops the originals (stale)."""
        rows = self._call_now(
            "mod_explode", handles=[e._handle for e in entities], remove=remove)
        if remove:
            self._retire(entities)
        return self._rows(rows)

    def trim(self, entity: "Entity", trim_point: Any, limit: "Entity",
             limit_point: Any, both: bool = False) -> list[Entity]:
        """Trim ``entity`` (line, arc, circle, ellipse) against ``limit``.

        ``trim_point`` lies on the part of ``entity`` to keep; ``limit_point``
        picks the intersection when there are several. ``both`` trims
        ``limit`` as well. The trimmed entities are replaced and become
        stale; the replacements are returned.
        """
        rows = self._call_now(
            "mod_trim", handle=entity._handle, trim_point=_pt(trim_point),
            limit_handle=limit._handle, limit_point=_pt(limit_point),
            both=both)
        self._retire([entity, limit] if both else [entity])
        return self._rows(rows)

    def cad_dim(self, kind: str, p1: Any, p2: Any, dimline: Any) -> None:
        """A real DIMENSION entity via LibreCAD's own dimension action.

        ``kind`` is "aligned", "linear", "horizontal", or "vertical"; ``p1``
        and ``p2`` are the extension line origins and ``dimline`` a point on
        the dimension line. Text, arrows, and sizing follow the drawing's
        dimension settings ($DIMTXT and friends -- set_variable() reaches
        them). The result is associative and editable in LibreCAD, unlike the
        drawn dim_*() methods.
        """
        self._call_now("cmd_dim", kind=kind,
                       p1=_pt(p1), p2=_pt(p2), dimline=_pt(dimline))

    def cad_dim_aligned(self, p1: Any, p2: Any, dimline: Any) -> None:
        self.cad_dim("aligned", p1, p2, dimline)

    def cad_dim_horizontal(self, p1: Any, p2: Any, dimline: Any) -> None:
        self.cad_dim("horizontal", p1, p2, dimline)

    def cad_dim_vertical(self, p1: Any, p2: Any, dimline: Any) -> None:
        self.cad_dim("vertical", p1, p2, dimline)

    def cad_hatch(self, entities: list["Entity"], pattern: str = "ANSI31",
                  scale: float = 1.0, angle: float = 0.0,
                  solid: bool = False) -> None:
        """A real HATCH entity over a closed boundary.

        ``entities`` are the boundary (from doc.entities()); they must form a
        closed contour or LibreCAD creates nothing and this raises. The hatch
        action's pattern dialog is filled in and accepted automatically.
        ``angle`` in radians, like everything else here.
        """
        self._call_now("cmd_hatch",
                       handles=[entity._handle for entity in entities],
                       pattern=pattern, scale=float(scale),
                       angle=float(angle), solid=solid)
