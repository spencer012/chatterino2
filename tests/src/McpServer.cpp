// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/mcp/McpServer.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/UserData.hpp"
#include "singletons/Settings.hpp"
#include "Test.hpp"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>

using namespace chatterino;
using namespace Qt::Literals;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : mock::BaseApplication(R"({})", false, true)
        , highlights(this->settings, &this->accounts)
    {
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    HighlightController *getHighlights() override
    {
        return &this->highlights;
    }

    IUserDataController *getUserData() override
    {
        return &this->userData;
    }

    AccountController accounts;
    HighlightController highlights;
    mock::UserDataController userData;
};

class McpServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        this->app = std::make_unique<MockApplication>();
        this->server = std::make_unique<mcp::McpServer>(this->app->settings);
    }

    void TearDown() override
    {
        this->server.reset();
        this->app.reset();
    }

    std::optional<QJsonObject> send(const QJsonObject &message)
    {
        std::optional<QJsonObject> response;
        bool called = false;
        this->server->handleMessage(message,
                                    [&](std::optional<QJsonObject> r) {
                                        called = true;
                                        response = std::move(r);
                                    });
        EXPECT_TRUE(called) << "the server didn't answer synchronously";
        return response;
    }

    /// Calls a tool and returns its parsed JSON output (or the error text in
    /// "error")
    QJsonObject callTool(const QString &name, const QJsonObject &arguments)
    {
        auto response = this->send({
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", QJsonObject{{"name", name}, {"arguments", arguments}}},
        });
        EXPECT_TRUE(response.has_value());
        auto result = response->value("result").toObject();
        auto text = result.value("content")
                        .toArray()
                        .at(0)
                        .toObject()
                        .value("text")
                        .toString();
        if (result.value("isError").toBool())
        {
            return {{"error", text}};
        }
        return QJsonDocument::fromJson(text.toUtf8()).object();
    }

    std::unique_ptr<MockApplication> app;
    std::unique_ptr<mcp::McpServer> server;
};

/// Sends a raw HTTP request to the server and returns the raw response
QByteArray httpRequest(int port, const QByteArray &request)
{
    QTcpSocket socket;
    QByteArray response;
    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);

    QObject::connect(&socket, &QTcpSocket::connected, [&] {
        socket.write(request);
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, [&] {
        response += socket.readAll();
        auto headerEnd = response.indexOf("\r\n\r\n");
        if (headerEnd < 0)
        {
            return;
        }
        qsizetype length = 0;
        for (const auto &line : response.left(headerEnd).split('\n'))
        {
            if (line.toLower().startsWith("content-length:"))
            {
                length = line.mid(15).trimmed().toLongLong();
            }
        }
        if (response.size() >= headerEnd + 4 + length)
        {
            loop.quit();
        }
    });
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &loop,
                     &QEventLoop::quit);

    socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    loop.exec();
    return response;
}

QByteArray post(const QByteArray &body, const QByteArray &extraHeaders)
{
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Content-Type: application/json\r\n"
           "Accept: application/json, text/event-stream\r\n" +
           extraHeaders + "Content-Length: " + QByteArray::number(body.size()) +
           "\r\n\r\n" + body;
}

const QString STREAMER_FILTER =
    uR"x(author.name == "streamer" && !(message.content match r"^(!\w+|Now playing:)"))x"_s;

}  // namespace

TEST_F(McpServerTest, Initialize)
{
    auto response = this->send({
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", QJsonObject{{"protocolVersion", "2025-06-18"}}},
    });
    ASSERT_TRUE(response.has_value());
    auto result = response->value("result").toObject();
    EXPECT_EQ(result.value("protocolVersion").toString(), "2025-06-18");
    EXPECT_EQ(result.value("serverInfo").toObject().value("name").toString(),
              "chatterino");
    EXPECT_TRUE(result.value("capabilities").toObject().contains("tools"));

    // Unknown versions get our newest one
    response = this->send({
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "initialize"},
        {"params", QJsonObject{{"protocolVersion", "1999-01-01"}}},
    });
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->value("result")
                     .toObject()
                     .value("protocolVersion")
                     .toString()
                     .isEmpty());
}

TEST_F(McpServerTest, NotificationsGetNoResponse)
{
    auto response = this->send({
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
    });
    EXPECT_FALSE(response.has_value());
}

TEST_F(McpServerTest, UnknownMethod)
{
    auto response = this->send({
        {"jsonrpc", "2.0"},
        {"id", "abc"},
        {"method", "does/not/exist"},
    });
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->value("id").toString(), "abc");
    EXPECT_EQ(response->value("error").toObject().value("code").toInt(),
              -32601);
}

