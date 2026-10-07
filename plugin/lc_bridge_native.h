/*****************************************************************************/
/*  lc_bridge_native.h - in-process access to LibreCAD beyond the plugin API */
/*                                                                           */
/*  The plugin interface cannot create DIMENSION or HATCH entities, but the   */
/*  plugin runs inside LibreCAD's process, and LibreCAD is open source. This  */
/*  class is compiled against LibreCAD's own headers (engine, main, ui) and   */
/*  talks to its objects as the C++ types they are:                          */
/*                                                                           */
/*   - execCommand() feeds a line to QG_CommandWidget::handleCommand(),      */
/*     exactly as if typed. That runs real LibreCAD actions, which is how     */
/*     real dimensions get drawn.                                            */
/*   - setSelected() reaches the RS_Entity behind a Plug_Entity through       */
/*     Plugin_Entity::getEnt() and calls RS_Entity::setSelected(); the        */
/*     virtual call picks the RS_EntityContainer override by itself.         */
/*     Hatching consumes the current selection, and the plugin API has no     */
/*     selection setter.                                                     */
/*   - armHatchDialog() watches for the modal QG_DlgHatch that the hatch      */
/*     action opens, fills in pattern/scale/angle/solid through its Ui        */
/*     members, and accepts it.                                              */
/*                                                                           */
/*  This couples to LibreCAD's class layouts and vtables, which no release    */
/*  promises to keep. Everything binds through object vtables, not exported   */
/*  symbols, so the plugin still loads outside LibreCAD (make check); the     */
/*  constructor refuses to touch anything unless the running LibreCAD        */
/*  reports the version this plugin was built against, and then every        */
/*  operation reports "unavailable" instead of crashing. See                 */
/*  docs/findings.md, "Native access".                                       */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef LC_BRIDGE_NATIVE_H
#define LC_BRIDGE_NATIVE_H

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <optional>

class Document_Interface;
class Plug_Entity;
class QG_CommandWidget;
class QTimer;
class QWidget;
class RS_Document;
class RS_GraphicView;

namespace lcbridge {

//! What a session asked to happen after it ends: open a drawing in a new
//! window, or make a new drawing, and then start a fresh session on it.
//! Document_Interface is bound to one document for the life of execComm()
//! (findings risk 7), so a different drawing means a different session.
struct SessionRestart
{
    enum Kind { None, OpenFile, NewDrawing, ActivateWindow, CloseWindow };
    Kind kind {None};
    QString path;
    //! ActivateWindow: position in the MDI area's subWindowList().
    int index {-1};
    //! CloseWindow: drop unsaved changes instead of refusing.
    bool discard {false};
};

//! One open document window, as list_documents reports it.
struct DocumentWindow
{
    int index {-1};         //!< position in QMdiArea::subWindowList()
    QString path;           //!< file name; empty for an unnamed drawing
    QString title;          //!< the window title LibreCAD shows
    bool modified {false};  //!< unsaved changes, including an open undo cycle
    bool active {false};    //!< the window the current session serves
    //! For a block editor or print preview, the index of the drawing window
    //! it belongs to; -1 for a drawing.
    int parent {-1};
};

//! Every document window of the application, in MDI order. False on a
//! version mismatch or before the application window exists.
bool documentWindows(QList<DocumentWindow> *windows);

//! Carry out \a restart against the application window. Returns false (with
//! \a error set) when the version check fails or LibreCAD refused. Static
//! because it runs after the session's NativeBridge is gone.
bool performSessionRestart(const SessionRestart &restart, QString *error);

//! True once the application window exists and has a current document --
//! the state in which LibreCAD enables plugin menu entries. False on a
//! version mismatch. Used by the auto-start poll.
bool hasActiveDocument();

class NativeBridge : public QObject
{
    Q_OBJECT

public:
    //! \a mainWindow is the QWidget LibreCAD passes to execComm(), i.e. the
    //! application window that owns the command widget.
    explicit NativeBridge(QWidget *mainWindow, QObject *parent = nullptr);
    ~NativeBridge() override;

