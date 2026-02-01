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

        // Check if this session actually exists - reject if stale
        if (!d->sessions.contains(session)) {
            qWarning() << "Root POST for unknown/stale session:" << session;
            // Return JSON-RPC error response
            QJsonObject errorResponse;
            errorResponse["jsonrpc"] = "2.0";
            QJsonObject error;
            error["code"] = -32600;  // InvalidRequest
            error["message"] = "Invalid session - please reconnect and re-initialize";
            QJsonObject errorData;
            errorData["sessionId"] = session.toString(QUuid::WithoutBraces);
            errorData["reason"] = "session_not_found";
            error["data"] = errorData;
            errorResponse["error"] = error;

            QByteArray response = QByteArrayLiteral("HTTP/1.1 400 Bad Request\r\n")
                                  + "Content-Type: application/json\r\n"
                                  + "Connection: close\r\n"
                                  + "\r\n"
                                  + QJsonDocument(errorResponse).toJson(QJsonDocument::Compact);

            QTcpSocket *socket = getSocketForRequest(request);
            if (socket) {
                socket->write(response);
                socket->flush();
            }
            return QByteArray();  // Return empty to prevent double-response
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

QByteArray HttpServer::getMcp(const QNetworkRequest &request)
{
    qCDebug(lcQMcpServerSsePlugin) << "/mcp GET received";

    // Check if client is requesting SSE stream
    bool wantsSSE = request.hasRawHeader("Accept") &&
                    request.rawHeader("Accept").contains("text/event-stream");

    // If client wants SSE but we don't support server-initiated messages yet,
    // return 405 Method Not Allowed per spec
    if (wantsSSE) {
        qCDebug(lcQMcpServerSsePlugin) << "Client requested SSE stream, returning 405 (not yet implemented)";

        QTcpSocket *socket = getSocketForRequest(request);
        if (socket) {
            QByteArray response = QByteArrayLiteral("HTTP/1.1 405 Method Not Allowed\r\n")
                                  + "Content-Type: text/plain\r\n"
                                  + "Content-Length: 53\r\n"
                                  + "Connection: close\r\n"
                                  + "\r\n"
                                  + "Server-initiated SSE streams are not yet supported";
            socket->write(response);
            socket->flush();
        }
        return QByteArray();  // Return empty to prevent double-response
    }

    // GET requests without SSE are used for session establishment
    // Extract session ID from header, or create a new session if this is an initial connection
    QUuid session;
    bool isNewSession = false;

    if (request.hasRawHeader("Mcp-Session-Id")) {
        QByteArray sessionIdHeader = request.rawHeader("Mcp-Session-Id");
        QUuid clientSession = QUuid::fromString(QString::fromUtf8(sessionIdHeader));

        if (clientSession.isNull()) {
            qWarning() << "Invalid Mcp-Session-Id in GET:" << sessionIdHeader;
            return QByteArray();
        }

        // Check if this session actually exists
        if (!d->sessions.contains(clientSession)) {
            // Stale session ID - create a fresh one so client knows to re-initialize
            session = QUuid::createUuid();
            isNewSession = true;
            qCDebug(lcQMcpServerSsePlugin) << "GET for unknown session" << clientSession
                                           << "- returning fresh session:" << session;
        } else {
            // Valid existing session
            session = clientSession;
            qCDebug(lcQMcpServerSsePlugin) << "GET for existing session:" << session;
        }
    } else {
        // Initial connection - create a new session
        session = QUuid::createUuid();
        isNewSession = true;
        qCDebug(lcQMcpServerSsePlugin) << "GET without session ID - creating new session:" << session;
    }

    // Get the socket for this request
    QTcpSocket *socket = getSocketForRequest(request);
    if (!socket) {
        qWarning() << "No socket found for GET /mcp request";
        return QByteArray();
    }

    // Register this socket as a session to prevent automatic HTTP wrapper
    registerSession(session, request);

    // Mark this session as using new protocol
    d->sessionUsesNewProtocol.insert(session, true);
    d->sessions.insert(session);

    // Emit newSession signal for initial connections
    if (isNewSession) {
        qCDebug(lcQMcpServerSsePlugin) << "Emitting newSession for session:" << session;
        emit newSession(session);
    }

    // Send 204 No Content for non-SSE GET requests
    QByteArray response = QByteArrayLiteral("HTTP/1.1 204 No Content\r\n")
                          + "Mcp-Session-Id: " + session.toByteArray(QUuid::WithoutBraces) + "\r\n"
                          + "Connection: keep-alive\r\n"
                          + "\r\n";

    socket->write(response);
    socket->flush();

    qCDebug(lcQMcpServerSsePlugin) << "Sent 204 No Content for GET request, session" << session;

    // Return empty since we already sent the response
    return QByteArray();
}

QByteArray HttpServer::headMcp(const QNetworkRequest &request)
{
    qCDebug(lcQMcpServerSsePlugin) << "/mcp HEAD received";

    // HEAD requests are used for connectivity testing
    // Return empty body with 200 OK to indicate server is available
    setResponseHeader("Mcp-Endpoint-Available", "true");
    return QByteArray();
}

QByteArray HttpServer::deleteMcp(const QNetworkRequest &request)
{
    qCDebug(lcQMcpServerSsePlugin) << "/mcp DELETE received";

    // DELETE requests are used to terminate a session
    // Extract session ID from header
    if (!request.hasRawHeader("Mcp-Session-Id")) {
        qWarning() << "DELETE /mcp without Mcp-Session-Id header";
        return QByteArray();
    }

    QByteArray sessionIdHeader = request.rawHeader("Mcp-Session-Id");
    QUuid session = QUuid::fromString(QString::fromUtf8(sessionIdHeader));

    if (session.isNull()) {
        qWarning() << "Invalid Mcp-Session-Id in DELETE:" << sessionIdHeader;
        return QByteArray();
    }

    qCDebug(lcQMcpServerSsePlugin) << "DELETE for session:" << session;

    // Get the socket for this request
    QTcpSocket *socket = getSocketForRequest(request);
    if (!socket) {
        qWarning() << "No socket found for DELETE /mcp request";
        return QByteArray();
    }

    // Register this socket as a session to prevent automatic HTTP wrapper
    registerSession(session, request);

    // Clean up session state
    d->sessionUsesNewProtocol.remove(session);
    d->sessions.remove(session);

    // Remove any pending requests for this session
    for (int i = d->pendingRequests.size() - 1; i >= 0; --i) {
        if (d->pendingRequests[i].sessionId == session) {
            d->pendingRequests.removeAt(i);
        }
    }

    qCDebug(lcQMcpServerSsePlugin) << "Terminated session" << session;

    // Send 200 OK response
    QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\n")
                          + "Mcp-Session-Id: " + session.toByteArray(QUuid::WithoutBraces) + "\r\n"
                          + "Content-Length: 0\r\n"
                          + "Connection: close\r\n"
                          + "\r\n";

    socket->write(response);
    socket->flush();

    qCDebug(lcQMcpServerSsePlugin) << "Sent 200 OK for DELETE request, session" << session;

    // Return empty since we already sent the response
    return QByteArray();
}

QByteArray HttpServer::postMcp(const QNetworkRequest &request, const QByteArray &body)
{
    // New Streamable HTTP protocol endpoint
    qCDebug(lcQMcpServerSsePlugin) << "/mcp POST received";

    QUuid session;

    // Check if client provided a session ID
    if (request.hasRawHeader("Mcp-Session-Id")) {
        const QByteArray sessionIdHeader = request.rawHeader("Mcp-Session-Id");
        session = QUuid::fromString(QString::fromUtf8(sessionIdHeader));

        if (session.isNull()) {
            qWarning() << "Invalid Mcp-Session-Id header:" << sessionIdHeader;
            // Return error for invalid session ID
            QJsonObject errorResponse;
            errorResponse["jsonrpc"] = "2.0";
            QJsonObject error;
            error["code"] = -32600;  // InvalidRequest
            error["message"] = "Invalid Mcp-Session-Id format";
            errorResponse["error"] = error;

            QByteArray response = QByteArrayLiteral("HTTP/1.1 400 Bad Request\r\n")
                                  + "Content-Type: application/json\r\n"
                                  + "Connection: close\r\n"
                                  + "\r\n"
                                  + QJsonDocument(errorResponse).toJson(QJsonDocument::Compact);

            QTcpSocket *socket = getSocketForRequest(request);
            if (socket) {
                socket->write(response);
                socket->flush();
            }
            return QByteArray();
        }

        // Check if this session actually exists - reject if stale
        if (!d->sessions.contains(session)) {
            qWarning() << "POST /mcp for unknown/stale session:" << session;
            // Return JSON-RPC error response
            QJsonObject errorResponse;
            errorResponse["jsonrpc"] = "2.0";
            QJsonObject error;
            error["code"] = -32600;  // InvalidRequest
            error["message"] = "Invalid session - please reconnect and re-initialize";
            QJsonObject errorData;
            errorData["sessionId"] = session.toString(QUuid::WithoutBraces);
            errorData["reason"] = "session_not_found";
            error["data"] = errorData;
            errorResponse["error"] = error;

            QByteArray response = QByteArrayLiteral("HTTP/1.1 400 Bad Request\r\n")
                                  + "Content-Type: application/json\r\n"
                                  + "Mcp-Session-Id: " + session.toByteArray(QUuid::WithoutBraces) + "\r\n"
                                  + "Connection: close\r\n"
                                  + "\r\n"
                                  + QJsonDocument(errorResponse).toJson(QJsonDocument::Compact);

            QTcpSocket *socket = getSocketForRequest(request);
            if (socket) {
                socket->write(response);
                socket->flush();
            }
            return QByteArray();
        }

        qCDebug(lcQMcpServerSsePlugin) << "Using existing session from header:" << session;
    } else {
        // No session ID provided - this shouldn't happen for POST /mcp
        qWarning() << "POST /mcp without Mcp-Session-Id header";
        QJsonObject errorResponse;
        errorResponse["jsonrpc"] = "2.0";
        QJsonObject error;
        error["code"] = -32600;  // InvalidRequest
        error["message"] = "Missing Mcp-Session-Id header - please establish session first with GET /mcp";
        errorResponse["error"] = error;

        QByteArray response = QByteArrayLiteral("HTTP/1.1 400 Bad Request\r\n")
                              + "Content-Type: application/json\r\n"
                              + "Connection: close\r\n"
                              + "\r\n"
                              + QJsonDocument(errorResponse).toJson(QJsonDocument::Compact);

        QTcpSocket *socket = getSocketForRequest(request);
        if (socket) {
            socket->write(response);
            socket->flush();
        }
        return QByteArray();
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

    // Mark this session as using new protocol
    d->sessionUsesNewProtocol.insert(session, true);

    // Parse and forward the request
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        auto jsonObj = doc.object();
        qCDebug(lcQMcpServerSsePlugin) << "/mcp: forwarding to session" << session << "method:" << jsonObj.value("method").toString();

        // Only queue requests that expect a response (have an "id" field)
        // Notifications don't have an id and don't get responses
        if (jsonObj.contains("id")) {
            // Queue this request for async response
            Private::PendingRequest pending;
            pending.socket = socket;
            pending.sessionId = session;
            d->pendingRequests.append(pending);
            qCDebug(lcQMcpServerSsePlugin) << "Queued request for session" << session;
        } else {
            // Notifications must receive HTTP 202 Accepted per MCP spec
            qCDebug(lcQMcpServerSsePlugin) << "Notification received, sending 202 Accepted";
            QByteArray response = QByteArrayLiteral("HTTP/1.1 202 Accepted\r\n")
                                  + "Mcp-Session-Id: " + session.toByteArray(QUuid::WithoutBraces) + "\r\n"
                                  + "Content-Length: 0\r\n"
                                  + "Connection: keep-alive\r\n"
                                  + "\r\n";
            socket->write(response);
            socket->flush();
        }

        emit received(session, jsonObj);
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
