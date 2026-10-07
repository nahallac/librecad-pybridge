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
#include "rs_layer.h"               // layer state, rename
#include "rs_layerlist.h"
#include "rs_block.h"               // block definition and removal
#include "rs_blocklist.h"
#include "rs_creation.h"            // RS_Creation::createBlock
#include "rs_insert.h"
#include "rs_information.h"         // intersections, point-in-contour
#include "rs_circle.h"
#include "rs_ellipse.h"
#include "rs_polyline.h"
#include "rs_graphic.h"             // paper size, scale, margins
#include "rs_graphicview.h"         // zoom, factor, offset
#include "rs_staticgraphicview.h"   // off-screen rendering for export
#include "rs_painterqt.h"           // the painter LibreCAD exports with
#include "rs_units.h"               // paper formats, unit conversion
#include "rs_actioninterface.h"     // RS_ActionInterface::trigger, for prompts

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QImageWriter>
#include <QInputDialog>
#include <QLineEdit>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPageLayout>
#include <QPageSize>
#include <QPixmap>
#include <QPrinter>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

#include <cmath>

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


//! Hide a pointer's provenance from the optimiser. Deleting a LibreCAD object
//! the plugin allocated must go through the object's vtable: if the compiler
//! can see the dynamic type it calls the destructor directly, and the inline
//! (implicit or defaulted) destructors of RS_PainterQt and
//! RS_StaticGraphicView store their class's vtable pointer -- a data symbol,
//! which would stop the plugin loading outside LibreCAD.
template <typename T>
T *opaque(T *pointer)
{
    asm volatile("" : "+r"(pointer));
    return pointer;
}

//! The QC_MDIWindow at \a index in the MDI area, or nullptr.
QC_MDIWindow *mdiWindowAt(QC_ApplicationWindow *app, int index)
{
    QMdiArea *area = app->getMdiArea();
    if (!area)
        return nullptr;
    const QList<QMdiSubWindow *> windows = area->subWindowList();
    if (index < 0 || index >= windows.size())
        return nullptr;
    QMdiSubWindow *window = windows.at(index);
    return isInstanceOf(window, "QC_MDIWindow") ? static_cast<QC_MDIWindow *>(window)
                                                : nullptr;
}

//! Draw the whole drawing into \a device, scaled to fit inside \a size less
//! \a borders -- what QC_ApplicationWindow::slotFileExport does, with the
//! painter and view on the heap (see opaque()). White-background semantics:
//! entities drawn in white come out black.
void renderFitted(QPaintDevice *device, RS_Graphic *graphic, const QSize &size,
                  const QSize &borders, bool printing)
{
    auto *painter = new RS_PainterQt(device);
    auto *view = new RS_StaticGraphicView(size.width(), size.height(), painter,
                                          &borders);
    view->setPrinting(printing);
    view->setContainer(graphic);
    view->zoomAuto(false, true);
    view->drawEntity(painter, view->getContainer());
    painter->end();
    delete opaque<QWidget>(view);
    delete opaque<RS_Painter>(painter);
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
    case SessionRestart::ActivateWindow: {
        QC_MDIWindow *window = mdiWindowAt(app, restart.index);
        if (!window) {
            *error = QStringLiteral("document window %1 no longer exists")
                         .arg(restart.index);
            return false;
        }
        // What clicking the window does: QMdiArea activates it and emits
        // subWindowActivated, which LibreCAD connects to
        // slotWindowActivated(QMdiSubWindow*) -- that switches the layer and
        // block lists, the action handler, and getMDIWindow(), which is what
        // execPlug() binds the next session to.
        window->show();
        app->getMdiArea()->setActiveSubWindow(window);
        if (app->getMDIWindow() != window) {
            *error = QStringLiteral("LibreCAD did not activate the window");
            return false;
        }
        return true;
    }
    case SessionRestart::CloseWindow: {
        QC_MDIWindow *window = app->getMDIWindow();
        if (!window || !window->getDocument()) {
            *error = QStringLiteral("no current document window");
            return false;
        }
        RS_Document *document = window->getDocument();
        // QC_MDIWindow::closeEvent asks about unsaved changes with a modal
        // dialog, which nothing would answer in an unattended run. The server
        // refused a modified document without "discard"; this is the last
        // check, after execPlug() closed its undo cycle (which is when
        // RS_Document sets its modified flag).
        if (restart.discard) {
            document->setModified(false);   // inline; closeEvent reads it
        } else if (document->isModified()) {
            *error = QStringLiteral("the drawing has unsaved changes");
            return false;
        }
        // The window's own close path: closeEvent -> doClose(), which also
        // closes its block editors and print previews and activates the last
        // remaining window, if any. WA_DeleteOnClose disposes of it.
        window->close();
        return true;
    }
    case SessionRestart::None:
        break;
    }
    *error = QStringLiteral("nothing to do");
    return false;
}

