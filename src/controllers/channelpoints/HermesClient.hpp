// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/websockets/WebSocketPool.hpp"
#include "util/ExponentialBackoff.hpp"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace chatterino {

/// Minimal client for Twitch's Hermes websocket (the successor of PubSub that
/// twitch.tv uses). Delivers PubSub topic messages for a set of topics.
///
/// The connection is only open while there's a token and at least one topic.
class HermesClient : public QObject
{
    Q_OBJECT

public:
    explicit HermesClient(QObject *parent = nullptr);
    ~HermesClient() override;

    /// Sets the OAuth token used to authenticate. An empty token disconnects.
    void setToken(const QString &token);
    void setTopics(const QSet<QString> &topics);
    void stop();

    /// True once the connection is authenticated.
    [[nodiscard]] bool isConnected() const;

Q_SIGNALS:
    void connectedChanged(bool connected);
    void notification(const QString &topic, const QJsonObject &message);
    void authenticationFailed(const QString &error);

private:
    enum class State {
        Closed,
        Connecting,
        Authenticating,
        Open,
    };

    class Listener;

    void sync();
    void open(const QUrl &url);
    void closeSocket();
    void setState(State state);
    void subscribe(const QString &topic);
    void scheduleReconnect(const QString &reason);
    void restartWatchdog();

    void handleOpen(quint64 generation);
    void handleMessage(quint64 generation, const QByteArray &data);
    void handleClose(quint64 generation);

    WebSocketPool pool_{"hermes"};
    WebSocketHandle socket_;
    quint64 generation_ = 0;
    State state_ = State::Closed;
    bool stopped_ = false;
    bool authRejected_ = false;

    QString token_;
    QSet<QString> topics_;
    /// subscription ID -> topic
    QHash<QString, QString> subscriptions_;
    QSet<QString> subscribedTopics_;
    QString authRequestId_;

    int keepaliveSec_ = 15;
    QTimer watchdog_;
    QTimer reconnectTimer_;
    QUrl reconnectUrl_;
    ExponentialBackoff<7> backoff_{std::chrono::milliseconds{1000}};
};

}  // namespace chatterino
