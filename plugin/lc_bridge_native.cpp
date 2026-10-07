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
#include "rs_graphicview.h"         // RS_GraphicView::redraw (virtual)
#include "rs_line.h"
#include "rs_mtext.h"
#include "rs_image.h"
#include "rs_hatch.h"
#include "rs_leader.h"
#include "rs_dimaligned.h"
#include "rs_dimlinear.h"
#include "rs_dimradial.h"
#include "rs_dimdiametric.h"
#include "rs_dimangular.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QCoreApplication>
#include <QImageReader>
#include <QLineEdit>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

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

// --------------------------------------------------------------------------
// Creation: entities the plugin API cannot make. Each one is built the way
// the LibreCAD action that draws it builds it in trigger() (the action
// sources are named per function), minus the mouse: the points an action
// would collect from clicks are computed here from the arguments.
// --------------------------------------------------------------------------

namespace {

// RS_Math::correctAngle, restated so it binds nothing: into [0, 2pi).
double correctAngle(double a)
{
    return std::fmod(M_PI + std::remainder(a - M_PI, 2.0 * M_PI), 2.0 * M_PI);
}

double angleOf(const QPointF &v)
{
    return correctAngle(std::atan2(v.y(), v.x()));
}

double distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

QPointF polar(double length, double angle)
{
    return QPointF(length * std::cos(angle), length * std::sin(angle));
}

double cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

//! The point on the line through \a origin with direction \a direction
//! (any length) nearest to \a point.
QPointF projectOntoLine(const QPointF &origin, const QPointF &direction,
                        const QPointF &point)
{
    const double length2 = QPointF::dotProduct(direction, direction);
    const double t = QPointF::dotProduct(point - origin, direction) / length2;
    return origin + direction * t;
}

QVariantList pointVariant(const RS_Vector &v)
{
    return QVariantList{v.x, v.y};
}

//! The data every dimension action starts from (RS_ActionDimension::reset):
//! no text position (updateDim() computes it), middle/centre alignment,
//! exact spacing 1.0, style "Standard". \a text "" means the measured value.
RS_DimensionData dimensionData(const RS_Vector &definitionPoint,
                               const QString &text)
{
    return RS_DimensionData(definitionPoint, RS_Vector(),
                            RS_MTextData::VAMiddle, RS_MTextData::HACenter,
                            RS_MTextData::Exact, 1.0, text,
                            QStringLiteral("Standard"), 0.0);
}

//! Clones of the atomic entities in \a entity, for a hatch loop: what the
//! hatch action's ResolveAll walk over the selection collects.
void collectBoundary(RS_Entity *entity, RS_EntityContainer *loop)
{
    if (entity->isContainer()) {
        auto *container = static_cast<RS_EntityContainer *>(entity);
        for (RS_Entity *child : *container)
            collectBoundary(child, loop);
        return;
    }
    RS_Entity *copy = entity->clone();
    copy->setPen(RS_Pen(RS2::FlagInvalid));
    copy->reparent(loop);
    loop->addEntity(copy);
}

} // namespace

void NativeBridge::commitNewEntity(RS_Entity *entity, bool update)
{
    // rs_actiondimradial.cpp (and its siblings) trigger(): the entity takes
    // the active layer and pen, builds its sub-entities, joins the document,
    // and is registered in an undo cycle of its own -- which nests in the
    // session's open cycle (see ensureUndoCycle), as an action's does.
    entity->setLayerToActive();
    entity->setPenToActive();
    if (update)
        entity->update();
    m_document->addEntity(entity);
    m_document->startUndoCycle();
    m_document->addUndoable(entity);   // virtual (RS_Undo)
    m_document->endUndoCycle();
    if (m_graphicView)
        m_graphicView->redraw(RS2::RedrawDrawing);
}

bool NativeBridge::addMText(const QString &text, const QString &style,
                            const QPointF &at, double height, double width,
                            double angle, int halign, int valign,
                            double lineSpacing)
{
    if (!modificationAvailable())
        return false;
    // rs_actiondrawmtext.cpp: left-to-right, exact spacing. DPI::HAlign and
    // DPI::VAlign list their values in RS_MTextData's order, which is what
    // Doc_plugin_interface::addMText relies on too.
    const RS_MTextData data(toVector(at), height, width,
                            static_cast<RS_MTextData::VAlign>(valign),
                            static_cast<RS_MTextData::HAlign>(halign),
                            RS_MTextData::LeftToRight, RS_MTextData::Exact,
                            lineSpacing, text, style, angle, RS2::Update);
    commitNewEntity(new RS_MText(m_document, data), true);
    return true;
}

