#include "sim/sim_invasion.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const invasion_calls &live_invasion_calls() {
    static const invasion_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
        MH_CRT(w_sprintf__vi), // DECLARED NEED 4 -- does not exist yet
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_spawn_enemy_landing),
        MH_LIBMH_BIND(llm_strat_revoke_invention),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_LIBMH_BIND(llm_strat_invasion_alert_arm),
    };
    return c;
}

namespace {

// UTF-16 "%s (%s)" @0x0050163b -- the asm's own inline operand comment (`; %s_(%s)`) already resolves
// this literal; reused verbatim rather than re-derived, same convention every sibling w_sprintf-calling
// sim TU follows for an already-named format string.
constexpr const wchar_t *TEXT_FMT_PLANET_NAME = L"%s (%s)";

// Conductor gap-close (2026-08-16): read-memory-confirmed. The 104 bytes at 0x0050164b
// (Ghidra label `u__0050164b`) are ALL ASCII SPACES (0x20 repeated, 52 wide chars) -- an
// unfilled/placeholder debug format string in the original, not a real `%d`-shaped message. Faithfully
// reproduced as-is: w_sprintf formats these 52 literal spaces into text_scratch() with the vararg
// int32 simply unconsumed (no conversion specifier present), matching the original's actual behavior.
// The call's entire effect is this dead store -- nothing downstream reads text_scratch() on this path.
constexpr const wchar_t *TEXT_FMT_UNVERIFIED_50164B =
    L"                                                   "; // 51 spaces, read-memory-exact

// `System[1].planets[1]` (the system's entry planet) via the RAW `system_define_index_base` accessor
// -- see the header's "SYSTEM ENTRY PLANET" section for the byte-offset derivation. Documented as
// `System[1].icon` until 2026-08-24; the address arithmetic never changed, only the field it lands on
// (finding 2026-08-24-0228-22).
constexpr int32_t SYSTEM_STRIDE_INTS     = 0x8c / 4; // 35
constexpr int32_t SYSTEM_HOME_INDEX      = 1;
constexpr int32_t SYSTEM_ICON_INT_OFFSET = 0x10 / 4; // 4

int32_t home_planet_index(const sim_view &v) {
    return v.system_define_index_base[SYSTEM_HOME_INDEX * SYSTEM_STRIDE_INTS + SYSTEM_ICON_INT_OFFSET];
}

} // namespace

namespace detail {

// game_HandleInvasion @0x004996c4. See the header banner for the full per-address derivation.
int32_t handle_invasion(const sim_view &v, sim_store &own, const invasion_calls &c, int32_t planet,
                        double since_time) {
    // 0x004996df-0x004996e8: unconditional, computed regardless of which arm below consumes it (only
    // the distant-planet arm actually reads it).
    const double time_delta = *v.game_clock - since_time;

    // 0x004996ee-0x00499745: the eligibility gate. Skipped entirely (eligible=true) when `planet` is
    // the one currently being viewed; otherwise the local player must already have researched this
    // planet's invention AND the planet's status must be CONQUERED.
    bool eligible = (planet == *v.planet_index);
    if (!eligible) {
        // reimpl-verify (2026-08-16): MOVZX at 0x004996f6 zero-extends PlayerSide -- a plain
        // int16_t->int32_t cast here would SIGN-extend instead, diverging from the original whenever
        // PlayerSide's stored bit pattern has bit 15 set. Widen through uint16_t first, matching this
        // file's own two other PlayerSide reads (lines below) and the original's MOVZX at
        // 0x004997ab/0x00499868.
        const int32_t          player  = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
        const int32_t          inv_row = v.cfg_planets[planet].invention_index;
        const player_progress &prog    = progress_of(v, player, inv_row);
        eligible                       = (prog.acquired == 1) && (v.planet_status[planet] == PLANET_STATUS_CONQUERED);
        if (!eligible) {
            // 0x00499747-0x00499775: THE DEAD CASCADE -- see the header banner. Reassembled as a
            // direct return rather than transcribing the value-independent CMP chain (uncertainties[]).
            return 0;
        }
    }

    if (planet == *v.planet_index) {
        // ---- own-planet arm (0x0049977a-0x004997d7) ----
        const int32_t spawn_result = c.spawn_enemy_landing();
        if (spawn_result == 0) {
            // DECLARED NEED 2: sim_view::local_player_slot does not exist yet.
            c.player_presence_lost(static_cast<uint32_t>(*v.local_player_slot), 0);
        } else {
            c.print_text_message(const_cast<wchar_t *>(v.text_ptrs[TEXT_ID_INVASION_BEGUN]));
            c.revoke_invention(static_cast<uint16_t>(*v.player_side),
                               static_cast<uint16_t>(v.cfg_planets[planet].invention_index));
            // DECLARED NEED 3: sim_store::planet_status_at() does not exist yet.
            own.planet_status_at(planet) = PLANET_STATUS_INVASION;
        }
    } else {
        // ---- distant-planet arm (0x004997dc-0x00499898) ----
        // 0x004997dc-0x00499808: clamp to at-least planet_time[planet].
        double clamped = time_delta;
        if (clamped < own.planet_time_at(planet)) clamped = own.planet_time_at(planet);
        own.planet_invasion_time_at(planet) = clamped;

        c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_PLANET_NAME, v.text_ptrs[TEXT_ID_INVASION_ON],
                         v.text_ptrs[v.cfg_planets[planet].name]);
        c.print_text_message(own.text_scratch());
        c.revoke_invention(static_cast<uint16_t>(*v.player_side),
                           static_cast<uint16_t>(v.cfg_planets[planet].invention_index));
        // DECLARED NEED 3: same missing accessor as the own-planet arm above.
        own.planet_status_at(planet) = PLANET_STATUS_INVASION;
        c.invasion_alert_arm(planet, *v.game_clock);
    }
    return 1;
}

