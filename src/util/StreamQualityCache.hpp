// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace chatterino {

/// The Twitch login that a bare login or a twitch.tv/<login> URL refers to,
/// in lowercase. Empty for anything else, which then isn't cached.
QString twitchLoginOf(const QString &channelOrUrl);

/// The result of listing a stream's qualities.
struct StreamQualityListing {
    /// Empty when listing failed.
    QStringList qualities;
    /// Shows why listing failed. Only called when someone is waiting for the
    /// list, so a failed prefetch stays quiet.
    std::function<void()> reportError;
};

/// Remembers the quality list of each live stream so the quality popup can
/// open right away.
///
/// Entries are keyed by player and channel and tied to the Twitch stream ID
/// they were listed for. A stream keeps its qualities while it's live. When
/// it goes offline and comes back it has a new stream ID, so the list is
/// fetched again. Only use this from the GUI thread.
class StreamQualityCache
{
public:
    using Clock = std::chrono::steady_clock;
    using DoneCallback = std::function<void(StreamQualityListing)>;
    /// Starts listing qualities and calls the callback once with the result.
    /// It may call it before returning.
    using Fetcher = std::function<void(DoneCallback)>;
    using ReadyCallback = std::function<void(const QStringList &)>;

    /// A fetch running longer than this is treated as hung, and the next
    /// prefetch or request starts a new one.
    static constexpr std::chrono::seconds HUNG_FETCH_AFTER{30};

    explicit StreamQualityCache(std::function<Clock::time_point()> now = {});

    /// Lists qualities in the background unless the list for this stream is
    /// already cached or being fetched. Does nothing without a stream ID.
    void prefetch(const QString &key, const QString &streamId,
                  const Fetcher &fetch);

    /// Calls onReady with this stream's qualities: right away when they're
    /// cached, otherwise when the running or a newly started fetch finishes.
    /// A failed fetch calls its reportError instead. Without a stream ID the
    /// list is fetched and not cached.
    void request(const QString &key, const QString &streamId,
                 const Fetcher &fetch, ReadyCallback onReady);

    /// Drops the entry for key, e.g. once the channel is seen offline. A
    /// fetch that is still running for it is ignored when it finishes.
    void forget(const QString &key);

    /// The cached list for this stream, or an empty list when there is none.
    QStringList cached(const QString &key, const QString &streamId) const;

    /// Whether a fetch for this stream is running.
    bool isFetching(const QString &key, const QString &streamId) const;

private:
    struct Entry {
        QString streamId;
        std::uint64_t generation = 0;
        bool fetching = false;
        Clock::time_point fetchStarted;
        QStringList qualities;
        std::vector<ReadyCallback> waiters;
    };

    /// Whether the entry holds or is getting the list for streamId.
    bool covers(const Entry &entry, const QString &streamId) const;
    void start(const QString &key, const QString &streamId,
               const Fetcher &fetch, ReadyCallback onReady);
    void finish(const QString &key, std::uint64_t generation,
                StreamQualityListing listing);

    std::function<Clock::time_point()> now_;
    std::unordered_map<QString, Entry> entries_;
    std::uint64_t nextGeneration_ = 1;
};

}  // namespace chatterino
