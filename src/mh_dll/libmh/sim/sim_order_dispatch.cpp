//
// sim/sim_order_dispatch.cpp -- the LOOP, the KIND ROUTER, the UNIT/STORAGE path, the RETENTION
// (compaction) logic and the tail of llm_strat_order_queue_dispatch @0x00466892 (RI-SIM / SIM1C).
//
// Translated from the DISASSEMBLY (tmp/decomp/llm_strat_order_queue_dispatch_00466892.asm), address
// range 0x00466892-0x0046732f plus the tail 0x0046997f-0x00469995. The two table regions --
// 0x00467330-0x004695cf (building) and 0x004695d4-0x0046997e (admin + the kind-0xf0 lockstep body) --
// are the two sibling TUs; this file CALLS them and never reproduces an arm.
//
// The Ghidra `.c` draft was read only as a cross-check. It agrees with this file's control flow
// everywhere EXCEPT the two places noted at their lines: it renders the energy gate as
// `0.0 < energy` (wrong on NaN -- see the gate) and it hoists the B/C branch chain into one
// compound condition (equivalent, but it hides which comparison is which address).
//
// ---------------------------------------------------------------------------------------------
// THE COMPACTION CURSOR IS THE WHOLE POINT
// ---------------------------------------------------------------------------------------------
//
// `kept` ([EBP-0x20]) starts at 0, is advanced at EXACTLY ONE site -- the unit path's retain branch
// at 0x00466bfe/0x00466c3b -- and its FINAL value, not the loop bound, is what the tail stores to
// _G_LLM_STRAT_ORDER_QUEUE_COUNT at 0x00469987. Every other path executes its record and DROPS it.
// A version that drained the queue and stored 0 passes an idle test and loses every deferred order
// under load.
//
// THE LOOP BOUND IS RE-READ EVERY ITERATION (0x004668bb reloads _G_LLM_STRAT_ORDER_QUEUE_COUNT), so
// it is NOT hoisted here either: a handler that enqueues while we walk extends this same pass, which
// is observable behaviour, not an accident.
//
#include "sim/rng_trace.h" // C-prime level 2: the defer-verdict note
#include "sim/sim_order_dispatch.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_export.gen.h" // the entry thunk that REPLACES the original when promoted
#include "state/hook_api.h"     // F4D-PRE: entry_owner_of -- the host's hook table, never hook/ directly
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::orders
#include "state/host_api.h"
#include "state/host_events.h"
#include "lockstep/overlay_hoist.h" // game_mode_saved_set -- the extend_ui_enter split's state half
#include "addr/mh_rebind.gen.h"     // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

// ---- WARNING TO THE CONDUCTOR: `mh::sim::calls` / `mh::sim::live_dispatch_calls()` COLLIDE ---------------
//
// sim_order_enqueue.h ALSO declares `struct calls` and `const dispatch_calls &live_dispatch_calls()` in namespace
// mh::sim, with a different member set. Two different definitions of one class name in one namespace
// is an ODR violation, and the two `live_dispatch_calls()` mangle IDENTICALLY under MSVC (the return type is
// part of the mangled name and both spell `const mh::sim::calls &`), so this TU and
// sim_order_enqueue.cpp will collide at LINK time with LNK2005 even though neither header is
// included by the other's TU. Reported in declared_needs[]: one of the two needs a distinct name
// (e.g. `dispatch_calls` / `dispatch_live_calls`) in the CONDUCTOR-OWNED header. This file uses the
// names sim_order_dispatch.h declares, unchanged, because a translator renaming a header symbol is
// how the two files stop agreeing.
//
// GENERATED, NOT TRANSCRIBED: the initializer order below is the member order of
// sim_order_dispatch.h's `calls`, produced mechanically from that header (see its own note on
// gen_module_calls.py). Re-run the generator rather than hand-editing a line.
const dispatch_calls &live_dispatch_calls() {
    static const dispatch_calls gc = {
        MH_LIBMH_BIND(game_SpendResource),
        MH_LIBMH_BIND(game_TryStartProject),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_bldg_construct_finalize),
        MH_LIBMH_BIND(llm_bldg_finish_current_order),
        MH_LIBMH_BIND(llm_bldg_footprint_is_clear),
        MH_LIBMH_BIND(llm_bldg_load_resource_tail_noop),
        MH_LIBMH_BIND(llm_bldg_pay_build_cost),
        MH_LIBMH_BIND(llm_bldg_queue_construction),
        MH_LIBMH_BIND(llm_bldg_reset_construction_anim),
        MH_LIBMH_BIND(llm_bldg_scrap_stored_units),
        MH_LIBMH_BIND(llm_bldg_set_connected_flag),
        MH_LIBMH_BIND(llm_bldg_transfer_notify_noop),
        MH_LIBMH_BIND(llm_combat_credit_planet_conquest_kills),
        MH_LIBMH_BIND(llm_debug_roll_random),
        MH_LIBMH_BIND(llm_diplomacy_set_relation),
        MH_LIBMH_BIND(llm_game_player_set_ai),
        MH_LIBMH_BIND(llm_game_player_set_human),
        MH_LIBMH_BIND(llm_game_speed_decrease),
        MH_LIBMH_BIND(llm_game_speed_increase),
        MH_LIBMH_BIND(llm_map_fow_reveal_full),
        mh::state::evt::wait_player_overlay_show_i32,
        mh::lockstep::game_mode_saved_set,
        MH_LIBMH_BIND(llm_net_send_lockstep_extend),
        MH_LIBMH_BIND(llm_prod_bldg_depart_finalize),
        MH_LIBMH_BIND(llm_prod_shuttle_depart),
        MH_LIBMH_BIND(llm_prod_shuttle_fuel_apply),
        MH_LIBMH_BIND(llm_prod_shuttle_fuel_check),
        MH_LIBMH_BIND(llm_prod_shuttle_load_passengers),
        MH_LIBMH_BIND(llm_prod_shuttle_load_resource),
        MH_LIBMH_BIND(llm_prod_shuttle_slot_bind_default),
        MH_LIBMH_BIND(llm_prod_shuttle_unload_passengers),
        MH_LIBMH_BIND(llm_prod_shuttle_unload_resource),
        MH_LIBMH_BIND(llm_progress_collect_available_projects),
        MH_LIBMH_BIND(llm_progress_recheck_buildings),
        MH_LIBMH_BIND(llm_progress_recheck_planet_system_all_players),
        MH_LIBMH_BIND(llm_progress_recheck_projects),
        MH_LIBMH_BIND(llm_resource_add),
        mh::state::evt::snd_play,
        MH_LIBMH_BIND(llm_storage_cancel_pending_docked),
        MH_LIBMH_BIND(llm_strat_ai_notify_bldg_constructed),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
        MH_LIBMH_BIND(llm_strat_ai_queue_release_order),
        MH_LIBMH_BIND(llm_strat_bldg_assign_workers),
        MH_LIBMH_BIND(llm_strat_bldg_clear_staffed_flag),
        MH_LIBMH_BIND(llm_strat_bldg_flush_cargo_hold),
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(llm_strat_bldg_pay_cycle_inputs),
        MH_LIBMH_BIND(llm_strat_bldg_set_staffed_flag),
        MH_LIBMH_BIND(llm_strat_bldg_unassign_workers),
        MH_LIBMH_BIND(llm_strat_dist_out_of_range),
        MH_LIBMH_BIND(llm_strat_population_add),
        MH_LIBMH_BIND(llm_strat_population_remove),
        MH_LIBMH_BIND(llm_strat_prod_bind_planet),
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_storage_purge_dead_docked),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_create),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_unit_apply_production_completion),
        MH_LIBMH_BIND(llm_unit_bldg_apply_lethal_damage),
        MH_LIBMH_BIND(llm_unit_bldg_apply_scaled_damage),
        MH_LIBMH_BIND(llm_unit_bldg_energy_refill_full),
        MH_LIBMH_BIND(llm_unit_create_soldier),
        MH_LIBMH_BIND(llm_unit_force_disembark),
        MH_LIBMH_BIND(llm_unit_recruit),
        MH_LIBMH_BIND(llm_unit_set_order_param),
        MH_LIBMH_BIND(llm_unit_state_is_boarding),
        MH_LIBMH_BIND(llm_unit_status_bit_clear),
        MH_LIBMH_BIND(llm_unit_status_bit_set),
        MH_LIBMH_BIND(llm_unit_transport_unload_docked),
        MH_LIBMH_BIND(llm_unit_transport_unload_field),
        // Hand-appended, below the generated block -- see sim_order_dispatch.h. The generator reads
        // mh_calls.gen.h and refuses a varargs name, which is right; this is one measured SHAPE.
        MH_CRT(w_sprintf__vss),
    };
    return gc;
}

