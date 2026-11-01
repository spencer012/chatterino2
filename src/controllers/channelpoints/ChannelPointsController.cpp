// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsController.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "controllers/channelpoints/ChannelPointsAuth.hpp"
#include "controllers/channelpoints/ChannelPointsLog.hpp"
#include "controllers/channelpoints/HermesClient.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "providers/twitch/ChannelEvents.hpp"
#include "providers/twitch/TwitchAccount.hpp"

#include <QUrl>

#include <algorithm>

namespace chatterino {

using namespace channelpoints;

namespace {

constexpr int TICK_INTERVAL_MS = 1000;
/// How old the reward data of a channel with queues may get.
constexpr qint64 MAX_QUEUE_DATA_AGE_MS = 60 * 1000;
/// Same, but while live updates are down.
constexpr qint64 MAX_QUEUE_DATA_AGE_DISCONNECTED_MS = 20 * 1000;
/// Data younger than this is reused when a popup opens.
constexpr qint64 FRESH_DATA_MS = 10 * 1000;
/// Minimum gap between redeems of a repeating queue.
constexpr int REPEAT_GAP_MS = 3000;
constexpr int MAX_SAME_TRANSACTION_RETRIES = 3;
constexpr int GQL_TIMEOUT_MS = 15 * 1000;

const QString USER_AGENT = QStringLiteral(
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/140.0.0.0 Safari/537.36");

QString normalizeLogin(const QString &login)
{
    return login.trimmed().toLower();
}

/// Delay before a queue tries again after Twitch said "not now".
int waitBackoffMs(int failures)
{
    static constexpr int STEPS[] = {5000, 10000, 30000, 60000};
    const auto index = std::clamp(failures - 1, 0, int(std::size(STEPS)) - 1);
    return STEPS[index];
}

QString modeName(ChannelPointQueueMode mode)
{
    switch (mode)
    {
        case ChannelPointQueueMode::Once:
            return QStringLiteral("once");
        case ChannelPointQueueMode::Repeat:
            return QStringLiteral("repeat");
        case ChannelPointQueueMode::None:
            break;
    }
    return QStringLiteral("manual");
}

// Upstream's highlight rework removed the global custom highlight sound, so
// this is always the built-in ping.
QUrl successSoundUrl()
{
    return QUrl("qrc:/sounds/ping2.wav");
}

}  // namespace

ChannelPointsController::ChannelPointsController(QObject *parent)
    : QObject(parent)
    , auth_(std::make_unique<ChannelPointsAuth>())
    , hermes_(std::make_unique<HermesClient>())
    , sessionId_(generateTransactionId())
{
    QObject::connect(this->auth_.get(), &ChannelPointsAuth::changed, this,
                     &ChannelPointsController::handleAuthChanged);
    QObject::connect(this->hermes_.get(), &HermesClient::connectedChanged, this,
                     &ChannelPointsController::handleHermesConnected);
    QObject::connect(this->hermes_.get(), &HermesClient::notification, this,
                     &ChannelPointsController::handleHermesNotification);
    QObject::connect(this->hermes_.get(), &HermesClient::authenticationFailed,
                     this, [this](const QString &error) {
                         this->auth_->reportTokenRejected("Hermes: " + error);
                     });

    this->tickTimer_.setInterval(TICK_INTERVAL_MS);
    QObject::connect(&this->tickTimer_, &QTimer::timeout, this,
                     &ChannelPointsController::tick);
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
    this->tickTimer_.stop();
    for (auto *timer : this->reloadTimers_)
    {
        timer->stop();
    }
    this->hermes_->stop();
}

ChannelPointsAuth *ChannelPointsController::auth()
{
    this->ensureInitialized();
    return this->auth_.get();
}

void ChannelPointsController::ensureInitialized()
{
    if (this->initialized_)
    {
        return;
    }
    this->initialized_ = true;

    this->currentUserConnection_ =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->updateChatterinoUser();
        });
    this->updateChatterinoUser();
}

void ChannelPointsController::updateChatterinoUser()
{
    auto user = getApp()->getAccounts()->twitch.getCurrent();
    if (!user || user->isAnon())
    {
        this->auth_->setChatterinoUser({}, {});
        return;
    }
    this->auth_->setChatterinoUser(user->getUserId(), user->getUserName());
}

// ---------------------------------------------------------------------------
// Public API

void ChannelPointsController::watchChannel(const QString &channelLogin)
{
    this->ensureInitialized();
    const auto login = normalizeLogin(channelLogin);
    if (login.isEmpty())
    {
        return;
    }

    auto &channel = this->getOrCreateChannel(login);
    channel.watchers++;

    const auto age = channel.lastLoadedAt.isValid()
                         ? channel.lastLoadedAt.msecsTo(
                               QDateTime::currentDateTimeUtc())
                         : std::numeric_limits<qint64>::max();
    if (!channel.loaded || age > FRESH_DATA_MS)
    {
        this->loadChannel(login);
    }

    this->updateTopics();
    this->emitChanged(login);
}

void ChannelPointsController::unwatchChannel(const QString &channelLogin)
{
    auto *channel = this->findChannel(normalizeLogin(channelLogin));
    if (channel == nullptr)
    {
        return;
    }

    channel->watchers = std::max(0, channel->watchers - 1);
    this->updateTopics();
    this->emitChanged(channel->login);
}

void ChannelPointsController::refreshChannel(const QString &channelLogin)
{
    this->ensureInitialized();
    this->loadChannel(normalizeLogin(channelLogin));
}

