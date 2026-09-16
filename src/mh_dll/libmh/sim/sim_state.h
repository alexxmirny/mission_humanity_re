//
// sim/sim_state.h -- the strategic simulation's state interface (RI-SIM / SIM0).
//
// The sim migration set is 307 functions (tools/data/sim_migration.json). This header is the ONE
// place that says where their state lives and which half of it they may write.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS NOT ai/ai_state.h WITH THE NAMES CHANGED
// ---------------------------------------------------------------------------------------------
//
// AI0's whole leverage was const-ness: `ai_view` is pointer-to-const throughout, `ai_store` holds
// nothing but the AI's own island, and so P2-RULES' R2 ("the AI writes orders, not sim state")
// became a compile error. That does not transfer. The sim IS the rosters' legitimate writer --
// 138 of these 307 functions write a region the state matrix classes SHARED, `units` alone has 47
// writers -- so a const view here would reject the subsystem's entire purpose rather than its
// mistakes.
//
// So the compile-time property is different in kind. Three rules, in the order they bite:
//
//   W1  THE READ VIEW IS STILL CONST, and its MEMBERSHIP is the claim. A region reachable only
//       through `sim_view` is one the sim demonstrably never writes -- the cfg tables (frozen
//       after boot), the map extents (written by the map loader), another subsystem's state. That
//       a write to one of those does not compile is a real fact about this subsystem, not a
//       restatement of "the sim writes things".
//
//   W2  THE WRITE STORE HAS NO REACHABLE ADDRESS. `sim_store`'s pointers are PRIVATE and every
//       write goes through a typed accessor that returns a reference to ONE record. No `unit *`
//       is ever handed out, so no translation can cache a base, do arithmetic past a record, or
//       hold a pointer across a rebase. This is what keeps ST3's ownership interlock meaningful
//       once the rosters physically move (ST2): there is no address to go stale, because there is
//       no address.
//
//   W3  THE STORE CANNOT BE CONSTRUCTED OUTSIDE THIS MODULE. `sim_store`'s constructor is private
//       with exactly two friends -- `state()`, which binds it from the region registry, and
//       `sim_fixture`, which binds it to the offline harness's heap buffers. Non-sim code that
//       wants a mutable handle has to write itself into that friend list, which is a deliberate,
//       greppable, reviewable act rather than an include away.
//
// WHAT THESE DO NOT PREVENT, stated plainly because a guarantee oversold is worse than none: a
// `const_cast`, a `reinterpret_cast` from a literal VA, or a self-declared `mh::sim::sim_fixture`
// all defeat every rule above. C++ has no way to stop code in the same binary from naming a
// symbol. What these rules buy is that the ACCIDENT is impossible -- drift, an include-what-you-
// need, a translation that reaches for the roster base because it was in scope -- and that the
// deliberate bypass is one grep away. tools/lint_repo.py's sim-raw-address check is the other
// half: no TU under libmh/sim/ except this one's .cpp may name `mh::state::ptr` or a game VA.
//
// mh_nettest/sim_write_negative.cpp is the checked-in proof that W1-W3 hold, driven by
// tools/check_const_view.py (which also requires a POSITIVE arm to compile, so a case cannot pass
// because the file is broken).
//
// ---------------------------------------------------------------------------------------------
// THE INVARIANTS INHERITED FROM AI0, WHICH DO TRANSFER UNCHANGED
// ---------------------------------------------------------------------------------------------
//
// `const` HERE MEANS "YOU MAY NOT WRITE THROUGH IT", NOT "THIS DOES NOT CHANGE". Nothing here
// copies a byte or caches a value; a view that snapshotted contents would pass every test in an
// idle world and desync in a busy one.
//
// AND NOTHING HERE CACHES AN ADDRESS. `state()` returns BY VALUE and re-resolves every pointer
// from the region registry on every call. A `static` bound once would go stale the moment a region
// is rebased under ST2, and it would go stale SILENTLY -- the view would keep serving the
// abandoned .bss and every consumer would agree with it. `net_selftest.exe statetest` rebases
// RID_UNITS and asserts BOTH halves follow, which is a check that can only exist because there is
// nothing to go stale.
//
// LAW 1 AND LAW 3. Record layouts come from addr/mh_structs.gen.h (static_assert'd against Ghidra,
// so a retype breaks the build instead of corrupting a save) and addresses from the region
// registry via mh::state::ptr. No byte offsets and no literal VAs appear in this module.
//
// THE FP LANDMINE (the reimplementation plan Sect. 6). The sim is x87 and MSVC x86 emits SSE2 by
// default. Every TU under libmh/sim/ is compiled /arch:IA32 /fp:precise -- set per-file in BOTH
// mh.vcxproj and mh_nettest/mh_nettest.vcxproj, because a TU that is x87 in the DLL and SSE2 in
// the selftest would make the offline oracle lie in the one place it matters most.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h" // SB-BIND T2: the derived per-player row capacities
#include "state/mode_planes.h"

namespace mh::sim {

// ---- the record types ---------------------------------------------------------------------
// Aliases only, so a translation can name the type it is indexing. These are the SAME structs the
// AI view aliases; the duplication is two `using` lines and buys libmh/sim/ not depending on libmh/ai/.
using unit         = mh::game::mh_map_object_unit;
using unit_weapon  = mh::game::mh_map_unit_weapon;
using building     = mh::game::mh_map_object_building;
using player_data  = mh::game::mh_game_player_data;
using cfg_unit     = mh::game::mh_cfg_final_struct_Unit;
using cfg_building = mh::game::mh_cfg_final_struct_Building;
using cfg_weapon   = mh::game::mh_cfg_final_struct_Weapon;
using cfg_resource = mh::game::mh_cfg_struct_resource;
// SIM1F (2026-08-17). The map-region graph's node type -- a HEAP object (allocated by
// the original's own map::block_8::GetNextBlock()/llm_map_region_free(), called through
// `mh::call::`, never by this closure's own storage). Pointers to it are an ADDRESS ESCAPE like
// `text_scratch()`/`available_buildings_row()` above, except the pointee lives on the live heap
// rather than in a registered .bss region -- there is no sim_view/sim_store accessor for its
// FIELDS, a translation dereferences `llm_map_region*` directly like any other pointer-chasing C++.
// See sim_invasion.h-style precedent comments in the region TUs for the full derivation.
using llm_map_region      = mh::game::mh_llm_map_region;
using llm_map_region_cell = mh::game::mh_llm_map_region_cell;
using llm_map_bfs_entry   = mh::game::mh_llm_map_bfs_entry;
// SIM1F (2026-08-16). Projects[100] (RID_PROJECTS) -- the research/build-project CFG
// table (distinct from `player_progress`/RID_PROGRESS above, which is per-player acquisition STATE).
using cfg_project = mh::game::mh_cfg_final_struct_Project;
// SIM1F (2026-08-16). Progress[300] (RID_PROGRESS_00E162E4) -- the CFG Invention TABLE
// (distinct from `player_progress`/RID_PROGRESS above, the per-player per-invention STATE array;
// see ai_state.h's identical distinction, which this is a SECOND independent binding of the SAME
// region -- libmh/sim/ does not depend on libmh/ai/, same posture as storage_stats/power_stats above).
using cfg_invention = mh::game::mh_cfg_final_struct_Invention;
// SIM1B (2026-08-12). Per-player per-invention acquisition state (RID_PROGRESS) -- a SECOND
// independent binding of the SAME region ai/ai_state.h's own `progress`/`player_progress` already
// bind (see that header's comment distinguishing this from `Progress`, the CFG Invention TABLE, a
// different region entirely). llm_bldg_pay_build_cost reads `.available` to gate a building's
// prerequisite invention before charging its cost.
using player_progress = mh::game::mh_game_progress;
// SIM1F (2026-08-16). Upgrades[99] (RID_UPGRADES) -- the researched-upgrade CFG table,
// indexed by upgrade_id. Read-only in this closure (game_HandleUpgrade only reads it; the WRITE
// target is `Unit`/`Weapon` below, not this table).
using cfg_upgrade = mh::game::mh_cfg_final_struct_Upgrade;
// SIM1B (2026-08-12). _G_LLM_STRAT_POWER_STATS[8] (RID_STRAT_POWER_STATS) -- per-player power
// generation/consumption + the brownout ratio llm_strat_power_recompute derives from them. Brand
// new type (no Ghidra struct existed here before this slice; derived field-for-field from
// tmp/decomp/llm_strat_power_recompute_00491260.asm).
using power_stats = mh::game::mh_llm_strat_power_stats;
// SIM1D (2026-08-13). _G_LLM_STRAT_STORAGE_STATS[8] (RID_STRAT_STORAGE_STATS) --
// per-player storage capacity ledger (cap_accum[10]/cap_prev[10]/subtick_b_clock). SECOND
// independent binding of the SAME region ai/ai_state.h's own `storage` already binds (that header's
// comment: the AI reads cap_prev[1..4] as the shortage-check capacity numerator). Read-only here --
// llm_strat_check_storage_overflow compares cap_prev[i] against player_resources to decide whether
// to decay the excess; no writer anywhere in the sim closure.
using storage_stats = mh::game::mh_llm_strat_storage_stats;
// Per-sprite metadata table (_G_LLM_SPRITE_META[22000], RID_SPRITE_META) -- boot-loaded from the
// .BNK files, no writer anywhere in the sim closure, same posture as cfg_units/cfg_buildings below.
// SIM1A's mount-position pair reads mount1_x/y, mount2_x/y, submount_x/y; the soldier
// screen-pos helper reads mount1_x/y generically as a per-frame draw offset -- see the struct's own
// field comments (docs/structs.md) for the derivation.
using sprite_meta_entry = mh::game::mh_gfx_sprite_meta;
// The GROUP-MOVE member scratch the unit state machine's 0x0a->0x0b->0x0c chain fills; read by
// sim_group_scratch_centroid.h, which is where the shape is documented.
using group_scratch_member = mh::game::mh_llm_strat_group_scratch_member;
// SIM1-G1 (2026-08-19). One compressed path waypoint ({byte heading; byte run_length}) in the
// per-unit path buffers _G_LLM_STRAT_PATH_BUFFERS[240000] = 8 players * 100 path slots * 300
// waypoints (RID_STRAT_PATH_BUFFERS, a SAVE-serialized region so the layout is frozen). Written by
// the pathfinder (batch G2) and by llm_strat_unit_state_move_walker (which decrements .run_length as
// each waypoint is spent); read by move_walker (.heading/.run_length) and group_marshal
// (.run_length). Indexed player*30000 + path_slot_id*300 + path_cursor -- see sim_store::path_buffer_at().
using path_waypoint = mh::game::mh_llm_strat_path_waypoint;
// SIM1-G1 (2026-08-20). One of 3 candidate turn-delta entries for a heading
// (_G_LLM_STRAT_HEADING_CANDIDATE_TABLE[72] = 24 headings * 3, RID_STRAT_HEADING_CANDIDATE_TABLE).
// Already named+typed in Ghidra before this slice; written only by the ORIGINAL, out-of-scope
// llm_strat_group_step_heading_table_init (boot-time init) -- read-only in this closure, consumed by
// llm_strat_unit_group_step_ground via llm_strat_group_member_find_matching_turn_delta.
// FIELDS 2 AND 3 WERE MIS-DOCUMENTED UNTIL 2026-08-20 and are now `start_col_delta` /
// `start_row_delta`: they are the per-slot (dx, dy) TILE STEP added to the greedy-path start
// column/row (`(local + delta) & general.width_mask` and `& general.height_mask` respectively,
// 9 read sites each), NOT the `unused/filler` and `override value` the old names claimed. Do not
// translate group_step_ground against the old reading -- see ghidra_findings 2026-08-20-g1s2-1.
using heading_slot = mh::game::mh_llm_strat_heading_slot;
// SIM1-G1 (2026-08-20). One squad-merge formation-anchor scratch entry {int32 x; int32
// y;} (_G_LLM_STRAT_SQUAD_FORMATION_ANCHOR_SCRATCH[5], RID_STRAT_SQUAD_FORMATION_ANCHOR_SCRATCH).
// Written by the ORIGINAL, out-of-scope llm_strat_unit_state_squad_merge; read-only here --
// llm_strat_squad_pick_free_formation_anchor consults it to find a still-free anchor.
using squad_formation_anchor_scratch = mh::game::mh_llm_strat_squad_formation_anchor_scratch;
// SIM1-G2 (2026-08-21). One diagonal-direction-code merge/splice table entry (see
// sim_view::pathtrace_dir_split_table below for the full derivation).
using llm_strat_pathtrace_dir_split = mh::game::mh_llm_strat_pathtrace_dir_split;
// SIM1-G1 (2026-08-20). One route step {byte dir_code; byte run_length;} in
// _G_LLM_STRAT_GROUP_ROUTE_STEPS[256] (RID_STRAT_GROUP_ROUTE_STEPS) -- the current group move order's
// computed route. Written by llm_strat_group_move_order_pathfind (original, out of scope); this
// slice's llm_strat_group_move_order_commit both reads and writes it (see that TU for the exact
// access pattern -- re-derive from its own .asm, do not assume read-only from this comment).
using route_step = mh::game::mh_llm_strat_route_step;
// SIM1-G1 (2026-08-20). One group-move member {uint unit_handle; byte cur_col; byte
// cur_row;} in _G_LLM_STRAT_GROUP_MEMBERS[256] (RID_STRAT_GROUP_MEMBERS) -- the group move order's
// member roster (DISTINCT from group_scratch_member/_G_LLM_STRAT_GROUP_MOVE_SCRATCH above -- two
// different arrays in the same order-commit machinery). llm_strat_group_move_order_commit both reads
// and writes it.
using group_member = mh::game::mh_llm_strat_group_member;
// SIM1-G-PREP (2026-08-20). One node {uint tile; ushort dir; ushort unit;} of
// llm_strat_unit_queue_advance_search's two search arrays, _G_LLM_UNITQ_CLOSED[1024]
// (RID_UNITQ_CLOSED) and _G_LLM_UNITQ_FRONTIER[128] (RID_UNITQ_FRONTIER). `tile` is PACKED as
// x*256+y (byte0 = y/row, byte1 = x/col, bytes 2-3 load-bearing zero) so the whole dword doubles as
// the flat index into passable[]/tile_objects[0][]; `dir` is an 8-neighbour direction 0..7.
using unitq_search_node = mh::game::mh_llm_strat_unitq_search_node;
// The map's precomputed torus-wrap masks (`general` @ addr::general). Only width_mask/height_mask
// are read by this closure; the pathfinder_params/pathfinder_workbuf pointers inside it are
// engine-internal and untouched. DISTINCT from width_m/height_m (RID_WIDTH_M/RID_HEIGHT_M) -- a
// different pair of globals at a different address, originally the AI spiral scanners' own wrap
// mask. SIM1B (2026-08-12) added a second independent binding of THAT pair too (sim_view::width_m/
// height_m below) once llm_strat_bldg_connectivity_flood_fill/_check_placement_encloses_neighbors
// turned up reading them directly rather than through `general` -- so "AI's own" is now "AI's own,
// ALSO bound here", not "sim never touches these".
using map_geom = mh::game::mh_llm_strat_map_geom;
// One slot of the localised string table (G_TEXT_PTRS). The ELEMENT is a pointer and it is
// assignable -- llm_strat_scenario_planet_clone replaces one -- while the STRING it addresses is
// read-only. Named rather than written out at the binding so the element type is what a reader
// (and tools/check_state_bindings.py) sees; see sim_store::text_ptr_at().
using text_slot = const wchar_t *;
// A player's storage/dock slots (map::object::unit_storage[8][25]) -- b_index (the roster building
// this slot belongs to), the docked-unit list, and the exit_tile_x/y pair the SIM1C order-enqueue
// handlers read as the default point a launching unit appears at.
using unit_storage = mh::game::mh_map_object_unit_storage;

// SIM1D (2026-08-13). _G_LLM_PROD_SHUTTLE_SLOTS[8][10] (RID_PROD_SHUTTLE_SLOTS) -- the
// interplanetary production/shuttle transfer slot record: transfer status/origin/dest planet,
// travel_duration(_copy), reserved resources/passengers, the planet-bind flag. Already fully typed
// in Ghidra (see mh_structs.gen.h for the full field list); this is its first DLL view-layer
// binding. WRITTEN by this closure (llm_strat_production_complete resets travel_duration/status/
// origin_planet on arrival), so it needs both the const view member below AND a mutable sim_store
// accessor -- same dual-binding shape as units/buildings.
using prod_shuttle_slot = mh::game::mh_llm_prod_shuttle_slot;

// SIM1D (2026-08-13). _G_LLM_STRAT_DIR8_OFFSET_TABLE[8] (RID_STRAT_DIR8_OFFSET_TABLE) --
// a boot-populated (dx,dy) delta pair per compass direction, indexed by the same 8-way direction
// code llm_strat_dir_from_to returns. No writer in the closure. Was `undefined` in Ghidra and read
// as two separate raw tables before this slice retyped it as one 8-entry array (see mh_addrs.gen.h's
// comment for the derivation).
using dir8_offset = mh::game::mh_llm_strat_dir8_offset;

// SIM1D (2026-08-14). _G_LLM_STRAT_DIR_STEP_OFFSET_TABLE[40] -- a SECOND, DISTINCT
// (dx,dy) offset table from dir8_offset above (different address, different element count, same
// {int32,int32} shape by coincidence). Indexed by a building's facing/orientation byte
// (Building[id].field_0x820) in llm_strat_storage_get_approach_tile's elevator-building approach-
// tile calculation. No writer in the closure.
using dir_step_offset = mh::game::mh_llm_vec2i;

// SIM1F (2026-08-18). _G_LLM_STRAT_DIR_REMAP_TABLE[24] -- a per-heading remap row
// {int step_primary; int step_alt1..3;} (16-byte stride) that maps a dir8/heading index to the
// STEP index into dir_step_offsets above. llm_strat_tile_neighbor_reverse_dir reads only
// `.step_primary`, then subtracts dir_step_offsets[step_primary] to step one tile BACKWARD.
// Runtime-populated .bss lookup, no writer in the closure. See mh_structs.gen.h for the field docs.
using dir_remap_row = mh::game::mh_llm_strat_dir_remap_row;

// SIM1-G2 (2026-08-20). _G_LLM_STRAT_MOVE_DIR_TABLE[24] -- one record per move-direction
// table entry (dir_code/opposite_idx/dcol/drow/next_straight/turn_a/turn_b/is_cardinal, 8-byte
// stride). Runtime-populated .bss lookup, no writer in the sim closure. See mh_structs.gen.h for the
// field docs (already fully documented from an earlier session).
using move_dir_step = mh::game::mh_llm_strat_move_dir_step;

// SIM1E (2026-08-15). _G_LLM_STRAT_FACING_TRIG_TABLE[24] -- see
// sim_view::facing_trig_table's comment. `llm_facing_trig` is already a Ghidra type with field
// comments; read them in mh_structs.gen.h rather than re-deriving the off-by-one in the init loop.
using facing_trig = mh::game::mh_llm_facing_trig;

// SIM1E (2026-08-15). _G_LLM_CAM_JUMP_QUEUE (RID_CAM_JUMP_QUEUE) -- the screen-shake
// keyframe list llm_strat_spawn_debris_burst fills. Ghidra types the whole 480-byte region
// `llm_vec2i[60]`, and that is HALF right: the disassembly writes it as TWO PARALLEL ARRAYS OF 30,
// not sixty vec2i. With `i` the loop index and stride `SHL,3`, the burst stores `dx` to
// `[i*8 + 0xa49868]` and `dy` to `[i*8 + 0xa4986c]` (two dwords) but stores an 8-byte
// `FSTP double ptr [i*8 + 0xa49958]` -- and 0xa49958 - 0xa49868 is exactly 30 entries, with
// 0xa49958 + 30*8 landing on the region end. So: `llm_vec2i offset[30]; double scale[30];`.
// sim_store binds the halves separately (cam_jump_offset_at / cam_jump_scale_at) over the one RID
// -- the same one-region-two-bindings shape sim_view::facing_step_offset already uses. Entries are
// written in MIRRORED PAIRS (the loop advances by 2 and writes i's (dx,dy) then i+1's (-dx,-dy)).
using cam_jump_offset = mh::game::mh_llm_vec2i;

// _G_LLM_CAM_JUMP_QUEUE's per-half element count -- see the alias comment above for the derivation
// (480 bytes = 30 * sizeof(llm_vec2i) + 30 * sizeof(double)).
inline constexpr int32_t CAM_JUMP_QUEUE_SLOTS = 30;

// _G_LLM_STRAT_FACING_TRIG_TABLE[24] -- the 24-way heading quantisation this binary uses
// everywhere a facing is stored (15-degree steps; see llm_facing_trig.angle_deg's field comment).
inline constexpr int32_t FACING_TRIG_ENTRIES = 24;

// SIM1D (2026-08-14; opening SIM1E). _G_LLM_STRAT_PROJECTILE_POOL[1500]
// (RID_STRAT_PROJECTILE_POOL) -- the strategic projectile pool. Already fully typed+commented in
// Ghidra (see mh_structs.gen.h). Slot [0]'s `.active` field doubles as the pool's live-count, not a
// real projectile record -- see the struct's own field comment. WRITTEN by
// llm_strat_projectile_tick (both the "current" entry via cur_projectile below, and slot [0]'s
// count on death), so it needs a mutable sim_store accessor alongside the const view member.
using projectile = mh::game::mh_llm_strat_projectile;

// SIM1E third batch (2026-08-14). _G_LLM_STRAT_FX_ANIMS[10000] (RID_STRAT_FX_ANIMS, already
// existed from an AI-domain building-placement-preview binding) -- the fx-anim pool. Already
// fully typed+commented in Ghidra. Slot [0]'s `.live` field doubles as the pool's live-count, not
// a real fx-anim record -- same convention as `projectile` above. WRITTEN by both
// llm_fx_anim_seq_cancel (raw index) and llm_strat_fx_anim_tick (through cur_fx_anim below), so it
// needs a mutable sim_store accessor alongside the const view member.
using fx_anim = mh::game::mh_llm_strat_fx_anim;

// SIM1E fog/sight family (2026-08-16). map::fow::t::tile_coord -- the {x,y,len} run-length entry
// the ten SIGHT_AREA_1..10 tables are made of (see sim_view::sight_area below). Already fully typed
// in Ghidra (category /Manual/map); this is its first DLL view-layer alias.
using map_t_tile_coord = mh::game::mh_map_t_tile_coord;

// SIM1B (2026-08-12). The three building-owned sub-rosters' READ-only aliases -- same types
// sim_store already binds MUTABLY for SIM1C's order dispatch (turret_at/production_at/lab_at); this
// is the "same RID, two legitimate bindings" pattern sim_state.h uses throughout (see profiles/
// population below): llm_strat_bldg_instant_construct_find_slot_enqueue only SCANS these rosters
// for a free slot, so it takes the const view rather than the mutable store.
using mine = mh::game::mh_map_object_mine; // map::object::mine[8][32] -- first sim/AI binding of
                                           // this region (RID_MINES); struct newly dumped 2026-08-12.

// SIM1-G4 (2026-08-22). map::resources[64][64], RID_RESOURCES -- ai/ai_state.h already
// binds this region read-only; this is sim/'s own first binding (read AND, via resources_at() below,
// the first sim-side WRITER -- llm_strat_bldg_completion_dispatch's MINE_EXTRACTING arm).
using map_resources = mh::game::mh_map_resources;
// SIM-RESID-IF (2026-08-31) -- record types the residual-writer bindings need.
// the squad-status blackboard slot; RID_SQUAD_STATUS is 1024 B / 0x10 = 64 of them.
//   tact_state.h binds the same region -- see squad_status_at()'s comment for why two bindings
//   is the sanctioned shape and not the hazard W3 warns about
using squad_status_slot = mh::game::mh_llm_squad_status_slot;
// one slot of the pathfinder's async job-result table (800 B / 8 = 100)
using job_result_entry = mh::game::mh_llm_strat_job_result_entry;
// the screen-fade state machine record
using ui_fade_transition_state = mh::game::mh_llm_ui_fade_transition_state;
// a UI widget record (68 B). Four separate widgets are bound below; they sit on one 68-byte
//   lattice but that is LINKER PACKING of separately-declared statics, not an array -- every
//   access in the binary is an absolute disp32 or a pointer in a widget-list children[] array
using ui_widget = mh::game::mh_llm_ui_widget;
// a UI widget list header
using widget_list = mh::game::mh_llm_ui_widget_list;
// the loaded map's .MP header record
using map_header = mh::game::mh_cfg_struct_map_header;


// SIM1B (2026-08-12). The cfg parser's Building SECTION record (BUILDING @0xe5c9e4) -- only
// `.total` is read here, same posture as ai/ai_state.h's own independent binding of the identical
// RID (cfg_unit_section's sibling comment explains why: a different object from the cfg_building
// TABLE above, which holds per-type records rather than parse state).
using cfg_building_section = mh::game::mh_cfg_static_struct_Building;

// SIM1D (2026-08-13). The cfg parser's Unit SECTION record -- only `.total` is read
// (llm_strat_bldg_side_has_aircraft_producer's inner scan upper bound), same posture as
// cfg_building_section above. SECOND independent binding of the SAME region ai/ai_state.h's own
// `cfg_unit_sec` already binds.
using cfg_unit_section = mh::game::mh_cfg_static_struct_Unit;

// SIM1D (2026-08-13). cfg::final::data::Planets[32] (RID_PLANETS) -- already a region
// (save/hash manifests) and already hand-typed in Ghidra (`cfg_final_struct_Planet`, category
// /Manual/cfg/final/struct, NOT an llm_ type), just never bound into either DLL view layer before.
// llm_strat_production_complete reads exactly one field, `.name` (a G_TEXT_PTRS text id), for the
// off-planet delivery message. No writer anywhere -- boot-loaded cfg data, same posture as
// cfg_units/cfg_buildings/cfg_weapons.
using cfg_planet = mh::game::mh_cfg_final_struct_Planet;

// SIM-RESID-IF re-close (2026-08-31). `Players` (0x00e587e9, RID_PLAYERS) -- the LOBBY-facing
// per-slot descriptor, llm_strat_player_desc[8] stride 0x34. THREE per-player arrays exist in this
// header and they are not interchangeable: `players` (player_data, the sim's own per-player record),
// `profiles` (_G_LLM_STRAT_PLAYERS) and this one. llm_strat_session_begin_multi reads
// controller_flags / race_or_faction / color_or_team / name / scenario_side_id out of it to drive
// the per-slot profile init; the lobby writes it. See sim_view::player_desc_slots.
using player_desc = mh::game::mh_llm_strat_player_desc;

// SIM-RESID-F (2026-09-01). _G_LLM_TUTORIAL_STEPS[16] (RID_TUTORIAL_STEPS) -- one loaded tutorial
// step, 0x60c bytes: `title` and `body` are UTF-16 text (char16_t, the fixed-width spelling of the
// wchar_t Ghidra models them as), then THREE op-lists of four `tutorial_step_op` each --
// `reakcje` (completion conditions), `panel` (UI gate/limit ops) and `komenda` (scripted AI
// commands). Ghidra's own names for the three lists are the Polish section headers from the script
// file the loader parses, so they are kept rather than anglicised. Each op is opcode-at-+0 with an
// UNALIGNED 8-entry operand list at +1; the operands are cfg_enum_E_UNIT_TYPE values, emitted as
// uint32_t. See sim_view::tutorial_steps and sim/resid/sim_tutorial_step_driver.h.
// NAMED `tutorial_step_record`, NOT `tutorial_step`, and not by preference: sim_view already has a
// MEMBER called `tutorial_step` (the int32 step cursor, _G_LLM_GAME_TUTORIAL_STEP), which shadows a
// same-named type inside the class scope -- `const tutorial_step *tutorial_steps;` does not compile.
// `_record` is the writer's own proposed name for the same thing.
using tutorial_step_record = mh::game::mh_llm_tutorial_step;
using tutorial_step_op     = mh::game::mh_llm_tutorial_step_op;

// SIM1F (2026-08-17). _G_LLM_STRAT_LANDING_SPOTS[16] (RID_STRAT_LANDING_SPOTS) -- {x, y,
// status}, status -1 = end-of-list/unused, -2 = taken, spots paired (idx^1). See
// sim_store::landing_spot_at().
using landing_spot = mh::game::mh_llm_strat_landing_spot;

// SIM1A. A (dx,dy) int32 pair -- the REINTERPRETATION of _G_LLM_CURSOR_ANIM_STATES's
// tail 12 entries (index 16..27, RID_CURSOR_ANIM_STATES) at an 8-byte stride instead of the array's
// declared 0x10 entry stride, i.e. a flat 24-entry table indexed directly by facing_target (0..23).
// See sim_unit_fine_pos.h for the full derivation (byte-address cross-check + the aux_symbols.json
// reference-materialization evidence). No Ghidra retype: the struct's own primary meaning at
// entries 0..15 is unchanged, this is a second, narrower view over the same bytes.
struct facing_step_offset_pair {
    int32_t dx;
    int32_t dy;
};

// SIM1-G4 (2026-08-22). llm_strat_ui_base_marker_coord[8] (RID_STRAT_UI_BASE_MARKER_
// COORDS, already registered MF_SAVE-only, 64 bytes = 8 entries of 8 bytes each) -- one camera-pan
// target column/row pair per base-marker slot. llm_strat_bldg_completion_dispatch's MOTHER-arrival
// branch writes index [0] (cam_col/cam_row, torus-wrapped by general's width/height masks); no
// other writer in this closure, no sim_view read-only sibling.
struct ui_base_marker_coord {
    int32_t cam_col;
    int32_t cam_row;
};

// The map occupancy plane (map::tile_object_data[256][256], RID_TILE_OBJECTS), 8 bytes/tile --
// {flags, building, unit, class_owner, visibility}. Same type ai_state.h aliases as `tile_object`;
// duplicated here for the same reason as `order`/`unit`/`building` above (libmh/sim/ does not depend on
// libmh/ai/). SIM1A's llm_strat_unit_attack_target_is_dead is the first sim reader (ATTACK_BUILDING
// arm: is there still a building on the target tile); llm_unit_create_soldier (SIM1A)
// is the first WRITER in the 307-function closure -- see sim_store::tile_object_at().
using tile_object = mh::game::mh_map_tile_object_data;

// SIM1A (2026-08-10). A control group's membership record (_G_LLM_STRAT_CTRL_GROUPS[10],
// RID_STRAT_CTRL_GROUPS): {count, unit_ids[200]}.
using ctrl_group = mh::game::mh_llm_strat_ctrl_group;

// SIM1A. One frame of the damage-smoke Anim chain (Anim[2501], RID_ANIM):
// {sprite_id, next, time}.
using anim_frame = mh::game::mh_cfg_final_struct_Anim;

// SIM1A. One soldier record in a unit's mounted squad (_G_LLM_STRAT_SOLDIERS[8][100],
// RID_STRAT_SOLDIERS). Same type llm_strat_unit_passive_engage.h's sibling family reads; duplicated
// per the same libmh/sim/-does-not-depend-on-libmh/ai/ rule as everything else in this block.
using soldier = mh::game::mh_llm_strat_crew_soldier;

// SIM1A. Per-player housing caps/used-counts (_G_LLM_STRAT_UNIT_HOUSING_STATS,
// RID_STRAT_UNIT_HOUSING_STATS) -- SAME type + RID ai_state.h's `housing_stats` alias binds
// independently (precedented second-binding, matching sim_state.h's own order-queue horizon note).
using housing_stats = mh::game::mh_llm_strat_unit_housing_stats;

// SIM1A. The original per-unit-state dispatch table (_G_LLM_STRAT_UNIT_STATE_FUNCS,
// RID_STRAT_UNIT_STATE_FUNCS): llm_strat_unit_tick calls through it by unit.state as an index.
// Dispatched-through only -- never resolved, inlined, or renamed.
using unit_state_fn = void (*)();

// SIM1B building_tick promotion-oracle session (2026-08-13, G19). The three original per-building
// dispatch tables llm_strat_building_tick/_bldg_tick_animation_state call through, same shape as
// unit_state_fn above -- dispatched-through only, never resolved/inlined/renamed:
//   bldg_state_fn: _G_LLM_STRAT_BLDG_STATE_FUNCS[255],  RID_STRAT_BLDG_STATE_FUNCS, by building.state
//   bldg_done_fn:  _G_LLM_STRAT_BLDG_DONE_FUNCS[100],   RID_STRAT_BLDG_DONE_FUNCS,  by building.building_id
//   bldg_tick2_fn: _G_LLM_STRAT_BLDG_TICK2_FUNCS[100],  RID_STRAT_BLDG_TICK2_FUNCS, by building.building_id
using bldg_state_fn = void (*)();
using bldg_done_fn  = void (*)();
using bldg_tick2_fn = void (*)();

// ---- SIM1-DISPATCH (2026-08-22): the two state tables, WRITABLE ---------------------------------
//
// Every member above hands out a CONST pointer because every translated body only ever dispatches
// through these tables. `sim_register_state_handlers.cpp` is the one caller that must WRITE them --
// owning the dispatch layer means replacing the game's registrar with one that installs OUR handler
// pointers -- and W2's rule (no game-state address reaches a sim translation; enforced by
// tools/check_sim_addresses.py) means that TU cannot resolve the regions itself.
//
// So the single binder hands them over, and only for that. This is deliberately NOT a sim_store
// accessor: sim_store's contract is "one RECORD by reference, no address escapes", which exists so a
// translated body cannot cache a roster base across an ST2 rebase. A dispatch table is the opposite
// kind of object -- it is written ONCE at boot, as a whole, by one installer that is not a
// translated body at all -- so forcing it through that contract would mean 255 single-slot calls and
// two more positional arguments on a 150-parameter constructor, for a weaker guarantee.
//
// `*_slots` is the LIVE region size in entries, read from the registry rather than assumed: the
// registrar refuses to install if a region is smaller than the table it is about to write, so an
// under-sized or unrebased region is a loud refusal instead of 1 KB of collateral .bss damage.
struct sim_state_tables {
    unit_state_fn *unit;       // _G_LLM_STRAT_UNIT_STATE_FUNCS, RID_STRAT_UNIT_STATE_FUNCS
    bldg_state_fn *bldg;       // _G_LLM_STRAT_BLDG_STATE_FUNCS, RID_STRAT_BLDG_STATE_FUNCS
    int32_t        unit_slots; // live_size(RID) / sizeof(unit_state_fn)
    int32_t        bldg_slots; // live_size(RID) / sizeof(bldg_state_fn)
};

// Resolve both dispatch tables for writing. Defined in sim_state.cpp -- the only file under libmh/sim/
// permitted to name the region registry.
sim_state_tables writable_state_tables();

// ---- SIM1-BLDGCB (2026-08-23): the two per-building-TYPE callback tables, WRITABLE ---------------
//
// The exact sibling of sim_state_tables above, for the SECOND fn-ptr registry -- the one
// llm_strat_register_bldg_type_callbacks fills and the one the state-table fold structurally could
// not see. Same reasoning applies verbatim: written ONCE at boot, as a whole, by an installer that
// is not a translated body, so it is a free binder rather than a sim_store accessor.
//
// The ONE addition over its sibling is `cfg` / `cfg_count`. This registrar's setters do not index
// by type: they SCAN, matching each building id's cfg type byte against the type argument (see
// addr/mh_bldg_type_callbacks.gen.h). The cfg table is therefore an INPUT to the fill, not a
// consumer of it, and it is handed over here for the same reason the tables are -- so the registrar
// can be driven over local arrays by the offline oracle without either side naming an address.
struct sim_bldg_callback_tables {
    bldg_done_fn       *done;        // _G_LLM_STRAT_BLDG_DONE_FUNCS,  RID_STRAT_BLDG_DONE_FUNCS
    bldg_tick2_fn      *tick2;       // _G_LLM_STRAT_BLDG_TICK2_FUNCS, RID_STRAT_BLDG_TICK2_FUNCS
    const cfg_building *cfg;         // Building[], RID_BUILDING -- the scan's input, READ-only
    int32_t             done_slots;  // live_size(RID) / sizeof(bldg_done_fn)
    int32_t             tick2_slots; // live_size(RID) / sizeof(bldg_tick2_fn)
    int32_t             cfg_count;   // live_size(RID_BUILDING) / sizeof(cfg_building)
};

// Resolve both per-building-type callback tables (and the cfg table they scan) for writing.
// Defined in sim_state.cpp, for the same reason as writable_state_tables().
sim_bldg_callback_tables writable_bldg_callback_tables();

// ---- the four sub-rosters the order dispatcher writes (SIM1C, llm_strat_order_queue_dispatch) ----
// Each hangs off a BUILDING rather than standing alone: the dispatcher reaches them as
// <roster>[player][building.sub_id], never by an index carried in the order.
using turret     = mh::game::mh_map_object_turret;     // [MAX_PLAYERS][TURRETS_PER_PLAYER]
using production = mh::game::mh_map_object_production; // [MAX_PLAYERS][PRODUCTIONS_PER_PLAYER]
using lab        = mh::game::mh_map_object_lab;        // [MAX_PLAYERS][LABS_PER_PLAYER]

// The player PROFILE table -- llm_strat_player_profile[8] @ RID_STRAT_PLAYERS, stride 0x740.
// A DIFFERENT ARRAY from `player_data` (@ RID_PLAYER_DATA, stride 0x288fc) which sim_view::players
// already carries, and the two are both reasonably called "players": mixing them up reads arbitrary
// memory at a plausible-looking offset. Named `profiles` here so no call site has to remember which
// `players` it meant. Same distinction orders/order_queue.h documents for its own `players`.
using player_profile = mh::game::mh_llm_strat_player_profile;

// The per-player population counters (_G_LLM_STRAT_POP_STATS, 416 bytes).
using pop_stats = mh::game::mh_llm_strat_pop_stats;

// One step of the unit turn/move animation: {x_off, y_off, facing}, THREE bytes.
// _G_LLM_STRAT_MOVE_MICROSTEPS is indexed [move_heading][move_microstep] with a 0x60 row stride, so
// 32 entries per heading -- see the addr-manifest note, which also records that Ghidra types the
// symbol as ONE row ([1][32], 96 bytes) while the index expression reaches row `move_heading`.
using move_microstep = mh::game::mh_llm_strat_move_microstep;
// SIM1-H wave 2: the 13x13 obstacle-proximity stencil entry (one byte, `bit_index`, plus padding to
// a 4-byte stride). See sim_view::proximity_stencil.
using proximity_stencil_entry = mh::game::mh_llm_map_proximity_stencil_entry;

// The order RECORD, 0x44 bytes. Aliased here for the same two `using` lines' worth of duplication as
// unit/building above: libmh/sim/ deliberately does not depend on libmh/orders/ (see sim_order_enqueue.h's
// note on why even the container CALLS go through `mh::call::` rather than mh::orders directly).
using order = mh::game::mh_llm_strat_order;

// ---- extents ------------------------------------------------------------------------------
inline constexpr int32_t MAX_PLAYERS = 8; // units[8][..], buildings[8][..], player_data[8]
// SIM1F (2026-08-17). _G_LLM_MAP_REGION_GRID[256][256]'s per-axis extent -- same
// 256x256 plane `tile_at()`'s `(tile_x << 8) | tile_y` indexes, named here rather than shifted
// since region_cell_at() is a plain row*dim+col computation, not a bit-packed one.
inline constexpr int32_t MAP_GRID_DIM = 256;
// SB-BIND T2 (2026-09-06): these are the STOCK values and nothing indexes with them any more.
// Production index expressions read the DERIVED capacities off the view/store (`v.caps.units`,
// `caps_.buildings`, ...), which follow whatever the host bound; these survive only as the offline
// fixtures' default and as the documentation of the stock strides. Adding a new production use of
// one is a regression -- it would pin that one site to 100 while every other site followed a
// cap-raised host, which is worse than uniform staleness. See state/roster_caps.h.
inline constexpr int32_t UNITS_PER_PLAYER     = mh::state::STOCK_ROSTER_CAPS.units;     // stride 0x5b04 / 0xe9
inline constexpr int32_t BUILDINGS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.buildings; // stride 0x6aa4 / 0x111
inline constexpr int32_t STORAGE_PER_PLAYER   = mh::state::STOCK_ROSTER_CAPS.storage;   // stride 0x17d4 / 0xf4 (RID_UNIT_STORAGE, 48800 = 8*25*0xf4)
inline constexpr int32_t SOLDIERS_PER_PLAYER  = mh::state::STOCK_ROSTER_CAPS.soldiers;  // stride 0x1d (RID_STRAT_SOLDIERS, 23200 = 8*100*0x1d)
// SIM1-G1 (2026-08-19). _G_LLM_STRAT_PATH_BUFFERS[240000] = 8 players * 100 path slots * 300
// waypoints -- indexing arithmetic read off move_walker/group_marshal: player*30000 + slot*300 + cursor.
inline constexpr int32_t PATH_WAYPOINTS_PER_SLOT   = 300;
inline constexpr int32_t PATH_SLOTS_PER_PLAYER     = 100;
inline constexpr int32_t PATH_WAYPOINTS_PER_PLAYER = PATH_SLOTS_PER_PLAYER * PATH_WAYPOINTS_PER_SLOT; // 30000

// The three building-owned sub-rosters. Row strides read off the dispatcher's own index arithmetic
// (`IMUL <row>,player,<stride>`), which is the authority -- the region sizes agree: turrets 14080 =
// 8*32*0x37, productions 25920 = 8*8*0x195, labs 1600 = 8*25*8.
inline constexpr int32_t TURRETS_PER_PLAYER     = mh::state::STOCK_ROSTER_CAPS.turrets;     // row stride 0x6e0 @0x0046780d
inline constexpr int32_t PRODUCTIONS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.productions; // row stride 0xca8 @0x004685d4
inline constexpr int32_t LABS_PER_PLAYER        = mh::state::STOCK_ROSTER_CAPS.labs;        // row stride 0xc8  @0x0046898e

// SIM1B (2026-08-12). mines[8][32], stride 0x38 (56 = sizeof(mine)) -- 14336 = 8*32*56. NOT one of
// the "three building-owned sub-rosters" above (no dispatcher writes it in this closure); named
// here anyway since it shares their per-player-array shape.
inline constexpr int32_t MINES_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.mines; // row stride 0x700 @0x0046d5cb (find_slot_enqueue)

inline constexpr int32_t PROD_SHUTTLE_SLOTS_PER_PLAYER = 10; // RID_PROD_SHUTTLE_SLOTS, 80 = 8*10; matches
                                                             // llm_strat_production_complete's own
                                                             // `player*10 + slot` indexing.

// SIM1E fog/sight family (2026-08-16). fog_of_war::visible_by_count is byte[256][256][8] (tile-major,
// one byte per player per tile) -- see sim_store::fog_visible_by_count_at()'s comment for why this is
// NOT the same dimension order mh_structs.gen.h's committed `mh_map_struct_fog_of_war::visible_by_count`
// field declares. Same value/name ai/ai_state.h's own FOG_PLAYERS_PER_TILE uses for its independent
// read-only binding of the same region; duplicated here per this file's own libmh/sim/-does-not-depend-
// on-libmh/ai/ rule.
inline constexpr int32_t FOG_PLAYERS_PER_TILE = 8;

// _G_LLM_STRAT_MOVE_MICROSTEPS is [heading][step] with a 0x60 row stride over a 3-byte entry.
inline constexpr int32_t MICROSTEPS_PER_HEADING = 32; // 0x60 / sizeof(move_microstep)

// The order queue's capacity, mirrored from orders/order_queue.h's QUEUE_CAP rather than shared:
// see the `order` alias above on why libmh/sim/ does not include libmh/orders/. Only the DISPATCHER needs
// it here, and only as the extent of the region it walks.
inline constexpr int32_t ORDER_QUEUE_CAP = 300;

// _G_LLM_STRAT_DIR_STEP_OFFSET_TABLE[40] -- see sim_view::dir_step_offsets's comment.
inline constexpr int32_t DIR_STEP_OFFSET_COUNT = 40;

// _G_LLM_STRAT_PROJECTILE_POOL[1500] -- see sim_view::projectile_pool's comment; slot 0's `.active`
// doubles as the pool's live-count.
inline constexpr int32_t PROJECTILE_POOL_CAP = 1500;

// A unit INSTANCE mounts four weapon slots (unit::weapons[4], stride 0x13). Distinct from the four
// slots of the unit's cfg TYPE record below -- same count, different array, different record.
inline constexpr int32_t UNIT_WEAPON_SLOTS = 4;
// cfg_unit::weapons is a (id, enabled) BYTE PAIR per slot, four slots. The generated header renders
// it flat as `uint8_t weapons[8]` because the sub-record type is not applied in Ghidra, so index it
// with cfg_unit_weapon_id() / cfg_unit_weapon_enabled() below rather than by hand -- the flattening
// is exactly the sort of detail a translation gets wrong once and never notices.
inline constexpr int32_t CFG_UNIT_WEAPON_SLOTS = 4;

// cfg_unit::resource / cfg_building::resource are SEVEN (id, val) pairs, not four: every walker in
// the image bounds at 7 and stops early on `.id == 0`. The cfg parser only ever fills four because
// the section grammar exposes four keywords -- the array is still seven and entries 4..6 read back
// zero, which is what terminates the walk. Reproduce the bound, not the parser's fill.
inline constexpr int32_t CFG_RESOURCE_SLOTS = 7;

// SIM1B (2026-08-12). `player_resources` (RID_PLAYER_RESOURCES) is int[MAX_PLAYERS][10], row-major,
// indexed by a cfg_enum_E_RESOURCE id -- the per-player HOLDINGS a build/cycle-cost check compares
// against. NOT the same width as CFG_RESOURCE_SLOTS above (that bounds a cfg record's (id,val) pair
// list; this is the row stride of the holdings table itself) -- same distinction ai_state.h's own
// comment on `player_resources` draws (0x58 stride there is a DIFFERENT array, resource_spend_total;
// this one is 10 ints/row). Same RID ai/ai_state.h already binds independently.
inline constexpr int32_t PLAYER_RESOURCE_SLOTS = 10;
// SIM1B (2026-08-12). `progress`'s per-player row width -- see the `player_progress` alias above.
inline constexpr int32_t PROGRESS_ROW_COUNT = 300;
// SIM1F. AvailableBuildings[8][50] / AvailableProjects[8][2][50] (int32 slots), derived
// from each region's byte size (1600 / 3200) over MAX_PLAYERS -- see sim_store::available_buildings_row/
// available_projects_bucket's comments.
inline constexpr int32_t AVAILABLE_BUILDINGS_ROW_INTS   = 50;
inline constexpr int32_t AVAILABLE_PROJECTS_TYPES       = 2;
inline constexpr int32_t AVAILABLE_PROJECTS_BUCKET_INTS = 50;

// player_data::resource_spend_total is int[8], indexed directly by resource id (there is no id-0
// resource; slot 0 is simply never accumulated into). Both spawn paths initialise all eight to 1,
// not 0, because it is used as a denominator elsewhere.
inline constexpr int32_t RESOURCE_SPEND_TOTAL_SLOTS = 8;

// The two spend rings are logically int[32][4] -- 32 slots selected by player_data::
// ai_spend_ring_cursor, each holding four per-resource ints -- declared flat as int[128] because
// the field grammar has no nested-array form.
inline constexpr int32_t SPEND_RING_SLOTS         = 32;
inline constexpr int32_t SPEND_RING_RESOURCE_COLS = 4;

// building::state == 4 is RUBBLE_SIGHT_DECAY, the value llm_strat_bldg_is_alive rejects alongside
// a non-positive energy. Read off the CMP at 0x004d3d90; the enum member name is Ghidra's own.
inline constexpr uint16_t BLDG_STATE_RUBBLE_SIGHT_DECAY = 4;

// SIM1B building_tick promotion-oracle session (2026-08-13, G19). building::state == 3 is
// DISMANTLE_FINISH, the state llm_strat_building_tick's runaway-loop guard forces after 10000
// unproductive dispatch iterations; also read off llm_strat_set_bldg_state_handler's committed
// llm_strat_bldg_state parameter type (Ghidra's own enum member name).
inline constexpr uint16_t BLDG_STATE_DISMANTLE_FINISH = 3;

// SIM1D (2026-08-14). building::state == 0x7d is DEPLOY_START ("departed/in-transit"),
// the state llm_prod_bldg_depart_finalize sets on A_MOTHER/A_SHUTTLE/H_MOTHER/H_SHUTTLE departure
// (no unit-conversion path, unlike A_PORT/H_PORT). Value/name from the Ghidra .c draft's own
// resolved llm_strat_bldg_state enum-member token, hoisted here from the translation unit's local
// definition per this file's own established sibling constants above.
inline constexpr uint16_t BLDG_STATE_DEPLOY_START = 0x7d;

// cfg_weapon::target is a bitmask of what the weapon may shoot at, per llm_strat_target_class's
// ground=1 / air=2 scheme.
inline constexpr uint8_t WEAPON_TARGET_GROUND = 0x1u;
inline constexpr uint8_t WEAPON_TARGET_AIR    = 0x2u;

// Several sim entry points take a PACKED object reference whose low nibble is the owning player;
// the callers pack other bits into the high nibble and every consumer masks before indexing.
inline constexpr uint32_t REF_OWNER_MASK = 0xfu;
inline uint32_t           ref_owner(uint32_t ref) { return ref & REF_OWNER_MASK; }

// ---- the READ view --------------------------------------------------------------------------
//
// Every member is a pointer-to-const, and W1 above is about which members are HERE and not also in
// sim_store: a region that appears only here is one this subsystem was measured never to write.
// Adding a member here that the sim writes is not a style slip, it is a false claim -- put it in
// sim_store and let the write be an accessor call.
//
// Grown demand-driven, one slice at a time, exactly as ai_view was. Do not pre-populate it from
// the region list.
struct sim_view {
    // ---- the rosters. Written by the sim (see sim_store), read here. ----
    const unit        *units;     // [MAX_PLAYERS][UNITS_PER_PLAYER], row-major
    const building    *buildings; // [MAX_PLAYERS][BUILDINGS_PER_PLAYER], row-major
    const player_data *players;   // [MAX_PLAYERS], stride 0x288fc