namespace detail {

// ---- THE KIND-0xf0 LOCKSTEP BODY IS NOT DECLARED ANYWHERE, AND IT IS NOT MINE ------------------
//
// The router's `CMP kind,0xf0 / JZ 0x004695d4` (0x0046692c) targets 0x004695d4, which is INSIDE the
// admin TU's address range (0x004695d4-0x0046997e) and is a DIFFERENT entry point from
// dispatch_admin_order @0x00469691. The batch context assigns that body (0x004695d4-0x0046961a) to
// sim_order_dispatch_admin.cpp, but sim_order_dispatch.h -- which is conductor-owned and which I may
// not edit -- declares only dispatch_building_order and dispatch_admin_order. So the entry point
// this router has to call does not exist in the contract.
//
// Declared here so this TU compiles and so the gap is a LINK error the conductor sees rather than a
// silently wrong route (folding kind 0xf0 into dispatch_admin_order would send the horizon-extension
// request through the order-code scan, which decodes 0xf0 to admin arm 11 -- a real, wrong handler).
// Reported in declared_needs[]: this declaration belongs in sim_order_dispatch.h, and the name has to
// be reconciled with whatever the admin TU actually defines.

} // namespace detail

namespace {

// ---- llm_strat_unit_state members this file compares against -----------------------------------
// Values read off the CMPs in the assembly; the NAMES are Ghidra's own enum members as they render
// in the .c draft. Local to this TU (rather than shared with sim_order_enqueue.h) because including
// that header would drag in the conflicting `struct calls` noted at the top of this file.
// PARKED/EXIT_WAIT agree value-for-value with sim_order_enqueue.h's UNIT_STATE_PARKED/_EXIT_WAIT.

// The "already executing a move" set tested at 0x00466a0d-0x00466a83, only when
// move_microstep < 0x1f.
inline constexpr uint16_t UNIT_STATE_MOVE_WALKER     = 0x0f;
inline constexpr uint16_t UNIT_STATE_MOVE_PATH       = 0x11;
inline constexpr uint16_t UNIT_STATE_MOVE_PATH_12    = 0x12;
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE    = 0x2e;
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE_2F = 0x2f;

// The air-transition set tested at 0x00466b1b-0x00466baf, after the turn-completion gate.
inline constexpr uint16_t UNIT_STATE_TAKEOFF            = 0x15;
inline constexpr uint16_t UNIT_STATE_LANDING            = 0x16;
inline constexpr uint16_t UNIT_STATE_ASCEND_TO_ORBIT    = 0x31;
inline constexpr uint16_t UNIT_STATE_DESCEND_CRUISE     = 0x30;
inline constexpr uint16_t UNIT_STATE_DEPLOY_TO_BUILDING = 0x17;
inline constexpr uint16_t UNIT_STATE_CLIMB_VERTICAL     = 0x7c;

inline constexpr uint16_t UNIT_STATE_PARKED      = 0x1f; // docked in a storage slot
inline constexpr uint16_t UNIT_STATE_EXIT_WAIT   = 0x22; // mid-exit, waiting
inline constexpr uint16_t UNIT_STATE_EXIT_CANCEL = 0x23;

// ---- order_code values the loop itself branches on ---------------------------------------------
// order_code is the unit-state code fed to llm_strat_unit_set_state_of, so these are state ids used
// as order ids. 0x1d/0x1e are proven by sim_order_enqueue.h's attack_building/attack_unit handlers,
// which enqueue exactly those codes; 0x23 is proven here (it sets state EXIT_CANCEL); 0x32 is proven
// here (it unloads a transport). 0x20 is NOT named -- it gates three separate branches and no site in
// this closure pins its meaning, so it keeps its number rather than a guess.
inline constexpr uint16_t ORDER_CODE_ATTACK_BUILDING  = 0x1d;
inline constexpr uint16_t ORDER_CODE_ATTACK_UNIT      = 0x1e;
inline constexpr uint16_t ORDER_CODE_UNK_0X20         = 0x20;
inline constexpr uint16_t ORDER_CODE_EXIT_CANCEL      = 0x23;
inline constexpr uint16_t ORDER_CODE_UNLOAD_TRANSPORT = 0x32;

// cfg_enum_E_UNIT_TYPE members. A_PLANE/H_PLANE agree with sim_order_enqueue.h's own constants.
// UNIT_TYPE_TURN_GATE_MAX is not an enum member: it is the literal 0xe in the SIGNED `CMP
// dword ptr [type],0xe / JLE` at 0x00466aab, i.e. "type <= 0xe skips the turn-completion gate".
inline constexpr int32_t  UNIT_TYPE_TURN_GATE_MAX = 0x0e;
inline constexpr uint32_t UNIT_TYPE_A_PLANE       = 0x11;
inline constexpr uint32_t UNIT_TYPE_H_PLANE       = 0x12;

// map_object_unit::move_microstep is compared `>= 0x1f` at 0x004669f0 (signed JGE) -- the last
// microstep index of a 32-entry row, i.e. "the slide into the tile is finished".
inline constexpr int32_t MOVE_MICROSTEP_LAST = 0x1f;

// llm_strat_target_release_ref's `mode` (EBX) at this function's four call sites. Numbers only; the
// callee is original and nothing here depends on what they mean.
inline constexpr uint32_t RELEASE_MODE_PRIMARY_CLEAR   = 1; // 0x00466cfb
inline constexpr uint32_t RELEASE_MODE_PRIMARY_REBOUND = 0; // 0x00467080
inline constexpr uint32_t RELEASE_MODE_SECONDARY_CLEAR = 3; // 0x00467156
inline constexpr uint32_t RELEASE_MODE_SECONDARY_BIND  = 2; // 0x00467297

// llm_strat_unit_notify_status's `status_code` (EBX) at the two sites in this slice.
inline constexpr uint32_t NOTIFY_STATUS_ORDER_REPLACED = 0x67; // 0x00466dbb
inline constexpr uint32_t NOTIFY_STATUS_ZERO           = 0;    // 0x00467055

// The order record's args[] slots this slice copies into the unit. Positional protocol, per
// The order-container notes; the field comment on mh_llm_strat_order::args names the same mapping.
inline constexpr int32_t ARG_GOAL_X         = 0;
inline constexpr int32_t ARG_GOAL_Y         = 1;
inline constexpr int32_t ARG_HOME_STORAGE   = 2;
inline constexpr int32_t ARG_TARGET_FINE_X  = 3;
inline constexpr int32_t ARG_TARGET_FINE_Y  = 4;
inline constexpr int32_t ARG_TARGET_REF     = 5;
inline constexpr int32_t ARG_TARGET_INDEX   = 6;
inline constexpr int32_t ARG_WEAPON         = 7;
inline constexpr int32_t ARG_TARGET2_FINE_X = 8;
inline constexpr int32_t ARG_TARGET2_FINE_Y = 9;
inline constexpr int32_t ARG_TARGET2_REF    = 10;
inline constexpr int32_t ARG_TARGET2_INDEX  = 11;
inline constexpr int32_t ARG_MOVE_GROUP     = 12;

// `-1 means leave the field unchanged` -- every args[] guard in this slice is `CMP dword ptr,-0x1`.
inline constexpr int32_t ARG_UNSET = -1;

// DOUBLE_005011b6 @0x00467101. A plain FP literal in .rdata, not tracked state, so it is a constant
// here rather than a view member.
inline constexpr double ACTIVITY_CLOCK_DIVISOR = 100.0;

// The `IDIV ECX` divisor at 0x004670eb.
inline constexpr int32_t ACTIVITY_CLOCK_MODULUS = 0x32;

} // namespace

