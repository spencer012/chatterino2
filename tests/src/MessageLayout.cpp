// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/helper/ChannelView.hpp"

#include <QDebug>
#include <QString>

#include <memory>

using namespace chatterino;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args_, this->paths_, this->settings, this->theme,
                        this->fonts)
    {
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    AccountController accounts;
    WindowManager windowManager;
    mock::MockTwitchIrcServer twitch;
};

constexpr int WIDTH = 300;

class MessageLayoutTest
{
public:
    // "aaaaaaaa bbbbbbbb cccccccc"
    MessageLayoutTest(const QString &text)
    {
        MessageBuilder builder;
        builder.append(
            std::make_unique<TextElement>(text, MessageElementFlag::Text));
        this->layout = std::make_unique<MessageLayout>(builder.release());
        MessageColors colors;
        this->layout->layout(
            {
                .messageColors = colors,
                .flags = MessageElementFlag::Text,
                .width = WIDTH,
                .scale = 1,
                .imageScale = 1,
            },
            false);
    }

    MockApplication mockApplication;
    std::unique_ptr<MessageLayout> layout;
};

}  // namespace

TEST(TextElement, BasicCase)
{
    auto test = MessageLayoutTest("abc");

    // Simulate we are clicking on the first word
    auto point = QPoint(WIDTH / 20, test.layout->getHeight() / 2);

    const auto *hoveredElement = test.layout->getElementAt(point);
    ASSERT_NE(hoveredElement, nullptr);

    const auto [wordStart, wordEnd] =
        test.layout->getWordBounds(hoveredElement, point);

    EXPECT_EQ(wordStart, 0);
    EXPECT_EQ(wordEnd, 3);
}

TEST(TextElement, ReleasedOffscreenLayoutRebuildsSelectionAndGeometry)
{
    auto test = MessageLayoutTest("abc def");
    const auto height = test.layout->getHeight();
    QString before;
    test.layout->addSelectionText(before);
    test.layout->deleteCache();
    EXPECT_EQ(test.layout->getHeight(), height);
    EXPECT_EQ(test.layout->getElementAt(QPointF(10, height / 2)), nullptr);
    MessageColors colors;
    EXPECT_TRUE(test.layout->layout({.messageColors = colors,
                                     .flags = MessageElementFlag::Text,
                                     .width = WIDTH,
                                     .scale = 1,
                                     .imageScale = 1},
                                    false));
    QString after;
    test.layout->addSelectionText(after);
    EXPECT_EQ(after, before);
    EXPECT_EQ(test.layout->getHeight(), height);
}

TEST(ChannelViewHistory, PauseWrapResizeAndClearPreserveIdentity)
{
    MockApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 3);
    ChannelView view(nullptr);
    view.setPausable(true);
    view.setChannel(channel);
    auto add = [&](int n) {
        MessageBuilder builder;
        builder->id = QString::number(n);
        builder.emplace<TextElement>(QString::number(n),
                                     MessageElementFlag::Text);
        channel->addMessage(builder.release(), MessageContext::Original);
    };
    add(0);
    add(1);
    add(2);
    const auto first = view.getMessagesSnapshot().front();
    view.pause(PauseReason::Selection);
    add(3);
    EXPECT_EQ(view.getMessagesSnapshot().front(), first);
    channel->setMessageLimit(2);
    EXPECT_EQ(view.getMessagesSnapshot().size(), 3);
    view.unpause(PauseReason::Selection);
    ASSERT_EQ(view.getMessagesSnapshot().size(), 2);
    EXPECT_EQ(view.getMessagesSnapshot().front()->getMessagePtr()->id, "2");
    EXPECT_EQ(view.getMessagesSnapshot().back()->getMessagePtr()->id, "3");
    view.pause(PauseReason::Selection);
    channel->clearMessages();
    EXPECT_TRUE(view.getMessagesSnapshot().empty());
}

TEST(ChannelViewHistory, ReplayIndexAppendsAtCapacityAndRebuildsOnReplacement)
{
    MockApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 3);
    ChannelView view(nullptr);
    view.setChannel(channel);
    const auto time =
        QDateTime::fromString("2026-10-22T12:00:00Z", Qt::ISODate);
    auto make = [&](int n) {
        MessageBuilder builder;
        builder->serverReceivedTime = time.addSecs(n);
        builder.emplace<TextElement>(QString::number(n),
                                     MessageElementFlag::Text);
        return builder.release();
    };
    for (int i = 0; i < 3; ++i)
    {
        channel->addMessage(make(i), MessageContext::Original);
    }
    view.scrollToServerTime(time.toMSecsSinceEpoch());
    for (int i = 3; i < 20; ++i)
    {
        channel->addMessage(make(i), MessageContext::Original);
        view.scrollToServerTime(time.addSecs(i - 1).toMSecsSinceEpoch());
    }
    EXPECT_TRUE(view.cacheDiagnostics().contains("Replay index rebuilds: 1"));
    EXPECT_TRUE(
        view.cacheDiagnostics().contains("Replay entries appended: 17"));
    channel->replaceMessage(size_t{1}, make(15));
    view.scrollToServerTime(time.addSecs(16).toMSecsSinceEpoch());
    EXPECT_TRUE(view.cacheDiagnostics().contains("Replay index rebuilds: 2"));
}
