# LibreCAD Python bridge

Scripting for LibreCAD: drive the open drawing from Python, with access to geometry, layers,
blocks, and selection.

Spun out of a separate house-layout project; see `LIBRECAD-SCRIPTING-HANDOFF.md` in that project for
the original scoping. `docs/findings.md` supersedes the handoff's risk list wherever the two
disagree.

**Status: milestone 2 (dispatch layer).** A Qt5 C++ plugin that builds against the
distro-installed LibreCAD headers, loads into LibreCAD, and exposes `Document_Interface` as a
table of named JSON operations. The operations are driven by a fixed self-test sequence; the
socket transport and the Python client come next. No Python yet.

## Build and install

No packages beyond `librecad` and `qt5-base` are required. In particular `qt5-tools` is **not**
needed, despite what the handoff said.

```bash
./scripts/install.sh
```

That builds the plugin and installs it to `~/.librecad/plugins/` — a user-writable directory
LibreCAD searches first, so no `sudo` is involved. Restart LibreCAD afterwards; plugins are
loaded once at startup.

Individual steps, if you want them:

```bash
make           # build build/liblc_pybridge.so
make test      # run the dispatch layer against a stub document, no LibreCAD needed
make check     # make test, plus load the plugin with QPluginLoader and print its metadata
make install   # copy to ~/.librecad/plugins/
make uninstall
```

The plugin **must** be built with the Qt 5 qmake (`/usr/bin/qmake` on Arch). `qmake6` produces a
plugin LibreCAD will refuse to load; `plugin/lc_pybridge.pro` hard-errors rather than let that
happen silently.

## Using it

Open a drawing, then look under **Plugins**. Plugin menu entries are disabled while no drawing is
open.

- **Python Bridge: Report document** — current layer, all layers and blocks, layer properties, and
  an entity census by type, plus the `cmd` string LibreCAD passed to `execComm()`. Exercises the
  read side of the API.
- **Python Bridge: Undo probe** — draws nine entities on a layer named `LC_PYBRIDGE_PROBE` using
  eight different creation calls, then explains what undo should do. One press of Ctrl+Z removes
  all nine; confirmed interactively, see `docs/findings.md` risk 1. The `LC_PYBRIDGE_PROBE` layer
  itself is not removed, because `setLayer()` creates layers outside the undo system.
- **Python Bridge: Dispatch self-test** — runs the whole operation table against the open drawing
  and shows a pass/fail report. It draws into a layer called `LC_BRIDGE_SELFTEST` and then moves,
  rotates, scales, rewrites, and deletes some of what it drew, so run it on a scratch drawing. All
  of it is one undo step.

## The operation table

The dispatch layer is the protocol. A request is a JSON object and so is its response:

```json
{"op": "add_line", "args": {"start": [0, 0], "end": [40, 0]}, "id": 7}
{"ok": true, "id": 7, "result": null}
{"ok": false, "id": 7, "error": {"code": "bad_args", "message": "missing required argument \"end\""}}
```

Conventions, uniform across every operation:

- Points are `[x, y]`. Polyline vertices are `[x, y]` or `[x, y, bulge]`.
- **Angles are radians everywhere**, `add_arc` included. `Document_Interface::addArc()` is the one
  call in LibreCAD's API that takes degrees; the dispatcher converts so callers never see that.
- Colors are integers: `-1` ByLayer, `-2` ByBlock, otherwise 24-bit RGB.
- Entity attributes are named per entity type (`start_x` on a LINE, `center_x` on a CIRCLE) rather
  than exposed as raw `EDATA` integers. Unknown or read-only names are rejected, not ignored.
- Entity types come from the attribute hash, never from `Plug_Entity::getEntityType()`, which
  reports a value from a different enumeration and mislabels everything — see `docs/findings.md`
  risk 9.
- `batch` takes a list of requests and returns a list of responses, so bulk geometry is one message
  instead of a round trip per entity.

Run `{"op": "operations"}` for the current list. Deliberately absent: the interactive prompts
(`getPoint`, `getEnt`, `getSelect`, `getSelectByType`, `getInt`, `getReal`, `getString`). Each one
spins a nested Qt event loop and cancels whatever action the user had in progress, so they need a
design of their own — see `docs/findings.md` risks 6 and 8. One consequence is worth knowing now:
**there is no way to read the current selection**, only to prompt for a new one.

## Layout

```
plugin/
  lc_pybridge.{h,cpp}        plugin entry point and menu actions
  lc_bridge_dispatch.{h,cpp} the operation table over Document_Interface
  lc_bridge_selftest.{h,cpp} the fixed request sequence, shared by both runners
tools/loadtest/      QPluginLoader harness, used by `make check`
tools/dispatchtest/  stub Document_Interface + runner, used by `make test`
docs/findings.md     what has been established about the plugin API, with evidence
vendor/              upstream LibreCAD v2.2.1.5 sources for reference, not built
scripts/install.sh
```

`tools/dispatchtest/` is worth knowing about: it links the dispatcher against a stub
`Document_Interface` that records calls and stores entity attributes, so the operation table, the
argument validation, and the handle lifetime rules can all be checked in a console binary without
starting LibreCAD. That is where to iterate.

`vendor/librecad-v2.2.1.5/` holds the upstream files the findings are drawn from
(`doc_plugin_interface.{h,cpp}`, `sample.cpp`). They are reference copies — nothing builds them —
so the claims in `docs/findings.md` can be rechecked without network access.

## What comes next

Milestone 3, the socket transport: a `QLocalServer` on a Unix socket, newline-delimited JSON,
served from inside `execComm()`. Then the Python client, then the ergonomic wrapper over it.

The architecture note, for context. `docs/findings.md` risk 7 establishes that a
plugin cannot hold a `Document_Interface*` past the end of `execComm()`: the object is
stack-allocated by LibreCAD for the duration of the call and the concrete class is not exported.
So the RPC server cannot live in the background and reach into the document when a request
arrives. Instead `execComm()` has to *be* the server loop — the menu entry starts the bridge,
serves requests while pumping Qt events, and returns when the client quits.

That is still Option A, and it still keeps Python out of LibreCAD's process. It also means a whole
scripting session is a single undo step, which falls out of risk 1, and which is the intended
behaviour here.
