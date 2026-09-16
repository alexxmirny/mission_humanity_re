//
// sim/sim_bldg_apply_damage.cpp -- see sim_bldg_apply_damage.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_apply_damage_004710ba.asm); the Ghidra .c draft checks out completely and
// is cited only as corroboration -- see the header banner for the full derivation.
//
#include "sim/sim_bldg_apply_damage.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // mh::ai::REF_BLDG_BIT + ai_say / trace_budget (the shared trace sink)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_apply_damage_calls &live_bldg_apply_damage_calls() {
    static const bldg_apply_damage_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_notify_object_removed),
        MH_LIBMH_BIND(llm_strat_bldg_state_destroyed),
        MH_LIBMH_BIND(llm_strat_bldg_update_charge_pips),
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// DAT_005012d4 -- read-only image `double` constant at 0x005012d4, added to buildings[cur_player][0].
// energy (that player's building slot 0, HQ) when the current building dies. RESOLVED by the conductor
// via ReVA read-memory: raw bytes `00 00 00 00 00 00 F0 BF` = IEEE754 double -1.0 exactly -- a per-death
// DECREMENT, not a positive refund (same shape and same value as sim_unit_apply_damage.cpp's
// _G_LLM_STRAT_UNIT_DEATH_HQ_ENERGY_CREDIT; both read as a per-death counter on the HQ/slot-0 record
// rather than literal HP restoration, despite the earlier plate's "energy refund" wording).
inline constexpr double BLDG_DESTROY_HQ_ENERGY_CREDIT = -1.0; // DAT_005012d4

} // namespace

namespace detail {

void bldg_apply_damage(const sim_view &v, sim_store &own, const bldg_apply_damage_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- top guard (0x004710d2-0x004710e8): a DESTROYED or RUBBLE_SIGHT_DECAY building takes no
    // further damage processing at all -- skips even the unconditional tail below.
    if (b.state == BLDG_STATE_DESTROYED || b.state == BLDG_STATE_RUBBLE_SIGHT_DECAY) return;

    // ---- energy -= pending_damage; pending_damage = 0.0 (0x004710ef-0x00471116) -------------------
    // Both are `double` fields (offsets 0x19/0x21) -- the HP-like ENERGY stat (docs/conventions.md#energy-is-not-power), unrelated to
    // the POWER resource. The two-dword-zero store the asm performs on pending_damage is Watcom's
    // plain `= 0.0` idiom, not two int32 writes -- see the header note.
    b.energy -= b.pending_damage;
    b.pending_damage = 0.0;

    // ---- lethal-drop check (0x0047111b-0x00471123): FLDZ; FCOMP energy; JC skips this arm when
    // energy>0.0, so the arm runs when energy<=0.0 -- see the header's FCOMP/SAHF derivation.
    if (b.energy <= 0.0) {
        b.energy = 0.0; // second double field, same two-dword-zero idiom (0x0047112a/0x00471131)

        // Fixed HQ energy refund to the player's home building, buildings[cur_player][0] -- slot 0
        // specifically, not the current (dying) building. own.building_at() is the roster's ordinary
        // mutable accessor (same one SIM1C's order dispatcher uses), not a new accessor.
        own.building_at(static_cast<uint32_t>(*v.cur_player), 0).energy +=
            BLDG_DESTROY_HQ_ENERGY_CREDIT; // DAT_005012d4

        b.state = BLDG_STATE_DESTROYED;

        // flags = (cur_player | REF_BLDG_BIT) zero-extended; object_index = cur_index; hard_remove=0
        // (XOR EBX,EBX at 0x00471162) -- see the header's packed-ref derivation.
        c.ai_notify_object_removed(static_cast<uint32_t>(*v.cur_player) | mh::ai::REF_BLDG_BIT,
                                   static_cast<uint32_t>(*v.cur_index), /*hard_remove=*/0);
        c.bldg_state_destroyed();
    }

    // ---- tail (0x00471180-0x004711b9): unconditional once past the top guard -- reached either by
    // falling through the lethal-drop arm above or by the JC that skipped it. No re-check of state
    // between the lethal-drop arm and here.
    c.bldg_update_charge_pips(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
    c.refresh_building(*v.cur_player, static_cast<int32_t>(*v.cur_index));
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_apply_damage() {
    sim_state st = state();
    detail::bldg_apply_damage(st.read, st.own, live_bldg_apply_damage_calls());
}


} // namespace mh::sim
