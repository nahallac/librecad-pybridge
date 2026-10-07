// Stand-in for lc_bridge_native.cpp in the offline dispatch test.
//
// The real native layer is compiled against LibreCAD's source tree and binds
// to its class layouts, which has no meaning against the stub document. This
// implementation reports everything as unavailable, which is exactly how the
// real layer behaves on a LibreCAD it was not built for, and what the
// self-test asserts for the native operations.
#include "lc_bridge_native.h"

namespace lcbridge {

QString NativeBridge::builtAgainst()
{
    return QStringLiteral("(dispatchtest stub)");
}

QString NativeBridge::running()
{
    return QStringLiteral("(dispatchtest stub)");
}

NativeBridge::NativeBridge(QWidget *mainWindow, QObject *parent)
    : QObject(parent)
{
    Q_UNUSED(mainWindow)
    m_reason = QStringLiteral("native layer not built into dispatchtest");
}

NativeBridge::~NativeBridge() = default;

bool NativeBridge::commandsAvailable() const { return false; }
bool NativeBridge::selectionAvailable() const { return false; }
bool NativeBridge::modificationAvailable() const { return false; }
const void *NativeBridge::entityKey(Plug_Entity *) const { return nullptr; }
bool NativeBridge::isUndone(Plug_Entity *, bool *) const { return false; }
bool NativeBridge::offset(const QPointF &, double, int, bool, bool, bool) { return false; }
bool NativeBridge::mirror(const QPointF &, const QPointF &, bool) { return false; }
bool NativeBridge::explode(bool) { return false; }
bool NativeBridge::fileInfo(QString *, bool *) const { return false; }
bool NativeBridge::saveAs(const QString &, const QString &) { return false; }
bool NativeBridge::undoCheckpoint() { return false; }
bool NativeBridge::undo(int, bool, int *) { return false; }
void NativeBridge::ensureUndoCycle() {}
bool hasActiveDocument() { return false; }
bool performSessionRestart(const SessionRestart &, QString *error)
{
    *error = QStringLiteral("native layer not built into dispatchtest");
    return false;
}
bool NativeBridge::trim(Plug_Entity *, const QPointF &, Plug_Entity *, const QPointF &, bool) { return false; }
bool NativeBridge::execCommand(const QString &) { return false; }
bool NativeBridge::setSelected(Plug_Entity *, bool) { return false; }
bool NativeBridge::isSelected(Plug_Entity *, bool *) const { return false; }
bool NativeBridge::boundingBox(Plug_Entity *, QPointF *, QPointF *) const { return false; }
void NativeBridge::armHatchDialog(const QString &, double, double, bool, int) {}
void NativeBridge::disarmHatchDialog() {}
void NativeBridge::pollForHatchDialog() {}

// Layer state, block definition, geometry queries.
bool NativeBridge::layerState(const QString &, LayerState *) const { return false; }
NativeBridge::Result NativeBridge::setLayerState(const QString &, const LayerStatePatch &) { return Result::Failed; }
NativeBridge::Result NativeBridge::renameLayer(const QString &, const QString &) { return Result::Failed; }
bool NativeBridge::blockNames(QStringList *) const { return false; }
NativeBridge::Result NativeBridge::defineBlock(const QString &, const QPointF &, bool, int *) { return Result::Failed; }
NativeBridge::Result NativeBridge::renameBlock(const QString &, const QString &) { return Result::Failed; }
NativeBridge::Result NativeBridge::removeBlock(const QString &) { return Result::Failed; }
int NativeBridge::blockInsertCount(const QString &) const { return -1; }
NativeBridge::Result NativeBridge::blockEntities(const QString &, Document_Interface *, QList<Plug_Entity *> *) { return Result::Failed; }
bool NativeBridge::entityLength(Plug_Entity *, double *) const { return false; }
bool NativeBridge::entityArea(Plug_Entity *, double *, bool *) const { return false; }
bool NativeBridge::intersections(Plug_Entity *, Plug_Entity *, bool, QList<QPointF> *) const { return false; }
bool NativeBridge::entityDistance(Plug_Entity *, const QPointF &, double *) const { return false; }
bool NativeBridge::nearestPoint(Plug_Entity *, const QPointF &, bool, QPointF *, double *) const { return false; }
NativeBridge::Result NativeBridge::pointInside(Plug_Entity *, const QPointF &, bool *, bool *) { return Result::Failed; }
// View control, document windows, export: unavailable like the rest.
bool documentWindows(QList<DocumentWindow> *windows) { windows->clear(); return false; }
bool NativeBridge::viewAvailable() const { return false; }
bool NativeBridge::viewState(ViewState *) const { return false; }
bool NativeBridge::zoomAuto(bool) { return false; }
bool NativeBridge::zoomWindow(const QPointF &, const QPointF &, bool) { return false; }
bool NativeBridge::zoomIn(double, bool, const QPointF &, bool) { return false; }
bool NativeBridge::zoomPan(int, int) { return false; }
bool NativeBridge::zoomPrevious() { return false; }
bool NativeBridge::zoomPage() { return false; }
bool NativeBridge::setView(bool, double, bool, int, int, bool, const QPointF &) { return false; }
bool NativeBridge::hasUnsavedChanges() const { return false; }
bool NativeBridge::exportImage(const QString &, const QString &, const QSize &, int, bool, bool, bool) { return false; }
bool NativeBridge::exportPdf(const QString &, const QString &, int, bool, int *, QSizeF *) { return false; }
// Modify tools: the rest of RS_Modification.
bool NativeBridge::move(const QPointF &, int, bool, bool) { return false; }
bool NativeBridge::rotate(const QPointF &, double, int, bool, bool) { return false; }
bool NativeBridge::scale(const QPointF &, const QPointF &, int, bool, bool) { return false; }
bool NativeBridge::moveRotate(const QPointF &, const QPointF &, double, int, bool, bool) { return false; }
bool NativeBridge::rotate2(const QPointF &, const QPointF &, double, double, int, bool, bool) { return false; }
bool NativeBridge::stretch(const QPointF &, const QPointF &, const QPointF &) { return false; }
bool NativeBridge::round(Plug_Entity *, const QPointF &, Plug_Entity *, const QPointF &, const QPointF &, double, bool) { return false; }
bool NativeBridge::bevel(Plug_Entity *, const QPointF &, Plug_Entity *, const QPointF &, double, double, bool) { return false; }
bool NativeBridge::cut(Plug_Entity *, const QPointF &) { return false; }
bool NativeBridge::isLineTypeName(const QString &) { return true; }
bool NativeBridge::isLineWidthName(const QString &) { return true; }
bool NativeBridge::changeAttributes(const AttributeChange &) { return false; }
bool NativeBridge::revertDirection() { return false; }
void NativeBridge::isolateReplacement(const QList<Plug_Entity *> &) {}
void NativeBridge::isolateStretch(const QPointF &, const QPointF &) {}

} // namespace lcbridge
