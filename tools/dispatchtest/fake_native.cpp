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
bool NativeBridge::execCommand(const QString &) { return false; }
bool NativeBridge::setSelected(Plug_Entity *, bool) { return false; }
void NativeBridge::armHatchDialog(const QString &, double, double, bool, int) {}
void NativeBridge::disarmHatchDialog() {}
void NativeBridge::pollForHatchDialog() {}

} // namespace lcbridge
