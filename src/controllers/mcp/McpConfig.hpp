// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/highlights/types/AnyHighlight.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <variant>

namespace chatterino::mcp {

/// Saves the highlights, ignores, filters, commands, highlight blacklist and
/// nicknames to a new backup file. Returns its id, or sets `error`.
QString saveBackup(const QString &reason, QString &error);

/// Returns the stored JSON of every item in the given settings list
/// (e.g. "highlights", "ignores")
QJsonArray currentSectionValues(const QString &section);

/// Reads and validates a highlight the same way add_config_items does.
/// `previous` is the highlight it replaces, if any.
std::variant<highlights::AnyHighlight, QString> parseHighlight(
    const QJsonObject &value, const highlights::AnyHighlight *previous);

/// Reads and validates an ignore phrase the same way add_config_items does
std::variant<IgnorePhrase, QString> parseIgnorePhrase(const QJsonObject &value);

}  // namespace chatterino::mcp
