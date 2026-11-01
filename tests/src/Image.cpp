// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/Image.hpp"

#include "controllers/emotes/EmoteController.hpp"
#include "mocks/BaseApplication.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "Test.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QColor>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>

using namespace chatterino;

namespace {

class ImageApplication : public mock::BaseApplication
{
public:
    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    EmoteController emotes;
};

QPixmap makePixmap(QSize size, Qt::GlobalColor color)
{
    QPixmap pixmap(size);
    pixmap.fill(color);
    return pixmap;
}

bool hasColor(const detail::Frames &frames, QColor color)
{
    const auto pixmap = frames.current();
    return pixmap && pixmap->toImage().pixelColor(0, 0) == color;
}

}  // namespace

TEST(Image, EmptyCachedFrames)
{
    ImageApplication app;
    detail::Frames frames;
    EXPECT_TRUE(frames.empty());
    EXPECT_FALSE(frames.animated());
    EXPECT_FALSE(frames.current());
    EXPECT_FALSE(frames.frameSize());
}

TEST(Image, StaticCachedFrame)
{
    ImageApplication app;
    detail::Frames frames(QList<detail::Frame>{
        {.image = makePixmap({1, 1}, Qt::red), .duration = 20},
    });
    EXPECT_FALSE(frames.empty());
    EXPECT_FALSE(frames.animated());

    app.emotes.getGIFTimer()->advance();
    EXPECT_TRUE(hasColor(frames, Qt::red));
}

TEST(Image, CachedFramesUseSharedTimer)
{
    ImageApplication app;
    detail::Frames frames(QList<detail::Frame>{
        {.image = makePixmap({1, 1}, Qt::red), .duration = 20},
        {.image = makePixmap({2, 2}, Qt::blue), .duration = 60},
    });
    ASSERT_TRUE(frames.animated());
    ASSERT_EQ(frames.frameSize(), QSize(1, 1));

    auto *timer = app.emotes.getGIFTimer();
    timer->advance();
    EXPECT_TRUE(hasColor(frames, Qt::red));
    timer->advance();
    EXPECT_TRUE(hasColor(frames, Qt::blue));
    EXPECT_EQ(frames.frameSize(), QSize(1, 1));
}

TEST(Image, CachedFramesClearDisconnectsTimer)
{
    ImageApplication app;
    auto *timer = app.emotes.getGIFTimer();
    {
        detail::Frames frames(QList<detail::Frame>{
            {.image = makePixmap({1, 1}, Qt::red), .duration = 20},
            {.image = makePixmap({1, 1}, Qt::blue), .duration = 60},
        });
        frames.clear();
        EXPECT_TRUE(frames.empty());
        EXPECT_FALSE(frames.animated());
        EXPECT_FALSE(frames.current());
        EXPECT_FALSE(frames.frameSize());
        timer->advance();
    }
    timer->advance();
}

TEST(Image, LazyFramesStaySynchronizedAcrossSkippedReadsAndNewLoads)
{
    ImageApplication app;
    const auto frames = QList<detail::Frame>{
        {.image = makePixmap({1, 1}, Qt::red), .duration = 20},
        {.image = makePixmap({1, 1}, Qt::blue), .duration = 60},
        {.image = makePixmap({1, 1}, Qt::green), .duration = 40},
    };
    detail::Frames existing{QList<detail::Frame>(frames)};
    auto *timer = app.emotes.getGIFTimer();
    ASSERT_TRUE(hasColor(existing, Qt::red));
    timer->advance();  // Inclusive first frame end: 20 ms.
    EXPECT_TRUE(hasColor(existing, Qt::red));
    for (int i = 0; i < 5; ++i)
        timer->advance();
    EXPECT_TRUE(hasColor(existing, Qt::green));  // 120 ms, final frame end.
    detail::Frames newlyLoaded{QList<detail::Frame>(frames)};
    EXPECT_TRUE(hasColor(newlyLoaded, Qt::green));
    timer->advance();
    EXPECT_TRUE(hasColor(existing, Qt::red));
    EXPECT_TRUE(hasColor(newlyLoaded, Qt::red));
    for (int i = 0; i < 10001; ++i)
        timer->advance();
    EXPECT_EQ(existing.current()->cacheKey(),
              newlyLoaded.current()->cacheKey());
}

