// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/pubsubmessages/Raid.hpp"

#include "util/QMagicEnum.hpp"

namespace chatterino {

PubSubRaidMessage::PubSubRaidMessage(const QJsonObject &root)
    : typeString(root.value("type").toString())
{
    auto oType = qmagicenum::enumCast<Type>(this->typeString);
    if (oType.has_value())
    {
        this->type = oType.value();
    }
    else if (this->typeString.startsWith(QLatin1String("raid_update")))
    {
        this->type = Type::Update;
    }
    else if (this->typeString.startsWith(QLatin1String("raid_go")))
    {
        this->type = Type::Go;
    }
    else if (this->typeString.startsWith(QLatin1String("raid_cancel")))
    {
        this->type = Type::Cancel;
    }

    const auto raidObj = root.value("raid").toObject();
    this->raid.id = raidObj.value("id").toString();
    this->raid.targetId = raidObj.value("target_id").toString();
    this->raid.targetLogin = raidObj.value("target_login").toString();
    this->raid.targetDisplayName =
        raidObj.value("target_display_name").toString();
    this->raid.targetProfileImage =
        raidObj.value("target_profile_image").toString();
    this->raid.viewerCount = raidObj.value("viewer_count").toInt();
    this->raid.forceRaidNowSeconds =
        raidObj.value("force_raid_now_seconds").toInt();
    this->raid.transitionJitterSeconds =
        raidObj.value("transition_jitter_seconds").toInt();
}

}  // namespace chatterino
