# LibreCAD Python bridge

Scripting for LibreCAD: drive a running LibreCAD from Python — geometry, layers, blocks,
selection, real dimensions and hatches, the modify tools, undo, files — in the drawing the user
has open, or in a LibreCAD you launch yourself, windowed or headless.

Spun out of a house-layout project that needed scripted floorplans. `docs/findings.md` records
what has been established about LibreCAD's plugin API and internals, with evidence for each claim.

**Status: in use.** A Qt5 C++ plugin exposes LibreCAD as a table of named JSON operations over a
Unix domain socket; a stdlib-only Python client wraps it (`Document`, `Entity`). The plugin API
(`Document_Interface`) covers geometry and layers; everything beyond it — real dimensions of every
kind, MTEXT, images, hatches, selection reads, the modify tools (offset, mirror, trim, explode,
move/rotate/scale with copies, stretch, fillet, chamfer, cut, bulk attributes), undo steps,
save/open/close and switching documents, layer state, block definition, geometry queries, zoom,
PNG/SVG/PDF export, blocking prompts and push events — comes from a *native layer* compiled
against LibreCAD's own source, version-gated at runtime. Validated against LibreCAD 2.2.1.5 on
Arch; other agents drive real floorplan work through it.

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

**Real dimensions and hatches.** `cad_dim()` and `cad_hatch()` create **real DIMENSION and
HATCH entities** — associative, editable with LibreCAD's own tools — by driving LibreCAD's command
line from in-process (and auto-filling the hatch pattern dialog); the `cad_dim_*()` helpers and
`add_hatch()` build the same entities directly (see "Creation through the engine" below). Appearance follows the drawing's
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

**More modify tools**, the same way: `doc.move()`, `rotate()`, `scale()`, `move_rotate()`,
`rotate2()` take a list of entities and `copies=0` (transform the originals, which are replaced
and go stale) or `copies=n` (keep them, add n copies at 1x ... nx the transformation) — unlike
`Entity.move()/rotate()/scale()`, which go through the plugin API, keep the handle, and make no
copies. `doc.stretch(corner1, corner2, offset)` works on the drawing by window, like the GUI
tool (handles of stretched entities die on the server; re-fetch). `doc.fillet(e1, p1, e2, p2,
radius)` and `doc.chamfer(e1, p1, e2, p2, length1, length2)` round or bevel a corner (`p1`/`p2`
on the parts to keep; `trim=True` by default; fillet's optional `corner` point picks the side,
default the midpoint of `p1` and `p2`). `doc.cut(entity, point)` splits a line/arc/ellipse in two
(a circle becomes one arc). `doc.change_attributes(entities, layer=, color=, width=, linetype=)`
takes entity-data formats (`color` also `"bylayer"`/`"byblock"`; `width` like `"0.25mm"`), and
`doc.revert_direction(entities)` swaps start and end. All return the entities they created.

```python
ring = doc.rotate([door], center=(0, 0), angle=math.pi / 6, copies=11)   # 11 copies
arc, *trimmed = doc.fillet(h, (800, 0), v, (0, 800), radius=100)
doc.change_attributes(trimmed, color=0xFF0000, linetype="DashLine")
```

One undo subtlety these ops handle for you: LibreCAD's undo cycle stores its entities in a set,
so an entity created and then replaced inside one undo step would come back on undo. The modify
ops therefore start an undo step of their own when (and only when) they replace an entity that
was created in the current step — draw-then-fillet is two undo steps.

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

A session is one undo step unless you checkpoint it (saving checkpoints too, so a saved drawing
stays saved when the session ends); `undo()`/`redo()` work on whole steps, and
queries in between do not discard redo history. `open()` and `new()` are different from the rest:
a session is bound to one drawing (see the architecture note below), so they end the session,
have LibreCAD open the drawing in a new window and start a fresh session on it, and reconnect.
Every `Entity` from before is stale afterwards; the `Document` object carries over. If LibreCAD
shows a dialog during the open (an unreadable file, say), the restart waits for it and the client
times out.

