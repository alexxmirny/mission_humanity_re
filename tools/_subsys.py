# _subsys.py -- the single subsystem taxonomy shared by the coverage + architecture tools.
#
# NOT a shim-run tool (the leading underscore marks it a library). It holds the name->subsystem
# classifiers that were originally inlined in the codemap generator, extracted 2026-07-26 when
# the state-matrix generator needed the SAME buckets: the state matrix rolls (region x function) site
# counts up by subsystem, and if it used a second taxonomy its rows could not be read against the
# codemap's coverage rows or against docs/architecture.md.
#
# One taxonomy, one place. Importers:
#   - the codemap generator       -- per-subsystem code + data coverage (tmp/codemap.html)
#   - the state-matrix generator  -- the (state region x function) read/write matrix
#
# Classification is by SYMBOL NAME PREFIX, which works because the naming convention (docs/conventions.md#symbol-naming)
# is enforced: llm_<domain>_ for functions, _G_LLM_<DOMAIN>_ for globals. An unnamed or
# off-convention symbol lands in "Other named" / "Game/boot/misc" -- that is a naming gap, not a
# tool bug, and the codemap's unnamed-mass rows are where it shows up.
#
# Pure Python (no Ghidra deps) so it imports trivially under PyGhidra and is unit-testable with a
# plain `python tools/_subsys.py`.
#
# To import from a Ghidra tool (which gets the repo root from currentProgram, not __file__):
#   import os, sys
#   sys.path.insert(0, os.path.join(repo_root(), "tools"))
#   import _subsys
#   try: import importlib; importlib.reload(_subsys)   # pick up edits in a live PyGhidra process
#   except Exception: pass

# Canonical order -- report rows render in this sequence so two reports line up visually.
SUBSYSTEMS = [
    "Strategic sim/economy",
    "Strategic AI",
    "Tactical mode",
    "Netcode",
    "UI/menus",
    "Input/window",
    "Graphics/FX",
    "Sound",
    "Map/geometry",
    "Config/formats",
    "Game/boot/misc",
    "Runtime/utility",
    "Other named",
]