namespace detail {

// ---- the whole function -------------------------------------------------------------------------
void order_queue_dispatch(const sim_view &v, sim_store &own, const dispatch_calls &c) {
    // 0x004668aa / 0x004668b1. `kept` is the compaction cursor; `slot` is the walk cursor.
    int32_t kept = 0;

    // LIB-REF step-5000, count-ledger tag 14: the count this pass was PRESENTED with. Read once here
    // so the tail note can state the pass's whole effect on order_queue_count as (entry -> kept)
    // without a second derivation. A plain read; the loop below re-reads the live count per
    // iteration exactly as the original does, and nothing here feeds that.
    const int32_t ledger_entry_count = own.order_count();

    // 0x004668b8: `slot < ORDER_QUEUE_COUNT`, RE-READ each iteration (see the header note).
    // 0x004668c8: the loop tail is just `++slot` -- the `MOV EAX,[EBP-0x24]` immediately before the
    // INC is DEAD (EAX is reloaded before any use on every path out of 0x004668b8).
    for (int32_t slot = 0; slot < own.order_count(); ++slot) {
        order &q = own.order_at(slot);
        // C-prime level 2, tag 2: EVERY record the loop touches, logged BEFORE any `continue`. The
        // tag-1 note further down sits after the defer decision and is therefore blind to records
        // that exit early (admin/building/lockstep kinds, the dead-energy skip, the attack-target
        // branch) -- which is exactly the population a "did this unit get an order at all?" question
        // is about.
        mh::sim::rng_trace_add_note(2u, (uint32_t)slot, (uint32_t)q.owner_and_kind,
                                    (uint32_t)q.order_code, (uint32_t)q.unit_index,
                                    (uint32_t)own.order_count(), 0u);

        // 0x004668d0-0x00466907. `owner_and_kind` is one 16-bit field masked twice; the two ANDs are
        // 8-bit, so anything above bit 7 of the field is ignored by BOTH.
        dispatch_ctx x;
        x.slot         = slot;
        x.player       = (uint32_t)q.owner_and_kind & ORDER_OWNER_MASK;
        x.kind         = (uint32_t)q.owner_and_kind & ORDER_KIND_MASK;
        x.object_index = (int32_t)(uint32_t)q.unit_index; // MOVZX: 0..0xffff, never negative

        // ---- THE ROUTER, 0x0046690a-0x00466947 ---------------------------------------------------
        //
        // Transcribed in the assembly's own comparison order. The three JMPs at 0x00466939,
        // 0x0046693e and 0x00466947 all target 0x00469691, so ADMIN is the catch-all: every kind the
        // router does not recognise runs the admin table, and so does kind 0x20 when the `== 0x20`
        // test fails (i.e. kinds 0x00/0x10/0x30).
        //
        // Restructured from a compare/jump chain into if/else-if. The two agree on ALL SIXTEEN nibble
        // values -- exhaustively, because `kind` can only be a multiple of 0x10 in 0x00..0xf0:
        //
        //   0x00 admin (JC 0x00466943, then JNZ)      0x80 UNIT   (JBE 0x0046694d)
        //   0x10 admin (      "        , then JNZ)    0x90 admin  (JZ 0xf0 fails -> JMP)
        //   0x20 UNIT  (      "        , falls thru)  0xa0 admin
        //   0x30 admin (      "        , then JNZ)    0xb0 admin
        //   0x40 BLDG  (JBE 0x00467330)               0xc0 admin
        //   0x50 admin (JC 0x0046693e -> JMP)         0xd0 admin
        //   0x60 admin (      "        )              0xe0 admin
        //   0x70 admin (      "        )              0xf0 LOCKSTEP (JZ 0x004695d4)
        //
        // Note kind 0x20 and kind 0x80 land on the SAME code at 0x0046694d -- the "storage/dock path"
        // is not a separate body, it is the unit path reached with a different kind nibble, and
        // nothing below re-tests `kind`.
        if (x.kind < ORDER_KIND_BLDG) {         // 0x0046690e JC
            if (x.kind != ORDER_KIND_STORAGE) { // 0x00466947 JNZ -> 0x00469691
                dispatch_admin_order(v, own, c, x);
                continue;
            }
            // kind == 0x20 FALLS THROUGH into the unit path at 0x0046694d.
        } else if (x.kind <= ORDER_KIND_BLDG) { // 0x00466914 JBE -> 0x00467330 (kind == 0x40)
            dispatch_building_order(v, own, c, x);
            continue;
        } else if (x.kind < ORDER_KIND_UNIT_EX) { // 0x00466921 JC -> 0x0046693e -> 0x00469691
            dispatch_admin_order(v, own, c, x);
            continue;
        } else if (x.kind > ORDER_KIND_UNIT_EX) { // 0x0046692a JBE -> 0x0046694d (kind == 0x80)
            if (x.kind == ORDER_KIND_LOCKSTEP) {  // 0x00466933 JZ -> 0x004695d4
                dispatch_lockstep_extend(v, own, c, x);
            } else { // 0x00466939 JMP -> 0x00469691
                dispatch_admin_order(v, own, c, x);
            }
            continue;
        }

        // ---- THE UNIT / STORAGE PATH @0x0046694d ------------------------------------------------
        //
        // The record's `unit_index` really is a unit index on this path (unlike the building and
        // storage arms of the table TUs), so the roster reference is taken once and every field
        // access below is a fresh read/write through it -- the original recomputes
        // `player*0x5b04 + index*0xe9` before literally every access, which is the same address.
        unit &u = own.unit_at(x.player, x.object_index);

        // 0x0046695d-0x00466968: FLDZ / FCOMP energy / FNSTSW / SAHF / JNC -> continue.
        //
        // SAHF puts x87 C0 into CF, and C0 is set for BOTH "ST(0) < src" (0 < energy) AND for
        // UNORDERED. So JNC -- skip this record -- is taken only when energy is ordered AND <= 0, and
        // a NaN energy RUNS the body. `energy <= 0.0` is false for NaN and so reproduces that
        // exactly; the .c draft's `if (0.0 < energy)` does not, and NaN is the one input the two
        // readings differ on.
        if (u.energy <= 0.0) continue;

        // 0x0046696e-0x00466988: the DUAL-TARGET orders bypass everything below, including the
        // retention branch -- they can never defer.
        if (q.order_code == ORDER_CODE_ATTACK_UNIT || q.order_code == ORDER_CODE_ATTACK_BUILDING) {
            // ---- @0x00467118 -----------------------------------------------------------------
            // Its own is_boarding call, NOT the [EBP-0x28] one below: this branch is taken before
            // that call happens, and the result here is only TESTed, never stored.
            if (c.llm_unit_state_is_boarding((int32_t)(uint32_t)u.state) != 0) continue;

            if (u.target2_ref != 0) { // 0x0046714c
                c.llm_strat_target_release_ref(x.player, x.object_index,
                                               RELEASE_MODE_SECONDARY_CLEAR);
                u.target2_ref   = 0;
                u.target2_index = 0;
            }
            if (q.args[ARG_TARGET2_FINE_X] != ARG_UNSET) // 0x00467198
                u.target2_fine_x = q.args[ARG_TARGET2_FINE_X];
            if (q.args[ARG_TARGET2_FINE_Y] != ARG_UNSET) // 0x004671c5
                u.target2_fine_y = q.args[ARG_TARGET2_FINE_Y];
            if (q.args[ARG_TARGET2_REF] != ARG_UNSET) // 0x004671f2
                u.target2_ref = (int16_t)q.args[ARG_TARGET2_REF];
            // PRESERVED GUARD/STORE MISMATCH (0x00467221-0x00467249): the guard re-tests args[10]
            // while the store moves args[11]. Faithful to the original -- args[11] is written iff
            // args[10] is set, and is NOT written when args[11] alone is set. Do not "fix".
            if (q.args[ARG_TARGET2_REF] != ARG_UNSET)
                u.target2_index = (int16_t)q.args[ARG_TARGET2_INDEX];
            if (q.args[ARG_WEAPON] != ARG_UNSET) // 0x00467250
                u.selected_weapon = (uint8_t)q.args[ARG_WEAPON];
            if (u.target2_ref != 0) // 0x0046728d
                c.llm_strat_target_release_ref(x.player, x.object_index,
                                               RELEASE_MODE_SECONDARY_BIND);
            continue; // 0x004672a7 -> case_0
        }

        // 0x0046698f-0x00466999: `CMP q.order_code,0x23` whose FLAGS ARE DEAD -- the very next
        // instruction (`IMUL EAX,[EBP-0x34],0x5b04`) overwrites them and nothing reads them. Omitted;
        // it has no observable effect.

        // 0x004669b2/0x004669b7: [EBP-0x28] -- the ONE is_boarding result, computed here and reused
        // by FOUR later tests (0x004669ba, 0x00466c3e, 0x00466c8f, 0x00466cb6). It is a stack local
        // in the original and is NOT recomputed even though callees in between can change the state,
        // so caching it here is faithful, not an optimisation.
        const int32_t boarding = c.llm_unit_state_is_boarding((int32_t)(uint32_t)u.state);

        // ---- the DEFER decision: does control reach B @0x00466bb8, or D @0x00466c48? ------------
        //
        // Three stacked gates, transcribed in address order. `defer == true` means "reach B".
        bool defer;
        if (boarding != 0 && q.order_code != ORDER_CODE_UNK_0X20 &&
            q.order_code != ORDER_CODE_UNLOAD_TRANSPORT) {
            // 0x004669ba/0x004669cc/0x004669da -> 0x00466a89 -> B.
            defer = true;
        } else if (u.move_microstep < MOVE_MICROSTEP_LAST &&
                   (u.state == UNIT_STATE_MOVE_WALKER || u.state == UNIT_STATE_MOVE_PATH ||
                    u.state == UNIT_STATE_MOVE_PATH_12 || u.state == UNIT_STATE_HOVER_ENGAGE ||
                    u.state == UNIT_STATE_HOVER_ENGAGE_2F)) {
            // 0x004669f0 (signed JGE past the whole chain) then 0x00466a0d-0x00466a83: still sliding
            // between tiles in a movement state -> B.
            defer = true;
        } else if ((int32_t)v.cfg_units[u.unit_proto_id].type <= UNIT_TYPE_TURN_GATE_MAX) {
            // 0x00466aab: SIGNED `CMP dword ptr [type],0xe / JLE` -> D. Ground classes skip the
            // turn-completion gate entirely.
            defer = false;
        } else if (u.facing_target !=
                       v.move_microsteps[(int32_t)u.move_heading * MICROSTEPS_PER_HEADING +
                                         u.move_microstep]
                           .facing ||
                   u.state == UNIT_STATE_TAKEOFF || u.state == UNIT_STATE_LANDING ||
                   u.state == UNIT_STATE_ASCEND_TO_ORBIT || u.state == UNIT_STATE_DESCEND_CRUISE ||
                   u.state == UNIT_STATE_DEPLOY_TO_BUILDING || u.state == UNIT_STATE_CLIMB_VERTICAL) {
            // 0x00466ac8-0x00466b09: the TURN-COMPLETION gate.
            // MICROSTEPS[move_heading][move_microstep].facing vs the unit's facing_target --
            // `IMUL EDX,move_heading,0x60` selects the ROW and `LEA EAX,[EAX+EAX*2]` the 3-byte
            // entry, i.e. MICROSTEPS_PER_HEADING entries per heading. Expressed through
            // v.move_microsteps with the element index, never as a byte offset.
            // The state chain at 0x00466b1b-0x00466baf follows it -> B. The `||` is short-circuit
            // exactly as the assembly's first-mismatch jump is, and none of the operands has a side
            // effect, so the two orders are indistinguishable.
            defer = true;
        } else {
            defer = false; // 0x00466bb3 -> D
        }

        // C-prime level 2: the DEFER VERDICT and every input it reads, per unit-order record. Gated
        // on the trace's own step window (sim/rng_trace.h), so it costs two integer compares when
        // disarmed. tag 1 = this decision. The fields are the gates in the order the chain tests
        // them, so a cross-arm diff of these lines names the differing INPUT, not just the outcome.
        mh::sim::rng_trace_add_note(1u, (uint32_t)x.player << 16 | (uint32_t)x.object_index,
                                    (uint32_t)q.order_code, (uint32_t)u.state,
                                    (uint32_t)u.move_microstep << 16 | (uint32_t)u.move_heading,
                                    (uint32_t)u.facing_target << 16 |
                                        (uint32_t)v.cfg_units[u.unit_proto_id].type,
                                    (uint32_t)boarding << 8 | (uint32_t)(defer ? 1u : 0u));

        if (defer) {
            // ---- B @0x00466bb8: DEFER, or the 0x23 special case ---------------------------------
            if (q.order_code == ORDER_CODE_EXIT_CANCEL) {
                // 0x00466bbc-0x00466bf9. Promotes a waiting unit's state and DROPS the record --
                // this arm does NOT retain, which is why the cursor is untouched here.
                if (u.state == UNIT_STATE_EXIT_WAIT) u.state = UNIT_STATE_EXIT_CANCEL;
                continue;
            }

            // ---- THE RETAIN PATH @0x00466bfe ----------------------------------------------------
            // The ONE place `kept` moves, and the ONE place a record survives the pass.
            u.order_queued = 1; // 0x00466c0e -- set BEFORE the copy

            // 0x00466c15-0x00466c36: `REP MOVSD` of 0x11 dwords = 0x44 bytes = one whole record,
            // SKIPPED when kept == slot (a self-copy the original refuses to make). `kept <= slot`
            // always holds, so this never clobbers a record the walk has not reached yet.
            if (kept != slot) own.order_at(kept) = own.order_at(slot);

            // 0x00466c38/0x00466c3b: the `MOV EAX,[EBP-0x20]` before the INC is DEAD (EAX is
            // reloaded on every path out of here).
            ++kept;

            // 0x00466c3e/0x00466c42: the SAME cached is_boarding value. Not boarding -> the record
            // was merely deferred and the iteration ends. Boarding -> fall into D and EXECUTE it as
            // well, so a boarding unit's order is both retained AND applied this pass.
            if (boarding == 0) continue;
        }

        // ---- D @0x00466c48: EXECUTE ---------------------------------------------------------------
        if (q.order_code == ORDER_CODE_UNLOAD_TRANSPORT) { // 0x00466c4c
            if (u.state == UNIT_STATE_PARKED)              // 0x00466c66
                c.llm_unit_transport_unload_docked(x.player, (uint32_t)x.object_index);
            else
                c.llm_unit_transport_unload_field(x.player, (uint32_t)x.object_index);
            continue; // 0x00466c8a
        }

        // 0x00466c8f-0x00466ca1: boarding, and not the 0x20 order -> force the unit off its ride and
        // drop the record.
        if (boarding != 0 && q.order_code != ORDER_CODE_UNK_0X20) {
            c.llm_unit_force_disembark(x.player, x.object_index); // 0x00466cac
            continue;
        }

        // 0x00466cb6-0x00466cdc: NOT boarding and the order is 0x20 or 0x23 -> nothing more to do.
        if (boarding == 0 && (q.order_code == ORDER_CODE_UNK_0X20 ||
                              q.order_code == ORDER_CODE_EXIT_CANCEL)) {
            continue;
        }

        // ---- F @0x00466ce1: apply the order to the unit -------------------------------------------
        if (u.target_ref != 0) { // 0x00466cf1
            c.llm_strat_target_release_ref(x.player, x.object_index, RELEASE_MODE_PRIMARY_CLEAR);
            u.target_ref   = 0;
            u.target_index = 0;
        }

        // 0x00466d3d-0x00466db9: both comparisons are 32-bit between a MOVZX'd 16-bit state and a
        // MOVZX'd 8-bit cfg byte, so they are zero-extended equality tests, not sign-sensitive.
        if ((uint32_t)u.state == (uint32_t)v.cfg_units[u.unit_proto_id].move_op_arg ||
            (uint32_t)u.state == (uint32_t)v.cfg_units[u.unit_proto_id].move_op_code) {
            c.llm_strat_unit_notify_status(x.player, x.object_index, NOTIFY_STATUS_ORDER_REPLACED);
        }

        // 0x00466dcb-0x00466df2. Both arguments reach EBX through a MOVZX (zero-extension) in the
        // original; the committed thunks take int16_t and sign-extend. THE DIRECTION CANNOT MATTER,
        // and the reason is structural rather than a bound on the values: both callees store only
        // the low word and never read bits 16-31. llm_unit_set_order_param @0x0048696f is
        // `units[player][unit_index].order = param` -> MOV word ptr [EDX+0xdd8c4c],AX, and
        // llm_strat_unit_set_state_of @0x004869b0 is `.state = state` -> MOV word ptr
        // [EDX+0xdd8c4e],AX. (This comment used to argue "every value here is below 0x8000, so the
        // two agree", which is true but incidental -- it would stop being true if the container ever
        // carried a high order code, and the code would still be correct. Corrected after
        // reimpl-verify decompiled both callees, 2026-08-08.)
        c.llm_unit_set_order_param((int32_t)x.player, x.object_index, q.param0);
        c.llm_strat_unit_set_state_of((int32_t)x.player, x.object_index, (int16_t)q.order_code);

        // 0x00466df7-0x00466f8a: the args[] -> unit field copies. Each guard is a FULL DWORD test
        // against -1 while several of the STORES are narrower (byte / word) -- the widths below are
        // the store widths from the assembly, not the field's declared type.
        if (q.args[ARG_GOAL_X] != ARG_UNSET) u.goal_x = (uint8_t)q.args[ARG_GOAL_X];
        if (q.args[ARG_GOAL_Y] != ARG_UNSET) u.goal_y = (uint8_t)q.args[ARG_GOAL_Y];
        if (q.args[ARG_HOME_STORAGE] != ARG_UNSET)
            u.home_storage_slot = (uint8_t)q.args[ARG_HOME_STORAGE];
        if (q.args[ARG_TARGET_FINE_X] != ARG_UNSET) u.target_fine_x = q.args[ARG_TARGET_FINE_X];
        if (q.args[ARG_TARGET_FINE_Y] != ARG_UNSET) u.target_fine_y = q.args[ARG_TARGET_FINE_Y];
        if (q.args[ARG_TARGET_REF] != ARG_UNSET) u.target_ref = (int16_t)q.args[ARG_TARGET_REF];
        // PRESERVED GUARD/STORE MISMATCH (0x00466f07-0x00466f2f): the guard re-tests args[5] while
        // the store moves args[6]. Same defect shape as the args[10]/args[11] pair above; both are
        // the original's, both are deliberate here.
        if (q.args[ARG_TARGET_REF] != ARG_UNSET) u.target_index = (int16_t)q.args[ARG_TARGET_INDEX];
        if (q.args[ARG_WEAPON] != ARG_UNSET) u.selected_weapon = (uint8_t)q.args[ARG_WEAPON];
        if (q.args[ARG_MOVE_GROUP] != ARG_UNSET) u.move_group_id = q.args[ARG_MOVE_GROUP];

        // 0x00466f90-0x00467010. `order_queued` is cleared here, undoing the retain flag for a unit
        // whose order we both retained AND executed this pass (the boarding case above).
        u.path_blocked_retry_count = 0;
        u.order_queued             = 0;
        u.home_x                   = u.x;
        u.home_y                   = u.y;

        // 0x00467016-0x0046705d: the state is re-read AFTER set_state_of ran, and this test uses
        // move_op_arg ONLY (unlike the pair at 0x00466d3d).
        if ((uint32_t)u.state == (uint32_t)v.cfg_units[u.unit_proto_id].move_op_arg)
            c.llm_strat_unit_notify_status(x.player, x.object_index, NOTIFY_STATUS_ZERO);

        // 0x00467072-0x0046710d: target_ref re-read after the args[] copies may have SET it.
        if (u.target_ref != 0) {
            c.llm_strat_target_release_ref(x.player, x.object_index, RELEASE_MODE_PRIMARY_REBOUND);
            // 0x004670aa/0x004670d0: planes skip the activity-clock nudge entirely.
            if (v.cfg_units[u.unit_proto_id].type != UNIT_TYPE_A_PLANE &&
                v.cfg_units[u.unit_proto_id].type != UNIT_TYPE_H_PLANE) {
                // 0x004670eb-0x0046710d: `IDIV` by 0x32 keeps the SIGNED remainder of the unit index
                // (always non-negative here -- object_index came from a MOVZX), then
                // FILD / FDIV 100.0 / FADD / FSTP. Written in that operand order so the x87 sequence
                // is the same one the original executes.
                const int32_t phase = x.object_index % ACTIVITY_CLOCK_MODULUS;
                u.activity_clock    = (double)phase / ACTIVITY_CLOCK_DIVISOR + u.activity_clock;
            }
        }
        // 0x00467113 -> 0x004672a7 -> case_0: end of iteration.
    }

    // ---- THE TAIL @0x00469984 -------------------------------------------------------------------
    // The COMPACTION CURSOR becomes the new count -- not zero, and not the count we walked.
    own.order_count() = kept;
    mh::sim::rng_trace_add_note(14u, (uint32_t)ledger_entry_count, 0u, 0u, 0u, (uint32_t)kept, 0u);
}

} // namespace detail

