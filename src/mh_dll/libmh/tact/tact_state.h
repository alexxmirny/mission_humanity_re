//
// tact/tact_state.h -- the tactical mission's state interface (RI-TACT / TACT0).
//
// The tactical migration set is 99 functions (tools/data/tact_migration.json). This header is the
// ONE place that says where their state lives and which half of it they may write.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS NEITHER ai/ai_state.h NOR sim/sim_state.h WITH THE NAMES CHANGED
// ---------------------------------------------------------------------------------------------
//
// AI0's leverage was const-ness: the AI writes orders, not sim state, so `ai_view` is
// pointer-to-const throughout and the rule became a compile error. That does not transfer -- a
// tactical mission writes its own arena constantly.
//
// SIM0's W1-W3 DO transfer, and are inherited verbatim below, because tactical is in the same
// position the sim is: the legitimate writer of a large arena. What does NOT transfer is the thing
// TACT0 existed to answer, and the answer turned out to be a THIRD shape rather than either of the
// two the item anticipated.
//
//   THE ITEM ASKED: tactical writes tile_objects / passable / _G_LLM_STRAT_PATH_BUFFERS, which
//   docs/state-boundary.md D2 says the SIM owns. So either the store needs a cross-owner write
//   path, or it must REFUSE those regions and the refusal needs a negative test.
//
//   THE ANSWER IS NEITHER, because the premise was wrong. Measured 2026-08-24 (TACT-WRITERS,
//   TACT0): those planes are not the sim's. No ORIGINAL function writes them from both modes --
//   the writer sets are DISJOINT by mode -- and of the 787 functions reachable from both mode
//   roots, the 15 that touch a plane are ALL READS. They are the MAP's substrate with two
//   mutually-exclusive tenants, and `tile_objects` and `passable` -- two halves of one 256x256
//   grid, indexed identically -- had been given DIFFERENT owners, which was a mistake in the data
//   rather than a fact about the code. They are now `Map/geometry`, both modes are named tenants
//   in the cross-write ledger, and the planes are bound ONCE in state/mode_planes.h and handed to
//   both stores. Refusing them would have made 22 of the 99 migration members untranslatable.
//
// ---------------------------------------------------------------------------------------------
// THE RULES, IN THE ORDER THEY BITE
// ---------------------------------------------------------------------------------------------
//
//   W1  THE READ VIEW IS CONST, AND ITS MEMBERSHIP IS THE CLAIM. A region reachable ONLY through
//       `tact_view` is one the tactical set demonstrably never writes. That set is MEASURED, not
//       chosen: 18 regions have a tactical reader and no tactical writer, and the load-bearing
//       members are the STRATEGIC ones -- `units`, `Unit` (cfg), `_G_LLM_STRAT_PLAYERS`,
//       `G_PLANET_INDEX`. So "a tactical mission does not write strategic rosters" is a compile
//       error here, not a convention. (The two channels that DO cross the excursion -- the squad
//       blackboard and the G_PLANET_STATUS latch -- are separate regions and separate accessors;
//       see the tactical closure Sect. 9.)
//
//       A region can be in BOTH the view and the store: the view is the READ path. W1's claim is
//       about what is reachable ONLY through it, exactly as in sim_state.h.
//
//   W2  THE WRITE STORE HANDS OUT NO ADDRESS. Private pointers, accessors returning a reference to
//       ONE record. No base for a translation to cache, nothing to go stale across a rebase.
//
//   W3  THE STORE CANNOT BE CONSTRUCTED OUTSIDE THIS MODULE. Private constructor, two friends --
//       `state()` (the region registry) and `tact_fixture` (the offline harness's heap buffers).
//       That list IS the rule; a third is a diff a reviewer sees.
//
// WHAT NONE OF IT PREVENTS, stated plainly because a guarantee oversold is worse than none: a
// `const_cast`, a `reinterpret_cast` from a literal VA, or a self-declared `mh::tact::tact_fixture`
// defeats every rule above. What W1-W3 buy is that the ACCIDENT is impossible and the deliberate
// bypass is one grep away. tools/check_sim_addresses.py is the other half and now covers `libmh/tact/`
// too: no TU here except tact_state.cpp may name `mh::state::ptr` or a game VA.
//
// AND ONE RULE THE COMPILER CANNOT CARRY, WHICH IS WHY IT IS WRITTEN DOWN INSTEAD OF FAKED. The
// real invariant on the shared planes is TEMPORAL: only one mode may write them at a time. A
// `mode_planes` handed to tactical code is indistinguishable, to a compiler, from one handed to
// sim code. The temporal half is therefore a RUNTIME guard with a red arm in
// `net_selftest.exe tacttest`, not a type.
//
// ---------------------------------------------------------------------------------------------
// INHERITED INVARIANTS
// ---------------------------------------------------------------------------------------------
//
// `const` MEANS "YOU MAY NOT WRITE THROUGH IT", NOT "THIS DOES NOT CHANGE". Nothing here copies a
// byte; a view that snapshotted contents would pass every test in an idle world and desync in a
// busy one.
//
// NOTHING CACHES AN ADDRESS. `state()` returns BY VALUE and re-resolves every pointer from the
// region registry on every call. `tacttest` rebases the tactical regions and asserts both halves
// follow -- a check that can only exist because there is nothing to go stale.
//
// THE FP MODEL IS MEASURED, NOT INHERITED FROM SIM0. Counted over all 99 members against EN v349:
// 365 x87 instructions, ZERO SSE floating-point, 30 functions doing float math. (The scan's first
// pass reported "9 SSE" -- every one of them `MOVSD.REP EDI,ESI`, the STRING move, whose mnemonic
// collides with the SSE2 scalar-double move. A 2001 Watcom binary has no SSE2; the 9 were a regex
// artifact and are recorded here so the next person does not re-derive the wrong number.) So the
// tactical bodies are x87 like the sim's, and every TU under libmh/tact/ is compiled
// `/arch:IA32 /fp:precise` -- set per file in BOTH mh.vcxproj and mh_nettest/mh_nettest.vcxproj,
// because a TU that is x87 in the DLL and SSE2 in the offline oracle would make `tacttest` verify a
// different function from the one the game runs.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/mode_planes.h"

