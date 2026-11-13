// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// Timings for searching and copying large message histories. These are
// disabled by default, run them with
//   chatterino-test --gtest_also_run_disabled_tests --gtest_filter=*SearchPerformance*
// in a release build.

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/SearchPopup.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>

#include <cstdio>

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

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    AccountController accounts;
    WindowManager windowManager;
    mock::MockTwitchIrcServer twitch;
    EmoteController emotes;
    HotkeyController hotkeys;
};

MessagePtr makeMessage(size_t i)
{
    static const QStringList words{
        "KEKW", "Pog", "hello", "chat", "what", "LUL", "is", "this", "stream",
    };
    auto message = std::make_shared<Message>();
    auto user = QString("user%1").arg(i % 5000);
    message->loginName = user;
    message->displayName = user;
    message->channelName = "test";
    message->id = QString::number(i);
    message->messageText =
        QString("%1 %2 %3 message %4")
            .arg(words[i % words.size()], words[(i / 7) % words.size()],
                 words[(i / 13) % words.size()], QString::number(i));
    message->searchText = user + ": " + message->messageText;
    message->timestamp = QDateTime::fromMSecsSinceEpoch(
        1'700'000'000'000 + static_cast<qint64>(i) * 50);
    message->elements.push_back(std::make_unique<TextElement>(
        message->messageText, MessageElementFlag::Text));
    return message;
}

/// Runs the event loop until @a done, and returns the longest time a single
/// pass of the event loop blocked the GUI thread.
qint64 pumpUntil(const std::function<bool()> &done, qint64 &totalMs)
{
    QElapsedTimer total;
    total.start();
    qint64 longest = 0;
    while (!done() && total.elapsed() < 60'000)
    {
        QElapsedTimer pass;
        pass.start();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        longest = std::max(longest, pass.elapsed());
    }
    totalMs = total.elapsed();
    return longest;
}

ChannelView *resultView(SearchPopup &popup)
{
    return popup.findChild<ChannelView *>();
}

size_t resultCount(SearchPopup &popup)
{
    return resultView(popup)->getMessagesSnapshot().size();
}

std::vector<const Message *> resultMessages(SearchPopup &popup)
{
    std::vector<const Message *> messages;
    for (const auto &layout : resultView(popup)->getMessagesSnapshot())
    {
        messages.push_back(layout->getMessage());
    }
    return messages;
}

}  // namespace

TEST(SearchPerformance, DISABLED_LargeHistory)
{
    constexpr size_t COUNT = 500'000;
    MockApplication app;

    std::vector<MessagePtr> messages;
    messages.reserve(COUNT);
    for (size_t i = 0; i < COUNT; ++i)
    {
        messages.push_back(makeMessage(i));
    }

    auto channel =
        std::make_shared<Channel>("test", Channel::Type::None, COUNT);
    for (const auto &message : messages)
    {
        channel->addMessage(message, MessageContext::Original);
    }

    QElapsedTimer timer;
    timer.start();
    {
        auto copy = channel->getMessageSnapshot();
        std::printf("copy channel snapshot: %lld ms\n",
                    static_cast<long long>(timer.elapsed()));
    }

    {
        timer.restart();
        auto prepared =
            ChannelView::prepareMessages(channel->getMessageSnapshot(), false);
        std::printf("prepare layouts: %lld ms\n",
                    static_cast<long long>(timer.elapsed()));
        timer.restart();
        auto copy =
            std::make_shared<Channel>("copy", Channel::Type::None, COUNT);
        copy->resetMessages(std::move(prepared.messages));
        std::printf("fill channel copy: %lld ms\n",
                    static_cast<long long>(timer.elapsed()));
        timer.restart();
        prepared.layouts.clear();
        std::printf("free layouts: %lld ms\n",
                    static_cast<long long>(timer.elapsed()));
        timer.restart();
        copy.reset();
        prepared.messages.clear();
        std::printf("free channel copy and messages: %lld ms\n",
                    static_cast<long long>(timer.elapsed()));
    }

    ChannelView source(nullptr, ChannelView::Context::None, COUNT);
    source.resize(400, 600);
    timer.restart();
    source.setChannel(channel);
    std::printf("split setChannel(%zu): %lld ms\n", COUNT,
                static_cast<long long>(timer.elapsed()));

    {
        SearchPopup popup(nullptr);
        popup.addChannel(source);
        timer.restart();
        popup.show();
        auto showMs = timer.elapsed();
        qint64 totalMs = 0;
        auto longest = pumpUntil(
            [&] {
                return resultCount(popup) == COUNT;
            },
            totalMs);
        std::printf("open search: show() %lld ms, results after %lld ms, "
                    "longest event loop pass %lld ms\n",
                    static_cast<long long>(showMs),
                    static_cast<long long>(totalMs),
                    static_cast<long long>(longest));
        EXPECT_EQ(resultCount(popup), COUNT);

        auto *input = popup.findChild<QLineEdit *>();
        ASSERT_NE(input, nullptr);
        const std::pair<QString, size_t> queries[] = {
            {"KEKW", 0},
            {"KEKW Pog", 0},
            {"from:user42", 0},
            {"", COUNT},
        };
        for (auto [query, expected] : queries)
        {
            if (expected == 0)
            {
                for (const auto &message : messages)
                {
                    bool match = true;
                    for (const auto &word : query.split(' '))
                    {
                        if (word.startsWith("from:"))
                        {
                            match = match && message->loginName == word.mid(5);
                        }
                        else
                        {
                            match = match && message->searchText.contains(
                                                 word, Qt::CaseInsensitive);
                        }
                    }
                    expected += match ? 1 : 0;
                }
            }
            timer.restart();
            input->setText(query);
            auto setTextMs = timer.elapsed();
            longest = pumpUntil(
                [&] {
                    return resultCount(popup) == expected;
                },
                totalMs);
            std::printf("type \"%s\" (%zu results): setText %lld ms, results "
                        "after %lld ms, longest event loop pass %lld ms\n",
                        qPrintable(query), expected,
                        static_cast<long long>(setTextMs),
                        static_cast<long long>(totalMs),
                        static_cast<long long>(longest));
            EXPECT_EQ(resultCount(popup), expected);
        }

        timer.restart();
    }
    std::printf("close search: %lld ms\n",
                static_cast<long long>(timer.elapsed()));
}

