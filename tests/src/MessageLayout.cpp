// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/dialogs/CacheDiagnosticsDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QString>
#include <QTemporaryDir>

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

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    AccountController accounts;
    WindowManager windowManager;
    mock::MockTwitchIrcServer twitch;
    EmoteController emotes;
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

TEST(ImageLayout, AnimatedBasePreservesStaticOverlayAndFrameTiming)
{
    MockApplication app;
    QPixmap red(20, 20), blue(20, 20), green(6, 6);
    red.fill(Qt::red);
    blue.fill(Qt::blue);
    green.fill(Qt::green);
    auto image = Image::fromResourcePixmap(red);
    detail::assignFrames(image,
                         QList<detail::Frame>{{.image = red, .duration = 20},
                                              {.image = blue, .duration = 60}});
    QCoreApplication::processEvents();
    auto overlay = Image::fromResourcePixmap(green);
    TextElement creator("layered", MessageElementFlag::Text);
    LayeredImageLayoutElement element(creator, {image, overlay},
                                      {QSizeF(20, 20), QSizeF(6, 6)},
                                      QSizeF(20, 20));
    ASSERT_TRUE(element.hasAnimatedContent());
    MessageLayoutElement &paintable = element;
    QPixmap canvas(20, 20);
    {
        QPainter painter(&canvas);
        ASSERT_TRUE(paintable.paintAnimated(painter, 0));
    }
    EXPECT_EQ(canvas.toImage().pixelColor(0, 0), QColor(Qt::red));
    EXPECT_EQ(canvas.toImage().pixelColor(10, 18), QColor(Qt::green));
    app.emotes.getGIFTimer()->advance();
    app.emotes.getGIFTimer()->advance();
    {
        QPainter painter(&canvas);
        ASSERT_TRUE(paintable.paintAnimated(painter, 0));
    }
    EXPECT_EQ(canvas.toImage().pixelColor(0, 0), QColor(Qt::blue));
    EXPECT_EQ(canvas.toImage().pixelColor(10, 18), QColor(Qt::green));
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

TEST(MessageCachePerformance, ReportsAppendPersistAndRotate)
{
    MockApplication app;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    app.paths_.miscDirectory = directory.path();
    const auto path = messageCachePerformanceLogPath();
    ASSERT_EQ(QFileInfo(path).absolutePath(), directory.path());

    logMessageCachePerformance("first report");
    logMessageCachePerformance("second report");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto contents = file.readAll();
    EXPECT_TRUE(contents.contains("\nfirst report\n\n"));
    EXPECT_TRUE(contents.contains("\nsecond report\n\n"));
    file.close();

    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.resize(20 * 1024 * 1024));
    file.close();
    logMessageCachePerformance("after rotation");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto fresh = file.readAll();
    EXPECT_TRUE(fresh.contains("\nafter rotation\n\n"));
    EXPECT_FALSE(fresh.contains("first report"));
    QFile previous(path + ".1");
    ASSERT_TRUE(previous.open(QIODevice::ReadOnly));
    EXPECT_TRUE(previous.read(128).contains("first report"));
}

TEST(MessageCachePerformance, IntervalSpikesRemainVisibleAfterStartupMaximum)
{
    MessageCacheTiming timing;
    timing.record(300000000);
    timing.resetInterval();
    timing.record(1000000);
    timing.record(60000000);
    timing.record(200000000);
    EXPECT_EQ(timing.calls, 4);
    EXPECT_EQ(timing.maximumNanoseconds, 300000000);
    EXPECT_EQ(timing.intervalCalls, 3);
    EXPECT_EQ(timing.intervalMaximumNanoseconds, 200000000);
    EXPECT_EQ(timing.intervalSlowCalls,
              (std::array<uint64_t, 5>{2, 2, 2, 1, 0}));
    timing.resetInterval();
    EXPECT_EQ(timing.intervalCalls, 0);
    EXPECT_EQ(timing.intervalMaximumNanoseconds, 0);
    EXPECT_EQ(timing.calls, 4);
}

