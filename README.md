# LibreCAD Python bridge

Scripting for LibreCAD: drive a running LibreCAD from Python — geometry, layers, blocks,
selection, real dimensions and hatches, the modify tools, undo, files — in the drawing the user
has open, or in a LibreCAD you launch yourself, windowed or headless.

Spun out of a house-layout project that needed scripted floorplans. `docs/findings.md` records
what has been established about LibreCAD's plugin API and internals, with evidence for each claim.

**Status: in use.** A Qt5 C++ plugin exposes LibreCAD as a table of named JSON operations over a
Unix domain socket; a stdlib-only Python client wraps it (`Document`, `Entity`). The plugin API
(`Document_Interface`) covers geometry and layers; everything beyond it — dimensions, hatches,
selection reads, offset/mirror/trim/explode, undo steps, save/open — comes from a *native layer*
compiled against LibreCAD's own source, version-gated at runtime. Validated against LibreCAD
2.2.1.5 on Arch; other agents drive real floorplan work through it.

**When not to use it:** to *generate* a DXF with no one looking, write the file with `ezdxf` and
open it in LibreCAD. The bridge is for interacting with the running application: seeing results
as they happen, sharing a drawing with a person, undo integration, reading what someone drew.

## Build and install

Required: `librecad`, `qt5-base`, and the **LibreCAD source tree of the installed binary**. The
native layer (real dimensions and hatches, see below) is compiled against LibreCAD's own headers.
On Arch, `librecad-debug` installs exactly that at `/usr/src/debug/librecad/LibreCAD`, which is the
default; elsewhere pass `LIBRECAD_SRC=<built checkout>` to qmake (it must be a built tree, the
generated `ui_*.h` files are needed) and `LIBRECAD_VERSION=<string>` if the binary reports
something other than `v2.2.1.5` (`native_status` shows what it reports). `qt5-tools` is **not**
needed.

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

Two ways to get a session:

- **By hand:** open a drawing, then **Plugins → Start Python bridge**. Plugin menu entries are
  disabled while no drawing is open.
- **Unattended:** with `LC_PYBRIDGE_AUTOSTART=1` in LibreCAD's environment the plugin starts the
  session itself as soon as a drawing is open:

  ```bash
  LC_PYBRIDGE_AUTOSTART=1 librecad plate.dxf &
  ```

  Add `QT_QPA_PLATFORM=offscreen` for no window at all. `Document.launch(drawing, headless=...)`
  in the Python client does exactly this and returns a connected `Document` (the process is on
  `doc.process`; `terminate()` it when done — `shutdown()` only ends the session).

A small status strip appears in the corner of the LibreCAD window; the session serves Python
clients until you press *Stop* or a client sends `{"op": "shutdown"}`. LibreCAD stays usable while
the session runs. Everything the session does is **one undo step** unless the script calls
`undo_checkpoint()` between stages.

## Driving it from Python

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

### The native layer

LibreCAD's plugin API stops at plain geometry. The plugin runs inside LibreCAD's process and is
compiled against LibreCAD's source, so it can use the engine directly. Everything in this section
needs that, and is therefore tied to one LibreCAD version: at runtime the plugin compares the
version it was built against with the one running and, on a mismatch, these operations report
`unavailable` instead of failing strangely (`native_status` shows both strings). The stub server
used by the tests reports the same.

**Real dimensions and hatches.** `cad_dim_*()` and `cad_hatch()` create **real DIMENSION and
HATCH entities** — associative, editable with LibreCAD's own tools — by driving LibreCAD's command
line from in-process (and auto-filling the hatch pattern dialog). Appearance follows the drawing's
dimension variables, reachable via `set_variable` (`$DIMTXT`, `$DIMASZ`, ...).

