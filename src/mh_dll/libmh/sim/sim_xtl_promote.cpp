//
// sim/sim_xtl_promote.cpp -- see sim_xtl_promote.h.
//
#include "sim/sim_xtl_promote.h"

#include "addr/mh_export.gen.h" // sig_<name>, MH_EXPORT_REPLACE, mh_export_install_<name>()
#include "ai/ai_state.h"        // ai_say -- the trace sink every mh::sim promoted_arm TU logs through

#include "state/hook_api.h" // F4D-PRE: entry_owner_of -- the host's hook table, never hook/ directly

// LIB-REF-IN: the adapters forward through the INBOUND C ENTRY, not into the C++ wrapper. See the
// header banner for the routing table and the ordering hazard; see sim/sim_hostreach_promote.cpp
// for the mechanism. All three of this module's rows route -- there is no printed exception here.
#include "../../libmh/include/libmh_host_in.h"

// The three headers that declare the public live wrappers these adapters' `sig_` typedefs are
// written against. KEPT for the same reason sim_hostreach_promote.cpp keeps its fifteen: the
// C entries are declared by libmh_host_in.h, the C++ wrappers they forward into are declared here,
// and removing them would hide a real dependency.
#include "sim/sim_bldg_init_all.h"
#include "sim/sim_bldg_instant_construct_find_slot.h"
#include "sim/sim_unit_order_disembark_soldiers.h"


