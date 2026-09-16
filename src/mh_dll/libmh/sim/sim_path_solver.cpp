//
// sim/sim_path_solver.cpp -- see sim_path_solver.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_path_write_from_solver_0049581d.asm,
// tmp/decomp_sim/llm_strat_path_find_free_slot_00495f1b.asm), not from the exported Ghidra `.c`
// drafts -- see the header banner for the full derivation, the one-shot-vs-walk dispatch
// re-derivation, and the unbacked-order-literal note.
//
#include "sim/sim_path_solver.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder (SIM1-P clause 2)
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_PLANE, UNIT_TYPE_H_PLANE (shared there; real enum)
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace detail {

// ---- llm_strat_unit_state (order) literals -- NOT backed by a real Ghidra enum (see the header
// banner). Own local constants (PATH_SOLVER_ prefix), matching every sibling TU's identical
// convention for this field (sim_unit_state_move_path.cpp's MOVE_PATH_ prefix, etc.).
inline constexpr uint16_t PATH_SOLVER_ORDER_DEPLOY_APPROACH = 0x18; // 0x004958f7 CMP
inline constexpr uint16_t PATH_SOLVER_ORDER_LANDING_REQUEST = 0x29; // 0x00495916 CMP

// llm_strat_path_write_from_solver @0x0049581d.
void path_write_from_solver(const sim_view &v, sim_store &own, uint32_t param_1, int32_t unit_index,
                            uint32_t param_3, uint32_t param_4, int32_t path_slot_id) {
    // param_3 (EBX) / param_4 (ECX): stored to their own stack slots at function entry and never read
    // again anywhere in the 0x283-byte body -- dead in this build. See header banner.
    (void)param_3;
    (void)param_4;

    // 0x0049583e: every use of param_1 in this function re-reads it as a 16-bit word, i.e. every use
    // is `param_1 & 0xffff` -- masked once here rather than at each call site.
    const uint32_t player = param_1 & 0xffffu;

    // 0x0049583e-0x00495858: prev_dir seeds from the unit's CURRENT move_heading, read once before the
    // solver-dir walk begins -- the FIRST iteration's remap-match check is against THIS heading.
    int32_t prev_dir = unit_of(v, player, unit_index).move_heading;

    int32_t idx = 0; // 0x0049585b: the path-buffer write cursor.

    // 0x00495862-0x00495867: the FRONTIER buffer -- llm_strat_pathtrace_dirs_get()'s own return
    // value, walked as a raw byte* exactly like the decompile's local_24 (NOT sim_view::pathtrace_dirs
    // /pathtrace_dir_at(), a DIFFERENT global -- see the header banner / _CONTEXT.md).
    const uint8_t *dirs = static_cast<const uint8_t *>(MH_LIBMH_BIND(llm_strat_pathtrace_dirs_get)());
    // 0x0049586d: incremented ONCE before its first dereference, so element [0] (a count/header byte
    // the frontier owns) is skipped and the walk starts at element [1].
    ++dirs;

    // 0x00495873-0x0049596c: the one-shot special case. Only attempted when the FIRST candidate is
    // already the frontier's own "blocked/no path" sentinel (>0x17); its own inner guard (not a
    // plane, and not mid-deploy/landing) decides whether it actually fires. See the header banner for
    // the wrote_special-flag-vs-JMP-chain re-derivation note.
    bool wrote_special = false;
    if (dirs[0] > 0x17) {
        // 0x00495878-0x004958c8: Unit[unit_proto_id].type == A_PLANE || == H_PLANE -- SAME
        // v.cfg_units/UNIT_TYPE_A_PLANE/UNIT_TYPE_H_PLANE pattern as
        // sim_bldg_side_has_aircraft_producer.cpp (reused per house rule, not reinvented).
        const cfg_unit &proto    = v.cfg_units[unit_of(v, player, unit_index).unit_proto_id];
        const bool      is_plane = proto.type == UNIT_TYPE_A_PLANE || proto.type == UNIT_TYPE_H_PLANE;

        // 0x004958e4-0x00495922: order != DEPLOY_APPROACH (0x18) && order != LANDING_REQUEST (0x29).
        const uint16_t order = unit_of(v, player, unit_index).order;

        if (!is_plane && order != PATH_SOLVER_ORDER_DEPLOY_APPROACH &&
            order != PATH_SOLVER_ORDER_LANDING_REQUEST) {
            // 0x00495922-0x0049596c: one waypoint -- the unit's CURRENT move_heading's primary remap
            // step, truncated to a byte (the asm's `MOV AL, byte ptr [...]` is a genuine byte-width
            // read of the int32 `.step_primary` field, not a translation error) -- then jump straight
            // to the shared final-terminator tail, skipping the walk entirely.
            const int32_t heading = unit_of(v, player, unit_index).move_heading;
            own.path_buffer_at(player, path_slot_id, idx).heading =
                static_cast<uint8_t>(v.dir_remap_table[heading].step_primary);
            idx           = 1;
            wrote_special = true;
        }
    }

    if (!wrote_special) {
        // 0x00495971-0x00495a47: the general greedy walk -- copy each frontier dir into the path
        // buffer, and stop the first time a dir does not continue the previous heading's run (none of
        // dir_remap_table[prev_dir]'s 4 step candidates match), overwriting the entry just written
        // with a 0xff terminator and forcing the loop to end (idx=0x12a fails the idx<299 continuation
        // test below on the next increment). Runs even when the FIRST dirs[0] is itself already
        // invalid (>0x17) and the one-shot special case above declined to fire -- the asm's JMP chain
        // reaches this loop either way; see header banner.
        do {
            own.path_buffer_at(player, path_slot_id, idx).heading = dirs[0];
            const int32_t cur_dir                                 = dirs[0];

            // 0x004959a0-0x004959f6: the 4-way remap match chain (step_primary/alt1/alt2/alt3), only
            // attempted when both cur_dir and prev_dir are valid (<0x18) -- out-of-range on either
            // side is treated as "no mismatch" (asm: falls straight to the loop-continue section
            // without touching the buffer a second time).
            if (cur_dir < 0x18 && prev_dir < 0x18 &&
                cur_dir != v.dir_remap_table[prev_dir].step_primary &&
                cur_dir != v.dir_remap_table[prev_dir].step_alt1 &&
                cur_dir != v.dir_remap_table[prev_dir].step_alt2 &&
                cur_dir != v.dir_remap_table[prev_dir].step_alt3) {
                own.path_buffer_at(player, path_slot_id, idx).heading = 0xff;
                idx                                                   = 0x12a; // forces exit next test
            }

            // 0x00495a23-0x00495a26: UNCONDITIONAL every iteration in the asm (the Ghidra draft folds
            // this into the while-condition's comma operator instead; behaviorally identical since the
            // value is dead once the loop exits either way -- transcribed as the asm's unconditional
            // form).
            prev_dir = cur_dir;
            ++idx;
            ++dirs;
        } while (dirs[0] < 0x18 && idx < 299); // 0x00495a38-0x00495a43
    }

    // 0x00495a49-0x00495a64: the final terminator write, ALWAYS performed regardless of which path
    // got here (the one-shot special case or the walk's natural/forced exit).
    own.path_buffer_at(player, path_slot_id, idx).heading = 0xff;

    // 0x00495a6b-0x00495a75: llm_strat_path_attach_slot(player, unit_index, path_slot_id) -- a
    // FRONTIER call; EAX/EDX/EBX register order matches its committed mh_calls.gen.h signature
    // exactly.
    MH_LIBMH_BIND(llm_strat_path_attach_slot)(static_cast<int32_t>(player), unit_index, path_slot_id);

    // 0x00495a8d: units[player][unit_index].path_cursor = 0 -- refetched AFTER the call above (W2:
    // no record reference is held across a call into original code) rather than reusing an earlier
    // reference.
    own.unit_at(player, unit_index).path_cursor = 0;
}

// llm_strat_path_find_free_slot @0x00495f1b. A pure query: no write anywhere in this function, so it
// reads THROUGH sim_store's existing mutable `path_slot_flag_at()` without ever writing through it
// (see the header banner).
int32_t path_find_free_slot(sim_store &own, int32_t player) {
    for (int32_t slot = 0; slot < PATH_SLOTS_PER_PLAYER; ++slot) { // 0x00495f44-0x00495f53
        if (own.path_slot_flag_at(player, slot) == 0) return slot; // 0x00495f5f-0x00495f6f
    }
    return -1; // 0x00495f73: fell out of the loop, no free slot found.
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void path_write_from_solver(uint32_t param_1, int32_t unit_index, uint32_t param_3, uint32_t param_4,
                            int32_t path_slot_id) {
    sim_state st = state();
    detail::path_write_from_solver(st.read, st.own, param_1, unit_index, param_3, param_4, path_slot_id);
}

int32_t path_find_free_slot(int32_t player) {
    sim_state st = state();
    return detail::path_find_free_slot(st.own, player);
}


} // namespace mh::sim
