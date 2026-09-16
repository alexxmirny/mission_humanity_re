//
// sim/resid/sim_unit_path_store.cpp -- see sim_unit_path_store.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_unit_assign_path_from_job_result_0049508a.asm,
//  tmp/decomp_sim_resid/llm_strat_unit_path_store_result_00495aa0.asm,
//  tmp/decomp_sim_resid/llm_strat_unit_assign_shared_path_00495f87.asm); the Ghidra .c beside each is
// a draft.
//
#include "sim/resid/sim_unit_path_store.h"

#include "addr/mh_calls.gen.h"  // typed callables for the frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_path_store_calls &live_unit_path_store_calls() {
    static const unit_path_store_calls c = {
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_path_attach_slot),
        MH_LIBMH_BIND(llm_strat_pathtrace_dirs_get),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_assign_path_from_job_result @0x0049508a ------------------------------------
int32_t assign_path_from_job_result(const sim_view &v, sim_store &own, const unit_path_store_calls &c,
                                    uint16_t player, int32_t unit_index, uint32_t job_result_idx) {
    (void)v;

    // 0x004950a9-0x004950e3: if the unit already carries a path, release its slot first. units is
    // only READ here (path_slot_id); the release write itself happens inside path_free_slot.
    if (own.unit_at(player, unit_index).path_slot_id != 0xffu) {
        c.path_free_slot(player, unit_index); // 0x004950e3
    }

    // 0x004950e8-0x00495202: scan the player's 100 path slots for the first free one.
    for (int32_t slot = 0; slot < 100; ++slot) {        // 0x004950ef/0x004950f3
        if (own.path_slot_flag_at(player, slot) == 1) { // 0x0049510c: literal CMP ...,1
            continue;                                   // 0x00495113: busy, next slot
        }

        // 0x004950b0-0x004950bd: the job-result row's packed step array (own.path_job_result_at is
        // sim_store's typed binding of _G_LLM_STRAT_PATH_JOB_RESULT_TABLE; only the LOW 16 bits of
        // job_result_idx are ever read by the original -- see the header banner).
        const uint8_t *steps = static_cast<const uint8_t *>(
            own.path_job_result_at(static_cast<int32_t>(job_result_idx & 0xffffu)).path_steps);

        // 0x00495119-0x004951bf: copy steps into this slot's waypoints, entry 0..298 (0x12b=299 is
        // the loop bound; entry never reaches the read at index 299, so the terminator write below is
        // always in-bounds). Stops early on the source's own 0xff terminator.
        int32_t entry = 0;
        for (; entry < 299; ++entry) { // 0x00495120/0x00495127 (0x12b)
            if (steps[entry * 3] == 0xff) {
                break; // 0x00495132/0x00495135
            }
            path_waypoint &wp = own.path_buffer_at(player, slot, entry);
            wp.heading        = static_cast<uint8_t>(steps[entry * 3] + 1); // 0x00495145-0x00495163
            wp.run_length     = steps[entry * 3 + 1];                       // 0x0049518c-0x0049518f
            // 0x00495195-0x004951b6 re-reads the just-written run_length into a local accumulator
            // that is never stored anywhere else in this function -- confirmed dead, not reproduced
            // (see the header banner and uncertainties[]).
        }

        // 0x004951c4-0x004951de: destination terminator is heading==0 here (see the header's
        // cross-function terminator-convention note).
        own.path_buffer_at(player, slot, entry).heading = 0;

        c.path_attach_slot(player, unit_index, slot); // 0x004951ef
        return 1;                                     // 0x004951f4
    }

    return 0; // 0x00495202
}

// ---- llm_strat_unit_path_store_result @0x00495aa0 -----------------------------------------------
void path_store_result(const sim_view &v, sim_store &own, const unit_path_store_calls &c,
                       uint32_t player, int32_t unit_idx, uint32_t unused1, uint32_t unused2,
                       int32_t path_slot) {
    (void)v;
    (void)unused1; // 0x00495abb: stored to a local, never read again in the original
    (void)unused2; // 0x00495abe: same

    // 0x00495ac8-0x00495ad3: fetch the live pathtrace dirs pointer and advance it ONE byte before the
    // copy loop starts (the dead MOV-then-INC pair) -- the first byte read below is dirs[1], not
    // dirs[0]. See the header banner / uncertainties[].
    const uint8_t *dirs = static_cast<const uint8_t *>(c.pathtrace_dirs_get()) + 1;

    // 0x00495ad6-0x00495b16: copy loop; writes at cursor 0,1,2,... and the terminator/bound checks run
    // AFTER both cursor and dirs have already been incremented for this entry (0x12c=300 is the loop
    // bound -- see the header banner). Only .heading is written (raw byte, no +1 shift -- unlike
    // assign_path_from_job_result's job-result convention).
    int32_t cursor = 0;
    for (;; ++cursor, ++dirs) {
        own.path_buffer_at(player, path_slot, cursor).heading = *dirs; // 0x00495af1-0x00495af6
        if (*(dirs + 1) == 0xff) {                                     // 0x00495b0b/0x00495b0e (peek)
            break;
        }
        if (cursor + 1 >= 300) { // 0x00495b10/0x00495b16 (0x12c)
            break;
        }
    }

    // 0x00495b18-0x00495b33: destination terminator is heading==0xff here (see the header's
    // cross-function terminator-convention note -- NOT the same value assign_path_from_job_result
    // uses). GENUINE OVERFLOW when the loop ran the full 300 entries: cursor+1==300 aliases into
    // path_slot+1's entry 0 (see the header banner) -- reproduced literally, not clamped.
    own.path_buffer_at(player, path_slot, cursor + 1).heading = 0xff;

    // 0x00495b3a-0x00495b4d: reset the unit's path cursor to the start of the new path.
    own.unit_at(player, unit_idx).path_cursor = 0;

    c.path_attach_slot(player, unit_idx, path_slot); // 0x00495b57-0x00495b61
}

