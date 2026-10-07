/*****************************************************************************/
/*  lc_bridge_native.cpp - in-process access to LibreCAD internals           */
/*                                                                           */
/*  Compiled against LibreCAD's own headers (see LIBRECAD_SRC in the .pro).   */
/*  Every call below goes through a vtable or an inline accessor, so the     */
/*  shared object carries no undefined LibreCAD symbols and still loads       */
/*  outside LibreCAD; what it does depend on is the class layouts of the      */
/*  version it was built against, which the constructor checks first.        */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_native.h"

// LibreCAD plugin interface (installed headers).
#include "document_interface.h"
// LibreCAD internals (source tree).
#include "doc_plugin_interface.h"   // Plugin_Entity
#include "rs_entity.h"              // RS_Entity::setSelected
#include "rs_atomicentity.h"        // RS_AtomicEntity, for trim
#include "rs_document.h"            // RS_Document is the RS_EntityContainer
#include "rs_modification.h"        // RS_Modification: the modify tools
#include "qc_applicationwindow.h"   // QC_ApplicationWindow::getAppWindow
#include "qc_mdiwindow.h"           // window title after save
#include "rs_fileio.h"              // RS_FileIO::detectFormat
#include "qg_commandwidget.h"       // QG_CommandWidget::handleCommand
#include "qg_dlghatch.h"            // QG_DlgHatch and its Ui members
#include "rs_actioninterface.h"     // RS_ActionInterface::trigger, for prompts
#include "rs_graphicview.h"         // current action, zoom factor and offsets

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QCoreApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

#ifndef LC_PYBRIDGE_LIBRECAD_VERSION
#error "LC_PYBRIDGE_LIBRECAD_VERSION must be defined (lc_pybridge.pro sets it)"
#endif

