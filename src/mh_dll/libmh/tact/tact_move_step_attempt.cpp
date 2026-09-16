//
// tact/tact_move_step_attempt.cpp -- see tact_move_step_attempt.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_move_step_attempt.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_move_path_build
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_move_path_build.h"

namespace mh::tact {
namespace detail {

namespace {
// The 8 dir24 notches this function ever writes (0x1,0x4,0x7,0xa,0xd,0x10,0x13,0x16 -- every 3rd of
// 24), named by the (delta_col,delta_row) they correspond to (0x00494c19-0x00494d04). No compass
// label is established elsewhere in this codebase for dir24's orientation.
inline constexpr uint8_t MOVE_DIR_C1_R1   = 0x16; // src_col<dst_col, src_row<dst_row
inline constexpr uint8_t MOVE_DIR_C1_R0   = 0x13; // src_col<dst_col, src_row==dst_row
inline constexpr uint8_t MOVE_DIR_C1_RN1  = 0x10; // src_col<dst_col, src_row>dst_row
inline constexpr uint8_t MOVE_DIR_CN1_RN1 = 0x0a; // src_col>dst_col, src_row>dst_row
inline constexpr uint8_t MOVE_DIR_CN1_R0  = 0x07; // src_col>dst_col, src_row==dst_row
inline constexpr uint8_t MOVE_DIR_CN1_R1  = 0x04; // src_col>dst_col, src_row<dst_row
inline constexpr uint8_t MOVE_DIR_C0_R1   = 0x01; // src_col==dst_col, src_row<dst_row
inline constexpr uint8_t MOVE_DIR_C0_RN1  = 0x0d; // src_col==dst_col, src_row>dst_row
} // namespace

int32_t move_step_attempt(tact_view &v, tact_store &own, int32_t src_col, int32_t src_row,
                          int32_t dst_col, int32_t dst_row) {
    // @0x00494b4a-0x00494b6d: already there -- no path slot used.
    if (src_col == dst_col && src_row == dst_row) {
        own.move_path_slot_id() = -1;
        return 1;
    }

    // @0x00494b72-0x00494b93: is the destination tile currently VISIBLE (tile_objects flags[1] bit
    // 0x80 -- the same bit move_path_preview_walk.h reads as the fog-of-war "currently visible" bit,
    // not independently re-verified here)?
    const bool dest_visible = (tile_at(v, dst_col, dst_row).flags[1] & 0x80) != 0;

    if (!dest_visible) {
        // @0x00494b99-0x00494bd2: scan _G_LLM_STRAT_PATH_SLOT_FLAGS[1..99] for the first free slot
        // (flag==0). No free slot -> fail immediately, no octant step attempted.
        own.move_path_slot_id() = -1;
        int32_t slot            = -1;
        for (int32_t i = 1; i < 0x64; ++i) {
            if (own.planes().path_slot_flag_at(0, i) == 0) {
                slot = i;
                break;
            }
        }
        if (slot == -1) return 0;
        own.move_path_slot_id() = slot;

        // @0x00494bf1: waypoint[0].run_length = 1 -- a single-step path, unconditionally, before the
        // direction is even picked.
        own.planes().path_waypoint_at(0, slot, 0).run_length = 1;

        // @0x00494bf8-0x00494d04: pick the octant from src toward dst and take exactly ONE step in
        // it, mirroring the branch structure of the disassembly rather than a unified formula.
        int32_t step_col = src_col;
        int32_t step_row = src_row;
        uint8_t heading;
        if (src_col < dst_col) {
            if (src_row < dst_row) {
                heading = MOVE_DIR_C1_R1;
                ++step_row;
            } else if (src_row == dst_row) {
                heading = MOVE_DIR_C1_R0;
            } else {
                heading = MOVE_DIR_C1_RN1;
                --step_row;
            }
            ++step_col;
        } else if (src_col > dst_col) {
            if (src_row < dst_row) {
                heading = MOVE_DIR_CN1_R1;
                ++step_row;
            } else if (src_row == dst_row) {
                heading = MOVE_DIR_CN1_R0;
            } else {
                heading = MOVE_DIR_CN1_RN1;
                --step_row;
            }
            --step_col;
        } else {
            // src_col == dst_col (0x00494cc4-0x00494d04). src_row == dst_row here would mean
            // src == dst, already excluded above -- LAB_0x00494d06's tail (heading=0/run_length=0,
            // FLOOD_RESULT=src, ret=1) is therefore UNREACHABLE from this call shape and is not
            // transcribed; every live path through this arm sets a heading and falls through below.
            if (src_row < dst_row) {
                heading = MOVE_DIR_C0_R1;
                ++step_row;
            } else {
                heading = MOVE_DIR_C0_RN1;
                --step_row;
            }
        }
        own.planes().path_waypoint_at(0, slot, 0).heading = heading;

        // @0x00494d44-0x00494d92: no corner-cutting. Three passability reads, EACH gated on the
        // previous one passing (matches the original's JZ/JNZ chain exactly, not merely equivalent
        // under short-circuit -- P2/P3 are never read once an earlier one fails):
        //   P1 = (step_col, step_row)        -- the tile stepped to
        //   P2 = (src_col,  step_row)         -- the flank using the ORIGINAL column
        //   P3 = (step_col, src_row)          -- the flank using the ORIGINAL row
        bool step_ok = own.planes().passable_at(step_col, step_row) != 0;
        if (step_ok) step_ok = own.planes().passable_at(src_col, step_row) != 0;
        if (step_ok) step_ok = own.planes().passable_at(step_col, src_row) != 0;
        if (!step_ok) {
            // @0x00494d7c: refused -- no path slot needed, report success anyway (matches the
            // original: MOVE_PATH_SLOT_ID=-1, ret=1).
            own.move_path_slot_id() = -1;
            return 1;
        }

        // @0x00494d92-0x00494e5e: waypoint[1] terminates the 1-step path; success only if the step
        // landed EXACTLY on the destination (src was already adjacent).
        own.move_flood_result_col()                          = static_cast<uint8_t>(step_col);
        own.move_flood_result_row()                          = static_cast<uint8_t>(step_row);
        own.planes().path_waypoint_at(0, slot, 1).heading    = 0;
        own.planes().path_waypoint_at(0, slot, 1).run_length = 0;
        return (step_col == dst_col && step_row == dst_row) ? 1 : 0;
    }

    // @0x00494df4-0x00494e69: the destination is visible -- run the real flood-fill pathfinder
    // (frontier) from dst BACK to src, and succeed only if the backtrace reconnects to dst.
    own.move_flood_start_col() = static_cast<uint8_t>(dst_col);
    own.move_flood_start_row() = static_cast<uint8_t>(dst_row);
    own.move_flood_goal_col()  = static_cast<uint8_t>(src_col);
    own.move_flood_goal_row()  = static_cast<uint8_t>(src_row);
    own.move_path_slot_id()    = -1;

    // @0x00494e1e-0x00494e42: read passable[dst], call the pathfinder, write the SAME value back --
    // a real read-modify-write in the disassembly, even though it is value-preserving (the
    // pathfinder itself never writes `passable`). Preserved literally.
    const uint8_t saved_dest_passable = own.planes().passable_at(dst_col, dst_row);
    MH_LIBMH_BIND(llm_tact_move_path_build)();
    own.planes().passable_at(dst_col, dst_row) = saved_dest_passable;

    return (*v.move_flood_result_col == static_cast<uint8_t>(dst_col) &&
            *v.move_flood_result_row == static_cast<uint8_t>(dst_row))
               ? 1
               : 0;
}

} // namespace detail

int32_t move_step_attempt(int32_t src_col, int32_t src_row, int32_t dst_col, int32_t dst_row) {
    tact_state st = state();
    return detail::move_step_attempt(st.read, st.own, src_col, src_row, dst_col, dst_row);
}

// ---- the rebind ABI shim -------------------------------------------------------------------------
//
// Four unsigned coordinates where the wrapper takes signed ones.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {

int32_t move_step_attempt(uint32_t src_col, uint32_t src_row, uint32_t dst_col, uint32_t dst_row) {
    return mh::tact::move_step_attempt(static_cast<int32_t>(src_col), static_cast<int32_t>(src_row),
                                       static_cast<int32_t>(dst_col), static_cast<int32_t>(dst_row));
}

} // namespace rebind_arm

} // namespace mh::tact
