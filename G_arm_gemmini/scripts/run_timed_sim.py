#!/usr/bin/env python3
"""Run a simulator while recording host wall times for IREE stage markers."""
from datetime import datetime, timezone
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


MARKERS = (
    ("arm_iree_start", "ARM IREE: initialize VM"),
    ("npu_queue_ready", "ARM NPU: queue ready"),
    ("vmfb_load_start", "ARM IREE: load model VMFB"),
    ("vmfb_loaded", "ARM IREE: model VMFB loaded"),
    ("context_init_start", "ARM IREE: initialize model context"),
    ("query_complete", "ARM NPU: QUERY phase complete"),
    ("module_main_start", "ARM IREE: invoke module.main"),
    ("output_read_start", "ARM IREE: read output"),
    ("mnist_pass", "MNIST PASS (Arm IREE / Rocket / Gemmini)"),
)

# The TestDriver reference clock is 1 GHz. Chipyard drives the external F-bus
# (and therefore the ARM host) at 500 MHz; the Rocket/Gemmini tile clock uses
# the 1 GHz reference clock in this configuration.
CLOCK_PERIODS_NS = {
    "testharness": 1.0,
    "arm_host": 2.0,
    "rocket": 1.0,
    "gemmini": 1.0,
}


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--timing", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a simulator command is required after --")

    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.timing.parent.mkdir(parents=True, exist_ok=True)
    start_utc = utc_now()
    start_mono = time.monotonic()
    marks = {}
    arm_profile = {}
    rocket_profile = {}
    gemmini_profile = {}
    with args.log.open("w", buffering=1) as log_file:
        try:
            proc = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except OSError as exc:
            raise SystemExit(f"failed to start simulator: {exc}")

        try:
            for line in proc.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                log_file.write(line)
                fields = dict(part.split("=", 1) for part in line.strip().split()
                              if "=" in part)
                if line.startswith("[ARM PERF]") and "marker" in fields:
                    arm_profile[fields["marker"]] = {
                        key: int(fields[key])
                        for key in ("soc_cycles", "active_cycles") if key in fields
                    }
                elif line.startswith("[ROCKET PERF]"):
                    if "query_count" in fields:
                        rocket_profile["query"] = {
                            key: int(fields[key])
                            for key in ("query_count", "query_exports", "slot_cycles", "handler_cycles")
                            if key in fields
                        }
                    elif "dispatch_count" in fields:
                        rocket_profile["dispatch"] = {
                            key: int(fields[key])
                            for key in ("dispatch_count", "workgroups", "slot_cycles", "handler_cycles")
                            if key in fields
                        }
                    elif "cycles_since_queue_ready" in fields:
                        rocket_profile["cycles_since_queue_ready"] = int(
                            fields["cycles_since_queue_ready"])
                elif line.startswith("[GEMMINI PERF]"):
                    for key, value in fields.items():
                        if key.endswith("_cycles") or key == "calls":
                            gemmini_profile[key] = int(value)
                elapsed = time.monotonic() - start_mono
                for key, marker in MARKERS:
                    if key not in marks and marker in line:
                        mark = {"elapsed_seconds": elapsed, "wall_utc": utc_now()}
                        marks[key] = mark
                        timing_line = (
                            f"[HOST TIMING] stage={key} "
                            f"elapsed_seconds={elapsed:.3f} wall_utc={mark['wall_utc']}\n"
                        )
                        sys.stdout.write(timing_line)
                        sys.stdout.flush()
                        log_file.write(timing_line)
            return_code = proc.wait()
        except KeyboardInterrupt:
            proc.terminate()
            return_code = proc.wait()
            raise

    end_mono = time.monotonic()
    end_utc = utc_now()

    arm_phase_cycles = {}
    phase_markers = (
        ("vmfb_load", "vmfb_load_start", "vmfb_loaded"),
        ("context_and_query", "context_init_start", "query_complete"),
        ("query_to_main", "query_complete", "module_main_start"),
        ("model_dispatch", "module_main_start", "output_read_start"),
        ("output_read_and_check", "output_read_start", "mnist_pass"),
        ("iree_start_to_pass", "arm_iree_start", "mnist_pass"),
    )
    for name, first, last in phase_markers:
        if first in arm_profile and last in arm_profile:
            arm_phase_cycles[name] = {
                "soc_cycles": arm_profile[last]["soc_cycles"] -
                              arm_profile[first]["soc_cycles"],
                "arm_active_cycles": arm_profile[last]["active_cycles"] -
                                     arm_profile[first]["active_cycles"],
            }

    def duration(first, last):
        if first not in marks or last not in marks:
            return None
        return round(marks[last]["elapsed_seconds"] - marks[first]["elapsed_seconds"], 3)

    result = {
        "simulator_start_utc": start_utc,
        "simulator_end_utc": end_utc,
        "simulator_wall_seconds": round(end_mono - start_mono, 3),
        "return_code": return_code,
        "command": command,
        "markers": marks,
        "core_profile": {
            "rtl_clock_period_ns": CLOCK_PERIODS_NS["testharness"],
            "clock_domains_period_ns": CLOCK_PERIODS_NS,
            "arm_markers": arm_profile,
            "arm_phase_cycles": arm_phase_cycles,
            "rocket": rocket_profile,
            "gemmini_call_latency": gemmini_profile,
            "notes": [
                "ARM soc/active counters use the 500 MHz host F-bus clock (2 ns per cycle).",
                "Rocket mcycle uses the 1 GHz Rocket tile clock and includes synchronous Gemmini call latency.",
                "Gemmini busy time uses the 1 GHz tile clock. The C import wrapper counter may miss direct-linked kernel calls; use the RTL busy waveform for hardware busy cycles.",
                "Core cycle times overlap and must not be added to simulator wall time.",
            ],
        },
        "durations_seconds": {
            "launch_to_arm_iree_start": marks.get("arm_iree_start", {}).get("elapsed_seconds"),
            "arm_iree_start_to_module_main_including_query": duration(
                "arm_iree_start", "module_main_start"),
            "launch_to_module_main_including_query": marks.get(
                "module_main_start", {}).get("elapsed_seconds"),
            "module_main_to_output_read": duration(
                "module_main_start", "output_read_start"),
            "output_read_to_mnist_pass": duration("output_read_start", "mnist_pass"),
        },
    }
    core = result["core_profile"]
    def elapsed(cycles, domain):
        seconds = int(cycles) * core["clock_domains_period_ns"][domain] * 1e-9
        return {"seconds": round(seconds, 9), "minutes": round(seconds / 60, 12)}

    core["derived_times"] = {
        "arm_active_cycles_total": elapsed(
            arm_profile.get("mnist_pass", {}).get("active_cycles", 0), "arm_host"),
        "arm_phases": {
            name: {
                "soc_elapsed": elapsed(values["soc_cycles"], "arm_host"),
                "arm_active": elapsed(values["arm_active_cycles"], "arm_host"),
            }
            for name, values in arm_phase_cycles.items()
        },
        "rocket": {
            "query_handler": elapsed(rocket_profile.get("query", {}).get("handler_cycles", 0), "rocket"),
            "query_slots": elapsed(rocket_profile.get("query", {}).get("slot_cycles", 0), "rocket"),
            "dispatch_handler": elapsed(rocket_profile.get("dispatch", {}).get("handler_cycles", 0), "rocket"),
            "dispatch_slots": elapsed(rocket_profile.get("dispatch", {}).get("slot_cycles", 0), "rocket"),
            "since_queue_ready": elapsed(rocket_profile.get("cycles_since_queue_ready", 0), "rocket"),
        },
        "gemmini_call_latency": elapsed(gemmini_profile.get("call_latency_cycles", 0), "gemmini"),
    }
    args.timing.write_text(json.dumps(result, indent=2) + "\n")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
