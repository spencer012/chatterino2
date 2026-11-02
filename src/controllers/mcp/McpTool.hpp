// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

namespace chatterino::mcp {

/// The outcome of a tool call. Either `data` is returned to the agent as JSON,
/// or `error` is returned as a tool error the agent can read and react to.
struct ToolResult {
    QJsonValue data;
    QString error;

    static ToolResult ok(QJsonValue data)
    {
        return {.data = std::move(data), .error = {}};
    }

    static ToolResult fail(QString error)
    {
        return {.data = {}, .error = std::move(error)};
    }
};

using ToolDone = std::function<void(ToolResult)>;

struct Tool {
    QString name;
    QString description;
    QJsonObject inputSchema;

    /// Whether the tool changes settings. These are refused while
    /// "Allow changes" is off.
    bool changesSettings = false;

    /// Runs on the GUI thread. `done` must be called exactly once, either
    /// right away or later (e.g. after a network request).
    std::function<void(const QJsonObject &args, ToolDone done)> run;
};

/// Helpers to build JSON schemas for tool inputs
namespace schema {

QJsonObject object(const QJsonObject &properties,
                   const QStringList &required = {});
QJsonObject string(const QString &description);
QJsonObject stringEnum(const QString &description, const QStringList &values);
QJsonObject integer(const QString &description);
QJsonObject boolean(const QString &description);
QJsonObject array(const QString &description, const QJsonObject &items);
QJsonObject anyObject(const QString &description);

}  // namespace schema

/// Helpers to read tool arguments
namespace args {

QString string(const QJsonObject &args, const QString &key,
               const QString &fallback = {});
int integer(const QJsonObject &args, const QString &key, int fallback);
bool boolean(const QJsonObject &args, const QString &key, bool fallback);

}  // namespace args

std::vector<Tool> makeConfigTools();
std::vector<Tool> makeChatTools();
std::vector<Tool> makeSimulationTools();

}  // namespace chatterino::mcp
