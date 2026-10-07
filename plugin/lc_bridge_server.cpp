/*****************************************************************************/
/*  lc_bridge_server.cpp - local-socket transport for the dispatch layer     */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_server.h"

#include "lc_bridge_dispatch.h"

#include <QEventLoop>
#include <QFileInfo>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>

namespace lcbridge {
namespace {

QByteArray encode(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject errorResponse(const QString &code, const QString &message,
                          const QJsonValue &id = QJsonValue())
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);

    QJsonObject response;
    if (!id.isUndefined() && !id.isNull())
        response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("ok"), false);
    response.insert(QStringLiteral("error"), error);
    return response;
}

} // namespace

BridgeServer::BridgeServer(Document_Interface *doc, NativeBridge *native,
                           QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_native(native)
    , m_dispatcher(new Dispatcher(doc, native))
    , m_server(new QLocalServer(this))
    , m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    // The socket lives in a user-owned directory, but enforce user-only access
    // on the socket itself as well: this is an open door into the drawing.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection,
            this, &BridgeServer::onNewConnection);
}

BridgeServer::~BridgeServer()
{
    delete m_dispatcher;
}

QString BridgeServer::defaultSocketName()
{
    const QByteArray fromEnvironment = qgetenv("LC_PYBRIDGE_SOCKET");
    if (!fromEnvironment.isEmpty())
        return QString::fromLocal8Bit(fromEnvironment);
    // A bare name: QLocalServer resolves it to QDir::tempPath()/<name>, i.e.
    // $TMPDIR or /tmp -- not XDG_RUNTIME_DIR. The Python client mirrors this.
    // The UserAccessOption set on the server keeps the socket 0600 even in a
    // world-writable /tmp.
    return QStringLiteral("librecad-pybridge");
}

bool BridgeServer::listen(const QString &socketName)
{
    // A stale socket file from a crashed session would make listen() fail.
    QLocalServer::removeServer(socketName);

    if (!m_server->listen(socketName)) {
        m_error = m_server->errorString();
        return false;
    }
    return true;
}

QString BridgeServer::fullServerName() const
{
    return m_server->fullServerName();
}

int BridgeServer::serve()
{
    // Local event loop, so execComm() stays on the stack for the whole
    // session and the Document_Interface stays valid. Qt keeps painting and
    // the user keeps control of LibreCAD while this runs.
    QEventLoop loop;
    connect(this, &BridgeServer::destroyed, &loop, &QEventLoop::quit);

    m_stopLoop = &loop;
    loop.exec();
    m_stopLoop = nullptr;

    m_server->close();
    if (m_client) {
        m_client->flush();
        m_client->disconnectFromServer();
        m_client = nullptr;
    }
    return m_requestsHandled;
}

void BridgeServer::stop()
{
    m_stopping = true;
    if (m_stopLoop)
        m_stopLoop->quit();
}

void BridgeServer::onNewConnection()
{
    while (QLocalSocket *pending = m_server->nextPendingConnection()) {
        if (m_client) {
            // One client owns the session; tell the other one why.
            pending->write(encode(errorResponse(
                QStringLiteral("busy"),
                QStringLiteral("another client is connected to this bridge"))));
            pending->flush();
            pending->disconnectFromServer();
            connect(pending, &QLocalSocket::disconnected,
                    pending, &QLocalSocket::deleteLater);
            continue;
        }

        m_client = pending;
        m_buffer.clear();
        connect(m_client, &QLocalSocket::readyRead,
                this, &BridgeServer::onReadyRead);
        connect(m_client, &QLocalSocket::disconnected,
                this, &BridgeServer::onDisconnected);
        emit clientChanged(true);
    }
}

