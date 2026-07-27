# SM64DS performance baseline

Date: 2026-07-26  
Host: Intel Core i9-9900K, Windows 10 19045  
Framework: `1f8d2a651de96b147d23a09f3520da1147aa3b27`  
Target: `b0b9b46efd87c2f98e1451ac540b9def61a41e4c`

This is a measurement baseline, not an optimization conclusion.

## Scenario

`tools/measure_sm64ds_scenario.py` launches a fresh process and drives the
following path entirely through the interactive TCP server:

1. firmware cartridge launch;
2. title/attract load;
3. Adventure;
4. blank File A;
5. new-game load and Peach letter;
6. castle/water flyover;
7. Mario, Luigi, and Wario arrivals;
8. transition to Yoshi alone;
9. controllable Yoshi outside the castle.

Menu transitions advance by presented DS frames. Touches remain down for
exactly four presented frames. After File A, scene boundaries advance by
cumulative ARM9 instruction counts. This keeps guest work invariant when host
performance changes.

The canonical executable was a Release build using the threaded software 3D
renderer, with deep tracing and internal timers disabled:

`c2f3475d913fe9be5929ce251243e477c8be825d3fd8177aeff091c20af74640`

## Canonical result

Three fresh-process repetitions:

| Phase | Median FPS | Min | Max | Median ms/frame |
|---|---:|---:|---:|---:|
| Initial load / attract | 40.07 | 39.71 | 41.19 | 24.96 |
| Title to Adventure menu | 47.31 | 45.92 | 48.72 | 21.14 |
| Adventure to File Select | 20.35 | 17.91 | 20.42 | 49.13 |
| New-game load | 42.38 | 40.88 | 44.85 | 23.60 |
| Castle and water cutscene | 47.09 | 42.19 | 49.50 | 21.23 |
| First character arrival | 48.54 | 44.79 | 50.41 | 20.60 |
| Everyone present | 36.97 | 33.49 | 39.69 | 27.05 |
| Everyone to Yoshi | 41.54 | 38.93 | 43.72 | 24.07 |
| Controllable Yoshi | 49.87 | 45.16 | 51.03 | 20.05 |

Whole-scenario median: **41.00 FPS** (run range 40.28–43.66).

The Adventure/File Select castle-water transition is the clear worst phase.
One matched profiled run measured its 49.04 ms average frame as:

- 48.59 ms emulation;
- 0.44 ms presentation;
- 0.002 ms audio drain;
- 1.95 ms GPU2D rendering, included inside emulation.

The sampled scheduler split for that phase was 69.9% ARM9, 14.1% ARM7, and
11.3% devices. These are attribution samples, not proof of a particular hot
function.

## Black-band artifact

An opt-in runtime observer examines the top framebuffer immediately before
SDL upload. It counts partial bands of 8–191 nearly-all-black rows, ignores
intentional full-black fades, and retains the worst exact framebuffer.

Matched playable-Yoshi runs:

| Software 3D mode | Affected frames | Scanned frames | Worst band | FPS |
|---|---:|---:|---:|---:|
| Threaded | 494 | 1,470 | 131 rows, starting at row 13 | 50.29 |
| Single-threaded | 0 | 1,470 | none | 45.13 |

The threaded failure ended at row 143; the renderer synchronizes at VCount
144. The cause was a counting semaphore being treated as line-specific even
when 2D skipped unused earlier 3D scanlines. The fix tracks the exact number of
completed scanlines and waits for the requested row.

Post-fix threaded validation found **0 affected frames out of 1,471**, at
50.13 FPS in the Yoshi phase. A later full-sequence validation again found
**0 out of 1,470**. Threaded rendering remains enabled.

## First measured optimization

A process-level RIP sampler captured the exact Adventure-to-File-Select
transition. Of 18,488 active main-thread samples inside the runner, 7,104
(38.4%) landed in `lookup_static_cached`, primarily its binary-search miss
path. Temporary counters then measured 60.3 million lookups in the phase,
including 22.1 million repeated searches for one inactive static validation
candidate.

The runtime now caches that single inactive candidate until a write changes
its backing-page generation. Multiple candidates remain uncached so a
different variant cannot become live unnoticed. In the diagnostic A/B:

- cache hit rate increased from 59.5% to 95.7%;
- repeated inactive-candidate searches fell from 22.1 million to 0.2 million;
- the phase increased from 19.29 FPS to 25.54 FPS.

A counter-free full-sequence validation measured the same phase at 28.74 FPS
and completed the complete Yoshi path correctly. This is a confirmed hotspot
reduction; a new multi-run performance baseline is still required because
whole-machine variance was high during these runs.

## Second measured optimization

The post-dispatch profile still attributed 743 of 13,055 active main-thread
samples (5.7%) directly to diagnostic trace-ring recording. Normal play had
deep tracing disabled, but block-level runtime events and Tier-3 instruction
events were still recorded into rings that nothing would read.

Trace-ring recording is now fully gated by the existing deep-trace switch.
The TCP `deep_trace` command can still enable both rings live when diagnostic
history is needed.