namespace lcbridge {
namespace {

//! The RS_Entity* behind a Plug_Entity wrapper.
//!
//! The object LibreCAD hands out as a Plug_Entity* is really a Plugin_Entity
//! (librecad/src/main/doc_plugin_interface.h): the two classes are unrelated
//! by inheritance and LibreCAD itself bridges them with reinterpret_cast, so
//! the same cast is the correct way back. getEnt() is an inline accessor,
//! which makes this a compile-time layout dependency on the header and
//! nothing more.
RS_Entity *underlyingEntity(Plug_Entity *entity)
{
    if (!entity)
        return nullptr;
    return reinterpret_cast<Plugin_Entity *>(entity)->getEnt();
}

//! True when \a object is an instance of the LibreCAD class named
//! \a className, by meta-object name rather than qobject_cast: a cast would
//! reference the class's staticMetaObject, a data symbol the dynamic loader
//! must resolve at load time, and that would stop the plugin loading
//! anywhere but inside LibreCAD.
bool isInstanceOf(const QObject *object, const char *className)
{
    for (const QMetaObject *meta = object->metaObject(); meta;
         meta = meta->superClass()) {
        if (qstrcmp(meta->className(), className) == 0)
            return true;
    }
    return false;
}

bool versionMatches()
{
    return QCoreApplication::applicationVersion()
           == QStringLiteral(LC_PYBRIDGE_LIBRECAD_VERSION);
}

bool formatFromName(const QString &name, RS2::FormatType *type)
{
    static const struct { const char *name; RS2::FormatType type; } table[] = {
        {"dxf2007", RS2::FormatDXFRW},  {"dxf2004", RS2::FormatDXFRW2004},
        {"dxf2000", RS2::FormatDXFRW2000}, {"dxf14", RS2::FormatDXFRW14},
        {"dxf12", RS2::FormatDXFRW12},  {"dxf1", RS2::FormatDXF1},
        {"lff", RS2::FormatLFF},        {"cxf", RS2::FormatCXF},
    };
    for (const auto &entry : table) {
        if (name.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0) {
            *type = entry.type;
            return true;
        }
    }
    return false;
}

RS_Vector toVector(const QPointF &point)
{
    // The (x, y, z) constructor is an exported symbol, but the default one
    // is inline and the members are public, so this binds nothing.
    RS_Vector vector;
    vector.x = point.x();
    vector.y = point.y();
    vector.z = 0.0;
    vector.valid = true;
    return vector;
}

} // namespace

bool performSessionRestart(const SessionRestart &restart, QString *error)
{
    if (!versionMatches()) {
        *error = QStringLiteral("LibreCAD version mismatch");
        return false;
    }
    QC_ApplicationWindow *app = QC_ApplicationWindow::getAppWindow().get();
    if (!app) {
        *error = QStringLiteral("no application window");
        return false;
    }
    switch (restart.kind) {
    case SessionRestart::OpenFile:
        // The same path File > Open takes: a new MDI window, the file loaded
        // into it, the window activated, recent files updated. Failure
        // closes the window again and shows LibreCAD's own message.
        app->slotFileOpen(restart.path, RS2::FormatUnknown);
        return true;
    case SessionRestart::NewDrawing:
        app->slotFileNewNew();
        return true;
    case SessionRestart::None:
        break;
    }
    *error = QStringLiteral("nothing to do");
    return false;
}

bool hasActiveDocument()
{
    if (!versionMatches())
        return false;
    QC_ApplicationWindow *app = QC_ApplicationWindow::getAppWindow().get();
    return app && app->getDocument() != nullptr;
}

QString NativeBridge::builtAgainst()
{
    return QStringLiteral(LC_PYBRIDGE_LIBRECAD_VERSION);
}

QString NativeBridge::running()
{
    return QCoreApplication::applicationVersion();
}

NativeBridge::NativeBridge(QWidget *mainWindow, QObject *parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
    // The prompt watcher handles QInputDialogs, which are plain Qt, so it
    // exists whatever the version check below decides; the parts of it that
    // reach into the graphic view check m_versionOk themselves.
    m_promptTimer = new QTimer(this);
    m_promptTimer->setInterval(25);
    connect(m_promptTimer, &QTimer::timeout, this, &NativeBridge::pollPrompt);

    // Layouts and vtables are only known to match the LibreCAD whose headers
    // this was compiled against, so nothing below runs unless the process
    // says it is that version (main.cpp sets applicationVersion from
    // LC_VERSION).
    m_versionOk = running() == builtAgainst();
    if (!m_versionOk) {
        m_reason = QStringLiteral("plugin built against LibreCAD %1, "
                                  "running %2")
                       .arg(builtAgainst(), running());
        return;
    }

    // The command widget is found by class, not object name: its objectName
    // is "Command" (lc_widgetfactory.cpp passes that to the constructor,
    // which pre-empts the .ui file's default).
    if (mainWindow) {
        const QList<QWidget *> children = mainWindow->findChildren<QWidget *>();
        for (QWidget *child : children) {
            if (isInstanceOf(child, "QG_CommandWidget")) {
                m_commandWidget = static_cast<QG_CommandWidget *>(child);
                break;
            }
        }
    }
    if (!m_commandWidget)
        m_reason = QStringLiteral("command widget not found in the main window");

    // The document and view behind the Document_Interface, for
    // RS_Modification. execComm() is only ever called for the active MDI
    // window, so the application window's current ones are the right ones.
    // These three accessors are the plugin's first bound symbols; they
    // resolve against the executable's export table when LibreCAD loads it.
    if (QC_ApplicationWindow *app = QC_ApplicationWindow::getAppWindow().get()) {
        m_document = app->getDocument();
        m_graphicView = app->getGraphicView();
    }
    if (!m_document) {
        if (!m_reason.isEmpty())
            m_reason += QStringLiteral("; ");
        m_reason += QStringLiteral("no current document in the application window");
    }

    m_hatchTimer = new QTimer(this);
    m_hatchTimer->setInterval(25);
    connect(m_hatchTimer, &QTimer::timeout,
            this, &NativeBridge::pollForHatchDialog);
}

NativeBridge::~NativeBridge() = default;

bool NativeBridge::commandsAvailable() const
{
    return m_versionOk && m_commandWidget != nullptr;
}

bool NativeBridge::selectionAvailable() const
{
    // Selection needs only the entity vtable, which the version check covers.
    return m_versionOk;
}

bool NativeBridge::modificationAvailable() const
{
    return m_versionOk && m_document != nullptr;
}

bool NativeBridge::execCommand(const QString &command)
{
    if (!commandsAvailable())
        return false;
    // Synchronous, like a typed command: the command, and any action it
    // starts or feeds, runs to completion before this returns.
    m_commandWidget->handleCommand(command);
    return true;
}

bool NativeBridge::setSelected(Plug_Entity *entity, bool selected)
{
    if (!selectionAvailable())
        return false;
    RS_Entity *rsEntity = underlyingEntity(entity);
    if (!rsEntity)
        return false;
    // Virtual: a container (polyline, insert, hatch, ...) gets the
    // RS_EntityContainer override, which also selects its members -- the
    // hatch action resolves selection on the members.
    rsEntity->setSelected(selected);
    return true;
}

bool NativeBridge::isSelected(Plug_Entity *entity, bool *selected) const
{
    if (!selectionAvailable())
        return false;
    RS_Entity *rsEntity = underlyingEntity(entity);
    if (!rsEntity)
        return false;
    *selected = rsEntity->isSelected();
    return true;
}

bool NativeBridge::boundingBox(Plug_Entity *entity, QPointF *min,
                               QPointF *max) const
{
    if (!m_versionOk)
        return false;
    RS_Entity *rsEntity = underlyingEntity(entity);
    if (!rsEntity)
        return false;
    // Inline accessors returning RS_Vector by value; x, y, valid are public.
    const RS_Vector lo = rsEntity->getMin();
    const RS_Vector hi = rsEntity->getMax();
    if (!lo.valid || !hi.valid)
        return false;
    *min = QPointF(lo.x, lo.y);
    *max = QPointF(hi.x, hi.y);
    return true;
}

bool NativeBridge::isUndone(Plug_Entity *entity, bool *undone) const
{
    if (!m_versionOk)
        return false;
    RS_Entity *rsEntity = underlyingEntity(entity);
    if (!rsEntity)
        return false;
    *undone = rsEntity->isUndone();   // RS_Undoable::isUndone, bound symbol
    return true;
}

const void *NativeBridge::entityKey(Plug_Entity *entity) const
{
    return m_versionOk ? underlyingEntity(entity) : nullptr;
}

bool NativeBridge::offset(const QPointF &side, double distance, int number,
                          bool keepOriginal, bool useCurrentLayer,
                          bool useCurrentAttributes)
{
    if (!modificationAvailable())
        return false;
    RS_OffsetData data;
    data.coord = toVector(side);
    data.distance = distance;
    // number == 0 is RS_Modification's "replace the originals" mode.
    data.number = keepOriginal ? qMax(1, number) : 0;
    data.useCurrentLayer = useCurrentLayer;
    data.useCurrentAttributes = useCurrentAttributes;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.offset(data)) {
        m_lastError = QStringLiteral("RS_Modification::offset refused");
        return false;
    }
    return true;
}

