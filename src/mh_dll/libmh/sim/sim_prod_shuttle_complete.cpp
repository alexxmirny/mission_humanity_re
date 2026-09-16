//
// sim/sim_prod_shuttle_complete.cpp -- see sim_prod_shuttle_complete.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_production_complete_0048d886.asm), not from the Ghidra .c draft
// (whose PLATE comment misnames the throttle==10 callee -- see the header).
//
#include "sim/sim_prod_shuttle_complete.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const production_complete_calls &live_production_complete_calls() {
    static const production_complete_calls c = {
        MH_CRT(utils_w_str_copy),
        MH_CRT(utils_concat),
        mh::state::evt::snd_play,
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_locate_active_port),
        MH_LIBMH_BIND(llm_strat_bldg_find_mothership_position),
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
        MH_LIBMH_BIND(llm_strat_prod_deliver_arrivals),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`). Value-for-value
// C's truncating `/ 32` -- see sim_order_enqueue.cpp's fine_to_tile() for the original verification;
// re-derived locally per this project's per-TU convention (libmh/sim/ TUs each define their own copy
// rather than sharing one, per the "write only your own new files" rule).
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// DAT_005014c8 / DAT_005014ce, the off-planet delivery message's prefix/suffix wide-string literals
// wrapped around the destination planet's name -- resolved by the conductor via ReVA read-memory
// (both null-terminated, 2 wchar_t + terminator each): 0x005014c8 = {0x0020, 0x0028, 0} = L" (";
// 0x005014ce = {0x0029, 0} = L")". So the composed message reads "<base text> (<planet name>)".
constexpr const wchar_t *TEXT_PLANET_PREFIX = L" ("; // 0x005014c8
constexpr const wchar_t *TEXT_PLANET_SUFFIX = L")";  // 0x005014ce

} // namespace

namespace detail {

void prod_shuttle_complete(const sim_view &v, sim_store &own, const production_complete_calls &c,
                           uint32_t player, uint32_t slot, double elapsed_time) {
    // reimpl-verify (2026-08-13) found a real divergence: the original re-loads player/slot from
    // their stack slots via MOVZX-WORD (a 16-bit truncation) before EVERY address computation that
    // indexes the shuttle-slot record and before every call that forwards them onward (confirmed at
    // 13 separate record-address sites plus all 3 outbound calls, e.g. 0x0048d8a3/0x0048d8ad,
    // 0x0048d96a/0x0048d96e before CALL llm_strat_prod_shuttle_slot_release). The committed export
    // thunk supplies the RAW unmasked registers (mh_export.gen.h's MH_EXPORT_THUNK_..._callee does a
    // bare PUSH EDX/PUSH EAX, no clearing), so a caller with garbage in the upper 16 bits of either
    // argument is not hypothetical. Mask ONCE here, mirroring what line ~90 below already does for
    // the is-local-player compare (CMP AX,word[PlayerSide]) -- an inconsistency the original
    // translation had introduced by applying the truncation in one place but not the rest.
    const uint32_t p  = player & 0xFFFFu;
    const uint32_t sl = slot & 0xFFFFu;

    // ONE fetch, reused for every read/write below (task brief hazard note: never re-resolve).
    prod_shuttle_slot &s = own.prod_shuttle_slot_at(p, static_cast<int32_t>(sl));

    // 0x0048d8b9-0x0048d8c5: FLD travel_duration; FCOMP elapsed_time; JBE -> transit finished.
    if (!(s.travel_duration <= elapsed_time)) {
        // 0x0048d8c7-0x0048d8ec: else arm -- decrement and return immediately. No other field touched,
        // no callee invoked.
        s.travel_duration -= elapsed_time;
        return;
    }

    // ---- transit finished (LAB_0048d8f1) --------------------------------------------------------
    //
    // 0x0048d8f1-0x0048d8fc: bump the shared throttle counter, capped at THROTTLE_INCREMENT_CAP(5).
    // DECLARED NEED 1 (see header): own.prod_complete_throttle() does not exist on sim_store yet.
    if (own.prod_complete_throttle() < THROTTLE_INCREMENT_CAP) {
        ++own.prod_complete_throttle();
    }
    // 0x0048d900: a SECOND `CMP throttle,5` follows here in the asm with no consuming branch anywhere
    // near it -- dead/inert (a bare compare has no observable effect), omitted. See header + uncertainties.

    // 0x0048d907-0x0048d97c: the throttle==10 quiet-release short-circuit.
    if (own.prod_complete_throttle() == THROTTLE_RELEASE_THRESHOLD) {
        const uint32_t unit_type = v.cfg_units[s.type_ref_id].type;
        if (unit_type != UNIT_TYPE_A_HELI_MOTHER && unit_type != UNIT_TYPE_H_HELI_MOTHER) {
            // 0x0048d96a-0x0048d977: release the slot and return immediately -- no message, no camera
            // pan, no slot-field reset, no deliver_arrivals.
            c.prod_shuttle_slot_release(static_cast<int32_t>(p), static_cast<int32_t>(sl));
            return;
        }
        // type IS a heli-mother variant -- falls through to the normal path below exactly as if
        // throttle were not 10 (LAB_0048d968 -> LAB_0048d97c).
    }

    // ---- normal path (LAB_0048d97c): local-player message + camera pan, then always reset --------
    const bool is_local_player = static_cast<uint16_t>(p) == static_cast<uint16_t>(*v.player_side);

    if (is_local_player) {
        // 1. base message text (0x0048d98c-0x0048d99c). Param order (src, dst) matches the committed
        //    utils_w_str_copy(void *src, void *dst) signature and its register marshalling exactly.
        c.w_str_copy(const_cast<wchar_t *>(v.text_ptrs[TEXT_ID_PRODUCTION_COMPLETE]), own.text_scratch());

        // 2. off-planet delivery: append "<prefix><planet name><suffix>" (0x0048d9b9-0x0048da16).
        if (static_cast<int32_t>(static_cast<uint16_t>(s.dest_planet)) != *v.planet_index) {
            c.concat(own.text_scratch(), const_cast<wchar_t *>(TEXT_PLANET_PREFIX));
            const int32_t planet_name_id = v.cfg_planets[static_cast<uint16_t>(s.dest_planet)].name;
            c.concat(own.text_scratch(), const_cast<wchar_t *>(v.text_ptrs[planet_name_id]));
            c.concat(own.text_scratch(), const_cast<wchar_t *>(TEXT_PLANET_SUFFIX));
        }

        // 3. always printed once the local-player gate passed (0x0048da1b-0x0048da25). Return value
        //    discarded, matching the original.
        c.print_text_message(own.text_scratch());

        // 4. camera pan runs ONLY for a delivery landing on the currently-viewed planet
        //    (0x0048da3b-0x0048da48).
        if (static_cast<int32_t>(static_cast<uint16_t>(s.dest_planet)) == *v.planet_index) {
            const uint32_t unit_type = v.cfg_units[s.type_ref_id].type;

            if (unit_type == UNIT_TYPE_A_HELI_CARGO || unit_type == UNIT_TYPE_H_HELI_CARGO) {
                // 5a. cargo-heli arrival: port sound + locate_active_port (0x0048daaa-0x0048db3d).
                if (*v.sim_active != 0) {
                    const int32_t sound_id =
                        (*v.player_race == 2 ? RACE2_SOUND_OFFSET : 0) + SND_PORT_ARRIVAL_BASE;
                    c.snd_play(sound_id, SND_VOLUME);
                }
                uint32_t port_slot_scratch = 0; // out-param, discarded after the call (same as the
                                                // original's local_28 -- never read again)
                const uint32_t found = c.locate_active_port(
                    p, &own.cam_pan_target_col(), &own.cam_pan_target_row(), &port_slot_scratch);
                if (found == 0) {
                    own.cam_pan_target_col() = CAM_PAN_TARGET_UNSET; // ROW deliberately left untouched
                } else {
                    own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
                    own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());
                }
            } else {
                // 5b. mothership/other arrival: mothership sound + find_mothership_position
                //     (0x0048db42-0x0048dbc8).
                if (*v.sim_active != 0) {
                    const int32_t sound_id =
                        (*v.player_race == 2 ? RACE2_SOUND_OFFSET : 0) + SND_MOTHERSHIP_ARRIVAL_BASE;
                    c.snd_play(sound_id, SND_VOLUME);
                }
                // cam_pan_target_col/row() are plain int32_t fields (sim_state.h), used as int32_t by
                // the sibling locate_active_port call above; find_mothership_position's committed
                // out-params are uint32_t * (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast here.
                const uint32_t found = c.find_mothership_position(
                    static_cast<int32_t>(p), reinterpret_cast<uint32_t *>(&own.cam_pan_target_col()),
                    reinterpret_cast<uint32_t *>(&own.cam_pan_target_row()));
                if (found == 0) {
                    own.cam_pan_target_col() = CAM_PAN_TARGET_UNSET; // ROW deliberately left untouched
                } else {
                    own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
                    own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());
                }
            }
        }
    }

    // ---- unconditional tail (LAB_0048dbd2): always reached once the local-player gate above is
    // resolved either way -- including the off-planet skip and the JNZ-past-camera-pan skip
    // (0x0048dbd2-0x0048dc55).
    s.travel_duration = 0.0; // two dword-zero stores in the asm -- ordinary 0.0
    s.status          = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet   = s.dest_planet;
    c.prod_deliver_arrivals();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void prod_shuttle_complete(uint32_t player, uint32_t slot, double elapsed_time) {
    sim_state st = state();
    detail::prod_shuttle_complete(st.read, st.own, live_production_complete_calls(), player, slot,
                                  elapsed_time);
}


} // namespace mh::sim