A near-time, reversed-order A/B measured the exact worst phase twice with
each executable:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Prior tracing behavior | 23.58 FPS | 23.02 FPS | 23.30 FPS |
| Trace rings gated | 25.82 FPS | 25.00 FPS | 25.41 FPS |

That is a **9.1% mean improvement** under the same noisy host conditions.
All four runs produced identical end-frame hashes. A subsequent complete
boot-to-Yoshi validation rendered all nine checkpoints correctly and found
**0 black-band frames out of 1,470 scanned**.

## Third measured optimization

Exact counters around the worst phase found 22,038,496 of roughly 94.9
million ARM9 instructions (23.2%) executing through the Tier-3 interpreter.
The phase crossed into Tier 3 512,112 times, averaging only 43 interpreted
instructions per entry.

Coverage discovery identified 928 missing entry points whose hot bytes match
the existing attract/menu ARM9 RAM capture. Adding those entries to that
content-validated bank reduced the phase to:

- 0 Tier-3 entries;
- 0 Tier-3 instructions.

Every promoted function still carries full live-byte validation, so a later
overlay generation cannot execute stale native code. A reversed-order
two-run A/B was noisy (the baseline ranged from 24.00 to 30.78 FPS), but the
means were 27.39 FPS before and 29.33 FPS after, a 7.1% improvement.

The full promoted-bank validation completed at 42.96 FPS overall and 33.27
FPS in the former worst phase. All nine visual checkpoints passed, final
framebuffer hashes matched the prior validated run, and the detector found
**0 black-band frames out of 1,470 scanned**.

## Fourth measured optimization

After Tier 3 was eliminated, the native profile attributed about 41% of
active main-thread samples to dispatch lookup and validation bookkeeping.
Each cached main-RAM hit still resolved its validation page back through the
bus solely to read the same generation counter.

The dispatch cache now retains a pointer to that stable main-RAM page counter.
ITCM, WRAM, and other mapping-sensitive ranges continue through the original
resolver. An in-process reset remains safe because RAM is initialized before
`runtime_init` clears the dispatch cache.

A near-time, reversed-order A/B measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Resolve generation page | 28.38 FPS | 25.09 FPS | 26.73 FPS |
| Cached generation pointer | 32.93 FPS | 31.58 FPS | 32.25 FPS |

That is a **20.6% mean improvement** in the target phase. Full-sequence
validation completed at 42.02 FPS overall, produced the exact prior final
framebuffer hashes, and found **0 black-band frames out of 1,470 scanned**.

## Fifth measured optimization

With the menu phase fully native, the original 4,096-entry direct-mapped
dispatch cache had more active PCs competing for slots. Expanding it to
16,384 entries reduced normalized `lookup_static_cached` samples from 19.2%
to 15.6% of the active main thread.

The reversed-order A/B was a smaller end-to-end result: 34.02 FPS before
versus 34.62 FPS after on average, a **1.8% improvement**. The added host
memory is roughly 1.5 MiB across both CPU caches. Full-sequence validation
completed successfully and found **0 black-band frames out of 1,470
scanned**.

## Sixth measured optimization

Instruction-level samples showed that the remaining lookup cost was still
dominated by binary-search refills. Raising the cache from 16,384 to 65,536
entries reduced those conflicts further at a total two-CPU cache size of
about 7 MiB.

The reversed-order A/B averaged 35.31 FPS with 16K entries and 37.86 FPS with
64K entries, a **7.2% improvement**. Full-sequence validation completed at
53.69 FPS overall in that run and found **0 black-band frames out of 1,469
scanned**.

## Seventh measured optimization

After expanding the dispatch cache, the native profile attributed another
7.4% of active main-thread samples to executable-page generation lookup.
The active static execution guard re-resolved its main-RAM validation pages
whenever a nested static call returned, despite those generation arrays
remaining stable for the lifetime of the runtime instance.

The active guard now retains direct pointers to main-RAM generation counters,
just as the dispatch cache does. Mapping-sensitive ranges still use the full
resolver. A reversed-order A/B of the target phase measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Resolve active-guard pages | 37.15 FPS | 35.26 FPS | 36.21 FPS |
| Cached guard generation pointers | 37.74 FPS | 37.05 FPS | 37.40 FPS |

That is a **3.3% mean improvement**. The earlier attract and Adventure-menu
phases improved by similar amounts in the same four launches. A subsequent
full-sequence run completed at 52.47 FPS overall and 39.22 FPS in the target
phase; its final framebuffer hashes exactly matched the prior validation and
the detector found **0 black-band frames out of 1,469 scanned**. The lower
single-run overall result versus the prior 53.69 FPS validation is retained
here as observed host/run variance rather than treated as an additional gain.

## Eighth measured optimization

Instruction-level dispatch samples showed roughly 5% of the active main
thread copying the complete 45-byte static execution guard into and out of a
global object around nested native calls. The guard is now held in its owning
dispatch stack frame, while a global pointer links to the currently active
guard. Nested calls restore only that pointer and still revalidate the outer
guard before resuming it, preserving detection of writes through aliases.