TEST_F(McpServerTest, ToolsList)
{
    auto response = this->send({
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/list"},
    });
    ASSERT_TRUE(response.has_value());
    QStringList names;
    for (const auto &tool :
         response->value("result").toObject().value("tools").toArray())
    {
        auto object = tool.toObject();
        names.append(object.value("name").toString());
        EXPECT_FALSE(object.value("description").toString().isEmpty());
        EXPECT_EQ(object.value("inputSchema").toObject().value("type"),
                  "object");
    }
    for (const auto *expected :
         {"list_config", "add_config_items", "update_config_items",
          "remove_config_items", "move_config_item", "list_backups",
          "create_backup", "restore_backup", "validate_filter",
          "filter_reference", "list_channels", "get_messages", "get_user",
          "test_filter", "simulate_config", "check_message"})
    {
        EXPECT_TRUE(names.contains(QString::fromLatin1(expected)))
            << expected;
    }
}

TEST_F(McpServerTest, ValidateFilter)
{
    auto ok = this->callTool("validate_filter", {{"filter", STREAMER_FILTER}});
    EXPECT_TRUE(ok.value("valid").toBool());
    EXPECT_EQ(ok.value("returnType").toString(), "Bool");

    auto bad = this->callTool("validate_filter",
                              {{"filter", "author.name == "}});
    EXPECT_FALSE(bad.value("valid").toBool());
    EXPECT_FALSE(bad.value("error").toString().isEmpty());
}

TEST_F(McpServerTest, ChangesCanBeTurnedOff)
{
    this->app->settings.mcpServerAllowChanges = false;
    auto result = this->callTool(
        "add_config_items",
        {{"section", "ignores"},
         {"items", QJsonArray{QJsonObject{{"pattern", "x"}}}}});
    EXPECT_TRUE(result.value("error").toString().contains("turned off"));
    EXPECT_TRUE(this->app->settings.ignoredMessages.raw().empty());
}

TEST_F(McpServerTest, ListHighlightsIncludesBuiltIns)
{
    auto result = this->callTool("list_config", {{"section", "highlights"}});
    auto items = result.value("items").toArray();
    ASSERT_FALSE(items.isEmpty());
    bool foundWhispers = false;
    for (const auto &item : items)
    {
        auto object = item.toObject();
        if (object.value("id").toString() == "whispers")
        {
            foundWhispers = true;
            EXPECT_TRUE(object.value("builtIn").toBool());
            EXPECT_EQ(object.value("value").toObject().value("id"),
                      "whispers");
        }
    }
    EXPECT_TRUE(foundWhispers);
}

TEST_F(McpServerTest, CheckMessageWithProposedFilterHighlight)
{
    QJsonObject highlight{
        {"type", "filter"},
        {"name", "streamer"},
        {"filterText", STREAMER_FILTER},
        {"showInMentions", true},
    };

    auto check = [&](const QString &user, const QString &text) {
        return this->callTool("check_message",
                              {{"user", user},
                               {"text", text},
                               {"add_highlights", QJsonArray{highlight}}});
    };

    auto normal = check("streamer", "hello chat");
    EXPECT_FALSE(normal.value("current")
                     .toObject()
                     .value("highlighted")
                     .toBool());
    auto proposed = normal.value("proposed").toObject();
    EXPECT_TRUE(proposed.value("highlighted").toBool());
    EXPECT_TRUE(proposed.value("showInMentions").toBool());
    EXPECT_EQ(proposed.value("matchedHighlights").toArray(),
              QJsonArray{"streamer"});

    auto command = check("streamer", "!discord");
    EXPECT_FALSE(
        command.value("proposed").toObject().value("highlighted").toBool());

    auto bot = check("streamer", "Now playing: some song");
    EXPECT_FALSE(bot.value("proposed").toObject().value("highlighted").toBool());

    auto otherUser = check("someone", "hello chat");
    EXPECT_FALSE(
        otherUser.value("proposed").toObject().value("highlighted").toBool());
}