TEST(SearchPopup, ThreadedSearchMatchesSequentialOrder)
{
    constexpr size_t COUNT = 40'000;
    MockApplication app;

    auto first = std::make_shared<Channel>("test", Channel::Type::None, COUNT);
    auto second = std::make_shared<Channel>("test", Channel::Type::None, COUNT);
    std::vector<MessagePtr> messages;
    for (size_t i = 0; i < COUNT; ++i)
    {
        messages.push_back(makeMessage(i));
        // Every third message is in both channels, the others alternate
        if (i % 3 == 0 || i % 2 == 0)
        {
            first->addMessage(messages.back(), MessageContext::Original);
        }
        if (i % 3 == 0 || i % 2 == 1)
        {
            second->addMessage(messages.back(), MessageContext::Original);
        }
    }

    ChannelView firstView(nullptr, ChannelView::Context::None, COUNT);
    firstView.setChannel(first);
    ChannelView secondView(nullptr, ChannelView::Context::None, COUNT);
    secondView.setChannel(second);

    auto expectQuery = [&](SearchPopup &popup,
                           const std::function<bool(const Message &)> &match,
                           const std::vector<MessagePtr> &source) {
        std::vector<const Message *> expected;
        for (const auto &message : source)
        {
            if (match(*message))
            {
                expected.push_back(message.get());
            }
        }
        qint64 totalMs = 0;
        pumpUntil(
            [&] {
                return resultMessages(popup) == expected;
            },
            totalMs);
        EXPECT_EQ(resultMessages(popup), expected);
    };

    {
        SearchPopup popup(nullptr);
        popup.addChannel(firstView);
        popup.show();
        expectQuery(
            popup,
            [](const auto &) {
                return true;
            },
            first->getMessageSnapshot());

        popup.findChild<QLineEdit *>()->setText("pog from:user7");
        expectQuery(
            popup,
            [](const Message &m) {
                return m.loginName == "user7" &&
                       m.searchText.contains("pog", Qt::CaseInsensitive);
            },
            first->getMessageSnapshot());

        popup.findChild<QLineEdit *>()->setText(R"(regex:\d5$)");
        expectQuery(
            popup,
            [](const Message &m) {
                const auto &text = m.messageText;
                return text.size() >= 2 && text.endsWith('5') &&
                       text.at(text.size() - 2).isDigit();
            },
            first->getMessageSnapshot());
    }

    {
        SearchPopup popup(nullptr);
        popup.addChannel(firstView);
        popup.addChannel(secondView);
        popup.show();
        // Duplicates are removed and the merged history is sorted by time
        expectQuery(
            popup,
            [](const auto &) {
                return true;
            },
            messages);

        popup.findChild<QLineEdit *>()->setText("KEKW");
        expectQuery(
            popup,
            [](const Message &m) {
                return m.searchText.contains("KEKW", Qt::CaseInsensitive);
            },
            messages);
    }
}