ChannelPointsViewState ChannelPointsController::getViewState(
    const QString &channelLogin)
{
    this->ensureInitialized();
    const auto login = normalizeLogin(channelLogin);

    ChannelPointsViewState out{
        .channelLogin = login,
        .auth = this->auth_->view(),
        .liveUpdatesConnected = this->hermes_->isConnected(),
    };

    const auto *channel = this->findChannel(login);
    if (channel == nullptr)
    {
        return out;
    }

    out.channelDisplayName = channel->displayName;
    out.channelId = channel->channelId;
    out.loading = channel->loading;
    out.loaded = channel->loaded;
    out.error = channel->error;
    out.lastLoadedAt = channel->lastLoadedAt;
    out.balance = channel->balance;
    out.queuedCount = this->queuedCount(login);

    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &rewardId : channel->order)
    {
        auto it = channel->rewards.find(rewardId);
        if (it == channel->rewards.end())
        {
            continue;
        }
        const auto &runtime = it.value();
        out.rewards.push_back(ChannelPointRewardView{
            .reward = runtime.reward,
            .block = evaluateReward(runtime.reward, channel->balance, now),
            .queueMode = runtime.queueMode,
            .inFlight = runtime.inFlight.has_value() ||
                        runtime.retryAfterReload.has_value(),
            .nextAttemptAt = runtime.nextAttemptAt,
            .lastResult = runtime.lastResult,
        });
    }

    return out;
}

void ChannelPointsController::redeemNow(const QString &channelLogin,
                                        const QString &rewardId,
                                        const QString &textInput,
                                        int confirmedCost)
{
    auto *channel = this->findChannel(normalizeLogin(channelLogin));
    if (channel == nullptr)
    {
        return;
    }
    auto it = channel->rewards.find(rewardId);
    if (it == channel->rewards.end() || it->inFlight || it->retryAfterReload)
    {
        return;
    }

    this->startAttempt(*channel, it.value(),
                       Attempt{
                           .transactionId = generateTransactionId(),
                           .textInput = normalizeRedeemInput(textInput),
                           .confirmedCost = confirmedCost,
                           .manual = true,
                       });
}

void ChannelPointsController::queueReward(const QString &channelLogin,
                                          const QString &rewardId,
                                          ChannelPointQueueMode mode,
                                          const QString &textInput,
                                          int confirmedCost)
{
    auto *channel = this->findChannel(normalizeLogin(channelLogin));
    if (channel == nullptr || mode == ChannelPointQueueMode::None)
    {
        return;
    }
    auto it = channel->rewards.find(rewardId);
    if (it == channel->rewards.end())
    {
        return;
    }

    auto &reward = it.value();
    reward.queueMode = mode;
    reward.queuedInput = normalizeRedeemInput(textInput);
    reward.queuedCost = confirmedCost;
    reward.consecutiveFailures = 0;
    reward.nextAttemptAt = {};
    reward.lastResult = makeResult(ChannelPointResult::Kind::Info,
                                   mode == ChannelPointQueueMode::Once
                                       ? QStringLiteral("Queued")
                                       : QStringLiteral("Repeating"));
    logInfo(QStringLiteral("Queue %1: #%2 \"%3\" cost=%4")
                .arg(modeName(mode), channel->login, reward.reward.title)
                .arg(confirmedCost));

    this->updateTicking();
    this->updateTopics();
    this->tryQueued(*channel);
    this->emitChanged(channel->login);
}

void ChannelPointsController::cancelQueue(const QString &channelLogin,
                                          const QString &rewardId)
{
    auto *channel = this->findChannel(normalizeLogin(channelLogin));
    if (channel == nullptr)
    {
        return;
    }
    auto it = channel->rewards.find(rewardId);
    if (it == channel->rewards.end() ||
        it->queueMode == ChannelPointQueueMode::None)
    {
        return;
    }

    logInfo(QStringLiteral("Queue cancelled: #%1 \"%2\"")
                .arg(channel->login, it->reward.title));
    it->queueMode = ChannelPointQueueMode::None;
    it->queuedInput.clear();
    it->lastResult =
        makeResult(ChannelPointResult::Kind::Info, QStringLiteral("Cancelled"));

    this->updateTicking();
    this->updateTopics();
    this->emitChanged(channel->login);
}

void ChannelPointsController::cancelAllQueues(const QString &channelLogin)
{
    auto *channel = this->findChannel(normalizeLogin(channelLogin));
    if (channel == nullptr)
    {
        return;
    }

    for (auto &reward : channel->rewards)
    {
        if (reward.queueMode != ChannelPointQueueMode::None)
        {
            reward.queueMode = ChannelPointQueueMode::None;
            reward.queuedInput.clear();
            reward.lastResult = makeResult(ChannelPointResult::Kind::Info,
                                           QStringLiteral("Cancelled"));
        }
    }
    logInfo(QStringLiteral("All queues cancelled: #%1").arg(channel->login));

    this->updateTicking();
    this->updateTopics();
    this->emitChanged(channel->login);
}

int ChannelPointsController::queuedCount(const QString &channelLogin) const
{
    const auto *channel = this->findChannel(normalizeLogin(channelLogin));
    if (channel == nullptr)
    {
        return 0;
    }
    return int(std::count_if(channel->rewards.begin(), channel->rewards.end(),
                             [](const RewardRuntime &reward) {
                                 return reward.queueMode !=
                                        ChannelPointQueueMode::None;
                             }));
}