def classify_fn(full):
    """Bucket a function by its (convention-following) name. `full` is the symbol name."""
    n = full
    if n.startswith("llm_strat_ai_") or n.startswith("llm_ai_"):
        return "Strategic AI"
    if n.startswith("llm_tact_"):
        return "Tactical mode"
    if n.startswith("llm_gfx_") or n.startswith("llm_fx_"):
        return "Graphics/FX"
    # NOTE the trailing underscore on llm_strat_play_ -- without it this rule also swallowed the
    # six llm_strat_player_* SIM functions into Sound (found 2026-07-26 via the state matrix's R4
    # "render writes sim state" violations, which were really misfiled-layer false positives; the
    # codemap's Sound/Strategic coverage split was slightly wrong for as long as the rule existed).
    if n.startswith("llm_snd_") or "select_voice" in n or n.startswith("llm_strat_play_"):
        return "Sound"
    if n.startswith("llm_net_"):
        return "Netcode"
    if (
        n.startswith("llm_map_")
        or n.startswith("GetMap")
        or n.startswith("ReadMap")
        or n.startswith("map_")
    ):
        return "Map/geometry"
    if (
        n.startswith("llm_ui_")
        or n.startswith("game_ui_")
        or n.startswith("llm_menu_")
        or n.startswith("llm_dlg_")
        or n.startswith("llm_lobby_")
        or n.startswith("llm_cursor_")
    ):
        return "UI/menus"
    if n.startswith("llm_input_") or n.startswith("llm_wnd_") or n.startswith("llm_key"):
        return "Input/window"
    if (
        n.startswith("llm_cfg_")
        or n.startswith("cfg_")
        or n.startswith("ReadObjects")
        or n.startswith("CountObjects")
        or n.startswith("HandleConfig")
        or n.startswith("HandleObject")
        or n.startswith("HandleText")
        or n.startswith("HandleDefine")
        or n.startswith("Parse")
        or n.startswith("Object")
        or n.startswith("GetParameter")
        or n.startswith("GetDefineIndex")
        or n.startswith("rsr_")
        or n.startswith("ReadToEndline")
    ):
        return "Config/formats"
    # `game_*` IS THE SIM, not boot -- MEASURED, not inferred from the prefix (2026-08-28).
    # The rule above used to read `game::` and mapped it to Game/boot/misc; when the extinct form
    # was repaired the flattened `game_` inherited that bucket unexamined. The ledger disagrees:
    # 14 of the 20 non-UI `game_*` functions are `verified` T1 rows of the SIM closure's batch F
    # (game_HandleProgress, game_HandleUpgrade, game_SetEvent, game_SpendResource,
    # game_UpdateResourceStats, game_AddTo*/AddProject*, game_HandleInvasion, game_TryStartProject,
    # game_GetStartingUnit, game_InsertItemInPlayerArray, game_UpdateProgress,
    # game_AddPlanetToAvailable). Of the remaining six, five are siblings of those families called
    # from sim (game_ClearAvailableProjects <- llm_strat_new_game_init;
    # game_RemoveFromAvailable{Buildings,Projects} <- llm_progress_finalize_acquire;
    # game_UpdatePlanetProgress <- llm_strat_session_begin_multi, reading STRAT_PLAYERS/PROGRESS).
    # game_SaveGame is the ONE exception and is named below rather than smuggled in.
    if n == "game_SaveGame":
        return "Game/boot/misc"
    if (
        n.startswith("llm_strat_")
        or n.startswith("llm_unit_")
        or n.startswith("llm_bldg_")
        or n.startswith("llm_progress_")
        or n.startswith("game_")
    ):
        return "Strategic sim/economy"
    if (
        n.startswith("llm_game_")
        or n.startswith("llm_boot_")
        or n.startswith("llm_frame_")
        or n.startswith("llm_debug_")
        or n.startswith("llm_view_")
        or n.startswith("llm_cam_")
        or n.startswith("llm_chat_")
    ):
        return "Game/boot/misc"
    if n.startswith("llm_"):
        return "Game/boot/misc"
    if (
        n.startswith("utils_")
        or n.startswith("w_")
        or n.startswith("New_Name")
        or n.startswith("thunk_")
    ):
        return "Runtime/utility"
    return "Other named"


def classify_data(name):
    """Bucket a NAMED data global into the same subsystem taxonomy the code map uses,
    from its symbol name. Kept intentionally parallel to classify_fn() so the two coverage
    sections read against the same subsystems."""
    u = name.upper()
    if u.startswith("_G_LLM_STRAT_AI_") or "STRAT_AI" in u:
        return "Strategic AI"
    if u.startswith("_G_LLM_TACT_"):
        return "Tactical mode"
    if (
        u.startswith("_G_LLM_GFX_")
        or "_FX_" in u
        or "SPRITE" in u
        or "_BLIT" in u
        or "PALETTE" in u
    ):
        return "Graphics/FX"
    if u.startswith("_G_LLM_SND_") or "AUDIO" in u or "_CD_" in u or "VOICE" in u:
        return "Sound"
    if u.startswith("_G_LLM_NET_") or "LOBBY" in u or "_MP_" in u or "LOCKSTEP" in u:
        return "Netcode"
    if (
        u.startswith("_G_LLM_MAP_")
        or "_PF_" in u
        or "PATHTRACE" in u
        or "REGION" in u
        or "_DIR_" in u
        or "FACING" in u
        or "HEADING" in u
    ):
        return "Map/geometry"
    if (
        u.startswith("_G_LLM_UI_")
        or "_MENU_" in u
        or "TOOLTIP" in u
        or "WIDGET" in u
        or "_TUT" in u
        or "_DLG" in u
    ):
        return "UI/menus"
    if u.startswith("_G_LLM_INPUT_") or "MOUSE" in u or "_WND_" in u or "_KEY" in u:
        return "Input/window"
    if (
        u.startswith("_G_LLM_CFG_")
        or u.startswith("CFG")
        or "RESOURCE_DIV" in u
        or "ANIMNM" in u
        or "_DIVIDER" in u
    ):
        return "Config/formats"
    if (
        "_UNIT" in u
        or "_BLDG" in u
        or "BUILDING" in u
        or "STORAGE" in u
        or "PROJECT" in u
        or "UNITQ" in u
        or u.startswith("_G_LLM_STRAT_")
        or "_ROSTER" in u
    ):
        return "Strategic sim/economy"
    if u.startswith("_G_LLM_") or u.startswith("_G_") or u.startswith("G_"):
        return "Game/boot/misc"
    return "Other named"


