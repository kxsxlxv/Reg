"""Run from repository root: python -m unittest discover -s tools/diagnostic_scenarios -p 'test_*.py' -v"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.diagnostic_scenarios.lab import (
    SCENARIOS, VERSION, U64_MAX, compact, generate, parse_json,
    validate_observation, verify, write_fixture,
)


class ScenarioTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def fixture(self, name="healthy_60", frames=40, seed=20261012):
        directory = self.root / name
        write_fixture(directory, name, seed, frames)
        return directory

    def update_stream(self, directory, edit):
        path = directory / "events.jsonl"
        events = [parse_json(x) for x in path.read_bytes().splitlines()]
        edit(events)
        output = b"".join(map(compact, events))
        path.write_bytes(output)
        manifest_path = directory / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["events_sha256"] = hashlib.sha256(output).hexdigest()
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        public_path = directory / "public_manifest.json"
        public = json.loads(public_path.read_text())
        public["events_sha256"] = manifest["events_sha256"]
        public_path.write_text(json.dumps(public, indent=2, sort_keys=True) + "\n")

    def test_every_scenario_runs_without_network_or_hardware(self):
        for name in SCENARIOS:
            with self.subTest(name=name):
                directory = self.fixture(name)
                result = verify(directory)
                self.assertGreater(result["event_count"], 0)
                self.assertEqual(result["decoded_frames"], result["matched"] + result["missing"] + result["evicted"])

    def test_byte_identity_with_same_seed(self):
        for name in ("jittered_60", "cv_timing", "delay_sweep", "clock_reset"):
            with self.subTest(name=name):
                a, b = self.root / (name + "1"), self.root / (name + "2")
                write_fixture(a, name, 123, 20)
                write_fixture(b, name, 123, 20)
                for file in ("manifest.json", "events.jsonl", "public_manifest.json"):
                    self.assertEqual((a / file).read_bytes(), (b / file).read_bytes())

    def test_seeded_jitter_changes_and_steady_remains_exact(self):
        a = generate("jittered_60", 1, 40)[0]
        b = generate("jittered_60", 2, 40)[0]
        self.assertNotEqual(a["events_sha256"], b["events_sha256"])
        steady = generate("healthy_60", 7, 40)[0]["expected_assertions"]["pacing"]
        self.assertEqual(steady["min_ns"], "16666666")
        self.assertEqual(steady["max_ns"], "16666667")

    def test_pacing_gap_and_burst_not_smoothed_by_average(self):
        pacing = generate("irregular_60", frames=40)[0]["expected_assertions"]["pacing"]
        self.assertEqual(pacing["min_ns"], "1000000")
        self.assertEqual(pacing["max_ns"], "180000000")
        self.assertEqual(pacing["sample_count"], "39")

    def test_source_epoch_restart_resets_frame_id(self):
        _, events, _ = generate("source_restart", frames=40)
        keys = [x["frame_key"] for x in events if x["type"] == "source.frame"]
        self.assertNotEqual(keys[0]["stream_epoch"], keys[20]["stream_epoch"])
        self.assertEqual(keys[0]["frame_id"], keys[20]["frame_id"])
        self.assertGreater(int(keys[0]["stream_epoch"]), 2**53)

    def test_mediamtx_restart_does_not_force_epoch_reset(self):
        _, events, _ = generate("mediamtx_restart", frames=40)
        self.assertEqual(len({e["frame_key"]["stream_epoch"] for e in events if e["type"] == "source.frame"}), 1)
        self.assertEqual(sum(e["type"] == "transport.relayed" for e in events), 33)
        self.assertEqual(sum(e["type"] == "video.decoded" for e in events), 33)

    def test_decoder_stall_transport_keeps_progressing(self):
        _, events, _ = generate("decode_stall", frames=40)
        self.assertEqual(sum(e["type"] == "transport.received" for e in events), 40)
        self.assertEqual(sum(e["type"] == "video.decoded" for e in events), 31)
        self.assertEqual({e["session_id"] for e in events if e["type"] == "video.decoded"}, {"reg-s1", "reg-s2"})

    def test_cv_early_late_duplicate_reordered_and_missing(self):
        _, events, _ = generate("cv_timing", frames=16)
        by_type = lambda kind: [e for e in events if e["type"] == kind]
        self.assertGreater(len(by_type("cv.duplicate")), 0)
        self.assertGreater(len(by_type("cv.late_after_drop")), 0)
        self.assertGreater(len(by_type("overlay.dropped_missing_metadata")), 0)
        rec = {int(e["frame_key"]["frame_id"]): e for e in by_type("cv.received")}
        self.assertGreater(int(rec[5]["t_monotonic_ns"]), int(rec[6]["t_monotonic_ns"]))
        # CV before the Reg decode in the *same* Reg-local clock domain.
        decode_1 = next(e for e in by_type("video.decoded") if e["frame_key"]["frame_id"] == "1")
        self.assertLess(int(rec[1]["t_monotonic_ns"]), int(decode_1["t_monotonic_ns"]))

    def test_zero_target_is_valid_received_packet(self):
        _, events, _ = generate("cv_zero_vs_missing", frames=40)
        rx = [e for e in events if e["type"] == "cv.received"]
        self.assertEqual(len(rx), 20)
        self.assertEqual(sum(e["attributes"]["target_count"] == 0 for e in rx), 10)
        self.assertGreater(sum(e["type"] == "cv.sent" for e in events), len(rx))

    def test_delay_sweep_includes_evictions_and_coverage_difference(self):
        manifest, events, _ = generate("delay_sweep", frames=20)
        counts = manifest["expected_assertions"]["event_counts"]
        self.assertGreater(counts.get("overlay.evicted", 0), 0)
        per_epoch = {}
        for e in events:
            if e["type"].startswith("overlay.") and e["frame_key"]:
                k = e["frame_key"]["stream_epoch"]
                per_epoch.setdefault(k, {}).setdefault(e["type"], 0)
                per_epoch[k][e["type"]] += 1
        self.assertEqual(len(per_epoch), 5)
        self.assertEqual(per_epoch[str(2**53 + 19)].get("overlay.presented_exact", 0), 0)
        self.assertGreater(per_epoch[str(2**53 + 21)].get("overlay.presented_exact", 0), 0)

    def test_source_does_not_halt_with_sidecar_service_failure(self):
        _, events, _ = generate("services_independent", frames=40)
        self.assertEqual(sum(e["type"] == "video.raw_presented" for e in events), 40)
        self.assertIn("service.diagnostics_stale", {e["type"] for e in events})
        self.assertIn("recorder.queue_saturated", {e["type"] for e in events})

    def test_reset_clock_can_move_back_between_sessions(self):
        _, events, _ = generate("clock_reset", frames=40)
        samples = [e for e in events if e["type"] == "cv.sent"]
        a = next(e for e in samples if e["session_id"] == "jetson-s1")
        b = next(e for e in samples if e["session_id"] == "jetson-s2")
        self.assertLess(int(b["t_monotonic_ns"]), int(a["t_monotonic_ns"]))
        self.assertEqual(next(e["attributes"]["sent_total"] for e in samples if e["session_id"] == "jetson-s2"), "1")

    def test_flood_aggregates_at_oracle_not_raw_stdout(self):
        directory = self.fixture("ambiguous_flood")
        m = json.loads((directory / "manifest.json").read_text())
        self.assertEqual(m["expected_assertions"]["event_counts"]["transport.warning"], 400)
        self.assertLess(len(json.dumps(verify(directory))), 500)

    def test_public_projection_contains_no_ground_truth(self):
        directory = self.fixture("cv_timing")
        public = json.loads((directory / "public_manifest.json").read_text())
        self.assertNotIn("ground_truth", public)
        self.assertNotIn("expected_assertions", public)
        self.assertNotIn("fault_code", json.dumps(public))
        self.assertEqual(len(public["events_sha256"]), 64)

    def test_one_frame_has_empty_pacing_population(self):
        directory = self.fixture(frames=1)
        m = json.loads((directory / "manifest.json").read_text())
        self.assertEqual(m["expected_assertions"]["pacing"]["sample_count"], "0")
        self.assertIsNone(m["expected_assertions"]["pacing"]["max_ns"])
        self.assertEqual(verify(directory)["decoded_frames"], 1)

    def test_invalid_envelope_values(self):
        _, observations, _ = generate("healthy_60", frames=1)
        original = observations[0]
        for patch in ({"t_monotonic_ns": "-10"}, {"t_monotonic_ns": 10},
                      {"t_monotonic_ns": str(U64_MAX + 1)}, {"data_origin": "measured"},
                      {"host_role": "unauthorized"}, {"clock_domain": "unknown"},
                      {"attributes": {"log": "x" * 1000}},
                      {"frame_key": {"stream_epoch": "0001", "frame_id": "1"}}):
            with self.subTest(patch=patch):
                with self.assertRaises(ValueError):
                    validate_observation({**original, **patch})
        with self.assertRaises(ValueError):
            parse_json(b'{"x":1,"x":2}')
        with self.assertRaises(ValueError):
            parse_json(b'{"x":NaN}')

    def test_replay_catches_frame_epoch_mismatch_even_after_digest_rewrite(self):
        directory = self.fixture(frames=8)
        def tamper(events):
            packet = next(e for e in events if e["type"] == "cv.received")
            packet["frame_key"]["stream_epoch"] = str(int(packet["frame_key"]["stream_epoch"]) + 1)
        self.update_stream(directory, tamper)
        with self.assertRaisesRegex(ValueError, "exact FrameKey"):
            verify(directory)

    def test_replay_catches_missing_late_and_corrupt_stream(self):
        directory = self.fixture("cv_timing", 16)
        with (directory / "events.jsonl").open("ab") as fh:
            fh.write(b'{"bad": true}\n')
        with self.assertRaises(ValueError):
            verify(directory)
        directory = self.root / "corrupt2"
        write_fixture(directory, "healthy_60", frames=8)
        self.update_stream(directory, lambda events: events[0].update(t_monotonic_ns="-1"))
        with self.assertRaises(ValueError):
            verify(directory)

    def test_bounded_output_and_invalid_budget(self):
        with self.assertRaises(ValueError):
            generate("healthy_120", frames=5001)
        with self.assertRaises(ValueError):
            generate("healthy_120", frames=30, max_events=10)
        with self.assertRaises(ValueError):
            generate("healthy_60", seed=-1)
        with self.assertRaises(ValueError):
            generate("not-a-scenario")

    def test_cli_bounded_and_validated(self):
        output = self.root / "cli"
        def call(*parts):
            return subprocess.run([sys.executable, "-m", "tools.diagnostic_scenarios", *parts],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=8)
        created = call("generate", "cv_timing", "--frames", "16", "--out", str(output))
        self.assertEqual(created.returncode, 0, created.stderr)
        self.assertLess(len(created.stdout), 250)
        verified = call("verify", str(output))
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertLess(len(verified.stdout), 300)
        self.assertEqual(call("generate", "healthy_60", "--frames", "6000", "--out", str(output)).returncode, 2)


if __name__ == "__main__":
    unittest.main()


class CheckedInGoldenTests(unittest.TestCase):
    def test_checked_in_golden_files_are_exactly_regenerable(self):
        fixtures = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "diagnostics"
        for name, frames in (("healthy_60", 4), ("cv_timing", 8)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                reference = fixtures / name
                self.assertEqual(verify(reference)["scenario_id"], name)
                write_fixture(Path(tmp), name, frames=frames)
                for filename in ("manifest.json", "events.jsonl", "public_manifest.json"):
                    self.assertEqual((reference / filename).read_bytes(), (Path(tmp) / filename).read_bytes())
