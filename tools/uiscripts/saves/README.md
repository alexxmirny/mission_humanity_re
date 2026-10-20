# Scenario saves (committed 2026-09-12, user ruling)

The save files the registered test scenarios enter from. Committed to the repo (and publishable
-- user ruling 2026-09-12: player-made saves, not game-shipped data) so a scenario is
self-contained instead of depending on a machine-local save directory.

| file | used by | source location it was rescued from |
| --- | --- | --- |
| `11.sav` | all five `poz*` tactical journals (`; save 11` in each header -- `--tact-replay` stages it into the lane's `save/`) | `workdir/mh_en/save/11.sav` (machine-local POLYGON install) |
| `2-almost.sav` | `res_walk` / `res_walk_auto` (the resolution walks: a campaign save on Aman, a 64 x 64 map with 8 available and 13 owned buildings, a selected landed building and two insect groups beside the wrap seam), `galaxy_bg_picker`, and the `campaign_planet_switch` scenario (ESC -> Load game from a campaign begun on Nortus; this save sits on Aman) | `workdir/mh_en/save/2-almost.sav` (machine-local POLYGON install) |
| `ayy30.sav` | `res_walk` (the second Load row: 21 available buildings and 74 units in groups of 5 / 18 / 51, for the panel lists that outgrow 2-almost) and the `soak_saved` registered scenario (seeds `--soak` via `loadgame_at`) | `workdir/mh_en/saves_storage/ayy30.sav` (SAVE_STORAGE, the 137-save community index) |
| `cfgmod.sav` | the `cfg_newgame_*` rows (mods:G17): `2-almost.sav` with one byte flipped in each of the seven baked cfg tables, so a Load visibly changes them and a New Game must put the INIT.CFG parse back | built by `tools/make_cfgmod_sav.py` (`--check` verifies it) |
| `5-kofu-c.sav` | the `mods:M1` movement probe (`m1_mv_replay.txt`, `[harness] mv_replay`): a campaign save on planet 5 (the user's 'Unit movement test', 2026-10-06; player 0 ground units 2, 3, 8, 12, 13, 19 are the ones the 20 recorded moves in `tools/uiscripts/m1_moves.csv` name) | `workdir/mh_en/save/5-kofu-c.sav` (machine-local POLYGON install; player-made, publishable per the ruling above) |
| `7-abigan-5.sav` | the `m1n_marshal_*` rows (mods:M1N field fix): planet 7, a 1-tile corridor at x=14 between building columns; player 0 unit 12 at (11,94), unit 1 idle at (14,106). The user's rc10 playtest save (2026-10-16): units stuck in the corridor behind a friend | the rc10 playtest report (`tmp/playtest/0.2.1-rc10/`; player-made, publishable per the ruling above) |
| `7-abigan-5-1.sav` | the `m1n_route_corner*` rows (mods:M1N follow-up 2, route planner): the same planet 7 base as `7-abigan-5.sav`, saved later in the rc11 playtest (2026-10-17) with player 0 unit 1 idle at (14,106) and unit 12 idle at (14,96) inside the x=14 corridor; unit 1 ordered to (7,93) has its only exit through unit 12's tile and ended in a dead-end pocket at (12,99) | the rc11 playtest report (`tmp/playtest/0.2.1-rc11/`; player-made, publishable per the ruling above) |
| `nortus-1.sav` | the `m1h_*` merge fixture F8 (mods:M1H; `m1h_f8.csv`): planet 1 (Nortus, 64x64), player 0 units 1-5 beside enemy building p1/1, unit 6 at 22,49. The save of the M1 squad-merge probe (a Shift-click merge into an attacking squad) | `workdir/mh_en/save/nortus-1.sav` (machine-local POLYGON install; player-made, publishable per the ruling above) |
| `ayy30-tests.sav` | the `m1h_*` group-speed fixture F6 (mods:M1H; `m1h_f6.csv`): a variant of `ayy30.sav` saved for the M1 group-speed probe (2026-10-14): player 0 units 1,2,3 / 75,76 / 77,78 of three speed classes in one blob | `workdir/mh_en/save/ayy30-tests.sav` (machine-local POLYGON install; player-made, publishable per the ruling above) |
| `6-taalor-6-c.sav` | the `walker_turn` row (the Centaur U-turn, render only): planet 6 (Taalor), player 0 has 30 alien Walker1 units, the lone one at tile 194,242 is selected by a click at 367,172 with the camera pinned by `camto 183 237`. The user's rc14 playtest save (2026-10-18) | the rc14 playtest report (`tmp/playtest/0.2.1-rc14/`; player-made, publishable per the ruling above) |

Since fork item F1D (2026-09-12) this directory IS the live staging source: `ui_test.resolve_save()`
prefers the committed copy here and falls back to the machine dirs (`machine.POLYGON/save`,
`machine.SAVE_STORAGE`) only for saves not committed here -- so these two scenarios run from a clean
clone, while the 137-save SAVE_STORAGE index and ad-hoc polygon saves keep working unchanged.

A scenario that needs two saves lists both in `deploy_save` (comma separated, e.g. `2-almost,ayy30`). The Load dialog
shows them newest-first -- the runner stamps the first name newest and each later one a minute older -- and the dialog
preselects the LAST LOADED save, not the top row, so a walk that switches saves clicks the row it wants (the split
click pattern, `cursor`/`press`/`release`) instead of relying on the preselection.

A row may list `name*N` (for example `2-almost*12`) to deploy N copies of one save as `name`, `name_02` .. `name_NN` -- the Load list then holds N rows (D15: `wheel_menu_lists` needs more than the 8 rows the dialog shows).
