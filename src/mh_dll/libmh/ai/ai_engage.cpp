//
// ai/ai_engage.cpp -- see ai_engage.h. Translated from the DISASSEMBLY (tmp/decomp_ai0/*.asm), not
// from Ghidra's C: the decompile of these three is wrong in four places that matter, each noted at
// the site below.
//
#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE / the entry-thunk shapes
#include "ai/ai_engage.h"


namespace mh::ai {
namespace detail {

namespace {

// The swap-remove the two consumers share, byte for byte with the original: the last entry is
// copied over slot `i` UNLESS `i` already IS the last, and the count is decremented either way.
// `i` is deliberately NOT advanced by the caller afterwards -- the entry now at `i` has not been
// examined yet.
void swap_remove(const ai_store &own, int32_t i) {
    const int32_t last = *own.engage_scratch_count - 1;
    if (i != last) own.engage_scratch[i] = own.engage_scratch[last];
    --*own.engage_scratch_count;
}

// The filter both consumers run. Keeps a candidate iff the attacker can shoot at it (a
// ground-capable weapon for anything flagged 0xc0, an AA weapon for anything flagged 0x20) and it
// is still alive. Everything else is swap-removed.
//
// NOTE THE 0xc0 TEST. The original tests `byte ptr [ref], 0xc0` -- 0xc0, not the 0x40 the roster
// selector uses, and not the 0xa0 the liveness helper uses. Three different masks over the same
// byte in one function; they are transcribed as they are.
void filter_by_weapons_and_liveness(const ai_store &own, const ai_calls &gc, int32_t ground,
                                    int32_t aa) {
    int32_t i = 0;
    while ((uint32_t)i < (uint32_t)*own.engage_scratch_count) {
        const engage_candidate &c = own.engage_scratch[i];
        bool                    keep;
        if ((c.target_ref & 0xc0u) != 0 && ground == 0) keep = false;
        else if ((c.target_ref & 0x20u) != 0 && aa == 0) keep = false;
        else keep = gc.target_ref_is_alive(c.target_ref, c.target_index) != 0;

        if (keep) ++i;
        else swap_remove(own, i);
    }
}

// The pick both consumers run after sorting: the first candidate whose accumulated incoming damage
// is still BELOW its own energy, i.e. the first one that is not already dead to shots in flight.
// Returns the index, or the (current) count if there is none.
//
// TWO THINGS A "SENSIBLE" REIMPLEMENTATION GETS WRONG, both from the original's FILD/FCOMP pair:
//  * the roster is chosen by (ref & 0x40) here -- the OPPOSITE polarity, and a different bit, from
//    the (ref & 0xa0) that the liveness helper and the commit branch below use. See the target_ref
//    field comment in addr/mh_structs.gen.h: they agree on class nibbles 2 and 4 and disagree on 0.
//  * the damage tally is loaded with MOVZX from an int16 field and only then converted to double,
//    so a NEGATIVE tally compares as a large positive and the candidate is rejected. Casting the
//    signed field straight to double would silently change which target is picked.
int32_t pick_first_survivable(const ai_view &v, const ai_store &own) {
    int32_t i = 0;
    while ((uint32_t)i < (uint32_t)*own.engage_scratch_count) {
        const engage_candidate &c = own.engage_scratch[i];
        bool                    survivable;
        if (ref_is_building_by_40(c.target_ref)) {
            const building &b = building_of(v, ref_owner(c.target_ref), c.target_index);
            survivable        = (double)(uint16_t)b.incoming_damage_tally < b.energy;
        } else {
            const unit &u = unit_of(v, ref_owner(c.target_ref), c.target_index);
            survivable    = (double)(uint16_t)u.incoming_threat_damage < u.energy;
        }
        if (survivable) break;
        ++i;
    }
    return i;
}

} // namespace

int32_t scan_target_list_for_engage_candidates(const ai_view &v, const ai_calls &gc, int32_t player,
                                               uint16_t ai_group_mask) {
    const player_data &pd      = v.players[player];
    int32_t            offered = 0;
    // UNSIGNED compare against the count (JC, not JL), so a negative count would run the loop over
    // essentially the whole address space rather than skipping it. The original is written that way.
    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];
        if ((e.victim_ref & 0xa0u) == 0) continue;