    //! False when the running LibreCAD is not the version this plugin was
    //! built against, or the command widget cannot be found; reason() then
    //! says which.
    bool commandsAvailable() const;
    bool selectionAvailable() const;
    //! Engine modifications need the current document; false when the
    //! application window has none (or the version check failed).
    bool modificationAvailable() const;
    QString reason() const { return m_reason; }

    //! The LibreCAD version string the plugin was compiled against, as
    //! QCoreApplication::applicationVersion() reports it (e.g. "v2.2.1.5").
    static QString builtAgainst();
    //! What the running process reports, for native_status.
    static QString running();

    //! Feed one line to the command widget, as if the user typed it and
    //! pressed enter. Coordinates ("10.5,20") drive the pending action's
    //! point prompts; see RS_EventHandler::commandEvent.
    bool execCommand(const QString &command);

    //! Select or deselect the entity behind a Plug_Entity wrapper.
    bool setSelected(Plug_Entity *entity, bool selected);
    //! Read the selection flag. The plugin API has no query for it (its
    //! getSelect() is a prompt); RS_Entity::isSelected() is the real one.
    bool isSelected(Plug_Entity *entity, bool *selected) const;
    //! The entity's bounding box as LibreCAD keeps it (RS_Entity::getMin/
    //! getMax, maintained by calculateBorders()). False when unavailable or
    //! when the entity has no valid extent.
    bool boundingBox(Plug_Entity *entity, QPointF *min, QPointF *max) const;
    //! Whether the entity has been removed from the drawing but kept for
    //! undo. Doc_plugin_interface::getAllEntities() hands those out too, so
    //! every census through the plugin API needs this to skip them.
    bool isUndone(Plug_Entity *entity, bool *undone) const;
    //! Identity of the engine entity behind a wrapper, for telling fresh
    //! wrappers of the same entity apart from new entities. Opaque.
    const void *entityKey(Plug_Entity *entity) const;

    // Engine modifications, via RS_Modification -- the same code the modify
    // tools run. offset/mirror/explode act on the current selection (set it
    // with setSelected first); trim takes its entities explicitly. Each one
    // reports false on an engine refusal, with lastError() saying why.
    //! Copies of the selection offset by  distance toward  side;
    //!  number copies (>= 1), or with  keepOriginal false the originals
    //! are replaced by a single offset copy.
    bool offset(const QPointF &side, double distance, int number,
                bool keepOriginal, bool useCurrentLayer,
                bool useCurrentAttributes);
    //! Mirror the selection across the axis p1-p2;  copy keeps originals.
    bool mirror(const QPointF &axisP1, const QPointF &axisP2, bool copy);
    //! Replace each selected container with its members;  remove drops the
    //! originals.
    bool explode(bool remove);
    //! Trim  trimEntity (atomic only) against  limitEntity.  trimPoint
    //! lies on the part to keep;  limitPoint picks the intersection when
    //! there are several.  both trims the limit entity too.
    bool trim(Plug_Entity *trimEntity, const QPointF &trimPoint,
              Plug_Entity *limitEntity, const QPointF &limitPoint, bool both);
    QString lastError() const { return m_lastError; }

    // Document file state. The session's undo cycle stays open throughout;
    // saving does not touch it.
    //! Current file name (empty for an unnamed drawing) and modified flag.
    bool fileInfo(QString *path, bool *modified) const;
    //! Write the drawing to \a path. \a format names a DXF version
    //! ("dxf2007", "dxf2004", "dxf2000", "dxf14", "dxf12", "dxf1") or is
    //! empty to pick by extension. The document's file name becomes \a path.
    bool saveAs(const QString &path, const QString &format);