A reversed-order A/B measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Copy guard state | 38.30 FPS | 38.83 FPS | 38.57 FPS |
| Stack-linked guard | 41.39 FPS | 42.12 FPS | 41.76 FPS |

That is an **8.3% mean improvement** in the target phase. Full-sequence
validation completed at 51.54 FPS overall and 39.91 FPS in the target phase.
Both final framebuffer hashes exactly matched the prior validation and the
detector found **0 black-band frames out of 1,471 scanned**. As above, the
single full-run overall FPS is retained as observed variance; the gain is
based on the near-time reversed-order A/B.

## Ninth measured optimization

The 56-byte dispatch-cache entry layout required repeated integer
multiplication to find a slot and allowed hot entries to straddle 64-byte host
cache lines. Aligning each entry to 64 bytes makes every entry occupy exactly
one cache line and lets the compiler replace the multiplications with shifts.
The two-CPU cache grows from about 7 MiB to 8 MiB.

A reversed-order A/B measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Packed 56-byte entries | 38.65 FPS | 36.18 FPS | 37.41 FPS |
| Aligned 64-byte entries | 39.33 FPS | 39.42 FPS | 39.38 FPS |

That is a **5.3% mean improvement** in the target phase. Full-sequence
validation completed at 52.82 FPS overall and 41.92 FPS in the target phase.
Both final framebuffer hashes exactly matched the prior validation and the
detector found **0 black-band frames out of 1,468 scanned**.

## Tenth measured optimization

ARM9 data-cacheability checks walked up to eight MPU regions for each memory
access. Each region test also decoded its size and recomputed 64-bit aligned
bounds, even though those values change only when the guest writes CP15.
Region writes now precompute the exact base and end once; hot cacheability
queries perform only comparisons and retain the same highest-region priority.

A reversed-order A/B measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Decode MPU bounds per access | 44.04 FPS | 45.66 FPS | 44.85 FPS |
| Precomputed MPU bounds | 46.27 FPS | 46.28 FPS | 46.28 FPS |

That is a **3.2% mean improvement** in the target phase. Full-sequence
validation completed at 57.32 FPS overall, 49.10 FPS in the target phase, and
59.66 FPS in castle/water. Both final framebuffer hashes exactly matched the
prior validation and the detector found **0 black-band frames out of 1,470
scanned**.

## Eleventh measured optimization

Dispatch lookup returned a two-pointer aggregate containing the selected
native function and validation descriptor. The Windows x64 ABI returned that
aggregate through a hidden stack buffer on every dispatch, even though both
pointers already live together in the immutable dispatch entry. Lookup and
the cache now return a single `DispatchEntry` pointer. A separate byte retains
the distinction between callable hits and cached inactive validation
candidates.

A reversed-order A/B measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Aggregate lookup result | 44.42 FPS | 46.41 FPS | 45.41 FPS |
| Dispatch-entry pointer | 46.61 FPS | 45.51 FPS | 46.06 FPS |

That is a **1.4% mean improvement** in the target phase, with normalized
emulator time improving by about 1.2%. Full-sequence validation completed at
57.14 FPS overall, 48.28 FPS in the target phase, and 59.79 FPS in
castle/water. Both final framebuffer hashes exactly matched the prior
validation and the detector found **0 black-band frames out of 1,470
scanned**. The lower single-run result versus the preceding validation is
retained as variance; the improvement claim is based on the reversed A/B.

## Twelfth measured optimization

Immutable dispatch-cache hits do not have writable-RAM validation pages.
Their zero `page_count` already records that fact inside the aligned cache
line, so the hot path now uses it directly instead of following the cached
dispatch-entry pointer to inspect its validation descriptor.

A near-time A/B measured:

| Build | Run 1 | Run 2 | Mean |
|---|---:|---:|---:|
| Entry validation pointer chase | 47.24 FPS | 47.07 FPS | 47.15 FPS |
| Cache-line `page_count` check | 47.37 FPS | 47.22 FPS | 47.30 FPS |

That is a small but repeatable **0.3% mean improvement** in the target phase.
Full-sequence validation completed at 57.05 FPS overall, 48.92 FPS in the
target phase, and 59.88 FPS in castle/water. Both final framebuffer hashes
exactly matched the prior validation and the detector found **0 black-band
frames out of 1,470 scanned**.

## Reproduction

```powershell
# Three-run FPS baseline
py -3 tools/measure_sm64ds_scenario.py --repetitions 3

# Visual-artifact A/B (requires a runner containing black_band_scan)
py -3 tools/measure_sm64ds_scenario.py --threaded 1 --scan-black-bands
py -3 tools/measure_sm64ds_scenario.py --threaded 0 --scan-black-bands

# Native instruction-pointer sample of the worst menu phase (Windows)
py -3 tools/profile_sm64ds_worst_phase.py --output perf-results/worst-phase-sample

# Tier-3 counts and copied-code entry coverage (timing is diagnostic only)
py -3 tools/profile_sm64ds_worst_phase.py `
  --output perf-results/worst-phase-coverage `
  --no-symbolize --discover-static-misses
```

Generated JSON, logs, screenshots, and copied executables live under the
ignored `perf-results/` directory.
