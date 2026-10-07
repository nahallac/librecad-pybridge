# Findings

Everything below was established on this machine against LibreCAD 2.2.1.5-3 (Arch) and the
upstream source at tag `v2.2.1.5`. Each item names its evidence so it can be rechecked after a
LibreCAD or Qt update.

Nothing here is now marked unverified; what is still unknown is collected under "Risks still
open" at the end.

---

## Build environment

| Fact | Value | Evidence |
|---|---|---|
| LibreCAD | 2.2.1.5-3 | `pacman -Q librecad` |
| Qt | 5.15.19 | `qmake -v`; `ldd` on the stock plugins shows `libQt5*` |
| `moc`, `qmake`, `uic`, `rcc` | `/usr/bin/`, owned by `qt5-base` | `pacman -Ql qt5-base` |
| C++ standard | C++17 | upstream `common.pri` |
| Installed headers | `/usr/include/librecad/{document_interface.h,qc_plugininterface.h}` | — |

**`qt5-tools` is not needed.** The handoff listed it as a prerequisite, but `qt5-base` already
provides `moc`, `qmake`, `uic`, and `rcc`; `qt5-tools` only adds Designer, Linguist, and friends.
Milestone 1 was built with nothing installed beyond what was already on the box.

**The installed headers are trustworthy.** `/usr/include/librecad/document_interface.h` is
byte-identical to `librecad/src/plugins/document_interface.h` at tag `v2.2.1.5`, so compiling
against the installed copy cannot produce a vtable mismatch.

The LibreCAD binary exports `Doc_plugin_interface::addMText`, `::addToUndo`, and
`::updateEntity`, none of which appear in the plugin-facing header. These are members of the
concrete `Doc_plugin_interface` class declared *after* the `Document_Interface` virtuals, not
hidden additions to the interface — see `librecad/src/main/doc_plugin_interface.h:82,100,132`.
They are not callable from a plugin, and `addMText` in particular means **plugins can create
TEXT but not MTEXT**. Dimensions are worse off still: `newEntity()` has every `DIM*` case
commented out and `updateData()` ignores dimension types, so **DIMENSION entities cannot be
created or edited through the plugin interface at all**. The Python API's `dim_*()` methods draw
dimensions from lines and text instead — correct to measure and print, but plain geometry. For
real DIMENSION and HATCH entities the bridge goes around the plugin API entirely; see "Native
access" below.

## Native access: around the plugin API from in-process

Two facts make more than the plugin API reachable:

1. **The command widget is drivable.** `QG_CommandWidget::handleCommand(QString)` is a public
   slot on a widget named `QG_CommandWidget`, findable from the main window Qt gives every
   `execComm()` call. Feeding it a line is exactly a typed command: it starts real LibreCAD
   actions, and coordinate lines (`"10,20"`) answer their point prompts synchronously
   (`RS_EventHandler::commandEvent` parses them with `RS_Math::eval`, so C-locale decimals work).
   Commands exist for everything, `dimaligned` through `hatch` (`librecad/src/cmd/rs_commands.cpp`).
2. **LibreCAD is open source, and its source is on the machine.** On Arch, `librecad-debug`
   installs the exact sources of the installed binary (558 headers, 757 `.cpp`, and the
   uic-generated `ui_*.h`) at `/usr/src/debug/librecad/LibreCAD`. The native layer is compiled
   against them, so it uses LibreCAD's classes as the C++ types they are: `Plugin_Entity::getEnt()`
   gives the `RS_Entity*` behind a `Plug_Entity`, `RS_Entity::setSelected()` is a virtual call
   (the `RS_EntityContainer` override is picked by the vtable), `QG_CommandWidget::handleCommand()`
   is called directly, and `QG_DlgHatch`'s `Ui` members are filled by name at compile time.
   Hatching consumes the current selection and the plugin API has no selection setter, which is
   why selection is needed at all.

   Two consequences. First, everything so far binds through vtables and inline accessors, so the
   plugin has **no undefined LibreCAD symbols** and still loads outside LibreCAD (`make check`
   relies on that). The executable *is* linked `--export-dynamic` (11k+ dynamic FUNC symbols:
   `RS_Modification::offset/mirror/trim/explode`, `RS_Document::startUndoCycle/endUndoCycle`,
   `QC_ApplicationWindow::slotFileOpen(QString)`, ...), so non-virtual engine calls are available
   the same way when needed; each one adds a symbol that must resolve when LibreCAD loads the
   plugin. Second, layouts and vtables are only known to match the version compiled against, so
   `NativeBridge` compares `QCoreApplication::applicationVersion()` (set from `LC_VERSION`,
   `"v2.2.1.5"` for 2.2.1.5) with the build-time `LIBRECAD_VERSION` before touching anything;
   `native_status` reports both as `built_against` and `running`.

   Earlier the same three hooks were reached by `dlsym` on mangled names and by reading the
   `Plugin_Entity` layout by hand; the headers replace that guesswork.