bool documentWindows(QList<DocumentWindow> *windows)
{
    windows->clear();
    if (!versionMatches())
        return false;
    QC_ApplicationWindow *app = QC_ApplicationWindow::getAppWindow().get();
    if (!app || !app->getMdiArea())
        return false;
    const QC_MDIWindow *current = app->getMDIWindow();
    const QList<QMdiSubWindow *> list = app->getMdiArea()->subWindowList();
    for (int i = 0; i < list.size(); ++i) {
        if (!isInstanceOf(list.at(i), "QC_MDIWindow"))
            continue;
        auto *window = static_cast<QC_MDIWindow *>(list.at(i));
        RS_Document *document = window->getDocument();
        DocumentWindow info;
        info.index = i;
        info.title = window->windowTitle().remove(QStringLiteral("[*]"));
        info.active = window == current;
        if (QC_MDIWindow *parent = window->getParentWindow())
            info.parent = list.indexOf(parent);
        if (document) {
            info.path = document->getFilename();
            // hasUndoable() is virtual: an undo cycle still open (the
            // session's) holds changes the modified flag does not show yet.
            info.modified = document->isModified() || document->hasUndoable();
        }
        windows->append(info);
    }
    return true;
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
    // execPlug() opened the session's undo cycle just before execComm().
    snapshotCycleStart();

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
    // Saving closes the current undo step first. RS_Document::endUndoCycle()
    // sets the modified flag when the cycle holds changes, so a save inside
    // the still-open cycle would be marked unsaved again the moment the
    // session ends -- and closing the window would then ask about it.
    undoCheckpoint();
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
    snapshotCycleStart();
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

// ---------------------------------------------------------------------------
// Layer state, block definition, geometry queries
// ---------------------------------------------------------------------------

bool NativeBridge::layerState(const QString &name, LayerState *state) const
{
    if (!modificationAvailable())
        return false;
    // getLayerList() is virtual on RS_Document: the graphic's list, also when
    // the active window is a block being edited.
    RS_LayerList *layers = m_document->getLayerList();
    RS_Layer *layer = layers ? layers->find(name) : nullptr;
    if (!layer)
        return false;
    state->frozen = layer->isFrozen();
    state->locked = layer->isLocked();
    state->print = layer->isPrint();
    state->construction = layer->isConstruction();
    return true;
}

NativeBridge::Result NativeBridge::setLayerState(const QString &name,
                                                 const LayerStatePatch &patch)
{
    if (!modificationAvailable())
        return Result::Failed;
    RS_LayerList *layers = m_document->getLayerList();
    RS_Layer *layer = layers ? layers->find(name) : nullptr;
    if (!layer) {
        m_lastError = QStringLiteral("no layer \"%1\"").arg(name);
        return Result::NotFound;
    }

    // The same calls the layer widget's checkboxes end in. Each takes the
    // layers to switch one way and the layers to switch the other way, sets
    // the flags, and fires layerToggled on every listener (which redraws the
    // view and refreshes the layer list). The layer goes in exactly one of
    // the two lists per call.
    const QList<RS_Layer *> one{layer};
    const QList<RS_Layer *> none;
    if (patch.frozen) {
        if (*patch.frozen)
            layers->setFreezeMulti(none, one);
        else
            layers->setFreezeMulti(one, none);
    }
    if (patch.locked) {
        if (*patch.locked)
            layers->setLockMulti(none, one);
        else
            layers->setLockMulti(one, none);
    }
    if (patch.print) {
        if (*patch.print)
            layers->setPrintMulti(none, one);
        else
            layers->setPrintMulti(one, none);
    }
    if (patch.construction) {
        if (*patch.construction)
            layers->setConstructionMulti(none, one);
        else
            layers->setConstructionMulti(one, none);
    }
    return Result::Done;
}

NativeBridge::Result NativeBridge::renameLayer(const QString &oldName,
                                               const QString &newName)
{
    if (!modificationAvailable())
        return Result::Failed;
    RS_LayerList *layers = m_document->getLayerList();
    RS_Layer *layer = layers ? layers->find(oldName) : nullptr;
    if (!layer) {
        m_lastError = QStringLiteral("no layer \"%1\"").arg(oldName);
        return Result::NotFound;
    }
    if (newName.isEmpty()) {
        m_lastError = QStringLiteral("the new layer name is empty");
        return Result::Refused;
    }
    if (layers->find(newName)) {
        m_lastError = QStringLiteral("a layer named \"%1\" already exists")
                          .arg(newName);
        return Result::Refused;
    }
    if (oldName == QLatin1String("0")) {
        m_lastError = QStringLiteral("layer \"0\" cannot be renamed "
                                     "(DXF requires it)");
        return Result::Refused;
    }

    // What the layer dialog does (RS_ActionLayersEdit): hand edit() a layer
    // carrying the new name; it copies it over the old one in place and fires
    // layerEdited. The RS_Layer object, and so every entity's pointer to it,
    // stays the same. clone() is a bound symbol; the copy is deleted here.
    RS_Layer *renamed = layer->clone();
    renamed->setName(newName);
    layers->edit(layer, *renamed);
    delete renamed;

    // The action then refreshes entities that depend on the layer.
    for (RS_Entity *entity : *m_document) {
        if (entity->getLayer(false) == layer)
            entity->update();
    }
    return Result::Done;
}

bool NativeBridge::blockNames(QStringList *names) const
{
    if (!modificationAvailable())
        return false;
    RS_BlockList *blocks = m_document->getBlockList();
    if (!blocks)
        return false;
    for (int i = 0; i < blocks->count(); ++i) {
        RS_Block *block = blocks->at(i);
        if (!block->isUndone())
            names->append(block->getName());
    }
    return true;
}

NativeBridge::Result NativeBridge::defineBlock(const QString &name,
                                               const QPointF &base,
                                               bool remove, int *selected)
{
    if (!modificationAvailable())
        return Result::Failed;
    RS_BlockList *blocks = m_document->getBlockList();
    if (!blocks)
        return Result::Failed;
    if (name.isEmpty()) {
        m_lastError = QStringLiteral("the block name is empty");
        return Result::Refused;
    }
    // RS_BlockList::add() deletes a block whose name is taken, and
    // createBlock() would hand back that dangling pointer, so check first.
    // find() also sees a removed block that is only held for undo.
    if (blocks->find(name)) {
        m_lastError = QStringLiteral("a block named \"%1\" already exists "
                                     "(or was removed and is held for undo)")
                          .arg(name);
        return Result::Refused;
    }

    *selected = 0;
    for (RS_Entity *entity : *m_document) {
        if (entity->isSelected() && !entity->isUndone())
            ++*selected;
    }
    if (*selected == 0) {
        m_lastError = QStringLiteral("none of the entities is in the drawing");
        return Result::Refused;
    }

    // RS_BlockData's constructor is a bound symbol, but its members are
    // public: fill them in instead.
    RS_BlockData data;
    data.name = name;
    data.basePoint = toVector(base);
    data.frozen = false;

    // Create Block's own call (RS_ActionBlocksCreate::trigger): clones the
    // selected entities into a new block translated by -base, optionally
    // removes the originals under an undo section, and adds the block to the
    // graphic's list. The session's undo cycle is already open, so the
    // section it starts nests inside it.
    RS_Creation creation(m_document, m_graphicView);
    RS_Block *block = creation.createBlock(&data, data.basePoint, remove);
    if (!block || !blocks->find(name)) {
        m_lastError = QStringLiteral("RS_Creation::createBlock did not add "
                                     "the block");
        return Result::Failed;
    }
    return Result::Done;
}

namespace {

//! Live INSERTs of \a name directly inside \a container.
int countInsertsIn(RS_EntityContainer *container, const QString &name)
{
    int count = 0;
    for (RS_Entity *entity : *container) {
        if (entity->rtti() == RS2::EntityInsert && !entity->isUndone()
            && static_cast<RS_Insert *>(entity)->getName() == name) {
            ++count;
        }
    }
    return count;
}

//! The container that holds the drawing's own entities (and its inserts):
//! the graphic, also when the active document is a block being edited.
RS_EntityContainer *drawingContainer(RS_Document *document)
{
    RS_Graphic *graphic = document->getGraphic();
    return graphic ? static_cast<RS_EntityContainer *>(graphic)
                   : static_cast<RS_EntityContainer *>(document);
}

} // namespace

// --------------------------------------------------------------------------
// Modify tools: the rest of RS_Modification
// --------------------------------------------------------------------------

namespace {

//! Line type names as the plugin API spells them (convLTW in
//! doc_plugin_interface.cpp), so change_attributes takes what entity data
//! reports. The table is repeated rather than reached: LibreCAD's converter
//! is a global object, and a data symbol would stop the plugin loading
//! outside LibreCAD.
bool lineTypeFromName(const QString &name, RS2::LineType *type)
{
    static const struct { const char *name; RS2::LineType type; } table[] = {
        {"BYLAYER", RS2::LineByLayer},       {"BYBLOCK", RS2::LineByBlock},
        {"SolidLine", RS2::SolidLine},
        {"DotLine", RS2::DotLine},           {"DotLine2", RS2::DotLine2},
        {"DotLineX2", RS2::DotLineX2},
        {"DashLine", RS2::DashLine},         {"DashLine2", RS2::DashLine2},
        {"DashLineX2", RS2::DashLineX2},
        {"DashDotLine", RS2::DashDotLine},   {"DashDotLine2", RS2::DashDotLine2},
        {"DashDotLineX2", RS2::DashDotLineX2},
        {"DivideLine", RS2::DivideLine},     {"DivideLine2", RS2::DivideLine2},
        {"DivideLineX2", RS2::DivideLineX2},
        {"CenterLine", RS2::CenterLine},     {"CenterLine2", RS2::CenterLine2},
        {"CenterLineX2", RS2::CenterLineX2},
        {"BorderLine", RS2::BorderLine},     {"BorderLine2", RS2::BorderLine2},
        {"BorderLineX2", RS2::BorderLineX2},
    };
    for (const auto &entry : table) {
        if (name.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0) {
            *type = entry.type;
            return true;
        }
    }
    return false;
}

//! Line width names as the plugin API spells them ("0.25mm", "BYLAYER").
bool lineWidthFromName(const QString &name, RS2::LineWidth *width)
{
    static const struct { const char *name; RS2::LineWidth width; } table[] = {
        {"0.00mm", RS2::Width00}, {"0.05mm", RS2::Width01},
        {"0.09mm", RS2::Width02}, {"0.13mm", RS2::Width03},
        {"0.15mm", RS2::Width04}, {"0.18mm", RS2::Width05},
        {"0.20mm", RS2::Width06}, {"0.25mm", RS2::Width07},
        {"0.30mm", RS2::Width08}, {"0.35mm", RS2::Width09},
        {"0.40mm", RS2::Width10}, {"0.50mm", RS2::Width11},
        {"0.53mm", RS2::Width12}, {"0.60mm", RS2::Width13},
        {"0.70mm", RS2::Width14}, {"0.80mm", RS2::Width15},
        {"0.90mm", RS2::Width16}, {"1.00mm", RS2::Width17},
        {"1.06mm", RS2::Width18}, {"1.20mm", RS2::Width19},
        {"1.40mm", RS2::Width20}, {"1.58mm", RS2::Width21},
        {"2.00mm", RS2::Width22}, {"2.11mm", RS2::Width23},
        {"BYLAYER", RS2::WidthByLayer}, {"BYBLOCK", RS2::WidthByBlock},
        {"BYDEFAULT", RS2::WidthDefault},
    };
    for (const auto &entry : table) {
        if (name.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0) {
            *width = entry.width;
            return true;
        }
    }
    return false;
}

} // namespace

int NativeBridge::blockInsertCount(const QString &name) const
{
    if (!modificationAvailable())
        return -1;
    int count = countInsertsIn(drawingContainer(m_document), name);
    // Inserts nested in other block definitions count as references too
    // (RS_ActionBlocksRemove looks in the same places).
    if (RS_BlockList *blocks = m_document->getBlockList()) {
        for (int i = 0; i < blocks->count(); ++i) {
            RS_Block *block = blocks->at(i);
            if (!block->isUndone())
                count += countInsertsIn(block, name);
        }
    }
    return count;
}

NativeBridge::Result NativeBridge::renameBlock(const QString &oldName,
                                               const QString &newName)
{
    if (!modificationAvailable())
        return Result::Failed;
    RS_BlockList *blocks = m_document->getBlockList();
    RS_Block *block = blocks ? blocks->find(oldName) : nullptr;
    if (!block || block->isUndone()) {
        m_lastError = QStringLiteral("no block \"%1\"").arg(oldName);
        return Result::NotFound;
    }
    if (newName.isEmpty()) {
        m_lastError = QStringLiteral("the new block name is empty");
        return Result::Refused;
    }
    if (blocks->find(newName)) {
        m_lastError = QStringLiteral("a block named \"%1\" already exists "
                                     "(or was removed and is held for undo)")
                          .arg(newName);
        return Result::Refused;
    }
    if (!blocks->rename(block, newName)) {
        m_lastError = QStringLiteral("RS_BlockList::rename refused");
        return Result::Failed;
    }
    // RS_BlockList::rename() renames the block and only the inserts nested in
    // *other blocks*; the inserts in the drawing are the caller's business,
    // which is why the Block Attributes action calls renameInserts() next.
    RS_EntityContainer *top = drawingContainer(m_document);
    top->renameInserts(oldName, newName);   // virtual
    blocks->addNotification();
    top->updateInserts();                   // virtual
    return Result::Done;
}

NativeBridge::Result NativeBridge::removeBlock(const QString &name)
{
    if (!modificationAvailable())
        return Result::Failed;
    RS_BlockList *blocks = m_document->getBlockList();
    RS_Block *block = blocks ? blocks->find(name) : nullptr;
    if (!block || block->isUndone()) {
        m_lastError = QStringLiteral("no block \"%1\"").arg(name);
        return Result::NotFound;
    }
    const int inserts = blockInsertCount(name);
    if (inserts > 0) {
        m_lastError = QStringLiteral("block \"%1\" is still referenced by %2 "
                                     "insert(s); remove them first")
                          .arg(name).arg(inserts);
        return Result::Refused;
    }

    // RS_ActionBlocksRemove, minus the dialog and the insert clean-up (there
    // are none): the block stays in the list, flagged undone, and is
    // registered with the undo cycle so undo brings it back. Everything that
    // lists blocks has to skip undone ones; blockNames() does.
    block->selectedInBlockList(false);
    if (block == blocks->getActive())
        blocks->activate(static_cast<RS_Block *>(nullptr));
    block->setUndoState(true);
    m_document->addUndoable(block);
    blocks->addNotification();
    drawingContainer(m_document)->updateInserts();
    return Result::Done;
}

NativeBridge::Result NativeBridge::blockEntities(const QString &name,
                                                 Document_Interface *doc,
                                                 QList<Plug_Entity *> *out)
{
    if (!modificationAvailable() || !doc)
        return Result::Failed;
    RS_BlockList *blocks = m_document->getBlockList();
    RS_Block *block = blocks ? blocks->find(name) : nullptr;
    if (!block || block->isUndone()) {
        m_lastError = QStringLiteral("no block \"%1\"").arg(name);
        return Result::NotFound;
    }
    // The same wrapper getAllEntities() builds. Doc_plugin_interface is the
    // class behind every Document_Interface LibreCAD hands to a plugin. The
    // wrapper does not own the entity. An entity with no resolvable layer is
    // left out: Plugin_Entity::getData() dereferences it.
    auto *dpi = static_cast<Doc_plugin_interface *>(doc);
    for (RS_Entity *entity : *block) {
        if (entity->isUndone() || !entity->getLayer())
            continue;
        out->append(reinterpret_cast<Plug_Entity *>(
            new Plugin_Entity(entity, dpi)));
    }
    return Result::Done;
}

bool NativeBridge::entityLength(Plug_Entity *entity, double *length) const
{
    RS_Entity *rsEntity = m_versionOk ? underlyingEntity(entity) : nullptr;
    if (!rsEntity)
        return false;
    // Virtual. Negative means "no length" (text, hatch, image, ...).
    const double value = rsEntity->getLength();
    if (value < 0.0)
        return false;
    *length = value;
    return true;
}

bool NativeBridge::entityArea(Plug_Entity *entity, double *area,
                              bool *meaningful) const
{
    RS_Entity *rsEntity = m_versionOk ? underlyingEntity(entity) : nullptr;
    if (!rsEntity)
        return false;
    *area = 0.0;
    *meaningful = false;
    // areaLineIntegral() is Green's theorem along the entity, from start to
    // end: for anything that is not a closed loop it is just that integral,
    // not an area. So only the three shapes that enclose one are asked.
    switch (rsEntity->rtti()) {
    case RS2::EntityCircle:
        *meaningful = true;
        break;
    case RS2::EntityEllipse:
        *meaningful = !static_cast<RS_Ellipse *>(rsEntity)->isEllipticArc();
        break;
    case RS2::EntityPolyline:
        *meaningful = static_cast<RS_Polyline *>(rsEntity)->isClosed();
        break;
    default:
        break;
    }
    if (*meaningful)
        *area = rsEntity->areaLineIntegral();   // virtual
    return true;
}

bool NativeBridge::intersections(Plug_Entity *a, Plug_Entity *b,
                                 bool onEntities, QList<QPointF> *points) const
{
    RS_Entity *first = m_versionOk ? underlyingEntity(a) : nullptr;
    RS_Entity *second = m_versionOk ? underlyingEntity(b) : nullptr;
    if (!first || !second)
        return false;
    const RS_VectorSolutions solutions =
        RS_Information::getIntersection(first, second, onEntities);   // bound
    for (size_t i = 0; i < solutions.size(); ++i) {
        const RS_Vector point = solutions.get(i);
        if (point.valid)
            points->append(QPointF(point.x, point.y));
    }
    return true;
}

bool NativeBridge::entityDistance(Plug_Entity *entity, const QPointF &point,
                                  double *distance) const
{
    RS_Entity *rsEntity = m_versionOk ? underlyingEntity(entity) : nullptr;
    // isVisible() is false for undone entities and those on frozen layers,
    // which is what the pick tools skip as well.
    if (!rsEntity || !rsEntity->isVisible())
        return false;
    // Virtual; the arguments after the point are the declared defaults,
    // spelled out because default arguments are resolved at the call site.
    const double value = rsEntity->getDistanceToPoint(
        toVector(point), nullptr, RS2::ResolveNone, RS_MAXDOUBLE);
    if (value >= RS_MAXDOUBLE)
        return false;
    *distance = qMax(0.0, value);   // negative: inside a solid
    return true;
}

bool NativeBridge::nearestPoint(Plug_Entity *entity, const QPointF &point,
                                bool onEntity, QPointF *nearest,
                                double *distance) const
{
    RS_Entity *rsEntity = m_versionOk ? underlyingEntity(entity) : nullptr;
    if (!rsEntity)
        return false;
    double value = 0.0;
    const RS_Vector found = rsEntity->getNearestPointOnEntity(
        toVector(point), onEntity, &value, nullptr);   // virtual
    if (!found.valid)
        return false;
    *nearest = QPointF(found.x, found.y);
    *distance = value;
    return true;
}

NativeBridge::Result NativeBridge::pointInside(Plug_Entity *entity,
                                               const QPointF &point,
                                               bool *inside, bool *onContour)
{
    RS_Entity *rsEntity = m_versionOk ? underlyingEntity(entity) : nullptr;
    if (!rsEntity)
        return Result::Failed;
    *inside = false;
    *onContour = false;
    constexpr double tolerance = 1.0e-6;

    switch (rsEntity->rtti()) {
    case RS2::EntityPolyline: {
        auto *polyline = static_cast<RS_Polyline *>(rsEntity);
        if (!polyline->isClosed()) {
            m_lastError = QStringLiteral("the polyline is not closed");
            return Result::Refused;
        }
        // isPointInsideContour() casts a ray and counts crossings with the
        // contour's atomic members (it resolves nested containers itself),
        // so a closed polyline is a valid contour as it stands. It reports
        // points exactly on the boundary as onContour and may or may not
        // count them as inside.
        bool on = false;
        *inside = RS_Information::isPointInsideContour(
            toVector(point), polyline, &on);   // bound
        *onContour = on;
        return Result::Done;
    }
    case RS2::EntityCircle: {
        auto *circle = static_cast<RS_Circle *>(rsEntity);
        const RS_Vector center = circle->getCenter();
        const double radius = circle->getRadius();
        const double distance = std::hypot(point.x() - center.x,
                                           point.y() - center.y);
        *onContour = std::abs(distance - radius) <= tolerance;
        *inside = distance <= radius + tolerance;
        return Result::Done;
    }
    case RS2::EntityEllipse: {
        auto *ellipse = static_cast<RS_Ellipse *>(rsEntity);
        if (ellipse->isEllipticArc()) {
            m_lastError = QStringLiteral("an elliptic arc encloses nothing");
            return Result::Refused;
        }
        // Into the ellipse's own frame: u along the major axis.
        const RS_Vector center = ellipse->getCenter();
        const RS_Vector major = ellipse->getMajorP();
        const double a = std::hypot(major.x, major.y);
        const double b = a * ellipse->getRatio();
        const double dx = point.x() - center.x;
        const double dy = point.y() - center.y;
        const double u = (dx * major.x + dy * major.y) / a;
        const double v = (-dx * major.y + dy * major.x) / a;
        const double value = (u * u) / (a * a) + (v * v) / (b * b);
        *onContour = std::abs(value - 1.0) <= tolerance;
        *inside = value <= 1.0 + tolerance;
        return Result::Done;
    }
    default:
        break;
    }
    m_lastError = QStringLiteral("only a closed polyline, circle, or full "
                                 "ellipse encloses a region");
    return Result::Refused;
}

// --------------------------------------------------------------------------
// View control, document windows, export
// --------------------------------------------------------------------------

bool NativeBridge::viewAvailable() const
{
    return m_versionOk && m_graphicView != nullptr;
}

bool NativeBridge::viewState(ViewState *state) const
{
    if (!viewAvailable())
        return false;
    // getFactor/getOffset/toGraph are exported non-virtuals; getWidth/
    // getHeight are virtual (QG_GraphicView's widget size).
    const RS_Vector factor = m_graphicView->getFactor();
    state->factorX = factor.x;
    state->factorY = factor.y;
    state->offsetX = m_graphicView->getOffsetX();
    state->offsetY = m_graphicView->getOffsetY();
    state->width = m_graphicView->getWidth();
    state->height = m_graphicView->getHeight();
    // Screen y grows downwards, drawing y upwards: the bottom-left pixel is
    // the minimum corner.
    state->min = QPointF(m_graphicView->toGraphX(0),
                         m_graphicView->toGraphY(state->height));
    state->max = QPointF(m_graphicView->toGraphX(state->width),
                         m_graphicView->toGraphY(0));
    return true;
}

bool NativeBridge::zoomAuto(bool keepAspectRatio)
{
    if (!viewAvailable())
        return false;
    // axis=false: fit the entities, not the entities plus the origin.
    m_graphicView->zoomAuto(false, keepAspectRatio);
    m_graphicView->redraw();
    return true;
}

bool NativeBridge::zoomWindow(const QPointF &p1, const QPointF &p2,
                              bool keepAspectRatio)
{
    if (!viewAvailable())
        return false;
    m_graphicView->zoomWindow(toVector(p1), toVector(p2), keepAspectRatio);
    m_graphicView->redraw();
    return true;
}

bool NativeBridge::zoomIn(double factor, bool hasCenter, const QPointF &center,
                          bool out)
{
    if (!viewAvailable())
        return false;
    // RS_ActionZoomIn does the same when it has no mouse position: zoom
    // about the middle of the view.
    const RS_Vector c = hasCenter
        ? toVector(center)
        : toVector(QPointF(m_graphicView->toGraphX(m_graphicView->getWidth() / 2),
                           m_graphicView->toGraphY(m_graphicView->getHeight() / 2)));
    if (out)
        m_graphicView->zoomOut(factor, c);
    else
        m_graphicView->zoomIn(factor, c);
    m_graphicView->redraw();
    return true;
}

bool NativeBridge::zoomPan(int dx, int dy)
{
    if (!viewAvailable())
        return false;
    m_graphicView->zoomPan(dx, dy);   // redraws itself
    return true;
}

bool NativeBridge::zoomPrevious()
{
    if (!viewAvailable())
        return false;
    m_graphicView->zoomPrevious();
    m_graphicView->redraw();
    return true;
}

bool NativeBridge::zoomPage()
{
    if (!viewAvailable())
        return false;
    m_graphicView->zoomPage();   // redraws itself
    return true;
}

bool NativeBridge::setView(bool hasFactor, double factor, bool hasOffset,
                           int offsetX, int offsetY, bool hasCenter,
                           const QPointF &center)
{
    if (!viewAvailable())
        return false;
    if (hasFactor)
        m_graphicView->setFactor(factor);
    if (hasCenter) {
        // Solve toGuiX(cx) = w/2 and toGuiY(cy) = h/2 for the offsets
        // (toGuiX = x*fx + ox, toGuiY = -y*fy + h - oy).
        const RS_Vector f = m_graphicView->getFactor();
        offsetX = qRound(m_graphicView->getWidth() / 2.0 - center.x() * f.x);
        offsetY = qRound(m_graphicView->getHeight() / 2.0 - center.y() * f.y);
        hasOffset = true;
    }
    if (hasOffset)
        m_graphicView->setOffset(offsetX, offsetY);
    // setFactor/setOffset only store; the GUI view also keeps scrollbars.
    m_graphicView->adjustOffsetControls();
    m_graphicView->adjustZoomControls();
    m_graphicView->redraw();
    return true;
}

bool NativeBridge::hasUnsavedChanges() const
{
    if (!modificationAvailable())
        return false;
    // hasUndoable() is virtual: the session's open undo cycle holds changes
    // that RS_Document only flags as modified when the cycle closes.
    return m_document->isModified() || m_document->hasUndoable();
}

bool NativeBridge::exportImage(const QString &path, const QString &format,
                               const QSize &size, int border,
                               bool blackBackground, bool blackWhite,
                               bool transparent)
{
    if (!modificationAvailable())
        return false;
    QC_ApplicationWindow *app = QC_ApplicationWindow::getAppWindow().get();
    RS_Graphic *graphic = m_document->getGraphic();
    if (!app || !graphic) {
        m_lastError = QStringLiteral("no drawing to export");
        return false;
    }
    if (!transparent) {
        // File > Export's own worker, minus its dialogs. It fits the whole
        // drawing into the image (RS_StaticGraphicView::zoomAuto) and writes
        // raster formats with QImageWriter, SVG with QSvgGenerator. For SVG
        // it returns false even on success (its result flag is only set in
        // the raster branch), so success there is the file appearing.
        const bool svg = format.compare(QLatin1String("svg"), Qt::CaseInsensitive) == 0;
        if (svg)
            QFile::remove(path);
        const bool written = app->slotFileExport(path, format, size,
                                                 QSize(border, border),
                                                 blackBackground, blackWhite);
        if (svg ? QFileInfo(path).size() <= 0 : !written) {
            m_lastError = QStringLiteral("LibreCAD could not write \"%1\"").arg(path);
            return false;
        }
        return true;
    }
    // slotFileExport has no transparency option, so the transparent variant
    // is the same rendering into a cleared pixmap.
    QPixmap picture(size);
    picture.fill(Qt::transparent);
    renderFitted(&picture, graphic, size, QSize(border, border), false);
    QImageWriter writer(path, format.toLatin1());
    if (!writer.write(picture.toImage())) {
        m_lastError = QStringLiteral("could not write \"%1\": %2")
                          .arg(path, writer.errorString());
        return false;
    }
    return true;
}

bool NativeBridge::exportPdf(const QString &path, const QString &paper,
                             int landscape, bool fitToPage, int *pages,
                             QSizeF *paperMm)
{
    if (!modificationAvailable())
        return false;
    RS_Graphic *graphic = m_document->getGraphic();
    if (!graphic) {
        m_lastError = QStringLiteral("no drawing to export");
        return false;
    }

    // Paper size in millimetres, normalised to portrait first.
    RS_Vector sizeMm;
    bool isLandscape = false;
    if (paper.isEmpty()) {
        // The drawing's own paper ($PLIMMIN/$PLIMMAX), as printing uses it.
        sizeMm = RS_Units::convert(graphic->getPaperSize(), graphic->getUnit(),
                                   RS2::Millimeter);
        isLandscape = sizeMm.x > sizeMm.y;
    } else {
        const RS2::PaperFormat format = RS_Units::stringToPaperFormat(paper);
        if (format == RS2::Custom) {
            m_lastError = QStringLiteral("unknown paper \"%1\" (A0-A4, Letter, "
                                         "Legal, Tabloid, Ansi C-E, Arch A-E)")
                              .arg(paper);
            return false;
        }
        sizeMm = RS_Units::paperFormatToSize(format);
    }
    if (sizeMm.x > sizeMm.y)
        std::swap(sizeMm.x, sizeMm.y);
    if (landscape >= 0)
        isLandscape = landscape > 0;
    if (isLandscape)
        std::swap(sizeMm.x, sizeMm.y);
    if (sizeMm.x <= 0.0 || sizeMm.y <= 0.0) {
        m_lastError = QStringLiteral("the drawing has no valid paper size");
        return false;
    }

    // QC_ApplicationWindow::slotFilePrint(true), minus the file dialog and
    // the print-preview requirement: a full-page PDF at 1200 dpi, page size
    // and margins (millimetres) from the drawing.
    const QMarginsF margins(graphic->getMarginLeft(), graphic->getMarginTop(),
                            graphic->getMarginRight(), graphic->getMarginBottom());
    // Removed first so that the existence check at the end means something:
    // QPrinter reports nothing back when it cannot write.
    QFile::remove(path);
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setColorMode(QPrinter::Color);
    printer.setResolution(1200);
    printer.setFullPage(true);
    QPageLayout layout = printer.pageLayout();
    layout.setMode(QPageLayout::FullPageMode);
    layout.setUnits(QPageLayout::Millimeter);
    layout.setMinimumMargins({});
    layout.setOrientation(QPageLayout::Portrait);
    layout.setPageSize(QPageSize(QSizeF(sizeMm.x, sizeMm.y), QPageSize::Millimeter,
                                 QString(), QPageSize::ExactMatch),
                       margins);
    if (!printer.setPageLayout(layout)) {
        m_lastError = QStringLiteral("Qt refused the page layout");
        return false;
    }
    *paperMm = QSizeF(sizeMm.x, sizeMm.y);

    const double printerFx = double(printer.width()) / printer.widthMM();
    const double printerFy = double(printer.height()) / printer.heightMM();
    *pages = 0;

    if (fitToPage) {
        // One page, the drawing fitted inside the margins.
        const QSize borders(qRound(qMax(margins.left(), margins.right()) * printerFx),
                            qRound(qMax(margins.top(), margins.bottom()) * printerFy));
        renderFitted(&printer, graphic, QSize(printer.width(), printer.height()),
                     borders, true);
        *pages = 1;
    } else {
        auto *painter = new RS_PainterQt(&printer);
        if (!painter->isActive()) {
            delete opaque<RS_Painter>(painter);
            m_lastError = QStringLiteral("could not open \"%1\" for writing").arg(path);
            return false;
        }
        painter->setClipRect(
            qRound(margins.left() * printerFx), qRound(margins.top() * printerFy),
            qRound(printer.width() - (margins.left() + margins.right()) * printerFx),
            qRound(printer.height() - (margins.top() + margins.bottom()) * printerFy));
        auto *view = new RS_StaticGraphicView(printer.width(), printer.height(), painter);
        view->setPrinting(true);
        view->setBorders(0, 0, 0, 0);
        const double unitToMm = RS_Units::getFactorToMM(graphic->getUnit());
        const double f = (printerFx * unitToMm + printerFy * unitToMm) / 2.0;
        view->setFactor(f * graphic->getPaperScale());
        view->setContainer(graphic);
        const RS_Vector base = graphic->getPaperInsertionBase();
        const int numX = qMax(1, graphic->getPagesNumHoriz());
        const int numY = qMax(1, graphic->getPagesNumVert());
        const RS_Vector area = graphic->getPrintAreaSize(false);
        for (int pY = 0; pY < numY; ++pY) {
            for (int pX = 0; pX < numX; ++pX) {
                if (pX > 0 || pY > 0)
                    printer.newPage();
                view->setOffset(int((base.x - area.x * pX) * f),
                                int((base.y - area.y * pY) * f));
                // Selected entities first, then everything, as LibreCAD does.
                painter->setDrawSelectedOnly(true);
                view->drawEntity(painter, graphic);
                painter->setDrawSelectedOnly(false);
                view->drawEntity(painter, graphic);
                ++*pages;
            }
        }
        painter->end();
        delete opaque<QWidget>(view);
        delete opaque<RS_Painter>(painter);
    }

    if (!QFileInfo(path).isFile()) {
        m_lastError = QStringLiteral("could not write \"%1\"").arg(path);
        return false;
    }
    return true;
}

bool NativeBridge::move(const QPointF &offset, int copies,
                        bool useCurrentLayer, bool useCurrentAttributes)
{
    if (!modificationAvailable())
        return false;
    RS_MoveData data;
    data.number = qMax(0, copies);
    data.useCurrentLayer = useCurrentLayer;
    data.useCurrentAttributes = useCurrentAttributes;
    data.offset = toVector(offset);
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.move(data)) {
        m_lastError = QStringLiteral("RS_Modification::move refused");
        return false;
    }
    return true;
}

