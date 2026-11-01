# Animation rendering and cache diagnostics

Animations share one 64-bit clock. A loaded image resolves its current frame
only when requested, once per clock position, using cumulative frame durations
and binary search. Offscreen images do not register callbacks on every animation
tick. The shared clock remains valid if an image outlives the timer.

Each message layout indexes only elements with animated content. Static text
and images are skipped by the animation pass. Layered emotes retain the ordering
of static overlays above an animated layer. Loading image frames forces a layout
generation, which rebuilds the animation index.

Animation redraws preserve disjoint message-row regions, so static rows between
animated rows do not need to be painted again. Hidden and cleared views discard
their animation region. Showing a hidden tab requests layout again so released
message geometry is restored before drawing.

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
Session reports, log-derived profiles and personal filesystem paths stay local
and are excluded from version control.