On top of those, `lc_bridge_native.cpp` implements:

- `exec_command` — raw command injection.
- `select_entities` — set the drawing selection to a list of entity handles. Container entities
  (polylines etc.) get the container override so their members are selected too, which the hatch
  action's `ResolveAll` walk requires.
- `cmd_dim` — escape, dimension command, three coordinates, escape. Verified by counting
  entities of the expected DIM type before and after.
- `cmd_hatch` — select the boundary, arm a 25 ms timer that waits for the modal `QG_DlgHatch`,
  fill `cbPattern`/`leScale`/`leAngle`/`cbSolid` by object name, accept it, then run `hatch`.
  The timer fires inside the dialog's own `exec()` loop. Verified by counting HATCH entities.
- `mod_offset`, `mod_mirror`, `mod_explode`, `mod_trim` — `RS_Modification`, the class the
  modify actions delegate to, constructed on the application window's current `RS_Document`
  and `RS_GraphicView` (`QC_ApplicationWindow::getAppWindow()`). These are the plugin's first
  *bound* symbols: eight `U` entries (`RS_Modification::{ctor,offset,mirror,explode,trim}`,
  `QC_ApplicationWindow::{getAppWindow,getDocument,getGraphicView}`) that the loader resolves
  from the executable when LibreCAD loads the plugin. Binding is lazy, so `make check`'s
  QPluginLoader run outside LibreCAD still succeeds. offset/mirror/explode act on the current
  selection, which the operation sets from the given handles first; each returns the entities
  it created, found by diffing engine entity identities before and after. `RS_Modification`
  wraps its work in `LC_UndoSection`, which nests in the session's outer section like the
  injected commands do. Verified live 2026-10-06 (offset/mirror/explode/trim, including
  `both`).
- `undo_checkpoint`, `undo`, `redo` — `RS_Undo` counts nested `startUndoCycle()` calls in
  `refCount`; `execPlug()`'s `LC_UndoSection` holds it at 1 for the whole session. A checkpoint
  is `endUndoCycle()` (1 → 0, the cycle is kept if it has undoables); `undo`/`redo` checkpoint
  and then call `RS_Undo::undo()/redo()`. The next cycle is opened **lazily**: `startUndoCycle()`
  discards the redo list (it drops every cycle past `undoPointer`), so opening one right after
  an undo would make redo impossible. The dispatcher opens it before the first operation that is
  not in its read-only set. A session that ends with the cycle closed leaves `execPlug()`'s
  `endUndoCycle()` unmatched; `RS_Undo` handles that with a debug warning and no state change.
  All virtual via `RS_Document`, so no new symbols. Found the hard way: the first version
  reopened eagerly and `redo()` returned 0.
- `file_info`, `file_save`, `file_save_as` — `RS_Document::getFilename()/isModified()` and
  `RS_Graphic::saveAs(path, type, force=true)` (virtual). `force` matters: `RS_Graphic::save()`
  writes only when the modified flag is set, and `saveAs` with `force` sets it. The format comes
  from `RS_FileIO::detectFormat(path, false)` (bound) unless named.
- `file_open`, `file_new` — handled by `BridgeServer`, not the dispatcher, because they cannot be
  done inside a session: `Document_Interface` is bound to one `RS_Document` for the life of
  `execComm()` (risk 7), and loading a file into the *current* document is unsafe anyway —
  `RS_Graphic::open()` calls `newDoc()`, which deletes every entity while `RS_Undo`'s list still
  points at them (LibreCAD only does this on a freshly created window). So the server
  acknowledges, stops with a `SessionRestart` pending, and the plugin, after `execComm()` has
  returned and `execPlug()` closed its undo section, runs `QC_ApplicationWindow::slotFileOpen()`
  / `slotFileNewNew()` from a zero-timer and triggers its own menu `QAction` (LibreCAD parents it
  to the plugin object) to start a new session on the now-active window. The Python client waits
  for the old socket to disappear and reconnects; entity handles carry a generation number so
  the old ones go stale. Verified live 2026-10-06.
