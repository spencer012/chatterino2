// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/mcp/McpTool.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

class QTcpServer;

namespace chatterino {

class Settings;

namespace mcp {

/// A Model Context Protocol server ("Streamable HTTP" transport) that lets an
/// agent read and change highlights, ignores, filters and commands, and query
/// the chat that is loaded in memory.
///
/// It only listens on 127.0.0.1 and requires the bearer token from the
/// settings. Everything runs on the GUI thread.
class McpServer : public QObject
{
public:
    explicit McpServer(Settings &settings);
    ~McpServer() override;

    McpServer(const McpServer &) = delete;
    McpServer(McpServer &&) = delete;
    McpServer &operator=(const McpServer &) = delete;
    McpServer &operator=(McpServer &&) = delete;

    /// Handles one JSON-RPC message. `done` gets the response, or nothing for
    /// notifications.
    ///
    /// Public so it can be tested without a socket.
    void handleMessage(const QJsonObject &message,
                       std::function<void(std::optional<QJsonObject>)> done);

    /// Returns the URL clients connect to, e.g. http://127.0.0.1:38777/mcp
    static QString url(int port);

    /// Creates a new random token
    static QString generateToken();

private:
    void restart();
    void onNewConnection();

    void callTool(const QJsonObject &params,
                  std::function<void(QJsonObject)> done);

    Settings &settings_;
    QTcpServer *server_ = nullptr;
    std::vector<Tool> tools_;
    pajlada::Signals::SignalHolder connections_;
};

}  // namespace mcp

}  // namespace chatterino