namespace mh::tact {

// ---- the record types -------------------------------------------------------------------------
using tact_unit   = mh::game::mh_tact_unit_record;
using strat_unit  = mh::game::mh_map_object_unit;
using player_data = mh::game::mh_game_player_data;
// The RID_STRAT_PLAYERS record type. _G_LLM_STRAT_PLAYERS @0x00cff060 is 14848 B = 8 x 0x740 --
// llm_strat_player_profile[8], per the region registry's own annotation and
// llm_tact_mission_start's IMUL-by-0x740 indexing (its +0xcff064 read is exactly `.race` at +0x4).
// The view originally typed this region `player_data` (sizeof 0x288fc -- a DIFFERENT region at
// 0x00e6dec0); retyped 2026-08-26 when mission_start became the first member to read a field.
using player_profile = mh::game::mh_llm_strat_player_profile;
using cfg_unit       = mh::game::mh_cfg_final_struct_Unit;
// The mission-loaded unit CLASS table (llm_tact_character_type[16], stride 0x6c). Filled by
// the CHARACTER section of a POZ*.DAT and read for the whole excursion -- llm_tact_unit_spawn
// turns the squad blackboard's hp PERCENT into absolute hp through `.energy`, so it is an
// input to entry determinism, not presentation.
using character_type = mh::game::mh_llm_tact_character_type;
// llm_tact_fx_type[64] @ _G_LLM_TACT_FX_TYPE_TABLE (RID_TACT_FX_TYPE_TABLE, stride 0x4a).
// Mission-loaded gun/explosion definitions; llm_tact_unit_weapon_in_range is the migration's
// first reader (range_min/range_max only).
using fx_type          = mh::game::mh_llm_tact_fx_type;
using anim_frame_range = mh::game::mh_llm_tact_anim_frame_range;
using mode_planes      = mh::state::mode_planes;
// llm_tact_fx[1024] @ _G_LLM_TACT_FX_POOL (RID_TACT_FX_POOL, stride 0x52). Projectiles and
// animation effects; llm_tact_tile_rebuild_occupancy_layer (TACT1A batch A) is the migration's
// only reader.
using fx_entry = mh::game::mh_llm_tact_fx;
// llm_tact_teleport[66] @ _G_LLM_TACT_TELEPORT_TABLE. Slot 65 (TELEPORT_SCRATCH_SLOT below) is a
// SCRATCH entry llm_tact_teleport_cmdqueue_jump (TACT1B, "run 1/3" grown slice) stages a
// one-shot teleport request into before calling llm_tact_unit_teleport -- not one of the 65
// mission-defined zones llm_tact_teleport_zone_scan_tick scans, hence index 65 of a 66-entry table.
using teleport_zone = mh::game::mh_llm_tact_teleport;
// llm_dir24_delta[24] @ _G_LLM_TACT_DIR24_APPROACH_DELTA. Read-only (dx,dy) offset table indexed by
// a computed dir24 sector; llm_tact_calc_approach_dir24_to_tile_stamp is the migration's only reader.
using dir24_delta = mh::game::mh_llm_dir24_delta;
// llm_tact_door[16] @ _G_LLM_TACT_DOOR_TABLE (RID_TACT_DOOR_TABLE, stride 0x68). Mission-loaded
// door records; llm_tact_mission_load's DOOR section is the migration's first writer (its
// init-clear loop deliberately overruns by ONE record -- JLE 0x10, preserved literally).
using door_record = mh::game::mh_llm_tact_door;
// llm_vec2i[8] @ _G_LLM_TACT_DIR8_DELTA_TABLE. Read-only (dx,dy) offset table indexed by a random
// octant (0..7); llm_tact_unit_owner_tick's idle-wander re-roll (TACT1B) is the
// migration's only reader.
using dir8_delta = mh::game::mh_llm_vec2i;
// gfx_sprite_meta[22000] @ _G_LLM_SPRITE_META (RID_SPRITE_META, stride 0x18, OWN_ISLAND, no writer
// in this or the sim closure -- boot-loaded from .BNK files). Same type sim_state.h already binds as
// `sprite_meta_entry`; TACT1C's llm_tact_unit_get_muzzle_offset is the migration's first tactical
// reader (origin_x/y, mount1_x/y, mount2_x/y).
using sprite_meta = mh::game::mh_gfx_sprite_meta;

// llm_snd_cfg_entry[200] @_G_LLM_SND_CFG_TABLE (TACT1E, 2026-08-27): shared /sound config table,
// first bound here (no sim/ai equivalent).
using snd_cfg_entry = mh::game::mh_llm_snd_cfg_entry;

// The tactical roster is tact_unit_record[129] -- slot 0 is not used by the mission code, which
// walks 1..0x80 inclusive. Stated here because the bound is an off-by-one worth preserving:
// llm_tact_units_reset_hp_for_active's loop is `for (i = 1; i < 0x81; ++i)`.
inline constexpr int32_t TACT_UNIT_SLOTS      = 129;
inline constexpr int32_t TACT_UNIT_FIRST_SLOT = 1;
inline constexpr int32_t TACT_UNIT_LAST_SLOT  = 0x80;

// A tactical mission runs on the top-left 128x128 sub-block of the declared [256][256] grid, at the
// same 256-element row stride (llm_tact_mission_start sets width = height = 0x80).
inline constexpr int32_t TACT_MAP_DIM = 0x80;

// llm_tact_character_type[16] @ _G_LLM_TACT_CHARACTER_TYPES -- the region registry's own extent
// (RID_TACT_CHARACTER_TYPES, 1728 B / 0x6c = 16). Section 1 of a mission file indexes it
// DIRECTLY by the CHARACTER number, and llm_tact_mission_load rejects 0, so slot 0 is a real
// slot the parser never fills rather than one it skips.
inline constexpr int32_t TACT_CHARACTER_TYPE_SLOTS = 16;

// llm_tact_fx_type[64] @ _G_LLM_TACT_FX_TYPE_TABLE (RID_TACT_FX_TYPE_TABLE, 4,736 B / 0x4a).
inline constexpr int32_t TACT_FX_TYPE_SLOTS = 64;

// The FRAMES list a mission's CHARACTER block carries. The struct holds EIGHT ranges;
// llm_tact_character_parse_frame_table fills only the first SIX from the brace-delimited list
// and leaves 6 and 7 as loaded. Both numbers matter, so both are named.
inline constexpr int32_t TACT_ANIM_FRAME_SLOTS   = 8;
inline constexpr int32_t TACT_ANIM_FRAMES_PARSED = 6;

// llm_squad_status_slot[64] @ _G_LLM_SQUAD_STATUS (RID_SQUAD_STATUS, 1024 B / 0x10 = 64). Indexed
// directly by squad-slot number, 0..TACT_SQUAD_SIZE-1 (llm_tact_squad_sync_hp's own loop bound is
// the runtime global _G_LLM_TACT_SQUAD_SIZE, which is <= this).
inline constexpr int32_t TACT_SQUAD_STATUS_SLOTS = 64;

// llm_tact_fx[1024] @ _G_LLM_TACT_FX_POOL (RID_TACT_FX_POOL, 83,968 B / 0x52 = 1024).
inline constexpr int32_t TACT_FX_POOL_SLOTS = 1024;

// llm_tact_teleport[66] @ _G_LLM_TACT_TELEPORT_TABLE (RID_TACT_TELEPORT_TABLE, 3,300 B / 0x32 = 66).
// Index 65 -- the LAST entry, one past the 65 real zone slots -- is the scratch slot
// llm_tact_teleport_cmdqueue_jump stages a one-shot request into (its own body loads the literal
// 0x41 = 65 for both the write index and the teleport_id argument it passes on).
inline constexpr int32_t TACT_TELEPORT_ZONE_SLOTS   = 66;
inline constexpr int32_t TACT_TELEPORT_SCRATCH_SLOT = 65;

// llm_dir24_delta[24] @ _G_LLM_TACT_DIR24_APPROACH_DELTA -- one entry per 15-degree dir24 sector.
inline constexpr int32_t TACT_DIR24_SLOTS = 24;

// llm_tact_tile_rebuild_occupancy_layer's own unit-table scan bound: 0x40 (64), NOT
// TACT_UNIT_SLOTS (129) and NOT TACT_UNIT_FIRST_SLOT-based -- it walks _G_LLM_TACT_UNITS[0..63]
// (0x00433e21/0x00433e55: `MOV ECX,0x40`, base is the array's OWN pointer, not +1). Preserved
// literally: the other 65 unit slots are never scanned by this function.
inline constexpr int32_t TACT_OCCUPANCY_UNIT_SCAN_SLOTS = 64;

// ---- the READ view ----------------------------------------------------------------------------
//
// W1. Grown demand-driven: a region joins the first time a translated function reads it. A member
// added here for a region the tactical set WRITES is not a style slip -- it is a false claim, and
// the write belongs in tact_store where it becomes a greppable accessor call.

struct tact_view {
    // ---- tactical's own arena, READ path ----
    const tact_unit *units;      // _G_LLM_TACT_UNITS, RID_TACT_UNITS
    const int32_t   *map_width;  // _G_LLM_TACT_MAP_WIDTH_CACHE
    const int32_t   *map_height; // _G_LLM_TACT_MAP_HEIGHT_CACHE
    // The tactical HUD's latched cursor position. Written by llm_tact_sidebar_dispatch (a migration
    // member, so it is in the store too) and read by the four sidebar hit-testers.
    const int32_t *sidebar_mouse_x; // RID_TACT_SIDEBAR_MOUSE_X
    const int32_t *sidebar_mouse_y; // RID_TACT_SIDEBAR_MOUSE_Y
    // A POINTER VARIABLE (RID_TACT_MAP_TILE_HEIGHT_SPRITES, size 4): set once by
    // llm_tact_mission_start to the mission's tile-height-sprite table base. Read by
    // llm_tact_map_compute_bounds (TACT1A batch A); *map_tile_height_sprites is the live base,
    // indexed in uint16_t units.
    const uint16_t **map_tile_height_sprites;

    // The squad-assault blackboard's remaining read-only fields (see tact_store's own
    // squad_bb_scan_player comment for the two that are ALSO read here). All four are
    // RID_SQUAD_BB_*, MF_VIEW, no writer in this closure -- llm_strat_squad_assault_resolve
    // (TACT1A batch A) is the sole reader of all four.
    const int32_t *squad_bb_scan_player;         // RID_SQUAD_BB_SCAN_PLAYER
    const int32_t *squad_bb_target_owner;        // RID_SQUAD_BB_TARGET_OWNER
    const int32_t *squad_bb_target_building_idx; // RID_SQUAD_BB_TARGET_BUILDING_IDX
    const int32_t *squad_bb_target_energy_pct;   // RID_SQUAD_BB_TARGET_ENERGY_PCT

    // Three read-only scale doubles llm_strat_squad_assault_resolve FMULs/FDIVs against the
    // blackboard sum -- already named+typed in Ghidra, added to the C mirror 2026-08-26 (see the
    // dll_addr_manifest.json entries for the derivation). No writer found.
    const double *squad_assault_power_percent_scale; // RID_STRAT_SQUAD_ASSAULT_POWER_PERCENT_SCALE
    const double *bldg_energy_to_percent_scale;      // RID_STRAT_BLDG_ENERGY_TO_PERCENT_SCALE
    const double *bldg_energy_percent_to_abs_scale;  // RID_STRAT_BLDG_ENERGY_PERCENT_TO_ABS_SCALE
    // How many blackboard slots the mission load consumed (RID_TACT_SQUAD_SIZE). llm_tact_squad_sync_hp's
    // own loop bound.
    const int32_t *squad_size;
    // The global pathfinder slot id llm_tact_move_step_attempt (frontier) assigns for the CURRENT
    // call; -1 = no path found. llm_tact_group_issue_order (TACT1B) reads it right after
    // calling that frontier function to decide whether to preview-walk the path. RID_TACT_MOVE_PATH_SLOT_ID.
    const int32_t *move_path_slot_id;
    // llm_dir24_delta[24] @ _G_LLM_TACT_DIR24_APPROACH_DELTA, read-only.
    const dir24_delta *dir24_approach_delta;
    // llm_vec2i[8] @ _G_LLM_TACT_DIR8_DELTA_TABLE, read-only.
    const dir8_delta *dir8_delta_table;
    // TACT1D: the tactical FOV raycaster's three ray-delta tables, read-only .rdata
    // (RID_TACT_FOV_DELTA_TABLE_{72,120,360}; short[72]/[120]/[360] in Ghidra). One is selected by
    // vision_dist and indexed by ANGLE_INDEX*2; only the LOW TWO BYTES of each entry are ever
    // consumed, as the {int8 dx; int8 dy;} step pair -- hence int8_t* rather than short*.
    // Bound HERE rather than read at their literal addresses because net_selftest.exe is a separate
    // image, where those addresses resolve into the test binary's own .text.
    const int8_t *fov_delta_table_72;
    const int8_t *fov_delta_table_120;
    const int8_t *fov_delta_table_360;
    // double @ _G_LLM_TACT_UNIT_WANDER_RETRY_INTERVAL, read-only (value 2.0).
    const double *unit_wander_retry_interval;
    // The "current move being processed" cursor -- llm_tact_unit_weapons_tick (frontier) is the
    // sole writer (TACT-READY R8, 2026-08-24); llm_tact_unit_move_tick (TACT1B) reads
    // both to compute the world position of the path step it is about to consume.
    const int32_t *move_cur_col; // RID_TACT_MOVE_CUR_COL
    const int32_t *move_cur_row; // RID_TACT_MOVE_CUR_ROW
    // The flood-fill pathfinder's last result tile (region_ownership.json writers:
    // llm_tact_move_path_build, llm_tact_move_flood_reachable_tile, llm_tact_move_step_attempt --
    // all frontier, none of them migration members). llm_tact_unit_move_tick reads both right after
    // calling llm_tact_move_step_attempt to detect a "already there" no-op result.
    const uint8_t *move_flood_result_col; // RID_TACT_MOVE_FLOOD_RESULT_COL (Ghidra type byte)
    const uint8_t *move_flood_result_row; // RID_TACT_MOVE_FLOOD_RESULT_ROW (Ghidra type byte)
    // double @ _G_LLM_TACT_MINE_BLAST_TIME_END, read-only. llm_tact_unit_enqueue_command's op==9
    // gate refuses the enqueue while time_GetCurrentTime() is still below this deadline. Also in
    // tact_store below (llm_tact_unit_mine_arm_tick, TACT1C, is the sole WRITER -- it stamps the
    // deadline the enqueue gate above reads).
    const double *mine_blast_time_end; // RID_TACT_MINE_BLAST_TIME_END
    // double @ _G_LLM_TACT_MINE_BLAST_DURATION, read-only (mission-loaded; llm_tact_mission_load's
    // write path is not yet translated). llm_tact_unit_mine_arm_tick (TACT1C) is the migration's
    // only reader: mine_blast_time_end = time_GetCurrentTime() + this.
    const double *mine_blast_duration; // RID_TACT_MINE_BLAST_DURATION