    // Undo cycles. execPlug() opens one cycle around the whole session.
    // RS_Undo::startUndoCycle() discards the redo list, so the cycle is
    // reopened lazily: undoCheckpoint()/undo() close it, and the dispatcher
    // calls ensureUndoCycle() before the next operation that changes the
    // drawing. A session that ends with the cycle closed leaves execPlug()'s
    // endUndoCycle() unmatched, which RS_Undo handles (a debug warning).
    //! Close the current undo step; the next change starts a new one.
    bool undoCheckpoint();
    //! Close the current step, then undo (or redo) up to \a steps cycles;
    //! the number actually undone is returned through \a done.
    bool undo(int steps, bool redo, int *done);
    //! Open a cycle if none is open. Call before anything undoable.
    void ensureUndoCycle();

    //! Start watching for the hatch dialog. When it appears, fill it in and
    //! accept it. armed() stays true until the dialog was handled or
    //! \a timeoutMs passed. \a angleDegrees because the dialog field is in
    //! degrees.
    void armHatchDialog(const QString &pattern, double scaleFactor,
                        double angleDegrees, bool solid, int timeoutMs = 3000);
    void disarmHatchDialog();
    bool hatchDialogHandled() const { return m_hatchDialogHandled; }

    // ---- Layer state, block definition, geometry queries -------------------
    //
    // Layers and blocks are reached through the document's RS_LayerList and
    // RS_BlockList (virtual accessors on RS_Document); the queries call
    // virtual RS_Entity methods or RS_Information's static functions. The
    // mutating calls report why they refused through lastError() and the
    // returned Result; the dispatcher maps NotFound / Refused / Failed to
    // not_found / bad_request / failed.

    enum class Result { Done, NotFound, Refused, Failed };

    //! A layer's four flags. "visible" is the inverse of frozen.
    struct LayerState
    {
        bool frozen {false};
        bool locked {false};
        bool print {true};
        bool construction {false};
    };
    //! Flags to change; unset members are left alone.
    struct LayerStatePatch
    {
        std::optional<bool> frozen;
        std::optional<bool> locked;
        std::optional<bool> print;
        std::optional<bool> construction;
    };

    //! The state of the layer called \a name; false when there is none.
    bool layerState(const QString &name, LayerState *state) const;
    //! Apply \a patch through RS_LayerList's set*Multi functions, the ones
    //! the layer widget uses, so every layer-list listener fires and the
    //! view redraws. Not undoable (layer state is not in the undo system).
    Result setLayerState(const QString &name, const LayerStatePatch &patch);
    //! Rename a layer in place with RS_LayerList::edit() and a clone that
    //! carries the new name -- the layer dialog's path. Entities hold the
    //! RS_Layer pointer, so they follow the rename. Not undoable.
    Result renameLayer(const QString &oldName, const QString &newName);

    //! Names of the blocks that are really in the drawing: LibreCAD's block
    //! removal only marks a block undone and leaves it in the list, and the
    //! plugin API's getAllBlocks() lists those too.
    bool blockNames(QStringList *names) const;
    //! Create a block named \a name from the current selection, with the
    //! base point \a base, via RS_Creation::createBlock (what Create Block
    //! does). \a remove takes the originals out of the drawing, undoably;
    //! the block definition itself is not undoable. \a selected receives the
    //! number of entities that went into the block.
    Result defineBlock(const QString &name, const QPointF &base, bool remove,
                       int *selected);
    //! Rename a block and the inserts that name it, everywhere (the block
    //! attributes action's two calls). Not undoable.
    Result renameBlock(const QString &oldName, const QString &newName);
    //! Remove a block the way the Remove Block action does: mark it undone
    //! and register it with the undo cycle, so it is undoable. Refused while
    //! any live INSERT, in the drawing or in another block, names it.
    Result removeBlock(const QString &name);
    //! Live INSERT entities that name \a name, in the drawing and in every
    //! block. -1 when unavailable.
    int blockInsertCount(const QString &name) const;
    //! Plugin wrappers for the entities inside block \a name, owned by the
    //! caller. \a doc is the session's Document_Interface (the wrapper needs
    //! its Doc_plugin_interface).
    Result blockEntities(const QString &name, Document_Interface *doc,
                         QList<Plug_Entity *> *out);

