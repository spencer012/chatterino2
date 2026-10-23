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

Enable **Log cache performance every 30 seconds** in that menu, or the matching
General setting, to emit the report through Qt logging category
`chatterino.cache`. Logging is off by default. Counters are cumulative for the
view's lifetime; retained-message counts describe the current live cache. The
report does not include chat text or usernames of message authors.

## General performance changes

Normal drawing and hit testing read a GUI-owned circular buffer directly. An
owned copy is needed only when pausing. Message ingestion and the backing channel
still use the thread-safe queue. Layout requests within an event-loop turn are
coalesced. Painting cleans up only the current and previous viewport ranges.

Detailed geometry is released when a message leaves the viewport. Measured
heights remain available for scrolling, and geometry is rebuilt for rendering or
copying a selection. Scrollbar markers use a sparse index and cached transparent
layer, so moving the thumb does not rescan every cached message.

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