    // int @ _G_LLM_VIEW_SIZE_MODE, read-only here (RID_VIEW_SIZE_MODE, OWN_SHARED -- the options-menu
    // resolution widget's own region). llm_tact_mission_end_return_to_strategic (TACT1A) branches on
    // it to decide whether to reinit the tactical view-tile-row cache (mode 0) or reapply the size
    // mode (nonzero); it never writes it.
    const int32_t *view_size_mode; // RID_VIEW_SIZE_MODE
    // double @ LAST_GAME_TIME, read-only here (RID_LAST_GAME_TIME, OWN_ISLAND -- llm_strat_time_tick's
    // own cross-frame wall-clock stamp). llm_tact_mission_end_return_to_strategic passes it straight
    // through to llm_snd_ambient_reseed_planet_event_times's current_time argument; never written here.
    const double *last_game_time; // RID_LAST_GAME_TIME

    // llm_tact_move_path_build's own read of its (previously store-only) request pair -- see
    // tact_store's own comment on the same four fields: nothing else translated reads them back,
    // but this function now does, so the "store-only" claim in the store's comment is no longer
    // literally true for these specific four (kept there anyway since that is still the WRITE path).
    const uint8_t *move_flood_start_col; // RID_TACT_MOVE_FLOOD_START_COL
    const uint8_t *move_flood_start_row; // RID_TACT_MOVE_FLOOD_START_ROW
    const uint8_t *move_flood_goal_col;  // RID_TACT_MOVE_FLOOD_GOAL_COL
    const uint8_t *move_flood_goal_row;  // RID_TACT_MOVE_FLOOD_GOAL_ROW

    // The SHARED (both-mode) map extent, RID_WIDTH/RID_HEIGHT -- "D6: ... in tiles ... TORUS" per
    // addr/mh_addrs.gen.h. NOT the same region as tact_store::map_width/map_height above (that pair
    // is RID_TACT_MAP_WIDTH_CACHE/RID_TACT_MAP_HEIGHT_CACHE, a tactical-only camera-bounds cache with
    // an unrelated meaning). llm_tact_mission_start (TACT1A, proven OFFLINE -- see
    // tact_mission_start.h) sets these to 0x80 for the excursion; llm_tact_move_path_build
    // (TACT1A/B) is the migration's first reader.
    const int32_t *grid_width;  // RID_WIDTH
    const int32_t *grid_height; // RID_HEIGHT

    // ---- W1's load-bearing members: STRATEGIC state a mission reads and must never write -------
    //
    // These four are why W1 is a real statement about this subsystem rather than a restatement of
    // "tactical writes things". Measured: a tactical reader, no tactical writer, in EVERY case.
    const strat_unit     *strat_units;   // RID_UNITS -- read by the squad-status gather path
    const cfg_unit       *cfg_units;     // RID_UNIT  -- the unit TYPE table, frozen after boot
    const player_profile *strat_players; // RID_STRAT_PLAYERS -- see the `player_profile` using above
    const int32_t        *planet_index;  // RID_G_PLANET_INDEX

    // The mission's unit CLASS table. In BOTH halves: the mission parsers WRITE it (so it is in
    // the store), and every consumer after load only reads it. W1's claim is about what is
    // reachable ONLY through the view, so a member here is not a contradiction.
    const character_type *character_types; // RID_TACT_CHARACTER_TYPES

    // llm_tact_fx_type[64] @ _G_LLM_TACT_FX_TYPE_TABLE (RID_TACT_FX_TYPE_TABLE, stride 0x4a).
    // Mission-loaded gun/explosion definitions (range_min/range_max, sound, damage power, ...),
    // constant during a mission. llm_tact_unit_weapon_in_range (TACT1C) is the migration's first
    // reader. No tactical function writes it.
    const fx_type *fx_type_table; // RID_TACT_FX_TYPE_TABLE

    // ---- the shared planes, READ path (the mutable half is tact_store::planes()) ---------------
    const mh::state::tile_object *tile_objects; // RID_TILE_OBJECTS
    const uint8_t                *passable;     // RID_PASSABLE

    // llm_tact_tile_rebuild_occupancy_layer's own read (TACT1A batch A). No tactical function
    // writes it.
    const fx_entry *fx_pool; // RID_TACT_FX_POOL

    // ---- TACT1A/TACT1C central batch (2026-08-26) ----------------------------------------------
    // int @ CurrentSystem -- the strategic star-system index; llm_tact_mission_start builds the
    // "poz%do.dat"/"poz%dl.dat" mission filenames from it. Never written here.
    const int32_t *current_system; // RID_CURRENTSYSTEM
    // A POINTER VARIABLE (RID_GFX_DRAW_SURFACE, 4 B): the renderer's active draw surface.
    // llm_tact_mission_start reads it while (re)initializing the tactical view; same view shape as
    // map_tile_height_sprites (deref gives the live base, read-only pointee).
    const uint8_t **gfx_draw_surface; // RID_GFX_DRAW_SURFACE
    // Keystate bytes (bit 0 = held). llm_tact_unit_update_anim TESTs both (@0x0042c5c2/0x0042c5cb)
    // to speed up the walk animation while a shift is held. Written by the input layer only.
    const uint8_t *key_rshift_held; // RID_KEY_RSHIFT_HELD
    const uint8_t *key_lshift_held; // RID_KEY_LSHIFT_HELD
    // Four read-only anim-timing doubles (two families: step interval + jitter scale), consumed by
    // llm_tact_unit_update_anim. No writer measured.
    const double *unit_anim_step_sec_1;     // RID_TACT_UNIT_ANIM_STEP_SEC_1
    const double *unit_anim_jitter_scale_1; // RID_TACT_UNIT_ANIM_JITTER_SCALE_1
    const double *unit_anim_step_sec_2;     // RID_TACT_UNIT_ANIM_STEP_SEC_2
    const double *unit_anim_jitter_scale_2; // RID_TACT_UNIT_ANIM_JITTER_SCALE_2
    // double base added to a spawned unit's frame_interval after llm_rand()/0x1999
    // (llm_tact_unit_spawn is the reader).
    const double *unit_anim_frame_interval_base; // RID_TACT_UNIT_ANIM_FRAME_INTERVAL_BASE
    // int mission flag: mines enabled for this mission; llm_tact_unit_weapons_tick's op-9 gate.
    const int32_t *mines_enabled; // RID_TACT_MINES_ENABLED

    // ---- mission_load's read-only inputs (2026-08-26) ------------------------------------------
    // The WHO-field XOR mask the CHARACTER parse path applies (runtime-set; image value 0).
    const int32_t *who_xor_key; // RID_TACT_WHO_XOR_KEY
    // Three read-only .rdata doubles (all 32.0, byte-verified) sitting inline after their keyword
    // strings: the COLISION1/COLISION2 defaults and the teleport/death coordinate bias.
    const double *colision1_bias;      // RID_TACT_COLISION1_BIAS
    const double *colision2_bias;      // RID_TACT_COLISION2_BIAS
    const double *teleport_death_bias; // RID_TACT_TELEPORT_DEATH_BIAS
    // double, read-only, 2.0 exactly (TACT1D). llm_tact_door_tick's state==2 (open/holding) arm adds
    // this to opened_at_time to decide when a fully-open door starts closing.
    const double *door_open_hold_time_sec; // RID_TACT_DOOR_OPEN_HOLD_TIME_SEC

    // ---- TACT1C batch (2026-08-26): unit_fire_weapon / fx_splash_damage / unit_get_muzzle_offset -
    // The last-hovered unit id the sidebar input phase latches (RID_TACT_HOVERED_UNIT_ID); already
    // in the view's WRITER list above at a different member (llm_tact_render_view/llm_tact_frame,
    // neither a migration member) -- this is a second, read-only binding for
    // llm_tact_unit_fire_weapon's own-vs-hovered-unit facing-side check.
    const int32_t *hovered_unit_id; // RID_TACT_HOVERED_UNIT_ID
    // gfx_sprite_meta[22000], read-only. llm_tact_unit_get_muzzle_offset indexes it by sprite_id to
    // find the weapon-mount anchor offsets.
    const sprite_meta *sprite_meta_table; // RID_SPRITE_META
    // int, read-only, value 0x0, NO WRITER anywhere in the binary (see the addr manifest entry's
    // derivation). llm_tact_fx_splash_damage's sole reader; gates an owner-vs-owner damage-skip arm
    // that is dead in every observed build, reproduced literally rather than folded away.
    const int32_t *fx_splash_spare_nonzero_owner; // RID_TACT_FX_SPLASH_SPARE_NONZERO_OWNER

    // ---- TACT1D batch (2026-08-27): the FOV raycaster's sprite-bank lookup + door table ----------
    // Same two-region pair sim_view already binds (sim_state.h): the sprite pixel bank's base
    // pointer (a POINTER VARIABLE, hence the double-const) and the per-frame byte-offset table into
    // it. llm_tact_door_update_tile_state/llm_tact_door_apply_to_map read a door's animation-frame
    // pixel byte through `*gfx_bank_pixels + sprite_pix_offsets[bank_sprite_base_at(20) + frame] + 1`
    // to decide the next frame index (0xff terminator). No tactical writer.
    const uint8_t *const *gfx_bank_pixels;    // RID_GFX_BANK_PIXELS
    const uint32_t       *sprite_pix_offsets; // RID_SPRITE_PIX_OFFSETS
    // int, DEAD READ -- ZERO writers anywhere in /eng/mh.exe (2026-08-27 finding, ghidra_findings.json
    // id 2026-08-27-0017-1; plate at 0x00437138). llm_tact_fov_update_nearest_target's sole reader
    // compares this against its two nearest-target trackers as if it were "the current ray distance",
    // but nothing ever computes or stores one here -- PRESERVE the literal read, never synthesize a
    // real distance.
    const int32_t *fov_candidate_dist; // RID_TACT_FOV_CANDIDATE_DIST

