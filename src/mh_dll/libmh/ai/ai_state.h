//
// ai/ai_state.h -- the strategic AI's state view (RI-AI / AI0).
//
// The AI cluster is ~215 functions (tools/data/ai_migration.json). Every one of them reads game
// state and a minority write some. This header is the ONE place that says which is which, and it
// says it in a way the COMPILER enforces rather than a lint: what the AI may read is reachable only
// through `ai_view`, whose every member is a pointer-to-const, and what the AI may write is
// reachable only through `ai_store`, which deliberately contains nothing but the AI's own store.
//
// SO THE ARCHITECTURE RULE BECOMES A TYPE ERROR. P2-RULES' R2 says "AI writes orders, not sim
// state"; `units` and `buildings` appear in `ai_view` and in NO writable structure, so
// `st.read.units[i].energy = 0` does not compile. That is the whole point of the split, and
// mh_nettest/ai_const_negative.cpp is the checked-in proof that it holds (driven by
// tools/check_const_view.py, which also proves the same file compiles once the write is removed --
// a negative test that cannot pass for the wrong reason).
//
// `const` HERE MEANS "YOU MAY NOT WRITE THROUGH IT", NOT "THIS DOES NOT CHANGE". Easy to get
// backwards and expensive if you do: the sim writes player_data at 157 sites during the same tick
// the AI reads it (the AI closure). A view that snapshotted or cached contents would pass every
// test in an idle world and desync in a busy one. Nothing here copies a byte.
//
// AND NOTHING HERE CACHES AN ADDRESS EITHER. `state()` returns BY VALUE and re-resolves every
// pointer from the region registry on every call -- five loads, inlined away in practice. The
// alternative (a `static` bound once, which is what mh::orders does) would go stale the moment a
// region is rebased under RI-STATE/ST2, and it would go stale SILENTLY: the view would keep serving
// the abandoned .bss and every consumer would agree with it. `net_selftest.exe statetest` rebases
// RID_PLAYER_DATA and asserts the view follows, which is a check that can only exist because there
// is nothing to go stale.
//
// LAW 1 AND LAW 3. The record layouts come from addr/mh_structs.gen.h (static_assert'd against
// Ghidra, so a retype breaks the build instead of corrupting a save) and the addresses from the
// region registry via mh::state::ptr. No byte offsets and no literal VAs appear in this module.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h" // SB-BIND T2: the derived per-player row capacities

namespace mh::ai {

using player_data      = mh::game::mh_game_player_data;
using unit             = mh::game::mh_map_object_unit;
using building         = mh::game::mh_map_object_building;
using target_entry     = mh::game::mh_llm_strat_ai_target_entry;
using engage_candidate = mh::game::mh_llm_strat_ai_engage_candidate;
// The two CONFIG tables (Building[100] @0xd9ec80, Weapon[32] @0xc3a520): read-only per-TYPE
// records loaded from the game's cfg files, as opposed to the per-INSTANCE rosters above. They
// entered the view for batch A layer 1 -- engage_partition_turret_candidates classifies a target
// by Building[id].type, and turret_threat_rescan reads Building[id].weapon_id -> Weapon[].range_max.
using cfg_building = mh::game::mh_cfg_final_struct_Building;
using cfg_weapon   = mh::game::mh_cfg_final_struct_Weapon;
// The third CONFIG table, alongside Building[]/Weapon[] above: per-UNIT-TYPE records
// (Unit[100] @0xe4a098, stride 0x23f). It entered the view for batch A layer 2 --
// target_list_scan_visible_enemies classifies a candidate by Unit[proto_id].type.
using cfg_unit = mh::game::mh_cfg_final_struct_Unit;
// Batch A layer 2. The MAP planes the AI's spiral scanners walk, plus the AI's own attack-candidate
// scratch and the (dx, dy) spiral table every ring scan is driven from.
using tile_object      = mh::game::mh_map_tile_object_data;
using spiral_offset    = mh::game::mh_llm_strat_ai_spiral_offset;
using attack_candidate = mh::game::mh_llm_strat_ai_attack_candidate;
// Batch A layer 3. The GLOBAL scan-target scratch record (stride 0x16) -- a different array and a
// different record from player_data::ai_target_list's llm_strat_ai_target_entry (stride 0x14), and
// the two are appended to by two similarly-named functions. See scan_targets / scan_target_count.
using scan_target_entry = mh::game::mh_llm_strat_ai_scan_target_entry;
// Batch A layer 4. The per-player PRODUCTION-BUILDING records (405 bytes each, 8 per player) and the
// cfg parser's Unit SECTION record -- of which only `.total`, the number of unit types the loaded cfg
// defines, is read here. The section record is a different object from the cfg_unit TABLE above:
// `Unit[100]` @0xe4a098 holds the per-type records, `UNIT` @0xe5f638 holds the parse state that
// counted them.
using production       = mh::game::mh_map_object_production;
using cfg_unit_section = mh::game::mh_cfg_static_struct_Unit;
// Batch A layer 5. The BUILD-SITE candidate record (stride 0x10) the three site scanners append to,
// and the RESOURCE-DEPOSIT record the third of them walks. The second lives INSIDE player_data
// (player_data::ai_resource_sites / ai_resource_site_count), so it needs no view member of its own --
// the alias exists only so a translation can name the type it is indexing.
using site_candidate = mh::game::mh_llm_strat_ai_site_candidate;
using resource_site  = mh::game::mh_llm_strat_ai_resource_site;
// Batch C layer 0. player_data::ai_groups[32] -- the AI's task forces. The alias exists so a
// translation can name the record it indexes; the array itself lives inside player_data and so
// needs no view member of its own (same arrangement as ai_resource_sites above).
using unit_group = mh::game::mh_llm_strat_ai_unit_group;
// Batch C layer 1. _G_LLM_STRAT_PLAYERS[8] @0x00cff060, stride 0x740 -- the SESSION player record,
// a different table from player_data (@0x00e6dec0, stride 0x288fc) and from game::g::Players
// (@0x00e587e9, stride 0x34). Only `status_flags` is read here, and only bit 0x2.
using player_profile = mh::game::mh_llm_strat_player_profile;
// Batch B layer 2. The INVENTION/tech table and its per-player acquisition state -- two different
// objects whose Ghidra labels differ only in case, which is worth stating once here rather than
// tripping over per use:
//   `Progress` @0xe162e4 is cfg::final::struct::Invention[300], the CFG table (one row per tech,
//                        shared by every player). Stride 0x67.
//   `progress` @0xc38750 is game::progress[8][300], the PER-PLAYER state of those same rows,
//                        3 bytes each. Row p of player q is progress[q * PROGRESS_ROW_COUNT + p].
// Plus the two remaining cfg SECTION records, the siblings of cfg_unit_section above: only `.total`
// is read from either, and each is the last dword of its record.
using cfg_invention        = mh::game::mh_cfg_final_struct_Invention;
using player_progress      = mh::game::mh_game_progress;
using cfg_building_section = mh::game::mh_cfg_static_struct_Building;
using cfg_progress_section = mh::game::mh_cfg_static_struct_Progress;
// Batch B layer 3. The per-player UNIT HOUSING ledger, [MAX_PLAYERS] of 0x40 bytes. Four unit
// classes (vehicles / soldiers / planes / helis), each with a live-count column, a LATCHED capacity
// column and an accumulator the capacity-granting buildings feed. llm_strat_ai_count_unit_build_sources
// tests `used_* >= cap_prev_*` -- the LATCHED column, not the accumulator -- and it is NOT the AI's
// own store: the sim writes it, the AI only reads it.
using housing_stats = mh::game::mh_llm_strat_unit_housing_stats;
// Batch B layer 3 (the two planners of 2026-08-02-2250). The per-player STORAGE ledger,
// [MAX_PLAYERS] of 0x58 bytes: cap_accum[10] is the per-resource capacity the sim re-accumulates
// each step and cap_prev[10] is that same figure LATCHED at step start. The AI reads cap_prev only.
// NOTE THE PAIRING TRAP: this is the CAPACITY side, and the HOLDINGS side is a completely separate
// array (`player_resources` below) with a different stride -- 0x58 here, 40 there.
using storage_stats = mh::game::mh_llm_strat_storage_stats;
// Batch B layer 3 (the two planners of 2026-08-03). The per-player POPULATION ledger,
// [MAX_PLAYERS] of 0x34 bytes. llm_strat_ai_rebalance_building_workers reads exactly three of its
// columns -- pop_total, human_in_field and housing_prev -- and writes none of them: the sim owns
// this record, the AI only reads it. Note `human` (+0xc, the idle-at-base count) is NOT what the
// rebalance uses; it derives its own idle figure as pop_total - human_in_field.
using pop_stats = mh::game::mh_llm_strat_pop_stats;
// Batch B layer 6. One coarse cell of the map's resource plane (map::resources, 0x10 bytes):
// `short value[8]` indexed by cfg resource id. The AI only ever looks at ids 1..4; llm_strat_spawn_
// ai_base's map sweep is the one place that sums all eight.
using map_resources = mh::game::mh_map_resources;
// Batch B layer 4, the mine portfolio. The three PARALLEL scratch tables the collect pass fills,
// row `i` of each describing the i-th mine it found, plus the sampling kernel it reads.
using mine_quality     = mh::game::mh_llm_strat_ai_mine_quality;     // near/mid/far ring counts
using mine_yield       = mh::game::mh_llm_strat_ai_mine_yield;       // by_resource[8], [0] a total
using mine_kernel_cell = mh::game::mh_llm_strat_ai_mine_kernel_cell; // dx, dy, weight
// RI-AI batch C layer 3 (2026-08-06). unit_storage[player][slot]: b_index (1-based buildings[]
// index, 0 = empty), docked_count (signed compare in the original, JGE not JAE), docked_units[50],
// occupancy/door_mutex_unit/door_waiter_count (unread by anything in this cluster).
using unit_storage_slot = mh::game::mh_map_object_unit_storage;
// The LARGE-branch spread-point scratch table llm_strat_ai_group_reposition_members fills: x, y
// (int16_t tile coords) and used (int16_t, 0/1).
using spread_tile = mh::game::mh_llm_strat_ai_spread_tile;
// RI-AI batch C layer 4 (2026-08-06). map::object::turret[8][32] @0xcc0fe0, stride 0x37 -- the
// per-player, per-turret-slot state llm_strat_ai_group_classify_target_object's BUILDING arm reads
// (counter_ref/counter_target_slot at +0x16/+0x18) to find what a turret is itself attacking. Only
// those two fields are named; the rest of the 0x37-byte record is unresolved and stays padding.
using turret = mh::game::mh_map_object_turret;

inline constexpr int32_t MAX_PLAYERS = 8; // player_data[8], units[8][..], buildings[8][..]
// SB-BIND T2 (2026-09-06): these are the STOCK values and nothing indexes with them any more.
// Production index expressions read the DERIVED capacities off the view/store (`v.caps.units`,
// `caps_.buildings`, ...), which follow whatever the host bound; these survive only as the offline
// fixtures' default and as the documentation of the stock strides. Adding a new production use of
// one is a regression -- it would pin that one site to 100 while every other site followed a
// cap-raised host, which is worse than uniform staleness. See state/roster_caps.h.
inline constexpr int32_t UNITS_PER_PLAYER     = mh::state::STOCK_ROSTER_CAPS.units;     // stride 0x5b04 / 0xe9
inline constexpr int32_t BUILDINGS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.buildings; // stride 0x6aa4 / 0x111
inline constexpr int32_t BUILDING_TYPE_COUNT  = 100;                                    // Building[100], stride 0x842
inline constexpr int32_t WEAPON_TYPE_COUNT    = 32;                                     // Weapon[32],    stride 0x16c
inline constexpr int32_t UNIT_TYPE_COUNT      = 100;                                    // Unit[100],     stride 0x23f
// Batch A layer 4. productions is [8][8] with a 0xca8 per-player stride over 0x195-byte records, and
// the INNER index is buildings[player][i].sub_id -- a production building's slot number, NOT its
// index in the building roster. Nothing range-checks sub_id, here or in the original.
inline constexpr int32_t PRODUCTIONS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.productions;
// RI-AI batch C layer 3 (2026-08-06). unit_storage[8][25], 0xf4-byte rows -- confirmed against the
// region's declared size (48800 = 8*25*0xf4). Slot 0 is never read by any known consumer.
inline constexpr int32_t UNIT_STORAGE_SLOTS_PER_PLAYER = 25;
// RI-AI batch C layer 4 (2026-08-06). turrets[8][32], stride 0x37 -- 14080 / 8 players / 0x37.
inline constexpr int32_t TURRET_SLOTS_PER_PLAYER = 32;
// Batch B layer 2. Progress[300] / progress[8][300]; the per-player row stride is 0x384 == 300 * 3,
// spelled by llm_strat_ai_plan_unit_training as IMUL EDI,ECX,0x384 at 0x004e6dad.
inline constexpr int32_t PROGRESS_ROW_COUNT = 300;
// The width of player_data::ai_train_queued_by_ai_unit, i.e. how many distinct cfg::enum::ai::E_UNIT
// roles the AI tallies. It is also the extent of plan_unit_training's per-role stack scratch and the
// bound of its role-picking loop (CMP EAX,0xc at 0x004e6e7a and 0x004e6d81).
inline constexpr int32_t AI_UNIT_ROLE_COUNT = 12;
// `player_resources` is int[MAX_PLAYERS][RESOURCE_SLOTS_PER_PLAYER], row-major, indexed by the cfg
// resource id. The per-player stride of 40 bytes is derived twice and from two different functions:
// llm_strat_ai_storage_capacity_short_and_cap_check computes player*40 at 0x004e3891-0x004e3899, and
// llm_unit_can_afford_resources / llm_bldg_can_afford_resources index it as
// `player_id * 0x28 + Unit[t].resource[i].id * 4` (0x004e23f6 / 0x004e2458). 10 slots because the
// region is 320 bytes for 8 players, and because llm_strat_storage_stats -- the capacity ledger the
// AI divides by this -- carries its two columns as int[10]. ONLY ids 1..4 are ever touched by the
// AI: every loop over it runs `for (d = 1; d < 5; ++d)`.
inline constexpr int32_t RESOURCE_SLOTS_PER_PLAYER = 10;
inline constexpr int32_t RESOURCE_ID_FIRST         = 1;
inline constexpr int32_t RESOURCE_ID_LAST          = 4;

// cfg::enum::E_BUILDING members, spelled out because the generated header renders an enum field as
// its underlying scalar. The originals compare Building[id].type against these two literals.
inline constexpr uint8_t BLDG_TYPE_A_TURRET = 5;
inline constexpr uint8_t BLDG_TYPE_H_TURRET = 25;
// Batch A layer 3. The other two race-paired members llm_strat_ai_scan_construction_sites compares
// Building[id].type against. EVERY such comparison in the original is selected by
// player_data::is_alien_race -- 0 picks the H_ member, non-zero the A_ one -- so translate the pair
// with the race_* helpers below rather than inlining the literals twice.
inline constexpr uint8_t BLDG_TYPE_A_RELAY = 15;
inline constexpr uint8_t BLDG_TYPE_H_RELAY = 35;
inline constexpr uint8_t BLDG_TYPE_A_MINE  = 2;
inline constexpr uint8_t BLDG_TYPE_H_MINE  = 22;
// Batch B layer 0. The THIRD race-paired member of the same family, 6 / 0x1a, which
// llm_strat_ai_bldg_register_visible_building tests alongside the mine and turret pairs at
// 0x004db378-0x004db385. Ghidra's own decompile of those three comparisons renders them
// A_MINE/H_MINE, A_TURRET/H_TURRET and A_MOTHER/H_MOTHER, which is where the name comes from --
// this pair had no constant in this header before, and nothing else in the DLL reads it yet.
inline constexpr uint8_t BLDG_TYPE_A_MOTHER = 6;
inline constexpr uint8_t BLDG_TYPE_H_MOTHER = 26;
// RI-AI batch B layer 3 (2026-08-06). llm_strat_ai_calc_power_supply_ratio's own generator-set test
// (0x004e39ac-0x004e39ce) -- unlike the four race_*_type() helpers below, this one is NOT gated on
// is_alien_race: it tests A_PLANT, H_PLANT, A_MOTHER AND H_MOTHER unconditionally (any building of
// any of these four types, either race, counts as a power generator), so no race_plant_type() helper
// is added. 3 / 0x17 follow the same alien/+20-for-human pattern the other four pairs above show.
inline constexpr uint8_t BLDG_TYPE_A_PLANT = 3;
inline constexpr uint8_t BLDG_TYPE_H_PLANT = 23;

inline uint8_t race_turret_type(int32_t is_alien_race) {
    return is_alien_race ? BLDG_TYPE_A_TURRET : BLDG_TYPE_H_TURRET;
}
inline uint8_t race_relay_type(int32_t is_alien_race) {
    return is_alien_race ? BLDG_TYPE_A_RELAY : BLDG_TYPE_H_RELAY;
}
inline uint8_t race_mine_type(int32_t is_alien_race) {
    return is_alien_race ? BLDG_TYPE_A_MINE : BLDG_TYPE_H_MINE;
}
inline uint8_t race_mother_type(int32_t is_alien_race) {
    return is_alien_race ? BLDG_TYPE_A_MOTHER : BLDG_TYPE_H_MOTHER;
}

// Batch B layer 4. The two pairs llm_strat_ai_bldg_production_type_dispatch tests alongside the
// mine pair at 0x004e7d73-0x004e7d80 and 0x004e7dae-0x004e7dbb. The names are read off the game's
// OWN cfg::enum::E_BUILDING in Ghidra (A_BARRAKS 7 / A_GARAGE 8 / H_BARRACKS 27 / H_GARAGE 28) --
// including the alien half's spelling, which is the data's, not a typo introduced here.
inline constexpr uint8_t BLDG_TYPE_A_BARRACKS = 7;
inline constexpr uint8_t BLDG_TYPE_H_BARRACKS = 27;
inline constexpr uint8_t BLDG_TYPE_A_GARAGE   = 8;
inline constexpr uint8_t BLDG_TYPE_H_GARAGE   = 28;

// Batch C layer 0. The two cfg::enum::E_UNIT members llm_strat_ai_notify_object_removed tests
// Unit[proto].type against at 0x004db75b / 0x004db768 before doing anything else: a removed HELI
// MOTHER makes the whole notification a no-op. Ghidra's own decompile renders the two literals
// A_HELI_MOTHER / H_HELI_MOTHER, which is where the names come from. NOT a race_* pair helper:
// the original tests BOTH unconditionally, with no is_alien_race selector.
// Batch C layer 0. player_data::ai_groups[] slots 0..4 are the five seed groups
// llm_strat_spawn_ai_base creates for every AI player, and llm_strat_ai_notify_object_removed
// refuses to disband a group below this index even when its member_count has reached zero
// (CMP ESI,0x5 / JL at 0x004db8ee -- a SIGNED compare). The bound is a literal at that site, not a
// derived value; nothing else in the AI cluster reads it as a constant.
inline constexpr int32_t AI_SEED_GROUP_COUNT = 5;

// Batch C layer 1. llm_strat_player_profile::status_flags bit 0x2 -- ALIVE / has presence, the
// SIM-OWNED bit (the struct's own field comment carries the full ownership split). It is the first
// gate of llm_strat_ai_unit_group_tick (TEST byte ptr [EAX + 0xcff060],0x2 at 0x004eb302), read as a
// BYTE off the low end of the dword. Named here rather than reusing the decompile's bare `ALIVE`
// because two other flag families in this header would collide with that name.
inline constexpr uint32_t PLAYER_STATUS_ALIVE = 0x2u;

// Batch C layer 1. player_data::ai_phase_flags bit 0x4 -- the unit-group task machine's own
// phase-enable bit, the second gate of llm_strat_ai_unit_group_tick (0x004eb325). The other two bits
// of that mask select the construction planner (0x1) and the unit-training planner (0x2); see the
// field comment on ai_phase_flags.
inline constexpr uint8_t AI_PHASE_UNIT_GROUPS = 0x4u;
// Its two siblings, named for the first time 2026-08-07 (RI-AI batch E): the same mask's
// other two bits, gating llm_strat_ai_player_tick's planner phases at 0x004e8d87 (0x1 ->
// plan_construction + scan_bldg_repair_upgrade) and 0x004e8d4f (0x2 -> plan_unit_training). They
// were already documented on the field and used as bare literals; the constants exist so the three
// bits of one mask are named in one place rather than one-named-and-two-literal.
inline constexpr uint8_t AI_PHASE_CONSTRUCTION  = 0x1u;
inline constexpr uint8_t AI_PHASE_UNIT_TRAINING = 0x2u;

// Batch C layer 1. The two llm_strat_ai_unit_group::task_code values llm_strat_ai_unit_group_tick's
// disband pass accepts (CMP word ptr [..+0xe7e452],0x9 / 0x14 at 0x004eb351 / 0x004eb35b). They are
// live task codes of the group's slot-0 task, and the names come from
// llm_strat_ai_group_task_activate's OWN switch @0x004eb131 -- case 9 dispatches to
// llm_strat_ai_group_task_recruit_from_storage and case 0x14 to llm_strat_ai_group_task_disband.
// (Read off that dispatch rather than guessed: an earlier draft of this comment had the pair
// reversed and called 0x14 `wait`, which is case 7.) The pass removes an EMPTY group only under one
// of these two -- an empty group parked on any other task code is left alone, which is what makes
// it a narrow reaper rather than a general GC.
inline constexpr int16_t AI_GROUP_TASK_RECRUIT_FROM_STORAGE = 9;
inline constexpr int16_t AI_GROUP_TASK_DISBAND              = 0x14;

inline constexpr int32_t UNIT_TYPE_A_HELI_MOTHER = 0x13;
inline constexpr int32_t UNIT_TYPE_H_HELI_MOTHER = 0x14;

// Batch A layer 3. The rest of the cfg::enum::E_UNIT_TYPE members llm_strat_ai_unit_squad_firepower_
// value's multiplier ladder tests (0x004d34d2-0x004d34ed). The VALUES come from that ladder's own
// literals in the disassembly; the NAMES from Ghidra's enum rendering of the same comparisons, and
// the two agree member-for-member (A_HELI_MOTHER = 0x13 was already pinned above, independently, by
// llm_strat_ai_notify_object_removed).
//
// The two infantry runs are CONTIGUOUS and parallel (2..5 alien, 7..0xa human), which is why the
// original tests them with a binary ladder rather than a table -- and why 6 is NOT in either run and
// falls through to the x1 default like every unlisted type.
inline constexpr int32_t UNIT_TYPE_A_INFANTRY_2 = 2;
inline constexpr int32_t UNIT_TYPE_A_INFANTRY_3 = 3;
inline constexpr int32_t UNIT_TYPE_A_INFANTRY_4 = 4;
inline constexpr int32_t UNIT_TYPE_A_INFANTRY_5 = 5;
inline constexpr int32_t UNIT_TYPE_H_INFANTRY_2 = 7;
inline constexpr int32_t UNIT_TYPE_H_INFANTRY_3 = 8;
inline constexpr int32_t UNIT_TYPE_H_INFANTRY_4 = 9;
inline constexpr int32_t UNIT_TYPE_H_INFANTRY_5 = 0xa;
// The far end of the heli band the firepower scorer zeroes. [A_HELI_MOTHER 0x13, H_HELI_CARGO 0x18]
// is tested as a RANGE by the original, so the two ends are what matter, not the members between.
inline constexpr int32_t UNIT_TYPE_H_HELI_CARGO = 0x18;

inline uint8_t race_barracks_type(int32_t is_alien_race) {
    return is_alien_race ? BLDG_TYPE_A_BARRACKS : BLDG_TYPE_H_BARRACKS;
}
inline uint8_t race_garage_type(int32_t is_alien_race) {
    return is_alien_race ? BLDG_TYPE_A_GARAGE : BLDG_TYPE_H_GARAGE;
}

// player_data::ai_tile_flags_grid bit stamped by llm_strat_ai_grid_stamp_threat_ring and cleared by
// llm_strat_ai_grid_clear_threat_bit -- "an enemy turret reaches this tile". Named here only so the
// grid's meaning is visible at the call site; neither helper takes it as a parameter.
inline constexpr uint8_t TILE_FLAG_TURRET_THREAT = 0x40;

// ---- batch A layer 6 ----------------------------------------------------------------------------
// The LOW FIVE BITS of that same byte are the influence/flood DISTANCE from the player's own area:
// llm_strat_ai_grid_flood_step propagates them outward (marking with 0x20 and clearing it in a second
// pass) and llm_strat_ai_grid_stamp_seeds re-seeds them while preserving 0xc0.
// llm_strat_ai_grid_stencil_all_near_unthreatened accepts a cell only at distance <= 3, i.e. "within
// three flood steps of my own area". The comparison is UNSIGNED (`CMP AL,3 ; JA`), which is exactly
// equivalent here because the value has already been masked to 0..31.
inline constexpr uint8_t TILE_INFLUENCE_DIST_MASK = 0x1f;
inline constexpr uint8_t TILE_INFLUENCE_DIST_NEAR = 3;

// _G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH is llm_strat_ai_engage_candidate[256]. The extent is
// established by ADJACENCY (0xc00 bytes to _G_LLM_STRAT_AI_SITE_CANDIDATES @0xfb59b0, exactly 256
// records) and is NOT enforced anywhere: llm_strat_ai_engage_candidate_add appends with no cap
// check at all. So this is a bound the original RELIES on rather than one it checks, and a
// reimplementation must not invent a rejection branch the original does not have.
inline constexpr int32_t ENGAGE_SCRATCH_CAP = 256;

// ---- batch A layer 2 constants ------------------------------------------------------------------
//
// The map planes are PHYSICALLY [256][256] whatever the map's own width/height are, and they are
// COLUMN-MAJOR: the original computes a tile's byte offset as (x << 11) | (y << 3), i.e. X is the
// outer index. Everything that walks them wraps first through map_width_mask / map_height_mask.
inline constexpr int32_t MAP_PLANE_DIM        = 256;
inline constexpr int32_t FOG_PLAYERS_PER_TILE = 8; // fog_of_war::visible_by_count is byte[256][256][8]

// _G_LLM_STRAT_AI_ATTACK_CANDIDATES extent, established by ADJACENCY (0x804 bytes to
// _G_LLM_STRAT_AI_TARGET_SCRATCH_COUNT = 128 records + 4 spare) and NOT enforced anywhere:
// llm_strat_ai_attack_candidate_add appends with no cap check at all, exactly like the engage
// scratch. A reimplementation must not invent the rejection branch the original does not have.
inline constexpr int32_t ATTACK_SCRATCH_CAP = 128;

// The two AI.SCR attack-wave tables are int[32] / float[32]; only the first
// *attack_milestone_count (AI.SCR nMaxAttacks, 13 in the shipped script) entries are meaningful.
inline constexpr int32_t ATTACK_MILESTONE_CAP = 32;

// ---- batch A layer 3 constants ------------------------------------------------------------------
//
// _G_LLM_STRAT_AI_SCAN_TARGETS extent, by ADJACENCY (0xb000 bytes to
// _G_LLM_STRAT_AI_ATTACK_CANDIDATES = exactly 2048 records of 0x16) and, like every other AI
// scratch here, NOT enforced: llm_strat_ai_scan_target_list_add appends with no cap check.
// llm_strat_ai_build_target_list works a SECOND BANK at record 1024, so the array is used as two
// halves of 1024 rather than one run of 2048.
inline constexpr int32_t SCAN_TARGET_CAP  = 2048;
inline constexpr int32_t SCAN_TARGET_BANK = 1024;
// _G_LLM_STRAT_AI_EXPAND_SITE_X/_Y extent, by adjacency (0x400 each). Also unenforced.
inline constexpr int32_t EXPAND_SITE_CAP = 1024;
// The footprint every build-placement test walks out of cfg Building::area, and the two `passable`
// cell values that make a footprint unbuildable. The original passes the 10x10 span as literals
// rather than reading Building::width/height, so a building smaller than its area box still has its
// whole box tested -- do not "fix" that.
inline constexpr int32_t FOOTPRINT_SPAN     = 10;
inline constexpr uint8_t PASSABLE_BLOCKED_A = 0;
inline constexpr uint8_t PASSABLE_BLOCKED_B = 6;

// The packed target ref's roster selector. TWO tests exist over the same field and BOTH are live in
// the original -- see the target_ref field comment in addr/mh_structs.gen.h. They agree on class
// nibbles 2 (unit) and 4 (building) and disagree on nibble 0, so they are kept as two named
// predicates rather than unified into one "is this a building" helper.
inline constexpr uint32_t REF_OWNER_MASK = 0x0fu;
inline constexpr uint32_t REF_UNIT_BITS  = 0xa0u; // (ref & 0xa0) == 0 -> BUILDING  [add / is_alive / sort / commit]
inline constexpr uint32_t REF_BLDG_BIT   = 0x40u; // (ref & 0x40) != 0 -> BUILDING  [partition / survivability scan]

// ---- batch A layer 5 constants ------------------------------------------------------------------
//
// _G_LLM_STRAT_AI_SITE_CANDIDATES extent, by adjacency (0x10000 bytes = 4096 records of 0x10).
//
// AND THIS ONE IS THE TRAP OF THE LAYER. The two grid scanners DO compare the count against 0x1000,
// which reads like the cap the other four scratches lack -- it is not one. The comparison is `==`
// (not `>=`), it runs AFTER the increment, and the branch it takes leaves the INNER loop only: the
// outer x loop keeps going and the next inner pass appends at 0x1001, where the test can never match
// again. So the array overruns exactly as freely as the others, just with one wasted column of work
// at the boundary. Reproduce the shape, do not "fix" it into a cap and do not hoist it to the outer
// loop. (Confirmed from the listing at 0x004e6683 / 0x004e683a, both jumping to the outer INC.)
inline constexpr int32_t SITE_CANDIDATE_CAP      = 4096;
inline constexpr int32_t SITE_CANDIDATE_BREAK_AT = 0x1000;
// Batch B layer 4. The mine scratch tables' PHYSICAL row count -- 128 + 384 + 1024 bytes of
// otherwise-unreferenced .bss divided by 4 + 12 + 32. It is NOT a bound the original enforces: the
// collect walk runs over the buildings roster (100 slots), so a player with more than 32 mines
// writes past every one of the three tables. Reproduced, not fixed; see ai_mine_rebalance.h.
inline constexpr int32_t MINE_SCRATCH_ROWS = 32;
// The 5x5 sampling kernel's cell count, the inner loop bound at 0x004e325a (CMP ECX,0x19).
inline constexpr int32_t MINE_KERNEL_CELLS = 25;
// The COARSE resource plane is a fixed 64x64 whatever the map's own extents are; the estimator
// derives its wrap moduli from map_width / map_height divided by four instead (SAR-based signed /4
// at 0x004e3107-0x004e312f), so a 256x256 map wraps at 64 and a smaller one wraps sooner.
inline constexpr int32_t COARSE_PLANE_SIDE = 64;
// The `kind` column each scanner stamps. Write-only in the image -- no reader exists -- but it is
// part of the bytes the shadow oracle compares, so it must be exact.
inline constexpr int32_t SITE_KIND_GRID_FIT = 0;
inline constexpr int32_t SITE_KIND_RESOURCE = 1;
// The resource scanner walks the shared spiral table out to RADIUS 5, i.e. it consumes
// spiral_ring_cell_counts[5] entries. That count had its own symbol
// (_G_LLM_STRAT_AI_BUILD_SPIRAL_SCAN_COUNT @0xfb32c4) until 2026-08-01, when it turned out to be
// element 5 of the uint[128] at 0xfb32b0 that llm_strat_ai_spiral_table_init writes wholesale -- not
// a global of its own. Read it through the view; do not declare a region for it.
inline constexpr int32_t SITE_SCAN_SPIRAL_RADIUS = 5;
// player_data::ai_resource_sites stores COARSE quarter-tile coordinates; every consumer converts with
// tile = coarse * 4 + 2 (llm_strat_sort_sites_by_dist uses a bare << 2 -- the two disagree by the
// half-cell offset, and that is the original's behaviour, not a bug to unify).
inline constexpr int32_t RESOURCE_SITE_COORD_SCALE = 4;
inline constexpr int32_t RESOURCE_SITE_COORD_BIAS  = 2;
// llm_strat_ai_resource_site::status. The scanner acts only on OPEN sites and stamps INVALID when no
// tile in the whole radius-5 disc can host the building.
inline constexpr uint16_t RESOURCE_SITE_STATUS_OPEN    = 0;
inline constexpr uint16_t RESOURCE_SITE_STATUS_INVALID = 0xffffu;

inline bool     ref_is_building_by_a0(uint32_t ref) { return (ref & REF_UNIT_BITS) == 0; }
inline bool     ref_is_building_by_40(uint32_t ref) { return (ref & REF_BLDG_BIT) != 0; }
inline uint32_t ref_owner(uint32_t ref) { return ref & REF_OWNER_MASK; }

// ---- the READ view ----------------------------------------------------------------------------
//
// Every member is a pointer-to-const. Adding a non-const member here is not a style slip, it is a
// hole in R2 -- put it in ai_store below and accept that the write becomes visible and countable.
struct ai_view {
    const player_data *players;   // [MAX_PLAYERS]
    const unit        *units;     // [MAX_PLAYERS][UNITS_PER_PLAYER], row-major
    const building    *buildings; // [MAX_PLAYERS][BUILDINGS_PER_PLAYER], row-major