TEST(MessageCachePerformance, QueuedReportsAreBatchedAndPersisted)
{
    MockApplication app;
    app.settings.cachePerformanceLogging = true;
    initializeMessageCachePerformance();
    QEventLoop warmup;
    QTimer::singleShot(75, &warmup, &QEventLoop::quit);
    warmup.exec();
    // An intentional GUI stall must be visible even outside paint/layout.
    QThread::msleep(150);
    QCoreApplication::processEvents();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    app.paths_.miscDirectory = directory.path();
    for (int i = 0; i < 60; ++i)
    {
        queueMessageCachePerformance(QString("synthetic report %1").arg(i));
    }
    QFile file(messageCachePerformanceLogPath());
    EXPECT_FALSE(file.exists());  // No synchronous IO on the caller.
    QElapsedTimer wait;
    wait.start();
    QByteArray contents;
    while (wait.elapsed() < 5000)
    {
        QCoreApplication::processEvents();
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            contents = file.readAll();
            file.close();
            if (contents.contains("synthetic report 59\n"))
            {
                break;
            }
        }
        QThread::msleep(5);
    }
    EXPECT_TRUE(contents.contains("synthetic report 0\n"));
    EXPECT_TRUE(contents.contains("synthetic report 59\n"));
    EXPECT_EQ(contents.count("Content deduplication (all channels)"), 1);
    EXPECT_TRUE(contents.contains("Event-loop delay beyond 50 ms interval"));
    auto maximum = QString::fromUtf8(contents)
                       .section("Event-loop delay beyond 50 ms interval: ", 1)
                       .section("max ", 1)
                       .section(" ms", 0, 0);
    maximum.remove(',');
    EXPECT_GE(maximum.toDouble(), 50.0);
}

TEST(ImageLayout, DecodeCompletionsAcrossEventTurnsShareOneInvalidation)
{
    MockApplication app;
    // Drain any pending image notifications left by earlier image tests.
    auto wait = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };
    wait(30);
    int invalidations = 0;
    pajlada::Signals::SignalHolder holder;
    holder.managedConnect(app.windowManager.layoutRequested, [&](Channel *) {
        ++invalidations;
    });
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::red);
    auto image = Image::fromResourcePixmap(pixmap);
    for (int i = 0; i < 3; ++i)
    {
        detail::assignFrames(image, QList<detail::Frame>{{pixmap, 20}});
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(invalidations, 0);
    wait(40);
    EXPECT_EQ(invalidations, 1);
    EXPECT_TRUE(image->loaded());
}

TEST(ChannelViewHistory, ScrollbarMeasurementsReuseGeometryAndRemainBounded)
{
    MockApplication app;
    app.settings.enableSmoothScrollingNewMessages = false;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 1000);
    ChannelView view(nullptr);
    view.resize(WIDTH, 150);
    view.setChannel(channel);
    auto add = [&](int n) {
        MessageBuilder builder;
        builder.emplace<TextElement>(QString("Synthetic message %1").arg(n),
                                     MessageElementFlag::Text);
        channel->addMessage(builder.release(), MessageContext::Original);
    };
    for (int i = 0; i < 200; ++i)
    {
        add(i);
    }
    view.show();
    view.grab();
    view.scrollbar()->scrollToTop(false);
    view.grab();
    const auto tail = view.getMessagesSnapshot().back();
    ASSERT_FALSE(tail->flags.has(MessageLayoutFlag::RequiresLayout));
    const auto builds = messageLayoutBuildCount();
    for (int i = 0; i < 5; ++i)
    {
        view.queueLayout();
        view.grab();
    }
    EXPECT_EQ(messageLayoutBuildCount(), builds);
    for (int i = 200; i < 500; ++i)
    {
        add(i);
        view.grab();
    }
    auto cached = view.cacheDiagnostics()
                      .section("Cached measurement layouts: ", 1)
                      .section('\n', 0, 0);
    cached.remove(',');
    EXPECT_LE(cached.toUInt(), 256);
    EXPECT_TRUE(tail->flags.has(MessageLayoutFlag::RequiresLayout));
    view.hide();
    EXPECT_TRUE(
        view.cacheDiagnostics().contains("Cached measurement layouts: 0"));
}

TEST(MessageCachePerformance, PaintAndLayoutTimingsFollowLoggingSetting)
{
    MockApplication app;
    app.settings.cachePerformanceLogging = false;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 3);
    ChannelView view(nullptr);
    view.resize(WIDTH, 200);
    view.setChannel(channel);
    MessageBuilder builder;
    builder.emplace<TextElement>("Measure this visible message",
                                 MessageElementFlag::Text);
    channel->addMessage(builder.release(), MessageContext::Original);
    view.show();
    view.grab();
    auto report = view.cacheDiagnostics();
    EXPECT_TRUE(report.contains("Layout wall time: 0 calls"));
    EXPECT_TRUE(report.contains("Paint wall time: 0 calls"));

    app.settings.cachePerformanceLogging = true;
    view.queueLayout();
    view.grab();
    report = view.cacheDiagnostics();
    EXPECT_FALSE(report.contains("Layout wall time: 0 calls"));
    EXPECT_FALSE(report.contains("Paint wall time: 0 calls"));
}