bool NativeBridge::imagePixelSize(const QString &path, QSize *size) const
{
    QImageReader reader(path);
    const QSize pixels = reader.size();
    if (!pixels.isValid() || pixels.isEmpty())
        return false;
    *size = pixels;
    return true;
}

bool NativeBridge::addImage(const QString &path, const QPointF &at,
                            double scale, double angle, int brightness,
                            int contrast, int fade)
{
    if (!modificationAvailable())
        return false;
    // rs_actiondrawimage.cpp: uVector and vVector are one pixel's width and
    // height in drawing units (the action's "factor"), rotated by the
    // angle; size is the pixel count, which RS_Image::update() fills in
    // when it loads the file. RS_Creation::createImage(), which the action
    // calls, then commits it the way commitNewEntity() does.
    const RS_ImageData data(0, toVector(at), toVector(polar(scale, angle)),
                            toVector(polar(scale, angle + M_PI_2)),
                            toVector(QPointF(1.0, 1.0)), path,
                            brightness, contrast, fade);
    auto *image = new RS_Image(m_document, data);
    image->update();
    const RS_Vector size = image->getData().size;
    if (!size.valid || size.x < 1.0 || size.y < 1.0) {
        delete image;
        m_lastError = QStringLiteral("LibreCAD could not load \"%1\"").arg(path);
        return false;
    }
    // RS_Image::update() rewrites the file name relative to the drawing's
    // folder (imageRelativePathName in rs_image.cpp), which keeps a saved
    // drawing and its images movable together. An unnamed drawing has no
    // folder, and the name comes out relative to LibreCAD's working
    // directory, which is meaningless once the drawing is saved anywhere
    // else; keep the absolute path then.
    if (m_document->getFilename().isEmpty())
        image->setFile(path);
    commitNewEntity(image, false);
    return true;
}

bool NativeBridge::dimAligned(const QPointF &p1, const QPointF &p2,
                              const QPointF &dimLine, const QString &text)
{
    if (!modificationAvailable())
        return false;
    // rs_actiondimaligned.cpp preparePreview(): the definition point is the
    // clicked point moved onto the perpendicular through the second
    // extension point.
    const QPointF along = p2 - p1;
    const QPointF definition =
        projectOntoLine(p2, QPointF(-along.y(), along.x()), dimLine);
    const RS_DimAlignedData edata(toVector(p1), toVector(p2));
    commitNewEntity(new RS_DimAligned(m_document,
                                      dimensionData(toVector(definition), text),
                                      edata),
                    true);
    return true;
}

bool NativeBridge::dimLinear(const QPointF &p1, const QPointF &p2,
                             const QPointF &dimLine, double angle,
                             const QString &text)
{
    if (!modificationAvailable())
        return false;
    // rs_actiondimlinear.cpp preparePreview(): as aligned, but the
    // perpendicular is to the fixed measuring direction.
    const QPointF definition =
        projectOntoLine(p2, polar(1.0, angle + M_PI_2), dimLine);
    const RS_DimLinearData edata(toVector(p1), toVector(p2), angle, 0.0);
    commitNewEntity(new RS_DimLinear(m_document,
                                     dimensionData(toVector(definition), text),
                                     edata),
                    true);
    return true;
}

bool NativeBridge::dimRadial(const QPointF &center, double radius, double angle,
                             const QString &text, bool diametric)
{
    if (!modificationAvailable())
        return false;
    // rs_actiondimradial.cpp / rs_actiondimdiametric.cpp preparePreview():
    // radial keeps the centre as the definition point and the point on the
    // circle in the extra data; diametric uses the two opposite points.
    // The leader length stays 0, as both actions leave it.
    const QPointF onCircle = center + polar(radius, angle);
    if (diametric) {
        const QPointF opposite = center + polar(radius, angle + M_PI);
        const RS_DimDiametricData edata(toVector(onCircle), 0.0);
        commitNewEntity(new RS_DimDiametric(m_document,
                                            dimensionData(toVector(opposite), text),
                                            edata),
                        true);
    } else {
        const RS_DimRadialData edata(toVector(onCircle), 0.0);
        commitNewEntity(new RS_DimRadial(m_document,
                                         dimensionData(toVector(center), text),
                                         edata),
                        true);
    }
    return true;
}

