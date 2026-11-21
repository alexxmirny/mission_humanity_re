"""Wiki CLI.

    python -m src.formats.wiki build --out tmp/wiki_site [--game EN] [--ru RU] [--lang en,ru]
                                     [--cache DIR] [--stills|--no-stills] [--mh-tools EXE]
                                     [--mh-exe EXE] [--layout py]
    python -m src.formats.wiki check --out tmp/wiki_site       # re-check an existing site
    python -m src.formats.wiki --selftest                      # synthetic data, no retail files

``--game`` / ``--ru`` default to ``source.default_game()`` / ``default_ru()`` (``MH_WIKI_EN`` /
``MH_WIKI_RU``, ``tools/machine_config.py``, the gitignored ``game_data/`` drops).  Exit status is
non-zero when any site check fails (or the selftest fails).
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path


def _ap():
    ap = argparse.ArgumentParser(prog="python -m src.formats.wiki", description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="run the synthetic-data selftest")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build", help="model + art + site + checks")
    b.add_argument("--game", help="EN install dir or zip (default: auto-detected)")
    b.add_argument(
        "--ru", help="RU install dir/zip or bare mh_ex pack dir (default: auto-detected)"
    )
    b.add_argument("--lang", default="en,ru", help="comma-separated languages (default en,ru)")
    b.add_argument("--out", required=True, help="output site directory")
    b.add_argument("--cache", help="art cache dir (default tmp/wiki_art)")
    b.add_argument("--stills", action=argparse.BooleanOptionalAction, default=False)
    b.add_argument("--mh-tools", help="mh_tools.exe path (sprite banks)")
    b.add_argument("--mh-exe", help="mh.exe path (icon table; default <game>/mh.exe)")
    b.add_argument("--layout", choices=("py",), default="py", help="tech-tree layout engine")
    c = sub.add_parser("check", help="re-check an already built site (links, languages)")
    c.add_argument("--out", required=True)
    c.add_argument("--lang", help="languages (default: every language directory under --out)")
    return ap


def _report(problems):
    bad = 0
    for k, v in problems.items():
        print(f"check {k}: {'OK' if not v else str(len(v)) + ' PROBLEMS'}")
        for x in v[:8]:
            print("   ", x)
        bad += len(v)
    return bad


def cmd_build(a):
    from . import art, site, source, starmap
    from .model import build

    t0 = time.time()
    game = a.game or source.default_game()
    ru = a.ru or source.default_ru()
    langs = tuple(x.strip() for x in a.lang.split(",") if x.strip())
    if not game:
        print("no EN install found: pass --game or set MH_WIKI_EN", file=sys.stderr)
        return 2
    if "ru" in langs and not ru:
        print(
            "no RU install found: pass --ru, set MH_WIKI_RU, or drop ru from --lang",
            file=sys.stderr,
        )
        return 2
    wiki = build(source.load(game, ru=ru if "ru" in langs else None))
    t1 = time.time()

    out = Path(a.out)
    art_dir = out.parent / (out.name + "_art")
    src = art.ArtSource(game, a.mh_exe, a.cache, a.mh_tools)
    store = art.SpriteStore(src)
    pm, pt, pi = art.extract_planet_maps(src, art_dir)
    maps = {
        "planetmaps": pm,
        "planetthumbs": pt,
        "planetmapinfo": pi,
        "icons": art.extract_icons(src, art_dir),
        "buildings": art.extract_building_sprites(src, art_dir, store),
        "units": art.extract_unit_sprites(src, art_dir, store),
        "planets": art.extract_planets(src, art_dir),
        "stills": art.extract_stills(src, art_dir) if a.stills else {},
        "starmaps": starmap.extract_starmaps(src, wiki, art_dir),
    }
    t2 = time.time()
    rep = site.build_site(wiki, maps, out, langs=langs, art_dir=art_dir, strict=False)
    shutil.rmtree(art_dir, ignore_errors=True)
    t3 = time.time()
    problems = site.run_checks(wiki, rep.mapping, out, langs)
    problems["tech_tree"] = list(rep.errors)
    t4 = time.time()

    n = sum(len(v) for v in wiki.by_kind.values())
    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file()) / 1e6
    print(f"entities {n}  pages {rep.pages}  search {rep.search_entries}  tech {rep.tech}")
    print(f"site {out.resolve()}  {size:.1f} MB")
    print(
        f"timings: model {t1 - t0:.1f}s  art {t2 - t1:.1f}s  site {t3 - t2:.1f}s  "
        f"checks {t4 - t3:.1f}s  total {t4 - t0:.1f}s"
    )
    for w in rep.warnings:
        print("warning:", w)
    for note in src.notes[:6]:
        print("art note:", note)
    bad = _report(problems)
    print("FAIL" if bad else "OK")
    return 1 if bad else 0


def cmd_check(a):
    from . import site

    out = Path(a.out)
    if not (out / "index.html").is_file():
        print(f"{out}: not a built site (no index.html)", file=sys.stderr)
        return 2
    langs = (
        tuple(a.lang.split(","))
        if a.lang
        else tuple(sorted(p.name for p in out.iterdir() if p.is_dir() and p.name != "assets"))
    )
    problems = {
        "links": site.check_links(out),
        "languages": site.check_languages(out, langs),
        "search_json": [],
    }
    for la in langs:
        try:
            json.loads((out / la / "search.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            problems["search_json"].append(f"{la}: {e}")
    bad = _report(problems)
    print("FAIL" if bad else "OK")
    return 1 if bad else 0


def main(argv=None):
    a = _ap().parse_args(argv)
    if a.selftest:
        from . import selftest

        return selftest.main()
    if a.cmd == "build":
        return cmd_build(a)
    if a.cmd == "check":
        return cmd_check(a)
    _ap().print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