    // ---- ambient sound (llm_tact_ambient_sound_tick, TACT1E 2026-08-27) --------------------------
    // Bound by RAW LITERAL ADDRESS in state() (tact_fov.cpp's DELTA_TABLE precedent), not a RID: all
    // six are read-only shared /sound subsystem globals this is the FIRST tactical translation to
    // touch, and none of them need write-region/shadow tracking. Read-only global config toggles.
    const int32_t *snd_enabled;       // _G_LLM_SND_ENABLED @0x00ae2ab0
    const int32_t *snd_master_volume; // _G_LLM_SND_MASTER_VOLUME @0x00ae2ab4
    // llm_snd_cfg_entry[200] base (_G_LLM_SND_CFG_TABLE @0x0070d5c0). Indexed with a DELIBERATELY
    // negative index at one call site -- see tact_ambient_sound.cpp's header banner (G28 stack-probe
    // watermark derivation) -- so this is a raw pointer, not a bounds-checked container.
    const snd_cfg_entry *snd_cfg_table;
    const double        *ambient_snd_min_interval_sec; // _G_LLM_TACT_AMBIENT_SND_MIN_INTERVAL_SEC @0x0050046a
    const int32_t       *ambient_snd_zone_count;       // _G_LLM_TACT_AMBIENT_SND_ZONE_COUNT @0x00558cac
    const int32_t       *ambient_snd_zone_table;       // _G_LLM_TACT_AMBIENT_SND_ZONE_TABLE[10] @0x00558cb0

    // ---- TACT1E batch (2026-08-27): the frame pump + selection/sidebar/panel residue -------------
    // llm_tact_ui_sel_panel_init's own loop bound + the icon-name table it formats into a sprite
    // filename (llm_tact_panel_icon_name[41], stride 0xa -- not yet in mh_structs.gen.h, so this is a
    // raw byte base like snd_cfg_table above; index with `*10`). No writer identified (mission/UI
    // init data, not migrated).
    const int32_t *sel_panel_icon_count; // RID_TACT_SEL_PANEL_ICON_COUNT
    const uint8_t *sel_panel_icon_names; // RID_TACT_SEL_PANEL_ICON_NAMES (byte base, stride 0xa)
    // llm_tact_cam_follow_selection_tick's own camera-vs-follow-target delta. No writer in this
    // closure (mission-load-adjacent, not migrated).
    const int32_t *cam_follow_target_col; // RID_TACT_CAM_FOLLOW_TARGET_COL
    const int32_t *cam_follow_target_row; // RID_TACT_CAM_FOLLOW_TARGET_ROW
    // The OS cursor position + current mouse-button mask (OWN_SHARED, read by the input layer and
    // every mode's click/hover logic). llm_tact_sidebar_dispatch/llm_tact_frame read-only here; the
    // write path is the input pump (not migrated).
    const int32_t *cursor_x;          // RID_CURSOR_X
    const int32_t *cursor_y;          // RID_CURSOR_Y
    const uint8_t *mouse_buttons_cur; // RID_MOUSE_BUTTONS_CUR
    // The strategic-viewport width (WindowWidth minus the side panel), OWN_ISLAND. llm_tact_frame
    // reads it once per frame for its click-vs-sidebar horizontal split; no tactical writer.
    const int32_t *win_w; // RID_G_WIN_W
    // byte[40] LUT read by llm_tact_sidebar_dispatch immediately preceding
    // _G_LLM_TACT_UNASSIGNED_UNIT_ROSTER at +0xb0; the roster itself (llm_tact_unit_roster_slot, not
    // yet in mh_structs.gen.h). Both raw byte bases, unchecked like snd_cfg_table above -- the
    // accessor removes the address, not the original's own indexing.
    const uint8_t *sidebar_icon_slot_unit_lut; // RID_TACT_SIDEBAR_ICON_SLOT_UNIT_LUT
    const uint8_t *unassigned_unit_roster;     // RID_TACT_UNASSIGNED_UNIT_ROSTER
    // llm_tact_unit_roster_slot[8] (not yet in mh_structs.gen.h) -- raw byte base, same unchecked
    // shape as unassigned_unit_roster/sidebar_icon_slot_unit_lut above.
    const uint8_t *group_unit_roster; // RID_TACT_GROUP_UNIT_ROSTER
    // The `pitch` argument llm_ui_set_draw_surface takes alongside gfx_draw_surface -- a
    // Graphics/FX-owned view-metrics global, read-only here (same family as gfx_draw_surface).
    const int32_t *gfx_panel_row_skip; // RID_GFX_PANEL_ROW_SKIP
    // The shared localized-string table (same shape/region sim_view already binds).
    const wchar_t *const *text_ptrs; // RID_G_TEXT_PTRS

    // ---- TACT1E batch (2026-08-28): the remaining frame-pump/sidebar residue --------
    // Keystate bytes (Input/window-owned, OWN_SHARED); llm_tact_frame/llm_tact_selection_clear_unless_ctrl
    // read them, the input pump (not migrated) writes them.
    const uint8_t *key_lctrl_held; // RID_KEY_LCTRL_HELD
    const uint8_t *key_lalt_held;  // RID_KEY_LALT_HELD
    // llm_tact_ui_order_buttons_minimap_tick's minimap hit-test anchor. No writer identified
    // (mission/UI init data, not migrated).
    const int32_t *tact_ui_minimap_origin_x; // RID_TACT_UI_MINIMAP_ORIGIN_X
    const int32_t *tact_ui_minimap_origin_y; // RID_TACT_UI_MINIMAP_ORIGIN_Y
    // The sidebar's visible-row/visible-slot bounds -- llm_tact_squad_roster_refresh/
    // llm_tact_ui_sel_panel_single_mode_tick/llm_tact_ui_sel_panel_multi_mode_tick all read these as
    // scroll-row bounds. No writer identified.
    const int32_t *sidebar_multi_panel_visible_rows; // RID_TACT_SIDEBAR_MULTI_PANEL_VISIBLE_ROWS
    const int32_t *sidebar_slot_visible_count;       // RID_TACT_SIDEBAR_SLOT_VISIBLE_COUNT
    // The sidebar roster row's two format-string literals (byte-verified L"%2d" / L"%s"), read by
    // llm_tact_ui_sidebar_row_draw_right/_left. No writer -- read-only data.
    const wchar_t *sidebar_fmt_unit_id;   // RID_TACT_SIDEBAR_FMT_UNIT_ID
    const wchar_t *sidebar_fmt_unit_name; // RID_TACT_SIDEBAR_FMT_UNIT_NAME
};

// ---- the WRITE store --------------------------------------------------------------------------

struct tact_state;
tact_state state();

// The offline harness's heap-buffer binder (mh_nettest/tact_selftest.cpp). Named here rather than
// left to a cast because W3 is only meaningful if the sanctioned exceptions are enumerated.
struct tact_fixture;

class tact_store {
public:
    tact_store(const tact_store &)            = default;
    tact_store &operator=(const tact_store &) = default;

    // ---- tactical's own arena ----------------------------------------------------------------
    //
    // Unchecked, like every accessor in sim_store: the accessor exists to remove the ADDRESS, not
    // to add a bound the original does not have.
    tact_unit &unit_at(int32_t index) { return units_[index]; }

    int32_t &sidebar_mouse_x() { return *sidebar_mouse_x_; }
    int32_t &sidebar_mouse_y() { return *sidebar_mouse_y_; }

    // llm_tact_map_compute_bounds (TACT1A batch A) is the write path: it re-derives these two as a
    // running max over the tile-height-sprite table and never resets them itself (the reset is
    // llm_tact_map_reset's job). Added to the store 2026-08-26 -- previously read-only via tact_view.
    int32_t &map_width() { return *map_width_; }
    int32_t &map_height() { return *map_height_; }

    // A SECOND, mutable binding of RID_TACT_MAP_TILE_HEIGHT_SPRITES's pointee -- same bytes as
    // tact_view::map_tile_height_sprites, same shape as move_path_slot_id's view/store pair.
    // llm_tact_map_reset (TACT1A batch A) is the only migration member that zeroes cells behind
    // the pointer; it never changes the pointer VARIABLE itself, so this returns the already-set
    // base directly rather than a reference-to-pointer.
    uint16_t *map_tile_height_sprites() { return *map_tile_height_sprites_; }

    // llm_tact_unit_move_tick (TACT1B) is a writer too (region_ownership.json,
    // alongside the frontier llm_tact_move_step_attempt/llm_tact_move_path_build) -- it forces the
    // sentinel to -1 when a fresh flood result equals the unit's current tile (no real move found).
    // group_issue_order's READ of the same region stays on tact_view::move_path_slot_id -- same
    // bytes, two bindings, exactly like the shared planes.
    int32_t &move_path_slot_id() { return *move_path_slot_id_; }

    // The squad-status blackboard (RID_SQUAD_STATUS, typed 2026-08-26): the one channel that
    // survives a tactical excursion. llm_tact_squad_sync_hp is the per-frame writer of
    // `.energy_pct`; the two strat-named accessors that fill/consume the other fields are the
    // mode boundary, not tactical's writers.
    mh::game::mh_llm_squad_status_slot &squad_status_at(int32_t index) {
        return squad_status_[index];
    }

    // llm_tact_teleport_cmdqueue_jump's (TACT1B) write path -- unchecked, like unit_at: the original
    // indexes this with the literal scratch-slot constant above, and the accessor exists to remove
    // the address, not to add a bound the original lacks.
    teleport_zone &teleport_zone_at(int32_t index) { return teleport_zones_[index]; }