    // SB-BIND T2: the per-player ROW CAPACITIES, derived from the sizes the host bound rather than
    // from the compile-time constants below. Defaults to the stock values so a hand-built fixture
    // view keeps working unchanged; state() overwrites it with live_roster_caps(). Read these --
    // never UNITS_PER_PLAYER and friends -- in any index expression, or a cap-raised host resolves
    // half our reads into the wrong player's row.
    mh::state::roster_caps caps{mh::state::STOCK_ROSTER_CAPS};

    // ---- the CFG tables. Loaded from the game's cfg files at boot and read-only thereafter, so
    // these are W1's load-bearing members: a sim function writing one is always a bug. ----
    //
    // THE MEASURED EXCEPTION, RESOLVED (2026-08-08 open question, closed SIM1F
    // 2026-08-16): `game_HandleUpgrade` IS in the sim ledger and DOES write `Unit`/`Weapon` in
    // place -- it applies a researched upgrade's per-player stat delta directly into the cfg TYPE
    // tables (step_speed/turn_speed/armor_prob on Unit; range_min/range_max/missing/power/fire_range
    // on Weapon), the FIRST writer to either table in the whole 307-function sim closure. So "cfg is
    // frozen after boot" was never quite true -- it holds for every OTHER reader, which is why these
    // two stay `const` here and get a sim_store mutable accessor instead (unit_mut()/weapon_mut()
    // below) rather than losing their const here. `cfg_buildings` still has NO writer.
    const cfg_unit     *cfg_units;     // Unit[100],     indexed by unit::unit_proto_id
    const cfg_building *cfg_buildings; // Building[100], indexed by building::building_id
    const cfg_weapon   *cfg_weapons;   // Weapon[32],    indexed by a weapon slot's id
    // SIM1F (2026-08-16). Upgrades[99] (RID_UPGRADES), indexed by upgrade_id. Read-only
    // (no writer in this closure) -- game_HandleUpgrade's own driving table.
    const cfg_upgrade *cfg_upgrades;

    // SIM1F (2026-08-16). game_HandleUpgrade's own boot constants -- see the addr
    // manifest entries for the read-memory-confirmed values (100.0 / 100.0, two INDEPENDENT storage
    // locations despite the identical value, per the established SUBTICK_POS/NEG precedent).
    const double *upgrade_unit_speed_pct_divisor; // _G_LLM_STRAT_UPGRADE_SPEED_PERCENT_DIVISOR
    const double *upgrade_weapon_pct_divisor;     // const::_100f (RID_100F)
    // The five raw-address wide-char literals game_HandleUpgrade concatenates into its summary
    // message. Single wchar16 CHARACTERS (not strings), read-memory-confirmed this slice: ' ', ':',
    // ' ', ',', ')' respectively -- see each region's own addr-manifest comment.
    const wchar_t *upgrade_msg_sep_before_category; // _G_LLM_STRAT_UPGRADE_MSG_SEP_BEFORE_CATEGORY
    const wchar_t *upgrade_msg_sep_before_name;     // _G_LLM_STRAT_UPGRADE_MSG_SEP_BEFORE_NAME
    const wchar_t *upgrade_msg_clause_sep_first;    // _G_LLM_STRAT_UPGRADE_MSG_CLAUSE_SEP_FIRST
    const wchar_t *upgrade_msg_clause_sep_next;     // _G_LLM_STRAT_UPGRADE_MSG_CLAUSE_SEP_NEXT
    const wchar_t *upgrade_msg_trailer;             // _G_LLM_STRAT_UPGRADE_MSG_TRAILER

    // SIM1F (2026-08-16). llm_strat_invasion_chance_roll's building_completed=0 arm base
    // offset, double, 300.0, read-memory-confirmed (was undefined/1-byte, retyped this slice).
    const double *invasion_roll_base_time; // _G_LLM_STRAT_INVASION_ROLL_BASE_TIME

    // SIM1F (2026-08-16). game_HandleInvasion's own-planet spawn-failure arm passes this
    // to llm_strat_player_presence_lost. Distinct from player_side (PlayerSide) above and from
    // _G_LLM_NET_LOCAL_PLAYER_SLOT (the injected MP stack's own local-slot int, a different address).
    const uint16_t *local_player_slot; // _G_LLM_STRAT_LOCAL_PLAYER_SLOT

    // SIM1F (2026-08-17). llm_strat_dir_from_to's own boot-constant block -- a THIRD
    // independent copy of the trig-to-sector idiom llm_strat_facing24_from_points already uses
    // (24-way, 15-degree sectors -- read-memory-confirmed this slice; the function's own plate
    // comment previously and WRONGLY claimed an 8-way octant, corrected in Ghidra this slice).
    const double *dir24_rad2deg_num;
    const double *dir24_rad2deg_den;
    const double *dir24_half_turn_deg;
    const double *dir24_bias_deg;
    const double *dir24_wrap_add_deg;
    const double *dir24_wrap_limit_deg;
    const double *dir24_wrap_sub_deg;
    const double *dir24_sector_deg;

    // SIM1F (2026-08-17). llm_strat_dir_sector_to's own boot-constant block -- a
    // 128-way heading (2.8125-degree sectors), already Ghidra-named `_G_LLM_STRAT_DIR128_*` from an
    // earlier session; this slice adds the MF_VIEW binding.
    const double *dir128_rad2deg_num;
    const double *dir128_rad2deg_den;
    const double *dir128_half_turn_deg;
    const double *dir128_bias_deg;
    const double *dir128_wrap_add_deg;
    const double *dir128_wrap_limit_deg;
    const double *dir128_wrap_sub_deg;
    const double *dir128_sector_deg;

    // sim_resid batch E. _G_LLM_STRAT_BLDG_COUNT_OFFLINE_DECREMENT
    // (RID_STRAT_BLDG_COUNT_OFFLINE_DECREMENT, 0x0050160e, double, OWN_READONLY). The amount
    // llm_strat_bldg_is_network_critical adds to the roster's SENTINEL slot-0 `.energy` headcount at
    // 0x00497522 to book-keep "one fewer live building" while it simulates the candidate offline.
    // The whole image has EXACTLY ONE referrer (ReVa find-cross-references: totalToCount 1, isRead,
    // from 0x00497522) and the stored value is exactly -1.0, which is what makes the function's
    // later unconditional FLD1/FADD at 0x004975df an exact inverse.
    const double *bldg_count_offline_decrement;

    // sim_resid batch E. Three more read-only boot constants, each already
    // Ghidra-named+typed and each registered as a region (OWN_READONLY, MF_VIEW):
    //   * unit_energy_status_percent_scale / bldg_energy_status_percent_scale
    //     (_G_LLM_STRAT_UNIT_ENERGY_STATUS_PERCENT_SCALE @0x00500f40 and its building twin
    //     @0x00500f48) -- llm_strat_bldg_gather_nearby_squad_status scales a unit's / a building's
    //     energy into the squad-status percentage with them.
    //   * ai_base_spawn_timer_stagger_scale (_G_LLM_STRAT_AI_BASE_SPAWN_TIMER_STAGGER_SCALE
    //     @0x00506d7c) -- read three times by llm_strat_spawn_ai_base (FMUL at 0x004dce5f /
    //     0x004dce74 / 0x004dce89). DISTINCT from ai_invasion_clock_stagger (0x00506da0) and
    //     ai_clock_stagger_fraction (0x00506da8): three constants, three sole referrers, do not fold.
    const double *unit_energy_status_percent_scale;
    const double *bldg_energy_status_percent_scale;
    const double *ai_base_spawn_timer_stagger_scale;

    // sim_resid batch E. _G_LLM_STRAT_AI_CFG_MINE_WORTH (RID_STRAT_AI_CFG_MINE_WORTH,
    // 0x0066936c, int). A SECOND independent binding of a region ai_state.h already binds under the
    // same name -- that header's comment names llm_strat_spawn_ai_base as one of its two readers, and
    // this is the read side only (both bindings are const), so there is no write to move out from
    // under either one.
    const int32_t *mine_worth;

    // SIM1F (2026-08-16). Projects[100] (RID_PROJECTS), indexed by cfg_t_project_id --
    // invention/name/icon/type/resource[7]/build_time. Same boot-loaded, no-writer-in-closure posture
    // as cfg_units/cfg_buildings above. Was MF_SAVE-only (the save format walks it too); this slice
    // adds the MF_VIEW claim. game_TryStartProject/llm_cfg_apply_project_resources/
    // game_AddProjectToAvailable are the readers.
    const cfg_project *cfg_projects; // Projects[100], indexed by cfg_t_project_id
    // SIM1F. Progress[300] -- see the `cfg_invention` alias comment above. Read-only,
    // boot-loaded cfg data. game_HandleProgress's `Progress[inv].type`/`.index` dispatch.
    const cfg_invention *cfg_inventions;

    // SIM1A. Per-sprite metadata (see sprite_meta_entry above), indexed by
    // Unit[proto].sprite + facing + a fixed per-caller offset. Same load-bearing const posture as
    // the cfg tables above (boot-loaded, no writer in the closure).
    const sprite_meta_entry *sprite_meta;

    // SIM1A; REBASED LT1E (2026-09-02). The per-facing_target pixel-delta table (see
    // facing_step_offset_pair above), entries 1..24 written once at boot by
    // llm_strat_unit_facing_offset_init (slot 0 never written), indexed DIRECTLY by facing_target.
    // Read by llm_strat_unit_get_coords/_calc_fine_axis_pos/_calc_render_fine_y/
    // _calc_interp_pixel_pos and (LT1E) facing_step_apply. Now bound at its OWN region
    // RID_STRAT_FACING_STEP_SIGN (@0xae4768, 200 B = 25 pairs incl. the unwritten slot 0) -- the old
    // RID_CURSOR_ANIM_STATES+0x100 alias pointed one past that region since its v312 shrink to 256 B.
    const facing_step_offset_pair *facing_step_offset;

    // LT1 lib_trans const singles (2026-09-02), all boot/image constants with no writer:
    // rand_state_advance's return normalization (65535.0 @RID_STRAT_RNG_NORM_DIVISOR); the
    // invasion-alert re-emit interval (25.0); llm_math_scale_pct's percent divisor (100.0); and the
    // 7x7 deploy formation stencil (byte[49] .rodata -- the decompiler's memcpy rendering drops its
    // 49th cell, the asm copies all 49).
    const double  *rng_norm_divisor;
    const double  *invasion_alert_interval;
    const double  *math_percent_divisor;
    const uint8_t *deploy_formation_stencil;

    // SIM1-G4 (2026-08-22). llm_strat_bldg_sprite_anchor_offset's own pair: a per-frame
    // BYTE offset table (RID_SPRITE_PIX_OFFSETS, parallel to sprite_meta -- same frame-index lookup)
    // added to the live pixel-bank base (RID_GFX_BANK_PIXELS, a pointer VALUE stored at a fixed VA --
    // same "pointer-to-pointer, dereference at use" shape as region_list_head above) to reach a
    // frame's gfx_sprite_hdr (the sprite-format notes: x_left@4/y_top@6/width@8/height@0xA, all uint16_t).
    // Boot-loaded by llm_gfx_load_banks, no writer in the sim closure.
    const uint32_t       *sprite_pix_offsets;
    const uint8_t *const *gfx_bank_pixels;

    // ---- the map extents, written by the map loader and by nothing in this closure. ----
    const int32_t *map_width;
    const int32_t *map_height;

    // SIM1F (2026-08-16). _G_LLM_MAP_ZOOM_SCALE_X (RID_MAP_ZOOM_SCALE_X), the map
    // horizontal zoom scale, boot-loaded double, no writer anywhere. llm_map_zoom_scale_x_get's
    // sole field.
    const double *zoom_scale_x;

    // SIM1F (2026-08-17). The map-region graph's own state -- see the `llm_map_region`
    // alias comment above for the heap-node caveat.
    //
    // _G_LLM_MAP_REGION_LIST_HEAD (RID_MAP_REGION_LIST_HEAD) -- the ACTIVE region list head, a
    // pointer VALUE stored at a fixed VA (distinct from the FREE list head -- which, since LT1C
    // brought the allocator itself into the closure, is now bound too: see
    // sim_store::region_pool_free_head()). This view binding stays read-only:
    // llm_map_merge_small_regions reads the current head then walks `.next` itself. The head slot IS
    // reassigned by the LT1C pool bodies -- through the mutable twin
    // sim_store::region_list_head_mut(), not through this member.
    // Modeled as a pointer-TO-pointer (matching `text_ptrs`'s shape) so `*v.region_list_head` reads
    // the live value on every call, never cached.
    llm_map_region *const *region_list_head;
    // _G_LLM_MAP_REGION_MERGE_THRESHOLD (RID_MAP_REGION_MERGE_THRESHOLD) -- the cell-count floor
    // below which llm_map_merge_small_regions treats a region as a merge candidate. NOT a boot
    // constant baked into the image (read-memory on the static file errors -- no initial value);
    // set at runtime, most likely map-load time. Read-only in this closure regardless.
    const uint32_t *region_merge_threshold;

    // ---- the group-move member scratch (see sim_group_scratch_centroid.h). ----
    const group_scratch_member *group_move_scratch;

    // ---- SIM1-G1 (2026-08-19): the per-unit compressed path buffers (see path_waypoint alias). ----
    // Read by llm_strat_unit_state_group_marshal (.run_length) and move_walker (.heading/.run_length);
    // sim_store::path_buffer_at() is the mutable sibling (move_walker decrements .run_length).
    const path_waypoint *path_buffers;

    // ---- SIM1-G1 (2026-08-20): unit_group_step_ground's heading/turn-delta lookups. ----
    // Both already named+typed in Ghidra; written only by the original, out-of-scope
    // llm_strat_group_step_heading_table_init -- read-only in this closure (see the alias comments).
    const heading_slot *heading_candidates;       // _G_LLM_STRAT_HEADING_CANDIDATE_TABLE[72]
    const int32_t      *group_step_heading_remap; // _G_LLM_STRAT_GROUP_STEP_HEADING_REMAP[24]

    // ---- SIM1-G1 (2026-08-20): an air/ground pathfinder-mode selector. It is a
    // BOOLEAN, and the values are not what this comment said until SIM1-G-PREP re-derived them from
    // the reference set (2026-08-20): llm_strat_unit_group_step_ground writes 0 (0x00483034);
    // llm_strat_unit_group_step_plane writes 1 before its pathfind (0x00484b66) and clears it back
    // to 0 after (0x00484d77); llm_strat_trace_greedy_path READS it as `CMP dword ptr [...],0x0`
    // (0x0066afc1) and branches. Two earlier claims -- "ground 1 / plane 2" here, and "1 for both"
    // from a translator report -- were BOTH wrong. See sim_store::pathfinder_air_mode_flag().
    const int32_t *pathfinder_air_mode_flag;

    // ---- SIM1-G1 (2026-08-20): llm_strat_squad_pick_free_formation_anchor's two inputs.
    // Both read-only in this closure. REBASED 2026-09-02 (LT0): the old byte[248] "anchor table
    // raw" @0xae3640 was an INTERIOR view (+0x28 = cell row0/col5) of the squad-placement offset
    // table @0xae3618 -- llm_squad_placement_offset[6][6], 288 B, row stride 0x30, col stride 8;
    // written once at boot stage 4 by llm_strat_squad_placement_offset_set (the LT-PREP dig that
    // settled this refuted both the old symbol's "no original accessor" claim and its "turn
    // duration" reading). Still exposed as raw bytes: the col-5 column this closure reads sits at
    // 0x28 + row*0x30 (+0 = x-ish, +4 = y-ish; field semantics per the Ghidra struct comments).
    const uint8_t                        *squad_placement_offset_table; // byte[288] = llm_squad_placement_offset[6][6]
    const squad_formation_anchor_scratch *squad_anchor_scratch;         // [5]

    // ---- SIM1-G1 (2026-08-20): llm_strat_group_move_order_commit's inputs/outputs. ----
    // Read+written by that function (see its own .cpp for the exact split) -- mutable siblings are
    // sim_store::group_route_steps_mut()/group_members_mut()/group_order_goal_x() etc below.
    const route_step   *group_route_steps; // _G_LLM_STRAT_GROUP_ROUTE_STEPS[256]
    const group_member *group_members;     // _G_LLM_STRAT_GROUP_MEMBERS[256]
    const uint8_t      *group_member_tile; // _G_LLM_STRAT_GROUP_MEMBER_TILE[256][2], flat: col
                                           // @ [i*2+0], row @ [i*2+1]. Read-only (no writer found).
    const uint32_t *path_wrap_mask;        // _G_LLM_STRAT_PATH_WRAP_MASK
    const int32_t  *group_order_goal_x;    // _G_LLM_STRAT_GROUP_ORDER_GOAL_X
    const int32_t  *group_order_goal_y;    // _G_LLM_STRAT_GROUP_ORDER_GOAL_Y
    const int32_t  *group_anchor_x;        // _G_LLM_STRAT_GROUP_ANCHOR_X
    const int32_t  *group_anchor_y;        // _G_LLM_STRAT_GROUP_ANCHOR_Y
    const int32_t  *group_order_owner;     // _G_LLM_STRAT_GROUP_ORDER_OWNER
    const int32_t  *group_member_count;    // _G_LLM_STRAT_GROUP_MEMBER_COUNT

    // _G_LLM_STRAT_PATHTRACE_LEN -- read by unit_group_step_ground (9 sites); not written by this
    // slice (group_step_plane does not touch it per the state matrix).
    const uint32_t *pathtrace_len;

    // ---- SIM1-G-PREP (2026-08-20): the batch-G WRITE SET, pre-registered. ------------------------
    //
    // WHY THESE ARE HERE BEFORE ANY OF THEIR FUNCTIONS IS TRANSLATED. R8 (the readiness report) says a
    // global a not-yet-verified migration function WRITES must be reachable through this view before
    // its batch opens, because a translation cannot emit a write that has no accessor -- and a batch
    // that discovers that mid-flight spends its budget on plumbing instead of translating (measured:
    // the 2026-08-20 run wired 13 regions inline and finished with 0 verified functions). Every
    // member below is bound from the region registry like any other; the derivation for each one is
    // in tools/data/dll_addr_manifest.json's note at the matching address, which is longer and more
    // specific than these comments and is the thing to read before translating.
    //
    // POSTURE, and it is unusual for this header: with ONE exception these are all WRITTEN by the
    // owning function, so the useful handle is the sim_store accessor, not the const member -- the
    // const members exist so a reader in the same TU does not have to go through the store. The one
    // read-only member is `region_coord_wrap_mask`.

    // (a) group-move working state (DGROUP 0x0051de30..0x0051de3f, one cluster with
    // group_member_count @0x0051de38 above). centroid_x/_y are the WRAP-AWARE MEAN member tile,
    // written by llm_strat_group_move_order_pathfind and read OUTSIDE the sim by
    // llm_map_minimap_render -- they are the minimap's group blip, not scratch.
    const int32_t *group_centroid_x;
    const int32_t *group_centroid_y;
    // The write cursor into path_buffers (NOT into group_route_steps): reset by
    // llm_strat_group_move_order_pathfind, INC'd on an append and DEC'd on a run-length coalesce by
    // llm_strat_group_path_step_record/_append.
    const int32_t *group_path_build_idx;

    // (b) map-region routing scratch. route_cand_scratch is 32 bytes whose REAL grain is 8 slots at a
    // 4-byte stride (byte+0 = row/y, byte+1 = col/x, bytes +2/+3 untouched -- CORRECTED 2026-08-21,
    // SIM1-G2: was recorded backwards as "x at slot+0, y at slot+1" here and in the
    // manifest note; llm_map_region_route_search's live decompilation writes byte+0 from the packed
    // region index's LOW byte and byte+1 from its HIGH byte, and the established (col<<8)|row
    // packing this codebase uses elsewhere puts col/x in the HIGH byte -- see
    // tools/data/ghidra_findings.json 2026-08-21-0048-2) -- exposed as raw bytes rather than a struct
    // array because that is what the code writes; see the manifest note, which also records that the
    // shipped corner-cut check legitimately reads one slot PAST the array into
    // _G_LLM_MAP_REGION_ROUTE_BEST_CAND at direction index 7.
    const uint8_t  *region_route_cand_scratch; // [32], slot i at [i*4]
    const uint32_t *region_flood_tile_queue;   // uint[4096], a mod-0x1000 CIRCULAR queue
    // The region-adjacency BFS queue. Elements are region POINTERS. The 512 bound is gap-to-next-
    // symbol, not a cap asserted anywhere in the code -- see the manifest note before relying on it.
    llm_map_region *const *region_route_bfs_queue; // [512]
    // The ONE read-only member here: the shared coordinate wrap mask, ANDed over a packed x/y cell.
    // No writer anywhere in the sim closure.
    const uint32_t *region_coord_wrap_mask;

