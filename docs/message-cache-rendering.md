# Animation rendering and cache diagnostics

Animations share one 64-bit clock. A loaded image resolves its current frame
only when requested, once per clock position, using cumulative frame durations
and binary search. Offscreen images do not register callbacks on every animation
tick. The shared clock remains valid if an image outlives the timer.

Each message layout indexes only elements with animated content. Static text
and images are skipped by the animation pass. Layered emotes retain the ordering
of static overlays above an animated layer. Loading image frames forces a layout
generation, which rebuilds the animation index. Decode completions are batched
over 20 ms so completions in adjacent event-loop turns share one invalidation.

Animation redraws preserve disjoint message-row regions, so static rows between
animated rows do not need to be painted again. Hidden and cleared views discard
their animation region. Showing a hidden tab requests layout again so released
message geometry is restored before drawing.

Scrollbar, wheel and replay measurements retain a bounded working set of up to
256 message geometries per view after painting. These measurements are reused
across smooth-scroll frames. Pixmaps are still released offscreen. Entries that
leave the working set release their geometry; hiding or clearing a view releases
the whole set. This avoids repeatedly shaping the bottom page while it is still
below the animated viewport without retaining layouts for the entire history.

## Reproducible synthetic benchmark

The opt-in `Image.DISABLED_AnimationClockMaintenanceBenchmark` compares production
lazy frames with a model of the previous per-image signal callback algorithm.
Run the test executable with both arguments:

```text
--gtest_filter=Image.DISABLED_AnimationClockMaintenanceBenchmark
--gtest_also_run_disabled_tests
```

The [saved synthetic results](../benchmarks/results/animation-clock-2026-11-01.csv)
are the median of three release-mode runs of 2,000 ticks, with 100, 1,000 and
10,000 loaded animations. No chat data is used. This measures clock maintenance
only, excluding decoding, drawing and other application work. It is not a
whole-application FPS or CPU result.

Functional tests cover frame boundaries, skipped reads, long animations, invalid
frame durations, timer lifetime, static overlays, animation-index rebuilding,
disjoint repaints, RTL ordering, pause snapshots and hidden-tab restoration.

## Local diagnostics

See [the cache controls guide](message-cache-controls.md) for logging and counters.
Reports use session/view IDs to distinguish splits for the same channel. Paint
and layout wall times can overlap, so their totals must not be added together.
Alongside cumulative values, each view reports interval maxima and counts over
8, 16, 50, 100 and 250 ms. Interval values reset after each periodic sample, so a
startup maximum cannot hide later stalls. Opening diagnostics does not reset
these intervals.

Reports arriving within 100 ms are batched. The content-pool summary is computed
once per batch. Console output, file append, flush and rotation run on a worker,
with only one write in flight and a bounded pending queue. Each batch includes
the previous write duration and any dropped-report count. Writes retain the
existing rotating file location.

One shared 50 ms timer measures event-loop delay beyond its expected interval
while logging is enabled. Runtime batches report its interval histogram, geometry
build count and image-load invalidation count. These counters help distinguish
expensive layouts from stalls elsewhere on the GUI thread. Timer delays also
include operating-system scheduling and suspend/resume; they do not identify the
blocking operation or measure Qt's backing-store flush independently.

Session reports, log-derived profiles and personal filesystem paths stay local
and are excluded from version control.