bool NativeBridge::mirror(const QPointF &axisP1, const QPointF &axisP2, bool copy)
{
    if (!modificationAvailable())
        return false;
    RS_MirrorData data;
    data.axisPoint1 = toVector(axisP1);
    data.axisPoint2 = toVector(axisP2);
    data.copy = copy;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.mirror(data)) {
        m_lastError = QStringLiteral("RS_Modification::mirror refused");
        return false;
    }
    return true;
}

bool NativeBridge::explode(bool remove)
{
    if (!modificationAvailable())
        return false;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.explode(remove)) {
        m_lastError = QStringLiteral("RS_Modification::explode refused "
                                     "(locked or hidden container?)");
        return false;
    }
    return true;
}

bool NativeBridge::trim(Plug_Entity *trimEntity, const QPointF &trimPoint,
                        Plug_Entity *limitEntity, const QPointF &limitPoint,
                        bool both)
{
    if (!modificationAvailable())
        return false;
    RS_Entity *toTrim = underlyingEntity(trimEntity);
    RS_Entity *limit = underlyingEntity(limitEntity);
    if (!toTrim || !limit) {
        m_lastError = QStringLiteral("entity not found");
        return false;
    }
    if (!toTrim->isAtomic()) {
        m_lastError = QStringLiteral("only atomic entities (line, arc, "
                                     "circle, ellipse) can be trimmed");
        return false;
    }
    if (both && !limit->isAtomic()) {
        m_lastError = QStringLiteral("\"both\" needs an atomic limit entity");
        return false;
    }
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.trim(toVector(trimPoint),
                           static_cast<RS_AtomicEntity *>(toTrim),
                           toVector(limitPoint), limit, both)) {
        m_lastError = QStringLiteral("RS_Modification::trim refused "
                                     "(no intersection, or locked/hidden)");
        return false;
    }
    return true;
}

bool NativeBridge::fileInfo(QString *path, bool *modified) const
{
    if (!modificationAvailable())
        return false;
    *path = m_document->getFilename();      // inline accessor
    *modified = m_document->isModified();   // virtual
    return true;
}

