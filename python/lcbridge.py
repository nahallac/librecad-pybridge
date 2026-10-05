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
