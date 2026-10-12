"""Deterministic, offline-only diagnostic fixture generator and strict replay oracle.

No real device, network, RTSP, subprocess or wall-clock dependencies.
"""
from __future__ import annotations

from collections import Counter, OrderedDict, defaultdict
from hashlib import sha256
import json
from pathlib import Path
import random
import re

VERSION = "diag-fixture/v1"
GENERATOR_VERSION = "1.0.0"
NANO = 1_000_000_000
U64_MAX = 2**64 - 1
MAX_FRAMES = 5000
MAX_EVENTS = 100_000
HOSTS = {
    "ubuntu-source": ("unigine-steady", 1_000_000_000, 55),
    "ubuntu-mediamtx": ("mediamtx-steady", 7_000_000_000, -33),
    "jetson": ("jetson-steady", 4_000_000_000, 86),
    "windows-reg": ("reg-steady", 12_000_000_000, -121),
}
SCENARIOS = {
    "healthy_60": "steady 60 FPS, exact CV and Raw",
    "healthy_120": "steady 120 FPS, exact CV and Raw",
    "jittered_60": "seeded bounded irregular pacing",
    "irregular_60": "60 FPS mean with 180 ms source gap and catch-up burst",
    "source_restart": "renew source epoch and restart frame IDs",
    "mediamtx_restart": "relay outage without source epoch reset",
    "decode_stall": "transport continues during decoder stall and reconnect",
    "cv_timing": "early, late, dropped, duplicate, reordered, missing CV",
    "cv_zero_vs_missing": "valid zero-target vs absent and receiver-lost CVM1",
    "inference_backlog": "Jetson inference slowdown and queue accumulation; Raw healthy",
    "delay_sweep": "50/100/150/200/300 ms and bounded overlay eviction",
    "services_independent": "NetImgui, recorder, diagnostic service fail independently",
    "clock_reset": "host offset/drift and Jetson session/counter reset",
    "ambiguous_flood": "coincidental warnings, ambiguous cause, bounded repeated logs",
}
DECIMAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
TYPE_NAME = re.compile(r"[a-z][a-z0-9_.]*\Z")