bool NativeBridge::dimAngular(const QPointF &l1a, const QPointF &l1b,
                              const QPointF &l2a, const QPointF &l2b,
                              const QPointF &dimLine, const QString &text)
{
    if (!modificationAvailable())
        return false;

    // What RS_DimAngular expects (rs_dimangular.cpp, calcDimension() and
    // updateDim()): its arc runs counterclockwise from the direction
    // definitionPoint1 -> definitionPoint2 to the direction definitionPoint3
    // -> definitionPoint (the dimension data's), around the intersection of
    // the two lines, at the radius of definitionPoint4. The action derives
    // those from its picks through a quadrant table (setData() in
    // rs_actiondimangular.cpp); a verbatim port of it measured 300 degrees
    // instead of 60 when the arc point lay in the sector opposite two lines
    // that meet at a corner, so they are derived from the geometry instead.
    struct Segment { QPointF start, end; };
    const Segment lines[2] = {{l1a, l1b}, {l2a, l2b}};
    const QPointF d1 = l1b - l1a;
    const QPointF d2 = l2b - l2a;
    const double length1 = std::hypot(d1.x(), d1.y());
    const double length2 = std::hypot(d2.x(), d2.y());
    if (length1 < 1e-12 || length2 < 1e-12) {
        m_lastError = QStringLiteral("a line has zero length");
        return false;
    }
    const double denominator = cross(d1, d2);
    if (std::abs(denominator) < 1e-12 * length1 * length2) {
        m_lastError = QStringLiteral("the lines are parallel; there is no angle");
        return false;
    }
    const QPointF center = l1a + d1 * (cross(l2a - l1a, d2) / denominator);
    if (distance(center, dimLine) < 1e-9 * (length1 + length2)) {
        m_lastError = QStringLiteral("\"dimline\" must not be the intersection "
                                     "of the lines");
        return false;
    }

    // The four rays from the centre along the lines, in counterclockwise
    // order; the two that bound the sector holding dimLine belong to
    // different lines, since the rays of the two lines alternate.
    struct Ray { double angle; int line; };
    Ray rays[4] = {{angleOf(d1), 0}, {correctAngle(angleOf(d1) + M_PI), 0},
                   {angleOf(d2), 1}, {correctAngle(angleOf(d2) + M_PI), 1}};
    std::sort(std::begin(rays), std::end(rays),
              [](const Ray &a, const Ray &b) { return a.angle < b.angle; });
    const double dimAngle = angleOf(dimLine - center);
    int first = 0;
    for (; first < 4; ++first) {
        const Ray &from = rays[first];
        const Ray &to = rays[(first + 1) % 4];
        if (correctAngle(dimAngle - from.angle)
            <= correctAngle(to.angle - from.angle))
            break;
    }
    const Ray &rayA = rays[first % 4];
    const Ray &rayB = rays[(first + 1) % 4];

    // Each line's end points, ordered along its bounding ray.
    const auto ordered = [](const Segment &line, double angle,
                            QPointF *from, QPointF *to) {
        const bool forward =
            QPointF::dotProduct(line.end - line.start, polar(1.0, angle)) > 0.0;
        *from = forward ? line.start : line.end;
        *to = forward ? line.end : line.start;
    };
    QPointF dp1, dp2, dp3, definition;
    ordered(lines[rayA.line], rayA.angle, &dp1, &dp2);
    ordered(lines[rayB.line], rayB.angle, &dp3, &definition);

    const RS_DimAngularData edata(toVector(dp1), toVector(dp2), toVector(dp3),
                                  toVector(dimLine));
    commitNewEntity(new RS_DimAngular(m_document,
                                      dimensionData(toVector(definition), text),
                                      edata),
                    true);
    return true;
}

