"""CLI: retail INIT.CFG + INITLANG.CFG -> editable YAML source tree.

  python -m src.formats.cfgkit.cfg_decompile [--init ...] [--lang ...] [--out mod_src/cfg]
                                             [--apply-aliases] [--bootstrap-aliases]

Writes the split-by-meaning tree (see cfgkit.yamlio). Run once to seed the
sources; thereafter edit the YAML and recompile with cfg_compile.

With ``--bootstrap-aliases`` it also (re)generates ``<out>/aliases.yaml`` — a
draft wire->English table seeded from initlang's English glosses (text ids) plus
the ore resources — and applies it, so the emitted tree reads in English while
the compiled cfg stays byte-identical. ``--apply-aliases`` applies an already
curated ``aliases.yaml`` without regenerating it.
"""
from __future__ import annotations

import argparse
import os
import sys

from . import aliases as A
from . import yamlio
from ._paths import CLEAN
from .parse import parse_files


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--init", default=CLEAN + "/mh/init/INIT.CFG")
    ap.add_argument("--lang", default=CLEAN + "/mh_ex/init/initlang.cfg")
    ap.add_argument("--out", default="mod_src/cfg")
    ap.add_argument("--apply-aliases", action="store_true",
                    help="emit the tree with English names from <out>/aliases.yaml")
    ap.add_argument("--bootstrap-aliases", action="store_true",
                    help="(re)generate <out>/aliases.yaml from glosses, then apply it")
    ap.add_argument("--layout", choices=("class", "per-object"), default="class",
                    help="class: one file per class (default); per-object: one file per "
                         "unit/building/weapon with its exclusively-owned anims/icons folded in")
    args = ap.parse_args(argv)

    model = parse_files(args.init, args.lang)

    table = None
    alias_path = os.path.join(args.out, "aliases.yaml")
    if args.bootstrap_aliases:
        table = A.bootstrap(model, args.lang)
        table.dump(alias_path)
        print(f"bootstrapped alias table -> {alias_path} ({len(table)} names)")
    elif args.apply_aliases:
        table = A.AliasTable.load(alias_path)
        errs, _ = table.check(model)
        for e in errs:
            print(f"  alias WARN: {e}")

    if args.layout == "per-object":
        yamlio.write_tree_per_object(model, args.out, aliases=table)
    else:
        yamlio.write_tree(model, args.out, aliases=table)
    cen = model.census()
    print(f"decompiled retail -> {args.out}"
          + (" (English aliases applied)" if table else "")
          + (" [per-object layout]" if args.layout == "per-object" else ""))
    print(f"  {cen['DEFINE']} defines, {cen['TEXT']} texts, "
          + ", ".join(f"{cen[c]} {c.lower()}" for c in
                      ("UNIT", "BUILDING", "WEAPON", "ANIM", "PROGRESS", "PLANET")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
