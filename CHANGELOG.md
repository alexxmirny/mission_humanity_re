# Changelog

## 0.2.2-rc4 — 2026-11-22

Release candidate 4, reclassifying the briefly published 0.2.2 release. Same gameplay source, locally rebuilt with rc4 stamps; no CI or test gates rerun by explicit request. Game Stable returns to 0.2.1; Launcher 0.2.2 is unchanged. The `release/v0.2.2` line remains hotfix-only; new features target a later release.

### Since game 0.2.2-rc3

- Queue strategic moves and attacks with Shift-right-click. Create repeating patrols with Alt-right-click and extend them with Shift+Alt-right-click, including aircraft patrols.
- Show complete selected routes and targets: green movement, red attacks, blue patrols and yellow storage rallies. Hold Shift to reveal short routes. Target display supports shared-command grouping and local appearance settings.
- Double-click selects visible own units of the same prototype; Shift adds them to the selection.
- Right-click sets a selected storage building's rally destination, including manual Exit All departures. Its optional Auto button dispatches newly produced units and units repaired to full health. Auto starts off; enabling it does not eject existing healthy occupants, begin repairs or spend resources. Shuttles have neither feature.
- Map-picker terrain, ore and landing-spot previews, with Description/Preview/Settings pages in the picker and lobby.
- Host-selected match speed: 100/125/150/175/200%, default 100%, fixed for that match.
- Launcher-managed loose-file mods, ordered resource overrides, new skirmish maps, transactional patches to existing configuration records and custom terrain. Later/lower mods win. Multiplayer checks the complete ordered content; save mod-set differences warn and load unless required resources are missing.
- Launcher-remappable strategic keys, modifiers and world mouse roles, with search, primary/alternate bindings, conflict feedback and resets. Corrected extended navigation-key handling in strategic dialogs.
- Fixed early Start/map-header and initial map-download reset races. Improved resync recovery and completed-simulation-boundary handling of disconnected players and host-migration teardown.
- Corrected loaded-save → New Campaign starting-planet selection to use the current boot configuration.
- Corrected native video worker/resource lifetime and shared-read handling.

### Control change

Shift-right-click now queues orders. Ctrl-right-click confirms special interactions such as landing/deployment, merging, storage entry and tactical entry. Unmodified double-right-click remains available. These are defaults; strategic bindings can be changed in the launcher.

### Also new since stable 0.2.1

- Interface size independent of rendering resolution, automatic display scaling and resolutions not restricted to multiples of 32.
- Optional raised limits: 500 units and buildings per player, specialist building pools up to 127 and 4000 projectiles.
- Improved sequential departures and storage boarding; steadier tactical scrolling.
- Optional campaign tech-tree unlock rechecks after loading or revisiting a planet.
- Build/research panel fixes for third and later player seats, soldier-shot desync repair and garage-exit diagonal flank guards.
- Host-selected maximum command delay: 400 ms default, with 600/800/1000 ms options.
- Recovered diagnostic faults no longer leave false crash markers.

### Launcher since launcher 0.2.2-rc2

- Mods page with discovery, enable/disable, priority ordering and Apply/Revert.
- Settings → Key bindings for strategic actions, modifier/mouse roles, capture, collisions and resets.
- One shared Shift role for queueing, route reveal, repeated construction and additive selection.
- Clean settings hide Apply/Revert; edits preserve unrelated INI content.
- Configuration-language selection for localized mod patches.

### Compatibility and defaults

- Back up saves before saving with this version. New storage-policy saves are rejected by older pre-policy builds. Old native and legacy raised saves remain supported inputs; raised-limit saves require Raised limits enabled.
- Multiplayer requires the same release and identical ordered mod content, including cosmetic and shadowed files. Mods are not automatically transferred.
- Mods and bindings apply next game start; no hot reload. Invalid active mod sets fall back together to stock with a diagnostic reason. Missing required custom terrain refuses save loading before mutation; other mod-set differences warn and load.
- Improved movement, group movement and combat remain on by default. Raised limits, Tech tree fixes and each building's Auto dispatch remain off.

### Known limits

- Windows only: 32-bit game, 64-bit launcher. Eight players; no joining an already-started match; one map transfer at a time. Direct-address hosting needs port forwarding; TCP has neither relay nor host migration.
- A different second-map download in one process and same-match resync across already-applied/repeated drop/rejoin removals remain unresolved.
- Remapped held big-map keys can release on Shift and fail to reacquire on Shift-up; default Tab works.
- Raised limits retain the original fog-viewer counter limitation at 256 overlapping viewers. The selection cap remains 200 units.
- Per-unit queues hold 32 unconsumed entries including the active head. Special landing/storage/merge/tactical interactions are not ordinary queued moves. Very dense maximum-length selected routes can be expensive.
- Configuration mods edit existing records; they cannot add/remove unit or building records. No automatic mod installation or dependency solver.
- Procedural map generation and PNG/TLO authoring are source/CLI tools, not an in-game Generate button or generated campaign.
- This release does not claim to resolve every historical field crash.

### Installation

Download `mh_launcher.exe` from the launcher release, place it beside `mh.exe` and run it. You need your own copy of the game; no retail game data is included. Normal UDP relay play needs no port forwarding. The Report page sends reports only with consent. Do not run the launcher as administrator; it asks when installation needs elevation.

### Source tooling

- Procedural skirmish-map generation and custom-terrain authoring tools.
- Expanded deterministic, replay and UI checks, with owned temporary-output retention and cleanup.
- Consistent native Windows file identities across Python 3.11/3.12, preserving active-producer snapshots and strict deletion safeguards; explicit LF checkout rules for generated strategic bindings.