TEST(MessageCachePerformance, LargeDiagnosticsWindowIsScrollableAndNonmodal)
{
    MockApplication app;
    QWidget parent;
    parent.resize(300, 200);
    parent.show();
    const auto report =
        QString("Channel: test\nValue: 1,000,000\n").repeated(1000);
    QElapsedTimer elapsed;
    elapsed.start();
    CacheDiagnosticsDialog dialog(report, &parent);
    dialog.show();
    QCoreApplication::processEvents();
    auto *text = dialog.findChild<QPlainTextEdit *>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->toPlainText(), report);
    EXPECT_TRUE(text->isReadOnly());
    EXPECT_GT(text->verticalScrollBar()->maximum(), 0);
    EXPECT_FALSE(dialog.isModal());
    EXPECT_NE(QApplication::activeModalWidget(), &dialog);
    EXPECT_TRUE(parent.isEnabled());
    EXPECT_LE(dialog.height(), 640);
    EXPECT_LT(elapsed.elapsed(), 5000);
    dialog.setReport("updated snapshot");
    EXPECT_EQ(text->toPlainText(), "updated snapshot");
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

TEST(ChannelViewHistory, ShowingHiddenViewRestoresReleasedMessageGeometry)
{
    MockApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 3);
    ChannelView view(nullptr);
    view.resize(WIDTH, 200);
    view.setPausable(true);
    view.setChannel(channel);

    MessageBuilder builder;
    builder.emplace<TextElement>("Text must reappear without scrolling",
                                 MessageElementFlag::Text);
    channel->addMessage(builder.release(), MessageContext::Original);

    view.show();
    view.grab();  // Paint once so the view tracks its on-screen layouts.
    const auto layout = view.getMessagesSnapshot().back();
    const auto characters = layout->getLastCharacterIndex();
    ASSERT_GT(characters, 0);
    ASSERT_FALSE(layout->flags.has(MessageLayoutFlag::RequiresLayout));

    for (const auto paused : {false, true})
    {
        SCOPED_TRACE(paused);
        if (paused)
        {
            view.pause(PauseReason::Selection);
        }
        view.hide();
        EXPECT_EQ(layout->getLastCharacterIndex(), 0);
        EXPECT_TRUE(layout->flags.has(MessageLayoutFlag::RequiresLayout));

        view.show();
        // No scrolling, new messages, resizing or queued events should be
        // needed to rebuild the text before the tab's first paint.
        EXPECT_EQ(layout->getLastCharacterIndex(), characters);
        EXPECT_FALSE(layout->flags.has(MessageLayoutFlag::RequiresLayout));
        view.grab();
        EXPECT_EQ(view.getMessagesSnapshot().back(), layout);
    }
}

TEST(ChannelViewHistory, DisjointRepaintsSkipUnchangedRowsBetweenRegions)
{
    MockApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 10);
    ChannelView view(nullptr);
    view.resize(WIDTH, 300);
    view.setChannel(channel);
    for (int i = 0; i < 5; ++i)
    {
        MessageBuilder builder;
        builder.emplace<TextElement>(QString::number(i),
                                     MessageElementFlag::Text);
        channel->addMessage(builder.release(), MessageContext::Original);
    }
    view.show();
    view.grab();
    QCoreApplication::processEvents();
    auto paints = [&] {
        auto number = view.cacheDiagnostics()
                          .section("Message rows painted: ", 1)
                          .section('\n', 0, 0);
        number.remove(',');
        return number.toULongLong();
    };
    const auto before = paints();
    const auto &layouts = view.getMessagesSnapshot();
    int lastY = 0;
    for (size_t i = 0; i + 1 < layouts.size(); ++i)
        lastY += layouts[i]->getHeight();
    view.repaint(QRegion(QRect(0, 0, WIDTH, layouts.front()->getHeight())) |
                 QRegion(QRect(0, lastY, WIDTH, layouts.back()->getHeight())));
    EXPECT_EQ(paints() - before, 2);
}

TEST(ChannelViewHistory, DuplicateChannelViewsHaveDifferentStableLogIds)
{
    MockApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::None, 10);
    ChannelView first(nullptr);
    ChannelView second(nullptr);
    first.setChannel(channel);
    second.setChannel(channel);
    auto id = [](const auto &view) {
        return view.cacheDiagnostics()
            .section("View ID: ", 1)
            .section('\n', 0, 0);
    };
    ASSERT_FALSE(id(first).isEmpty());
    EXPECT_NE(id(first), id(second));
    const auto original = id(first);
    channel->setMessageLimit(20);
    EXPECT_EQ(id(first), original);
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
