# Large message cache investigation

Investigated on 2026-10-22 at `1ead6c1ce`, using the current working tree.
This investigation adds a standalone benchmark and this report. It does not
change application behavior, settings, or the existing channel-points work.

The ring buffer is already suitable for a larger cache. The main CPU costs are
copies and scans around it. Removing those costs should make more than 100,000
messages practical to test, but memory use needs separate work before a million
messages can be a sensible general limit. No larger limit has been validated in
the running GUI.

## Measurements

The probe uses the production `LimitedQueue` header, distinct shared pointers,
and the existing x64 MSVC release configuration with `/O2` and `NDEBUG`. Each
payload is 64 bytes, deliberately smaller than a real message. Each result is
the median of three samples of at least 100 ms. Allocation and destruction of
the snapshot vector and its shared pointers are included. Queue population is
excluded.

These are isolated queue costs. They do not measure total GUI frame time,
rendering, actual message allocations, memory consumption, or lock contention.
Cache layout and other running programs affect the numbers. In particular,
large-cache results should not be extrapolated linearly from the 1,000-entry case.

| Cached messages | Full snapshot | Last 30 entries | Missing ID scan |
| ---: | ---: | ---: | ---: |
| 1,000 | 0.0097 ms | 0.347 us | 0.0016 ms |
| 10,000 | 0.1565 ms | 0.350 us | 0.0287 ms |
| 100,000 | 1.8368 ms | 0.362 us | 0.3375 ms |
| 250,000 | 11.5497 ms | 0.349 us | 4.6666 ms |
| 500,000 | 23.4276 ms | 0.344 us | 9.5209 ms |
| 1,000,000 | 39.2480 ms | 0.341 us | 18.0923 ms |

Append and eviction remained around 0.02 us. This operation reuses a pointer and
excludes message construction. Finding the newest entry stayed around 0.02 us.
Copying only the last three pointers stayed around 0.08-0.10 us.

A second complete run confirmed the trend. Full snapshots took 1.78 ms at
100,000 entries, 9.89 ms at 250,000, 19.33 ms at 500,000, and 38.99 ms at one
million. The complete second-run measurements are saved in
[`benchmarks/results/message-cache-2026-10-22.csv`](../benchmarks/results/message-cache-2026-10-22.csv).
The table above records the first run.

A full snapshot at 500,000 entries already exceeds the entire 16.7 ms budget
for a 60 Hz frame in this probe. One snapshot at 60 calls per second would
consume about 1.4 seconds of CPU time per second. The current application makes
snapshots in several paths, so there is no basis for just raising the setting.
This arithmetic illustrates the cost, it is not an observed application rate.

## Findings and proposed changes

### 1. Avoid copying history for disabled similarity filtering

`Channel::applySimilarityFilters` in `src/common/Channel.cpp` unconditionally
calls `messages_.getSnapshot()`. Only after copying all pointers does
`setSimilarityFlags` check `similarityEnabled`. The setting defaults to false.
`IrcMessageHandler::addMessage` calls this hook for normal parsed messages, including
messages that will not need a similarity comparison.

When enabled, `inMessages` examines only `hideSimilarMaxMessagesToCheck` recent
messages, which defaults to three. It can stop even earlier based on age.

Return before constructing a snapshot when filtering is disabled. When enabled,
take only the configured tail with the existing `getMessageSnapshot(n)` or
`lastN(n)` API. Preserve the same-user, own-message, timestamp, and flag rules.
The snapshot then costs O(K), where K is the comparison setting, rather than O(N)
for the cache size. A deliberately large K still has a cost.

This is the smallest, clearest first change. The string comparison algorithm is
also expensive, with a matrix proportional to both text lengths, but that is a
separate cost from the amount of history retained.

### 2. Stop taking a full snapshot on every paint and interaction

`ChannelView::getMessagesSnapshot` copies the entire layout queue every time it
is called while unpaused, even if no message changed. Callers include
`performLayout`, `drawMessages`, wheel handling, hit testing, message navigation,
and replay scrolling. `queueLayout` performs layout synchronously for a visible
view, so every append can incur a full copy before a later paint copies again.
Animated emotes and smooth scrolling can trigger further paints without new
messages.

First, track a mutation generation and reuse `snapshot_` until the queue changes.
Invalidate on append, prepend, replacement, clear, and history refill. Cache size
alone cannot detect mutations when a full ring evicts and appends. Keep paused
snapshots frozen until unpause, with the existing selection and scrollbar offsets.
Coalesce layout requests on the GUI event loop to avoid repeating layout for
each message in an incoming burst.