// ---------------------------------------------------------------------------
// Auth & live updates

void ChannelPointsController::handleAuthChanged()
{
    const auto view = this->auth_->view();
    const auto token = this->auth_->token();
    const auto userId = this->auth_->userId();

    const bool signedOut =
        view.state == ChannelPointsAuthState::SignedOut ||
        view.state == ChannelPointsAuthState::NoChatterinoAccount;
    const bool userChanged = !userId.isEmpty() && userId != this->lastUserId_;

    if ((signedOut && !this->lastUserId_.isEmpty()) ||
        (userChanged && !this->lastUserId_.isEmpty()))
    {
        // Everything we know belongs to the previous account.
        this->cancelAllQueuesInternal(QStringLiteral("Signed out"));
        for (auto &channel : this->channels_)
        {
            channel.balance.reset();
            channel.rewards.clear();
            channel.order.clear();
            channel.loaded = false;
            channel.error.clear();
            channel.lastLoadedAt = {};
        }
    }
    if (signedOut)
    {
        this->lastUserId_.clear();
    }
    else if (!userId.isEmpty())
    {
        this->lastUserId_ = userId;
    }

    if (token != this->lastToken_)
    {
        this->lastToken_ = token;
        this->hermes_->setToken(token);
        if (!token.isEmpty())
        {
            for (auto &channel : this->channels_)
            {
                if (this->isActive(channel))
                {
                    this->loadChannel(channel.login);
                }
            }
        }
    }

    this->lastAuthState_ = view.state;
    this->updateTopics();
    this->emitChanged({});
}

void ChannelPointsController::handleHermesConnected(bool connected)
{
    if (connected)
    {
        // Catch up on anything we missed while disconnected.
        for (auto &channel : this->channels_)
        {
            if (this->isActive(channel))
            {
                this->loadChannel(channel.login);
            }
        }
    }
    this->emitChanged({});
}

void ChannelPointsController::handleHermesNotification(
    const QString & /*topic*/, const QJsonObject &message)
{
    const auto event = parsePointsEvent(message);
    if (event.type == PointsEvent::Type::Unknown)
    {
        return;
    }

    auto *channel = this->findChannelById(event.channelId);
    if (channel == nullptr)
    {
        return;
    }

    switch (event.type)
    {
        case PointsEvent::Type::BalanceUpdate:
            channel->balance = event.balance;
            break;

        case PointsEvent::Type::RewardCreated:
        case PointsEvent::Type::RewardUpdated: {
            if (!channel->loaded || event.reward.id.isEmpty())
            {
                break;
            }
            auto it = channel->rewards.find(event.reward.id);
            if (it == channel->rewards.end())
            {
                auto &runtime = channel->rewards[event.reward.id];
                runtime.reward = event.reward;
                runtime.reward.userRedemptionsThisStream = 0;
                channel->order.append(event.reward.id);
                break;
            }

            const auto oldCost = it->reward.cost;
            applyLiveRewardUpdate(it->reward, event.reward);
            if (it->queueMode != ChannelPointQueueMode::None &&
                it->reward.cost != it->queuedCost)
            {
                this->stopQueue(it.value(),
                                QStringLiteral("cost changed from %1 to %2")
                                    .arg(oldCost)
                                    .arg(it->reward.cost));
            }
        }
        break;

        case PointsEvent::Type::RewardDeleted: {
            auto it = channel->rewards.find(event.reward.id);
            if (it != channel->rewards.end())
            {
                if (it->queueMode != ChannelPointQueueMode::None)
                {
                    logWarning(QStringLiteral("Queue dropped: #%1 \"%2\" was "
                                              "deleted by the channel")
                                   .arg(channel->login, it->reward.title));
                }
                channel->rewards.erase(it);
                channel->order.removeAll(event.reward.id);
            }
        }
        break;

        case PointsEvent::Type::RewardRedeemed: {
            auto it = channel->rewards.find(event.reward.id);
            if (it != channel->rewards.end())
            {
                // Carries the new cooldown/stock state.
                applyLiveRewardUpdate(it->reward, event.reward);
                if (event.redeemUserId == this->auth_->userId())
                {
                    logInfo(QStringLiteral("Twitch event: you redeemed #%1 \"%2\"")
                                .arg(channel->login, it->reward.title));
                    this->scheduleReload(channel->login, 1500);
                }
            }
        }
        break;

        case PointsEvent::Type::Unknown:
            break;
    }

    this->tryQueued(*channel);
    this->updateTicking();
    this->emitChanged(channel->login);
}

// ---------------------------------------------------------------------------
// Channel bookkeeping

ChannelPointsController::ChannelRuntime *ChannelPointsController::findChannel(
    const QString &login)
{
    auto it = this->channels_.find(login);
    return it == this->channels_.end() ? nullptr : &it.value();
}

const ChannelPointsController::ChannelRuntime *
    ChannelPointsController::findChannel(const QString &login) const
{
    auto it = this->channels_.find(login);
    return it == this->channels_.end() ? nullptr : &it.value();
}

ChannelPointsController::ChannelRuntime *
    ChannelPointsController::findChannelById(const QString &channelId)
{
    if (channelId.isEmpty())
    {
        return nullptr;
    }
    for (auto &channel : this->channels_)
    {
        if (channel.channelId == channelId)
        {
            return &channel;
        }
    }
    return nullptr;
}