// ---- llm_strat_unit_assign_shared_path @0x00495f87 -----------------------------------------------
int32_t assign_shared_path(const sim_view &v, sim_store &own, const unit_path_store_calls &c,
                           uint32_t player, int32_t unit_index, uint8_t start_col, uint8_t start_row) {
    // 0x00495fbe-0x004960fe: same free-slot scan as assign_path_from_job_result.
    for (int32_t slot = 0; slot < 100; ++slot) { // 0x00495fbe (0x64)
        if (own.path_slot_flag_at(player, slot) != 0) {
            continue; // 0x00495fe6: busy, next slot
        }

        // 0x00495fec-0x00495fef: same one-byte pre-skip as path_store_result, taken right after the
        // free slot is found (dead MOV-then-INC pair).
        const uint8_t *dirs = static_cast<const uint8_t *>(c.pathtrace_dirs_get()) + 1;

        int32_t cursor = 0;
        for (;; ++cursor, ++dirs) {
            // 0x00496013-0x00496015: raw byte, no +1 shift (same convention as path_store_result).
            own.path_buffer_at(player, slot, cursor).heading = *dirs;

            // 0x0049601e-0x00496036: dx/dy for this dir code out of the SAME table
            // sim_view::dir_step_offsets binds for the (unrelated) elevator-approach-tile use; only
            // the low byte of each 32-bit component is added.
            const dir_step_offset &step = v.dir_step_offsets[*dirs];
            start_col                   = static_cast<uint8_t>((start_col + static_cast<uint8_t>(step.dx)) &
                                                               static_cast<uint8_t>(map_width_mask(v))); // 0x0049603f-0x0049604d
            start_row                   = static_cast<uint8_t>((start_row + static_cast<uint8_t>(step.dy)) &
                                                               static_cast<uint8_t>(map_height_mask(v))); // 0x00496050-0x0049605e
            // start_col/start_row are never stored or returned anywhere else in this function --
            // confirmed dead, reproduced literally per Law 2. See the header banner / uncertainties[].

            if (*(dirs + 1) == 0xff) { // 0x00496070/0x00496073 (peek)
                break;
            }
            if (cursor + 1 >= 300) { // 0x00496075/0x0049607b (0x12c)
                break;
            }
        }

        // 0x00496081-0x0049609f: primary slot's terminator, heading==0xff.
        own.path_buffer_at(player, slot, cursor + 1).heading = 0xff;

        // 0x004960a6-0x004960bc: SECOND terminator, entry 0 of the NEXT slot's buffer -- the
        // "shared" companion buffer. See the header banner's uncertainties note on the ambiguous
        // source instruction at 0x004960ba; this reading includes the player stride. NOT
        // bounds-checked in the original (W2): if slot==99 (the last slot found free), this aliases
        // into player+1's slot 0 entry 0 -- reproduced literally, no guard added.
        own.path_buffer_at(player, slot + 1, 0).heading = 0xff;

        // 0x004960c3-0x004960d6: same unit.path_cursor reset as path_store_result.
        own.unit_at(player, unit_index).path_cursor = 0;

        c.path_attach_slot(player, unit_index, slot); // 0x004960e0-0x004960eb
        return 1;                                     // 0x004960f0
    }

    return 0; // 0x004960fe
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

int32_t unit_assign_path_from_job_result(uint16_t player, int32_t unit_index, uint32_t job_result_idx) {
    sim_state st = state();
    return detail::assign_path_from_job_result(st.read, st.own, live_unit_path_store_calls(), player,
                                               unit_index, job_result_idx);
}

void unit_path_store_result(uint32_t player, int32_t unit_idx, uint32_t unused1, uint32_t unused2,
                            int32_t path_slot) {
    sim_state st = state();
    detail::path_store_result(st.read, st.own, live_unit_path_store_calls(), player, unit_idx, unused1,
                              unused2, path_slot);
}

int32_t unit_assign_shared_path(uint32_t player, int32_t unit_index, uint8_t start_col,
                                uint8_t start_row) {
    sim_state st = state();
    return detail::assign_shared_path(st.read, st.own, live_unit_path_store_calls(), player, unit_index,
                                      start_col, start_row);
}

} // namespace mh::sim
