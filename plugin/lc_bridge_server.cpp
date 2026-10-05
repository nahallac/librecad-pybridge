/*****************************************************************************/
/*  lc_bridge_server.cpp - local-socket transport for the dispatch layer     */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_server.h"

#include "lc_bridge_dispatch.h"

#include <QEventLoop>
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

BridgeServer::BridgeServer(Document_Interface *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_dispatcher(new Dispatcher(doc))
    , m_server(new QLocalServer(this))
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
    // A bare name: QLocalServer resolves it to $XDG_RUNTIME_DIR/<name>, or
    // /tmp/<name> without XDG_RUNTIME_DIR. The Python client mirrors this.
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
        if (request.value(QStringLiteral("op")).toString()
                == QLatin1String("shutdown")) {
            // Server-level: acknowledged before the loop is told to quit, so
            // the client sees the reply.
            response.insert(QStringLiteral("ok"), true);
            response.insert(QStringLiteral("result"), QJsonValue());
            if (request.contains(QStringLiteral("id")))
                response.insert(QStringLiteral("id"),
                                request.value(QStringLiteral("id")));
            shutdown = true;
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

void BridgeServer::sendToClient(const QByteArray &line)
{
    if (!m_client)
        return;
    m_client->write(line);
    m_client->flush();
}

} // namespace lcbridge