ChannelPointsController::ChannelRuntime &
    ChannelPointsController::getOrCreateChannel(const QString &login)
{
    auto &channel = this->channels_[login];
    channel.login = login;
    return channel;
}

bool ChannelPointsController::hasQueues(const ChannelRuntime &channel) const
{
    return std::any_of(channel.rewards.begin(), channel.rewards.end(),
                       [](const RewardRuntime &reward) {
                           return reward.queueMode !=
                                  ChannelPointQueueMode::None;
                       });
}

bool ChannelPointsController::isActive(const ChannelRuntime &channel) const
{
    if (channel.watchers > 0 || this->hasQueues(channel))
    {
        return true;
    }
    return std::any_of(channel.rewards.begin(), channel.rewards.end(),
                       [](const RewardRuntime &reward) {
                           return reward.inFlight || reward.retryAfterReload;
                       });
}

void ChannelPointsController::updateTopics()
{
    QSet<QString> topics;
    if (!this->auth_->token().isEmpty())
    {
        bool anyActive = false;
        for (const auto &channel : this->channels_)
        {
            if (!this->isActive(channel))
            {
                continue;
            }
            anyActive = true;
            if (!channel.channelId.isEmpty())
            {
                topics.insert("community-points-channel-v1." + channel.channelId);
            }
        }
        if (anyActive && !this->auth_->userId().isEmpty())
        {
            topics.insert("community-points-user-v1." + this->auth_->userId());
        }
    }
    this->hermes_->setTopics(topics);
}

void ChannelPointsController::updateTicking()
{
    if (this->stopping_)
    {
        return;
    }

    const bool anyQueues =
        std::any_of(this->channels_.begin(), this->channels_.end(),
                    [this](const ChannelRuntime &channel) {
                        return this->hasQueues(channel);
                    });
    if (anyQueues && !this->tickTimer_.isActive())
    {
        this->tickTimer_.start();
    }
    else if (!anyQueues)
    {
        this->tickTimer_.stop();
    }
}

void ChannelPointsController::emitChanged(const QString &login)
{
    Q_EMIT this->channelStateChanged(login);
}

ChannelPointResult ChannelPointsController::makeResult(
    ChannelPointResult::Kind kind, const QString &text)
{
    return {
        .kind = kind,
        .text = text,
        .at = QDateTime::currentDateTime(),
    };
}

// ---------------------------------------------------------------------------
// GQL

void ChannelPointsController::postGql(
    const QString &operation, const std::function<QJsonObject(bool)> &buildBody,
    std::function<void(GqlResponse)> callback, bool inlineQuery)
{
    const auto token = this->auth_->token();
    if (token.isEmpty())
    {
        callback(GqlResponse{
            .transportError = QStringLiteral("Not signed in"),
        });
        return;
    }

    auto handle = [this, operation, buildBody, callback,
                   inlineQuery](const NetworkResult &result) {
        GqlResponse response;
        response.status = result.status();

        if (!response.status)
        {
            response.transportError = result.formatError();
            callback(response);
            return;
        }
        if (*response.status == 401)
        {
            response.unauthorized = true;
            response.transportError = QStringLiteral("Unauthorized");
            callback(response);
            return;
        }
        if (*response.status >= 500)
        {
            response.transportError = QStringLiteral("HTTP %1").arg(*response.status);
            callback(response);
            return;
        }

        response.root = result.parseJson();
        if (response.root.isEmpty())
        {
            response.transportError = QStringLiteral("Invalid response (HTTP %1)")
                                          .arg(*response.status);
            logWarning(QStringLiteral("GQL %1: invalid response: %2")
                           .arg(operation,
                                QString::fromUtf8(result.getData().left(300))));
            callback(response);
            return;
        }

        const auto errors = inspectGqlErrors(response.root);
        if (errors.persistedQueryNotFound && !inlineQuery)
        {
            logWarning(QStringLiteral("GQL %1: persisted query unknown, "
                                      "retrying with the full query")
                           .arg(operation));
            this->postGql(operation, buildBody, callback, true);
            return;
        }
        if (errors.hasErrors)
        {
            logWarning(QStringLiteral("GQL %1 returned errors: %2")
                           .arg(operation, errors.message));
        }
        response.unauthorized = errors.unauthorized;
        callback(response);
    };

    NetworkRequest(GQL_URL, NetworkRequestType::Post)
        .header("Authorization", "OAuth " + token)
        .header("Client-Id", WEB_CLIENT_ID)
        .header("X-Device-Id", ChannelPointsAuth::deviceId())
        .header("Client-Session-Id", this->sessionId_)
        .header("User-Agent", USER_AGENT)
        .json(buildBody(inlineQuery))
        .timeout(GQL_TIMEOUT_MS)
        .caller(this)
        .onSuccess(handle)
        .onError(handle)
        .execute();
}

bool ChannelPointsController::isSignedIn()
{
    this->ensureInitialized();
    return !this->auth_->token().isEmpty();
}