    // SB-BIND T2: the per-player ROW CAPACITIES, derived from the sizes the host bound. Defaults to
    // the stock values so a hand-built fixture view is unchanged; state() overwrites it. Use these
    // in index expressions -- never the constants below -- or a cap-raised host resolves our reads
    // into the wrong player's row (docs/state-boundary.md D6.3).
    mh::state::roster_caps  caps{mh::state::STOCK_ROSTER_CAPS};
    const engage_candidate *engage_scratch;       // [ENGAGE_SCRATCH_CAP]
    const int32_t          *engage_scratch_count; // live entries in engage_scratch
    const cfg_building     *cfg_buildings;        // [BUILDING_TYPE_COUNT], indexed by building_id
    const cfg_weapon       *cfg_weapons;          // [WEAPON_TYPE_COUNT], indexed by cfg_building::weapon_id
    const cfg_unit         *cfg_units;            // [UNIT_TYPE_COUNT],   indexed by unit::unit_proto_id
    // The map is a TORUS of map_width x map_height tiles. Pointers, not values, for the same reason
    // everything else here is: ReadMapFile writes them at load and nothing may cache them.
    const int32_t *map_width;
    const int32_t *map_height;
    // Highest CLAIMED player index + 1 -- the loop bound every per-player AI sweep uses. NOT
    // MAX_PLAYERS: it is 0 until a player is initialised and rises as slots are claimed.
    const int32_t *active_player_count;
    // ---- batch C layer 1 ----
    // The SESSION player records, [MAX_PLAYERS]. Read for `status_flags & PLAYER_STATUS_ALIVE`
    // only. Deliberately NOT folded into `players` above: that one is player_data, a different
    // table with a different stride, and the AI reads both.
    const player_profile *strat_players;

