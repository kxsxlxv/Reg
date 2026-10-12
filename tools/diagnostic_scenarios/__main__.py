"""python -m tools.diagnostic_scenarios {list,generate,verify,summarize}."""
import argparse
import json
from pathlib import Path
import sys

from .lab import SCENARIOS, generate, verify, write_fixture


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Offline synthetic Reg diagnostics (no hardware)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    gen = sub.add_parser("generate")
    gen.add_argument("scenario", choices=sorted(SCENARIOS))
    gen.add_argument("--out", type=Path, required=True)
    gen.add_argument("--seed", type=int, default=20261012)
    gen.add_argument("--frames", type=int, default=60)
    gen.add_argument("--max-events", type=int, default=100000)
    check = sub.add_parser("verify")
    check.add_argument("directory", type=Path)
    check = sub.add_parser("summarize")
    check.add_argument("directory", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "list":
            for name, desc in sorted(SCENARIOS.items()):
                print(f"{name}: {desc}")
        elif args.command == "generate":
            manifest = write_fixture(args.out, args.scenario, args.seed, args.frames, args.max_events)
            print(json.dumps({"scenario_id": manifest["scenario_id"], "event_count": manifest["event_count"],
                              "digest": manifest["events_sha256"]}, sort_keys=True))
        else:
            print(json.dumps(verify(args.directory), sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"fixture error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
