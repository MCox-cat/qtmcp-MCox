#include "httpserver.h"
#include <QtCore/QUrlQuery>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtNetwork/QTcpSocket>

Q_DECLARE_LOGGING_CATEGORY(lcQMcpServerSsePlugin)

class HttpServer::Private{
public:
    QSet<QUuid> sessions;
    QUuid implicitSession;  // For handling direct POSTs without prior SSE connection
    QHash<QUuid, bool> sessionUsesNewProtocol;  // Track which sessions use new protocol

    // For new protocol: store pending requests that need responses
    struct PendingRequest {
        QTcpSocket *socket;
        QUuid sessionId;
    };
    QList<PendingRequest> pendingRequests;
};

HttpServer::HttpServer(QObject *parent)
    : QMcpAbstractHttpServer(parent)
    , d(new Private)
{
}

HttpServer::~HttpServer() = default;

QByteArray HttpServer::getSse(const QNetworkRequest &request)
{
    QByteArray response;
    if (request.hasRawHeader("Accept") && request.rawHeader("Accept") == "text/event-stream") {
        auto uuid = registerSseRequest(request);
        if (!uuid.isNull()) {
            d->sessions.insert(uuid);
            response += "event: endpoint\r\ndata: /messages/?session_id=";
            response += uuid.toByteArray(QUuid::WithoutBraces);
            response += "\r\n\r\n";
            emit newSession(uuid);
        } else {
            qWarning() << uuid << "is empty";
        }
    } else {
        qWarning() << request.headers();
    }
    return response;
}

QByteArray HttpServer::post(const QNetworkRequest &request, const QByteArray &body)
{
    // Handle root POST - Check for new protocol (Mcp-Session-Id header)
    qCDebug(lcQMcpServerSsePlugin) << "Root POST received";

    // Check if this is using the new Streamable HTTP protocol (has Mcp-Session-Id header)
    QUuid session;
    bool usesNewProtocol = false;

    if (request.hasRawHeader("Mcp-Session-Id")) {
        // New protocol: extract session ID from header
        const QByteArray sessionIdHeader = request.rawHeader("Mcp-Session-Id");
        session = QUuid::fromString(QString::fromUtf8(sessionIdHeader));
        usesNewProtocol = true;
        qCDebug(lcQMcpServerSsePlugin) << "New protocol POST with session ID from header:" << session;
        
        if (session.isNull()) {
            qWarning() << "Invalid Mcp-Session-Id header:" << sessionIdHeader;
            return QByteArray();
        }

        // Queue this request for async response
        QTcpSocket *socket = getSocketForRequest(request);
        if (socket) {
            Private::PendingRequest pending;
            pending.socket = socket;
            pending.sessionId = session;
            d->pendingRequests.append(pending);
            qCDebug(lcQMcpServerSsePlugin) << "Queued request for session" << session;
        }
    } else {
        // Legacy protocol: create or reuse implicit session
        if (!d->sessions.isEmpty()) {
            // Use an existing SSE session
            session = *d->sessions.begin();
        } else if (!d->implicitSession.isNull()) {
            // Reuse implicit session from previous request
            session = d->implicitSession;
        } else {
            // Create an implicit session for direct POST (legacy clients)
            session = QUuid::createUuid();
            d->implicitSession = session;
            qCDebug(lcQMcpServerSsePlugin) << "Created implicit session for legacy POST:" << session;
            emit newSession(session);
        }
    }

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        qCDebug(lcQMcpServerSsePlugin) << "POST: forwarding to session" << session;
        emit received(session, doc.object());
    } else {
        qWarning() << body;
        qWarning() << "error parsing message" << error.errorString();
    }

    // For new protocol, return empty (response will be sent via sendWithHeader)
    // For legacy, return "Accept"
    return usesNewProtocol ? QByteArray() : "Accept"_ba;
}

QByteArray HttpServer::postMessages(const QNetworkRequest &request, const QByteArray &body)
{
    QUrlQuery query(request.url().query());
    const auto session = QUuid::fromString("{"_L1 + query.queryItemValue("session_id") + "}"_L1);
    if (session.isNull()) {
        qWarning() << "session id error" << query.queryItemValue("session_id");
        return QByteArray();
    }
    if (!d->sessions.contains(session)) {
        qWarning() << "missing session id" << session;
        return QByteArray();
    }

    // Parse message body for target client ID
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        emit received(session, doc.object());
    } else {
        qWarning() << body;
        qWarning() << "error parsing message" << error.errorString();
    }
    return "Accept"_ba;
}