    // ---- batch A layer 2 ----
    // The two map planes the tile scanners read. Both are [256][256] and COLUMN-major -- use
    // tile_at() / fog_visible_count_at() rather than indexing them by hand.
    const tile_object *tile_objects;
    const uint8_t     *fog_visible_by_count; // fog_of_war::visible_by_count, byte[256][256][8]
    // The torus wrap MASKS (extent - 1), NOT the extents. Separate globals from map_width /
    // map_height above, written by the same map load; the ring scanners AND with these, which only
    // works because map dimensions are powers of two.
    const uint32_t *map_width_mask;
    const uint32_t *map_height_mask;
    // The precomputed spiral: offsets[0 .. ring_cell_counts[r]) is every tile within radius r of an
    // anchor, as signed (dx, dy) byte pairs. The count is CUMULATIVE from the table start, so a
    // "ring" scan is really a disc scan bounded by radius. Read-only after llm_strat_ai_spiral_table_init.
    const spiral_offset *spiral_offsets;
    const uint32_t      *spiral_ring_cell_counts; // [128]
    // The grid module's shared wrap-mask scratch, packed ((x_extent-1) << 8) | (y_extent-1) -- X in
    // the HIGH byte, matching the grid's own (x << 8) | y indexing. (This comment said the opposite
    // when the region was first declared on 2026-08-01; the batch's own translator caught it against
    // the raw byte stores at 0x004b4c4c/0x004b4c55, and the field comment on
    // player_data::ai_tile_flags_grid had it right all along.) READ side; the write side is in
    // ai_store, because llm_strat_ai_grid_stamp_threat_ring stores it before use.
    const uint32_t *grid_wrap_mask;
    // The AI's per-tick attack-candidate scratch and its live count (reset-then-fill, carries
    // nothing between ticks -- the same discipline as the engage scratch).
    const attack_candidate *attack_candidates; // [ATTACK_SCRATCH_CAP]
    const int32_t          *attack_candidate_count;
    // AI.SCR configuration: the attack-wave milestone tables and the two scalars that bound/tune them.
    const int32_t *attack_strength_table; // [ATTACK_MILESTONE_CAP]
    const float   *attack_time_table;     // [ATTACK_MILESTONE_CAP]
    const int32_t *attack_milestone_count;
    const float   *repair_ratio;
    // ---- RI-AI batch D / AI1D (2026-08-28) ----
    // The three AI.SCR SCHEDULER PERIODS llm_strat_ai_players_tick advances its per-player clocks by
    // -- _G_LLM_STRAT_AI_STRATEGY_PERIOD @0x0066931c (FLD @0x004dba0e),
    // _G_LLM_STRAT_AI_TACTIC_PERIOD @0x00669320 (@0x004dba7c) and
    // _G_LLM_STRAT_AI_MOVE_PERIOD @0x00669324 (@0x004dbaba). Floats, read-only after the AI.SCR
    // parse; each is the step of one catch-up loop, so a wrong one changes how OFTEN a phase runs
    // rather than what it does -- which no per-call oracle would notice.
    const float *ai_strategy_period;
    const float *ai_tactic_period;
    const float *ai_move_period;
    // Batch C layer 2. Four more read-only tuning scalars from the same .data run, named in EN v223
    // (findings 2026-08-05-1221-1..4). Each has exactly ONE referring function in the image, which
    // is both why they were unnamed and why the derivation is unusually tight -- see the EOL
    // comments in Ghidra and the notes at the top of ai_group_form.h.
    const int32_t *patrol_group_size;       // (=3)      form_patrol: gate x3, headcount, sub_code
    const int32_t *patrol_group_max;        // (=2)      form_patrol: cap on concurrent goal-5 groups
    const int32_t *patrol_wander_bounces;   // (=100000) form_patrol: wander sub_code (truncates!)
    const int32_t *surplus_group_min_pool4; // (=5)      form_surplus_from_pool4: pool floor
    // Named 2026-08-05 (session 1541), same shape as the four above: one referring function
    // (llm_strat_ai_group_home_guard_replenish), already named in Ghidra from an earlier session.
    const int32_t *home_guard_target_pct; // (=80, 0x50) home_guard_replenish: housing-quota percent

    // ---- batch A layer 3 ----
    // The GLOBAL scan-target scratch: reset-then-fill per pass, same discipline as the engage and
    // attack scratches, and appended to with NO cap check. NOT player_data::ai_target_list.
    const scan_target_entry *scan_targets; // [SCAN_TARGET_CAP]
    const int32_t           *scan_target_count;
    // The AI's expansion-site candidate list: two PARALLEL byte arrays of tile coordinates plus
    // their count, rebuilt from zero on every llm_strat_ai_scan_construction_sites call. Two arrays
    // rather than one of pairs because that is what the original is, and the shadow site compares
    // them byte for byte.
    const uint8_t *expand_site_x; // [EXPAND_SITE_CAP]
    const uint8_t *expand_site_y; // [EXPAND_SITE_CAP]
    const int32_t *expand_site_count;
    // The terrain/occupancy plane every build-placement footprint test walks: byte[256][256],
    // COLUMN-major like the other planes, index (x << 8) | y. A cell reading 0 or 6 is what
    // llm_scan_masked_table_for_empty_cell rejects a footprint on.
    const uint8_t *passable;
    // AI.SCR expansion tuning, all int and all read only by the two expansion functions.
    const int32_t *expand_min_ticks;          // vs player_data::ai_expand_gate_value (UNSIGNED)
    const int32_t *expand_min_building_count; // vs buildings[player][0].index (SIGNED)
    const int32_t *relay_to_mine_ratio_pct;   // expand only while turrets*100 <= others*this
    // The four diagonal-quadrant offsets at distance 2, indexed by the `category` argument.
    const int32_t *quadrant_dx2; // [4]
    const int32_t *quadrant_dy2; // [4]

    // ---- batch A layer 4 ----
    // The production-building records, [MAX_PLAYERS][PRODUCTIONS_PER_PLAYER] row-major. Use
    // production_of(); the inner index is a building's sub_id, not its roster index.
    const production *productions;
    // The cfg parser's Unit section record. Only `.total` is read (see cfg_unit_section above).
    const cfg_unit_section *cfg_unit_sec;

    // ---- batch A layer 5 ----
    // The SHARED build-site candidate list all three site scanners append to, and its live count.
    // It is the one AI scratch here that is NOT reset-then-fill: no scanner zeroes the count, they
    // all append to whatever is already there, and llm_strat_ai_plan_construction is what clears it
    // between planning passes. So a scanner's output depends on the count it INHERITS, which is why
    // its region has to be declared (and restored) by every site that reaches one of them.
    const site_candidate *site_candidates; // [SITE_CANDIDATE_CAP], and see SITE_CANDIDATE_BREAK_AT
    const int32_t        *site_candidate_count;

    // ---- batch B layer 0 ----
    // _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG. A single dword, READ at exactly one place in the
    // image (0x004db45c) and written nowhere the AI cluster can see, which is why it had no region
    // until 2026-08-02. While it is non-zero, every damage report re-stamps the aggressor hostile
    // unconditionally; while it is zero the stamp is made only when the relation is still 0.
    const int32_t *foreign_bldg_change_flag;

    // ---- batch B layer 2 ----
    // _G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS, int[4], indexed by the RING's resource column (0..3,
    // i.e. resource ids 1..4). Read twice per ring cell by llm_strat_ai_resource_spend_rate_update
    // -- once for the numerator term and once for the denominator term -- and by nothing else.
    const int32_t *spend_weights;

    // The invention/tech pair and the two remaining cfg section counts -- see the `using` block at
    // the top of this header for why `cfg_inventions` and `progress` are two different things.
    // llm_strat_ai_plan_unit_training is the only reader of all four so far.
    const cfg_invention   *cfg_inventions; // Progress[300] @0xe162e4, the CFG table
    const player_progress *progress;       // progress[8][300] @0xc38750, the per-player state
    // `.total` only, and each is an INCLUSIVE loop bound in the original (JBE, not JC):
    // building_sec->total bounds the ai_train_source_state CLEAR loop even though that array is
    // indexed by UNIT type -- a copied bound, reproduced rather than corrected.
    const cfg_building_section *cfg_building_sec; // BUILDING section record @0xe5c9e4
    const cfg_progress_section *cfg_progress_sec; // PROGRESS section record @0xe5c0d8

    // ---- batch B layer 3 ----
    // The per-player housing ledger, [MAX_PLAYERS]. Read only by llm_strat_ai_count_unit_build_sources
    // so far, which uses it to veto a whole unit CLASS when it is at capacity.
    const housing_stats *unit_housing;
    // _G_LLM_STRAT_AI_TRAIN_QUEUE_PER_UNIT_CAP. AI.SCR-configured uint; how many kind-0 (train)
    // entries for ONE unit type may sit in the build queue at once. Compared UNSIGNED, and only
    // while player_data::ai_start_units_remaining is zero -- the scripted opening army is exempt.
    const uint32_t *train_queue_per_unit_cap;
    // _G_LLM_STRAT_AI_CFG_PROMO_ADD / _SUB, the AI.SCR scalars `nPromoAdd` / `nPromoSub` (both 1 in
    // the shipped script). Read ONLY by llm_strat_ai_bldg_queue_handle_recruit_state, and THE
    // ARITHMETIC IS INVERTED RELATIVE TO THE KEY NAMES: promo_add is SUBTRACTED from
    // player_data::ai_promo_credit on the free path (0x004e8153) and promo_sub is ADDED on the
    // paying one (0x004e81cd). Named for the script keys because that is the strongest evidence
    // there is (the keyword table at 0x006693b4 points straight at both addresses); the behaviour
    // lives in this comment rather than in the name.
    const int32_t *promo_add;
    const int32_t *promo_sub;

    // ---- batch B layer 3, the storage/mine planners (2026-08-02-2250) ----
    // The two halves of the AI's silo test, and getting them the wrong way round is the error the
    // function's own plate used to make: `storage` holds CAPACITY (cap_prev[d]) and is the
    // NUMERATOR; `player_resources` holds HOLDINGS and is the DIVISOR, flushed to 1.0 when it is
    // +-0.0. Ratio below `silo_ratio` == "I am running out of somewhere to put resource d".
    const storage_stats *storage;          // [MAX_PLAYERS]
    const int32_t       *player_resources; // [MAX_PLAYERS][RESOURCE_SLOTS_PER_PLAYER], row-major
    const float         *silo_ratio;       // AI.SCR fSiloRatio, 1.1 in the shipped image
    // _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT. A four-byte SCRATCH global, not a config value
    // despite the name's shape: llm_strat_ai_plan_mine_construction computes it, hands it to
    // llm_strat_ai_mine_portfolio_rebalance (which reads it at 0x004e35ab) and reads it back itself.
    // It is BOTH a view member and a store member for exactly that reason -- see ai_store.
    const int32_t *mine_rebalance_min_count;

    // ---- batch B layer 3, the turret / worker planners (2026-08-03) ----
    // The per-player population ledger, [MAX_PLAYERS]. Read-only to the AI; see `pop_stats` above.
    const pop_stats *pop;
    // Four AI.SCR scalars whose ONLY reader in the whole image is
    // llm_strat_ai_rebalance_building_workers (whole-image dword scan, 2026-08-03; each also has
    // exactly one more occurrence, its own row in the keyword table at 0x00669490).
    //
    // TWO OF THE FOUR ARE NAMED THE OPPOSITE OF WHAT THEY DO, and the names are the shipped script's,
    // not ours:
    //   unemployed_min   -- `nUnemployedMin`, int 50. A FLOOR on the idle reserve, compared SIGNED.
    //   unemployed_ratio -- `fUnemployedRatio`, 0.2f. reserve = floor((pop_total - human_in_field) * this).
    //   max_fuck_ratio   -- `fMaxFuckRatio` (verbatim dev name), 0.3f. Despite `MAX` it is a FLOOR:
    //                       ai_labor_utilization is flushed to 0.0 when this EXCEEDS it.
    //   extra_space      -- `fExtraSpace`, 1.1f. A HOUSING-HEADROOM threshold, not the building-spacing
    //                       multiplier the AI.SCR notes called it until 2026-08-03: the function returns 1
    //                       when this > housing_prev / pop_total.
    const int32_t *unemployed_min;
    const float   *unemployed_ratio;
    const float   *max_fuck_ratio;
    const float   *extra_space;
    // The two per-building connectivity scratch planes, byte[BUILDINGS_PER_PLAYER] each, indexed by
    // building SLOT (1..99; slot 0 is never written). Both are filled by the game's
    // llm_strat_bldg_connectivity_flood_fill through an OUT-POINTER, which is why the state matrix
    // credits that function with no writes at all -- the out-pointer trap. _BASE is
    // the full graph; _TRIAL is the same flood fill re-run with ONE candidate building excluded, and
    // a slot where the two disagree is a building the candidate was the only connection for.
    // PRIVATE to llm_strat_ai_plan_turret_upgrade: nothing else in the image references either.
    const uint8_t *bldg_connectivity_base;
    const uint8_t *bldg_connectivity_trial;

    // ---- batch B layers 4 and 6, the mine-worth gate (2026-08-03) ----
    // The map's per-COARSE-CELL resource plane, map::resources[64][64] @0xc28530: 64 rows of 64
    // 16-byte cells, each cell `short value[8]` keyed by cfg resource id. X SELECTS THE ROW -- every
    // reader spells the index as `(x >> 2) << 10 | (y >> 2) << 4`, so it is resources[x][y], not
    // [y][x]; the 1024-byte shift is the row stride and the 16-byte one is the cell. The `>> 2` is
    // the FINE-to-coarse conversion: callers hold fine coords and the plane is quarter resolution.
    // Read UNSIGNED (MOVZX) at every site even though Ghidra types the field `short`.
    const map_resources *resources; // [64][64], row-major with X as the row
    // _G_LLM_STRAT_AI_CFG_MINE_WORTH, the AI.SCR scalar `nMineWorth`. Bound live and never folded:
    // the image ships 3000 and init/AI.SCR overwrites it with 5000, so a constant would be wrong in
    // every real game. Compared UNSIGNED by both readers.
    const int32_t *mine_worth;
    // _G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS, {4, 2, 2, 1}. ONE-BASED IN THE ORIGINAL'S ADDRESSING:
    // every reader computes `[id * 4 + 0x6616a8]` for id 1..4, and 0x6616a8 is the unrelated live
    // counter _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT sitting in front of the table. So this pointer is
    // the id-1 base -- use resource_value_weight() below rather than indexing it directly.
    // The region registry reports size 0 for it, which is expected and not a gap: the state matrix
    // attributes every access to the 0x6616a8 base, so nothing measures this symbol's extent. Twenty
    // -odd view members are in the same position (_G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG among
    // them) and `ptr<T>` only ever needs the base.
    const int32_t *resource_value_weights; // [4], element [0] == resource id 1

    // ---- batch B layer 4, the mine portfolio rebalance (2026-08-05) ----
    // Three PARALLEL 32-row tables plus the sampling kernel. Row `i` of all three describes the
    // i-th mine llm_strat_ai_mine_portfolio_rebalance's collect pass found, in collect order.
    // NOTHING BOUNDS THE ROW INDEX against MINE_SCRATCH_ROWS -- the collect walk is driven by the
    // buildings roster, which is 100 slots deep. That overrun is the original's and is reproduced;
    // see ai_mine_rebalance.h.
    const int32_t      *mine_roster_index;   // [32] buildings[player][] slot, 0 == retired/empty
    const mine_quality *mine_quality_hist;   // [32] filled by calc_mine_yield_estimate's out-pointer
    const mine_yield   *mine_yield_estimate; // [32] ditto; [0] is a total, [1..4] per resource id
    // The 5x5 nearest-first sampling kernel, INITIALISED READ-ONLY DATA in the image: the centre
    // cell at weight 1.0, the 8 ring-1 cells at 0.5, the 16 ring-2 cells at 0.1. Bound live rather
    // than folded because the three bucket thresholds are derived from these values and a future
    // build could ship a different table.
    const mine_kernel_cell *mine_yield_kernel; // [25]
    // _G_LLM_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD, image value 10. The first of the five keep tests
    // in the retire pass; compared UNSIGNED against the summed per-resource percentage share.
    const int32_t *mine_low_share_pct_threshold;

    // ---- batch C layer 2, the 2026-08-05 slice ----
    // game::t::Player_s @0x00e58354 -- THE LOCAL player index the sim uses. A ushort, and it is a
    // THIRD player-indexing global, distinct from both `strat_players` above and from the injected
    // MP stack's _G_LLM_NET_LOCAL_PLAYER_SLOT (see the mh_addrs.gen.h entry, which carries the
    // 2026-07-27 renaming of the one that used to be confused with it).
    //
    // Read by exactly one function in this cluster, llm_strat_ai_unit_order_move_with_bump, and only
    // as the gate `player == PlayerSide` guarding its placement-preview + sound tail. THAT GATE IS
    // NOT A "THIS IS THE HUMAN" TEST UNDER --soak: the all-AI soak converts the host slot too, so
    // PlayerSide is an AI player there and the guarded tail DOES run. Do not read a clean armed run
    // as evidence that the tail is unreachable.
    const uint16_t *player_side;

    // ---- RI-AI batch C layer 3 (2026-08-06) ----
    // map::object::unit_storage[8][25], flat row-major: index as
    // `unit_storage[player * UNIT_STORAGE_SLOTS_PER_PLAYER + slot]`. Read-only in this cluster.
    const unit_storage_slot *unit_storage;
    // _G_LLM_STRAT_AI_REPOSITION_SMALL_GROUP_MAX, AI.SCR scalar. llm_strat_ai_group_reposition_members's
    // SMALL-vs-LARGE branch threshold (eligible_count < this -> SMALL), unsigned compare.
    const int32_t *reposition_small_group_max;
    // The LARGE-branch spread-point scratch table + its live count, paired.
    // _G_LLM_STRAT_AI_SPREAD_TILES[1024] / _G_LLM_STRAT_AI_SPREAD_TILE_COUNT.
    const spread_tile *spread_tiles;
    const int32_t     *spread_tile_count;
    // llm_strat_ai_group_task_attack_random_target's building-candidate scratch, reset-then-fill.
    // _G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST[256] / _COUNT.
    const int32_t *building_candidate_scratch_list;
    const int32_t *building_candidate_scratch_count;

    // ---- RI-AI batch C layer 3 (2026-08-06) -- llm_strat_ai_group_task_loiter_wander's two
    // AI.SCR-adjacent tunables. Both were untracked literals (auto DAT_) previously; named
    // + typed from the translator's declared_needs (see ai_group_task_movement.h). Sole referrer is
    // loiter_wander in both cases.
    //
    // Radian angle-scale factor, FMUL'd against the raw channel-2 RNG draw before the sin/cos pair.
    const double *loiter_angle_scale;
    // Jitter radius (tile units), FILD'd and multiplied against sin(angle)/cos(angle). Sits right
    // after _G_LLM_STRAT_AI_PATROL_WANDER_BOUNCES but is a SEPARATE scalar -- that region's declared
    // reach does not cover it.
    const int32_t *loiter_jitter_radius;

    // ---- RI-AI batch C layer 4 (2026-08-06) -- llm_strat_ai_group_classify_target_object's BUILDING
    // arm. [MAX_PLAYERS][TURRET_SLOTS_PER_PLAYER], row-major; use turret_of() rather than indexing by
    // hand. Region carried MF_SAVE|MF_PATCH|MF_HASH only until -- see mh_addrs.gen.h.
    const turret *turrets;

