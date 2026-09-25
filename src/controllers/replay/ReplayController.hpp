// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>
#include <cstdint>
#include <string>

namespace chatterino {
class ReplayServer;
class Settings;

struct ReplayInstance {
    QString channel;
    qint64 wallMs = 0;
    double speed = 1;
    bool paused = false;
    qint64 liveMs = 0;
    bool seeked = false;
    QDateTime lastUpdate;
};

class ReplayController : public QObject
{
public:
    explicit ReplayController(Settings &settings);
    ~ReplayController() override;

    pajlada::Signals::Signal<QString> positionUpdated;
    pajlada::Signals::Signal<QString> instanceRemoved;

    const ReplayInstance *instance(const QString &id) const;
    QString latestInstanceForChannel(const QString &channel) const;

private:
    void restart();
    void receive(std::uint64_t connection, const std::string &payload);

    Settings &settings_;
    std::unique_ptr<ReplayServer> server_;
    QHash<QString, ReplayInstance> instances_;
    QHash<std::uint64_t, QString> connections_;
    std::uint64_t generation_ = 0;
    pajlada::Signals::SignalHolder connectionsHolder_;
};
}  // namespace chatterino
