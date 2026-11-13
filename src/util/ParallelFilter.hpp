// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QList>
#include <QtConcurrent>
#include <QThreadPool>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace chatterino {

/// Splits [0, count) into consecutive ranges, calls @a filterRange(begin, end)
/// for each of them on the global thread pool and concatenates the vectors it
/// returns, in order. The calling thread works on ranges too and blocks until
/// all of them are done.
///
/// @a filterRange runs on several threads at once. Anything it doesn't own
/// must be safe to read concurrently, so create non-thread-safe helpers (such
/// as search predicates) inside it.
template <typename T, typename FilterRange>
std::vector<T> parallelFilterRanges(size_t count, size_t minRangeSize,
                                    FilterRange &&filterRange)
{
    auto threads = static_cast<size_t>(
        std::max(1, QThreadPool::globalInstance()->maxThreadCount()));
    // A few ranges per thread so that one slow range doesn't hold up the rest
    auto rangeSize = std::max(std::max<size_t>(1, minRangeSize),
                              (count + (threads * 4) - 1) / (threads * 4));
    if (count <= rangeSize)
    {
        return filterRange(size_t{0}, count);
    }

    QList<std::pair<size_t, size_t>> ranges;
    for (size_t begin = 0; begin < count; begin += rangeSize)
    {
        ranges.emplace_back(begin, std::min(count, begin + rangeSize));
    }

    auto parts = QtConcurrent::blockingMapped<QList<std::vector<T>>>(
        ranges, [&filterRange](const std::pair<size_t, size_t> &range) {
            return filterRange(range.first, range.second);
        });

    size_t total = 0;
    for (const auto &part : parts)
    {
        total += part.size();
    }
    std::vector<T> result;
    result.reserve(total);
    for (auto &part : parts)
    {
        std::move(part.begin(), part.end(), std::back_inserter(result));
    }
    return result;
}

}  // namespace chatterino
