#include "tact/tact_squad_roster_refresh.h"
#include "tact_test_support.h"

#include <cstring>

namespace mh::tact::test {

namespace {

int g_fill_data_calls = 0;
struct fill_call {
    void    *ptr;
    uint32_t size;
    uint8_t  value;
};
fill_call g_fill_calls[4];

void *mock_fill_data(void *ptr, uint32_t size, uint8_t value) {
    if (g_fill_data_calls < 4) g_fill_calls[g_fill_data_calls] = {ptr, size, value};
    ++g_fill_data_calls;
    std::memset(ptr, value, size); // REAL semantics -- the body reads this memory right after.
    return ptr;
}

squad_roster_refresh_calls mock_calls() { return {mock_fill_data}; }

int32_t roster_count(uint8_t *base) {
    int32_t c;
    std::memcpy(&c, base, sizeof(c));
    return c;
}
int32_t roster_id_at(uint8_t *base, int32_t idx) {
    int32_t v;
    std::memcpy(&v, base + ROSTER_IDS_BYTE_OFFSET + idx * 4, sizeof(v));
    return v;
}

} // namespace

void run_squad_roster_refresh_tests() {
    // T1: population -- fill_data zeroes both rosters, then the loop assigns each eligible unit to
    // the unassigned roster (squad==0xff) or its group slot, in visitation (ascending slot) order,
    // and the gate excludes owner!=0 / type==0 / type>=0x80. 0x004356d5-0x004357bd.
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1;
            fx.units[i].type  = 0;
        }
        fx.units[5]                = {};
        fx.units[5].owner          = 0;
        fx.units[5].type           = 3;
        fx.units[5].squad_group_id = 0xff; // unassigned
        fx.units[9]                = {};
        fx.units[9].owner          = 0;
        fx.units[9].type           = 3;
        fx.units[9].squad_group_id = 0xff; // unassigned, second one -> tests append order
        fx.units[7]                = {};
        fx.units[7].owner          = 0;
        fx.units[7].type           = 3;
        fx.units[7].squad_group_id = 2; // group 2
        fx.units[3]                = {};
        fx.units[3].owner          = 1; // excluded: owner != 0
        fx.units[3].type           = 3;
        fx.units[3].squad_group_id = 0;
        fx.units[4]                = {};
        fx.units[4].owner          = 0;
        fx.units[4].type           = 0; // excluded: type == 0
        fx.units[4].squad_group_id = 0;
        fx.units[6]                = {};
        fx.units[6].owner          = 0;
        fx.units[6].type           = 0x80; // excluded: type >= 0x80
        fx.units[6].squad_group_id = 0;
        g_fill_data_calls          = 0;

        tact_store own = fx.store();
        detail::squad_roster_refresh(fx.view(), own, mock_calls());

        ck_eq((uint32_t)g_fill_data_calls, 2u, "T1: fill_data called exactly twice");
        ck_eq((uint32_t)g_fill_calls[0].size, (uint32_t)UNASSIGNED_ROSTER_BYTES,
              "T1: first fill_data zeroes the UNASSIGNED roster (256 B), 0x004356d5");
        ck_eq((uint32_t)g_fill_calls[1].size, (uint32_t)GROUP_ROSTER_BYTES,
              "T1: second fill_data zeroes the GROUP roster (2048 B), 0x004356ea");
        ck_eq((uint32_t)own.active_unit_count(), 3u,
              "T1: active count = slots 5,7,9 (owner==0 && type!=0 && type<0x80)");

        uint8_t *unassigned = own.unassigned_unit_roster();
        ck_eq((uint32_t)roster_count(unassigned), 2u, "T1: unassigned roster count == 2");
        ck_eq((uint32_t)roster_id_at(unassigned, 0), 5u,
              "T1: unassigned.ids[0] == slot 5 (visited first, increment-then-write lands on ids[0])");
        ck_eq((uint32_t)roster_id_at(unassigned, 1), 9u,
              "T1: unassigned.ids[1] == slot 9 (visited second)");