    // llm_tact_move_step_attempt's (TACT1B) write path into the flood-fill pathfinder's scratch
    // START/GOAL pair -- previously frontier-owned (llm_tact_move_path_build/
    // llm_tact_move_flood_reachable_tile/llm_tact_move_step_attempt, none translated), now this
    // function's own writes. The WRITE path stays here (nothing else translated writes them); the
    // READ path is tact_view::move_flood_start_col/row / move_flood_goal_col/row above --
    // llm_tact_move_path_build (TACT1A/B) is the migration's first reader.
    uint8_t &move_flood_start_col() { return *move_flood_start_col_; }
    uint8_t &move_flood_start_row() { return *move_flood_start_row_; }
    uint8_t &move_flood_goal_col() { return *move_flood_goal_col_; }
    uint8_t &move_flood_goal_row() { return *move_flood_goal_row_; }
    // The flood-fill pathfinder's result tile -- a writer here now too (previously read-only via
    // tact_view::move_flood_result_col/row, same bytes, two bindings, same shape as
    // move_path_slot_id above).
    uint8_t &move_flood_result_col() { return *move_flood_result_col_; }
    uint8_t &move_flood_result_row() { return *move_flood_result_row_; }

    // llm_tact_move_path_build's OWN private solver scratch (the shadow manifest's
    // 19-region closure minus the 4 above and PATH_BUFFERS/PASSABLE which route through planes()/
    // move_flood_result_* already) -- read AND written within that one function, never by anything
    // else in the closure (shadow_region_closure.py: "1 function reachable (itself)"). One reference
    // accessor per region, same shape as occupancy_rebuild_map_id below, because both halves of each
    // read-modify-write live in the same function.
    uint32_t &move_path_coord_mask() { return *move_path_coord_mask_; }
    uint32_t &move_path_goal_packed() { return *move_path_goal_packed_; }
    uint32_t &move_path_start_packed() { return *move_path_start_packed_; }
    uint32_t &move_path_tile_cost() { return *move_path_tile_cost_; }
    uint32_t &move_path_trace_tile() { return *move_path_trace_tile_; }
    uint32_t &move_path_trace_base() { return *move_path_trace_base_; }
    uint32_t &move_path_trace_diag_cand() { return *move_path_trace_diag_cand_; }
    uint8_t  &move_path_trace_best_dir() { return *move_path_trace_best_dir_; }
    uint32_t &move_path_trace_best_tile() { return *move_path_trace_best_tile_; }
    uint32_t &move_path_trace_cost_sum() { return *move_path_trace_cost_sum_; }
    uint32_t &move_path_rle_count() { return *move_path_rle_count_; }
    // The "which buffer is the current wave" pointer variable -- holds the real VA of whichever of
    // move_path_queue_a()/move_path_queue_b() is live, exactly as the original stores it (both
    // buffers are real process memory, so a genuine address round-trips byte-identically under
    // shadow). uint32_t, not a typed pointer: the original's own slot is 4 raw bytes with no type.
    uint32_t &move_path_queue_cur_addr() { return *move_path_queue_cur_addr_; }
    // The two double-buffered BFS wave arrays (_G_LLM_TACT_MOVE_PATH_QUEUE_A/B, 8192 B / 4096
    // uint16_t packed-tile slots each). Raw base pointers, like tact_view's array members --
    // move_path_build indexes them by a byte/word offset it computes itself, same as the original.
    uint16_t *move_path_queue_a() { return move_path_queue_a_; }
    uint16_t *move_path_queue_b() { return move_path_queue_b_; }
    // The flood-fill cost map (_G_LLM_TACT_MOVE_COST_MAP, 262144 B / 65536 uint32_t entries, indexed
    // by packed tile (col<<8)|row).
    uint32_t *move_path_cost_map() { return move_path_cost_map_; }

    // llm_tact_tile_rebuild_occupancy_layer_for_map's own scratch global
    // (_G_LLM_TACT_OCCUPANCY_REBUILD_MAP_ID, RID_TACT_OCCUPANCY_REBUILD_MAP_ID): written once by
    // the wrapper, read back by llm_tact_tile_rebuild_occupancy_layer -- a persistent side channel
    // between the two, not a parameter, exactly as the original's single global is.
    int32_t &occupancy_rebuild_map_id() { return *occupancy_rebuild_map_id_; }

    // A per-tick memo bit (RID_TACT_MOVE_PATH_CACHE_VALID, region_ownership.json: writers
    // llm_tact_update_units_and_fx x2, llm_tact_unit_move_tick x2). llm_tact_unit_move_tick sets it
    // (1) once it has fetched _G_LLM_TACT_MOVE_FLOOD_RESULT_COL/ROW for the current
    // (move_cur_col, move_cur_row) cursor, so a second path-slot assignment later in the SAME call
    // reuses the flood result instead of re-deriving it (0x0042fe7d, 0x0042ffaa). The
    // llm_tact_update_units_and_fx write site is outside this closure -- believed to be the
    // per-frame reset before any unit's move_tick runs, unverified.
    int32_t &move_path_cache_valid() { return *move_path_cache_valid_; }

    // The mission-load write path into the CHARACTER table. Unchecked for the same reason
    // unit_at is: the original indexes this array with the raw CHARACTER number from the file
    // and the accessor exists to remove the ADDRESS, not to add a bound the original lacks.
    character_type &character_type_at(int32_t index) { return character_types_[index]; }

    // RID_TACT_UNIT_ACTIVE_COUNT. llm_tact_unit_despawn (TACT1C) decrements it; no other migration
    // member writes it yet.
    int32_t &unit_active_count() { return *unit_active_count_; }

    // RID_TACT_MINE_BLAST_TIME_END -- the write half of tact_view::mine_blast_time_end above (same
    // bytes, two bindings, same shape as move_path_slot_id). llm_tact_unit_mine_arm_tick (TACT1C) is
    // the sole writer: stamps time_GetCurrentTime()+MINE_BLAST_DURATION once progress crosses 0x1f.
    double &mine_blast_time_end() { return *mine_blast_time_end_; }
    // RID_TACT_BLAST_MARKER_COL/ROW -- the live mine-blast marker tile. llm_tact_unit_mine_arm_tick
    // (TACT1C) is the only migration writer (a 24-way dir facing switch nudges the marker toward the
    // arming unit's facing); llm_tact_mission_start (TACT1A) also writes both (reset to a start
    // value), unrelated to this write path.
    int32_t &blast_marker_col() { return *blast_marker_col_; }
    int32_t &blast_marker_row() { return *blast_marker_row_; }

    // RID_GAME_MODE (byte; region_ownership.json's GAME_MODE entry names
    // llm_tact_mission_end_return_to_strategic as one of the accepted cross-writers of this
    // canonical mode flag -- shared by design, not a misattribution). llm_tact_mission_end_return_
    // to_strategic (TACT1A) is this closure's sole writer, setting it back to 2 (strategic) on exit.
    uint8_t &game_mode() { return *game_mode_; }
    // RID_G_PLANET_STATUS -- E_PLANET_STATUS[32] (Ghidra enum, /Manual/game/E_PLANET_STATUS:
    // UNKNOWN=0, VISITED=1, CONQUERED=2, INVASION=4), indexed by planet. region_ownership.json's
    // producer-consumer note: llm_tact_mission_end_return_to_strategic (TACT1A) latches the
    // just-exited planet UNKNOWN->VISITED; SwitchToPlanet (frontier) consumes it on the next visit.
    int32_t &planet_status_at(int32_t planet_index) { return planet_status_[planet_index]; }

    // ---- TACT1A/TACT1C central batch (2026-08-26) ---------------------------------------------
    //
    // llm_tact_mission_start's write set (all offline-proven; see tact_mission_start.h), plus
    // llm_tact_unit_weapons_tick's move-cursor pair and llm_tact_fx_update_projectile's fx-pool
    // write path.

    // _G_LLM_BANK_SPRITE_BASE: 47 int32 slots (188 B). mission_start writes a SPARSE subset
    // (indices 0-3, 5-14, 20); the untouched slots' values are behaviour, so this is a per-slot
    // accessor, not a fill helper.
    int32_t &bank_sprite_base_at(int32_t index) { return bank_sprite_base_[index]; }
    // The write half of the RID_TACT_MAP_TILE_HEIGHT_SPRITES pointer VARIABLE itself --
    // mission_start stores struct_array_malloc_impl's result into it. map_tile_height_sprites()
    // above returns the pointee base (its comment predates this writer existing).
    uint16_t *&map_tile_height_sprites_ptr() { return *map_tile_height_sprites_; }
    // WindowWidth/WindowHeight -- read AND written by mission_start (it saves, clamps and applies
    // the tactical resolution).
    int32_t &window_width() { return *window_width_; }
    int32_t &window_height() { return *window_height_; }
    int32_t &saved_window_width() { return *saved_window_width_; }
    int32_t &gfx_view_tile_height_px() { return *gfx_view_tile_height_px_; }
    // Written then read back after an intervening outward call -- a live accessor by construction.
    int32_t &map_cam_col() { return *map_cam_col_; }
    int32_t &map_cam_row() { return *map_cam_row_; }
    // The write half of tact_view::grid_width/grid_height (RID_WIDTH/RID_HEIGHT): mission_start
    // sets both to 0x80 for the excursion -- the write path the view's own comment promised.
    int32_t &grid_width() { return *grid_width_; }
    int32_t &grid_height() { return *grid_height_; }
    int32_t &view_tiles_w() { return *view_tiles_w_; }
    int32_t &view_tiles_h() { return *view_tiles_h_; }
    double  &cam_col_f() { return *cam_col_f_; }
    double  &cam_row_f() { return *cam_row_f_; }
    int32_t &click_action_taken() { return *click_action_taken_; }
    int32_t &scroll_cmd() { return *scroll_cmd_; }
    // RID_FRAMEBUFFER is a pointer VARIABLE; mission_start writes the BYTES behind it (memcpy
    // destination) and never reassigns the pointer, so this returns the live base, mutable pointee.
    uint8_t *gfx_framebuffer() { return *gfx_framebuffer_; }
    // The write half of tact_view::move_cur_col/move_cur_row -- llm_tact_unit_weapons_tick is the
    // sole original writer (it advances the "current move being processed" cursor per unit).
    int32_t &move_cur_col() { return *move_cur_col_; }
    int32_t &move_cur_row() { return *move_cur_row_; }
    // llm_tact_fx_update_projectile's write path into the fx pool + its live counter (the view's
    // fx_pool member stays the read path; same two-bindings-one-region shape as the planes).
    fx_entry &fx_at(int32_t index) { return fx_pool_[index]; }
    int32_t  &fx_live_count() { return *fx_live_count_; }