- Auto-start (`LC_PYBRIDGE_AUTOSTART`) — the plugin is instantiated at load, before the main
  window exists, so its constructor polls every 100 ms until `hasActiveDocument()` (app window
  with a document) and then triggers the same `QAction`. The session-ended `QMessageBox` becomes
  a status-bar message in that mode: nothing would dismiss a modal box in an unattended run.

**Verified live 2026-10-05** against LibreCAD 2.2.1.5: `cmd_dim` produced DIMALIGNED and
DIMLINEAR entities and `cmd_hatch` a HATCH, through a real bridge session
(`python/examples/native_demo.py`). One detail found only at runtime: the command widget's
objectName is `"Command"` (set by `lc_widgetfactory.cpp`), not the `.ui` default, so it is
located by class name.

Fragility is the price: class layouts, vtables, widget class names, and command spellings are
all LibreCAD internals with no compatibility promise. Everything fails soft — a version mismatch
or a missing widget turns the operations into `"unavailable"` errors (which is also how they
behave against the offline stub, whose tests assert exactly that). A LibreCAD update therefore
means: install its sources, rebuild, and if `applicationVersion()` changed, pass the new
`LIBRECAD_VERSION`. Undo still collapses to one step per session: the injected actions' undo
cycles nest inside `execPlug()`'s outer `LC_UndoSection` like everything else.

Dimension appearance follows the drawing's dimension variables (`$DIMTXT`, `$DIMASZ`, `$DIMEXO`,
`$DIMEXE`, ...), which `addVariable()` can set through the ordinary `set_variable` operation.

---

## Risk 1 — undo integration: resolved, better than hoped

**All document changes made during one `execComm()` call collapse into a single undo step.**

`QC_ApplicationWindow::execPlug()` opens an undo section around the entire plugin call:

```cpp
LC_UndoSection undo(currdoc);
plugin->execComm(&pligundoc, this, action->data().toString());
```

Each `Document_Interface::add*()` opens its own `LC_UndoSection` too, but
`RS_Undo::startUndoCycle()` is reference counted — only the outermost call allocates a cycle, and
`endUndoCycle()` only commits when the count returns to zero. The inner sections therefore nest
into `execPlug()`'s cycle rather than creating their own.

Consequences for the bridge:

- One menu invocation = one Ctrl+Z, no matter how many entities were drawn. A script that draws a
  whole floorplan is undone in one press. This is the behaviour we wanted and did not expect.
- Granularity cannot be made finer from inside `execComm()`. There is no way to close the outer
  cycle early, so a plugin cannot offer per-operation undo. If finer steps are ever wanted, they
  have to come from separate `execComm()` invocations.
- Calls made *outside* `execComm()` get per-call undo steps instead, because then each
  `add*()`'s own `LC_UndoSection` is the outermost one. See risk 7 for why that path is not
  usable anyway.

Not covered by undo:

- `setLayer()` creates a missing layer via `RS_Graphic::addLayer` → `RS_LayerList::add`, neither
  of which registers an undoable. **A layer created by a script survives undo.** The undo probe
  demonstrates this.
- `deleteLayer()` likewise removes a layer with no undo registration.
- `addBlockfromFromdisk()` has no undo section at all.
- `Plug_Entity::updatePolylineData()` mutates the polyline in place without registering an
  undoable and without updating the view — the one modification call in the API that is neither
  undoable nor self-refreshing. Prefer rebuilding the polyline with `addPolyline()`.

Everything else is covered: all other `add*()` calls, `removeEntity()`, `updateData()` (via
`updateEntity`), and `move`/`moveRotate`/`rotate`/`scale` (via `addToUndo`).

**Confirmed in the running application 2026-10-04.** A probe action (since removed from the
plugin menu) drew nine entities through eight separate `add*()` calls, and a single Ctrl+Z removed
all nine. The reference-counted nesting behaves as the source says.

---

## Risk 3 — menu entry points: resolved

`menuEntryPoint` is **`plugins_menu`**. All eleven stock plugins use that exact string and
nothing else; it is visible in each `.so` with `strings -el`, and every upstream plugin's
`getCapabilities()` uses it.

