//
// sim/sim_unit_recruit.h -- the direct-grant unit order (RI-SIM / SIM1A).
//
// One function: llm_unit_recruit @0x00463ec8 (0x247 bytes).
//
// llm_strat_order_queue_dispatch order-code table case 14/0xe (order_code 0xf3, /llm/E_ORDER_CODE
// member UNIT_RECRUIT -- see sim_order_dispatch.h's decode table, which already names this exact
// case). NOT an order_dispatch case body itself -- it is a plain __watcall(EAX,EDX) function the
// dispatcher (or another caller) invokes directly with (player, unit_type_id); this file translates
// only the callee, not the dispatch site. Full per-category housing/headcount cap checks, then
// spawns via the SAME llm_strat_unit_spawn_docked/llm_strat_unit_add_docked pair the normal
// building-production-completion path uses (sim_bldg_finish_order.cpp) -- reads as an instant/
// direct-grant unit order distinct from the normal factory production queue.
//
// ---- HAZARD: the units[player][0].order QUEUE-LENGTH IDIOM (NOT the unit-order-state meaning) -----
// unit::order (mh_map_object_unit::order) is documented in mh_structs.gen.h as "pending/queued
// state" for a REAL unit slot. For the DUMMY slot 0 of a player's roster, both this function and
// llm_unit_create_soldier (@0x00463ac6, tmp/decomp/llm_unit_create_soldier_00463ac6.asm, sibling
// case in the same order-code family, NOT reimplemented in this closure -- called only via
// mh::call:: elsewhere, e.g. ai/ai_state.h's live_ai_hq_attack_scenario_calls) instead (ab)use it as
// a per-player COUNTER: this function's opening guard reads units[player][0].order+1 against a 0x5a
// (90) cap and rejects with RECRUIT_ERR_QUEUE_FULL if over, then on the grant path increments it by
// exactly `STOP_TO_DEFAULT` (sim_order_enqueue.h's UNIT_STATE_STOP_TO_DEFAULT == 1) at
// 0x00464076-0x00464086 (`INC word ptr [... + 0xdd8c4c]`). llm_unit_create_soldier's own body does
// the BYTE-IDENTICAL `INC word ptr [... + 0xdd8c4c]` at its own address (0x00463c27), confirming this
// is one shared idiom across the family, not a one-off. Preserve the add-by-one verbatim; do not
// reinterpret it through the unit-order-state enum (STOP_TO_DEFAULT here is just the literal 1, not
// a state assignment).
//
// ---- HAZARD: the housing-category branch, using Ghidra's own cfg_enum_E_UNIT_TYPE ordering --------
// Unit[unit_type_id].type (cfg_final_struct_Unit::type, cfg_enum_E_UNIT_TYPE) is compared against
// four thresholds that exactly match sim_unit_type_predicates.h's already-pinned values (re-derived
// there from FIVE other functions' own CMP chains, cross-checked against mh::ai::ai_state.h's
// independent pin): UNIT_TYPE_A_WALKER=0xb, UNIT_TYPE_A_HELI=0xf, UNIT_TYPE_A_PLANE=0x11,
// UNIT_TYPE_A_HELI_MOTHER=0x13. This function's own branch structure (see the .cpp) is:
//   type == UNDEFINED (0)                    -> no housing check at all
//   0 < type < A_WALKER (1..0xa)              -> SOLDIER housing check (headcount-weighted)
//   A_WALKER <= type < A_HELI (0xb..0xe)       -> VEHICLE housing check (simple used>=cap)
//   A_HELI <= type < A_PLANE (0xf..0x10)       -> HELI housing check
//   A_PLANE <= type < A_HELI_MOTHER (0x11..0x12) -> PLANE housing check
//   type >= A_HELI_MOTHER (0x13+)              -> no housing check at all
// Read directly off the CMP/JC/JBE chain at 0x00463f08-0x00464016; use the enum's own member names,
// per naming-convention rule 17a -- not a re-derived category scheme.
//
// ---- HAZARD: two DIFFERENT llm_strat_unit_housing_stats members feed two DIFFERENT checks ----------
// The soldier branch compares cap_prev_soldiers against used_soldiers PLUS the cfg unit's own
// soldier_count (a unit can occupy more than one soldier slot); the other three branches compare
// cap_prev_* against used_* alone with no addend. Read as-is, not normalized.
//
// ---- HAZARD: dispatches to EITHER spawn_docked OR add_docked on Unit[unit_type_id].move_op_code ----
// == 0xf (mh_structs.gen.h's own field comment already documents this EXACT comparison at this
// function's own address, 0x0046408e, calling 0xf the "ground" class -- a different domain from
// cfg_enum_E_UNIT_TYPE despite sharing the numeral). Both callees are ORIGINAL functions (mh::call),
// NOT reimplemented in this closure; each success path (return != 0) calls
// llm_strat_ai_notify_unit_lifecycle(player, unit_type_id, unit_id, 4) IDENTICALLY -- mode 4 = "unit
// just created" per the AI notes' llm_ai_notify_unit_lifecycle note (the AI side's own name for this
// same function). A zero return from either spawn call means "spawn failed" and short-circuits to
// RECRUIT_ERR_SPAWN_FAILED WITHOUT the notify call and WITHOUT the caller learning which of the two
// paths was tried.
//
// ---- DECLARED NEED 1: sim_view has no member for RID_STRAT_UNIT_HOUSING_STATS -----------------------
// The region is already fully registered (RID_STRAT_UNIT_HOUSING_STATS, addr::_G_LLM_STRAT_UNIT_HOUSING_STATS
// = 0x00c38530, OWN_ISLAND | MF_VIEW | MF_SAVE, 512 bytes) and already bound on the AI side
// (ai/ai_state.h's `housing_stats` alias + `ai_view::unit_housing`, ai_state.cpp:
// `s.read.unit_housing = ptr<const housing_stats>(RID_STRAT_UNIT_HOUSING_STATS);`) -- this is a
// SECOND legitimate binding of the same RID through the registry, exactly the precedent
// sim_state.h's own header comment sanctions (order-queue's `horizon`). This TU references
// `v.unit_housing` as if sim_view already had it. Needed central addition, mirroring ai/ai_state.h:
//   sim_state.h, alongside the other `using` aliases (sim/ deliberately does not depend on ai/, so
//   this duplicates ai_state.h's one-line alias rather than including it):
//     using housing_stats = mh::game::mh_llm_strat_unit_housing_stats;
//   sim_state.h, on sim_view:
//     const housing_stats *unit_housing; // RID_STRAT_UNIT_HOUSING_STATS
//   sim_state.cpp, in state():
//     s.read.unit_housing = ptr<const housing_stats>(RID_STRAT_UNIT_HOUSING_STATS);
//
// ---- DECLARED NEED 2: sim_view has no member for _G_LLM_STRAT_SOLDIERS, and its record type does
// not exist in mh_structs.gen.h at all --------------------------------------------------------------
// _G_LLM_STRAT_SOLDIERS is region-registered (RID_STRAT_SOLDIERS = 228, addr 0x00e0e5a8, 23200 bytes,
// OWN_SHARED | MF_SAVE | MF_PATCH | MF_HASH -- a real, hashed, 66-write-site region elsewhere in the
// image) but the record type Ghidra calls `llm_strat_crew_soldier` (size 0x1d, category
// `/Manual/map`, docs/structs.md lines 1267-1283) has never been added to the struct manifest, so
// there is no `mh::game::mh_llm_strat_crew_soldier` to alias. This function reads only field +0x00
// (`owner_unit`, a `short`) of record [player][0] -- documented in docs/structs.md itself as "in
// record [0]: used-slot count", the SAME reuse-slot-0-as-a-scalar-counter idiom as
// units[player][0].order above; llm_unit_create_soldier's own `INC word ptr [...]` at the identical
// derived address (0x00463c2d, same row/column arithmetic: player*0x5b04 has NOTHING to do with this
// array -- llm_strat_crew_soldier's own row stride is player*0xb54 = player*100*0x1d, confirmed by
// 23200 = 8*100*0x1d) is the cross-check that this is the array's own established idiom, not a
// one-off reading. Needed central addition:
//   mh_structs.gen.h (via the manifest's `structs` list), the full 0x1d-byte record per
//   docs/structs.md: {int16_t owner_unit; uint16_t next_soldier; uint8_t cur_x, cur_y, sprite_frame,
//   start_x, start_y, end_x, end_y; double walk_elapsed; double walk_duration; uint8_t
//   anim_change_count, idle_wander_flag;} -- this function only needs owner_unit, but the whole
//   record should be typed in one pass per Law 3.
//   sim_state.h: `using soldier = mh::game::mh_llm_strat_crew_soldier;`,
//     `inline constexpr int32_t SOLDIERS_PER_PLAYER = 100;` (23200 / 8 / 0x1d == 100, matching
//     docs/symbols.md's own `llm_strat_crew_soldier[8][100]` typing), and on sim_view:
//     `const soldier *soldiers; // [MAX_PLAYERS][SOLDIERS_PER_PLAYER], RID_STRAT_SOLDIERS`.
//   sim_state.cpp: `s.read.soldiers = ptr<const soldier>(RID_STRAT_SOLDIERS);`.
// This TU references `v.soldiers[player * SOLDIERS_PER_PLAYER + 0].owner_unit` as if all of the
// above already existed, with a LOCAL `SOLDIERS_PER_PLAYER = 100` fallback so the arithmetic below is
// at least self-documenting until the central member lands.
//
// ---- DECLARED NEED 3: _G_LLM_STRAT_PLAYER_RACE is not in the region registry AT ALL, and Ghidra
// still types it `undefined4` (docs/symbols.md: `0x00e58361 | _G_LLM_STRAT_PLAYER_RACE | undefined4`)
// -------------------------------------------------------------------------------------------------
// Unlike the two globals above, this one has NO region entry to bind a second time -- it is a
// genuine gap, not a missing accessor on an existing binding. The final else-branch return code
// (RECRUIT_ERR_SOLDIER_HEADCOUNT_RACE1 vs _OTHER) depends on it, read with a single unindexed `CMP
// dword ptr [0x00e58361],0x1` (0x00464046) -- a plain global scalar, NOT a per-player array despite
// the "PLAYER" in the name (it sits among UI-session globals in the symbol table:
// _G_LLM_STRAT_LOCAL_PLAYER_SLOT immediately before it, _G_LLM_STRAT_UI_BLDG_TAB_SELECT_BLOCKED
// immediately after), so it reads more like "the (human/local) player's selected race" than a
// per-recruiting-player value. No `cfg_enum_E_RACE` or similar exists in the DTM (searched
// docs/structs.md and mh_structs.gen.h) to type it against, so it is read as a plain int32_t here.
// Needed central addition:
//   dll_addr_manifest.json: `_G_LLM_STRAT_PLAYER_RACE` @ 0x00e58361, a NEW region (no existing RID
//   covers it) sized 4 bytes; classify OWN_SHARED vs OWN_ISLAND and MF_* flags per the same audit the
//   other UI-session globals in this neighborhood already went through -- not guessed here.
//   sim_state.h, on sim_view: `const int32_t *player_race; // _G_LLM_STRAT_PLAYER_RACE`.
//   sim_state.cpp: `s.read.player_race = ptr<const int32_t>(RID_STRAT_PLAYER_RACE);` (new RID).
// Until all three needs land, this TU will not compile on its own -- by design, so the gaps stay
// visible rather than silently patched over with a raw offset (translator-brief rule 2 / the loop's
// "declare it" discipline).
//
#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT, UNIT_TYPE_A_HELI/H_HELI/A_PLANE/H_PLANE
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_WALKER, UNIT_TYPE_A_HELI_MOTHER

