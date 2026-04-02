#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace chatterino {

enum class ChannelPointQueueMode {
    None,
    Once,
    Repeat,
};

struct ChannelPointRewardData {
    QString id;
    QString title;
    QString prompt;
    QString backgroundColor;
    QString cooldownExpiresAt;
    int cost = 0;
    int globalCooldownSeconds = 0;
    int maxPerStream = 0;
    bool isCustom = false;
    bool isEnabled = false;
    bool isPaused = false;
    bool isInStock = false;
    bool isUserInputRequired = false;
    bool isSubOnly = false;
    bool isRedeemable = false;
    bool hasGlobalCooldown = false;
    bool hasMaxPerStream = false;
    bool isGloballyLimited = false;
};

struct ChannelPointRewardView {
    ChannelPointRewardData reward;
    ChannelPointQueueMode queueMode = ChannelPointQueueMode::None;
    bool inFlight = false;
    bool canAfford = false;
    bool canRedeemNow = false;
    bool hasCooldown = false;
    bool isAvailable = false;
    bool isFavorite = false;
    QString statusText;
};

struct ChannelPointsViewState {
    QString channelLogin;
    QString channelId;
    bool transportConnected = false;
    bool sessionReady = false;
    bool subscribed = false;
    bool loadingBalance = false;
    bool loadingRewards = false;
    bool hasBalance = false;
    bool availableClaim = false;
    int balance = 0;
    int queuedRewardCount = 0;
    QString lastError;
    QDateTime lastUpdated;
    QVector<ChannelPointRewardView> customRewards;
    QVector<ChannelPointRewardView> automaticRewards;
};

}  // namespace chatterino