void BridgeServer::onReadyRead()
{
    if (!m_client)
        return;
    m_buffer += m_client->readAll();

    // Complete lines only; a partial line stays buffered for the next read.
    int newline = -1;
    while (!m_stopping && (newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (!line.trimmed().isEmpty())
            processLine(line);
    }
}

void BridgeServer::onDisconnected()
{
    if (auto *socket = qobject_cast<QLocalSocket *>(sender()))
        socket->deleteLater();
    m_client = nullptr;
    m_buffer.clear();
    emit clientChanged(false);
    // The server keeps listening: a script may run, exit, and a later script
    // connect again, all within one session (and one undo step).
}

void BridgeServer::processLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);

    QJsonObject response;
    bool shutdown = false;

    if (parseError.error != QJsonParseError::NoError) {
        response = errorResponse(QStringLiteral("parse_error"),
                                 parseError.errorString());
    } else if (!document.isObject()) {
        response = errorResponse(QStringLiteral("bad_request"),
                                 QStringLiteral("request must be a JSON object"));
    } else {
        const QJsonObject request = document.object();
        const QString op = request.value(QStringLiteral("op")).toString();
        if (op == QLatin1String("shutdown") || op == QLatin1String("session")
            || op == QLatin1String("file_open") || op == QLatin1String("file_new")
            || op == QLatin1String("activate_document")
            || op == QLatin1String("file_close")) {
            // Session-level: acknowledged before the loop is told to quit, so
            // the client sees the reply.
            response = sessionRequest(request, &shutdown);
        } else {
            response = m_dispatcher->dispatch(request);
        }
    }

    ++m_requestsHandled;
    sendToClient(encode(response));
    emit requestHandled(m_requestsHandled);

    if (shutdown)
        stop();
}