        uint8_t *group2 = own.group_unit_roster() + 2 * ROSTER_SLOT_BYTES;
        ck_eq((uint32_t)roster_count(group2), 1u, "T1: group[2] roster count == 1");
        ck_eq((uint32_t)roster_id_at(group2, 0), 7u, "T1: group[2].ids[0] == slot 7");
        uint8_t *group0 = own.group_unit_roster() + 0 * ROSTER_SLOT_BYTES;
        ck_eq((uint32_t)roster_count(group0), 0u,
              "T1: group[0] roster untouched -- slot 3's squad==0 unit was excluded by owner!=0");
    }

    // T2: unassigned scroll-row CLAMP -- a stale scroll_row that no longer fits the just-rebuilt
    // (smaller) unassigned count is reclamped to `count - visible_rows`, floored at 0.
    // 0x004357bd-0x004357e9.
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1;
            fx.units[i].type  = 0;
        }
        fx.units[1]                         = {};
        fx.units[1].owner                   = 0;
        fx.units[1].type                    = 3;
        fx.units[1].squad_group_id          = 0xff; // exactly 1 unassigned unit this refresh
        fx.sidebar_multi_panel_visible_rows = 10;
        fx.sidebar_unassigned_scroll_row    = 50; // stale, far too large for count=1
        g_fill_data_calls                   = 0;

        tact_store own = fx.store();
        detail::squad_roster_refresh(fx.view(), own, mock_calls());

        // count(1) < scroll_row(50) + visible_rows(10) -> scroll_row = 1 - 10 = -9 -> floored to 0.
        ck_eq((uint32_t)own.sidebar_unassigned_scroll_row(), 0u,
              "T2: scroll_row reclamped to 0 (1 - 10 = -9, floored), 0x004357cf-0x004357e9");
        // "scroll up" flag pair -- scroll_row(0) < 1 -> AND-clear (&=1) on both cells.
        ck_eq((uint32_t)(own.sidebar_scrollbtn_state_at(0) & 2), 0u,
              "T2: unassigned scroll-up flag cleared (scroll_row<1), 0x004357f3");
        ck_eq((uint32_t)(own.sidebar_scrollbtn_state_at(1) & 2), 0u,
              "T2: unassigned scroll-up flag[1] cleared identically");
    }

    // T3: unassigned "scroll up" ENABLED and "scroll down" flag pairs, both directions -- OR-set
    // (|=2) when scroll_row>=1, and the down-flag keyed off the row AFTER the visible window.
    // 0x0043581a-0x0043583f.
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1;
            fx.units[i].type  = 0;
        }
        // 3 unassigned units so the roster survives a scroll_row of 1 without reclamping (count=3,
        // visible_rows=1: 3 < 1+1=2 is false, so scroll_row(1) is NOT reclamped).
        for (int32_t slot : {1, 2, 3}) {
            fx.units[slot]                = {};
            fx.units[slot].owner          = 0;
            fx.units[slot].type           = 3;
            fx.units[slot].squad_group_id = 0xff;
        }
        fx.sidebar_multi_panel_visible_rows = 1;
        fx.sidebar_unassigned_scroll_row    = 1;
        g_fill_data_calls                   = 0;

        tact_store own = fx.store();
        detail::squad_roster_refresh(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_unassigned_scroll_row(), 1u,
              "T3: scroll_row NOT reclamped -- 3 unassigned units cover it");
        ck((own.sidebar_scrollbtn_state_at(0) & 2) != 0,
           "T3: scroll-up flag SET (scroll_row(1) >= 1), 0x004357f3-0x0043580a");
        // down-flag tested against ids[visible_rows(1) + scroll_row(1)] = ids[2] = slot 3 (>=1) -> SET.
        ck((own.sidebar_scrollbtn_state_at(2) & 2) != 0,
           "T3: scroll-down flag SET -- ids[2] holds a real id (slot 3), 0x0043581a-0x0043583f");
    }

    // T4: the ACTIVE-GROUP roster's own clamp + flag pair, mirroring T2/T3 for group_unit_roster
    // (a SEPARATE code path, 0x0043584f-0x004358ef, not a copy-paste of the unassigned one).
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1;
            fx.units[i].type  = 0;
        }
        fx.units[1]                         = {};
        fx.units[1].owner                   = 0;
        fx.units[1].type                    = 3;
        fx.units[1].squad_group_id          = 4; // group 4 has exactly 1 member
        fx.sidebar_active_group_id          = 4;
        fx.sidebar_multi_panel_visible_rows = 10;
        fx.sidebar_group_scroll_row         = 50; // stale, reclamps to 0
        g_fill_data_calls                   = 0;

        tact_store own = fx.store();
        detail::squad_roster_refresh(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_group_scroll_row(), 0u,
              "T4: group scroll_row reclamped to 0 (1 - 10 = -9, floored), 0x0043584f-0x0043588d");
        ck_eq((uint32_t)(own.sidebar_scrollbtn_state_at(4) & 2), 0u,
              "T4: group scroll-up flag cleared (scroll_row<1), 0x00435897");
        // down-flag: ids[visible_rows(10) + scroll_row(0)] = ids[10], out of the 1-member roster -> 0
        // (fill_data zeroed it) -> < 1 -> AND-clear.
        ck_eq((uint32_t)(own.sidebar_scrollbtn_state_at(6) & 2), 0u,
              "T4: group scroll-down flag cleared -- ids[10] is unpopulated (zero), 0x004358be-0x004358ef");
    }
}

} // namespace mh::tact::test