# ── the architecture layer each subsystem belongs to (the reimplementation plan §5) ──────────────────
# Coarser than the subsystem taxonomy: the rules R1-R5 are stated over LAYERS, not subsystems,
# because "sim must not read presentation state" is a claim about roles, not about name prefixes.
LAYER_OF_SUBSYSTEM = {
    "Strategic sim/economy": "sim",
    "Strategic AI": "ai",
    "Tactical mode": "sim",
    "Netcode": "net",
    "UI/menus": "ui",
    "Input/window": "ui",
    "Graphics/FX": "view",
    "Sound": "view",
    "Map/geometry": "sim",
    "Config/formats": "data",
    "Game/boot/misc": "boot",
    "Runtime/utility": "boot",
    "Other named": "boot",
}

LAYERS = ["sim", "ai", "net", "ui", "view", "data", "boot"]


def layer_of(subsystem):
    return LAYER_OF_SUBSYSTEM.get(subsystem, "boot")


# ── architectural overrides, by NAME ───────────────────────────────────────────────────────────
# Subsystem and architectural layer answer DIFFERENT questions. "Which subsystem's coverage does
# this count toward" (classify_fn -> the codemap's rows) is not "which layer may it write"
# (layer_of_fn -> the architecture lint). llm_strat_ui_storage_bldg_panel is honestly strategic-mode
# code for coverage purposes, but architecturally it is UI: it reads presentation state, which is
# exactly what rule R3 forbids sim from doing. Overriding here rather than in classify_fn keeps the
# published per-subsystem coverage numbers stable while the lint gets the right answer.
#
# SEED LIST, not a finished one. P1-QUERIES is the item that adjudicates every
# reported violation; each adjudication that concludes "misfiled layer, not a real violation"
# belongs here, and each that concludes "genuinely wrong" stays a violation.
FN_LAYER_OVERRIDES = [
    ("llm_strat_ui_", "ui"),  # strategic-mode HUD/panel code
    ("llm_tact_ui_", "ui"),  # tactical-mode HUD/panel code
    # P1-QUERIES adjudication 2026-07-26. `SetEvent` @0x413a52 is NOT the Win32 API -- it is 1613
    # bytes of game code (not a thunk, not external) that reads/writes the strategic panel mode/page
    # /tab globals and is called by CreateBuilding, llm_debug_console_dispatch, llm_game_start_tutorial,
    # llm_map_view_size_mode_apply. It is a UI panel-state setter carrying a wrong, API-colliding
    # name; it accounted for 30 of the 69 apparent "sim reads presentation state" sites. Override is
    # a STOPGAP -- the fix is to rename it; drop this line then.
    ("SetEvent", "ui"),
    # The strategic DRAW family -- exactly two functions, both renderers by name and by body:
    # llm_strat_draw_floating_messages and llm_strat_draw_mine_resource_overlay. They classify as
    # sim on the llm_strat_ prefix, and the first is the SOLE READER of the on-screen message ring,
    # so without this it reads as "sim reads presentation state" while being the presentation.
    # Added 2026-08-28 alongside the ring's re-attribution.
    # NOT DONE HERE, and stated so it is a decision rather than an oversight: llm_strat_render_* has
    # the identical shape (sim.json already walls it as presentation) and is NOT overridden -- that
    # is a broader sweep than the message accounting needed, and each of its members should be
    # looked at rather than swept.
    ("llm_strat_draw_", "ui"),
]