namespace mh::sim {

// UNIT_TYPE_UNDEFINED (cfg_enum_E_UNIT_TYPE member 0) now lives in
// sim_unit_type_predicates.h, included above, with the derivation and the reason it was
// hoisted out of this file (two definitions in one TU once a second header needed it).

// Unit::move_op_code's own "ground" class value -- a DIFFERENT domain from cfg_enum_E_UNIT_TYPE
// despite sharing the numeral 0xf; mh_structs.gen.h's field comment on move_op_code already documents
// this EXACT comparison at this function's own address (0x0046408e), so this is citing an existing
// Ghidra annotation, not inventing one. move_op_code is a plain uint8_t field (no enum type applied).
inline constexpr uint8_t MOVE_OP_CODE_GROUND = 0x0fu;

// This function's own player-role headcount cap (0x5a == 90), and the soldier-headcount cap (100)
// used further down -- both bare CMP immediates with no Ghidra enum backing them (unlike the
// UNIT_TYPE_* thresholds above, these are not members of any discriminated domain, just literal
// capacity constants for this one function).
inline constexpr int32_t RECRUIT_ORDER_QUEUE_CAP      = 0x5a;
inline constexpr int32_t RECRUIT_SOLDIER_HEADROOM_CAP = 100;

// SOLDIERS_PER_PLAYER now lives in sim_state.h alongside UNITS_PER_PLAYER/STORAGE_PER_PLAYER (the
// conductor landed `soldiers`/`soldier_at()` for this slice -- see DECLARED NEED 2 above).

// llm_unit_recruit's own result codes -- READ OFF THE ASM (the CMP/MOV-immediate/JMP-to-return chain
// at 0x00463efc-0x00464106), NOT from a Ghidra enum: none exists for this domain (searched
// docs/structs.md and mh_structs.gen.h for an order-result/status enum and found none), so per
// naming-convention rule 17a this is the "no enum exists" case -- named locally rather than left as
// bare literals at each return site, and NOT proposed as a shared enum since nothing else in the
// closure was found returning through the same convention.
inline constexpr int32_t RECRUIT_OK                          = 0;
inline constexpr int32_t RECRUIT_ERR_QUEUE_FULL              = 0x6; // order-queue-length cap (see hazard note)
inline constexpr int32_t RECRUIT_ERR_VEHICLE_CAP             = 0xf;
inline constexpr int32_t RECRUIT_ERR_SOLDIER_CAP             = 0x10;
inline constexpr int32_t RECRUIT_ERR_HELI_CAP                = 0x11;
inline constexpr int32_t RECRUIT_ERR_PLANE_CAP               = 0x12;
inline constexpr int32_t RECRUIT_ERR_SPAWN_FAILED            = 1;
inline constexpr int32_t RECRUIT_ERR_SOLDIER_HEADCOUNT_RACE1 = 0xa;  // _G_LLM_STRAT_PLAYER_RACE == 1
inline constexpr int32_t RECRUIT_ERR_SOLDIER_HEADCOUNT_OTHER = 0xaf; // _G_LLM_STRAT_PLAYER_RACE != 1

// "mode 4 = unit just created" per the AI notes' llm_ai_notify_unit_lifecycle note; no named constant
// exists elsewhere in the closure for this (sim_bldg_finish_order.cpp / sim_order_dispatch_bldg.cpp
// both pass their own mode (2, "production complete") as a bare literal too) -- kept consistent with
// that precedent rather than introducing a lone named constant for one call site.

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as sim_bldg_finish_order.h / sim_unit_passive_engage.h: a direct
// mh::call:: inside a detail:: body reaches into the live game image, which makes the body
// untestable by net_selftest.exe simtest. All four are ORIGINAL functions outside this batch -- none
// need reimplementing here; live_unit_recruit_calls() is the only binder.
struct unit_recruit_calls {
    // llm_strat_unit_housing_count_add(player, unit_type_id) -- register order EAX=player,
    // EDX=unit_type_id per the asm (0x0046406a-0x00464071); mh_calls.gen.h's own generic parameter
    // names ("housing_id, unit_index") do not reflect this.
    void (*unit_housing_count_add)(int32_t player, int32_t unit_type_id);
    // llm_strat_unit_spawn_docked(unit_proto_id, player, probe_slot=0) -- the GROUND-class path.
    int32_t (*unit_spawn_docked)(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot);
    // llm_strat_unit_add_docked(unit_proto_id, player, probe_slot=0) -- the non-ground path; NOTE its
    // first arg is the FULL uint32_t unit_type_id (not masked to uint16_t like spawn_docked's), read
    // directly off 0x004640d6 (`MOV EAX,dword ptr [...]`, no MOVZX).
    int32_t (*unit_add_docked)(uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot);
    // llm_strat_ai_notify_unit_lifecycle(player, unit_type_id, unit_id, mode) -- called identically
    // from both success paths with mode=4 ("unit just created").
    void (*ai_notify_unit_lifecycle)(uint16_t player, uint16_t unit_type_id, uint32_t unit_id,
                                     uint32_t mode);
};

const unit_recruit_calls &live_unit_recruit_calls();

namespace detail {

// llm_unit_recruit @0x00463ec8. See the header hazards above for the full derivation; summary:
//   1. units[player][0].order+1 > 0x5a (90)                -> RECRUIT_ERR_QUEUE_FULL
//   2. per-category housing cap (see the cfg_enum_E_UNIT_TYPE chain above) -> the matching *_CAP code
//   3. soldier-count != 0 AND projected headcount >= 100    -> race-dependent RACE1/OTHER code
//   4. else: housing_count_add, order += STOP_TO_DEFAULT, dispatch to spawn_docked (move_op_code ==
//      GROUND) or add_docked (otherwise); a zero unit-id return -> RECRUIT_ERR_SPAWN_FAILED, else
//      notify_unit_lifecycle(..., 4) and return RECRUIT_OK.
int32_t unit_recruit(const sim_view &v, sim_store &own, const unit_recruit_calls &c, uint32_t player,
                     uint32_t unit_type_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_recruit_calls(). Matches the original's
// committed __watcall(EAX,EDX) shape (sig_llm_unit_recruit).
int32_t unit_recruit(uint32_t player, uint32_t unit_type_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