// ---- the public surface ---------------------------------------------------------------------------

void order_queue_dispatch() {
    sim_state st = state();
    detail::order_queue_dispatch(st.read, st.own, live_dispatch_calls());
}

// ---- the differential-oracle arm --------------------------------------------------------------------
//
// SHADOW SAFETY: see sim_order_dispatch.h. Four reachable callees are classed `effectful`
// (game_ui_PrintTextMessage, llm_snd_play, llm_net_send_lockstep_extend,
// llm_net_lockstep_extend_ui_enter) and the SIM-CUT effect seam must be armed or they fire twice per
// compared call. The seam covers all four. Everything else runs FOR REAL in both arms and the
// between-arms restore undoes it.
// ---- the rig's work indicator (declared in the header; see the note there) ---------------------
void tally_coverage(const order &q, dispatch_coverage &cov) {
    const uint32_t kind = (uint32_t)q.owner_and_kind & ORDER_KIND_MASK;
    cov.kinds |= (uint16_t)(1u << (kind >> 4));

    if (kind == ORDER_KIND_BLDG) {
        cov.bldg_arms |= 1u << (uint32_t)decode_building_order((uint16_t)q.param0);
    } else if (kind == ORDER_KIND_STORAGE || kind == ORDER_KIND_UNIT_EX ||
               kind == ORDER_KIND_LOCKSTEP) {
        // Not table arms -- the unit/storage path and the lockstep body are reached by the router
        // directly, so the kind bit is their whole coverage story.
    } else {
        cov.admin_arms |= 1u << (uint32_t)decode_admin_order(q.order_code);
    }
}