# Presentation state that lives under a _G_LLM_STRAT_ name because it belongs to the strategic
# screen -- camera/selection/panel/input-echo. Read by render AND by UI, never legitimately by the
# deterministic sim: this is the "third state island" the reimplementation plan §5 calls out.
REGION_LAYER_OVERRIDES = [
    ("_G_LLM_STRAT_UI_", "ui"),
    ("_G_LLM_STRAT_CHAT_INPUT", "ui"),
    ("_G_LLM_STRAT_CTRL_GROUPS", "ui"),  # llm_strat_ctrl_group[10] -- 10 groups, NOT per-player
    ("_G_LLM_STRAT_MINIMAP_", "ui"),
    # P1-QUERIES adjudication 2026-07-26 (the state-flow notes). These carry a _G_LLM_STRAT_/_G_LLM_TACT_
    # prefix -- so classify_data files them as sim -- but their ONLY writers are llm_strat_ai_group_*
    # / llm_tact_ui_*, which is what makes them AI-island and presentation state respectively.
    ("_G_LLM_STRAT_GROUP_", "ai"),  # AI group order/route/centroid/member scratch
    ("_G_LLM_STRAT_PATH_JOB_", "ai"),  # AI pathfind job result table
    ("_G_LLM_TACT_UI_", "ui"),
    ("_G_LLM_TACT_SEL_PANEL_", "ui"),
    ("_G_LLM_TACT_SIDEBAR_", "ui"),
    # The on-screen MESSAGE RING, re-attributed to UI/menus in region_ownership.json on 2026-08-28.
    # The override is needed because the two answers to "what layer is this region" come from
    # different places: region_ownership.json is the SB2 human declaration (D2, one declared owner
    # per region) and is what the census and gen_writer_attribution read, while lint_arch reads the
    # layer the state matrix stamped, which is classify_data on the SYMBOL NAME. These carry
    # _G_LLM_STRAT_ / bare MESSAGE_ names, so without an entry here the declaration lands in one
    # place and not the other. The ring is presentation: game_ui_AddTextToPrintQueue shifts it and
    # llm_strat_draw_floating_messages renders it; every other writer POSTS a message rather than
    # owning the buffer. See region_ownership.json for the per-region evidence.
    # NOTE the two BARE names: the ring and its timestamp carry no _G_LLM_ prefix in either the
    # registry or the state matrix (they are `MESSAGE_QUEUE` / `MESSAGE_TIME`), which is also why
    # classify_data files them under Game/boot/misc rather than sim -- a third answer again. Matched
    # literally rather than by a `MESSAGE` prefix, which would be broad enough to catch unrelated
    # names. The _G_LLM_STRAT_FLOATING_MSG_ prefix additionally covers _SUPPRESS_FLAG, which is in
    # the state matrix but NOT in the region registry -- same family, same verdict, and covering it
    # here means the layering is right if it is ever registered.
    ("_G_LLM_STRAT_FLOATING_MSG_", "ui"),
    ("MESSAGE_TIME", "ui"),
    ("MESSAGE_QUEUE", "ui"),
]
# DELIBERATELY NOT overridden, though they showed up as violations -- the evidence was ambiguous and
# an override would bury the question rather than answer it (the state-flow notes "Unresolved"):
#   _G_LLM_STRAT_DIR_STEP_OFFSET_TABLE -- reads like shared const geometry, yet AI group code WRITES
#     it (via a MOVS-resolved LEA). Const table or per-call scratch copy? Undetermined.


