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

}  // namespace mcp

}  // namespace chatterino