bool NativeBridge::rotate(const QPointF &center, double angle, int copies,
                          bool useCurrentLayer, bool useCurrentAttributes)
{
    if (!modificationAvailable())
        return false;
    // 2.2.1.5's RS_RotateData has a single angle; later versions add a
    // second one for rotating the copies about their own reference point.
    RS_RotateData data;
    data.number = qMax(0, copies);
    data.useCurrentLayer = useCurrentLayer;
    data.useCurrentAttributes = useCurrentAttributes;
    data.center = toVector(center);
    data.angle = angle;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.rotate(data)) {
        m_lastError = QStringLiteral("RS_Modification::rotate refused");
        return false;
    }
    return true;
}

bool NativeBridge::scale(const QPointF &center, const QPointF &factor,
                         int copies, bool useCurrentLayer,
                         bool useCurrentAttributes)
{
    if (!modificationAvailable())
        return false;
    // isotropicScaling and toFindFactor only steer the GUI action's point
    // prompts; RS_Modification::scale reads referencePoint and factor.
    RS_ScaleData data;
    data.referencePoint = toVector(center);
    data.factor = toVector(factor);
    data.number = qMax(0, copies);
    data.useCurrentLayer = useCurrentLayer;
    data.useCurrentAttributes = useCurrentAttributes;
    data.isotropicScaling = qFuzzyCompare(factor.x(), factor.y());
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.scale(data)) {
        m_lastError = QStringLiteral("RS_Modification::scale refused");
        return false;
    }
    return true;
}

