// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/RapidjsonHelpers.hpp"

#include <pajlada/serialize.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <rapidjson/document.h>

#include <optional>

namespace chatterino::mcp {

/// Converts a settings item to the JSON it's stored as in settings.json
template <typename T>
QJsonObject toJson(const T &item)
{
    rapidjson::Document doc;
    auto value = pajlada::Serialize<T>::get(item, doc.GetAllocator());
    return QJsonDocument::fromJson(rj::stringify(value).toUtf8()).object();
}

/// Reads a settings item from the JSON it's stored as in settings.json
template <typename T>
std::optional<T> fromJson(const QJsonObject &object)
{
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    rapidjson::Document doc;
    doc.Parse(bytes.constData(), static_cast<size_t>(bytes.size()));
    if (doc.HasParseError())
    {
        return std::nullopt;
    }
    bool error = false;
    auto value = pajlada::Deserialize<T>::get(doc, &error);
    if (error)
    {
        return std::nullopt;
    }
    return value;
}

}  // namespace chatterino::mcp