```python
doc.set_variable("$DIMTXT", 60.0)                    # dimension text height
doc.cad_dim_horizontal((0, 0), (1200, 0), (600, -150))
doc.cad_dim_aligned(p1, p2, dimline_point)
boundary = [e for e in doc.entities(types=["POLYLINE"]) if e["layer"] == "HOLE"]
doc.cad_hatch(boundary, pattern="ANSI31", scale=10.0)
doc.exec_command("zoomauto")                         # raw command-line access
```

**Selection and extents.** `entity.selected`, `entity.bbox()`, `doc.bbox()` (union over given
entities or the whole drawing), and `doc.entities(selected_only=True)` — the current selection,
straight from the engine. The plugin API can only *prompt* for a selection.

**Modify tools**, run through the engine class behind them (`RS_Modification`), not the command
line: `doc.offset(entities, distance, side)`, `doc.mirror(entities, p1, p2, copy=False)`,
`doc.explode(entities)`, and `doc.trim(entity, trim_point, limit, limit_point, both=False)`. Each
returns the entities it created; entities the engine replaced (trimmed lines, exploded polylines,
mirrored or offset originals when not kept) go stale like after `update()`.

```python
wall = doc.entities(types=["LINE"])[0]
inner, = doc.offset([wall], 150, side=(0, 0))      # one parallel line, toward the origin
doc.mirror([wall, inner], (2000, 0), (2000, 1), copy=True)
kept, = doc.trim(wall, trim_point=(10, 0), limit=inner, limit_point=(500, 0))
```

**Files and undo:**

```python
doc.file_info()                         # {"path": "...", "modified": bool}
doc.save()                              # to its own file; no_filename if unnamed
doc.save_as("/tmp/plate.dxf")           # DXF 2007 by default; format="dxf2000" etc.
doc.undo_checkpoint()                   # close the current undo step, start another
doc.undo(); doc.redo()                  # whole steps, as Ctrl+Z would
doc.open("/path/to/other.dxf")          # new window, new session, same Document object
doc.new()
```

A session is one undo step unless you checkpoint it; `undo()`/`redo()` work on whole steps, and
queries in between do not discard redo history. `open()` and `new()` are different from the rest:
a session is bound to one drawing (see the architecture note below), so they end the session,
have LibreCAD open the drawing in a new window and start a fresh session on it, and reconnect.
Every `Entity` from before is stale afterwards; the `Document` object carries over. If LibreCAD
shows a dialog during the open (an unreadable file, say), the restart waits for it and the client
times out.

`python/examples/native_demo.py` draws a hatched, dimensioned plate through all of this. See
`docs/findings.md`, "Native access", for how it works and what it depends on.

### Prompts and events

**Prompts** ask the person at LibreCAD for input and block until they answer:

```python
p = doc.prompt_point("Door hinge:", base=(0, 0))   # click, or type 10,20 on the command line
picked = doc.prompt_select("Walls to thicken, then Enter")   # list[Entity]
n = doc.prompt_int("How many shelves?", default=4)  # LibreCAD input dialogs
w = doc.prompt_real("Width:", default=600.0)
s = doc.prompt_string("Label:", timeout=30)         # None if unanswered after 30 s
```

Each returns `None` when cancelled. While a prompt waits, the session serves nothing else, so the
client lifts its socket timeout for the call; `timeout=` (seconds) instead has the plugin cancel
the prompt itself (native layer). Point and select prompts end whatever action the user had
running. `prompt_select` returns whatever is selected when the user finishes (Enter, Escape or a
right click all finish it; anything already selected counts) — LibreCAD's own `getSelect()`
reports every ending as a cancel, so the selection is read back through the native layer.

**Events** are pushed by the plugin once subscribed; nothing is sent before that:

```python
doc.on("selection_changed", lambda e: print("selected:", e["data"]["count"]))
doc.subscribe(["document_modified", "layer_changed"])
while True:
    for frame in doc.events(timeout=1.0):          # also runs the on() callbacks
        print(frame["event"], frame["data"])
```