bool NativeBridge::moveRotate(const QPointF &offset, const QPointF &center,
                              double angle, int copies, bool useCurrentLayer,
                              bool useCurrentAttributes)
{
    if (!modificationAvailable())
        return false;
    RS_MoveRotateData data;
    data.number = qMax(0, copies);
    data.useCurrentLayer = useCurrentLayer;
    data.useCurrentAttributes = useCurrentAttributes;
    data.referencePoint = toVector(center);
    data.offset = toVector(offset);
    data.angle = angle;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.moveRotate(data)) {
        m_lastError = QStringLiteral("RS_Modification::moveRotate refused");
        return false;
    }
    return true;
}

bool NativeBridge::rotate2(const QPointF &center1, const QPointF &center2,
                           double angle1, double angle2, int copies,
                           bool useCurrentLayer, bool useCurrentAttributes)
{
    if (!modificationAvailable())
        return false;
    RS_Rotate2Data data;
    data.number = qMax(0, copies);
    data.useCurrentLayer = useCurrentLayer;
    data.useCurrentAttributes = useCurrentAttributes;
    data.center1 = toVector(center1);
    data.center2 = toVector(center2);
    data.angle1 = angle1;
    data.angle2 = angle2;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.rotate2(data)) {
        m_lastError = QStringLiteral("RS_Modification::rotate2 refused");
        return false;
    }
    return true;
}

