// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/websockets/WebSocketPool.hpp"
#include "controllers/channelpoints/ChannelPointsClient.hpp"
#include "controllers/channelpoints/ChannelPointsModels.hpp"
#include "util/ExponentialBackoff.hpp"

#include <QObject>
#include <QHash>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace chatterino {

class ChannelPointsController : public QObject
{
    Q_OBJECT

public:
    explicit ChannelPointsController(QObject *parent = nullptr);
    ~ChannelPointsController() override;

    void stop();

    void watchChannel(const QString &channelLogin);
    void unwatchChannel(const QString &channelLogin);
    void refreshChannel(const QString &channelLogin);

    [[nodiscard]] ChannelPointsViewState getViewState(
        const QString &channelLogin) const;

    bool redeemReward(const QString &channelLogin,
                      const ChannelPointRewardData &reward);
    bool armQueueOnce(const QString &channelLogin, const QString &rewardId);
    bool setRepeatMode(const QString &channelLogin, const QString &rewardId,
                       bool enabled);
    void clearQueue(const QString &channelLogin);

Q_SIGNALS:
    void channelStateChanged(const QString &channelLogin);

private:
    struct RewardRuntime {
        ChannelPointRewardData reward;
        ChannelPointQueueMode queueMode = ChannelPointQueueMode::None;
        bool inFlight = false;
        qint64 lastAttemptAt = 0;
        quint64 queuedOnceGeneration = 0;
        QString inFlightTransactionId;
        QString lastResult;
    };

    struct ChannelRuntime {
        QString channelId;
        int visibleRefs = 0;
        int balance = 0;
        bool hasBalance = false;
        bool availableClaim = false;
        bool subscribed = false;
        bool loadingBalance = false;
        bool loadingRewards = false;
        QString lastError;
        QDateTime lastUpdated;
        QHash<QString, RewardRuntime> rewards;
        QVector<QString> rewardOrder;
    };

    enum class RequestKind {
        ReplaceSubscriptions,
        GetRewards,
        GetChannelPoints,
        Redeem,
    };

    struct RequestContext {
        RequestKind kind = RequestKind::ReplaceSubscriptions;
        QString channelLogin;
        QString rewardId;
        QString transactionId;
    };

    class Listener;

    ChannelRuntime &getOrCreateChannel(const QString &channelLogin);
    const ChannelRuntime *findChannel(const QString &channelLogin) const;
    RewardRuntime *findReward(const QString &channelLogin, const QString &rewardId);
    const RewardRuntime *findReward(const QString &channelLogin,
                                    const QString &rewardId) const;

    void ensureConnected();
    void openSocket();
    void closeSocketIfIdle();
    void syncSubscriptions();
    void requestSnapshots(const QString &channelLogin);

    void sendReplaceSubscriptions();
    QString nextRequestId();
    QString nextTransactionId();

    void handleSocketOpened();
    void handleSocketClosed();
    void handleRawMessage(const QByteArray &payload);
    void handleParsedMessage(const ChannelPointsClient::ParsedMessage &message);
    void handleReplaceSubscriptions(
        const ChannelPointsClient::ParsedMessage &message);
    void handleChannelPointsSnapshot(
        const ChannelPointsClient::ParsedMessage &message);
    void handleRewardsSnapshot(const ChannelPointsClient::ParsedMessage &message);
    void handleRedeemResult(const ChannelPointsClient::ParsedMessage &message);
    void handleError(const ChannelPointsClient::ParsedMessage &message);
    void handleEvent(const ChannelPointsClient::ParsedMessage &message);

    void scheduleReconnect();
    void markAllDisconnected();
    void emitChannelState(const QString &channelLogin);

    void tickQueue();
    bool tryRedeem(const QString &channelLogin, RewardRuntime &reward,
                   bool manual);
    bool canAttemptRedeem(const ChannelRuntime &channel,
                          const RewardRuntime &reward) const;
    void updateRewardSnapshot(ChannelRuntime &channel,
                              const QVector<ChannelPointRewardData> &rewards);

    [[nodiscard]] QStringList desiredSubscriptions() const;
    [[nodiscard]] int queuedRewardCount(const ChannelRuntime &channel) const;
    [[nodiscard]] bool shouldRefreshForRewardEvent(
        const QString &channelLogin, const QJsonObject &payload) const;
    [[nodiscard]] static QString buildRewardStatus(const ChannelRuntime &channel,
                                                   const RewardRuntime &reward);
    [[nodiscard]] static bool rewardHasCooldown(
        const ChannelPointRewardData &reward);
    [[nodiscard]] static bool rewardIsCurrentlyInStock(
        const ChannelPointRewardData &reward);
    [[nodiscard]] static QDateTime rewardCooldownAt(
        const ChannelPointRewardData &reward);
    [[nodiscard]] static QUrl successSoundUrl();

    static constexpr int MAX_ACTIVE_QUEUES_PER_CHANNEL = 16;
    static constexpr qint64 MIN_REDEEM_INTERVAL_MS = 3000;

    ExponentialBackoff<5> reconnectBackoff_{
        std::chrono::milliseconds{1000}};
    QTimer reconnectTimer_;
    QTimer queueTimer_;
    WebSocketPool pool_{"channel-points"};
    WebSocketHandle socket_;
    QHash<QString, ChannelRuntime> channels_;
    QHash<QString, RequestContext> pendingRequests_;
    bool socketOpening_ = false;
    bool transportConnected_ = false;
    bool sessionReady_ = false;
    bool stopping_ = false;
    quint64 nextRequestId_ = 1;
    quint64 nextTransactionId_ = 1;
};

}  // namespace chatterino
