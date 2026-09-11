// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace chatterino {

enum class RaidPhase : std::uint8_t {
    Active,
    GoneThrough,
    Cancelled,
};

struct RaidInfo {
    QString id;
    QString targetId;
    QString targetLogin;
    QString targetDisplayName;
    QString targetProfileImage;
    int viewerCount = 0;
    int forceRaidNowSeconds = 0;
    int transitionJitterSeconds = 0;

    [[nodiscard]] QString targetName() const
    {
        return this->targetDisplayName.isEmpty() ? this->targetLogin
                                                 : this->targetDisplayName;
    }
};

struct RaidState {
    RaidInfo info;
    RaidPhase phase = RaidPhase::Active;
    QDateTime lastUpdateAt;
    QString streamTitle;
    QString gameName;
    int streamViewerCount = 0;
    bool live = false;
    bool streamInfoFetched = false;
    bool announcedStart = false;
    bool autoFollowConsumed = false;

    [[nodiscard]] int remainingSeconds() const
    {
        if (this->info.forceRaidNowSeconds <= 0 ||
            !this->lastUpdateAt.isValid())
        {
            return this->info.forceRaidNowSeconds;
        }

        const auto elapsed = this->lastUpdateAt.secsTo(
            QDateTime::currentDateTimeUtc());
        return std::max(0, this->info.forceRaidNowSeconds -
                               static_cast<int>(elapsed));
    }
};

}  // namespace chatterino