bool NativeBridge::saveAs(const QString &path, const QString &format)
{
    if (!modificationAvailable())
        return false;
    RS2::FormatType type = RS2::FormatUnknown;
    if (format.isEmpty()) {
        type = RS_FileIO::detectFormat(path, false);   // bound symbol
        if (type == RS2::FormatUnknown) {
            m_lastError = QStringLiteral("cannot tell the format from the "
                                         "extension of \"%1\"; pass \"format\"")
                              .arg(path);
            return false;
        }
    } else if (!formatFromName(format, &type)) {
        m_lastError = QStringLiteral("unknown format \"%1\"").arg(format);
        return false;
    }
    // force: write even when nothing is flagged modified, and (re)adopt the
    // name. RS_Graphic::saveAs restores the old name if the write fails.
    if (!m_document->saveAs(path, type, true)) {
        m_lastError = QStringLiteral("LibreCAD could not write \"%1\"").arg(path);
        return false;
    }
    // What File > Save does afterwards, minus the recent-files menu (its
    // caption helper is private; the file name is what it shows anyway).
    if (QC_ApplicationWindow *app = QC_ApplicationWindow::getAppWindow().get()) {
        if (QC_MDIWindow *window = app->getMDIWindow()) {
            window->setWindowTitle(QFileInfo(path).fileName()
                                   + QStringLiteral("[*]"));
            window->setWindowModified(false);
        }
    }
    return true;
}

bool NativeBridge::undoCheckpoint()
{
    if (!modificationAvailable())
        return false;
    // Virtual (RS_Undo via RS_Document): refCount 1 -> 0, the cycle is kept
    // if it has undoables. ensureUndoCycle() opens the next one on demand.
    if (m_undoCycleOpen) {
        m_document->endUndoCycle();
        m_undoCycleOpen = false;
    }
    return true;
}

bool NativeBridge::undo(int steps, bool redo, int *done)
{
    if (!modificationAvailable())
        return false;
    undoCheckpoint();
    *done = 0;
    for (int i = 0; i < steps; ++i) {
        if (!(redo ? m_document->redo() : m_document->undo()))
            break;
        ++*done;
    }
    return true;
}

void NativeBridge::ensureUndoCycle()
{
    if (!modificationAvailable() || m_undoCycleOpen)
        return;
    m_document->startUndoCycle();   // discards the redo list, like any action
    m_undoCycleOpen = true;
}

void NativeBridge::armHatchDialog(const QString &pattern, double scaleFactor,
                                  double angleDegrees, bool solid, int timeoutMs)
{
    m_hatchPattern = pattern;
    m_hatchScale = scaleFactor;
    m_hatchAngleDegrees = angleDegrees;
    m_hatchSolid = solid;
    m_hatchDialogHandled = false;
    if (!m_hatchTimer)
        return;
    m_hatchPollsLeft = qMax(1, timeoutMs / qMax(1, m_hatchTimer->interval()));
    m_hatchTimer->start();
}

void NativeBridge::disarmHatchDialog()
{
    if (m_hatchTimer)
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
        if (!top->isVisible() || !isInstanceOf(top, "QG_DlgHatch"))
            continue;
        auto *dialog = static_cast<QG_DlgHatch *>(top);

        dialog->cbSolid->setChecked(m_hatchSolid);
        // setPattern() adds a name the pattern list does not know, selects
        // it in cbPattern, and refreshes the dialog's own RS_Pattern.
        dialog->setPattern(m_hatchPattern);
        dialog->leScale->setText(QString::number(m_hatchScale));
        dialog->leAngle->setText(QString::number(m_hatchAngleDegrees));

        m_hatchDialogHandled = true;
        m_hatchTimer->stop();
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        return;
    }
}

// --------------------------------------------------------------------------
// Prompts and push events
// --------------------------------------------------------------------------

QWidget *NativeBridge::mainWindow() const
{
    return m_mainWindow;
}

void NativeBridge::armPrompt(PromptKind kind, int timeoutMs,
                             const QVariant &dialogDefault)
{
    m_promptKind = kind;
    m_promptTimeoutMs = timeoutMs;
    m_promptDefault = dialogDefault;
    m_promptDefaultApplied = false;
    m_promptCancelled = false;
    m_promptAction.clear();
    m_promptArmed = true;
    m_promptClock.start();
    // Always polled while armed, even with no default and no deadline: a
    // point prompt's action is captured on the first tick so that the Stop
    // button can cancel it cleanly later.
    m_promptTimer->start();
}

bool NativeBridge::disarmPrompt()
{
    m_promptTimer->stop();
    m_promptArmed = false;
    m_promptAction.clear();
    return m_promptCancelled;
}