    // SIM1-G2 (2026-08-21). llm_map_region_route_search's per-direction step/output
    // tables -- both already named+typed in Ghidra, only the sim_view binding was missing. See
    // tools/data/dll_addr_manifest.json's notes at these addresses for the full derivation.
    const uint8_t *region_route_step_deltas; // byte[32], 8 dirs x 4-byte stride: +0=row/y, +1=col/x

    // SIM1-H wave 2 (2026-09-10). _G_LLM_MAP_PROXIMITY_STENCIL @0x0051deac,
    // llm_map_proximity_stencil_entry[169] -- a 13x13 obstacle-proximity stencil, index i mapping to
    // the offset (i%13 - 6, i/13 - 6). Read-only static boot data with no writer anywhere.
    // llm_map_compute_obstacle_proximity_flags ORs `1 << (bit_index & 0x1f)` into
    // llm_map_region_cell::terrain_flags for every passable cell within the stencil of an obstacle;
    // the live values are 0/2/3/4 and terrain_flags' own field comment independently says
    // "bits 2/3/4 = obstacle within ~5/~3/~1 tiles (13x13 stencil)", so the data and the consuming
    // field's documentation corroborate each other.
    const proximity_stencil_entry *proximity_stencil;
    const int32_t                 *region_route_dir_codes; // int[8], 8 dirs, output direction-code table

    // SIM1-G2 (2026-08-21). llm_strat_pathtrace_normalize_repeat_dir_table's diagonal-
    // direction-code (8..23) merge/splice lookup. Already named+typed in Ghidra
    // (llm_strat_pathtrace_dir_split[16] @0x0066b421) by an earlier session; index with
    // `[dirs[i] - 8]`, NOT `[dirs[i]]` -- the array base is already at the first index the caller's
    // own guard (dirs[i] >= 8) can ever reach. See tools/data/dll_addr_manifest.json's note at this
    // address for why the .asm's own literal LEA base (0x0066b3e1) must NOT be used directly (it
    // aliases two other functions' code bytes, not real table data).
    const llm_strat_pathtrace_dir_split *pathtrace_dir_split_table;

    // (c) llm_strat_bldg_completion_dispatch's invasion-pacing "heat" counter. Counts building
    // COMPLETION EVENTS the local player can see (+10/+1/+15 by completion state), triggers an
    // invasion roll at >= 0x46, then decays /2 or /4 -- it is neither `energy` (the HP-like stat) nor
    // `power` (the generated resource), and it sits next to construction-progress code, which is why
    // that is worth saying. Also zeroed by llm_strat_session_state_reset, outside this closure.
    const int32_t *bldg_completion_accum;

    // (d) llm_strat_unit_queue_advance_search's tile search. cur_tile/iter are per-call scratch the
    // original compiler happened to place in .bss; closed_count and frontier_count gate the two
    // arrays, and frontier_count doubles as the LIFO stack pointer AND the function's return value.
    const uint32_t          *unitq_cur_tile;
    const int32_t           *unitq_closed_count;
    const int32_t           *unitq_iter;
    const int32_t           *unitq_frontier_count;
    const unitq_search_node *unitq_closed;   // [1024]
    const unitq_search_node *unitq_frontier; // [128], byte-adjacent to closed but a DISTINCT array
    // SIM1-G1 tail slice (2026-08-20): the 8-neighbour compass delta tables, indexed by direction
    // 0..7. Read-only, no writer in this closure -- each entry is read as `(int8_t)table[dir]`
    // (pointer arithmetic on an int32_t* scales by 4, matching the .asm's implicit x4 stride; the
    // narrowing cast then truncates to the low byte, same idiom _G_LLM_STRAT_GROUP_STEP_HEADING_REMAP
    // already established).
    const int32_t *unitq_neighbor_dx; // _G_LLM_UNITQ_NEIGHBOR_DX[8]
    const int32_t *unitq_neighbor_dy; // _G_LLM_UNITQ_NEIGHBOR_DY[8]

    // (e) the pathtrace working state -- one contiguous DGROUP block 0x0066a334..0x0066a9a8 owned by
    // llm_strat_trace_greedy_path, with llm_strat_pathtrace_remove_loops and
    // llm_strat_pathtrace_normalize_repeat_dir_table writing only into dirs[]/pos[]/pathtrace_len.
    //
    // coord_mask is a PACKED PAIR of masks in one dword (byte +0 row, byte +1 col) so it can mask a
    // packed (col<<8)|row position in one AND; col_mask/row_mask are the separate full-dword smears.
    const uint32_t *pathtrace_coord_mask;
    const uint32_t *pathtrace_col_mask;
    const uint32_t *pathtrace_row_mask;
    const uint32_t *pathtrace_map_w;
    const uint32_t *pathtrace_map_h;
    const uint32_t *pathtrace_half_w;
    const uint32_t *pathtrace_half_h;
    // half_w_m1/half_h_m1 are DEAD STORES: one whole-binary reference each, the write itself. They
    // are bound so a translation can still perform the store (Law 2) -- nothing reads them.
    const uint32_t *pathtrace_half_w_m1;
    const uint32_t *pathtrace_half_h_m1;
    const int32_t  *pathtrace_neg_half_w; // signed, unlike its half_* siblings
    const int32_t  *pathtrace_neg_half_h;
    const uint32_t *pathtrace_start_col;
    const uint32_t *pathtrace_start_row;
    const uint32_t *pathtrace_walk_dir; // the tracer's INITIAL step direction (a dir-table index)
    const uint32_t *pathtrace_goal_col;
    const uint32_t *pathtrace_goal_row;
    // (col<<8)|row -- the SAME packing as every pathtrace_pos element, NOT row*map_w+col.
    const uint32_t *pathtrace_goal_packed;
    // The required final approach direction; 0xff is the "no approach constraint" sentinel and is
    // the one value NOT translated through the move-direction table on the way in.
    const uint32_t *pathtrace_approach_dir;
    // dirs[] and pos[] are INDEX-PARALLEL and their live length is pathtrace_len above. dirs[] also
    // carries ONE terminator byte at dirs[len] (0xff goal reached / 0xfe dead end) which sits
    // deliberately OUTSIDE the live range.
    const uint8_t  *pathtrace_dirs; // [512], move-direction-table indices 0..7
    const uint16_t *pathtrace_pos;  // [512], packed (col<<8)|row
    const uint32_t *pathtrace_best_dir;
    const int32_t  *pathtrace_best_dist;
    // uint[7], but only [0]..[4] are ever read or written -- slots 5 and 6 are dead. Sentinel
    // 0xffffffff means "empty", which is safe because it can never equal a masked packed coordinate.
    const uint32_t *pathtrace_forbid_cells;

    // SIM1-G2 (2026-08-20). The pathtrace tracer's supporting read-only game-data
    // tables, all immediately adjacent in DGROUP (0x0066a0a4..0x0066a334, right before the pathtrace
    // scratch block above) and all already named+typed in Ghidra except dir_merge_lut (named+typed
    // this slice, see tools/data/ghidra_findings.json 2026-08-20-4slice-1). No writer anywhere in the
    // sim closure for any of the five -- static game data.
    const uint32_t      *dir_bitmask_table;       // _G_LLM_STRAT_DIR_BITMASK_TABLE[8]
    const uint8_t       *coord_sign_lut;          // _G_LLM_STRAT_COORD_SIGN_LUT[256]
    const move_dir_step *move_dir_table;          // _G_LLM_STRAT_MOVE_DIR_TABLE[24]
    const int8_t        *dir8_step_offsets;       // _G_LLM_STRAT_DIR8_STEP_OFFSETS[8][2], signed
    const uint8_t       *pathtrace_dir_merge_lut; // _G_LLM_STRAT_PATHTRACE_DIR_MERGE_LUT[8][8]

    // ---- the map's wrap-mask/pathfinder record (see map_geom above). ----
    const map_geom *geom;

    // ---- storage/dock slots (see unit_storage above). [MAX_PLAYERS][STORAGE_PER_PLAYER] ----
    const unit_storage *storage;

    // ---- the map occupancy plane (see tile_object above). [256][256], column-major -- see tile_at().
    const tile_object *tile_objects;

    // ---- SIM1C / llm_strat_order_queue_dispatch ------------------------------------------------
    //
    // TWO GROUPS, AND THE SPLIT IS THE POINT (W1). The first group is written by the sim SOMEWHERE
    // ELSE in the closure and is only READ from here -- exactly like `units`/`buildings` above, so a
    // writer arriving later adds a sim_store accessor and these stay. The second group is written by
    // NO function in the 307 -- that is the claim their being view-only makes.
    //
    // Both groups were measured against the state matrix over the whole sim ledger, not asserted;
    // the first draft of this block called all of them read-only and the check went red on three.
    // Re-run it before moving a member between the groups:
    //   [c for c in state_matrix.cells if c.region == R and c.fn in sim_ledger
    //    and (c.counts.write or c.counts.rw or c.counts.movs)]
    // -- and keep a CONTROL in the query (`units` must come back with 47 writers), because an empty
    // ledger set makes every region look read-only.

    // (1) sim-written elsewhere, read-only on the dispatch path.
    const player_profile *profiles;     // NOT `players`; see the alias. 19 writers in the closure
    const pop_stats      *population;   // _G_LLM_STRAT_POP_STATS. 10 writers
    const int32_t        *session_mode; // 3 == MP lockstep. 1 writer: llm_strat_player_presence_lost

    // (2) written by no function in the sim closure.
    const move_microstep *move_microsteps; // [heading][MICROSTEPS_PER_HEADING]; see the alias
    const int32_t        *sim_active;      // _G_LLM_STRAT_SIM_ACTIVE
    // _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG -- read-only here too (ai_state.h's own comment: read
    // never written, at exactly one AI site outside this closure). SIM1A's
    // llm_strat_unit_passive_engage_tick reads it to set player_data[player].ai_player_relation[player],
    // the SAME entry snippet ai_active_unit_tick.cpp's active_unit_tick() already translates for its
    // AI-controlled sibling -- see that file for the derivation of the diagonal self-index.
    const int32_t *foreign_bldg_change_flag;
    const int32_t *tutorial_step; // _G_LLM_GAME_TUTORIAL_STEP (matrix layer 'ui'; read here)
    const int16_t *player_side;   // PlayerSide -- 2 bytes, vs the order's owner nibble
    const int32_t *planet_index;  // G_PLANET_INDEX
    // SIM1D (2026-08-14). G_PLANET_STATUS[32] (RID_G_PLANET_STATUS), E_PLANET_STATUS
    // per planet, indexed by planet index (int32 stride, NOT the byte-array `planet_status` alias
    // save_ext.h/save_live.cpp use for a different purpose). Already typed + region-registered in
    // Ghidra; this closure's only reader is llm_strat_prod_deliver_arrivals (its
    // `G_PLANET_STATUS[G_PLANET_INDEX] == UNKNOWN` local-player auto-select gate). No writer in the
    // sim closure.
    const int32_t *planet_status;
    const double  *game_clock;         // _G_LLM_STRAT_GAME_CLOCK
    const double  *lockstep_step_size; // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE

    // SIM1F (2026-08-16). CurrentSystem -- scalar int, no prior region at all.
    // game_AddPlanetToAvailable's local-player/current-system gate reads it
    // (`Planets[planet_id].system_index == CurrentSystem`). No writer in the sim closure.
    const int32_t *current_system;
    // SIM1F (2026-08-16). Boot-constant double, 60.0 (game-clock seconds), was
    // unnamed/unregistered (DOUBLE_00500a68). game_AddPlanetToAvailable's toast-suppression deadline:
    // `planet_time[planet_id] = *game_clock + *planet_available_notify_delay`.
    const double *planet_available_notify_delay;

    // SIM1A. _G_LLM_STRAT_POP_DECAY_FACTOR, a constant double (0.1 exactly -- see
    // sim_unit_population_remove.cpp). No writer in the closure (retyped from `undefined` to
    // `double` in Ghidra this slice; raw bytes confirmed the width).
    const double *pop_decay_factor;

    // SIM1F (2026-08-18). _G_LLM_STRAT_POP_GROWTH_FACTOR @0x005014e4 (RID_STRAT_POP_
    // GROWTH_FACTOR), a boot-constant double = 0.1 exactly (raw bytes `9A 99 99 99 99 99 B9 3F`,
    // read-confirmed) -- the 8-byte sibling immediately BEFORE pop_decay_factor's region. Read by
    // llm_strat_population_add's count==0 colony-growth branch:
    //   pop_fraction += (colony_hp_sum+1)*human*pop_growth_factor / (colony_hp_max_sum+1).
    // No writer in the sim closure.
    const double *pop_growth_factor;

    // SIM1-G3 (2026-08-21). Seven boot-constant doubles, contiguous in .rdata
    // (0x005013d8..0x00501410, stride 8) but each an INDEPENDENT literal read by a different
    // storage-lifecycle state handler at its own fixed address -- not a runtime-indexed array (same
    // "adjacent but not one region" situation pop_growth_factor/pop_decay_factor already document).
    // All seven read-confirmed via ReVa read-memory, all zero writers in the sim closure.
    const double *exit_wait_operational_activity_bump;  // llm_strat_unit_state_exit_wait, built_flags==3 arm (0.2)
    const double *exit_wait_default_activity_bump;      // llm_strat_unit_state_exit_wait, default arm (5.0)
    const double *exit_walk_out_soldier_step_scale;     // llm_strat_unit_state_exit_walk_out, soldier_count>0 (0.5)
    const double *taxi_takeoff_step_speed_mult;         // llm_strat_unit_state_takeoff_taxi (2.0)
    const double *taxi_dock_step_speed_mult;            // llm_strat_unit_state_dock_taxi_in (2.0, a DIFFERENT cell)
    const double *enter_wait_activity_backoff_seconds;  // llm_strat_unit_state_enter_wait (0.05)
    const double *enter_walk_in_soldier_transport_mult; // llm_strat_unit_state_enter_walk_in, soldier_count>0 (0.5)

    // SIM1-G3 (2026-08-21). Three MORE boot-constant doubles, same "adjacent .rdata
    // literal, independent per-caller cell" situation as the seven above -- 0x00501430 and 0x00501472
    // happen to share the same byte pattern (3.0) at DIFFERENT addresses, a coincidence not a shared
    // cell (confirmed via ReVa read-memory on both).
    const double *takeoff_landing_step_cost_scale;         // llm_strat_unit_state_takeoff_landing (2.0)
    const double *takeoff_landing_a_helipad_activity_bump; // llm_strat_unit_state_takeoff_landing, A_HELIPAD landing-complete arm (3.0)
    const double *step_adjacent_budget_gate;               // llm_strat_unit_state_step_adjacent, tiles-blocked TICK_BUDGET gate (3.0)

    // SIM1F (2026-08-17). _G_LLM_MAP_CAM_COL / _G_LLM_MAP_CAM_ROW (RID_MAP_CAM_COL/
    // RID_MAP_CAM_ROW, already region-registered with MF_VIEW -- llm_ui/net consumers bind them
    // elsewhere; this is a second, independent read-only binding). llm_strat_spawn_enemy_landing's
    // sole reader: when claim_landing_spot fails to find a spot, it falls back to a landing position
    // relative to the current camera tile (`general.width_mask & _G_LLM_MAP_CAM_COL - 0xf`). No
    // writer in this closure.
    const int32_t *cam_col;
    const int32_t *cam_row;

    // The localised string table (G_TEXT_PTRS): pointers to wide strings, indexed by a text id. The
    // dispatcher reads cfg_building's leading text id out of it to name a building in a "%s: %s"
    // message. The STRINGS it points at are read-only too, hence the doubled const.
    const wchar_t *const *text_ptrs;

    // ---- SIM1F (2026-08-18): llm_strat_player_presence_lost's read-only inputs ------
    // debug_campaign_cheat (_G_LLM_STRAT_DEBUG_CAMPAIGN_CHEAT @0x005d0c74) -- always 0 in retail; a
    // developer debug gate read only in the SP elimination path (halves resource yield, suppresses
    // the system-captured message, reveals a cheat string). mp_ally_victory_rule_flag
    // (_G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG @0xe58993) -- when set, an all-allied surviving set is a
    // shared MP victory. system_lost_msg_shown_flag (_G_LLM_STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG @0xe58997)
    // -- gates the SP low-system defeat message vs plain defeat. cheat_cmd_table (_G_LLM_CHEAT_CMD_TABLE
    // @0xd033c0) -- char*[48] cheat-string table; presence_lost reads entry [1] only on the dead
    // debug-cheat path. All four have NO writer in the sim closure.
    const int32_t *debug_campaign_cheat;
    const int32_t *mp_ally_victory_rule_flag;
    const int32_t *system_lost_msg_shown_flag;
    char *const   *cheat_cmd_table;

    // ---- SIM1A (llm_strat_unit_tick and neighbours) ------------------------------
    //
    // _G_LLM_STRAT_CUR_UNIT/_CUR_PLAYER/_CUR_INDEX are set TOGETHER by the per-unit tick driver
    // (outside this batch) before calling llm_strat_unit_tick and its siblings, which operate on
    // "the current unit" with no parameters. cur_unit is read-only here for consumers that never
    // write through it (e.g. llm_strat_unit_update_soldiers); sim_store::cur_unit() is the mutable
    // sibling for the writers (unit_tick/update_anim/update_rotation), bound from the SAME resolved
    // pointer value -- see sim_state.cpp.
    const unit     *cur_unit;
    const uint16_t *cur_player;
    const uint16_t *cur_index;

    // Read/drained by llm_strat_unit_tick's state-machine loop; llm_strat_unit_update_soldiers also
    // reads it (walk-interpolation delta). Written only by unit_tick -- see sim_store::tick_budget().
    const double *tick_budget;

    // The original per-state dispatch table -- see unit_state_fn above. Dispatched through, never
    // written.
    const unit_state_fn *unit_state_funcs;

    // _G_LLM_STRAT_CTRL_GROUPS[10]. Read by llm_strat_ctrl_group_contains_unit; the mutable sibling
    // sim_store::ctrl_group_at() is for llm_strat_unit_ctrl_group_assign, which hands a group's
    // .count field's ADDRESS to the original ctrlgroup add/remove helpers (an escape, like
    // text_scratch() below).
    const ctrl_group *ctrl_groups;

    // SIM1-H (2026-09-10). The four modifier-key bytes inside _G_LLM_INPUT_KEYSTATE, bound
    // INDIVIDUALLY (they are separately named 1-byte MF_VIEW regions -- base + scancode:
    // LCTRL=+0x1d, LSHIFT=+0x2a, RSHIFT=+0x36, LALT=+0x38), which is the shape tact_view uses for
    // the same bytes. Read by llm_strat_group_issue_attack_order to pick between three attack-order
    // flavours; nothing in this domain writes them (OWN_READONLY -- the writer is
    // llm_input_wndproc_tap, permanently host-side).
    //
    // BOTH BIT ENCODINGS ARE LIVE and a translation must test both, exactly as the original does:
    // the tap sets `|= 5` for a plain key and `|= 10` for an extended one, so a held key can read
    // bit 0 OR bit 1 (the original spells this as two CMPs per byte, e.g. @0x00444bd6/0x00444be0).
    const uint8_t *key_lctrl_held;  // RID_KEY_LCTRL_HELD  (_G_LLM_INPUT_KEYSTATE + 0x1d)
    const uint8_t *key_lshift_held; // RID_KEY_LSHIFT_HELD (+0x2a)
    const uint8_t *key_rshift_held; // RID_KEY_RSHIFT_HELD (+0x36)
    const uint8_t *key_lalt_held;   // RID_KEY_LALT_HELD   (+0x38)

    // Anim[2501] cfg data table. Read by llm_strat_unit_update_anim to advance the damage-smoke
    // chain. No writer anywhere (cfg data, loaded at boot).
    const anim_frame *anim_frames;

    // _G_LLM_STRAT_PLAYER_RACE. Plain global (not per-player). Read by llm_unit_recruit's
    // housing-cap-exceeded fallback to pick a rejection text code. No writer in this closure.
    const int32_t *player_race;

    // map::g::passable[256][256], byte per tile, same (tile_x<<8)|tile_y indexing as tile_at().
    // Read by llm_unit_recruit's neighbor-tile search; the mutable sibling
    // sim_store::passable_at() is for the same function's write when it claims a tile.
    const uint8_t *passable;

    // _G_LLM_STRAT_SOLDIERS[8][100]. Read by llm_unit_create_soldier/llm_unit_recruit's housing-cap
    // checks (record [0]'s owner_unit doubles as the roster's used-slot count -- see
    // sim_unit_passive_engage.cpp's derivation of the same slot). The mutable sibling
    // sim_store::soldier_at() is for llm_strat_unit_update_soldiers, which writes the walk/anim
    // fields of individual soldier records.
    const soldier *soldiers;

    // _G_LLM_STRAT_UNIT_HOUSING_STATS[8]. Read by llm_unit_recruit's per-category housing caps.
    // SAME RID ai_state.h's own `unit_housing` binds independently.
    const housing_stats *unit_housing;

    // SIM1A (2026-08-11). _G_LLM_CLICK_SELECT_TARGET_FLAGS, plain uint16_t scalar (NOT
    // per-player -- immediately follows _G_LLM_CLICK_SELECT_TARGET_ID at +2, same RID family).
    // Read-only in this closure: llm_strat_unit_remove_from_map/_on_destroyed compare it against
    // `player | 0x80` / `player | 0x20` before touching the companion TARGET_ID; no write site
    // found for it in either function or anywhere else in the migration set.
    const uint16_t *click_select_target_flags;

    // ---- SIM1B (llm_strat_bldg_instant_construct_find_slot_enqueue's free-slot scan) -----------
    //
    // The three building-owned sub-rosters, READ-ONLY -- same RIDs sim_store already binds mutably
    // for SIM1C's dispatcher (turret_at/production_at/lab_at). `mines` has no mutable sibling at
    // all: this is the closure's first binding of that region.
    const turret     *turrets;
    const production *productions;
    const lab        *labs;
    const mine       *mines;

    // The cfg parser's Building section record (see cfg_building_section above). Only `.total` is
    // read, by the same function as the four rosters above.
    const cfg_building_section *cfg_building_sec;

    // SIM1D (2026-08-13). The cfg parser's Unit section record (see cfg_unit_section
    // above). Only `.total` is read.
    const cfg_unit_section *cfg_unit_sec;

    // SIM1D (2026-08-13). See cfg_planet alias above. No writer anywhere.
    const cfg_planet *cfg_planets; // Planets[32]

    // SIM1B (2026-08-12). The torus wrap MASKS width_m/height_m (RID_WIDTH_M/RID_HEIGHT_M) -- a
    // SECOND independent binding of the SAME region ai/ai_state.h's map_width_mask/map_height_mask
    // already bind, NOT the `geom`/map_geom masks above (a different pair of globals at a different
    // address -- see the `geom` comment). llm_strat_bldg_connectivity_flood_fill and
    // llm_strat_bldg_check_placement_encloses_neighbors both read these directly rather than through
    // `general`.
    const uint32_t *width_m;
    const uint32_t *height_m;

    // SIM1B (2026-08-12). _G_LLM_ANIM_PLACE_DENIED/_ALLOWED, int32_t[2] (DENIED at [0], ALLOWED at
    // [1]) -- the FX anim-seq ids llm_bldg_placement_check_and_preview reads as both the seq-id
    // argument to llm_fx_anim_seq_cancel and the per-tile allow/deny "kind" argument to
    // llm_strat_fx_anim_spawn. Declared_need from the translate fan-out; no writer in this closure.
    const int32_t *anim_place_seq_ids;

    // SIM1B (2026-08-12; building_tick machinery slice). llm_bldg_pay_build_cost / _pay_cycle_inputs
    // need the per-player invention-acquisition state and resource holdings to gate/charge a cost --
    // see the `player_progress`/PLAYER_RESOURCE_SLOTS comments above. `progress` is written by the
    // research/acquisition code outside the sim, still no writer here. `player_resources` WAS
    // "game_SpendResource/llm_resource_add, both called OUT to, never written directly here" until
    // SIM1F (2026-08-16) translated those two functions themselves -- see
    // sim_store::player_resource_at() below, the first direct writer.
    const player_progress *progress;         // progress[8][300] @RID_PROGRESS
    const int32_t         *player_resources; // [MAX_PLAYERS][PLAYER_RESOURCE_SLOTS] @RID_PLAYER_RESOURCES

    // SIM1B (2026-08-13; tick_pip_anim bug fix). A_OGIEN, cfg_t_frame_index[4] -- the game's
    // hardcoded 'fire' effect animation frame table (populated by cfg_ConstructAnims), which is
    // what llm_strat_bldg_tick_pip_anim's chain-restart branch actually reads
    // (`A_OGIEN[pip_level-1]`, pip_level always >=1 there). Replaces a wrong binding to
    // _G_LLM_STRAT_BLDG_PIP_LEVEL_BASE_FRAME @0xc38730 (a hex-transcription slip landed 3 entries
    // early on an unrelated, real, save-tracked block -- see mh_addrs.gen.h's A_OGIEN comment and
    // the transcription slip recorded for it). No writer in this closure -- boot-populated cfg data.
    const int32_t *pip_fire_anim_frames;

    // SIM1B (2026-08-13; building_tick machinery). _G_LLM_STRAT_CUR_BUILDING
    // dereferenced -- the per-building tick driver's ambient pointer to "the current building", same
    // shape as cur_unit above (POINTER-VALUED global, resolved once per sim_state() call, bound to
    // both sim_view::cur_building and sim_store::cur_building_ from the SAME resolved live pointer).
    // Read-only here for consumers that never write through it; sim_store::cur_building() is the
    // mutable sibling for llm_strat_bldg_state_destroyed, the only writer in this slice.
    const building *cur_building;

    // SIM1B building_tick promotion-oracle session (2026-08-13, G19). The three original building
    // dispatch tables -- see bldg_state_fn/bldg_done_fn/bldg_tick2_fn above for the shape/index/RID
    // of each. Dispatched-through only, never resolved/inlined/renamed, same posture as
    // unit_state_funcs above.
    const bldg_state_fn *bldg_state_funcs;
    const bldg_done_fn  *bldg_done_funcs;
    const bldg_tick2_fn *bldg_tick2_funcs;

    // _G_LLM_STRAT_DEATH_ANIM_TABLE, int32_t[6][4] -- a hardcoded death-FX-animation-id lookup table
    // (boot-populated cfg-adjacent data, same posture as A_OGIEN/pip_fire_anim_frames above), read by
    // llm_strat_bldg_state_destroyed as `death_anim_table[cfg_buildings[building_id].trace * 4 +
    // llm_rand_below(4)]` and by llm_strat_unit_state_die_explode as
    // `death_anim_table[Unit[proto].debris_anim_row * 4 + llm_rand_below(4)]` -- both readers share
    // ONE table. EXTENT SETTLED 2026-08-21 (finding 2026-08-21-1329-1): Ghidra-typed
    // `cfg_t_frame_index[6][4]` (96 bytes), confirmed independently by (a) a raw byte-scan for the
    // address literal finding both readers Ghidra's reference manager misses (the doubly-indexed
    // `[EAX+EDX+base]` blindness already hit for A_OGIEN), (b) a fresh
    // full-image instruction sweep attributing the same 96 bytes, and (c) the gap to the next symbol
    // (_G_LLM_STRAT_FX_ANIMS @0x00bf50e0) being exactly 6*16=96 bytes with zero remainder. No writer
    // anywhere, so bound as a raw read-only pointer.
    const int32_t *death_anim_table;

    // _G_LLM_STRAT_UI_PANEL_MODE/_PAGE, int32_t scalars in the DGROUP data segment (0x0050xxxx, not
    // the usual 0x00e5xxxx UI-state block). Read here in llm_strat_bldg_unmap_footprint's
    // UI-selection-cleanup arms (mode==1 && page==2 gates BUILD_TAB_BUILDINGS; mode==1 && page==0
    // gates BUILD_TAB_UNITS in the MOTHER-teardown arm). SIM1F (2026-08-18): game_SetEvent
    // WRITES both (it is the strategic-HUD panel dispatcher), so they now also have sim_store mutable
    // accessors reusing these RIDs -- these read-only view members stay for the unmap_footprint reader.
    const int32_t *ui_panel_mode;
    const int32_t *ui_panel_page;

    // SIM1F (2026-08-18). game_SetEvent's read-only inputs. ui_event_defer_active: when
    // nonzero the event is queued instead of applied (set by the drain path outside this closure).
    // ui_bldg_tab_select_blocked: gates the buildings tab/hotkey. ui_panel_fallback_table: a
    // 0-terminated int table (all-zero at boot, so the trailing fallback pass is normally inert) that
    // can force a fallback panel mode. See sim_game_set_event.h for the derivation.
    const int32_t *ui_event_defer_active;
    const uint8_t *ui_bldg_tab_select_blocked; // 1-BYTE flag (asm: `CMP byte ptr [0x00e58365]`);
                                               // G_PLANET_INDEX is at 0x00e58366, immediately adjacent,
                                               // so a 4-byte read would pull 3 bytes of it into the gate.
    const int32_t *ui_panel_fallback_table;

    // SIM1B (2026-08-13; building_tick machinery). _G_LLM_STRAT_POWER_STATS[player],
    // READ-ONLY -- a second independent binding of the SAME RID_STRAT_POWER_STATS region
    // sim_store::power_stats_at() already binds mutably (see the `power_stats` alias above).
    // llm_strat_refresh_building reads `.ratio` here; it never writes through this member.
    const power_stats *power_stats;

    // SIM1D (2026-08-13). See the prod_shuttle_slot alias comment above. READ-ONLY here;
    // sim_store::prod_shuttle_slot_at() is the mutable sibling llm_strat_production_complete writes
    // through, both bound from the same live pointer in sim_state().
    const prod_shuttle_slot *prod_shuttle_slots; // [MAX_PLAYERS][PROD_SHUTTLE_SLOTS_PER_PLAYER]

    // SIM1D (2026-08-13). See the dir8_offset alias comment above. No writer in the
    // closure, so no sim_store sibling.
    const dir8_offset *dir8_offsets; // [8], indexed by llm_strat_dir_from_to's return code

    // SIM1D (2026-08-13). See the storage_stats alias comment above. No writer in the
    // closure, so no sim_store sibling.
    const storage_stats *storage_stats; // [MAX_PLAYERS]

    // SIM1D (2026-08-14; llm_game_notify_system_available). The System record's two
    // leading scalar fields (invention@+0, name/define_index@+4), stride 0x8c per system, base
    // 0xbe1a30 -- see mh_addrs.gen.h's `System` comment for the offset/undercount caveat on the
    // SEPARATE, already-typed `cfg_final_struct_System` array (do NOT use that type for this raw
    // access; it is anchored 8 bytes late and is a known, deliberately-unfixed gap). Raw
    // `const int32_t *`, same posture as death_anim_table/A_OGIEN: index as
    // `system_define_index_base[system_idx * (0x8c / 4) + 1]` for the define_index field
    // llm_game_notify_system_available reads. No writer in this closure.
    const int32_t *system_define_index_base;

    // SIM1D (2026-08-14). See mh_addrs.gen.h's comments -- two independent boot-constant
    // doubles near _G_LLM_STRAT_POP_DECAY_FACTOR. `shuttle_board_activity_bump` (20.0) is read by
    // llm_strat_unit_load_into_shuttle_cargo. `shuttle_duration_mult_dead_branch` (10.0) is read by
    // llm_prod_bldg_depart_finalize inside a permanently-dead `&& (false)` branch -- translate the
    // branch faithfully, but no live/rig evidence can ever exercise it. No writer for either.
    const double *shuttle_board_activity_bump;
    const double *shuttle_duration_mult_dead_branch;

    // SIM1D / SIM1E opening (2026-08-14). _G_LLM_STRAT_UNIT_DEATH_HQ_ENERGY_CREDIT
    // (RID_STRAT_UNIT_DEATH_HQ_ENERGY_CREDIT), a boot-constant double = -1.0 (confirmed via
    // read-memory). llm_strat_unit_apply_damage adds it to units[cur_player][0].energy (that
    // player's unit slot 0, HQ/mothership) on a unit's lethal-drop death -- a per-death DECREMENT,
    // not a positive refund, despite the earlier plate's "energy refund" wording (same shape and
    // same -1.0 value as llm_strat_bldg_apply_damage's local DAT_005012d4 constant for buildings;
    // both read as a per-death counter on the HQ/slot-0 record rather than literal HP restoration).
    // No writer in the closure.
    const double *unit_death_hq_energy_credit;

    // SIM1E (2026-08-15). Four boot-constant doubles in ONE contiguous 8-byte-stride
    // run (0x00500f0c/14/1c/24), registered together because they are one table and a later reader
    // should not have to rediscover that. All read-only, no writer anywhere. Values are
    // read-memory-confirmed, not inferred from the names:
    //   bldg_main_base_damage_mult          0.0001 -- llm_strat_bldg_kill_credit FMULs incoming
    //     damage by it when the victim's cfg type is H_MAIN_BASE (0x22) / A_MAIN_BASE (0x0e), i.e.
    //     a main base takes one ten-thousandth of splash damage (0x0044c6f1).
    //   bldg_lost_feedback_cooldown_mother   20.0 -- added to *game_clock and stored into
    //     buildings[player][0].cycle_progress as the next-allowed building-lost notify deadline,
    //     MOTHER arm (0x0044c8dc).
    //   bldg_lost_feedback_cooldown_other    60.0 -- the non-MOTHER counterpart, stored into
    //     buildings[player][0].last_tick_time (0x0044c9cd). SAME VALUE as the unit-side constant
    //     below and a DIFFERENT address and field -- do not collapse the two.
    const double *bldg_main_base_damage_mult;
    const double *bldg_lost_feedback_cooldown_mother;
    const double *bldg_lost_feedback_cooldown_other;

    // SIM1E (2026-08-15). _G_LLM_FX_DEBRIS_SCALE_DIVISOR = 100.0, and it is a
    // **float**, not a double -- 4 bytes (read-memory 0000C842), and the reader is `FDIV float ptr`
    // (opcode D8 /6, a 32-bit operand) at 0x0044dc7d / 0x0044dcc5. Binding it as `const double *`
    // would read it and its neighbour as one value. llm_strat_spawn_debris_burst divides
    // llm_rand_below_fx(0x32) by it and adds 1.0 -> a per-particle scale in [1.00, 1.49].
    const float *debris_scale_divisor;

    // SIM1E (2026-08-15). llm_strat_projectile_spawn's two launch-offset constants.
    // NAMED FOR WHAT THEY DO, which is not symmetric: `projectile_dist_scale` (16.0) multiplies the
    // integer source-to-target delta ONCE, feeding BOTH axes; `projectile_dir_y_sign` (-1.0) is then
    // applied to the SIN component only (0x00464b59), because screen rows grow downward while the
    // trig table is in math orientation. A translation that applies the second one to both axes, or
    // reads them as an (x_scale, y_scale) pair, is wrong.
    const double *projectile_dist_scale;
    const double *projectile_dir_y_sign;

