/*****************************************************************************/
/*  lc_bridge_native.cpp - in-process access to LibreCAD internals           */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_native.h"

#include "document_interface.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

#include <dlfcn.h>

namespace lcbridge {
namespace {

// Mangled names of the selection setters LibreCAD exports (it is linked with
// --export-dynamic, so the executable's symbols resolve from a plugin).
const char *const kSetSelectedEntity = "_ZN9RS_Entity11setSelectedEb";
const char *const kSetSelectedContainer = "_ZN18RS_EntityContainer11setSelectedEb";

//! The RS_Entity* behind a Plug_Entity wrapper.
//!
//! LibreCAD's Plugin_Entity (librecad/src/main/doc_plugin_interface.h) is the
//! object a Plug_Entity actually points at, and its first data member after
//! the vtable pointer is `RS_Entity* entity`. There is no accessor reachable
//! through the plugin interface (getEnt() is non-virtual and inline), so the
//! pointer is read by layout. The layout is pinned by the vendored copy of
//! that header (vendor/librecad-v2.2.1.5/doc_plugin_interface.h) and would
//! need rechecking on a LibreCAD update, like every other internal detail
//! this file touches.
void *underlyingEntity(Plug_Entity *entity)
{
    if (!entity)
        return nullptr;
    return *reinterpret_cast<void **>(
        reinterpret_cast<char *>(entity) + sizeof(void *));
}

//! Entity types whose engine class derives from RS_EntityContainer, per the
//! DPI::ETYPE enum comment ("SOLID /*end atomicEntity, start entityContainer*/").
bool isContainerType(int dpiType)
{
    switch (dpiType) {
    case DPI::MTEXT:
    case DPI::TEXT:
    case DPI::INSERT:
    case DPI::POLYLINE:
    case DPI::SPLINE:
    case DPI::SPLINEPOINTS:
    case DPI::HATCH:
    case DPI::DIMLEADER:
    case DPI::DIMALIGNED:
    case DPI::DIMLINEAR:
    case DPI::DIMRADIAL:
    case DPI::DIMDIAMETRIC:
    case DPI::DIMANGULAR:
        return true;
    default:
        return false;
    }
}

} // namespace

NativeBridge::NativeBridge(QWidget *mainWindow, QObject *parent)
    : QObject(parent)
{
    // The command widget is a QG_CommandWidget; its .ui file names the root
    // widget after the class, and handleCommand(QString) is a public slot, so
    // it is reachable with nothing but QObject machinery.
    if (mainWindow) {
        m_commandWidget =
            mainWindow->findChild<QWidget *>(QStringLiteral("QG_CommandWidget"));
    }
    if (!m_commandWidget)
        m_reason = QStringLiteral("command widget not found in the main window");

    m_setSelectedEntity = reinterpret_cast<SetSelectedFn>(
        dlsym(RTLD_DEFAULT, kSetSelectedEntity));
    m_setSelectedContainer = reinterpret_cast<SetSelectedFn>(
        dlsym(RTLD_DEFAULT, kSetSelectedContainer));
    if (!m_setSelectedEntity || !m_setSelectedContainer) {
        if (!m_reason.isEmpty())
            m_reason += QStringLiteral("; ");
        m_reason += QStringLiteral("selection symbols not exported by this "
                                   "LibreCAD build");
    }

    m_hatchTimer = new QTimer(this);
    m_hatchTimer->setInterval(25);
    connect(m_hatchTimer, &QTimer::timeout,
            this, &NativeBridge::pollForHatchDialog);
}

NativeBridge::~NativeBridge() = default;

bool NativeBridge::commandsAvailable() const
{
    return m_commandWidget != nullptr;
}

bool NativeBridge::selectionAvailable() const
{
    return m_setSelectedEntity && m_setSelectedContainer;
}

bool NativeBridge::execCommand(const QString &command)
{
    if (!m_commandWidget)
        return false;
    // Direct connection: the command, and any action it starts or feeds, runs
    // to completion before this returns, exactly like a typed command.
    return QMetaObject::invokeMethod(m_commandWidget, "handleCommand",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, command));
}

bool NativeBridge::setSelected(Plug_Entity *entity, int dpiType, bool selected)
{
    if (!selectionAvailable())
        return false;
    void *rsEntity = underlyingEntity(entity);
    if (!rsEntity)
        return false;

    if (isContainerType(dpiType))
        m_setSelectedContainer(rsEntity, selected);
    else
        m_setSelectedEntity(rsEntity, selected);
    return true;
}

void NativeBridge::armHatchDialog(const QString &pattern, double scaleFactor,
                                  double angleDegrees, bool solid, int timeoutMs)
{
    m_hatchPattern = pattern;
    m_hatchScale = scaleFactor;
    m_hatchAngleDegrees = angleDegrees;
    m_hatchSolid = solid;
    m_hatchDialogHandled = false;
    m_hatchPollsLeft = qMax(1, timeoutMs / qMax(1, m_hatchTimer->interval()));
    m_hatchTimer->start();
}

void NativeBridge::disarmHatchDialog()
{
    m_hatchTimer->stop();
}

void NativeBridge::pollForHatchDialog()
{
    // The hatch action opens QG_DlgHatch with exec(), a nested event loop, in
    // which this timer keeps firing. Fill the dialog the way a user would and
    // accept it; requestHatchDialog() then reads the widgets back into the
    // hatch data.
    if (--m_hatchPollsLeft <= 0) {
        m_hatchTimer->stop();
        return;
    }

    for (QWidget *top : QApplication::topLevelWidgets()) {
        auto *dialog = qobject_cast<QDialog *>(top);
        if (!dialog || dialog->objectName() != QLatin1String("QG_DlgHatch")
            || !dialog->isVisible()) {
            continue;
        }

        if (auto *solid = dialog->findChild<QCheckBox *>(QStringLiteral("cbSolid")))
            solid->setChecked(m_hatchSolid);
        if (auto *patternBox =
                dialog->findChild<QComboBox *>(QStringLiteral("cbPattern"))) {
            const int index = patternBox->findText(m_hatchPattern,
                                                   Qt::MatchFixedString);
            if (index >= 0)
                patternBox->setCurrentIndex(index);
        }
        if (auto *scale = dialog->findChild<QLineEdit *>(QStringLiteral("leScale")))
            scale->setText(QString::number(m_hatchScale));
        if (auto *angle = dialog->findChild<QLineEdit *>(QStringLiteral("leAngle")))
            angle->setText(QString::number(m_hatchAngleDegrees));

        m_hatchDialogHandled = true;
        m_hatchTimer->stop();
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        return;
    }
}

} // namespace lcbridge
