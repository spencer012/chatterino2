// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>

#include <map>
#include <vector>

namespace chatterino {

class IgnorePhrase;
struct TwitchSpecialOccurrence;

enum class ShowIgnoredUsersMessages { Never, IfModerator, IfBroadcaster };

struct IgnoredMessageParameters {
    QString message;

    QString twitchUserID;
    QString twitchUserLogin;
    bool isMod;
    bool isBroadcaster;

    /// When set, a message hidden by an ignore phrase is counted in
    /// ignoreBlockStats() under this channel and sender
    QString statsChannel = {};
    QString statsSender = {};
};

/// What one blocking ignore phrase has hidden since Chatterino started
struct IgnoreBlockStats {
    qint64 count = 0;
    QDateTime last;
    QString lastChannel;
    QString lastSender;
    QString lastText;
    /// Hidden messages per channel and per sender (each capped, the rest is
    /// counted under an empty key)
    std::map<QString, qint64> channels;
    std::map<QString, qint64> senders;
};

/// Blocking statistics by ignore phrase pattern. Only chat messages are
/// counted. Used by the MCP server so an agent can tell whose messages an
/// ignore phrase hides, which it can't see otherwise because hidden
/// messages are never stored.
std::map<QString, IgnoreBlockStats> ignoreBlockStats();

struct IgnoredHighlightsResult {
    bool suppressMentions = false;
    bool suppressHighlight = false;

    bool hasMatch() const
    {
        return this->suppressMentions || this->suppressHighlight;
    }
};

bool isIgnoredMessage(IgnoredMessageParameters &&params);
IgnoredHighlightsResult checkIgnoredHighlights(const QString &message);

/// @brief Processes replacement ignore-phrases for a message
///
/// @param phrases A list of IgnorePhrases to process. Block phrases as well as
/// 	           invalid phrases are ignored.
/// @param content The message text. This gets altered by replacements.
/// @param twitchSpecials A list of special items present in the message. Occurrences
///                     that have been removed from the message will also be
///                     removed in this list. Similarly, if new emotes are added
///                     from a replacement, this list gets updated as well.
void processIgnorePhrases(const std::vector<IgnorePhrase> &phrases,
                          QString &content,
                          std::vector<TwitchSpecialOccurrence> &twitchSpecials);

}  // namespace chatterino
