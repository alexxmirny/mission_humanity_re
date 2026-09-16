//
// sim/resid/sim_advisor_tick.cpp -- see sim_advisor_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_advisor_tick_0049b5b0.asm), the exported `.c` being a draft.
//
#include "sim/resid/sim_advisor_tick.h"

#include "addr/mh_calls.gen.h" // typed callables for the effectful/helper originals we still call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const advisor_tick_calls &live_advisor_tick_calls() {
    static const advisor_tick_calls c = {
        MH_CRT(utils_w_str_copy),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
    };
    return c;
}

namespace {

// llm_strat_bldg_state members this function reads (0x0049b72e/0x0049b73a/0x0049b749/0x0049b757 for
// the construction-group gate, 0x0049b767 for the upgrade-vs-direct builder_count split) -- same
// values as sim_bldg_add_workers.cpp's own BLDG_STATE_* block; re-declared here (anonymous-namespace,
// different TU) per that file's own convention comment.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;

// built_flags == 3 (connected AND staffed) -- the ubiquitous "fully operational" test.
inline constexpr uint8_t BUILT_FLAGS_OPERATIONAL = 3;

// G_TEXT_PTRS indices the two floating messages use (0x0058509c / 0x00585090 in the raw assembly).
inline constexpr int32_t TEXT_ID_STORAGE_OVERFLOW = 0x324;
inline constexpr int32_t TEXT_ID_NEEDS_WORKERS    = 0x321;

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), 2 occurrences
// in this function (0x0049b84a-0x0049b85d for COL, 0x0049b865-0x0049b878 for ROW). Value-for-value
// C's truncating `/ 32` -- see sim_order_enqueue.cpp's fine_to_tile() for the verification;
// re-derived locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

} // namespace

