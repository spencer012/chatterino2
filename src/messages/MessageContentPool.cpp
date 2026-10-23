// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageContentPool.hpp"

#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"

#include <QHash>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chatterino {

namespace detail {

std::atomic<std::size_t> liveContentRefSlots{0};

enum class ContentKind : std::uint8_t {
    Words,
    Text,
};

/// `copyBytes` before any hit released a separate copy.
constexpr std::size_t NO_COPY = std::numeric_limits<std::size_t>::max();

struct ContentPoolEntry {
    ContentPoolEntry(std::shared_ptr<ContentPoolState> state_,
                     ContentKind kind_, std::size_t hash_)
        : state(std::move(state_))
        , hash(hash_)
        , kind(kind_)
    {
    }

    // Keeps the shared state (and its mutex) alive for as long as any
    // participant exists, even after the pool object is destroyed.
    std::shared_ptr<ContentPoolState> state;

    // Exactly one of these holds the payload, depending on `kind`.
    QStringList words;
    QString text;

    std::size_t hash;
    std::size_t payloadBytes = 0;

    // The fields below are guarded by state->mutex.

    // Smallest storage released by a counted hit. Used as the conservative
    // size of one duplicate copy.
    std::size_t copyBytes = NO_COPY;
    std::uint32_t counted = 0;
    std::uint32_t passive = 0;
    ContentKind kind;
    bool indexed = false;

    // Intrusive LRU list of indexed entries, most recently used first.
    ContentPoolEntry *lruPrev = nullptr;
    ContentPoolEntry *lruNext = nullptr;
};

// The low bit of MessageContentRef::bits_ marks passive participants.
static_assert(alignof(ContentPoolEntry) >= 2);

struct ContentPoolState {
    explicit ContentPoolState(const MessageContentPoolOptions &options_)
        : enabled(options_.enabled)
        , maxIndexedEntries(options_.maxIndexedEntries)
        , maxPayloadLength(options_.maxPayloadLength)
        , forceHashCollisions(options_.forceHashCollisionsForTesting)
    {
        if (options_.admissionFilterSlots > 0)
        {
            this->filter.resize(std::bit_ceil(options_.admissionFilterSlots));
        }
    }

    mutable std::mutex mutex;

    std::atomic<bool> enabled;
    std::size_t maxIndexedEntries;
    const qsizetype maxPayloadLength;
    const bool forceHashCollisions;

    std::unordered_multimap<std::size_t, ContentPoolEntry *> index;
    ContentPoolEntry *lruHead = nullptr;
    ContentPoolEntry *lruTail = nullptr;

    // Direct-mapped hash fingerprints of payloads seen once. Fixed size.
    std::vector<std::uint64_t> filter;

    std::size_t liveEntries = 0;
    std::size_t liveParticipants = 0;
    std::size_t passiveParticipants = 0;
    std::size_t retainedPayloadBytes = 0;
    std::size_t retainedGrossSavedBytes = 0;

    std::uint64_t hits = 0;
    std::uint64_t hitsWithoutSaving = 0;
    std::uint64_t misses = 0;
    std::uint64_t deferred = 0;
    std::atomic<std::uint64_t> skipped = 0;
    std::uint64_t evictions = 0;
    std::uint64_t expired = 0;
    std::uint64_t cumulativeDeduplicatedBytes = 0;
    std::atomic<std::uint64_t> processingTimeNs = 0;

    void lruUnlink(ContentPoolEntry *entry)
    {
        if (entry->lruPrev != nullptr)
        {
            entry->lruPrev->lruNext = entry->lruNext;
        }
        else
        {
            this->lruHead = entry->lruNext;
        }
        if (entry->lruNext != nullptr)
        {
            entry->lruNext->lruPrev = entry->lruPrev;
        }
        else
        {
            this->lruTail = entry->lruPrev;
        }
        entry->lruPrev = nullptr;
        entry->lruNext = nullptr;
    }

    void lruPushFront(ContentPoolEntry *entry)
    {
        entry->lruPrev = nullptr;
        entry->lruNext = this->lruHead;
        if (this->lruHead != nullptr)
        {
            this->lruHead->lruPrev = entry;
        }
        this->lruHead = entry;
        if (this->lruTail == nullptr)
        {
            this->lruTail = entry;
        }
    }