    // ---- RI-AI batch B layer 2 (2026-08-06) -- economy/opponent-relations/construction-plan slice.
    // _G_LLM_STRAT_AI_ENERGY_RATIO. AI.SCR fEnergyRatio -- despite the key's name this is the POWER
    // economy, not the HP-like ENERGY stat (the ENERGY-vs-POWER distinction). Was already
    // named+typed in Ghidra; had no region/view wiring until llm_strat_ai_plan_construction's
    // translation surfaced it as a declared_need.
    const float *energy_ratio;
    // AI.SCR nAttackOverKill / nMinAttackStr (the AI.SCR notes), both found+named as the
    // located consumer for llm_strat_ai_recompute_shortage_state's strong-enemy-threat gate: overkill
    // is an IMUL weight against the strongest opponent's total_unit_power; min_attack_strength is
    // compared against the player's own soldier_count+ground_count. Both read UNSIGNED at their sites.
    const int32_t *attack_overkill_pct;
    const int32_t *min_attack_str;

    // ---- RI-AI batch E (2026-08-07) -- llm_strat_ai_scr_parse's dispatch table ----
    // Base of a zero-terminated array of {int32_t type; void *target; char *name} records (12 bytes
    // each) mapping an AI.SCR keyword to the process-global tunable it feeds and how to parse it
    // (type 1 = int via atoi, 2 = float via strtod). scr_parse walks it with pointer arithmetic
    // exactly like the original (advance by 3 int32_t's per record, stop at a zero `type`) -- there
    // is no known record count, only the sentinel. Read-only to this cluster.
    const int32_t *ai_scr_keyword_table;
};

// REMOVED 2026-08-07: `group_member_scratch` and its GROUP_MEMBER_SCRATCH_STRIDE = 5 companion. The
// one function that read them (group_scratch_compute_centroid) was never AI -- it moved to
// sim/sim_group_scratch_centroid.h, which binds the region itself. The array is also no longer a
// hand-derived "flat 5-int32 row": it is _G_LLM_STRAT_GROUP_MOVE_SCRATCH,
// llm_strat_group_scratch_member[100], and the column that was being read by index arithmetic is
// the record's `unit_idx` field.

// The AI's value weight for cfg resource id 1..4. Spelled as a helper because the original's
// addressing is one-based off a neighbouring symbol and writing `weights[id]` at a call site would
// silently read _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT for id 0 and run one past the table for id 4.
inline int32_t resource_value_weight(const ai_view &v, int32_t resource_id) {
    return v.resource_value_weights[resource_id - RESOURCE_ID_FIRST];
}

// One coarse resource cell, from FINE coordinates. `(x >> 2) << 10 | (y >> 2) << 4` in the original.
inline const map_resources &resource_cell_at_fine(const ai_view &v, uint32_t fine_x,
                                                  uint32_t fine_y) {
    return v.resources[(fine_x >> 2) * 64u + (fine_y >> 2)];
}

// ---- the FIELD-SCOPED ROSTER WINDOW (RI-AI batch D / AI1D, 2026-08-28) --------------------------
//
// Batch D's writers put bytes INSIDE map_object_unit / map_object_building -- the sim's records,
// which ai_view deliberately publishes as pointer-to-const so that R2 ("the AI writes orders, not
// sim state") is a type error rather than a lint finding. D1 (docs/state-boundary.md) froze the
// layout, so those writes are ordinary stores at fixed offsets and a bit-identical translation is
// available; what is NOT available is a `unit *units` in ai_store, because that hands the AI every
// field of every unit and stops the const view catching a genuinely wrong AI write -- the one
// property AI0 was built for.
//
// So the hole is a CLASS with one accessor PER FIELD and no member that yields a record address.
// The only roster bytes an AI translation can reach are the seven named below; reaching any other
// field means naming `units_`/`buildings_`, which is private, so it is a COMPILE ERROR and not a
// review note. ai_const_negative.cpp's batch-D cases demonstrate that rather than asserting it.
//
// EVERY FIELD CARRIES ITS OWNERSHIP MEASUREMENT, and the measurements are NOT all the same. They
// come from an image-wide instruction scan of all 252238 instructions for the little-endian dword of
// each field's absolute address (2026-08-28) -- independent of Ghidra's reference manager, which is
// the founding false negative of this whole class. Writers only; reads are restricted by nothing here.
//
//   +0xd4 ai_group_next   AI-EXCLUSIVE. 5 write sites, ALL llm_strat_ai_*: group_member_unlink
//                         0x004d4958, group_member_link 0x004d4a7b / 0x004d4a9b / 0x004d4aba,
//                         notify_unit_lifecycle 0x004dbbbc / 0x004dbd49. No non-AI writer exists.
//   +0xd6 ai_group_prev   AI-EXCLUSIVE. Writes at unlink 0x004d49bb, link 0x004d4a8b / 0x004d4ac3,
//                         notify_unit_lifecycle 0x004dbbc5 / 0x004dbd52.
//   +0xd8 ai_group_index  AI-ASSIGNED, SIM-CLEARED -- the asymmetry IS the measurement, so do not
//                         read this row as "AI-owned". The AI is the only writer that ASSIGNS a
//                         group (link 0x004d4a4e, group_remove 0x004d4f98, unlink 0x004d49fc writes
//                         the 0xffff sentinel, notify_unit_lifecycle 0x004dbbae / 0x004dbbce /
//                         0x004dbd5b). FOUR non-AI writers CLEAR it on death/teardown --
//                         remove_from_map 0x004874d8, teardown_mapped 0x004876d3, on_destroyed
//                         0x004879b8, teardown 0x00487d61, all `MOV ...,0` -- and TWO more treat the
//                         same word as a BITFIELD: llm_unit_status_bit_set 0x0044b0dd (OR) and
//                         llm_unit_status_bit_clear 0x0044b132 (AND). That last pair is unexplained
//                         and is queued as a Ghidra finding, not resolved here.
//   +0xdc incoming_threat_damage / +0xde committed_weapon_damage_est / +0xe2 the engagement word /
//   building +0x10f incoming_damage_tally
//                         The COMMIT half of the AI's commit/release overkill-avoidance protocol.
//                         The non-AI co-writer is llm_strat_target_release_ref -- 0x004dad31 ADD,
//                         0x004daddd SUB, 0x004dacaa, 0x004dad4e AND 0x7ffe, 0x004dadf1, 0x004dacfa,
//                         0x004dada9 -- i.e. the RELEASE half of the same protocol, plus
//                         map_CreateBuilding 0x0046245e which only initialises the building tally to
//                         0. There is no third writer. NOT sole ownership, and it is recorded as
//                         what it is rather than rounded up.
//
// The window is reached as `own.roster` and its accessors are const member functions returning
// non-const references, because every translated body takes `const ai_store &own` -- the STORE is
// const, the pointed-to bytes are not, exactly as ai_store's raw pointers already work.
class ai_roster_window {
public:
    // Bound by ai/ai_state.cpp (live) and by the aitest fixture (offline). Takes the two roster
    // bases and nothing else, so there is no way to point it at a single record.
    void bind(unit *units, building *buildings) {
        units_     = units;
        buildings_ = buildings;
        // SB-BIND T2: the row capacities come along with the bases, in the SAME call, so neither
        // caller can bind one and forget the other -- a store holding live pointers with stock
        // capacities would index a cap-raised roster into the wrong row and nothing would say so.
        caps_ = mh::state::live_roster_caps();
    }

    // ---- the AI's own doubly-linked group member list, physically inside map_object_unit --------
    // 0 is BOTH "no next" and "no prev" (the list is 1-based over unit ids); 0xffff in
    // ai_group_index is "in no group". Bounds are unchecked here for the same reason sim_store's
    // accessors are: the originals index straight off the byte they read.
    uint16_t &ai_group_next(uint32_t player, int32_t index) const { return unit_rec(player, index).ai_group_next; }
    uint16_t &ai_group_prev(uint32_t player, int32_t index) const { return unit_rec(player, index).ai_group_prev; }
    uint16_t &ai_group_index(uint32_t player, int32_t index) const { return unit_rec(player, index).ai_group_index; }

    // ---- the overkill-avoidance accounting ------------------------------------------------------
    int16_t &incoming_threat_damage(uint32_t player, int32_t index) const {
        return unit_rec(player, index).incoming_threat_damage;
    }
    int16_t &committed_weapon_damage_est(uint32_t player, int32_t index) const {
        return unit_rec(player, index).committed_weapon_damage_est;
    }
    int16_t &bldg_incoming_damage_tally(uint32_t player, int32_t index) const {
        return bldg_rec(player, index).incoming_damage_tally;
    }

    // ---- the engagement/order-status WORD, and why it is two named operations and not a handle --
    //
    // engagement_flags (+0xe2) and order_status_flags (+0xe3) are adjacent bytes that the original
    // writes with ONE 16-bit store, and the high byte belongs to the ORDER subsystem (bit 0x40 is
    // order-notify-pending, which O4B re-homed to mh::orders). Publishing `order_status_flags` as a
    // reference would hand the AI that bit for arbitrary use; publishing a `uint16_t &` over two
    // uint8_t members would be a type pun. So the window exposes exactly the two word operations the
    // batch-D bodies perform, and nothing else here touches +0xe3.
    //
    // engage_commit_set -- `OR word ptr [..+0xe2],0x8001` @0x004d5617 (commit_attack_order) and
    // @0x004d5730 (_alt). Reproduced as two byte ORs: OR-ing disjoint bit sets into two adjacent
    // bytes writes exactly the bytes the 16-bit OR writes, and the sim is single-threaded, so the
    // memory result is identical. Pinned by the aitest oracle rather than left as an assertion.
    void engage_commit_set(uint32_t player, int32_t index) const {
        unit &u              = unit_rec(player, index);
        u.engagement_flags   = (uint8_t)(u.engagement_flags | 0x01u);
        u.order_status_flags = (uint8_t)(u.order_status_flags | 0x80u);
    }
    // engage_status_word_clear -- `MOV word ptr [..+0xe2],0x0` @0x004dbb9c (notify_unit_lifecycle,
    // case 1). This ALSO zeroes order_status_flags, bit 0x40 included. That is the original's
    // behaviour on a unit-lifecycle event, not an accident of the merged store, and it is preserved.
    void engage_status_word_clear(uint32_t player, int32_t index) const {
        unit &u              = unit_rec(player, index);
        u.engagement_flags   = 0u;
        u.order_status_flags = 0u;
    }

private:
    unit &unit_rec(uint32_t player, int32_t index) const {
        return units_[player * caps_.units + index];
    }
    building &bldg_rec(uint32_t player, int32_t index) const {
        return buildings_[player * caps_.buildings + index];
    }
    unit     *units_     = nullptr;
    building *buildings_ = nullptr;
    // SB-BIND T2: the derived row capacities this store indexes with. Set by the binder alongside
    // the pointers; stock default so an offline fixture that never sets it is unchanged.
    mh::state::roster_caps caps_{mh::state::STOCK_ROSTER_CAPS};
};

// ---- the WRITE channel --------------------------------------------------------------------------
//
// The AI's OWN store and nothing else. `players` is here because player_data IS the AI's store
// (AI2's scope note: "player_data is NOT an R2 violation -- it is the AI's own store"); the rosters
// are deliberately absent, which is what makes an AI roster write a compile error.
//
// `players` is a RAW POINTER, not a reference to an array of 8, on purpose:
// llm_strat_ai_init_build_candidate_priorities writes four slots through `players[p + 1]`, which
// for p == 7 lands past the end of the array. That overrun is load-bearing -- seven readers read it
// back through the identical aliasing -- so the type must not make it impossible to express.
struct ai_store {
    player_data      *players;              // [MAX_PLAYERS] (+ the deliberate [p+1] overrun)
    engage_candidate *engage_scratch;       // [ENGAGE_SCRATCH_CAP] -- shared TRANSIENT, reset-then-fill
    int32_t          *engage_scratch_count; //                         (carries nothing between AI ticks)
    // ---- batch A layer 2 ----
    attack_candidate *attack_candidates;      // [ATTACK_SCRATCH_CAP] -- the same reset-then-fill transient
    int32_t          *attack_candidate_count; //                        discipline as the engage scratch
    // The grid module's wrap-mask scratch. It is WRITABLE here because
    // llm_strat_ai_grid_stamp_threat_ring stores its own arguments into it and reads them back a few
    // instructions later; six functions do the same and none of them carries a value across a call.
    uint32_t *grid_wrap_mask;
    // ---- batch A layer 3 ----
    scan_target_entry *scan_targets; // [SCAN_TARGET_CAP] -- reset-then-fill transient
    int32_t           *scan_target_count;
    uint8_t           *expand_site_x; // [EXPAND_SITE_CAP] -- rebuilt on every call
    uint8_t           *expand_site_y;
    int32_t           *expand_site_count;
    // ---- batch A layer 5 ----
    // APPEND-ONLY, not reset-then-fill -- see the read-side comment. The scanners never write the
    // count except to increment it.
    site_candidate *site_candidates; // [SITE_CANDIDATE_CAP]
    int32_t        *site_candidate_count;
    // ---- batch B layer 3, the mine planner (2026-08-02-2250) ----
    // _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT, writable. It is an ARGUMENT CHANNEL rather than
    // durable state: llm_strat_ai_plan_mine_construction stores it and then calls
    // llm_strat_ai_mine_portfolio_rebalance, which reads it. It is in ai_store rather than being a
    // local because the call between the two goes through the ORIGINAL binary, which can only see
    // the global.
    int32_t *mine_rebalance_min_count;
    // ---- batch B layer 3, the turret planner (2026-08-03) ----
    // The two connectivity planes, writable, for the same ARGUMENT-CHANNEL reason as the scratch
    // above and not because our code stores into them: llm_strat_ai_plan_turret_upgrade hands each
    // one's ADDRESS to the game's llm_strat_bldg_connectivity_flood_fill, which does the filling.
    // A `const uint8_t *` would not convert to the call's `uint8_t *` (TACT1-P C6, 2026-09-04: was
    // `void *`, same const problem either way), and casting the const away at the call site is
    // exactly the hole ai_view exists to close.
    uint8_t *bldg_connectivity_base;
    uint8_t *bldg_connectivity_trial;
    // ---- batch B layer 4, the mine portfolio rebalance (2026-08-05) ----
    // The three parallel scratch tables, writable. `mine_roster_index` and `mine_yield_estimate` are
    // written by our own code (the collect pass and the per-mine re-sum); `mine_quality_hist` is an
    // ARGUMENT CHANNEL like the connectivity planes above -- we hand row `i`'s ADDRESS to
    // calc_mine_yield_estimate, which does the filling. All three are pure per-call scratch: every
    // row the rebalance reads it wrote earlier in the same call.
    int32_t      *mine_roster_index;
    mine_quality *mine_quality_hist;
    mine_yield   *mine_yield_estimate;

    // ---- RI-AI batch C layer 3 (2026-08-06) ----
    // llm_strat_ai_group_reposition_members's SMALL-branch member-id scratch, reset-then-fill.
    // _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST[100] (dword unit-id entries) / _COUNT.
    int32_t *group_relocation_scratch_list;
    int32_t *group_relocation_scratch_count;
    // The LARGE-branch spread-point table, writable (our own fill pass) + its count.
    spread_tile *spread_tiles;
    int32_t     *spread_tile_count;
    // llm_strat_ai_group_task_attack_random_target's own reset-to-0 before its four collect calls.
    int32_t *building_candidate_scratch_count;
    // RI-AI batch C layer 4 (2026-08-06). The writable half of the pair above:
    // llm_strat_ai_group_collect_buildings_of_types APPENDS building_index entries into the list
    // itself, not just the count. ai_view only ever carried the const read side.
    int32_t *building_candidate_scratch_list;

    // ---- RI-AI batch E (2026-08-07) ----
    // llm_strat_ai_spiral_table_init's own three globals -- ai_view already carried the read side of
    // the first two (spiral_offsets/spiral_ring_cell_counts, read-only after init everywhere else in
    // the cluster); this is the one function that WRITES them, plus the third sibling
    // (ai_tile_spiral_cell_count) that had no region at all previously.
    spiral_offset *spiral_offsets;
    uint32_t      *spiral_ring_cell_counts;
    int32_t       *ai_tile_spiral_cell_count;

    // llm_strat_ai_start_hq_attack_scenario's three globals. Not shadow-armed (see
    // ai_hq_attack_scenario.cpp), but real store members so the translation compiles against a
    // region instead of a raw VA -- Law 1.
    int32_t  *ai_enabled;                       // process-global master AI-on latch (0/1)
    uint32_t *ai_attack_hq_unit_id;             // map_t_unit_id of the scripted HQ-attacker
    bool     *tutorial_hq_attack_scenario_done; // one-shot latch

    // ---- RI-AI batch D / AI1D (2026-08-28) ----
    // The field-scoped roster window; see the class banner above for the per-field ownership
    // measurement. This is the ONLY route from an AI translation to a byte inside a unit or
    // building record, and it is why ai_view stays pointer-to-const.
    ai_roster_window roster;