bool NativeBridge::stretch(const QPointF &firstCorner,
                           const QPointF &secondCorner, const QPointF &offset)
{
    if (!modificationAvailable())
        return false;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.stretch(toVector(firstCorner), toVector(secondCorner),
                              toVector(offset))) {
        m_lastError = QStringLiteral("RS_Modification::stretch refused");
        return false;
    }
    return true;
}

bool NativeBridge::round(Plug_Entity *entity1, const QPointF &point1,
                         Plug_Entity *entity2, const QPointF &point2,
                         const QPointF &corner, double radius, bool trim)
{
    if (!modificationAvailable())
        return false;
    RS_Entity *first = underlyingEntity(entity1);
    RS_Entity *second = underlyingEntity(entity2);
    if (!first || !second) {
        m_lastError = QStringLiteral("entity not found");
        return false;
    }
    if (first == second) {
        m_lastError = QStringLiteral("a fillet needs two different entities");
        return false;
    }
    if (!first->isAtomic() || !second->isAtomic()) {
        m_lastError = QStringLiteral("only atomic entities (line, arc, "
                                     "circle, ellipse) can be filleted");
        return false;
    }
    RS_RoundData data;
    data.radius = radius;
    data.trim = trim;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.round(toVector(corner),
                            toVector(point1), static_cast<RS_AtomicEntity *>(first),
                            toVector(point2), static_cast<RS_AtomicEntity *>(second),
                            data)) {
        m_lastError = QStringLiteral("RS_Modification::round refused (no "
                                     "fillet of that radius fits, or "
                                     "locked/hidden)");
        return false;
    }
    return true;
}

