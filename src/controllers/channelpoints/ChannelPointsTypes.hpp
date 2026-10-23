// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

namespace chatterino {

/// A custom channel point reward, as seen by the signed in viewer.
struct PointsReward {
    QString id;
    QString title;
    QString prompt;
    QString backgroundColor;
    QString pricingType = QStringLiteral("POINTS");
    int cost = 0;

    /// Invalid if the reward has no active cooldown.
    QDateTime cooldownExpiresAt;
    bool hasGlobalCooldown = false;
    int globalCooldownSeconds = 0;

    /// 0 = no limit
    int maxPerStream = 0;
    /// 0 = no limit
    int maxPerUserPerStream = 0;
    /// -1 = unknown
    int redemptionsThisStream = -1;
    /// -1 = unknown
    int userRedemptionsThisStream = -1;

    bool isEnabled = true;
    bool isPaused = false;
    bool isInStock = true;
    bool isUserInputRequired = false;
    bool isSubOnly = false;
};

/// Why a reward can't be redeemed right now. `None` means it's redeemable.
enum class RewardBlock {
    None,
    Disabled,
    Paused,
    Cooldown,
    OutOfStock,
    StreamLimit,
    UserStreamLimit,
    NotEnoughPoints,
};

/// Decides whether `reward` can be redeemed at `now` with the given balance.
///
/// This is the single source of truth used by both the queue and the UI.
RewardBlock evaluateReward(const PointsReward &reward,
                           std::optional<int> balance, const QDateTime &now);

/// Short human readable description of a block, e.g. "On cooldown".
QString describeRewardBlock(RewardBlock block);

enum class ChannelPointQueueMode {
    None,
    /// Redeem once as soon as the reward becomes available.
    Once,
    /// Redeem every time the reward becomes available.
    Repeat,
};

struct ChannelPointResult {
    enum class Kind {
        None,
        Info,
        Success,
        Error,
    };

    Kind kind = Kind::None;
    QString text;
    QDateTime at;
};

struct ChannelPointRewardView {
    PointsReward reward;
    RewardBlock block = RewardBlock::None;
    ChannelPointQueueMode queueMode = ChannelPointQueueMode::None;
    bool inFlight = false;
    /// When a queued reward will be attempted next (invalid if unknown/now).
    QDateTime nextAttemptAt;
    ChannelPointResult lastResult;
};

enum class ChannelPointsAuthState {
    /// Chatterino itself has no Twitch account selected.
    NoChatterinoAccount,
    SignedOut,
    RequestingCode,
    AwaitingActivation,
    Validating,
    SignedIn,
};

struct ChannelPointsAuthView {
    ChannelPointsAuthState state = ChannelPointsAuthState::SignedOut;
    QString userCode;
    QString verificationUri;
    QDateTime codeExpiresAt;
    /// Login of the account the channel points token belongs to.
    QString login;
    QString error;
};

struct ChannelPointsViewState {
    QString channelLogin;
    QString channelDisplayName;
    QString channelId;

    ChannelPointsAuthView auth;
    bool liveUpdatesConnected = false;

    bool loading = false;
    bool loaded = false;
    QString error;
    QDateTime lastLoadedAt;

    std::optional<int> balance;
    int queuedCount = 0;
    QVector<ChannelPointRewardView> rewards;
};

}  // namespace chatterino