    //! RS_Entity::getLength(); false when the entity has none (text, hatch).
    bool entityLength(Plug_Entity *entity, double *length) const;
    //! The enclosed area: circle, full ellipse, closed polyline. Everything
    //! else is reported as 0 with \a meaningful false.
    bool entityArea(Plug_Entity *entity, double *area, bool *meaningful) const;
    //! Intersection points of two entities (RS_Information::getIntersection).
    bool intersections(Plug_Entity *a, Plug_Entity *b, bool onEntities,
                       QList<QPointF> *points) const;
    //! Distance from \a point to the entity, false when the entity is hidden
    //! (undone, frozen layer, invisible) or has no distance.
    bool entityDistance(Plug_Entity *entity, const QPointF &point,
                        double *distance) const;
    //! The point on the entity (or on its infinite extension when
    //! \a onEntity is false) nearest to \a point.
    bool nearestPoint(Plug_Entity *entity, const QPointF &point, bool onEntity,
                      QPointF *nearest, double *distance) const;
    //! Whether \a point is inside a closed polyline, circle, or full ellipse.
    //! Refused for anything else.
    Result pointInside(Plug_Entity *entity, const QPointF &point, bool *inside,
                       bool *onContour);

    // ---- View control, document windows, export (roadmap item 4) --------
    // All through the session's RS_GraphicView and the application window.
    // None of it changes the drawing's entities, so the dispatcher treats
    // these as read-only for undo; note that LibreCAD's zoom functions mark
    // the drawing modified anyway (RS_GraphicView::saveView).

    //! The graphic view is there and the version check passed.
    bool viewAvailable() const;

    //! What the view shows: zoom factor, pixel offset, size in pixels, and
    //! the drawing-coordinate rectangle that is visible.
    struct ViewState
    {
        double factorX {1.0};
        double factorY {1.0};
        int offsetX {0};
        int offsetY {0};
        int width {0};
        int height {0};
        QPointF min;
        QPointF max;
    };
    bool viewState(ViewState *state) const;

    bool zoomAuto(bool keepAspectRatio);
    //! Show the drawing-coordinate rectangle p1-p2.
    bool zoomWindow(const QPointF &p1, const QPointF &p2, bool keepAspectRatio);
    //! Zoom by \a factor about \a center (drawing coordinates), or about the
    //! middle of the view when \a hasCenter is false -- LibreCAD's own
    //! default is the mouse position, which means nothing to a script.
    bool zoomIn(double factor, bool hasCenter, const QPointF &center, bool out);
    //! Shift the view by pixels; positive dy moves the drawing up.
    bool zoomPan(int dx, int dy);
    bool zoomPrevious();
    bool zoomPage();
    //! Set the factor and/or the offset directly; with \a hasCenter the
    //! offset is computed so \a center (drawing coordinates) is in the middle
    //! of the view.
    bool setView(bool hasFactor, double factor, bool hasOffset, int offsetX,
                 int offsetY, bool hasCenter, const QPointF &center);

    //! True when the current document has changes not yet saved, counting
    //! the session's open undo cycle (RS_Document only sets its modified flag
    //! when a cycle closes).
    bool hasUnsavedChanges() const;

    //! Render the whole drawing to an image file, as File > Export does.
    //! \a format is a QImageWriter format name or "svg".
    //! \a transparent leaves the background transparent (raster formats only).
    bool exportImage(const QString &path, const QString &format, const QSize &size,
                     int border, bool blackBackground, bool blackWhite,
                     bool transparent);
    //! Print the drawing to a PDF file without the print dialog, the way
    //! File > Export as PDF does. \a paper empty means the drawing's own
    //! paper size; \a landscape < 0 means the drawing's (or portrait for an
    //! explicit paper). \a fitToPage scales the drawing onto one page inside
    //! the drawing's margins; otherwise the drawing's paper scale and
    //! insertion base apply, over as many pages as it sets up.
    bool exportPdf(const QString &path, const QString &paper, int landscape,
                   bool fitToPage, int *pages, QSizeF *paperMm);