bool NativeBridge::bevel(Plug_Entity *entity1, const QPointF &point1,
                         Plug_Entity *entity2, const QPointF &point2,
                         double length1, double length2, bool trim)
{
    if (!modificationAvailable())
        return false;
    RS_Entity *first = underlyingEntity(entity1);
    RS_Entity *second = underlyingEntity(entity2);
    if (!first || !second) {
        m_lastError = QStringLiteral("entity not found");
        return false;
    }
    if (first == second) {
        m_lastError = QStringLiteral("a chamfer needs two different entities");
        return false;
    }
    if (!first->isAtomic() || !second->isAtomic()) {
        m_lastError = QStringLiteral("only atomic entities (line, arc, "
                                     "circle, ellipse) can be chamfered");
        return false;
    }
    RS_BevelData data;
    data.length1 = length1;
    data.length2 = length2;
    data.trim = trim;
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.bevel(toVector(point1), static_cast<RS_AtomicEntity *>(first),
                            toVector(point2), static_cast<RS_AtomicEntity *>(second),
                            data)) {
        m_lastError = QStringLiteral("RS_Modification::bevel refused (the "
                                     "entities do not intersect, or "
                                     "locked/hidden)");
        return false;
    }
    return true;
}

bool NativeBridge::cut(Plug_Entity *entity, const QPointF &point)
{
    if (!modificationAvailable())
        return false;
    RS_Entity *target = underlyingEntity(entity);
    if (!target) {
        m_lastError = QStringLiteral("entity not found");
        return false;
    }
    if (!target->isAtomic()) {
        m_lastError = QStringLiteral("only atomic entities (line, arc, "
                                     "circle, ellipse) can be cut");
        return false;
    }
    // The cut action only fires with a point on the entity (it snaps the
    // mouse there); RS_Modification::cut trusts that and would trim to an
    // off-entity point. Project, the way the snap would.
    const RS_Vector at = target->getNearestPointOnEntity(toVector(point), true);
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.cut(at, static_cast<RS_AtomicEntity *>(target))) {
        m_lastError = QStringLiteral("RS_Modification::cut refused (the point "
                                     "is an endpoint, or locked/hidden)");
        return false;
    }
    return true;
}

