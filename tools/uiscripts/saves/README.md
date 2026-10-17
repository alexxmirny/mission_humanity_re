# Scenario saves (committed 2026-09-12, user ruling)

The save files the registered test scenarios enter from. Committed to the repo (and publishable
-- user ruling 2026-09-12: player-made saves, not game-shipped data) so a scenario is
self-contained instead of depending on a machine-local save directory.

| file | used by | source location it was rescued from |
| --- | --- | --- |
| `11.sav` | all five `poz*` tactical journals (`; save 11` in each header -- `--tact-replay` stages it into the lane's `save/`) | `workdir/mh_en/save/11.sav` (machine-local POLYGON install) |
| `2-almost.sav` | `res_walk` / `res_walk_auto` (the resolution walks: a campaign save on Aman, a 64 x 64 map with 8 available and 13 owned buildings, a selected landed building and two insect groups beside the wrap seam), `galaxy_bg_picker`, and the `campaign_planet_switch` scenario (ESC -> Load game from a campaign begun on Nortus; this save sits on Aman) | `workdir/mh_en/save/2-almost.sav` (machine-local POLYGON install) |
| `ayy30.sav` | `res_walk` (the second Load row: 21 available buildings and 74 units in groups of 5 / 18 / 51, for the panel lists that outgrow 2-almost) and the `soak_saved` registered scenario (seeds `--soak` via `loadgame_at`) | `workdir/mh_en/saves_storage/ayy30.sav` (SAVE_STORAGE, the 137-save community index) |
| `5-kofu-c.sav` | stock scene-replay vehicle (mp:REPSCENE, `tools/replay_match_segment.py` `DEFAULT_VEHICLE_SAVE`): a campaign save on planet 5 (the user's 'Unit movement test', 2026-10-06); the scene import overwrites the world over it | player-made, publishable per the ruling above |
| `nortus-pre-c.sav` | stock scene-replay vehicle for a Nortus (planet 1) new-game segment (mp:REPSCENE `VEHICLE_BY_PLANET`) | player-made, publishable per the ruling above |

Since fork item F1D (2026-09-12) this directory IS the live staging source: `ui_test.resolve_save()`
prefers the committed copy here and falls back to the machine dirs (`machine.POLYGON/save`,
`machine.SAVE_STORAGE`) only for saves not committed here -- so these two scenarios run from a clean
clone, while the 137-save SAVE_STORAGE index and ad-hoc polygon saves keep working unchanged.

A scenario that needs two saves lists both in `deploy_save` (comma separated, e.g. `2-almost,ayy30`). The Load dialog
shows them newest-first -- the runner stamps the first name newest and each later one a minute older -- and the dialog
preselects the LAST LOADED save, not the top row, so a walk that switches saves clicks the row it wants (the split
click pattern, `cursor`/`press`/`release`) instead of relying on the preselection.