TEST(Image, LongAnimationLoadsAtCurrentClockAndSurvivesTimerDestruction)
{
    std::unique_ptr<detail::Frames> frames;
    {
        ImageApplication app;
        for (int i = 0; i < 4000; ++i)
            app.emotes.getGIFTimer()->advance();
        frames = std::make_unique<detail::Frames>(QList<detail::Frame>{
            {.image = makePixmap({1, 1}, Qt::red), .duration = 70000},
            {.image = makePixmap({1, 1}, Qt::blue), .duration = 70000},
        });
        EXPECT_TRUE(hasColor(*frames, Qt::blue));
    }
    EXPECT_TRUE(hasColor(*frames, Qt::blue));
    frames->clear();
    EXPECT_FALSE(frames->current());
}

TEST(Image, InvalidFrameDurationsCannotSpinOrDivideByZero)
{
    ImageApplication app;
    detail::Frames frames(QList<detail::Frame>{
        {.image = makePixmap({1, 1}, Qt::red), .duration = 0},
        {.image = makePixmap({1, 1}, Qt::blue), .duration = -1},
    });
    app.emotes.getGIFTimer()->advance();
    EXPECT_TRUE(hasColor(frames, Qt::blue));
}

// Explicit opt-in microbenchmark: clock maintenance only, excluding drawing and
// decoding. Compares production lazy Frames with the previous per-image signal
// callback algorithm. It does not measure application FPS or total CPU usage.
TEST(Image, DISABLED_AnimationClockMaintenanceBenchmark)
{
    ImageApplication app;
    constexpr int TICKS = 2000;
    auto measure = [](auto &&tick) {
        std::array<double, 3> samples;
        for (auto &sample : samples)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < TICKS; ++i)
                tick();
            sample = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
        }
        std::sort(samples.begin(), samples.end());
        return samples[1];
    };
    std::cout << "loaded_animations,ticks,legacy_callbacks_ms,lazy_clock_ms\n";
    for (const int count : {100, 1000, 10000})
    {
        struct LegacyState {
            int offset = 0;
            int index = 0;
        };
        std::vector<LegacyState> state(count);
        pajlada::Signals::NoArgSignal legacy;
        std::vector<pajlada::Signals::ScopedConnection> connections;
        connections.reserve(count);
        std::vector<std::unique_ptr<detail::Frames>> frames;
        frames.reserve(count);
        const auto red = makePixmap({1, 1}, Qt::red);
        const auto blue = makePixmap({1, 1}, Qt::blue);
        for (int i = 0; i < count; ++i)
        {
            auto *entry = &state[i];
            connections.emplace_back(legacy.connect([entry] {
                entry->offset += GIF_FRAME_LENGTH;
                while (entry->offset > (entry->index == 0 ? 20 : 60))
                {
                    entry->offset -= entry->index == 0 ? 20 : 60;
                    entry->index = 1 - entry->index;
                }
            }));
            frames.emplace_back(std::make_unique<detail::Frames>(
                QList<detail::Frame>{{.image = red, .duration = 20},
                                     {.image = blue, .duration = 60}}));
        }
        const auto legacyMs = measure([&] {
            legacy.invoke();
        });
        const auto lazyMs = measure([&] {
            app.emotes.getGIFTimer()->advance();
        });
        ASSERT_TRUE(frames.back()->current());
        std::cout << count << ',' << TICKS << ',' << legacyMs << ',' << lazyMs
                  << '\n';
    }
}
