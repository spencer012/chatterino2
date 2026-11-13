// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace chatterino {

namespace detail {

/// Calls @a releaseSome with a budget of items until it returns true, once per
/// event loop turn.
void scheduleGradualRelease(std::function<bool(size_t budget)> releaseSome);

}  // namespace detail

/// Destroys @a items on the GUI thread over several event loop turns, so that
/// dropping a large message history doesn't block it all at once. Small
/// vectors are destroyed right away. Must be called on the GUI thread.
template <typename T>
void releaseGradually(std::vector<T> &&items)
{
    constexpr size_t immediateLimit = 4096;
    if (items.size() <= immediateLimit)
    {
        items.clear();
        return;
    }

    auto pending = std::make_shared<std::vector<T>>(std::move(items));
    detail::scheduleGradualRelease([pending](size_t budget) {
        auto count = std::min(budget, pending->size());
        pending->erase(pending->end() - static_cast<std::ptrdiff_t>(count),
                       pending->end());
        return pending->empty();
    });
}

}  // namespace chatterino
