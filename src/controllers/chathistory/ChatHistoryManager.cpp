// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/chathistory/ChatHistoryManager.hpp"

#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace chatterino {

ChatHistoryManager::ChatHistoryManager(const Paths &paths)
    : historyFilePath_(combinePath(paths.miscDirectory, "chat-history.json"))
{
    this->load();

    // Set up periodic save timer (every 60 seconds)
    this->saveTimer_.setInterval(60 * 1000);
    QObject::connect(&this->saveTimer_, &QTimer::timeout, [this]() {
        this->save();
    });
    this->saveTimer_.start();
}

ChatHistoryManager::~ChatHistoryManager()
{
    this->save();
}

void ChatHistoryManager::addMessage(const QString &channelName,
                                    const QString &message)
{
    if (channelName.isEmpty() || message.trimmed().isEmpty())
    {
        qCDebug(chatterinoApp)
            << "ChatHistoryManager::addMessage - empty channel or message";
        return;
    }

    auto &messages = this->history_[channelName];

    // Don't add consecutive duplicates
    if (!messages.isEmpty() && messages.last() == message)
    {
        qCDebug(chatterinoApp)
            << "ChatHistoryManager::addMessage - duplicate message, skipping";
        return;
    }

    messages.append(message);
    qCDebug(chatterinoApp) << "ChatHistoryManager::addMessage - added message"
                           << "to channel" << channelName << ", total:"
                           << messages.size();

    // Enforce maximum history size
    while (messages.size() > MAX_HISTORY_PER_CHANNEL)
    {
        messages.removeFirst();
    }

    // Schedule a debounced save (5 seconds after last message)
    this->debouncedSaveTimer_.stop();
    this->debouncedSaveTimer_.setSingleShot(true);
    this->debouncedSaveTimer_.setInterval(5000);
    if (!this->debouncedSaveConnected_)
    {
        QObject::connect(&this->debouncedSaveTimer_, &QTimer::timeout, [this]() {
            this->save();
        });
        this->debouncedSaveConnected_ = true;
    }
    this->debouncedSaveTimer_.start();
}

QStringList ChatHistoryManager::getMessages(const QString &channelName) const
{
    auto messages = this->history_.value(channelName);
    qCDebug(chatterinoApp) << "ChatHistoryManager::getMessages - channel:"
                           << channelName << "returning" << messages.size()
                           << "messages";
    return messages;
}

QStringList ChatHistoryManager::getFiltered(const QString &channelName,
                                            const QString &searchText) const
{
    const auto &messages = this->history_.value(channelName);

    qCDebug(chatterinoApp) << "ChatHistoryManager::getFiltered - channel:"
                           << channelName << "searchText:" << searchText
                           << "total messages:" << messages.size();

    if (searchText.isEmpty())
    {
        return messages;
    }

    // Filter and deduplicate - keep only the most recent occurrence of each message
    QStringList filtered;
    QSet<QString> seen;

    // Iterate in reverse to keep most recent, then reverse the result
    for (int i = messages.size() - 1; i >= 0; --i)
    {
        const auto &msg = messages[i];
        bool matches = msg.contains(searchText, Qt::CaseInsensitive);
        if (matches && !seen.contains(msg))
        {
            filtered.prepend(msg);
            seen.insert(msg);
            qCDebug(chatterinoApp)
                << "  Match:" << msg.left(50) << (msg.length() > 50 ? "..." : "");
        }
    }

    qCDebug(chatterinoApp) << "ChatHistoryManager::getFiltered - returning"
                           << filtered.size() << "results";
    return filtered;
}

void ChatHistoryManager::save()
{
    qCDebug(chatterinoApp) << "ChatHistoryManager::save() called, saving to"
                           << this->historyFilePath_;

    QJsonObject root;
    root["version"] = 1;

    QJsonObject channels;
    int totalMessages = 0;
    for (auto it = this->history_.constBegin(); it != this->history_.constEnd();
         ++it)
    {
        QJsonArray messagesArray;
        for (const auto &msg : it.value())
        {
            messagesArray.append(msg);
            totalMessages++;
        }
        channels[it.key()] = messagesArray;
    }
    root["channels"] = channels;

    qCDebug(chatterinoApp) << "Saving" << totalMessages << "messages across"
                           << this->history_.size() << "channels";

    QJsonDocument doc(root);

    QSaveFile file(this->historyFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        qCWarning(chatterinoApp)
            << "Failed to open chat history file for writing:"
            << this->historyFilePath_;
        return;
    }

    file.write(doc.toJson(QJsonDocument::Compact));
    if (!file.commit())
    {
        qCWarning(chatterinoApp) << "Failed to save chat history file:"
                                 << this->historyFilePath_;
    }
    else
    {
        qCDebug(chatterinoApp) << "Successfully saved chat history";
    }
}

void ChatHistoryManager::load()
{
    QFile file(this->historyFilePath_);
    if (!file.exists())
    {
        return;
    }

    if (!file.open(QIODevice::ReadOnly))
    {
        qCWarning(chatterinoApp)
            << "Failed to open chat history file for reading:"
            << this->historyFilePath_;
        return;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
    {
        qCWarning(chatterinoApp) << "Invalid chat history file format";
        return;
    }

    QJsonObject root = doc.object();
    int version = root["version"].toInt(0);
    if (version != 1)
    {
        qCWarning(chatterinoApp)
            << "Unknown chat history file version:" << version;
        return;
    }

    QJsonObject channels = root["channels"].toObject();
    for (auto it = channels.begin(); it != channels.end(); ++it)
    {
        QStringList messages;
        QJsonArray messagesArray = it.value().toArray();
        for (const auto &msgVal : messagesArray)
        {
            QString msg = msgVal.toString();
            if (!msg.isEmpty())
            {
                messages.append(msg);
            }
        }

        // Enforce maximum when loading
        while (messages.size() > MAX_HISTORY_PER_CHANNEL)
        {
            messages.removeFirst();
        }

        this->history_[it.key()] = messages;
        qCDebug(chatterinoApp)
            << "  Loaded" << messages.size() << "messages for channel:"
            << it.key();
    }

    qCDebug(chatterinoApp)
        << "Loaded chat history for" << this->history_.size() << "channels";
}

}  // namespace chatterino
