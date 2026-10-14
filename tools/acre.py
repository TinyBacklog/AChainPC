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

    args = parser.parse_args(argv)
    if args.command == "setup":
        from acre import setup
        if args.update is None and not args.skip_update:
            parser.error("--update is required (the supported version is 1.0.1)")
        setup.run(args.base, None if args.skip_update else args.update, args.keys)
    return 0


if __name__ == "__main__":
    sys.exit(main())