    // llm_strat_ai_players_tick's own two globals -- `width_m` / `height_m` @0x00fe5b40 / 0x00fe5b44,
    // the torus wrap MASKS whose read side ai_view has carried since batch A layer 2. Same
    // arrangement as spiral_offsets above: the read side was already there and this is the one
    // function in the cluster that writes them (`MOV [0x00fe5b40],EAX` @0x004db91a, `MOV
    // [0x00fe5b44],EAX` @0x004db925, each storing extent-1 re-derived from map width/height).
    //
    // NOT AI-OWNED, and deliberately not claimed to be: the same 2026-08-28 image-wide scan finds
    // THREE writers of each -- llm_map_build_regions (0x00423353 / 0x0042335e),
    // llm_strat_planet_map_session_init (0x004dc66c / 0x004dc677) and this one -- against 88 and 86
    // total sites dominated by map/geometry readers. players_tick simply re-stamps them every AI
    // tick. They are ordinary writable scalars in ai_store, not members of the roster window, which
    // is reserved for the roster fields whose ownership was measured.
    uint32_t *map_width_mask;
    uint32_t *map_height_mask;
};

struct ai_state {
    ai_view  read;
    ai_store own;
};

// ---- accessors ----------------------------------------------------------------------------------
// The rosters are 2-D in the game and flat here, so the index arithmetic lives in one place instead
// of once per translated function.
inline const unit &unit_of(const ai_view &v, uint32_t player, int32_t index) {
    return v.units[player * v.caps.units + index];
}
inline const building &building_of(const ai_view &v, uint32_t player, int32_t index) {
    return v.buildings[player * v.caps.buildings + index];
}
inline const turret &turret_of(const ai_view &v, uint32_t player, int32_t slot) {
    return v.turrets[player * TURRET_SLOTS_PER_PLAYER + slot];
}
// Batch A layer 4. `slot` is buildings[player][i].sub_id -- a production building's slot, NOT its
// roster index. Unchecked on purpose: the original indexes straight off the byte it read.
inline const production &production_of(const ai_view &v, uint32_t player, int32_t slot) {
    return v.productions[player * v.caps.productions + slot];
}
// Batch B layer 2. `row` is an index into the cfg Invention table, so the same subscript addresses
// v.cfg_inventions[row] and progress_of(v, player, row). Unchecked, like everything else here: the
// original computes player * 0x384 + row * 3 with no bound on either (0x004e6dad-0x004e6dba).
inline const player_progress &progress_of(const ai_view &v, int32_t player, int32_t row) {
    return v.progress[player * PROGRESS_ROW_COUNT + row];
}

// The two map planes, COLUMN-major. The original computes a tile's byte offset as
// (x << 11) | (y << 3) in both -- 8 bytes per tile in tile_objects (one record) and 8 bytes per tile
// in the fog plane (one visibility counter PER PLAYER). Callers must have wrapped x and y through
// map_width_mask / map_height_mask first; nothing here bounds-checks, and neither does the original.
inline const tile_object &tile_at(const ai_view &v, int32_t x, int32_t y) {
    return v.tile_objects[(x << 8) | y];
}
inline uint8_t fog_visible_count_at(const ai_view &v, int32_t x, int32_t y, uint32_t player) {
    return v.fog_visible_by_count[(((x << 8) | y) * FOG_PLAYERS_PER_TILE) + player];
}

struct ai_calls {
    // engage
    void (*engage_candidate_add)(uint32_t target_ref, int32_t target_index);
    int32_t (*unit_has_ground_weapon)(uint32_t unit_ref, int32_t unit_index);
    int32_t (*unit_has_aa_weapon)(uint32_t unit_ref, int32_t unit_index);
    int32_t (*target_ref_is_alive)(uint32_t target_ref, int32_t target_index);
    // THREE arguments, not two. The original's third input is the register ESI, which it reads as
    // the bubble sort's "swapped" flag before ever writing it -- an ambient input, not a local. It
    // was undeclared until REBIND-AI-ESI (2026-09-10) committed it as a custom-storage ESI
    // parameter, which is what made this member bindable at all: with a 2-argument ABI the
    // standalone arm could not supply the flag and the row stayed an R3 trap. Every caller now
    // passes it explicitly; see ai_engage.cpp's two sites for the per-site proof.
    void (*engage_sort_candidates_by_dist)(uint32_t source_ref, int32_t source_index,
                                           int32_t inherited_sorted_flag);
    void (*engage_partition_turret_candidates)();
    void (*commit_attack_order)(uint32_t player, int32_t unit_index, uint32_t target_ref,
                                int32_t target_index);
    void (*commit_attack_order_alt)(uint32_t player, int32_t unit_index, uint32_t target_ref,
                                    int32_t target_index);
    // `player` and `target_player` are uint32_t, not uint16_t: SIM-READY (2026-08-07) corrected
    // these two Ghidra prototypes from `AX:2`/`BX:2` to the full EAX/EBX the entry sequence
    // actually spills, so mh_calls.gen.h now marshals 32 bits. The call sites keep their explicit
    // (uint16_t) casts -- the original masks with `& 0xffff` internally, so the truncation is real
    // behaviour and worth keeping visible rather than implied by a narrow parameter type.
    void (*order_attack_building_enqueue)(uint32_t player, int32_t unit_index, uint32_t target_ref,
                                          int32_t target_index, uint32_t weapon_id);
    void (*order_attack_unit_enqueue)(uint32_t player, int32_t unit_index, uint32_t target_player,
                                      int32_t target_index, uint32_t weapon_id);
    // build candidates
    // NOTE the first parameter is named `unused` in the committed Ghidra prototype, but every call
    // site loads the player index into EAX before the call, so it is passed as the player index
    // here and named for what the CALLER means by it.
    uint32_t (*bldg_find_by_ai_build_and_type)(int32_t player, uint32_t ai_build_id,
                                               uint32_t building_type);
    // ---- batch A layer 1 ----
    // The two threat-grid helpers are __cdecl and their COMMITTED PARAMETER NAMES ARE MISLEADING
    // (`grid_height`/`grid_width`, `center_y`/`center_x`); the mirrored names below are what the
    // call sites actually pass, read off the push order. Both write only inside
    // player_data::ai_tile_flags_grid, i.e. inside player_data.
    void (*grid_clear_threat_bit)(uint8_t *grid, int32_t width, int32_t height);
    void (*grid_stamp_threat_ring)(uint8_t *grid, int32_t width, int32_t height, int32_t x, int32_t y,
                                   int32_t radius);
    int32_t (*bldg_is_alive)(int32_t player, int32_t building_index);
    uint32_t (*unit_get_ai_group_index)(int32_t player, int32_t unit_index);
    uint32_t (*unit_get_sight)(uint32_t unit_ref, int32_t unit_index);
    uint32_t (*unit_max_weapon_range)(uint32_t unit_ref, int32_t unit_index);
    // Appends to the engage scratch; `ring_index` is a RADIUS (it indexes a per-radius cell-count
    // table), and `target_mask` is a full dword -- the Ghidra prototype said `byte` until 2026-08-01.
    int32_t (*scan_spiral_ring_for_engage_candidates)(int32_t player, int32_t x, int32_t y,
                                                      int32_t ring_index, uint32_t target_mask);
    // Squared distance on the torus, wrapping through map_width / map_height. Pure.
    uint32_t (*toroidal_dist_sq)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

    // ---- batch A layer 2 ----
    // Pure predicates and queries over the rosters.
    int32_t (*unit_is_idle_or_parked)(int32_t player, int32_t unit_index);
    int32_t (*unit_attack_target_is_dead)(int32_t player, int32_t unit_index);
    int32_t (*target_ref_has_engageable_weapon)(int32_t target_ref_kind, int32_t target_ref_index,
                                                uint32_t attacker_weapon_flags);
    uint32_t (*target_dist_sq)(uint32_t ref_a, int32_t idx_a, uint32_t ref_b, int32_t idx_b);
    uint32_t (*unit_squad_firepower_value)(int32_t player, int32_t unit_idx);
    // Appends to the GLOBAL scan-target scratch (_G_LLM_STRAT_AI_SCAN_TARGETS + its count) with its
    // own alive test and dedupe and NO cap check. NOT the same function as
    // llm_strat_ai_target_list_add (0x004d6d67, batch A layer 1), which is the one that appends to
    // player_data[player].ai_target_list with a 0x40 cap -- this comment said THAT until 2026-08-01,
    // when layer 3 translated 0x004ec2d0 and found it writes a different array entirely. Its second
    // parameter is the packed target REF, not an id; Ghidra's committed name said `target_id` and
    // was corrected in the same session.
    void (*scan_target_list_add)(int32_t player, uint32_t target_ref, int32_t target_index);
    // The AI unit-group machine. group_create returns a group index or -1.
    int32_t (*group_create)(int32_t player_id);
    void (*group_member_move)(uint32_t player, int32_t src_group, int32_t dst_group, int32_t unit_id);
    // Batch C layer 6 (own shadow site since 2026-08-06). BACK-INSERT: appends at slot
    // task_queue_count (before increment), no-op at cap 0x40. param_4 = pending_param,
    // param_5..param_8 = param_a..param_d, param_9 = sub_code -- see ai_group_task_queue_ops.h/.cpp
    // for the exact field mapping. Called throughout batches A-C wherever a group needs a new task
    // queued at the back (group_create's own initial task, group_form, group_expansion,
    // group_split_off, army_milestone, invasion, ...).
    void (*group_task_enqueue)(int32_t player, int32_t group, uint16_t task_code, uint32_t param_4,
                               uint32_t param_5, uint32_t param_6, uint32_t param_7,
                               uint32_t param_8, uint16_t param_9);
    // Batch C layer 2. group_task_enqueue's front-insert sibling: same nine arguments, same
    // destination fields, but it shifts the queue rather than appending. Its only AI caller in the
    // cluster is llm_strat_ai_group_enter_hold, which uses it to make HOLD (task_code 0xb) the
    // ACTIVE task rather than a queued one. Same region profile as enqueue -- player_data only.
    void (*group_task_preempt)(int32_t player, int32_t group, int16_t task_code, int32_t param_4,
                               int32_t param_5, int32_t param_6, int32_t param_7, int32_t param_8,
                               int16_t param_9);
    void (*unit_launch_from_storage_enqueue)(uint8_t player, int32_t unit_id, uint32_t target_x,
                                             uint32_t target_y);
    // OUTPUT-POINTER helper: writes a tile the player owns, or its home tile, through the two args.
    void (*pick_owned_tile_or_home)(int32_t player, uint32_t *out_x, uint32_t *out_y);
    // The AI construction/repair queue. All four write player_data[player].ai_bldg_queue[] + its count.
    // Both return 1 = REJECTED because ai_bldg_queue_count had already reached 0x40, 0 = appended.
    // repair's return type was corrected void -> int32_t on 2026-08-02 (ghidra_findings
    // 2026-08-02-1646-1): `MOV EAX,0x1` at 0x004e2881 on the queue-full path and `XOR EAX,EAX` at
    // 0x004e2a1e on the append path, i.e. the same two constants its already-int sibling uses. The
    // original's sole call site (0x004e5fc4) discards the value, and so does ours.
    int32_t (*queue_bldg_repair)(int32_t player, int32_t building_index);
    int32_t (*queue_bldg_upgrade)(int32_t player, uint32_t building_index);
    void (*queue_rotate_newest_to_front)(int32_t player);
    void (*queue_remove_at)(int32_t player, uint32_t slot_index);
    // The GAME's qsort and the comparator address llm_strat_ai_scan_target_list_sort passes it. Both
    // are here rather than being replaced by a host sort because the comparator returns 0 on a tie:
    // the permutation of equal keys is Watcom qsort's, and any other sort would produce a different
    // (still "sorted") array -- a divergence the oracle would report and a desync if promoted.
    void (*qsort)(void *base, uint32_t num, uint32_t width, void *compare);
    void *scan_target_sort_cmp;

    // ---- batch A layer 3 ----
    // Fills the score/flags/position/range/counter-target fields of a scan-target record IN PLACE.
    // Reads only (units/buildings/player_data/Building/Weapon/turrets); everything it writes goes
    // through the record pointer, so it is safe to run for real wherever that record's array is a
    // declared shadow region.
    void (*group_classify_target_object)(uint32_t player, scan_target_entry *target_record);
    // The two weapon-class predicates target_ref_has_engageable_weapon ANDs together. Pure.
    int32_t (*target_ref_has_ground_weapon)(uint32_t target_ref, int32_t target_index);
    int32_t (*target_ref_has_aa_weapon)(uint32_t target_ref, int32_t target_index);
    int32_t (*rand_below_ai)(uint32_t range);
    // THE BUILD-PLACEMENT FOOTPRINT TEST, and its return polarity is inverted from its name:
    // llm_scan_masked_table_for_empty_cell returns 0 as soon as a covered cell reads 0 or 6, and
    // NONZERO when the whole footprint fits. It also writes the grid wrap mask on entry.
    int32_t (*footprint_scan_for_blocked_cell)(uint8_t *grid, int32_t grid_width, int32_t grid_height,
                                               uint8_t *footprint_mask, int32_t span_x,
                                               int32_t span_y, int32_t start_x, int32_t start_y);
    // "Is a build of this type already sitting in the player's AI build queue?" Pure read.
    int32_t (*bldg_type_queue_has_pending)(int32_t player, uint32_t building_type);
    // Appends to player_data[player].ai_bldg_queue and bumps its count -- player_data ONLY, and it
    // calls nothing. It does NOT place a building or issue an order, so it is NOT an escape and must
    // run for real: the caller immediately stamps ai_bldg_queue[count - 1].
    int32_t (*bldg_queue_construction)(int32_t player, int32_t building_type, int16_t x, uint16_t y);
    // Re-stamps every OTHER player's tile-ownership grid around a newly placed footprint. Writes
    // player_data (all players) and, via llm_strat_ai_grid_stamp_seeds, the grid wrap mask.
    void (*notify_map_changed)(int32_t builder_player, int32_t building_type, int32_t tile_x,
                               int32_t tile_y);

    // ---- batch A layer 4 ----
    // The BUILDING half of the AA-weapon predicate (0x004d746c). llm_strat_ai_target_ref_has_aa_weapon
    // does not CALL it -- it masks the owner nibble and FALLS THROUGH into it, it being the next
    // function in the image, which is a tail call spelled without a jump. Its first argument is
    // therefore the ref's owner nibble, ALREADY MASKED, not the packed ref. Pure.
    int32_t (*bldg_has_aa_weapon)(uint32_t player, int32_t building_index);

    // ---- batch A layer 5 ----
    // THE TWO WINDOW TESTS over player_data::ai_tile_flags_grid. Same walk, same stack slots, same
    // single side effect (they store the grid wrap mask on entry and read it back), differing only in
    // the per-cell predicate: `_match_stencil` demands the cell EQUAL target_byte (all three scanners
    // pass 2), `_all_near_unthreatened` demands (cell & 0x40) == 0 && (cell & 0x1f) <= 3.
    // `footprint_mask` selects which cells of the span_x x span_y window are tested (non-zero = test
    // it) and is walked CONTIGUOUSLY, one byte per cell, not reset per row.
    // Both are __cdecl and both PRESERVE every register but EAX, which is why the callers' decompiles
    // are full of extraout_ECX/extraout_EDX that are cspec artifacts rather than real values -- the
    // pre-call value survives. Read the .asm, not the .c, around these calls.
    int32_t (*grid_match_stencil)(uint8_t *grid, int32_t grid_width, int32_t grid_height,
                                  uint8_t *footprint_mask, int32_t span_x, int32_t span_y,
                                  int32_t start_x, int32_t start_y, int32_t target_byte);
    int32_t (*grid_stencil_all_near_unthreatened)(uint8_t *grid, int32_t grid_width,
                                                  int32_t grid_height, uint8_t *footprint_mask,
                                                  int32_t span_x, int32_t span_y, int32_t start_x,
                                                  int32_t start_y);
    // "Is this resource deposit rich enough to be worth a mine?" -- batch B keeps the body; layer 5
    // only calls it. Its second argument is a cfg Building[] TYPE id, not a resource kind: the
    // committed Ghidra name said `building_idx` and the caller's said `resource_kind`, and both were
    // corrected on 2026-08-01 off the caller's IMUL by the 0x842 Building stride.
    int32_t (*resource_site_meets_threshold)(int32_t player, int32_t building_type, int32_t fine_x,
                                             int32_t fine_y);
    // The placement legality test shared with the human build path: would putting a width x height
    // building at (x, y) seal a neighbouring tile off? `width`/`height` come from cfg Building +0xa /
    // +0x9 -- note that order, the HEIGHT byte is the lower one.
    int32_t (*bldg_check_placement_encloses_neighbors)(int32_t player, int32_t x, int32_t y,
                                                       uint32_t width, uint32_t height);

    // ---- batch B layer 0 ----
    // "Is this unit an aircraft?" -- a pure ten-comparison lookup of Unit[type].category against
    // 0x0f..0x18 (0x98 bytes, WRITES NOTHING, both exits set the full dword: MOV EAX,1 @0x004d464f
    // and XOR EAX,EAX @0x004d4655). Its prototype was committed 2026-08-02; before that the
    // interop layer could not call it at all.
    int32_t (*unit_is_aircraft)(uint32_t unit_ref, int32_t unit_index);
    // THE OTHER target_list_add: appends to player_data[player].ai_target_list (0x40 cap), NOT to
    // the global scan-target scratch that `scan_target_list_add` above feeds. Writes player_data
    // and nothing else (state matrix: one region, 6 writes + 1 rmw).
    //
    // Parameter names match the Ghidra prototype (finding 2026-08-03-1549-1, applied 2026-08-21):
    // the one AI call site passes (row player, the DAMAGED object's packed ref, that object's roster
    // index, the AGGRESSOR's packed ref, the aggressor's roster index) -- see ai_attacker_intel.cpp
    // for the call site and ai_target.{h,cpp} for the writer.
    void (*target_list_add)(uint32_t player, int32_t victim_ref, int32_t victim_index,
                            uint32_t aggressor_ref, int32_t aggressor_index);