    // ---- llm_tact_mission_load's write set (2026-08-26) ----------------------------------------
    // Unchecked like unit_at: mission_load's own init-clear loops deliberately overrun the fx-type
    // table by 0 and the door table by ONE record (JLE bounds, preserved literally) -- the accessor
    // exists to remove the address, never to add a bound the original lacks.
    fx_type     &fx_type_table_at(int32_t index) { return fx_type_table_[index]; }
    door_record &door_table_at(int32_t index) { return door_table_[index]; }
    int32_t     &squad_size() { return *squad_size_; }         // write half of tact_view::squad_size
    int32_t     &enemy_count() { return *enemy_count_; }       // RID_TACT_ENEMY_COUNT
    int32_t     &see_enemy_flag() { return *see_enemy_flag_; } // RID_TACT_SEE_ENEMY_FLAG
    // The mine-blast mission parameters (mission_load's write half; the view reads duration).
    int32_t &mine_blast_first_frame() { return *mine_blast_first_frame_; }
    int32_t &mine_blast_frame_count() { return *mine_blast_frame_count_; }
    double  &mine_blast_duration() { return *mine_blast_duration_; }
    int32_t &mine_blast_sound_id() { return *mine_blast_sound_id_; }
    int32_t &quit_tile_col() { return *quit_tile_col_; }
    int32_t &quit_tile_row() { return *quit_tile_row_; }
    int32_t &target_tile_col() { return *target_tile_col_; }
    int32_t &target_tile_row() { return *target_tile_row_; }
    // void*[16] panel-gfx pointer table; PANEL's swap-on-match logic writes slots directly.
    void *&char_panel_gfx_at(int32_t index) { return char_panel_gfx_[index]; }
    // The mission-file buffer pointer variable (RID_FILE_PTR); GROUND stores its texture ptr here.
    void *&file_ptr() { return *file_ptr_; }

    // ---- TACT1D batch (2026-08-27): the FOV raycaster's cross-function scratch -----------------
    //
    // Every field below crosses a FUNCTION boundary (one migration member writes it, another
    // reads it) -- that is the ONLY reason each one is a store member rather than a local: the
    // raycaster's PURELY internal scratch (angle step/index, ray index/delta, step budget, the
    // direction-table pointer) never escapes llm_tact_fov_raycast_stencil's own body in the
    // original, so the translation keeps those as plain C++ locals instead of manufacturing
    // cross-call state for them.
    int32_t &fov_col() { return *fov_col_; }                 // RID_TACT_FOV_COL
    int32_t &fov_row() { return *fov_row_; }                 // RID_TACT_FOV_ROW
    int32_t &fov_angle_base() { return *fov_angle_base_; }   // RID_TACT_FOV_ANGLE_BASE
    int32_t &fov_angle_width() { return *fov_angle_width_; } // RID_TACT_FOV_ANGLE_WIDTH (uint in Ghidra)
    int32_t &fov_dist() { return *fov_dist_; }               // RID_TACT_FOV_DIST
    // 64x64 byte stencil (RID_TACT_FOV_STENCIL, 4096 B), index = stencil_row*64 + stencil_col
    // (center cell = index 0x7df = 31*64+31). Written by fov_raycast_stencil, read by
    // unit_vision_add/unit_vision_remove.
    uint8_t &fov_stencil_at(int32_t index) { return fov_stencil_[index]; }
    // The nearest-visible-target trackers (Ghidra type undefined2 for the CELL pair -- stored as
    // the packed stencil-relative (col,row) the fov_probe caller unpacks via a SAR/SBB/IDIV
    // sign-decode; see tact_unit_vision.h for the exact bit trace). Written by
    // fov_raycast_stencil/fov_update_nearest_target; read by fov_probe_far_cell_and_door_state.
    int16_t &fov_nearest_hibit_cell() { return *fov_nearest_hibit_cell_; } // RID_TACT_FOV_NEAREST_HIBIT_CELL
    int16_t &fov_nearest_low_cell() { return *fov_nearest_low_cell_; }     // RID_TACT_FOV_NEAREST_LOW_CELL
    int32_t &fov_nearest_hibit_dist() { return *fov_nearest_hibit_dist_; } // RID_TACT_FOV_NEAREST_HIBIT_DIST
    int32_t &fov_nearest_low_dist() { return *fov_nearest_low_dist_; }     // RID_TACT_FOV_NEAREST_LOW_DIST
    // Screen-space redraw-skip cache (RID_TILE_VIS_MAP, 300 B, OWN_SHARED -- other subsystems write
    // it too). llm_tact_door_update_tile_state/llm_tact_door_apply_to_map stamp value 2 at
    // `row_screen*view_tiles_w() + col_screen` for an on-screen door tile.
    uint8_t &tile_vis_map_at(int32_t index) { return tile_vis_map_[index]; }

    // Channel 0 of the shared double[6] retrigger-timestamp array (RID_SND_CHANNEL_NEXT_RETRIGGER_TIME,
    // OWN_SHARED -- llm_tact_fx_play_sound/llm_snd_stop_all_channels, both never-migrated cut-set
    // functions, write the other 5 channels). llm_tact_ambient_sound_tick is the sole tactical writer,
    // and only ever touches channel 0.
    double &snd_channel0_retrigger_time() { return *snd_channel0_retrigger_time_; }

    // ---- TACT1E batch (2026-08-27): the frame pump + selection/sidebar/panel residue -------------
    //
    // llm_tact_frame's own input-latch/camera/roster-scan writes.
    int32_t &cam_scroll_up_held() { return *cam_scroll_up_held_; }       // RID_TACT_CAM_SCROLL_UP_HELD
    int32_t &cam_scroll_down_held() { return *cam_scroll_down_held_; }   // RID_TACT_CAM_SCROLL_DOWN_HELD
    int32_t &cam_scroll_left_held() { return *cam_scroll_left_held_; }   // RID_TACT_CAM_SCROLL_LEFT_HELD
    int32_t &cam_scroll_right_held() { return *cam_scroll_right_held_; } // RID_TACT_CAM_SCROLL_RIGHT_HELD
    int32_t &exit_confirm_open() { return *exit_confirm_open_; }         // RID_TACT_EXIT_CONFIRM_OPEN
    int32_t &click_scan_scratch() { return *click_scan_scratch_; }       // RID_TACT_CLICK_SCAN_SCRATCH
    int32_t &drag_anchor_x() { return *drag_anchor_x_; }                 // RID_TACT_DRAG_ANCHOR_X
    int32_t &drag_anchor_y() { return *drag_anchor_y_; }                 // RID_TACT_DRAG_ANCHOR_Y
    int32_t &drag_select_active() { return *drag_select_active_; }       // RID_TACT_DRAG_SELECT_ACTIVE
    // The write half of tact_view::hovered_unit_id (RID_TACT_HOVERED_UNIT_ID) -- llm_tact_frame is a
    // second writer alongside the sidebar input phase the view's own comment names.
    int32_t &hovered_unit_id() { return *hovered_unit_id_; }
    // RID_TACT_ACTIVE_UNIT_COUNT / _CACHED -- llm_tact_frame's roster-rescan gate and
    // llm_tact_select_next_unit's re-selection gate both read+write this pair (NOT the same region
    // as tact_store::unit_active_count()/RID_TACT_UNIT_ACTIVE_COUNT above -- two distinct globals,
    // see the dll_addr_manifest.json derivation).
    int32_t &active_unit_count() { return *active_unit_count_; }
    int32_t &active_unit_count_cached() { return *active_unit_count_cached_; }
    // The screen-space "already drawn this frame" cache (RID_TILE_DRAWN_MAP, 300 B, same shape as
    // tile_vis_map_at). llm_tact_frame is the migration's first writer.
    uint8_t &tile_drawn_map_at(int32_t index) { return tile_drawn_map_[index]; }

    // llm_tact_cam_follow_selection_tick's own kill-switch write (RID_TACT_CAM_FOLLOW_SELECTION):
    // clears the follow flag once the camera has caught up to within the dead zone.
    int32_t &cam_follow_selection() { return *cam_follow_selection_; }

    // llm_tact_ui_sel_panel_init's write set: the converted icon-sprite pointer table and the
    // multi-select-mode latch it resets on (re)entry.
    void   *&sel_panel_icon_gfx_at(int32_t index) { return sel_panel_icon_gfx_[index]; } // RID_TACT_SEL_PANEL_ICON_GFX
    int32_t &ui_sel_panel_multi_mode() { return *ui_sel_panel_multi_mode_; }             // RID_TACT_UI_SEL_PANEL_MULTI_MODE

    // llm_tact_sidebar_dispatch's own hit-testing/highlight/scroll write set (RID_TACT_SEL_PANEL_*
    // /RID_TACT_SIDEBAR_*). sidebar_active_group_id/sidebar_slot_scroll are read-only in this
    // closure (written by frontier sidebar helpers) but live here because the region is a
    // tact_store-only binding, same shape as target_tile_col/row above.
    int32_t &sel_panel_icon_slot_state_at(int32_t index) { return sel_panel_icon_slot_state_[index]; } // RID_TACT_SEL_PANEL_ICON_SLOT_STATE
    int32_t &sidebar_active_group_id() { return *sidebar_active_group_id_; }                           // RID_TACT_SIDEBAR_ACTIVE_GROUP_ID
    int32_t &sidebar_slot_scroll() { return *sidebar_slot_scroll_; }                                   // RID_TACT_SIDEBAR_SLOT_SCROLL
    int32_t &sidebar_scrollbtn_state_at(int32_t index) { return sidebar_scrollbtn_state_[index]; }     // RID_TACT_SIDEBAR_SCROLLBTN_STATE
    int32_t &sidebar_ui_hit_code() { return *sidebar_ui_hit_code_; }                                   // RID_TACT_SIDEBAR_UI_HIT_CODE
    int32_t &sidebar_highlighted_unit_id() { return *sidebar_highlighted_unit_id_; }                   // RID_TACT_SIDEBAR_HIGHLIGHTED_UNIT_ID
    // int[64] (see dll_addr_manifest.json for the retyping derivation) -- llm_tact_sidebar_dispatch's own
    // read of the visible-slot roster the frontier llm_tact_ui_sidebar_roster_refresh fills.
    int32_t &sidebar_slot_unit_ids_at(int32_t index) { return sidebar_slot_unit_ids_[index]; } // RID_TACT_SIDEBAR_SLOT_UNIT_IDS
    // The write half of tact_view::squad_bb_target_energy_pct (RID_SQUAD_BB_TARGET_ENERGY_PCT) --
    // llm_tact_scroll_target_proximity_tick is this closure's sole writer.
    int32_t &squad_bb_target_energy_pct() { return *squad_bb_target_energy_pct_; }

