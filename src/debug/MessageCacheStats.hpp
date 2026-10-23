// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QLoggingCategory>
#include <QString>

#include <cstdint>

namespace chatterino {

inline const QLoggingCategory &messageCacheLog()
{
    static const QLoggingCategory category("chatterino.cache", QtInfoMsg);
    return category;
}

struct MessageCacheStats {
    uint64_t snapshotBuilds = 0;
    uint64_t snapshotReuses = 0;
    uint64_t copiedReferences = 0;
    uint64_t snapshotNanoseconds = 0;
    uint64_t replayRebuilds = 0;
    uint64_t replayAppends = 0;
    uint64_t layoutsReleased = 0;

    QString describe() const
    {
        return QString(
                   "Frozen snapshots built: %1\nLive/frozen history reads: %2\n"
                   "Pointers copied: %3\nSnapshot copy time: %4 ms\n"
                   "Replay index rebuilds: %5\nReplay entries appended: %6\n"
                   "Offscreen layouts released: %7")
            .arg(this->snapshotBuilds)
            .arg(this->snapshotReuses)
            .arg(this->copiedReferences)
            .arg(static_cast<double>(this->snapshotNanoseconds) / 1000000.0, 0,
                 'f', 2)
            .arg(this->replayRebuilds)
            .arg(this->replayAppends)
            .arg(this->layoutsReleased);
    }
};

}  // namespace chatterino
