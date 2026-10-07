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

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QCoreApplication>
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

} // namespace lcbridge
