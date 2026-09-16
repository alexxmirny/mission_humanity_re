"""CLI: YAML source tree -> validated, compiled game cfg.

  python -m src.formats.cfgkit.cfg_compile [--src mod_src/cfg] [--out mod_src/overlay]
                                           [--assets-root DIR] [--no-validate] [--force]

Reads the YAML sources, runs the static validator (aborting on any error unless
--force), and emits ``<out>/mh/init/INIT.CFG`` + ``<out>/mh_ex/init/initlang.cfg``
in build_mod.py's ``--overlay`` shape. Full loop:

  edit mod_src/cfg/**  ->  cfg_compile  ->  build_mod --overlay mod_src/overlay  ->  launch
"""
from __future__ import annotations

import argparse
import os
import sys

from . import aliases as A
from . import emit, validate, yamlio


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", default="mod_src/cfg")
    ap.add_argument("--out", default="mod_src/overlay")
    ap.add_argument("--assets-root", help="verify PLANET.map etc. exist under this dir")
    ap.add_argument("--no-validate", action="store_true")
    ap.add_argument("--no-aliases", action="store_true",
                    help="treat source identifiers as wire names (skip aliases.yaml)")
    ap.add_argument("--force", action="store_true", help="emit even if validation has errors")
    args = ap.parse_args(argv)

    table = None
    if not args.no_aliases:
        table = A.AliasTable.load(os.path.join(args.src, "aliases.yaml"))
        if len(table):
            print(f"aliases: {len(table)} English names -> wire "
                  f"(from {args.src}/aliases.yaml)")

    try:
        model = yamlio.read_tree(args.src, aliases=table)
    except yamlio.IndexDensityError as e:
        print(e)
        print("ABORT: fix the object indices (dense 0..N-1 per class).")
        return 1

    if not args.no_validate:
        findings = validate.validate(model, assets_root=args.assets_root)
        errs = [f for f in findings if f.level == "error"]
        warns = [f for f in findings if f.level == "warn"]
        for f in findings:
            print(f"  [{f.level:5}] {f.cls}.{f.field} {f.obj!r}: {f.message}")
        print(f"validation: {len(errs)} errors, {len(warns)} warnings")
        if errs and not args.force:
            print("ABORT: fix the errors above or pass --force.")
            return 1

    ip = os.path.join(args.out, "mh", "init", "INIT.CFG")
    lp = os.path.join(args.out, "mh_ex", "init", "initlang.cfg")
    emit.write_files(model, ip, lp)
    print(f"compiled -> {ip}\n         -> {lp}")
    print(f"next: python src/formats/build_mod.py --overlay {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