TEST_F(McpServerTest, CheckMessageWithProposedIgnores)
{
    QJsonObject highlight{
        {"type", "filter"},
        {"filterText", R"(author.name == "streamer")"},
        {"showInMentions", true},
    };
    QJsonObject suppress{
        {"pattern", "giveaway"},
        {"isBlock", true},
        {"highlightOnly", true},
        {"suppressMentions", true},
        {"suppressHighlight", true},
        {"caseSensitive", false},
    };
    QJsonObject block{
        {"pattern", "badword"},
        {"isBlock", true},
        {"caseSensitive", false},
    };

    auto check = [&](const QString &text) {
        return this->callTool(
            "check_message",
            {{"user", "streamer"},
             {"text", text},
             {"add_highlights", QJsonArray{highlight}},
             {"add_ignores", QJsonArray{suppress, block}}})
            .value("proposed")
            .toObject();
    };

    auto suppressed = check("join the GIVEAWAY now");
    EXPECT_TRUE(suppressed.value("highlighted").toBool());
    EXPECT_FALSE(suppressed.value("showInMentions").toBool());
    EXPECT_TRUE(suppressed.value("color").isNull());
    EXPECT_EQ(suppressed.value("suppressedBy").toArray(),
              QJsonArray{"giveaway"});

    auto blocked = check("that is a badword");
    EXPECT_TRUE(blocked.value("blocked").toBool());

    auto plain = check("hello");
    EXPECT_TRUE(plain.value("highlighted").toBool());
    EXPECT_TRUE(plain.value("showInMentions").toBool());
    EXPECT_FALSE(plain.value("color").isNull());
}

TEST_F(McpServerTest, InvalidProposalIsRejected)
{
    auto result = this->callTool(
        "check_message",
        {{"user", "a"},
         {"text", "b"},
         {"add_highlights",
          QJsonArray{QJsonObject{{"type", "filter"},
                                 {"filterText", "author.name =="}}}}});
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(McpServerTest, Http)
{
    auto port = 40000 + static_cast<int>(QRandomGenerator::global()->bounded(
                            20000));
    this->app->settings.mcpServerPort = port;
    this->app->settings.mcpServerEnabled = true;
    auto token = this->app->settings.mcpServerToken.getValue().toUtf8();
    ASSERT_FALSE(token.isEmpty()) << "enabling the server creates a token";
    auto auth = "Authorization: Bearer " + token + "\r\n";

    QByteArray initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})";

    // No token
    EXPECT_TRUE(httpRequest(port, post(initialize, {}))
                    .startsWith("HTTP/1.1 401"));
    // Wrong token
    EXPECT_TRUE(httpRequest(port, post(initialize,
                                       "Authorization: Bearer nope\r\n"))
                    .startsWith("HTTP/1.1 401"));
    // A website
    EXPECT_TRUE(
        httpRequest(port, post(initialize, auth + "Origin: https://evil.com\r\n"))
            .startsWith("HTTP/1.1 403"));
    // GET (no server-sent event stream)
    EXPECT_TRUE(httpRequest(port, "GET /mcp HTTP/1.1\r\n" + auth + "\r\n")
                    .startsWith("HTTP/1.1 405"));
    // Wrong path
    EXPECT_TRUE(httpRequest(port, "POST /other HTTP/1.1\r\n" + auth +
                                      "Content-Length: 0\r\n\r\n")
                    .startsWith("HTTP/1.1 404"));

    // Working request
    auto response = httpRequest(
        port, post(initialize, auth + "Origin: http://localhost:1234\r\n"));
    ASSERT_TRUE(response.startsWith("HTTP/1.1 200")) << response.toStdString();
    auto body = response.mid(response.indexOf("\r\n\r\n") + 4);
    auto doc = QJsonDocument::fromJson(body).object();
    EXPECT_EQ(doc.value("result").toObject().value("protocolVersion"),
              "2025-06-18");

    // Notification
    EXPECT_TRUE(
        httpRequest(port,
                    post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                         auth))
            .startsWith("HTTP/1.1 202"));

    // Chunked body
    QByteArray list = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";
    QByteArray chunked = "POST /mcp HTTP/1.1\r\n" + auth +
                         "Transfer-Encoding: chunked\r\n\r\n" +
                         QByteArray::number(10, 16) + "\r\n" + list.left(10) +
                         "\r\n" + QByteArray::number(list.size() - 10, 16) +
                         "\r\n" + list.mid(10) + "\r\n0\r\n\r\n";
    response = httpRequest(port, chunked);
    ASSERT_TRUE(response.startsWith("HTTP/1.1 200")) << response.toStdString();
    body = response.mid(response.indexOf("\r\n\r\n") + 4);
    EXPECT_FALSE(QJsonDocument::fromJson(body)
                     .object()
                     .value("result")
                     .toObject()
                     .value("tools")
                     .toArray()
                     .isEmpty());

    this->app->settings.mcpServerEnabled = false;
}
