//
// sim/sim_hostreach_promote.cpp -- see sim_hostreach_promote.h.
//
#include "sim/sim_hostreach_promote.h"

#include "addr/mh_export.gen.h" // sig_<name> typedefs, MH_EXPORT_REPLACE, mh_export_install_<name>()
#include "ai/ai_state.h"        // ai_say -- the shared trace sink every mh::sim promoted_arm TU logs through

#include "state/hook_api.h" // F4D-PRE: entry_owner_of -- the host's hook table, never hook/ directly

// LIB-REF-IN: THE ADAPTERS FORWARD INTO THE INBOUND C ENTRY, NOT INTO THE C++ WRAPPER DIRECTLY.
//
// This is the item's whole proof strategy and it is one include plus one call per row. A forked
// host reaches these bodies through libmh_host_in.h; the hosted configuration now reaches them
// through the SAME entries, so the UI suite and the determinism run exercise the inbound contract
// instead of a parallel copy of it. Nothing about the injection machinery moves: the
// MH_EXPORT_REPLACE rows, the install order, the per-row [promote_skip] ladder (which keys on the
// ORIGINAL name) and SHIP_PROMOTE_SIM_HOSTREACH are untouched, and the exact liveness line
// tools/test_ui.py greps is emitted BEFORE the inbound call, so a run that enters the seam still
// prints it whatever the entry does.
//
// THE ORDERING HAZARD, and where it is paid: every entry refuses until libmh_in_open() has
// succeeded, so an unopened surface would turn all 21 of these into silent no-ops. The open
// happens in MH_Harness_Init beside the two host-api binds -- the same G104 earliest-common-point
// argument, for the same reason -- and its rc is reported at arm time with [hostapi]'s.
#include "../../libmh/include/libmh_host_in.h"

// The 15 headers that declare the public live wrappers. KEPT even though every adapter now
// forwards through the C entry instead: the ORDER-ID adapters below need nothing from them, but
// removing them would hide a real dependency -- libmh_host_in.h declares the C surface, not the
// C++ wrappers, and the sig_ typedefs these adapters must match are written against the latter.
#include "sim/sim_bldg_finish_order.h"
#include "sim/sim_bldg_footprint_is_clear.h"
#include "sim/sim_bldg_shuttle_slot_is_free.h"
#include "sim/sim_bldg_uses_workers.h"
#include "sim/sim_game_player_set_side.h"
#include "sim/sim_game_set_event.h"
#include "sim/sim_locate_active_port.h"
#include "sim/sim_prod_planet_distance_factor.h"
#include "sim/sim_storage_purge_dead_docked.h"
#include "sim/sim_tile_delta_wrapped.h"
#include "sim/sim_tile_pixel_wrap_delta.h"
#include "sim/sim_unit_ctrl_group.h"
#include "sim/sim_unit_ctrlgroup_member.h"
#include "sim/sim_unit_fine_pos.h"
#include "sim/sim_unit_is_boarding.h"


namespace mh::sim {

namespace promoted_arm {

// `mh::sim::promoted_arm` is shared with sim/resid/resid_promote.cpp, sim/libtrans/sim_lt_promote.cpp,
// sim/sim_order_dispatch.cpp and sim/libtrans/sim_lt_frame.cpp -- namespaces merge across translation
// units, so both the flag and the first-call helper stay internal-linkage (anonymous sub-namespace),
// matching those files' own convention, so no two TUs can ever collide on the name at link time.
namespace {
bool g_hr_promote_installed = false;

// PER-ROW FIRST-CALL LIVENESS (see the header banner). `fired` is a static local owned by the calling
// adapter, so each of the 21 adapters gets its own one-shot latch. The "(OURS is live)" suffix is
// exact -- tools/test_ui.py greps it -- do not reword it.
void hr_mark_first_call(bool &fired, const char *orig_name) {
    if (fired) return;
    fired = true;
    mh::ai::ai_say("; [promote] sim_hostreach: %s call #1 (OURS is live)\n", orig_name);
}
} // namespace

// clang-format off

// ---- sim_game_set_event.h ---------------------------------------------------------------------
// 34 front-end call sites reach this one row -- every HUD button, panel tick, dialog and mode
// change in the game. It is the row X-SPINE's first whole-ledger tombstone sweep fired on
// -- and the single largest reason section 5 was red.
uint32_t hr_game_SetEvent(uint32_t type) {
    static bool fired = false;
    hr_mark_first_call(fired, "game_SetEvent");
    return ::libmh_post_event(type);
}

// ---- sim_bldg_finish_order.h -------------------------------------------------------------------
void hr_llm_bldg_finish_current_order(uint32_t player, uint32_t building_index) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_bldg_finish_current_order");
    ::libmh_bldg_finish_current_order(player, building_index);
}

// ---- sim_bldg_footprint_is_clear.h -------------------------------------------------------------
int32_t hr_llm_bldg_footprint_is_clear(int32_t x, int32_t y, int32_t building_type, uint32_t viewer) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_bldg_footprint_is_clear");
    return ::libmh_bldg_footprint_is_clear(x, y, building_type, viewer);
}