QJsonObject BridgeServer::sessionRequest(const QJsonObject &request, bool *stopAfter)
{
    const QString op = request.value(QStringLiteral("op")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();
    QJsonObject response;
    QJsonValue result;

    if (op == QLatin1String("session")) {
        QJsonObject out;
        out.insert(QStringLiteral("id"), m_sessionId);
        out.insert(QStringLiteral("requests"), m_requestsHandled);
        result = out;
    } else if (op == QLatin1String("shutdown")) {
        *stopAfter = true;
    } else if (!m_native || !m_native->modificationAvailable()) {
        response = errorResponse(
            QStringLiteral("unavailable"),
            m_native ? QStringLiteral("native access unavailable: %1")
                           .arg(m_native->reason())
                     : QStringLiteral("no native bridge in this server "
                                      "(offline stub?)"));
    } else if (op == QLatin1String("file_open")) {
        const QJsonValue pathValue = args.value(QStringLiteral("path"));
        const QFileInfo info(pathValue.toString());
        if (!pathValue.isString() || pathValue.toString().isEmpty()) {
            response = errorResponse(QStringLiteral("bad_args"),
                                     QStringLiteral("\"path\" must be a string"));
        } else if (!info.isFile() || !info.isReadable()) {
            response = errorResponse(QStringLiteral("not_found"),
                                     QStringLiteral("\"%1\" is not a readable file")
                                         .arg(info.filePath()));
        } else {
            m_pendingRestart.kind = SessionRestart::OpenFile;
            m_pendingRestart.path = info.absoluteFilePath();
            QJsonObject out;
            out.insert(QStringLiteral("path"), m_pendingRestart.path);
            result = out;
            *stopAfter = true;
        }
    } else if (op == QLatin1String("activate_document")) {
        response = activateDocument(args, &result, stopAfter);
    } else if (op == QLatin1String("file_close")) {
        response = closeDocument(args, &result, stopAfter);
    } else {   // file_new
        m_pendingRestart.kind = SessionRestart::NewDrawing;
        m_pendingRestart.path.clear();
        *stopAfter = true;
    }

    if (response.isEmpty()) {
        response.insert(QStringLiteral("ok"), true);
        response.insert(QStringLiteral("result"), result);
    }
    if (request.contains(QStringLiteral("id")))
        response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    return response;
}

QJsonObject BridgeServer::activateDocument(const QJsonObject &args,
                                           QJsonValue *result, bool *stopAfter)
{
    const QJsonValue indexValue = args.value(QStringLiteral("index"));
    const QJsonValue pathValue = args.value(QStringLiteral("path"));
    const bool byIndex = indexValue.isDouble();
    const bool byPath = pathValue.isString() && !pathValue.toString().isEmpty();
    if (byIndex == byPath) {
        return errorResponse(QStringLiteral("bad_args"),
                             QStringLiteral("pass \"index\" (a number) or "
                                            "\"path\" (a string)"));
    }
    QList<DocumentWindow> windows;
    if (!documentWindows(&windows)) {
        return errorResponse(QStringLiteral("failed"),
                             QStringLiteral("cannot read the document windows"));
    }
    const DocumentWindow *target = nullptr;
    const QString wanted = byPath ? QFileInfo(pathValue.toString()).absoluteFilePath()
                                  : QString();
    for (const DocumentWindow &window : windows) {
        if (byIndex && window.index == indexValue.toInt()) {
            target = &window;
            break;
        }
        // A drawing window, not the block editors and previews that share
        // its document's file name. A file can be open in several windows;
        // the session's own one wins, otherwise the first.
        if (byPath && window.parent < 0 && !window.path.isEmpty()
            && QFileInfo(window.path).absoluteFilePath() == wanted
            && (!target || window.active)) {
            target = &window;
        }
    }
    if (!target) {
        return errorResponse(QStringLiteral("not_found"),
                             byIndex ? QStringLiteral("no document window %1")
                                           .arg(indexValue.toInt())
                                     : QStringLiteral("no open document \"%1\"")
                                           .arg(wanted));
    }
    QJsonObject out;
    out.insert(QStringLiteral("index"), target->index);
    out.insert(QStringLiteral("path"), target->path);
    out.insert(QStringLiteral("restart"), !target->active);
    *result = out;
    if (!target->active) {
        m_pendingRestart.kind = SessionRestart::ActivateWindow;
        m_pendingRestart.index = target->index;
        m_pendingRestart.path = target->path;
        *stopAfter = true;
    }
    return QJsonObject();
}

QJsonObject BridgeServer::closeDocument(const QJsonObject &args,
                                        QJsonValue *result, bool *stopAfter)
{
    const QJsonValue discardValue = args.value(QStringLiteral("discard"));
    if (!discardValue.isUndefined() && !discardValue.isNull() && !discardValue.isBool()) {
        return errorResponse(QStringLiteral("bad_args"),
                             QStringLiteral("\"discard\" must be a boolean"));
    }
    const bool discard = discardValue.toBool(false);
    // Refuse here rather than let QC_MDIWindow::closeEvent put up its modal
    // save/discard question, which would hang an unattended run.
    if (!discard && m_native->hasUnsavedChanges()) {
        return errorResponse(QStringLiteral("bad_request"),
                             QStringLiteral("unsaved changes; pass discard=True "
                                            "or save first"));
    }
    QList<DocumentWindow> windows;
    if (!documentWindows(&windows)) {
        return errorResponse(QStringLiteral("failed"),
                             QStringLiteral("cannot read the document windows"));
    }
    int active = -1;
    for (const DocumentWindow &window : windows) {
        if (window.active)
            active = window.index;
    }
    if (active < 0) {
        return errorResponse(QStringLiteral("failed"),
                             QStringLiteral("no active document window"));
    }
    // Closing a drawing closes its block editors and print previews too.
    int remaining = 0;
    for (const DocumentWindow &window : windows) {
        if (window.index != active && window.parent != active)
            ++remaining;
    }
    QJsonObject out;
    out.insert(QStringLiteral("remaining"), remaining);
    *result = out;
    m_pendingRestart.kind = SessionRestart::CloseWindow;
    m_pendingRestart.discard = discard;
    m_pendingRestart.path.clear();
    *stopAfter = true;
    return QJsonObject();
}

void BridgeServer::sendToClient(const QByteArray &line)
{
    if (!m_client)
        return;
    m_client->write(line);
    m_client->flush();
}

} // namespace lcbridge
