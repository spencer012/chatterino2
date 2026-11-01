# Channel history and cache diagnostics

Open a channel's dropdown menu and choose **Channel history**. Choose the normal
default, a preset, or **Custom length...**. A custom value of zero follows the
default. The preference applies immediately to every split showing that channel
and persists across restarts. Preferences use the broadcaster's normalized login.
Other channels keep following the default in Settings > General.

Increasing the limit collects more future messages. It does not retrieve messages
already evicted, and does not change the separate limit for history fetched at
connection. Reducing the limit discards the oldest live entries. A paused view
keeps its frozen reading snapshot until unpaused. Large limits still require more
memory. The available limit is up to one million messages per channel, rather
than forcing that allowance on every channel. The separate usercard limit stays
in effect.

Choose **Channel history > Cache diagnostics...** for a selectable report of:

- Live retained messages and the channel/view limits.
- Frozen snapshots built when pausing and the number of pointers copied.
- Live history reads, which borrow the GUI ring without copying all its entries.
- Snapshot-copy time, measured while performance logging is enabled.
- Replay-index rebuilds and incremental appended entries.
- Detailed offscreen layouts released.
- The backing channel's message-ID index size. This index is built lazily for
  lookups on histories of at least 10,000 messages.

The report opens in a nonmodal, scrollable window so a long report cannot block
chat input or force an oversized message box. **Copy report** copies the complete
snapshot. Reopening the menu action refreshes an already open report. Snapshot
generation time is shown at the bottom to distinguish data collection from
window presentation delays.

Enable **Log cache performance every 30 seconds** in that menu, or the matching
General setting, to save timestamped reports to
`Misc/message-cache-performance.log` in Chatterino's data folder. The full path
is shown at the bottom of Cache diagnostics. The file appends across restarts
and rotates at 16 MiB, retaining one previous file with a `.1` suffix. Reports
also go to Qt logging category `chatterino.cache`. Logging is off by default.
Earlier builds emitted only the console report and did not save it to a file.

Paint and layout call counts, total wall time, mean and maximum are collected
only while logging is enabled. Paint can include a layout, so their totals
overlap and should not be added together. Timings do not measure total CPU usage,
message parsing or operating-system composition. Totals are cumulative; separate
interval maxima and counts over 8/16/50/100/250 ms reset at each periodic sample.
Retained-message counts describe the current live cache. The report does not
include chat text or usernames of message authors.

Reports are batched for 100 ms and written on a worker so console output and disk
flushes do not block chat rendering. Global runtime counters appear once per
batch, including event-loop delays beyond a shared 50 ms timer interval, geometry
builds, image-load invalidations and the previous log-write duration. Timer delay
can include OS scheduling or suspend/resume. See
[rendering diagnostics](message-cache-rendering.md) for measurement limits.

Each report includes a session and view ID so multiple splits for the same
channel remain distinguishable. Unchanged views log a heartbeat every five
minutes; changed views still log at the normal 30-second interval. The report
also counts message rows actually painted. These IDs and counters were added
after the initial October logging build, so older files lack them.

Animation redraws keep separate dirty regions for animated rows rather than
repainting the rows between them. Static text and images are excluded from the
animation element pass. Loaded animations read a shared 64-bit clock on demand;
offscreen images no longer receive a separate callback on every animation tick.
Frame timing, focus-based animation pausing and layered emotes stay supported.

## General performance changes

Normal drawing and hit testing read a GUI-owned circular buffer directly. An
owned copy is needed only when pausing. Message ingestion and the backing channel
still use the thread-safe queue. Layout requests within an event-loop turn are
coalesced. Painting cleans up only the current and previous viewport ranges.

Offscreen drawing buffers are released. A bounded working set of up to 256
scrollbar, wheel and replay measurement layouts keeps its geometry across scroll
frames; other offscreen geometry is released. Hiding or clearing a view releases
that working set too. Measured heights remain available for scrolling, and
released geometry is rebuilt for rendering or copying a selection. Scrollbar
markers use a sparse index and cached transparent layer, so moving the thumb
does not rescan every cached message.

Replay timestamps are appended and evicted using stable sequence numbers rather
than rebuilding the whole index when a full ring advances. Replacements, history
refills and capacity changes invalidate the index. Out-of-order timestamps use a
linear fallback instead of binary-searching unsorted data.

Disabled similarity filtering no longer copies history. Enabled filtering copies
only the configured comparison tail. Timeout stacking reads the last 20 entries;
disabling older messages still visits the complete cache without copying it.
History refill uses a batch merge and retains the newest entries. Large-history
ID lookups use a lazily built index. Search input is debounced, and search-result
storage follows the source snapshot size rather than truncating it to the global
default.

The isolated timings in `benchmarks/message-cache-probe.cpp` compare copying a
full thread-safe queue with borrowing the GUI ring and copying a paused snapshot.
They are not end-to-end application frame timings.

The saved optimized probe results are in
[`benchmarks/results/message-cache-optimized-2026-10-22.csv`](../benchmarks/results/message-cache-optimized-2026-10-22.csv).
At one million synthetic entries, the full snapshot took 40.43 ms, while the
borrowed-ring read remained constant-time across the tested sizes. Freezing a
paused snapshot still took 44.62 ms. Those timings include snapshot destruction,
use synthetic payloads, and ran while the application was compiling. They show
which operation was removed from normal rendering, rather than a measured frame
rate improvement.

## Content deduplication

**Deduplicate repeated message content** is enabled by default and can be changed
in the channel history menu or Settings > General. It shares equal message text
and parsed text word lists. Emote-bearing messages participate, including their
surrounding text and text fallbacks. Each emote element, message ID, sender,
timestamp, moderation state, link and layout remains independent. Parsing still
runs for every occurrence.

The pool has an 8,192-entry lookup index and a 64 KiB admission filter. The first
sighting records a fingerprint, the second can create an entry, and subsequent
matches can share it. Full equality checks follow hashing. Unique content avoids
per-payload entries. Evicting an indexed entry leaves existing messages valid;
its bookkeeping is freed when its last participant dies. Turning the feature off
stops new sharing and clears the index. Already shared history remains valid.

Cache diagnostics adds all-channel content statistics:

- **Estimated CURRENT net saving** subtracts pool bookkeeping, the admission
  filter and the extra tracking field in every message and text element. Negative
  values are possible, especially with unique content or short histories.
- **Current gross payload saving** counts retained duplicate storage
  conservatively. Clones and copies already sharing Qt storage add no savings.
- Cumulative duplicate bytes released and pool lookup wall time describe work
  over the process lifetime. They are not current memory savings or CPU time.

Byte figures use capacities and a fixed allocation model. They are not heap or
process RAM measurements. Turning the feature off keeps the eight-byte tracking
fields in existing object types. Reverting the separate deduplication commit also
removes those fields. An on/off comparison should use fresh processes with the
same retained chat to avoid counting older shared history.

## Production-pool log replay

`benchmarks/message-content-probe.cpp` replays the last 30 daily log files through
the production pool. Use a fresh process for each enabled/disabled run and retain
the same number of messages. Its arguments are `CHANNEL_LOG_DIRECTORY CHANNEL
CAPACITY on|off`.

This probe treats every body as plain text. It does not resolve native emotes or
build real message envelopes, search text, layouts or GUI views. It checks pool
costs and lifetime on repetition patterns, but its word-list savings cannot
predict the application's emote-heavy workload. The live diagnostics use the
actual parsed messages. Reports and log-derived replay results are local-only.
