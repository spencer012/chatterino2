// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"
#include "util/StreamQualityCache.hpp"
#include "util/StreamQualitySort.hpp"

#include <chrono>
#include <vector>

using namespace chatterino;

namespace {

QString joined(const QStringList &labels)
{
    return labels.join(u'|');
}

}  // namespace

TEST(StreamLinkQualities, TwitchPipeScreenshotOrder)
{
    const QStringList input{
        QStringLiteral("720p60"),     QStringLiteral("480p"),
        QStringLiteral("360p"),       QStringLiteral("160p"),
        QStringLiteral("audio_only"), QStringLiteral("1080p60"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("1080p60|720p60|480p|360p|160p|audio_only"));
}

TEST(StreamLinkQualities, HeightThenFrameRate)
{
    const QStringList input{
        QStringLiteral("720p"),    QStringLiteral("720p30"),
        QStringLiteral("1080p"),   QStringLiteral("720p60"),
        QStringLiteral("1080p60"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("1080p60|1080p|720p60|720p30|720p"));
}

TEST(StreamLinkQualities, SourceSuffixStaysSelectable)
{
    const QStringList input{
        QStringLiteral("1080p"),
        QStringLiteral("1080p60 (source)"),
        QStringLiteral("1080p60"),
        QStringLiteral("480p"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("1080p60 (source)|1080p60|1080p|480p"));
}

TEST(StreamLinkQualities, NamedAndUnknownLabels)
{
    const QStringList input{
        QStringLiteral("worst"),         QStringLiteral("mystery"),
        QStringLiteral("audio_only"),    QStringLiteral("best"),
        QStringLiteral("720p"),          QStringLiteral("chunked"),
        QStringLiteral("best (source)"), QStringLiteral("160p (worst)"),
        QStringLiteral("audio"),         QStringLiteral("audio-only"),
    };

    EXPECT_EQ(joined(sortStreamQualities(input)),
              QStringLiteral("best|best (source)|720p|160p (worst)|mystery|"
                             "chunked|worst|audio_only|audio|audio-only"));
}

TEST(StreamLinkQualities, CaseDuplicatesAndEmpty)
{
    const QStringList mixed{
        QStringLiteral("AUDIO_ONLY"), QStringLiteral("720p"),
        QStringLiteral("1080P60"),    QStringLiteral("Best"),
        QStringLiteral("720p"),       QStringLiteral("Worst"),
        QStringLiteral("zeta"),       QStringLiteral("alpha"),
    };

    EXPECT_EQ(
        joined(sortStreamQualities(mixed)),
        QStringLiteral("Best|1080P60|720p|720p|zeta|alpha|Worst|AUDIO_ONLY"));
    EXPECT_EQ(sortStreamQualities({}), QStringList());
}

namespace {

// A fetcher that records each fetch and lets the test finish it later.
struct FakeFetches {
    std::vector<StreamQualityCache::DoneCallback> pending;

    StreamQualityCache::Fetcher fetcher()
    {
        return [this](StreamQualityCache::DoneCallback done) {
            this->pending.push_back(std::move(done));
        };
    }

    void succeed(std::size_t index, const QStringList &qualities)
    {
        // Copy first: finishing can start another fetch and grow pending.
        auto done = this->pending.at(index);
        done({.qualities = qualities, .reportError = {}});
    }

    void fail(std::size_t index, int *reported)
    {
        auto done = this->pending.at(index);
        done({
            .qualities = {},
            .reportError =
                [reported] {
                    ++*reported;
                },
        });
    }
};

const QStringList QUALITIES{QStringLiteral("1080p60"), QStringLiteral("720p"),
                            QStringLiteral("audio_only")};

}  // namespace

TEST(StreamQualityCache, TwitchLoginOf)
{
    EXPECT_EQ(twitchLoginOf(QStringLiteral("Forsen")), QStringLiteral("forsen"));
    EXPECT_EQ(twitchLoginOf(QStringLiteral("twitch.tv/forsen")),
              QStringLiteral("forsen"));
    EXPECT_EQ(twitchLoginOf(QStringLiteral("https://www.twitch.tv/Forsen/")),
              QStringLiteral("forsen"));
    EXPECT_EQ(twitchLoginOf(QStringLiteral("https://m.twitch.tv/forsen")),
              QStringLiteral("forsen"));
    EXPECT_EQ(twitchLoginOf(QStringLiteral("twitch.tv/popout/forsen")),
              QStringLiteral("forsen"));

    EXPECT_EQ(twitchLoginOf(QStringLiteral("twitch.tv/forsen/videos")),
              QString());
    EXPECT_EQ(twitchLoginOf(QStringLiteral("https://youtube.com/forsen")),
              QString());
    EXPECT_EQ(twitchLoginOf(QStringLiteral("/mentions")), QString());
    EXPECT_EQ(twitchLoginOf(QString()), QString());
}

TEST(StreamQualityCache, PrefetchThenRequestUsesCachedList)
{
    StreamQualityCache cache;
    FakeFetches fetches;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    ASSERT_EQ(fetches.pending.size(), 1U);
    EXPECT_TRUE(cache.isFetching("streamlink|forsen", "100"));

    fetches.succeed(0, QUALITIES);
    EXPECT_EQ(cache.cached("streamlink|forsen", "100"), QUALITIES);

    // Neither another prefetch nor a request lists the stream again.
    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    QStringList shown;
    cache.request("streamlink|forsen", "100", fetches.fetcher(),
                  [&](const QStringList &qualities) {
                      shown = qualities;
                  });
    EXPECT_EQ(shown, QUALITIES);
    EXPECT_EQ(fetches.pending.size(), 1U);
}

TEST(StreamQualityCache, RequestDuringPrefetchWaitsForIt)
{
    StreamQualityCache cache;
    FakeFetches fetches;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    int shownCount = 0;
    cache.request("streamlink|forsen", "100", fetches.fetcher(),
                  [&](const QStringList &qualities) {
                      EXPECT_EQ(qualities, QUALITIES);
                      ++shownCount;
                  });
    EXPECT_EQ(fetches.pending.size(), 1U);
    EXPECT_EQ(shownCount, 0);

    fetches.succeed(0, QUALITIES);
    EXPECT_EQ(shownCount, 1);
}

TEST(StreamQualityCache, NewBroadcastListsAgain)
{
    StreamQualityCache cache;
    FakeFetches fetches;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    fetches.succeed(0, QUALITIES);

    // The stream went offline and came back with a new stream ID.
    const QStringList sourceOnly{QStringLiteral("1080p60")};
    QStringList shown;
    cache.request("streamlink|forsen", "200", fetches.fetcher(),
                  [&](const QStringList &qualities) {
                      shown = qualities;
                  });
    ASSERT_EQ(fetches.pending.size(), 2U);
    EXPECT_TRUE(cache.cached("streamlink|forsen", "100").isEmpty());

    fetches.succeed(1, sourceOnly);
    EXPECT_EQ(shown, sourceOnly);
    EXPECT_EQ(cache.cached("streamlink|forsen", "200"), sourceOnly);
}

TEST(StreamQualityCache, ResultForReplacedStreamIsIgnored)
{
    StreamQualityCache cache;
    FakeFetches fetches;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    cache.prefetch("streamlink|forsen", "200", fetches.fetcher());
    ASSERT_EQ(fetches.pending.size(), 2U);

    fetches.succeed(0, QUALITIES);
    EXPECT_TRUE(cache.cached("streamlink|forsen", "100").isEmpty());
    EXPECT_TRUE(cache.isFetching("streamlink|forsen", "200"));
}

TEST(StreamQualityCache, FailedPrefetchIsQuiet)
{
    StreamQualityCache cache;
    FakeFetches fetches;
    int reported = 0;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    fetches.fail(0, &reported);
    EXPECT_EQ(reported, 0);
    EXPECT_FALSE(cache.isFetching("streamlink|forsen", "100"));

    // The next prefetch tries again.
    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    EXPECT_EQ(fetches.pending.size(), 2U);
}

TEST(StreamQualityCache, FailureIsReportedToWaitingRequest)
{
    StreamQualityCache cache;
    FakeFetches fetches;
    int reported = 0;
    bool shown = false;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    cache.request("streamlink|forsen", "100", fetches.fetcher(),
                  [&](const QStringList &) {
                      shown = true;
                  });
    fetches.fail(0, &reported);

    EXPECT_EQ(reported, 1);
    EXPECT_FALSE(shown);
}

TEST(StreamQualityCache, ForgetDropsListAndRunningFetch)
{
    StreamQualityCache cache;
    FakeFetches fetches;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    fetches.succeed(0, QUALITIES);
    cache.forget("streamlink|forsen");
    EXPECT_TRUE(cache.cached("streamlink|forsen", "100").isEmpty());

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    cache.forget("streamlink|forsen");
    fetches.succeed(1, QUALITIES);
    EXPECT_TRUE(cache.cached("streamlink|forsen", "100").isEmpty());
}

TEST(StreamQualityCache, NoStreamIdIsNotCached)
{
    StreamQualityCache cache;
    FakeFetches fetches;
    int shownCount = 0;
    auto onReady = [&](const QStringList &) {
        ++shownCount;
    };

    cache.prefetch("streamlink|forsen", "", fetches.fetcher());
    EXPECT_TRUE(fetches.pending.empty());

    cache.request("streamlink|forsen", "", fetches.fetcher(), onReady);
    fetches.succeed(0, QUALITIES);
    cache.request("streamlink|forsen", "", fetches.fetcher(), onReady);
    fetches.succeed(1, QUALITIES);

    EXPECT_EQ(shownCount, 2);
    EXPECT_EQ(fetches.pending.size(), 2U);
}

TEST(StreamQualityCache, FetcherCanFinishImmediately)
{
    StreamQualityCache cache;
    auto immediate = [](StreamQualityCache::DoneCallback done) {
        done({.qualities = QUALITIES, .reportError = {}});
    };

    QStringList shown;
    cache.request("streamlink|forsen", "100", immediate,
                  [&](const QStringList &qualities) {
                      shown = qualities;
                  });
    EXPECT_EQ(shown, QUALITIES);
    EXPECT_EQ(cache.cached("streamlink|forsen", "100"), QUALITIES);
}

TEST(StreamQualityCache, HungFetchIsReplaced)
{
    auto now = StreamQualityCache::Clock::time_point{};
    StreamQualityCache cache([&] {
        return now;
    });
    FakeFetches fetches;

    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    now += std::chrono::seconds(5);
    cache.prefetch("streamlink|forsen", "100", fetches.fetcher());
    EXPECT_EQ(fetches.pending.size(), 1U);

    now += StreamQualityCache::HUNG_FETCH_AFTER;
    int shownCount = 0;
    cache.request("streamlink|forsen", "100", fetches.fetcher(),
                  [&](const QStringList &) {
                      ++shownCount;
                  });
    ASSERT_EQ(fetches.pending.size(), 2U);

    // The hung fetch finishing late doesn't count.
    fetches.succeed(0, QUALITIES);
    EXPECT_EQ(shownCount, 0);
    fetches.succeed(1, QUALITIES);
    EXPECT_EQ(shownCount, 1);
}
