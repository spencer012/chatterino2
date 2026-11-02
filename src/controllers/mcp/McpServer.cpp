// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/mcp/McpServer.hpp"

#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Settings.hpp"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <memory>

namespace chatterino::mcp {

using namespace Qt::StringLiterals;

namespace {

const auto &LOG = chatterinoMcp;

/// Requests bigger than this are refused. A full set of highlights and
/// ignores is far smaller than this.
constexpr qsizetype MAX_REQUEST_SIZE = 16 * 1024 * 1024;

const QStringList SUPPORTED_PROTOCOL_VERSIONS{
    u"2025-11-25"_s,
    u"2025-06-18"_s,
    u"2025-03-26"_s,
    u"2024-11-05"_s,
};

const QString SERVER_INSTRUCTIONS = QStringLiteral(
    "Chatterino is a Twitch chat client. These tools read and change its "
    "highlights, ignores, filters, commands, highlight blacklist and "
    "nicknames, and read the chat that is loaded in memory (open splits, "
    "/mentions, /whispers).\n"
    "\n"
    "Workflow for changing highlights or ignores:\n"
    "1. Read the current state with list_config.\n"
    "2. Check filter syntax with validate_filter, and try it on loaded chat "
    "with test_filter.\n"
    "3. Use simulate_config to compare the current setup with the proposed "
    "one on loaded messages, and check_message for messages that have not "
    "appeared yet.\n"
    "4. Apply the change with add_config_items, update_config_items or "
    "remove_config_items. Every change saves a backup first; list_backups and "
    "restore_backup undo it.\n"
    "\n"
    "Filter language notes (see filter_reference for the full list): "
    "author.name is the display name; ==, !=, contains, startswith and "
    "endswith on strings are case-insensitive; r\"...\" is a case-sensitive "
    "regex and ri\"...\" a case-insensitive one; && and || work on booleans "
    "and ! negates. Highlights never hide messages. Blocking ignore phrases "
    "do, and messages they blocked are never stored, so they cannot be "
    "simulated.");

QJsonObject jsonRpcError(const QJsonValue &id, int code, const QString &message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", QJsonObject{{"code", code}, {"message", message}}},
    };
}

QJsonObject jsonRpcResult(const QJsonValue &id, const QJsonObject &result)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result},
    };
}

struct HttpRequest {
    QByteArray method;
    QByteArray path;
    /// Header names are lowercase
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

QByteArray reasonPhrase(int status)
{
    switch (status)
    {
        case 200:
            return "OK";
        case 202:
            return "Accepted";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 415:
            return "Unsupported Media Type";
        default:
            return "Error";
    }
}

/// Decodes a chunked body. Returns nullopt while it's incomplete.
std::optional<QByteArray> decodeChunked(const QByteArray &data, bool &bad,
                                        qsizetype &consumed)
{
    QByteArray out;
    qsizetype pos = 0;
    while (true)
    {
        auto lineEnd = data.indexOf("\r\n", pos);
        if (lineEnd < 0)
        {
            return std::nullopt;
        }
        auto sizeText = data.mid(pos, lineEnd - pos);
        if (auto ext = sizeText.indexOf(';'); ext >= 0)
        {
            sizeText.truncate(ext);
        }
        bool ok = false;
        auto size = sizeText.trimmed().toLongLong(&ok, 16);
        if (!ok || size < 0 || size > MAX_REQUEST_SIZE)
        {
            bad = true;
            return std::nullopt;
        }
        pos = lineEnd + 2;
        if (size == 0)
        {
            // Skip trailers up to the final empty line
            auto end = data.indexOf("\r\n", pos);
            while (end >= 0 && end != pos)
            {
                pos = end + 2;
                end = data.indexOf("\r\n", pos);
            }
            if (end < 0)
            {
                return std::nullopt;
            }
            consumed = end + 2;
            return out;
        }
        if (data.size() < pos + size + 2)
        {
            return std::nullopt;
        }
        out.append(data.mid(pos, size));
        if (out.size() > MAX_REQUEST_SIZE)
        {
            bad = true;
            return std::nullopt;
        }
        pos += size + 2;
    }
}

/// One client connection. Requests on it are answered in order; while a tool
/// is still running, later requests wait in the buffer.
class HttpConnection : public QObject
{
public:
    using Handler =
        std::function<void(HttpRequest, std::function<void(
                                            int status, QByteArray contentType,
                                            QByteArray body)>)>;

