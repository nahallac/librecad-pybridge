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
#include "rs_graphic.h"             // paper size, scale, margins
#include "rs_graphicview.h"         // zoom, factor, offset
#include "rs_staticgraphicview.h"   // off-screen rendering for export
#include "rs_painterqt.h"           // the painter LibreCAD exports with
#include "rs_units.h"               // paper formats, unit conversion

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QImageWriter>
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
{
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

} // namespace lcbridge
