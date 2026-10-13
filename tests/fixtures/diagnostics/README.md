# Canonical synthetic golden fixtures

All files under each directory are **synthetic test data**, not recorded Jetson/RTSP evidence. Regenerate from the repository root using Python 3.10+:

```bash
python -m tools.diagnostic_scenarios generate healthy_60 --frames 4 --seed 20261012 --out tests/fixtures/diagnostics/healthy_60
python -m tools.diagnostic_scenarios generate cv_timing --frames 8 --seed 20261012 --out tests/fixtures/diagnostics/cv_timing
python -m tools.diagnostic_scenarios verify tests/fixtures/diagnostics/healthy_60
python -m tools.diagnostic_scenarios verify tests/fixtures/diagnostics/cv_timing
python -m unittest discover -s tools/diagnostic_scenarios -p 'test_*.py' -v
```

`healthy_60` is the clean exact-match example: 4 decoded and Raw-presented frames, 4 valid CV packets and 4 `overlay.presented_exact`, with 0 missing/evicted. `cv_timing` contains a full eight-case pattern including early CV, late CV after a dropped video frame, no CV, duplicates, receive reorder and valid zero-target metadata. Ground-truth fault labels reside only in its private `manifest.json` and must be excluded from blind diagnosis. The public manifest deliberately uses an opaque fixture ID instead of scenario name.

Other (larger) cases, including the 5-way delay sweep and 400-warning context flood, are generated on demand to avoid repository bloat. Manifest digests and expected event counts should change only alongside intentional generator-version/semantic changes.
