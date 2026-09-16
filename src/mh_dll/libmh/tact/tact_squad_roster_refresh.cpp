//
// tact/tact_squad_roster_refresh.cpp -- see tact_squad_roster_refresh.h. Translated from the
// DISASSEMBLY, not from Ghidra's C draft.
//
#include "tact/tact_squad_roster_refresh.h"

#include <cstring> // std::memcpy -- same idiom tact_sidebar_dispatch.cpp uses for a raw byte base

#include "addr/mh_calls.gen.h" // frontier callee (Law 4): utils_fill_data
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const squad_roster_refresh_calls &live_squad_roster_refresh_calls() {
    static const squad_roster_refresh_calls c = {
        MH_CRT(utils_fill_data),
    };
    return c;
}

namespace detail {

void squad_roster_refresh(const tact_view &v, tact_store &own, const squad_roster_refresh_calls &c) {
    // @0x004356d5-0x004356f7: bulk zero-fill both roster structures via the ORIGINAL callee, then
    // reset the active-unit count. `own.unassigned_unit_roster()` / `own.group_unit_roster()` are
    // DECLARED NEEDS N4/N5 -- mutable bases the store does not yet expose.
    c.fill_data(own.unassigned_unit_roster(), UNASSIGNED_ROSTER_BYTES, 0);
    c.fill_data(own.group_unit_roster(), GROUP_ROSTER_BYTES, 0);
    own.active_unit_count() = 0;

    // LOOP (@0x00435701-0x004357bd): i = TACT_UNIT_FIRST_SLOT..TACT_UNIT_LAST_SLOT inclusive.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        const tact_unit &u = unit_of(v, i);
        // @0x0043571e-0x0043574e: owner==0 && type!=0 && type<0x80, all three required (unsigned
        // compares throughout -- `type` is uint8_t).
        if (u.owner != 0) continue;
        if (u.type == 0) continue;
        if (u.type >= 0x80) continue;

        // @0x00435752: unconditional once the gate above passes.
        own.active_unit_count() += 1;

        // @0x0043575f-0x00435766: squad_group_id, 0xff = unassigned, 0..7 = squad/control-group id
        // (mh_tact_unit_record::squad_group_id, offset 0x5f0). UNCHECKED past the 0xff sentinel test
        // -- preserved literally (see header HAZARD note).
        const uint32_t squad = u.squad_group_id;

        if (squad == 0xff) {
            // @0x00435772-0x00435789: increment count FIRST (in place), re-read the NEW count, then
            // write `i` at base + new_count*4 -- lands on ids[new_count-1] == ids[old_count].
            uint8_t *const base = own.unassigned_unit_roster();
            int32_t        count;
            std::memcpy(&count, base, sizeof(count));
            ++count;
            std::memcpy(base, &count, sizeof(count));
            std::memcpy(base + count * 4, &i, sizeof(i));
        } else {
            // @0x0043578c-0x004357b7: same increment-then-write shape, on
            // group_unit_roster[squad] (slot base = roster base + squad * ROSTER_SLOT_BYTES).
            uint8_t *const slot = own.group_unit_roster() + squad * ROSTER_SLOT_BYTES;
            int32_t        count;
            std::memcpy(&count, slot, sizeof(count));
            ++count;
            std::memcpy(slot, &count, sizeof(count));
            std::memcpy(slot + count * 4, &i, sizeof(i));
        }
    }

    // @0x004357bd-0x004357e9: reclamp the unassigned-roster scroll row against the just-rebuilt
    // count. `own.sidebar_unassigned_scroll_row()` is DECLARED NEED N2;
    // `v.sidebar_multi_panel_visible_rows` is DECLARED NEED N1.
    {
        int32_t unassigned_count;
        std::memcpy(&unassigned_count, own.unassigned_unit_roster(), sizeof(unassigned_count));
        if (unassigned_count < own.sidebar_unassigned_scroll_row() + *v.sidebar_multi_panel_visible_rows) {
            own.sidebar_unassigned_scroll_row() = unassigned_count - *v.sidebar_multi_panel_visible_rows;
            if (own.sidebar_unassigned_scroll_row() < 0) own.sidebar_unassigned_scroll_row() = 0;
        }
    }