Generation caching removes copies for unchanged history, but still costs O(N)
for each refreshed snapshot during continuous chat. For sustained large caches,
use viewport range reads or immutable chunks with stable sequence numbers. Copy
only visible entries plus a small margin, and fetch the bottom range separately
for page-size calculation. Paused reading needs owned references to the frozen
data. A single shared vector with copy-on-write would still copy N entries on
each append while readers retain it.

The queue is shared infrastructure. Do not remove its locking or hand out unsafe
references as a shortcut. A read callback under its lock must not invoke signals
that can reenter a queue mutation.

### 3. Bound the offscreen-buffer bookkeeping to the viewport

In `ChannelView::drawMessages`, the loop after the comment
`remove messages that are on screen` scans from `start` to the end of the cache.
The paint loop has already found the last visible layout, `end`, but this first
cleanup loop does not use it. Scrolling near the beginning of a large cache
therefore performs almost N hash-set lookups per paint. This also erases some
previously visible layouts that are now below the viewport from the cleanup set,
preventing their buffers from being deleted in that pass.

Track the visible end index in the paint loop and use the exact visible range
for both cleanup and rebuilding `messagesOnScreen_`. Work should depend on the
previous and current visible sets. Verify pixel-buffer release when scrolling
up as well as down.

### 4. Make replay indexing handle eviction incrementally

`Split::replayStepTimer_` runs at 50 ms intervals while replay is active.
`ChannelView::scrollToServerTime` requests a full snapshot even when the timestamp
index is unchanged. Its append optimization requires the first cached layout to
remain the same and the snapshot to grow. Once the ring is full, every eviction
changes the first layout, so the next replay update clears and rebuilds the
entire timestamp index. Seeking itself already uses `upper_bound`.

Maintain timestamp entries against stable sequence numbers. Pop evicted entries
and append new entries, translating a sequence number into the current view
offset when positioning the scrollbar. Use a ring or deque so removing the oldest
entry does not shift the whole index. Handle replacements, missing timestamps,
history insertion, clears, and timestamp-order assumptions explicitly. Rebuild
for exceptional structural changes rather than every normal eviction. Snapshot
generation caching alone does not solve rebuilding at capacity.

### 5. Cache the scrollbar highlight rendering

`Scrollbar::paintEvent` visits every highlight entry, including null entries,
whenever it paints. There is one slot per cached message. Repainting a few hundred
vertical pixels should not repeatedly scan hundreds of thousands of entries.

Cache a rendered highlight layer and invalidate it on highlight changes, size,
scale, theme, and relevant settings. Draw thumb movement over that layer. When
message traffic keeps invalidating it, coalesce rebuilds and consider pixel-row
aggregation or a sparse marker index. Preserve the existing paint order, colors,
and styles where multiple markers overlap. A cached layer alone still has an
O(N) rebuild cost on updates.

### 6. Treat lookup, moderation, refill, and search as the next tier

- `Channel::findMessageByID` searches backwards. Recent hits are cheap, but
  missing IDs and old hits scan the cache. Replies can perform multiple lookups.
  An ID index needs eviction/replacement updates and must preserve newest-match
  behavior for duplicate IDs. Empty system-message IDs should not fill the index.
  Account for the map's own memory cost before enabling it universally.
- Timeout handling copies and scans all messages to disable a user's history.
  Its stacking check only uses 20 recent entries, but replacing the whole
  snapshot with a 20-entry tail would incorrectly stop disabling older messages.
  Separate stacking from user-message updates. A per-user index may help if
  profiling shows moderation traffic matters, but it adds memory and bookkeeping.
- `Channel::fillInMissingMessages` builds an ID set and then repeatedly scans the
  existing snapshot and inserts into a circular buffer. Cost can approach O(H*N)
  for H loaded messages and N retained messages. A batch merge can avoid repeated
  scans and moves. Preserve retention and ordering rules. Views currently rebuild
  every layout after refill, so changing the channel merge alone is insufficient.
- Search filters the snapshot and rebuilds result channels and layouts on every
  text change. Multichannel search also copies and sorts combined history.
  Debounce input, cancel superseded searches, and page results before considering
  full-text indexing. Background work should use safe searchable metadata, with
  GUI layout and pixmap work remaining on the GUI thread.
