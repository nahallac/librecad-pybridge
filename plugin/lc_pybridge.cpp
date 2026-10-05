/*****************************************************************************/
/*  lc_pybridge.cpp - LibreCAD scripting bridge plugin                       */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_pybridge.h"

#include "lc_bridge_server.h"

#include "document_interface.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QString>

#include <functional>
#include <utility>

namespace {

const char *const kPluginTitle = "Python Bridge";

} // namespace

QString LC_PyBridge::name() const
{
    return tr(kPluginTitle);
}

PluginCapabilities LC_PyBridge::getCapabilities() const
{
    PluginCapabilities capabilities;
    capabilities.menuEntryPoints
        << PluginMenuLocation(QStringLiteral("plugins_menu"),
                              tr("Start Python bridge"));
    return capabilities;
}

void LC_PyBridge::execComm(Document_Interface *doc, QWidget *parent, QString cmd)
{
    Q_UNUSED(cmd)
    if (!doc) {
        QMessageBox::warning(parent, tr(kPluginTitle),
                             tr("No document interface; open a drawing first."));
        return;
    }
    runBridgeSession(doc, parent);
}

void LC_PyBridge::runBridgeSession(Document_Interface *doc, QWidget *parent)
{
    // Server and dispatcher live on the stack: the Document_Interface they
    // borrow dies when execComm() returns, and serve() keeps execComm() on the
    // stack for the whole session. Everything a session draws is one undo
    // step, because execPlug() wraps this call in an LC_UndoSection.
    lcbridge::BridgeServer server(doc);

    if (!server.listen(lcbridge::BridgeServer::defaultSocketName())) {
        QMessageBox::warning(parent, tr(kPluginTitle),
                             tr("Could not open the bridge socket:\n%1")
                                 .arg(server.errorString()));
        return;
    }

    // Status display. A separate window is wrong here twice over: it steals
    // focus from the drawing, and on Wayland Qt cannot position a top-level
    // window, so the compositor drops it in the middle of the screen. So the
    // status is a small child widget overlaid inside LibreCAD's own window,
    // pinned to the bottom-right corner above the status bar, where it blocks
    // nothing and never takes focus.
    QWidget overlay(parent);
    overlay.setObjectName(QStringLiteral("lc_pybridge_overlay"));
    overlay.setAutoFillBackground(true);

    auto *layout = new QHBoxLayout(&overlay);
    layout->setContentsMargins(10, 6, 10, 6);

    auto *status = new QLabel(tr("Bridge listening on %1")
                                  .arg(server.fullServerName()),
                              &overlay);
    layout->addWidget(status);

    auto *stopButton = new QPushButton(tr("Stop"), &overlay);
    stopButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(stopButton);

    connect(stopButton, &QPushButton::clicked, &server,
            &lcbridge::BridgeServer::stop);
    connect(&server, &lcbridge::BridgeServer::clientChanged, status,
            [status](bool connected) {
                status->setText(connected ? tr("Bridge: client connected")
                                          : tr("Bridge: listening"));
            });
    connect(&server, &lcbridge::BridgeServer::requestHandled, status,
            [status](int total) {
                status->setText(tr("Bridge: %1 requests").arg(total));
            });

    // Keep the overlay pinned to the parent's bottom-right corner, tracking
    // both its own size (the label text changes) and parent resizes.
    const auto reposition = [parentWidget = parent, &overlay]() {
        overlay.adjustSize();
        const int margin = 12;
        overlay.move(parentWidget->width() - overlay.width() - margin,
                     parentWidget->height() - overlay.height() - margin);
        overlay.raise();
    };

    class RepinFilter : public QObject
    {
    public:
        explicit RepinFilter(std::function<void()> repin)
            : m_repin(std::move(repin)) {}
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            if (event->type() == QEvent::Resize)
                m_repin();
            return QObject::eventFilter(watched, event);
        }
    private:
        std::function<void()> m_repin;
    } repinFilter{reposition};

    parent->installEventFilter(&repinFilter);
    connect(&server, &lcbridge::BridgeServer::requestHandled, &overlay,
            [reposition](int) { reposition(); });
    connect(&server, &lcbridge::BridgeServer::clientChanged, &overlay,
            [reposition](bool) { reposition(); });

    reposition();
    overlay.show();
    overlay.raise();

    const int handled = server.serve();

    parent->removeEventFilter(&repinFilter);
    overlay.hide();
    doc->updateView();
    QMessageBox::information(parent, tr(kPluginTitle),
                             tr("Bridge session ended after %1 requests.")
                                 .arg(handled));
}