        // The mask is a ushort passed in DX, but the original TESTs the full 32-bit register
        // against a MOVZX'd 16-bit ai_group_index -- so the inherited high half of the caller's EDX
        // is ANDed against zeros and cannot affect the result. Widening it here is faithful.
        const unit &target = unit_of(v, (uint32_t)player, e.victim_index);
        if (((uint32_t)ai_group_mask & (uint32_t)target.ai_group_index) == 0) continue;

        // Hostility is a SIGN test: `CMP <relation>,-1 / JG skip` keeps only relation <= -1.
        if (pd.ai_player_relation[e.aggressor_ref & 0xf] > -1) continue;

        gc.engage_candidate_add(e.aggressor_ref, e.aggressor_index);
        ++offered;
    }
    return offered;
}

bool engage_select_and_commit(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player, int32_t unit_index, int32_t use_alt_commit) {
    if (*own.engage_scratch_count != 0) {
        const int32_t ground = gc.unit_has_ground_weapon((uint32_t)player, unit_index);
        const int32_t aa     = gc.unit_has_aa_weapon((uint32_t)player, unit_index);
        filter_by_weapons_and_liveness(own, gc, ground, aa);
    }
    if (*own.engage_scratch_count == 0) return false;

    // `OR AL,0x80` on the player index: the attacker is a UNIT, in the packed-ref encoding the sort
    // helper reads. Only the low byte is OR'd, which for a 0..7 player index is the same value.
    // THIRD ARGUMENT = the ORIGINAL's ambient ESI at 0x004edd8e, recovered rather than guessed
    // (REBIND-AI-ESI, 2026-09-10). This function's sole caller is llm_strat_unit_passive_engage_tick
    // (0x004ee50e), whose ESI is the unit-slot index -- seeded `MOV ESI,0x1` at 0x004ee559, only ever
    // `INC ESI` at 0x004ee724, and handed to us as EDX by `MOV EDX,ESI` at each of its four call
    // sites. So the original's flag IS `unit_index`, and it is >= 1 at every one of them. (On the
    // path where the weapon/liveness filter above swap-removed an entry the original's ESI is
    // instead a scratch POINTER, `LEA ESI,[EAX+0xfb4db0]` at 0x004edd5f -- a different value, the
    // same verdict, since the callee only tests it against zero.) NON-ZERO means the sort phase
    // never runs: see ai_engage_scan.cpp phase 2. Passing 0 here would sort a list the shipped game
    // leaves in producer order.
    gc.engage_sort_candidates_by_dist((uint32_t)player | 0x80u, unit_index, unit_index);
    gc.engage_partition_turret_candidates();

    const int32_t i = pick_first_survivable(v, own);
    if (i == *own.engage_scratch_count) return false;

    const engage_candidate &c = own.engage_scratch[i];
    if (use_alt_commit != 0)
        gc.commit_attack_order_alt((uint32_t)player, unit_index, c.target_ref, c.target_index);
    else
        gc.commit_attack_order((uint32_t)player, unit_index, c.target_ref, c.target_index);
    return true;
    // The original's two exits are `MOV EAX,1` / `XOR EAX,EAX` followed by a JMP to 0x004ec1ee,
    // which is a Watcom SHARED EPILOGUE and not a function -- Ghidra materialised it as one and the
    // decompile therefore shows a phantom call and an `extraout_AL` return. There is nothing to
    // call here. (Renamed llm_watcom_epilogue_004ec1ee, 2026-08-01.)
}

