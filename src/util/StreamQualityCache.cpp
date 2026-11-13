// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/StreamQualityCache.hpp"

#include <QRegularExpression>

#include <utility>

namespace chatterino {

QString twitchLoginOf(const QString &channelOrUrl)
{
    static const QRegularExpression loginRegex(
        R"(^(?:(?:https?://)?(?:www\.|m\.|go\.)?twitch\.tv/(?:popout/)?)?([a-z0-9_]{1,25})/?$)",
        QRegularExpression::CaseInsensitiveOption);

    const auto match = loginRegex.match(channelOrUrl.trimmed());
    if (!match.hasMatch())
    {
        return {};
    }
    return match.captured(1).toLower();
}

StreamQualityCache::StreamQualityCache(std::function<Clock::time_point()> now)
    : now_(now ? std::move(now) : [] {
        return Clock::now();
    })
{
}

bool StreamQualityCache::covers(const Entry &entry,
                                const QString &streamId) const
{
    if (entry.streamId != streamId)
    {
        return false;
    }
    if (!entry.fetching)
    {
        return true;
    }
    return this->now_() - entry.fetchStarted < HUNG_FETCH_AFTER;
}

void StreamQualityCache::prefetch(const QString &key, const QString &streamId,
                                  const Fetcher &fetch)
{
    if (streamId.isEmpty())
    {
        return;
    }

    auto it = this->entries_.find(key);
    if (it != this->entries_.end() && this->covers(it->second, streamId))
    {
        return;
    }

    this->start(key, streamId, fetch, {});
}

void StreamQualityCache::request(const QString &key, const QString &streamId,
                                 const Fetcher &fetch, ReadyCallback onReady)
{
    if (streamId.isEmpty())
    {
        fetch([onReady = std::move(onReady)](StreamQualityListing listing) {
            if (listing.qualities.isEmpty())
            {
                if (listing.reportError)
                {
                    listing.reportError();
                }
                return;
            }
            onReady(listing.qualities);
        });
        return;
    }

    auto it = this->entries_.find(key);
    if (it != this->entries_.end() && this->covers(it->second, streamId))
    {
        if (it->second.fetching)
        {
            it->second.waiters.push_back(std::move(onReady));
            return;
        }

        // Copy first: onReady may show UI, which can process events.
        const QStringList qualities = it->second.qualities;
        onReady(qualities);
        return;
    }

    this->start(key, streamId, fetch, std::move(onReady));
}

void StreamQualityCache::forget(const QString &key)
{
    this->entries_.erase(key);
}

QStringList StreamQualityCache::cached(const QString &key,
                                       const QString &streamId) const
{
    auto it = this->entries_.find(key);
    if (it == this->entries_.end() || it->second.fetching ||
        it->second.streamId != streamId)
    {
        return {};
    }
    return it->second.qualities;
}

bool StreamQualityCache::isFetching(const QString &key,
                                    const QString &streamId) const
{
    auto it = this->entries_.find(key);
    return it != this->entries_.end() && it->second.fetching &&
           it->second.streamId == streamId;
}

void StreamQualityCache::start(const QString &key, const QString &streamId,
                               const Fetcher &fetch, ReadyCallback onReady)
{
    auto &entry = this->entries_[key];
    // Anyone still waiting wants this channel's current qualities, so they
    // move over to the new fetch.
    entry.streamId = streamId;
    entry.generation = this->nextGeneration_++;
    entry.fetching = true;
    entry.fetchStarted = this->now_();
    entry.qualities.clear();
    if (onReady)
    {
        entry.waiters.push_back(std::move(onReady));
    }

    const auto generation = entry.generation;
    // The fetcher may finish before returning, which can change entries_, so
    // don't use the entry reference after this.
    fetch([this, key, generation](StreamQualityListing listing) {
        this->finish(key, generation, std::move(listing));
    });
}

void StreamQualityCache::finish(const QString &key, std::uint64_t generation,
                                StreamQualityListing listing)
{
    auto it = this->entries_.find(key);
    if (it == this->entries_.end() || it->second.generation != generation ||
        !it->second.fetching)
    {
        // Forgotten, replaced by a newer fetch, or already finished.
        return;
    }

    auto waiters = std::move(it->second.waiters);
    it->second.waiters.clear();

    if (listing.qualities.isEmpty())
    {
        this->entries_.erase(it);
        if (!waiters.empty() && listing.reportError)
        {
            listing.reportError();
        }
        return;
    }

    it->second.fetching = false;
    it->second.qualities = listing.qualities;

    // The callbacks may show UI, which can process events and change
    // entries_, so only use local copies from here on.
    for (const auto &waiter : waiters)
    {
        waiter(listing.qualities);
    }
}

}  // namespace chatterino
