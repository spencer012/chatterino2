// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsTypes.hpp"

#include <QDateTime>
#include <QLocale>
#include <QString>

#include <algorithm>
#include <optional>

/// Text formatting shared by the channel points popup and confirm dialog.
namespace chatterino::channelpoints::format {

inline QString points(int value)
{
    return QLocale().toString(value);
}

/// "45s", "3:05", "1:02:03"
inline QString duration(qint64 seconds)
{
    seconds = std::max<qint64>(0, seconds);
    if (seconds < 60)
    {
        return QStringLiteral("%1s").arg(seconds);
    }
    const auto h = seconds / 3600;
    const auto m = (seconds % 3600) / 60;
    const auto s = seconds % 60;
    if (h > 0)
    {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

inline QString timeUntil(const QDateTime &when)
{
    return duration(QDateTime::currentDateTimeUtc().secsTo(when) + 1);
}

/// Why the reward isn't available, with details where we have them,
/// e.g. "On cooldown · 1:23" or "Need 1,200 more points".
inline QString blockDetail(const ChannelPointRewardView &view,
                           std::optional<int> balance)
{
    const auto &reward = view.reward;
    switch (view.block)
    {
        case RewardBlock::Cooldown:
            if (reward.cooldownExpiresAt.isValid())
            {
                return QStringLiteral("On cooldown · %1")
                    .arg(timeUntil(reward.cooldownExpiresAt));
            }
            break;

        case RewardBlock::NotEnoughPoints:
            if (balance)
            {
                return QStringLiteral("Need %1 more points")
                    .arg(points(reward.cost - *balance));
            }
            break;

        case RewardBlock::UserStreamLimit:
            if (reward.maxPerUserPerStream > 0)
            {
                return QStringLiteral("You've used %1 of %1 this stream")
                    .arg(reward.maxPerUserPerStream);
            }
            break;

        case RewardBlock::StreamLimit:
            if (reward.maxPerStream > 0)
            {
                return QStringLiteral("All %1 redeemed this stream")
                    .arg(reward.maxPerStream);
            }
            break;

        default:
            break;
    }
    return describeRewardBlock(view.block);
}

}  // namespace chatterino::channelpoints::format
