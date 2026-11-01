// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/MessageContentPool.hpp"
#include "util/Helpers.hpp"

#include <QString>

namespace chatterino {

inline QString describeMessageContentPool()
{
    const auto stats = MessageContentPool::global().stats();
    auto mib = [](auto bytes) {
        return QLocale().toString(
            static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 2);
    };
    auto description =
        QString("Content deduplication (all channels): %1\n"
                "Estimated CURRENT net saving: %2 MiB\n"
                "Current gross payload saving: %3 MiB\n"
                "Pool bookkeeping: %4 MiB\n"
                "Tracking fields, including unpooled objects: %5 MiB\n"
                "Indexed payloads: %6 / %7; live payloads: %8\n"
                "Shared fields: %9; already-shared/clone fields: %10")
            .arg(stats.enabled ? "on" : "off")
            .arg(mib(stats.retainedNetSavedBytes))
            .arg(mib(stats.retainedGrossSavedBytes))
            .arg(mib(stats.estimatedOverheadBytes))
            .arg(mib(stats.trackingSlotBytes))
            .arg(localizeNumbers(stats.indexedEntries))
            .arg(localizeNumbers(stats.maxIndexedEntries))
            .arg(localizeNumbers(stats.liveEntries))
            .arg(localizeNumbers(stats.liveParticipants))
            .arg(localizeNumbers(stats.passiveParticipants));
    description +=
        QString(
            "\nCumulative hits: %1; hits without releasing storage: %2\n"
            "Misses: %3; first-seen payloads deferred: %4; index evictions: "
            "%5\n"
            "Cumulative duplicate payload bytes released: %6 MiB\n"
            "Cumulative pool lookup wall time: %7 ms\n"
            "Byte figures are allocation estimates, not measured process RAM.\n"
            "Each occurrence and emote element remains independent.")
            .arg(localizeNumbers(stats.hits))
            .arg(localizeNumbers(stats.hitsWithoutSaving))
            .arg(localizeNumbers(stats.misses))
            .arg(localizeNumbers(stats.deferred))
            .arg(localizeNumbers(stats.evictions))
            .arg(mib(stats.cumulativeDeduplicatedBytes))
            .arg(QLocale().toString(
                static_cast<double>(stats.processingTimeNs) / 1000000.0, 'f',
                2));
    return description;
}

}  // namespace chatterino
