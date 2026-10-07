"""Python client for the LibreCAD Python bridge.

Two layers over the bridge's line protocol (one JSON object per line over a
Unix domain socket):

  - Bridge: a thin mirror of the plugin's operation table, request()/batch().
  - Document / Entity: the API scripts are meant to use. Geometry, layers,
    blocks, queries, and -- through the plugin's native layer -- real
    dimensions and hatches, selection and bounding-box reads, offset/mirror/
    explode/trim, undo steps, save/open/new, zoom and view, switching and
    closing document windows, image/PDF export. Document.launch() starts a
    LibreCAD of its own, windowed or headless, with the session auto-started.
    Interactive prompts (prompt_point/select/int/real/string) ask the person
    at LibreCAD and block until answered; subscribe()/events()/on() receive
    push events (selection changed, document modified, ...).

Usage:

    from lcbridge import Document

    with Document.connect() as doc:          # a session someone started
        doc.set_layer("FLOORPLAN")
        doc.add_line((0, 0), (5000, 0))
        print(doc.layers())

    doc = Document.launch("plan.dxf", headless=True)   # or start your own
    ...
    doc.shutdown(); doc.process.terminate()

Native operations raise BridgeError("unavailable") against the offline stub
or a LibreCAD the plugin was not built for; doc.native_status() says which.

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

__version__ = "0.3.0"

import collections
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


EVENT_QUEUE_LIMIT = 1000


def _is_event(frame: dict[str, Any]) -> bool:
    """Push-event frames carry "event"; responses carry "ok"."""
    return "event" in frame and "ok" not in frame


class _SocketTimeout:
    """Context manager: a different socket timeout for one call."""

    def __init__(self, sock: socket.socket, timeout: float | None):
        self._sock = sock
        self._timeout = timeout
        self._previous: float | None = None

    def __enter__(self) -> None:
        self._previous = self._sock.gettimeout()
        self._sock.settimeout(self._timeout)

    def __exit__(self, *exc_info: object) -> None:
        self._sock.settimeout(self._previous)


class Bridge:
    """One connection to a running bridge session.

    Context manager; closing does not end the session in LibreCAD, it only
    disconnects. Call shutdown() to end the session itself.

    Push events: after a ``subscribe`` request the server may write event
    frames, ``{"event": name, "seq": n, "data": {...}}``, at any time. Frames
    that arrive while a response is awaited are queued in ``events`` (a
    deque bounded at EVENT_QUEUE_LIMIT, oldest dropped first); poll_events()
    drains the queue and reads whatever else is pending. A client that never
    subscribes never sees one.
    """

    def __init__(self, path: str | None = None, timeout: float | None = 30.0):
        self._path = path or default_socket_path()
        self._timeout = timeout
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(self._path)
        # Own line buffer rather than socket.makefile(): a file object is
        # unusable after one read timeout, and poll_events() times out by
        # design. A partial line simply stays here for the next read.
        self._buffer = bytearray()
        self._next_id = 1
        self.events: collections.deque[dict[str, Any]] = \
            collections.deque(maxlen=EVENT_QUEUE_LIMIT)

    # -- context manager ----------------------------------------------------

    def __enter__(self) -> "Bridge":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def close(self) -> None:
        """Disconnect. The bridge session in LibreCAD keeps running."""
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
            while True:
                frame = self._read_frame()
                if not _is_event(frame):
                    return frame
                self.events.append(frame)    # arrived ahead of the response
        except OSError as error:
            raise ProtocolError(f"connection failed: {error}") from error

    def _read_frame(self) -> dict[str, Any]:
        """The next complete line as a JSON object, reading as needed under
        the socket's current timeout (socket.timeout / BlockingIOError
        propagate, leaving any partial line buffered)."""
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line = bytes(self._buffer[:newline])
                del self._buffer[:newline + 1]
                if line.strip():
                    break
                continue
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ProtocolError("server closed the connection")
            self._buffer += chunk
        try:
            frame = json.loads(line)
        except json.JSONDecodeError as error:
            raise ProtocolError(f"unreadable response: {error}") from error
        if not isinstance(frame, dict):
            raise ProtocolError("response was not a JSON object")
        return frame

    def socket_timeout(self, timeout: float | None) -> _SocketTimeout:
        """``with bridge.socket_timeout(None): ...`` -- another timeout for
        the calls inside (None waits forever), restored afterwards. For the
        blocking prompt operations."""
        return _SocketTimeout(self._sock, timeout)

    def poll_events(self, timeout: float = 0.0) -> list[dict[str, Any]]:
        """Return queued event frames plus any that arrive now.

        Waits up to ``timeout`` seconds for the first frame when none is
        queued, then takes whatever else is already pending without
        waiting. ``timeout=0`` never blocks. Call only between requests.
        """
        drained = list(self.events)
        self.events.clear()
        deadline = time.monotonic() + max(0.0, timeout)
        previous = self._sock.gettimeout()
        try:
            while True:
                remaining = 0.0 if drained else deadline - time.monotonic()
                # 0.0 puts the socket in non-blocking mode for this read.
                self._sock.settimeout(max(0.0, remaining))
                try:
                    frame = self._read_frame()
                except (socket.timeout, BlockingIOError):
                    break
                except ProtocolError:
                    if drained:     # e.g. session_ending, then the close
                        break
                    raise
                if not _is_event(frame):
                    raise ProtocolError(
                        "received a response with no request pending")
                drained.append(frame)
        except OSError as error:
            raise ProtocolError(f"connection failed: {error}") from error
        finally:
            try:
                self._sock.settimeout(previous)
            except OSError:
                pass
        return drained

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


class _DisconnectedBridge:
    """Stands in for the Bridge after the last drawing was closed.

    There is no session to talk to any more (LibreCAD starts one only on a
    drawing), so every request raises BridgeError("disconnected").
    """

    def __init__(self, path: str, timeout: float | None):
        self._path = path
        self._timeout = timeout

    def request(self, op: str, **args: Any) -> Any:
        raise BridgeError("disconnected",
                          "the last drawing was closed; there is no bridge "
                          "session any more (open a drawing in LibreCAD and "
                          "start the bridge, then Document.connect())")

    def request_raw(self, request: dict[str, Any]) -> dict[str, Any]:
        return self.request(request.get("op", ""))

    def batch(self, requests: list[dict[str, Any]],
              stop_on_error: bool = True) -> list[dict[str, Any]]:
        return self.request("batch")

    def operations(self) -> list[str]:
        return self.request("operations")

    def shutdown(self) -> None:
        self.request("shutdown")

    def close(self) -> None:
        pass


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

    # These go through the plugin API and keep the handle. Document.move(),
    # rotate(), scale(), move_rotate() run LibreCAD's own modify tools
    # instead: several entities at once, n copies, current layer/attributes.

    def move(self, offset: Any, keep_original: bool = False) -> "Entity":
        """Move by ``offset`` (dx, dy) through the plugin API.

        The handle survives. For copies or several entities at once, see
        Document.move(), which runs LibreCAD's Move tool.
        """
        self._call("entity_move", offset=_pt(offset), keep_original=keep_original)
        self._data = None
        return self

    def rotate(self, center: Any, angle: float,
               keep_original: bool = False) -> "Entity":
        """Rotate by ``angle`` radians around ``center`` (plugin API; the
        handle survives). Document.rotate() is the Rotate tool, with copies."""
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
        """``factor`` is (fx, fy); pass the same value twice for uniform.

        Plugin API; the handle survives. Document.scale() is the Scale tool,
        with copies and a single-number factor.
        """
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

    # -- geometry queries (native: need a real LibreCAD session) ---------------

    #: Set by Document.nearest_entity(): how far the query point was.
    distance: float | None = None

    def _require_live(self) -> None:
        if self._stale or self._generation != self._doc._generation:
            raise StaleEntityError(
                f"{self.type} #{self._handle} is stale; re-fetch it with "
                f"Document.entities()")

    @property
    def id(self) -> int:
        """LibreCAD's own id for the entity (RS_Entity::getId()).

        Unlike a handle, which only means something to this session, the id
        belongs to the engine entity: it is the same across get_entities
        calls and ``Document.find_entity(id)`` finds the entity again. It is
        not stored in the file -- ids are assigned when an entity is created
        or loaded, so they change when the drawing is reopened -- and an
        entity LibreCAD replaces is a new entity with a new id: that is what
        ``move()``, ``rotate()``, ``scale()`` and ``update()`` do (the handle
        survives the first three, the id does not), as do trim and offset
        without ``keep_original``. Cached from the row when present, else
        asked for (it is also in ``data["id"]``).
        """
        if self._data is not None and "id" in self._data:
            return int(self._data["id"])
        return int(self._call("entity_id")["id"])

    def length(self) -> float | None:
        """Length of the entity (perimeter for a closed polyline or circle),
        or None for entities without one (text, hatch, image)."""
        return self._call("entity_length")["length"]

    def area(self) -> float:
        """Enclosed area of a circle, a full ellipse, or a closed polyline;
        0.0 for anything that encloses nothing (lines, arcs, open
        polylines, ...). Positive whatever the winding. Curved polyline
        segments (bulges) are included."""
        return float(self._call("entity_area")["area"])

    def intersections(self, other: "Entity",
                      on_entities: bool = True) -> list[tuple[float, float]]:
        """Points where this entity crosses ``other``.

        With ``on_entities`` (default) only points that lie on both entities
        as drawn; with False, where their infinite extensions meet (lines
        extended, arcs completed to full circles).
        """
        self._require_live()
        other._require_live()
        points = self._doc._call_now("intersections", a=self._handle,
                                     b=other._handle, on_entities=on_entities)
        return [(p[0], p[1]) for p in points]

    def nearest_point(self, point: Any, on_entity: bool = True
                      ) -> tuple[tuple[float, float], float] | None:
        """The point of this entity closest to ``point``: ((x, y), distance).

        ``on_entity`` False allows the point to lie on the infinite
        extension (a line beyond its ends, an arc's full circle). None when
        the entity has no such point.
        """
        result = self._call("nearest_point", point=_pt(point),
                            on_entity=on_entity)
        if result is None:
            return None
        return (tuple(result["point"]), result["distance"])

    def contains(self, point: Any) -> bool:
        """Whether ``point`` is inside this closed polyline, circle, or full
        ellipse. BridgeError("bad_request") for an open polyline or any
        other entity. A point exactly on the boundary may go either way."""
        return bool(self._call("point_inside", point=_pt(point)))


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
        # Push events: what this Document subscribed to (re-subscribed after
        # open()/new(), since subscriptions belong to a connection) and the
        # callbacks on() registered, by event name ("*" for all).
        self._subscriptions: set[str] = set()
        self._handlers: dict[str, list[Any]] = {}

    @classmethod
    def connect(cls, path: str | None = None,
                timeout: float | None = 30.0) -> "Document":
        return cls(Bridge(path, timeout))

    @classmethod
    def launch(cls, drawing: str | None = None, *, socket_path: str | None = None,
               headless: bool = False, startup_timeout: float = 60.0,
               timeout: float | None = 30.0,
               librecad: str = "librecad") -> "Document":
        """Start LibreCAD with the bridge auto-started and connect to it.

        Sets LC_PYBRIDGE_AUTOSTART so the plugin opens a session as soon as
        the drawing is up; ``headless`` runs it on Qt's offscreen platform
        (no window). The process is on the returned Document as
        ``process``; end it with doc.process.terminate() when done --
        shutdown() only ends the session.
        """
        import subprocess
        env = dict(os.environ)
        env["LC_PYBRIDGE_AUTOSTART"] = "1"
        socket_path = socket_path or default_socket_path()
        env["LC_PYBRIDGE_SOCKET"] = socket_path
        if headless:
            env["QT_QPA_PLATFORM"] = "offscreen"
        argv = [librecad] + ([os.path.abspath(drawing)] if drawing else [])
        process = subprocess.Popen(argv, env=env,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + startup_timeout
        while True:
            try:
                bridge = Bridge(socket_path, timeout)
                bridge.request("ping")
                break
            except (OSError, BridgeError, ProtocolError):
                if process.poll() is not None:
                    raise BridgeError("launch_failed",
                                      f"{librecad} exited with "
                                      f"{process.returncode}") from None
                if time.monotonic() > deadline:
                    process.terminate()
                    raise BridgeError("launch_timeout",
                                      "no bridge session appeared") from None
                time.sleep(0.2)
        doc = cls(bridge)
        doc.process = process
        return doc

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

    def _restart_session(self, op: str, timeout: float, **args: Any) -> Any:
        """Run a session-ending op and reconnect to the session after it.

        Returns the op's result. Two results end differently: one with
        ``"restart": False`` (activate_document on the window already
        served) left the session running, and one with ``"remaining": 0``
        (file_close of the last drawing) leaves no session to reconnect to,
        so the Document ends up disconnected.
        """
        self._flush()
        old_id = self.session_id()
        result = self._call_now(op, **args)   # acknowledged, then the session ends
        if isinstance(result, dict) and result.get("restart") is False:
            return result
        socket_path = self._bridge._path
        bridge_timeout = self._bridge._timeout
        old_events = list(self._bridge.events)
        self._bridge.close()
        self._generation += 1
        if isinstance(result, dict) and result.get("remaining") == 0:
            self._bridge = _DisconnectedBridge(socket_path, bridge_timeout)
            return result
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
                        # Events the old connection queued (session_ending,
                        # say) stay readable; subscriptions start afresh on
                        # a new connection, so renew them.
                        probe.events.extend(old_events)
                        if self._subscriptions:
                            probe.request("subscribe",
                                          events=sorted(self._subscriptions))
                        self._bridge = probe
                        return result
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
        """A real DIMENSION entity via LibreCAD's command line.

        Command path: types the dimension command and its three points into
        LibreCAD's command widget, so the real action runs. Kept for
        compatibility; cad_dim_aligned() / cad_dim_linear() and friends build
        the same entities directly and return them.

        ``kind`` is "aligned", "linear", "horizontal", or "vertical"; ``p1``
        and ``p2`` are the extension line origins and ``dimline`` a point on
        the dimension line. Text, arrows, and sizing follow the drawing's
        dimension settings ($DIMTXT and friends -- set_variable() reaches
        them). The result is associative and editable in LibreCAD, unlike the
        drawn dim_*() methods.
        """
        self._call_now("cmd_dim", kind=kind,
                       p1=_pt(p1), p2=_pt(p2), dimline=_pt(dimline))

    def cad_dim_aligned(self, p1: Any, p2: Any, dimline: Any,
                        text: str | None = None) -> Entity:
        """A real DIMALIGNED entity measuring p1-p2 along its own direction.

        Direct path: built by the engine like the dimension action builds it
        (no command line). ``dimline`` is any point the dimension line should
        pass through. ``text`` overrides the label: None for the measured
        value, "<>" inside the text stands for the measurement, " "
        suppresses it. Returns the new entity; its data carries
        ``definition_point``, ``extension_point1/2`` and the drawn ``label``.
        """
        return self._create("dim_aligned", p1=_pt(p1), p2=_pt(p2),
                            dimline=_pt(dimline), **self._dim_text(text))

    def cad_dim_horizontal(self, p1: Any, p2: Any, dimline: Any,
                           text: str | None = None) -> Entity:
        """A real horizontal DIMLINEAR; cad_dim_linear() with angle 0."""
        return self.cad_dim_linear(p1, p2, dimline, 0.0, text)

    def cad_dim_vertical(self, p1: Any, p2: Any, dimline: Any,
                         text: str | None = None) -> Entity:
        """A real vertical DIMLINEAR; cad_dim_linear() with angle pi/2."""
        import math as _math
        return self.cad_dim_linear(p1, p2, dimline, _math.pi / 2, text)

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


    # -- layer state (native) -----------------------------------------------------

    def layer_state(self, name: str) -> dict[str, bool]:
        """{"frozen", "locked", "print", "construction", "visible"} of layer
        ``name`` ("visible" is just "not frozen"). BridgeError("not_found")
        for an unknown layer."""
        return self._call_now("get_layer_state", name=name)

    def layer_states(self) -> list[dict[str, Any]]:
        """Every layer with its state, in one call: dicts as layer_state()
        returns, plus "name"."""
        return self._call_now("get_layer_states")

    def set_layer_state(self, name: str, **flags: bool) -> dict[str, bool]:
        """Freeze, lock, or otherwise flag a layer: any of ``frozen=``,
        ``locked=``, ``print=``, ``construction=``; omitted ones are kept.
        Goes through the layer list the layer widget uses, so the view and
        the layer panel follow. Not undoable. Returns the new state.
        """
        unknown = set(flags) - {"frozen", "locked", "print", "construction"}
        if unknown:
            raise TypeError(f"unknown layer flag(s): {sorted(unknown)}")
        return self._call_now("set_layer_state", name=name,
                              **{k: bool(v) for k, v in flags.items()})

    def rename_layer(self, old: str, new: str) -> dict[str, Any]:
        """Rename layer ``old`` to ``new``, in place: its entities stay on it
        (they point at the layer, not at its name). BridgeError("bad_request")
        if ``new`` is empty or taken, or ``old`` is layer "0". Not undoable.
        Returns the renamed layer's state, with its "name"."""
        return self._call_now("rename_layer", old=old, new=new)

    # -- blocks (native) ------------------------------------------------------------

    def define_block(self, name: str, base_point: Any, entities: list["Entity"],
                     remove: bool = True, insert: bool = False
                     ) -> "str | tuple[str, Entity]":
        """Create block ``name`` from ``entities`` -- what Create Block does.

        The entities are copied into the new block with ``base_point`` as
        its origin. ``remove`` (default) takes the originals out of the
        drawing (undoably; they become stale here); ``insert`` also places
        one INSERT of the block at ``base_point``, so the geometry reappears
        where it was. The block definition itself is not undoable.
        Returns the block name, or ``(name, insert_entity)`` with
        ``insert=True``. BridgeError("bad_request") when the name is empty
        or already used.
        """
        result = self._call_now(
            "block_define", name=name, base_point=_pt(base_point),
            handles=[e._handle for e in entities], remove=remove,
            insert=insert)
        if remove:
            self._retire(entities)
        if insert:
            row = result["insert"]
            return (result["name"],
                    Entity(self, row["handle"], row["type"], row.get("data")))
        return result["name"]

    def rename_block(self, old: str, new: str) -> str:
        """Rename a block and every INSERT that refers to it (in the drawing
        and in other blocks); the INSERT entities keep their handles. Not
        undoable. BridgeError("bad_request") if ``new`` is empty or taken."""
        return self._call_now("block_rename", old=old, new=new)

    def remove_block(self, name: str) -> None:
        """Remove a block definition. Refused (BridgeError("bad_request")) while
        any INSERT -- in the drawing or in another block -- still refers to
        it; remove those first. Undoable, like the Remove Block tool."""
        self._call_now("block_remove", name=name)

    def block_entities(self, name: str) -> list[Entity]:
        """The entities inside block ``name``, in block coordinates (the base
        point already subtracted). The handles are for reading
        (``data``, ``length()``, ``bbox()``, ...); do not move, update, or
        remove them, and ``release()`` them before removing the block."""
        rows = self._call_now("block_entities", name=name)
        return self._rows(rows)

    # -- queries (native) -------------------------------------------------------------

    def nearest_entity(self, point: Any, types: list[str] | None = None,
                       max_distance: float | None = None) -> Entity | None:
        """The visible entity closest to ``point``, or None.

        ``types`` restricts the search to those type names, ``max_distance``
        ignores anything farther away. The distance is on the result as
        ``entity.distance``. Frozen layers and undone entities are skipped;
        locked layers are not.
        """
        args: dict[str, Any] = {"point": _pt(point)}
        if types:
            args["types"] = [t.upper() for t in types]
        if max_distance is not None:
            args["max_distance"] = float(max_distance)
        row = self._call_now("nearest_entity", **args)
        if row is None:
            return None
        entity = Entity(self, row["handle"], row["type"], row.get("data"))
        entity.distance = row["distance"]
        return entity

    def find_entity(self, id: int) -> Entity | None:
        """The entity with LibreCAD id ``id`` (see Entity.id) with a fresh
        handle, or None when no such entity is in the drawing."""
        try:
            row = self._call_now("find_entity", id=int(id))
        except BridgeError as error:
            if error.code == "not_found":
                return None
            raise
        return Entity(self, row["handle"], row["type"], row.get("data"))

    # -- view, document windows, export (need a real LibreCAD session) ------
    #
    # Every zoom returns view(). None of them touch entities or the undo
    # stack, but LibreCAD flags the drawing modified when the view changes
    # (the view is saved in the DXF), so close_document() wants discard=True
    # afterwards.

    def view(self) -> dict[str, Any]:
        """The view: {"factor", "offset": [px, py], "size": [w, h] pixels,
        "visible": {"min", "max"} in drawing coordinates, "center"}."""
        return self._call_now("get_view")

    def zoom_auto(self, keep_aspect: bool = True) -> dict[str, Any]:
        """Fit the whole drawing into the view."""
        return self._call_now("zoom_auto", keep_aspect=keep_aspect)

    def zoom_window(self, p1: Any, p2: Any,
                    keep_aspect: bool = True) -> dict[str, Any]:
        """Show the rectangle p1-p2 (drawing coordinates)."""
        return self._call_now("zoom_window", p1=_pt(p1), p2=_pt(p2),
                              keep_aspect=keep_aspect)

    def zoom_in(self, factor: float = 1.137,
                center: Any = None) -> dict[str, Any]:
        """Zoom in by ``factor`` about ``center`` (default: the middle of
        the view). 1.137 is the step LibreCAD's own zoom-in uses."""
        args: dict[str, Any] = {"factor": float(factor)}
        if center is not None:
            args["center"] = _pt(center)
        return self._call_now("zoom_in", **args)

    def zoom_out(self, factor: float = 1.137,
                 center: Any = None) -> dict[str, Any]:
        """Zoom out by ``factor``; see zoom_in()."""
        args: dict[str, Any] = {"factor": float(factor)}
        if center is not None:
            args["center"] = _pt(center)
        return self._call_now("zoom_out", **args)

    def zoom_pan(self, dx: int, dy: int) -> dict[str, Any]:
        """Shift the view by pixels; positive ``dy`` moves the drawing up."""
        return self._call_now("zoom_pan", dx=int(dx), dy=int(dy))

    def zoom_previous(self) -> dict[str, Any]:
        """Back to the view before the last zoom_auto/zoom_window/zoom_in/
        zoom_out. LibreCAD records at most one view per half second."""
        return self._call_now("zoom_previous")

    def zoom_page(self) -> dict[str, Any]:
        """Fit the drawing's paper (print area) into the view."""
        return self._call_now("zoom_page")

    def set_view(self, factor: float | None = None, offset: Any = None,
                 center: Any = None) -> dict[str, Any]:
        """Set the zoom factor (pixels per drawing unit) and either the pixel
        ``offset`` or the drawing point to ``center`` the view on."""
        args: dict[str, Any] = {}
        if factor is not None:
            args["factor"] = float(factor)
        if offset is not None:
            args["offset"] = _pt(offset)
        if center is not None:
            args["center"] = _pt(center)
        return self._call_now("set_view", **args)

    def documents(self) -> list[dict[str, Any]]:
        """Every open document window: {"index", "path", "title",
        "modified", "active", "parent"}. ``active`` marks the one this
        session serves; ``parent`` is the drawing's index for a block editor
        or print preview, None for a drawing."""
        return self._call_now("list_documents")

    def activate_document(self, index_or_path: int | str,
                          timeout: float = 30.0) -> dict[str, Any]:
        """Switch to another open document window and continue there.

        ``index_or_path`` is an index from documents() or a drawing's file
        path. Like open(), this ends the session and reconnects to a new one
        on that window (Entities from before go stale) -- unless it already
        is the session's window, which changes nothing. Returns file_info().
        """
        if isinstance(index_or_path, int):
            args: dict[str, Any] = {"index": index_or_path}
        else:
            args = {"path": os.path.abspath(os.fspath(index_or_path))}
        self._restart_session("activate_document", timeout, **args)
        return self.file_info()

    def close_document(self, discard: bool = False,
                       timeout: float = 30.0) -> dict[str, Any]:
        """Close the session's document window.

        Refused (BridgeError "bad_request") when the drawing has unsaved
        changes unless ``discard`` is set -- LibreCAD would otherwise ask in
        a dialog. Returns {"remaining": n}. When other windows remain the
        Document reconnects to a new session on the one LibreCAD activates;
        after the last one it is left disconnected (``connected`` is False
        and every call raises BridgeError "disconnected").
        """
        return self._restart_session("file_close", timeout,
                                     discard=bool(discard))

    @property
    def connected(self) -> bool:
        """False once close_document() closed the last drawing."""
        return not isinstance(self._bridge, _DisconnectedBridge)

    def export_image(self, path: str, width: int, height: int, *,
                     format: str | None = None, border: int = 0,
                     background: str = "white", black_white: bool = False,
                     transparent: bool = False) -> dict[str, Any]:
        """Render the whole drawing to an image file, as File > Export does.

        The format comes from the extension (png, jpg, bmp, svg, and
        whatever else Qt can write) unless ``format`` names it. The drawing
        is fitted into ``width`` x ``height`` pixels less ``border`` on each
        side. ``background`` is "white" or "black"; ``black_white`` draws
        everything in the foreground colour; ``transparent`` (raster formats,
        white background only) leaves the background clear.
        """
        args: dict[str, Any] = {
            "path": os.path.abspath(os.fspath(path)),
            "width": int(width), "height": int(height), "border": int(border),
            "background": background, "black_white": black_white,
            "transparent": transparent}
        if format:
            args["format"] = format
        return self._call_now("export_image", **args)

    def export_pdf(self, path: str, *, paper: str | None = None,
                   landscape: bool | None = None,
                   fit_to_page: bool = True) -> dict[str, Any]:
        """Print the drawing to a PDF file, as File > Export as PDF does.

        ``paper`` is "A4", "A3", "Letter", ... or None for the drawing's own
        paper size; ``landscape`` None keeps the drawing's orientation (or
        portrait for a named paper). ``fit_to_page`` scales the drawing onto
        one page inside the drawing's margins; False prints at the drawing's
        paper scale and insertion base, over as many pages as it sets up.
        Returns {"path", "pages", "paper_mm": [w, h]}.
        """
        args: dict[str, Any] = {"path": os.path.abspath(os.fspath(path)),
                                "fit_to_page": fit_to_page}
        if paper:
            args["paper"] = paper
        if landscape is not None:
            args["landscape"] = bool(landscape)
        return self._call_now("export_pdf", **args)

    # -- more modify tools (RS_Modification; need a real LibreCAD session) ----
    #
    # The transforms below differ from Entity.move()/rotate()/scale(), which
    # go through the plugin API: these run LibreCAD's own modify-tool code on
    # many entities at once, can make ``copies`` (at 1x, 2x, ... the
    # transformation), and honour use_current_layer/use_current_attributes.
    # With copies=0 (the default) the originals are replaced by transformed
    # clones and become stale -- use the returned entities from then on. With
    # copies >= 1 the originals stay and only the copies are returned.

    def _transform(self, op: str, entities: list["Entity"], copies: int,
                   use_current_layer: bool, use_current_attributes: bool,
                   **args: Any) -> list[Entity]:
        rows = self._call_now(
            op, handles=[e._handle for e in entities], copies=int(copies),
            use_current_layer=use_current_layer,
            use_current_attributes=use_current_attributes, **args)
        if int(copies) == 0:
            self._retire(entities)
        return self._rows(rows)

    def move(self, entities: list["Entity"], offset: Any, copies: int = 0,
             use_current_layer: bool = False,
             use_current_attributes: bool = False) -> list[Entity]:
        """Move ``entities`` by ``offset`` (dx, dy) with the Move tool.

        ``copies`` >= 1 keeps the originals and adds that many copies at
        offset, 2*offset, ...; 0 moves the originals (replaced, stale).
        Returns the moved entities or the copies.
        """
        return self._transform("mod_move", entities, copies, use_current_layer,
                               use_current_attributes, offset=_pt(offset))

    def rotate(self, entities: list["Entity"], center: Any, angle: float,
               copies: int = 0, use_current_layer: bool = False,
               use_current_attributes: bool = False) -> list[Entity]:
        """Rotate ``entities`` by ``angle`` radians around ``center``.
        Copies go at angle, 2*angle, ...; see move() for ``copies``."""
        return self._transform("mod_rotate", entities, copies,
                               use_current_layer, use_current_attributes,
                               center=_pt(center), angle=float(angle))

    def scale(self, entities: list["Entity"], center: Any, factor: Any,
              copies: int = 0, use_current_layer: bool = False,
              use_current_attributes: bool = False) -> list[Entity]:
        """Scale ``entities`` around ``center``.

        ``factor`` is a number (uniform) or (fx, fy); unequal factors turn
        circles and arcs into ellipses, as in LibreCAD. Copies are scaled by
        factor, factor**2, ...; see move() for ``copies``.
        """
        if isinstance(factor, (int, float)):
            factor_arg: Any = float(factor)
        else:
            factor_arg = _pt(factor)
        return self._transform("mod_scale", entities, copies,
                               use_current_layer, use_current_attributes,
                               center=_pt(center), factor=factor_arg)

    def move_rotate(self, entities: list["Entity"], offset: Any, center: Any,
                    angle: float, copies: int = 0,
                    use_current_layer: bool = False,
                    use_current_attributes: bool = False) -> list[Entity]:
        """Move by ``offset``, then rotate by ``angle`` radians around
        ``center`` + offset (the reference point travels with the move).
        Copy n uses n*offset and n*angle; see move() for ``copies``."""
        return self._transform("mod_move_rotate", entities, copies,
                               use_current_layer, use_current_attributes,
                               offset=_pt(offset), center=_pt(center),
                               angle=float(angle))

    def rotate2(self, entities: list["Entity"], center1: Any, center2: Any,
                angle1: float, angle2: float, copies: int = 0,
                use_current_layer: bool = False,
                use_current_attributes: bool = False) -> list[Entity]:
        """Rotate by ``angle1`` around ``center1``, then by ``angle2`` around
        ``center2`` (itself carried along by the first rotation). Radians;
        see move() for ``copies``."""
        return self._transform("mod_rotate2", entities, copies,
                               use_current_layer, use_current_attributes,
                               center1=_pt(center1), center2=_pt(center2),
                               angle1=float(angle1), angle2=float(angle2))

    def stretch(self, first_corner: Any, second_corner: Any,
                offset: Any) -> list[Entity]:
        """Stretch by ``offset`` everything in the window between the corners.

        Works on the drawing, not on given entities, like the Stretch tool:
        entities wholly inside the window move, entities with an endpoint
        inside have those endpoints moved. Every touched entity is replaced;
        the replacements are returned. Entity objects you hold for touched
        entities are dead afterwards (the bridge answers no_such_handle), but
        this side cannot tell which they were, so they are not marked stale
        -- re-fetch with entities(). The selection is cleared first: LibreCAD
        would otherwise delete whatever was selected.
        """
        return self._rows(self._call_now(
            "mod_stretch", first_corner=_pt(first_corner),
            second_corner=_pt(second_corner), offset=_pt(offset)))

    def fillet(self, entity1: "Entity", point1: Any, entity2: "Entity",
               point2: Any, radius: float, trim: bool = True,
               corner: Any = None) -> list[Entity]:
        """Round the corner between two lines/arcs/circles with an arc.

        ``point1`` and ``point2`` lie on the parts of each entity to keep.
        ``corner`` is a point on the side of both entities where the arc
        belongs (LibreCAD's "round" offsets each entity toward it); it
        defaults to the midpoint of point1 and point2, which is inside the
        corner for picks on the kept parts. ``trim`` cuts both entities back
        to the arc; they are then replaced (stale). Returns the new arc and,
        when trimming, the trimmed entities.
        """
        args: dict[str, Any] = dict(
            entity1=entity1._handle, point1=_pt(point1),
            entity2=entity2._handle, point2=_pt(point2),
            radius=float(radius), trim=trim)
        if corner is not None:
            args["corner"] = _pt(corner)
        rows = self._call_now("mod_round", **args)
        if trim:
            self._retire([entity1, entity2])
        return self._rows(rows)

    def chamfer(self, entity1: "Entity", point1: Any, entity2: "Entity",
                point2: Any, length1: float, length2: float | None = None,
                trim: bool = True) -> list[Entity]:
        """Bevel the corner between two entities with a straight line.

        The chamfer starts ``length1`` from the intersection along entity1
        and ``length2`` (default: length1) along entity2; ``point1`` and
        ``point2`` lie on the parts to keep. ``trim`` cuts both entities back
        to the chamfer; they are then replaced (stale). Returns the chamfer
        line and, when trimming, the trimmed entities.
        """
        rows = self._call_now(
            "mod_bevel", entity1=entity1._handle, point1=_pt(point1),
            entity2=entity2._handle, point2=_pt(point2),
            length1=float(length1),
            length2=float(length1 if length2 is None else length2), trim=trim)
        if trim:
            self._retire([entity1, entity2])
        return self._rows(rows)

    def cut(self, entity: "Entity", point: Any) -> list[Entity]:
        """Split a line, arc, circle, or ellipse at ``point``.

        ``point`` is projected onto the entity first, as LibreCAD's snap
        would. The original is replaced (stale) by two pieces -- or, for a
        circle, by a single full-turn arc starting and ending at the point.
        Endpoints cannot be cut at.
        """
        rows = self._call_now("mod_cut", handle=entity._handle,
                              point=_pt(point))
        self._retire([entity])
        return self._rows(rows)

    def change_attributes(self, entities: list["Entity"],
                          layer: str | None = None, color: Any = None,
                          width: str | None = None,
                          linetype: str | None = None) -> list[Entity]:
        """Set layer, color, line width, and/or line type on ``entities``.

        Formats are those of entity data: ``color`` an RGB int, -1 / -2 or
        "bylayer" / "byblock"; ``width`` a lineweight name ("0.25mm",
        "BYLAYER", ...); ``linetype`` a name ("DashLine", "BYLAYER", ...);
        ``layer`` must exist. Every entity is replaced (stale) by a
        re-attributed clone; the clones are returned.
        """
        args: dict[str, Any] = {}
        if layer is not None:
            args["layer"] = layer
        if color is not None:
            args["color"] = color
        if width is not None:
            args["width"] = width
        if linetype is not None:
            args["linetype"] = linetype
        rows = self._call_now("mod_change_attributes",
                              handles=[e._handle for e in entities], **args)
        self._retire(entities)
        return self._rows(rows)

    def revert_direction(self, entities: list["Entity"]) -> list[Entity]:
        """Swap start and end of each entity (lines, arcs, polylines, ...).
        The originals are replaced (stale); the reversed clones returned."""
        rows = self._call_now("mod_revert_direction",
                              handles=[e._handle for e in entities])
        self._retire(entities)
        return self._rows(rows)

    # -- push events ------------------------------------------------------------
    #
    # Subscribed events arrive as frames on the connection; they are queued
    # while requests run (bounded, oldest dropped) and handed out by
    # events(). Nothing is sent before subscribe().

    def subscribe(self, events: list[str] | str = "*") -> list[str]:
        """Start receiving ``events`` (names, or "*" for all); returns the
        full subscription. Names: document_modified, selection_changed,
        entity_count_changed, layer_changed, view_changed, grid_changed,
        windows_changed, session_ending. All but the last two are polled
        every 100 ms and report changes only; most need the native layer
        (on the stub only entity_count_changed and layer_changed fire)."""
        names = [events] if isinstance(events, str) else list(events)
        result = self._call_now("subscribe", events=names)
        self._subscriptions = set(result["subscribed"])
        return result["subscribed"]

    def unsubscribe(self, events: list[str] | str | None = None) -> list[str]:
        """Stop receiving ``events`` (all of them when None)."""
        args: dict[str, Any] = {}
        if events is not None:
            args["events"] = [events] if isinstance(events, str) else list(events)
        result = self._call_now("unsubscribe", **args)
        self._subscriptions = set(result["subscribed"])
        return result["subscribed"]

    def on(self, event: str, callback: Any) -> None:
        """Call ``callback(event_frame)`` for every ``event`` that events()
        hands out ("*" for all), subscribing to it if needed. Callbacks run
        inside events(), on the caller's thread."""
        self._handlers.setdefault(event, []).append(callback)
        if event != "*" and event not in self._subscriptions:
            self.subscribe([event])

    def events(self, timeout: float = 0.0) -> list[dict[str, Any]]:
        """Event frames received so far, waiting up to ``timeout`` seconds
        for one if none is pending; dispatches each to the on() callbacks.

        Each frame is {"event": name, "seq": n, "data": {...}}; ``seq``
        counts up through the session, so a gap means frames were dropped
        from the bounded queue.
        """
        self._flush()
        frames = self._bridge.poll_events(timeout)
        for frame in frames:
            for key in (frame.get("event"), "*"):
                for callback in self._handlers.get(key, []):
                    callback(frame)
        return frames

    # -- interactive prompts ------------------------------------------------------
    #
    # Each one asks the person at LibreCAD and blocks until they answer -- the
    # server handles nothing else meanwhile -- so the socket timeout is lifted
    # for the call. ``timeout`` (seconds) has the plugin cancel the prompt
    # after that long instead; it needs the native layer. A cancelled prompt
    # returns None. Point and select prompts end whatever action the user had
    # running in LibreCAD.

    def _prompt(self, op: str, timeout: float | None,
                **args: Any) -> dict[str, Any]:
        self._flush()
        if timeout is not None:
            args["timeout_ms"] = max(1, int(round(float(timeout) * 1000)))
        socket_timeout = None if timeout is None else float(timeout) + 30.0
        with self._bridge.socket_timeout(socket_timeout):
            return self._bridge.request(op, **args)

    def prompt_point(self, message: str = "", base: Any = None,
                     timeout: float | None = None
                     ) -> tuple[float, float] | None:
        """Ask for a point: a click in the drawing, or coordinates typed on
        LibreCAD's command line. ``base`` draws a rubber band from there.
        None when the user cancels (right click) or ``timeout`` passes."""
        args: dict[str, Any] = {"message": str(message)}
        if base is not None:
            args["base"] = _pt(base)
        result = self._prompt("prompt_point", timeout, **args)
        if result.get("cancelled"):
            return None
        x, y = result["point"]
        return (x, y)

    def prompt_select(self, message: str = "",
                      timeout: float | None = None) -> list[Entity] | None:
        """Ask the user to select entities and finish with Enter or a right
        click; returns what is selected then (anything already selected
        counts), or None when nothing is or ``timeout`` passes."""
        result = self._prompt("prompt_select", timeout, message=str(message))
        if result.get("cancelled"):
            return None
        return self._rows(result["entities"])

    def prompt_int(self, message: str = "", default: int | None = None,
                   title: str = "", timeout: float | None = None) -> int | None:
        """Ask for an integer in a LibreCAD input dialog; None on Cancel."""
        args: dict[str, Any] = {"message": str(message), "title": str(title)}
        if default is not None:
            args["default"] = int(default)
        result = self._prompt("prompt_int", timeout, **args)
        return None if result.get("cancelled") else int(result["value"])

    def prompt_real(self, message: str = "", default: float | None = None,
                    title: str = "", timeout: float | None = None
                    ) -> float | None:
        """Ask for a number in a LibreCAD input dialog; None on Cancel."""
        args: dict[str, Any] = {"message": str(message), "title": str(title)}
        if default is not None:
            args["default"] = float(default)
        result = self._prompt("prompt_real", timeout, **args)
        return None if result.get("cancelled") else float(result["value"])

    def prompt_string(self, message: str = "", default: str | None = None,
                      title: str = "", timeout: float | None = None
                      ) -> str | None:
        """Ask for a line of text in a LibreCAD input dialog; None on
        Cancel."""
        args: dict[str, Any] = {"message": str(message), "title": str(title)}
        if default is not None:
            args["default"] = str(default)
        result = self._prompt("prompt_string", timeout, **args)
        return None if result.get("cancelled") else str(result["value"])

    # -- creation through the engine (need a real LibreCAD session) -----------
    #
    # Entities the plugin API cannot make, built directly the way LibreCAD's
    # own actions build them -- no command line, no picks. Each returns the
    # new Entity. Not batchable: the result is needed.

    def _create(self, op: str, **args: Any) -> Entity:
        rows = self._call_now(op, **args)
        if not rows:
            raise BridgeError("failed", f"{op} reported no new entity")
        return self._rows(rows)[0]

    @staticmethod
    def _dim_text(text: str | None) -> dict[str, Any]:
        return {} if text is None else {"text": str(text)}

    def add_mtext(self, text: str, at: Any, height: float, angle: float = 0.0,
                  halign: str = "left", valign: str = "top",
                  style: str = "standard", width: float = 100.0,
                  line_spacing: float = 1.0) -> Entity:
        """A multi-line text (MTEXT) entity; lines break at "\\n".

        Same arguments as add_text(), plus ``width`` (the reference
        rectangle width, LibreCAD's default 100) and ``line_spacing`` (a
        factor). ``at`` is the attachment point named by ``halign``
        ("left", "center", "right") and ``valign`` ("top", "middle",
        "bottom"); unlike add_text() the default is the top left corner, as
        LibreCAD's MText tool places it.
        """
        return self._create("add_mtext", text=str(text), at=_pt(at),
                            height=float(height), angle=float(angle),
                            halign=halign, valign=valign, style=style,
                            width=float(width),
                            line_spacing=float(line_spacing))

    def add_image(self, path: str, at: Any, *, scale: float | None = None,
                  width: float | None = None, height: float | None = None,
                  angle: float = 0.0, brightness: int = 50,
                  contrast: int = 50, fade: int = 0) -> Entity:
        """A raster IMAGE entity referencing the file at ``path``.

        ``at`` is the lower left corner. Size it with at most one of
        ``scale`` (drawing units per pixel; the default 1, as LibreCAD's
        image tool), ``width`` or ``height`` (of the whole image, in drawing
        units, keeping the aspect ratio). ``angle`` rotates it about ``at``.
        The drawing references the file, it does not embed it. Any format
        Qt reads works (PNG, JPEG, BMP, PPM, ...).
        """
        args: dict[str, Any] = {"path": os.path.abspath(os.fspath(path)),
                                "at": _pt(at), "angle": float(angle),
                                "brightness": int(brightness),
                                "contrast": int(contrast), "fade": int(fade)}
        for name, value in (("scale", scale), ("width", width),
                            ("height", height)):
            if value is not None:
                args[name] = float(value)
        return self._create("add_image", **args)

    def cad_dim_linear(self, p1: Any, p2: Any, dimline: Any,
                       angle: float = 0.0, text: str | None = None) -> Entity:
        """A real DIMLINEAR entity measuring p1-p2 along ``angle``.

        Direct path. ``angle`` 0 measures horizontally, pi/2 vertically;
        ``dimline`` is any point the dimension line should pass through.
        ``text`` as for cad_dim_aligned().
        """
        return self._create("dim_linear", p1=_pt(p1), p2=_pt(p2),
                            dimline=_pt(dimline), angle=float(angle),
                            **self._dim_text(text))

    def _circle_args(self, target: Any, radius: float | None) -> dict[str, Any]:
        if isinstance(target, Entity):
            return {"entity": target._handle}
        if radius is None:
            raise TypeError("give a CIRCLE/ARC entity, or a center and a radius")
        return {"center": _pt(target), "radius": float(radius)}

    def cad_dim_radial(self, target: Any, radius: float | None = None,
                       angle: float = 0.785398, text: str | None = None
                       ) -> Entity:
        """A real DIMRADIAL entity ("R50").

        Direct path; no pick needed. ``target`` is a CIRCLE or ARC Entity,
        or a center point with ``radius``. ``angle`` (radians, from the
        center) is where on the circle the dimension points. ``text`` as for
        cad_dim_aligned(). The data reports ``definition_point`` (the
        center), ``definition_point2`` (the point on the circle), ``label``.
        """
        return self._create("dim_radial", **self._circle_args(target, radius),
                            angle=float(angle), **self._dim_text(text))

    def cad_dim_diametric(self, target: Any, radius: float | None = None,
                          angle: float = 0.785398, text: str | None = None
                          ) -> Entity:
        """A real DIMDIAMETRIC entity, across the circle through ``angle``.

        Arguments as for cad_dim_radial(). The data reports the two opposite
        points on the circle as ``definition_point`` and
        ``definition_point2``.
        """
        return self._create("dim_diametric",
                            **self._circle_args(target, radius),
                            angle=float(angle), **self._dim_text(text))

    @staticmethod
    def _line_arg(line: Any) -> Any:
        if isinstance(line, Entity):
            return line._handle
        start, end = line
        return [_pt(start), _pt(end)]

    def cad_dim_angular(self, line1: Any, line2: Any, dimline: Any,
                        text: str | None = None) -> Entity:
        """A real DIMANGULAR entity between two lines.

        Direct path. ``line1`` and ``line2`` are LINE entities or point
        pairs ((x1, y1), (x2, y2)); they need not touch, only not be
        parallel. The two lines cross at a center and divide the plane into
        four sectors; the dimension measures the sector that contains
        ``dimline``, with its arc through that point -- as LibreCAD's tool
        does with the last click.
        """
        return self._create("dim_angular", line1=self._line_arg(line1),
                            line2=self._line_arg(line2),
                            dimline=_pt(dimline), **self._dim_text(text))

    def cad_dim_leader(self, points: list[Any], arrow: bool = True) -> Entity:
        """A real LEADER entity: a polyline of ``points`` (at least two)
        with an arrow head at the first point unless ``arrow`` is False.
        Size follows $DIMASZ. It carries no text; add one with add_mtext()."""
        return self._create("dim_leader", points=[_pt(p) for p in points],
                            arrow=bool(arrow))

    def add_hatch(self, entities: list["Entity"], pattern: str = "ANSI31",
                  scale: float = 1.0, angle: float = 0.0,
                  solid: bool = False) -> Entity:
        """A real HATCH entity bounded by ``entities``, built directly.

        Unlike cad_hatch() no selection, command or dialog is involved and
        the hatch is returned. ``entities`` (lines, arcs, circles, ellipses,
        polylines, splines) must form closed contours; the hatch keeps
        copies of them as its boundary, the originals stay. Raises
        BridgeError("failed") for an open boundary, an unknown pattern, or
        a scale LibreCAD refuses as too dense or too sparse for the area.
        """
        return self._create("add_hatch",
                            handles=[entity._handle for entity in entities],
                            pattern=pattern, scale=float(scale),
                            angle=float(angle), solid=bool(solid))
