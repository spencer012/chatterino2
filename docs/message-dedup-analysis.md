# Immutable message content pooling

The pool shares equal message text and parsed word-list storage through Qt's
copy-on-write containers. It keeps message identity, sender metadata, moderation
state, emote elements and layout state independent. Messages containing emotes
remain eligible. Parsing still runs for every occurrence.

The circular history retains every occurrence normally. Eviction unregisters
pooled fields; the last participant releases the pool entry. An LRU lookup index
and an admission filter bound lookup bookkeeping. Hash matches always require
full equality. First-seen content does not allocate a pool entry.

## Accounting and evaluation

Current net savings subtract entry/index overhead, the admission filter and the
tracking fields carried by all message/text objects. Capacity estimates count
only independently owned storage released by a hit. Clones and already shared
Qt storage add no new savings. Estimates can be negative and are not RSS or
measured allocator totals. Cumulative duplicate bytes released and lookup wall
time describe lifetime work rather than current retained memory.

The controls and diagnostics are described in
[the cache controls guide](message-cache-controls.md#content-deduplication).
Turning sharing off stops new lookups while existing shared history stays valid.
Compare fresh processes with equal retained workloads for an on/off experiment.

## Local replay tools

`scripts/analyze-message-dedup.py` analyzes explicitly selected local daily logs.
Supply `--log-root`, `--channels`, and an output path outside tracked results.
It preserves exact body equality and excludes stream-specific duplicate files.
Its string/body models are sensitivity scenarios, not actual application savings.

`benchmarks/message-content-probe.cpp` replays text and username word lists through
the production pool. Run a fresh process for each mode and retained capacity.
It treats every body as plain text and does not construct native emote elements,
message envelopes, search text or GUI layouts. Its results cannot predict the
full application's emote-heavy workload. Check that releasing the retained
history returns live payloads and tracking slots to zero.

Session-derived reports, source-file manifests, channel names and local log paths
are intentionally kept out of version control. Synthetic queue and animation
benchmarks may be published without user-specific inputs.