**View, documents, export:**

```python
doc.zoom_auto()                          # each zoom returns doc.view():
doc.zoom_window((0, 0), (2000, 1500))    # {"factor", "offset", "size", "visible", "center"}
doc.zoom_in(2.0, center=(500, 500)); doc.zoom_out(); doc.zoom_pan(100, 0)
doc.set_view(factor=0.5, center=(1000, 750))
doc.documents()                          # [{"index", "path", "active", "modified", ...}]
doc.activate_document("/path/to/b.dxf")  # or an index; new session on that window
doc.close_document(discard=True)         # {"remaining": n}
doc.export_image("/tmp/plan.png", 1600, 1200, border=20)    # png/jpg/bmp/svg by extension
doc.export_pdf("/tmp/plan.pdf", paper="A3", landscape=True)  # fitted onto one page
```

Zooms move the view only, not the undo stack — but LibreCAD flags the drawing modified when the
view changes (the view is saved with it). `activate_document()` and `close_document()` are
session-level like `open()`: the session ends and the client reconnects to a new one on the window
that is then active. `close_document()` refuses a drawing with unsaved changes unless
`discard=True` (LibreCAD would ask in a dialog); closing the last drawing leaves the `Document`
disconnected (`doc.connected` is False, further calls raise `BridgeError("disconnected")`).
Exports always show the whole drawing, not the current view: `export_image()` is File > Export's
renderer (`background="black"`, `black_white=True`, or `transparent=True` for raster formats);
`export_pdf()` is File > Export as PDF without its dialogs, on the drawing's paper and margins
unless `paper=` names one, fitted to the page unless `fit_to_page=False` (then the drawing's
paper scale and insertion base apply, as LibreCAD's own print does).
**Creation through the engine.** MTEXT, IMAGE, HATCH and every dimension kind, built directly
the way LibreCAD's own tools build them — no command line, no picks — and returned as `Entity`
objects. Angles are radians; dimension text is `None` for the measured value, `"<>"` inside it
stands for the measurement, `" "` hides it.

```python
note = doc.add_mtext("Line one\nLine two", (0, 0), 2.5)          # top-left attached by default
img = doc.add_image("site.png", (0, 0), width=4000)               # or scale= (units per pixel), height=
doc.cad_dim_aligned((0, 0), (3000, 4000), (-500, 2000))           # DIMALIGNED, label "5000"
doc.cad_dim_horizontal(p1, p2, dimline)                           # DIMLINEAR at angle 0
doc.cad_dim_linear(p1, p2, dimline, angle=math.radians(30))
circle = doc.entities(types=["CIRCLE"])[0]
doc.cad_dim_radial(circle, angle=math.pi / 4)                     # or (center, radius)
doc.cad_dim_diametric((500, 500), 120, text="<> TYP")
doc.cad_dim_angular(wall_a, wall_b, (300, 200))                   # LINE entities or point pairs
doc.cad_dim_leader([(0, 0), (200, 150), (400, 150)])              # arrow at the first point
doc.add_hatch([outline], pattern="ANSI31", scale=10)              # raises on an open boundary
```

- `cad_dim_angular` measures the one of the four sectors around the lines' intersection that
  contains the third point, with its arc through that point; the lines need not touch.
- `add_image` references the file (absolute path while the drawing is unnamed; LibreCAD stores it
  relative to the drawing's folder once the drawing has a file name). Any format Qt reads.
- `add_hatch` copies the boundary into the hatch, like LibreCAD's hatch tool; unlike `cad_hatch`
  it needs no selection or dialog, and an unknown pattern or a scale too large for the contour is
  an error instead of an empty hatch.
- `cad_dim()` still drives the command line; the `cad_dim_*` helpers use the direct path.
- `get_entities` / `entity_data` rows now carry what the plugin API leaves out: for dimensions
  `definition_point`, `text`, `label` (as drawn) and the kind's points (`extension_point1/2`,
  `definition_point2`, `definition_point1..4` and `center` for angular); `vertices` and `arrow`
  for leaders; `width`, `halign`, `valign`, `line_spacing`, `style` for MTEXT; `pattern`,
  `scale`, `angle`, `solid` for hatches. Points are `[x, y]`; these fields are read-only.

`python/examples/native_demo.py` draws a hatched, dimensioned plate through all of this. See
`docs/findings.md`, "Native access", for how it works and what it depends on.

**Layer state.** `doc.layer_state(name)` (`frozen`, `locked`, `print`, `construction`, and
`visible`, which is just "not frozen"), `doc.layer_states()` for every layer in one call,
`doc.set_layer_state(name, frozen=True, locked=False, ...)` (omitted flags are kept; goes through
the same layer-list calls as the layer panel's checkboxes, so the panel and the view follow), and
`doc.rename_layer(old, new)`. A rename is done in place, so entities stay on the layer; it is
refused for an empty or taken name and for layer `0`. Layer state is not in LibreCAD's undo
system, so none of this is undoable.

```python
doc.set_layer_state("FURNITURE", frozen=True)          # hide it
doc.rename_layer("WALLS", "WALLS-EXISTING")
{s["name"]: s["locked"] for s in doc.layer_states()}
```

**Blocks.** `doc.define_block(name, base_point, entities, remove=True, insert=False)` is Create
Block: the entities are copied into a new block with `base_point` as its origin, the originals are
removed (undoably; they go stale) unless `remove=False`, and `insert=True` also drops one INSERT at
the base point, returned as `(name, entity)`. The block definition itself is not undoable (LibreCAD
adds it to the block list without an undo record), so undoing the step brings the originals back
but leaves the block. `doc.rename_block(old, new)` renames the block and every INSERT naming it
(`RS_BlockList::rename` alone only updates inserts nested in other blocks; the drawing's inserts
need the second call the Block Attributes tool makes); not undoable.
`doc.remove_block(name)` is refused while any INSERT, in the drawing or in another block, refers
to it, and otherwise removes it undoably like the Remove Block tool: LibreCAD only flags the block
undone and keeps it in its list, so the bridge's `blocks` skips those, and the name stays taken
until the undo history lets go of it. `doc.block_entities(name)` returns the block's members (in
block coordinates) with ordinary handles; use them for reads, release them before removing the
block.

```python
a, b = doc.entities(types=["LINE"])[:2]
name, insert = doc.define_block("CORNER", (0, 0), [a, b], insert=True)
doc.rename_block("CORNER", "CORNER-A")
for member in doc.block_entities("CORNER-A"): print(member.type, member.length())
```

**Geometry queries.** `entity.length()` (None for text and hatches), `entity.area()` (circle, full
ellipse, closed polyline, bulges included; 0.0 for anything that encloses nothing),
`entity.intersections(other, on_entities=True)`, `doc.nearest_entity(point, types=None,
max_distance=None)` (the distance is `entity.distance`; frozen layers are skipped, locked ones are
not), `entity.nearest_point(point, on_entity=True)` (`((x, y), distance)`),
`entity.contains(point)` (closed polyline, circle, full ellipse; anything else is refused), and
`entity.id` / `doc.find_entity(id)`. The id is LibreCAD's own entity id: the same across
`entities()` calls (a handle is per call), but not saved in the file and replaced when LibreCAD
replaces the entity (`move()` and the like make a new one).

```python
room = doc.entities(types=["POLYLINE"])[0]
room.area(), room.length(), room.contains((1500, 1000))
wall = doc.nearest_entity((1500, -30), types=["LINE"], max_distance=100)
room.id, doc.find_entity(room.id)
```
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
| Layer state* | `get_layer_state` (`layer_state`), `get_layer_states` (`layer_states`), `set_layer_state`, `rename_layer` |
| Blocks* | `block_define` (`define_block`), `block_rename` (`rename_block`), `block_remove` (`remove_block`), `block_entities`; `get_blocks` also stops listing removed blocks |
| Geometry queries* | `entity_length` (`length`), `entity_area` (`area`), `intersections`, `nearest_entity`, `nearest_point`, `point_inside` (`contains`), `entity_id` (`id`), `find_entity` |
| View* | `get_view` (`view`), `zoom_auto`, `zoom_window`, `zoom_in`, `zoom_out`, `zoom_pan`, `zoom_previous`, `zoom_page`, `set_view` |
| Documents* | `list_documents` (`documents`); server-level: `activate_document`, `file_close` (`close_document`) |
| Export* | `export_image`, `export_pdf` |
| More modify* | `mod_move` (`move`), `mod_rotate` (`rotate`), `mod_scale` (`scale`), `mod_move_rotate` (`move_rotate`), `mod_rotate2` (`rotate2`), `mod_stretch` (`stretch`), `mod_round` (`fillet`), `mod_bevel` (`chamfer`), `mod_cut` (`cut`), `mod_change_attributes` (`change_attributes`), `mod_revert_direction` (`revert_direction`) |
| Prompts (blocking) | `prompt_point`, `prompt_select`, `prompt_int`, `prompt_real`, `prompt_string` — `timeout_ms`* optional |
| Events (server-level, not in the list) | `subscribe`, `unsubscribe` (`on`, `events` in Python) |
| Create via the engine* | `add_mtext`, `add_image`, `add_hatch`, `dim_aligned` (`cad_dim_aligned`), `dim_linear` (`cad_dim_linear`, `cad_dim_horizontal`, `cad_dim_vertical`), `dim_radial` (`cad_dim_radial`), `dim_diametric` (`cad_dim_diametric`), `dim_angular` (`cad_dim_angular`), `dim_leader` (`cad_dim_leader`) |

\* native layer; `unavailable` on a version mismatch or the stub.

Still absent: `getEnt` and `getSelectByType`. `getEnt` dereferences its action after the action
stack has deleted it whenever the user cancels; `prompt_select` covers both.

## Layout

```
plugin/
  lc_pybridge.{h,cpp}        plugin entry point, menu action, auto-start,
                             session restart after file_open/file_new/
                             activate_document/file_close
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

`python/examples/native_demo.py` is the live check for dimensions and hatches. Against a fresh,
isolated `$HOME`, write `[Startup]` / `FirstLoad=0` to `~/.config/LibreCAD/LibreCAD.conf` first:
otherwise LibreCAD's first-run dialog blocks the headless start and `launch()` times out.

## What comes next

Still missing, roughly in order of value:

- The plugin-API edits (`entity_update`, `entity_remove`, `entity_move`/`rotate`/`scale`) still
  have the undo ghost the modify ops avoid: edit an entity drawn in the same undo step, undo,
  and the original comes back. `NativeBridge::isolateReplacement()` is the fix; whether those
  ops should split undo steps is a design call (see findings, 2026-10-06).
- Queries across sessions: entity ids are per process (they are not stored in the file).

The architecture note, for context. `docs/findings.md` risk 7 establishes that a plugin cannot
hold a `Document_Interface*` past the end of `execComm()`: the object is stack-allocated by
LibreCAD for the duration of the call and the concrete class is not exported. So the RPC server
cannot live in the background and reach into the document when a request arrives. Instead
`execComm()` has to *be* the server loop — the menu entry (or the auto-start) starts the bridge,
serves requests while pumping Qt events, and returns when the client quits. Python stays out of
LibreCAD's process. A session is therefore bound to one drawing, which is why `open()` and
`new()` restart the session, and why a session is one undo step until it is checkpointed.
