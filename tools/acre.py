#!/usr/bin/env python3
"""AstralChainRe pipeline: your dump + update + keys -> native executable.

    acre.py setup --base BASE.nsp --update UPDATE.nsp [--keys DIR]
    acre.py export
    acre.py build
    acre.py run [--validate [--seconds N]]
    acre.py roots --from REPORT.json
    acre.py bench --script TAS.txt --binary A [--binary B] [--runs N]
"""
import argparse
import sys
from pathlib import Path

from acre import config


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="acre.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    p = commands.add_parser("setup", help="install the update, dump and verify the game modules")
    p.add_argument("--base", type=Path, required=True, help="base game NSP (v0)")
    p.add_argument("--update", type=Path, help="update 1.0.1 NSP (v65536)")
    p.add_argument("--keys", type=Path, default=config.default_keys_dir(),
                   help="directory holding prod.keys (default: %(default)s)")
    p.add_argument("--skip-update", action="store_true", help=argparse.SUPPRESS)

    commands.add_parser("export", help="generate C from the dumped modules (incremental)")

    p = commands.add_parser("build", help="compile astralchain-static from the export")
    p.add_argument("-j", "--jobs", type=int, help="parallel compile jobs (default: ninja's)")

    p = commands.add_parser("run", help="play, or --validate a headless strict run")
    p.add_argument("--validate", action="store_true", help="headless, strict AOT, timed run")
    p.add_argument("--seconds", type=int, default=180, help="validation length (default: %(default)s)")
    p.add_argument("extra", nargs=argparse.REMAINDER, help="extra frontend options after --")

    p = commands.add_parser("roots", help="add a run's fallback sites to game/roots")
    p.add_argument("--from", dest="report", type=Path, required=True, help="report JSON of a run")

    p = commands.add_parser("bench", help="A/B builds by replaying a TAS script from a profile snapshot")
    p.add_argument("--script", type=Path, required=True, help="TAS script recorded with --tas-record")
    p.add_argument("--binary", type=Path, action="append", required=True,
                   help="astralchain-static to measure; repeat to compare")
    p.add_argument("--runs", type=int, default=3, help="runs per binary (default: %(default)s)")
    p.add_argument("--profile", type=Path, default=config.WORK / "bench" / "snapshot",
                   help="profile copied fresh for every run (default: %(default)s)")
    p.add_argument("--ini", type=Path, default=config.WORK / "bench" / "bench.ini",
                   help="astralchain.ini for the runs (default: %(default)s)")
    p.add_argument("--settle", type=int, default=10, help="seconds after the script before measuring")
    p.add_argument("--seconds", type=int, default=40, help="measurement window (default: %(default)s)")

    args = parser.parse_args(argv)
    if args.command == "setup":
        from acre import setup
        if args.update is None and not args.skip_update:
            parser.error("--update is required (the supported version is 1.0.1)")
        setup.run(args.base, None if args.skip_update else args.update, args.keys)
    elif args.command == "export":
        from acre import export
        export.run()
    elif args.command == "build":
        from acre import build
        build.run(args.jobs)
    elif args.command == "run":
        from acre import run
        extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
        run.run(args.validate, args.seconds, extra)
    elif args.command == "roots":
        from acre import coverage
        coverage.run(args.report)
    elif args.command == "bench":
        from acre import bench
        bench.run(args.binary, args.script, args.runs, args.profile, args.ini, args.settle, args.seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