# ── the region-OWNERSHIP vocabulary (RI-STATE / SB2) ──────────────────────────────────────────
# The values tools/data/region_ownership.json may declare as a region's `owner_subsystem`. It is
# SUBSYSTEMS minus "Other named" -- that bucket exists for FUNCTIONS whose symbol is unnamed or
# off-convention, i.e. a naming gap, and a naming gap is never an owner -- plus one sentinel.
#
# UNATTRIBUTED is the honest escape, and it is NARROW on purpose: gen_state_registry's rule 8
# accepts it only for a region the state matrix never saw touched at all (no writer, no reader,
# ours or original). "Nothing measured it" is a fact worth declaring; "I could not be bothered"
# is not, and every other region has evidence to attribute it with.
UNATTRIBUTED = "unattributed"
OWNER_SUBSYSTEMS = [s for s in SUBSYSTEMS if s != "Other named"] + [UNATTRIBUTED]


def layer_of_fn(name):
    """Architectural layer of a FUNCTION (overrides first, then its subsystem's layer)."""
    for prefix, layer in FN_LAYER_OVERRIDES:
        if name.startswith(prefix):
            return layer
    return layer_of(classify_fn(name))


def layer_of_region(name, subsystem=None):
    """Architectural layer of a STATE REGION (overrides first, then its subsystem's layer)."""
    for prefix, layer in REGION_LAYER_OVERRIDES:
        if name.startswith(prefix):
            return layer
    return layer_of(subsystem if subsystem is not None else classify_data(name))