    // SIM1E (2026-08-15). _G_LLM_STRAT_UNIT_LOST_FEEDBACK_COOLDOWN
    // (RID_STRAT_UNIT_LOST_FEEDBACK_COOLDOWN), a boot-constant double = 60.0 (confirmed via
    // read-memory, hexBytes 0000000000004e40). llm_strat_unit_kill_credit adds it to
    // *game_clock at 0x0044ccdf to stamp the next "unit lost" feedback deadline. Same
    // OWN_READONLY posture as the two shuttle constants above. No writer anywhere.
    const double *unit_lost_feedback_cooldown;

    // SIM1E (2026-08-15). _G_LLM_STRAT_FACING_TRIG_TABLE[24]
    // (RID_STRAT_FACING_TRIG_TABLE) -- the 24-way heading trig table, indexed DIRECTLY by a
    // projectile/unit `facing` (0..23) at a 20-byte stride. Already fully typed + commented in
    // Ghidra (`llm_facing_trig {double cos; double sin; int angle_deg;}`, see mh_structs.gen.h,
    // which carries the field comments including the init loop's documented OFF-BY-ONE: rec[N].cos
    // is the cosine of rec[N-1].angle_deg, and rec[0] is left uninitialised). Read by
    // llm_strat_projectile_spawn (`.cos` @0x00464b23, `.sin` @0x00464b50) to turn a launch heading
    // into per-axis pixel deltas. Runtime-filled once by llm_strat_unit_facing_offset_init, which
    // is NOT in this closure -- so read-only here and no sim_store sibling.
    const facing_trig *facing_trig_table; // [24]

    // SIM1D (2026-08-14). See the dir_step_offset alias comment above. No writer in
    // the closure, so no sim_store sibling.
    const dir_step_offset *dir_step_offsets; // [40], indexed by a building facing byte

    // SIM1F (2026-08-18). See the dir_remap_row alias comment above. Read-only here
    // (runtime-filled by a non-closure init), so no sim_store sibling.
    const dir_remap_row *dir_remap_table; // [24], indexed by a dir8/heading index

    // SIM1F (2026-08-18). _G_LLM_STRAT_LANDING_SPOTS[16] (RID_STRAT_LANDING_SPOTS) --
    // the READ-ONLY sibling of sim_store::landing_spot_at (mutable, added). Walked by
    // llm_strat_count_landing_spots: the number of leading entries whose .status != -1 (capped 16).
    const landing_spot *landing_spots; // [16]

    // SIM1D (2026-08-14; opening SIM1E). See the projectile alias comment above.
    // READ-ONLY here; sim_store::cur_projectile()/projectile_pool_at() are the mutable siblings
    // llm_strat_projectile_tick writes through, all bound from the same live region/pointer.
    const projectile *projectile_pool; // [1500]

    // _G_LLM_STRAT_CUR_PROJECTILE dereferenced -- the per-tick driver's ambient pointer to "the
    // projectile currently being ticked", same POINTER-VALUED-global shape as cur_unit/cur_building
    // above (both halves resolved once per sim_state() call from the SAME live pointer).
    const projectile *cur_projectile;

    // SIM1E third batch (2026-08-14). See the fx_anim alias comment above. READ-ONLY here;
    // sim_store::fx_anim_pool_at()/cur_fx_anim() are the mutable siblings llm_fx_anim_seq_cancel/
    // llm_strat_fx_anim_tick write through.
    const fx_anim *fx_anim_pool; // [10000]

    // _G_LLM_STRAT_CUR_FX_ANIM dereferenced -- the per-tick driver's ambient pointer to "the fx-anim
    // pool entry currently being ticked", same POINTER-VALUED-global shape as cur_unit/cur_building/
    // cur_projectile above (the 4th instance; both halves resolved once per sim_state() call from
    // the SAME live pointer). llm_fx_anim_seq_cancel does not use this -- it walks fx_anim_pool[]
    // directly by raw index instead.
    const fx_anim *cur_fx_anim;

    // SIM1E third batch (2026-08-14). A_DYM_POJAZD[4] (cfg_t_frame_index, a plain int32_t typedef --
    // see mh_addrs.gen.h) -- the vehicle smoke-anim-frame-id table, indexed by dmg_smoke_level-1 in
    // llm_strat_unit_update_damage_smoke. FOUND VIA A DATA-ADJACENCY TRAP: the function's raw
    // address arithmetic reads as an offset past _G_LLM_STRAT_POP_STATS[7]'s end, which is actually
    // this wholly separate, already-typed global -- see its own dll_addr_manifest.json entry. No
    // writer in the closure.
    const int32_t *a_dym_pojazd; // [4]

    // SIM1E third batch (2026-08-14). _G_LLM_STRAT_DMG_SMOKE_LEVEL_SCALE -- a boot-constant double
    // (4.0, confirmed via read-memory), the FMUL scale factor llm_strat_unit_update_damage_smoke
    // applies to a unit's HP-loss fraction before truncating into dmg_smoke_level. Sole reader; no
    // writer in the closure.
    const double *dmg_smoke_level_scale;

    // ---- SIM1E fog/sight family (2026-08-16) ---------------------------------------------------
    //
    // The ten precomputed circular-footprint tables (RID_SIGHT_AREA_1..RID_SIGHT_AREA_10), index i
    // holding sight radius i+1's table. map_fow_ConvertSightToArea's whole logic is picking one of
    // these ten fixed bases (a literal 10-way cascade, not arithmetic over one array -- see
    // sim_fog_of_war.cpp). No writer anywhere; boot-populated cfg-adjacent data.
    const map_t_tile_coord *sight_area[10];

    // _G_LLM_IS_HUMAN, per-player human-controlled bitmask (bit i set = player i is human), already
    // a committed region (RID_IS_HUMAN) but bound into neither AI nor sim view before this slice.
    // Read-only in this closure.
    const uint32_t *is_human;

    // ---- SIM1E sim_step (the domain root, 2026-08-16) ------------------------------------------
    //
    // _G_LLM_STRAT_AI_ENABLED -- a second, independent mh::sim binding of the SAME region
    // ai_store::ai_enabled already binds (RID_STRAT_AI_ENABLED), matching this file's own
    // established second-binding pattern (profiles/pop_stats/storage_stats/etc.). Gates whether
    // llm_strat_sim_step calls out to the AI tick at all. Read-only here; no writer in the sim
    // closure.
    const int32_t *ai_enabled;

    // mh::addr::GAME_TIME_DELTA -- a second, independent, READ-ONLY mh::sim binding of the SAME
    // region mh::lockstep::turn_engine already binds mutably. llm_strat_sim_step forwards it
    // verbatim as the AI tick's dt argument; it never writes it.
    const double *game_time_delta;

    // Seven boot-constant doubles, one contiguous table (0x00500a30..0x00500a60, 8 bytes apart).
    // Already named+typed in Ghidra (found already committed under their real _G_LLM_STRAT_* names
    // when this slice went to add them, so the RIDs below carry those names, not the bare ones this
    // slice's manifest entries proposed). Read-memory-confirmed values: SUBTICK_A_PERIOD 5.0,
    // SUBTICK_A_PERIOD_POS 5.0, SUBTICK_A_PERIOD_NEG -5.0 (exact negation), SUBTICK_B_PERIOD 5.0,
    // SUBTICK_B_PERIOD_POS 5.0, SUBTICK_B_PERIOD_NEG -5.0 (exact negation), PROD_CHECK_PERIOD_NEG
    // -1.0. No writer anywhere in the sim closure -- llm_strat_sim_step's two catch-up loops (A/B)
    // and its per-second production-ready scan are the sole readers.
    const double *subtick_a_period;
    const double *subtick_a_period_pos;
    const double *subtick_a_period_neg;
    const double *subtick_b_period;
    const double *subtick_b_period_pos;
    const double *subtick_b_period_neg;
    const double *prod_check_period_neg;

    // ---- SIM1F (2026-08-16) ----------------------------------------------------------
    //
    // llm_strat_decay_excess_resources's spend-fraction boot constant (0.1, read-memory-confirmed).
    // No writer -- read-only.
    const double *resource_decay_rate;

    // llm_game_speed_increase/_decrease's four boot-constant gates: cap 2.0, up-multiplier 1.2,
    // floor 0.25, down-divisor 1.2 (same value as up-multiplier but a distinct storage location --
    // read-memory-confirmed). No writer -- read-only.
    const double *game_speed_factor_max;
    const double *game_speed_factor_step_up;
    const double *game_speed_factor_min;
    const double *game_speed_factor_step_down;

    // _G_LLM_GAME_SPEED_PLAYER_FACTOR[8], READ-ONLY here -- llm_game_speed_increase/_decrease need
    // read-modify-write, see sim_store::game_speed_player_factor_at() below.
    const double *game_speed_player_factor;

    // ---- SIM1F (llm_strat_spawn_invasion_force, the LAST SIM function) ----------------
    //
    // The three AI think-cycle periods (move/tactic/strategy) and the per-player clock STAGGER scale.
    // Read-memory-confirmed values: move 1.0, tactic 5.0, strategy 10.0 (all float), stagger 0.125
    // (double). spawn_invasion_force seeds ai_clock_{m,t,s} = (float)player * period * stagger, so
    // each AI player's think phases start staggered by player/8 of a period. No writer -- read-only.
    const float  *ai_move_period;
    const float  *ai_tactic_period;
    const float  *ai_strategy_period;
    const double *ai_invasion_clock_stagger;

    // _G_LLM_STRAT_AI_CFG_START_UNITS (int 12) -- the AI starting-unit budget spawn_invasion_force
    // stamps into ai_start_units_remaining + ai_start_units_pending_spawn. Read-only.
    const int32_t *ai_cfg_start_units;

    // _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT (highest claimed player index + 1), READ side. The write
    // side is sim_store::ai_active_player_count() -- spawn_invasion_force raises it to player+1 when
    // the spawned player is beyond the current high-water mark.
    const int32_t *ai_active_player_count;

    // SIM1-G2 (2026-08-20). _G_LLM_MAP_DIR_STEP_DELTAS[28][2] (RID_MAP_DIR_STEP_DELTAS), flat:
    // dx at [i*2], dy at [i*2+1]. Read by llm_strat_unit_path_detour and
    // llm_strat_group_move_order_pathfind to step a tile by an 8/24-direction heading index. No
    // writer in the closure -- read-only. This slice only ever indexes 1..24, but the extent is 28
    // (an aux_symbols.json cross-reference shows another function materializing a reference at
    // index 25), so all 28 are exposed. UNSIGNED (uint8_t), not signed, even though the Ghidra type
    // is char[28][2] -- every read site in the .asm uses MOVZX (zero-extend), never MOVSX, so a
    // 0xff entry means +255 wrapped through the caller's toroidal mask, not -1. Confirmed against
    // llm_strat_unit_path_detour's .asm (e.g. 0x00420a90/0x00420a9f) before typing this member.
    const uint8_t *map_dir_step_deltas;

    // SIM1-G2 (2026-08-20). llm_strat_pathfind_route_leg_group_and_sort's wave-index tuning
    // constants (RID_STRAT_GROUP_MOVE_WAVE_DIST_SCALE/_BIAS): each scratch member's integer
    // distance-to-ref is FILD'd, FMUL'd by the scale, FADD'd with the bias, then truncated
    // (utils_math_trunc) to produce the wave rank passed to llm_strat_claim_free_slots_within_dist.
    // No writer in the closure -- read-only. See tools/data/ghidra_findings.json 2026-08-20-1920-1.
    const double *group_move_wave_dist_scale;
    const double *group_move_wave_dist_bias;

    // SIM1-G2 (2026-08-20). llm_strat_unit_state_move_path's per-tick-budget heading scale
    // (RID_STRAT_MOVE_PATH_HEADING_SCALE_FAR/_MID): FAR applies when the ticking unit's order/state
    // byte at +0xac is > 7, MID when it is in 4..7 (<=3 applies neither). No writer -- read-only.
    // See tools/data/ghidra_findings.json 2026-08-20-1920-1.
    const double *move_path_heading_scale_far;
    const double *move_path_heading_scale_mid;

    // SIM1-G4 (2026-08-22). Nine boot-constant doubles, each already named+typed in
    // Ghidra by an earlier SIM-READY prep pass but never wired into dll_addr_manifest.json/sim_view
    // until this slice's translators declared the gap. All read-only, no writer anywhere. Values are
    // read-memory-confirmed (see mh_addrs.gen.h's per-symbol comments), not inferred from the names.
    // hangar_recharge_period(_neg): 20.0/-20.0, llm_strat_bldg_state_hangar_recharge_units' per-cycle
    // recharge-pulse period + its FADD decrement.
    const double *hangar_recharge_period;
    const double *hangar_recharge_period_neg;
    // dismantle_progress_divisor: 0.2, llm_strat_bldg_state_dismantling's FDIV divisor.
    const double *dismantle_progress_divisor;
    // bldg_dismantle_hq_energy_credit: -1.0, llm_strat_bldg_state_dismantle_finish's HQ-energy credit
    // (same per-event COUNTER-decrement shape as unit_death_hq_energy_credit above).
    const double *bldg_dismantle_hq_energy_credit;
    // rubble_sight_decay_period(_neg): 5.0/-5.0, rubble_cleanup_period: 20.0 (coincidentally the same
    // value as hangar_recharge_period at a different .rdata address) -- all three read by
    // llm_strat_bldg_state_rubble_sight_decay's while-loop.
    const double *rubble_sight_decay_period;
    const double *rubble_sight_decay_period_neg;
    const double *rubble_cleanup_period;
    // prod_retry_period(_neg): 10.0/-10.0, llm_strat_bldg_state_prod_retry_wait's cycle_progress gate
    // + its FADD on the transition-taken arm.
    const double *prod_retry_period;
    const double *prod_retry_period_neg;

    // SIM1-G4 (2026-08-22). Same gap class as the nine above -- already named+typed in
    // Ghidra, never wired until this slice's translator declared it. Boot-constant double = 0.5
    // exactly (read-memory-confirmed), single reader, no writer.
    // refund_energy_factor: llm_strat_bldg_refund_resources_scaled_by_energy multiplies the
    // building's CURRENT energy by this before dividing by the building type's MAX energy to get the
    // refund ratio.
    const double *refund_energy_factor;

    // SIM1-G4 (2026-08-22). Same gap class as above. Boot-constant doubles = 10.0/-10.0
    // exactly (read-memory-confirmed), single reader each, no writer.
    // mine_extract_period(_neg): llm_strat_bldg_state_mine_extracting's cycle_progress gate + its
    // FADD decrement.
    const double *mine_extract_period;
    const double *mine_extract_period_neg;
    // mine_rescan_period(_neg): llm_strat_bldg_state_mine_rescan_wait's cycle_progress gate + its
    // FADD decrement. Same 10.0/-10.0 bit pattern as mine_extract_period(_neg) at DIFFERENT .rdata
    // addresses -- coincidence, not a shared cell.
    const double *mine_rescan_period;
    const double *mine_rescan_period_neg;

    // SIM1-G4 (2026-08-22; llm_strat_bldg_completion_dispatch). Same gap class as above
    // -- already named+typed in Ghidra, never wired until this slice's translator declared it.
    // mother_lost_escalation_interval: boot-constant double = 60.0 exactly (read-memory-confirmed),
    // the invasion-escalation footer's escalation_bonus term.
    const double *mother_lost_escalation_interval;
    // bldg_completion_slot_count: MINE_EXTRACTING's per-mine deposit-slot loop bound. A real .bss
    // byte, not a boot constant -- its only writer in the whole binary is
    // llm_strat_session_state_reset (find-cross-references-confirmed), never mutated inside this
    // closure's own loop.
    const uint8_t *bldg_completion_slot_count;
    // resources: map::resources[64][64], RID_RESOURCES -- ai/ai_state.h's own read-only binding of
    // the SAME region; this is sim/'s first. See sim_store::resources_at() for the mutable sibling.
    const map_resources *resources;

    // ================= SIM-RESID-IF re-close (2026-08-31): the READ surface the batch A/B
    // ================= translations declared and this header did not carry.
    //
    // advisor_due_delay / advisor_staff_threshold / advisor_interval: three .rdata doubles at
    // 0x00501775 / 0x0050177d / 0x00501785, each with EXACTLY ONE referrer in the whole binary --
    // llm_strat_advisor_tick. The delay is added to advisor_next_time for the due comparison, the
    // interval is added when the advisor re-arms, the threshold is the staffing bar it tests.
    const double *advisor_due_delay;
    const double *advisor_staff_threshold;
    const double *advisor_interval;

    // empty_name_str: char[1] "" at 0x0050109d, whose ADDRESS llm_strat_new_game_init pushes as
    // llm_strat_player_profile_init's `name_str` -- i.e. a new game seeds every profile with a
    // blank name. Physically the first NUL of the padding after " wynalazek "; Ghidra had merged
    // all three statics in that packed blob into one 15-byte string until 2026-08-31.
    const char *empty_name_str;

    // storage_stats_init_delay: double 10.0 at 0x0050109e (UNALIGNED -- the third static in that
    // same packed blob). One reader, no writer.
    //
    // CORRECTED 2026-08-31 (finding 2026-08-31-1757-1). This comment used to say map_FillDefaults
    // seeds _G_LLM_STRAT_STORAGE_STATS[i] +0x50 from game_clock + this, for i in 0..7. That is
    // what the code INTENDS and not what it does, and the translator's own rule 2b (derive the
    // index from the address, never from a field name or an existing comment) was pointed at this
    // very sentence before it was believed. The store @0x004562c3-0x004562d3 indexes with the
    // INNER loop counter EBP-0x1c -- left holding 10 by the cap_accum/cap_prev loop above it --
    // not the player counter EBP-0x18, so `IMUL EAX,[EBP-0x1c],0x58` + `FSTP [EAX+0xbf4d50]`
    // resolves to the FIXED address 0xbf50c0 on all eight iterations. storage_stats is
    // 0x00bf4d00..0x00bf4fc0, so no player's subtick_b_clock is ever seeded; 0xbf50c0 is 0x40
    // bytes into _G_LLM_STRAT_DEATH_ANIM_TABLE, which is clobbered eight times per call. It is a
    // PRESERVE-BUG -- reproduced, not fixed; see sim_store::death_anim_table_mut().
    const double *storage_stats_init_delay;

    // win_w / win_h: the strategic viewport extent (G_WIN_W = window width minus the 160px side
    // panel; G_WIN_H four bytes BEFORE it). Owned by Graphics/FX -- set once by
    // llm_gfx_view_metrics_init -- and only READ here: llm_game_land_players_on_planet centres the
    // initial camera on them. G_WIN_W was already a registered region with no view member;
    // G_WIN_H had 84 referrers in Ghidra and no addr constant at all.
    const int32_t *win_w;
    const int32_t *win_h;

    // player_desc_slots: `Players` (0x00e587e9), llm_strat_player_desc[8] stride 0x34 -- the
    // LOBBY-facing per-slot descriptor. NOT `profiles` (_G_LLM_STRAT_PLAYERS) and not `players`
    // (player_data): three different per-player arrays, and this header's own `profiles` alias
    // comment warns about exactly this confusion. llm_strat_session_begin_multi reads
    // controller_flags / race_or_faction / color_or_team / name / scenario_side_id out of it to
    // drive the per-slot profile init.
    const player_desc *player_desc_slots;

    // net_local_player_slot: RID_NET_LOCAL_PLAYER_SLOT -- the LOBBY's own local-slot index, which
    // llm_strat_session_begin_multi copies into PlayerSide. Deliberately NOT local_player_slot
    // above (RID_STRAT_LOCAL_PLAYER_SLOT): two different regions, and conflating them would make
    // the MP entry path read back what it is about to write.
    const uint16_t *net_local_player_slot;

    // net_lobby_scan_host_count: RID_NET_LOBBY_SCAN_HOST_COUNT. The MP entry path picks
    // SESSION_MP_LOCAL vs SESSION_MP_LOCKSTEP on `host_count <= 1`.
    const uint32_t *net_lobby_scan_host_count;

    // lockstep_session_adapt_delay: double @0x00501031, one referrer -- added to the game clock to
    // seed STRAT_LOCKSTEP_ADAPT_NEXT_TIME on the MP session-entry success path.
    const double *lockstep_session_adapt_delay;

    // ================= SIM-RESID-IF REOPEN (2026-08-31): the two read-only constants that
    // ================= blocked a translation each, one line apiece.
    //
    // ai_clock_stagger_fraction: double 0.125 @0x00506da8 (RID_STRAT_AI_CLOCK_STAGGER_FRACTION).
    // llm_strat_init_human_player_data @0x004dd9a8-0x004dd9e6 multiplies it by the player index and
    // each of ai_move/tactic/strategy_period to seed that player's ai_clock_m/_t/_s, so player N's
    // think phases start N/8 of a period apart. No writer.
    //
    // NOT ai_invasion_clock_stagger ABOVE, and the distinction is load-bearing: that one is
    // 0x00506da0, eight bytes earlier, read by llm_strat_spawn_invasion_force. The two constants
    // hold the SAME value (0.125, both read-memory-confirmed), so binding either for the other
    // would produce identical behaviour today and a silent divergence the day one is retuned.
    const double *ai_clock_stagger_fraction;

    // invention_name_placeholder: the 12-byte .rdata string " wynalazek " @0x00501091
    // (RID_STRAT_INVENTION_NAME_PLACEHOLDER). llm_strat_tech_tables_reset copies it into every
    // Progress[i] name field as the placeholder invention name. Same packed blob and same posture
    // as empty_name_str above -- Watcom pooled three statics into it, and this is the first.
    const char *invention_name_placeholder;

    // ================= SIM-RESID-F (2026-09-01): the six READ-ONLY regions the parked tutorial
    // ================= pair needed. Nothing in the sim closure writes any of them.
    //
    // tutorial_steps / tutorial_step_count: the loaded tutorial script and its count prefix
    // (RID_TUTORIAL_STEPS @0x0065732e, llm_tutorial_step[16] at 0x60c each; RID_TUTORIAL_STEP_COUNT
    // is the int32 four bytes BEFORE the base). llm_tutorial_load_script -- outside this closure --
    // is the sole writer of both. llm_tutorial_step_driver indexes [step - 1] and walks
    // .reakcje/.panel/.komenda; every one of its access sites uses the FULL 0x60c stride
    // (IMUL EAX,EAX,0x60c at 0x004ba968/0x004bad18/0x004bad4a/0x004bad69/0x004badbc), so the "buggy
    // 0x306 half-stride" the Ghidra field comment on llm_tutorial_step.body describes is some other
    // reader, not this one.
    const tutorial_step_record *tutorial_steps;
    const int32_t              *tutorial_step_count;

    // tutorial_colors_rgb: the tutorial's OWN source palette, int[5][3] = 60 B
    // (RID_TUTORIAL_COLORS_RGB). llm_tutorial_step_driver block-copies it over the shared
    // gfx_ui_color_at() table -- but the copy is REP MOVSD x12, i.e. exactly 48 bytes, because the
    // destination is int[4][3]. The FIFTH ROW IS NEVER READ by this path; the region is registered
    // at its true 60-byte extent and this closure reads 4 rows. Flattened [row * 3 + channel] to
    // match gfx_ui_color_at()'s own write-side layout.
    const int32_t *tutorial_colors_rgb;

    // ui_menu_async_callback_b: RID_UI_MENU_ASYNC_CALLBACK_B, the third slot of the contiguous
    // 3-pointer menu async-callback family (_CALLBACK @0x006542a9, _A @0x006542ad, _B @0x006542b1).
    // A GENUINE DYNAMIC READ, not an address escape: `CMP dword ptr [0x006542b1],0x0` @0x004ba936
    // gates nearly the whole of llm_tutorial_step_driver -- non-null means a menu callback owns the
    // screen, so the driver only restores tutorial UI state and returns. Read-only here on purpose:
    // unlike ui_menu_async_callback_a() (store-only, "installs and clears, never calls through"),
    // nothing in this closure writes _B.
    const void *const *ui_menu_async_callback_b;

    // ui_screen_main_menu_id / ui_screen_racebck_id: the two screen ids llm_game_start_tutorial
    // reads to seed ui_fade_transition().src_screen_id and .dst_screen_id. Both are real memory
    // reads (`MOV EAX,[addr]`), not immediates. racebck's only writer in the binary is
    // llm_boot_progress_draw, before any menu exists.
    const int32_t *ui_screen_main_menu_id;
    const int32_t *ui_screen_racebck_id;
};

// ---- the WRITE store ------------------------------------------------------------------------
//
// W2 and W3 live here. The pointers are private; the accessors hand back a reference to ONE record
// and nothing else, so there is no base for a translation to keep. The constructor is private with
// two friends, so a non-sim TU cannot obtain one at all.
//
// Grown demand-driven like the view: add a region here the first time a translated function writes
// it, together with the accessor that names what is being written.

struct sim_state;
sim_state state();

// The offline harness's heap-buffer binder (mh_nettest/sim_selftest.cpp). Named here rather than
// left to a cast because W3 is only meaningful if the sanctioned exceptions are enumerated: these
// two friends ARE the list, and adding a third is a diff a reviewer sees.
struct sim_fixture;

class sim_store {
public:
    sim_store(const sim_store &)            = default;
    sim_store &operator=(const sim_store &) = default;

    // ---- the rosters ----
    // One record, by reference. Deliberately NOT a pointer and NOT a row: the caller gets exactly
    // the record it named, so `store.unit_at(p, i)` cannot become `... + 1` or outlive a rebase.
    // Bounds are NOT checked -- several original bodies index deliberately out of range (see the
    // ai_store note on players[p + 1]) and a reimplementation must be able to express that. The
    // accessor exists to remove the ADDRESS, not to add a check the original does not have.
    unit        &unit_at(uint32_t player, int32_t index) { return units_[player * caps_.units + index]; }
    player_data &player_at(uint32_t player) { return players_[player]; }

    // _G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER -- byte[MAX_PLAYERS]. The 5th (stack) `move_flag` arg every
    // retail caller of llm_strat_unit_order_move(_enqueue) passes; llm_strat_unit_order_exit_storage_
    // enqueue's storage-rejected fallback is the one function in SIM1C that writes it
    // (INC with a wraparound-skip-zero, so the sequence never revisits 0).
    uint8_t &order_seq_id_by_player(uint32_t player) { return order_seq_id_[player]; }

    // _G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT. SIM1A's llm_strat_unit_passive_engage_tick
    // resets this to 0 at four points before claiming the scratch buffer (see sim_view -- there is
    // no reader-side member for the buffer itself in this closure, only the count) -- the same
    // reset-then-fill transient the AI domain's ai_store::engage_scratch_count binds independently
    // (same RID, two legitimate bindings, same reasoning as order_count() above).
    int32_t &engage_candidate_scratch_count() { return *engage_candidate_scratch_count_; }

    // ---- SIM1C: the roster half llm_strat_order_queue_dispatch writes -------------------------
    // Same one-record-by-reference contract as unit_at(). The three sub-rosters are addressed
    // [player][building.sub_id], never by an index the order carries -- the dispatcher always reaches
    // them through the building it is acting on -- but the accessor takes the raw slot because the
    // sub_id lookup is the CALLER's step and hiding it would hide a real indirection.
    building   &building_at(uint32_t player, int32_t index) { return buildings_[player * caps_.buildings + index]; }
    turret     &turret_at(uint32_t player, int32_t slot) { return turrets_[player * caps_.turrets + slot]; }
    production &production_at(uint32_t player, int32_t slot) { return productions_[player * caps_.productions + slot]; }
    lab        &lab_at(uint32_t player, int32_t slot) { return labs_[player * caps_.labs + slot]; }

    // SIM1D (2026-08-13). See sim_view::prod_shuttle_slots's comment -- same RID, mutable
    // sibling. llm_strat_production_complete is this closure's writer (travel_duration/status/
    // origin_planet on arrival).
    prod_shuttle_slot &prod_shuttle_slot_at(uint32_t player, int32_t slot) {
        return prod_shuttle_slots_[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot];
    }

    // ---- SIM1C: the order queue, which the dispatcher CONSUMES ---------------------------------
    //
    // WHY THE SIM BINDS THIS AT ALL, when mh::orders owns the container. The dispatcher is the
    // queue's READER: it walks every record, executes it, and COMPACTS the survivors to the front,
    // then stores the survivor count. That filter-in-place is the dispatcher's own logic, not a
    // container operation, and mh::orders exposes no interface for it (`pending_count`/
    // `staging_count` are deliberately the only order state it publishes outward).
    //
    // This is NOT the second-independent-binding hazard ST1 warns about. Both modules resolve the
    // same RID through mh::state::ptr, so a rebase moves both -- exactly as both already bind
    // RID_UNITS independently. The hazard is a binding that does NOT come from the registry.
    //
    // ONE ORDER, BY REFERENCE, like every other accessor here: no `order *` escapes, so the
    // compaction has to name both the source and destination slot rather than doing pointer
    // arithmetic across the array.
    order   &order_at(int32_t index) { return order_queue_[index]; }
    int32_t &order_count() { return *order_queue_count_; }

    // ---- SIM1C: the lockstep horizon extension (the kind-0xf0 order) ----------------------------
    // Both are written by ONE case of the dispatcher: it raises our requested horizon by one step
    // and sets the 0x40 "extension requested" bit so the request fires once. `horizon` is also
    // written by mh::orders::schedule() -- same region, same registry, two legitimate writers.
    double  &lockstep_horizon() { return *lockstep_horizon_; }
    uint8_t &net_lockstep_status_flags() { return *net_lockstep_status_flags_; }

    // ---- the presentation scratch buffer, and why it is HERE ------------------------------------
    //
    // G_TEXT_TMP is the 512-byte wide-string buffer the dispatcher formats its "<building>: <reason>"
    // messages into before handing them to the UI. It is the one address in this class that MUST
    // escape, because it is an OUT-PARAM to an original callee (mh::call::w_sprintf__vss) rather
    // than something we write ourselves -- so W2's "no address ever escapes" is bought out here
    // deliberately, and narrowly: a 512-byte buffer whose region record carries MF_VIEW and nothing
    // else (no MF_HASH, no MF_SAVE, no MF_MEASURED), i.e. it reaches neither the determinism hash nor
    // a save file, and every site discards w_sprintf's return value. Nothing about the simulation
    // can be read back out of it. It is in sim_store rather than sim_view only because the sim does
    // cause it to be written; it is not sim state.
    wchar_t *text_scratch() { return text_scratch_; }

    // G_TEXT_PTRS[text_id], MUTABLE -- the write half of sim_view::text_ptrs (same RID). Added
    // 2026-08-31 for llm_strat_scenario_planet_clone, which is the domain's ONLY writer of the
    // string table: one store, `G_TEXT_PTRS[0xa8] = llm_str_ansi_to_wide(...)` @0x0045bb4d, giving
    // the cloned scenario planet its display name. Everything else in this closure reads the table.
    //
    // THE SLOT is mutable; the STRING it points at is not, which is why this hands back
    // `const wchar_t *&` and the binding is `ptr<const wchar_t *>` -- that resolves to
    // `const wchar_t **`, an assignable slot whose pointee stays const. The pointer stored comes
    // from an original callee that owns the allocation; we never write through it.
    //
    // THE BINDING IS SPELLED `ptr<text_slot>` (the alias at the top of this namespace), not `ptr<const wchar_t *>`,
    // and that is not cosmetic. check_state_bindings.py reads the const-ness of `T` in `ptr<T>` as
    // the write-vs-read verdict; written out longhand, the leading `const` there belongs to the
    // POINTEE and the checker would read the whole binding as read-only -- reporting G_TEXT_PTRS
    // unbound for a writer that can in fact write it. The alias names the ELEMENT, which is what
    // the rule is actually about, and the element is assignable. `ptr<const wchar_t *const>` (the
    // view's own binding, four hundred lines up) is the genuinely read-only form.
    const wchar_t *&text_ptr_at(int32_t text_id) { return text_ptrs_mut_[text_id]; }

    // ---- SIM1A (llm_strat_unit_tick and neighbours) ------------------------------

    // _G_LLM_STRAT_CUR_UNIT dereferenced. See sim_view::cur_unit's comment: both are bound from the
    // SAME resolved pointer value, so a read through one and a write through the other name the
    // same record. Written by unit_tick (activity_clock), update_anim (dmg_smoke_*),
    // update_rotation (facing_current/facing_target/rotation_clock).
    unit &cur_unit() { return *cur_unit_; }

    // _G_LLM_STRAT_TICK_BUDGET. Written once by unit_tick per tick; read by it and by
    // update_soldiers via sim_view::tick_budget. Also zeroed (both dwords) by
    // llm_strat_bldg_state_destroyed's tail -- SIM1B (building_tick machinery) --
    // reusing the SAME ambient scratch double the unit-tick driver uses, not a second region.
    double &tick_budget() { return *tick_budget_; }

    // _G_LLM_STRAT_STATE_LOOP_GUARD. unit_tick's own runaway-state-loop iteration counter; no other
    // reader or writer in the closure.
    int32_t &state_loop_guard() { return *state_loop_guard_; }

    // SIM1D (2026-08-13). _G_LLM_STRAT_PROD_COMPLETE_THROTTLE -- a shared (not
    // per-player) counter llm_strat_production_complete both reads and writes; no other reader or
    // writer in the closure. Not per-player, so a plain scalar accessor like state_loop_guard above.
    uint8_t &prod_complete_throttle() { return *prod_complete_throttle_; }

    // One control group, by reference -- same one-record contract as unit_at(). unit_ctrl_group_
    // assign takes `&store.ctrl_group_at(g).count` as the out-param the original ctrlgroup add/
    // remove helpers write through (an address escape to an ORIGINAL callee, same exception
    // text_scratch() documents).
    ctrl_group &ctrl_group_at(int32_t group_index) { return ctrl_groups_[group_index]; }

    // The map occupancy plane, MUTABLE. Same (tile_x<<8)|tile_y indexing as sim_view's tile_at()
    // helper, and the SAME RID -- not a second independent binding. llm_unit_create_soldier is the
    // first (and, as of this slice, only) writer in the 307-function closure.
    tile_object &tile_object_at(int32_t tile_x, int32_t tile_y) {
        return planes_.tile_object_at(tile_x, tile_y);
    }

    // map::g::passable, MUTABLE, same indexing/RID as sim_view::passable. Written by
    // llm_unit_create_soldier when it claims a tile.
    uint8_t &passable_at(int32_t tile_x, int32_t tile_y) {
        return planes_.passable_at(tile_x, tile_y);
    }

    // _G_LLM_STRAT_POP_STATS, MUTABLE, same RID as sim_view::population (llm_strat_order_queue_
    // dispatch's read-only binding). Written by llm_unit_create_soldier's population-stat branch.
    pop_stats &population_at(uint32_t player) { return population_[player]; }

    // One soldier record, by reference, same RID as sim_view::soldiers. Written by
    // llm_strat_unit_update_soldiers (walk interpolation + idle-animation fields).
    soldier &soldier_at(uint32_t player, int32_t index) { return soldiers_[player * caps_.soldiers + index]; }

