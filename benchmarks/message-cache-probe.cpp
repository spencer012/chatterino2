// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// Standalone probe. See docs/message-cache-performance.md for build instructions.
// Uses the production queue, without linking or running the application.
#include "messages/LimitedQueue.hpp"
#include "messages/MessageViewQueue.hpp"

#ifdef CACHE_PROBE_OBJECT_SIZES
#    include "messages/layouts/MessageLayout.hpp"
#    include "messages/layouts/MessageLayoutElement.hpp"
#    include "messages/Message.hpp"
#    include "messages/MessageElement.hpp"
#    include "widgets/helper/ScrollbarHighlight.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

// Distinct allocations model shared_ptr reference-count traffic. These are
// deliberately small and do not model a real message's heap allocations.
struct Payload {
    std::uint64_t id;
    std::array<std::uint64_t, 7> padding{};
};
using Pointer = std::shared_ptr<const Payload>;
using Queue = chatterino::LimitedQueue<Pointer>;

volatile std::uint64_t sink = 0;

template <typename Operation>
double measure(Operation operation)
{
    using Clock = std::chrono::steady_clock;
    // Report median of three samples, each at least 100ms. Include destruction
    // of local snapshots, as the application must eventually release them too.
    std::array<double, 3> samples{};
    for (auto &sample : samples)
    {
        std::uint64_t iterations = 0;
        auto start = Clock::now();
        double elapsed;
        do
        {
            for (int i = 0; i < 64; ++i)
            {
                operation();
            }
            iterations += 64;
            elapsed =
                std::chrono::duration<double>(Clock::now() - start).count();
        } while (elapsed < 0.1);
        sample = elapsed * 1e6 / static_cast<double>(iterations);
    }
    std::sort(samples.begin(), samples.end());
    return samples[1];
}

}  // namespace

int main()
{
    // Also keeps reference-count behavior representative on standard libraries
    // that optimize shared_ptr operations in a never-threaded process.
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

#ifdef CACHE_PROBE_OBJECT_SIZES
    std::cerr << "sizeof(Message)=" << sizeof(chatterino::Message) << '\n'
              << "sizeof(MessageLayout)=" << sizeof(chatterino::MessageLayout)
              << '\n'
              << "sizeof(TextElement)=" << sizeof(chatterino::TextElement)
              << '\n'
              << "sizeof(EmoteElement)=" << sizeof(chatterino::EmoteElement)
              << '\n'
              << "sizeof(LayeredEmoteElement)="
              << sizeof(chatterino::LayeredEmoteElement) << '\n'
              << "sizeof(TextLayoutElement)="
              << sizeof(chatterino::TextLayoutElement) << '\n'
              << "sizeof(ScrollbarHighlight)="
              << sizeof(chatterino::ScrollbarHighlight) << '\n';
#endif
    std::cerr << "sizeof(shared_ptr)=" << sizeof(Pointer) << '\n';
    std::cout << "messages,operation,median_us\n"
              << std::fixed << std::setprecision(4);
    for (const std::size_t count :
         {1000, 10000, 100000, 250000, 500000, 1000000})
    {
        Queue queue(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            queue.pushBack(std::make_shared<Payload>(Payload{i}));
        }
        auto emit = [&](const char *name, auto operation) {
            std::cout << count << ',' << name << ',' << measure(operation)
                      << std::endl;
        };

        auto newest = *queue.last();
        // At capacity, every push evicts an item, without allocating a payload.
        emit("append_evict", [&] {
            queue.pushBack(newest);
        });
        // Restore distinct IDs and allocations after append testing.
        queue.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            queue.pushBack(std::make_shared<Payload>(Payload{i}));
        }
        emit("full_snapshot", [&] {
            auto snapshot = queue.getSnapshot();
            sink = snapshot.back()->id;
        });
        emit("tail_3", [&] {
            auto snapshot = queue.lastN(3);
            sink = snapshot.back()->id;
        });
        emit("tail_30", [&] {
            auto snapshot = queue.lastN(30);
            sink = snapshot.back()->id;
        });
        auto snapshot = queue.getSnapshot();
        emit("reuse_snapshot", [&] {
            sink = snapshot.back()->id;
        });
        emit("find_newest", [&] {
            auto found = queue.rfind([&](const auto &item) {
                return item->id == count - 1;
            });
            sink = (*found)->id;
        });
        emit("find_missing", [&] {
            auto found = queue.rfind([&](const auto &item) {
                return item->id == count;
            });
            sink = found.has_value();
        });
        chatterino::MessageViewQueue<Pointer> view(count);
        for (const auto &item : snapshot)
        {
            view.pushBack(item);
        }
        emit("view_live_read", [&] {
            sink = view.live().back()->id;
        });
        emit("view_paused_copy", [&] {
            auto frozen = view.live();
            sink = frozen.back()->id;
        });
    }
    stop.store(true, std::memory_order_relaxed);
    worker.join();
}
