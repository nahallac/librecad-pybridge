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

#include <QList>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>

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

private:
    void pollForHatchDialog();

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
};

} // namespace lcbridge

#endif // LC_BRIDGE_NATIVE_H