`menuEntryPoint` is not limited to existing menus. `loadPlugins()` splits it on `/` and creates
missing menus as it walks the path, so `"plugins_menu/Python"` would nest a submenu.

**`execComm()`'s `cmd` argument is the `menuEntryActionName` of the invoked entry.**
`loadPlugins()` does `actpl->setData(loc.menuEntryActionName)` and `execPlug()` passes
`action->data().toString()`. Every stock plugin ignores `cmd`, which is why this was not
documented anywhere — but it means one plugin can register several menu entries and dispatch on
`cmd`, which is what `LC_PyBridge::execComm()` does.

Plugin menu actions are connected to `windowsChanged(bool)`, so they are disabled when no
drawing is open. That is the only thing standing between a plugin and
`Doc_plugin_interface::getAllEntities()`, which dereferences its document pointer without a null
check.

---

## Risk 4 — install requires root: resolved, it does not

`RS_System::getDirectoryList("plugins")` includes `$HOME/.<QC_APPDIR>/plugins`, and LibreCAD is
built with `QC_APPDIR=librecad` (`librecad/src/src.pro:45`, `CMakeLists.txt:14`). So
**`~/.librecad/plugins/` works and no `sudo` is needed.**

Confirmed on this machine: with the plugin installed there, a running LibreCAD maps
`/home/jonathan/.librecad/plugins/liblc_pybridge.so` alongside the eleven stock plugins
(`/proc/<pid>/maps`). `librecad -d 6` lists `/home/jonathan/.librecad/plugins` as the **first**
directory searched.

`loadPlugins()` skips a plugin whose *filename* has already been loaded, and the user directory
is searched first, so a file in `~/.librecad/plugins/` shadows a system plugin of the same name.
Useful for overriding; a hazard if a name collides by accident.

---

## Risk 7 — new: `Document_Interface` is only valid during `execComm()`

This was not in the handoff and it constrains the RPC architecture directly.

`execPlug()` stack-allocates the document interface:

```cpp
Doc_plugin_interface pligundoc(currdoc, w->getGraphicView(), this);
LC_UndoSection undo(currdoc);
plugin->execComm(&pligundoc, this, action->data().toString());
```

The object is destroyed when `execComm()` returns, and `Doc_plugin_interface` is internal to the
LibreCAD executable — it is not exported, and a plugin cannot construct one. So:

- **A plugin cannot hold a usable `Document_Interface*` past the end of `execComm()`.** Storing
  the pointer and using it later from a timer, thread, or socket callback is a dangling-pointer
  bug, not a slow path.
- A background RPC server that owns the socket across invocations therefore cannot touch the
  document on its own. Only the user, via the menu, can open a window in which document access
  is legal.

This reshapes Option A from the handoff. The workable shape is for `execComm()` itself to be the
server loop: the menu entry starts the bridge, `execComm()` accepts a connection and serves
requests while pumping Qt events, and returns when the client sends a quit message or
disconnects. That keeps every document call inside the legal window and, per risk 1, makes the
whole scripting session one undo step.

The alternative — one `execComm()` per request — would need a user click per request and is not
viable.

---

## API details worth knowing

- **`addArc()` takes degrees**, not radians; `doc_plugin_interface.cpp` applies `deg2rad`
  internally. Nothing else in the API takes an angle in degrees: `addEllipse`, `addText`, and
  `Plug_Entity::rotate`/`moveRotate` all pass their angles straight through to the engine as
  radians.
- **Undo granularity favours batch calls anyway.** `addLines()` and `addPolyline()` create their
  geometry in one call; per-line `addLine()` loops cost a round trip each in the eventual RPC
  protocol even though the undo result is identical.
- **Ownership.** `getAllEntities()`, `getSelect()`, and `getSelectByType()` fill a caller-owned
  `QList<Plug_Entity*>` with heap-allocated entities. The caller deletes both the entities and
  the list. `Plug_Entity` has a virtual destructor, so `qDeleteAll()` is correct.
- **`Plug_Entity` pointers are invalidated by modification.** `move`/`rotate`/`scale`/`moveRotate`
  clone the entity, register the clone, and re-point the wrapper at the clone; the original is
  marked undone but the wrapper is still the only handle to it. Do not cache raw entity pointers
  across modifications — re-query instead.
- **`EDATA` keys are not unique.** `STARTZ` and `ENDZ` are both `30`, and `CLOSEPOLY` and
  `COLCOUNT` are both `70` — a polyline's closed flag and an insert's column count share one key.
  So an attribute wrapper has to map names to keys per entity type, not with one global table.
