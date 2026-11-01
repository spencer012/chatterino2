// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Helpers.hpp"

#include <QLoggingCategory>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace chatterino {

inline const QLoggingCategory &messageCacheLog()
{
    static const QLoggingCategory category("chatterino.cache", QtInfoMsg);
    return category;
}

QString messageCachePerformanceLogPath();
void logMessageCachePerformance(const QString &report);
uint64_t nextMessageCacheViewId();
const QString &messageCacheSessionId();

struct MessageCacheTiming {
    uint64_t calls = 0;
    uint64_t nanoseconds = 0;
    uint64_t maximumNanoseconds = 0;

    QString describe(const QString &name) const
    {
        auto milliseconds = [](double nanoseconds) {
            return QLocale().toString(nanoseconds / 1000000.0, 'f', 3);
        };
        return QString("%1: %2 calls; total %3 ms; mean %4 ms; max %5 ms")
            .arg(name)
            .arg(localizeNumbers(this->calls))
            .arg(milliseconds(this->nanoseconds))
            .arg(milliseconds(this->calls == 0
                                  ? 0.0
                                  : static_cast<double>(this->nanoseconds) /
                                        this->calls))
            .arg(milliseconds(this->maximumNanoseconds));
    }
};

/// Wall-clock timing is collected only while performance logging is enabled.
class MessageCacheTimer
{
public:
    MessageCacheTimer(MessageCacheTiming &timing, bool enabled)
        : timing_(enabled ? &timing : nullptr)
    {
        if (enabled)
        {
            this->start_ = std::chrono::steady_clock::now();
        }
    }

    ~MessageCacheTimer()
    {
        if (this->timing_)
        {
            const auto elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - this->start_)
                    .count());
            ++this->timing_->calls;
            this->timing_->nanoseconds += elapsed;
            this->timing_->maximumNanoseconds =
                std::max(this->timing_->maximumNanoseconds, elapsed);
        }
    }

    MessageCacheTimer(const MessageCacheTimer &) = delete;
    MessageCacheTimer &operator=(const MessageCacheTimer &) = delete;

private:
    MessageCacheTiming *timing_;
    std::chrono::steady_clock::time_point start_{};
};

struct MessageCacheStats {
    uint64_t viewId = nextMessageCacheViewId();
    uint64_t snapshotBuilds = 0;
    uint64_t snapshotReuses = 0;
    uint64_t copiedReferences = 0;
    uint64_t snapshotNanoseconds = 0;
    uint64_t replayRebuilds = 0;
    uint64_t replayAppends = 0;
    uint64_t layoutsReleased = 0;
    uint64_t messagePaints = 0;
    MessageCacheTiming layout;
    MessageCacheTiming paint;

    QString describe() const
    {
        return QString(
                   "Frozen snapshots built: %1\nLive/frozen history reads: %2\n"
                   "Pointers copied: %3\nSnapshot copy time: %4 ms\n"
                   "Replay index rebuilds: %5\nReplay entries appended: %6\n"
                   "Offscreen layouts released: %7")
                   .arg(localizeNumbers(this->snapshotBuilds))
                   .arg(localizeNumbers(this->snapshotReuses))
                   .arg(localizeNumbers(this->copiedReferences))
                   .arg(QLocale().toString(
                       static_cast<double>(this->snapshotNanoseconds) /
                           1000000.0,
                       'f', 2))
                   .arg(localizeNumbers(this->replayRebuilds))
                   .arg(localizeNumbers(this->replayAppends))
                   .arg(localizeNumbers(this->layoutsReleased)) +
               "\nMessage rows painted: " +
               localizeNumbers(this->messagePaints) + "\n" +
               this->layout.describe("Layout wall time") + "\n" +
               this->paint.describe("Paint wall time") +
               "\nTimings are cumulative while logging is enabled. "
               "Paint includes layout performed during painting; do not add "
               "the totals.";
    }
};

}  // namespace chatterino