// ---- sim_game_player_set_side.h ----------------------------------------------------------------
void hr_llm_game_player_set_human(uint8_t player) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_game_player_set_human");
    ::libmh_game_player_set_human(player);
}

// ---- sim_prod_planet_distance_factor.h ---------------------------------------------------------
double hr_llm_prod_planet_distance_factor(int32_t src_planet, int32_t dest_planet) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_prod_planet_distance_factor");
    return ::libmh_planet_distance_factor(src_planet, dest_planet);
}

// ---- sim_bldg_shuttle_slot_is_free.h -----------------------------------------------------------
int32_t hr_llm_strat_bldg_shuttle_slot_is_free(int32_t player, int32_t building_id) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_bldg_shuttle_slot_is_free");
    return ::libmh_bldg_shuttle_slot_is_free(player, building_id);
}

// ---- sim_bldg_uses_workers.h -------------------------------------------------------------------
int32_t hr_llm_strat_bldg_uses_workers(uint32_t player, int32_t building_index) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_bldg_uses_workers");
    return ::libmh_bldg_uses_workers(player, building_index);
}

// ---- sim_unit_ctrl_group.h ---------------------------------------------------------------------
int32_t hr_llm_strat_ctrl_group_contains_unit(uint32_t unit_id, int32_t count, int32_t group_index) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_ctrl_group_contains_unit");
    return ::libmh_ctrlgroup_contains(unit_id, count, group_index);
}

void hr_llm_strat_unit_ctrl_group_assign(int32_t unit_id, int32_t new_group_id) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_unit_ctrl_group_assign");
    ::libmh_ctrlgroup_assign(unit_id, new_group_id);
}

// ---- sim_locate_active_port.h ------------------------------------------------------------------
// `__mh_watcall_ecx_ebx_volatile` callee: the generated naked thunk marshals the register contract,
// so this adapter is an ordinary positional forward and must not try to reproduce it.
uint32_t hr_llm_strat_locate_active_port(uint32_t player, int32_t *out_col, int32_t *out_row,
                                         uint32_t *out_port_slot) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_locate_active_port");
    return ::libmh_locate_active_port(player, out_col, out_row, out_port_slot);
}

// ---- sim_tile_pixel_wrap_delta.h ---------------------------------------------------------------
// The sig_ typedef spells its parameters param_1/param_2/a2/param_4/param_5/param_6 (an
// uncommitted-name prototype); they are (x1, y1, x2, y2, *out_dx, *out_dy) in that order -- a
// straight positional forward, not a reordering.
void hr_llm_strat_pixel_delta_wrapped(int32_t param_1, int32_t param_2, int32_t a2, int32_t param_4,
                                      int32_t *param_5, int32_t *param_6) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_pixel_delta_wrapped");
    ::libmh_map_pixel_delta(param_1, param_2, a2, param_4, param_5, param_6);
}

int32_t hr_llm_strat_tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_tile_dist_wrapped");
    return ::libmh_map_tile_dist(x1, y1, x2, y2);
}

// ---- sim_storage_purge_dead_docked.h -----------------------------------------------------------
void hr_llm_strat_storage_purge_dead_docked(int32_t player, int32_t storage_sub_id) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_storage_purge_dead_docked");
    ::libmh_storage_purge_dead_docked(player, storage_sub_id);
}

// ---- sim_tile_delta_wrapped.h ------------------------------------------------------------------
void hr_llm_strat_tile_delta_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                     int32_t *out_dx, int32_t *out_dy) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_tile_delta_wrapped");
    ::libmh_map_tile_delta(x1, y1, x2, y2, out_dx, out_dy);
}

// ---- sim_unit_ctrlgroup_member.h ---------------------------------------------------------------
// Both rows are `__mh_watcall_ebx_volatile` callees -- see the locate_active_port note; the thunk
// owns the register contract, the adapter forwards positionally.
void hr_llm_strat_unit_ctrlgroup_add_member(int32_t param_1, int32_t *param_2, int32_t a2) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_unit_ctrlgroup_add_member");
    ::libmh_ctrlgroup_add_member(param_1, param_2, a2);
}