bool NativeBridge::dimLeader(const QList<QPointF> &points, bool arrowHead)
{
    if (!modificationAvailable())
        return false;
    if (points.size() < 2) {
        m_lastError = QStringLiteral("a leader needs at least two points");
        return false;
    }
    // rs_actiondimleader.cpp trigger(): vertices are added one by one;
    // addVertex() itself draws the arrow head once the first segment
    // exists. The action does not call update() afterwards, so neither
    // does this.
    auto *leader = new RS_Leader(m_document, RS_LeaderData(arrowHead));
    for (const QPointF &point : points)
        leader->addVertex(toVector(point));
    commitNewEntity(leader, false);
    return true;
}

bool NativeBridge::addHatch(const QList<Plug_Entity *> &boundary,
                            const QString &pattern, double scale, double angle,
                            bool solid)
{
    if (!modificationAvailable())
        return false;
    // rs_actiondrawhatch.cpp trigger(): one loop container inside the hatch
    // holds copies of every boundary entity (resolved to atomic entities,
    // pen invalid so they follow the hatch); the originals stay where they
    // are. Unlike the action this runs update() before committing, so that
    // a pattern that is missing or too dense for the area is reported
    // instead of leaving an empty hatch behind.
    auto *hatch = new RS_Hatch(m_document,
                               RS_HatchData(solid, scale, angle, pattern));
    auto *loop = new RS_EntityContainer(hatch);
    loop->setPen(RS_Pen(RS2::FlagInvalid));
    for (Plug_Entity *wrapper : boundary) {
        if (RS_Entity *entity = underlyingEntity(wrapper))
            collectBoundary(entity, loop);
    }
    hatch->addEntity(loop);

    if (!hatch->validate()) {
        delete hatch;
        m_lastError = QStringLiteral("the boundary does not form closed contours");
        return false;
    }
    hatch->setLayerToActive();
    hatch->setPenToActive();
    hatch->update();
    const int error = hatch->getUpdateError();
    if (error != RS_Hatch::HATCH_OK) {
        delete hatch;
        // An unknown pattern name does not come back as PATTERN_NOT_FOUND:
        // the pattern list hands out an empty pattern, which fills nothing
        // and reports TOO_SMALL.
        switch (error) {
        case RS_Hatch::HATCH_PATTERN_NOT_FOUND:
            m_lastError = QStringLiteral("no hatch pattern named \"%1\"").arg(pattern);
            break;
        case RS_Hatch::HATCH_TOO_SMALL:
            m_lastError = QStringLiteral("pattern \"%1\" at scale %2 puts nothing "
                                         "inside the contour (unknown pattern "
                                         "name, or scale too large)")
                              .arg(pattern).arg(scale);
            break;
        case RS_Hatch::HATCH_AREA_TOO_BIG:
            m_lastError = QStringLiteral("the area is too big for the pattern "
                                         "(pattern scale too small?)");
            break;
        default:
            m_lastError = QStringLiteral("LibreCAD refused the contour");
            break;
        }
        return false;
    }
    commitNewEntity(hatch, false);
    return true;
}

