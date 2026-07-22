// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace chatterino {

class Paths;

/// @brief Manages per-channel chat message history with persistence
///
/// ChatHistoryManager stores the history of sent messages for each channel,
/// allowing users to search and recall previously sent messages using
/// terminal-style reverse search (Ctrl+R).
class ChatHistoryManager
{
public:
    explicit ChatHistoryManager(const Paths &paths);
    ~ChatHistoryManager();

    ChatHistoryManager(const ChatHistoryManager &) = delete;
    ChatHistoryManager(ChatHistoryManager &&) = delete;
    ChatHistoryManager &operator=(const ChatHistoryManager &) = delete;
    ChatHistoryManager &operator=(ChatHistoryManager &&) = delete;

    /// Maximum number of messages to store per channel
    static constexpr int MAX_HISTORY_PER_CHANNEL = 5000;

    /// @brief Adds a message to the history for the given channel
    /// @param channelName The channel identifier (e.g., "twitch:pajlada")
    /// @param message The message text to add
    /// @note Consecutive duplicate messages are not added
    void addMessage(const QString &channelName, const QString &message);

    /// @brief Gets all messages for a channel
    /// @param channelName The channel identifier
    /// @return List of messages, most recent last
    QStringList getMessages(const QString &channelName) const;

    /// @brief Gets messages filtered by search text
    /// @param channelName The channel identifier
    /// @param searchText Text to search for (case-insensitive)
    /// @return List of matching messages, most recent last
    QStringList getFiltered(const QString &channelName,
                            const QString &searchText) const;

    /// @brief Saves the history to disk
    void save();

    /// @brief Loads the history from disk
    void load();

private:
    /// Channel name -> list of messages (most recent last)
    QHash<QString, QStringList> history_;

    /// Path to the history file
    QString historyFilePath_;

    /// Periodic save timer (every 60 seconds)
    QTimer saveTimer_;

    /// Debounced save timer (5 seconds after last message)
    QTimer debouncedSaveTimer_;

    /// Whether the debounced save timer is connected
    bool debouncedSaveConnected_{false};
};

}  // namespace chatterino