| Event | Data | Source |
|---|---|---|
| `entity_count_changed` | `count`, `previous` (live entities) | polled* |
| `selection_changed` | `count`, `previous` (selected entities) | polled* |
| `layer_changed` | `current`, `layers` | polled |
| `document_modified` | `modified` | polled* |
| `view_changed` | `factor`, `offset` (zoom/pan) | polled* |
| `grid_changed` | `on` | polled* |
| `windows_changed` | `windows_left` (the active drawing window changed) | Qt signal* |
| `session_ending` | `reason`: `shutdown`, `file_open`, `file_new`, `stopped` | server |

Polled events are checked every 100 ms while subscribed and report changes only (the entity and
selection counts are one pass over the engine's entity list, skipping undone entities); none is
polled during a request. A client that subscribes and never reads lets frames pile up in
`doc.bridge.events`, a deque capped at 1000 (oldest dropped; `seq` gaps show it). Subscriptions
belong to the connection; `open()`/`new()` renew them on the new session.

### Drawn dimensions

The portable fallback: the API can also draw dimensions out of plain lines and text: extension lines, dimension line, tick or arrow
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

**Event frames.** After `{"op": "subscribe", "args": {"events": ["selection_changed"]}}` (or
`["*"]`) the server may also write unsolicited frames on the connection, same framing:

```json
{"event": "selection_changed", "seq": 4, "data": {"count": 2, "previous": 0}}
```

They have `"event"` and no `"ok"`, so a client tells them from responses by shape; one can arrive
between a request and its response (the Python `Bridge` queues those in `Bridge.events`). Each is
a whole line. Events a request causes are written after its response, except `session_ending`,
which precedes the response to the `shutdown`/`file_open`/`file_new` that ends the session.
`unsubscribe` takes the same `events` list (none: drop all); both reply `{"subscribed": [...],
"available": [...]}`.

**Prompts block.** `prompt_*` operations wait for the user, so the server answers nothing else
meanwhile: call them with no socket timeout, or pass `timeout_ms` to have the plugin cancel the
prompt. Replies are `{"cancelled": false, "point" | "entities" | "value": ...}` or
`{"cancelled": true}` (plus `"timed_out": true` when `timeout_ms` fired).

### Operations

`{"op": "operations"}` returns the live list. Grouped, with the Python method where it differs:

| Group | Operations |
|---|---|
| Meta | `ping`, `operations`, `batch`, `update_view`, `native_status` |
| Create | `add_point`, `add_line`, `add_lines`, `add_polyline`, `add_spline_points` (`add_spline`), `add_circle`, `add_arc`, `add_ellipse`, `add_text`, `add_insert`, `add_block_from_file` |
| Layers, blocks | `get_current_layer`, `set_layer`, `get_layers`, `delete_layer`, `get_layer_properties`, `set_layer_properties`, `get_blocks` |
| Query | `get_entities` (filters: `types`, `visible_only`, `selected_only`*), `entity_data`, `entity_polyline`, `release_handles`, `get_variable`, `real_to_string` |
| Entity edit | `entity_update`, `entity_set_polyline`, `entity_move`, `entity_rotate`, `entity_move_rotate`, `entity_scale`, `entity_remove`, `set_variable`, `unselect` |
| Selection, extents* | `select_entities` (`select`), `entity_selected`, `entity_bbox`, `get_bbox` |
| Command line* | `exec_command`, `cmd_dim` (`cad_dim*`), `cmd_hatch` (`cad_hatch`) |
| Modify* | `mod_offset`, `mod_mirror`, `mod_explode`, `mod_trim` |
| Undo* | `undo_checkpoint`, `undo`, `redo` |
| Files* | `file_info`, `file_save`, `file_save_as` |
| Session (server-level, not in the list) | `session`, `shutdown`, `file_open`* (`open`), `file_new`* (`new`) |
| Prompts (blocking) | `prompt_point`, `prompt_select`, `prompt_int`, `prompt_real`, `prompt_string` — `timeout_ms`* optional |
| Events (server-level, not in the list) | `subscribe`, `unsubscribe` (`on`, `events` in Python) |

\* native layer; `unavailable` on a version mismatch or the stub.

Still absent: `getEnt` and `getSelectByType`. `getEnt` dereferences its action after the action
stack has deleted it whenever the user cancels; `prompt_select` covers both.

## Layout

```
plugin/
  lc_pybridge.{h,cpp}        plugin entry point, menu action, auto-start,
                             session restart after file_open/file_new
  lc_bridge_dispatch.{h,cpp} the operation table over Document_Interface
                             and the native layer
  lc_bridge_native.{h,cpp}   the native layer: command line, selection,
                             hatch dialog, RS_Modification, undo cycles,
                             file save/open -- compiled against LibreCAD's
                             source tree (LIBRECAD_SRC), version-gated
  lc_bridge_selftest.{h,cpp} the fixed request sequence, shared by both runners
  lc_bridge_server.{h,cpp}   QLocalServer transport; session-level ops
  lc_bridge_events.{h,cpp}   push events: polled state diffs, Qt signals
python/
  pyproject.toml             pip packaging for the client (pip install -e python/)
  lcbridge.py                Python client, stdlib only: Bridge (protocol) +
                             Document/Entity (ergonomic API), Document.launch()
  smoke_test.py              end-to-end checks of the protocol layer
  api_test.py                end-to-end checks of the ergonomic layer
  examples/room_demo.py      a furnished room drawn through the API
  examples/native_demo.py    a hatched, dimensioned plate via the native layer
tools/loadtest/      QPluginLoader harness, used by `make check`
tools/dispatchtest/  stub Document_Interface + stub native layer
                     (fake_native.cpp) + runner (`make test`); --serve mode
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
so the claims in `docs/findings.md` can be rechecked without network access. The build itself
reads the full tree from `LIBRECAD_SRC`.

## Testing

`make check` runs everything that needs no LibreCAD: the dispatcher against the stub document,
the stub served over the socket with the Python tests driving it, and a QPluginLoader load of the
built plugin (which works outside LibreCAD because the native layer binds nothing at load time —
see findings). The native layer itself can only be verified live. The quick way:

```python
from lcbridge import Document
doc = Document.launch("some.dxf", headless=True)   # or headless=False to watch
...
doc.shutdown(); doc.process.terminate()
```

`python/examples/native_demo.py` is the live check for dimensions and hatches.

## What comes next

Still missing, roughly in order of value:

- Remaining modify tools through `RS_Modification`: move/rotate/scale with copies, stretch,
  fillet (round), chamfer (bevel), cut, bulk attribute change, revert direction.
- Creation gaps: `add_mtext` (the plugin API has `addMText`, unwrapped), `add_image`, the other
  dimension kinds (radial, diametric, angular, leader) through the command line.
- Layer state (freeze, lock, hide, rename) and block definition from entities.
- View and windows: zoom, visible area, `file_close`, listing and switching documents (every
  `open()` leaves its window behind), export to PDF/SVG.
- Queries: lengths, areas, intersections, nearest entity, stable entity ids across sessions.

The architecture note, for context. `docs/findings.md` risk 7 establishes that a plugin cannot
hold a `Document_Interface*` past the end of `execComm()`: the object is stack-allocated by
LibreCAD for the duration of the call and the concrete class is not exported. So the RPC server
cannot live in the background and reach into the document when a request arrives. Instead
`execComm()` has to *be* the server loop — the menu entry (or the auto-start) starts the bridge,
serves requests while pumping Qt events, and returns when the client quits. Python stays out of
LibreCAD's process. A session is therefore bound to one drawing, which is why `open()` and
`new()` restart the session, and why a session is one undo step until it is checkpointed.
