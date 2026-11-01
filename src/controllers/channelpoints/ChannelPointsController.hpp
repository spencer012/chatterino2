// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsTypes.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class ChannelPointsAuth;
class HermesClient;
class NetworkResult;

/// Redeems channel point rewards and runs the redeem queues.
///
/// Talks to Twitch directly: GQL for loading rewards and redeeming, Hermes for
/// live updates (balance, reward changes). A channel is tracked while a popup
/// for it is open or while it has queued redeems.
class ChannelPointsController : public QObject
{
    Q_OBJECT

public:
    explicit ChannelPointsController(QObject *parent = nullptr);
    ~ChannelPointsController() override;

    void stop();

    ChannelPointsAuth *auth();

    /// A popup for the channel opened.
    void watchChannel(const QString &channelLogin);
    /// A popup for the channel closed. Queues keep running.
    void unwatchChannel(const QString &channelLogin);
    void refreshChannel(const QString &channelLogin);

    [[nodiscard]] ChannelPointsViewState getViewState(
        const QString &channelLogin);

    /// Redeems the reward right away.
    ///
    /// @param confirmedCost The cost the user confirmed. If the reward's cost
    ///        changed in the meantime, it isn't redeemed.
    void redeemNow(const QString &channelLogin, const QString &rewardId,
                   const QString &textInput, int confirmedCost);
    /// Redeems the reward as soon as (Once) or every time (Repeat) it's
    /// available.
    void queueReward(const QString &channelLogin, const QString &rewardId,
                     ChannelPointQueueMode mode, const QString &textInput,
                     int confirmedCost);
    void cancelQueue(const QString &channelLogin, const QString &rewardId);
    void cancelAllQueues(const QString &channelLogin);

    /// Number of queued redeems for the channel.
    [[nodiscard]] int queuedCount(const QString &channelLogin) const;

    /// Called once a vote or prediction is done. `message` explains a
    /// failure.
    using ActionCallback = std::function<void(bool ok, const QString &message)>;

    [[nodiscard]] bool isSignedIn();
    /// Votes for a poll choice (the free vote).
    void voteInPoll(const QString &channelLogin, const QString &pollId,
                    const QString &choiceId, ActionCallback callback);
    /// Predicts `points` channel points on an outcome.
    void makePrediction(const QString &channelLogin, const QString &eventId,
                        const QString &outcomeId, std::int64_t points,
                        ActionCallback callback);

Q_SIGNALS:
    /// Something about the channel changed. Empty login = all channels.
    void channelStateChanged(const QString &channelLogin);

private:
    struct Attempt {
        QString transactionId;
        QString textInput;
        /// Cost the user agreed to. We never redeem for a different cost.
        int confirmedCost = 0;
        bool manual = false;
        /// Retries reusing `transactionId` after getting no answer.
        int sameTransactionRetries = 0;
        bool regeneratedTransaction = false;
        bool refreshedOnce = false;
    };

    struct RewardRuntime {
        PointsReward reward;
        ChannelPointQueueMode queueMode = ChannelPointQueueMode::None;
        QString queuedInput;
        int queuedCost = 0;
        std::optional<Attempt> inFlight;
        /// Waiting for a reload before retrying (PROPERTIES_MISMATCH).
        std::optional<Attempt> retryAfterReload;
        QDateTime nextAttemptAt;
        int consecutiveFailures = 0;
        ChannelPointResult lastResult;
    };

    struct ChannelRuntime {
        QString login;
        QString channelId;
        QString displayName;
        int watchers = 0;
        std::optional<int> balance;
        bool loading = false;
        bool reloadQueued = false;
        bool loaded = false;
        QString error;
        QDateTime lastLoadedAt;
        QHash<QString, RewardRuntime> rewards;
        QStringList order;
    };

    struct GqlResponse {
        std::optional<int> status;
        QJsonObject root;
        /// Set if there was no usable response (network error, 5xx).
        QString transportError;
        bool unauthorized = false;
    };

    void ensureInitialized();
    void updateChatterinoUser();
    void handleAuthChanged();
    void handleHermesConnected(bool connected);
    void handleHermesNotification(const QString &topic,
                                  const QJsonObject &message);

    ChannelRuntime *findChannel(const QString &login);
    const ChannelRuntime *findChannel(const QString &login) const;
    ChannelRuntime *findChannelById(const QString &channelId);
    ChannelRuntime &getOrCreateChannel(const QString &login);
    [[nodiscard]] bool isActive(const ChannelRuntime &channel) const;
    [[nodiscard]] bool hasQueues(const ChannelRuntime &channel) const;

    void updateTopics();
    void updateTicking();
    void emitChanged(const QString &login);

    void postGql(const QString &operation,
                 const std::function<QJsonObject(bool)> &buildBody,
                 std::function<void(GqlResponse)> callback,
                 bool inlineQuery = false);

    void loadChannel(const QString &login);
    void handleContextLoaded(const QString &login, const GqlResponse &response);
    void scheduleReload(const QString &login, int delayMs);

    void tick();
    void tryQueued(ChannelRuntime &channel);
    void startAttempt(ChannelRuntime &channel, RewardRuntime &reward,
                      Attempt attempt);
    void handleRedeemResponse(const QString &login, const QString &rewardId,
                              Attempt attempt, const GqlResponse &response);
    void handleRedeemSuccess(ChannelRuntime &channel, RewardRuntime &reward,
                             const Attempt &attempt, const QString &note);
    void handleRedeemFailure(ChannelRuntime &channel, RewardRuntime &reward,
                             const Attempt &attempt, const QString &message,
                             bool keepQueue);
    void retryLater(const QString &login, const QString &rewardId,
                    Attempt attempt, int delayMs);
    void stopQueue(RewardRuntime &reward, const QString &reason);
    void cancelAllQueuesInternal(const QString &reason);

    static ChannelPointResult makeResult(ChannelPointResult::Kind kind,
                                         const QString &text);

    std::unique_ptr<ChannelPointsAuth> auth_;
    std::unique_ptr<HermesClient> hermes_;
    bool initialized_ = false;
    bool stopping_ = false;
    QString lastToken_;
    QString lastUserId_;
    ChannelPointsAuthState lastAuthState_ =
        ChannelPointsAuthState::NoChatterinoAccount;
    QString sessionId_;

    QHash<QString, ChannelRuntime> channels_;
    QTimer tickTimer_;
    QHash<QString, QTimer *> reloadTimers_;

    pajlada::Signals::ScopedConnection currentUserConnection_;
};

}  // namespace chatterino