    HttpConnection(QTcpSocket *socket, Handler handler)
        : QObject(socket)
        , socket_(socket)
        , handler_(std::move(handler))
    {
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this] {
            this->buffer_.append(this->socket_->readAll());
            this->processBuffer();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket,
                         &QObject::deleteLater);
    }

private:
    void processBuffer()
    {
        while (!this->busy_)
        {
            auto request = this->takeRequest();
            if (!request)
            {
                return;
            }

            this->busy_ = true;
            QPointer<HttpConnection> self(this);
            this->handler_(std::move(*request),
                           [self](int status, const QByteArray &contentType,
                                  const QByteArray &body) {
                               if (!self)
                               {
                                   return;
                               }
                               self->respond(status, contentType, body);
                               self->busy_ = false;
                               self->processBuffer();
                           });
        }
    }

    /// Takes one complete request out of the buffer, if there is one
    std::optional<HttpRequest> takeRequest()
    {
        auto headerEnd = this->buffer_.indexOf("\r\n\r\n");
        if (headerEnd < 0)
        {
            if (this->buffer_.size() > 64 * 1024)
            {
                this->fail(413);
            }
            return std::nullopt;
        }

        HttpRequest request;
        auto lines = this->buffer_.left(headerEnd).split('\n');
        auto requestLine = lines.value(0).trimmed().split(' ');
        if (requestLine.size() < 3)
        {
            this->fail(400);
            return std::nullopt;
        }
        request.method = requestLine[0];
        request.path = requestLine[1];
        for (qsizetype i = 1; i < lines.size(); ++i)
        {
            auto colon = lines[i].indexOf(':');
            if (colon <= 0)
            {
                continue;
            }
            request.headers.insert(lines[i].left(colon).trimmed().toLower(),
                                   lines[i].mid(colon + 1).trimmed());
        }

        auto bodyStart = headerEnd + 4;
        if (request.headers.value("transfer-encoding").toLower() == "chunked")
        {
            bool bad = false;
            qsizetype consumed = 0;
            auto body =
                decodeChunked(this->buffer_.mid(bodyStart), bad, consumed);
            if (bad)
            {
                this->fail(400);
                return std::nullopt;
            }
            if (!body)
            {
                return std::nullopt;
            }
            request.body = std::move(*body);
            this->buffer_.remove(0, bodyStart + consumed);
            return request;
        }

        bool ok = true;
        auto length = request.headers.value("content-length", "0").toLongLong(&ok);
        if (!ok || length < 0)
        {
            this->fail(400);
            return std::nullopt;
        }
        if (length > MAX_REQUEST_SIZE)
        {
            this->fail(413);
            return std::nullopt;
        }
        if (this->buffer_.size() < bodyStart + length)
        {
            return std::nullopt;
        }
        request.body = this->buffer_.mid(bodyStart, length);
        this->buffer_.remove(0, bodyStart + length);
        return request;
    }

    void respond(int status, const QByteArray &contentType,
                 const QByteArray &body)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' +
                         reasonPhrase(status) + "\r\n";
        if (!contentType.isEmpty())
        {
            out += "Content-Type: " + contentType + "\r\n";
        }
        if (status == 405)
        {
            out += "Allow: POST\r\n";
        }
        if (status == 401)
        {
            out += "WWW-Authenticate: Bearer\r\n";
        }
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        out += "Cache-Control: no-store\r\n";
        out += "\r\n";
        out += body;
        this->socket_->write(out);
    }

    void fail(int status)
    {
        this->respond(status, {}, {});
        this->buffer_.clear();
        this->socket_->disconnectFromHost();
    }

    QTcpSocket *socket_;
    Handler handler_;
    QByteArray buffer_;
    bool busy_ = false;
};

bool isLocalOrigin(const QByteArray &origin)
{
    auto host = QUrl(QString::fromUtf8(origin)).host();
    return host == u"localhost" || host == u"127.0.0.1" || host == u"::1";
}

}  // namespace

McpServer::McpServer(Settings &settings)
    : settings_(settings)
{
    for (auto &&list : {makeConfigTools(), makeChatTools(),
                        makeSimulationTools()})
    {
        for (const auto &tool : list)
        {
            this->tools_.push_back(tool);
        }
    }

    this->settings_.mcpServerEnabled.connect(
        [this](bool) {
            this->restart();
        },
        this->connections_, false);
    this->settings_.mcpServerPort.connect(
        [this](int) {
            this->restart();
        },
        this->connections_, false);
    this->restart();
}

McpServer::~McpServer() = default;

QString McpServer::url(int port)
{
    return u"http://127.0.0.1:%1/mcp"_s.arg(port);
}

QString McpServer::generateToken()
{
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32 *>(bytes.data()), bytes.size() / 4);
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding |
                       QByteArray::OmitTrailingEquals));
}

