"""Sample native instruction pointers during the slow SM64DS menu transition."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import struct
import subprocess
import time
from collections import Counter, defaultdict
from typing import Any

import measure_sm64ds_scenario as scenario


TOOLS_DIR = pathlib.Path(__file__).resolve().parent
SAMPLER_SOURCE = TOOLS_DIR / "windows_rip_sampler.cpp"
SAMPLER_EXE = TOOLS_DIR / "windows_rip_sampler.exe"
MINGW_ROOT = pathlib.Path(r"C:\msys64\mingw64\bin")
COMPILER = MINGW_ROOT / "g++.exe"
ADDR2LINE = MINGW_ROOT / "addr2line.exe"
NM = MINGW_ROOT / "nm.exe"


def build_sampler() -> None:
    if SAMPLER_EXE.exists() and (
        SAMPLER_EXE.stat().st_mtime_ns >= SAMPLER_SOURCE.stat().st_mtime_ns
    ):
        return
    subprocess.run(
        [
            str(COMPILER),
            "-std=c++20",
            "-O2",
            "-s",
            str(SAMPLER_SOURCE),
            "-o",
            str(SAMPLER_EXE),
        ],
        check=True,
    )


def preferred_image_base(executable: pathlib.Path) -> int:
    with executable.open("rb") as source:
        source.seek(0x3C)
        pe_offset = struct.unpack("<I", source.read(4))[0]
        source.seek(pe_offset + 24)
        magic = struct.unpack("<H", source.read(2))[0]
        if magic == 0x20B:
            source.seek(pe_offset + 24 + 24)
            return struct.unpack("<Q", source.read(8))[0]
        if magic == 0x10B:
            source.seek(pe_offset + 24 + 28)
            return struct.unpack("<I", source.read(4))[0]
    raise RuntimeError("unsupported PE optional-header format")


def parse_samples(path: pathlib.Path) -> tuple[int, dict[int, Counter[int]]]:
    runtime_base = 0
    by_thread: dict[int, Counter[int]] = defaultdict(Counter)
    with path.open(newline="", encoding="utf-8") as source:
        lines = source.readlines()
    for line in lines:
        if line.startswith("# runtime_image_base,"):
            runtime_base = int(line.split(",", 1)[1], 16)
    rows = csv.DictReader(line for line in lines if not line.startswith("#"))
    for row in rows:
        by_thread[int(row["thread_id"])][int(row["rip"], 16)] += int(row["samples"])
    if not runtime_base:
        raise RuntimeError("sampler did not report the runtime image base")
    return runtime_base, by_thread


def nearest_symbols(
    executable: pathlib.Path, addresses: list[int]
) -> dict[int, str]:
    if not addresses:
        return {}
    pending = sorted(addresses)
    resolved: dict[int, str] = {}
    index = 0
    previous_name = "??"
    process = subprocess.Popen(
        [str(NM), "-n", "-C", str(executable)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert process.stdout is not None
    for line in process.stdout:
        parts = line.rstrip().split(maxsplit=2)
        if len(parts) != 3 or parts[1] not in {"T", "t", "W", "w"}:
            continue
        try:
            symbol_address = int(parts[0], 16)
        except ValueError:
            continue
        while index < len(pending) and pending[index] < symbol_address:
            resolved[pending[index]] = previous_name
            index += 1
        previous_name = parts[2]
    while index < len(pending):
        resolved[pending[index]] = previous_name
        index += 1
    stderr = process.stderr.read() if process.stderr is not None else ""
    if process.wait() != 0:
        raise RuntimeError(f"nm failed: {stderr}")
    return resolved


def symbolize(
    executable: pathlib.Path,
    runtime_base: int,
    preferred_base: int,
    by_thread: dict[int, Counter[int]],
) -> dict[str, Any]:
    image_size = executable.stat().st_size
    runner_addresses = sorted(
        {
            rip
            for counts in by_thread.values()
            for rip in counts
            if runtime_base <= rip < runtime_base + image_size
        }
    )
    adjusted = [preferred_base + (rip - runtime_base) for rip in runner_addresses]
    symbols: dict[int, tuple[str, str]] = {}
    if adjusted:
        function_names = nearest_symbols(executable, adjusted)
        result = subprocess.run(
            [
                str(ADDR2LINE),
                "-f",
                "-C",
                "-e",
                str(executable),
            ],
            check=True,
            capture_output=True,
            text=True,
            errors="replace",
            input="\n".join(f"0x{address:x}" for address in adjusted) + "\n",
        )
        lines = result.stdout.splitlines()
        for index, rip in enumerate(runner_addresses):
            symbols[rip] = (
                function_names[adjusted[index]],
                lines[index * 2 + 1],
            )

    result_threads = []
    aggregate: Counter[tuple[str, str]] = Counter()
    for thread_id, counts in sorted(by_thread.items()):
        runner_samples: Counter[tuple[str, str]] = Counter()
        total = sum(counts.values())
        for rip, count in counts.items():
            symbol = symbols.get(rip)
            if symbol:
                runner_samples[symbol] += count
                aggregate[symbol] += count
        result_threads.append(
            {
                "thread_id": thread_id,
                "total_samples": total,
                "runner_samples": sum(runner_samples.values()),
                "top_runner_symbols": [
                    {"function": function, "source": source, "samples": count}
                    for (function, source), count in runner_samples.most_common(30)
                ],
            }
        )
    return {
        "threads": result_threads,
        "aggregate_top_runner_symbols": [
            {"function": function, "source": source, "samples": count}
            for (function, source), count in aggregate.most_common(50)
        ],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        type=pathlib.Path,
        default=scenario.FRAMEWORK_ROOT / "runner/build-sm64-native/nds_runner.exe",
    )
    parser.add_argument(
        "--bios", type=pathlib.Path, default=scenario.FRAMEWORK_ROOT / "bios"
    )
    parser.add_argument(
        "--rom",
        type=pathlib.Path,
        default=scenario.TARGET_ROOT / "Super Mario 64 DS.nds",
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=19842)
    parser.add_argument("--interval-us", type=int, default=1000)
    parser.add_argument(
        "--phase",
        default="adventure_to_file_select",
        choices=[label for label, _ in scenario.FRAME_PHASES]
        + [label for label, _ in scenario.INSN_PHASES],
        help="scenario phase to sample (navigation replays every earlier phase)",
    )
    parser.add_argument(
        "--screen-layout",
        choices=("stacked", "separate"),
        default="stacked",
        help="host presentation layout to profile (default: stacked)",
    )
    parser.add_argument(
        "--adaptive-widescreen",
        choices=("none", "top"),
        default="none",
        help="adaptive surface to profile (default: none)",
    )
    parser.add_argument(
        "--no-symbolize",
        action="store_true",
        help="retain raw RIP samples but skip the slower symbol-resolution pass",
    )
    parser.add_argument(
        "--discover-static-misses",
        action="store_true",
        help="record Tier-3 entry coverage (diagnostic run; distorts timing)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    executable = args.exe.resolve()
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    build_sampler()

    extra_args = (
        "--screen-layout",
        args.screen_layout,
        "--adaptive-widescreen",
        args.adaptive_widescreen,
    )
    if args.discover_static_misses:
        extra_args += ("--discover-static-misses",)
    process, stdout_file, stderr_file = scenario.launch_runtime(
        executable,
        args.bios.resolve(),
        args.rom.resolve(),
        args.port,
        False,
        True,
        "soft",
        False,
        False,
        "manual",
        None,
        output_dir / "runner.stdout.log",
        output_dir / "runner.stderr.log",
        extra_args,
    )
    client = None
    sampler = None
    samples_path = output_dir / "rip-samples.csv"
    try:
        client = scenario.wait_for_client(process, args.port)
        scenario.wait_until_frame(client, scenario.BOOT_TO_FIRMWARE_FRAMES)
        scenario.touch(client, 128, 48)

        # Replay the canonical scenario up to the requested phase, then
        # sample only that phase's window. Touch order matches
        # measure_sm64ds_scenario.run_repetition exactly.
        frame_touches = ((128, 120), (128, 168), (43, 70))
        frame_labels = [label for label, _ in scenario.FRAME_PHASES]
        insn_labels = [label for label, _ in scenario.INSN_PHASES]

        def start_sampler() -> subprocess.Popen[str]:
            return subprocess.Popen(
                [
                    str(SAMPLER_EXE),
                    str(process.pid),
                    str(samples_path),
                    str(args.interval_us),
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

        phase = None
        before_tier3 = None
        for index, frame_phase in enumerate(scenario.FRAME_PHASES):
            if frame_phase[0] == args.phase:
                sampler = start_sampler()
                before_tier3 = client.cmd("static_coverage")
                phase = scenario.measure_frames(client, *frame_phase)
                break
            scenario.measure_frames(client, *frame_phase)
            scenario.touch(client, *frame_touches[index])
        else:
            # Target is an instruction-anchored phase past File A.
            file_a_insn9 = client.event_counts()["insn9"]
            previous_target = 0
            for label, cumulative_target in scenario.INSN_PHASES:
                if label == args.phase:
                    sampler = start_sampler()
                    before_tier3 = client.cmd("static_coverage")
                phase = scenario.measure_to_insn9(
                    client,
                    label,
                    file_a_insn9 + cumulative_target,
                    cumulative_target - previous_target,
                    None,
                )
                previous_target = cumulative_target
                if label == args.phase:
                    break
        if sampler is None or phase is None or before_tier3 is None:
            raise RuntimeError(f"phase {args.phase!r} was never reached")
        after_tier3 = client.cmd("static_coverage")
        tier3_coverage = (
            client.cmd("tier3_coverage", max=262144)["entries"]
            if args.discover_static_misses
            else []
        )
        tier3_hot_bytes = []
        if args.discover_static_misses:
            for entry in sorted(
                tier3_coverage, key=lambda item: item["hits"], reverse=True
            )[:32]:
                memory = client.cmd(
                    "read_mem", cpu=entry["cpu"], addr=entry["pc"], len=64
                )
                tier3_hot_bytes.append({**entry, "bytes": memory["hex"]})
        assert sampler.stdin is not None
        sampler.stdin.write("\n")
        sampler.stdin.flush()
        sampler_stdout, sampler_stderr = sampler.communicate(timeout=30.0)
        if sampler.returncode:
            raise RuntimeError(
                f"sampler failed ({sampler.returncode}): "
                f"{sampler_stdout}\n{sampler_stderr}"
            )

        runtime_base, by_thread = parse_samples(samples_path)
        preferred_base = preferred_image_base(executable)
        report = {
            "created_local": time.strftime("%Y%m%d-%H%M%S"),
            "executable": str(executable),
            "executable_sha256": scenario.sha256_file(executable),
            "phase": phase,
            "tier3_delta": scenario.subtract(after_tier3, before_tier3),
            "tier3_coverage": tier3_coverage,
            "tier3_hot_bytes": tier3_hot_bytes,
            "sampler": {
                "interval_us": args.interval_us,
                "runtime_image_base": f"0x{runtime_base:x}",
                "preferred_image_base": f"0x{preferred_base:x}",
            },
        }
        (output_dir / "report.partial.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if not args.no_symbolize:
            report.update(
                symbolize(
                    executable,
                    runtime_base,
                    preferred_base,
                    by_thread,
                )
            )
        report_path = output_dir / "report.json"
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(report_path)
        return 0
    finally:
        if sampler is not None and sampler.poll() is None:
            if sampler.stdin is not None:
                sampler.stdin.write("\n")
                sampler.stdin.flush()
            sampler.wait(timeout=10.0)
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


if __name__ == "__main__":
    raise SystemExit(main())