    void unindex(ContentPoolEntry *entry)
    {
        if (!entry->indexed)
        {
            return;
        }
        auto [it, end] = this->index.equal_range(entry->hash);
        for (; it != end; ++it)
        {
            if (it->second == entry)
            {
                this->index.erase(it);
                break;
            }
        }
        this->lruUnlink(entry);
        entry->indexed = false;
    }

    void evictToBound()
    {
        while (this->index.size() > this->maxIndexedEntries &&
               this->lruTail != nullptr)
        {
            this->unindex(this->lruTail);
            this->evictions++;
        }
    }

    void clearIndex()
    {
        while (this->lruHead != nullptr)
        {
            this->unindex(this->lruHead);
        }
    }

    void clearFilter()
    {
        std::fill(this->filter.begin(), this->filter.end(), 0);
    }

    /// Returns true if a payload with `hash` should get an entry now. The
    /// first sighting only records the hash.
    bool admit(std::size_t hash)
    {
        if (this->filter.empty())
        {
            return true;
        }
        const auto wide = static_cast<std::uint64_t>(hash);
        const std::uint64_t fingerprint = wide | 1;
        auto &slot =
            this->filter[static_cast<std::size_t>(wide ^ (wide >> 32)) &
                         (this->filter.size() - 1)];
        if (slot == fingerprint)
        {
            // The entry takes over; free the slot for other payloads.
            slot = 0;
            return true;
        }
        slot = fingerprint;
        this->deferred++;
        return false;
    }

    static std::size_t contribution(const ContentPoolEntry &entry)
    {
        if (entry.counted < 2 || entry.copyBytes == NO_COPY)
        {
            return 0;
        }
        return static_cast<std::size_t>(entry.counted - 1) * entry.copyBytes;
    }

    /// Adds a participant whose own copy was replaced, releasing
    /// `releasedBytes` (NO_COPY for the first participant of an entry).
    void addCounted(ContentPoolEntry *entry, std::size_t releasedBytes)
    {
        this->retainedGrossSavedBytes -= contribution(*entry);
        entry->counted++;
        entry->copyBytes = std::min(entry->copyBytes, releasedBytes);
        this->retainedGrossSavedBytes += contribution(*entry);
        this->liveParticipants++;
    }

    /// Adds a participant whose payload already shared storage.
    void addPassive(ContentPoolEntry *entry)
    {
        entry->passive++;
        this->liveParticipants++;
        this->passiveParticipants++;
    }

    /// Returns true if the entry has no participants left and must be
    /// deleted by the caller after releasing the lock.
    bool removeParticipant(ContentPoolEntry *entry, bool passive)
    {
        this->retainedGrossSavedBytes -= contribution(*entry);
        if (passive)
        {
            assert(entry->passive > 0);
            entry->passive--;
            this->passiveParticipants--;
        }
        else
        {
            assert(entry->counted > 0);
            entry->counted--;
        }
        this->liveParticipants--;
        this->retainedGrossSavedBytes += contribution(*entry);

        if (entry->counted > 0 || entry->passive > 0)
        {
            return false;
        }

        if (entry->indexed)
        {
            this->unindex(entry);
            this->expired++;
        }
        this->liveEntries--;
        this->retainedPayloadBytes -= entry->payloadBytes;
        return true;
    }
};

}  // namespace detail

namespace {

using detail::ContentKind;
using detail::ContentPoolEntry;
using detail::ContentPoolState;
using detail::NO_COPY;

// Allocation model shared with docs/message-dedup-analysis.md.
constexpr std::size_t QT_ARRAY_HEADER_BYTES = 16;
constexpr std::size_t ALLOCATOR_ALLOWANCE_BYTES = 16;
// One unordered_multimap node (links, cached hash, key, value) plus its share
// of the bucket array.
constexpr std::size_t INDEX_NODE_BYTES = 48 + ALLOCATOR_ALLOWANCE_BYTES + 16;

constexpr std::uintptr_t PASSIVE_TAG = 1;

constexpr std::size_t roundUp16(std::size_t n)
{
    return (n + 15) & ~std::size_t{15};
}

ContentPoolEntry *entryOf(std::uintptr_t bits)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<ContentPoolEntry *>(bits & ~PASSIVE_TAG);
}