- **`DPI::EntColor` is a stub** (`enum EntColor { das };`) — ignore it. Colors are plain ints:
  `-1` ByLayer, `-2` ByBlock, otherwise 24-bit RGB packed as an integer.
- **Text styles** are passed by name as a string; `"standard"` is what the stock plugins use.

---

## API detail — `getAllEntities()` reports undone entities

`Doc_plugin_interface::getAllEntities()` iterates the container with no `isUndone()` check, and
LibreCAD keeps removed entities in the container for undo. So after a `remove()` (or anything the
modify tools replace), the plugin API still lists the dead entity: a census of a drawing with one
trimmed line reports two LINEs. Found 2026-10-06 while verifying the modify operations. With the
native layer, every census in the dispatcher (`get_entities`, `get_bbox`, the type counts, the
select-all pass, and the created-entity diff) skips entities whose `RS_Undoable::isUndone()` is
true; without it, the quirk shows through unchanged.

## Risk 8 — new: there is no way to read the current selection

`getSelect()`, `getSelectByType()`, and `getEnt()` are **prompts, not queries**. Each one starts an
interactive LibreCAD action and waits:

```cpp
QC_ActionGetSelect* a = new QC_ActionGetSelect(*doc, *gView);
gView->killAllActions();
gView->setCurrentAction(a);
QEventLoop ev;
while (!a->isCompleted()) { ev.processEvents(); ... }
```

So "read the current selection", which the handoff listed as a success criterion, is not something
the plugin API offers. The only non-interactive query is `getAllEntities()`, and the attribute hash
it reports through `Plug_Entity::getData()` carries no selected flag — only `VISIBLE`.

What the bridge can offer instead:

- `get_entities`, filtered by type, as a non-interactive census. This is what the dispatch layer
  exposes.
- A deliberately interactive `prompt_selection` operation later, which blocks until the user picks.
  That matches what the API actually does and is honest about the wait.
- **Superseded by the native layer (2026-10-06):** with the engine headers in reach,
  `RS_Entity::isSelected()` is one virtual call away, so `entity_selected` and
  `get_entities {selected_only: true}` read the real selection; `entity_bbox` / `get_bbox` read
  `RS_Entity::getMin()/getMax()` the same way. Both are gated like every native operation.

Note also that `killAllActions()` call: a prompt cancels whatever the user was in the middle of.

---

## Risk 9 — new: `Plug_Entity::getEntityType()` reports the wrong enumeration

Found by running the dispatch self-test against a real drawing: every entity came back mislabelled
— a POINT as IMAGE, a LINE as OVERLAYBOX, a CIRCLE as INSERT — while the entity *count* was right.

Two things are going on, and the first explains the second.

**`Plugin_Entity` does not derive from `Plug_Entity`.** They are unrelated classes with
hand-matched vtable layouts, bridged by `reinterpret_cast`:

```cpp
Plugin_Entity *pe = new Plugin_Entity(e, this);
sel->append(reinterpret_cast<Plug_Entity*>(pe));
```

Nothing enforces that the two declarations stay in step, and they have not. The plugin-facing
header declares

```cpp
virtual int getEntityType();                     // document_interface.h
```

while the implementation declares

```cpp
virtual RS2::EntityType getEntityType();         // doc_plugin_interface.h:58
```

`RS2::EntityType` is a different enumeration in a different order from `DPI::ETYPE`, so the value
that arrives through the plugin interface is an RS2 value being read as a DPI one. The overlap is
what makes it dangerous: the result is not obviously garbage, it is a plausible wrong type.

| Entity | `RS2::EntityType` | read as `DPI::ETYPE` |
|---|---|---|
| POINT | `EntityPoint` = 6 | `IMAGE` = 6 |
| LINE | `EntityLine` = 7 | `OVERLAYBOX` = 7 |
| POLYLINE | `EntityPolyline` = 8 | `SOLID` = 8 |
| ARC | `EntityArc` = 10 | `TEXT` = 10 |
| CIRCLE | `EntityCircle` = 11 | `INSERT` = 11 |
| ELLIPSE | `EntityEllipse` = 12 | `POLYLINE` = 12 |
| TEXT | `EntityText` = 17 | nothing — `UNKNOWN` |

