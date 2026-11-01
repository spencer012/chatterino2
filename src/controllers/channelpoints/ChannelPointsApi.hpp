// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsTypes.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

/// Request builders and response parsers for Twitch's first-party channel
/// points APIs (GQL, Hermes, OAuth device flow).
///
/// Everything in here is free of side effects so it can be unit tested.
namespace chatterino::channelpoints {

/// Client ID of the Twitch website. Used for GQL and Hermes.
inline const QString WEB_CLIENT_ID = QStringLiteral("kimne78kx3ncx6brgo4mv6wki5h1ko");
/// Client ID of the Twitch TV app. Used for the device code login.
inline const QString TV_CLIENT_ID = QStringLiteral("ue6666qo983tsx6so1t0vnawi233wa");

inline const QString GQL_URL = QStringLiteral("https://gql.twitch.tv/gql");
inline const QString HERMES_URL =
    QStringLiteral("wss://hermes.twitch.tv/v1?clientId=") + WEB_CLIENT_ID;
inline const QString DEVICE_CODE_URL =
    QStringLiteral("https://id.twitch.tv/oauth2/device");
inline const QString TOKEN_URL = QStringLiteral("https://id.twitch.tv/oauth2/token");
inline const QString VALIDATE_URL =
    QStringLiteral("https://id.twitch.tv/oauth2/validate");
inline const QString DEVICE_SCOPES =
    QStringLiteral("channel_read chat:read user_blocks_edit user_blocks_read "
                   "user_follows_edit user_read");

/// Generates a unique transaction ID for a redemption (32 lowercase hex chars).
///
/// Twitch deduplicates redemptions by transaction ID, so these must never be
/// reused across attempts that are meant to be separate redemptions.
QString generateTransactionId();

/// Generates a random ID used for Hermes requests/subscriptions.
QString generateHermesId();

/// Twitch posts reward text to chat as a single chat message. Longer text
/// isn't blocked (Twitch has accepted it before), only warned about.
constexpr int MAX_REDEEM_INPUT_LENGTH = 500;

/// Replaces line breaks (and whitespace around them) with a single space.
/// Chat messages are a single line, anything after a line break is lost.
QString flattenLineBreaks(const QString &text);

/// The text actually sent with a redemption: single line, trimmed.
QString normalizeRedeemInput(const QString &text);

/// Parses a Twitch timestamp. Twitch uses up to nanosecond precision, which
/// Qt doesn't handle, so the fraction is truncated to milliseconds.
QDateTime parseTwitchTime(const QString &value);

// ---------------------------------------------------------------------------
// GQL

QJsonObject buildContextRequest(const QString &channelLogin, bool inlineQuery);
QJsonObject buildRedeemRequest(const QString &channelId,
                               const PointsReward &reward,
                               const QString &textInput,
                               const QString &transactionId, bool inlineQuery);

struct GqlErrorInfo {
    bool hasErrors = false;
    bool persistedQueryNotFound = false;
    bool unauthorized = false;
    QString message;
};

GqlErrorInfo inspectGqlErrors(const QJsonObject &root);

struct ChannelContext {
    QString channelId;
    QString login;
    QString displayName;
    std::optional<int> balance;
    QVector<PointsReward> rewards;
};

/// Parses a `ChannelPointsContext` response.
/// Returns std::nullopt and sets `error` if the response is unusable.
std::optional<ChannelContext> parseContext(const QJsonObject &root,
                                           QString &error);

struct RedeemResponse {
    bool ok = false;
    QString errorCode;
    QString errorMessage;
};

/// Parses a `RedeemCustomReward` response.
std::optional<RedeemResponse> parseRedeemResponse(const QJsonObject &root,
                                                  QString &error);

/// Parses a reward as returned by GQL (camelCase).
PointsReward parseGqlReward(const QJsonObject &obj);
/// Parses a reward as sent by PubSub/Hermes (snake_case).
PointsReward parsePubSubReward(const QJsonObject &obj);

/// Applies a live update to a known reward, keeping fields that live
/// updates don't carry (per-user redemption counts).
void applyLiveRewardUpdate(PointsReward &existing,
                           const PointsReward &update);

/// What to do after a redeem attempt returned an error code.
enum class RedeemOutcome {
    Success,
    /// The reward isn't available right now. Queues keep waiting.
    Wait,
    /// Our copy of the reward is stale. Reload and try again.
    RefreshAndRetry,
    /// Try again with a fresh transaction ID.
    NewTransactionAndRetry,
    /// Twitch is still processing this transaction. Try again with the same
    /// transaction ID so it can't be redeemed twice.
    RetrySameTransaction,
    /// Give up.
    Stop,
};

/// @param retriedSameTransaction true if this attempt reused the transaction ID
///        of a previous attempt that got no response (network failure).
///        In that case DUPLICATE_TRANSACTION means the first attempt went through.
RedeemOutcome classifyRedeemError(const QString &code,
                                  bool retriedSameTransaction);

/// Human readable description of a redeem error code.
QString describeRedeemError(const QString &code);

// ---------------------------------------------------------------------------
// PubSub messages (delivered through Hermes)

struct PointsEvent {
    enum class Type {
        Unknown,
        /// points-earned / points-spent
        BalanceUpdate,
        RewardUpdated,
        RewardCreated,
        RewardDeleted,
        RewardRedeemed,
    };

    Type type = Type::Unknown;
    QString rawType;
    QString channelId;
    int balance = 0;
    PointsReward reward;
    QString redeemUserId;
};

PointsEvent parsePointsEvent(const QJsonObject &message);

// ---------------------------------------------------------------------------
// Hermes protocol

struct HermesMessage {
    enum class Type {
        Unknown,
        Welcome,
        Keepalive,
        AuthenticateResponse,
        SubscribeResponse,
        Notification,
        Reconnect,
    };

    Type type = Type::Unknown;
    QString rawType;
    QString id;
    QString parentId;
    int keepaliveSec = 0;
    QString reconnectUrl;
    /// authenticateResponse / subscribeResponse result ("ok" on success).
    QString result;
    QString error;
    QString errorCode;
    QString subscriptionId;
    /// The decoded PubSub message of a notification.
    QJsonObject pubsub;
};

std::optional<HermesMessage> parseHermesMessage(const QByteArray &data);
QByteArray buildHermesAuthenticate(const QString &requestId,
                                   const QString &token);
QByteArray buildHermesSubscribe(const QString &requestId,
                                const QString &subscriptionId,
                                const QString &topic);

// ---------------------------------------------------------------------------
// OAuth device code flow

struct DeviceCode {
    QString deviceCode;
    QString userCode;
    QString verificationUri;
    int expiresIn = 0;
    int interval = 5;
};

std::optional<DeviceCode> parseDeviceCodeResponse(const QJsonObject &root);

struct TokenResponse {
    enum class Status {
        Success,
        /// The user hasn't entered the code yet.
        Pending,
        SlowDown,
        Expired,
        Failed,
    };

    Status status = Status::Failed;
    QString accessToken;
    QString refreshToken;
    QString message;
};

/// Parses a response from the token endpoint (device code or refresh grant).
TokenResponse parseTokenResponse(std::optional<int> httpStatus,
                                 const QJsonObject &root);

struct ValidateResponse {
    bool valid = false;
    QString login;
    QString userId;
    QString clientId;
};

ValidateResponse parseValidateResponse(std::optional<int> httpStatus,
                                       const QJsonObject &root);

}  // namespace chatterino::channelpoints
