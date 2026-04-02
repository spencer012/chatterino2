// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>

#include <algorithm>

namespace chatterino {

class ChannelPointsController::Listener : public WebSocketListener
{
public:
    explicit Listener(QPointer<ChannelPointsController> controller)
        : controller_(std::move(controller))
    {
    }

    void onOpen() override
    {
        runInGuiThread([controller = this->controller_] {
            if (controller)
            {
                controller->handleSocketOpened();
            }
        });
    }

    void onTextMessage(QByteArray data) override
    {
        runInGuiThread(
            [controller = this->controller_, payload = std::move(data)] {
                if (controller)
                {
                    controller->handleRawMessage(payload);
                }
            });
    }

    void onBinaryMessage(QByteArray data) override
    {
        this->onTextMessage(std::move(data));
    }

    void onClose(std::unique_ptr<WebSocketListener> /* self */) override
    {
        runInGuiThread([controller = this->controller_] {
            if (controller)
            {
                controller->handleSocketClosed();
            }
        });
    }

private:
    QPointer<ChannelPointsController> controller_;
};

namespace {

constexpr auto ISO_DATE = Qt::ISODate;

}  // namespace

ChannelPointsController::ChannelPointsController(QObject *parent)
    : QObject(parent)
{
    this->reconnectTimer_.setSingleShot(true);
    QObject::connect(&this->reconnectTimer_, &QTimer::timeout, this, [this] {
        if (!this->stopping_)
        {
            this->openSocket();
        }
    });

    this->queueTimer_.setInterval(500);
    QObject::connect(&this->queueTimer_, &QTimer::timeout, this,
                     &ChannelPointsController::tickQueue);
}

ChannelPointsController::~ChannelPointsController()
{
    this->stop();
}

void ChannelPointsController::stop()
{
    if (this->stopping_)
    {
        return;
    }

    this->stopping_ = true;
    this->reconnectTimer_.stop();
    this->queueTimer_.stop();
    this->pendingRequests_.clear();
    this->socket_.close();
    this->socket_ = {};
    this->socketOpening_ = false;
    this->transportConnected_ = false;
    this->sessionReady_ = false;
}

void ChannelPointsController::watchChannel(const QString &channelLogin)
{
    auto login = channelLogin.trimmed().toLower();
    if (login.isEmpty())
    {
        return;
    }

    auto &channel = this->getOrCreateChannel(login);
    channel.visibleRefs++;
    channel.lastError.clear();
    this->emitChannelState(login);
    this->queueTimer_.start();

    this->ensureConnected();
    this->sendReplaceSubscriptions();
}

void ChannelPointsController::unwatchChannel(const QString &channelLogin)
{
    auto login = channelLogin.trimmed().toLower();
    auto *channel = this->findChannel(login);
    if (!channel)
    {
        return;
    }

    auto &mutableChannel = this->channels_[login];
    mutableChannel.visibleRefs = std::max(0, mutableChannel.visibleRefs - 1);
    if (mutableChannel.visibleRefs == 0)
    {
        for (auto it = mutableChannel.rewards.begin(); it != mutableChannel.rewards.end();
             ++it)
        {
            it->queueMode = ChannelPointQueueMode::None;
            it->inFlight = false;
            it->inFlightTransactionId.clear();
            it->lastResult = "Inactive";
        }
        mutableChannel.subscribed = false;
        mutableChannel.loadingBalance = false;
        mutableChannel.loadingRewards = false;
    }

    this->emitChannelState(login);
    this->sendReplaceSubscriptions();
    this->closeSocketIfIdle();
}

void ChannelPointsController::refreshChannel(const QString &channelLogin)
{
    const auto login = channelLogin.trimmed().toLower();
    auto *channel = this->findChannel(login);
    if (!channel || channel->visibleRefs <= 0)
    {
        return;
    }

    this->ensureConnected();
    if (this->sessionReady_ && channel->subscribed)
    {
        this->requestSnapshots(login);
    }
    else
    {
        this->sendReplaceSubscriptions();
    }
}

ChannelPointsViewState ChannelPointsController::getViewState(
    const QString &channelLogin) const
{
    const auto login = channelLogin.trimmed().toLower();
    ChannelPointsViewState out{
        .channelLogin = login,
        .transportConnected = this->transportConnected_,
        .sessionReady = this->sessionReady_,
    };

    const auto *channel = this->findChannel(login);
    if (!channel)
    {
        return out;
    }

    out.channelId = channel->channelId;
    out.subscribed = channel->subscribed;
    out.loadingBalance = channel->loadingBalance;
    out.loadingRewards = channel->loadingRewards;
    out.hasBalance = channel->hasBalance;
    out.availableClaim = channel->availableClaim;
    out.balance = channel->balance;
    out.queuedRewardCount = this->queuedRewardCount(*channel);
    out.lastError = channel->lastError;
    out.lastUpdated = channel->lastUpdated;

    for (const auto &rewardId : channel->rewardOrder)
    {
        const auto *reward = this->findReward(login, rewardId);
        if (!reward)
        {
            continue;
        }

        const auto hasCooldown = rewardHasCooldown(reward->reward);
        const auto isAvailable = this->canAttemptRedeem(*channel, *reward);
        ChannelPointRewardView view{
            .reward = reward->reward,
            .queueMode = reward->queueMode,
            .inFlight = reward->inFlight,
            .canAfford = !channel->hasBalance || channel->balance >= reward->reward.cost,
            .canRedeemNow = isAvailable && !reward->inFlight,
            .hasCooldown = hasCooldown,
            .isAvailable = isAvailable,
            .statusText = buildRewardStatus(*channel, *reward),
        };

        if (reward->reward.isCustom)
        {
            out.customRewards.push_back(std::move(view));
        }
        else
        {
            out.automaticRewards.push_back(std::move(view));
        }
    }

    return out;
}

bool ChannelPointsController::redeemReward(const QString &channelLogin,
                                           const ChannelPointRewardData &reward)
{
    auto *runtimeReward = this->findReward(channelLogin, reward.id);
    if (!runtimeReward)
    {
        return false;
    }

    runtimeReward->queueMode = ChannelPointQueueMode::None;
    return this->tryRedeem(channelLogin.trimmed().toLower(), *runtimeReward, true);
}

bool ChannelPointsController::armQueueOnce(const QString &channelLogin,
                                           const QString &rewardId)
{
    auto login = channelLogin.trimmed().toLower();
    auto *channel = this->findChannel(login);
    auto *reward = this->findReward(login, rewardId);
    if (!channel || !reward || reward->inFlight || !reward->reward.isRedeemable)
    {
        return false;
    }

    if (this->queuedRewardCount(*channel) >= MAX_ACTIVE_QUEUES_PER_CHANNEL &&
        reward->queueMode == ChannelPointQueueMode::None)
    {
        return false;
    }

    if (reward->queueMode == ChannelPointQueueMode::Once)
    {
        reward->queueMode = ChannelPointQueueMode::None;
        reward->lastResult = "Stopped";
    }
    else
    {
        reward->queueMode = ChannelPointQueueMode::Once;
        reward->queuedOnceGeneration++;
        reward->lastResult = "Once";
    }

    this->emitChannelState(login);
    return true;
}

bool ChannelPointsController::setRepeatMode(const QString &channelLogin,
                                            const QString &rewardId,
                                            bool enabled)
{
    auto login = channelLogin.trimmed().toLower();
    auto *channel = this->findChannel(login);
    auto *reward = this->findReward(login, rewardId);
    if (!channel || !reward || !reward->reward.isRedeemable)
    {
        return false;
    }

    if (enabled && this->queuedRewardCount(*channel) >= MAX_ACTIVE_QUEUES_PER_CHANNEL &&
        reward->queueMode == ChannelPointQueueMode::None)
    {
        return false;
    }

    reward->queueMode = enabled ? ChannelPointQueueMode::Repeat
                                : ChannelPointQueueMode::None;
    reward->lastResult = enabled ? "Loop" : "Stopped";
    this->emitChannelState(login);
    return true;
}

void ChannelPointsController::clearQueue(const QString &channelLogin)
{
    auto login = channelLogin.trimmed().toLower();
    auto *channel = this->findChannel(login);
    if (!channel)
    {
        return;
    }

    auto &mutableChannel = this->channels_[login];
    for (auto it = mutableChannel.rewards.begin(); it != mutableChannel.rewards.end();
         ++it)
    {
        it->queueMode = ChannelPointQueueMode::None;
        if (!it->inFlight)
        {
            it->lastResult = "Cleared";
        }
    }

    mutableChannel.lastError.clear();
    this->emitChannelState(login);
}

ChannelPointsController::ChannelRuntime &ChannelPointsController::getOrCreateChannel(
    const QString &channelLogin)
{
    return this->channels_[channelLogin];
}

const ChannelPointsController::ChannelRuntime *
ChannelPointsController::findChannel(const QString &channelLogin) const
{
    auto it = this->channels_.find(channelLogin);
    if (it == this->channels_.end())
    {
        return nullptr;
    }
    return &it.value();
}

ChannelPointsController::RewardRuntime *ChannelPointsController::findReward(
    const QString &channelLogin, const QString &rewardId)
{
    auto channelIt = this->channels_.find(channelLogin);
    if (channelIt == this->channels_.end())
    {
        return nullptr;
    }

    auto rewardIt = channelIt->rewards.find(rewardId);
    if (rewardIt == channelIt->rewards.end())
    {
        return nullptr;
    }

    return &rewardIt.value();
}

const ChannelPointsController::RewardRuntime *
ChannelPointsController::findReward(const QString &channelLogin,
                                    const QString &rewardId) const
{
    auto channelIt = this->channels_.find(channelLogin);
    if (channelIt == this->channels_.end())
    {
        return nullptr;
    }

    auto rewardIt = channelIt->rewards.find(rewardId);
    if (rewardIt == channelIt->rewards.end())
    {
        return nullptr;
    }

    return &rewardIt.value();
}

void ChannelPointsController::ensureConnected()
{
    if (this->transportConnected_ || this->socketOpening_ || this->stopping_)
    {
        return;
    }

    if (this->desiredSubscriptions().isEmpty())
    {
        return;
    }

    this->openSocket();
}

void ChannelPointsController::openSocket()
{
    if (this->stopping_ || this->socketOpening_)
    {
        return;
    }

    this->socketOpening_ = true;
    this->socket_ = this->pool_.createSocket(
        WebSocketOptions{
            .url = QUrl("ws://127.0.0.1:8765/channel-points/ws"),
        },
        std::make_unique<Listener>(this));
}

void ChannelPointsController::closeSocketIfIdle()
{
    if (!this->desiredSubscriptions().isEmpty())
    {
        return;
    }

    this->reconnectTimer_.stop();
    this->queueTimer_.stop();
    this->socket_.close();
    this->socket_ = {};
    this->socketOpening_ = false;
    this->transportConnected_ = false;
    this->sessionReady_ = false;
    this->markAllDisconnected();
}

void ChannelPointsController::syncSubscriptions()
{
    if (!this->transportConnected_ || !this->sessionReady_)
    {
        return;
    }

    this->sendReplaceSubscriptions();
}

void ChannelPointsController::requestSnapshots(const QString &channelLogin)
{
    if (!this->sessionReady_)
    {
        return;
    }

    auto &channel = this->getOrCreateChannel(channelLogin);
    channel.loadingBalance = true;
    channel.loadingRewards = true;

    auto pointsRequestId = this->nextRequestId();
    this->pendingRequests_.insert(pointsRequestId,
                                  RequestContext{
                                      .kind = RequestKind::GetChannelPoints,
                                      .channelLogin = channelLogin,
                                  });
    this->socket_.sendText(ChannelPointsClient::buildGetChannelPointsRequest(
        pointsRequestId, channelLogin));

    auto rewardsRequestId = this->nextRequestId();
    this->pendingRequests_.insert(rewardsRequestId,
                                  RequestContext{
                                      .kind = RequestKind::GetRewards,
                                      .channelLogin = channelLogin,
                                  });
    this->socket_.sendText(ChannelPointsClient::buildGetRewardsRequest(
        rewardsRequestId, channelLogin));

    this->emitChannelState(channelLogin);
}

void ChannelPointsController::sendReplaceSubscriptions()
{
    const auto desired = this->desiredSubscriptions();
    if (desired.isEmpty())
    {
        return;
    }

    this->ensureConnected();
    if (!this->sessionReady_)
    {
        return;
    }

    auto requestId = this->nextRequestId();
    this->pendingRequests_.insert(requestId,
                                  RequestContext{
                                      .kind = RequestKind::ReplaceSubscriptions,
                                  });
    this->socket_.sendText(ChannelPointsClient::buildReplaceSubscriptionsRequest(
        requestId, desired));
}

QString ChannelPointsController::nextRequestId()
{
    return QString("channel-points-%1").arg(this->nextRequestId_++);
}

QString ChannelPointsController::nextTransactionId()
{
    return QString("cp-tx-%1").arg(this->nextTransactionId_++);
}

void ChannelPointsController::handleSocketOpened()
{
    this->socketOpening_ = false;
    this->transportConnected_ = true;
    for (auto it = this->channels_.begin(); it != this->channels_.end(); ++it)
    {
        this->emitChannelState(it.key());
    }
}

void ChannelPointsController::handleSocketClosed()
{
    this->socket_ = {};
    this->socketOpening_ = false;
    this->transportConnected_ = false;
    this->sessionReady_ = false;
    this->markAllDisconnected();

    if (!this->desiredSubscriptions().isEmpty() && !this->stopping_)
    {
        this->scheduleReconnect();
    }
}

void ChannelPointsController::handleRawMessage(const QByteArray &payload)
{
    QString error;
    auto parsed = ChannelPointsClient::parseMessage(payload, error);
    if (!parsed)
    {
        qCWarning(chatterinoWebsocket)
            << "Failed to parse channel points message:" << error << payload;
        return;
    }

    this->handleParsedMessage(*parsed);
}

void ChannelPointsController::handleParsedMessage(
    const ChannelPointsClient::ParsedMessage &message)
{
    switch (message.kind)
    {
        case ChannelPointsClient::MessageKind::Connected: {
            this->sessionReady_ = true;
            this->reconnectBackoff_.reset();
            this->syncSubscriptions();
        }
        break;
        case ChannelPointsClient::MessageKind::SubscriptionsReplaced:
            this->handleReplaceSubscriptions(message);
            break;
        case ChannelPointsClient::MessageKind::ChannelPointsSnapshot:
            this->handleChannelPointsSnapshot(message);
            break;
        case ChannelPointsClient::MessageKind::RewardsSnapshot:
            this->handleRewardsSnapshot(message);
            break;
        case ChannelPointsClient::MessageKind::RedeemResult:
            this->handleRedeemResult(message);
            break;
        case ChannelPointsClient::MessageKind::Error:
            this->handleError(message);
            break;
        case ChannelPointsClient::MessageKind::Event:
            this->handleEvent(message);
            break;
        case ChannelPointsClient::MessageKind::Subscribed:
        case ChannelPointsClient::MessageKind::Unsubscribed:
        case ChannelPointsClient::MessageKind::Unknown:
        default:
            break;
    }
}

void ChannelPointsController::handleReplaceSubscriptions(
    const ChannelPointsClient::ParsedMessage &message)
{
    if (!message.requestId.isEmpty())
    {
        this->pendingRequests_.remove(message.requestId);
    }

    const auto desired = this->desiredSubscriptions();
    for (auto it = this->channels_.begin(); it != this->channels_.end(); ++it)
    {
        it->subscribed = desired.contains(it.key());
        this->emitChannelState(it.key());
    }

    for (const auto &channelLogin : desired)
    {
        this->requestSnapshots(channelLogin);
    }
}

void ChannelPointsController::handleChannelPointsSnapshot(
    const ChannelPointsClient::ParsedMessage &message)
{
    const auto channelLogin = message.channelLogin.trimmed().toLower();
    auto &channel = this->getOrCreateChannel(channelLogin);
    channel.channelId = message.channelId;
    channel.loadingBalance = false;
    channel.lastError.clear();
    channel.lastUpdated = QDateTime::currentDateTimeUtc();

    const auto points = message.root.value("points").toObject();
    channel.balance = points.value("balance").toInt();
    channel.hasBalance = true;
    channel.availableClaim = !points.value("availableClaim").isNull() &&
                             !points.value("availableClaim").isUndefined();

    if (!message.requestId.isEmpty())
    {
        this->pendingRequests_.remove(message.requestId);
    }
    this->emitChannelState(channelLogin);
}

void ChannelPointsController::handleRewardsSnapshot(
    const ChannelPointsClient::ParsedMessage &message)
{
    const auto channelLogin = message.channelLogin.trimmed().toLower();
    auto &channel = this->getOrCreateChannel(channelLogin);
    channel.channelId = message.channelId;
    channel.loadingRewards = false;
    channel.lastError.clear();
    channel.lastUpdated = QDateTime::currentDateTimeUtc();

    const auto rewards = message.root.value("rewards").toObject();
    if (rewards.contains("balance"))
    {
        channel.balance = rewards.value("balance").toInt(channel.balance);
        channel.hasBalance = true;
    }
    channel.availableClaim = !rewards.value("availableClaim").isNull() &&
                             !rewards.value("availableClaim").isUndefined();

    auto rewardList =
        ChannelPointsClient::parseRewards(rewards.value("customRewards").toArray(),
                                          true);
    const auto automaticRewards = ChannelPointsClient::parseRewards(
        rewards.value("automaticRewards").toArray(), false);
    rewardList += automaticRewards;
    this->updateRewardSnapshot(channel, rewardList);

    if (!message.requestId.isEmpty())
    {
        this->pendingRequests_.remove(message.requestId);
    }
    this->emitChannelState(channelLogin);
}

void ChannelPointsController::handleRedeemResult(
    const ChannelPointsClient::ParsedMessage &message)
{
    auto requestIt = this->pendingRequests_.find(message.requestId);
    if (requestIt == this->pendingRequests_.end() ||
        requestIt->kind != RequestKind::Redeem)
    {
        return;
    }
    const auto ctx = requestIt.value();
    this->pendingRequests_.erase(requestIt);

    auto *reward = this->findReward(ctx.channelLogin, ctx.rewardId);
    auto *channel = this->findChannel(ctx.channelLogin);
    if (!reward || !channel)
    {
        return;
    }

    reward->inFlight = false;
    reward->inFlightTransactionId.clear();

    const auto result = message.root.value("result").toObject();
    const auto ok = result.value("ok").toBool(false);
    const auto error = result.value("error").toString();
    reward->lastResult = ok ? "Sent" : (error.isEmpty() ? "Failed" : error);

    if (ok)
    {
        if (reward->queueMode == ChannelPointQueueMode::Once)
        {
            reward->queueMode = ChannelPointQueueMode::None;
        }

        getApp()->getSound()->play(successSoundUrl());
    }

    this->requestSnapshots(ctx.channelLogin);
    this->emitChannelState(ctx.channelLogin);
}

void ChannelPointsController::handleError(
    const ChannelPointsClient::ParsedMessage &message)
{
    RequestContext ctx;
    auto requestIt = this->pendingRequests_.find(message.requestId);
    if (requestIt != this->pendingRequests_.end())
    {
        ctx = requestIt.value();
        this->pendingRequests_.erase(requestIt);
    }
    if (!ctx.channelLogin.isEmpty())
    {
        auto &channel = this->getOrCreateChannel(ctx.channelLogin);
        channel.lastError = message.errorMessage.isEmpty()
                                ? message.errorCode
                                : message.errorMessage;
        if (ctx.kind == RequestKind::GetChannelPoints)
        {
            channel.loadingBalance = false;
        }
        else if (ctx.kind == RequestKind::GetRewards)
        {
            channel.loadingRewards = false;
        }
        else if (ctx.kind == RequestKind::Redeem)
        {
            if (auto *reward = this->findReward(ctx.channelLogin, ctx.rewardId))
            {
                reward->inFlight = false;
                reward->inFlightTransactionId.clear();
                reward->lastResult = channel.lastError;
            }
        }
        this->emitChannelState(ctx.channelLogin);
        return;
    }

    qCWarning(chatterinoWebsocket)
        << "Channel points server error:" << message.errorCode
        << message.errorMessage;
}

void ChannelPointsController::handleEvent(
    const ChannelPointsClient::ParsedMessage &message)
{
    const auto channelLogin = message.channelLogin.trimmed().toLower();
    if (channelLogin.isEmpty() || this->findChannel(channelLogin) == nullptr)
    {
        return;
    }

    const auto eventName = message.event;
    if (eventName == "reward_updated")
    {
        this->requestSnapshots(channelLogin);
    }
    else if (eventName == "reward_redeemed")
    {
        const auto payload = message.root.value("payload").toObject();
        if (this->shouldRefreshForRewardEvent(channelLogin, payload))
        {
            this->requestSnapshots(channelLogin);
        }
    }
}

void ChannelPointsController::scheduleReconnect()
{
    if (this->reconnectTimer_.isActive())
    {
        return;
    }

    auto delay = this->reconnectBackoff_.next();
    this->reconnectTimer_.start(int(delay.count()));
}

void ChannelPointsController::markAllDisconnected()
{
    for (auto it = this->channels_.begin(); it != this->channels_.end(); ++it)
    {
        it->subscribed = false;
        it->loadingBalance = false;
        it->loadingRewards = false;
        for (auto rewardIt = it->rewards.begin(); rewardIt != it->rewards.end();
             ++rewardIt)
        {
            rewardIt->inFlight = false;
            rewardIt->inFlightTransactionId.clear();
        }
        this->emitChannelState(it.key());
    }
}

void ChannelPointsController::emitChannelState(const QString &channelLogin)
{
    Q_EMIT this->channelStateChanged(channelLogin);
}

void ChannelPointsController::tickQueue()
{
    if (!this->sessionReady_)
    {
        return;
    }

    for (auto channelIt = this->channels_.begin(); channelIt != this->channels_.end();
         ++channelIt)
    {
        if (channelIt->visibleRefs <= 0 || !channelIt->subscribed)
        {
            continue;
        }

        for (const auto &rewardId : channelIt->rewardOrder)
        {
            auto rewardIt = channelIt->rewards.find(rewardId);
            if (rewardIt == channelIt->rewards.end())
            {
                continue;
            }

            if (rewardIt->queueMode == ChannelPointQueueMode::None)
            {
                continue;
            }

            this->tryRedeem(channelIt.key(), rewardIt.value(), false);
        }
    }
}

bool ChannelPointsController::tryRedeem(const QString &channelLogin,
                                        RewardRuntime &reward, bool manual)
{
    auto *channel = this->findChannel(channelLogin);
    if (!channel || reward.inFlight)
    {
        return false;
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!manual && (now - reward.lastAttemptAt) < MIN_REDEEM_INTERVAL_MS)
    {
        return false;
    }

    if (!this->canAttemptRedeem(*channel, reward))
    {
        return false;
    }

    reward.inFlight = true;
    reward.lastAttemptAt = now;
    reward.lastResult = "Sending";
    reward.inFlightTransactionId = this->nextTransactionId();

    auto requestId = this->nextRequestId();
    this->pendingRequests_.insert(
        requestId,
        RequestContext{
            .kind = RequestKind::Redeem,
            .channelLogin = channelLogin,
            .rewardId = reward.reward.id,
            .transactionId = reward.inFlightTransactionId,
        });
    this->socket_.sendText(ChannelPointsClient::buildRedeemRequest(
        requestId, channelLogin, reward.reward, reward.inFlightTransactionId));
    this->emitChannelState(channelLogin);
    return true;
}

bool ChannelPointsController::canAttemptRedeem(const ChannelRuntime &channel,
                                               const RewardRuntime &reward) const
{
    if (!this->transportConnected_ || !this->sessionReady_ || !channel.subscribed)
    {
        return false;
    }

    if (!reward.reward.isRedeemable || !reward.reward.isEnabled ||
        reward.reward.isPaused || !rewardIsCurrentlyInStock(reward.reward))
    {
        return false;
    }

    if (rewardHasCooldown(reward.reward))
    {
        return false;
    }

    if (channel.hasBalance && channel.balance < reward.reward.cost)
    {
        return false;
    }

    return true;
}

void ChannelPointsController::updateRewardSnapshot(
    ChannelRuntime &channel, const QVector<ChannelPointRewardData> &rewards)
{
    QSet<QString> seen;
    channel.rewardOrder.clear();

    for (const auto &rewardData : rewards)
    {
        auto &reward = channel.rewards[rewardData.id];
        reward.reward = rewardData;
        channel.rewardOrder.push_back(rewardData.id);
        seen.insert(rewardData.id);
    }

    for (auto it = channel.rewards.begin(); it != channel.rewards.end();)
    {
        if (!seen.contains(it.key()))
        {
            it = channel.rewards.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

QStringList ChannelPointsController::desiredSubscriptions() const
{
    QStringList desired;
    for (auto it = this->channels_.begin(); it != this->channels_.end(); ++it)
    {
        if (it->visibleRefs > 0)
        {
            desired.push_back(it.key());
        }
    }
    desired.sort();
    return desired;
}

int ChannelPointsController::queuedRewardCount(const ChannelRuntime &channel) const
{
    int count = 0;
    for (auto it = channel.rewards.begin(); it != channel.rewards.end(); ++it)
    {
        if (it->queueMode != ChannelPointQueueMode::None)
        {
            count++;
        }
    }
    return count;
}

bool ChannelPointsController::shouldRefreshForRewardEvent(
    const QString &channelLogin, const QJsonObject &payload) const
{
    const auto data = payload.value("data").toObject();
    const auto redemption = data.value("redemption").toObject();
    const auto rewardObject = redemption.value("reward").toObject();
    const auto rewardId = rewardObject.value("id").toString();
    if (rewardId.isEmpty())
    {
        return false;
    }

    const auto *reward = this->findReward(channelLogin, rewardId);
    if (reward == nullptr)
    {
        return false;
    }

    return reward->reward.isGloballyLimited;
}

QString ChannelPointsController::buildRewardStatus(const ChannelRuntime &channel,
                                                   const RewardRuntime &reward)
{
    if (reward.inFlight)
    {
        return reward.lastResult.isEmpty() ? "Sending" : reward.lastResult;
    }

    if (!reward.lastResult.isEmpty() &&
        reward.queueMode == ChannelPointQueueMode::None &&
        reward.lastResult != "Inactive")
    {
        return reward.lastResult;
    }

    if (reward.queueMode == ChannelPointQueueMode::Once)
    {
        return "Once";
    }

    if (reward.queueMode == ChannelPointQueueMode::Repeat)
    {
        return "Loop";
    }

    if (!reward.reward.isRedeemable)
    {
        return "N/A";
    }

    if (!reward.reward.isEnabled)
    {
        return "Off";
    }

    if (reward.reward.isPaused)
    {
        return "Paused";
    }

    const auto cooldownAt = rewardCooldownAt(reward.reward);
    if (cooldownAt.isValid() && cooldownAt > QDateTime::currentDateTimeUtc())
    {
        return QString("CD %1").arg(
            cooldownAt.toLocalTime().toString("mm:ss"));
    }

    if (!rewardIsCurrentlyInStock(reward.reward))
    {
        return "OOS";
    }

    if (channel.hasBalance && channel.balance < reward.reward.cost)
    {
        return QString("Need %1")
            .arg(reward.reward.cost - channel.balance);
    }

    return "Ready";
}

bool ChannelPointsController::rewardHasCooldown(
    const ChannelPointRewardData &reward)
{
    const auto cooldownAt = rewardCooldownAt(reward);
    return cooldownAt.isValid() &&
           cooldownAt > QDateTime::currentDateTimeUtc();
}

bool ChannelPointsController::rewardIsCurrentlyInStock(
    const ChannelPointRewardData &reward)
{
    if (reward.isInStock)
    {
        return true;
    }

    if (reward.hasGlobalCooldown)
    {
        const auto cooldownAt = rewardCooldownAt(reward);
        if (cooldownAt.isValid() &&
            cooldownAt <= QDateTime::currentDateTimeUtc())
        {
            return true;
        }
    }

    return false;
}

QDateTime ChannelPointsController::rewardCooldownAt(
    const ChannelPointRewardData &reward)
{
    if (reward.cooldownExpiresAt.isEmpty())
    {
        return {};
    }

    auto dateTime = QDateTime::fromString(reward.cooldownExpiresAt, ISO_DATE);
    if (dateTime.isValid() && dateTime.timeSpec() == Qt::LocalTime)
    {
        dateTime = dateTime.toUTC();
    }
    return dateTime;
}

QUrl ChannelPointsController::successSoundUrl()
{
    const auto customPath = getSettings()->pathHighlightSound.getValue();
    if (!customPath.isEmpty())
    {
        return QUrl::fromLocalFile(customPath);
    }

    return QUrl("qrc:/sounds/ping2.wav");
}

}  // namespace chatterino
