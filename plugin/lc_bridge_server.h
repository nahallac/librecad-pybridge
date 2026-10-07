/*****************************************************************************/
/*  lc_bridge_server.h - local-socket transport for the dispatch layer       */
/*                                                                           */
/*  Serves newline-delimited JSON requests over a QLocalServer, dispatching   */
/*  each one through lcbridge::Dispatcher. The server runs a nested event     */
/*  loop and returns when asked to stop, because the Document_Interface it    */
/*  serves is only valid while execComm() is on the stack (docs/findings.md,  */
/*  risk 7). The whole session lands in one undo step (risk 1) by design.     */
/*                                                                           */
/*  UI-free on purpose: the plugin wraps this in a small dialog, and          */
/*  tools/dispatchtest serves a stub document headless with the same class.   */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef LC_BRIDGE_SERVER_H
#define LC_BRIDGE_SERVER_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>

#include "lc_bridge_native.h"
#include <QObject>
#include <QString>

class Document_Interface;
class QEventLoop;
class QLocalServer;
class QLocalSocket;

namespace lcbridge {

class Dispatcher;
class EventMonitor;
class NativeBridge;

/**
 * Line protocol: one JSON object per line in each direction, UTF-8, '\n'
 * terminated. Requests and responses are exactly what Dispatcher::dispatch()
 * takes and returns, plus one server-level operation:
 *
 *   {"op": "session"}   ->  {"ok": true, "result": {"id": <unique per
 *                           session>, "requests": n}}. Clients use the id to
 *                           tell a new session from the one they left.
 *   {"op": "shutdown"}  ->  {"ok": true, "result": null}, then the server
 *                           stops and serve() returns.
 *   {"op": "file_open", "args": {"path": ...}} and {"op": "file_new"}
 *                       ->  {"ok": true, "result": {"path": ...}}, then the
 *                           server stops with pendingRestart() set, for the
 *                           plugin to open the drawing and start a new
 *                           session on it. Session-level because the
 *                           Document_Interface cannot follow a change of
 *                           document (findings risk 7).
 *   {"op": "subscribe", "args": {"events": ["selection_changed", ...]}}
 *   {"op": "unsubscribe", "args": {"events": [...]}}
 *                       ->  {"ok": true, "result": {"subscribed": [...],
 *                           "available": [...]}}. "*" means every event;
 *                           unsubscribe without "events" drops them all.
 *                           Subscriptions belong to the connection.
 *
 * Push events: once subscribed, the server also writes unsolicited frames
 *
 *   {"event": "<name>", "seq": n, "data": {...}}
 *
 * on the same connection, same framing, each one a whole line. They never
 * split a response line; events raised while a request is being handled
 * are written after its response, except session_ending, which goes out
 * just before the response to the request that ends the session, so the
 * client holds it by the time that response arrives. See EventMonitor.
 *
 * One client at a time; a second connection is sent an error line and closed.
 * A client disconnect does not stop the server -- the session ends on
 * "shutdown" or stop().
 */
class BridgeServer : public QObject
{
    Q_OBJECT

public:
    //! \a native enables the in-process operations (exec_command, cmd_dim,
    //! cmd_hatch, select_entities); without it they report "unavailable".
    explicit BridgeServer(Document_Interface *doc, NativeBridge *native = nullptr,
                          QObject *parent = nullptr);
    ~BridgeServer() override;

    //! Socket path used when none is given: $LC_PYBRIDGE_SOCKET if set,
    //! otherwise the name "librecad-pybridge", which QLocalServer places in
    //! $XDG_RUNTIME_DIR (user-only) or /tmp.
    static QString defaultSocketName();

    //! Listen on \a socketName. False on failure; error in errorString().
    bool listen(const QString &socketName);

    //! Serve until stop() or a "shutdown" request. Spins a nested event loop,
    //! so the GUI stays responsive while a session is open. Returns the number
    //! of requests handled.
    int serve();

    QString errorString() const { return m_error; }
    QString fullServerName() const;
    int requestsHandled() const { return m_requestsHandled; }
    //! Set when the session ended on file_open or file_new.
    SessionRestart pendingRestart() const { return m_pendingRestart; }
    QString sessionId() const { return m_sessionId; }

public slots:
    //! End serve() from outside, e.g. a Stop button.
    void stop();

signals:
    //! Emitted after each handled request, for a status display.
    void requestHandled(int total);
    void clientChanged(bool connected);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onEventRaised(const QString &name, const QJsonObject &data);

private:
    void processLine(const QByteArray &line);
    void sendToClient(const QByteArray &line);
    QByteArray eventFrame(const QString &name, const QJsonObject &data);
    //! Write session_ending (once per session) if the client subscribed.
    void announceSessionEnding(const QString &reason);

    QJsonObject sessionRequest(const QJsonObject &request, bool *stopAfter);

    Document_Interface *m_doc {nullptr};
    NativeBridge *m_native {nullptr};
    Dispatcher *m_dispatcher {nullptr};
    SessionRestart m_pendingRestart;
    QString m_sessionId;
    QLocalServer *m_server {nullptr};
    QLocalSocket *m_client {nullptr};
    QByteArray m_buffer;
    QString m_error;
    int m_requestsHandled {0};
    bool m_stopping {false};
    QEventLoop *m_stopLoop {nullptr};

    EventMonitor *m_events {nullptr};
    QList<QByteArray> m_pendingEvents;   // raised while a request ran
    bool m_processing {false};
    bool m_sessionEndingSent {false};
    int m_eventSeq {0};
};

} // namespace lcbridge

#endif // LC_BRIDGE_SERVER_H