- Usercards scan full channel history to select a user's messages. The usercard
  display limit does not bound that source scan. Reuse a per-user index if one is
  justified by moderation or usercard measurements.

## Memory and larger limits

Measured object sizes for this release configuration are:

| Object | Bytes, excluding separately allocated data |
| --- | ---: |
| `Message` | 464 |
| `MessageLayout` | 240 |
| `TextElement` | 128 |
| `TextLayoutElement` | 160 |
| `ScrollbarHighlight` | 24 |
| `shared_ptr` | 16 |

A normal split has a backing channel queue, a filtered proxy channel queue, a
layout queue, a current snapshot, and scrollbar markers. The message objects are
shared between them, not copied wholesale. With a full current snapshot, those
four pointer arrays and the markers add 88 bytes per slot. Adding one message
and one layout gives a fixed subtotal of 792 bytes per retained message:

| Retained messages in one channel shown in one split | Fixed subtotal |
| ---: | ---: |
| 100,000 | 75.5 MiB |
| 250,000 | 188.8 MiB |
| 500,000 | 377.7 MiB |
| 1,000,000 | 755.3 MiB |

These are calculated lower subtotals, not measured process memory. They exclude
shared-pointer control blocks, allocation overhead, string data, badges, original
message elements, laid-out text elements, images, reply threads, and transient
snapshots. Additional views share message objects when possible but add their own
layouts, queues, markers, and snapshots. Differently filtered views can retain
messages already evicted from their backing channel.

Queue capacity is allocated up front. For one channel and one split, pointer
queues and markers alone reserve about 68.7 MiB at a one-million-entry setting,
before populating messages or taking a snapshot. A paused snapshot can keep old
messages and layouts alive as the live queue turns over. `replyParent` and thread
roots can retain messages beyond the ring's limit. Thus a per-queue message limit
is not a process-wide bound on live message objects or bytes.

Pixmap buffers are normally released when messages leave the viewport, subject
to the cleanup issue above. Text layout elements and line vectors remain in
`MessageLayoutContainer`. `MessageLayout::deleteCache` only clears the container
inside an `#ifdef XD` block. Clearing pixmaps alone will not recover most retained
layout data after reading through a long history.

Use a bounded cache of detailed layouts around the viewport, retaining lightweight
message references and height metadata for older entries. Rebuilding a layout
must preserve scrolling, selection/copy behavior, hit testing, and links. Measure
heap ownership before choosing a byte budget. For much deeper history, use a
bounded hot cache and a searchable disk archive with asynchronous page loading.
That requires a format that can reconstruct rendering and reply context; plain
text logs alone do not preserve all of that information.

The settings UI currently allows up to 100,000 messages for splits and usercards.
The 800-message connect-history limit is separate. A larger retention cache does
not automatically fetch more historical messages at connection time.

## Implementation order and validation

### Follow-up decisions: duplicate content and per-channel limits

Repeated text can share storage while every occurrence remains a separate entry
in the circular buffer. Each occurrence keeps its own message ID, sender,
timestamp, flags, and reply context. The buffer evicts occurrences in the normal
order; shared content remains alive while any occurrence or frozen snapshot needs
it. Ring-buffer storage does not prevent this design.

Do not deduplicate entire `Message` objects by text. Deleting one occurrence,
timing out one sender, selecting a reply target, and replaying messages at their
original timestamps must still work independently. Representing a hundred
occurrences as one message with a count would require a separate design for all
of those operations.

Start with a bounded pool for exact repeated text if heap measurements justify
it. QString copies can share backing storage, but independently received equal
strings are not automatically interned. Use content hashing and equality checks,
not a scan through the message ring. Bound the pool's own retained memory and
clean up unused entries. A weak value alone does not bound memory if the map's
owning string keys keep all content alive.

Sharing parsed body data offers more savings for long repeated text, but requires
a refactor. Message elements currently have unique ownership, layout elements
refer back to their creators, and some element methods mutate cached state.
Native emote ranges and channel-specific emote definitions can also differ for
equal raw text. Keep immutable body data separate from per-occurrence styling,
links, mutable state, and layout data before sharing it. Emote/image resources
already use shared pointers, so repeated emotes do not imply a separate image
allocation for each message.

Content deduplication does not remove the fixed per-occurrence subtotal above,
and a hundred identical messages still consume a hundred message-count slots.
Its savings depend on how much repeated content is present and how much is
already shared. It is a complement to bounded layouts and cheaper snapshots.

