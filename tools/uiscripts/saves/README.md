# Scenario saves (committed 2026-09-12, user ruling)

The save files the registered test scenarios enter from. Committed to the repo (and publishable
-- user ruling 2026-09-12: player-made saves, not game-shipped data) so a scenario is
self-contained instead of depending on a machine-local save directory.

| file | used by | source location it was rescued from |
| --- | --- | --- |
| `11.sav` | all five `poz*` tactical journals (`; save 11` in each header -- `--tact-replay` stages it into the lane's `save/`) | `workdir/mh_en/save/11.sav` (machine-local POLYGON install) |
| `ayy30.sav` | the `soak_saved` registered scenario (seeds `--soak` via `loadgame_at`) | `workdir/mh_en/saves_storage/ayy30.sav` (SAVE_STORAGE, the 137-save community index) |

Since fork item F1D (2026-09-12) this directory IS the live staging source: `ui_test.resolve_save()`
prefers the committed copy here and falls back to the machine dirs (`machine.POLYGON/save`,
`machine.SAVE_STORAGE`) only for saves not committed here -- so these two scenarios run from a clean
clone, while the 137-save SAVE_STORAGE index and ad-hoc polygon saves keep working unchanged.