    // SIM1-G1 (2026-08-19). One path waypoint, by reference, same RID as sim_view::path_buffers.
    // Indexing player*30000 + slot*300 + entry read off move_walker/group_marshal. Written by
    // llm_strat_unit_state_move_walker (decrements .run_length as each waypoint is spent).
    path_waypoint &path_buffer_at(uint32_t player, int32_t slot_id, int32_t entry) {
        return planes_.path_waypoint_at(player, slot_id, entry);
    }

    // One group-move scratch member, by reference, same RID as sim_view::group_move_scratch. Written
    // by llm_strat_unit_state_group_marshal (the wave-compaction memcpy shuffles surviving members
    // down the array). NOT bounds-checked (W2) -- the cap-100 discipline lives in the marshal loop.
    group_scratch_member &group_move_scratch_at(int32_t index) { return group_move_scratch_[index]; }

    // SIM1-G1 (2026-08-20). _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG, MUTABLE, same RID as
    // sim_view::pathfinder_air_mode_flag. A BOOLEAN: llm_strat_unit_group_step_ground writes 0,
    // llm_strat_unit_group_step_plane writes 1 then clears to 0, llm_strat_trace_greedy_path reads
    // it against 0 -- corrected 2026-08-20 from the reference set; see the sim_view member.
    int32_t &pathfinder_air_mode_flag() { return *pathfinder_air_mode_flag_; }

    // SIM1-G1 (2026-08-20). llm_strat_group_move_order_commit's mutable writes -- same
    // RIDs as the sim_view members above.
    route_step   &group_route_step_at(int32_t index) { return group_route_steps_[index]; }
    group_member &group_member_at(int32_t index) { return group_members_[index]; }
    uint32_t     &path_wrap_mask_mut() { return *path_wrap_mask_; }
    int32_t      &group_order_goal_x_mut() { return *group_order_goal_x_; }
    int32_t      &group_order_goal_y_mut() { return *group_order_goal_y_; }
    int32_t      &group_anchor_x_mut() { return *group_anchor_x_; }
    int32_t      &group_anchor_y_mut() { return *group_anchor_y_; }
    int32_t      &group_order_owner_mut() { return *group_order_owner_; }
    int32_t      &group_member_count_mut() { return *group_member_count_; }

    // SIM1-G-PREP (2026-08-20). The mutable half of the batch-G write set pre-registered in sim_view
    // above -- same RIDs, one accessor per global that a not-yet-translated G function WRITES. They
    // exist so a batch never has to wire a region inline (R8); read the sim_view block and the
    // dll_addr_manifest note at each address for what the value MEANS before using one.
    //
    // (a) group-move working state.
    int32_t &group_centroid_x_mut() { return *group_centroid_x_; }
    int32_t &group_centroid_y_mut() { return *group_centroid_y_; }
    int32_t &group_path_build_idx_mut() { return *group_path_build_idx_; }

    // (b) map-region routing scratch. cand_scratch is addressed BY BYTE with a 4-byte slot stride
    // (x at slot*4+0, y at slot*4+1) because that is exactly what route_search's two unrolled loops
    // write -- there is no struct here to name, only a documented stride. NOT bounds-checked (W2).
    uint8_t         &region_route_cand_byte(int32_t byte_index) { return region_route_cand_scratch_[byte_index]; }
    uint32_t        &region_flood_tile_at(int32_t index) { return region_flood_tile_queue_[index]; }
    llm_map_region *&region_route_bfs_at(int32_t index) { return region_route_bfs_queue_[index]; }

    // (c) the invasion-pacing completion counter.
    int32_t &bldg_completion_accum_mut() { return *bldg_completion_accum_; }

    // SIM1-G1 tail slice (2026-08-20). _G_LLM_STRAT_GROUP_MEMBER_TILE, MUTABLE, same RID as
    // sim_view::group_member_tile -- that member's own comment used to say "no writer found"; that
    // was wrong, llm_strat_group_plan_formation_positions writes it. Addressed BY BYTE with a 2-byte
    // slot stride (col at slot*2+0, row at slot*2+1), same raw-byte-accessor shape as
    // region_route_cand_byte() above. NOT bounds-checked (W2).
    uint8_t &group_member_tile_byte(int32_t byte_index) { return group_member_tile_[byte_index]; }

    // (d) the unit-queue tile search.
    uint32_t          &unitq_cur_tile_mut() { return *unitq_cur_tile_; }
    int32_t           &unitq_closed_count_mut() { return *unitq_closed_count_; }
    int32_t           &unitq_iter_mut() { return *unitq_iter_; }
    int32_t           &unitq_frontier_count_mut() { return *unitq_frontier_count_; }
    unitq_search_node &unitq_closed_at(int32_t index) { return unitq_closed_[index]; }
    unitq_search_node &unitq_frontier_at(int32_t index) { return unitq_frontier_[index]; }

    // (e) the pathtrace working state. half_w_m1/half_h_m1 have accessors despite having no reader
    // anywhere in the binary -- the STORE is what has to be transcribable (Law 2).
    uint32_t &pathtrace_coord_mask_mut() { return *pathtrace_coord_mask_; }
    uint32_t &pathtrace_col_mask_mut() { return *pathtrace_col_mask_; }
    uint32_t &pathtrace_row_mask_mut() { return *pathtrace_row_mask_; }
    uint32_t &pathtrace_map_w_mut() { return *pathtrace_map_w_; }
    uint32_t &pathtrace_map_h_mut() { return *pathtrace_map_h_; }
    uint32_t &pathtrace_half_w_mut() { return *pathtrace_half_w_; }
    uint32_t &pathtrace_half_h_mut() { return *pathtrace_half_h_; }
    uint32_t &pathtrace_half_w_m1_mut() { return *pathtrace_half_w_m1_; }
    uint32_t &pathtrace_half_h_m1_mut() { return *pathtrace_half_h_m1_; }
    int32_t  &pathtrace_neg_half_w_mut() { return *pathtrace_neg_half_w_; }
    int32_t  &pathtrace_neg_half_h_mut() { return *pathtrace_neg_half_h_; }
    uint32_t &pathtrace_start_col_mut() { return *pathtrace_start_col_; }
    uint32_t &pathtrace_start_row_mut() { return *pathtrace_start_row_; }
    uint32_t &pathtrace_walk_dir_mut() { return *pathtrace_walk_dir_; }
    uint32_t &pathtrace_goal_col_mut() { return *pathtrace_goal_col_; }
    uint32_t &pathtrace_goal_row_mut() { return *pathtrace_goal_row_; }
    uint32_t &pathtrace_goal_packed_mut() { return *pathtrace_goal_packed_; }
    uint32_t &pathtrace_approach_dir_mut() { return *pathtrace_approach_dir_; }
    // dirs[] is indexed one PAST pathtrace_len for the terminator byte -- that is intended, not an
    // overrun; the region is 512 bytes and the live length never reaches it.
    uint8_t  &pathtrace_dir_at(int32_t index) { return pathtrace_dirs_[index]; }
    uint16_t &pathtrace_pos_at(int32_t index) { return pathtrace_pos_[index]; }
    uint32_t &pathtrace_best_dir_mut() { return *pathtrace_best_dir_; }
    int32_t  &pathtrace_best_dist_mut() { return *pathtrace_best_dist_; }
    uint32_t &pathtrace_forbid_cell_at(int32_t index) { return pathtrace_forbid_cells_[index]; }
    // _G_LLM_STRAT_PATHTRACE_LEN was already REGISTERED (sim_view::pathtrace_len, bound read-only for
    // llm_strat_unit_group_step_ground) but had no mutable accessor -- and R8 checks registration,
    // not bindability, so it did not flag it. Both llm_strat_trace_greedy_path (write) and
    // llm_strat_pathtrace_remove_loops (read+write) need this one; added here rather than leaving
    // batch G2 to discover it.
    uint32_t &pathtrace_len_mut() { return *pathtrace_len_; }

    // _G_LLM_STRAT_UNIT_HOUSING_STATS[8], MUTABLE, same RID as sim_view::unit_housing (which
    // llm_unit_recruit added read-only for its per-category cap checks) and the same RID ai_state.h
    // binds independently. Added SIM1A for the two writers the batch reached at once:
    // llm_strat_unit_housing_count_add (the whole point of that function) and
    // llm_strat_unit_spawn_on_tile's used_soldiers bump.
    //
    // THE INDEX IS NOT ALWAYS A PLAYER. The two callers disagree in vocabulary and both are right:
    // spawn_on_tile indexes by `player` (SHL EAX,6 -- sizeof(housing_stats) == 64), while
    // housing_count_add takes it as a `housing_id` parameter from its own callers. Same table, same
    // stride; the parameter is named for the arithmetic rather than for either caller's story.
    housing_stats &unit_housing_at(int32_t housing_id) { return unit_housing_[housing_id]; }

    // _G_LLM_STRAT_PATH_SLOT_FLAGS[8][100], MUTABLE, byte per slot. Added SIM1A --
    // llm_strat_path_free_slot's release write (`path_slot_flags_at(player, slot) = 0`). No reader
    // in this closure (the allocate side that SETS a flag is outside the migration set).
    uint8_t &path_slot_flag_at(int32_t player, int32_t slot) {
        return planes_.path_slot_flag_at(player, slot);
    }

    // _G_LLM_STRAT_PATH_FREE_SLOT_COUNT[8], MUTABLE, int32_t per player. Added SIM1A --
    // llm_strat_path_free_slot's only other write (increment on release). No reader in this closure.
    int32_t &path_free_slot_count_at(int32_t player) {
        return planes_.path_free_slot_count_at(player);
    }

    // _G_LLM_CLICK_SELECT_TARGET_ID, MUTABLE, plain uint16_t scalar (not per-player). Added SIM1A
    // -- llm_strat_unit_remove_from_map/_on_destroyed both clear it (= 0) after
    // confirming it pointed at the unit being removed; sim_view::click_select_target_flags is the
    // read-only companion they gate on first, same RID family as this one (RID_CLICK_SELECT_TARGET_ID).
    uint16_t &click_select_target_id() { return *click_select_target_id_; }

    // player_profile[8] (_G_LLM_STRAT_PLAYERS), MUTABLE, same RID as sim_view::profiles. Added
    // SIM1A -- llm_strat_unit_on_destroyed's only write in this slice
    // (`primary_mother_unit[*v.planet_index] = 0`, the mother-ship-lost clear). Every other read of
    // this table in the migration set stays through the const `profiles` view.
    player_profile &profile_at(int32_t player) { return profiles_[player]; }

    // mh_llm_strat_map_geom::change_flag / change_flag2, MUTABLE, same RID_GENERAL as sim_view::geom
    // -- NOT a second independent binding of a different region, the mutable twin of the same
    // struct. Added SIM1A for llm_strat_unit_notify_ui's local-player arm.
    //
    // CORRECTED 2026-08-12 (reimpl-verify caught it): the original's store at 0x00488a4b is a SINGLE
    // 4-byte `MOV dword ptr [general+0x24],1`, and change_flag (int16_t @0x24) / change_flag2
    // (int16_t @0x26) are contiguous with no padding (static_assert'd, struct ends at 0x28) -- so
    // that one instruction sets BOTH fields (change_flag=1, change_flag2=0) atomically. The Ghidra
    // .c draft's two-statement rendering was RIGHT; an earlier reading of "one instruction, so only
    // one field" was backwards about what a dword store touches. Both accessors exist so a caller
    // sets both explicitly, matching the .c draft's own shape.
    int16_t &change_flag() { return geom_mut_->change_flag; }
    int16_t &change_flag2() { return geom_mut_->change_flag2; }

    // LT1C (2026-09-02). llm_map_setup_dimensions recomputes the whole wrap-mask/extent record --
    // six further fields of the SAME RID_GENERAL geom_mut_ binding (the change_flag() precedent),
    // NOT a new region. Per-field per house style.
    uint32_t &bw_mask_mut() { return geom_mut_->bw_mask; }
    uint32_t &bh_mask_mut() { return geom_mut_->bh_mask; }
    uint32_t &width_mask_mut() { return geom_mut_->width_mask; }
    uint32_t &height_mask_mut() { return geom_mut_->height_mask; }
    uint32_t &big_width_mut() { return geom_mut_->big_width; }
    uint32_t &big_height_mut() { return geom_mut_->big_height; }

    // LT1C (2026-09-02). The map-region POOL slots -- the allocator this closure now owns
    // (map_block_8_GetNextBlock / llm_map_region_free / llm_map_region_pool_reset). The active-list
    // head is the mutable twin of sim_view::region_list_head (same RID, write half -- not a second
    // independent binding); the free head, by-index table (llm_map_region*[4096], an address-escape
    // shape: the elements are raw heap pointers), node-index source (starts at 1 -- index 0 is never
    // allocated) and live-node count (a COUNT despite the G_LAST_MAP_INDEX name: ++/--/:=0) had no
    // binding anywhere before LT1C brought the pool in.
    llm_map_region *&region_list_head_mut() { return *region_list_head_mut_; }
    llm_map_region *&region_pool_free_head() { return *region_pool_free_head_; }
    llm_map_region **region_by_index() { return region_by_index_; }
    int32_t         &region_alloc_counter() { return *region_alloc_counter_; }
    int32_t         &last_map_index() { return *last_map_index_; }

    // LT1D (2026-09-02). llm_menu_force_return_to_main's teardown writes: the async-callback BASE
    // slot (0x006542a9 -- the store already binds _A at +4; d5 writes both), the menu state byte,
    // and the forced-quit-teardown flag.
    void   **ui_menu_async_callback_base() { return ui_menu_async_callback_base_; }
    uint8_t &ui_menu_state_mut() { return *ui_menu_state_mut_; }
    int32_t &quit_teardown_forced_flag() { return *quit_teardown_forced_flag_; }

    // LT1D (2026-09-02). The ambient planet-event table (RID_SND_AMBIENT_BY_PLANET, 23168 B =
    // 32 planets x 0x2d4), whole-region ADDRESS ESCAPE like fog_of_war_base(): the record layout
    // OVERLAPS by design (each event's next_time double at event+0x20 spills into the next event's
    // first dword at stride 0x24; the last one ends exactly at the planet stride), so no Ghidra
    // struct can model it -- consumers use named byte offsets (see sim_lt_ambient.cpp).
    uint8_t *snd_ambient_by_planet_base() { return snd_ambient_by_planet_base_; }

    // LT1C c4 (2026-09-02). _G_LLM_STRAT_BLDG_CELL_GRID[type_id*64 + row*8 + col], MUTABLE
    // (RID_STRAT_BLDG_CELL_GRID, byte[100][8][8]): llm_strat_bldg_recompute_cell_grid's derived
    // per-building-TYPE footprint+halo cache, rebuilt once per planet-map session init. FLAT byte
    // accessor on purpose: the original addresses the 8-connected halo probes as flat byte deltas
    // (-9..+9) off one running index and the translation mirrors that arithmetic exactly
    // (sim_lt_bldg_cell_grid.cpp). NO READER of either output was found by LT-PREP's 4-method
    // sweep -- see the unit header banner and the c4 runtime probe record.
    uint8_t &bldg_cell_grid_byte(int32_t flat_index) { return bldg_cell_grid_[flat_index]; }
    // LT1C c4 (2026-09-02). _G_LLM_STRAT_BLDG_CELL_GRID_ROW_SHIFT[type_id], MUTABLE
    // (RID_STRAT_BLDG_CELL_GRID_ROW_SHIFT, int32[100]): 0, or -1 for a type-0x1c building --
    // SUBTRACTED from the stamped row index when the 5x5 cfg area is copied into the 8x8 grid.
    int32_t &bldg_cell_grid_row_shift_at(int32_t type_id) {
        return bldg_cell_grid_row_shift_[type_id];
    }

    // mh_llm_strat_map_geom::pathfinder_workbuf (+0x1c) / ::pathfinder_params (+0x0c), MUTABLE,
    // the SAME RID_GENERAL binding as change_flag() above -- two further fields of an
    // already-bound struct, NOT a new region. Added by the SIM-RESID-IF reopen (2026-08-31) for
    // llm_strat_pathfinder_init, which allocates both blocks and stores them here (the workbuf at
    // 0x00461555-0x00461585, the params block at 0x0046158f-0x004615ae) -- the two pointers every
    // later pathfinder call reaches its working state through.
    //
    // The params field is TYPED (llm_strat_pathfinder_params *, 0x20 bytes) and this accessor
    // keeps that type rather than handing back `void *&`: the same function immediately writes
    // three of its fields, and a void reference would put a cast at every one of them.
    void                                     *&pathfinder_workbuf_mut() { return geom_mut_->pathfinder_workbuf; }
    mh::game::mh_llm_strat_pathfinder_params *&pathfinder_params_mut() {
        return geom_mut_->pathfinder_params;
    }

    // _G_LLM_STRAT_BLDG_ENCLOSURE_SCRATCH, MUTABLE, OWN_ISLAND -- the PRIVATE 256x256 byte grid
    // llm_strat_bldg_check_placement_encloses_neighbors uses as its own working set (seed from
    // `passable`, stamp the proposed footprint, flood-fill from existing buildings' 31x31 zones).
    // Added SIM1B (2026-08-12), the closure's only reader/writer -- the region was pre-declared
    // (addr/mh_addrs.gen.h's own comment) specifically so this function's shadow arm can run for
    // real without the scratch escaping the rollback. Same (x<<8)|y indexing as tile_object_at().
    uint8_t &bldg_enclosure_scratch_at(int32_t tile_x, int32_t tile_y) {
        return bldg_enclosure_scratch_[(tile_x << 8) | tile_y];
    }

    // _G_LLM_STRAT_POWER_STATS[player], MUTABLE -- see the `power_stats` alias above.
    // llm_strat_power_recompute is the closure's only reader+writer (reads generated/consumed,
    // writes prev_generated/prev_consumed/ratio through the same reference).
    power_stats &power_stats_at(uint32_t player) { return power_stats_[player]; }

    // _G_LLM_STRAT_CUR_BUILDING dereferenced. See sim_view::cur_building's comment: both are bound
    // from the SAME resolved pointer value, so a read through one and a write through the other name
    // the same record. Written by llm_strat_bldg_state_destroyed (cycle_progress/state/anim[0]).
    building &cur_building() { return *cur_building_; }

    // _G_LLM_STRAT_CAM_PAN_TARGET_COL/_ROW, MUTABLE, plain int32_t scalars (not per-player). Added
    // SIM1B building_tick machinery -- llm_strat_bldg_state_destroyed's local-player arm
    // is the closure's only writer. No sim_view read sibling: nothing in this closure reads them back.
    int32_t &cam_pan_target_col() { return *cam_pan_target_col_; }
    int32_t &cam_pan_target_row() { return *cam_pan_target_row_; }

    // SIM1E (2026-08-15). _G_LLM_CAM_JUMP_QUEUE, MUTABLE, the screen-shake keyframe
    // list -- see the cam_jump_offset alias's comment for why ONE region gets TWO accessors: the
    // first 30 entries are `llm_vec2i {dx, dy}` and the 30 that follow are raw `double` scales,
    // which is what the disassembly stores even though Ghidra types the whole span `llm_vec2i[60]`.
    // llm_strat_spawn_debris_burst is the closure's only writer of all three members. No sim_view
    // read siblings: nothing in this closure reads them back (the camera code outside it does).
    cam_jump_offset &cam_jump_offset_at(int32_t slot) { return cam_jump_offsets_[slot]; }
    double          &cam_jump_scale_at(int32_t slot) { return cam_jump_scales_[slot]; }
    int32_t         &cam_jump_queue_count() { return *cam_jump_queue_count_; }

    // _G_LLM_STRAT_PLANET_MOTHER_LOST_TIME[32], MUTABLE, one double per planet. Added SIM1B
    // building_tick machinery -- llm_strat_bldg_state_destroyed's only write in this
    // closure (stamps the game clock when the LAST enemy mothership on a planet is destroyed). No
    // sim_view read sibling: nothing in this closure reads it back.
    double &planet_mother_lost_time_at(int32_t planet_index) { return planet_mother_lost_time_[planet_index]; }

    // map::g::mines[player][slot], MUTABLE, same RID as sim_view::mines (SIM1B's first binding of
    // this region, read-only). Added SIM1B building_tick machinery --
    // llm_strat_bldg_unmap_footprint's MINE-type teardown arm is the closure's first writer.
    mine &mine_at(uint32_t player, int32_t slot) { return mines_[player * caps_.mines + slot]; }

    // map::object::unit_storage[player][slot], MUTABLE, same RID as sim_view::storage (bound
    // read-only for SIM1C's storage_of helper). Added SIM1B building_tick machinery --
    // llm_strat_bldg_unmap_footprint's BARRAKS/GARAGE/.../SHUTTLE-type teardown arm is the closure's
    // first writer (b_index/docked_count/docked_units[]/occupancy).
    unit_storage &storage_at(uint32_t player, int32_t slot) { return storage_[player * caps_.storage + slot]; }

    // _G_LLM_STRAT_UI_SELECTED_BLDG_INDEX, MUTABLE, plain uint16_t scalar (not per-player) -- already
    // MF_SAVE-tracked, this is its first MF_VIEW binding. Added SIM1B building_tick machinery third
    // slice -- llm_strat_bldg_unmap_footprint's local-player UI-selection-cleanup arm is the
    // closure's only reader+writer (clears it to 0 when the torn-down building was selected).
    uint16_t &ui_selected_bldg_index() { return *ui_selected_bldg_index_; }

    // ---- SIM1F (2026-08-18): game_SetEvent's UI-panel/event-queue/chat-input state ----
    // game_SetEvent is the strategic-HUD panel dispatcher and it is the sim closure's writer of all of
    // these. ui_panel_mode/_page reuse the SAME RIDs sim_view binds read-only (bldg_unmap_footprint's
    // reader) -- one region, a read view and a write accessor, exactly like storage/mines above.
    int32_t &ui_panel_mode() { return *ui_panel_mode_; }
    int32_t &ui_panel_page() { return *ui_panel_page_; }
    int32_t &ui_panel_switch_pending() { return *ui_panel_switch_pending_; }
    int32_t &ui_bldg_panel_refresh_pending() { return *ui_bldg_panel_refresh_pending_; }
    int32_t &ui_unit_panel_refresh_pending() { return *ui_unit_panel_refresh_pending_; }
    int32_t &ui_mainpanel_refresh_pending() { return *ui_mainpanel_refresh_pending_; }
    int32_t &ui_mainpanel_tab_index() { return *ui_mainpanel_tab_index_; }
    int32_t &ui_unit_tab_toggle() { return *ui_unit_tab_toggle_; }
    int32_t &ui_view_resize_pending() { return *ui_view_resize_pending_; }
    int32_t &ui_event_queue_pos() { return *ui_event_queue_pos_; }
    // _G_LLM_STRAT_UI_EVENT_QUEUE_BUF[256] (game_e_event, 4 bytes each -> int32_t here). The deferral
    // ring buffer; game_SetEvent's defer path writes QUEUE_BUF[pos] and a trailing 0xffffffff terminator.
    int32_t &ui_event_queue_at(int32_t i) { return ui_event_queue_buf_[i]; }
    int32_t &chat_input_active() { return *chat_input_active_; }
    int32_t &chat_input_len() { return *chat_input_len_; }
    int32_t &chat_input_cursor() { return *chat_input_cursor_; }
    // char[81]; game_SetEvent only writes [0]=0 (CHAT_INPUT_OPEN). Returns the base.
    char *chat_input_line() { return chat_input_line_; }

    // SIM1F (2026-08-18): llm_strat_player_presence_lost's two mutable session-level
    // writes. debug_resource_yield_cut is a DOUBLE (_G_LLM_STRAT_DEBUG_RESOURCE_YIELD_CUT @0xe58356);
    // presence_lost stores 0.5 (byte-identically to the original's two dword halves 0 / 0x3fe00000).
    // session_mode (_G_LLM_GAME_SESSION_MODE) downgrades 3->2 (MP lockstep -> local) on game-over --
    // the read-only sim_view::session_mode above binds the same region (RID_GAME_SESSION_MODE).
    double  &debug_resource_yield_cut() { return *debug_resource_yield_cut_; }
    int32_t &session_mode() { return *session_mode_w_; }

    // SIM1F (llm_strat_spawn_invasion_force). _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT,
    // MUTABLE -- spawn_invasion_force raises it to player+1 when the spawned player is beyond the
    // current high-water mark (the read side is sim_view::ai_active_player_count, same region).
    int32_t &ai_active_player_count() { return *ai_active_player_count_w_; }

    // _G_LLM_STRAT_CUR_PROJECTILE dereferenced. See sim_view::cur_projectile's comment: both are
    // bound from the SAME resolved pointer value, so a read through one and a write through the
    // other name the same record. llm_strat_projectile_tick is the closure's only writer.
    projectile &cur_projectile() { return *cur_projectile_; }

    // One projectile-pool slot, by reference, same RID as sim_view::projectile_pool. Its only use
    // in this closure is slot 0's `.active` field as the pool's live-count (see the struct's own
    // field comment) -- llm_strat_projectile_tick decrements it when a tracked projectile dies.
    projectile &projectile_pool_at(int32_t slot) { return projectile_pool_[slot]; }

    // _G_LLM_STRAT_CUR_FX_ANIM dereferenced. See sim_view::cur_fx_anim's comment: both are bound
    // from the SAME resolved pointer value. llm_strat_fx_anim_tick is the closure's only writer.
    fx_anim &cur_fx_anim() { return *cur_fx_anim_; }

    // One fx-anim-pool slot, by reference, same RID as sim_view::fx_anim_pool. llm_fx_anim_seq_cancel
    // writes arbitrary slots by raw index (clearing `.live`); llm_strat_fx_anim_tick writes slot 0's
    // header `.live` (the live-count) through this same accessor on chain-end.
    fx_anim &fx_anim_pool_at(int32_t slot) { return fx_anim_pool_[slot]; }

    // ---- SIM1E fog/sight family (2026-08-16) ---------------------------------------------------
    //
    // fog_of_war::visible_by_count, MUTABLE, indexed TILE-MAJOR (`((tile_x<<8)|tile_y)*
    // FOG_PLAYERS_PER_TILE + player`), matching ai_state.h's own established fog_visible_count_at()
    // workaround -- NOT mh_map_struct_fog_of_war::visible_by_count[player][x][y], whose committed
    // PLAYER-major dimension order is wrong for how every function in this closure actually reaches
    // these bytes (see sim_fog_of_war.cpp's derivation). map_fow_UpdateFoWPlus_impl increments;
    // llm_strat_fow_remove_sight_apply decrements.
    uint8_t &fog_visible_by_count_at(int32_t tile_x, int32_t tile_y, uint32_t player) {
        return fog_visible_by_count_[(((tile_x << 8) | tile_y) * FOG_PLAYERS_PER_TILE) + player];
    }

    // fog_of_war::discovered, MUTABLE, ONE byte per tile (a per-player bit mask), tile-major
    // `(tile_x<<8)|tile_y` -- this half DOES match the committed struct's layout. Both fog/sight
    // functions OR a player's bit into it; no function in this closure clears it.
    uint8_t &fog_discovered_at(int32_t tile_x, int32_t tile_y) { return fog_discovered_[(tile_x << 8) | tile_y]; }

    // G_TMP_PLAYER/_x/_y/_SIGHT, MUTABLE -- the fog/sight family's shared staging globals.
    // map_fow_UpdateFoWPlus/llm_strat_fow_remove_sight write all four before calling out to their
    // _impl/_apply sibling, which reads them back. Plain 4-byte scalar stores, confirmed by the
    // assembly (see sim_fog_of_war.cpp's header banner on the Ghidra .c draft's misleading
    // two-statement union rendering for G_TMP_SIGHT).
    int32_t &g_tmp_player() { return *g_tmp_player_; }
    int32_t &g_tmp_x() { return *g_tmp_x_; }
    int32_t &g_tmp_y() { return *g_tmp_y_; }
    int32_t &g_tmp_sight() { return *g_tmp_sight_; }

    // G_OTHER_PLAYERS_MASK, MUTABLE. llm_strat_fow_remove_sight_apply writes it as a side effect of
    // building its own AND-mask (the original writes it globally even though nothing but this same
    // function's own logic reads it back locally) -- reproduced so a differential shadow arm matches
    // the original's write, not just its externally-visible effect.
    uint8_t &other_players_mask() { return *other_players_mask_; }

    // ---- SIM1E sim_step (the domain root, 2026-08-16) ------------------------------------------
    //
    // _G_LLM_STRAT_CUR_PLAYER/_CUR_INDEX, MUTABLE scalars. sim_view carries read-only siblings
    // (cur_player/cur_index) for every OTHER function in the migration set, which only ever consumes
    // them ambiently; llm_strat_sim_step is the one function that DRIVES them.
    void set_cur_player(uint16_t player) { *cur_player_mut_ = player; }
    void set_cur_index(uint16_t index) { *cur_index_mut_ = index; }

    // _G_LLM_STRAT_CUR_UNIT/_CUR_BUILDING/_CUR_PROJECTILE/_CUR_FX_ANIM -- writes a NEW POINTER VALUE
    // into the real ambient memory SLOT itself, distinct from cur_unit()/cur_building()/
    // cur_projectile()/cur_fx_anim() above (which dereference that slot ONCE at state()-bind time
    // for CONSUMERS). llm_strat_sim_step is the DRIVER: after selecting each roster record via the
    // ordinary per-index accessors, it must also point the ambient slot at it so the ORIGINAL
    // callees it invokes through mh::call:: (unit_tick/building_tick/projectile_tick/fx_anim_tick,
    // all void(void), all reading their subject ambiently) see the same record.
    void set_cur_unit_ptr(unit *p) { *cur_unit_slot_ = p; }
    void set_cur_building_ptr(building *p) { *cur_building_slot_ = p; }
    void set_cur_projectile_ptr(projectile *p) { *cur_projectile_slot_ = p; }
    void set_cur_fx_anim_ptr(fx_anim *p) { *cur_fx_anim_slot_ = p; }

    // _G_LLM_STRAT_STORAGE_STATS[player], MUTABLE -- sim_view::storage_stats was read-only-only
    // ("no writer in the closure"); llm_strat_sim_step IS that writer (cap_prev[i]=cap_accum[i]
    // latch, cap_accum[i] zero at tick end, subtick_b_clock advance). Same RID as the view member,
    // same one-record-by-reference contract as every other per-player accessor here.
    storage_stats &storage_stats_at(uint32_t player) { return storage_stats_[player]; }

    // ---- SIM1F (2026-08-16) ----------------------------------------------------------
    //
    // _G_LLM_GAME_SPEED_PLAYER_FACTOR[player], MUTABLE -- same RID as the sim_view member above;
    // llm_game_speed_increase/_decrease read-modify-write it.
    double &game_speed_player_factor_at(uint32_t player) { return game_speed_player_factor_[player]; }

    // _G_LLM_STRAT_RNG_STATE[channel], MUTABLE -- the strategic-sim deterministic PRNG.
    // llm_strat_rng_next is the sole reader+writer in this closure, so there is no read-only sim_view
    // sibling (unlike storage_stats_at above).
    uint32_t &rng_state_at(uint32_t channel) { return rng_state_[channel]; }

    // is_human, MUTABLE -- sim_view::is_human is read-only-only (bound by the fog/sight family);
    // llm_game_player_set_human/_set_ai ARE the writer, same RID, same one-record-by-reference
    // contract as storage_stats_at() above.
    uint32_t &is_human_mut() { return *is_human_mut_; }

    // _G_LLM_GAME_HUMAN_PLAYER_MASK, MUTABLE -- the narrower byte mask llm_game_player_set_human/
    // _set_ai actually read-modify-write (is_human above is a uint32 mirror written from it
    // immediately after). No sim_view sibling -- read and write both happen inside the same two
    // functions, same posture as rng_state_at() above.
    uint8_t &game_human_player_mask() { return *game_human_player_mask_; }

    // Players[player].relation[other], MUTABLE, byte[8][8] flattened (RID_PLAYERS, base =
    // Players[0]; the relation sub-array sits at +0x8 in llm_strat_player_desc's documented 0x34-byte
    // layout, docs/structs.md). llm_diplomacy_set_relation is this closure's only writer and never
    // reads the old value, so a raw byte accessor is enough -- no need to commit the whole
    // player_desc struct for one field.
    uint8_t &player_relation_at(uint32_t player_a, uint32_t player_b) {
        return players_raw_[player_a * 0x34 + 0x8 + player_b];
    }

    // SIM1F (2026-08-16). progress[player][row], MUTABLE (RID_PROGRESS -- same region
    // sim_view::progress binds read-only above). game_HandleProgress is this closure's writer
    // (`.available = true`); one-record-by-reference, same contract as unit_at().
    player_progress &progress_at(uint32_t player, int32_t row) {
        return progress_[player * PROGRESS_ROW_COUNT + row];
    }

    // game_speed, MUTABLE (RID_GAME_SPEED -- was MF_VIEW-claimed already but never bound).
    // llm_game_speed_recompute is this closure's sole writer; no sim_view read sibling since the one
    // reader inside this closure is the writer itself (self-read of the value it just assigned).
    double &game_speed() { return *game_speed_; }

    // planet_time[planet_id], MUTABLE (RID_PLANET_TIME, double[32] -- was MF_VIEW-claimed already
    // but never bound). game_AddPlanetToAvailable's sole writer in this closure (sets the "new planet
    // available" toast-suppression deadline to now+delay).
    double &planet_time_at(int32_t planet_id) { return planet_time_[planet_id]; }

    // _G_LLM_STRAT_INVASION_TIME[planet_id], MUTABLE (RID_STRAT_INVASION_TIME, double[32] -- new
    // this slice, no prior binding). game_HandleInvasion's non-current-planet arm is the sole
    // writer/reader in this closure (llm_strat_invasion_due_check, the consumer, is not in this
    // migration set).
    double &planet_invasion_time_at(int32_t planet_id) { return planet_invasion_time_[planet_id]; }

    // SIM1F (2026-08-17). _G_LLM_STRAT_INVASION_ALERT_TIME[planet_id], MUTABLE
    // (RID_STRAT_INVASION_ALERT_TIME, double[32] -- new this slice, no prior binding). DISTINCT from
    // planet_invasion_time_at() above: that one is game_HandleInvasion's "when was this planet
    // invaded" timestamp (RID_STRAT_INVASION_TIME); this one is the recurring on-screen alert timer
    // llm_strat_invasion_alert_arm sets and llm_strat_invasion_alert_poll (not in this migration set)
    // polls each tick to re-announce "Invasion on <planet>". llm_strat_invasion_alert_arm is this
    // closure's sole writer/reader.
    double &invasion_alert_time_at(int32_t planet_id) { return invasion_alert_time_[planet_id]; }

    // SIM1F (2026-08-17). _G_LLM_STRAT_LANDING_SPOTS[16], MUTABLE (RID_STRAT_LANDING_SPOTS,
    // llm_strat_landing_spot[16] -- {x, y, status}, new this slice). llm_strat_claim_landing_spot is
    // this closure's sole writer (claims a spot by setting .status = -2) and reader (scans .status/.x/
    // .y to find a free or paired spot).
    landing_spot &landing_spot_at(int32_t index) { return landing_spots_[index]; }

    // G_PLANET_STATUS[planet_id], MUTABLE (RID_G_PLANET_STATUS -- second independent binding of the
    // SAME region sim_view::planet_status binds read-only above). game_HandleInvasion is the FIRST
    // writer in the closure (sets PLANET_STATUS_INVASION on both its own-planet and distant-planet
    // arms) -- every other function only reads this region.
    int32_t &planet_status_at(int32_t planet_id) { return planet_status_mut_[planet_id]; }