void hr_llm_strat_unit_ctrlgroup_remove_member(uint32_t unit_idx, int32_t *count_ptr,
                                               int32_t group_idx) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_unit_ctrlgroup_remove_member");
    ::libmh_ctrlgroup_remove_member(unit_idx, count_ptr, group_idx);
}

// ---- sim_unit_fine_pos.h -----------------------------------------------------------------------
void hr_llm_strat_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x,
                                  int32_t *out_y) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_unit_get_coords");
    ::libmh_unit_get_coords(player, unit_index, out_x, out_y);
}

// ---- sim_unit_is_boarding.h --------------------------------------------------------------------
int32_t hr_llm_unit_state_is_boarding(int32_t state) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_unit_state_is_boarding");
    return ::libmh_unit_is_boarding(state);
}

// ---- LIB-REF-IN: the three EX-REGISTER-HAZARD rows (18 -> 21) -----------------------------------
//
// These three were RESIDUE, not work: SIM-HOSTREACH Phase A found each body reading an `unaff_EBX`
// no committed prototype described, and an MH_EXPORT_REPLACE redirects the ENTRY -- so it serves
// every original caller including register state the prototype could not see. That is the
// REBIND-AI-ESI failure, which is why installing a redirect on them was a live
// risk rather than a chore.
//
// THE HAZARD IS GONE BECAUSE THE RE WAS DONE, not because the risk was re-judged: the register
// contract is now NAMED and COMMITTED (EN v402) -- flash/select are `__mh_watcall_ebx_volatile`
// with `group_index` in EBX, planet_distance is `__mh_watcall_ecx_ebx_volatile` with x2 in EBX and
// y2 in ECX -- so the generated naked thunk marshals what used to be ambient, and these adapters
// are ordinary positional forwards like the eighteen above. Their rows were deleted from
// tools/data/reconciliation_hostreach_residue.json in the same motion, leaving
// llm_map_fog_of_war_recompute as its sole (and differently-classed) entry.
//
// The two control-group rows go through libmh_issue_order rather than a command entry, and that is
// the ORIGINAL's split, not ours: both already dispatch lockstep orders 0x34/0x35, so they ride the
// order stream. The ROSTER half (add/remove/assign/contains, above) stays local -- its region is
// group-indexed with no player dimension and appears in no lockstep hash column.
void hr_llm_strat_order_ctrlgrp_flash_member(uint16_t side, uint16_t unit_id, int32_t group_index) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_order_ctrlgrp_flash_member");
    const int32_t argv[3] = {(int32_t)side, (int32_t)unit_id, group_index};
    ::libmh_issue_order(LIBMH_ORD_ORDER_CTRLGRP_FLASH_MEMBER, argv, 3u);
}

void hr_llm_strat_order_ctrlgrp_select_member(uint32_t side, uint16_t unit_id,
                                              int32_t group_index) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_order_ctrlgrp_select_member");
    const int32_t argv[3] = {(int32_t)side, (int32_t)unit_id, group_index};
    ::libmh_issue_order(LIBMH_ORD_ORDER_CTRLGRP_SELECT_MEMBER, argv, 3u);
}

double hr_llm_strat_planet_distance(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    static bool fired = false;
    hr_mark_first_call(fired, "llm_strat_planet_distance");
    return ::libmh_planet_distance(x1, y1, x2, y2);
}

// clang-format on

} // namespace promoted_arm

} // namespace mh::sim

