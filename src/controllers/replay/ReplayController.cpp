// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT
#include "controllers/replay/ReplayController.hpp"

#include "controllers/replay/ReplayServer.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

namespace chatterino {

ReplayController::ReplayController(Settings &settings)
    : settings_(settings)
{
    this->settings_.replayServerEnabled.connect(
        [this](bool) { this->restart(); }, this->connectionsHolder_);
    this->settings_.replayServerPort.connect(
        [this](int) { this->restart(); }, this->connectionsHolder_);
    this->restart();
}

ReplayController::~ReplayController()
{
    this->server_.reset();
}

void ReplayController::restart()
{
    ++this->generation_;
    this->server_.reset();
    for (auto it = this->instances_.cbegin(); it != this->instances_.cend(); ++it)
    {
        this->instanceRemoved.invoke(it.key());
    }
    this->instances_.clear();
    this->connections_.clear();
    if (!this->settings_.replayServerEnabled)
    {
        return;
    }
    const auto port = this->settings_.replayServerPort.getValue();
    if (port < 1 || port > 65535)
    {
        return;
    }
    QPointer<ReplayController> self(this);
    const auto generation = this->generation_;
    this->server_ = std::make_unique<ReplayServer>(
        port, [self, generation](std::uint64_t connection, const std::string &payload) {
            runInGuiThread([self, generation, connection, payload] {
                if (self && self->generation_ == generation)
                {
                    self->receive(connection, payload);
                }
            });
        });
}

void ReplayController::receive(std::uint64_t connection,
                               const std::string &payload)
{
    if (payload.empty())
    {
        const auto id = this->connections_.take(connection);
        if (!id.isEmpty())
        {
            this->instances_.remove(id);
            this->instanceRemoved.invoke(id);
        }
        return;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(
        QByteArray(payload.data(), static_cast<qsizetype>(payload.size())),
        &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
    {
        return;
    }
    const auto obj = doc.object();
    const auto type = obj.value("type").toString();
    const auto id = obj.value("instance").toString();
    if (id.isEmpty())
    {
        return;
    }
    if (type == "hello")
    {
        const auto channel = obj.value("channel").toString().toLower();
        if (channel.isEmpty())
        {
            return;
        }
        for (auto it = this->connections_.begin();
             it != this->connections_.end();)
        {
            if (it.value() == id)
            {
                it = this->connections_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        this->connections_.insert(connection, id);
        auto &state = this->instances_[id];
        state.channel = channel;
        state.lastUpdate = QDateTime::currentDateTimeUtc();
        this->positionUpdated.invoke(id);
    }
    else if (type == "position" && this->connections_.value(connection) == id)
    {
        auto &state = this->instances_[id];
        state.wallMs = obj.value("wall_ms").toVariant().toLongLong();
        state.liveMs = obj.value("live_ms").toVariant().toLongLong();
        state.speed = obj.value("speed").toDouble(1);
        state.paused = obj.value("paused").toBool();
        state.seeked = obj.value("seeked").toBool();
        state.lastUpdate = QDateTime::currentDateTimeUtc();
        this->positionUpdated.invoke(id);
    }
    else if (type == "bye" && this->connections_.value(connection) == id)
    {
        this->connections_.remove(connection);
        this->instances_.remove(id);
        this->instanceRemoved.invoke(id);
    }
}

const ReplayInstance *ReplayController::instance(const QString &id) const
{
    const auto it = this->instances_.constFind(id);
    return it == this->instances_.cend() ? nullptr : &it.value();
}

QString ReplayController::latestInstanceForChannel(const QString &channel) const
{
    QString best;
    QDateTime latest;
    for (auto it = this->instances_.cbegin(); it != this->instances_.cend(); ++it)
    {
        if (it.value().channel.compare(channel, Qt::CaseInsensitive) == 0 &&
            (best.isEmpty() || it.value().lastUpdate > latest))
        {
            best = it.key();
            latest = it.value().lastUpdate;
        }
    }
    return best;
}
}  // namespace chatterino