void ChannelPointsController::voteInPoll(const QString &channelLogin,
                                         const QString &pollId,
                                         const QString &choiceId,
                                         ActionCallback callback)
{
    this->ensureInitialized();
    const auto login = normalizeLogin(channelLogin);
    const auto userId = this->auth_->userId();
    if (this->auth_->token().isEmpty() || userId.isEmpty())
    {
        callback(false, QStringLiteral("Sign in to channel points first."));
        return;
    }

    const auto voteId = generateTransactionId();
    logInfo(QStringLiteral("[%1] voting in poll %2 for choice %3 (vote %4)")
                .arg(login, pollId, choiceId, voteId));

    this->postGql(
        QStringLiteral("VoteInPoll"),
        [pollId, choiceId, userId, voteId](bool inlineQuery) {
            return channelevents::buildVoteRequest(pollId, choiceId, userId,
                                                   voteId, inlineQuery);
        },
        [this, login, callback](const GqlResponse &response) {
            if (response.unauthorized)
            {
                this->auth_->reportTokenRejected(QStringLiteral("VoteInPoll"));
            }
            if (!response.transportError.isEmpty())
            {
                logWarning(QStringLiteral("[%1] vote failed: %2")
                               .arg(login, response.transportError));
                callback(false,
                         QStringLiteral("No answer from Twitch (%1).")
                             .arg(response.transportError));
                return;
            }

            QString error;
            const auto parsed =
                channelevents::parseVoteResponse(response.root, error);
            if (!parsed)
            {
                logWarning(
                    QStringLiteral("[%1] vote failed: %2").arg(login, error));
                callback(false, error);
                return;
            }
            if (!parsed->ok)
            {
                logWarning(QStringLiteral("[%1] vote refused: %2")
                               .arg(login, parsed->errorCode));
                callback(false,
                         channelevents::describeVoteError(parsed->errorCode));
                return;
            }

            logInfo(QStringLiteral("[%1] vote accepted").arg(login));
            callback(true, {});
        });
}

void ChannelPointsController::makePrediction(const QString &channelLogin,
                                             const QString &eventId,
                                             const QString &outcomeId,
                                             std::int64_t points,
                                             ActionCallback callback)
{
    this->ensureInitialized();
    const auto login = normalizeLogin(channelLogin);
    if (this->auth_->token().isEmpty())
    {
        callback(false, QStringLiteral("Sign in to channel points first."));
        return;
    }
    if (points <= 0 || points > channelevents::MAX_PREDICTION_POINTS)
    {
        callback(false, QStringLiteral("Predict between 1 and %1 points.")
                            .arg(channelevents::MAX_PREDICTION_POINTS));
        return;
    }

    const auto transactionId = generateTransactionId();
    logInfo(QStringLiteral("[%1] predicting %2 points on outcome %3 of %4 "
                           "(tx %5)")
                .arg(login)
                .arg(points)
                .arg(outcomeId, eventId, transactionId));

    this->postGql(
        QStringLiteral("MakePrediction"),
        [eventId, outcomeId, points, transactionId](bool inlineQuery) {
            return channelevents::buildPredictionRequest(
                eventId, outcomeId, points, transactionId, inlineQuery);
        },
        [this, login, callback](const GqlResponse &response) {
            if (response.unauthorized)
            {
                this->auth_->reportTokenRejected(
                    QStringLiteral("MakePrediction"));
            }
            if (!response.transportError.isEmpty())
            {
                logWarning(QStringLiteral("[%1] prediction failed: %2")
                               .arg(login, response.transportError));
                callback(false,
                         QStringLiteral("No answer from Twitch (%1). Check "
                                        "whether the prediction went through "
                                        "before trying again.")
                             .arg(response.transportError));
                return;
            }

            QString error;
            const auto parsed =
                channelevents::parsePredictionResponse(response.root, error);
            if (!parsed)
            {
                logWarning(QStringLiteral("[%1] prediction failed: %2")
                               .arg(login, error));
                callback(false, error);
                return;
            }
            if (!parsed->ok)
            {
                logWarning(QStringLiteral("[%1] prediction refused: %2")
                               .arg(login, parsed->errorCode));
                callback(false, channelevents::describePredictionError(
                                    parsed->errorCode));
                return;
            }

            logInfo(QStringLiteral("[%1] prediction accepted").arg(login));
            if (this->findChannel(login) != nullptr)
            {
                this->loadChannel(login);
            }
            callback(true, {});
        });
}

void ChannelPointsController::loadChannel(const QString &login)
{
    auto *channel = this->findChannel(login);
    if (channel == nullptr || this->stopping_ ||
        this->auth_->token().isEmpty())
    {
        return;
    }

    if (channel->loading)
    {
        channel->reloadQueued = true;
        return;
    }

    channel->loading = true;
    this->emitChanged(login);

    this->postGql(
        QStringLiteral("ChannelPointsContext"),
        [login](bool inlineQuery) {
            return buildContextRequest(login, inlineQuery);
        },
        [this, login](const GqlResponse &response) {
            this->handleContextLoaded(login, response);
        });
}

