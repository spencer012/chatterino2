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
    qCDebug(chatterinoApp)
        << "ChatHistoryManager primary history path:" << this->historyFilePath_;
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

QString ChatHistoryManager::normalizeChannelName(const QString &channelName)
{
    auto key = channelName.trimmed();
    if (key.isEmpty())
    {
        return {};
    }

    auto colonIndex = key.indexOf(':');
    if (colonIndex != -1)
    {
        auto platform = key.left(colonIndex).toLower();
        auto channel = key.mid(colonIndex + 1).trimmed();
        if (channel.startsWith('#'))
        {
            channel.remove(0, 1);
        }
        return platform + ':' + channel.toLower();
    }

    if (key.startsWith('#'))
    {
        key.remove(0, 1);
    }
    return key.toLower();
}

void ChatHistoryManager::addMessage(const QString &channelName,
                                    const QString &message)
{
    auto normalizedChannelName = normalizeChannelName(channelName);
    qCDebug(chatterinoApp)
        << "ChatHistoryManager::addMessage - raw channel:" << channelName
        << "normalized:" << normalizedChannelName;
    if (normalizedChannelName.isEmpty() || message.trimmed().isEmpty())
    {
        qCDebug(chatterinoApp)
            << "ChatHistoryManager::addMessage - empty channel or message";
        return;
    }

    auto &messages = this->history_[normalizedChannelName];

    // Don't add consecutive duplicates
    if (!messages.isEmpty() && messages.last() == message)
    {
        qCDebug(chatterinoApp)
            << "ChatHistoryManager::addMessage - duplicate message, skipping";
        return;
    }

    messages.append(message);
    qCDebug(chatterinoApp) << "ChatHistoryManager::addMessage - added message"
                           << "to channel" << normalizedChannelName << ", total:"
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
    auto normalizedChannelName = normalizeChannelName(channelName);
    auto messages = this->history_.value(normalizedChannelName);
    qCDebug(chatterinoApp) << "ChatHistoryManager::getMessages - channel:"
                           << channelName << "normalized:" << normalizedChannelName
                           << "returning" << messages.size()
                           << "messages";
    if (messages.isEmpty())
    {
        qCDebug(chatterinoApp)
            << "ChatHistoryManager::getMessages - available channels:"
            << this->history_.keys();
    }
    return messages;
}

QStringList ChatHistoryManager::getFiltered(const QString &channelName,
                                            const QString &searchText) const
{
    auto normalizedChannelName = normalizeChannelName(channelName);
    const auto &messages = this->history_.value(normalizedChannelName);

    qCDebug(chatterinoApp) << "ChatHistoryManager::getFiltered - channel:"
                           << channelName << "normalized:" << normalizedChannelName
                           << "searchText:" << searchText
                           << "total messages:" << messages.size();
    if (messages.isEmpty())
    {
        qCDebug(chatterinoApp)
            << "ChatHistoryManager::getFiltered - available channels:"
            << this->history_.keys();
    }

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
    this->history_.clear();

    qCDebug(chatterinoApp)
        << "ChatHistoryManager::load - loading from:" << this->historyFilePath_;

    auto loaded = this->loadFromFile(this->historyFilePath_);

    if (!loaded)
    {
        qCDebug(chatterinoApp)
            << "No chat history loaded. Primary path:" << this->historyFilePath_;
    }

    qCDebug(chatterinoApp)
        << "Loaded chat history for" << this->history_.size() << "channels";
}

bool ChatHistoryManager::loadFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.exists())
    {
        qCDebug(chatterinoApp) << "Chat history file does not exist:" << filePath;
        return false;
    }

    qCDebug(chatterinoApp)
        << "ChatHistoryManager::loadFromFile - file exists:" << filePath
        << "size:" << file.size();

    if (!file.open(QIODevice::ReadOnly))
    {
        qCWarning(chatterinoApp)
            << "Failed to open chat history file for reading:"
            << filePath;
        return false;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
    {
        qCWarning(chatterinoApp) << "Invalid chat history file format:" << filePath;
        return false;
    }

    QJsonObject root = doc.object();
    int version = root["version"].toInt(0);
    if (version != 1)
    {
        qCWarning(chatterinoApp)
            << "Unknown chat history file version:" << version << "in" << filePath;
        return false;
    }

    qCDebug(chatterinoApp) << "Loading chat history from:" << filePath;

    QJsonObject channels = root["channels"].toObject();
    qCDebug(chatterinoApp)
        << "ChatHistoryManager::loadFromFile - raw channel count:"
        << channels.size();
    for (auto it = channels.begin(); it != channels.end(); ++it)
    {
        auto channelName = normalizeChannelName(it.key());
        if (channelName.isEmpty())
        {
            continue;
        }

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

        auto &existingMessages = this->history_[channelName];
        if (existingMessages.isEmpty())
        {
            existingMessages = messages;
        }
        else
        {
            for (const auto &msg : messages)
            {
                if (existingMessages.isEmpty() || existingMessages.last() != msg)
                {
                    existingMessages.append(msg);
                }
            }
            while (existingMessages.size() > MAX_HISTORY_PER_CHANNEL)
            {
                existingMessages.removeFirst();
            }
        }

        qCDebug(chatterinoApp)
            << "  Loaded" << messages.size() << "messages for channel:"
            << channelName;
    }

    return true;
}

}  // namespace chatterino