    // ---- TACT1E batch (2026-08-28) ------------------------------------------------
    //
    // llm_tact_squad_roster_refresh's/llm_tact_ui_sel_panel_single_mode_tick's write path -- both
    // regions already existed (RID_TACT_SIDEBAR_UNASSIGNED_SCROLL_ROW/_GROUP_SCROLL_ROW); only the
    // mutable accessor was missing.
    int32_t &sidebar_unassigned_scroll_row() { return *sidebar_unassigned_scroll_row_; } // RID_TACT_SIDEBAR_UNASSIGNED_SCROLL_ROW
    int32_t &sidebar_group_scroll_row() { return *sidebar_group_scroll_row_; }           // RID_TACT_SIDEBAR_GROUP_SCROLL_ROW
    // llm_tact_squad_roster_refresh's bulk zero-fill + append targets -- MUTABLE bases (the region
    // already had a read-only tact_view binding above; this is the write half, same shape as
    // map_tile_height_sprites' view/store pair). Unchecked raw byte base, indexed by the caller with
    // the literal `llm_tact_unit_roster_slot` stride (0x100) until that struct is materialized
    // (dll_addr_manifest.json N7).
    uint8_t *unassigned_unit_roster() { return unassigned_unit_roster_; } // RID_TACT_UNASSIGNED_UNIT_ROSTER
    uint8_t *group_unit_roster() { return group_unit_roster_; }           // RID_TACT_GROUP_UNIT_ROSTER
    // llm_tact_frame's mouse-pump write: STORE mouse_buttons_get() -> mouse_buttons_cur(). A second,
    // mutable binding of the SAME region tact_view::mouse_buttons_cur above reads (OWN_SHARED, same
    // shape as map_width_'s view/store pair).
    uint8_t &mouse_buttons_cur() { return *mouse_buttons_cur_; } // RID_MOUSE_BUTTONS_CUR
    // llm_tact_frame's exit-confirm-prompt scratch buffer (RID_G_TEXT_TMP, the same shared 512-byte
    // wide-string scratch sim_store::text_scratch() already binds -- OWN_SHARED, MF_VIEW|MF_MEASURED,
    // nothing about the simulation is read back out of it).
    wchar_t *text_scratch() { return text_scratch_; } // RID_G_TEXT_TMP

    // llm_tact_view_shift_col_inc/dec/row_inc/dec's write path: both are POINTER VARIABLES (same
    // shape as gfx_framebuffer above) -- the functions shift-copy the bytes the pointer points to and
    // never reassign the pointer itself.
    uint8_t *tile_vis_map_ptr() { return *tile_vis_map_ptr_; } // RID_TILE_VIS_MAP_PTR
    uint8_t *los_cache_ptr() { return *los_cache_ptr_; }       // RID_TACT_LOS_CACHE_PTR

    // ---- the mode-shared planes ---------------------------------------------------------------
    //
    // NOT members of this class, and that is the point: they are ONE binding (state/mode_planes.h)
    // handed to whichever mode is live, so the sim and a mission cannot disagree about the arena's
    // shape. Reaching them is deliberately a second, differently-named step -- `own.planes()` reads
    // as "the substrate", not as "tactical's state".
    mh::state::mode_planes       &planes() { return planes_; }
    const mh::state::mode_planes &planes() const { return planes_; }

private:
    // W3: the only two callers are the region-registry binder and the offline fixture.
    tact_store(tact_unit *units, int32_t *sidebar_mouse_x, int32_t *sidebar_mouse_y,
               character_type *character_types, int32_t *map_width, int32_t *map_height,
               uint16_t                          **map_tile_height_sprites,
               mh::game::mh_llm_squad_status_slot *squad_status, mh::state::mode_planes planes,
               int32_t *occupancy_rebuild_map_id, int32_t *move_path_cache_valid,
               int32_t *move_path_slot_id, teleport_zone *teleport_zones,
               uint8_t *move_flood_start_col, uint8_t *move_flood_start_row,
               uint8_t *move_flood_goal_col, uint8_t *move_flood_goal_row,
               uint8_t *move_flood_result_col, uint8_t *move_flood_result_row,
               uint32_t *move_path_coord_mask, uint32_t *move_path_goal_packed,
               uint32_t *move_path_start_packed, uint32_t *move_path_tile_cost,
               uint32_t *move_path_trace_tile, uint32_t *move_path_trace_base,
               uint32_t *move_path_trace_diag_cand, uint8_t *move_path_trace_best_dir,
               uint32_t *move_path_trace_best_tile, uint32_t *move_path_trace_cost_sum,
               uint32_t *move_path_rle_count, uint32_t *move_path_queue_cur_addr,
               uint16_t *move_path_queue_a, uint16_t *move_path_queue_b,
               uint32_t *move_path_cost_map, int32_t *unit_active_count,
               double *mine_blast_time_end, int32_t *blast_marker_col, int32_t *blast_marker_row,
               uint8_t *game_mode, int32_t *planet_status, int32_t *bank_sprite_base,
               int32_t *window_width, int32_t *window_height, int32_t *saved_window_width,
               int32_t *gfx_view_tile_height_px, int32_t *map_cam_col, int32_t *map_cam_row,
               int32_t *grid_width, int32_t *grid_height, int32_t *view_tiles_w,
               int32_t *view_tiles_h, double *cam_col_f, double *cam_row_f,
               int32_t *click_action_taken, int32_t *scroll_cmd, uint8_t **gfx_framebuffer,
               int32_t *move_cur_col, int32_t *move_cur_row, fx_entry *fx_pool,
               int32_t *fx_live_count, fx_type *fx_type_table, door_record *door_table,
               int32_t *squad_size, int32_t *enemy_count, int32_t *see_enemy_flag,
               int32_t *mine_blast_first_frame, int32_t *mine_blast_frame_count,
               double *mine_blast_duration, int32_t *mine_blast_sound_id, int32_t *quit_tile_col,
               int32_t *quit_tile_row, int32_t *target_tile_col, int32_t *target_tile_row,
               void **char_panel_gfx, void **file_ptr, int32_t *fov_col, int32_t *fov_row,
               int32_t *fov_angle_base, int32_t *fov_angle_width, int32_t *fov_dist,
               uint8_t *fov_stencil, int16_t *fov_nearest_hibit_cell, int16_t *fov_nearest_low_cell,
               int32_t *fov_nearest_hibit_dist, int32_t *fov_nearest_low_dist, uint8_t *tile_vis_map,
               double *snd_channel0_retrigger_time,
               // TACT1E batch (2026-08-27).
               int32_t *cam_scroll_up_held, int32_t *cam_scroll_down_held,
               int32_t *cam_scroll_left_held, int32_t *cam_scroll_right_held,
               int32_t *exit_confirm_open, int32_t *click_scan_scratch, int32_t *drag_anchor_x,
               int32_t *drag_anchor_y, int32_t *drag_select_active, int32_t *hovered_unit_id,
               int32_t *active_unit_count, int32_t *active_unit_count_cached,
               uint8_t *tile_drawn_map, int32_t *cam_follow_selection, void **sel_panel_icon_gfx,
               int32_t *ui_sel_panel_multi_mode, int32_t *sel_panel_icon_slot_state,
               int32_t *sidebar_active_group_id, int32_t *sidebar_slot_scroll,
               int32_t *sidebar_scrollbtn_state, int32_t *sidebar_ui_hit_code,
               int32_t *sidebar_highlighted_unit_id, int32_t *sidebar_slot_unit_ids,
               int32_t *squad_bb_target_energy_pct, uint8_t **tile_vis_map_ptr,
               uint8_t **los_cache_ptr,
               // TACT1E batch (2026-08-28).
               int32_t *sidebar_unassigned_scroll_row, int32_t *sidebar_group_scroll_row,
               uint8_t *unassigned_unit_roster, uint8_t *group_unit_roster,
               uint8_t *mouse_buttons_cur, wchar_t *text_scratch)
        : units_(units), sidebar_mouse_x_(sidebar_mouse_x), sidebar_mouse_y_(sidebar_mouse_y),
          character_types_(character_types), map_width_(map_width), map_height_(map_height),
          map_tile_height_sprites_(map_tile_height_sprites),
          squad_status_(squad_status), planes_(planes),
          occupancy_rebuild_map_id_(occupancy_rebuild_map_id),
          move_path_cache_valid_(move_path_cache_valid), move_path_slot_id_(move_path_slot_id),
          teleport_zones_(teleport_zones), move_flood_start_col_(move_flood_start_col),
          move_flood_start_row_(move_flood_start_row), move_flood_goal_col_(move_flood_goal_col),
          move_flood_goal_row_(move_flood_goal_row), move_flood_result_col_(move_flood_result_col),
          move_flood_result_row_(move_flood_result_row),
          move_path_coord_mask_(move_path_coord_mask),
          move_path_goal_packed_(move_path_goal_packed),
          move_path_start_packed_(move_path_start_packed),
          move_path_tile_cost_(move_path_tile_cost), move_path_trace_tile_(move_path_trace_tile),
          move_path_trace_base_(move_path_trace_base),
          move_path_trace_diag_cand_(move_path_trace_diag_cand),
          move_path_trace_best_dir_(move_path_trace_best_dir),
          move_path_trace_best_tile_(move_path_trace_best_tile),
          move_path_trace_cost_sum_(move_path_trace_cost_sum),
          move_path_rle_count_(move_path_rle_count),
          move_path_queue_cur_addr_(move_path_queue_cur_addr),
          move_path_queue_a_(move_path_queue_a), move_path_queue_b_(move_path_queue_b),
          move_path_cost_map_(move_path_cost_map), unit_active_count_(unit_active_count),
          mine_blast_time_end_(mine_blast_time_end), blast_marker_col_(blast_marker_col),
          blast_marker_row_(blast_marker_row), game_mode_(game_mode),
          planet_status_(planet_status), bank_sprite_base_(bank_sprite_base),
          window_width_(window_width), window_height_(window_height),
          saved_window_width_(saved_window_width),
          gfx_view_tile_height_px_(gfx_view_tile_height_px), map_cam_col_(map_cam_col),
          map_cam_row_(map_cam_row), grid_width_(grid_width), grid_height_(grid_height),
          view_tiles_w_(view_tiles_w), view_tiles_h_(view_tiles_h), cam_col_f_(cam_col_f),
          cam_row_f_(cam_row_f), click_action_taken_(click_action_taken),
          scroll_cmd_(scroll_cmd), gfx_framebuffer_(gfx_framebuffer),
          move_cur_col_(move_cur_col), move_cur_row_(move_cur_row), fx_pool_(fx_pool),
          fx_live_count_(fx_live_count), fx_type_table_(fx_type_table), door_table_(door_table),
          squad_size_(squad_size), enemy_count_(enemy_count), see_enemy_flag_(see_enemy_flag),
          mine_blast_first_frame_(mine_blast_first_frame),
          mine_blast_frame_count_(mine_blast_frame_count),
          mine_blast_duration_(mine_blast_duration), mine_blast_sound_id_(mine_blast_sound_id),
          quit_tile_col_(quit_tile_col), quit_tile_row_(quit_tile_row),
          target_tile_col_(target_tile_col), target_tile_row_(target_tile_row),
          char_panel_gfx_(char_panel_gfx), file_ptr_(file_ptr), fov_col_(fov_col),
          fov_row_(fov_row), fov_angle_base_(fov_angle_base), fov_angle_width_(fov_angle_width),
          fov_dist_(fov_dist), fov_stencil_(fov_stencil),
          fov_nearest_hibit_cell_(fov_nearest_hibit_cell),
          fov_nearest_low_cell_(fov_nearest_low_cell),
          fov_nearest_hibit_dist_(fov_nearest_hibit_dist),
          fov_nearest_low_dist_(fov_nearest_low_dist), tile_vis_map_(tile_vis_map),
          snd_channel0_retrigger_time_(snd_channel0_retrigger_time),
          cam_scroll_up_held_(cam_scroll_up_held), cam_scroll_down_held_(cam_scroll_down_held),
          cam_scroll_left_held_(cam_scroll_left_held),
          cam_scroll_right_held_(cam_scroll_right_held), exit_confirm_open_(exit_confirm_open),
          click_scan_scratch_(click_scan_scratch), drag_anchor_x_(drag_anchor_x),
          drag_anchor_y_(drag_anchor_y), drag_select_active_(drag_select_active),
          hovered_unit_id_(hovered_unit_id), active_unit_count_(active_unit_count),
          active_unit_count_cached_(active_unit_count_cached), tile_drawn_map_(tile_drawn_map),
          cam_follow_selection_(cam_follow_selection), sel_panel_icon_gfx_(sel_panel_icon_gfx),
          ui_sel_panel_multi_mode_(ui_sel_panel_multi_mode),
          sel_panel_icon_slot_state_(sel_panel_icon_slot_state),
          sidebar_active_group_id_(sidebar_active_group_id),
          sidebar_slot_scroll_(sidebar_slot_scroll),
          sidebar_scrollbtn_state_(sidebar_scrollbtn_state),
          sidebar_ui_hit_code_(sidebar_ui_hit_code),
          sidebar_highlighted_unit_id_(sidebar_highlighted_unit_id),
          sidebar_slot_unit_ids_(sidebar_slot_unit_ids),
          squad_bb_target_energy_pct_(squad_bb_target_energy_pct),
          tile_vis_map_ptr_(tile_vis_map_ptr), los_cache_ptr_(los_cache_ptr),
          sidebar_unassigned_scroll_row_(sidebar_unassigned_scroll_row),
          sidebar_group_scroll_row_(sidebar_group_scroll_row),
          unassigned_unit_roster_(unassigned_unit_roster), group_unit_roster_(group_unit_roster),
          mouse_buttons_cur_(mouse_buttons_cur), text_scratch_(text_scratch) {}