bool NativeBridge::entityDetails(Plug_Entity *entity, QVariantMap *details) const
{
    if (!m_versionOk)
        return false;
    RS_Entity *rsEntity = underlyingEntity(entity);
    if (!rsEntity)
        return false;

    const RS2::EntityType type = rsEntity->rtti();   // virtual
    switch (type) {
    case RS2::EntityDimAligned:
    case RS2::EntityDimLinear:
    case RS2::EntityDimRadial:
    case RS2::EntityDimDiametric:
    case RS2::EntityDimAngular: {
        auto *dimension = static_cast<RS_Dimension *>(rsEntity);
        const RS_DimensionData data = dimension->getData();
        details->insert(QStringLiteral("definition_point"),
                        pointVariant(data.definitionPoint));
        if (data.middleOfText.valid) {
            details->insert(QStringLiteral("text_point"),
                            pointVariant(data.middleOfText));
        }
        details->insert(QStringLiteral("text"), data.text);
        // The label as drawn: the measurement, or the text with "<>"
        // replaced by it. Bound symbol.
        details->insert(QStringLiteral("label"), dimension->getLabel(true));
        details->insert(QStringLiteral("style"), data.style);
        break;
    }
    default:
        break;
    }

    switch (type) {
    case RS2::EntityDimAligned: {
        const RS_DimAlignedData &edata =
            static_cast<RS_DimAligned *>(rsEntity)->getEData();   // bound
        details->insert(QStringLiteral("extension_point1"),
                        pointVariant(edata.extensionPoint1));
        details->insert(QStringLiteral("extension_point2"),
                        pointVariant(edata.extensionPoint2));
        return true;
    }
    case RS2::EntityDimLinear: {
        const RS_DimLinearData edata =
            static_cast<RS_DimLinear *>(rsEntity)->getEData();
        details->insert(QStringLiteral("extension_point1"),
                        pointVariant(edata.extensionPoint1));
        details->insert(QStringLiteral("extension_point2"),
                        pointVariant(edata.extensionPoint2));
        details->insert(QStringLiteral("angle"), edata.angle);
        details->insert(QStringLiteral("oblique"), edata.oblique);
        return true;
    }
    case RS2::EntityDimRadial: {
        const RS_DimRadialData edata =
            static_cast<RS_DimRadial *>(rsEntity)->getEData();
        details->insert(QStringLiteral("definition_point2"),
                        pointVariant(edata.definitionPoint));
        return true;
    }
    case RS2::EntityDimDiametric: {
        const RS_DimDiametricData edata =
            static_cast<RS_DimDiametric *>(rsEntity)->getEData();
        details->insert(QStringLiteral("definition_point2"),
                        pointVariant(edata.definitionPoint));
        return true;
    }
    case RS2::EntityDimAngular: {
        auto *angular = static_cast<RS_DimAngular *>(rsEntity);
        const RS_DimAngularData edata = angular->getEData();
        details->insert(QStringLiteral("definition_point1"),
                        pointVariant(edata.definitionPoint1));
        details->insert(QStringLiteral("definition_point2"),
                        pointVariant(edata.definitionPoint2));
        details->insert(QStringLiteral("definition_point3"),
                        pointVariant(edata.definitionPoint3));
        details->insert(QStringLiteral("definition_point4"),
                        pointVariant(edata.definitionPoint4));
        details->insert(QStringLiteral("center"),
                        pointVariant(angular->getCenter()));   // bound
        return true;
    }
    case RS2::EntityDimLeader: {
        auto *leader = static_cast<RS_Leader *>(rsEntity);
        QVariantList vertices;
        for (RS_Entity *child : *leader) {
            if (child->rtti() != RS2::EntityLine)
                continue;   // the arrow head is an RS_Solid
            auto *line = static_cast<RS_Line *>(child);
            if (vertices.isEmpty())
                vertices.append(QVariant(pointVariant(line->getStartpoint())));
            vertices.append(QVariant(pointVariant(line->getEndpoint())));
        }
        details->insert(QStringLiteral("vertices"), vertices);
        details->insert(QStringLiteral("arrow"), leader->hasArrowHead());
        return true;
    }
    case RS2::EntityMText: {
        const RS_MTextData data = static_cast<RS_MText *>(rsEntity)->getData();
        static const char *const hNames[] = {"left", "center", "right"};
        static const char *const vNames[] = {"top", "middle", "bottom"};
        details->insert(QStringLiteral("width"), data.width);
        details->insert(QStringLiteral("line_spacing"), data.lineSpacingFactor);
        details->insert(QStringLiteral("style"), data.style);
        if (data.halign >= 0 && data.halign <= 2) {
            details->insert(QStringLiteral("halign"),
                            QString::fromLatin1(hNames[data.halign]));
        }
        if (data.valign >= 0 && data.valign <= 2) {
            details->insert(QStringLiteral("valign"),
                            QString::fromLatin1(vNames[data.valign]));
        }
        return true;
    }
    case RS2::EntityHatch: {
        const RS_HatchData data = static_cast<RS_Hatch *>(rsEntity)->getData();
        details->insert(QStringLiteral("pattern"), data.pattern);
        details->insert(QStringLiteral("scale"), data.scale);
        details->insert(QStringLiteral("angle"), data.angle);
        details->insert(QStringLiteral("solid"), data.solid);
        return true;
    }
    default:
        return !details->isEmpty();
    }
}

} // namespace lcbridge