// ---- the instrumentation, shared by BOTH oracles ----------------------------------------------
//
// At mh::sim scope rather than inside either arm's namespace, because the shadow arm and the
// promoted entry are two ways of running the SAME body and they report the same counters.
namespace instrumented {

// std::popcount would need <bit>, and this TU is built /arch:IA32 for FP fidelity -- a portable
// four-line loop keeps the reporting path free of anything the codegen settings could interact with.
inline int popcount32(uint32_t v) {
    int n = 0;
    for (; v != 0; v &= v - 1) ++n;
    return n;
}

// ---- THE WORK INDICATOR, and why a call count is not one here -------------------------------------
//
// The single behaviour this site exists to get right is RETENTION, and retention happens only on the
// unit path's defer branch. An idle or lightly-loaded run dispatches thousands of records and retains
// NONE of them, in which case a clean divergence count says only "we also stored 0", which the
// drain-the-queue bug this whole file warns about would ALSO satisfy. That is the "a REACHED site is
// not an EXERCISED one" trap.
//
// So the arm reports the two numbers that make a green verdict mean something: how many calls found a
// NON-EMPTY queue, and how many calls RETAINED at least one record. Both are read from
// own.order_count() around the call -- the count at entry is the pre-state (the harness has already
// restored it after the original arm ran) and the count at exit is `kept` by construction, so no new
// interface is needed to observe the cursor. A run whose `calls_retaining` stays 0 has not tested the
// compaction at all, whatever its divergence count says.
//
// ---- AND THE SECOND WORK INDICATOR: WHICH OPCODES ACTUALLY FIRED ---------------------------------
//
// SIM1C's done_when asks for the opcode mix, not the call total, and the reason is the same trap one
// level down. This site is FIFTY handlers behind two switches. A scenario that only ever sends two
// opcodes exercises two arms; the other 47 are as unverified after a 100 000-call clean run as they
// were before it, and nothing in `calls=100000 divergences=0` says so. The masks below are what turns
// "the dispatcher was called a lot" into "these specific arms were compared".
//
// Read the ARMS, not the opcodes: the tally runs each record through the same decode the dispatcher
// does, so a bit here means the arm's BODY was reached (a value the scan rejects lands on bit 0, the
// default arm, which is exactly where the dispatcher sends it). Recording raw opcodes instead would
// report 200 distinct unrecognised values as 200 kinds of coverage of one arm.
//
// The walk is over the PRE-STATE, before the dispatcher runs. That is deliberate: it is the work
// PRESENTED to the site. A handler that enqueues during the walk is a later call's input, and the
// loop's re-read of the count is what lets that happen -- counting it here would double-count it.
namespace {
long              g_calls           = 0;
long              g_calls_nonempty  = 0;
long              g_calls_retaining = 0;
long              g_retained_total  = 0;
dispatch_coverage g_cov             = {};
} // namespace

// ONE instrumented body, shared by the shadow arm and the promoted entry. They are never active in
// the same run -- the two installers refuse each other -- so sharing the counters is deliberate, and
// `tag` keeps them apart in the log. The same counter name under two different arming regimes means
// different
// things: under shadow the numbers describe OUR arm of a per-call A/B, under promotion they describe
// the only body there is.
void run_instrumented(const char *tag) {
    sim_state st = state();

    const int32_t pending = st.own.order_count();
    ++g_calls;
    if (pending != 0) ++g_calls_nonempty;
    for (int32_t i = 0; i < pending; ++i) tally_coverage(st.own.order_at(i), g_cov);

    static long n_trace = 0;
    if (++n_trace <= mh::ai::trace_budget())
        mh::ai::ai_say("; [%s] ai_trace order_queue_dispatch pending=%d\n", tag, pending);

    detail::order_queue_dispatch(st.read, st.own, live_dispatch_calls());

    const int32_t kept = st.own.order_count();
    g_retained_total += kept;
    if (kept != 0) ++g_calls_retaining;

    // Same widening cadence as the site's own divergence line, so the two read together. The masks
    // are printed in hex AND as popcounts: the mask is what a later run diffs against, the counts
    // are what a human reads to see whether the scenario was worth running.
    // The %200 tail matches the site's own divergence-count cadence, so the two read together AND
    // so the LAST line of a run reflects the run. The earlier cadence (1/100/1000/10000/%5000) left
    // a 3600-call run reporting coverage measured at call 1000 -- the numbers were three quarters
    // stale, and the one thing this line exists to answer is "was THIS run worth anything".
    if (n_trace == 1 || n_trace == 100 || n_trace % 200 == 0)
        mh::ai::ai_say("; [%s] ai_work order_queue_dispatch calls=%ld nonempty=%ld "
                       "calls_retaining=%ld retained_total=%ld kinds=%04x bldg_arms=%08x/%d "
                       "admin_arms=%08x/%d\n",
                       tag, g_calls, g_calls_nonempty, g_calls_retaining, g_retained_total,
                       (unsigned)g_cov.kinds, (unsigned)g_cov.bldg_arms,
                       popcount32(g_cov.bldg_arms), (unsigned)g_cov.admin_arms,
                       popcount32(g_cov.admin_arms));
}

} // namespace instrumented


