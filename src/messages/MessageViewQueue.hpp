// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/ThreadGuard.hpp"

#include <boost/circular_buffer.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace chatterino {

/// GUI-owned message history. Live reads borrow the ring; only paused views copy
/// it. Callers must not retain iterators across event-loop turns or mutations.
template <typename T>
class MessageViewQueue
{
public:
    using Snapshot = boost::circular_buffer<T>;

    explicit MessageViewQueue(size_t limit)
        : items_(std::max<size_t>(1, limit))
    {
    }

    const Snapshot &live() const
    {
        this->guard_.guard();
        return this->items_;
    }

    size_t size() const
    {
        return this->live().size();
    }
    size_t limit() const
    {
        return this->live().capacity();
    }

    void clear()
    {
        this->guard_.guard();
        this->items_.clear();
    }

    bool pushBack(const T &item)
    {
        this->guard_.guard();
        const bool full = this->items_.full();
        this->items_.push_back(item);
        return full;
    }

    std::vector<T> pushFront(const std::vector<T> &items)
    {
        this->guard_.guard();
        const auto count = std::min(items.size(), this->limit() - this->size());
        const auto first = items.size() - count;
        for (size_t i = items.size(); i > first;)
        {
            this->items_.push_front(items[--i]);
        }
        return {items.begin() + first, items.end()};
    }

    std::optional<T> last() const
    {
        return this->live().empty() ? std::nullopt
                                    : std::optional<T>{this->items_.back()};
    }

    std::optional<std::pair<size_t, T>> find(size_t hint,
                                             auto &&predicate) const
    {
        const auto &items = this->live();
        if (hint < items.size() && predicate(items[hint]))
        {
            return std::pair{hint, items[hint]};
        }
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (predicate(items[i]))
            {
                return std::pair{i, items[i]};
            }
        }
        return std::nullopt;
    }

    void replaceItem(size_t index, const T &item)
    {
        this->guard_.guard();
        if (index < this->items_.size())
        {
            this->items_[index] = item;
        }
    }

    std::vector<T> setLimit(size_t limit)
    {
        this->guard_.guard();
        limit = std::max<size_t>(1, limit);
        std::vector<T> removed;
        while (this->items_.size() > limit)
        {
            removed.push_back(std::move(this->items_.front()));
            this->items_.pop_front();
        }
        this->items_.set_capacity(limit);
        return removed;
    }

private:
    mutable ThreadGuard guard_;
    Snapshot items_;
};

}  // namespace chatterino