void NativeBridge::pollPrompt()
{
    if (!m_promptArmed) {
        m_promptTimer->stop();
        return;
    }

    if (m_promptKind == DialogPrompt && !m_promptDefaultApplied
        && m_promptDefault.isValid()) {
        // Doc_plugin_interface::getInt/getReal/getString call the static
        // QInputDialog helpers, which always start at 0 or empty; filling in
        // the dialog the helper opened is the only way to offer a default.
        for (QWidget *top : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QInputDialog *>(top);   // Qt class: no binding
            if (!dialog || !dialog->isVisible())
                continue;
            switch (dialog->inputMode()) {
            case QInputDialog::IntInput:
                dialog->setIntValue(m_promptDefault.toInt());
                break;
            case QInputDialog::DoubleInput:
                dialog->setDoubleValue(m_promptDefault.toDouble());
                break;
            case QInputDialog::TextInput:
                dialog->setTextValue(m_promptDefault.toString());
                break;
            }
            m_promptDefaultApplied = true;
            break;
        }
    }

    // getPoint() installs its QC_ActionGetPoint before it spins; remember it
    // -- weakly: it is a QObject, and the user may finish it at any moment --
    // so that cancelPrompt() can complete that very action.
    if (m_promptKind == PointPrompt && !m_promptAction && m_versionOk
        && m_graphicView) {
        RS_ActionInterface *action = m_graphicView->getCurrentAction();
        if (action && isInstanceOf(action, "QC_ActionGetPoint"))
            m_promptAction = action;
    }

    if (m_promptTimeoutMs > 0 && m_promptClock.elapsed() >= m_promptTimeoutMs)
        cancelPrompt();
}

void NativeBridge::cancelPrompt()
{
    if (!m_promptArmed || m_promptCancelled)
        return;
    m_promptCancelled = true;
    m_promptTimer->stop();

    if (m_promptKind == DialogPrompt) {
        // The static QInputDialog helpers report ok == false on reject().
        // Queued, so the dialog's exec() loop sees it on its next turn rather
        // than from inside this timer callback.
        for (QWidget *top : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QInputDialog *>(top);
            if (dialog && dialog->isVisible())
                QMetaObject::invokeMethod(dialog, "reject", Qt::QueuedConnection);
        }
        return;
    }

    if (!m_versionOk || !m_graphicView)
        return;

    if (m_promptKind == PointPrompt && m_promptAction) {
        // Not killAllActions(): the action stack owns the action through a
        // shared_ptr, and getPoint() dereferences its raw pointer after the
        // loop (a->isCompleted(), a->wasCanceled()), so emptying the stack
        // under it leaves getPoint() reading freed memory. trigger() only
        // sets "completed"; the action stays alive, getPoint() leaves its
        // loop normally and kills the action itself. It then reports the
        // last mouse position as the answer, which the dispatcher discards
        // because disarmPrompt() says the prompt was cancelled.
        static_cast<RS_ActionInterface *>(m_promptAction.data())->trigger();
        return;
    }

    // getSelect(): its loop ends when the action stack is empty, and after
    // the loop it only compares the action pointer (isValid), never
    // dereferences it, so emptying the stack is safe there. Also the
    // fallback for a point prompt whose action was never seen.
    m_graphicView->killAllActions();
}

bool NativeBridge::census(int *live, int *selected) const
{
    if (!modificationAvailable())
        return false;
    int liveCount = 0;
    int selectedCount = 0;
    // RS_EntityContainer::begin()/end() const are exported functions; the
    // undone filter is the one every census in the dispatcher applies.
    const RS_EntityContainer &container = *m_document;
    for (RS_Entity *entity : container) {
        if (!entity || entity->isUndone())
            continue;
        ++liveCount;
        if (entity->isSelected())
            ++selectedCount;
    }
    *live = liveCount;
    *selected = selectedCount;
    return true;
}

bool NativeBridge::viewState(double *factor, int *offsetX, int *offsetY) const
{
    if (!m_versionOk || !m_graphicView)
        return false;
    *factor = m_graphicView->getFactor().x;   // three bound symbols
    *offsetX = m_graphicView->getOffsetX();
    *offsetY = m_graphicView->getOffsetY();
    return true;
}

bool NativeBridge::gridState(bool *on) const
{
    if (!modificationAvailable())
        return false;
    // What RS_Graphic::isGridOn() does, default included (an unset
    // $GRIDMODE means on). The plugin API's getVariableInt() cannot tell an
    // unset variable from 0, and QC_ApplicationWindow's gridChanged signal
    // only fires on window activation and file loads, not when the grid is
    // toggled -- so the state is polled.
    *on = m_document->getGraphicVariableInt(QStringLiteral("$GRIDMODE"), 1) != 0;
    return true;
}

} // namespace lcbridge