    // ---- batch B layer 1 ----
    // The two influence-grid primitives llm_strat_ai_recompute_map_influence drives, both __cdecl
    // over player_data::ai_tile_flags_grid handed in as a raw pointer. They are AI manifest members
    // at antichain LAYER 2, so they stay original until that slice; neither had a committed
    // prototype until 2026-08-02, which is what blocked layer 1.
    //
    // `width` bounds the HIGH index byte (X) and `height` the LOW one (Y), matching
    // ai_tile_flags_grid's (x << 8) | y packing -- NOT what either function's Ghidra plate said
    // previously, and not what the layer-2 sibling helpers above are still committed as.
    // See ghidra_findings 2026-08-02-0810-1/-2/-4 for the three derivations.
    //
    // fill_below_threshold writes only the grid. flood_step ALSO writes
    // _G_LLM_STRAT_AI_GRID_WRAP_MASK (two byte stores of the extents on entry), so any site that
    // reaches it must declare that region.
    void (*grid_fill_below_threshold)(uint8_t *grid, int32_t width, int32_t height, int32_t threshold,
                                      int32_t fill_value);
    void (*grid_flood_step)(uint8_t *grid, int32_t width, int32_t height, int32_t source_level,
                            int32_t fill_value);
    // Compacts the player's queued unit-training entries. Writes player_data only: one queue status
    // OR and two decrements of the training-queued counters at +0x284e8 / +0x28678. Its 20-byte
    // entry at 0x004e2c98 jumps into a shared tail at 0x004e2c67; the committed prototype takes one
    // register argument and the body reads only that one, even though its caller happens to leave
    // building_index live in EDX across the call.
    void (*queue_flush_unit_train_entries_2)(int32_t player);
    // Sums a cfg Building type's resource costs. PURE -- writes no memory at all. Its argument is a
    // cfg TYPE index (IMUL 0x842), not a roster slot. NOTE its summation STOPS at the first slot
    // with a zero resource id rather than skipping it (JE straight to the exit @0x004e2d7c), which
    // its plate wording still blurs -- ghidra_findings 2026-08-02-0810-6.
    int32_t (*bldg_total_resource_cost)(int32_t building_type);
    // ---- batch B layer 2 (the rest of it) ----
    // llm_strat_ai_plan_unit_training's two callees.
    // count_unit_build_sources is an OUT-POINTER helper: it writes ONLY through `out_counts`, which
    // is a 100-int buffer on the CALLER's stack, and it fills indices 1..G_UNIT_COUNT_TOTAL
    // INCLUSIVE (0x004e2201-0x004e2211) -- index 0 is never written. It reads buildings, the cfg
    // Building/Unit tables and _G_LLM_STRAT_UNIT_HOUSING_STATS. Writes no tracked region.
    void (*count_unit_build_sources)(int32_t player, int32_t *out_counts);
    // Appends a TRAIN entry to player_data[player].ai_bldg_queue and bumps the two training tallies
    // -- the exact inverse of queue_flush_unit_train_entries_2 above. player_data only. Its int
    // return is ignored by plan_unit_training (no TEST/CMP follows the CALL at 0x004e6ef6).
    int32_t (*queue_train_unit)(int32_t player, uint32_t unit_id);
    void (*order_grant_resource_raw)(uint16_t player, uint32_t res_type, uint32_t amount);
    void (*bldg_queue_handle_recruit_state)(uint32_t player, int32_t queue_index);
    void (*bldg_queue_process_entry)(uint32_t player, int32_t queue_index);
    // The 11-byte no-op at 0x004e81df. It is in this table rather than elided because eliding a
    // dispatch arm is how a table slot silently stops being reproduced; see its Ghidra plate.
    void (*bldg_queue_handle_state2_empty)();
    void (*bldg_queue_handle_upgrade_or_cancel)(uint32_t player, int32_t queue_index);

    // ---- batch B layer 3 (the build-queue dispatch arms' own callees) ----
    // "Is there an idle producer building that can make unit type U?" -> a 1-based buildings roster
    // index, or 0 for none. Pure: it reads the roster and the cfg Building unit_quant double table.
    int32_t (*bldg_find_idle_producer_for_unit)(int32_t player_id, int32_t unit_id);
    // THE ARGUMENT ORDER IS REVERSED relative to order_population_delta_enqueue below, and that is
    // the original's, not a slip -- 0x0046d9a9 feeds the order's value field from EAX and the player
    // from EDX, 0x0046f63a does the opposite. Returns a constant 1.
    int32_t (*order_recruit_unit_enqueue)(uint32_t unit_id, uint32_t player_id);
    // ITS NAME IS UNDER DOUBT (ghidra_findings 2026-08-02-1922-10): the third argument is a unit
    // TYPE id, and its one AI caller uses it to start a unit's production at a producer building.
    // The Ghidra symbol was renamed off `..._repair_start_enqueue` on 2026-08-23 to match; the
    // third argument is committed as `int unit_type`, hence int32_t here.
    void (*bldg_order_production_add_enqueue)(uint16_t player, int32_t producer_index,
                                              int32_t unit_type);
    // Accrues the unit's cfg resource cost into the player's spend rings. player_data only.
    void (*econ_track_unit_resource_spend)(int32_t player_idx, int32_t unit_idx);
    // FILLS the shared site-candidate scratch for a building type -- a producer, not a query: its
    // caller reads *site_candidate_count immediately afterwards to decide whether a tile was found.
    void (*bldg_production_type_dispatch)(uint32_t player_id, int32_t building_id);
    // The ordinary construction order. Parameter names here are what the CALL SITE passes; the
    // committed Ghidra names are param_1/param_2/a2/param_4 (storage EAX/EDX/EBX/ECX).
    uint32_t (*order_queue_construction_enqueue)(uint32_t tile_x, uint32_t tile_y,
                                                 uint32_t building_type, uint16_t player);
    // The instant-construct path taken when the affordability waiver is NOT set. `param_4` is a
    // literal 0 at the one AI call site (XOR ECX,ECX @0x004e7f6d) and is left unnamed because
    // nothing establishes what it means.
    int32_t (*bldg_instant_construct_find_slot_enqueue)(uint32_t tile_x, uint32_t tile_y,
                                                        int32_t building_type_id, uint32_t param_4,
                                                        uint16_t player);
    void (*bldg_record_resource_expenditure_stats)(uint32_t player_id, int32_t building_id);
    // See order_recruit_unit_enqueue on the swapped argument order. The AI passes cfg
    // Building[type].worker_count as the delta.
    void (*order_population_delta_enqueue)(uint32_t player_id, int32_t population_delta);
    // The SECOND map-changed notifier, distinct from notify_map_changed above: it re-seeds every
    // OTHER player's influence grid around a footprint that could NOT be built, and it takes the
    // same (builder_player, building_type, tile_x, tile_y) shape.
    void (*notify_map_changed_2)(int32_t builder_player, int32_t building_type, int32_t tile_x,
                                 int32_t tile_y);
    void (*bldg_order_upgrade_enqueue)(uint32_t player_id, int32_t building_index);
    void (*bldg_order_repair_cycle_start_enqueue)(uint32_t player_id, int32_t building_index);

    // ---- batch B layer 3, the build-category / housing planners' callees ----
    //
    // All five are PURE roster+cfg queries and all five got their prototype committed on 2026-08-02
    // (EN v203) because they had none at all -- the generated call layer emitted compile-error stubs
    // for them and no translation could reach them. See ghidra_findings 2026-08-02-2103-3 for the
    // derivations, including why Ghidra's decompiler under-counted the two counters' parameters (both
    // bodies terminate by JMPing into a SHARED tail, which loses the second argument).
    //
    // THE THREE COUNTERS COUNT DIFFERENT THINGS OVER THE SAME ROSTER, and the names are easy to swap:
    //   _by_id       matches buildings[player][i].building_id against the argument directly.
    //   _by_type     matches cfg Building[buildings[player][i].building_id].type.
    //   _by_category matches the AI's own build-category column of the same cfg row.
    int32_t (*bldg_count_by_id)(int32_t player, int32_t building_id);
    int32_t (*bldg_count_by_type)(int32_t player, int32_t bldg_type);
    int32_t (*bldg_count_by_category)(int32_t player, uint32_t category);
    // "Is a build of this type already sitting in the player's AI build queue?" -- the SIBLING of
    // bldg_type_queue_has_pending above, not a duplicate: they test the same queue with different
    // acceptance rules, and the originals use both. Returns in AL (committed `bool`), which is why
    // the callers' decompiles show a CONCAT31(extraout_var, ...) that is a Watcom artifact.
    int32_t (*bldg_type_already_queued)(int32_t player, uint32_t building_type);
    // Two per-player capability predicates over (buildings roster x cfg Building.unit_quant x cfg
    // Unit): "does any building I own produce a unit whose Unit[].ai_unit is 6 (helicopter)" and
    // "... whose Unit[].type is 0x11 or 0x12 (plane)". Both return 0/1 in EAX.
    int32_t (*bldg_has_heli_unit)(int32_t player);
    int32_t (*bldg_side_has_aircraft_producer)(int32_t player);

    // ---- batch B layer 3, the mine planner (2026-08-02-2250) ----
    // Recomputes player_data[player].ai_mine_yield_by_resource. AN OUT-POINTER helper: `out_yield`
    // is the address of that array, which is why an instruction sweep finds no writer for it, and
    // why declaring player_data is what makes this call safe to run for real in a shadow arm.
    //
    // TWO THINGS ITS SIGNATURE DOES NOT SHOW. It reads
    // _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT at 0x004e35ab, so the caller's store into that
    // global must already have happened; and its callee closure reaches llm_strat_order_enqueue
    // (via llm_strat_bldg_order_restart_construction_enqueue @0x0046e627), so it writes the ORDER
    // container as well -- a site that reaches it must declare _G_LLM_STRAT_ORDER_QUEUE and
    // _G_LLM_STRAT_ORDER_QUEUE_COUNT. It also calls llm_strat_ai_queue_flush_unit_train_entries,
    // which writes player_data.
    void (*mine_portfolio_rebalance)(uint32_t player, int32_t *out_yield);

    // ---- batch B layer 3, the turret-upgrade planner (2026-08-03) ----
    // Fills `flag_array[1 .. buildings[player][0].index]` with 1 for every building reachable from an
    // HQ/mother-base seed (cfg Building.type 0x1a or 6) through the player's own tiles, 0 otherwise,
    // SKIPPING `exclude_bldg_idx` as if it were not there. It writes NOTHING else -- the flags go out
    // through the pointer, so a site that reaches it must declare the array's region by hand.
    void (*bldg_connectivity_flood_fill)(uint32_t player, int32_t exclude_bldg_idx,
                                         uint8_t *flag_array);
    // Nearest building of `player` whose map::object::building.built_flags bit 0x1 (connected/
    // reached) is set, by toroidal distance to (x, y); -1 for none. Pure -- it reads the ROSTER and
    // nothing else. CORRECTED 2026-08-03 (AI1B layer 4): this comment used to say it reads the AI's
    // own _BASE connectivity PLANE, and the tracker's AI1B progress block said so too. It does not.
    // Its complete set of data absolutes is the four roster fields plus llm_strat_toroidal_dist_sq;
    // the plane is what llm_strat_bldg_connectivity_flood_fill writes and only the CALLER reads.
    // Its return gate also gives the -1 answer more often than "no connected building" would --
    // see ai_nearest_flagged.h, which is now the reimplementation of record.
    int32_t (*find_nearest_flagged_building)(int32_t player, int32_t x, int32_t y);
    // __cdecl OUT-POINTER helper: the midpoint of (x0,y0)-(x1,y1) ON THE TORUS. It picks the SHORTER
    // way round per axis (dx -= width when dx > width/2, dx += width when dx < -width/2), halves with
    // an ARITHMETIC shift, then reduces mod the extent with an UNSIGNED divide after adding one full
    // extent. Writes only through the two out-pointers; preserves EAX/EBX/EDX and returns nothing.
    // The two out-parameters carry the committed pointee, `int32_t *` (TACT1-P C6, 2026-09-04): the
    // GENERATED wrapper now types them that way too -- gen_dll_calls.py used to map every non-struct,
    // non-char pointer parameter down to `void *` regardless of what Ghidra had committed, which is
    // what forced the blunt type here. Pass `&your_int32`.
    void (*tile_midpoint_wrapped)(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t *out_x,
                                  int32_t *out_y);
    // Order 0x6b on a building. Reaches llm_strat_order_enqueue DIRECTLY (never the scratch-args
    // path, never llm_strat_order_schedule), so it writes _G_LLM_STRAT_ORDER_QUEUE and
    // _G_LLM_STRAT_ORDER_QUEUE_COUNT and nothing else. The IMMEDIATE lane -- nothing goes on the wire.
    void (*bldg_order_restart_construction_enqueue)(uint32_t player_id, int32_t building_index);

    // ---- batch B layer 3, the worker-rebalance planner (2026-08-03) ----
    // cfg Building[buildings[player][i].building_id] byte at +0x375, zero-extended: "does this
    // building TYPE employ workers at all". Pure table lookup, no calls.
    int32_t (*bldg_uses_workers)(uint32_t player, int32_t building_index);
    // llm_strat_ai_is_worker_priority_candidate @0x004e3a77 -- reached through the CALL layer rather
    // than through mh::ai::detail, because in a shadow arm the callee must behave exactly as the
    // original does; a C++-to-C++ edge is only correct once that function is promoted.
    int32_t (*is_worker_priority_candidate)(int32_t player, int32_t site_index);
    // Three more IMMEDIATE-lane building orders, same closure shape as
    // bldg_order_restart_construction_enqueue above except that the two worker ones also pass through
    // llm_strat_order_scratch_set_field -- so a site reaching THEM must declare
    // _G_LLM_STRAT_ORDER_SCRATCH_ARGS as well as the two queue regions.
    void (*bldg_order_activate_enqueue)(uint32_t player_id, int32_t building_index);
    void (*bldg_order_assign_workers_enqueue)(uint16_t player, uint16_t bldg_idx,
                                              uint32_t worker_count);
    void (*bldg_order_unassign_workers_enqueue)(uint16_t player, uint16_t bldg_idx,
                                                uint32_t worker_count);

    // ---- batch B layer 4, the site-scanner dispatch (2026-08-03) ----
    //
    // The three scanners llm_strat_ai_bldg_production_type_dispatch routes to, and the sort that
    // always follows. All four write ONLY _G_LLM_STRAT_AI_SITE_CANDIDATES and its count, so a site
    // that reaches them declares those two regions and lets them run for real.
    //
    // THEY APPEND; THEY DO NOT RESET. Each scanner's only write to the count is an INC (0x004e6502,
    // 0x004e6673, 0x004e682a). The single constant store in the image is the dispatcher's own, at
    // 0x004e7cfa -- which is why ai_site_dispatch.cpp does the reset itself and must keep doing it.
    //
    // Each scanner's SECOND parameter reaches the original implicitly: the dispatcher's call sites
    // set only EAX and leave its own building_id sitting in EDX. Passed explicitly here.
    void (*bldg_scan_resource_site_candidates)(uint32_t player, int32_t building_type);
    void (*scan_build_site_candidates)(int32_t player_idx, int32_t building_idx);
    void (*bldg_scan_grid_candidates)(uint32_t player, int32_t building_idx);
    // Sorts the candidate list nearest-first, in place, over *site_candidate_count entries.
    void (*sort_site_candidates_by_dist)();

    // ---- batch B layer 4, the storage-launch adapter (2026-08-03) ----
    //
    // llm_strat_unit_order_auto_launch_from_storage_enqueue @0x0046cf2c -- the function
    // llm_strat_ai_unit_launch_from_storage_enqueue TAIL-JUMPS into. Distinct from the
    // `unit_launch_from_storage_enqueue` slot far above, which is the ADAPTER (0x004d58e7) and stays
    // stubbed (`inert_launch`) for every OTHER site: from a caller's point of view it escapes.
    //
    // THIS SLOT IS DELIBERATELY NOT AN ESCAPE, and the distinction is the whole design of the
    // adapter's shadow site. Its closure is exactly three leaf writers -- llm_strat_order_enqueue
    // (_G_LLM_STRAT_ORDER_QUEUE + _COUNT) and llm_strat_order_scratch_reset / _set_field
    // (_G_LLM_STRAT_ORDER_SCRATCH_ARGS) -- and nothing else, verified twice: the state matrix
    // credits those three regions and no others, and an enumeration of the closure finds no callee
    // beyond assert_stack_capacity. Because all three regions are DECLARED by the adapter's site,
    // the harness rolls our arm's enqueue back and hands the game the original's, so the order goes
    // out exactly once and the comparison is over the real queued bytes.
    void (*unit_order_auto_launch_from_storage_enqueue)(uint32_t player, int32_t unit_idx, uint32_t x,
                                                        uint32_t y);

    // ---- batch B layer 4/5, the mine portfolio rebalance (2026-08-05) ----
    //
    // llm_strat_ai_calc_mine_yield_estimate @0x004e30e9. Writes ONLY through its two out-pointers,
    // which the rebalance always aims at rows of _G_LLM_STRAT_AI_MINE_YIELD_ESTIMATE and
    // _G_LLM_STRAT_AI_MINE_QUALITY_HIST, and calls nothing but the inert stack probe and the x87
    // truncation helper utils_math_trunc @0x004d0596. So a site that reaches it declares those two
    // regions and lets it run for real; nothing is stubbed.
    //
    // TWO CONTRACTS ITS SIGNATURE DOES NOT SHOW. out_yield[0] is NOT zeroed -- the estimator's init
    // loop runs 1..4 and its total pass only ADDs -- so the caller owns that slot. And the tile
    // coordinates are FINE coords: the estimator shifts them right by two itself.
    void (*calc_mine_yield_estimate)(int32_t building_id, uint32_t tile_x, uint32_t tile_y,
                                     int32_t *out_yield, int32_t *out_quality);
    // llm_strat_ai_queue_flush_unit_train_entries @0x004e2c8e -- the ALIAS ENTRY, and this slot is
    // bound to 0x004e2c8e rather than to the `_2` slot far above (0x004e2c98) so that the arm makes
    // the same call the original makes. The two differ by one inert Watcom stack probe and a
    // FALL-THROUGH with no JMP: 0x004e2c8e is `PUSH 4 / CALL assert_stack_capacity` and then simply
    // runs into 0x004e2c98. Nothing derives that edge from a reference, which is why it lives in
    // tools/data/fallthrough_entries.json and why check_arming_set.py has to be run rather than
    // remembered. Writes player_data only.
    void (*queue_flush_unit_train_entries)(int32_t player);