bool NativeBridge::isLineTypeName(const QString &name)
{
    RS2::LineType type;
    return lineTypeFromName(name, &type);
}

bool NativeBridge::isLineWidthName(const QString &name)
{
    RS2::LineWidth width;
    return lineWidthFromName(name, &width);
}

bool NativeBridge::changeAttributes(const AttributeChange &change)
{
    if (!modificationAvailable())
        return false;
    RS_AttributesData data;
    if (change.changeLayer) {
        data.changeLayer = true;
        data.layer = change.layer;
    }
    if (change.changeColor) {
        // fromIntColor is the plugin API's own int -> RS_Color conversion
        // (Plugin_Entity::updateData uses it), so the encodings agree.
        RS_Color color(0, 0, 0);
        color.fromIntColor(change.color);
        data.pen.setColor(color);
        data.changeColor = true;
    }
    if (change.changeLineType) {
        RS2::LineType type = RS2::LineByLayer;
        if (!lineTypeFromName(change.lineType, &type)) {
            m_lastError = QStringLiteral("unknown line type \"%1\"")
                              .arg(change.lineType);
            return false;
        }
        data.pen.setLineType(type);
        data.changeLineType = true;
    }
    if (change.changeWidth) {
        RS2::LineWidth width = RS2::WidthByLayer;
        if (!lineWidthFromName(change.width, &width)) {
            m_lastError = QStringLiteral("unknown line width \"%1\"")
                              .arg(change.width);
            return false;
        }
        data.pen.setWidth(width);
        data.changeWidth = true;
    }
    RS_Modification modification(*m_document, m_graphicView, true);
    if (!modification.changeAttributes(data)) {
        m_lastError = QStringLiteral("RS_Modification::changeAttributes refused");
        return false;
    }
    return true;
}