    // _G_LLM_MAP_REGION_GRID[x][y], MUTABLE (RID_MAP_REGION_GRID, llm_map_region_cell[256][256],
    // column-major -- x is the outer index, matching `_G_LLM_MAP_REGION_GRID[x][y]` in every one of
    // this closure's original sites). No sim_view read-only sibling: every reader in this closure is
    // also a writer of the same cell. One-record-by-reference, same contract as unit_at().
    llm_map_region_cell &region_cell_at(int32_t x, int32_t y) { return region_grid_[x * MAP_GRID_DIM + y]; }

    // SIM1-H (2026-09-10). Building[building_id], MUTABLE (RID_BUILDING -- the write half of the
    // SAME region sim_view::cfg_buildings binds read-only, not a second independent binding).
    //
    // WHY A CONFIG TABLE HAS A WRITER AT ALL, since sim_view::cfg_buildings' own comment says it has
    // none: `llm_strat_bldg_init_defaults` (0x0045a068) is the boot-time defaults seeder that runs
    // AFTER the .cfg parse and BEFORE any tick, filling the per-type fields the config file does not
    // carry -- state_transition_ids, the door/park/dock/shuttle-pad offsets, pip_slot_count and the
    // sprite anchors it derives from the loaded sprite headers. It is the ONLY writer of this region
    // in the whole 538-row sim set (check_state_bindings names it and SwitchToPlanet, and
    // SwitchToPlanet's two Building[] stores sit in a block that is DEAD BY CONSTRUCTION -- see
    // sim_switch_to_planet.h). Whole-record by reference, same contract as unit_at().
    cfg_building &cfg_building_at(int32_t building_id) { return cfg_buildings_mut_[building_id]; }

    // SIM1-H (2026-09-10). _G_LLM_MAP_REGION_COORD_WRAP_MASK, MUTABLE (write half of
    // sim_view::region_coord_wrap_mask). SIM1-G-PREP recorded the region as "READ-ONLY in the whole
    // sim closure -- no writer found ... so it is initialised elsewhere (map-size setup)". This is
    // that elsewhere: `llm_map_build_regions` (0x00423335) recomputes it as
    // `width_m * 0x100 + height_m` at the top of every nav-region rebuild, and batch H is what
    // brought the writer inside the domain. The prediction was right and is now closed.
    uint32_t &region_coord_wrap_mask_mut() { return *region_coord_wrap_mask_mut_; }

    // _G_LLM_MAP_BFS_QUEUE_REGIONSPLIT, MUTABLE, an ADDRESS ESCAPE (RID_MAP_BFS_QUEUE_REGIONSPLIT,
    // llm_map_bfs_entry[2048]) -- same escape posture as `text_scratch()`/`available_buildings_row()`
    // above. llm_map_region_split's own private flood-fill scratch; nothing else in this closure
    // reads or writes it, and it carries no state across calls (fully overwritten from index 0 every
    // time before being read back).
    llm_map_bfs_entry *region_bfs_queue() { return region_bfs_queue_; }

    // ---- SIM1-H wave 2 (2026-09-10): the nav-region pipeline's own write set ---------------------
    // Three are the WRITE HALVES of regions sim_view already binds read-only (same RID, not a second
    // binding); `path_` and the delta wrap are new both ways.
    //
    // _G_LLM_MAP_REGION_MERGE_THRESHOLD, MUTABLE. llm_map_compute_region_merge_threshold derives it
    // once per rebuild and llm_map_merge_small_regions -- an ALREADY-VERIFIED row that reads
    // sim_view::region_merge_threshold -- then applies it. Producer and consumer on the same RID.
    uint32_t &region_merge_threshold_mut() { return *region_merge_threshold_mut_; }

    // _G_LLM_MAP_REGION_ROUTE_STEP_DELTAS, MUTABLE (byte[32], 8 dirs x 4-byte stride, +0=row/y,
    // +1=col/x). llm_map_init_region_route_step_deltas is the sole writer; route_search reads it.
    uint8_t *region_route_step_deltas_mut() { return region_route_step_deltas_mut_; }

    // _G_LLM_MAP_REGION_ROUTE_STEP_DELTA_WRAP (byte[4]) -- the NINTH SLOT, immediately past the
    // 32-byte array above (0x00708af4 + 32 == 0x00708b14, exactly). The same writer's last act copies
    // direction entry 0 into it (0x00423321 `MOV EAX,[0x00708af4]` / 0x00423326
    // `MOV [0x00708b14],EAX`), so llm_map_region_route_search can read [i] and [i+1] for any i in
    // 0..7 without a bounds check. A WRAP SENTINEL, not a ninth direction -- do not fold it into the
    // array by widening that to 36: every existing consumer indexes the 32.
    uint8_t *region_route_step_delta_wrap() { return region_route_step_delta_wrap_; }

    // `path_` (llm_map_bfs_entry[2048]), MUTABLE, an ADDRESS ESCAPE -- same posture as
    // region_bfs_queue() above, and deliberately a SECOND buffer rather than a share: this is
    // llm_map_region_flood_fill's queue, region_bfs_queue_ is llm_map_region_split's, and the two
    // flood fills are independent. Entry is {x at +0, y at +1, depth at +2 (word)}. The bare name is
    // Ghidra's: the symbol is `map::g::path_`, one of the 13 hand-namespaced LABELS the 2026-08-05
    // `::` flattening missed, and getName() returns the short form.
    llm_map_bfs_entry *region_flood_path_queue() { return region_flood_path_queue_; }

    // _G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING, MUTABLE (RID_STRAT_FOREIGN_BLDG_EVENT_PENDING). No
    // sim_view read-only sibling -- map_CreateBuilding is the only reader-or-writer in this closure
    // (unconditionally clears it, never reads it back).
    int32_t &foreign_bldg_event_pending() { return *foreign_bldg_event_pending_; }

    // player_resources[player][resource_id], MUTABLE (RID_PLAYER_RESOURCES -- second independent
    // binding of the SAME region sim_view::player_resources binds read-only above). The FIRST direct
    // writer in the closure: `llm_resource_add`/`game_SpendResource` (SIM1F, 2026-08-16)
    // -- every earlier caller (decay_excess_resources, econ_track_unit_resource_spend, ...) reaches
    // this array by CALLING one of these two, never by writing it directly.
    int32_t &player_resource_at(uint32_t player, int32_t resource_id) {
        return player_resources_mut_[player * PLAYER_RESOURCE_SLOTS + resource_id];
    }

    // AvailableBuildings[player]'s row base, MUTABLE, an ADDRESS ESCAPE (RID_AVAILABLEBUILDINGS,
    // int32_t[8][50]). game_AddToAvailableBuildings never dereferences these bytes itself -- it hands
    // the row base to the untranslated original game_InsertItemInPlayerArray, which performs the
    // actual write. Same escape posture as text_scratch() above.
    int32_t *available_buildings_row(uint32_t player) {
        return available_buildings_ + player * AVAILABLE_BUILDINGS_ROW_INTS;
    }

    // AvailableProjects[player][type]'s bucket base, MUTABLE, an ADDRESS ESCAPE (RID_AVAILABLEPROJECTS,
    // int32_t[8][2][50]). Same escape posture as available_buildings_row() above -- game_AddProjectToAvailable
    // hands the bucket base to game_InsertItemInPlayerArray.
    int32_t *available_projects_bucket(uint32_t player, int32_t type) {
        return available_projects_ + (player * AVAILABLE_PROJECTS_TYPES + type) * AVAILABLE_PROJECTS_BUCKET_INTS;
    }

    // LT1B (2026-09-02). The WHOLE AvailableProjects region (RID_AVAILABLEPROJECTS,
    // int32_t[8][2][50], 3200 B). game_ClearAvailableProjects clears it wholesale -- the same
    // whole-region posture as fog_of_war_base() below, added so its translation does not have to
    // iterate 16 bucket accessors to express one memset-shaped loop.
    int32_t *available_projects_base() { return available_projects_; }

    // SIM1F (2026-08-16). Unit[proto_id]/Weapon[weapon_id], MUTABLE, SECOND independent
    // bindings of the SAME regions sim_view::cfg_units/cfg_weapons bind read-only above (RID_UNIT/
    // RID_WEAPON) -- the first writer to either cfg table in the whole closure. game_HandleUpgrade
    // is the sole writer: one-record-by-reference, same contract as unit_at(), so the caller indexes
    // the per-player array fields (`.step_speed[player]`, `.range_min[player]`, ...) itself.
    cfg_unit   &unit_mut(uint32_t proto_id) { return cfg_units_mut_[proto_id]; }
    cfg_weapon &weapon_mut(uint32_t weapon_id) { return cfg_weapons_mut_[weapon_id]; }

    // SIM1F (2026-08-16). map::g::width/height, MUTABLE, SECOND independent bindings of
    // the SAME regions sim_view::map_width/map_height bind read-only above (RID_WIDTH/RID_HEIGHT).
    // llm_strat_planet_distance is the sole writer: it temporarily forces both to 100000 (so the
    // wrapped tile-delta helper it calls does NOT toroidally wrap -- planets sit on a non-wrapping
    // starfield, not the tile map) and restores the saved originals before returning. No other
    // function in the closure touches either.
    int32_t &map_width_mut() { return *map_width_mut_; }
    int32_t &map_height_mut() { return *map_height_mut_; }

    // SIM1-G2 (2026-08-20). llm_strat_pathfind_route_leg_group_and_sort's per-call scratch: the
    // caller's (ref_x, ref_y) and the map's half-extents, stashed at function entry so the
    // frontier callees llm_strat_slot_dist_to_ref / llm_strat_claim_free_slots_within_dist can read
    // them without an explicit parameter. See tools/data/ghidra_findings.json 2026-08-20-1920-1.
    int32_t &group_move_dist_ref_x_mut() { return *group_move_dist_ref_x_; }
    int32_t &group_move_dist_ref_y_mut() { return *group_move_dist_ref_y_; }
    int32_t &group_move_dist_half_width_mut() { return *group_move_dist_half_width_; }
    int32_t &group_move_dist_half_height_mut() { return *group_move_dist_half_height_; }

    // SIM1-G2 (2026-08-20). llm_strat_unit_state_move_path caches llm_strat_unit_chase_check's
    // return value here (RID_STRAT_UNIT_CHASE_RESULT), gated on the ticking unit's order being
    // 0x1a/0x1b/0x1c. This is the same call that makes move_path's shadow write-closure reach the
    // SIM-CUT effectful UI/snd/net set -- move_path itself ships arm_ready:false permanently (see
    // this item's tracker progress, first-slice safety finding).
    int32_t &unit_chase_result_mut() { return *unit_chase_result_; }

    // SIM1-G3 (2026-08-21). One squad-formation-anchor scratch record, MUTABLE, same
    // RID as sim_view::squad_anchor_scratch (llm_strat_squad_pick_free_formation_anchor's read-only
    // binding). llm_strat_unit_state_squad_merge is this region's only writer in the whole closure.
    squad_formation_anchor_scratch &squad_anchor_scratch_at(int32_t index) {
        return squad_anchor_scratch_mut_[index];
    }

    // SIM1-G4 (2026-08-22; llm_strat_bldg_completion_dispatch). map::resources[64][64],
    // MUTABLE, SECOND independent binding of the SAME region sim_view::resources binds read-only
    // above (RID_RESOURCES) -- the FIRST direct writer of this region in either the sim or AI
    // closure (MINE_EXTRACTING's depletion write).
    map_resources &resources_at(int32_t x, int32_t y) { return resources_mut_[x * 64 + y]; }

    // _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG, MUTABLE, SECOND independent binding of the SAME
    // region sim_view::foreign_bldg_change_flag binds read-only above. completion_dispatch's
    // other-player branch is this region's only writer in the whole sim closure (the AI cluster's
    // own read-only binding predates this).
    int32_t &foreign_bldg_change_flag_mut() { return *foreign_bldg_change_flag_mut_; }

    // llm_strat_ui_base_marker_coord[8], MUTABLE (RID_STRAT_UI_BASE_MARKER_COORDS). No sim_view
    // read-only sibling -- completion_dispatch's MOTHER-arrival branch is the only reader-or-writer
    // in this closure (writes index [0] unconditionally on the local-player arm).
    ui_base_marker_coord &ui_base_marker_coords_at(int32_t index) {
        return ui_base_marker_coords_mut_[index];
    }

    // _G_LLM_STRAT_DMP_PATH_SCRATCH, MUTABLE, an ADDRESS ESCAPE (RID_STRAT_DMP_PATH_SCRATCH, char[32])
    // -- same escape posture as text_scratch() above. completion_dispatch's own utils_sprintf writes
    // the filename here, then hands the pointer to the untranslated original
    // llm_strat_load_base_layout_dmp, which does the actual file read.
    char *dmp_path_scratch() { return dmp_path_scratch_; }

    // ================= SIM-RESID-IF (2026-08-31): the residual-writer bindings =================
    // The 67 regions the sim_resid translate set writes that this header did not bind. DERIVED,
    // never listed: fold the ledger's own writes_shared+writes_island against the RID set in
    // sim_state.cpp. The count read 26, then 18, then 15, then 62 before it was measured that way
    // -- each earlier number answered an adjacent question, and the 15 in particular came from a
    // field that lists only regions libmh ALSO writes, i.e. "where will there be two writers", not
    // "what must a body be able to write".
    //
    // Grouped by WRITER, not alphabetically, because the question a later reader has is who writes
    // this and why.

    // ---- session reset -- the clocks and the per-session counters ----
    // llm_strat_session_state_reset is this closure's broad per-session sweep and writes all of these;
    // llm_strat_time_resync_and_tick rewrites last_game_time on every resync and llm_strat_new_game_init
    // seeds save_misc_dword. The three clocks are CONTIGUOUS doubles (0x00e587b1/b9/c1) but three separate
    // registry regions, so they get three bindings rather than one array.
    //
    // CURRENT_GAME_TIME: the strategic clock the sim advances each frame.
    //   Its registry extent used to read 48 B -- not because the symbol is 48 B, but because the
    //   determinism hash claimed one 48-byte slice across all six clock doubles and a manifest
    //   claim raises the extent it is measured against. SB-HOSTFREE H0 split that slice, so the
    //   extent is now the measured 8, which is what this binding always took
    double &current_game_time() { return *current_game_time_; }
    // LAST_GAME_TIME: previous frame's clock value; time_resync_and_tick rewrites it every
    //   resync, the reset zeroes it
    double &last_game_time() { return *last_game_time_; }
    // TOTAL_GAME_TIME: cumulative elapsed session time, zeroed at session entry and never wound
    //   back
    double &total_game_time() { return *total_game_time_; }
    // CHEAT_PENALTY_SCORE: score penalty accumulated by cheat use; zeroed per session
    int32_t &cheat_penalty_score() { return *cheat_penalty_score_; }
    // DEBUG_TAP_FLAG: developer tap/trace enable; zeroed per session
    int32_t &debug_tap_flag() { return *debug_tap_flag_; }
    // NET_BW_STAT: int[2] bandwidth accumulator, indexed 0..1. CROSS-SUBSYSTEM,
    //   region_ownership class A: netcode owns it during play and the sim only ZEROES it at
    //   session entry, never reading it back
    int32_t &net_bw_stat_at(int32_t i) { return net_bw_stat_[i]; }
    // STRAT_FLOATING_MSG_SUPPRESS_FLAG: suppresses the floating-message queue while a session
    //   is torn down or rebuilt
    uint8_t &floating_msg_suppress_flag() { return *floating_msg_suppress_flag_; }
    // STRAT_LOCKSTEP_STEP_MULT: lockstep step multiplier. Class A again: the sim's reset zeroes
    //   a netcode field it never reads
    uint8_t &lockstep_step_mult() { return *lockstep_step_mult_; }
    // STRAT_PLANET_INT_TABLE: int[32], one per planet slot; the reset clears the whole table
    int32_t &planet_int_table_at(int32_t i) { return planet_int_table_[i]; }
    // STRAT_SIM_STEP_INTERVAL: seconds between simulation steps at the current game speed
    double &sim_step_interval() { return *sim_step_interval_; }
    // STRAT_SAVE_MISC_DWORD: the save-file miscellaneous dword; llm_strat_new_game_init seeds
    //   it. The ONLY region in this whole set that is in the `save` manifest rather than `view`
    int32_t &save_misc_dword() { return *save_misc_dword_; }

    // ---- planet landing, transition, and multiplayer session entry ----
    // Written by the boot/landing path -- llm_game_land_players_on_planet, llm_strat_planet_session_begin,
    // llm_strat_planet_transition_finalize, llm_strat_session_begin_multi -- handing a configured world to
    // the sim. region_ownership class B, which is deliberately NOT class A: a real value is written, not a
    // zero, so a boot path that computed one differently WOULD diverge. simtest's session-entry assertion
    // is the check.
    //
    // GAME_LAND_NO_START_UNIT_FLAG: set when the landing must not spawn the default starting
    //   unit
    int32_t &game_land_no_start_unit_flag() { return *game_land_no_start_unit_flag_; }
    // STRAT_OUTER_PLANET_LANDED_FLAG: whether the player has landed on the currently selected
    //   outer planet
    int32_t &outer_planet_landed_flag() { return *outer_planet_landed_flag_; }
    // STRAT_OUTER_PLANET_LAND_STATE: the outer-planet landing state machine's current state
    int32_t &outer_planet_land_state() { return *outer_planet_land_state_; }
    // STRAT_PLANET_TRANSITION_STATE: planet-to-planet transition phase;
    //   planet_transition_finalize clears it on arrival
    uint8_t &planet_transition_state() { return *planet_transition_state_; }
    // STRAT_SHOW_UNIT_FLAGS: per-session unit-overlay flags; three boot entry points write it
    int32_t &show_unit_flags() { return *show_unit_flags_; }
    // STRAT_RNG_SEED_BYTE: the seed byte session_begin_multi stamps so both peers start the
    //   same PRNG
    uint8_t &rng_seed_byte() { return *rng_seed_byte_; }
    // STRAT_LOCKSTEP_ADAPT_NEXT_TIME: next wall-clock time the lockstep rate adaption may run;
    //   seeded at MP session entry
    double &lockstep_adapt_next_time() { return *lockstep_adapt_next_time_; }
    // CHAT_TARGET_MASK: bitmask of players a chat line is addressed to; reset by the tutorial
    //   and by MP session entry
    uint8_t &chat_target_mask() { return *chat_target_mask_; }

    // ---- the planet-map palette entries ----
    // All nine written ONCE by llm_strat_planet_map_session_init when the planet map's session state is
    // built. region_ownership class D: the sim supplies the value at init, but Graphics/FX owns the format
    // and every consumer, so these have zero sim readers. Two families -- PAL4 (0x00e69ec0..c7, four
    // contiguous ushorts) and PAL5 (0x00fe5b60..69, five) -- kept as nine regions because that is how the
    // registry and the original's nine separate stores address them.
    //
    // STRAT_PLANET_MAP_PAL4_WHITE: 565/555 packed white
    uint16_t &planet_map_pal4_white() { return *planet_map_pal4_white_; }
    // STRAT_PLANET_MAP_PAL4_BLACK: 565/555 packed black
    uint16_t &planet_map_pal4_black() { return *planet_map_pal4_black_; }
    // STRAT_PLANET_MAP_PAL4_MAGENTA: 565/555 packed magenta
    uint16_t &planet_map_pal4_magenta() { return *planet_map_pal4_magenta_; }
    // STRAT_PLANET_MAP_PAL4_YELLOW: 565/555 packed yellow
    uint16_t &planet_map_pal4_yellow() { return *planet_map_pal4_yellow_; }
    // STRAT_PLANET_MAP_PAL5_BLACK: 565/555 packed black
    uint16_t &planet_map_pal5_black() { return *planet_map_pal5_black_; }
    // STRAT_PLANET_MAP_PAL5_MAGENTA: 565/555 packed magenta
    uint16_t &planet_map_pal5_magenta() { return *planet_map_pal5_magenta_; }
    // STRAT_PLANET_MAP_PAL5_GREEN: 565/555 packed blue
    uint16_t &planet_map_pal5_green() { return *planet_map_pal5_green_; }
    // STRAT_PLANET_MAP_PAL5_RED: 565/555 packed red
    uint16_t &planet_map_pal5_red() { return *planet_map_pal5_red_; }
    // STRAT_PLANET_MAP_PAL5_BLUE: 565/555 packed green
    uint16_t &planet_map_pal5_blue() { return *planet_map_pal5_blue_; }

    // ---- the squad-status blackboard ----
    // llm_strat_bldg_gather_nearby_squad_status writes all seven. SQUAD_STATUS is a SECOND REGISTRY
    // BINDING: libmh/tact/tact_state.cpp:221 already binds RID_SQUAD_STATUS mutably as
    // llm_squad_status_slot[64]. That is the shape sim_state.h:1591 blesses -- same region, same registry,
    // two legitimate writers -- and not the hazard it names, which is a binding that does NOT come from
    // the registry. The five SQUAD_BB_* scalars are the scan's input/output blackboard, laid out around
    // the slot array at 0x00e15e48..5c.
    //
    // SQUAD_BB_SCAN_PLAYER: the player whose squads the current scan is gathering
    int32_t &squad_bb_scan_player() { return *squad_bb_scan_player_; }
    // SQUAD_BB_TARGET_OWNER: owner of the building the scan is centred on
    int32_t &squad_bb_target_owner() { return *squad_bb_target_owner_; }
    // SQUAD_BB_TARGET_BUILDING_ID: that building's id
    int32_t &squad_bb_target_building_id() { return *squad_bb_target_building_id_; }
    // SQUAD_BB_TARGET_ENERGY_PCT: the target's ENERGY as a percentage -- the HP-like stat, NOT
    //   generated power
    int32_t &squad_bb_target_energy_pct() { return *squad_bb_target_energy_pct_; }
    // SQUAD_BB_TARGET_BUILDING_IDX: that building's roster index
    int32_t &squad_bb_target_building_idx() { return *squad_bb_target_building_idx_; }
    // SQUAD_STATUS: llm_squad_status_slot[64] (1024 B / 0x10). Second binding; tact_state.cpp
    //   holds the first
    squad_status_slot &squad_status_at(int32_t i) { return squad_status_[i]; }
    // SQUAD_STATUS_COUNT: how many of the 64 slots the last gather filled
    int32_t &squad_status_count() { return *squad_status_count_; }

    // ---- the order queue's pending/staging counters ----
    // map_FillDefaults is the ONLY code that clears PENDING and STAGING -- recorded independently at
    // libmh/orders/order_queue.h:241, which also binds both mutably. Second registry binding, and deliberate:
    // the rejected alternative was for FillDefaults to call a reset primitive in the orders module, which
    // would replace three literal zero stores with a call. Law 2 says reproduce, not improve, and it would
    // additionally cost the offline fixture a binding to orders state. Both are `hash`-manifest regions,
    // so a wrong value desyncs rather than merely looking wrong.
    //
    // STRAT_ORDER_PENDING_COUNT: pending-order count
    int32_t &order_pending_count() { return *order_pending_count_; }
    // STRAT_ORDER_STAGING_COUNT: staging-order count
    int32_t &order_staging_count() { return *order_staging_count_; }

    // ---- the per-peer lockstep horizons ----
    // llm_strat_player_param_defaults_init seeds both to constants (10.0 and -1.0). SECOND registry
    // binding for NET_PEER_HORIZON -- libmh/lockstep/turn_engine.cpp:39 binds it as double[8] already -- for
    // the same reason as the order counters: the write is a literal store in a defaults initialiser, and
    // routing it through turn_engine would both change the code and drag turn_engine state into the
    // offline fixture. Both are hash-manifest, i.e. determinism-carrying.
    //
    // NET_PEER_HORIZON: double[8], one per player slot. Second binding; turn_engine.cpp holds
    //   the first
    double &net_peer_horizon_at(int32_t i) { return net_peer_horizon_[i]; }
    // NET_PEER_HORIZON_PENDING: double[8], the not-yet-acknowledged twin
    double &net_peer_horizon_pending_at(int32_t i) { return net_peer_horizon_pending_[i]; }

    // ---- building placement ----
    // llm_strat_bldg_try_begin_placement and llm_strat_bldg_set_footprint_passable.
    //
    // BUILD_PLACEMENT_ID: the building type id the player is currently placing; the session
    //   reset also clears it
    int32_t &build_placement_id() { return *build_placement_id_; }
    // BLDG_FOOTPRINT_PASSABLE_SAVE_SLOT: the slot the footprint's pre-stamp passability was
    //   saved into, so the stamp can be undone
    uint32_t &bldg_footprint_passable_save_slot() { return *bldg_footprint_passable_save_slot_; }

    // ---- the advisor tick ----
    // llm_strat_advisor_tick. floating_msg_queue_active is also written by
    // llm_strat_bldg_try_begin_placement.
    //
    // STRAT_ADVISOR_PHASE: which advisor message phase is currently showing
    int32_t &advisor_phase() { return *advisor_phase_; }
    // STRAT_FLOATING_MSG_QUEUE_ACTIVE: non-zero while a floating message is on screen
    int32_t &floating_msg_queue_active() { return *floating_msg_queue_active_; }

    // ---- the pathfinder's async job-result table ----
    // llm_strat_pathfinder_init clears it at session entry; llm_strat_unit_assign_path_from_job_result
    // (batch D) is the consumer. Its struct type was added to dll_addr_manifest.json's `structs` list by
    // this item so the binding could be TYPED: W2 wants a typed accessor returning ONE record, and a
    // uint8_t* would have handed out an address to do arithmetic on -- the exact thing W2 forbids.
    //
    // STRAT_PATH_JOB_RESULT_TABLE: llm_strat_job_result_entry[100] (800 B / 8)
    job_result_entry &path_job_result_at(int32_t i) { return path_job_result_[i]; }

    // ---- the mode flag the tactical hand-off writes ----
    // llm_strat_try_enter_tactical_mission. One byte, and the axis the whole architecture turns on.
    //
    // GAME_MODE: strategic vs tactical mode selector
    uint8_t &game_mode() { return *game_mode_; }

    // ---- the tutorial path's own state, and the shared UI chrome it restamps ----
    // llm_game_start_tutorial and llm_tutorial_step_driver write every region below. The UI half is bound
    // in the STORE, not the read view, and that is NOT a weakening of W1. W1 is a claim about the READ
    // VIEW's membership -- a region reachable only through sim_view is one the sim demonstrably never
    // writes -- and every region here is demonstrably written by a translate-set member. The precedent is
    // already in this header: SIM1F binds game_SetEvent's UI-panel and chat-input state
    // exactly this way. region_ownership classes E and F carry the cross-write adjudication (the menu path
    // clears tutorial state on the way out; a screen entry point restamps shared chrome).
    //
    // THE THREE WIDGETS ARE NOT TUTORIAL-OWNED, and their accessor names deliberately do not say tutorial:
    // _G_LLM_UI_WGT_MENU_SCREEN_TITLE is children[0] of ~14 menu/lobby/browser lists with 16 writers, and
    // the two outcome-dialog widgets belong to the mission-outcome modal that the tutorial-done dialog
    // reuses. The tutorial is a CLIENT of all three. Each is bound as the whole 68-byte llm_ui_widget, the
    // way _G_LLM_UI_WGT_TUTORIAL_WELCOME was, not as the 4-byte .label field the writers actually touch --
    // the region is the widget.
    //
    // TUTORIAL_BUILD_TYPE_FILTER: restricts which building types the tutorial lets the player
    //   place
    int32_t &tutorial_build_type_filter() { return *tutorial_build_type_filter_; }
    // TUTORIAL_FORCED_BLDG_SELECTION: building the tutorial forces selected for the current
    //   step
    uint32_t &tutorial_forced_bldg_selection() { return *tutorial_forced_bldg_selection_; }
    // TUTORIAL_HQ_ATTACK_SCENARIO_DONE: one-byte bool: the HQ-attack scenario has been
    //   completed
    uint8_t &tutorial_hq_attack_scenario_done() { return *tutorial_hq_attack_scenario_done_; }
    // TUTORIAL_PENDING_BUILD_PLACEMENT_ID: placement the tutorial has queued for the player
    int32_t &tutorial_pending_build_placement_id() { return *tutorial_pending_build_placement_id_; }
    // TUTORIAL_RMB_LIMIT_FLAG: limits right-mouse actions during a guided step
    int32_t &tutorial_rmb_limit_flag() { return *tutorial_rmb_limit_flag_; }
    // TUTORIAL_RESET_SLOT_0050A678: A PROVENANCE NAME, NOT A SEMANTIC ONE -- the address is in
    //   the name for the same reason CRT_<addr> keeps one. All four of its referrers are links
    //   in one Watcom chained store (`BUILD_TYPE_FILTER = X678 = PENDING_BUILD_PLACEMENT_ID =
    //   RMB_LIMIT_FLAG = 0`), both sites only ever store 0, and nothing consumes the value. It
    //   may be a dead scalar or it may be _G_LLM_STRAT_UI_PANEL_FALLBACK_TABLE[4] -- its two
    //   consumers walk that int[4] UNBOUNDED to the first zero, so a NUL-terminated list
    //   structurally needs a zero at [4]. BINDING IS CORRECT UNDER BOTH: a 0 store to a fixed
    //   address is the same write either way and Law 1 freezes the layout. The manifest note
    //   carries the experiment that would settle it
    int32_t &tutorial_reset_slot_0050a678() { return *tutorial_reset_slot_0050a678_; }
    // DLG_STATE_FLAGS: the modal-dialog state bitfield. Ghidra types it llm_dlg_state_flags32,
    //   a 4-byte typedef rather than a record, so it is bound as its int32_t storage
    int32_t &dlg_state_flags() { return *dlg_state_flags_; }
    // PLAYER_CONTROL_MASK: which players the local machine controls; the tutorial narrows it,
    //   the session reset restores it
    uint8_t &player_control_mask() { return *player_control_mask_; }
    // GFX_UI_COLORS_RGB: int[4][3] flattened to [row*3 + channel]: four UI colours, three
    //   channels each. Class F
    int32_t &gfx_ui_color_at(int32_t i) { return gfx_ui_color_[i]; }
    // UI_RACE_SEL_PENDING_GFX_IDX: deferred menu-GFX sprite index for the race-select overlay:
    //   the Human/Alien buttons' shared draw_cb stores an index here instead of drawing, and a
    //   third widget flushes it. 0 = nothing pending. llm_game_start_tutorial clears it
    int32_t &ui_race_sel_pending_gfx_idx() { return *ui_race_sel_pending_gfx_idx_; }
    // UI_FADE_TRANSITION: the screen-fade state machine (32 B); its struct type was added to
    //   the manifest by this item
    ui_fade_transition_state &ui_fade_transition() { return *ui_fade_transition_; }
    // UI_MENU_ASYNC_CALLBACK_A: a FUNCTION-POINTER slot (Ghidra `eax_eax_func *`), bound as
    //   void*& -- the store only installs and clears it, never calls through it
    void *&ui_menu_async_callback_a() { return *ui_menu_async_callback_a_; }
    // UI_MENU_WIDGET_LIST: pointer to the active menu's widget list; the tutorial swaps it
    widget_list *&ui_menu_widget_list() { return *ui_menu_widget_list_; }
    // UI_TUTORIAL_HINT_WIDGET: llm_ui_widget (68 B), the tutorial's hint box -- one of the two
    //   here that IS tutorial-owned
    ui_widget &ui_tutorial_hint_widget() { return *ui_tutorial_hint_widget_; }
    // UI_WGT_TUTORIAL_WELCOME: llm_ui_widget (68 B), the welcome panel. NAMED by SIM-RESID-
    //   PREP, and naming it is what made it visible to R8 -- which is why this item's blocker
    //   count rose before it fell
    ui_widget &ui_wgt_tutorial_welcome() { return *ui_wgt_tutorial_welcome_; }
    // UI_WGT_MENU_SCREEN_TITLE: llm_ui_widget (68 B). The SHARED menu-screen title: children[0]
    //   of ~14 menu/lobby/browser widget lists, .label stamped by each screen-open function. 16
    //   writers, of which the tutorial is one
    ui_widget &ui_wgt_menu_screen_title() { return *ui_wgt_menu_screen_title_; }
    // UI_OUTCOME_DLG_TITLE_WIDGET: llm_ui_widget (68 B). Title line of the mission-outcome
    //   modal, children[0] of both the outcome dialog and the tutorial-done dialog that reuses
    //   it. llm_ui_outcome_dialog sets it ONCE at entry, before the 12-way outcome switch --
    //   which is what makes it a title and not a message
    ui_widget &ui_outcome_dlg_title_widget() { return *ui_outcome_dlg_title_widget_; }
    // UI_OUTCOME_DLG_MESSAGE_WIDGET: llm_ui_widget (68 B). The per-outcome result line of that
    //   modal: the dialog's switch writes one text id per arm into this single .label across
    //   ten sites
    ui_widget &ui_outcome_dlg_message_widget() { return *ui_outcome_dlg_message_widget_; }
    // UI_WGT_FRAME_MENU_PANEL: llm_ui_widget (68 B). The SHARED frame/backdrop widget -- the
    //   `frame` member (list+8) of SEVEN widget lists, and the panel
    //   llm_ui_menu_screen_slide_transition slides in. Its disp_idx (41) is a SPRITE index, not
    //   a menu-GFX index: llm_ui_widget_list_center's flag-0x0200 branch takes the sprite's
    //   width/height as the list's centred extent. The tutorial's write reaches it as
    //   `list->frame`, not by absolute address, and is idempotent. It was auto-named
    //   llm_ui_widget_006516d3 until SIM-RESID-READY named it on 2026-08-31 -- which RENAMED
    //   ITS REGION, because the registry is re-merged from the manifests; a binding written
    //   against the old RID stops compiling, which is the good failure mode
    ui_widget &ui_wgt_frame_menu_panel() { return *ui_wgt_frame_menu_panel_; }
    // STRAT_INJECTED_MAP_PLANET_SLOT: one byte, stamped 0x1f by the two map-injection preambles
    //   (llm_game_start_tutorial and llm_lobby_finalize_custom_map_and_sync) that inject
    //   current_map_data into the reserved planet slot 0x1f. THE 68th BINDING, and it exists
    //   only because SIM-RESID-READY defined the byte: it was UNDEFINED data, and
    //   the state-matrix generator indexes regions with listing.getDefinedData(), so BOTH writes were
    //   dropped from the matrix and from every write set derived from it. An undefined byte is
    //   invisible to the whole write-attribution chain.
    uint8_t &injected_map_planet_slot() { return *injected_map_planet_slot_; }
    // VIEW_SIZE_MODE: strategic view size/zoom mode
    int32_t &view_size_mode() { return *view_size_mode_; }
    // VIEW_SIZE_MODE_SAVE: the saved view mode the tutorial restores on exit. Class F
    int32_t &view_size_mode_save() { return *view_size_mode_save_; }
    // CURRENT_MAP_DATA: cfg_struct_map_header (380 B), the loaded map's header.
    //   llm_game_start_tutorial writes the hardcoded tutorial map path into it. Struct type
    //   added to the manifest by this item
    map_header &current_map_data() { return *current_map_data_; }