// clang-format off
MH_EXPORT_REPLACE(game_SetEvent, mh::sim::promoted_arm::hr_game_SetEvent)
MH_EXPORT_REPLACE(llm_bldg_finish_current_order, mh::sim::promoted_arm::hr_llm_bldg_finish_current_order)
MH_EXPORT_REPLACE(llm_bldg_footprint_is_clear, mh::sim::promoted_arm::hr_llm_bldg_footprint_is_clear)
MH_EXPORT_REPLACE(llm_game_player_set_human, mh::sim::promoted_arm::hr_llm_game_player_set_human)
MH_EXPORT_REPLACE(llm_prod_planet_distance_factor, mh::sim::promoted_arm::hr_llm_prod_planet_distance_factor)
MH_EXPORT_REPLACE(llm_strat_bldg_shuttle_slot_is_free, mh::sim::promoted_arm::hr_llm_strat_bldg_shuttle_slot_is_free)
MH_EXPORT_REPLACE(llm_strat_bldg_uses_workers, mh::sim::promoted_arm::hr_llm_strat_bldg_uses_workers)
MH_EXPORT_REPLACE(llm_strat_ctrl_group_contains_unit, mh::sim::promoted_arm::hr_llm_strat_ctrl_group_contains_unit)
MH_EXPORT_REPLACE(llm_strat_locate_active_port, mh::sim::promoted_arm::hr_llm_strat_locate_active_port)
MH_EXPORT_REPLACE(llm_strat_pixel_delta_wrapped, mh::sim::promoted_arm::hr_llm_strat_pixel_delta_wrapped)
MH_EXPORT_REPLACE(llm_strat_storage_purge_dead_docked, mh::sim::promoted_arm::hr_llm_strat_storage_purge_dead_docked)
MH_EXPORT_REPLACE(llm_strat_tile_delta_wrapped, mh::sim::promoted_arm::hr_llm_strat_tile_delta_wrapped)
MH_EXPORT_REPLACE(llm_strat_tile_dist_wrapped, mh::sim::promoted_arm::hr_llm_strat_tile_dist_wrapped)
MH_EXPORT_REPLACE(llm_strat_unit_ctrl_group_assign, mh::sim::promoted_arm::hr_llm_strat_unit_ctrl_group_assign)
MH_EXPORT_REPLACE(llm_strat_unit_ctrlgroup_add_member, mh::sim::promoted_arm::hr_llm_strat_unit_ctrlgroup_add_member)
MH_EXPORT_REPLACE(llm_strat_unit_ctrlgroup_remove_member, mh::sim::promoted_arm::hr_llm_strat_unit_ctrlgroup_remove_member)
MH_EXPORT_REPLACE(llm_strat_unit_get_coords, mh::sim::promoted_arm::hr_llm_strat_unit_get_coords)
MH_EXPORT_REPLACE(llm_unit_state_is_boarding, mh::sim::promoted_arm::hr_llm_unit_state_is_boarding)
MH_EXPORT_REPLACE(llm_strat_order_ctrlgrp_flash_member, mh::sim::promoted_arm::hr_llm_strat_order_ctrlgrp_flash_member)
MH_EXPORT_REPLACE(llm_strat_order_ctrlgrp_select_member, mh::sim::promoted_arm::hr_llm_strat_order_ctrlgrp_select_member)
MH_EXPORT_REPLACE(llm_strat_planet_distance, mh::sim::promoted_arm::hr_llm_strat_planet_distance)
// clang-format on

