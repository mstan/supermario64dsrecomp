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

## Reproduction

```powershell
# Three-run FPS baseline
py -3 tools/measure_sm64ds_scenario.py --repetitions 3

# Visual-artifact A/B (requires a runner containing black_band_scan)
py -3 tools/measure_sm64ds_scenario.py --threaded 1 --scan-black-bands
py -3 tools/measure_sm64ds_scenario.py --threaded 0 --scan-black-bands

# Native instruction-pointer sample of the worst menu phase (Windows)
py -3 tools/profile_sm64ds_worst_phase.py --output perf-results/worst-phase-sample
```

Generated JSON, logs, screenshots, and copied executables live under the
ignored `perf-results/` directory.
