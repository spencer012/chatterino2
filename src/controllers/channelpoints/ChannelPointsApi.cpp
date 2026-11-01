// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsApi.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

namespace chatterino::channelpoints {

namespace {

const QString CONTEXT_HASH = QStringLiteral(
    "7fe050e3761eb2cf258d70ee1a21cbd76fa8cf3d7e7b12fc437e7029d446b5e3");
const QString REDEEM_HASH = QStringLiteral(
    "d56249a7adb4978898ea3412e196688d4ac3cea1c0c2dfd65561d229ea5dcc42");

// Only used if Twitch no longer knows the persisted query hash.
const QString CONTEXT_QUERY = QStringLiteral(R"(
query ChannelPointsContext($channelLogin: String!) {
  community: user(login: $channelLogin) {
    id
    login
    displayName
    channel {
      id
      self {
        communityPoints {
          balance
          userRedemptions {
            reward { id }
            userRedemptionsCurrentStream
          }
        }
      }
      communityPointsSettings {
        customRewards {
          id
          backgroundColor
          cooldownExpiresAt
          cost
          pricingType
          isEnabled
          isInStock
          isPaused
          isSubOnly
          isUserInputRequired
          prompt
          title
          redemptionsRedeemedCurrentStream
          maxPerStreamSetting { isEnabled maxPerStream }
          maxPerUserPerStreamSetting { isEnabled maxPerUserPerStream }
          globalCooldownSetting { isEnabled globalCooldownSeconds }
        }
      }
    }
  }
})");

const QString REDEEM_QUERY = QStringLiteral(R"(
mutation RedeemCustomReward($input: RedeemCommunityPointsCustomRewardInput!) {
  redeemCommunityPointsCustomReward(input: $input) {
    error {
      code
      message
    }
  }
})");

QJsonObject persistedQuery(const QString &hash)
{
    return {
        {"persistedQuery",
         QJsonObject{
             {"version", 1},
             {"sha256Hash", hash},
         }},
    };
}

QString randomAlphanumeric(int length)
{
    static const QString alphabet = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    QString out;
    out.reserve(length);
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < length; i++)
    {
        out.append(alphabet.at(int(rng->bounded(alphabet.size()))));
    }
    return out;
}

/// Reads `obj[key]` as an int, accepting numbers and numeric strings.
int readInt(const QJsonObject &obj, const QString &key, int fallback = 0)
{
    const auto value = obj.value(key);
    if (value.isDouble())
    {
        return value.toInt(fallback);
    }
    if (value.isString())
    {
        bool ok = false;
        auto parsed = value.toString().toInt(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

/// Reads a `{ isEnabled, <valueKey> }` limit object. Returns 0 if disabled.
int readLimit(const QJsonObject &obj, const QString &enabledKey,
              const QString &valueKey)
{
    if (!obj.value(enabledKey).toBool(false))
    {
        return 0;
    }
    return std::max(0, readInt(obj, valueKey));
}

QString hermesTimestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

}  // namespace

QString generateTransactionId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

QString generateHermesId()
{
    return randomAlphanumeric(21);
}

QString flattenLineBreaks(const QString &text)
{
    static const QRegularExpression lineBreaks(
        QStringLiteral(R"([ \t]*[\r\n\v\f\x{2028}\x{2029}][\s\x{2028}\x{2029}]*)"));
    auto out = text;
    out.replace(lineBreaks, QStringLiteral(" "));
    return out;
}

QString normalizeRedeemInput(const QString &text)
{
    return flattenLineBreaks(text).trimmed();
}

QDateTime parseTwitchTime(const QString &value)
{
    if (value.isEmpty())
    {
        return {};
    }

    // Qt can't parse more than millisecond precision, Twitch sends up to
    // nanoseconds ("2026-10-05T00:29:54.713618729Z").
    static const QRegularExpression fraction(R"(\.(\d{1,3})\d*)");
    auto normalized = value;
    normalized.replace(fraction, R"(.\1)");

    auto dt = QDateTime::fromString(normalized, Qt::ISODateWithMs);
    if (!dt.isValid())
    {
        dt = QDateTime::fromString(normalized, Qt::ISODate);
    }
    if (dt.isValid())
    {
        dt = dt.toUTC();
    }
    return dt;
}

// ---------------------------------------------------------------------------
// GQL

QJsonObject buildContextRequest(const QString &channelLogin, bool inlineQuery)
{
    QJsonObject request{
        {"operationName", "ChannelPointsContext"},
        {"variables",
         QJsonObject{
             {"channelLogin", channelLogin},
             {"includeGoalTypes", QJsonArray{"CREATOR", "BOOST"}},
         }},
    };
    if (inlineQuery)
    {
        request.insert("query", CONTEXT_QUERY);
    }
    else
    {
        request.insert("extensions", persistedQuery(CONTEXT_HASH));
    }
    return request;
}

QJsonObject buildRedeemRequest(const QString &channelId,
                               const PointsReward &reward,
                               const QString &textInput,
                               const QString &transactionId, bool inlineQuery)
{
    QJsonObject input{
        {"channelID", channelId},
        {"cost", reward.cost},
        {"pricingType",
         reward.pricingType.isEmpty() ? QStringLiteral("POINTS")
                                      : reward.pricingType},
        {"prompt", reward.prompt},
        {"rewardID", reward.id},
        {"title", reward.title},
        {"transactionID", transactionId},
    };
    if (!textInput.isEmpty())
    {
        input.insert("textInput", textInput);
    }

    QJsonObject request{
        {"operationName", "RedeemCustomReward"},
        {"variables", QJsonObject{{"input", input}}},
    };
    if (inlineQuery)
    {
        request.insert("query", REDEEM_QUERY);
    }
    else
    {
        request.insert("extensions", persistedQuery(REDEEM_HASH));
    }
    return request;
}

GqlErrorInfo inspectGqlErrors(const QJsonObject &root)
{
    GqlErrorInfo info;
    const auto errors = root.value("errors").toArray();
    QStringList messages;
    for (const auto &value : errors)
    {
        const auto message = value.toObject().value("message").toString();
        if (message.isEmpty())
        {
            continue;
        }
        messages.append(message);
        if (message == "PersistedQueryNotFound")
        {
            info.persistedQueryNotFound = true;
        }
        if (message.compare("unauthorized", Qt::CaseInsensitive) == 0 ||
            message.contains("authentication", Qt::CaseInsensitive))
        {
            info.unauthorized = true;
        }
    }

    // Some errors come back as a top level `{"error": ..., "status": 401}`.
    if (root.contains("error") && !root.contains("data"))
    {
        messages.append(root.value("error").toString() + ": " +
                        root.value("message").toString());
        if (readInt(root, "status") == 401)
        {
            info.unauthorized = true;
        }
    }

    info.hasErrors = !messages.isEmpty();
    info.message = messages.join("; ");
    return info;
}

PointsReward parseGqlReward(const QJsonObject &obj)
{
    PointsReward reward;
    reward.id = obj.value("id").toString();
    reward.title = obj.value("title").toString();
    reward.prompt = obj.value("prompt").toString();
    reward.backgroundColor = obj.value("backgroundColor").toString();
    reward.pricingType = obj.value("pricingType").toString("POINTS");
    reward.cost = readInt(obj, "cost");
    reward.cooldownExpiresAt =
        parseTwitchTime(obj.value("cooldownExpiresAt").toString());

    const auto cooldown = obj.value("globalCooldownSetting").toObject();
    reward.hasGlobalCooldown = cooldown.value("isEnabled").toBool(false);
    reward.globalCooldownSeconds = readInt(cooldown, "globalCooldownSeconds");

    reward.maxPerStream = readLimit(obj.value("maxPerStreamSetting").toObject(),
                                    "isEnabled", "maxPerStream");
    reward.maxPerUserPerStream =
        readLimit(obj.value("maxPerUserPerStreamSetting").toObject(),
                  "isEnabled", "maxPerUserPerStream");
    reward.redemptionsThisStream =
        readInt(obj, "redemptionsRedeemedCurrentStream", -1);

    reward.isEnabled = obj.value("isEnabled").toBool(true);
    reward.isPaused = obj.value("isPaused").toBool(false);
    reward.isInStock = obj.value("isInStock").toBool(true);
    reward.isUserInputRequired = obj.value("isUserInputRequired").toBool(false);
    reward.isSubOnly = obj.value("isSubOnly").toBool(false);
    return reward;
}

PointsReward parsePubSubReward(const QJsonObject &obj)
{
    PointsReward reward;
    reward.id = obj.value("id").toString();
    reward.title = obj.value("title").toString();
    reward.prompt = obj.value("prompt").toString();
    reward.backgroundColor = obj.value("background_color").toString();
    reward.pricingType = obj.value("pricing_type").toString("POINTS");
    reward.cost = readInt(obj, "cost");
    reward.cooldownExpiresAt =
        parseTwitchTime(obj.value("cooldown_expires_at").toString());

    const auto cooldown = obj.value("global_cooldown").toObject();
    reward.hasGlobalCooldown = cooldown.value("is_enabled").toBool(false);
    reward.globalCooldownSeconds = readInt(cooldown, "global_cooldown_seconds");

    reward.maxPerStream = readLimit(obj.value("max_per_stream").toObject(),
                                    "is_enabled", "max_per_stream");
    reward.maxPerUserPerStream =
        readLimit(obj.value("max_per_user_per_stream").toObject(), "is_enabled",
                  "max_per_user_per_stream");
    reward.redemptionsThisStream =
        readInt(obj, "redemptions_redeemed_current_stream", -1);

    reward.isEnabled = obj.value("is_enabled").toBool(true);
    reward.isPaused = obj.value("is_paused").toBool(false);
    reward.isInStock = obj.value("is_in_stock").toBool(true);
    reward.isUserInputRequired =
        obj.value("is_user_input_required").toBool(false);
    reward.isSubOnly = obj.value("is_sub_only").toBool(false);
    return reward;
}

void applyLiveRewardUpdate(PointsReward &existing,
                           const PointsReward &update)
{
    const auto userRedemptions = existing.userRedemptionsThisStream;
    const auto redemptions = existing.redemptionsThisStream;
    existing = update;
    existing.userRedemptionsThisStream = userRedemptions;
    if (existing.redemptionsThisStream < 0)
    {
        existing.redemptionsThisStream = redemptions;
    }
}

std::optional<ChannelContext> parseContext(const QJsonObject &root,
                                           QString &error)
{
    const auto data = root.value("data").toObject();
    const auto communityValue = data.value("community");
    if (!communityValue.isObject())
    {
        if (data.isEmpty())
        {
            const auto info = inspectGqlErrors(root);
            error = info.hasErrors ? info.message
                                   : QStringLiteral("Empty response");
        }
        else
        {
            error = QStringLiteral("Channel not found");
        }
        return std::nullopt;
    }

    const auto community = communityValue.toObject();
    const auto channel = community.value("channel").toObject();
    if (channel.isEmpty())
    {
        error = QStringLiteral("Channel has no channel points data");
        return std::nullopt;
    }

    ChannelContext context;
    context.channelId = channel.value("id").toString();
    if (context.channelId.isEmpty())
    {
        context.channelId = community.value("id").toString();
    }
    context.login = community.value("login").toString();
    context.displayName = community.value("displayName").toString();

    QHash<QString, int> userRedemptions;
    const auto self = channel.value("self").toObject();
    const auto points = self.value("communityPoints").toObject();
    if (points.value("balance").isDouble())
    {
        context.balance = points.value("balance").toInt();
    }
    for (const auto &value : points.value("userRedemptions").toArray())
    {
        const auto obj = value.toObject();
        const auto rewardId =
            obj.value("reward").toObject().value("id").toString();
        if (!rewardId.isEmpty())
        {
            userRedemptions.insert(
                rewardId, readInt(obj, "userRedemptionsCurrentStream", 0));
        }
    }

    const auto settings = channel.value("communityPointsSettings").toObject();
    for (const auto &value : settings.value("customRewards").toArray())
    {
        auto reward = parseGqlReward(value.toObject());
        if (reward.id.isEmpty())
        {
            continue;
        }
        reward.userRedemptionsThisStream = userRedemptions.value(reward.id, 0);
        context.rewards.push_back(std::move(reward));
    }

    return context;
}

std::optional<RedeemResponse> parseRedeemResponse(const QJsonObject &root,
                                                  QString &error)
{
    const auto payloadValue = root.value("data").toObject().value(
        "redeemCommunityPointsCustomReward");
    if (!payloadValue.isObject())
    {
        const auto info = inspectGqlErrors(root);
        error = info.hasErrors ? info.message
                               : QStringLiteral("Unexpected response");
        return std::nullopt;
    }

    const auto payload = payloadValue.toObject();
    const auto errorValue = payload.value("error");
    if (errorValue.isNull() || errorValue.isUndefined())
    {
        return RedeemResponse{.ok = true};
    }

    const auto errorObj = errorValue.toObject();
    return RedeemResponse{
        .ok = false,
        .errorCode = errorObj.value("code").toString("UNKNOWN"),
        .errorMessage = errorObj.value("message").toString(),
    };
}

RedeemOutcome classifyRedeemError(const QString &code,
                                  bool retriedSameTransaction)
{
    if (code.isEmpty())
    {
        return RedeemOutcome::Success;
    }

    if (code == "DUPLICATE_TRANSACTION" || code == "TRANSACTION_ALREADY_COMMITTED")
    {
        // A retry of an attempt that never got a response: Twitch already
        // processed the first one.
        if (retriedSameTransaction || code == "TRANSACTION_ALREADY_COMMITTED")
        {
            return RedeemOutcome::Success;
        }
        // A brand new ID collided. This shouldn't happen with random IDs.
        return RedeemOutcome::NewTransactionAndRetry;
    }

    if (code == "TRANSACTION_IN_PROGRESS")
    {
        return RedeemOutcome::RetrySameTransaction;
    }

    if (code == "PROPERTIES_MISMATCH")
    {
        return RedeemOutcome::RefreshAndRetry;
    }

    static const QStringList waitCodes{
        "NOT_ENOUGH_POINTS", "GLOBAL_COOLDOWN", "COOLDOWN",
        "DISABLED",          "PAUSED",          "OUT_OF_STOCK",
        "MAX_PER_STREAM",    "MAX_PER_USER_PER_STREAM",
        "RATE_LIMITED",      "STREAM_NOT_LIVE",
    };
    if (waitCodes.contains(code))
    {
        return RedeemOutcome::Wait;
    }

    return RedeemOutcome::Stop;
}

QString describeRedeemError(const QString &code)
{
    static const QHash<QString, QString> descriptions{
        {"NOT_ENOUGH_POINTS", "Not enough points"},
        {"GLOBAL_COOLDOWN", "On cooldown"},
        {"COOLDOWN", "On cooldown"},
        {"DISABLED", "Reward is disabled"},
        {"PAUSED", "Reward is paused"},
        {"OUT_OF_STOCK", "Out of stock"},
        {"MAX_PER_STREAM", "Stream limit reached"},
        {"MAX_PER_USER_PER_STREAM", "Your limit for this stream is reached"},
        {"RATE_LIMITED", "Rate limited by Twitch"},
        {"STREAM_NOT_LIVE", "Stream isn't live"},
        {"PROPERTIES_MISMATCH", "Reward changed"},
        {"DUPLICATE_TRANSACTION", "Duplicate transaction"},
        {"TRANSACTION_IN_PROGRESS", "Still processing"},
        {"FORBIDDEN", "Not allowed"},
        {"NOT_FOUND", "Reward no longer exists"},
        {"SUB_ONLY", "Subscribers only"},
        {"USER_BANNED", "You're banned in this channel"},
    };
    auto it = descriptions.find(code);
    if (it != descriptions.end())
    {
        return it.value();
    }
    return code.isEmpty() ? QStringLiteral("Unknown error") : code;
}

// ---------------------------------------------------------------------------
// PubSub messages

PointsEvent parsePointsEvent(const QJsonObject &message)
{
    PointsEvent event;
    event.rawType = message.value("type").toString();
    const auto data = message.value("data").toObject();

    if (event.rawType == "points-earned" || event.rawType == "points-spent")
    {
        const auto balance = data.value("balance").toObject();
        event.type = PointsEvent::Type::BalanceUpdate;
        event.channelId = balance.value("channel_id").toString();
        event.balance = readInt(balance, "balance");
        return event;
    }

    auto fromReward = [&](PointsEvent::Type type, const QString &key) {
        event.type = type;
        event.reward = parsePubSubReward(data.value(key).toObject());
        event.channelId = data.value(key).toObject().value("channel_id").toString();
    };

    if (event.rawType == "custom-reward-updated")
    {
        fromReward(PointsEvent::Type::RewardUpdated, "updated_reward");
    }
    else if (event.rawType == "custom-reward-created")
    {
        fromReward(PointsEvent::Type::RewardCreated, "new_reward");
    }
    else if (event.rawType == "custom-reward-deleted")
    {
        fromReward(PointsEvent::Type::RewardDeleted, "deleted_reward");
    }
    else if (event.rawType == "reward-redeemed")
    {
        const auto redemption = data.value("redemption").toObject();
        event.type = PointsEvent::Type::RewardRedeemed;
        event.reward = parsePubSubReward(redemption.value("reward").toObject());
        event.channelId = redemption.value("channel_id").toString();
        event.redeemUserId =
            redemption.value("user").toObject().value("id").toString();
    }

    return event;
}

// ---------------------------------------------------------------------------
// Hermes

std::optional<HermesMessage> parseHermesMessage(const QByteArray &data)
{
    QJsonParseError parseError{};
    const auto doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        return std::nullopt;
    }

    const auto root = doc.object();
    HermesMessage msg;
    msg.rawType = root.value("type").toString();
    msg.id = root.value("id").toString();
    msg.parentId = root.value("parentId").toString();

    if (msg.rawType == "welcome")
    {
        msg.type = HermesMessage::Type::Welcome;
        msg.keepaliveSec =
            readInt(root.value("welcome").toObject(), "keepaliveSec", 15);
    }
    else if (msg.rawType == "keepalive")
    {
        msg.type = HermesMessage::Type::Keepalive;
    }
    else if (msg.rawType == "authenticateResponse")
    {
        const auto obj = root.value("authenticateResponse").toObject();
        msg.type = HermesMessage::Type::AuthenticateResponse;
        msg.result = obj.value("result").toString();
        msg.error = obj.value("error").toString();
        msg.errorCode = obj.value("errorCode").toString();
    }
    else if (msg.rawType == "subscribeResponse")
    {
        const auto obj = root.value("subscribeResponse").toObject();
        msg.type = HermesMessage::Type::SubscribeResponse;
        msg.result = obj.value("result").toString();
        msg.error = obj.value("error").toString();
        msg.errorCode = obj.value("errorCode").toString();
        msg.subscriptionId =
            obj.value("subscription").toObject().value("id").toString();
    }
    else if (msg.rawType == "notification")
    {
        const auto obj = root.value("notification").toObject();
        msg.type = HermesMessage::Type::Notification;
        msg.subscriptionId =
            obj.value("subscription").toObject().value("id").toString();
        // The PubSub payload is a JSON document encoded as a string.
        const auto pubsub = obj.value("pubsub");
        if (pubsub.isString())
        {
            msg.pubsub =
                QJsonDocument::fromJson(pubsub.toString().toUtf8()).object();
        }
        else
        {
            msg.pubsub = pubsub.toObject();
        }
    }
    else if (msg.rawType == "reconnect")
    {
        msg.type = HermesMessage::Type::Reconnect;
        msg.reconnectUrl = root.value("reconnect").toObject().value("url").toString();
    }

    return msg;
}

QByteArray buildHermesAuthenticate(const QString &requestId,
                                   const QString &token)
{
    QJsonObject root{
        {"id", requestId},
        {"type", "authenticate"},
        {"authenticate", QJsonObject{{"token", token}}},
        {"timestamp", hermesTimestamp()},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray buildHermesSubscribe(const QString &requestId,
                                const QString &subscriptionId,
                                const QString &topic)
{
    QJsonObject root{
        {"id", requestId},
        {"type", "subscribe"},
        {"subscribe",
         QJsonObject{
             {"id", subscriptionId},
             {"type", "pubsub"},
             {"pubsub", QJsonObject{{"topic", topic}}},
         }},
        {"timestamp", hermesTimestamp()},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// ---------------------------------------------------------------------------
// OAuth

std::optional<DeviceCode> parseDeviceCodeResponse(const QJsonObject &root)
{
    DeviceCode code{
        .deviceCode = root.value("device_code").toString(),
        .userCode = root.value("user_code").toString(),
        .verificationUri = root.value("verification_uri").toString(),
        .expiresIn = readInt(root, "expires_in", 1800),
        .interval = std::max(1, readInt(root, "interval", 5)),
    };
    if (code.deviceCode.isEmpty() || code.userCode.isEmpty())
    {
        return std::nullopt;
    }
    if (code.verificationUri.isEmpty())
    {
        code.verificationUri = QStringLiteral("https://www.twitch.tv/activate");
    }
    return code;
}

TokenResponse parseTokenResponse(std::optional<int> httpStatus,
                                 const QJsonObject &root)
{
    TokenResponse response;
    response.message = root.value("message").toString();

    const auto accessToken = root.value("access_token").toString();
    if (httpStatus.value_or(0) == 200 && !accessToken.isEmpty())
    {
        response.status = TokenResponse::Status::Success;
        response.accessToken = accessToken;
        response.refreshToken = root.value("refresh_token").toString();
        return response;
    }

    const auto message = response.message.toLower();
    if (message == "authorization_pending")
    {
        response.status = TokenResponse::Status::Pending;
    }
    else if (message == "slow_down")
    {
        response.status = TokenResponse::Status::SlowDown;
    }
    else if (message == "expired_token" || message.contains("invalid device code"))
    {
        response.status = TokenResponse::Status::Expired;
    }
    else
    {
        response.status = TokenResponse::Status::Failed;
        if (response.message.isEmpty())
        {
            response.message =
                QStringLiteral("HTTP %1").arg(httpStatus.value_or(0));
        }
    }
    return response;
}

ValidateResponse parseValidateResponse(std::optional<int> httpStatus,
                                       const QJsonObject &root)
{
    ValidateResponse response;
    response.login = root.value("login").toString();
    response.userId = root.value("user_id").toString();
    response.clientId = root.value("client_id").toString();
    response.valid = httpStatus.value_or(0) == 200 && !response.userId.isEmpty();
    return response;
}

}  // namespace chatterino::channelpoints