    friend tact_state state();
    friend struct tact_fixture;

    // W2: private, so no game-state address ever escapes this class.
    tact_unit                          *units_;
    int32_t                            *sidebar_mouse_x_;
    int32_t                            *sidebar_mouse_y_;
    character_type                     *character_types_;
    int32_t                            *map_width_;
    int32_t                            *map_height_;
    uint16_t                          **map_tile_height_sprites_;
    mh::game::mh_llm_squad_status_slot *squad_status_;

    mh::state::mode_planes planes_;
    int32_t               *occupancy_rebuild_map_id_;
    int32_t               *move_path_cache_valid_;
    int32_t               *move_path_slot_id_;
    teleport_zone         *teleport_zones_;
    uint8_t               *move_flood_start_col_;
    uint8_t               *move_flood_start_row_;
    uint8_t               *move_flood_goal_col_;
    uint8_t               *move_flood_goal_row_;
    uint8_t               *move_flood_result_col_;
    uint8_t               *move_flood_result_row_;

    uint32_t *move_path_coord_mask_;
    uint32_t *move_path_goal_packed_;
    uint32_t *move_path_start_packed_;
    uint32_t *move_path_tile_cost_;
    uint32_t *move_path_trace_tile_;
    uint32_t *move_path_trace_base_;
    uint32_t *move_path_trace_diag_cand_;
    uint8_t  *move_path_trace_best_dir_;
    uint32_t *move_path_trace_best_tile_;
    uint32_t *move_path_trace_cost_sum_;
    uint32_t *move_path_rle_count_;
    uint32_t *move_path_queue_cur_addr_;
    uint16_t *move_path_queue_a_;
    uint16_t *move_path_queue_b_;
    uint32_t *move_path_cost_map_;
    int32_t  *unit_active_count_;

    double  *mine_blast_time_end_;
    int32_t *blast_marker_col_;
    int32_t *blast_marker_row_;
    uint8_t *game_mode_;
    int32_t *planet_status_;

    int32_t  *bank_sprite_base_;
    int32_t  *window_width_;
    int32_t  *window_height_;
    int32_t  *saved_window_width_;
    int32_t  *gfx_view_tile_height_px_;
    int32_t  *map_cam_col_;
    int32_t  *map_cam_row_;
    int32_t  *grid_width_;
    int32_t  *grid_height_;
    int32_t  *view_tiles_w_;
    int32_t  *view_tiles_h_;
    double   *cam_col_f_;
    double   *cam_row_f_;
    int32_t  *click_action_taken_;
    int32_t  *scroll_cmd_;
    uint8_t **gfx_framebuffer_;
    int32_t  *move_cur_col_;
    int32_t  *move_cur_row_;
    fx_entry *fx_pool_;
    int32_t  *fx_live_count_;

    fx_type     *fx_type_table_;
    door_record *door_table_;
    int32_t     *squad_size_;
    int32_t     *enemy_count_;
    int32_t     *see_enemy_flag_;
    int32_t     *mine_blast_first_frame_;
    int32_t     *mine_blast_frame_count_;
    double      *mine_blast_duration_;
    int32_t     *mine_blast_sound_id_;
    int32_t     *quit_tile_col_;
    int32_t     *quit_tile_row_;
    int32_t     *target_tile_col_;
    int32_t     *target_tile_row_;
    void       **char_panel_gfx_;
    void       **file_ptr_;

    int32_t *fov_col_;
    int32_t *fov_row_;
    int32_t *fov_angle_base_;
    int32_t *fov_angle_width_;
    int32_t *fov_dist_;
    uint8_t *fov_stencil_;
    int16_t *fov_nearest_hibit_cell_;
    int16_t *fov_nearest_low_cell_;
    int32_t *fov_nearest_hibit_dist_;
    int32_t *fov_nearest_low_dist_;
    uint8_t *tile_vis_map_;
    double  *snd_channel0_retrigger_time_;

    // TACT1E batch (2026-08-27).
    int32_t  *cam_scroll_up_held_;
    int32_t  *cam_scroll_down_held_;
    int32_t  *cam_scroll_left_held_;
    int32_t  *cam_scroll_right_held_;
    int32_t  *exit_confirm_open_;
    int32_t  *click_scan_scratch_;
    int32_t  *drag_anchor_x_;
    int32_t  *drag_anchor_y_;
    int32_t  *drag_select_active_;
    int32_t  *hovered_unit_id_;
    int32_t  *active_unit_count_;
    int32_t  *active_unit_count_cached_;
    uint8_t  *tile_drawn_map_;
    int32_t  *cam_follow_selection_;
    void    **sel_panel_icon_gfx_;
    int32_t  *ui_sel_panel_multi_mode_;
    int32_t  *sel_panel_icon_slot_state_;
    int32_t  *sidebar_active_group_id_;
    int32_t  *sidebar_slot_scroll_;
    int32_t  *sidebar_scrollbtn_state_;
    int32_t  *sidebar_ui_hit_code_;
    int32_t  *sidebar_highlighted_unit_id_;
    int32_t  *sidebar_slot_unit_ids_;
    int32_t  *squad_bb_target_energy_pct_;
    uint8_t **tile_vis_map_ptr_;
    uint8_t **los_cache_ptr_;

    int32_t *sidebar_unassigned_scroll_row_;
    int32_t *sidebar_group_scroll_row_;
    uint8_t *unassigned_unit_roster_;
    uint8_t *group_unit_roster_;
    uint8_t *mouse_buttons_cur_;
    wchar_t *text_scratch_;
};

struct tact_state {
    tact_view  read;
    tact_store own;
};

// Binds both halves from the region registry. BY VALUE and re-resolved per call.
tact_state state();

// ---- read helpers -----------------------------------------------------------------------------
//
// Free functions over tact_view, matching sim_state.h's shape: an index expression appears once
// rather than at every call site, and the stride is stated in one place.

inline const tact_unit &unit_of(const tact_view &v, int32_t index) { return v.units[index]; }

// The shared grid through the READ path. Same (x<<8)|y column-major indexing as
// mode_planes::tile_object_at -- one layout, stated twice only because one is const.
inline const mh::state::tile_object &tile_at(const tact_view &v, int32_t x, int32_t y) {
    return v.tile_objects[(x << 8) | y];
}
inline uint8_t passable_at(const tact_view &v, int32_t x, int32_t y) {
    return v.passable[(x << 8) | y];
}


} // namespace mh::tact