    // ---- batch C layer 0, the object-removed notification hook (2026-08-05) ----
    //
    // llm_strat_ai_grid_stamp_seeds @0x004b4ac2 -- the influence-grid stamping primitive, and the
    // SAME 9-argument __cdecl shape as grid_match_stencil / footprint_scan_for_blocked_cell above
    // (grid, extents, stencil, span, origin, value), which is what makes the arg roles legible.
    // It writes the caller's grid and _G_LLM_STRAT_AI_GRID_WRAP_MASK and calls nothing, so a site
    // that declares player_data and the wrap mask runs it FOR REAL. Its prototype had never been
    // committed until 2026-08-05 (ghidra_findings 2026-08-05-0441-1); before that the generated
    // call layer emitted a compile-error stub for it.
    void (*grid_stamp_seeds)(uint8_t *grid, int32_t map_width, int32_t map_height, uint8_t *stencil,
                             int32_t span_x, int32_t span_y, int32_t origin_x, int32_t origin_y,
                             int32_t seed_value);
    // Purges one packed target ref from player_data[player].ai_target_list. Writes player_data and
    // nothing else (matrix: 1 write + 1 rw + 2 movs, all player_data) and calls nothing -- REAL.
    void (*target_list_remove)(int32_t player, uint32_t aggressor_ref, int32_t aggressor_index);
    // Re-reconciles the AI build queue after a building of the player's changed type/existence.
    // Writes player_data; its callees (bldg_queue_construction, queue_flush_unit_train_entries_2,
    // bldg_total_resource_cost) write player_data too and nothing else -- REAL under a site that
    // declares it.
    void (*queue_reconcile_bldg_change)(uint32_t player, uint32_t building_index);
    // Unlinks a unit from its AI group's intrusive member list. THIS is what DECREMENTS
    // llm_strat_ai_unit_group::member_count (the DEC at 0x004d4a22) -- notify_object_removed does
    // not, whatever its old plate said. Writes `units` (the +0xd4/+0xd6/+0xd8 link words) and
    // player_data (head/tail/count), calls nothing.
    void (*group_member_unlink)(uint32_t player, int32_t ai_group_index, uint32_t unit_id);
    // group_member_unlink's link-side sibling (RI-AI batch C layer 5, 2026-08-06): links a unit into
    // `dst_group`'s intrusive member list (the INC at 0x004d4a22's counterpart). Writes `units` (the
    // +0xd4/+0xd6/+0xd8 link words) and player_data (head/tail/count on dst_group), calls nothing.
    // group_member_move (@0x004d4af5) is the unlink-then-link wrapper that calls both this and
    // group_member_unlink above.
    void (*group_member_link)(uint32_t player, int32_t ai_group_index, uint32_t unit_id);
    // Removes an AI group and compacts the last group into the freed slot. Writes player_data and
    // `units`, and its closure reaches llm_strat_order_enqueue via
    // llm_strat_ai_route_unit_to_home_storage -> unit_order_move/exit_storage_enqueue, so a site
    // must declare _G_LLM_STRAT_ORDER_QUEUE/_COUNT/_SCRATCH_ARGS as well. It does NOT reach
    // llm_strat_order_schedule or _dispatch, so nothing goes on the wire and it is not an escape.
    void (*group_remove)(int32_t player, uint32_t group_index);

    // ---- batch C layer 1, the unit-group task machine's two entry points ----
    //
    // Both are called by llm_strat_ai_unit_group_tick and by nothing else, and both are heavy: the
    // ACTIVATION dispatcher writes the group's active_param_a / task_start_time / active_flag and
    // then switches task_code 2..0x18 into a per-task handler, and the per-tick STEP is the
    // continuation of whichever handler activation chose. Their combined closure (69 + 15 functions
    // over tools/data/call_graph_no_crt.json) writes exactly six regions -- player_data, units, the
    // three order regions and _G_LLM_STRAT_RNG_STATE -- and contains neither llm_strat_order_schedule
    // nor llm_strat_order_dispatch, so nothing goes on the wire and a site declaring those six runs
    // both FOR REAL. THE PRNG IS THE ONE TO WATCH: several task handlers reach llm_rand_below_ai, so
    // a site that omits _G_LLM_STRAT_RNG_STATE would let our arm consume the channel a second time
    // on top of the original's draws and desync the sim it is being measured inside.
    void (*group_task_activate)(int32_t player, int32_t group_index);
    // Returns non-zero to ask its caller to RE-ENTER the same group immediately rather than move to
    // the next one -- llm_strat_ai_unit_group_tick loops on it. Not a success/failure flag.
    uint32_t (*group_task_step)(uint32_t player, int32_t group_index);

    // ---- batch C layer 2: llm_strat_ai_group_task_activate's NINETEEN dispatch arms ----
    //
    // The jump table lives at 0x004eb0cd, 25 dwords for task codes 0..0x18, and it is NOT in the
    // exported .asm -- it was read out of the image directly (ReVA read of that address, 2026-08-05,
    // reproducing the same table the 2026-08-05-1221 session recorded). The mapping is spelled once,
    // in ai_group_task_machine.cpp's ACTIVATE_ARMS table; the order of the members below is the
    // TASK-CODE order of that table, so a reader can check the two against each other by eye.
    //
    // THREE CODES (0, 1, 0xa) DISPATCH TO THE FUNCTION'S OWN EPILOGUE and so have no member here.
    // Two arms are genuine no-op stubs in the image (task_wait @0x004eaf67 and task_hold @0x004eab33
    // are `PUSH n / CALL assert_stack_capacity / RET`, eleven bytes each) and take NO arguments --
    // they are in this table rather than elided for the reason the build-queue's state-2 no-op is:
    // eliding a dispatch arm is how a table slot silently stops being reproduced.
    //
    // ALL NINETEEN RUN FOR REAL in the shadow arm. Their combined write closure is the six regions
    // llm_strat_ai_unit_group_tick already declares (derive_write_closure over the 69 reachable
    // functions), and none of them escapes: no llm_strat_order_schedule, no llm_strat_order_dispatch,
    // no sound, no UI. Stubbing one would not be the safe choice -- activation is the only thing that
    // stamps active_flag, and llm_strat_ai_unit_group_tick loops until it is set.
    void (*group_task_rally_formup)(int32_t player, int32_t group_index);            // 0x02
    void (*group_task_advance_to_anchor)(int32_t player, int32_t group_index);       // 0x03, 0x0c
    void (*group_task_disperse_passable)(uint32_t player, int32_t group_index);      // 0x04
    void (*group_task_patrol_shuttle)(int32_t player, int32_t group_index);          // 0x05
    void (*group_task_nudge_stragglers)(uint32_t player, int32_t group_index);       // 0x06
    void (*group_task_wait)();                                                       // 0x07 (no-op)
    void (*group_task_recall_home)(int32_t player, int32_t group_index);             // 0x08
    void (*group_task_recruit_from_storage)(uint32_t player, int32_t group_index);   // 0x09
    void (*group_task_hold)();                                                       // 0x0b (no-op)
    void (*group_task_scatter_random)(int32_t player, int32_t group_index);          // 0x0d
    void (*group_task_loiter_wander)(int32_t player, int32_t group_index);           // 0x0e
    void (*group_task_muster_from_pool)(uint32_t player, int32_t group_index);       // 0x0f,0x10,0x11
    void (*group_task_recruit_from_pool3)(uint32_t player, int32_t group_index);     // 0x12
    void (*group_task_recruit_from_pool4)(uint32_t player, int32_t group_index);     // 0x13
    void (*group_task_disband)(uint32_t player, int32_t group_index);                // 0x14
    void (*group_task_attack_nearest_defended)(int32_t player, int32_t group_index); // 0x15
    // ITS PROTOTYPE WAS FOUR PARAMETERS UNTIL 2026-08-05 (EN v224). Ghidra had over-counted from the
    // callee-save PUSH EBX at 0x004ea07c; ECX is never referenced in the body, and the sole call site
    // -- the arm below -- leaves the task_code in EBX and a player_data byte offset in ECX, neither
    // of which is an argument. ghidra_findings 2026-08-05-1343-1.
    void (*group_task_engage_target)(int32_t player, int32_t group_index);         // 0x16
    void (*group_task_drain_reserve_attack)(uint32_t player, int32_t group_index); // 0x17
    void (*group_task_attack_random_target)(int32_t player, int32_t group_index);  // 0x18

    // ---- batch C layer 2: llm_strat_ai_group_task_step's own callees ----
    //
    // Pops task slot 0 and shifts the backlog down; player_data only.
    void (*group_task_dequeue)(int32_t player, int32_t group_index);
    // "Is this player's storage/dock slot busy?" -- the recruit arms' gate. Returns a full dword.
    int32_t (*dock_slot_is_busy)(int32_t player, int32_t slot);
    // The four PROGRESS PREDICATES the step machine polls. Each returns non-zero to mean "this task
    // is finished, dequeue it"; a zero return leaves the task running and makes task_step return 0.
    int32_t (*group_check_arrival_status)(uint32_t player, int32_t group_index);
    // RETURN POLARITY IS THE NEGATION OF ITS NAME: 1 when NO member is within dist_sq < 0x64 of the
    // group centroid, 0 as soon as one is. Its return type was committed `void` until 2026-08-05
    // (EN v225) even though this is its only caller and the caller both TESTs and returns the value;
    // the constant-1 exit lives at 0x004d66d1, eight bytes BEFORE the function's own entry, inside
    // the preceding function's shared epilogue. ghidra_findings 2026-08-05-1343-2.
    int32_t (*group_check_unit_near_centroid)(int32_t player, int32_t group_index);
    // `owner_mask` is passed in EBX. Task code 6 passes 0x40, task code 7 passes 0xc0 -- the same
    // owner-nibble mask family the target-ref predicates use.
    int32_t (*group_area_scan_hostile)(uint32_t player, int32_t group_index, uint8_t owner_mask);
    int32_t (*group_all_units_settled)(int32_t player, int32_t group_index);

    // ---- llm_strat_ai_group_home_guard_replenish's three own callees (RI-AI batch C layer 2) ----
    void (*group1_drain_to_group0)(uint32_t player_id);
    void (*route_unit_to_home_storage)(uint32_t player, int32_t unit_id);
    void (*group_reposition_members)(uint32_t player, int32_t group_idx);

    // ---- RI-AI batch C layer 2, the 2026-08-05 slice ------------------------------------------
    //
    // llm_strat_ai_group_redistribute_units' four own callees.
    //
    // group_split_off_create IS THE ONE TO READ THE COMMENT ON. Its fifth argument is a STACK
    // argument (`RET 0x4`) and redistribute's sole call site passes THREE UNINITIALISED FRAME SLOTS
    // for centroid_x / centroid_y / member_count -- see ai_group_redistribute.h. Its own body gates
    // on `member_count != 0` and `member_count <= player_data[p].+0x1249e` (0x004e690b-0x004e691f),
    // so an out-of-range garbage word makes the whole call a no-op; that is what the original relies
    // on without knowing it.
    void (*group_split_off_create)(uint32_t player, int32_t centroid_x, int32_t centroid_y,
                                   int32_t source_group_idx, uint32_t member_count);
    // group_split_off_create's own gate (RI-AI batch C layer 4, 2026-08-06). Pure: scans
    // player_data[player].ai_groups[5..] for an existing goal==7 link to target_group_id. Writes
    // nothing (verified against its own disassembly, 0x004d3af2) -- REAL in the shadow arm.
    int32_t (*group_has_split_group_link)(int32_t player_id, uint32_t target_group_id);
    // OUTPUT-POINTER helper, the AI group's mean member position. NOT the same function as
    // llm_strat_ai_group_compute_centroid_0048d36f (a different address with a different second
    // parameter): this one is 0x004d5c17 and takes the GROUP INDEX.
    void (*group_compute_centroid)(int32_t player, int32_t group_index, uint32_t *out_x,
                                   uint32_t *out_y);
    // Flags a unit as group-controlled and issues its move order. Writes `units` directly and reaches
    // llm_strat_order_enqueue, so a site must declare units + the three order regions. Not an escape:
    // its closure contains neither llm_strat_order_schedule nor llm_strat_order_dispatch.
    void (*unit_flag_and_move)(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y);
    void (*group_split_excess_members)(uint32_t player, int32_t group_idx);
    // llm_strat_ai_group_expansion_form_or_repurpose's one own callee that had no slot yet.
    // OUTPUT-POINTER: writes the indexed player's mother-building tile through the two args.
    uint32_t (*bldg_find_mother_position_indexed)(int32_t player, uint32_t *out_x, uint32_t *out_y);

    // llm_strat_ai_unit_order_move_with_bump's own callees. THIS FUNCTION LIVES OUTSIDE THE AI
    // ADDRESS BAND (0x0046ae0a, in the order layer) and is the one member of the batch whose callees
    // are the ORDER path rather than the group machinery.
    void (*order_scratch_reset)();
    void (*order_scratch_set_field)(int32_t index, int32_t value);
    // NOTE THE PARAMETER NAMES. The committed Ghidra prototype calls the EBX/ECX pair
    // `param0`/`order_code`, but the sibling llm_strat_order_dispatch calls the SAME pair
    // `op_code`/`arg`, and the dispatch reading is the right one: llm_strat_unit_order_move loads
    // cfg Unit's move_op_code into EBX and move_op_arg into ECX for both functions. Named here for
    // what the call sites mean rather than for what the older prototype says.
    int32_t (*order_enqueue)(uint16_t unit_index, uint16_t owner_and_kind, int16_t op_code,
                             uint16_t arg);
    void (*unit_notify_status)(uint32_t player, int32_t unit_index, uint32_t status_code);
    // OUTPUT-POINTER, pure otherwise: writes zero tracked regions (derive_write_closure, 1 reachable
    // function). Runs FOR REAL in the shadow arm.
    void (*bldg_calc_placement_corner_from_center)(uint16_t unit_index, int32_t center_x,
                                                   int32_t center_y, uint32_t *out_col,
                                                   uint32_t *out_row);
    int32_t (*bldg_placement_check_and_preview)(int32_t origin_x, int32_t origin_y,
                                                int32_t building_index);
    // Audible. Its subtree writes DAT_00669928 (the sound-channel table) and talks to the sound
    // driver, which no restore can undo.
    void (*snd_play)(int32_t sound_id, int32_t volume);

    // ---- RI-AI batch C layer 3 (2026-08-06) -- the four unit-role classifiers, shared between
    // llm_strat_ai_route_unit_to_home_storage and llm_strat_ai_group_reposition_members. Pure; all
    // four run FOR REAL in the shadow arm.
    int32_t (*unit_is_ai_soldier)(uint32_t player, uint32_t unit_id);
    int32_t (*unit_is_ai_ground)(uint16_t player, uint32_t unit_id); // NOTE narrower player width --
                                                                     // the committed prototype's own
    int32_t (*unit_is_ai_plane)(uint32_t player, uint32_t unit_id);
    int32_t (*unit_is_ai_heli)(uint32_t player, uint32_t unit_id);
    // Order-state / order-pending reads. Pure; REAL in the shadow arm.
    int32_t (*unit_order_state_is_settled)(int32_t player, int32_t unit_idx);
    uint32_t (*unit_is_order_pending)(uint32_t player, uint32_t unit_id);
    int32_t (*unit_state_is_in_transit)(uint32_t player, uint32_t unit_id);
    int32_t (*unit_state_is_in_storage_transit)(uint32_t player, uint32_t unit_id);
    // Enqueues a unit's exit-from-storage order through the LOCAL order queue (own closure: the three
    // order regions + _G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER + units -- checked in isolation, no
    // order_schedule/order_dispatch anywhere in it). Not an escape; REAL in the shadow arm.
    // `player` widened to uint32_t by SIM-READY 2026-08-07 (see order_attack_*_enqueue above).
    void (*unit_order_exit_storage_enqueue)(uint32_t player, uint32_t unit_idx, int32_t storage_idx,
                                            uint32_t param_4, uint32_t param_5);
    // llm_strat_ai_group_reposition_members's SMALL-branch callee. Untranslated; own closure is
    // units + the three order regions (checked in isolation, immediate lane). REAL in the shadow arm.
    void (*group_scatter_to_passable_tile)(uint32_t player, int32_t x, int32_t y);
    // llm_strat_ai_group_task_attack_{nearest_defended,drain_reserve_attack}'s shared query callee.
    // Untranslated and not decompiled (see ai_group_task_attack.h uncertainties) --
    // presumed pure (a "find nearest" query); REAL in the shadow arm pending re-derivation.
    int32_t (*group_find_nearest_building_of_types)(int32_t player_id, int32_t query_x, int32_t query_y,
                                                    uint32_t building_type_1, uint32_t building_type_2,
                                                    uint32_t building_type_3, uint32_t building_type_4);
    // llm_strat_ai_group_task_attack_random_target's building-candidate collector. Writes
    // _G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST (own closure, checked). REAL in the shadow arm.
    void (*group_collect_buildings_of_types)(int32_t player_id, uint32_t unused_edx_slot,
                                             uint32_t unused_ebx_slot, uint32_t building_type_1,
                                             uint32_t building_type_2, uint32_t building_type_3,
                                             uint32_t building_type_4);

    // ---- RI-AI batch C layer 3 (2026-08-06) -- the recruit/lifecycle/movement task-machine
    // arms' own callees. The write closures derived for all nine: none reaches an escape
    // (snd_play / bldg_placement_check_and_preview); all nine REAL in the shadow arm.
    //
    // "Find one unit from this source group" pair, muster_from_pool's two pick policies (task_code
    // 0x10/0x11). Pure query, zero writable closure -- returns a unit index in EAX.
    uint32_t (*group_find_slowest_unit)(int32_t player, int32_t src_group);
    uint32_t (*group_pick_best_weapon_unit)(int32_t player, int32_t src_group);
    // Issues a unit's default standing order. Closure is the three order regions ONLY (no player_data
    // /units directly) -- muster_from_pool's pool-2 arm and disband's drain loop.
    // `player` widened to uint32_t by SIM-READY 2026-08-07 (see order_attack_*_enqueue above).
    void (*unit_issue_default_order)(uint32_t player, int32_t unit_index);
    // recruit_from_storage's match-found tail: assigns a unit into a group by role/storage slot.
    // Closure: the three order regions + _G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER + player_data + units.
    void (*unit_group_assign_by_type)(uint32_t player, int32_t group_index, int32_t unit_id,
                                      int32_t storage_slot);
    // rally_formup / scatter_random's tail-call/forwarding-thunk targets -- the real workers, one
    // level deeper than the thunk translates. rally_formup_worker's own direct write is
    // player_data; scatter_random_worker's is reached entirely through its own callees (RNG + order +
    // units).
    void (*group_rally_formup_worker)(int32_t player_id, int32_t group_index);
    void (*group_scatter_random_worker)(int32_t player_id, int32_t group_index,
                                        uint32_t class_or_radius);
    // scatter_random_worker's own callee (RI-AI batch C layer 4, 2026-08-06). OUTPUT-POINTER:
    // writes only through out_x/out_y, but draws channel 2 of _G_LLM_STRAT_RNG_STATE
    // (llm_rand_state_advance(2)) before the sin/cos pair -- confirmed by decompiling 0x004d5eaf,
    // not inferred from the name. A site reaching it must declare _G_LLM_STRAT_RNG_STATE or the
    // arm consumes the channel a second time on top of the original's draw.
    void (*random_point_near)(int32_t x, int32_t y, int32_t radius, int32_t *out_x, int32_t *out_y);
    // loiter_wander's channel-2 PRNG draw (the sin/cos jitter angle). Direct write:
    // _G_LLM_STRAT_RNG_STATE, nothing else.
    double (*rand_state_advance)(int32_t channel);
    // loiter_wander's range-reduced sin/cos pair. BOTH take their argument implicitly on ST0 in the
    // original (Ghidra's watcomcpp cspec does not model FPU-stack PARAMETERS, unlike the ST0 RETURN
    // fix from 2026-07-07) -- there is no committed prototype/mh_calls.gen.h shape for "argument via
    // ST0", and this is the ONLY ST0-argument call site in the whole committed call surface (checked:
    // 0 other functions in dll_call_protos.json use ST0 as a param storage), so a hand-written naked
    // adapter (mh::ai::detail::call_f64_st0_arg, ai_state.cpp) is the right-sized fix rather than a
    // case for extending gen_dll_calls.py's shape-derivation engine for a population of one. Pure:
    // zero writable closure either way.
    double (*math_fsin_reduce_loop)(double angle);
    double (*math_cos_impl)(double angle);