    // --- Modify tools: the rest of RS_Modification --------------------------
    // Like offset/mirror/explode, the transforms act on the current selection.
    // \a copies follows RS_Modification's "number": 0 transforms the
    // originals (they are replaced by transformed clones); n >= 1 keeps them
    // and adds n copies at 1x, 2x, ... nx the transformation.
    //! Move the selection by \a offset.
    bool move(const QPointF &offset, int copies, bool useCurrentLayer,
              bool useCurrentAttributes);
    //! Rotate the selection by \a angle radians around \a center.
    bool rotate(const QPointF &center, double angle, int copies,
                bool useCurrentLayer, bool useCurrentAttributes);
    //! Scale the selection around \a center by (factor.x, factor.y). Unequal
    //! factors turn circles and arcs into ellipses, as in the GUI.
    bool scale(const QPointF &center, const QPointF &factor, int copies,
               bool useCurrentLayer, bool useCurrentAttributes);
    //! Move by \a offset, then rotate by \a angle around the moved \a center
    //! (RS_MoveRotateData::referencePoint).
    bool moveRotate(const QPointF &offset, const QPointF &center, double angle,
                    int copies, bool useCurrentLayer, bool useCurrentAttributes);
    //! Rotate by \a angle1 around \a center1, then by \a angle2 around
    //! \a center2 (itself carried along by the first rotation).
    bool rotate2(const QPointF &center1, const QPointF &center2,
                 double angle1, double angle2, int copies,
                 bool useCurrentLayer, bool useCurrentAttributes);
    //! Stretch everything visible and unlocked that lies in, or has an
    //! endpoint in, the window \a firstCorner-\a secondCorner by \a offset.
    //! Works on the whole document, not the selection -- but the engine
    //! removes every selected entity afterwards, so the caller must clear
    //! the selection first.
    bool stretch(const QPointF &firstCorner, const QPointF &secondCorner,
                 const QPointF &offset);
    //! Fillet the corner between two atomic entities. \a point1 and \a point2
    //! pick the part of each entity to keep; \a corner says on which side of
    //! both entities the arc goes (the GUI passes its second click). \a trim
    //! replaces both entities with trimmed clones.
    bool round(Plug_Entity *entity1, const QPointF &point1,
               Plug_Entity *entity2, const QPointF &point2,
               const QPointF &corner, double radius, bool trim);
    //! Chamfer the corner between two atomic entities, \a length1 along the
    //! first and \a length2 along the second.
    bool bevel(Plug_Entity *entity1, const QPointF &point1,
               Plug_Entity *entity2, const QPointF &point2,
               double length1, double length2, bool trim);
    //! Split an atomic entity at the point on it nearest \a point. A circle
    //! becomes one full-turn arc starting there; anything else two pieces.
    bool cut(Plug_Entity *entity, const QPointF &point);
    //! What changeAttributes() sets. Names use the plugin API's spellings
    //! ("0.25mm", "DashLine", "BYLAYER"); color is the plugin API's int
    //! (-1 ByLayer, -2 ByBlock, else 24-bit RGB).
    struct AttributeChange
    {
        bool changeLayer {false};
        QString layer;
        bool changeColor {false};
        int color {-1};
        bool changeLineType {false};
        QString lineType;
        bool changeWidth {false};
        QString width;
    };
    //! Whether changeAttributes() knows a line type / line width name, so
    //! the dispatcher can reject a typo as bad arguments up front.
    static bool isLineTypeName(const QString &name);
    static bool isLineWidthName(const QString &name);
    //! Apply \a change to every selected entity; each is replaced by a clone.
    bool changeAttributes(const AttributeChange &change);
    //! Reverse the direction of every selected entity (start <-> end); each
    //! is replaced by a clone.
    bool revertDirection();