// ---- the PROMOTED entry, and why it is the stronger oracle for THIS function -------------------
//
// Shadow mode runs both bodies per call across a snapshot/restore, which is exactly what made it
// sensitive to every region a CALLEE writes: eight undeclared regions and five rig runs before it
// read clean. PROMOTION has none of that. Our body simply IS the function
// for the whole run -- nothing is snapshotted, nothing restored, effects fire once -- and the
// comparison moves up a level: the per-step state-hash trajectory of a promoted run against an
// unpromoted one (`test_ui.py --soak --soak-golden`).
//
// What it buys: no region set to get wrong, no effect-seam requirement, and the function is
// exercised IN COMPOSITION over thousands of steps rather than one call at a time.
// What it costs: a divergence names the first differing STEP and region, not which of the fifty arms
// caused it. So this is the integration check; shadow and simtest remain the localising ones.
namespace promoted_arm {
namespace {
bool g_installed = false;
// D18. Registered by the harness when the entry is promoted and it therefore cannot install its own
// trampoline. Null in every run that does not ask for the recorder, which is nearly all of them.
void (*g_dispatch_observer)() = nullptr;
} // namespace

// D18: the observer fires HERE and not inside run_instrumented, because run_instrumented is shared
// with the shadow arm -- which runs the body twice per call across a snapshot/restore. An observer
// that writes a file (the order recorder does) must fire once per real dispatch, not once per arm.
// Before the body, matching where the displaced trampoline ran: it was a run-before detour that
// jumped to the stolen prologue afterwards.
void order_queue_dispatch() {
    fire_dispatch_observer();
    instrumented::run_instrumented("promote");
}

bool active() { return g_installed; }
void mark_installed(bool on) { g_installed = on; }
} // namespace promoted_arm

