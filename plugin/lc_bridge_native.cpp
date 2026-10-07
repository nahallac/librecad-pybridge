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
#include "rs_graphic.h"
#include "rs_insert.h"
#include "rs_information.h"         // intersections, point-in-contour
#include "rs_circle.h"
#include "rs_ellipse.h"
#include "rs_polyline.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QCoreApplication>
#include <QLineEdit>
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

} // namespace lcbridge
