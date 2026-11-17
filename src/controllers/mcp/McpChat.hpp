// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>

#include <memory>
#include <vector>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;

namespace mcp {

/// Finds a loaded channel by name. Besides Twitch channel names this accepts
/// "/mentions", "/whispers", "/automod" and "/live". Returns nullptr if the
/// channel isn't open.
ChannelPtr findChannel(const QString &name);

/// Returns the channels to look at for `name`, or every open Twitch channel
/// when `name` is empty
std::vector<ChannelPtr> channelsFor(const QString &name);

/// Whether this is a chat message sent by a user (not a system message)
bool isUserMessage(const Message &message);

QJsonObject messageToJson(const Message &message);

/// One line per message, much smaller than messageToJson:
/// "2026-11-17T12:56:08 #channel login [flag,flag]: text"
QString messageToLine(const Message &message);

/// Schema for the "format" argument of tools that return messages
QJsonObject messageFormatSchema();

/// Whether the "format" argument asks for one line per message
bool wantsLines(const QJsonObject &args);

/// messageToLine or messageToJson, depending on `lines`
QJsonValue formatMessage(const Message &message, bool lines);

}  // namespace mcp

}  // namespace chatterino