if __name__ == "__main__":
    # Smoke test: the classifiers must be total (never raise, always return a known bucket) and
    # every subsystem must map to a known layer.
    probes = [
        ("llm_strat_ai_player_tick", "Strategic AI"),
        ("llm_strat_sim_step", "Strategic sim/economy"),
        ("llm_tact_render_view", "Tactical mode"),
        ("llm_net_lockstep_pump", "Netcode"),
        ("llm_ui_bldg_panel_open", "UI/menus"),
        ("llm_gfx_blit_sprite_rle", "Graphics/FX"),
        # THE FLATTENED FAMILIES. Every one of these was written "cfg::Init" / "map::ReadMap" /
        # "rsr::…" / "game::…" / "utils::…" until 2026-08-05 flattened Ghidra's namespaces into `_`
        # (docs/conventions.md#symbol-naming). classify_fn kept testing the `::` forms, and this selftest kept PROBING them,
        # so it went on passing while 129 real functions -- all 56 cfg_*, 17 map_*, 5 rsr_*, 22
        # game_*, 29 utils_* -- fell through every rule into "Other named". Nothing reported it:
        # "Other named" is the naming-gap bucket, so a misclassification looks exactly like the
        # condition the bucket exists to represent. Found 2026-08-28 while scoping SIM-RESID, where
        # it had put map_FillDefaults and the whole cfg parser in with the unnamed residue.
        # The REAL-NAME GUARD below is what stops it recurring: a probe naming a symbol the binary
        # does not have is now a failure, so a test cannot be green over an extinct form again.
        ("cfg_Init", "Config/formats"),
        ("rsr_ReadRsrFile", "Config/formats"),
        ("map_ReadMap", "Map/geometry"),
        ("map_FillDefaults", "Map/geometry"),
        ("utils_malloc", "Runtime/utility"),
        ("game_HandleProgress", "Strategic sim/economy"),
        ("game_SetEvent", "Strategic sim/economy"),
        ("game_SaveGame", "Game/boot/misc"),
        # ... and game_ui_ is presentation, ahead of the game_ rule: sim.json walls it by name and
        # sim_effect_classes.json classes both game_ui_* targets `effectful`.
        ("game_ui_PrintTextMessage", "UI/menus"),
        ("FUN_00401000", "Other named"),
    ]
    bad = [(n, classify_fn(n), want) for n, want in probes if classify_fn(n) != want]
    assert not bad, bad

    # THE REAL-NAME GUARD. Every probe above that is not a deliberate synthetic must name a function
    # the committed extent table actually carries. This is the arm that would have caught the `::`
    # regression the day the flattening landed.
    import json as _json
    import os as _os

    _ext = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "data", "en_functions.json")
    if _os.path.isfile(_ext):
        with open(_ext, encoding="utf-8-sig") as _fh:
            _real = {f["name"] for f in _json.load(_fh)["functions"]}
        _synthetic = {"FUN_00401000"}
        _extinct = sorted(n for n, _ in probes if n not in _real and n not in _synthetic)
        assert not _extinct, (
            "classify_fn selftest probes name(s) the binary does not have: %s -- a probe over an "
            "extinct symbol form passes without testing anything (the 2026-08-05 `::` flattening "
            "did exactly this for 129 functions). Re-key the probe to a committed name."
            % ", ".join(_extinct)
        )
    dprobes = [
        ("_G_LLM_STRAT_ORDER_QUEUE", "Strategic sim/economy"),
        ("_G_LLM_NET_SEND_BUF", "Netcode"),
        ("_G_LLM_TACT_UNITS", "Tactical mode"),
    ]
    dbad = [(n, classify_data(n), want) for n, want in dprobes if classify_data(n) != want]
    assert not dbad, dbad
    assert set(SUBSYSTEMS) == set(LAYER_OF_SUBSYSTEM), "SUBSYSTEMS and LAYER_OF_SUBSYSTEM disagree"
    assert set(LAYER_OF_SUBSYSTEM.values()) <= set(LAYERS)
    # architectural overrides must actually override, and must fall through when they do not apply
    assert layer_of_fn("llm_strat_ui_storage_bldg_panel") == "ui"
    assert layer_of_fn("llm_strat_sim_step") == "sim"
    assert layer_of_fn("llm_strat_draw_floating_messages") == "ui"
    assert layer_of_fn("llm_strat_render_tile_object") == "sim"  # deliberately NOT overridden
    assert layer_of_fn("llm_tact_ui_order_buttons_minimap_tick") == "ui"
    assert layer_of_fn("llm_tact_frame") == "sim"
    assert layer_of_region("_G_LLM_STRAT_UI_PANEL_PAGE") == "ui"
    assert layer_of_region("_G_LLM_STRAT_ORDER_QUEUE") == "sim"
    # The message ring: the override must beat the _G_LLM_STRAT_ prefix, and must NOT swallow the
    # neighbouring sim flags that also carry MSG in their names.
    assert layer_of_region("_G_LLM_STRAT_FLOATING_MSG_QUEUE_ACTIVE") == "ui"
    assert layer_of_region("MESSAGE_QUEUE") == "ui"
    assert layer_of_region("MESSAGE_TIME") == "ui"
    assert layer_of_region("_G_LLM_STRAT_FLOATING_MSG_SUPPRESS_FLAG") == "ui"
    assert layer_of_region("_G_LLM_STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG") == "sim"
    assert layer_of_region("_G_LLM_STRAT_UPGRADE_MSG_TRAILER") == "sim"
    assert set(l for _, l in FN_LAYER_OVERRIDES + REGION_LAYER_OVERRIDES) <= set(LAYERS)
    # the ownership vocabulary: every real subsystem except the naming-gap bucket, plus the sentinel
    assert "Other named" not in OWNER_SUBSYSTEMS and UNATTRIBUTED in OWNER_SUBSYSTEMS
    assert set(OWNER_SUBSYSTEMS) - {UNATTRIBUTED} < set(SUBSYSTEMS)
    print(
        "_subsys OK -- %d subsystems (%d valid region owners), %d layers, %d fn + %d region overrides"
        % (
            len(SUBSYSTEMS),
            len(OWNER_SUBSYSTEMS),
            len(LAYERS),
            len(FN_LAYER_OVERRIDES),
            len(REGION_LAYER_OVERRIDES),
        )
    )
