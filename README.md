# LibreCAD Python bridge

Scripting for LibreCAD: drive the open drawing from Python, with access to geometry, layers,
blocks, and selection.

Spun out of a house-layout project that needed scripted floorplans. `docs/findings.md` records
what has been established about LibreCAD's plugin API, with evidence for each claim.

**Status: milestone 4 (Python API).** A Qt5 C++ plugin that exposes `Document_Interface` as a
table of named JSON operations and serves them over a Unix domain socket, plus a Python client in
two layers: a thin protocol mirror (`Bridge`) and the ergonomic API scripts are meant to use
(`Document`, `Entity`). What remains of the original plan is the honest test — driving a real
floorplan through it.

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
make             # build build/liblc_pybridge.so
make test        # dispatch layer against a stub document, no LibreCAD needed
make test-socket # stub document served over a socket, Python client driving it
make check       # both tests, plus load the plugin with QPluginLoader
make install     # copy to ~/.librecad/plugins/
make uninstall
```

The plugin **must** be built with the Qt 5 qmake (`/usr/bin/qmake` on Arch). `qmake6` produces a
plugin LibreCAD will refuse to load; `plugin/lc_pybridge.pro` hard-errors rather than let that
happen silently.

## Using it

Open a drawing, then **Plugins → Start Python bridge**. Plugin menu entries are disabled while no
drawing is open. A small status strip appears in the corner of the LibreCAD window; the session
serves Python clients until you press *Stop* or a client sends `{"op": "shutdown"}`. LibreCAD
stays usable while the session runs, and everything the session does is **one undo step**, by
design.

## Driving it from Python

With a session running:

Install the client into your project's environment (stdlib-only, Python 3.10+):

```bash
pip install -e "/path/to/librecad-pybridge/python"
```

or copy `python/lcbridge.py` next to your script — it is a single file with no dependencies.

```python
import math
from lcbridge import Document

with Document.connect() as doc:
    doc.set_layer("WALLS")
    doc.add_polyline([(0, 0), (4000, 0), (4000, 3000), (0, 3000)], closed=True)

    with doc.layer("OPENINGS"):                    # switches back on exit
        doc.add_arc((600, 0), 900, 0, math.pi / 2)

    with doc.batch():                              # one wire message
        for x in range(100):
            doc.add_circle((x * 50, -500), 10)

    for circle in doc.entities(types=["CIRCLE"]):
        if circle["radius"] < 15:
            circle.update(color=0xFF0000)          # handle dies; re-fetch to reuse
    doc.release()
```

Points are tuples, angles radians, colors ints (`-1` ByLayer, else 24-bit RGB). `doc.batch()`
queues creation calls and sends them as a single message; a query inside the batch flushes first,
so results always reflect what was queued, and an exception discards the unsent queue. Entity
objects follow LibreCAD's lifetime rules: `move`/`rotate`/`scale` keep the handle, `update()` and
`remove()` end it — further use raises `StaleEntityError`, re-fetch with `doc.entities()`.

### Dimensions

LibreCAD's plugin interface cannot create DIMENSION entities (every `DIM*` case is disabled
upstream), so the API draws them instead: extension lines, dimension line, tick or arrow
terminators, and a measured label. They measure and print correctly, but they are plain geometry —
not associative, and LibreCAD's dimension tools will not edit them.

```python
from lcbridge import Document, DimStyle

mm = DimStyle(text_height=120, terminator_size=60, precision=0)  # sized for mm drawings
with doc.layer("DIMS"):
    doc.dim_horizontal((0, 0), (4000, 0), y=-600, style=mm)      # measured label: 4000
    doc.dim_vertical((4000, 0), (4000, 3000), x=4600, style=mm)
    doc.dim_aligned(p1, p2, offset=-250, style=mm)               # parallel to p1-p2
    doc.dim_radius(center, 450, style=mm)                        # R450
    doc.dim_diameter(center, 450, style=mm)                      # Ø900
```

`DimStyle` holds sizes (in drawing units), tick vs arrow terminators, precision, and a value
scale; pass `text=` to override the measured label.

`python/examples/room_demo.py` draws a furnished room with walls, a door swing, a window, labels,
and dimensions — a working template for real drawings. The raw `Bridge` class remains available
(also as `doc.bridge`) for anything the ergonomic layer does not wrap.

The socket is `$TMPDIR/librecad-pybridge` (usually `/tmp/librecad-pybridge`, mode 0600); override with the `LC_PYBRIDGE_SOCKET` environment variable, which both the
plugin and the client honour. `python/smoke_test.py` is a working end-to-end example — it runs
against a live LibreCAD session or against the stub server (`make test-socket` does the latter).
One client at a time; a disconnect leaves the session running, so consecutive scripts can share
one session (and therefore one undo step).

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
  lc_bridge_server.{h,cpp}   QLocalServer transport serving the dispatcher
python/
  pyproject.toml             pip packaging for the client (pip install -e python/)
  lcbridge.py                Python client, stdlib only: Bridge (protocol) +
                             Document/Entity (ergonomic API)
  smoke_test.py              end-to-end checks of the protocol layer
  api_test.py                end-to-end checks of the ergonomic layer
  examples/room_demo.py      a furnished room drawn through the API
tools/loadtest/      QPluginLoader harness, used by `make check`
tools/dispatchtest/  stub Document_Interface + runner (`make test`); --serve mode
                     serves the stub over the socket for `make test-socket`
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

Milestone 5: drive a real floorplan through the API — the honest test of whether it is pleasant
to use. Candidates that may fall out of that: a `prompt_selection`
operation (deliberately interactive, see findings risk 8), MTEXT support if LibreCAD grows it in
the plugin interface, and block insert workflows.

The architecture note, for context. `docs/findings.md` risk 7 establishes that a
plugin cannot hold a `Document_Interface*` past the end of `execComm()`: the object is
stack-allocated by LibreCAD for the duration of the call and the concrete class is not exported.
So the RPC server cannot live in the background and reach into the document when a request
arrives. Instead `execComm()` has to *be* the server loop — the menu entry starts the bridge,
serves requests while pumping Qt events, and returns when the client quits.

That is still Option A, and it still keeps Python out of LibreCAD's process. It also means a whole
scripting session is a single undo step, which falls out of risk 1, and which is the intended
behaviour here.