// llm_strat_invasion_chance_roll @0x0049953a. See the header banner for the full per-address
// derivation. Calls handle_invasion() directly (in-TU C++ call, not through `c`/mh::call::) per the
// batch context -- both functions are translated together this slice.
int32_t invasion_chance_roll(const sim_view &v, sim_store &own, const invasion_calls &c,
                             int32_t building_completed) {
    // 0x00499555-0x00499569: early-out. Note this does NOT depend on `building_completed` at all --
    // reproduced exactly (both branches of the outer OR converge here per the asm; see the header
    // pseudocode banner).
    if (*v.planet_index == 0x1f && *v.current_system == 0) return 0;

    if (building_completed == 1) {
        // 0x0049957b-0x004995b5: immediate roll against the CURRENT planet.
        const int32_t roll = c.rand_below(100);
        if (*v.planet_index != home_planet_index(v) && roll < 0x1e &&
            handle_invasion(v, own, c, *v.planet_index, 0.0) == 1) {
            return 1;
        }
    } else {
        // 0x004995bf-0x004996b0: scan all 32 planets of the current system.
        for (int32_t i = 0; i < 0x20; ++i) {
            if (v.cfg_planets[i].system_index != *v.current_system) continue; // 0x004995d9-0x004995ec
            if (i == home_planet_index(v)) continue;                          // 0x004995f2-0x004995fb

            // 0x004995fd-0x00499612: ALWAYS called once the two guards above pass, regardless of the
            // status gate below -- side-effect only (writes G_TEXT_TMP; DECLARED NEED 5 covers the
            // format string content). Return value discarded, matching both drafts.
            c.w_sprintf__vi(own.text_scratch(), TEXT_FMT_UNVERIFIED_50164B, i);

            // 0x0049961b-0x0049962d: FIRST read of planet_status[i].
            if (!(v.planet_status[i] == PLANET_STATUS_CONQUERED || i == *v.planet_index)) continue;

            int32_t roll = c.rand_below(100) - (*v.current_system) * 2; // 0x00499633-0x00499647
            // 0x0049964a-0x00499662: SECOND, independent read of planet_status[i] -- NOT cached from
            // the guard above (a call, c.rand_below, happened in between; per the const-view rule this
            // region must be re-read rather than assumed stable across it, even though rand_below has
            // no documented planet_status side effect).
            if (i == *v.planet_index && v.planet_status[i] != PLANET_STATUS_CONQUERED) {
                roll *= 3; // 0x00499666-0x0049966c
            }
            if (roll < 0xf) { // 0x0049966f-0x00499673
                const int32_t r = c.rand_below(2);
                const double  since_time =
                    *v.invasion_roll_base_time -
                    static_cast<double>((r + 1) * 60); // DECLARED NEED 1 -- 0x00499675-0x00499692
                if (handle_invasion(v, own, c, i, since_time) == 1) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

} // namespace detail

// ---- the public wrappers ----------------------------------------------------------------------------

uint32_t handle_invasion(uint32_t planet, double since_time) {
    sim_state st = state();
    return (uint32_t)detail::handle_invasion(st.read, st.own, live_invasion_calls(),
                                             (int32_t)planet, since_time);
}

int32_t invasion_chance_roll(int32_t building_completed) {
    sim_state st = state();
    return detail::invasion_chance_roll(st.read, st.own, live_invasion_calls(), building_completed);
}


} // namespace mh::sim
