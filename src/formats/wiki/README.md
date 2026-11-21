# Game wiki generator

Builds a static, bilingual (EN/RU) wiki of the game's units, buildings, projects, weapons, planets
and systems, plus a tech tree, straight from a retail install. No retail data is committed.

    python -m src.formats.wiki build --out tmp/wiki_site --stills   # model + art + site + checks
    python -m src.formats.wiki check --out tmp/wiki_site            # re-check a built site
    python -m src.formats.wiki --selftest                           # synthetic data, ~1 s

`--game` / `--ru` default to `MH_WIKI_EN` / `MH_WIKI_RU`, `tools/machine_config.py` and the
gitignored `game_data/` drops. Exit status is non-zero when any check fails.

Modules: `source` (packs -> INIT/INITLANG/INFO), `model` (view-models, in-game panel formulas),
`art/` (Pillow extraction), `techtree` (layout + SVG), `site` (Jinja2 pages, search, checks),
`selftest`.