    // @0x004357f3-0x0043580a: "scroll up" flag pair, unassigned list.
    if (own.sidebar_unassigned_scroll_row() < 1) {
        own.sidebar_scrollbtn_state_at(0) &= 1;
        own.sidebar_scrollbtn_state_at(1) &= 1;
    } else {
        own.sidebar_scrollbtn_state_at(0) |= 2;
        own.sidebar_scrollbtn_state_at(1) |= 2;
    }

    // @0x0043581a-0x0043583f: "scroll down" flag pair, unassigned list -- tested against
    // unassigned.ids[MULTI_PANEL_VISIBLE_ROWS + sidebar_unassigned_scroll_row].
    {
        const int32_t idx = *v.sidebar_multi_panel_visible_rows + own.sidebar_unassigned_scroll_row();
        int32_t       next_id;
        std::memcpy(&next_id, own.unassigned_unit_roster() + ROSTER_IDS_BYTE_OFFSET + idx * 4,
                    sizeof(next_id));
        if (next_id < 1) {
            own.sidebar_scrollbtn_state_at(2) &= 1;
            own.sidebar_scrollbtn_state_at(3) &= 1;
        } else {
            own.sidebar_scrollbtn_state_at(2) |= 2;
            own.sidebar_scrollbtn_state_at(3) |= 2;
        }
    }

    // @0x0043584f-0x0043588d: same clamp shape for the ACTIVE group's roster.
    // `own.sidebar_group_scroll_row()` is DECLARED NEED N3.
    {
        const int32_t  active_group = own.sidebar_active_group_id();
        uint8_t *const slot         = own.group_unit_roster() + active_group * ROSTER_SLOT_BYTES;
        int32_t        group_count;
        std::memcpy(&group_count, slot, sizeof(group_count));
        if (group_count < own.sidebar_group_scroll_row() + *v.sidebar_multi_panel_visible_rows) {
            own.sidebar_group_scroll_row() = group_count - *v.sidebar_multi_panel_visible_rows;
            if (own.sidebar_group_scroll_row() < 0) own.sidebar_group_scroll_row() = 0;
        }
    }

    // @0x00435897-0x004358ae: "scroll up" flag pair, active group's roster.
    if (own.sidebar_group_scroll_row() < 1) {
        own.sidebar_scrollbtn_state_at(4) &= 1;
        own.sidebar_scrollbtn_state_at(5) &= 1;
    } else {
        own.sidebar_scrollbtn_state_at(4) |= 2;
        own.sidebar_scrollbtn_state_at(5) |= 2;
    }

    // @0x004358be-0x004358ef: "scroll down" flag pair, active group's roster -- tested against
    // group_unit_roster[active_group].ids[MULTI_PANEL_VISIBLE_ROWS + sidebar_group_scroll_row].
    // `sidebar_active_group_id` is re-read here exactly as the original re-reads it, rather than
    // reusing the value captured above -- matches the disassembly's own second load @0x004358ce.
    {
        const int32_t  active_group = own.sidebar_active_group_id();
        uint8_t *const slot         = own.group_unit_roster() + active_group * ROSTER_SLOT_BYTES;
        const int32_t  idx          = *v.sidebar_multi_panel_visible_rows + own.sidebar_group_scroll_row();
        int32_t        next_id;
        std::memcpy(&next_id, slot + ROSTER_IDS_BYTE_OFFSET + idx * 4, sizeof(next_id));
        if (next_id < 1) {
            own.sidebar_scrollbtn_state_at(6) &= 1;
            own.sidebar_scrollbtn_state_at(7) &= 1;
        } else {
            own.sidebar_scrollbtn_state_at(6) |= 2;
            own.sidebar_scrollbtn_state_at(7) |= 2;
        }
    }
}

} // namespace detail

void squad_roster_refresh() {
    tact_state st = state();
    detail::squad_roster_refresh(st.read, st.own, live_squad_roster_refresh_calls());
}


} // namespace mh::tact
