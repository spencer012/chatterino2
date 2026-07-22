// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsClient.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace chatterino {

namespace {

int parseRewardCost(const QJsonObject &reward)
{
    const auto directCost = reward.value("cost");
    if (directCost.isDouble())
    {
        return directCost.toInt();
    }

    const auto minimumCost = reward.value("minimumCost");
    if (minimumCost.isDouble())
    {
        return minimumCost.toInt();
    }

    const auto bitsCost = reward.value("bitsCost");
    if (bitsCost.isDouble())
    {
        return bitsCost.toInt();
    }

    const auto defaultBitsCost = reward.value("defaultBitsCost");
    if (defaultBitsCost.isDouble())
    {
        return defaultBitsCost.toInt();
    }

    return 0;
}

QString parseRewardTitle(const QJsonObject &reward)
{
    const auto title = reward.value("title").toString();
    if (!title.isEmpty())
    {
        return title;
    }

    return reward.value("type").toString();
}

bool parseRewardFlag(const QJsonObject &reward, const char *camelCase,
                     const char *snakeCase, bool defaultValue = false)
{
    const auto camel = reward.value(camelCase);
    if (!camel.isUndefined() && !camel.isNull())
    {
        return camel.toBool(defaultValue);
    }

    const auto snake = reward.value(snakeCase);
    if (!snake.isUndefined() && !snake.isNull())
    {
        return snake.toBool(defaultValue);
    }

    return defaultValue;
}

QString parseRewardString(const QJsonObject &reward, const char *camelCase,
                          const char *snakeCase)
{
    const auto camel = reward.value(camelCase).toString();
    if (!camel.isEmpty())
    {
        return camel;
    }

    return reward.value(snakeCase).toString();
}

QByteArray buildRequest(const QJsonObject &payload)
{
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

int parseNestedInt(const QJsonObject &parent, const char *key, const char *nested)
{
    const auto object = parent.value(key).toObject();
    return object.value(nested).toInt();
}

bool parseNestedEnabled(const QJsonObject &parent, const char *key)
{
    return parent.value(key).toObject().value("isEnabled").toBool(false);
}

}  // namespace

QByteArray ChannelPointsClient::buildReplaceSubscriptionsRequest(
    const QString &requestId, const QStringList &channels)
{
    QJsonArray channelArray;
    for (const auto &channel : channels)
    {
        channelArray.append(channel);
    }

    return buildRequest({
        {"action", "replace_subscriptions"},
        {"requestId", requestId},
        {"channels", channelArray},
    });
}

QByteArray ChannelPointsClient::buildGetRewardsRequest(
    const QString &requestId, const QString &channelLogin)
{
    return buildRequest({
        {"action", "get_rewards"},
        {"requestId", requestId},
        {"channelLogin", channelLogin},
    });
}

QByteArray ChannelPointsClient::buildGetChannelPointsRequest(
    const QString &requestId, const QString &channelLogin)
{
    return buildRequest({
        {"action", "get_channel_points"},
        {"requestId", requestId},
        {"channelLogin", channelLogin},
    });
}

QByteArray ChannelPointsClient::buildRedeemRequest(
    const QString &requestId, const QString &channelLogin,
    const ChannelPointRewardData &reward, const QString &redeemInput,
    const QString &transactionId)
{
    QJsonObject payload{
        {"action", "redeem"},
        {"requestId", requestId},
        {"channelLogin", channelLogin},
        {"rewardId", reward.id},
        {"title", reward.title},
        {"cost", reward.cost},
        {"prompt", redeemInput},
    };

    if (!transactionId.isEmpty())
    {
        payload.insert("transactionId", transactionId);
    }

    return buildRequest(payload);
}

std::optional<ChannelPointsClient::ParsedMessage>
ChannelPointsClient::parseMessage(const QByteArray &payload, QString &error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = parseError.errorString();
        return std::nullopt;
    }

    const auto root = document.object();
    ParsedMessage message{
        .type = root.value("type").toString(),
        .requestId = root.value("requestId").toString(),
        .channelLogin = root.value("channelLogin").toString(),
        .channelId = root.value("channelId").toString(),
        .event = root.value("event").toString(),
        .errorCode = root.value("code").toString(),
        .errorMessage = root.value("message").toString(),
        .root = root,
    };

    if (message.type == "connected")
    {
        message.kind = MessageKind::Connected;
    }
    else if (message.type == "error")
    {
        message.kind = MessageKind::Error;
    }
    else if (message.type == "event")
    {
        message.kind = MessageKind::Event;
    }
    else if (message.type == "channel_points_snapshot")
    {
        message.kind = MessageKind::ChannelPointsSnapshot;
    }
    else if (message.type == "rewards_snapshot")
    {
        message.kind = MessageKind::RewardsSnapshot;
    }
    else if (message.type == "redeem_result")
    {
        message.kind = MessageKind::RedeemResult;
    }
    else if (message.type == "subscribed")
    {
        message.kind = MessageKind::Subscribed;
    }
    else if (message.type == "unsubscribed")
    {
        message.kind = MessageKind::Unsubscribed;
    }
    else if (message.type == "subscriptions_replaced")
    {
        message.kind = MessageKind::SubscriptionsReplaced;
    }

    return message;
}

QVector<ChannelPointRewardData> ChannelPointsClient::parseRewards(
    const QJsonArray &items, bool customRewards)
{
    QVector<ChannelPointRewardData> rewards;
    rewards.reserve(items.size());

    for (const auto &value : items)
    {
        if (!value.isObject())
        {
            continue;
        }

        const auto reward = value.toObject();
        const auto cooldownExpiresAt =
            parseRewardString(reward, "cooldownExpiresAt", "cooldown_expires_at");
        const auto title = parseRewardTitle(reward);
        const auto prompt = reward.value("prompt").isNull()
                                ? QString()
                                : reward.value("prompt").toString();

        auto cost = parseRewardCost(reward);
        auto enabled = parseRewardFlag(reward, "isEnabled", "is_enabled", true);
        auto paused = parseRewardFlag(reward, "isPaused", "is_paused", false);
        auto inStock = parseRewardFlag(reward, "isInStock", "is_in_stock", true);
        const auto hasGlobalCooldown =
            parseNestedEnabled(reward, "globalCooldownSetting");
        const auto hasMaxPerStream =
            parseNestedEnabled(reward, "maxPerStreamSetting");

        rewards.push_back(ChannelPointRewardData{
            .id = reward.value("id").toString(),
            .title = title,
            .prompt = prompt,
            .backgroundColor = reward.value("backgroundColor").toString(),
            .cooldownExpiresAt = cooldownExpiresAt,
            .cost = cost,
            .globalCooldownSeconds = parseNestedInt(
                reward, "globalCooldownSetting", "globalCooldownSeconds"),
            .maxPerStream =
                parseNestedInt(reward, "maxPerStreamSetting", "maxPerStream"),
            .isCustom = customRewards,
            .isEnabled = enabled,
            .isPaused = paused,
            .isInStock = inStock,
            .isUserInputRequired = parseRewardFlag(
                reward, "isUserInputRequired", "is_user_input_required", false),
            .isSubOnly =
                parseRewardFlag(reward, "isSubOnly", "is_sub_only", false),
            .isRedeemable = cost > 0,
            .hasGlobalCooldown = hasGlobalCooldown,
            .hasMaxPerStream = hasMaxPerStream,
            .isGloballyLimited = hasGlobalCooldown || hasMaxPerStream,
        });
    }

    return rewards;
}

}  // namespace chatterino