**Use the attribute hash instead.** `Plugin_Entity::getData()` inserts the correct `DPI::ETYPE`
explicitly in every branch of its switch, so that value is reliable. `lcbridge::entityType()`
reads it, and nothing in this project calls `getEntityType()`.

The mismatched return type also means the declaration a plugin compiles against and the function it
actually calls disagree, which is formally undefined behaviour. It happens to work because both
types are 32-bit and returned in the same register. A LibreCAD change here would break silently
rather than fail to link, which is a good reason to keep reading the type from the hash.

`tools/dispatchtest`'s stub returns RS2 values from `getEntityType()` on purpose, so the self-test
reproduces this failure if anyone switches back. Reverting the fix turns 51 passes into the same
32-passed/1-failed result seen against the real drawing.

---

## API details confirmed while building the dispatch layer

- **`DPI::ETYPE` cannot be named directly.** Namespace `DPI` declares both an enum type `ETYPE` and
  an `EDATA` enumerator `ETYPE = 0`, and the enumerator wins ordinary name lookup. The type has to
  be spelled `enum DPI::ETYPE`, which is why `Document_Interface::newEntity()` is declared that
  way. Using `int` for entity types avoids the problem entirely.
- **`newEntity()` + `addEntity()` is a double-free hazard.** `Plugin_Entity`'s destructor deletes
  the underlying `RS_Entity` when `hasContainer` is false, and that is exactly the case for a
  wrapper from `newEntity()`. After `addEntity()` hands the entity to the document, deleting the
  wrapper deletes an entity the document owns. The wrapper has to be leaked. Wrappers from
  `getAllEntities()` are the opposite case — `hasContainer` is true, so deleting them is correct
  and required. The dispatch layer avoids `newEntity()` for this reason; the `add*()` calls cover
  everything it can create anyway.
- **`entity_update` invalidates its handle; move/rotate/scale do not.**
  `Plugin_Entity::move`, `rotate`, `scale`, and `moveRotate` all clone, register the clone through
  `addToUndo()`, and then re-point the wrapper at the clone, so the wrapper stays usable.
  `updateData()` does the same cloning but **never re-points the wrapper**, leaving it addressing
  an entity that has been marked undone. The dispatcher drops the handle after `entity_update` and
  after `entity_remove`, so a stale use is refused rather than silently touching a removed entity.
- **Arc angles are inconsistent between create and read.** `addArc()` takes degrees, but
  `getData()` reports `STARTANGLE`/`ENDANGLE` for an arc in radians, and `updateData()` expects
  radians too. The dispatch layer takes radians everywhere and converts for `addArc()` alone.
- **`Plug_Entity::getEntityType()` is declared virtual but not defined in the headers**, and being
  the class's key function, its absence means no vtable or typeinfo for `Plug_Entity` is emitted
  either. That is fine inside the plugin, which gets all three from the LibreCAD binary, but a
  standalone test binary that derives from `Plug_Entity` has to define it. `tools/dispatchtest`
  does. Do not call it for the entity's type — see risk 9.
- **`Plugin_Entity::updateData()` has a harmless upstream bug**: the `STARTANGLE` branch for arcs
  calls `hash.take(DPI::STARTANGLE)` twice, so the second call yields a default `QVariant`. The
  result is assigned to a local that is never used again, so nothing is affected.

---

## Risks still open

- **Risk 2 — `execComm()` runs on the GUI thread.** Addressed by the transport design and so far
  borne out: `BridgeServer::serve()` runs a nested `QEventLoop`, so LibreCAD keeps painting and
  responding while a session is open, and each request is handled as a socket event on the GUI
  thread. A single long-running *request* (a huge batch) still blocks the UI for its duration;
  nothing pumps mid-request. Not yet measured under load.
- **Risk 5 — ABI fragility.** Unchanged. Qt 5.15 is end-of-life; an Arch move to Qt 6 forces a
  rebuild. The `.pro` file hard-errors on a non-Qt-5 qmake so the failure is loud rather than a
  silently unloadable plugin.
- **Risk 6 — interactive prompts re-enter the event loop.** Confirmed from the source, still
  untested at runtime. `getPoint`, `getEnt`, `getSelect`, and `getSelectByType` each construct a
  `QC_Action*`, call `gView->killAllActions()`, install the action, and then spin a
  `QEventLoop` until it completes. Calling one from inside the server loop nests event loops, and
  it also destroys whatever action the user had in progress.