bool NativeBridge::revertDirection()
{
    if (!modificationAvailable())
        return false;
    RS_Modification modification(*m_document, m_graphicView, true);
    modification.revertDirection();   // void: it cannot refuse
    return true;
}

void NativeBridge::snapshotCycleStart()
{
    m_cycleStartKeys.clear();
    if (!modificationAvailable())
        return;
    // Top-level entities only, undone ones included: that is the level the
    // bridge's handles and RS_Modification's replacements live at.
    for (RS_Entity *entity : *m_document)
        m_cycleStartKeys.insert(entity);
}

void NativeBridge::splitUndoCycle()
{
    undoCheckpoint();
    ensureUndoCycle();
}

void NativeBridge::isolateReplacement(const QList<Plug_Entity *> &replaced)
{
    if (!modificationAvailable() || !m_undoCycleOpen)
        return;
    for (Plug_Entity *entity : replaced) {
        RS_Entity *rsEntity = underlyingEntity(entity);
        if (rsEntity && !m_cycleStartKeys.contains(rsEntity)) {
            splitUndoCycle();
            return;
        }
    }
}

void NativeBridge::isolateStretch(const QPointF &firstCorner,
                                  const QPointF &secondCorner)
{
    if (!modificationAvailable() || !m_undoCycleOpen)
        return;
    const RS_Vector v1 = toVector(firstCorner);
    const RS_Vector v2 = toVector(secondCorner);
    // The same test RS_Modification::stretch applies.
    for (RS_Entity *entity : *m_document) {
        if (!entity || m_cycleStartKeys.contains(entity) || !entity->isVisible()
            || entity->isLocked()) {
            continue;
        }
        if (entity->isInWindow(v1, v2)
            || entity->hasEndpointsWithinWindow(v1, v2)) {
            splitUndoCycle();
            return;
        }
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