void ChannelPointsController::handleContextLoaded(const QString &login,
                                                  const GqlResponse &response)
{
    auto *channel = this->findChannel(login);
    if (channel == nullptr)
    {
        return;
    }
    channel->loading = false;

    if (response.unauthorized)
    {
        channel->error = QStringLiteral("Twitch rejected the channel points "
                                        "sign-in");
        this->auth_->reportTokenRejected(QStringLiteral("ChannelPointsContext"));
    }
    else if (!response.transportError.isEmpty())
    {
        channel->error = QStringLiteral("Couldn't load rewards: %1")
                             .arg(response.transportError);
        logWarning(QStringLiteral("Loading #%1 failed: %2")
                       .arg(login, response.transportError));
    }
    else
    {
        QString error;
        auto context = parseContext(response.root, error);
        if (!context)
        {
            channel->error = error;
            logWarning(QStringLiteral("Loading #%1 failed: %2").arg(login, error));
        }
        else
        {
            channel->error.clear();
            channel->loaded = true;
            channel->lastLoadedAt = QDateTime::currentDateTimeUtc();
            const bool idChanged = channel->channelId != context->channelId;
            channel->channelId = context->channelId;
            channel->displayName = context->displayName;
            if (context->balance)
            {
                channel->balance = context->balance;
            }

            QSet<QString> seen;
            QStringList order;
            for (auto &reward : context->rewards)
            {
                const auto id = reward.id;
                channel->rewards[id].reward = std::move(reward);
                order.append(id);
                seen.insert(id);
            }
            for (auto it = channel->rewards.begin();
                 it != channel->rewards.end();)
            {
                if (seen.contains(it.key()))
                {
                    ++it;
                    continue;
                }
                if (it->queueMode != ChannelPointQueueMode::None)
                {
                    logWarning(QStringLiteral("Queue dropped: #%1 \"%2\" no "
                                              "longer exists")
                                   .arg(login, it->reward.title));
                }
                it = channel->rewards.erase(it);
            }
            channel->order = order;

            for (auto &reward : channel->rewards)
            {
                if (reward.retryAfterReload)
                {
                    auto attempt = *reward.retryAfterReload;
                    reward.retryAfterReload.reset();
                    // The reward changed. A fresh ID keeps this from being
                    // mistaken for the failed attempt.
                    attempt.transactionId = generateTransactionId();
                    attempt.sameTransactionRetries = 0;
                    this->startAttempt(*channel, reward, attempt);
                    continue;
                }
                if (reward.queueMode != ChannelPointQueueMode::None &&
                    reward.reward.cost != reward.queuedCost)
                {
                    this->stopQueue(reward,
                                    QStringLiteral("cost changed from %1 to %2")
                                        .arg(reward.queuedCost)
                                        .arg(reward.reward.cost));
                }
            }

            if (idChanged)
            {
                this->updateTopics();
            }
        }
    }

    if (!channel->loaded || !channel->error.isEmpty())
    {
        // Attempts waiting for this reload can't continue.
        for (auto &reward : channel->rewards)
        {
            if (reward.retryAfterReload)
            {
                auto attempt = *reward.retryAfterReload;
                reward.retryAfterReload.reset();
                this->handleRedeemFailure(
                    *channel, reward, attempt,
                    QStringLiteral("Reward changed, and reloading it failed"),
                    true);
            }
        }
    }

    if (channel->reloadQueued)
    {
        channel->reloadQueued = false;
        this->loadChannel(login);
    }
    else
    {
        this->tryQueued(*channel);
    }
    this->updateTicking();
    this->emitChanged(login);
}

void ChannelPointsController::scheduleReload(const QString &login, int delayMs)
{
    auto *&timer = this->reloadTimers_[login];
    if (timer == nullptr)
    {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, this, [this, login] {
            this->loadChannel(login);
        });
    }
    timer->start(delayMs);
}

// ---------------------------------------------------------------------------
// Queue & redeeming

void ChannelPointsController::tick()
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto maxAge = this->hermes_->isConnected()
                            ? MAX_QUEUE_DATA_AGE_MS
                            : MAX_QUEUE_DATA_AGE_DISCONNECTED_MS;

    // Collect first, loading/attempting may modify channels_.
    QStringList logins;
    for (const auto &channel : this->channels_)
    {
        if (this->hasQueues(channel))
        {
            logins.append(channel.login);
        }
    }

    for (const auto &login : logins)
    {
        auto *channel = this->findChannel(login);
        if (channel == nullptr)
        {
            continue;
        }

        // Live updates can be missed; make sure queued rewards don't wait on
        // stale data forever.
        if (!channel->loading &&
            (!channel->lastLoadedAt.isValid() ||
             channel->lastLoadedAt.msecsTo(now) > maxAge))
        {
            this->loadChannel(login);
            continue;
        }

        this->tryQueued(*channel);
    }
}

void ChannelPointsController::tryQueued(ChannelRuntime &channel)
{
    if (this->stopping_ || this->auth_->token().isEmpty() || !channel.loaded ||
        channel.loading || channel.channelId.isEmpty())
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &rewardId : channel.order)
    {
        auto it = channel.rewards.find(rewardId);
        if (it == channel.rewards.end())
        {
            continue;
        }

        auto &reward = it.value();
        if (reward.queueMode == ChannelPointQueueMode::None ||
            reward.inFlight || reward.retryAfterReload)
        {
            continue;
        }
        if (reward.nextAttemptAt.isValid() && now < reward.nextAttemptAt)
        {
            continue;
        }
        if (evaluateReward(reward.reward, channel.balance, now) !=
            RewardBlock::None)
        {
            continue;
        }

        this->startAttempt(channel, reward,
                           Attempt{
                               .transactionId = generateTransactionId(),
                               .textInput = reward.queuedInput,
                               .confirmedCost = reward.queuedCost,
                               .manual = false,
                           });
    }
}