    // ---- RI-AI batch C layer 3 (2026-08-06) -- the layer's last callee. ----
    // group_task_advance_to_anchor's mover. Reads back the member id list that its caller has just
    // published in _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST (see ai_group_task_formation.h) and moves
    // that formation toward (target_x, target_y). Closure: the three order regions +
    // _G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER + units -- no escape; REAL in the shadow arm.
    // `unused_param2`/`unused_param3` carry the caller's centroid and keep the committed prototype's
    // own parameter names rather than a guess at what the callee does with them.
    void (*group_move_formation_rotating)(uint32_t player, int32_t unused_param2,
                                          int32_t unused_param3, int32_t target_x, int32_t target_y);

    // ---- RI-AI batch B layer 2 (2026-08-06) -- update_opponent_relations / recompute_shortage_state
    // / plan_construction's callees. All eleven were already __watcall-prototype-committed
    // (dll_call_protos.json status=ok, so mh::call:: wrappers already existed in mh_calls.gen.h) but
    // absent from this struct and from live_calls/shadow_calls until the translations
    // surfaced them as declared_needs.
    int32_t (*player_score_tier)(int32_t player_idx);
    void (*score_build_categories)(int32_t player);
    int32_t (*unit_weapon_power)(uint32_t player, uint32_t unit_id);
    // Re-derives one player's construction priorities from its cached category scores. Writes
    // player_data only (its own translation, ai_worker_rebalance.cpp).
    int32_t (*rebalance_building_workers)(uint32_t player);
    void (*plan_turret_upgrade)(uint32_t player);
    double (*calc_power_supply_ratio)(int32_t player);
    void (*plan_mine_construction)(int32_t player);
    int32_t (*storage_capacity_short_and_cap_check)(int32_t player);
    void (*react_resource_shortage)(int32_t player);
    void (*maintain_unit_housing)(int32_t player);
    int32_t (*scan_construction_sites)(int32_t player, int32_t category);

    // ---- RI-AI batch E (2026-08-07) ----
    // llm_strat_ai_start_hq_attack_scenario's three callees. Not shadow-armed (irreversible: spawns
    // a real soldier and flips a real diplomacy state each call -- arming would run both twice), but
    // still routed through ai_calls rather than called directly, matching every other AI-cluster
    // outward call. All three already had committed __watcall prototypes (mh_calls.gen.h existed)
    // previously; only the ai_calls wiring was missing.
    uint32_t (*unit_create_soldier)(uint32_t x, uint32_t y, uint16_t a2, uint16_t param_4,
                                    char param_5);
    void (*diplomacy_set_relation)(int32_t player_a, int32_t player_b, uint8_t relation);
    void (*unit_commit_attack_on_enemy_hq)();

    // llm_strat_ai_scr_parse's callees. Also not shadow-armed (the write target is resolved at
    // runtime through the keyword table's own pointer, so a shadow site would have to declare every
    // AI.SCR tunable the table can name -- see ai_scr_parse.cpp).
    //
    // SIMABI-VFS (2026-09-10): these two were `res_bank_get_file_size(char*, uint32_t)` and
    // `res_bank_get_file_ptr` -- the original thunks' shapes, dead second parameter and all, with
    // the returned pointer freed HERE through the vendored CRT. The host entries now name what they
    // do and hand back a COPY; the register-artifact argument and the free are mh.dll's binder's.
    int32_t (*asset_size)(const char *name);
    int32_t (*asset_read)(const char *name, void *dst, uint32_t dst_cap);
    void *(*utils_malloc)(uint32_t size);
    void (*utils_free)(void *p);
    int32_t (*utils_str_cmp_ci)(char *a, char *b);

    // llm_strat_ai_spiral_table_init's qsort comparator. NOT a callable -- a POINTER handed to
    // gc.qsort, exactly like scan_target_sort_cmp above (which IS itself a translated function; this
    // one is an anonymous label with no Ghidra function boundary -- see the dll_addr_manifest.json
    // entry). Never invoked by our own code, so its untranslated body cannot desync either arm.
    void *spiral_offset_sort_cmp;

    // ---- RI-AI batch E (2026-08-07) ------------------------------------------------------
    //
    // llm_strat_ai_player_tick's phase dispatch. player_tick is the per-player strategy driver: it is
    // almost entirely a sequence of gated calls, so twenty of these twenty-one members exist purely
    // to BE that sequence. It is NEVER SHADOW-ARMED (778 reachable functions, 127 escape candidates
    // under the write-closure derivation -- gfx/snd/net/ui all sit in its transitive closure), so
    // none of these is ever reached through shadow_calls(); they are wired there for
    // calls_are_complete() and bound REAL, since stubbing a call an armed site never makes buys
    // nothing and would mislead the next reader.
    int32_t (*GetStartingUnit)(uint32_t race);
    // The engine's unit constructor, the second of the two creation primitives already reachable from
    // this cluster (llm_unit_create_soldier is the other, above). player_tick calls it ONCE per
    // player, on the ai_established == 0 edge, to place the AI's opening unit next to its home tile.
    // Flagged for AI2/AI-CREATE: creation has no order form, so this edge is one of the AI's roster
    // writes that batch D's direct-write rule structurally cannot see.
    uint32_t (*unit_create)(uint32_t x, uint32_t y, uint16_t unit, uint16_t player, uint8_t is_ship);
    uint32_t (*bldg_max_defense_radius_sq)(int32_t player, int32_t x, int32_t y);
    // THE x87 SQUARE ROOT, and it is not the `uint8_t f()` its generated wrapper claims. CRT_004da9f0
    // (sub:crt, dumped 2026-08-07) takes its argument on ST0 and returns on ST0: `FTST` / `SAHF` /
    // `JNC` -> `FSQRT` on the non-negative arm, and on the negative arm it pops, calls the math error
    // handler at 0x004e91d0 and returns AL=1. The AL result is the ERROR FLAG, not the value, which
    // is why mh_calls.gen.h's derived `uint8_t` shape cannot be used for it -- like
    // math_fsin_reduce_loop/math_cos_impl above, there is no committed shape for an ST0 ARGUMENT. Its
    // one call site in this cluster feeds it a squared distance, so the error arm is unreachable
    // there; player_tick reproduces the whole FILD/FSQRT/trunc/FISTP chain inline instead of calling
    // it (see ai_player_tick.cpp), so no member is declared for it.
    void (*resource_spend_rate_update)(int32_t player);
    void (*update_opponent_relations)(uint32_t                                       assessed_player,
                                      mh::game::mh_llm_strat_ai_opponent_assessment *out);
    void (*recompute_shortage_state)(uint32_t player_idx);
    void (*plan_unit_training)(int32_t player);
    void (*plan_construction)(uint32_t player_idx);
    void (*scan_bldg_repair_upgrade)(int32_t player);
    void (*order_collect_available_projects_thunk)(int32_t player);
    void (*bldg_queue_process)(uint32_t player_id);
    void (*group_home_guard_replenish)(int32_t player_id);
    void (*group_form_patrol)(int32_t player_id);
    void (*group_form_surplus_from_pool4)(int32_t player_id);
    void (*group_form_standby_from_pool3)(int32_t player_id);
    void (*group_expansion_form_or_repurpose)(uint32_t player_id);
    void (*army_milestone_advance_or_attack)(uint32_t player);
    void (*group_redistribute_units)(uint32_t player, int32_t group_index);
    void (*invasion_launch_attack_group)(uint32_t player);
    void (*unit_order_move_with_bump)(uint32_t player, int32_t unit_idx, uint32_t order_arg0,
                                      uint32_t order_arg1);

    // llm_strat_ai_active_unit_tick's own callees. UNLIKE player_tick, this one IS SHADOW-ARMED:
    // derive_write_closure gives it 48 reachable functions, ZERO escape candidates and an eleven-
    // region closure, so every call below runs FOR REAL in its arm and the site binds live_calls()
    // rather than shadow_calls() (see ai_active_unit_tick.cpp for why that is the correct set here
    // and not a shortcut). That includes commit_attack_order, which the engage sites above stub:
    // "escape" is a property of a call RELATIVE TO A SITE's declared regions, not of the call, and
    // this site declares units/buildings/player_data/the order regions that make it restorable.
    int32_t (*holding_pen_scan_targets)(int32_t player, uint32_t pen_index);
    int32_t (*target_list_invalidate_by_id)(int32_t player, int32_t target_id);
    int32_t (*target_list_refresh_mothers)(int32_t player);
    int32_t (*target_list_scan_visible_enemies)(uint32_t player);
    uint8_t (*group_seed_resolved_target)(int32_t player_id, int32_t group_index);
    void (*attack_candidate_add)(int32_t player, int32_t unit_index);
    // Returns a RING RADIUS, used to index spiral_ring_cell_counts -- not a distance in tiles.
    int32_t (*building_defense_weapon_range)(int32_t player, int32_t building_index);
    void (*scan_target_list_sort)(void *base, uint32_t count);
    void (*group_enter_hold)(int32_t player_id, int32_t group_index);

    // llm_strat_ai_unit_commit_attack_on_enemy_hq's three callees. That function is translated this
    // session but NOT armed: its own direct write set is EMPTY -- everything it does happens through
    // the two order calls below, whose closure reaches llm_strat_order_dispatch plus two UI escapes.
    // A site there would restore and compare regions neither arm writes, i.e. it would go green
    // without comparing anything (the vacuous-oracle class). Verified offline only.
    uint8_t (*unit_select_weapon)(uint16_t player, int32_t unit_index, uint32_t target_mask);
    // `player` WIDENED to uint32_t 2026-08-24. The Ghidra prototype used to declare it `ushort`,
    // but the body homes the full DWORD of EAX at 0x0046bfb1 -- the register carries the packed
    // `player & 0xffff | 0x40` the whole attack-order family marshals, so the high half is the
    // order flags, not padding. Five siblings had the identical defect. Behaviour is unchanged
    // (the thunk always passed the full register); the type now says what is really in it.
    void (*unit_order_attack_building_reposition)(uint32_t player, int32_t unit_index,
                                                  uint32_t target_ref, int32_t target_index,
                                                  uint32_t weapon_id);
    // PARAMETER NAMES READ OFF THE CALL SITE, not off the committed prototype (which says
    // param_1/param_2/a2/param_4/param_5). At 0x004ba7be-0x004ba7d9 the register loads are
    // MOVZX EBX,<building>.x and MOVZX ECX,<building>.y with a literal 0 pushed -- so the EBX/ECX
    // pair is the destination TILE, not a flag pair, and the stack argument is a mode/flags slot
    // this cluster's one call site always passes 0 for. Named for the tile; the fifth keeps a
    // deliberately non-committal name because one site cannot establish what a 0 means.
    void (*unit_order_move)(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y,
                            uint32_t arg5_always_zero_here);

    // The invasion-reinforcement family. All three are translated; the two AI ones stay
    // routed through this struct anyway (the cluster-wide convention -- llm_strat_ai_notify_map_changed
    // and llm_strat_ai_score_build_categories are translated and still gc. members), which is also
    // what lets net_selftest bind them to recording stubs and test the spawner's SELECTION and
    // ROUND-ROBIN independently of what the two leaves compute.
    uint32_t (*score_reinforcement_unit)(int32_t player, int32_t unit_proto_id);
    void (*create_reinforcement_unit)(uint32_t x, uint32_t y, uint32_t unit_proto_id, uint16_t player);
    // The third of the family, and player_tick's invasion-force branch calls it too. RETURNS the
    // spawn count; BOTH call sites discard it, and so do both of our translations -- kept in the
    // signature because the original's `MOV EAX,0x64` / `MOV EAX,ECX` tails are what a promotion
    // would have to reproduce, and a `void` member here would quietly erase that.
    int32_t (*invasion_spawn_reinforcements)(int32_t player_idx);
    // llm_strat_ai_unit_should_abandon_target @0x004ec84d. Already translated (batch A,
    // ai_abandon_target.cpp) but it had no slot, so active_unit_tick's first draft reached its C++
    // directly. Routed through the struct instead, for ATTRIBUTION rather than purity: with the
    // direct call, a divergence at the active_unit_tick site could have come from either body, and
    // the site's own log line would still have named only this one.
    int32_t (*unit_should_abandon_target)(uint32_t player, int32_t unit_index);
    // A PROVEN NO-OP: llm_debug_log_msg_stub @0x004ee8df is `PUSH 4 / CALL assert_stack_capacity /
    // RET` and nothing else -- the developer log it once fronted is gone from the shipped build. It
    // was a member here (and a sim host-table entry) until SIMABI-HOOKS 2026-09-10; the call site in
    // ai_reinforcements.cpp now carries the note instead. NOTE the member removal shifts every
    // POSITIONAL binding after it -- both live_calls() and shadow_calls() lost the same line.

    // ---- RI-AI batch D / AI1D (2026-08-28) ----
    // Ten outward edges batch D needs and no earlier batch did. APPENDED AT THE END on purpose:
    // both call sets are POSITIONAL aggregate initialisers (see live_calls()/shadow_calls()), so a
    // member inserted in the middle silently re-pairs every binding after it.
    //
    // The attack-commit pair. NOTE these are NOT the `order_attack_building_enqueue` /
    // `order_attack_unit_enqueue` members above: those bind
    // llm_strat_unit_order_attack_building_enqueue / _unit_enqueue, different functions from the
    // three below, and reusing one would have been a silent wrong callee.
    uint32_t (*estimate_weapon_damage)(int32_t attacker_player, int32_t attacker_unit_index,
                                       uint32_t target_ref, int32_t target_index);
    void (*order_attack_target_enqueue)(uint32_t player, int32_t unit_idx, uint32_t target_player,
                                        int32_t target_index, uint32_t weapon_idx);
    void (*order_attack_target_alt_enqueue)(uint32_t player, int32_t unit_idx, uint32_t target_player,
                                            int32_t target_index, uint32_t weapon_idx);
    void (*order_attack_building_reposition_alt_enqueue)(uint32_t player, int32_t unit_idx,
                                                         uint32_t target_player,
                                                         int32_t  target_bldg_idx,
                                                         uint32_t weapon_idx);
    // llm_strat_ai_players_tick's six phase edges. `passive_engage_tick` is the one that is NOT an
    // AI function: players_tick dispatches it for players whose ai_enabled is 0, which is why that
    // function drives HUMAN players' opportunistic fire (AI2's finding (1)).
    void (*recompute_map_influence)(int32_t player);
    void (*turret_threat_rescan)(uint32_t player);
    void (*player_tick)(int32_t player);
    void (*ai_unit_group_tick)(int32_t player);
    void (*active_unit_tick)(int32_t player);
    void (*passive_engage_tick)(int32_t player);
};

// Resolved from the state region registry on EVERY call -- see the header comment on why this is
// by value and why nothing is cached.
ai_state        state();
const ai_calls &live_calls();

// THE SHADOW ARM'S CALL SET, and it is deliberately NOT "everything inert".
//
// The rule (reimpl-loop, "what shadow mode cannot do") is that an outward call is safe to run for
// real in the second arm exactly when its whole write-set lies INSIDE the site's declared regions,
// because then the restore between the arms undoes it. Stubbing such a call does not make the run
// safer -- it GUARANTEES a divergence, since the original arm made the write and ours did not. So
// this set stubs exactly the four calls that ESCAPE and leaves everything else real:
//
//   escaping, stubbed  -- commit_attack_order / _alt and the two order_attack_*_enqueue. They
//                         mutate `units`/`buildings`/`player_data` and put an order into the game;
//                         a second one would double-issue it.
//   in-region, REAL    -- engage_candidate_add, engage_sort_candidates_by_dist and
//                         engage_partition_turret_candidates write only the engage scratch and its
//                         count, both declared by every site that reaches them.
//   pure, REAL         -- target_ref_is_alive, the two weapon predicates, and
//                         bldg_find_by_ai_build_and_type read the rosters and return a value.
//                         Stubbing a pure predicate feeds the body different inputs and
//                         manufactures a divergence rather than silencing a side effect.

// True when EVERY slot of both call sets is bound. Both are positional aggregate initialisers over a
// struct of pointers, so a member added to ai_calls and wired into only one of them is silently
// zero-filled in the other -- a null that misbehaves only once its site is armed AND reached, which
// reads as a crash in the body under test. install_shadow() refuses to arm when this is false.
// `net_selftest.exe aitest` asserts it too, so the hole is caught with no game running.
bool calls_are_complete();

namespace detail {

} // namespace detail

// ---- logging ------------------------------------------------------------------------------------
//
// The AI module's own line sink. The per-call trace this used to drive went with the differential
// oracle (F2D); set_logger/ai_say remain because the module still has things to say.
void set_logger(void (*fn)(const char *));
void ai_say(const char *fmt, ...);
int  trace_budget(); // remaining traced calls; decremented by the arms

// LT1C (2026-09-02). The OWNER accessor for _G_LLM_STRAT_AI_GRID_WRAP_MASK -- this module binds it
// writable (ai_store::own.grid_wrap_mask), and llm_scan_masked_table_for_empty_cell's mh::sim
// translation also writes it on entry. Per ST1 ("the order counts come from their OWNER, not a
// second binding"; libmh/orders/order_queue.h:142's one-region-two-bindings warning), the sim unit
// reaches the region through THIS accessor instead of binding it a second time. The write packing
// the caller owes: two byte stores, low = height-1, HIGH = width-1 (X in the HIGH byte -- documented
// backwards once, corrected against 0x004b4be9/0x004b4bf2; see this header ~line 454), bits 16-31
// preserved -- the (*p & 0xffff0000u) | ... idiom ai_grid.cpp uses at three sites.
uint32_t *grid_wrap_mask();

} // namespace mh::ai
