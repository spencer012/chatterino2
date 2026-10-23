// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/HermesClient.hpp"

#include "controllers/channelpoints/ChannelPointsApi.hpp"
#include "controllers/channelpoints/ChannelPointsLog.hpp"
#include "util/PostToThread.hpp"

#include <QPointer>

namespace chatterino {

using namespace channelpoints;

class HermesClient::Listener : public WebSocketListener
{
public:
    Listener(QPointer<HermesClient> client, quint64 generation)
        : client_(std::move(client))
        , generation_(generation)
    {
    }

    void onOpen() override
    {
        runInGuiThread([client = this->client_, gen = this->generation_] {
            if (client)
            {
                client->handleOpen(gen);
            }
        });
    }

    void onTextMessage(QByteArray data) override
    {
        runInGuiThread([client = this->client_, gen = this->generation_,
                        data = std::move(data)] {
            if (client)
            {
                client->handleMessage(gen, data);
            }
        });
    }

    void onBinaryMessage(QByteArray data) override
    {
        this->onTextMessage(std::move(data));
    }

    void onClose(std::unique_ptr<WebSocketListener> /* self */) override
    {
        runInGuiThread([client = this->client_, gen = this->generation_] {
            if (client)
            {
                client->handleClose(gen);
            }
        });
    }

private:
    QPointer<HermesClient> client_;
    quint64 generation_;
};

HermesClient::HermesClient(QObject *parent)
    : QObject(parent)
{
    this->watchdog_.setSingleShot(true);
    QObject::connect(&this->watchdog_, &QTimer::timeout, this, [this] {
        logWarning(QStringLiteral(
            "Hermes: no message within the keepalive window, reconnecting"));
        this->closeSocket();
        this->scheduleReconnect("keepalive timeout");
    });

    this->reconnectTimer_.setSingleShot(true);
    QObject::connect(&this->reconnectTimer_, &QTimer::timeout, this, [this] {
        this->sync();
    });
}

HermesClient::~HermesClient()
{
    this->stop();
}

void HermesClient::setToken(const QString &token)
{
    if (token == this->token_)
    {
        return;
    }

    this->token_ = token;
    this->authRejected_ = false;
    // A new token needs a fresh authentication.
    this->closeSocket();
    this->backoff_.reset();
    this->reconnectTimer_.stop();
    this->sync();
}

void HermesClient::setTopics(const QSet<QString> &topics)
{
    if (topics == this->topics_)
    {
        return;
    }

    this->topics_ = topics;
    this->sync();
}

void HermesClient::stop()
{
    this->stopped_ = true;
    this->reconnectTimer_.stop();
    this->closeSocket();
}

bool HermesClient::isConnected() const
{
    return this->state_ == State::Open;
}

void HermesClient::sync()
{
    const bool wanted = !this->stopped_ && !this->authRejected_ &&
                        !this->token_.isEmpty() && !this->topics_.isEmpty();

    if (!wanted)
    {
        this->reconnectTimer_.stop();
        if (this->state_ != State::Closed)
        {
            logInfo(QStringLiteral("Hermes: disconnecting (no longer needed)"));
            this->closeSocket();
        }
        return;
    }

    if (this->state_ == State::Closed)
    {
        if (!this->reconnectTimer_.isActive())
        {
            auto url = this->reconnectUrl_.isValid() ? this->reconnectUrl_
                                                     : QUrl(HERMES_URL);
            this->reconnectUrl_ = {};
            this->open(url);
        }
        return;
    }

    if (this->state_ == State::Open)
    {
        for (const auto &topic : this->topics_)
        {
            if (!this->subscribedTopics_.contains(topic))
            {
                this->subscribe(topic);
            }
        }
    }
}

void HermesClient::open(const QUrl &url)
{
    this->generation_++;
    this->subscriptions_.clear();
    this->subscribedTopics_.clear();
    this->setState(State::Connecting);

    logInfo(QStringLiteral("Hermes: connecting to %1").arg(url.host()));
    this->socket_ = this->pool_.createSocket(
        WebSocketOptions{
            .url = url,
            .headers = {{"Origin", "https://www.twitch.tv"}},
        },
        std::make_unique<Listener>(this, this->generation_));

    // Also covers a handshake that never completes.
    this->keepaliveSec_ = 15;
    this->restartWatchdog();
}

void HermesClient::closeSocket()
{
    // Ignore events from the old socket from now on.
    this->generation_++;
    this->watchdog_.stop();
    this->socket_.close();
    this->socket_ = {};
    this->subscriptions_.clear();
    this->subscribedTopics_.clear();
    this->setState(State::Closed);
}

void HermesClient::setState(State state)
{
    if (this->state_ == state)
    {
        return;
    }

    const bool wasConnected = this->state_ == State::Open;
    this->state_ = state;
    const bool connected = this->state_ == State::Open;
    if (wasConnected != connected)
    {
        Q_EMIT this->connectedChanged(connected);
    }
}

void HermesClient::subscribe(const QString &topic)
{
    const auto subscriptionId = generateHermesId();
    this->subscriptions_.insert(subscriptionId, topic);
    this->subscribedTopics_.insert(topic);
    this->socket_.sendText(
        buildHermesSubscribe(generateHermesId(), subscriptionId, topic));
}

void HermesClient::scheduleReconnect(const QString &reason)
{
    if (this->stopped_ || this->reconnectTimer_.isActive())
    {
        return;
    }

    auto delay = this->backoff_.next();
    logInfo(QStringLiteral("Hermes: reconnecting in %1 ms (%2)")
                .arg(delay.count())
                .arg(reason));
    this->reconnectTimer_.start(int(delay.count()));
}

void HermesClient::restartWatchdog()
{
    // Twitch sends a keepalive every `keepaliveSec` seconds.
    this->watchdog_.start((this->keepaliveSec_ * 2 + 5) * 1000);
}

void HermesClient::handleOpen(quint64 generation)
{
    if (generation != this->generation_)
    {
        return;
    }
    // Wait for the welcome message before authenticating.
    this->restartWatchdog();
}

void HermesClient::handleMessage(quint64 generation, const QByteArray &data)
{
    if (generation != this->generation_)
    {
        return;
    }

    this->restartWatchdog();

    auto parsed = parseHermesMessage(data);
    if (!parsed)
    {
        logWarning(QStringLiteral("Hermes: unparsable message: %1")
                       .arg(QString::fromUtf8(data.left(300))));
        return;
    }

    const auto &msg = *parsed;
    switch (msg.type)
    {
        case HermesMessage::Type::Welcome: {
            this->keepaliveSec_ = std::max(5, msg.keepaliveSec);
            this->restartWatchdog();
            this->authRequestId_ = generateHermesId();
            this->setState(State::Authenticating);
            this->socket_.sendText(
                buildHermesAuthenticate(this->authRequestId_, this->token_));
        }
        break;

        case HermesMessage::Type::AuthenticateResponse: {
            if (msg.result == "ok")
            {
                logInfo(QStringLiteral("Hermes: authenticated"));
                this->backoff_.reset();
                this->setState(State::Open);
                this->sync();
            }
            else
            {
                const auto error =
                    QStringLiteral("%1 %2").arg(msg.errorCode, msg.error).trimmed();
                logWarning(
                    QStringLiteral("Hermes: authentication failed: %1").arg(error));
                this->authRejected_ = true;
                this->closeSocket();
                Q_EMIT this->authenticationFailed(error);
            }
        }
        break;

        case HermesMessage::Type::SubscribeResponse: {
            if (msg.result != "ok")
            {
                const auto topic = this->subscriptions_.value(msg.subscriptionId);
                logWarning(QStringLiteral("Hermes: subscribing to %1 failed: %2 %3")
                               .arg(topic, msg.errorCode, msg.error));
            }
        }
        break;

        case HermesMessage::Type::Notification: {
            const auto topic = this->subscriptions_.value(msg.subscriptionId);
            // Topics we dropped stay subscribed until the next reconnect.
            if (!topic.isEmpty() && this->topics_.contains(topic))
            {
                Q_EMIT this->notification(topic, msg.pubsub);
            }
        }
        break;

        case HermesMessage::Type::Reconnect: {
            logInfo(QStringLiteral("Hermes: server requested reconnect"));
            this->reconnectUrl_ = QUrl(msg.reconnectUrl);
            this->closeSocket();
            this->sync();
        }
        break;

        case HermesMessage::Type::Keepalive:
        case HermesMessage::Type::Unknown:
            break;
    }
}

void HermesClient::handleClose(quint64 generation)
{
    if (generation != this->generation_)
    {
        return;
    }

    logWarning(QStringLiteral("Hermes: connection closed"));
    this->socket_ = {};
    this->watchdog_.stop();
    this->subscriptions_.clear();
    this->subscribedTopics_.clear();
    this->setState(State::Closed);
    this->scheduleReconnect("connection closed");
}

}  // namespace chatterino