namespace mh::sim {

namespace promoted_arm {

// `mh::sim::promoted_arm` is shared with sim/sim_hostreach_promote.cpp,
// sim/hostreach/sim_batch_h_promote.cpp, sim/resid/resid_promote.cpp, sim/libtrans/sim_lt_promote.cpp
// and sim/sim_order_dispatch.cpp -- namespaces merge across translation units, so both the flag and
// the first-call helper stay internal-linkage (anonymous sub-namespace), matching those files'
// convention, so no two TUs can collide at link time.
namespace {
bool g_xtl_promote_installed = false;

// PER-ROW FIRST-CALL LIVENESS (see the header banner). `fired` is a static local owned by the
// calling adapter, so each of the 3 gets its own one-shot latch. The '(OURS is live)' suffix is
// exact -- tools/test_ui.py greps it -- do not reword it.
void xtl_mark_first_call(bool &fired, const char *orig_name) {
    if (fired) return;
    fired = true;
    mh::ai::ai_say("; [promote] sim_xtl: %s call #1 (OURS is live)\n", orig_name);
}
} // namespace

// clang-format off

// ---- sim/sim_bldg_init_all.h -----------------------------------------------------------------
// void __watcall llm_strat_bldg_init_all(void). Its own inbound entry, not a sequence member.
void xtl_llm_strat_bldg_init_all(void) {
    static bool fired = false;
    xtl_mark_first_call(fired, "llm_strat_bldg_init_all");
    ::libmh_bldg_init_all();
}

// ---- sim/sim_bldg_instant_construct_find_slot.h -----------------------------------------------
// int32_t __mh_watcall_ecx_ebx_volatile llm_strat_bldg_instant_construct_find_slot(
//     int32_t tile_col, int32_t tile_row, int32_t building_type_id, int32_t initial_workers,
//     uint32_t player).  THE RETURN IS LOAD-BEARING (the buildings-roster slot, or 0), so unlike
// every other order-id adapter in the tree this one returns libmh_issue_order's result rather than
// discarding it -- see the header banner for why that round trip is exact.
int32_t xtl_llm_strat_bldg_instant_construct_find_slot(int32_t tile_col, int32_t tile_row,
                                                      int32_t building_type_id,
                                                      int32_t initial_workers, uint32_t player) {
    static bool fired = false;
    xtl_mark_first_call(fired, "llm_strat_bldg_instant_construct_find_slot");
    const int32_t argv[5] = {tile_col, tile_row, building_type_id, initial_workers, (int32_t)player};
    return ::libmh_issue_order(LIBMH_ORD_BLDG_INSTANT_CONSTRUCT_FIND_SLOT, argv, 5u);
}

// ---- sim/sim_unit_order_disembark_soldiers.h --------------------------------------------------
// void __watcall llm_unit_order_disembark_soldiers(uint32_t player, int32_t unit_idx).
void xtl_llm_unit_order_disembark_soldiers(uint32_t player, int32_t unit_idx) {
    static bool fired = false;
    xtl_mark_first_call(fired, "llm_unit_order_disembark_soldiers");
    const int32_t argv[2] = {(int32_t)player, unit_idx};
    ::libmh_issue_order(LIBMH_ORD_UNIT_ORDER_DISEMBARK_SOLDIERS, argv, 2u);
}

} // namespace promoted_arm

MH_EXPORT_REPLACE(llm_strat_bldg_init_all, mh::sim::promoted_arm::xtl_llm_strat_bldg_init_all)
MH_EXPORT_REPLACE(llm_strat_bldg_instant_construct_find_slot, mh::sim::promoted_arm::xtl_llm_strat_bldg_instant_construct_find_slot)
MH_EXPORT_REPLACE(llm_unit_order_disembark_soldiers, mh::sim::promoted_arm::xtl_llm_unit_order_disembark_soldiers)
// clang-format on

bool xtl_promotion_active() { return promoted_arm::g_xtl_promote_installed; }

int install_promotion_xtl(int default_on) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: PROMOTION IS INJECTION, so the whole installer is hosted. Its `seams[]` table
    // pairs each callee's ORIGINAL ENTRY ADDRESS with the generated installer; compiling it
    // standalone would put three more original VAs into libmh.lib for no standalone purpose, and
    // LIB-VA0's recorded baseline is ZERO attributable hits. The MH_EXPORT_REPLACE macros above
    // already degrade to refusing installers under this define (addr/mh_export.gen.h's standalone
    // arm), so nothing here is lost -- this arm is what keeps their ADDRESSES out too.
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
        {"llm_strat_bldg_init_all",                    mh::exp::addr_llm_strat_bldg_init_all,                    mh_export_install_llm_strat_bldg_init_all},
        {"llm_strat_bldg_instant_construct_find_slot", mh::exp::addr_llm_strat_bldg_instant_construct_find_slot, mh_export_install_llm_strat_bldg_instant_construct_find_slot},
        {"llm_unit_order_disembark_soldiers",          mh::exp::addr_llm_unit_order_disembark_soldiers,          mh_export_install_llm_unit_order_disembark_soldiers},
    };
    // clang-format on

    int ok = 0, attempted = 0;
    for (const entry &e : seams) {
        // The per-row ladder. These three are LEAVES OF EACH OTHER (none calls another), so unlike
        // batch H's pipeline members a skip here really is "this row stays original" and IS a
        // behavioural bisect.
        ++attempted;
        if (e.install()) {
            ++ok;
            mh::ai::ai_say("; [promote] sim_xtl: + %s\n", e.name);
        } else {
            // Distinguish 'another detour already owns this entry' (name it -- the fix is a rebind,
            // not a rebuild) from 'the entry bytes do not match this build' (wrong image).
            if (const char *owner = mh::hosthook::entry_owner_of(e.addr))
                mh::ai::ai_say("; [promote] sim_xtl: seam %s REFUSED -- the entry is held by %s. NOT "
                               "necessarily a build problem: an owned entry means another detour got "
                               "there first (rebind it, or accept this row stays original)\n",
                               e.name, owner);
            else
                mh::ai::ai_say("; [promote] sim_xtl: seam %s REFUSED -- entry guard mismatch, NOT "
                               "promoted\n",
                               e.name);
        }
    }

    if (ok != attempted) {
        mh::ai::ai_say("; [promote] sim_xtl: %d/%d seams installed -- PARTIAL, treat this run as "
                       "invalid\n",
                       ok, attempted);
    } else {
        mh::ai::ai_say("; [promote] sim_xtl: ALL %d seams installed -- X-TL is LIVE\n", ok);
    }

    promoted_arm::g_xtl_promote_installed = ok > 0;
    return ok;
#endif // MH_LIBMH_BUILD
}

} // namespace mh::sim
