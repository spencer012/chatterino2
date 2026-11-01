// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/WindowDescriptors.hpp"

#include "Test.hpp"

#include <QJsonObject>

using namespace chatterino;

TEST(SplitDescriptor, BannersHidden)
{
    // Layouts saved before the option existed show the banners.
    const QJsonObject old{
        {"type", "split"},
        {"data", QJsonObject{{"type", "twitch"}, {"name", "forsen"}}},
    };
    EXPECT_FALSE(SplitDescriptor::loadFromJSON(old).bannersHidden_);

    SplitDescriptor descriptor;
    descriptor.type_ = "twitch";
    descriptor.channelName_ = "forsen";
    descriptor.bannersHidden_ = true;
    const auto json = descriptor.toJson();
    EXPECT_TRUE(json.value("bannersHidden").toBool());

    const auto loaded = SplitDescriptor::loadFromJSON(json);
    EXPECT_TRUE(loaded.bannersHidden_);
    EXPECT_EQ(loaded.channelName_, "forsen");
}
