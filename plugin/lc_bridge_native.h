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

#include <QObject>
#include <QPointF>
#include <QString>

class Plug_Entity;
class QG_CommandWidget;
class QTimer;
class QWidget;

namespace lcbridge {

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

    //! Start watching for the hatch dialog. When it appears, fill it in and
    //! accept it. armed() stays true until the dialog was handled or
    //! \a timeoutMs passed. \a angleDegrees because the dialog field is in
    //! degrees.
    void armHatchDialog(const QString &pattern, double scaleFactor,
                        double angleDegrees, bool solid, int timeoutMs = 3000);
    void disarmHatchDialog();
    bool hatchDialogHandled() const { return m_hatchDialogHandled; }

private:
    void pollForHatchDialog();

    bool m_versionOk {false};
    QG_CommandWidget *m_commandWidget {nullptr};
    QString m_reason;

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
