// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/crowdcopy/CrowdCopyEngine.hpp"

#include "messages/MessageFlag.hpp"
#include "singletons/Settings.hpp"

#include <QRandomGenerator>
#include <QTime>

#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {

using namespace chatterino;

bool shouldIgnoreMessage(const MessagePtr &message)
{
    if (message->flags.hasAny({MessageFlag::System, MessageFlag::Timeout,
                               MessageFlag::Subscription,
                               MessageFlag::ModerationAction, MessageFlag::Whisper,
                               MessageFlag::AutoMod, MessageFlag::ClearChat}))
    {
        return true;
    }

    return false;
}

double messageAgeSeconds(const MessagePtr &message, const QDateTime &now)
{
    if (message->serverReceivedTime.isValid())
    {
        return message->serverReceivedTime.msecsTo(now) / 1000.0;
    }

    if (message->parseTime.isValid())
    {
        int seconds = message->parseTime.secsTo(QTime::currentTime());
        // Crossing midnight yields a negative delta, normalize to same-day age.
        if (seconds < 0)
        {
            seconds += 24 * 60 * 60;
        }
        return static_cast<double>(seconds);
    }

    return std::numeric_limits<double>::infinity();
}

double decayWeight(double ageSeconds, double halfLifeSeconds)
{
    if (halfLifeSeconds <= 0.0)
    {
        return 1.0;
    }

    constexpr double ln2 = 0.6931471805599453;
    return std::exp(-ln2 * ageSeconds / halfLifeSeconds);
}

}  // namespace

namespace chatterino {

QString CrowdCopyEngine::normalizeText(const QString &text)
{
    QString out;
    out.reserve(text.size());

    for (const auto ch : text)
    {
        // Drop format characters and selected invisible controls commonly seen in copied chat text.
        if (ch.category() == QChar::Other_Format || ch == QChar(0x00AD) ||
            ch == QChar(0x180E))
        {
            continue;
        }
        out.append(ch);
    }

    return out.trimmed();
}

CrowdCopyResult CrowdCopyEngine::evaluate(const std::vector<MessagePtr> &messages,
                                          const QDateTime &now)
{
    const auto *settings = getSettings();
    const auto maxAgeSeconds = settings->crowdCopyTimeWindowSec.getValue();
    const auto halfLifeSeconds =
        static_cast<double>(settings->crowdCopyDecayHalfLifeSec.getValue());
    const auto minimumUsers = settings->crowdCopyMinUsers.getValue();
    const auto minimumRatio =
        static_cast<double>(settings->crowdCopyMinRatio.getValue());

    struct WeightedMessage {
        QString text;
        QString caseInsensitiveKey;
        double weight;
    };

    std::vector<WeightedMessage> weightedMessages;
    weightedMessages.reserve(messages.size());

    // Deduplicate by user by scanning from newest to oldest and only taking a user's latest message.
    std::unordered_set<QString> seenUsers;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        const auto &message = *it;
        if (!message || shouldIgnoreMessage(message))
        {
            continue;
        }

        const auto ageSeconds = messageAgeSeconds(message, now);
        if (ageSeconds < 0.0 || ageSeconds > static_cast<double>(maxAgeSeconds))
        {
            continue;
        }

        auto userKey = message->loginName.trimmed().toLower();
        if (userKey.isEmpty())
        {
            userKey = message->displayName.trimmed().toLower();
        }
        if (userKey.isEmpty())
        {
            continue;
        }

        if (seenUsers.contains(userKey))
        {
            continue;
        }
        seenUsers.insert(userKey);

        const auto normalizedText = normalizeText(message->messageText);
        if (normalizedText.isEmpty())
        {
            continue;
        }

        weightedMessages.push_back(
            {.text = normalizedText,
             .caseInsensitiveKey = normalizedText.toCaseFolded(),
             .weight = decayWeight(ageSeconds, halfLifeSeconds)});
    }

    if (weightedMessages.empty())
    {
        return {};
    }

    struct VariantStats {
        double score{0.0};
        int users{0};
    };

    struct GroupStats {
        double score{0.0};
        int users{0};
        std::unordered_map<QString, VariantStats> variants;
    };

    std::unordered_map<QString, GroupStats> grouped;
    grouped.reserve(weightedMessages.size());

    double totalScore = 0.0;
    for (const auto &msg : weightedMessages)
    {
        auto &group = grouped[msg.caseInsensitiveKey];
        group.score += msg.weight;
        group.users += 1;
        auto &variant = group.variants[msg.text];
        variant.score += msg.weight;
        variant.users += 1;
        totalScore += msg.weight;
    }

    QString bestKey;
    GroupStats bestStats;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (const auto &[key, stats] : grouped)
    {
        if (stats.score > bestScore)
        {
            bestScore = stats.score;
            bestKey = key;
            bestStats = stats;
        }
    }

    if (bestKey.isEmpty())
    {
        return {};
    }

    if (bestStats.users < minimumUsers)
    {
        return {};
    }

    if (totalScore <= 0.0 || bestStats.score < totalScore * minimumRatio)
    {
        return {};
    }

    QString chosenText;
    VariantStats chosenVariant;
    bool hasChosen = false;
    for (const auto &[variantText, stats] : bestStats.variants)
    {
        if (!hasChosen || stats.users > chosenVariant.users ||
            (stats.users == chosenVariant.users &&
             stats.score > chosenVariant.score))
        {
            chosenText = variantText;
            chosenVariant = stats;
            hasChosen = true;
            continue;
        }

        if (stats.users == chosenVariant.users &&
            qFuzzyCompare(stats.score + 1.0, chosenVariant.score + 1.0))
        {
            if (QRandomGenerator::global()->bounded(2) == 0)
            {
                chosenText = variantText;
                chosenVariant = stats;
            }
        }
    }

    if (chosenText.isEmpty())
    {
        return {};
    }

    return {
        .text = chosenText,
        .userCount = bestStats.users,
    };
}

}  // namespace chatterino