void McpServer::restart()
{
    if (this->server_)
    {
        this->server_->close();
        this->server_->deleteLater();
        this->server_ = nullptr;
    }

    if (!this->settings_.mcpServerEnabled)
    {
        return;
    }

    if (this->settings_.mcpServerToken.getValue().isEmpty())
    {
        this->settings_.mcpServerToken = McpServer::generateToken();
    }

    auto port = this->settings_.mcpServerPort.getValue();
    if (port < 1 || port > 65535)
    {
        qCWarning(LOG) << "Invalid MCP server port" << port;
        return;
    }

    this->server_ = new QTcpServer(this);
    QObject::connect(this->server_, &QTcpServer::newConnection, this,
                     &McpServer::onNewConnection);
    if (!this->server_->listen(QHostAddress::LocalHost,
                               static_cast<quint16>(port)))
    {
        qCWarning(LOG) << "Could not listen on port" << port << ':'
                       << this->server_->errorString();
        return;
    }
    qCDebug(LOG) << "Listening on" << McpServer::url(port);
}

void McpServer::onNewConnection()
{
    while (auto *socket = this->server_->nextPendingConnection())
    {
        QPointer<McpServer> self(this);
        new HttpConnection(socket, [self](HttpRequest request, auto reply) {
            if (!self)
            {
                reply(503, {}, {});
                return;
            }

            auto path = request.path;
            if (auto query = path.indexOf('?'); query >= 0)
            {
                path.truncate(query);
            }
            if (path != "/mcp" && path != "/mcp/")
            {
                reply(404, {}, {});
                return;
            }

            // Browsers send an Origin header. Refuse pages that aren't local
            // so a website can't talk to this server (DNS rebinding).
            if (request.headers.contains("origin") &&
                !isLocalOrigin(request.headers.value("origin")))
            {
                reply(403, {}, {});
                return;
            }

            auto token = self->settings_.mcpServerToken.getValue().toUtf8();
            if (token.isEmpty() ||
                request.headers.value("authorization") != "Bearer " + token)
            {
                reply(401, "application/json",
                      R"({"error":"Missing or wrong bearer token. Copy it from Chatterino's settings (External tools > MCP server)."})");
                return;
            }

            if (request.method != "POST")
            {
                reply(405, {}, {});
                return;
            }

            QJsonParseError parseError;
            auto doc = QJsonDocument::fromJson(request.body, &parseError);
            if (parseError.error != QJsonParseError::NoError)
            {
                reply(400, "application/json",
                      QJsonDocument(jsonRpcError({}, -32700,
                                                 parseError.errorString()))
                          .toJson(QJsonDocument::Compact));
                return;
            }

            // The 2025-03-26 protocol allows batches, later ones send one
            // message per request.
            QJsonArray messages;
            bool isBatch = doc.isArray();
            if (isBatch)
            {
                messages = doc.array();
            }
            else if (doc.isObject())
            {
                messages.append(doc.object());
            }
            else
            {
                reply(400, "application/json",
                      QJsonDocument(jsonRpcError({}, -32600, "Invalid request"))
                          .toJson(QJsonDocument::Compact));
                return;
            }

            struct Pending {
                qsizetype remaining = 0;
                QJsonArray responses;
            };
            auto pending = std::make_shared<Pending>();
            pending->remaining = messages.size();

            auto finish = [pending, isBatch, reply] {
                if (pending->responses.isEmpty())
                {
                    reply(202, {}, {});
                    return;
                }
                auto body =
                    isBatch
                        ? QJsonDocument(pending->responses)
                              .toJson(QJsonDocument::Compact)
                        : QJsonDocument(pending->responses.first().toObject())
                              .toJson(QJsonDocument::Compact);
                reply(200, "application/json", body);
            };

            if (messages.isEmpty())
            {
                finish();
                return;
            }

            for (const auto &message : messages)
            {
                self->handleMessage(
                    message.toObject(),
                    [pending, finish](std::optional<QJsonObject> response) {
                        if (response)
                        {
                            pending->responses.append(*response);
                        }
                        if (--pending->remaining == 0)
                        {
                            finish();
                        }
                    });
            }
        });
    }
}

void McpServer::handleMessage(
    const QJsonObject &message,
    std::function<void(std::optional<QJsonObject>)> done)
{
    auto method = message.value("method").toString();
    auto id = message.value("id");
    bool isNotification = !message.contains("id");

    if (method.isEmpty())
    {
        // A response to something we sent (we never send requests) or junk
        done(isNotification ? std::nullopt
                            : std::optional(jsonRpcError(
                                  id, -32600, "Invalid request")));
        return;
    }

    if (isNotification)
    {
        // notifications/initialized, notifications/cancelled, ...
        done(std::nullopt);
        return;
    }

    auto params = message.value("params").toObject();

    if (method == u"initialize")
    {
        auto requested = params.value("protocolVersion").toString();
        auto version = SUPPORTED_PROTOCOL_VERSIONS.contains(requested)
                           ? requested
                           : SUPPORTED_PROTOCOL_VERSIONS.first();
        done(jsonRpcResult(
            id, {
                    {"protocolVersion", version},
                    {"capabilities",
                     QJsonObject{
                         {"tools", QJsonObject{{"listChanged", false}}},
                     }},
                    {"serverInfo",
                     QJsonObject{
                         {"name", "chatterino"},
                         {"title", "Chatterino"},
                         {"version", Version::instance().version()},
                     }},
                    {"instructions", SERVER_INSTRUCTIONS},
                }));
        return;
    }

    if (method == u"ping")
    {
        done(jsonRpcResult(id, {}));
        return;
    }

    if (method == u"tools/list")
    {
        QJsonArray tools;
        for (const auto &tool : this->tools_)
        {
            tools.append(QJsonObject{
                {"name", tool.name},
                {"description", tool.description},
                {"inputSchema", tool.inputSchema},
                {"annotations",
                 QJsonObject{
                     {"readOnlyHint", !tool.changesSettings},
                     {"destructiveHint", tool.changesSettings},
                     {"openWorldHint", false},
                 }},
            });
        }
        done(jsonRpcResult(id, {{"tools", tools}}));
        return;
    }

    if (method == u"tools/call")
    {
        this->callTool(params, [id, done](const QJsonObject &result) {
            done(jsonRpcResult(id, result));
        });
        return;
    }

    done(jsonRpcError(id, -32601, u"Method not found: %1"_s.arg(method)));
}

void McpServer::callTool(const QJsonObject &params,
                         std::function<void(QJsonObject)> done)
{
    auto name = params.value("name").toString();
    auto arguments = params.value("arguments").toObject();

    auto toResult = [](const ToolResult &result) {
        if (!result.error.isEmpty())
        {
            return QJsonObject{
                {"content", QJsonArray{QJsonObject{
                                {"type", "text"},
                                {"text", result.error},
                            }}},
                {"isError", true},
            };
        }

        QString text;
        if (result.data.isObject())
        {
            text = QString::fromUtf8(QJsonDocument(result.data.toObject())
                                         .toJson(QJsonDocument::Indented));
        }
        else if (result.data.isArray())
        {
            text = QString::fromUtf8(QJsonDocument(result.data.toArray())
                                         .toJson(QJsonDocument::Indented));
        }
        else
        {
            text = result.data.toVariant().toString();
        }

        return QJsonObject{
            {"content", QJsonArray{QJsonObject{
                            {"type", "text"},
                            {"text", text},
                        }}},
            {"isError", false},
        };
    };

    for (const auto &tool : this->tools_)
    {
        if (tool.name != name)
        {
            continue;
        }

        if (tool.changesSettings && !this->settings_.mcpServerAllowChanges)
        {
            done(toResult(ToolResult::fail(
                "Changes are turned off in Chatterino's settings (External "
                "tools > MCP server > Allow changes). Ask the user to turn "
                "them on.")));
            return;
        }

        // Guard against tools calling `done` twice
        auto called = std::make_shared<bool>(false);
        tool.run(arguments, [done, toResult, called](const ToolResult &result) {
            if (*called)
            {
                return;
            }
            *called = true;
            done(toResult(result));
        });
        return;
    }

    done(toResult(ToolResult::fail(u"Unknown tool: %1"_s.arg(name))));
}

namespace schema {

QJsonObject object(const QJsonObject &properties, const QStringList &required)
{
    QJsonObject o{
        {"type", "object"},
        {"properties", properties},
        {"additionalProperties", false},
    };
    if (!required.isEmpty())
    {
        o.insert("required", QJsonArray::fromStringList(required));
    }
    return o;
}

QJsonObject string(const QString &description)
{
    return {{"type", "string"}, {"description", description}};
}

QJsonObject stringEnum(const QString &description, const QStringList &values)
{
    return {
        {"type", "string"},
        {"description", description},
        {"enum", QJsonArray::fromStringList(values)},
    };
}

QJsonObject integer(const QString &description)
{
    return {{"type", "integer"}, {"description", description}};
}

QJsonObject boolean(const QString &description)
{
    return {{"type", "boolean"}, {"description", description}};
}

QJsonObject array(const QString &description, const QJsonObject &items)
{
    return {
        {"type", "array"},
        {"description", description},
        {"items", items},
    };
}

QJsonObject anyObject(const QString &description)
{
    return {{"type", "object"}, {"description", description}};
}

}  // namespace schema

namespace args {

QString string(const QJsonObject &args, const QString &key,
               const QString &fallback)
{
    auto v = args.value(key);
    return v.isString() ? v.toString() : fallback;
}

int integer(const QJsonObject &args, const QString &key, int fallback)
{
    auto v = args.value(key);
    return v.isDouble() ? v.toInt(fallback) : fallback;
}

bool boolean(const QJsonObject &args, const QString &key, bool fallback)
{
    auto v = args.value(key);
    return v.isBool() ? v.toBool() : fallback;
}

}  // namespace args

}  // namespace chatterino::mcp