void HttpServer::send(const QUuid &session, const QJsonObject &object)
{
    // Check if this session uses the new protocol
    if (d->sessionUsesNewProtocol.value(session, false)) {
        sendWithHeader(session, object);
    } else {
        // Legacy SSE protocol
        sendSseEvent(session, QJsonDocument(object).toJson(QJsonDocument::Compact), "message"_L1);
    }
}

void HttpServer::sendWithHeader(const QUuid &session, const QJsonObject &object)
{
    // New protocol: Send response with Mcp-Session-Id header
    // Find the pending request for this session
    for (int i = 0; i < d->pendingRequests.size(); ++i) {
        if (d->pendingRequests[i].sessionId == session) {
            auto pending = d->pendingRequests.takeAt(i);
            QTcpSocket *socket = pending.socket;

            QByteArray jsonData = QJsonDocument(object).toJson(QJsonDocument::Compact);

            QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\n")
                                  + "Content-Type: application/json\r\n"
                                  + "Mcp-Session-Id: " + session.toByteArray(QUuid::WithoutBraces) + "\r\n"
                                  + "Content-Length: " + QByteArray::number(jsonData.size()) + "\r\n"
                                  + "Connection: keep-alive\r\n"
                                  + "\r\n"
                                  + jsonData;

            socket->write(response);
            socket->flush();
            qCDebug(lcQMcpServerSsePlugin) << "Sent response with Mcp-Session-Id header for session" << session;
            return;
        }
    }
    
    qWarning() << "No pending request found for session" << session;
}

QByteArray HttpServer::postMcp(const QNetworkRequest &request, const QByteArray &body)
{
    // New Streamable HTTP protocol endpoint
    qCDebug(lcQMcpServerSsePlugin) << "/mcp POST received";

    QUuid session;
    bool isNewSession = false;

    // Check if client provided a session ID
    if (request.hasRawHeader("Mcp-Session-Id")) {
        const QByteArray sessionIdHeader = request.rawHeader("Mcp-Session-Id");
        session = QUuid::fromString(QString::fromUtf8(sessionIdHeader));
        qCDebug(lcQMcpServerSsePlugin) << "Using existing session from header:" << session;

        if (session.isNull()) {
            qWarning() << "Invalid Mcp-Session-Id header:" << sessionIdHeader;
            // Create new session as fallback
            session = QUuid::createUuid();
            isNewSession = true;
        }
    }

    if (session.isNull()) {
        // Create a new session for the new protocol
        session = QUuid::createUuid();
        isNewSession = true;
    }

    // Get the socket for this request
    QTcpSocket *socket = getSocketForRequest(request);
    if (!socket) {
        qWarning() << "Could not find socket for request";
        return QByteArray();
    }

    // Register this socket as a session so HTTP layer won't wrap the response
    // This prevents the automatic HTTP response wrapper
    registerSession(session, request);

    if (isNewSession) {
        d->sessionUsesNewProtocol.insert(session, true);
        qCDebug(lcQMcpServerSsePlugin) << "Created new protocol session:" << session;
        emit newSession(session);
    }

    // Queue this request for async response
    Private::PendingRequest pending;
    pending.socket = socket;
    pending.sessionId = session;
    d->pendingRequests.append(pending);
    qCDebug(lcQMcpServerSsePlugin) << "Queued request for session" << session;

    // Parse and forward the request
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        qCDebug(lcQMcpServerSsePlugin) << "/mcp: forwarding to session" << session;
        emit received(session, doc.object());
    } else {
        qWarning() << "Error parsing /mcp request:" << error.errorString();
        qWarning() << body;

        // Remove from pending and send error response immediately
        for (int i = 0; i < d->pendingRequests.size(); ++i) {
            if (d->pendingRequests[i].sessionId == session) {
                d->pendingRequests.removeAt(i);
                break;
            }
        }

        QByteArray errorJson = QByteArrayLiteral("{\"error\":\"Invalid JSON\"}");
        QByteArray response = QByteArrayLiteral("HTTP/1.1 400 Bad Request\r\n")
                              + "Content-Type: application/json\r\n"
                              + "Content-Length: " + QByteArray::number(errorJson.size()) + "\r\n"
                              + "Connection: close\r\n"
                              + "\r\n"
                              + errorJson;
        socket->write(response);
        socket->flush();
        return QByteArray();  // Return empty since we handled the response
    }

    // Return empty - response will be sent asynchronously via sendWithHeader
    // Socket is registered as a session, so this won't be wrapped in HTTP response
    return QByteArray();
}
