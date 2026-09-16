#include "ai/ai_build_sources.h"

namespace mh::ai {
namespace detail {

build_sources_report count_unit_build_sources(const ai_view &v, int32_t player,
                                              int32_t *out_counts) {
    build_sources_report rep{};

    const uint32_t total = v.cfg_unit_sec->total;

    // ---- pass 1: clear, from index 1, INCLUSIVE of `total` (0x004e2201-0x004e2211) ----
    for (uint32_t t = 1; t <= total; ++t) out_counts[t] = 0;

    // ---- pass 2: the count-driven roster walk (0x004e2213-0x004e22a5) ----
    // `remaining` is a WORD read of the header slot's `index` (MOVZX @0x004e2230), and the loop
    // condition is `TEST EBP,EBP / JA` -- an UNSIGNED test, which after the MOVZX is just != 0.
    uint32_t remaining =
        (uint32_t)(uint16_t)v.buildings[(size_t)player * v.caps.buildings].index;
    rep.roster_count = (int32_t)remaining;

    int32_t slot = 1; // the header occupies slot 0
    while (remaining != 0) {
        // NOT bounds-checked against BUILDINGS_PER_PLAYER -- see the header. The index arithmetic is
        // the original's, so an over-long header count walks into the next player's row exactly as
        // the original does.
        const building &b =
            v.buildings[(size_t)player * v.caps.buildings + (size_t)slot];
        const uint32_t bid = (uint32_t)b.building_id; // MOVZX word @0x004e226d

        if (bid != 0) {
            const cfg_building &cb = v.cfg_buildings[bid];
            for (uint32_t t = 1; t <= total; ++t) {
                // FLDZ / FCOMP / JNC @0x004e2283-0x004e228f: increment only when 0.0 < quant.
                if (0.0 < cb.unit_quant[t]) ++out_counts[t];
            }
            --remaining;
            ++rep.scanned;
        } else {
            ++rep.empty_slots;
        }
        ++slot;
        if (slot > rep.max_cursor) rep.max_cursor = slot;
    }

    // ---- pass 3: the housing veto (0x004e22b1-0x004e23a0) ----
    const housing_stats &h = v.unit_housing[player];

    for (uint32_t t = 1; t <= total; ++t) {
        const uint32_t role   = v.cfg_units[t].ai_unit;
        bool           vetoed = false;

        // Four SEQUENTIAL blocks in the original, not else-if. Each compare is SIGNED (JL), so the
        // veto fires on used >= cap.
        if (role == 2 || role == 4 || role == 5 || role == 3) {
            if (h.used_vehicles >= h.cap_prev_vehicles) {
                out_counts[t] = 0;
                vetoed        = true;
            }
        }
        if (role == 1) {
            if (h.used_soldiers >= h.cap_prev_soldiers) {
                out_counts[t] = 0;
                vetoed        = true;
            }
        }
        if (role == 6) {
            if (h.used_helis >= h.cap_prev_helis) {
                out_counts[t] = 0;
                vetoed        = true;
            }
        }
        if (role == 7 || role == 8) {
            if (h.used_planes >= h.cap_prev_planes) {
                out_counts[t] = 0;
                vetoed        = true;
            }
        }

        if (vetoed) ++rep.vetoed;
        if (out_counts[t] != 0) ++rep.types_hit;
    }
    return rep;
}

} // namespace detail

void count_unit_build_sources(int32_t player, int32_t *out_counts) {
    const ai_state st = state();
    (void)detail::count_unit_build_sources(st.read, player, out_counts);
}

} // namespace mh::ai
