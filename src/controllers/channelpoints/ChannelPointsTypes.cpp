// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsTypes.hpp"

namespace chatterino {

RewardBlock evaluateReward(const PointsReward &reward,
                           std::optional<int> balance, const QDateTime &now)
{
    if (!reward.isEnabled)
    {
        return RewardBlock::Disabled;
    }
    if (reward.isPaused)
    {
        return RewardBlock::Paused;
    }

    const auto cooldownValid = reward.cooldownExpiresAt.isValid();
    if (cooldownValid && reward.cooldownExpiresAt > now)
    {
        return RewardBlock::Cooldown;
    }

    if (!reward.isInStock)
    {
        // Twitch marks rewards as out of stock while their global cooldown
        // runs, but doesn't always announce when it's back in stock.
        const auto cooldownOver = reward.hasGlobalCooldown && cooldownValid &&
                                  reward.cooldownExpiresAt <= now;
        if (!cooldownOver)
        {
            if (reward.maxPerStream > 0 &&
                reward.redemptionsThisStream >= reward.maxPerStream)
            {
                return RewardBlock::StreamLimit;
            }
            return RewardBlock::OutOfStock;
        }
    }

    if (reward.maxPerUserPerStream > 0 &&
        reward.userRedemptionsThisStream >= reward.maxPerUserPerStream)
    {
        return RewardBlock::UserStreamLimit;
    }

    if (balance && *balance < reward.cost)
    {
        return RewardBlock::NotEnoughPoints;
    }

    return RewardBlock::None;
}

QString describeRewardBlock(RewardBlock block)
{
    switch (block)
    {
        case RewardBlock::None:
            return QStringLiteral("Ready");
        case RewardBlock::Disabled:
            return QStringLiteral("Not available");
        case RewardBlock::Paused:
            return QStringLiteral("Paused");
        case RewardBlock::Cooldown:
            return QStringLiteral("On cooldown");
        case RewardBlock::OutOfStock:
            return QStringLiteral("Out of stock");
        case RewardBlock::StreamLimit:
            return QStringLiteral("Stream limit reached");
        case RewardBlock::UserStreamLimit:
            return QStringLiteral("Your limit for this stream reached");
        case RewardBlock::NotEnoughPoints:
            return QStringLiteral("Not enough points");
    }
    return {};
}

}  // namespace chatterino
