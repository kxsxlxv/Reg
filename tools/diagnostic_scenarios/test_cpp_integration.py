"""Offline end-to-end oracle: B synthetic JSONL -> A statistics + C ExactSync.

Runs the real compiled C++ classes in an isolated subprocess with only
Reg-local clock values. No RTSP, Jetson, GPU, sockets or test-run ground-truth
are sent into the bridge. The oracle (this process) scores the bridge output.
"""
from __future__ import annotations

from collections import Counter
import math
from pathlib import Path
import subprocess
import sys

from .lab import generate

# Decoder stall has an explicit Windows process-session transition. Its
# generation-aware adapter is a separate future test, not silently conflated.
SCENARIOS = (
    "healthy_60", "healthy_120", "jittered_60", "irregular_60",
    "source_restart", "mediamtx_restart", "cv_timing",
    "cv_zero_vs_missing", "inference_backlog", "delay_sweep",
    "services_independent", "clock_reset", "ambiguous_flood",
)
BASE_EPOCH = 2**53 + 19
DELAYS = (50, 100, 150, 200, 300)
KINDS = {
    "video.decoded": "D",
    "cv.received": "M",
    "cv.duplicate": "M",
    "overlay.presented_exact": "P",
    "overlay.dropped_missing_metadata": "X",
    "overlay.evicted": "E",
}


def ensure(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def key(event: dict) -> tuple[str, str]:
    frame_key = event["frame_key"]
    return (frame_key["stream_epoch"], frame_key["frame_id"])


def make_bridge_input(name: str, events: list[dict]) -> tuple[str, dict]:
    reg = [e for e in events if e["host_role"] == "windows-reg" and e["type"] in KINDS]
    terminals = {
        key(e): e for e in reg
        if e["type"] in ("overlay.presented_exact", "overlay.dropped_missing_metadata", "overlay.evicted")
    }
    decoded = {key(e): e for e in reg if e["type"] == "video.decoded"}
    received = {}
    for e in reg:
        if e["type"] == "cv.received":
            received.setdefault(key(e), e)
    expected = Counter(e["type"] for e in reg)
    lines = []
    for e in reg:
        kind = KINDS[e["type"]]
        epoch, fid = key(e)
        timestamp = int(e["t_monotonic_ns"])
        deadline = 0
        count = 0
        if kind == "D":
            terminal = terminals[(epoch, fid)]
            if terminal["type"] != "overlay.evicted":
                deadline = int(terminal["t_monotonic_ns"])
            else:
                # Eviction precedes the original playout deadline. Infer the
                # configured delay only within the test-generated sweep.
                trial = int(epoch) - BASE_EPOCH
                delay_ms = DELAYS[trial] if name == "delay_sweep" else 150
                # Reg synthetic clock has -121 ppm drift. This estimate is
                # sufficient for eviction accounting, not hardware latency.
                deadline = timestamp + delay_ms * 1_000_000 * (1_000_000 - 121) // 1_000_000
        if kind == "M":
            count = int(e["attributes"].get("target_count", 0))
        lines.append(f"{kind} {epoch} {fid} {timestamp} {deadline} {count}")
    matched_waits = sorted(max(0, int(received[k]["t_monotonic_ns"]) -
                                int(decoded[k]["t_monotonic_ns"]))
                           for k, event in terminals.items()
                           if event["type"] == "overlay.presented_exact")
    oracle = {
        "decoded": expected["video.decoded"],
        "matched": expected["overlay.presented_exact"],
        "missing": expected["overlay.dropped_missing_metadata"],
        "evicted": expected["overlay.evicted"],
        "valid_empty": sum(e["attributes"]["target_count"] == 0
                           for e in reg if e["type"] == "cv.received"),
        "late_after_drop": expected["cv.late_after_drop"],  # see below
        "p95_exact": (matched_waits[math.ceil(.95 * len(matched_waits)) - 1]
                      if matched_waits else None),
    }
    # cv.late_after_drop carries independent evidence from a source event
    # outside the bridge's dispatch types: count from full JSONL, not 'reg'.
    oracle["late_after_drop"] = sum(e["type"] == "cv.late_after_drop" for e in events)
    return "\n".join(lines) + "\n", oracle


def run_case(binary: Path, scenario: str, frames: int = 24) -> None:
    manifest, events, _ = generate(scenario, seed=20261012, frames=frames)
    ensure(manifest["data_origin"] == "synthetic", "non-synthetic source forbidden")
    wire, oracle = make_bridge_input(scenario, events)
    process = subprocess.run([str(binary)], input=wire, text=True,
                             capture_output=True, timeout=20, check=False)
    ensure(process.returncode == 0,
           f"{scenario}: bridge exit={process.returncode} stderr={process.stderr[:1200]}")
    pairs = dict(part.split("=", 1) for part in process.stdout.strip().split())
    actual = {name: int(pairs[name]) for name in
              ("decoded", "matched", "missing", "evicted", "valid_empty",
               "late_after_drop", "decision_count", "distribution_success",
               "outcome_missing", "outcome_evicted", "frame_pacing_count", "pacing_rejected")}
    for field in ("decoded", "matched", "missing", "evicted", "valid_empty", "late_after_drop"):
        ensure(actual[field] == oracle[field],
               f"{scenario}: {field}: C++={actual[field]} oracle={oracle[field]}")
    ensure(actual["decision_count"] == oracle["matched"] + oracle["missing"] + oracle["evicted"],
           f"{scenario}: terminal decision denominator")
    ensure(actual["distribution_success"] == oracle["matched"] and
           actual["outcome_missing"] == oracle["missing"] and
           actual["outcome_evicted"] == oracle["evicted"], f"{scenario}: statistics outcomes mismatch")
    ensure(actual["frame_pacing_count"] + actual["pacing_rejected"] == oracle["decoded"],
           f"{scenario}: decode event accounting")
    ensure(pairs["complete"] == "1", f"{scenario}: unexpectedly truncated source")

    approximate = pairs["p95_wait_ns"]
    exact = oracle["p95_exact"]
    if exact is None:
        ensure(approximate == "unavailable", f"{scenario}: fabricated P95")
    else:
        numeric = int(approximate)
        ensure(numeric >= exact, f"{scenario}: upper-bound histogram underestimated P95")
        ensure(numeric <= exact * 1.0625 + 2,
               f"{scenario}: histogram relative error exceeded documented limit")
    if scenario == "healthy_60":
        ensure(actual["decoded"] == frames and actual["matched"] == frames,
               "healthy_60 must match all frames without missing CV")
        ensure(actual["pacing_rejected"] == 0, "healthy decoder generated duplicate local timestamps")
    print(f"PASS {scenario}: decoded={actual['decoded']} matched={actual['matched']}"
          f" missing={actual['missing']} evicted={actual['evicted']}"
          f" p95_wait={approximate}", flush=True)


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: python -m tools.diagnostic_scenarios.test_cpp_integration BRIDGE_BINARY",
              file=sys.stderr)
        return 2
    binary = Path(argv[0]).resolve()
    if not binary.is_file():
        print(f"bridge binary unavailable: {binary}", file=sys.stderr)
        return 2
    for scenario in SCENARIOS:
        run_case(binary, scenario)
    print(f"PASS {len(SCENARIOS)} Reg diagnostics C++/Python oracle scenarios")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (AssertionError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
