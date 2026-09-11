// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/RaidInfo.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QJsonObject>
#include <QString>

#include <cstdint>

namespace chatterino {

struct PubSubRaidMessage {
    enum class Type : std::uint8_t {
        Update,
        Go,
        Cancel,

        INVALID,
    };

    QString typeString;
    Type type = Type::INVALID;
    RaidInfo raid;

    PubSubRaidMessage(const QJsonObject &root);
};

}  // namespace chatterino

template <>
constexpr magic_enum::customize::customize_t
    magic_enum::customize::enum_name<  // NOLINT(readability-identifier-naming)
        chatterino::PubSubRaidMessage::Type>(
        chatterino::PubSubRaidMessage::Type value) noexcept
{
    switch (value)
    {
        case chatterino::PubSubRaidMessage::Type::Update:
            return "raid_update_v2";
        case chatterino::PubSubRaidMessage::Type::Go:
            return "raid_go_v2";
        case chatterino::PubSubRaidMessage::Type::Cancel:
            return "raid_cancel_v2";
        default:
            return default_tag;  // NOLINT(clazy-rule-of-two-soft)
    }
}