    // ================= SIM-RESID-IF re-close (2026-08-31): THE WRITE HALVES =====================
    //
    // WHY THIS BLOCK EXISTS AT ALL, since SIM-RESID-IF closed once already. Its acceptance test
    // folded the domain's write set against the RID set MENTIONED in sim_state.cpp and returned
    // zero unbound. That fold cannot tell `v.X = ptr<const T>(RID)` from `ptr<T>(RID)` in the store
    // constructor -- so a region bound read-only satisfied it while a translation still could not
    // write it. 24 regions were in that state, and eight complete, linted translations were
    // unbuildable because each named a `sim_store` accessor that does not exist. The gap was in the
    // TEST, not in anyone's work; tools/check_state_bindings.py is the write-half-aware replacement
    // and reports write-bound / READ-ONLY / unbound as three separate answers.
    //
    // `_mut` SUFFIX, and it is not decoration: every region below already has a `sim_view` member
    // of the same name, so the suffix is what stops a reader mistaking a store write for a view
    // read at the call site. The two resolve the SAME RID -- one copy of the state, which is what
    // statetest's rebase consumer checks -- exactly like map_width_mut/cur_player_mut above. The
    // regions with NO view sibling (advisor_next_time, mouse_buttons_prev, the two address
    // escapes) keep bare names, matching advisor_phase() next to them.

    // ---- the session/planet-entry spine (batch B) ----
    // CURRENTSYSTEM / G_PLANET_INDEX: which system and planet the session is in.
    // llm_strat_session_state_reset walks them; llm_strat_session_begin_multi pokes the MP
    // "virtual home planet" (system 0, planet 0x1f) into them literally.
    int32_t &current_system_mut() { return *current_system_mut_; }
    int32_t &planet_index_mut() { return *planet_index_mut_; }
    // STRAT_GAME_CLOCK / GAME_TIME_DELTA: the strategic clock and its per-frame delta, both zeroed
    // at session reset. Distinct from current_game_time()/last_game_time() above, which are the
    // save-facing clock triple.
    double &game_clock_mut() { return *game_clock_mut_; }
    double &game_time_delta_mut() { return *game_time_delta_mut_; }
    // PLAYERSIDE: the local player's slot index as the SIM sees it. MP session entry copies
    // sim_view::net_local_player_slot (a different region) into this one.
    int16_t &player_side_mut() { return *player_side_mut_; }
    // STRAT_LOCAL_PLAYER_SLOT / STRAT_PLAYER_RACE: the local slot and race the session runs as.
    uint16_t &local_player_slot_mut() { return *local_player_slot_mut_; }
    int32_t  &player_race_mut() { return *player_race_mut_; }
    // STRAT_SIM_ACTIVE: the simulation's own run gate. Both session-entry paths clear it, do the
    // setup that must not tick, then set it -- so it is written twice per entry, on purpose.
    int32_t &sim_active_mut() { return *sim_active_mut_; }
    // STRAT_MP_ALLY_VICTORY_RULE_FLAG: cleared at MP and single-planet session entry.
    int32_t &mp_ally_victory_rule_flag_mut() { return *mp_ally_victory_rule_flag_mut_; }
    // STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG / STRAT_UI_BLDG_TAB_SELECT_BLOCKED /
    // STRAT_BLDG_COMPLETION_SLOT_COUNT: per-system state the reset sweep re-seeds. The slot count
    // is a real .bss byte whose ONLY writer in the binary is that sweep -- see the sim_view
    // sibling, which MINE_EXTRACTING reads as a loop bound.
    int32_t &system_lost_msg_shown_flag_mut() { return *system_lost_msg_shown_flag_mut_; }
    uint8_t &ui_bldg_tab_select_blocked_mut() { return *ui_bldg_tab_select_blocked_mut_; }
    uint8_t &bldg_completion_slot_count_mut() { return *bldg_completion_slot_count_mut_; }

    // ---- the planet-map session extent (batch B) ----
    // WIDTH_M / HEIGHT_M: the planet map's extent, seeded from map_width-1 / map_height-1 by
    // llm_strat_planet_map_session_init -- and, SIM1-H (2026-09-10), by `llm_map_build_regions`
    // (0x0042334d-0x0042335e) too, which recomputes the SAME pair with the SAME formula at the top
    // of every nav-region rebuild. A SECOND writer this comment named for nine days while claiming
    // one; found by the batch-H translator, who also corrected the batch context's guess that these
    // were part of the RID_GENERAL `geom_mut_` family (they are not -- dedicated RID_WIDTH_M /
    // RID_HEIGHT_M globals at 0x00fe5b40 / 0x00fe5b44, confirmed by address).
    uint32_t &width_m_mut() { return *width_m_mut_; }
    uint32_t &height_m_mut() { return *height_m_mut_; }

    // ---- the tutorial (batch E) ----
    // GAME_TUTORIAL_STEP / STRAT_AI_ENABLED: the tutorial's step cursor and its AI gate.
    int32_t &tutorial_step_mut() { return *tutorial_step_mut_; }
    int32_t &ai_enabled_mut() { return *ai_enabled_mut_; }
    // STRAT_UI_PANEL_FALLBACK_TABLE: int table the tutorial driver rewrites per step.
    int32_t &ui_panel_fallback_table_at(int32_t index) { return ui_panel_fallback_table_mut_[index]; }

    // ---- the config tables a new game / tech reset re-seeds (batches B, C) ----
    // ONE RECORD BY REFERENCE, the same W2 contract as unit_at(): the caller names the record it
    // is writing, so no base pointer escapes and no `+ 1` is expressible.
    // Planets: llm_strat_session_begin_multi writes Planets[0x1f].system_index; the tutorial
    // rewrites its own slot.
    cfg_planet &cfg_planet_at(int32_t index) { return cfg_planets_mut_[index]; }
    // Players (0x00e587e9, llm_strat_player_desc[8], stride 0x34), MUTABLE, ONE RECORD BY
    // REFERENCE -- the same W2 contract as cfg_planet_at()/profile_at(). Added SIM-RESID-F for
    // llm_game_start_tutorial, which writes controller_flags / race_or_faction / color_or_team /
    // scenario_side_id / name for Players[0..1] and controller_flags for Players[2..7]. The region
    // was reachable from the store before only through player_relation_at(), which lands in the
    // +0x8 relation sub-array and cannot name the scalar fields. No `_mut` suffix: the read sibling
    // is the PLURAL sim_view::player_desc_slots, so there is no name to disambiguate from.
    player_desc &player_desc_at(int32_t index) { return player_desc_slots_mut_[index]; }
    // Progress / Projects / Upgrades: the three tech tables llm_strat_tech_tables_reset clears.
    cfg_invention &cfg_invention_at(int32_t index) { return cfg_inventions_mut_[index]; }
    cfg_project   &cfg_project_at(int32_t index) { return cfg_projects_mut_[index]; }
    cfg_upgrade   &cfg_upgrade_at(int32_t index) { return cfg_upgrades_mut_[index]; }
    // System: bound as a RAW INT TABLE, exactly like its read sibling
    // sim_view::system_define_index_base -- the index is `system_idx * (0x8c / 4) + field`, which
    // that member's comment documents. Deliberately NOT a struct accessor: giving the write half a
    // record shape the read half does not have would be inventing a layout, and the one writer
    // (llm_strat_session_begin_multi's System[0].planets[1] = 0x1f) pokes a single cell.
    int32_t &system_define_index_at(int32_t index) { return system_define_index_mut_[index]; }

    // ---- the two regions bound in NEITHER half (no view sibling, so no `_mut`) ----
    // STRAT_ADVISOR_NEXT_TIME: the wall-clock time llm_strat_advisor_tick may next speak, which it
    // both reads and re-arms. It was invisible to the state matrix until 2026-08-31 -- a labelled
    // .bss address with no applied data type is not a defined data object, so build_regions never
    // made it a region and its `FSTP double ptr [0x00b64ba8]` store reached no write set at all.
    // the readiness report's R11 now fails on that class.
    double &advisor_next_time() { return *advisor_next_time_; }
    // MOUSE_BUTTONS_PREV: ONE byte, zeroed by map_FillDefaults. The registry records a 17-byte
    // extent because the SAVE manifest names a 17-byte blob starting here (this byte plus the four
    // dword globals after it) and the region took its first field's name -- write the byte, never
    // the blob.
    uint8_t &mouse_buttons_prev() { return *mouse_buttons_prev_; }

    // ---- two ADDRESS ESCAPES, in the text_scratch() family and for the same reason ------------
    // Both hand out a raw pointer because both are OUT-PARAMS to an ORIGINAL callee, not something
    // we write ourselves -- so W2's "no address ever escapes" is bought out here deliberately and
    // narrowly, exactly as text_scratch()/dmp_path_scratch() already are.
    //
    // land_dmp_scratch: the 32-byte ASCII filename buffer llm_game_land_players_on_planet formats
    // with utils_sprintf("%s%s_%02d%02d.DMP", ...). A DIFFERENT buffer from dmp_path_scratch()
    // above (0x00e58146 vs 0x00e58245) -- the landing path, not the completion-dispatch one.
    char *land_dmp_scratch() { return land_dmp_scratch_; }
    // scenario_planet_name_w: the wchar_t[16] UTF-16 buffer at 0x00e589c0 that
    // llm_strat_scenario_planet_clone hands to llm_str_ansi_to_wide as its DESTINATION, then
    // publishes by storing the returned pointer into G_TEXT_PTRS[0xa8] (sim_store::text_ptr_at).
    // THIRD member of the address-escape family, and it is here for the family's reason: the
    // widening is performed by the ORIGINAL callee, so we never write a byte of this region
    // ourselves -- we only supply where it goes.
    //
    // WHY THE EXTENT IS 16 AND NOT MEASURED. Exactly one instruction in the image names this
    // address (0x0045bb35 MOV EAX,0xe589c0; an operand scan over all 252238 instructions, recorded
    // in the Ghidra plate), so nothing bounds the allocation from below. 16 is the single writer's
    // measured REQUIREMENT: the source name at cfg_blob+0xfc is capped at 15 chars by the
    // truncate-to-12-plus-"..." branch at 0x0045baed, so 15 wide chars plus the terminator is
    // everything llm_str_ansi_to_wide can store. Read it as a floor, not as the allocation.
    wchar_t *scenario_planet_name_w() { return scenario_planet_name_w_; }
    // fog_of_war_base: the WHOLE RID_FOG_OF_WAR region (589824 bytes). llm_strat_new_game_init
    // clears it with ONE utils_fill_data call over the entire span, which neither field-level
    // accessor (fog_visible_by_count_at / fog_discovered_at, differently indexed) can express.
    // Same pointer value those two are derived from -- see state()'s fog_of_war_base local.
    uint8_t *fog_of_war_base() { return fog_of_war_base_; }

    // ---- SIM-RESID-IF REOPEN (2026-08-31): three more whole-region bases, for map_FillDefaults --
    //
    // WHY BASES AND NOT PER-RECORD ACCESSORS, which is the shape question this reopen had to
    // settle. map_FillDefaults performs 25 `utils_fill_data(dest, len, 0)` BYTE fills over whole
    // regions. A byte fill is not `= T{}` over a record array: these are save-serialized regions
    // whose padding bytes are part of the on-disk format, and a value-initialisation loop would
    // write a different byte pattern wherever a struct has padding. So the fill is expressed as
    // the original expresses it -- one call with a base and a length -- and the precedent is
    // fog_of_war_base() directly above, added for llm_strat_new_game_init's single whole-region
    // clear for exactly the same reason.
    //
    // The three are the destinations map_FillDefaults could not reach until now. Two of them had
    // no EXTENT in Ghidra at all (`undefined` length 1) until EN v381 typed them from the fill
    // lengths this very function loads; see tools/data/ghidra_findings.json 2026-08-31-1757-2/-3.
    // Element types are still unknown, which is precisely why nothing here indexes them: this
    // closure zeroes them and reads no field of any of them.
    uint8_t *map_objects_base() { return map_objects_base_; }           // RID_MAP_OBJECTS, 240000 B
    uint8_t *map_object_table_base() { return map_object_table_base_; } // RID_MAP_OBJECT_TABLE, 240000 B
    uint8_t *map_halfres_grid_base() { return map_halfres_grid_base_; } // RID_MAP_HALFRES_GRID, 65536 B

    // death_anim_table_mut: the MUTABLE base of RID_STRAT_DEATH_ANIM_TABLE (int32_t[6][4], 96 B),
    // whose read-only twin is sim_view::death_anim_table. It exists for ONE writer and that writer
    // is a bug -- Law 2 says reproduce it, and reproducing it needs an accessor.
    //
    // map_FillDefaults @0x004562c3-0x004562d3 means to store `game_clock + storage_stats_init_delay`
    // into each player's storage_stats[p].subtick_b_clock, but indexes with the INNER loop counter
    // (EBP-0x1c), which the preceding cap_accum/cap_prev loop leaves holding 10, instead of the
    // player counter (EBP-0x18). `IMUL EAX,[EBP-0x1c],0x58` + `FSTP [EAX+0xbf4d50]` therefore
    // resolves to the fixed address 0xbf50c0 on all eight iterations -- outside storage_stats
    // (0x00bf4d00..0x00bf4fc0) and 0x40 bytes into this table, i.e. elements [4][0] and [4][1].
    //
    // HOW TO WRITE IT: the store is an 8-byte double landing on two int32 slots, so the faithful
    // expression is a byte copy, not two arithmetic halves --
    //     std::memcpy(own.death_anim_table_mut() + 16, &value, sizeof(double));
    // 0x40/4 == 16, and 16..17 are inside the 24-element table, so the write is in bounds here in
    // the same way it is in bounds in the original.
    int32_t *death_anim_table_mut() { return death_anim_table_mut_; }

    // SB-BIND T2: the derived per-player row capacities this store indexes with. Read-only to
    // everyone but the binder and the fixture; use it in place of UNITS_PER_PLAYER and friends.
    const mh::state::roster_caps &caps() const { return caps_; }

private:
    // W3: the only two callers are the region-registry binder and the offline fixture.
    sim_store(unit *units, player_data *players, uint8_t *order_seq_id, building *buildings,
              turret *turrets, production *productions, lab *labs, order *order_queue,
              int32_t *order_queue_count, double *lockstep_horizon,
              uint8_t *net_lockstep_status_flags, wchar_t *text_scratch,
              int32_t *engage_candidate_scratch_count, unit *cur_unit, double *tick_budget,
              int32_t *state_loop_guard, ctrl_group *ctrl_groups, tile_object *tile_objects,
              uint8_t *passable, pop_stats *population, soldier *soldiers,
              housing_stats *unit_housing, uint8_t *path_slot_flags, int32_t *path_free_slot_count,
              uint16_t *click_select_target_id, player_profile *profiles, map_geom *geom_mut,
              uint8_t *bldg_enclosure_scratch, power_stats *power_stats_arr, building *cur_building,
              int32_t *cam_pan_target_col, int32_t *cam_pan_target_row,
              double *planet_mother_lost_time, mine *mines, unit_storage *storage,
              uint16_t *ui_selected_bldg_index, prod_shuttle_slot *prod_shuttle_slots,
              uint8_t *prod_complete_throttle, projectile *cur_projectile, projectile *projectile_pool,
              fx_anim *cur_fx_anim, fx_anim *fx_anim_pool, cam_jump_offset *cam_jump_offsets,
              double *cam_jump_scales, int32_t *cam_jump_queue_count,
              uint8_t *fog_visible_by_count, uint8_t *fog_discovered, int32_t *g_tmp_player,
              int32_t *g_tmp_x, int32_t *g_tmp_y, int32_t *g_tmp_sight, uint8_t *other_players_mask,
              uint16_t *cur_player_mut, uint16_t *cur_index_mut, unit **cur_unit_slot,
              building **cur_building_slot, projectile **cur_projectile_slot,
              fx_anim **cur_fx_anim_slot, storage_stats *storage_stats_arr,
              double *game_speed_player_factor, uint32_t *rng_state, uint32_t *is_human_mut,
              uint8_t *game_human_player_mask, uint8_t *players_raw, player_progress *progress,
              double *game_speed, double *planet_time, int32_t *available_buildings,
              int32_t *available_projects, cfg_unit *cfg_units_mut, cfg_weapon *cfg_weapons_mut,
              int32_t *map_width_mut, int32_t *map_height_mut, double *planet_invasion_time,
              int32_t *player_resources_mut, int32_t *planet_status_mut,
              llm_map_region_cell *region_grid, llm_map_bfs_entry *region_bfs_queue,
              int32_t *foreign_bldg_event_pending, double *invasion_alert_time,
              landing_spot *landing_spots, int32_t *ui_panel_mode_w, int32_t *ui_panel_page_w,
              int32_t *ui_panel_switch_pending, int32_t *ui_bldg_panel_refresh_pending,
              int32_t *ui_unit_panel_refresh_pending, int32_t *ui_mainpanel_refresh_pending,
              int32_t *ui_mainpanel_tab_index, int32_t *ui_unit_tab_toggle,
              int32_t *ui_view_resize_pending, int32_t *ui_event_queue_pos,
              int32_t *ui_event_queue_buf, int32_t *chat_input_active, int32_t *chat_input_len,
              int32_t *chat_input_cursor, char *chat_input_line,
              double *debug_resource_yield_cut, int32_t *session_mode_w,
              int32_t       *ai_active_player_count_w,
              path_waypoint *path_buffers, group_scratch_member *group_move_scratch,
              int32_t    *pathfinder_air_mode_flag,
              route_step *group_route_steps, group_member *group_members, uint32_t *path_wrap_mask,
              int32_t *group_order_goal_x, int32_t *group_order_goal_y, int32_t *group_anchor_x,
              int32_t *group_anchor_y, int32_t *group_order_owner, int32_t *group_member_count,
              uint8_t *group_member_tile,
              // SIM1-G-PREP (2026-08-20): the batch-G write set, in the SAME family order as
              // sim_state.cpp's argument list and as the member-init list below -- keep all three in
              // step. (a) group-move, (b) map-region routing, (c) completion accum, (d) unit queue,
              // (e) pathtrace.
              int32_t *group_centroid_x, int32_t *group_centroid_y, int32_t *group_path_build_idx,
              uint8_t *region_route_cand_scratch, uint32_t *region_flood_tile_queue,
              llm_map_region **region_route_bfs_queue, int32_t *bldg_completion_accum,
              uint32_t *unitq_cur_tile, int32_t *unitq_closed_count, int32_t *unitq_iter,
              int32_t *unitq_frontier_count, unitq_search_node *unitq_closed,
              unitq_search_node *unitq_frontier, uint32_t *pathtrace_coord_mask,
              uint32_t *pathtrace_col_mask, uint32_t *pathtrace_row_mask, uint32_t *pathtrace_map_w,
              uint32_t *pathtrace_map_h, uint32_t *pathtrace_half_w, uint32_t *pathtrace_half_h,
              uint32_t *pathtrace_half_w_m1, uint32_t *pathtrace_half_h_m1,
              int32_t *pathtrace_neg_half_w, int32_t *pathtrace_neg_half_h,
              uint32_t *pathtrace_start_col, uint32_t *pathtrace_start_row,
              uint32_t *pathtrace_walk_dir, uint32_t *pathtrace_goal_col,
              uint32_t *pathtrace_goal_row, uint32_t *pathtrace_goal_packed,
              uint32_t *pathtrace_approach_dir, uint8_t *pathtrace_dirs, uint16_t *pathtrace_pos,
              uint32_t *pathtrace_best_dir, int32_t *pathtrace_best_dist,
              uint32_t *pathtrace_forbid_cells, uint32_t *pathtrace_len,
              // SIM1-G2 (2026-08-20). Appended at the tail rather than interleaved, to avoid the
              // positional-argument-swap hazard this constructor already carries a warning about
              // (same-typed pointers in a row). (f) group-move distance scratch, (g) the chase-check
              // result cache.
              int32_t *group_move_dist_ref_x, int32_t *group_move_dist_ref_y,
              int32_t *group_move_dist_half_width, int32_t *group_move_dist_half_height,
              int32_t *unit_chase_result,
              // SIM1-G3 (2026-08-21). Appended at the tail per the same positional-
              // swap-avoidance precedent as the SIM1-G2 group above. llm_strat_unit_state_squad_merge
              // is this region's only writer in the whole 492-function closure.
              squad_formation_anchor_scratch *squad_anchor_scratch_mut,
              // SIM1-G4 (2026-08-22). Appended at the tail per the same positional-swap-
              // avoidance precedent as the SIM1-G2/G3 groups above.
              map_resources *resources_mut, int32_t *foreign_bldg_change_flag_mut,
              ui_base_marker_coord *ui_base_marker_coords_mut, char *dmp_path_scratch,
              // SIM-RESID-IF (2026-08-31): appended at the tail, never inserted -- a positional ctor with
              // 68 same-typed pointer params cannot survive an insertion.
              double *current_game_time, double *last_game_time, double *total_game_time, int32_t *cheat_penalty_score, int32_t *debug_tap_flag, int32_t *net_bw_stat, uint8_t *floating_msg_suppress_flag, uint8_t *lockstep_step_mult, int32_t *planet_int_table,
              double *sim_step_interval, int32_t *save_misc_dword, int32_t *game_land_no_start_unit_flag, int32_t *outer_planet_landed_flag, int32_t *outer_planet_land_state, uint8_t *planet_transition_state, int32_t *show_unit_flags, uint8_t *rng_seed_byte, double *lockstep_adapt_next_time, uint8_t *chat_target_mask, uint16_t *planet_map_pal4_white, uint16_t *planet_map_pal4_black,
              uint16_t *planet_map_pal4_magenta, uint16_t *planet_map_pal4_yellow, uint16_t *planet_map_pal5_black, uint16_t *planet_map_pal5_magenta, uint16_t *planet_map_pal5_green, uint16_t *planet_map_pal5_red, uint16_t *planet_map_pal5_blue, int32_t *squad_bb_scan_player, int32_t *squad_bb_target_owner, int32_t *squad_bb_target_building_id, int32_t *squad_bb_target_energy_pct, int32_t *squad_bb_target_building_idx,
              squad_status_slot *squad_status, int32_t *squad_status_count, int32_t *order_pending_count, int32_t *order_staging_count, double *net_peer_horizon, double *net_peer_horizon_pending, int32_t *build_placement_id, uint32_t *bldg_footprint_passable_save_slot, int32_t *advisor_phase, int32_t *floating_msg_queue_active, job_result_entry *path_job_result, uint8_t *game_mode,
              int32_t *tutorial_build_type_filter, uint32_t *tutorial_forced_bldg_selection,
              uint8_t *tutorial_hq_attack_scenario_done, int32_t *tutorial_pending_build_placement_id, int32_t *tutorial_rmb_limit_flag, int32_t *tutorial_reset_slot_0050a678, int32_t *dlg_state_flags, uint8_t *player_control_mask, int32_t *gfx_ui_color, int32_t *ui_race_sel_pending_gfx_idx,
              ui_fade_transition_state *ui_fade_transition, void **ui_menu_async_callback_a,
              widget_list **ui_menu_widget_list, ui_widget *ui_tutorial_hint_widget, ui_widget *ui_wgt_tutorial_welcome, ui_widget *ui_wgt_menu_screen_title, ui_widget *ui_outcome_dlg_title_widget, ui_widget *ui_outcome_dlg_message_widget, ui_widget *ui_wgt_frame_menu_panel, uint8_t *injected_map_planet_slot, int32_t *view_size_mode, int32_t *view_size_mode_save, map_header *current_map_data,
              // SIM-RESID-IF re-close (2026-08-31): the WRITE halves. Appended at the tail, never
              // inserted -- the positional-swap hazard this constructor warns about only gets
              // worse as the arity grows.
              int32_t *current_system_mut, int32_t *planet_index_mut, double *game_clock_mut,
              double *game_time_delta_mut, int16_t *player_side_mut,
              uint16_t *local_player_slot_mut, int32_t *player_race_mut, int32_t *sim_active_mut,
              int32_t *mp_ally_victory_rule_flag_mut, int32_t *system_lost_msg_shown_flag_mut,
              uint8_t *ui_bldg_tab_select_blocked_mut, uint8_t *bldg_completion_slot_count_mut,
              uint32_t *width_m_mut, uint32_t *height_m_mut, int32_t *tutorial_step_mut,
              int32_t *ai_enabled_mut, int32_t *ui_panel_fallback_table_mut,
              cfg_planet *cfg_planets_mut, cfg_invention *cfg_inventions_mut,
              cfg_project *cfg_projects_mut, cfg_upgrade *cfg_upgrades_mut,
              int32_t *system_define_index_mut, double *advisor_next_time,
              uint8_t *mouse_buttons_prev, char *land_dmp_scratch, uint8_t *fog_of_war_base,
              // SIM-RESID-IF reopen (2026-08-31), appended at the tail for the same reason the
              // 2026-08-31 re-close block above was: never inserted.
              uint8_t *map_objects_base, uint8_t *map_object_table_base,
              uint8_t *map_halfres_grid_base, int32_t *death_anim_table_mut,
              wchar_t        *scenario_planet_name_w,
              const wchar_t **text_ptrs_mut,
              // SIM-RESID-F (2026-09-01), appended at the tail for the same reason every block
              // above it was: never inserted.
              player_desc *player_desc_slots_mut,
              // LT1 lib_trans (2026-09-02), appended at the tail: the map-region pool slots (LT1C),
              // the menu-teardown writes and the ambient table escape (LT1D).
              llm_map_region **region_list_head_mut, llm_map_region **region_pool_free_head,
              llm_map_region **region_by_index, int32_t *region_alloc_counter,
              int32_t *last_map_index, void **ui_menu_async_callback_base,
              uint8_t *ui_menu_state_mut, int32_t *quit_teardown_forced_flag,
              uint8_t *snd_ambient_by_planet_base,
              // LT1C c4 (2026-09-02), appended at the tail like every block above it:
              // recompute_cell_grid's two island outputs.
              uint8_t *bldg_cell_grid, int32_t *bldg_cell_grid_row_shift,
              // SIM1-H (2026-09-10), appended at the tail like every block above it: the two write
              // halves batch H's host-side callers need -- the cfg Building[] record (the ONLY
              // writer in the whole sim set is llm_strat_bldg_init_defaults, the boot-time defaults
              // seeder) and the region coordinate wrap mask (written by llm_map_build_regions).
              cfg_building *cfg_buildings_mut, uint32_t *region_coord_wrap_mask_mut,
              // SIM1-H wave 2 (2026-09-10), appended at the tail like every block above it: the
              // nav-region pipeline's four write bindings.
              uint32_t *region_merge_threshold_mut, uint8_t *region_route_step_deltas_mut,
              uint8_t *region_route_step_delta_wrap, llm_map_bfs_entry *region_flood_path_queue)
        : units_(units), players_(players), order_seq_id_(order_seq_id), buildings_(buildings),
          turrets_(turrets), productions_(productions), labs_(labs), order_queue_(order_queue),
          order_queue_count_(order_queue_count), lockstep_horizon_(lockstep_horizon),
          net_lockstep_status_flags_(net_lockstep_status_flags), text_scratch_(text_scratch),
          engage_candidate_scratch_count_(engage_candidate_scratch_count), cur_unit_(cur_unit),
          tick_budget_(tick_budget), state_loop_guard_(state_loop_guard), ctrl_groups_(ctrl_groups),
          planes_(tile_objects, passable, path_buffers, path_slot_flags,
                  path_free_slot_count),
          population_(population), soldiers_(soldiers), unit_housing_(unit_housing),
          click_select_target_id_(click_select_target_id), profiles_(profiles),
          geom_mut_(geom_mut), bldg_enclosure_scratch_(bldg_enclosure_scratch),
          power_stats_(power_stats_arr), cur_building_(cur_building),
          cam_pan_target_col_(cam_pan_target_col), cam_pan_target_row_(cam_pan_target_row),
          planet_mother_lost_time_(planet_mother_lost_time), mines_(mines), storage_(storage),
          ui_selected_bldg_index_(ui_selected_bldg_index),
          prod_shuttle_slots_(prod_shuttle_slots),
          prod_complete_throttle_(prod_complete_throttle), cur_projectile_(cur_projectile),
          projectile_pool_(projectile_pool), cur_fx_anim_(cur_fx_anim),
          fx_anim_pool_(fx_anim_pool), cam_jump_offsets_(cam_jump_offsets),
          cam_jump_scales_(cam_jump_scales), cam_jump_queue_count_(cam_jump_queue_count),
          fog_visible_by_count_(fog_visible_by_count), fog_discovered_(fog_discovered),
          g_tmp_player_(g_tmp_player), g_tmp_x_(g_tmp_x), g_tmp_y_(g_tmp_y),
          g_tmp_sight_(g_tmp_sight), other_players_mask_(other_players_mask),
          cur_player_mut_(cur_player_mut), cur_index_mut_(cur_index_mut),
          cur_unit_slot_(cur_unit_slot), cur_building_slot_(cur_building_slot),
          cur_projectile_slot_(cur_projectile_slot), cur_fx_anim_slot_(cur_fx_anim_slot),
          storage_stats_(storage_stats_arr),
          game_speed_player_factor_(game_speed_player_factor), rng_state_(rng_state),
          is_human_mut_(is_human_mut), game_human_player_mask_(game_human_player_mask),
          players_raw_(players_raw), progress_(progress), game_speed_(game_speed),
          planet_time_(planet_time), available_buildings_(available_buildings),
          available_projects_(available_projects), cfg_units_mut_(cfg_units_mut),
          cfg_weapons_mut_(cfg_weapons_mut), map_width_mut_(map_width_mut),
          map_height_mut_(map_height_mut), planet_invasion_time_(planet_invasion_time),
          player_resources_mut_(player_resources_mut), planet_status_mut_(planet_status_mut),
          region_grid_(region_grid), region_bfs_queue_(region_bfs_queue),
          foreign_bldg_event_pending_(foreign_bldg_event_pending),
          invasion_alert_time_(invasion_alert_time), landing_spots_(landing_spots),
          ui_panel_mode_(ui_panel_mode_w), ui_panel_page_(ui_panel_page_w),
          ui_panel_switch_pending_(ui_panel_switch_pending),
          ui_bldg_panel_refresh_pending_(ui_bldg_panel_refresh_pending),
          ui_unit_panel_refresh_pending_(ui_unit_panel_refresh_pending),
          ui_mainpanel_refresh_pending_(ui_mainpanel_refresh_pending),
          ui_mainpanel_tab_index_(ui_mainpanel_tab_index), ui_unit_tab_toggle_(ui_unit_tab_toggle),
          ui_view_resize_pending_(ui_view_resize_pending), ui_event_queue_pos_(ui_event_queue_pos),
          ui_event_queue_buf_(ui_event_queue_buf), chat_input_active_(chat_input_active),
          chat_input_len_(chat_input_len), chat_input_cursor_(chat_input_cursor),
          chat_input_line_(chat_input_line),
          debug_resource_yield_cut_(debug_resource_yield_cut), session_mode_w_(session_mode_w),
          ai_active_player_count_w_(ai_active_player_count_w),
          group_move_scratch_(group_move_scratch),
          pathfinder_air_mode_flag_(pathfinder_air_mode_flag),
          group_route_steps_(group_route_steps), group_members_(group_members),
          path_wrap_mask_(path_wrap_mask), group_order_goal_x_(group_order_goal_x),
          group_order_goal_y_(group_order_goal_y), group_anchor_x_(group_anchor_x),
          group_anchor_y_(group_anchor_y), group_order_owner_(group_order_owner),
          group_member_count_(group_member_count), group_member_tile_(group_member_tile),
          group_centroid_x_(group_centroid_x),
          group_centroid_y_(group_centroid_y), group_path_build_idx_(group_path_build_idx),
          region_route_cand_scratch_(region_route_cand_scratch),
          region_flood_tile_queue_(region_flood_tile_queue),
          region_route_bfs_queue_(region_route_bfs_queue),
          bldg_completion_accum_(bldg_completion_accum), unitq_cur_tile_(unitq_cur_tile),
          unitq_closed_count_(unitq_closed_count), unitq_iter_(unitq_iter),
          unitq_frontier_count_(unitq_frontier_count), unitq_closed_(unitq_closed),
          unitq_frontier_(unitq_frontier), pathtrace_coord_mask_(pathtrace_coord_mask),
          pathtrace_col_mask_(pathtrace_col_mask), pathtrace_row_mask_(pathtrace_row_mask),
          pathtrace_map_w_(pathtrace_map_w), pathtrace_map_h_(pathtrace_map_h),
          pathtrace_half_w_(pathtrace_half_w), pathtrace_half_h_(pathtrace_half_h),
          pathtrace_half_w_m1_(pathtrace_half_w_m1), pathtrace_half_h_m1_(pathtrace_half_h_m1),
          pathtrace_neg_half_w_(pathtrace_neg_half_w), pathtrace_neg_half_h_(pathtrace_neg_half_h),
          pathtrace_start_col_(pathtrace_start_col), pathtrace_start_row_(pathtrace_start_row),
          pathtrace_walk_dir_(pathtrace_walk_dir), pathtrace_goal_col_(pathtrace_goal_col),
          pathtrace_goal_row_(pathtrace_goal_row), pathtrace_goal_packed_(pathtrace_goal_packed),
          pathtrace_approach_dir_(pathtrace_approach_dir), pathtrace_dirs_(pathtrace_dirs),
          pathtrace_pos_(pathtrace_pos), pathtrace_best_dir_(pathtrace_best_dir),
          pathtrace_best_dist_(pathtrace_best_dist), pathtrace_forbid_cells_(pathtrace_forbid_cells),
          pathtrace_len_(pathtrace_len),
          group_move_dist_ref_x_(group_move_dist_ref_x), group_move_dist_ref_y_(group_move_dist_ref_y),
          group_move_dist_half_width_(group_move_dist_half_width),
          group_move_dist_half_height_(group_move_dist_half_height),
          unit_chase_result_(unit_chase_result),
          squad_anchor_scratch_mut_(squad_anchor_scratch_mut), resources_mut_(resources_mut),
          foreign_bldg_change_flag_mut_(foreign_bldg_change_flag_mut),
          ui_base_marker_coords_mut_(ui_base_marker_coords_mut),
          dmp_path_scratch_(dmp_path_scratch),
          current_game_time_(current_game_time), last_game_time_(last_game_time),
          total_game_time_(total_game_time), cheat_penalty_score_(cheat_penalty_score),
          debug_tap_flag_(debug_tap_flag), net_bw_stat_(net_bw_stat),
          floating_msg_suppress_flag_(floating_msg_suppress_flag),
          lockstep_step_mult_(lockstep_step_mult), planet_int_table_(planet_int_table),
          sim_step_interval_(sim_step_interval), save_misc_dword_(save_misc_dword),
          game_land_no_start_unit_flag_(game_land_no_start_unit_flag),
          outer_planet_landed_flag_(outer_planet_landed_flag),
          outer_planet_land_state_(outer_planet_land_state),
          planet_transition_state_(planet_transition_state), show_unit_flags_(show_unit_flags),
          rng_seed_byte_(rng_seed_byte), lockstep_adapt_next_time_(lockstep_adapt_next_time),
          chat_target_mask_(chat_target_mask), planet_map_pal4_white_(planet_map_pal4_white),
          planet_map_pal4_black_(planet_map_pal4_black),
          planet_map_pal4_magenta_(planet_map_pal4_magenta),
          planet_map_pal4_yellow_(planet_map_pal4_yellow),
          planet_map_pal5_black_(planet_map_pal5_black),
          planet_map_pal5_magenta_(planet_map_pal5_magenta),
          planet_map_pal5_green_(planet_map_pal5_green), planet_map_pal5_red_(planet_map_pal5_red),
          planet_map_pal5_blue_(planet_map_pal5_blue),
          squad_bb_scan_player_(squad_bb_scan_player),
          squad_bb_target_owner_(squad_bb_target_owner),
          squad_bb_target_building_id_(squad_bb_target_building_id),
          squad_bb_target_energy_pct_(squad_bb_target_energy_pct),
          squad_bb_target_building_idx_(squad_bb_target_building_idx),
          squad_status_(squad_status), squad_status_count_(squad_status_count),
          order_pending_count_(order_pending_count), order_staging_count_(order_staging_count),
          net_peer_horizon_(net_peer_horizon),
          net_peer_horizon_pending_(net_peer_horizon_pending),
          build_placement_id_(build_placement_id),
          bldg_footprint_passable_save_slot_(bldg_footprint_passable_save_slot),
          advisor_phase_(advisor_phase), floating_msg_queue_active_(floating_msg_queue_active),
          path_job_result_(path_job_result), game_mode_(game_mode),
          tutorial_build_type_filter_(tutorial_build_type_filter),
          tutorial_forced_bldg_selection_(tutorial_forced_bldg_selection),
          tutorial_hq_attack_scenario_done_(tutorial_hq_attack_scenario_done),
          tutorial_pending_build_placement_id_(tutorial_pending_build_placement_id),
          tutorial_rmb_limit_flag_(tutorial_rmb_limit_flag),
          tutorial_reset_slot_0050a678_(tutorial_reset_slot_0050a678),
          dlg_state_flags_(dlg_state_flags), player_control_mask_(player_control_mask),
          gfx_ui_color_(gfx_ui_color), ui_race_sel_pending_gfx_idx_(ui_race_sel_pending_gfx_idx),
          ui_fade_transition_(ui_fade_transition),
          ui_menu_async_callback_a_(ui_menu_async_callback_a),
          ui_menu_widget_list_(ui_menu_widget_list),
          ui_tutorial_hint_widget_(ui_tutorial_hint_widget),
          ui_wgt_tutorial_welcome_(ui_wgt_tutorial_welcome),
          ui_wgt_menu_screen_title_(ui_wgt_menu_screen_title),
          ui_outcome_dlg_title_widget_(ui_outcome_dlg_title_widget),
          ui_outcome_dlg_message_widget_(ui_outcome_dlg_message_widget),
          ui_wgt_frame_menu_panel_(ui_wgt_frame_menu_panel),
          injected_map_planet_slot_(injected_map_planet_slot), view_size_mode_(view_size_mode),
          view_size_mode_save_(view_size_mode_save), current_map_data_(current_map_data),
          current_system_mut_(current_system_mut), planet_index_mut_(planet_index_mut),
          game_clock_mut_(game_clock_mut), game_time_delta_mut_(game_time_delta_mut),
          player_side_mut_(player_side_mut), local_player_slot_mut_(local_player_slot_mut),
          player_race_mut_(player_race_mut), sim_active_mut_(sim_active_mut),
          mp_ally_victory_rule_flag_mut_(mp_ally_victory_rule_flag_mut),
          system_lost_msg_shown_flag_mut_(system_lost_msg_shown_flag_mut),
          ui_bldg_tab_select_blocked_mut_(ui_bldg_tab_select_blocked_mut),
          bldg_completion_slot_count_mut_(bldg_completion_slot_count_mut),
          width_m_mut_(width_m_mut), height_m_mut_(height_m_mut),
          tutorial_step_mut_(tutorial_step_mut), ai_enabled_mut_(ai_enabled_mut),
          ui_panel_fallback_table_mut_(ui_panel_fallback_table_mut),
          cfg_planets_mut_(cfg_planets_mut), cfg_inventions_mut_(cfg_inventions_mut),
          cfg_projects_mut_(cfg_projects_mut), cfg_upgrades_mut_(cfg_upgrades_mut),
          system_define_index_mut_(system_define_index_mut),
          advisor_next_time_(advisor_next_time), mouse_buttons_prev_(mouse_buttons_prev),
          land_dmp_scratch_(land_dmp_scratch), fog_of_war_base_(fog_of_war_base),
          map_objects_base_(map_objects_base), map_object_table_base_(map_object_table_base),
          map_halfres_grid_base_(map_halfres_grid_base),
          death_anim_table_mut_(death_anim_table_mut),
          scenario_planet_name_w_(scenario_planet_name_w), text_ptrs_mut_(text_ptrs_mut),
          player_desc_slots_mut_(player_desc_slots_mut),
          region_list_head_mut_(region_list_head_mut),
          region_pool_free_head_(region_pool_free_head), region_by_index_(region_by_index),
          region_alloc_counter_(region_alloc_counter), last_map_index_(last_map_index),
          ui_menu_async_callback_base_(ui_menu_async_callback_base),
          ui_menu_state_mut_(ui_menu_state_mut),
          quit_teardown_forced_flag_(quit_teardown_forced_flag),
          snd_ambient_by_planet_base_(snd_ambient_by_planet_base),
          bldg_cell_grid_(bldg_cell_grid),
          bldg_cell_grid_row_shift_(bldg_cell_grid_row_shift),
          cfg_buildings_mut_(cfg_buildings_mut),
          region_coord_wrap_mask_mut_(region_coord_wrap_mask_mut),
          region_merge_threshold_mut_(region_merge_threshold_mut),
          region_route_step_deltas_mut_(region_route_step_deltas_mut),
          region_route_step_delta_wrap_(region_route_step_delta_wrap),
          region_flood_path_queue_(region_flood_path_queue) {}
    friend sim_state state();
    friend struct sim_fixture;

