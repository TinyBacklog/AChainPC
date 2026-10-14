#!/usr/bin/env python3
"""AstralChainRe pipeline: your dump + update + keys -> native executable.

    acre.py setup --base BASE.nsp --update UPDATE.nsp [--keys DIR]
    acre.py export
    acre.py build
    acre.py run [--validate [--seconds N]]
    acre.py roots --from REPORT.json
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