namespace detail {

// ---- llm_strat_advisor_tick @0x0049b5b0 ------------------------------------------------------------
void advisor_tick(const sim_view &v, sim_store &own, const advisor_tick_calls &c, double now) {
    // 0x0049b5cf-0x0049b5e1: due-time gate. Skip the whole body (matching the JNC straight to the
    // epilogue) unless NEXT_TIME + DUE_DELAY < now.
    if (!(own.advisor_next_time() + *v.advisor_due_delay < now)) {
        return;
    }

    // 0x0049b5e7-0x0049b5ed: re-latch CUR_PLAYER from PlayerSide for this whole tick (a 16-bit copy).
    own.set_cur_player(static_cast<uint16_t>(*v.player_side));
    const uint16_t player = *v.cur_player;

    if (own.advisor_phase() == 0) {
        // ==== 0x0049b615-0x0049b693: PHASE 0 -- storage-overflow scan =============================
        bool overflow = false;
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            if (v.storage_stats[player].cap_prev[i] < player_resource_of(v, player, i)) {
                overflow = true;
                break;
            }
        }
        if (overflow) {
            c.w_str_copy(const_cast<wchar_t *>(v.text_ptrs[TEXT_ID_STORAGE_OVERFLOW]), own.text_scratch());
            own.floating_msg_queue_active() = 1;
            c.ui_print_text_message(own.text_scratch());
        }
    } else if (own.advisor_phase() == 1) {
        // ==== 0x0049b698-0x0049b8b6: PHASE 1 -- understaffed-building scan =========================
        // 0x0049b698-0x0049b6ba: unconditional initial store, even if the scan below never runs a
        // single iteration (buildings[player][0].index == 0).
        own.set_cur_index(1);
        own.set_cur_building_ptr(&own.building_at(static_cast<uint32_t>(player), 1));

        // 0x0049b6bf-0x0049b6d3: remaining = buildings[player][0].index, a per-player building COUNT
        // cached in slot 0's `index` field -- NOT an index into anything.
        uint32_t remaining =
            static_cast<uint16_t>(building_of(v, player, 0).index);

        bool    understaffed = false;
        int32_t idx          = 1;
        // The original's index/pointer advance (LAB_0049b6e1: `INC word ptr [CUR_INDEX]` /
        // `ADD dword ptr [CUR_BUILDING],0x111`) sits BETWEEN the body and the `remaining == 0` test
        // at LAB_0049b6d6, and EVERY body exit -- the empty-slot skip (0x0049b6fe), the
        // energy/built_flags skip (0x0049b724), builder_count == 0 (0x0049b7ca), uses_workers == 0
        // (0x0049b813), at-or-above threshold (0x0049b824) and the understaffed hit itself (which
        // zeroes `remaining` at 0x0049b880 and falls through) -- funnels into it. So the scan always
        // leaves these two AMBIENT globals pointing ONE SLOT PAST the last slot it examined, and
        // they are cross-function state that later readers consume. Setting them at the top of each
        // executed iteration reproduces every in-loop value but leaves them one behind on exit, so
        // the advance is restated after the loop below.
        for (; remaining != 0; ++idx) {
            own.set_cur_index(static_cast<uint16_t>(idx));
            own.set_cur_building_ptr(&own.building_at(static_cast<uint32_t>(player), idx));
            const building &b = building_of(v, player, idx);

            if (b.building_id == 0) {
                continue; // 0x0049b6fe: empty slot, skip straight to the next index.
            }

            // 0x0049b707: the count is drained the moment a real (nonzero-id) slot is seen, BEFORE
            // the energy/built_flags gate below -- an occupied-but-unbuilt/dead slot still counts.
            --remaining;

            if (!(b.energy > 0.0) || b.built_flags != BUILT_FLAGS_OPERATIONAL) {
                continue; // 0x0049b717/0x0049b722: not (damaged-or-charging AND fully operational).
            }

            float ratio;
            bool  have_ratio = false;

            if (b.state == BLDG_STATE_CONSTRUCTION || b.state == BLDG_STATE_UPGRADING ||
                b.state == BLDG_STATE_DISMANTLING || b.state == BLDG_STATE_CHARGE_STEP) {
                // 0x0049b762-0x0049b7ab: construction-group ratio. UPGRADING reads builder_count
                // through the UPGRADE TARGET's own cfg record -- one extra indirection vs the others.
                const int32_t builder_count =
                    (b.state == BLDG_STATE_UPGRADING)
                        ? v.cfg_buildings[v.cfg_buildings[b.building_id].upgrade_index].builder_count
                        : v.cfg_buildings[b.building_id].builder_count;
                if (builder_count != 0) {
                    // 0x0049b7bd-0x0049b7c5: current_workers / builder_count.
                    ratio      = static_cast<float>(b.current_workers) / static_cast<float>(builder_count);
                    have_ratio = true;
                }
                // 0x0049b7af/0x0049b7ca: builder_count == 0 -> no ratio, this slot is simply skipped.
            } else if (c.bldg_uses_workers(player, idx) != 0) {
                // 0x0049b7d1-0x0049b80e: non-construction-group ratio, gated by uses_workers().
                ratio = static_cast<float>(b.current_workers) /
                        static_cast<float>(v.cfg_buildings[b.building_id].worker_count);
                have_ratio = true;
            }

            // 0x0049b818-0x0049b824: the original FLDs the float `ratio` (exact, x87 extends it) and
            // FCOMPs it against the double threshold operand directly -- promote `ratio` to double
            // rather than narrowing the threshold to float, so no precision is discarded that the
            // original's comparison never lost.
            if (!have_ratio || !(static_cast<double>(ratio) < *v.advisor_staff_threshold)) {
                continue; // at/above threshold, or no ratio computed at all.
            }

            // 0x0049b826-0x0049b880: understaffed hit -- pan the camera to this building's tile and
            // force the scan to stop (remaining = 0) after this slot.
            understaffed = true;
            c.bldg_get_coords(player, idx, &own.cam_pan_target_col(), &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());
            remaining                = 0;
        }

        // 0x0049b6e1-0x0049b6f2: the loop-exit advance. `idx` has already been stepped past the last
        // examined slot by the `++idx` above, so this is the original's post-increment value.
        own.set_cur_index(static_cast<uint16_t>(idx));
        own.set_cur_building_ptr(&own.building_at(static_cast<uint32_t>(player), idx));

        if (understaffed) {
            c.w_str_copy(const_cast<wchar_t *>(v.text_ptrs[TEXT_ID_NEEDS_WORKERS]), own.text_scratch());
            own.floating_msg_queue_active() = 1;
            c.ui_print_text_message(own.text_scratch());
        }
    }
    // 0x0049b60b: any other ADVISOR_PHASE value runs neither advisory (unreachable in practice given
    // the mod-2 advance below, but a real branch in the assembly) -- falls straight through.

    // 0x0049b8b6-0x0049b8e3: unconditional housekeeping -- advance the phase (mod 2) and the timer.
    own.advisor_phase()     = (own.advisor_phase() + 1) % 2;
    own.advisor_next_time() = own.advisor_next_time() + *v.advisor_interval;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void advisor_tick(double now) {
    sim_state st = state();
    detail::advisor_tick(st.read, st.own, live_advisor_tick_calls(), now);
}

} // namespace mh::sim
