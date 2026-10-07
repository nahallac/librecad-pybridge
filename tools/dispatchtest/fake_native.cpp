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

// Prompts and push events: the stub document answers every prompt at once,
// so there is nothing to watch or cancel.
QWidget *NativeBridge::mainWindow() const { return nullptr; }
void NativeBridge::armPrompt(PromptKind, int, const QVariant &) {}
bool NativeBridge::disarmPrompt() { return false; }
void NativeBridge::cancelPrompt() {}
void NativeBridge::pollPrompt() {}
bool NativeBridge::census(int *, int *) const { return false; }
bool NativeBridge::viewState(double *, int *, int *) const { return false; }
bool NativeBridge::gridState(bool *) const { return false; }

} // namespace lcbridge