void set_dispatch_observer(void (*fn)()) { promoted_arm::g_dispatch_observer = fn; }
void (*dispatch_observer())() { return promoted_arm::g_dispatch_observer; }
void fire_dispatch_observer() {
    if (promoted_arm::g_dispatch_observer) promoted_arm::g_dispatch_observer();
}

} // namespace mh::sim


MH_EXPORT_REPLACE(llm_strat_order_queue_dispatch, mh::sim::promoted_arm::order_queue_dispatch)

namespace mh::sim {

// `[promote] sim_dispatch=1`. The shipping default is PASSED IN rather than read from a header here:
// SHIP_PROMOTE_* lives in mh/seams/net_internal.h with the rest of the ship defaults and a
// reimplementation TU must not include a seams header (the layering lint). Same arrangement as
// mh::orders::install_promotion.
// D18: the generated entry thunk's address, for the harness-mediated rebind. Same accessor shape as
// mh::sim::sim_step_entry_thunk() and for the same reason: the thunk is a static in THIS TU, and the
// seam that decides between rebind and entry-install (reimpl_probe) cannot name it directly.
void *order_queue_dispatch_entry_thunk() {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the caller is a harness detour, and the standalone artifact has none -- so
    // there is no thunk to hand out and MH_EXPORT_REPLACE does not generate one. Null, not a stub
    // address: a caller that gets one here has reached injection machinery that is not present.
    return nullptr;
#else
    return (void *)mh_export_thunk_llm_strat_order_queue_dispatch;
#endif
}

// The rebind route reaches the promoted body without going through install_promotion_dispatch, so it
// has to mark the arm live itself -- `active()` is what the run report reads to say whether ours was
// the function, and a rebound run that reported "not installed" would be the same class of lie this
// whole thread is about.
void mark_promoted_dispatch_installed(bool on) { promoted_arm::mark_installed(on); }

int install_promotion_dispatch(int default_on) {
    if (default_on == 0) return 0;
    if (!mh_export_install_llm_strat_order_queue_dispatch()) {
        // The generated installer already logged WHY, and since C9(c) it can distinguish the two
        // causes -- so this line must not flatten them back. "entry guard mismatch" was written when
        // a wrong image was the only cause anyone had in mind; an entry a DLL detour holds is the
        // other, it is not a build problem, and it has a different remedy (rebind, C4/C6).
        if (const char *owner = mh::hosthook::entry_owner_of(mh::exp::addr_llm_strat_order_queue_dispatch))
            mh::ai::ai_say("; [promote] sim_dispatch REFUSED -- the entry is held by %s. NOT a build "
                           "problem: rebind that detour's fall-through instead (C4/C6)\n",
                           owner);
        else
            mh::ai::ai_say("; [promote] sim_dispatch REFUSED -- entry guard mismatch, NOT promoted\n");
        return 0;
    }
    promoted_arm::mark_installed(true);
    mh::ai::ai_say("; [promote] sim_dispatch: llm_strat_order_queue_dispatch is LIVE -- ours IS the "
                   "function, there is no original arm in this run\n");
    return 1;
}

} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