def compact(obj: object) -> bytes:
    return (json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def decimal(value: object) -> int:
    if not isinstance(value, str) or not DECIMAL.fullmatch(value):
        raise ValueError(f"expected canonical unsigned decimal string, got {value!r}")
    number = int(value)
    if number > U64_MAX:
        raise ValueError("unsigned 64-bit overflow")
    return number


def frame_key(epoch: int, frame: int) -> dict:
    return {"stream_epoch": str(epoch), "frame_id": str(frame)}


def clock_spec(role: str, session: str) -> tuple[str, int, int]:
    if role == "jetson" and session == "jetson-s2":
        return ("jetson-steady-reboot", 500_000_000, -90)
    return HOSTS[role]


def local_time(role: str, session: str, virtual_ns: int) -> tuple[str, str]:
    domain, offset, ppm = clock_spec(role, session)
    return domain, str(offset + virtual_ns + virtual_ns * ppm // 1_000_000)


class Simulator:
    def __init__(self, name: str, seed: int, frames: int, max_events: int):
        if name not in SCENARIOS:
            raise ValueError(f"unknown scenario {name!r}")
        if not 1 <= frames <= MAX_FRAMES or not 1 <= max_events <= MAX_EVENTS:
            raise ValueError("frames or max_events outside bounded allowed range")
        if not 0 <= seed <= U64_MAX:
            raise ValueError("seed out of uint64 range")
        self.name, self.seed, self.frames, self.max_events = name, seed, frames, max_events
        self.rng = random.Random(seed)
        self.timeline: list[tuple[int, int, dict]] = []
        self.decoded: list[dict] = []
        self.index = 0
        self.ground_truth: list[dict] = []
        self.source_keys: list[tuple[int, int, int]] = []

    def emit(self, t: int, role: str, session: str, kind: str, key: dict | None = None,
             **attributes: object) -> None:
        if t < 0 or len(self.timeline) >= self.max_events:
            raise ValueError("virtual time negative or max_events budget exceeded")
        domain, stamp = local_time(role, session, t)
        event = {"schema_version": VERSION, "data_origin": "synthetic",
                 "host_role": role, "session_id": session, "clock_domain": domain,
                 "t_monotonic_ns": stamp, "type": kind, "frame_key": key,
                 "attributes": attributes}
        self.timeline.append((t, self.index, event))
        self.index += 1

    def inject(self, code: str, start: int, end: int, **details: object) -> None:
        self.ground_truth.append({"fault_code": code, "start_virtual_ns": str(start),
                                  "end_virtual_ns": str(end), "parameters": details})

    def capture_ns(self, i: int, fps: int) -> int:
        if self.name == "irregular_60":
            if i == 0:
                return 0
            # Integrate a 180 ms pause and 11 one-ms catch-up intervals.
            return sum(180_000_000 if j == 20 else 1_000_000 if 21 <= j <= 31
                       else NANO // fps for j in range(1, i + 1))
        base = i * NANO // fps
        if self.name == "jittered_60" and i > 0:
            # Bound the jitter to prevent inversion of capture order.
            return base + self.rng.randint(-3_000_000, 3_000_000)
        return base

    def frames_stream(self) -> None:
        fps = 120 if self.name == "healthy_120" else 60
        total = self.frames * 5 if self.name == "delay_sweep" else self.frames
        periods = [50, 100, 150, 200, 300]
        if self.name == "irregular_60":
            self.inject("source_pacing_gap_and_burst", 19 * NANO // 60, 32 * NANO // 60, gap_ms=180)
        if self.name == "jittered_60":
            self.inject("bounded_source_jitter", 0, (self.frames - 1) * NANO // fps, max_abs_jitter_ms=3)
        if self.name == "source_restart":
            self.inject("unigine_restart", self.capture_ns(total // 2, fps), self.capture_ns(total // 2, fps))
        if self.name == "mediamtx_restart":
            self.inject("mediamtx_restart", self.capture_ns(total // 3, fps), self.capture_ns(total // 3 + 6, fps))
        if self.name == "decode_stall":
            self.inject("decoder_stall_reconnect", self.capture_ns(total // 3, fps), self.capture_ns(total // 3 + 8, fps))
        if self.name == "cv_timing":
            self.inject("cv_transport_mixed_faults", 0, self.capture_ns(total - 1, fps))
        if self.name == "cv_zero_vs_missing":
            self.inject("cv_zero_missing_and_rx_loss", 0, self.capture_ns(total - 1, fps))
        if self.name == "inference_backlog":
            self.inject("jetson_inference_backlog", self.capture_ns(total // 3, fps), self.capture_ns(total - 1, fps))
        if self.name == "delay_sweep":
            self.inject("overlay_delay_capacity_sweep", 0, 5 * NANO, delays_ms=periods)
        if self.name == "services_independent":
            self.inject("side_service_failures", self.capture_ns(total // 3, fps), self.capture_ns(2 * total // 3, fps))
        if self.name == "clock_reset":
            self.inject("jetson_reboot_counter_reset", self.capture_ns(total // 2, fps), self.capture_ns(total // 2, fps))
        if self.name == "ambiguous_flood":
            self.inject("unknown_or_coincidental_warnings", self.capture_ns(total // 3, fps), self.capture_ns(2 * total // 3, fps),
                        known_root_cause=False)
        cv_sent_by_session = defaultdict(int)
        for i in range(total):
            trial = i // self.frames if self.name == "delay_sweep" else 0
            j = i % self.frames if self.name == "delay_sweep" else i
            t = trial * NANO + self.capture_ns(j, fps)
            restart = (self.name == "source_restart" and i >= total // 2)
            epoch = (2**53 + 19) + trial if self.name == "delay_sweep" else (2**53 + 20 if restart else 2**53 + 19)
            fid = j + 1 if self.name == "delay_sweep" else (i - total // 2 + 1 if restart else i + 1)
            key = frame_key(epoch, fid)
            src_session = f"source-s{trial + 1}" if self.name == "delay_sweep" else "source-s2" if restart else "source-s1"
            media_restarted = self.name == "mediamtx_restart" and i >= total // 3 + 7
            media_session = "mediamtx-s2" if media_restarted else "mediamtx-s1"
            decoder_restarted = self.name == "decode_stall" and i >= total // 3 + 9
            reg_session = "reg-s2" if decoder_restarted else "reg-s1"
            jetson_restarted = self.name == "clock_reset" and i >= total // 2
            jet_session = "jetson-s2" if jetson_restarted else "jetson-s1"
            if (self.name == "source_restart" and i == total // 2) or (self.name == "delay_sweep" and j == 0 and i != 0):
                self.emit(t, "ubuntu-source", src_session, "source.restart", None, new_epoch=str(epoch))
            if self.name == "mediamtx_restart" and i == total // 3:
                self.emit(t, "ubuntu-mediamtx", media_session, "mediamtx.disconnected")
            if self.name == "mediamtx_restart" and i == total // 3 + 7:
                self.emit(t, "ubuntu-mediamtx", media_session, "mediamtx.restarted")
            if self.name == "decode_stall" and i == total // 3:
                self.emit(t, "windows-reg", reg_session, "decoder.stalled")
            if self.name == "decode_stall" and i == total // 3 + 9:
                self.emit(t, "windows-reg", reg_session, "decoder.reconnected")
            if self.name == "clock_reset" and i == total // 2:
                self.emit(t, "jetson", jet_session, "cv.process_restarted", None, sent_total="0")
            if self.name == "services_independent" and i == total // 3:
                self.emit(t, "windows-reg", reg_session, "ui.netimgui_disconnected")
                self.emit(t, "windows-reg", reg_session, "recorder.queue_saturated", None, queue_depth=128)
                self.emit(t, "windows-reg", reg_session, "service.diagnostics_stale")
            if self.name == "services_independent" and i == 2 * total // 3:
                self.emit(t, "windows-reg", reg_session, "ui.netimgui_reconnected")
                self.emit(t, "windows-reg", reg_session, "recorder.recovered")
                self.emit(t, "windows-reg", reg_session, "service.diagnostics_fresh")
            self.emit(t, "ubuntu-source", src_session, "source.frame", key, encoded_bytes=18000)
            self.source_keys.append((epoch, fid, t))
            relay_absent = (self.name == "mediamtx_restart" and total // 3 <= i < total // 3 + 7)
            if relay_absent:
                continue
            self.emit(t + 2_000_000, "ubuntu-mediamtx", media_session, "transport.relayed", key)
            self.emit(t + 4_000_000, "windows-reg", reg_session, "transport.received", key)
            decode_absent = self.name == "decode_stall" and total // 3 <= i < total // 3 + 9
            decode_t = t + (60_000_000 if self.name == "cv_timing" and i % 8 == 0 else 10_000_000)
            if not decode_absent:
                self.emit(decode_t, "windows-reg", reg_session, "video.decoded", key)
                self.emit(decode_t + 2_000_000, "windows-reg", reg_session, "video.raw_presented", key)
            queue_wait = min(170_000_000, max(0, i - total // 3) * 5_000_000) if self.name == "inference_backlog" else 0
            inference_ns = 32_000_000 if self.name == "inference_backlog" and i >= total // 3 else 8_000_000
            self.emit(t + 5_000_000, "jetson", jet_session, "cv.frame_input", key)
            begin = t + 6_000_000 + queue_wait
            self.emit(begin, "jetson", jet_session, "cv.inference_started", key, queue_wait_ns=str(queue_wait))
            self.emit(begin + inference_ns, "jetson", jet_session, "cv.inference_completed", key, duration_ns=str(inference_ns))
            n_targets = 0 if (self.name == "cv_zero_vs_missing" and i % 4 == 0) or (self.name == "cv_timing" and i % 8 == 7) else 1
            never_sent = self.name == "cv_timing" and i % 8 == 2 or self.name == "cv_zero_vs_missing" and i % 4 == 1
            recv_loss = self.name == "cv_zero_vs_missing" and i % 4 == 2
            if never_sent:
                rx = None
            else:
                send_t = begin + inference_ns + 1_000_000
                cv_sent_by_session[jet_session] += 1
                self.emit(send_t, "jetson", jet_session, "cv.sent", key, target_count=n_targets,
                          sent_total=str(cv_sent_by_session[jet_session]), sender_result="accepted_locally")
                rx = None if recv_loss else send_t + 2_000_000
                if self.name == "cv_timing" and i % 8 == 1 and not decode_absent:
                    rx = decode_t + 150_000_000 + 20_000_000
                if self.name == "cv_timing" and i % 8 == 4:
                    rx = send_t + 65_000_000
                if self.name == "cv_timing" and i % 8 == 6 and not decode_absent:
                    rx = decode_t + 150_000_000 + 15_000_000
                if self.name == "delay_sweep":
                    rx = t + 125_000_000
                if rx is not None:
                    self.emit(rx, "windows-reg", reg_session, "cv.received", key, target_count=n_targets,
                              packet_valid=True)
                    if self.name == "cv_timing" and i % 8 == 3:
                        self.emit(rx + 2_000_000, "windows-reg", reg_session, "cv.duplicate", key)
            if not decode_absent:
                delay = periods[trial] if self.name == "delay_sweep" else 150
                capacity = (8 if delay >= 200 else 16) if self.name == "delay_sweep" else 64
                self.decoded.append({"key": key, "decode_t": decode_t, "reg_session": reg_session,
                                     "delay_ns": delay * 1_000_000, "capacity": capacity, "receive_t": rx})
        if self.name == "ambiguous_flood":
            start = self.capture_ns(total // 3, fps)
            for n in range(400):
                self.emit(start + n * 100_000, "ubuntu-mediamtx", "mediamtx-s1", "transport.warning", None,
                          warning_code="synthetic_rtp_warning", repeats=1)
        self.finish_overlay()

    def finish_overlay(self) -> None:
        actions = []
        for i, item in enumerate(self.decoded):
            actions.append((item["decode_t"], 0, i, item))
            actions.append((item["decode_t"] + item["delay_ns"], 1, i, item))
        actions.sort(key=lambda x: (x[0], x[1], x[2]))
        buffer: OrderedDict[int, dict] = OrderedDict()
        terminal: dict[int, tuple[int, str]] = {}
        for time, action, idx, item in actions:
            if action == 0:
                buffer[idx] = item
                if len(buffer) > item["capacity"]:
                    evicted_idx, evicted = buffer.popitem(last=False)
                    self.emit(time, "windows-reg", evicted["reg_session"], "overlay.evicted", evicted["key"])
                    terminal[evicted_idx] = (time, "evicted")
            elif idx in buffer:
                del buffer[idx]
                received = item["receive_t"]
                kind = "overlay.presented_exact" if received is not None and received <= time else "overlay.dropped_missing_metadata"
                self.emit(time, "windows-reg", item["reg_session"], kind, item["key"],
                          configured_delay_ms=item["delay_ns"] // 1_000_000)
                terminal[idx] = (time, kind)
        for idx, item in enumerate(self.decoded):
            received = item["receive_t"]
            if received is not None and received > terminal[idx][0]:
                self.emit(received, "windows-reg", item["reg_session"], "cv.late_after_drop", item["key"])

    def export(self) -> tuple[dict, list[dict], dict]:
        self.frames_stream()
        stream = [obj for _, _, obj in sorted(self.timeline, key=lambda row: (row[0], row[1]))]
        counts = Counter(obj["type"] for obj in stream)
        intervals = [b[2] - a[2] for a, b in zip(self.source_keys, self.source_keys[1:]) if a[0] == b[0]]
        pacing = {"unit": "ns", "population": "consecutive_source_frames_same_epoch",
                  "reset_scope": "source_session_and_stream_epoch", "sample_count": str(len(intervals)),
                  "window": "entire_synthetic_run", "min_ns": str(min(intervals)) if intervals else None,
                  "max_ns": str(max(intervals)) if intervals else None}
        raw = b"".join(compact(row) for row in stream)
        clocks = []
        scopes = {(e["host_role"], e["session_id"]) for e in stream}
        for role, session in sorted(scopes):
            domain, offset, ppm = clock_spec(role, session)
            clocks.append({"host_role": role, "session_id": session, "clock_domain": domain,
                           "offset_ns": str(offset), "drift_ppm": ppm, "sync_state": "synthetic_known"})
        manifest = {"schema_version": VERSION, "data_origin": "synthetic",
                    "scenario_id": self.name, "generator_version": GENERATOR_VERSION, "seed": str(self.seed),
                    "base_fps": 120 if self.name == "healthy_120" else 60,
                    "requested_source_frames": self.frames,
                    "duration_virtual_ns": str(max((t for t, _, _ in self.timeline), default=0)),
                    "nominal_overlay_delay_ms": 150, "event_count": len(stream),
                    "events_sha256": sha256(raw).hexdigest(), "clock_specs": clocks,
                    "expected_assertions": {"event_counts": dict(sorted(counts.items())), "pacing": pacing},
                    "ground_truth": {"injected_faults": self.ground_truth, "test_only": True}}
        public = {k: v for k, v in manifest.items() if k not in ("scenario_id", "ground_truth", "expected_assertions")}
        public["fixture_id"] = "blind-" + sha256(compact([self.name, self.seed])).hexdigest()[:16]
        public["public_observations_only"] = True
        return manifest, stream, public


def generate(name: str, seed: int = 20261012, frames: int = 60,
             max_events: int = MAX_EVENTS) -> tuple[dict, list[dict], dict]:
    return Simulator(name, seed, frames, max_events).export()


def write_fixture(directory: Path, name: str, seed: int = 20261012, frames: int = 60,
                  max_events: int = MAX_EVENTS) -> dict:
    manifest, events, public = generate(name, seed, frames, max_events)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "manifest.json").write_bytes(json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False).encode() + b"\n")
    (directory / "public_manifest.json").write_bytes(json.dumps(public, indent=2, sort_keys=True, ensure_ascii=False).encode() + b"\n")
    (directory / "events.jsonl").write_bytes(b"".join(map(compact, events)))
    return manifest


def _unique_pairs(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key}")
        result[key] = value
    return result


def parse_json(raw: bytes) -> object:
    return json.loads(raw, object_pairs_hook=_unique_pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError("nonfinite JSON number")))


def validate_observation(obj: object) -> None:
    if not isinstance(obj, dict):
        raise ValueError("event must be JSON object")
    required = {"schema_version", "data_origin", "host_role", "session_id", "clock_domain",
                "t_monotonic_ns", "type", "frame_key", "attributes"}
    if set(obj) != required or obj["schema_version"] != VERSION or obj["data_origin"] != "synthetic":
        raise ValueError("invalid observation envelope")
    role, session, domain = obj["host_role"], obj["session_id"], obj["clock_domain"]
    if role not in HOSTS or not isinstance(session, str) or not 1 <= len(session) <= 64:
        raise ValueError("invalid host/session")
    if not isinstance(domain, str) or domain != clock_spec(role, session)[0]:
        raise ValueError("invalid clock domain")
    decimal(obj["t_monotonic_ns"])
    if not isinstance(obj["type"], str) or not TYPE_NAME.fullmatch(obj["type"]):
        raise ValueError("invalid event type")
    key = obj["frame_key"]
    if key is not None:
        if not isinstance(key, dict) or set(key) != {"stream_epoch", "frame_id"}:
            raise ValueError("invalid frame key")
        decimal(key["stream_epoch"])
        decimal(key["frame_id"])
    if not isinstance(obj["attributes"], dict) or len(obj["attributes"]) > 16:
        raise ValueError("invalid attributes")
    for name, value in obj["attributes"].items():
        if not isinstance(name, str) or not TYPE_NAME.fullmatch(name):
            raise ValueError("invalid attribute name")
        if type(value) not in (str, int, bool) or (isinstance(value, str) and len(value) > 256) or (type(value) is int and not -2**63 <= value <= 2**63-1):
            raise ValueError("invalid attribute value")
    if obj["type"] in {"video.decoded", "source.frame", "cv.received", "cv.sent",
                       "overlay.presented_exact", "overlay.evicted", "overlay.dropped_missing_metadata"} and key is None:
        raise ValueError("FrameKey required for frame event")


def verify(directory: Path, max_events: int = MAX_EVENTS) -> dict:
    manifest = parse_json((directory / "manifest.json").read_bytes())
    public = parse_json((directory / "public_manifest.json").read_bytes())
    if not isinstance(manifest, dict) or not isinstance(public, dict):
        raise ValueError("invalid manifest object")
    if manifest.get("schema_version") != VERSION or manifest.get("data_origin") != "synthetic":
        raise ValueError("invalid manifest schema or origin")
    if public != {**{k: v for k, v in manifest.items() if k not in ("scenario_id", "ground_truth", "expected_assertions")},
                  "fixture_id": "blind-" + sha256(compact([manifest["scenario_id"], decimal(manifest["seed"])] )).hexdigest()[:16],
                  "public_observations_only": True}:
        raise ValueError("public projection leaks test oracle or diverges")
    if manifest.get("generator_version") != GENERATOR_VERSION or manifest.get("scenario_id") not in SCENARIOS:
        raise ValueError("unsupported fixture scenario/version")
    decimal(manifest.get("seed"))
    if not isinstance(manifest.get("ground_truth"), dict) or manifest["ground_truth"].get("test_only") is not True:
        raise ValueError("ground truth missing/not test-only")
    declared_clocks = {}
    for spec in manifest.get("clock_specs", []):
        if not isinstance(spec, dict) or spec.get("host_role") not in HOSTS or not isinstance(spec.get("session_id"), str):
            raise ValueError("invalid clock specification")
        scope = (spec["host_role"], spec["session_id"], spec.get("clock_domain"))
        if scope in declared_clocks or tuple((spec.get("clock_domain"), decimal(spec.get("offset_ns")), spec.get("drift_ppm"))) != clock_spec(*scope[:2]):
            raise ValueError("invalid/duplicate clock specification")
        declared_clocks[scope] = spec
    digest, count = sha256(), 0
    last_clock: dict[tuple[str, str, str], int] = {}
    last_source: dict[int, int] = {}
    decoded: set[tuple[str, str]] = set()
    recv: dict[tuple[str, str], int] = {}
    terminal: dict[tuple[str, str], tuple[str, int]] = {}
    late: set[tuple[str, str]] = set()
    counts: Counter[str] = Counter()
    source: list[tuple[int, int, int]] = []
    with (directory / "events.jsonl").open("rb") as stream:
        for line in stream:
            count += 1
            if count > max_events or len(line) > 4096 or not line.endswith(b"\n"):
                raise ValueError("unbounded/malformed event stream")
            digest.update(line)
            obj = parse_json(line)
            validate_observation(obj)
            if compact(obj) != line:
                raise ValueError("noncanonical JSONL event encoding")
            role, sess, dom = obj["host_role"], obj["session_id"], obj["clock_domain"]
            now = decimal(obj["t_monotonic_ns"])
            scope = (role, sess, dom)
            if scope not in declared_clocks:
                raise ValueError("undeclared host clock/session")
            if scope in last_clock and now < last_clock[scope]:
                raise ValueError("local host/session monotonic clock went backwards")
            last_clock[scope] = now
            kind, key = obj["type"], obj["frame_key"]
            counts[kind] += 1
            pair = (key["stream_epoch"], key["frame_id"]) if key else None
            if kind == "source.frame":
                ep, fid = map(decimal, pair)
                if ep in last_source and fid <= last_source[ep]:
                    raise ValueError("source FrameKey not increasing within epoch")
                last_source[ep] = fid
                source.append((ep, fid, now))
            if kind == "video.decoded":
                if pair in decoded:
                    raise ValueError("duplicate video frame decode key")
                decoded.add(pair)
            if kind == "cv.received":
                recv[pair] = min(now, recv.get(pair, now))
            if kind in ("overlay.presented_exact", "overlay.dropped_missing_metadata", "overlay.evicted"):
                if pair not in decoded or pair in terminal:
                    raise ValueError("invalid/duplicate overlay terminal decision")
                terminal[pair] = (kind, now)
            if kind == "cv.late_after_drop":
                late.add(pair)
    if set(last_clock) != set(declared_clocks):
        raise ValueError("manifest clock scopes do not match events")
    if count != manifest.get("event_count") or digest.hexdigest() != manifest.get("events_sha256"):
        raise ValueError("fixture count or SHA-256 mismatch")
    if dict(sorted(counts.items())) != manifest.get("expected_assertions", {}).get("event_counts"):
        raise ValueError("expected event counts mismatch")
    if set(terminal) != decoded:
        raise ValueError("not every decoded frame has one terminal Overlay result")
    for pair, (kind, due) in terminal.items():
        observed = recv.get(pair)
        if kind == "overlay.presented_exact" and (observed is None or observed > due):
            raise ValueError("Overlay matched without timely exact FrameKey CV")
        if kind == "overlay.dropped_missing_metadata" and observed is not None and observed <= due:
            raise ValueError("Overlay dropped despite exact metadata")
        if observed is not None and observed > due and pair not in late:
            raise ValueError("late metadata not accounted")
    intervals = [b[2] - a[2] for a, b in zip(source, source[1:]) if a[0] == b[0]]
    pacing = manifest["expected_assertions"]["pacing"]
    if pacing != {"unit": "ns", "population": "consecutive_source_frames_same_epoch",
                  "reset_scope": "source_session_and_stream_epoch", "sample_count": str(len(intervals)),
                  "window": "entire_synthetic_run", "min_ns": str(min(intervals)) if intervals else None,
                  "max_ns": str(max(intervals)) if intervals else None}:
        # Host-local source clock includes offset/drift, so expected pacing must be
        # compared against the known synthetic clock mapping, never another host.
        times = []
        for ep, fid, src_clock in source:
            spec = next(x for x in manifest["clock_specs"] if x["host_role"] == "ubuntu-source" and
                        x["clock_domain"] == "unigine-steady")
            offset, ppm = decimal(spec["offset_ns"]), spec["drift_ppm"]
            # Integer clock inversion via binary search (small test-only oracle).
            lo, hi = 0, src_clock
            while lo < hi:
                mid = (lo + hi) // 2
                computed = offset + mid + mid * ppm // 1_000_000
                if computed < src_clock:
                    lo = mid + 1
                else:
                    hi = mid
            times.append((ep, fid, lo))
        nominal = [b[2] - a[2] for a, b in zip(times, times[1:]) if a[0] == b[0]]
        if pacing["sample_count"] != str(len(nominal)) or pacing["min_ns"] != (str(min(nominal)) if nominal else None) or pacing["max_ns"] != (str(max(nominal)) if nominal else None):
            raise ValueError("expected source pacing mismatch")
    return {"scenario_id": manifest["scenario_id"], "event_count": count,
            "decoded_frames": len(decoded), "matched": counts["overlay.presented_exact"],
            "missing": counts["overlay.dropped_missing_metadata"], "evicted": counts["overlay.evicted"]}