    //! Undo-cycle hygiene for operations that replace entities.
    //! RS_UndoCycle keeps its undoables in a std::set, so an entity that is
    //! created and then replaced (marked undone) within one cycle is listed
    //! once, and undo toggles it back to life -- the entity reappears. A
    //! GUI action is its own cycle, so LibreCAD never meets this; a bridge
    //! session is one long cycle and does. Call this before an operation
    //! replaces \a replaced: if any of them was created in the current
    //! cycle, the cycle is closed and a fresh one opened, so the operation
    //! becomes an undo step of its own. Otherwise nothing happens and the
    //! session stays one step.
    void isolateReplacement(const QList<Plug_Entity *> &replaced);
    //! The same for stretch, which picks what it replaces by window: every
    //! visible, unlocked entity inside the window or with an endpoint in it.
    void isolateStretch(const QPointF &firstCorner, const QPointF &secondCorner);

    // ---- Prompts and push events --------------------------------------------
    //! What a prompt waits on, which decides how cancelPrompt() ends it.
    enum PromptKind { PointPrompt, SelectPrompt, DialogPrompt };
    //! The application window given to the constructor (nullptr in the
    //! offline stub). Its Qt signals (gridChanged(bool), ...) can be
    //! connected by name without binding any LibreCAD symbol.
    QWidget *mainWindow() const;
    //! Watch the plugin-API prompt that is about to start. While armed, a
    //! 25 ms timer fills \a dialogDefault into the QInputDialog that
    //! getInt/getReal/getString open (when valid), and after \a timeoutMs
    //! (<= 0: never) cancels the prompt. Arm right before the call and
    //! disarm right after it; the timer fires inside the prompt's own
    //! nested event loop.
    void armPrompt(PromptKind kind, int timeoutMs, const QVariant &dialogDefault);
    //! Stop watching; true when the prompt was cancelled by the timeout
    //! (or by cancelPrompt()) rather than answered.
    bool disarmPrompt();
    //! Cancel the armed prompt now (the session's Stop button, the
    //! timeout). Does nothing when no prompt is armed. Point and select
    //! prompts need the version check to pass; dialogs are plain Qt.
    void cancelPrompt();
    //! Live (not undone) entities in the document and how many of them are
    //! selected, counted on the engine's own list: no Plug_Entity wrappers
    //! are allocated, so this is cheap enough to poll.
    bool census(int *live, int *selected) const;
    //! The graphic view's zoom factor and pixel offsets.
    bool viewState(double *factor, int *offsetX, int *offsetY) const;
    //! Whether the drawing's grid is shown ($GRIDMODE, as
    //! RS_Graphic::isGridOn reads it).
    bool gridState(bool *on) const;

private:
    void pollForHatchDialog();
    void pollPrompt();
    //! Remember which entities exist as the current undo cycle opens; see
    //! isolateReplacement().
    void snapshotCycleStart();
    //! Close the current undo cycle and open the next one.
    void splitUndoCycle();
    QSet<const void *> m_cycleStartKeys;

    bool m_versionOk {false};
    QG_CommandWidget *m_commandWidget {nullptr};
    RS_Document *m_document {nullptr};
    RS_GraphicView *m_graphicView {nullptr};
    bool m_undoCycleOpen {true};   // execPlug() opened it before execComm()
    QString m_reason;
    QString m_lastError;

    QTimer *m_hatchTimer {nullptr};
    QString m_hatchPattern;
    double m_hatchScale {1.0};
    double m_hatchAngleDegrees {0.0};
    bool m_hatchSolid {false};
    bool m_hatchDialogHandled {false};
    int m_hatchPollsLeft {0};

    QWidget *m_mainWindow {nullptr};
    QTimer *m_promptTimer {nullptr};
    QElapsedTimer m_promptClock;
    QPointer<QObject> m_promptAction;
    QVariant m_promptDefault;
    PromptKind m_promptKind {PointPrompt};
    int m_promptTimeoutMs {0};
    bool m_promptArmed {false};
    bool m_promptDefaultApplied {false};
    bool m_promptCancelled {false};
};

} // namespace lcbridge

#endif // LC_BRIDGE_NATIVE_H
