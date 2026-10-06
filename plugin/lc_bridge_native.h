/*****************************************************************************/
/*  lc_bridge_native.h - in-process access to LibreCAD beyond the plugin API */
/*                                                                           */
/*  The plugin interface cannot create DIMENSION or HATCH entities, but the   */
/*  plugin runs inside LibreCAD's process, and LibreCAD both ships a command  */
/*  line and exports its internal symbols (--export-dynamic). This class      */
/*  reaches both:                                                            */
/*                                                                           */
/*   - execCommand() feeds a line to the command widget exactly as if typed,  */
/*     via the public slot QG_CommandWidget::handleCommand(QString). That     */
/*     runs real LibreCAD actions, which is how real dimensions get drawn.    */
/*   - setSelected() flips an entity's selection flag by resolving            */
/*     RS_Entity::setSelected from the executable with dlsym(). Hatching      */
/*     consumes the current selection, and the plugin API has no setter.      */
/*   - armHatchDialog() watches for the modal hatch dialog that the hatch     */
/*     action opens, fills in pattern/scale/angle/solid, and accepts it.      */
/*                                                                           */
/*  All of this is deliberate coupling to LibreCAD internals that the plugin  */
/*  API does not promise. Everything here fails soft: when a widget or        */
/*  symbol is missing, the operation reports "unavailable" instead of         */
/*  crashing. Validated against LibreCAD 2.2.1.5; see docs/findings.md.      */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef LC_BRIDGE_NATIVE_H
#define LC_BRIDGE_NATIVE_H

#include <QObject>
#include <QString>

class Plug_Entity;
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

    //! False when the command widget or the needed symbols cannot be found;
    //! reason() then says which.
    bool commandsAvailable() const;
    bool selectionAvailable() const;
    QString reason() const { return m_reason; }

    //! Feed one line to the command widget, as if the user typed it and
    //! pressed enter. Coordinates ("10.5,20") drive the pending action's
    //! point prompts; see RS_EventHandler::commandEvent.
    bool execCommand(const QString &command);

    //! Select or deselect the entity behind a Plug_Entity wrapper.
    //! \a dpiType decides between RS_Entity::setSelected and the container
    //! override, which also selects the children -- calling the base version
    //! on a container would leave its members unselected, and the hatch
    //! action resolves selection on the members.
    bool setSelected(Plug_Entity *entity, int dpiType, bool selected);

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

    QWidget *m_commandWidget {nullptr};
    QString m_reason;

    // void(RS_Entity::*)(bool) and the RS_EntityContainer override, called
    // through plain function pointers with an explicit this argument.
    using SetSelectedFn = void (*)(void *, bool);
    SetSelectedFn m_setSelectedEntity {nullptr};
    SetSelectedFn m_setSelectedContainer {nullptr};

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
