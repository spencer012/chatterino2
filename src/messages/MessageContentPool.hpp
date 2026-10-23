// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace chatterino {

struct Message;
class MessageContentPool;

namespace detail {

struct ContentPoolEntry;
struct ContentPoolState;

/// Number of MessageContentRef objects alive in the process, shared or not.
/// Every Message and TextElement carries one, whether pooling is on or off.
extern std::atomic<std::size_t> liveContentRefSlots;

}  // namespace detail

/// A participant's claim on one shared, immutable content payload.
///
/// The payload itself is held by the owner's own QString/QStringList, which
/// shares its implicitly-shared data with the pool entry. This handle only
/// tracks that the owner participates, so statistics follow the lifetime of
/// message elements. It never keeps the pool index alive.
///
/// A participant is either *counted* (its payload was a separate copy that
/// the pool replaced, so it contributes to savings) or *passive* (its payload
/// already shared storage, e.g. a clone, so sharing saved nothing). Copying a
/// handle registers a passive participant: copying the owner already shares
/// the Qt payload without any pool. Destroying or resetting it unregisters.
class MessageContentRef
{
public:
    MessageContentRef() noexcept
    {
        detail::liveContentRefSlots.fetch_add(1, std::memory_order_relaxed);
    }
    ~MessageContentRef();

    MessageContentRef(const MessageContentRef &other);
    MessageContentRef &operator=(const MessageContentRef &other);
    MessageContentRef(MessageContentRef &&other) noexcept;
    MessageContentRef &operator=(MessageContentRef &&other) noexcept;

    /// Stops participating. Call this before mutating the payload copy that
    /// this handle describes.
    void reset();

    bool isShared() const
    {
        return this->bits_ != 0;
    }

    /// Whether this handle is a passive participant (see class docs).
    bool isPassive() const
    {
        return (this->bits_ & PASSIVE_BIT) != 0;
    }

private:
    friend class MessageContentPool;

    static constexpr std::uintptr_t PASSIVE_BIT = 1;

    // Pointer to a detail::ContentPoolEntry, tagged with PASSIVE_BIT. Kept
    // pointer-sized so owners pay a single word per slot.
    std::uintptr_t bits_ = 0;
};

struct MessageContentPoolOptions {
    bool enabled = true;

    /// Maximum number of payloads kept in the lookup index. Payloads evicted
    /// from the index stay alive (and shared) while any participant uses them,
    /// but new equal content can no longer find them.
    std::size_t maxIndexedEntries = 8192;

    /// Size of the admission filter (rounded up to a power of two). A payload
    /// that is not in the index only gets a pool entry the second time its
    /// hash is seen within the filter's window, so content seen once costs no
    /// per-payload bookkeeping. 0 admits every payload on first sight.
    std::size_t admissionFilterSlots = 8192;

    /// Payloads with more UTF-16 code units than this are not pooled.
    qsizetype maxPayloadLength = 16384;

    /// Testing only: make every payload hash to the same bucket.
    bool forceHashCollisionsForTesting = false;
};

/// All byte values are estimates from Qt capacities and a fixed allocator
/// model (16-byte Qt array header, 16-byte rounding and a 16-byte allocator
/// allowance per allocation). They are not measurements of process memory.
///
/// Savings only count storage that was actually released: when a hit
/// replaces a payload, only parts of it that were not already shared with
/// other storage (detached lists and strings) are counted, sized by their
/// capacity. Clones and other passive participants never count.
struct MessageContentPoolStats {
    bool enabled = false;

    /// Payload entries currently in the lookup index.
    std::size_t indexedEntries = 0;
    std::size_t maxIndexedEntries = 0;

    /// Entries alive because at least one participant uses them, whether
    /// indexed or not.
    std::size_t liveEntries = 0;
    /// Live entries that were evicted from (or never re-entered) the index.
    /// They cannot gain counted participants anymore but stay until their
    /// participants die.
    std::size_t unindexedLiveEntries = 0;
    /// Element/message fields currently sharing a pooled payload, counted
    /// and passive.
    std::size_t liveParticipants = 0;
    /// The subset of `liveParticipants` that are passive.
    std::size_t passiveParticipants = 0;

