"""Measure the repeatable SM64DS new-game path through the live TCP surface.

The scenario deliberately advances by presented DS frames, not host sleeps:

    firmware -> cartridge -> title/attract -> Adventure -> File A
    -> new-game load -> castle/water -> character arrivals -> Yoshi

Each repetition launches a fresh process, so the runtime's in-memory cartridge
EEPROM begins blank and File A follows the new-game path.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import socket
import statistics
import subprocess
import sys
import time
from typing import Any


TARGET_ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = TARGET_ROOT.parent
FRAMEWORK_ROOT = WORKSPACE_ROOT / "ndsrecomp"
sys.path.insert(0, str(FRAMEWORK_ROOT / "oracle"))

from _client import DebugClient  # noqa: E402


# These intervals were mapped against the requested visible sequence. They are
# long enough to place each boundary inside a stable part of the next scene,
# while preserving every expensive transition as measured work.
BOOT_TO_FIRMWARE_FRAMES = 650
FRAME_PHASES = (
    ("initial_load_attract", 720),
    # The title star flies into the main menu before its buttons become live.
    ("title_to_adventure_menu", 600),
    # Adventure has its own zoom/fade before File A accepts a touch.
    ("adventure_to_file_select", 360),
)

# Cumulative ARM9 instructions after the File A touch. These landmarks came
# from the visually audited manual path. Unlike host seconds or display-frame
# guesses, they remain the same guest work when a future build gets faster.
INSN_PHASES = (
    ("new_game_load", 140_000_000),       # Peach letter -> clouds
    ("castle_and_water", 240_000_000),    # castle moat / water sweep
    ("first_character_arrival", 335_000_000),
    ("everyone_present", 440_000_000),
    ("everyone_to_yoshi", 720_000_000),   # departure/fade -> Yoshi alone
    ("yoshi_settled", 850_000_000),
)


def subtract(after: Any, before: Any) -> Any:
    if isinstance(after, dict):
        return {
            key: subtract(value, before[key])
            for key, value in after.items()
            if key in before
        }
    if isinstance(after, (int, float)):
        return after - before
    return after


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision(path: pathlib.Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def wait_for_client(process: subprocess.Popen[bytes], port: int) -> DebugClient:
    deadline = time.monotonic() + 60.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"runtime exited during startup ({process.returncode})")
        try:
            client = DebugClient(port=port, timeout=5.0)
            client.ping()
            return client
        except (ConnectionError, OSError, RuntimeError, socket.timeout) as error:
            last_error = error
            time.sleep(0.1)
    raise TimeoutError(f"runtime TCP server did not start: {last_error}")


def live_stats(client: DebugClient) -> dict[str, Any]:
    stats = client.cmd("frontend_stats")
    if not stats["active"]:
        raise RuntimeError("runtime does not have an active interactive frontend")
    return stats


def profile_snapshot(client: DebugClient) -> dict[str, Any]:
    # dispatch_stats is always-on; merging it here lets subtract() produce
    # per-phase dispatch composition deltas alongside the profile buckets.
    snapshot = client.cmd("profile")
    try:
        snapshot["dispatch"] = client.cmd("dispatch_stats")
    except RuntimeError as error:
        if str(error) != "dispatch_stats: unknown cmd":
            raise
    return snapshot


def wait_until_frame(
    client: DebugClient, target: int, timeout_seconds: float = 180.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    while True:
        stats = live_stats(client)
        remaining = target - stats["frames"]
        if remaining <= 0:
            return stats
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"frontend did not reach frame {target}; stopped at {stats['frames']}"
            )
        # Audio pacing normally caps the frontend near 60 FPS. Dividing by 90
        # approaches the boundary without polling the debug pump every frame.
        time.sleep(max(0.03, min(1.0, remaining / 90.0)))


def wait_until_insn9(
    client: DebugClient, target: int, timeout_seconds: float = 180.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    while True:
        counts = client.event_counts()
        remaining = target - counts["insn9"]
        if remaining <= 0:
            return counts
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"runtime did not reach ARM9 instruction {target}; "
                f"stopped at {counts['insn9']}"
            )
        # The current build sustains roughly 5-20 M ARM9 instructions/second
        # across these scenes. Approach at a conservative 40 M/s so the final
        # poll does not skip a large fraction of a phase on faster builds.
        time.sleep(max(0.03, min(1.0, remaining / 40_000_000.0)))


def touch(client: DebugClient, x: int, y: int, hold_frames: int = 4) -> None:
    before = live_stats(client)["frames"]
    client.cmd("touch", x=x, y=y, down=True)
    wait_until_frame(client, before + hold_frames)
    client.cmd("touch", x=x, y=y, down=False)


def framebuffer_digest(client: DebugClient) -> dict[str, str]:
    result: dict[str, str] = {}
    for engine in ("A", "B"):
        width, height, rgb = client.framebuffer(engine)
        result[engine] = hashlib.sha256(rgb).hexdigest()
        result[f"{engine}_size"] = f"{width}x{height}"
    return result


def save_screenshot(
    client: DebugClient,
    path: pathlib.Path,
    adaptive_top: bool = False,
) -> None:
    # Pillow is optional unless visual checkpoint evidence is requested.
    from PIL import Image

    width_a, height_a, rgb_a = client.framebuffer(
        "A", adaptive=adaptive_top
    )
    width_b, height_b, rgb_b = client.framebuffer("B")
    canvas_width = max(width_a, width_b)
    image = Image.new(
        "RGB", (canvas_width, height_a + height_b), (20, 20, 20)
    )
    image.paste(
        Image.frombytes("RGB", (width_a, height_a), rgb_a),
        ((canvas_width - width_a) // 2, 0),
    )
    image.paste(
        Image.frombytes("RGB", (width_b, height_b), rgb_b),
        ((canvas_width - width_b) // 2, height_a),
    )
    image.save(path)


def summarize_window(
    label: str,
    requested_frames: int,
    before_front: dict[str, Any],
    after_front: dict[str, Any],
    before_profile: dict[str, Any],
    after_profile: dict[str, Any],
) -> dict[str, Any]:
    front = subtract(after_front, before_front)
    profile = subtract(after_profile, before_profile)
    frames = front["frames"]
    frequency = after_front["freq"]
    seconds = front["now_ticks"] / frequency
    phase_ticks = (
        front["emu_ticks"] + front["present_ticks"] + front["drain_ticks"]
    )
    sched = profile["sched"]
    sampled_ns = sched["sampled_round_ns"]
    result = {
        "label": label,
        "requested_frames": requested_frames,
        "frames": frames,
        "seconds": seconds,
        "fps": frames / seconds,
        "average_frame_ms": seconds * 1000.0 / max(frames, 1),
        "underruns": front["underruns"],
        "phase_ms_per_frame": {
            "emu": front["emu_ticks"] * 1000.0 / frequency / max(frames, 1),
            "present": front["present_ticks"]
            * 1000.0
            / frequency
            / max(frames, 1),
            "audio_drain": front["drain_ticks"]
            * 1000.0
            / frequency
            / max(frames, 1),
            "unaccounted": (front["now_ticks"] - phase_ticks)
            * 1000.0
            / frequency
            / max(frames, 1),
        },
        "gpu2d_ms_per_frame": {
            key.removesuffix("_ns"): value / max(frames, 1) / 1.0e6
            for key, value in profile["gpu2d"].items()
            if key.endswith("_ns")
        },
        "gpu3d_ms_per_frame": {
            key.removesuffix("_ns"): value / max(frames, 1) / 1.0e6
            for key, value in profile["gpu3d"].items()
            if key.endswith("_ns")
        },
        "scheduler_sample_share": {
            key.removesuffix("_ns"): value / sampled_ns
            for key, value in sched.items()
            if key.endswith("_ns")
            and key != "sampled_round_ns"
            and sampled_ns
        },
        "raw_front_delta": front,
        "raw_profile_delta": profile,
    }
    return result


def measure_frames(
    client: DebugClient,
    label: str,
    frame_count: int,
    screenshot_path: pathlib.Path | None = None,
    adaptive_screenshot: bool = False,
) -> dict[str, Any]:
    before_front = live_stats(client)
    before_profile = profile_snapshot(client)
    target = before_front["frames"] + frame_count
    wait_until_frame(client, target)
    after_front = live_stats(client)
    after_profile = profile_snapshot(client)
    result = summarize_window(
        label,
        frame_count,
        before_front,
        after_front,
        before_profile,
        after_profile,
    )
    result["end_event_counts"] = client.event_counts()
    result["end_framebuffer_sha256"] = framebuffer_digest(client)
    if screenshot_path is not None:
        save_screenshot(client, screenshot_path, adaptive_screenshot)
        result["end_screenshot"] = str(screenshot_path)
        if adaptive_screenshot:
            result["end_gx_polygons"] = client.cmd("gx_polygons")
    print(
        f"{label}: {result['frames']} frames in {result['seconds']:.3f}s, "
        f"{result['fps']:.3f} FPS",
        flush=True,
    )
    return result


def measure_to_insn9(
    client: DebugClient,
    label: str,
    target_insn9: int,
    requested_insn9: int,
    screenshot_path: pathlib.Path | None = None,
    adaptive_screenshot: bool = False,
) -> dict[str, Any]:
    before_counts = client.event_counts()
    before_front = live_stats(client)
    before_profile = profile_snapshot(client)
    end_counts = wait_until_insn9(client, target_insn9)
    after_front = live_stats(client)
    after_profile = profile_snapshot(client)
    result = summarize_window(
        label,
        0,
        before_front,
        after_front,
        before_profile,
        after_profile,
    )
    result.pop("requested_frames")
    result["requested_insn9"] = requested_insn9
    result["insn9"] = end_counts["insn9"] - before_counts["insn9"]
    result["end_event_counts"] = end_counts
    result["end_framebuffer_sha256"] = framebuffer_digest(client)
    if screenshot_path is not None:
        save_screenshot(client, screenshot_path, adaptive_screenshot)
        result["end_screenshot"] = str(screenshot_path)
        if adaptive_screenshot:
            result["end_gx_polygons"] = client.cmd("gx_polygons")
    print(
        f"{label}: {result['insn9']} ARM9 instructions and "
        f"{result['frames']} frames in {result['seconds']:.3f}s, "
        f"{result['fps']:.3f} FPS",
        flush=True,
    )
    return result


def launch_runtime(
    executable: pathlib.Path,
    bios: pathlib.Path,
    rom: pathlib.Path,
    port: int,
    profiled: bool,
    threaded: bool,
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    extra_args: tuple[str, ...] = (),
) -> tuple[subprocess.Popen[bytes], Any, Any]:
    environment = os.environ.copy()
    environment["NDS_3D_THREADED"] = "1" if threaded else "0"
    # This traversal intentionally measures the firmware menu. Keep it stable
    # when the title's normal game.toml opts into retail Automatic boot.
    environment["NDS_STARTUP_MODE"] = "manual"
    environment.pop("NDS_3D_RENDERER", None)  # default software renderer
    environment.pop("NDS_DEEP_TRACE", None)  # interactive defaults it off
    if profiled:
        environment["NDS_PROFILE_GPU"] = "1"
        environment["NDS_PROFILE_SCHED"] = "1"
    else:
        environment.pop("NDS_PROFILE_GPU", None)
        environment.pop("NDS_PROFILE_SCHED", None)
    stdout_file = stdout_path.open("wb")
    stderr_file = stderr_path.open("wb")
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    process = subprocess.Popen(
        [
            str(executable),
            str(bios),
            "--interactive",
            "--rom",
            str(rom),
            "--no-save",
            "--port",
            str(port),
            *extra_args,
        ],
        cwd=FRAMEWORK_ROOT,
        env=environment,
        stdout=stdout_file,
        stderr=stderr_file,
        creationflags=creationflags,
    )
    return process, stdout_file, stderr_file


def aggregate_runs(runs: list[dict[str, Any]]) -> dict[str, Any]:
    if not runs:
        return {}
    labels = [phase["label"] for phase in runs[0]["phases"]]
    phases: list[dict[str, Any]] = []
    for index, label in enumerate(labels):
        fps_values = [run["phases"][index]["fps"] for run in runs]
        phases.append(
            {
                "label": label,
                "fps_values": fps_values,
                "median_fps": statistics.median(fps_values),
                "min_fps": min(fps_values),
                "max_fps": max(fps_values),
                "median_frame_ms": 1000.0 / statistics.median(fps_values),
            }
        )
    overall_fps = []
    for run in runs:
        frames = sum(phase["frames"] for phase in run["phases"])
        seconds = sum(phase["seconds"] for phase in run["phases"])
        overall_fps.append(frames / seconds)
    return {
        "run_count": len(runs),
        "overall_fps_values": overall_fps,
        "overall_median_fps": statistics.median(overall_fps),
        "overall_min_fps": min(overall_fps),
        "overall_max_fps": max(overall_fps),
        "phases": phases,
    }


def run_repetition(
    executable: pathlib.Path,
    bios: pathlib.Path,
    rom: pathlib.Path,
    output_dir: pathlib.Path,
    port: int,
    profiled: bool,
    repetition: int,
    screenshots: bool,
    threaded: bool,
    scan_black_bands: bool,
    adaptive_screenshots: bool,
    stop_after_phase: str | None,
) -> dict[str, Any]:
    mode = "profiled" if profiled else "plain"
    stem = f"{mode}-{repetition:02d}"
    process, stdout_file, stderr_file = launch_runtime(
        executable,
        bios,
        rom,
        port,
        profiled,
        threaded,
        output_dir / f"{stem}.stdout.log",
        output_dir / f"{stem}.stderr.log",
        ("--adaptive-widescreen", "top")
        if adaptive_screenshots
        else (),
    )
    client: DebugClient | None = None
    try:
        client = wait_for_client(process, port)
        wait_until_frame(client, BOOT_TO_FIRMWARE_FRAMES)
        touch(client, 128, 48)  # firmware DS cartridge banner

        phases: list[dict[str, Any]] = []
        screenshot_dir = output_dir / f"{stem}-screenshots"
        if screenshots:
            screenshot_dir.mkdir()

        def measure(phase: tuple[str, int]) -> dict[str, Any]:
            label, frames = phase
            screenshot_path = (
                screenshot_dir / f"{len(phases) + 1:02d}-{label}.png"
                if screenshots
                else None
            )
            return measure_frames(
                client,
                label,
                frames,
                screenshot_path,
                adaptive_screenshots,
            )

        phases.append(measure(FRAME_PHASES[0]))
        touch(client, 128, 120)  # TOUCH TO START

        phases.append(measure(FRAME_PHASES[1]))
        touch(client, 128, 168)  # ADVENTURE

        phases.append(measure(FRAME_PHASES[2]))
        touch(client, 43, 70)  # FILE A / File 1

        file_a_insn9 = client.event_counts()["insn9"]
        previous_target = 0
        black_band_capture: dict[str, Any] | None = None
        for label, cumulative_target in INSN_PHASES:
            if scan_black_bands and label == "yoshi_settled":
                client.cmd("black_band_scan", on=1, reset=1)
            screenshot_path = (
                screenshot_dir / f"{len(phases) + 1:02d}-{label}.png"
                if screenshots
                else None
            )
            phases.append(
                measure_to_insn9(
                    client,
                    label,
                    file_a_insn9 + cumulative_target,
                    cumulative_target - previous_target,
                    screenshot_path,
                    adaptive_screenshots,
                )
            )
            previous_target = cumulative_target
            if scan_black_bands and label == "yoshi_settled":
                client.cmd("black_band_scan", on=0)
                black_band_capture = client.cmd("black_band_capture")
                rgb_hex = black_band_capture.pop("rgb", "")
                if black_band_capture.get("has_capture") and rgb_hex:
                    from PIL import Image

                    capture_path = output_dir / f"{stem}-worst-black-band.png"
                    Image.frombytes(
                        "RGB",
                        (
                            black_band_capture.pop("w"),
                            black_band_capture.pop("h"),
                        ),
                        bytes.fromhex(rgb_hex),
                    ).save(capture_path)
                    black_band_capture["screenshot"] = str(capture_path)
            if label == stop_after_phase:
                break

        result = {
            "mode": mode,
            "repetition": repetition,
            "phases": phases,
            "final_event_counts": client.event_counts(),
            "final_framebuffer_sha256": framebuffer_digest(client),
        }
        if black_band_capture is not None:
            result["black_band_capture"] = black_band_capture
        return result
    finally:
        if client is not None:
            client.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10.0)
        stdout_file.close()
        stderr_file.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        type=pathlib.Path,
        default=FRAMEWORK_ROOT / "runner/build-sm64-native/nds_runner.exe",
    )
    parser.add_argument("--bios", type=pathlib.Path, default=FRAMEWORK_ROOT / "bios")
    parser.add_argument(
        "--rom",
        type=pathlib.Path,
        default=TARGET_ROOT / "Super Mario 64 DS.nds",
    )
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--port", type=int, default=19842)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument(
        "--profile",
        action="store_true",
        help="enable the built-in GPU and sampled scheduler timers",
    )
    parser.add_argument(
        "--screenshots",
        action="store_true",
        help="save ignored visual checkpoint evidence at each phase boundary",
    )
    parser.add_argument(
        "--threaded",
        type=int,
        choices=(0, 1),
        default=1,
        help="select the software 3D renderer's host worker thread (default: 1)",
    )
    parser.add_argument(
        "--scan-black-bands",
        action="store_true",
        help="scan every settled-Yoshi frame and retain the worst partial black band",
    )
    parser.add_argument(
        "--adaptive-screenshots",
        action="store_true",
        help="request the runner's top adaptive surface for screenshots",
    )
    parser.add_argument(
        "--stop-after-phase",
        choices=[label for label, _ in INSN_PHASES],
        help="end a diagnostic run after the selected instruction phase",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    executable = args.exe.resolve()
    bios = args.bios.resolve()
    rom = args.rom.resolve()
    for required in (executable, bios, rom):
        if not required.exists():
            raise FileNotFoundError(required)
    if args.repetitions < 1:
        raise ValueError("--repetitions must be positive")

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    mode = "profiled" if args.profile else "plain"
    output_dir = (
        args.output.resolve()
        if args.output
        else TARGET_ROOT / "perf-results" / f"{timestamp}-{mode}"
    )
    output_dir.mkdir(parents=True, exist_ok=False)

    report: dict[str, Any] = {
        "schema": 1,
        "created_local": timestamp,
        "scenario": {
            "boot_to_firmware_frames": BOOT_TO_FIRMWARE_FRAMES,
            "frame_phases": [
                {"label": label, "frames": frames}
                for label, frames in FRAME_PHASES
            ],
            "insn9_phases": [
                {"label": label, "cumulative_insn9": insn9}
                for label, insn9 in INSN_PHASES
            ],
            "inputs": [
                {"at": "firmware", "touch": [128, 48]},
                {"at": "title", "touch": [128, 120]},
                {"at": "main_menu", "touch": [128, 168]},
                {"at": "file_select", "touch": [43, 70]},
            ],
        },
        "build": {
            "executable": str(executable),
            "executable_sha256": sha256_file(executable),
            "framework_revision": git_revision(FRAMEWORK_ROOT),
            "target_revision": git_revision(TARGET_ROOT),
            "profiled": args.profile,
            "renderer": "soft",
            "gpu3d_threaded": bool(args.threaded),
            "deep_trace": False,
        },
        "host": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "python": sys.version,
        },
        "runs": [],
    }

    try:
        for repetition in range(1, args.repetitions + 1):
            run = run_repetition(
                executable,
                bios,
                rom,
                output_dir,
                args.port,
                args.profile,
                repetition,
                args.screenshots,
                bool(args.threaded),
                args.scan_black_bands,
                args.adaptive_screenshots,
                args.stop_after_phase,
            )
            report["runs"].append(run)
            report["summary"] = aggregate_runs(report["runs"])
            (output_dir / "report.partial.json").write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    finally:
        report_path = output_dir / "report.json"
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    partial = output_dir / "report.partial.json"
    if partial.exists():
        partial.unlink()
    print(report_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