int32_t engage_filter_and_commit_target(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                        uint32_t attacker_ref, int32_t context) {
    if (*own.engage_scratch_count != 0) {
        // Same two predicates as the sibling above, with the attacker given as a PACKED REF rather
        // than a bare player index -- the callees mask it themselves.
        const int32_t ground = gc.unit_has_ground_weapon(attacker_ref, context);
        const int32_t aa     = gc.unit_has_aa_weapon(attacker_ref, context);
        filter_by_weapons_and_liveness(own, gc, ground, aa);
    }
    if (*own.engage_scratch_count == 0) return 0;

    // NO TURRET PARTITION on this path -- the sibling has one and this one does not. Adding it
    // would change which target is picked whenever a turret is in range.
    // Same recovered third argument as the sibling above, same single caller: at 0x004ee71f
    // llm_strat_unit_passive_engage_tick does `MOV EDX,ESI` before the call, so the original's
    // ambient ESI at 0x004edf50 is `context` -- the unit-slot index, >= 1. (Swap-removal replaces it
    // with `LEA ESI,[ESI+0xfb4db0]` at 0x004edf21; non-zero either way.)
    gc.engage_sort_candidates_by_dist(attacker_ref | 0x80u, context, context);

    const int32_t i = pick_first_survivable(v, own);
    if (i == *own.engage_scratch_count) return 0;

    // THE DECOMPILE IS WRONG HERE and the disassembly is not: Ghidra renders the building branch's
    // third argument as `(ref & 0xff) & 0xffffff0f` and the unit branch's as `ref & 0xf`. Both
    // branches emit the IDENTICAL five instructions (`MOV AX,word[ref] / XOR AH,AH / AND AL,0xf /
    // MOVZX EBX,AX`), so both are simply the owner nibble.
    const engage_candidate &c            = own.engage_scratch[i];
    const uint16_t          attacker     = (uint16_t)attacker_ref;
    const uint16_t          target_owner = (uint16_t)(c.target_ref & 0x0fu);
    const uint32_t          weapon_id    = 4; // `PUSH 0x4`, both branches

    if (ref_is_building_by_a0(c.target_ref))
        gc.order_attack_building_enqueue(attacker, context, target_owner, c.target_index, weapon_id);
    else
        gc.order_attack_unit_enqueue(attacker, context, target_owner, c.target_index, weapon_id);
    return 1;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t scan_target_list_for_engage_candidates(int32_t player, uint16_t ai_group_mask) {
    const ai_state st = state();
    return detail::scan_target_list_for_engage_candidates(st.read, live_calls(), player,
                                                          ai_group_mask);
}

bool engage_select_and_commit(int32_t player, int32_t unit_index, int32_t use_alt_commit) {
    const ai_state st = state();
    return detail::engage_select_and_commit(st.read, st.own, live_calls(), player, unit_index,
                                            use_alt_commit);
}

int32_t engage_filter_and_commit_target(uint32_t attacker_ref, int32_t context) {
    const ai_state st = state();
    return detail::engage_filter_and_commit_target(st.read, st.own, live_calls(), attacker_ref,
                                                   context);
}

// ---- differential-oracle arms -------------------------------------------------------------------
//
// All three run on mh::ai::shadow_calls(), which stubs ONLY the calls that escape the declared
// region set -- see the rationale on shadow_calls() in ai_state.h and the per-site `why_extra` in
// the shadow manifest. The two consumers keep their sort/partition callees REAL,
// because those write nothing but the engage scratch, which every one of these sites declares and
// therefore restores between the arms; stubbing them would guarantee the divergence it looks like
// it is preventing.
//
// The signatures here are the GENERATED ones (mh::exp::sig_*), which is why the second parameter of
// engage_filter_and_commit_target is uint32_t: the site's dispatcher is typed from the committed
// Ghidra prototype, not from the module's own wrapper. engage_select_and_commit's return was uint8_t
// for the same reason until SIM-READY (2026-08-07) corrected the Ghidra prototype from `bool`
// (1 byte) to `int`: the body sets the FULL EAX on both exit paths (MOV EAX,0x1 @0x004ede7a /
// XOR EAX,EAX @0x004ede84), and declaring it a byte is what produced `CONCAT31(extraout_var, ...)`
// in every caller's decompile -- five of them blocked on R3 because of this one return type.

} // namespace mh::ai