    // W2: private, so no game-state address ever escapes this class.
    // SB-BIND T2: the same derived row capacities sim_view carries. Set by the two friends after
    // construction rather than threaded through a 35-argument constructor; defaults to stock so an
    // offline fixture that never sets it behaves exactly as before.
    mh::state::roster_caps caps_{mh::state::STOCK_ROSTER_CAPS};

    unit        *units_;
    player_data *players_;
    uint8_t     *order_seq_id_;
    building    *buildings_;
    turret      *turrets_;
    production  *productions_;
    lab         *labs_;
    order       *order_queue_;
    int32_t     *order_queue_count_;
    double      *lockstep_horizon_;
    uint8_t     *net_lockstep_status_flags_;
    wchar_t     *text_scratch_;
    int32_t     *engage_candidate_scratch_count_;
    unit        *cur_unit_;
    double      *tick_budget_;
    int32_t     *state_loop_guard_;
    ctrl_group  *ctrl_groups_;
    // TACT0 (2026-08-24): the five mode-shared planes are ONE binding, shared with libmh/tact/.
    // They used to be five pointers here and would have been five more in tact_store --
    // two independent bindings of the same alternating bytes. See state/mode_planes.h.
    mh::state::mode_planes planes_;
    pop_stats             *population_;
    soldier               *soldiers_;
    housing_stats         *unit_housing_;
    uint16_t              *click_select_target_id_;
    player_profile        *profiles_;
    map_geom              *geom_mut_;
    uint8_t               *bldg_enclosure_scratch_;
    power_stats           *power_stats_;
    building              *cur_building_;
    int32_t               *cam_pan_target_col_;
    int32_t               *cam_pan_target_row_;
    double                *planet_mother_lost_time_;
    mine                  *mines_;
    unit_storage          *storage_;
    uint16_t              *ui_selected_bldg_index_;
    prod_shuttle_slot     *prod_shuttle_slots_;
    uint8_t               *prod_complete_throttle_;
    projectile            *cur_projectile_;
    projectile            *projectile_pool_;
    fx_anim               *cur_fx_anim_;
    fx_anim               *fx_anim_pool_;
    cam_jump_offset       *cam_jump_offsets_;
    double                *cam_jump_scales_;
    int32_t               *cam_jump_queue_count_;

    // ---- SIM1E fog/sight family (2026-08-16) ----
    uint8_t *fog_visible_by_count_;
    uint8_t *fog_discovered_;
    int32_t *g_tmp_player_;
    int32_t *g_tmp_x_;
    int32_t *g_tmp_y_;
    int32_t *g_tmp_sight_;
    uint8_t *other_players_mask_;

    // ---- SIM1E sim_step (the domain root, 2026-08-16) ----
    uint16_t      *cur_player_mut_;
    uint16_t      *cur_index_mut_;
    unit         **cur_unit_slot_;
    building     **cur_building_slot_;
    projectile   **cur_projectile_slot_;
    fx_anim      **cur_fx_anim_slot_;
    storage_stats *storage_stats_;

    // ---- SIM1F (2026-08-16) ----
    double   *game_speed_player_factor_;
    uint32_t *rng_state_;
    uint32_t *is_human_mut_;
    uint8_t  *game_human_player_mask_;
    uint8_t  *players_raw_;

    // ---- SIM1F (2026-08-16) ----
    player_progress *progress_;
    double          *game_speed_;
    double          *planet_time_;
    int32_t         *available_buildings_;
    int32_t         *available_projects_;

    // ---- SIM1F (2026-08-16) ----
    cfg_unit            *cfg_units_mut_;
    cfg_weapon          *cfg_weapons_mut_;
    int32_t             *map_width_mut_;
    int32_t             *map_height_mut_;
    double              *planet_invasion_time_;
    int32_t             *player_resources_mut_;
    int32_t             *planet_status_mut_;
    llm_map_region_cell *region_grid_;
    llm_map_bfs_entry   *region_bfs_queue_;
    int32_t             *foreign_bldg_event_pending_;

    // ---- SIM1F (2026-08-17) ----
    double       *invasion_alert_time_;
    landing_spot *landing_spots_;

    // ---- SIM1F (2026-08-18): game_SetEvent's UI-panel/event-queue/chat-input state ----
    int32_t *ui_panel_mode_;
    int32_t *ui_panel_page_;
    int32_t *ui_panel_switch_pending_;
    int32_t *ui_bldg_panel_refresh_pending_;
    int32_t *ui_unit_panel_refresh_pending_;
    int32_t *ui_mainpanel_refresh_pending_;
    int32_t *ui_mainpanel_tab_index_;
    int32_t *ui_unit_tab_toggle_;
    int32_t *ui_view_resize_pending_;
    int32_t *ui_event_queue_pos_;
    int32_t *ui_event_queue_buf_;
    int32_t *chat_input_active_;
    int32_t *chat_input_len_;
    int32_t *chat_input_cursor_;
    char    *chat_input_line_;
    // SIM1F (2026-08-18): llm_strat_player_presence_lost's mutable session-level writes.
    double  *debug_resource_yield_cut_;
    int32_t *session_mode_w_;
    int32_t *ai_active_player_count_w_;

    // ---- SIM1-G1 (2026-08-19): the move state handlers' mutable roster writes ----
    group_scratch_member *group_move_scratch_; // _G_LLM_STRAT_GROUP_MOVE_SCRATCH (group_marshal compaction)

    // ---- SIM1-G1 (2026-08-20) ----
    int32_t      *pathfinder_air_mode_flag_; // _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG (group_step_ground/_plane)
    route_step   *group_route_steps_;        // _G_LLM_STRAT_GROUP_ROUTE_STEPS (group_move_order_commit)
    group_member *group_members_;            // _G_LLM_STRAT_GROUP_MEMBERS
    uint32_t     *path_wrap_mask_;           // _G_LLM_STRAT_PATH_WRAP_MASK
    int32_t      *group_order_goal_x_;       // _G_LLM_STRAT_GROUP_ORDER_GOAL_X
    int32_t      *group_order_goal_y_;       // _G_LLM_STRAT_GROUP_ORDER_GOAL_Y
    int32_t      *group_anchor_x_;           // _G_LLM_STRAT_GROUP_ANCHOR_X
    int32_t      *group_anchor_y_;           // _G_LLM_STRAT_GROUP_ANCHOR_Y
    int32_t      *group_order_owner_;        // _G_LLM_STRAT_GROUP_ORDER_OWNER
    int32_t      *group_member_count_;       // _G_LLM_STRAT_GROUP_MEMBER_COUNT
    uint8_t      *group_member_tile_;        // _G_LLM_STRAT_GROUP_MEMBER_TILE (group_plan_formation_positions)

    // SIM1-G-PREP (2026-08-20). The batch-G write set. Order matches the ctor parameter list, the
    // member-init list, and sim_state.cpp's argument list -- all four are one sequence.
    int32_t           *group_centroid_x_;          // _G_LLM_STRAT_GROUP_CENTROID_X
    int32_t           *group_centroid_y_;          // _G_LLM_STRAT_GROUP_CENTROID_Y
    int32_t           *group_path_build_idx_;      // _G_LLM_STRAT_GROUP_PATH_BUILD_IDX
    uint8_t           *region_route_cand_scratch_; // _G_LLM_MAP_REGION_ROUTE_CAND_SCRATCH
    uint32_t          *region_flood_tile_queue_;   // _G_LLM_MAP_REGION_FLOOD_TILE_QUEUE
    llm_map_region   **region_route_bfs_queue_;    // _G_LLM_MAP_REGION_ROUTE_BFS_QUEUE
    int32_t           *bldg_completion_accum_;     // _G_LLM_STRAT_BLDG_COMPLETION_ACCUM
    uint32_t          *unitq_cur_tile_;            // _G_LLM_UNITQ_CUR_TILE
    int32_t           *unitq_closed_count_;        // _G_LLM_UNITQ_CLOSED_COUNT
    int32_t           *unitq_iter_;                // _G_LLM_UNITQ_ITER
    int32_t           *unitq_frontier_count_;      // _G_LLM_UNITQ_FRONTIER_COUNT
    unitq_search_node *unitq_closed_;              // _G_LLM_UNITQ_CLOSED
    unitq_search_node *unitq_frontier_;            // _G_LLM_UNITQ_FRONTIER
    uint32_t          *pathtrace_coord_mask_;      // _G_LLM_STRAT_PATHTRACE_COORD_MASK
    uint32_t          *pathtrace_col_mask_;        // _G_LLM_STRAT_PATHTRACE_COL_MASK
    uint32_t          *pathtrace_row_mask_;        // _G_LLM_STRAT_PATHTRACE_ROW_MASK
    uint32_t          *pathtrace_map_w_;           // _G_LLM_STRAT_PATHTRACE_MAP_W
    uint32_t          *pathtrace_map_h_;           // _G_LLM_STRAT_PATHTRACE_MAP_H
    uint32_t          *pathtrace_half_w_;          // _G_LLM_STRAT_PATHTRACE_HALF_W
    uint32_t          *pathtrace_half_h_;          // _G_LLM_STRAT_PATHTRACE_HALF_H
    uint32_t          *pathtrace_half_w_m1_;       // _G_LLM_STRAT_PATHTRACE_HALF_W_M1 (dead store)
    uint32_t          *pathtrace_half_h_m1_;       // _G_LLM_STRAT_PATHTRACE_HALF_H_M1 (dead store)
    int32_t           *pathtrace_neg_half_w_;      // _G_LLM_STRAT_PATHTRACE_NEG_HALF_W
    int32_t           *pathtrace_neg_half_h_;      // _G_LLM_STRAT_PATHTRACE_NEG_HALF_H
    uint32_t          *pathtrace_start_col_;       // _G_LLM_STRAT_PATHTRACE_START_COL
    uint32_t          *pathtrace_start_row_;       // _G_LLM_STRAT_PATHTRACE_START_ROW
    uint32_t          *pathtrace_walk_dir_;        // _G_LLM_STRAT_PATHTRACE_WALK_DIR
    uint32_t          *pathtrace_goal_col_;        // _G_LLM_STRAT_PATHTRACE_GOAL_COL
    uint32_t          *pathtrace_goal_row_;        // _G_LLM_STRAT_PATHTRACE_GOAL_ROW
    uint32_t          *pathtrace_goal_packed_;     // _G_LLM_STRAT_PATHTRACE_GOAL_PACKED
    uint32_t          *pathtrace_approach_dir_;    // _G_LLM_STRAT_PATHTRACE_APPROACH_DIR
    uint8_t           *pathtrace_dirs_;            // _G_LLM_STRAT_PATHTRACE_DIRS
    uint16_t          *pathtrace_pos_;             // _G_LLM_STRAT_PATHTRACE_POS
    uint32_t          *pathtrace_best_dir_;        // _G_LLM_STRAT_PATHTRACE_BEST_DIR
    int32_t           *pathtrace_best_dist_;       // _G_LLM_STRAT_PATHTRACE_BEST_DIST
    uint32_t          *pathtrace_forbid_cells_;    // _G_LLM_STRAT_PATHTRACE_FORBID_CELLS
    uint32_t          *pathtrace_len_;             // _G_LLM_STRAT_PATHTRACE_LEN

    // SIM1-G2 (2026-08-20). Appended at the tail -- see the ctor's matching comment.
    int32_t *group_move_dist_ref_x_;       // _G_LLM_STRAT_GROUP_MOVE_DIST_REF_X
    int32_t *group_move_dist_ref_y_;       // _G_LLM_STRAT_GROUP_MOVE_DIST_REF_Y
    int32_t *group_move_dist_half_width_;  // _G_LLM_STRAT_GROUP_MOVE_DIST_HALF_WIDTH
    int32_t *group_move_dist_half_height_; // _G_LLM_STRAT_GROUP_MOVE_DIST_HALF_HEIGHT
    int32_t *unit_chase_result_;           // _G_LLM_STRAT_UNIT_CHASE_RESULT

    // SIM1-G3 (2026-08-21). Appended at the tail -- see the ctor's matching comment.
    squad_formation_anchor_scratch *squad_anchor_scratch_mut_;     // _G_LLM_STRAT_SQUAD_FORMATION_ANCHOR_SCRATCH
    map_resources                  *resources_mut_;                // RID_RESOURCES, second binding
    int32_t                        *foreign_bldg_change_flag_mut_; // _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG, second binding
    ui_base_marker_coord           *ui_base_marker_coords_mut_;    // _G_LLM_STRAT_UI_BASE_MARKER_COORDS
    char                           *dmp_path_scratch_;             // _G_LLM_STRAT_DMP_PATH_SCRATCH

    // SIM-RESID-IF (2026-08-31) -- appended at the tail, per the same positional-swap-avoidance
    // precedent as the SIM1-G2/G3/G4 groups above.
    double                   *current_game_time_;                   // RID_CURRENT_GAME_TIME
    double                   *last_game_time_;                      // RID_LAST_GAME_TIME
    double                   *total_game_time_;                     // RID_TOTAL_GAME_TIME
    int32_t                  *cheat_penalty_score_;                 // RID_CHEAT_PENALTY_SCORE
    int32_t                  *debug_tap_flag_;                      // RID_DEBUG_TAP_FLAG
    int32_t                  *net_bw_stat_;                         // RID_NET_BW_STAT
    uint8_t                  *floating_msg_suppress_flag_;          // RID_STRAT_FLOATING_MSG_SUPPRESS_FLAG
    uint8_t                  *lockstep_step_mult_;                  // RID_STRAT_LOCKSTEP_STEP_MULT
    int32_t                  *planet_int_table_;                    // RID_STRAT_PLANET_INT_TABLE
    double                   *sim_step_interval_;                   // RID_STRAT_SIM_STEP_INTERVAL
    int32_t                  *save_misc_dword_;                     // RID_STRAT_SAVE_MISC_DWORD
    int32_t                  *game_land_no_start_unit_flag_;        // RID_GAME_LAND_NO_START_UNIT_FLAG
    int32_t                  *outer_planet_landed_flag_;            // RID_STRAT_OUTER_PLANET_LANDED_FLAG
    int32_t                  *outer_planet_land_state_;             // RID_STRAT_OUTER_PLANET_LAND_STATE
    uint8_t                  *planet_transition_state_;             // RID_STRAT_PLANET_TRANSITION_STATE
    int32_t                  *show_unit_flags_;                     // RID_STRAT_SHOW_UNIT_FLAGS
    uint8_t                  *rng_seed_byte_;                       // RID_STRAT_RNG_SEED_BYTE
    double                   *lockstep_adapt_next_time_;            // RID_STRAT_LOCKSTEP_ADAPT_NEXT_TIME
    uint8_t                  *chat_target_mask_;                    // RID_CHAT_TARGET_MASK
    uint16_t                 *planet_map_pal4_white_;               // RID_STRAT_PLANET_MAP_PAL4_WHITE
    uint16_t                 *planet_map_pal4_black_;               // RID_STRAT_PLANET_MAP_PAL4_BLACK
    uint16_t                 *planet_map_pal4_magenta_;             // RID_STRAT_PLANET_MAP_PAL4_MAGENTA
    uint16_t                 *planet_map_pal4_yellow_;              // RID_STRAT_PLANET_MAP_PAL4_YELLOW
    uint16_t                 *planet_map_pal5_black_;               // RID_STRAT_PLANET_MAP_PAL5_BLACK
    uint16_t                 *planet_map_pal5_magenta_;             // RID_STRAT_PLANET_MAP_PAL5_MAGENTA
    uint16_t                 *planet_map_pal5_green_;               // RID_STRAT_PLANET_MAP_PAL5_GREEN
    uint16_t                 *planet_map_pal5_red_;                 // RID_STRAT_PLANET_MAP_PAL5_RED
    uint16_t                 *planet_map_pal5_blue_;                // RID_STRAT_PLANET_MAP_PAL5_BLUE
    int32_t                  *squad_bb_scan_player_;                // RID_SQUAD_BB_SCAN_PLAYER
    int32_t                  *squad_bb_target_owner_;               // RID_SQUAD_BB_TARGET_OWNER
    int32_t                  *squad_bb_target_building_id_;         // RID_SQUAD_BB_TARGET_BUILDING_ID
    int32_t                  *squad_bb_target_energy_pct_;          // RID_SQUAD_BB_TARGET_ENERGY_PCT
    int32_t                  *squad_bb_target_building_idx_;        // RID_SQUAD_BB_TARGET_BUILDING_IDX
    squad_status_slot        *squad_status_;                        // RID_SQUAD_STATUS
    int32_t                  *squad_status_count_;                  // RID_SQUAD_STATUS_COUNT
    int32_t                  *order_pending_count_;                 // RID_STRAT_ORDER_PENDING_COUNT
    int32_t                  *order_staging_count_;                 // RID_STRAT_ORDER_STAGING_COUNT
    double                   *net_peer_horizon_;                    // RID_NET_PEER_HORIZON
    double                   *net_peer_horizon_pending_;            // RID_NET_PEER_HORIZON_PENDING
    int32_t                  *build_placement_id_;                  // RID_BUILD_PLACEMENT_ID
    uint32_t                 *bldg_footprint_passable_save_slot_;   // RID_BLDG_FOOTPRINT_PASSABLE_SAVE_SLOT
    int32_t                  *advisor_phase_;                       // RID_STRAT_ADVISOR_PHASE
    int32_t                  *floating_msg_queue_active_;           // RID_STRAT_FLOATING_MSG_QUEUE_ACTIVE
    job_result_entry         *path_job_result_;                     // RID_STRAT_PATH_JOB_RESULT_TABLE
    uint8_t                  *game_mode_;                           // RID_GAME_MODE
    int32_t                  *tutorial_build_type_filter_;          // RID_TUTORIAL_BUILD_TYPE_FILTER
    uint32_t                 *tutorial_forced_bldg_selection_;      // RID_TUTORIAL_FORCED_BLDG_SELECTION
    uint8_t                  *tutorial_hq_attack_scenario_done_;    // RID_TUTORIAL_HQ_ATTACK_SCENARIO_DONE
    int32_t                  *tutorial_pending_build_placement_id_; // RID_TUTORIAL_PENDING_BUILD_PLACEMENT_ID
    int32_t                  *tutorial_rmb_limit_flag_;             // RID_TUTORIAL_RMB_LIMIT_FLAG
    int32_t                  *tutorial_reset_slot_0050a678_;        // RID_TUTORIAL_RESET_SLOT_0050A678
    int32_t                  *dlg_state_flags_;                     // RID_DLG_STATE_FLAGS
    uint8_t                  *player_control_mask_;                 // RID_PLAYER_CONTROL_MASK
    int32_t                  *gfx_ui_color_;                        // RID_GFX_UI_COLORS_RGB
    int32_t                  *ui_race_sel_pending_gfx_idx_;         // RID_UI_RACE_SEL_PENDING_GFX_IDX
    ui_fade_transition_state *ui_fade_transition_;                  // RID_UI_FADE_TRANSITION
    void                    **ui_menu_async_callback_a_;            // RID_UI_MENU_ASYNC_CALLBACK_A
    widget_list             **ui_menu_widget_list_;                 // RID_UI_MENU_WIDGET_LIST
    ui_widget                *ui_tutorial_hint_widget_;             // RID_UI_TUTORIAL_HINT_WIDGET
    ui_widget                *ui_wgt_tutorial_welcome_;             // RID_UI_WGT_TUTORIAL_WELCOME
    ui_widget                *ui_wgt_menu_screen_title_;            // RID_UI_WGT_MENU_SCREEN_TITLE
    ui_widget                *ui_outcome_dlg_title_widget_;         // RID_UI_OUTCOME_DLG_TITLE_WIDGET
    ui_widget                *ui_outcome_dlg_message_widget_;       // RID_UI_OUTCOME_DLG_MESSAGE_WIDGET
    ui_widget                *ui_wgt_frame_menu_panel_;             // RID_UI_WGT_FRAME_MENU_PANEL
    uint8_t                  *injected_map_planet_slot_;            // RID_STRAT_INJECTED_MAP_PLANET_SLOT
    int32_t                  *view_size_mode_;                      // RID_VIEW_SIZE_MODE
    int32_t                  *view_size_mode_save_;                 // RID_VIEW_SIZE_MODE_SAVE
    map_header               *current_map_data_;                    // RID_CURRENT_MAP_DATA

    // SIM-RESID-IF re-close (2026-08-31) -- appended at the tail, per the same positional-swap-
    // avoidance precedent as the SIM1-G2/G3/G4 and SIM-RESID-IF groups above.
    int32_t        *current_system_mut_;             // CurrentSystem
    int32_t        *planet_index_mut_;               // G_PLANET_INDEX
    double         *game_clock_mut_;                 // _G_LLM_STRAT_GAME_CLOCK
    double         *game_time_delta_mut_;            // GAME_TIME_DELTA
    int16_t        *player_side_mut_;                // PlayerSide
    uint16_t       *local_player_slot_mut_;          // _G_LLM_STRAT_LOCAL_PLAYER_SLOT
    int32_t        *player_race_mut_;                // _G_LLM_STRAT_PLAYER_RACE
    int32_t        *sim_active_mut_;                 // _G_LLM_STRAT_SIM_ACTIVE
    int32_t        *mp_ally_victory_rule_flag_mut_;  // _G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG
    int32_t        *system_lost_msg_shown_flag_mut_; // _G_LLM_STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG
    uint8_t        *ui_bldg_tab_select_blocked_mut_; // _G_LLM_STRAT_UI_BLDG_TAB_SELECT_BLOCKED
    uint8_t        *bldg_completion_slot_count_mut_; // _G_LLM_STRAT_BLDG_COMPLETION_SLOT_COUNT
    uint32_t       *width_m_mut_;                    // width_m
    uint32_t       *height_m_mut_;                   // height_m
    int32_t        *tutorial_step_mut_;              // _G_LLM_GAME_TUTORIAL_STEP
    int32_t        *ai_enabled_mut_;                 // _G_LLM_STRAT_AI_ENABLED
    int32_t        *ui_panel_fallback_table_mut_;    // _G_LLM_STRAT_UI_PANEL_FALLBACK_TABLE
    cfg_planet     *cfg_planets_mut_;                // Planets
    cfg_invention  *cfg_inventions_mut_;             // Progress
    cfg_project    *cfg_projects_mut_;               // Projects
    cfg_upgrade    *cfg_upgrades_mut_;               // Upgrades
    int32_t        *system_define_index_mut_;        // System (raw int table, 0x8c stride)
    double         *advisor_next_time_;              // _G_LLM_STRAT_ADVISOR_NEXT_TIME
    uint8_t        *mouse_buttons_prev_;             // _G_LLM_MOUSE_BUTTONS_PREV
    char           *land_dmp_scratch_;               // _G_LLM_STRAT_LAND_DMP_PATH_SCRATCH
    uint8_t        *fog_of_war_base_;                // RID_FOG_OF_WAR, whole region
    uint8_t        *map_objects_base_;               // RID_MAP_OBJECTS, whole region
    uint8_t        *map_object_table_base_;          // RID_MAP_OBJECT_TABLE, whole region
    uint8_t        *map_halfres_grid_base_;          // RID_MAP_HALFRES_GRID, whole region
    int32_t        *death_anim_table_mut_;           // RID_STRAT_DEATH_ANIM_TABLE, the PRESERVE-BUG target
    wchar_t        *scenario_planet_name_w_;         // RID_STRAT_SCENARIO_PLANET_NAME_W, address escape
    const wchar_t **text_ptrs_mut_;                  // RID_G_TEXT_PTRS, write half of sim_view::text_ptrs
    player_desc    *player_desc_slots_mut_;          // RID_PLAYERS, write half of sim_view::player_desc_slots
    // LT1 lib_trans (2026-09-02) -- see the accessor comments above.
    llm_map_region **region_list_head_mut_;        // RID_MAP_REGION_LIST_HEAD, write half
    llm_map_region **region_pool_free_head_;       // RID_MAP_REGION_POOL_FREE_HEAD
    llm_map_region **region_by_index_;             // RID_MAP_REGION_BY_INDEX, 4096 slots
    int32_t         *region_alloc_counter_;        // RID_COUNTER, starts at 1
    int32_t         *last_map_index_;              // RID_G_LAST_MAP_INDEX, live-node count
    void           **ui_menu_async_callback_base_; // RID_UI_MENU_ASYNC_CALLBACK, base slot
    uint8_t         *ui_menu_state_mut_;           // RID_UI_MENU_STATE
    int32_t         *quit_teardown_forced_flag_;   // RID_GAME_QUIT_TEARDOWN_FORCED_FLAG
    uint8_t         *snd_ambient_by_planet_base_;  // RID_SND_AMBIENT_BY_PLANET, whole-region escape
    uint8_t         *bldg_cell_grid_;              // RID_STRAT_BLDG_CELL_GRID (byte[100][8][8])
    int32_t         *bldg_cell_grid_row_shift_;    // RID_STRAT_BLDG_CELL_GRID_ROW_SHIFT (int32[100])
    // SIM1-H (2026-09-10) -- see the accessor comments above.
    cfg_building *cfg_buildings_mut_;          // RID_BUILDING, write half of sim_view::cfg_buildings
    uint32_t     *region_coord_wrap_mask_mut_; // RID_MAP_REGION_COORD_WRAP_MASK, write half
    // SIM1-H wave 2 (2026-09-10) -- see the accessor comments above.
    uint32_t          *region_merge_threshold_mut_;   // RID_MAP_REGION_MERGE_THRESHOLD, write half
    uint8_t           *region_route_step_deltas_mut_; // RID_MAP_REGION_ROUTE_STEP_DELTAS, write half
    uint8_t           *region_route_step_delta_wrap_; // RID_MAP_REGION_ROUTE_STEP_DELTA_WRAP
    llm_map_bfs_entry *region_flood_path_queue_;      // RID_PATH, whole-region escape
};

// The pair, returned together so a translation cannot end up reading one binding and writing
// another. Note there is no default constructor: `sim_state s{}` does not compile anywhere,
// including inside this module, because sim_store has no accessible default constructor.
struct sim_state {
    sim_view  read;
    sim_store own;
};

// Binds both halves from the region registry. BY VALUE and re-resolved per call -- see the header
// comment; `net_selftest.exe statetest` is what proves the follow.
sim_state state();

// ---- read helpers -----------------------------------------------------------------------------
//
// Free functions over sim_view, matching ai_state.h's shape. They exist so an index expression
// appears once rather than at every call site, and so the row stride is stated in one place.

inline const unit &unit_of(const sim_view &v, uint32_t player, int32_t index) {
    return v.units[player * v.caps.units + index];
}
inline const building &building_of(const sim_view &v, uint32_t player, int32_t index) {
    return v.buildings[player * v.caps.buildings + index];
}
inline const player_data  &player_of(const sim_view &v, uint32_t player) { return v.players[player]; }
inline const unit_storage &storage_of(const sim_view &v, uint32_t player, int32_t slot) {
    return v.storage[player * v.caps.storage + slot];
}

// cfg_unit::weapons is FOUR (id, enabled) byte pairs rendered flat as uint8_t[8] -- see
// CFG_UNIT_WEAPON_SLOTS. These two are the only sanctioned way to read it.
inline uint8_t cfg_unit_weapon_id(const cfg_unit &u, int32_t slot) {
    return u.weapons[slot * 2 + 0];
}
inline uint8_t cfg_unit_weapon_enabled(const cfg_unit &u, int32_t slot) {
    return u.weapons[slot * 2 + 1];
}

// SIM1F (2026-08-16). cfg_upgrade::objects is FOUR-BYTE-STRIDE (16 entries of
// cfg_t_upgrade_object_index, flattened as uint8_t[64] -- `SHL EAX,0x2` in the .asm, NOT the
// byte-pair shape cfg_unit_weapon_id() above documents for cfg_unit::weapons). Little-endian dword
// read via byte composition (no reinterpret_cast, per house rule). game_HandleUpgrade is the sole
// reader.
inline uint32_t cfg_upgrade_object_id(const cfg_upgrade &up, int32_t slot) {
    const uint8_t *p = up.objects + slot * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The torus-wrap masks: `tile & mask` wraps a signed tile coordinate onto the map, valid because
// map dimensions are powers of two. Named accessors rather than `v.geom->width_mask` at call sites
// so a translation reads as domain vocabulary, matching cfg_unit_weapon_id() above.
inline uint32_t map_width_mask(const sim_view &v) { return v.geom->width_mask; }
inline uint32_t map_height_mask(const sim_view &v) { return v.geom->height_mask; }

// The occupancy plane, COLUMN-major: byte offset is (tile_x << 11) | (tile_y << 3), i.e. 8 bytes per
// tile with tile_x as the outer index -- same layout ai_state.h's tile_at() reads. Unchecked, like
// every other accessor here: callers that need wrap must mask through map_width_mask/map_height_mask
// first, matching the original (this closure's one reader does not).
inline const tile_object &tile_at(const sim_view &v, int32_t tile_x, int32_t tile_y) {
    return v.tile_objects[(tile_x << 8) | tile_y];
}

// SIM1B. `progress[player][row]` -- see the `player_progress`/PROGRESS_ROW_COUNT comments above.
// Matches ai_state.h's own progress_of() shape for its independent binding of the same region.
inline const player_progress &progress_of(const sim_view &v, int32_t player, int32_t row) {
    return v.progress[player * PROGRESS_ROW_COUNT + row];
}

// SIM1B. `player_resources[player][resource_id]` -- the per-player holdings row.
inline int32_t player_resource_of(const sim_view &v, int32_t player, int32_t resource_id) {
    return v.player_resources[player * PLAYER_RESOURCE_SLOTS + resource_id];
}


} // namespace mh::sim
