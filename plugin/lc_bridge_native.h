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
#include <QString>
#include <QVariantMap>

class Plug_Entity;
class RS_Entity;
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
    enum Kind { None, OpenFile, NewDrawing };
    Kind kind {None};
    QString path;
};

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

    // ---- Creation: entities the plugin API cannot make -------------------
    //
    // Each of these builds the engine entity directly and commits it the way
    // the corresponding LibreCAD action's trigger() does: active layer and
    // pen, update(), addEntity() on the document, one nested undo cycle
    // around addUndoable(), redraw. Nothing goes through the command line,
    // so no entity pick is needed. All need modificationAvailable(); false
    // with lastError() set on a refusal. Angles are radians.

    //! MTEXT at \a at (its attachment point, per \a halign / \a valign,
    //! which take DPI::HAlign / DPI::VAlign values). \a width is the
    //! reference rectangle width, \a lineSpacing the line spacing factor.
    bool addMText(const QString &text, const QString &style, const QPointF &at,
                  double height, double width, double angle, int halign,
                  int valign, double lineSpacing);
    //! Pixel size of the image file at \a path; false when Qt cannot read it.
    bool imagePixelSize(const QString &path, QSize *size) const;
    //! IMAGE of the file at \a path (absolute), lower left corner at \a at,
    //! each pixel \a scale drawing units wide, rotated by \a angle.
    bool addImage(const QString &path, const QPointF &at, double scale,
                  double angle, int brightness, int contrast, int fade);
    //! DIMALIGNED between \a p1 and \a p2, dimension line through \a dimLine.
    //! \a text: empty for the measured value, "<>" inside it is replaced by
    //! the measurement, " " suppresses it (RS_Dimension::getLabel).
    bool dimAligned(const QPointF &p1, const QPointF &p2, const QPointF &dimLine,
                    const QString &text);
    //! DIMLINEAR measuring along \a angle (0 horizontal, pi/2 vertical).
    bool dimLinear(const QPointF &p1, const QPointF &p2, const QPointF &dimLine,
                   double angle, const QString &text);
    //! DIMRADIAL (or DIMDIAMETRIC with \a diametric) of the circle \a center,
    //! \a radius, pointing at the circle in direction \a angle.
    bool dimRadial(const QPointF &center, double radius, double angle,
                   const QString &text, bool diametric);
    //! DIMANGULAR between the lines \a l1a-\a l1b and \a l2a-\a l2b, its arc
    //! through \a dimLine; the angle measured is the one of the four
    //! sectors around the intersection that contains \a dimLine.
    bool dimAngular(const QPointF &l1a, const QPointF &l1b, const QPointF &l2a,
                    const QPointF &l2b, const QPointF &dimLine,
                    const QString &text);
    //! LEADER through \a points (at least two), arrow head at the first.
    bool dimLeader(const QList<QPointF> &points, bool arrowHead);
    //! HATCH whose single boundary loop holds copies of \a boundary (atomic
    //! members of containers such as polylines), like the hatch action.
    bool addHatch(const QList<Plug_Entity *> &boundary, const QString &pattern,
                  double scale, double angle, bool solid);
    //! Read-only attributes getData() does not report: dimension geometry
    //! and label, leader vertices, MTEXT layout, hatch pattern. Points are
    //! two-element lists. False when the entity has none or no native access.
    bool entityDetails(Plug_Entity *entity, QVariantMap *details) const;

private:
    void pollForHatchDialog();
    //! What every creating action's trigger() does with its new entity.
    void commitNewEntity(RS_Entity *entity, bool update);

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