Per-channel limits should be the first retention-setting change. The current
global setting is a default applied independently to channel and view queues,
not a single combined message allowance across the application. Keep that
default for ordinary channels, with explicit overrides for selected broadcasters.
Treat "per stream" as per broadcaster channel, persistent across broadcasts,
rather than resetting the preference for each live session.

Use a "Use default" choice and a custom retained-message count per channel.
Persist by broadcaster ID where available, with normalized channel login as a
fallback. Apply the resolved limit to the backing channel, filtered split proxy,
layout queue, and scrollbar markers. Raising only the backing channel's limit
would leave the view showing the old smaller history window. Multiple splits of
one broadcaster should resolve the same retention preference, while retaining
their separate layout costs. Search should see the selected channel's full
retained history; the existing separate usercard limit can remain a presentation
limit rather than expanding automatically.

The current queue's capacity is fixed and views are constructed before their
channel is assigned. Implement capacity changes and eviction notifications
consistently, or deliberately require reopening/restarting before an override
applies. Increasing capacity cannot restore already-evicted messages. A live
resize must preserve the newest messages and correctly update paused snapshots,
selection, scrolling, markers, reply cleanup, and replay indexing. A change to
the global default should leave explicit channel overrides intact.

Prioritize the CPU fixes below and per-channel overrides before a broad parsed
body deduplication refactor. Then measure repeated long-text and short-emote
workloads separately to decide how much content pooling is worth keeping.

The follow-up [content-pooling guide](message-dedup-analysis.md)
describes the implementation and local evaluation tools.

### Validation sequence

1. Remove disabled-similarity snapshots and bound enabled comparisons. Add
   coverage through `Channel::applySimilarityFilters`, since the existing
   similarity test calls the lower-level function directly.
2. Reuse snapshots by generation, coalesce layouts, and fix viewport cleanup.
   Check appends, replacement at unchanged size, clear, refill, pause/unpause,
   selection, animated emotes, smooth scrolling, and multiple splits.
3. Fix replay eviction indexing and cache scrollbar marker drawing. Exercise a
   full ring for several turnover cycles, seeks in both directions, invalid
   timestamps, and paused manual reading.
4. Implement viewport/chunk reads and bounded detailed layouts. Measure live
   frame-time percentiles, incoming-message processing, heap growth, and search
   latency with representative text, emotes, replies, and moderation traffic.
5. Test an opt-in 250,000-entry limit, then 500,000, with multiple splits and a
   long session. Raise the exposed limit only after those application-level
   checks pass. These are test targets, not established safe limits.

For one million or more, prefer a memory budget or paged archive over multiplying
every split's fully materialized cache. CPU improvements do not reduce the bytes
needed to retain all message and layout objects.

## Reproducing the probe

Source: `benchmarks/message-cache-probe.cpp`. This is intentionally separate from
the Google Benchmark target so it can run with the already installed toolchain
without enabling a dependency download or rebuilding the application.

On this checkout, run the following from the repository root in PowerShell after
the release CMake build has been configured. Adjust the Visual Studio and Qt
paths for another machine. The flags file supplies this checkout's existing
include paths and release definitions. The executable only uses Qt for the
optional object-size inspection; it does not create a window or load settings.

```powershell
New-Item -ItemType Directory -Force build\cache-probe | Out-Null
$probeFlags = Get-Content build\src\CMakeFiles\chatterino-lib.dir\flags.make |
    Where-Object { $_ -match '^CXX_(DEFINES|INCLUDES|FLAGS) = ' } |
    ForEach-Object { $_ -replace '^CXX_(DEFINES|INCLUDES|FLAGS) = ', '' }
$probeFlags += '/nologo /DCACHE_PROBE_OBJECT_SIZES /Fobuild\cache-probe\probe.obj /Febuild\cache-probe\probe.exe benchmarks\message-cache-probe.cpp /link C:\Qt\6.10.1\msvc2022_64\lib\Qt6Core.lib'
$probeFlags | Set-Content build\cache-probe\probe.rsp
& cmd.exe /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl.exe @build\cache-probe\probe.rsp'
& cmd.exe /d /s /c 'set "PATH=C:\Qt\6.10.1\msvc2022_64\bin;%PATH%" && build\cache-probe\probe.exe > build\cache-probe\results.csv 2> build\cache-probe\sizes.txt'
```

Without `CACHE_PROBE_OBJECT_SIZES`, the probe only needs C++20, the source include
directory, Boost headers, and thread support. No application or Qt link is needed.