void ChannelPointsController::startAttempt(ChannelRuntime &channel,
                                           RewardRuntime &reward,
                                           Attempt attempt)
{
    if (reward.reward.cost != attempt.confirmedCost)
    {
        // Never spend a different amount than the user agreed to.
        this->handleRedeemFailure(
            channel, reward, attempt,
            QStringLiteral("Cost changed from %1 to %2, not redeemed")
                .arg(attempt.confirmedCost)
                .arg(reward.reward.cost),
            false);
        return;
    }

    reward.inFlight = attempt;
    reward.lastResult = makeResult(ChannelPointResult::Kind::Info,
                                   attempt.sameTransactionRetries > 0
                                       ? QStringLiteral("Retrying…")
                                       : QStringLiteral("Redeeming…"));

    logInfo(QStringLiteral("Redeem attempt: #%1 \"%2\" cost=%3 balance=%4 "
                           "mode=%5 input=%6 chars tx=%7%8")
                .arg(channel.login, reward.reward.title)
                .arg(attempt.confirmedCost)
                .arg(channel.balance ? QString::number(*channel.balance)
                                     : QStringLiteral("?"))
                .arg(attempt.manual ? QStringLiteral("manual")
                                    : modeName(reward.queueMode))
                .arg(attempt.textInput.size())
                .arg(attempt.transactionId,
                     attempt.sameTransactionRetries > 0
                         ? QStringLiteral(" (retry %1, same tx)")
                               .arg(attempt.sameTransactionRetries)
                         : QString()));

    const auto login = channel.login;
    const auto rewardId = reward.reward.id;
    const auto channelId = channel.channelId;
    const auto rewardData = reward.reward;

    this->postGql(
        QStringLiteral("RedeemCustomReward"),
        [channelId, rewardData, attempt](bool inlineQuery) {
            return buildRedeemRequest(channelId, rewardData, attempt.textInput,
                                      attempt.transactionId, inlineQuery);
        },
        [this, login, rewardId, attempt](const GqlResponse &response) {
            this->handleRedeemResponse(login, rewardId, attempt, response);
        });

    this->updateTopics();
    this->emitChanged(login);
}

void ChannelPointsController::handleRedeemResponse(const QString &login,
                                                   const QString &rewardId,
                                                   Attempt attempt,
                                                   const GqlResponse &response)
{
    auto *channel = this->findChannel(login);
    if (channel == nullptr)
    {
        return;
    }
    auto it = channel->rewards.find(rewardId);
    if (it == channel->rewards.end())
    {
        logWarning(QStringLiteral("Redeem response for a reward that no longer "
                                  "exists (#%1, tx=%2)")
                       .arg(login, attempt.transactionId));
        return;
    }

    auto &reward = it.value();
    reward.inFlight.reset();

    if (response.unauthorized)
    {
        this->handleRedeemFailure(*channel, reward, attempt,
                                  QStringLiteral("Twitch rejected the channel "
                                                 "points sign-in"),
                                  true);
        this->auth_->reportTokenRejected(QStringLiteral("RedeemCustomReward"));
        return;
    }

    if (!response.transportError.isEmpty())
    {
        // We don't know whether Twitch processed it. Retrying with the same
        // transaction ID lets Twitch tell us without redeeming twice.
        if (attempt.sameTransactionRetries < MAX_SAME_TRANSACTION_RETRIES)
        {
            attempt.sameTransactionRetries++;
            logWarning(QStringLiteral("Redeem #%1 \"%2\": no answer (%3), "
                                      "retrying with the same tx=%4")
                           .arg(login, reward.reward.title,
                                response.transportError, attempt.transactionId));
            reward.inFlight = attempt;
            reward.lastResult = makeResult(ChannelPointResult::Kind::Info,
                                           QStringLiteral("No answer, retrying…"));
            this->retryLater(login, rewardId, attempt,
                             2000 * attempt.sameTransactionRetries);
            this->emitChanged(login);
            return;
        }

        this->handleRedeemFailure(
            *channel, reward, attempt,
            QStringLiteral("No answer from Twitch (%1). It may or may not "
                           "have gone through.")
                .arg(response.transportError),
            true);
        this->scheduleReload(login, 1000);
        return;
    }

    QString parseError;
    auto result = parseRedeemResponse(response.root, parseError);
    if (!result)
    {
        logWarning(QStringLiteral("Redeem #%1 \"%2\": unexpected response: %3")
                       .arg(login, reward.reward.title, parseError));
        this->handleRedeemFailure(*channel, reward, attempt, parseError, true);
        return;
    }

    if (result->ok)
    {
        this->handleRedeemSuccess(*channel, reward, attempt, {});
        return;
    }

    const auto outcome = classifyRedeemError(
        result->errorCode, attempt.sameTransactionRetries > 0);
    logInfo(QStringLiteral("Redeem #%1 \"%2\" returned %3 %4 (tx=%5)")
                .arg(login, reward.reward.title, result->errorCode,
                     result->errorMessage, attempt.transactionId));
    const auto description = describeRedeemError(result->errorCode);

    switch (outcome)
    {
        case RedeemOutcome::Success:
            this->handleRedeemSuccess(
                *channel, reward, attempt,
                QStringLiteral("confirmed by %1").arg(result->errorCode));
            return;

        case RedeemOutcome::NewTransactionAndRetry:
            if (!attempt.regeneratedTransaction)
            {
                attempt.regeneratedTransaction = true;
                attempt.transactionId = generateTransactionId();
                attempt.sameTransactionRetries = 0;
                this->startAttempt(*channel, reward, attempt);
                return;
            }
            break;

        case RedeemOutcome::RetrySameTransaction:
            if (attempt.sameTransactionRetries < MAX_SAME_TRANSACTION_RETRIES)
            {
                attempt.sameTransactionRetries++;
                reward.inFlight = attempt;
                this->retryLater(login, rewardId, attempt, 2000);
                this->emitChanged(login);
                return;
            }
            break;

        case RedeemOutcome::RefreshAndRetry:
            if (!attempt.refreshedOnce)
            {
                attempt.refreshedOnce = true;
                reward.retryAfterReload = attempt;
                reward.lastResult = makeResult(
                    ChannelPointResult::Kind::Info,
                    QStringLiteral("Reward changed, reloading…"));
                this->loadChannel(login);
                this->emitChanged(login);
                return;
            }
            break;

        case RedeemOutcome::Wait:
            if (result->errorCode == "MAX_PER_USER_PER_STREAM" &&
                reward.reward.maxPerUserPerStream > 0)
            {
                reward.reward.userRedemptionsThisStream =
                    std::max(reward.reward.userRedemptionsThisStream,
                             reward.reward.maxPerUserPerStream);
            }
            this->handleRedeemFailure(*channel, reward, attempt, description,
                                      true);
            this->scheduleReload(login, 1500);
            return;

        case RedeemOutcome::Stop:
            this->handleRedeemFailure(
                *channel, reward, attempt,
                QStringLiteral("%1 (%2)").arg(description, result->errorCode),
                false);
            return;
    }

    // Retries exhausted.
    this->handleRedeemFailure(*channel, reward, attempt, description, true);
}