    /// Estimated bytes (by capacity) of one copy of every live payload.
    std::size_t retainedPayloadBytes = 0;
    /// Conservative estimate of bytes that would exist as duplicate copies
    /// without sharing, for counted participants that are alive. Per entry:
    /// (counted participants - 1) * smallest copy released by any hit.
    std::size_t retainedGrossSavedBytes = 0;
    /// Estimated per-payload bookkeeping: entry objects and index nodes.
    std::size_t estimatedOverheadBytes = 0;
    /// Fixed bookkeeping that exists regardless of content (admission filter).
    /// Included in `estimatedOverheadBytes`.
    std::size_t fixedOverheadBytes = 0;

    /// Process-wide MessageContentRef slots (one per Message and per
    /// TextElement, pooled or not, for every pool) and their bytes. These
    /// exist even with pooling disabled. Note that real allocation size
    /// classes may hide or amplify the 8 bytes per object.
    std::size_t trackingSlots = 0;
    std::size_t trackingSlotBytes = 0;

    /// Gross savings minus overhead and tracking slots. Can be negative.
    /// Only meaningful for the global pool (tracking slots are process-wide).
    std::int64_t retainedNetSavedBytes = 0;

    std::uint64_t hits = 0;
    /// Hits that released no storage (payload already shared). Included in
    /// `hits`; these became passive participants.
    std::uint64_t hitsWithoutSaving = 0;
    std::uint64_t misses = 0;
    /// Payloads seen for the first time by the admission filter and left
    /// unpooled.
    std::uint64_t deferred = 0;
    /// Payloads that were not considered (empty, too long or pool disabled).
    std::uint64_t skipped = 0;
    /// Entries dropped from the index because it reached its bound.
    std::uint64_t evictions = 0;
    /// Entries removed because their last participant died.
    std::uint64_t expired = 0;

    /// Estimated bytes of freshly built payload copies released by hits, over
    /// the lifetime of the pool. Parsing still allocates those copies
    /// transiently; this counts duplicates that were not retained.
    std::uint64_t cumulativeDeduplicatedBytes = 0;

    /// Wall-clock time spent inside pool lookups on the calling threads.
    std::uint64_t processingTimeNs = 0;
};

/// Deduplicates immutable content payloads of message elements.
///
/// Each message occurrence keeps its own Message, its own elements and all
/// per-occurrence state (ID, sender, time, flags, reply context, colors,
/// links, emotes, layout caches). What is shared are the immutable, implicitly
/// shared Qt payloads inside them: the word lists of text runs (including
/// mentions, usernames, timestamps and emote text fallbacks) and the message
/// text.
///
/// The key is the complete payload compared with full equality, so collisions
/// never share unequal content. Because only leaf payloads are shared, the
/// surrounding emotes, colors and flags of an occurrence do not need to be
/// part of the key and emote-bearing messages participate normally.
///
/// The index only references payloads that still have participants. When the
/// last participant of a payload dies, the entry is removed from the index.
/// The index is bounded by `maxIndexedEntries` in LRU order. An admission
/// filter keeps payloads seen only once from allocating entries.
///
/// Thread safe. Participants may be destroyed on any thread and may outlive
/// the pool.
class MessageContentPool
{
public:
    explicit MessageContentPool(MessageContentPoolOptions options = {});
    ~MessageContentPool();

    MessageContentPool(const MessageContentPool &) = delete;
    MessageContentPool &operator=(const MessageContentPool &) = delete;
    MessageContentPool(MessageContentPool &&) = delete;
    MessageContentPool &operator=(MessageContentPool &&) = delete;

    /// The process-wide pool used by MessageBuilder and element layout.
    static MessageContentPool &global();

    /// Disabling clears the index and the admission filter and stops pooling
    /// new content. Content that is already shared stays valid and keeps being
    /// tracked until its participants die.
    void setEnabled(bool enabled);
    bool isEnabled() const;

    void setMaxIndexedEntries(std::size_t maxEntries);

    /// Forgets all lookup entries. Shared content stays alive.
    void clearIndex();

    /// Replaces `words` with an equal pooled list and registers `ref` as a
    /// participant. Returns true if `words` is now pooled (hit or miss).
    bool shareWords(QStringList &words, MessageContentRef &ref);

    /// Same as shareWords, for a single string.
    bool shareText(QString &text, MessageContentRef &ref);

    /// Shares the payloads of every eligible element and the message text of
    /// a fully built message. Returns the number of pooled payloads.
    ///
    /// The message must not be visible to other threads yet.
    std::size_t shareMessageContent(Message &message);

    MessageContentPoolStats stats() const;

private:
    std::shared_ptr<detail::ContentPoolState> state_;
};

}  // namespace chatterino
