// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageSimilarity.hpp"

#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "Test.hpp"

#include <QDateTime>

#include <memory>
#include <vector>

namespace chatterino {

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    AccountController accounts;
};

MessagePtr makeMessage(const QDateTime &time)
{
    auto message = std::make_shared<Message>();
    message->loginName = "forsen";
    message->messageText = "i'm chicken";
    message->timestamp = time;
    return message;
}

}  // namespace

TEST(MessageSimilarity, R9KUsesMessageTimestamps)
{
    MockApplication app;
    app.settings.similarityEnabled = true;
    app.settings.hideSimilarMyself = true;
    app.settings.hideSimilarBySameUser = true;
    app.settings.hideSimilarMaxDelay = 5;
    app.settings.hideSimilarMaxMessagesToCheck = 10;
    app.settings.similarityPercentage = 0.9F;

    const auto time =
        QDateTime::fromString("1984-04-20T23:59:58Z", Qt::ISODate);
    auto previous = makeMessage(time);
    std::vector<MessagePtr> messages{previous};

    auto withinDelay = makeMessage(time.addSecs(4));
    setSimilarityFlags(withinDelay, messages);
    EXPECT_TRUE(withinDelay->flags.has(MessageFlag::Similar));

    auto beyondDelay = makeMessage(time.addSecs(6));
    setSimilarityFlags(beyondDelay, messages);
    EXPECT_FALSE(beyondDelay->flags.has(MessageFlag::Similar));
}

TEST(MessageSimilarity, ChannelChecksOnlyConfiguredTailAndCanDisableFiltering)
{
    MockApplication app;
    app.settings.similarityEnabled = true;
    app.settings.hideSimilarMyself = true;
    app.settings.hideSimilarMaxMessagesToCheck = 1;
    app.settings.similarityPercentage = 0.9F;
    const auto time =
        QDateTime::fromString("2026-10-22T12:00:00Z", Qt::ISODate);
    Channel channel("test", Channel::Type::None, 10);
    channel.addMessage(makeMessage(time), MessageContext::Original);
    auto different = std::make_shared<Message>();
    different->loginName = "other";
    different->messageText = "zzzzzzzzzz";
    different->serverReceivedTime = time;
    channel.addMessage(different, MessageContext::Original);
    auto candidate = makeMessage(time.addSecs(1));
    channel.applySimilarityFilters(candidate);
    EXPECT_FALSE(candidate->flags.has(MessageFlag::Similar));
    app.settings.hideSimilarMaxMessagesToCheck = 2;
    channel.applySimilarityFilters(candidate);
    EXPECT_TRUE(candidate->flags.has(MessageFlag::Similar));
    app.settings.similarityEnabled = false;
    auto disabled = makeMessage(time.addSecs(1));
    channel.applySimilarityFilters(disabled);
    EXPECT_FALSE(disabled->flags.has(MessageFlag::Similar));
}

TEST(ChannelHistory, PerChannelOverridesFollowDefaultAndResizeLive)
{
    MockApplication app;
    app.settings.scrollbackSplitLimit = 100;
    app.settings.channelScrollbackOverrides = "{}";
    Channel ordinary("ordinary", Channel::Type::None);
    Channel chosen("chosen", Channel::Type::Twitch);
    EXPECT_EQ(chosen.messageLimit(), 100);
    app.settings.setChannelScrollbackOverride("CHOSEN", 200);
    EXPECT_EQ(chosen.messageLimit(), 200);
    EXPECT_EQ(ordinary.messageLimit(), 100);
    app.settings.scrollbackSplitLimit = 300;
    EXPECT_EQ(ordinary.messageLimit(), 300);
    EXPECT_EQ(chosen.messageLimit(), 200);
    app.settings.setChannelScrollbackOverride("chosen", 0);
    EXPECT_EQ(chosen.messageLimit(), 300);
    EXPECT_FALSE(app.settings.channelScrollbackOverride("chosen"));
}

TEST(ChannelHistory, ResizeAndBatchRefillPreserveNewestUniqueMessages)
{
    MockApplication app;
    Channel channel("test", Channel::Type::None, 4);
    const auto time =
        QDateTime::fromString("2026-10-22T12:00:00Z", Qt::ISODate);
    auto make = [&](int n) {
        auto message = std::make_shared<Message>();
        message->id = QString::number(n);
        message->serverReceivedTime = time.addSecs(n);
        return message;
    };
    channel.addMessage(make(1), MessageContext::Original);
    channel.addMessage(make(4), MessageContext::Original);
    channel.fillInMissingMessages({make(2), make(3), make(3), make(5)});
    auto snapshot = channel.getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 4);
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        EXPECT_EQ(snapshot[i]->id, QString::number(i + 2));
    }
    channel.setMessageLimit(2);
    snapshot = channel.getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot.front()->id, "4");
    EXPECT_EQ(snapshot.back()->id, "5");
}

TEST(ChannelHistory, IDIndexTracksWrapAndReplacement)
{
    MockApplication app;
    Channel channel("test", Channel::Type::None, 10000);
    for (int i = 0; i < 10000; ++i)
    {
        auto message = std::make_shared<Message>();
        message->id = QString::number(i);
        channel.addMessage(message, MessageContext::Original);
    }
    EXPECT_NE(channel.findMessageByID(u"0"), nullptr);
    auto newest = std::make_shared<Message>();
    newest->id = "new";
    channel.addMessage(newest, MessageContext::Original);
    EXPECT_EQ(channel.findMessageByID(u"0"), nullptr);
    EXPECT_EQ(channel.findMessageByID(u"new"), newest);
    auto replacement = std::make_shared<Message>();
    replacement->id = "replacement";
    channel.replaceMessage(newest, replacement);
    EXPECT_EQ(channel.findMessageByID(u"new"), nullptr);
    EXPECT_EQ(channel.findMessageByID(u"replacement"), replacement);
}

}  // namespace chatterino