std::uintptr_t tagged(ContentPoolEntry *entry, bool passive)
{
    return reinterpret_cast<std::uintptr_t>(entry) |
           (passive ? PASSIVE_TAG : 0);
}

std::size_t allocationBytes(std::size_t dataBytes)
{
    return roundUp16(QT_ARRAY_HEADER_BYTES + dataBytes) +
           ALLOCATOR_ALLOWANCE_BYTES;
}

/// Estimated heap size of the string's own allocation. Static and raw data
/// (e.g. literals) report no capacity and cost nothing.
std::size_t allocatedBytes(const QString &str)
{
    const auto capacity = str.capacity();
    if (capacity <= 0)
    {
        return 0;
    }
    return allocationBytes(sizeof(char16_t) *
                           (static_cast<std::size_t>(capacity) + 1));
}

std::size_t allocatedArrayBytes(const QStringList &words)
{
    const auto capacity = words.capacity();
    if (capacity <= 0)
    {
        return 0;
    }
    return allocationBytes(sizeof(QString) *
                           static_cast<std::size_t>(capacity));
}

/// Estimated heap size of one copy of the payload, including strings that
/// may also be shared with other storage.
std::size_t allocatedBytes(const QStringList &words)
{
    std::size_t bytes = allocatedArrayBytes(words);
    for (const auto &word : words)
    {
        bytes += allocatedBytes(word);
    }
    return bytes;
}

/// Estimated bytes freed if `str` were released now: only storage that no
/// one else references.
std::size_t releasableBytes(const QString &str)
{
    return str.isDetached() ? allocatedBytes(str) : 0;
}

std::size_t releasableBytes(const QStringList &words)
{
    if (!words.isDetached())
    {
        // Another list holds the same array, so neither it nor the strings
        // in it would be freed.
        return 0;
    }
    std::size_t bytes = allocatedArrayBytes(words);
    for (const auto &word : words)
    {
        bytes += releasableBytes(word);
    }
    return bytes;
}

qsizetype payloadLength(const QString &str)
{
    return str.size();
}

qsizetype payloadLength(const QStringList &words)
{
    qsizetype len = 0;
    for (const auto &word : words)
    {
        len += word.size() + 1;
    }
    return len;
}

std::size_t payloadHash(const QString &str)
{
    return qHash(str, 0x2545F491U);
}

std::size_t payloadHash(const QStringList &words)
{
    // qHashRange includes every element in order, but not the boundaries
    // between them, so mix in the count and each length.
    std::size_t seed = 0x9E3779B9U ^ static_cast<std::size_t>(words.size());
    for (const auto &word : words)
    {
        seed = qHashMulti(seed, word.size(), word);
    }
    return seed;
}

QStringList &payloadOf(ContentPoolEntry &entry, const QStringList * /*tag*/)
{
    return entry.words;
}

QString &payloadOf(ContentPoolEntry &entry, const QString * /*tag*/)
{
    return entry.text;
}

class ProcessingTimer
{
public:
    explicit ProcessingTimer(ContentPoolState &state)
        : state_(state)
        , start_(std::chrono::steady_clock::now())
    {
    }

    ~ProcessingTimer()
    {
        auto elapsed = std::chrono::steady_clock::now() - this->start_;
        this->state_.processingTimeNs.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    .count()),
            std::memory_order_relaxed);
    }

    ProcessingTimer(const ProcessingTimer &) = delete;
    ProcessingTimer &operator=(const ProcessingTimer &) = delete;
    ProcessingTimer(ProcessingTimer &&) = delete;
    ProcessingTimer &operator=(ProcessingTimer &&) = delete;

private:
    ContentPoolState &state_;
    std::chrono::steady_clock::time_point start_;
};