namespace mh::sim {

bool hostreach_promotion_active() { return promoted_arm::g_hr_promote_installed; }

int install_promotion_hostreach(int default_on) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: PROMOTION IS INJECTION, so the whole installer is hosted. Its `seams[]` table
    // pairs each callee's ORIGINAL ENTRY ADDRESS with the generated installer, and standalone every
    // one of those installers already refuses (addr/mh_export.gen.h's standalone arm) -- but the
    // table is built on the stack before any of them is called, so the addresses reached the object
    // anyway: 76 distinct original VAs across the three installers that still carry one, measured
    // 2026-09-11 after the macro-level arms had taken everything else.
    //
    // 0, not a partial attempt: the existing contract is "0 = not promoted, treat this run as
    // unpromoted", and a standalone build is exactly a run where nothing is promoted because there
    // is nothing to promote OVER. The ini read below would also be meaningless -- there is no ini.
    (void)default_on;
    return 0;
#else
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        uintptr_t   addr;
        bool (*install)();
    };
    // clang-format off
    const entry seams[] = {
        {"game_SetEvent",                          mh::exp::addr_game_SetEvent,                          mh_export_install_game_SetEvent},
        {"llm_bldg_finish_current_order",          mh::exp::addr_llm_bldg_finish_current_order,          mh_export_install_llm_bldg_finish_current_order},
        {"llm_bldg_footprint_is_clear",            mh::exp::addr_llm_bldg_footprint_is_clear,            mh_export_install_llm_bldg_footprint_is_clear},
        {"llm_game_player_set_human",              mh::exp::addr_llm_game_player_set_human,              mh_export_install_llm_game_player_set_human},
        {"llm_prod_planet_distance_factor",        mh::exp::addr_llm_prod_planet_distance_factor,        mh_export_install_llm_prod_planet_distance_factor},
        {"llm_strat_bldg_shuttle_slot_is_free",    mh::exp::addr_llm_strat_bldg_shuttle_slot_is_free,    mh_export_install_llm_strat_bldg_shuttle_slot_is_free},
        {"llm_strat_bldg_uses_workers",            mh::exp::addr_llm_strat_bldg_uses_workers,            mh_export_install_llm_strat_bldg_uses_workers},
        {"llm_strat_ctrl_group_contains_unit",     mh::exp::addr_llm_strat_ctrl_group_contains_unit,     mh_export_install_llm_strat_ctrl_group_contains_unit},
        {"llm_strat_locate_active_port",           mh::exp::addr_llm_strat_locate_active_port,           mh_export_install_llm_strat_locate_active_port},
        {"llm_strat_pixel_delta_wrapped",          mh::exp::addr_llm_strat_pixel_delta_wrapped,          mh_export_install_llm_strat_pixel_delta_wrapped},
        {"llm_strat_storage_purge_dead_docked",    mh::exp::addr_llm_strat_storage_purge_dead_docked,    mh_export_install_llm_strat_storage_purge_dead_docked},
        {"llm_strat_tile_delta_wrapped",           mh::exp::addr_llm_strat_tile_delta_wrapped,           mh_export_install_llm_strat_tile_delta_wrapped},
        {"llm_strat_tile_dist_wrapped",            mh::exp::addr_llm_strat_tile_dist_wrapped,            mh_export_install_llm_strat_tile_dist_wrapped},
        {"llm_strat_unit_ctrl_group_assign",       mh::exp::addr_llm_strat_unit_ctrl_group_assign,       mh_export_install_llm_strat_unit_ctrl_group_assign},
        {"llm_strat_unit_ctrlgroup_add_member",    mh::exp::addr_llm_strat_unit_ctrlgroup_add_member,    mh_export_install_llm_strat_unit_ctrlgroup_add_member},
        {"llm_strat_unit_ctrlgroup_remove_member", mh::exp::addr_llm_strat_unit_ctrlgroup_remove_member, mh_export_install_llm_strat_unit_ctrlgroup_remove_member},
        {"llm_strat_unit_get_coords",              mh::exp::addr_llm_strat_unit_get_coords,              mh_export_install_llm_strat_unit_get_coords},
        {"llm_unit_state_is_boarding",             mh::exp::addr_llm_unit_state_is_boarding,             mh_export_install_llm_unit_state_is_boarding},
        // LIB-REF-IN: the three ex-residue rows. Appended rather than sorted in, so the install
        // ORDER of the original eighteen is bit-for-bit what it was -- these rows are leaves of
        // unrelated sub-systems, so order carries no meaning, but "the eighteen are untouched" is
        // easier to read off a diff than to argue.
        {"llm_strat_order_ctrlgrp_flash_member",   mh::exp::addr_llm_strat_order_ctrlgrp_flash_member,   mh_export_install_llm_strat_order_ctrlgrp_flash_member},
        {"llm_strat_order_ctrlgrp_select_member",  mh::exp::addr_llm_strat_order_ctrlgrp_select_member,  mh_export_install_llm_strat_order_ctrlgrp_select_member},
        {"llm_strat_planet_distance",              mh::exp::addr_llm_strat_planet_distance,              mh_export_install_llm_strat_planet_distance},
    };
    // clang-format on

    int ok = 0, attempted = 0;
    for (const entry &e : seams) {
        // The per-row red ladder: one row can be rolled back without rebuilding or disarming the
        // other 20. These 21 are leaves of unrelated sub-systems, not a call closure, so unlike
        // ai_promote.cpp a hole here really is just one row staying original.
        ++attempted;
        if (e.install()) {
            ++ok;
            mh::ai::ai_say("; [promote] sim_hostreach: + %s\n", e.name);
        } else {
            // Distinguish "another detour already owns this entry" (name it -- the fix is a rebind,
            // not a rebuild) from "the entry bytes don't match this build" (wrong image), the same
            // discipline sim_lt_promote.cpp and sim_order_dispatch.cpp already use.
            if (const char *owner = mh::hosthook::entry_owner_of(e.addr))
                mh::ai::ai_say(
                    "; [promote] sim_hostreach: seam %s REFUSED -- the entry is held by %s. NOT "
                    "necessarily a build problem: an owned entry means another detour got there "
                    "first (rebind it, or accept this row stays original)\n",
                    e.name, owner);
            else
                mh::ai::ai_say("; [promote] sim_hostreach: seam %s REFUSED -- entry guard "
                               "mismatch, NOT promoted\n",
                               e.name);
        }
    }

    if (ok != attempted) {
        mh::ai::ai_say("; [promote] sim_hostreach: %d/%d seams installed -- PARTIAL, treat this "
                       "run as invalid\n",
                       ok, attempted);
    } else {
        mh::ai::ai_say("; [promote] sim_hostreach: ALL %d seams installed -- the front-end-reached "
                       "sim leaves are LIVE\n",
                       ok);
    }

    promoted_arm::g_hr_promote_installed = ok > 0;
    return ok;
#endif // MH_LIBMH_BUILD
}

} // namespace mh::sim