void ChannelPointsController::retryLater(const QString &login,
                                         const QString &rewardId,
                                         Attempt attempt, int delayMs)
{
    QTimer::singleShot(delayMs, this, [this, login, rewardId, attempt] {
        auto *channel = this->findChannel(login);
        if (channel == nullptr)
        {
            return;
        }
        auto it = channel->rewards.find(rewardId);
        if (it == channel->rewards.end() || !it->inFlight ||
            it->inFlight->transactionId != attempt.transactionId)
        {
            return;
        }
        it->inFlight.reset();
        this->startAttempt(*channel, it.value(), attempt);
    });
}

void ChannelPointsController::handleRedeemSuccess(ChannelRuntime &channel,
                                                  RewardRuntime &reward,
                                                  const Attempt &attempt,
                                                  const QString &note)
{
    logInfo(QStringLiteral("Redeemed #%1 \"%2\" for %3 (tx=%4)%5")
                .arg(channel.login, reward.reward.title)
                .arg(attempt.confirmedCost)
                .arg(attempt.transactionId,
                     note.isEmpty() ? QString() : QStringLiteral(", ") + note));

    reward.consecutiveFailures = 0;
    reward.lastResult =
        makeResult(ChannelPointResult::Kind::Success, QStringLiteral("Redeemed"));

    // Live updates correct these shortly; update now so the UI and the queue
    // don't act on stale numbers in the meantime.
    if (channel.balance)
    {
        channel.balance = std::max(0, *channel.balance - attempt.confirmedCost);
    }
    if (reward.reward.userRedemptionsThisStream >= 0)
    {
        reward.reward.userRedemptionsThisStream++;
    }

    if (!attempt.manual)
    {
        if (reward.queueMode == ChannelPointQueueMode::Once)
        {
            reward.queueMode = ChannelPointQueueMode::None;
            reward.queuedInput.clear();
        }
        else if (reward.queueMode == ChannelPointQueueMode::Repeat)
        {
            reward.nextAttemptAt =
                QDateTime::currentDateTimeUtc().addMSecs(REPEAT_GAP_MS);
        }
    }

    getApp()->getSound()->play(successSoundUrl());

    this->scheduleReload(channel.login, 1500);
    this->updateTicking();
    this->updateTopics();
    this->emitChanged(channel.login);
}

void ChannelPointsController::handleRedeemFailure(ChannelRuntime &channel,
                                                  RewardRuntime &reward,
                                                  const Attempt &attempt,
                                                  const QString &message,
                                                  bool keepQueue)
{
    logWarning(QStringLiteral("Redeem failed: #%1 \"%2\": %3 (tx=%4)")
                   .arg(channel.login, reward.reward.title, message,
                        attempt.transactionId));

    reward.lastResult = makeResult(ChannelPointResult::Kind::Error, message);

    if (!attempt.manual && reward.queueMode != ChannelPointQueueMode::None)
    {
        if (keepQueue)
        {
            reward.consecutiveFailures++;
            reward.nextAttemptAt = QDateTime::currentDateTimeUtc().addMSecs(
                waitBackoffMs(reward.consecutiveFailures));
        }
        else
        {
            this->stopQueue(reward, message);
        }
    }

    this->updateTicking();
    this->updateTopics();
    this->emitChanged(channel.login);
}

void ChannelPointsController::stopQueue(RewardRuntime &reward,
                                        const QString &reason)
{
    logWarning(QStringLiteral("Queue stopped: \"%1\": %2")
                   .arg(reward.reward.title, reason));
    reward.queueMode = ChannelPointQueueMode::None;
    reward.queuedInput.clear();
    reward.lastResult = makeResult(ChannelPointResult::Kind::Error,
                                   QStringLiteral("Queue stopped: %1").arg(reason));
}

void ChannelPointsController::cancelAllQueuesInternal(const QString &reason)
{
    for (auto &channel : this->channels_)
    {
        for (auto &reward : channel.rewards)
        {
            if (reward.queueMode != ChannelPointQueueMode::None)
            {
                this->stopQueue(reward, reason);
            }
        }
    }
    this->updateTicking();
}

}  // namespace chatterino