template <typename Payload>
bool sharePayload(const std::shared_ptr<ContentPoolState> &statePtr,
                  ContentKind kind, Payload &payload, std::uintptr_t &refBits)
{
    auto &state = *statePtr;
    if (refBits != 0)
    {
        // Already participating. Owners reset the handle before mutating.
        return true;
    }

    if (!state.enabled.load(std::memory_order_relaxed) || payload.isEmpty())
    {
        state.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ProcessingTimer timer(state);

    if (payloadLength(payload) > state.maxPayloadLength)
    {
        state.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::size_t hash =
        state.forceHashCollisions
            ? 0
            : payloadHash(payload) ^ static_cast<std::size_t>(kind);

    // Destroyed after the lock is released: the freshly built duplicate.
    Payload replaced;

    std::lock_guard lock(state.mutex);
    if (!state.enabled.load(std::memory_order_relaxed) ||
        state.maxIndexedEntries == 0)
    {
        state.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto [it, end] = state.index.equal_range(hash);
    for (; it != end; ++it)
    {
        auto *entry = it->second;
        auto &shared = payloadOf(*entry, &payload);
        if (entry->kind != kind || shared != payload)
        {
            continue;
        }

        // Measured before the swap: what releasing our own copy frees.
        const std::size_t released = releasableBytes(payload);
        replaced = std::exchange(payload, shared);
        state.hits++;
        if (released == 0)
        {
            // Our payload already shared its storage (e.g. a clone), so
            // pooling it saves nothing.
            state.addPassive(entry);
            state.hitsWithoutSaving++;
            refBits = tagged(entry, true);
        }
        else
        {
            state.addCounted(entry, released);
            state.cumulativeDeduplicatedBytes += released;
            refBits = tagged(entry, false);
        }
        state.lruUnlink(entry);
        state.lruPushFront(entry);
        return true;
    }

    if (!state.admit(hash))
    {
        return false;
    }

    auto *entry = new ContentPoolEntry(statePtr, kind, hash);
    payloadOf(*entry, &payload) = payload;
    entry->payloadBytes = allocatedBytes(payload);
    entry->indexed = true;
    state.index.emplace(hash, entry);
    state.lruPushFront(entry);
    state.liveEntries++;
    state.retainedPayloadBytes += entry->payloadBytes;
    state.addCounted(entry, NO_COPY);
    state.misses++;
    refBits = tagged(entry, false);

    state.evictToBound();
    return true;
}

}  // namespace

// MessageContentRef

MessageContentRef::~MessageContentRef()
{
    static_assert(PASSIVE_BIT == PASSIVE_TAG);
    this->reset();
    detail::liveContentRefSlots.fetch_sub(1, std::memory_order_relaxed);
}

MessageContentRef::MessageContentRef(const MessageContentRef &other)
    : MessageContentRef()
{
    auto *entry = entryOf(other.bits_);
    if (entry != nullptr)
    {
        // Copying the owner shares its Qt payload even without a pool, so a
        // copy is tracked for lifetime but never counted as a saving.
        std::lock_guard lock(entry->state->mutex);
        entry->state->addPassive(entry);
        this->bits_ = tagged(entry, true);
    }
}

MessageContentRef &MessageContentRef::operator=(const MessageContentRef &other)
{
    if (this != &other)
    {
        MessageContentRef copy(other);
        *this = std::move(copy);
    }
    return *this;
}

MessageContentRef::MessageContentRef(MessageContentRef &&other) noexcept
    : MessageContentRef()
{
    this->bits_ = std::exchange(other.bits_, 0);
}

MessageContentRef &MessageContentRef::operator=(
    MessageContentRef &&other) noexcept
{
    if (this != &other)
    {
        this->reset();
        this->bits_ = std::exchange(other.bits_, 0);
    }
    return *this;
}

void MessageContentRef::reset()
{
    const auto bits = std::exchange(this->bits_, 0);
    if (bits == 0)
    {
        return;
    }
    auto *entry = entryOf(bits);

    bool remove = false;
    {
        std::lock_guard lock(entry->state->mutex);
        remove =
            entry->state->removeParticipant(entry, (bits & PASSIVE_TAG) != 0);
    }
    if (remove)
    {
        // Outside the lock: this may destroy the state and its mutex.
        delete entry;
    }
}

// MessageContentPool

MessageContentPool::MessageContentPool(MessageContentPoolOptions options)
    : state_(std::make_shared<ContentPoolState>(options))
{
}

MessageContentPool::~MessageContentPool()
{
    // Live entries keep the state alive through their participants.
    this->clearIndex();
}

MessageContentPool &MessageContentPool::global()
{
    static MessageContentPool pool;
    return pool;
}

void MessageContentPool::setEnabled(bool enabled)
{
    std::lock_guard lock(this->state_->mutex);
    this->state_->enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled)
    {
        this->state_->clearIndex();
        this->state_->clearFilter();
    }
}

bool MessageContentPool::isEnabled() const
{
    return this->state_->enabled.load(std::memory_order_relaxed);
}

void MessageContentPool::setMaxIndexedEntries(std::size_t maxEntries)
{
    std::lock_guard lock(this->state_->mutex);
    this->state_->maxIndexedEntries = maxEntries;
    this->state_->evictToBound();
}

void MessageContentPool::clearIndex()
{
    std::lock_guard lock(this->state_->mutex);
    this->state_->clearIndex();
}

bool MessageContentPool::shareWords(QStringList &words, MessageContentRef &ref)
{
    return sharePayload(this->state_, ContentKind::Words, words, ref.bits_);
}

bool MessageContentPool::shareText(QString &text, MessageContentRef &ref)
{
    return sharePayload(this->state_, ContentKind::Text, text, ref.bits_);
}

std::size_t MessageContentPool::shareMessageContent(Message &message)
{
    if (!this->isEnabled())
    {
        return 0;
    }

    std::size_t shared = 0;
    for (const auto &element : message.elements)
    {
        shared += element->shareContent(*this);
    }
    if (this->shareText(message.messageText, message.messageTextContent))
    {
        shared++;
    }
    return shared;
}

MessageContentPoolStats MessageContentPool::stats() const
{
    const auto &state = *this->state_;
    std::lock_guard lock(state.mutex);

    MessageContentPoolStats stats;
    stats.enabled = state.enabled.load(std::memory_order_relaxed);
    stats.indexedEntries = state.index.size();
    stats.maxIndexedEntries = state.maxIndexedEntries;
    stats.liveEntries = state.liveEntries;
    stats.unindexedLiveEntries = state.liveEntries - state.index.size();
    stats.liveParticipants = state.liveParticipants;
    stats.passiveParticipants = state.passiveParticipants;
    stats.retainedPayloadBytes = state.retainedPayloadBytes;
    stats.retainedGrossSavedBytes = state.retainedGrossSavedBytes;
    stats.fixedOverheadBytes =
        state.filter.empty() ? 0
                             : (state.filter.size() * sizeof(std::uint64_t)) +
                                   ALLOCATOR_ALLOWANCE_BYTES;
    stats.estimatedOverheadBytes =
        (state.liveEntries * (roundUp16(sizeof(ContentPoolEntry)) +
                              ALLOCATOR_ALLOWANCE_BYTES)) +
        (state.index.size() * INDEX_NODE_BYTES) + stats.fixedOverheadBytes;
    stats.trackingSlots =
        detail::liveContentRefSlots.load(std::memory_order_relaxed);
    stats.trackingSlotBytes = stats.trackingSlots * sizeof(MessageContentRef);
    stats.retainedNetSavedBytes =
        static_cast<std::int64_t>(stats.retainedGrossSavedBytes) -
        static_cast<std::int64_t>(stats.estimatedOverheadBytes) -
        static_cast<std::int64_t>(stats.trackingSlotBytes);
    stats.hits = state.hits;
    stats.hitsWithoutSaving = state.hitsWithoutSaving;
    stats.misses = state.misses;
    stats.deferred = state.deferred;
    stats.skipped = state.skipped.load(std::memory_order_relaxed);
    stats.evictions = state.evictions;
    stats.expired = state.expired;
    stats.cumulativeDeduplicatedBytes = state.cumulativeDeduplicatedBytes;
    stats.processingTimeNs =
        state.processingTimeNs.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace chatterino
