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
$XDG_RUNTIME_DIR/librecad-pybridge, otherwise /tmp/librecad-pybridge.
"""

from __future__ import annotations

import json
import os
import socket
from typing import Any, Iterator


def default_socket_path() -> str:
    """The socket path the plugin listens on by default."""
    explicit = os.environ.get("LC_PYBRIDGE_SOCKET")
    if explicit:
        return explicit
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    return os.path.join(runtime_dir, "librecad-pybridge")


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
        if self._stale:
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


class _BatchScope:
    def __init__(self, doc: "Document"):
        self._doc = doc

    def __enter__(self) -> None:
        self._doc._begin_batch()

    def __exit__(self, exc_type: object, *exc_info: object) -> None:
        # On an exception nothing queued is sent: the batch dies with the
        # scope rather than half-applying.
        self._doc._end_batch(discard=exc_type is not None)


class Document:
    """The open LibreCAD drawing, over a bridge session."""

    def __init__(self, bridge: Bridge):
        self._bridge = bridge
        self._queue: list[dict[str, Any]] | None = None

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
                 visible_only: bool = False) -> list[Entity]:
        """Entities in the drawing, optionally filtered by type name.

        Each call hands out fresh handles; call release() (or let the session
        end) when a large result set is no longer needed.
        """
        args: dict[str, Any] = {"visible_only": visible_only}
        if types:
            args["types"] = [t.upper() for t in types]
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
