//
// sim/sim_unit_group_step_ground.cpp -- see sim_unit_group_step_ground.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_group_step_ground_00483011.asm) -- see the header
// banner for the pathfinder_air_mode_flag hazard (this function writes 0, once), the
// start_col_delta/width_mask vs start_row_delta/height_mask axis pairing, the start_col/start_row
// carry-over defect that is preserved deliberately, and the two ways the three commit passes differ.
//
#include "sim/sim_unit_group_step_ground.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_IDLE_SCATTER, UNIT_STATE_HOVER_ENGAGE (shared there)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_group_step_ground_calls &live_unit_group_step_ground_calls() {
    static const unit_group_step_ground_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_strat_unit_calc_range_approach_point),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_group_scratch_add_unit_and_normalize_heading),
        MH_LIBMH_BIND(llm_strat_group_scratch_compute_centroid),
        MH_LIBMH_BIND(llm_strat_unit_get_ready_home_building),
        MH_LIBMH_BIND(llm_strat_trace_greedy_path),
        MH_LIBMH_BIND(llm_strat_pathtrace_remove_loops),
        MH_LIBMH_BIND(llm_strat_path_find_free_slot),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_path_write_from_solver),
        MH_LIBMH_BIND(llm_strat_heading_candidate_find_slot),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(map_unit_PutOnMap),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_state (order/state) literals. The NAMES are the Ghidra enum's own -- see
// sim_order_enqueue.h's UNIT_STATE_* block, whose comment records the 2026-08-08
// `get-data-type-by-string llm_strat_unit_state` dump; the enum is real, it just is not emitted as a
// C enum by the struct generator, so each TU declares the members it needs as local constants. The
// four below are the ones no sibling has declared yet; IDLE_SCATTER (0x13) and HOVER_ENGAGE (0x2e)
// are NOT redeclared here -- they are reused from sim_order_enqueue.h, as are UNIT_TYPE_A_PLANE /
// UNIT_TYPE_H_PLANE (cfg_enum_E_UNIT_TYPE 0x11 / 0x12), which this function tests at four sites.
inline constexpr uint16_t GROUP_STEP_GROUND_ORDER_ATTACK_UNIT        = 0x1a; // 0x00483055
inline constexpr uint16_t GROUP_STEP_GROUND_ORDER_ATTACK_UNIT_RETURN = 0x1b; // 0x00483061
inline constexpr uint16_t GROUP_STEP_GROUND_ORDER_ATTACK_BUILDING    = 0x1c; // 0x0048306f
inline constexpr uint16_t GROUP_STEP_GROUND_ORDER_LANDING_REQUEST    = 0x29; // 0x00483461/0x004835bf
inline constexpr uint16_t GROUP_STEP_GROUND_STATE_GROUP_STEP         = 0x0b; // 0x00483416

// The state a member is put into once it has been RELOCATED one tile into formation (0x00483b36,
// 0x00484208, 0x004848fa). UNLIKE the four above, this one has NO name: the Ghidra .c draft prints
// it as a bare 0x37 (llm_strat_unit_set_state_of's `state` parameter is not typed with the enum, so
// the decompiler never resolves it) and no other TU in this tree names the value either.
// declared_needs: give 0x37 a member name in llm_strat_unit_state and retype that parameter, rather
// than letting a fourth TU invent a local spelling.
inline constexpr int16_t GROUP_STEP_GROUND_STATE_FORMATION_PLACED = 0x37;

// llm_strat_unit_notify_status's status-code literal this function passes at all three commit sites
// (0x00483c0f, 0x00484332, 0x004849d3). Same VALUE as the plane sibling's
// GROUP_STEP_PLANE_NOTIFY_PATH_COMPUTED and, like it, unbacked -- named locally per the same
// convention rather than shared across TUs.
inline constexpr uint32_t GROUP_STEP_GROUND_NOTIFY_PATH_COMPUTED = 100; // 0x64

// llm_strat_target_release_ref's `mode` (EBX = 1 at 0x00483326) -- the same literal every sibling
// release site passes.
inline constexpr uint32_t GROUP_STEP_GROUND_TARGET_RELEASE_MODE = 1;

// map_object_unit::target_ref bit that selects WHICH ROSTER the target lives in: set -> units[],
// clear -> buildings[] (`TEST byte ptr [EAX + 0x8c],0x80` / JZ at 0x004830a2-0x004830a9).
inline constexpr int16_t GROUP_STEP_GROUND_TARGET_REF_IS_UNIT = 0x80;

// map_object_unit::path_slot_id's "no path assigned" sentinel (0x00483918).
inline constexpr uint8_t GROUP_STEP_GROUND_PATH_SLOT_NONE = 0xff;

// The candidate list is a fixed THREE slots per heading (`CMP [i],0x3; JGE` at 0x00483668,
// 0x00483d0a, 0x004843fc), terminated early by turn_delta == -1 rather than by a count.
inline constexpr int32_t GROUP_STEP_GROUND_CANDIDATE_SLOTS = 3;
inline constexpr int32_t GROUP_STEP_GROUND_SLOT_INACTIVE   = -1;

// The group scratch's member cap. The scan stops the moment the count REACHES it (0x0048358f) --
// note the member whose insert took it there has already been added; this is a cap check on the
// callee's result, not a rejected insert. Same numeric value as UNITS_PER_PLAYER by coincidence of
// the array sizing, kept as its own name so the two do not look interchangeable.
inline constexpr int32_t GROUP_STEP_GROUND_SCRATCH_CAP = 0x64;

// fine->tile: the ESTABLISHED `/ 32` form (see the header banner on why this function does NOT use
// move_walker's shift-form override, and on why the name is NOT the bare `fine_to_tile` two sibling
// TUs already define incompatibly). Provably identical to the SAR/SHL/SBB/SAR sequence this
// function's own .asm emits at 0x00483114-0x00483131 and its three siblings.
inline int32_t ground_fine_to_tile(int32_t fine) { return fine / 32; }

// ---- ONE PLANNING PASS ------------------------------------------------------------------------
//
// 0x00483600-0x00483867, 0x00483cb8-0x00483f39 and 0x004843aa-0x0048462b. Diffed instruction by
// instruction: the three sites are the SAME code with different stack slots, so this is factored
// rather than pasted three times. Returns the `chosen` value the following commit pass compares each
// member's heading against (stack slot -0x3c) -- note it is cur_unit->move_heading in the
// no-candidate arm and a slot's turn_delta otherwise, i.e. the two arms publish values from two
// different domains. That is the original's own conflation, not a translation shortcut.
//
// cur_unit->move_heading is RE-READ from the record at every site the original re-reads it (the .asm
// reloads the CUR_UNIT pointer and the byte before each use), so nothing is hoisted across the
// outward trace_greedy_path / pathtrace_remove_loops calls.
static int32_t ground_plan_pass(const sim_view &v, unit &u, const unit_group_step_ground_calls &c,
                                int32_t start_col, int32_t start_row, int32_t goal_col, int32_t goal_row,
                                uint8_t facing_override) {
    // The straight-ahead reference trace, from the pass's start tile along the unit's own heading.
    c.trace_greedy_path(start_col, start_row, u.move_heading, goal_col, goal_row, facing_override);
    while (c.pathtrace_remove_loops() != 0) {
        // drain until the trace has no more loops to remove (0x00483650-0x00483657).
    }

    // 0x00483659-0x00483661: seed the "best so far" from the straight trace's length.
    uint32_t best_len  = *v.pathtrace_len;
    int32_t  best_cand = -1;

    for (int32_t i = 0; i < GROUP_STEP_GROUND_CANDIDATE_SLOTS; ++i) {
        const heading_slot &slot =
            v.heading_candidates[u.move_heading * GROUP_STEP_GROUND_CANDIDATE_SLOTS + i];
        // 0x00483685/0x0048368c: `CMP dword,-1; JG` -- a SIGNED test, and turn_delta == -1 is the
        // inactive-slot sentinel that ends the candidate list. (The Ghidra draft renders this as
        // `(uint)turn_delta < 0x80000000`, the same predicate spelled the decompiler's way.)
        if (!(slot.turn_delta > GROUP_STEP_GROUND_SLOT_INACTIVE)) break;

        // start_col_delta goes with WIDTH_MASK and arg 1; start_row_delta with HEIGHT_MASK and
        // arg 2. Swapping them breaks pathfinding silently on every non-square map -- see the
        // header banner and mh_llm_strat_heading_slot's own field comments.
        const int32_t cand_col = static_cast<int32_t>(
            static_cast<uint32_t>(start_col + slot.start_col_delta) & map_width_mask(v));
        const int32_t cand_row = static_cast<int32_t>(
            static_cast<uint32_t>(start_row + slot.start_row_delta) & map_height_mask(v));
        c.trace_greedy_path(cand_col, cand_row, slot.turn_delta, goal_col, goal_row,
                            facing_override);
        while (c.pathtrace_remove_loops() != 0) {
            // drain (0x00483717-0x0048371e).
        }
        // 0x00483720-0x00483736: `CMP best_len,LEN; JBE` -- UNSIGNED, and pathtrace_len is a
        // uint32_t, so a shorter trace wins. Ties keep the earlier candidate.
        if (best_len > *v.pathtrace_len) {
            best_len  = *v.pathtrace_len;
            best_cand = i;
        }
    }

    // 0x00483744-0x00483867: re-run the winner so the pathtrace buffers hold IT rather than the last
    // candidate tried, and publish `chosen`.
    int32_t chosen;
    if (best_cand == -1) {
        chosen = u.move_heading; // 0x0048374a
        c.trace_greedy_path(start_col, start_row, u.move_heading, goal_col, goal_row,
                            facing_override);
        while (c.pathtrace_remove_loops() != 0) {
            // drain (0x00483783-0x0048378a).
        }
    } else {
        const heading_slot &slot =
            v.heading_candidates[u.move_heading * GROUP_STEP_GROUND_CANDIDATE_SLOTS + best_cand];
        chosen                 = slot.turn_delta; // 0x004837a8
        const int32_t cand_col = static_cast<int32_t>(
            static_cast<uint32_t>(start_col + slot.start_col_delta) & map_width_mask(v));
        const int32_t cand_row = static_cast<int32_t>(
            static_cast<uint32_t>(start_row + slot.start_row_delta) & map_height_mask(v));
        // 0x00483828 loads turn_delta a SECOND time into its own stack slot before the push;
        // transcribed as a second read rather than reusing `chosen` so the shape matches.
        const int32_t cand_mode = slot.turn_delta;
        c.trace_greedy_path(cand_col, cand_row, cand_mode, goal_col, goal_row, facing_override);
        while (c.pathtrace_remove_loops() != 0) {
            // drain (0x0048385e-0x00483865).
        }
    }
    return chosen;
}

// ---- ONE COMMIT PASS --------------------------------------------------------------------------
//
// 0x00483867-0x00483c31, 0x00483f39-0x00484354 and 0x0048462b-0x004849f5. The three sites are the
// same body except for the two differences the header banner names, both passed in explicitly:
//   first_index            -- 0 for pass 1 (0x0048386e), 1 for passes 2 and 3 (0x00483f40/0x00484632)
//   write_state_directly   -- TRUE only for pass 2, whose no-relocation arm additionally stores
//                             cfg Unit[proto].move_op_code straight into units[p][member].state as a
//                             word (0x00484275-0x00484296) before calling unit_set_state_of with the
//                             same value. Passes 1 and 3 call unit_set_state_of only.
//
// start_col / start_row are IN-OUT by reference on purpose: they are one pair of stack slots the
// original shares with the planner, and this pass overwrites them with each member's own tile. The
// next planning pass then starts from wherever the last member stood. See the header banner --
// preserved defect, not a translation slip.
//
// Returns TRUE when the pass hit the "no free path slot" bail-out, which aborts the WHOLE function:
// the original JMPs from inside the loop straight to the epilogue at 0x00484a0a, WITHOUT restoring
// cur_unit->move_heading. In passes 2 and 3 move_heading has already been re-classed by then, so the
// unit is left holding the synthetic heading. That is what the binary does; reproduced.
static bool ground_commit_pass(const sim_view &v, sim_store &own, const unit_group_step_ground_calls &c,
                               uint16_t player, int32_t first_index, int32_t group_count,
                               int32_t shape_class, int32_t chosen, double saved_activity_clock,
                               bool write_state_directly, int32_t &start_col, int32_t &start_row,
                               int32_t &processed_count) {
    // Stack slot -0x24. The original increments it once per matching member and NEVER reads it back
    // (`MOV EAX,[-0x24]; INC [-0x24]`, result discarded) at all three sites. Kept so the shape is
    // visible; it touches no tracked state either way.
    [[maybe_unused]] int32_t matched = 0;

    for (int32_t i = first_index; i < group_count; ++i) {
        const int32_t member = v.group_move_scratch[i].unit_idx; // 0x0048388a

        // Re-read helper: the original reloads the roster record from scratch (`IMUL player,0x5b04;
        // IMUL member,0xe9`) at every single field access, including across the outward calls below.
        // A lambda keeps that faithful instead of caching a reference the callees could invalidate.
        auto m = [&]() -> const unit & { return v.units[player * v.caps.units + member]; };

        // 0x004838b3-0x004838c6: only members whose heading belongs to THIS pass's path-shape class.
        if (shape_class !=
            v.heading_candidates[m().move_heading * GROUP_STEP_GROUND_CANDIDATE_SLOTS].cand_facing) {
            continue;
        }
        ++matched; // 0x004838c8

        const int32_t free_slot = c.path_find_free_slot(player); // 0x004838d5
        if (free_slot == -1) {
            // 0x004838e3-0x004838fa: no path slot left -- park THIS member and abandon the whole
            // function (not just this pass).
            c.unit_set_state_of(player, member, static_cast<int16_t>(UNIT_STATE_IDLE_SCATTER));
            return true;
        }

        // 0x004838ff-0x00483930: the member already held a slot -- free it, it is about to be
        // reassigned to `free_slot`.
        if (m().path_slot_id != GROUP_STEP_GROUND_PATH_SLOT_NONE) {
            c.path_free_slot(player, member);
        }

        // 0x00483933-0x00483976: the shared start-tile slots take THIS member's tile.
        start_col = m().x;
        start_row = m().y;

        // 0x00483992-0x004839cd: does the member need turning into the chosen heading, and is there
        // a candidate slot that describes how? `CMP [cand],-1; JG` is signed (the draft's
        // `0x7fffffff < cand` is the same predicate). The heading is re-read for the find_slot call.
        bool    relocate  = false;
        int32_t cand_slot = -1;
        if (m().move_heading != chosen) {
            cand_slot = c.heading_candidate_find_slot(m().move_heading, chosen);
            relocate  = cand_slot > GROUP_STEP_GROUND_SLOT_INACTIVE;
        }

        if (relocate) {
            // 0x004839d4-0x00483b78: lift the member off its tile, step it one candidate delta into
            // formation, and re-stamp the driver's activity clock onto it.
            c.unit_unlink_tile(player, static_cast<uint16_t>(member));
            c.fow_remove_sight(player, start_col, start_row,
                               v.cfg_units[m().unit_proto_id].sight);

            // The deltas come from the member's ORIGINAL heading -- move_heading is only overwritten
            // further down, at 0x00483b30, after PutOnMap and the FoW update.
            const heading_slot &slot =
                v.heading_candidates[m().move_heading * GROUP_STEP_GROUND_CANDIDATE_SLOTS +
                                     cand_slot];
            const uint32_t new_col =
                static_cast<uint32_t>(start_col + slot.start_col_delta) & map_width_mask(v);
            const uint32_t new_row =
                static_cast<uint32_t>(start_row + slot.start_row_delta) & map_height_mask(v);

            // 0x00483aae-0x00483aca: PutOnMap takes the two masked values TRUNCATED TO BYTES...
            c.map_unit_PutOnMap(player, static_cast<uint16_t>(member),
                                static_cast<uint8_t>(new_col), static_cast<uint8_t>(new_row));
            // ...while the FoW update at 0x00483afc-0x00483b0f takes the FULL dwords. Not a
            // simplification opportunity: the two callees are given different widths on purpose.
            c.map_fow_UpdateFoWPlus(player, new_col, new_row, v.cfg_units[m().unit_proto_id].sight);

            own.unit_at(player, member).move_heading = static_cast<uint8_t>(chosen); // 0x00483b30
            c.unit_set_state_of(player, member, GROUP_STEP_GROUND_STATE_FORMATION_PLACED);
            // 0x00483b4d-0x00483b78: the original copies the snapshot as two dwords into the
            // member's activity_clock; those two dwords are that double.
            own.unit_at(player, member).activity_clock = saved_activity_clock;
        } else {
            // 0x00483b7a / 0x0048424f / 0x0048493e -- the pass-2 site is the odd one, see the
            // function comment.
            if (write_state_directly) {
                own.unit_at(player, member).state =
                    static_cast<uint16_t>(v.cfg_units[m().unit_proto_id].move_op_code);
            }
            c.unit_set_state_of(player, member,
                                static_cast<int16_t>(v.cfg_units[m().unit_proto_id].move_op_code));
        }

        // 0x00483bb9-0x00483c0a: the source tile handed to the path writer is RE-READ from the
        // roster, so on the relocation path it is the NEW tile, not start_col/start_row.
        c.path_write_from_solver(player, member, m().x, m().y, free_slot);
        c.unit_notify_status(player, member, GROUP_STEP_GROUND_NOTIFY_PATH_COMPUTED);
        ++processed_count; // 0x00483c26
    }
    return false;
}

void unit_group_step_ground(const sim_view &v, sim_store &own,
                            const unit_group_step_ground_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    uint8_t facing_override = 0xff; // 0x00483029 -- no facing override for the path solves below.
    int32_t group_count     = 0;    // 0x0048302d -- the group scratch's member count (by address).

    // 0x00483034: GROUND mode. ZERO, unconditional, and the only write to this global in the whole
    // function -- see the header hazard note before trusting any prose that says otherwise.
    own.pathfinder_air_mode_flag() = 0;

    // 0x0048303e-0x0048304d: snapshot the driver unit's activity clock (two dword loads out of
    // unit+0x8/+0xc, which is this double). Re-stamped onto every RELOCATED member, and onto nothing
    // else -- a member that merely gets a new path keeps its own clock.
    const double saved_activity_clock = u.activity_clock;

    if (u.order == GROUP_STEP_GROUND_ORDER_ATTACK_UNIT ||
        u.order == GROUP_STEP_GROUND_ORDER_ATTACK_UNIT_RETURN ||
        u.order == GROUP_STEP_GROUND_ORDER_ATTACK_BUILDING) {
        // 0x0048307a-0x0048309a.
        const int32_t target_owner =
            static_cast<int32_t>(ref_owner(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref))));
        const int32_t target_index = static_cast<uint16_t>(u.target_index);

        if ((u.target_ref & GROUP_STEP_GROUND_TARGET_REF_IS_UNIT) != 0) {
            // 0x004830af-0x00483166: the target is a UNIT.
            if (v.units[target_owner * v.caps.units + target_index].energy <= 0.0) {
                c.unit_set_state_order(UNIT_STATE_IDLE_SCATTER, UNIT_STATE_IDLE_SCATTER);
                return;
            }
            // The original re-derives owner/index inline here rather than reusing the two locals it
            // just filled; identical values, so the locals are used.
            c.unit_get_coords(static_cast<uint16_t>(target_owner), target_index, &u.target_fine_x,
                              &u.target_fine_y);
            u.goal_x = static_cast<uint8_t>(ground_fine_to_tile(u.target_fine_x));
            u.goal_y = static_cast<uint8_t>(ground_fine_to_tile(u.target_fine_y));
        } else {
            // 0x00483181-0x00483217: the target is a BUILDING.
            if (v.buildings[target_owner * v.caps.buildings + target_index].energy <= 0.0) {
                c.unit_set_state_order(UNIT_STATE_IDLE_SCATTER, UNIT_STATE_IDLE_SCATTER);
                return;
            }
            c.bldg_get_coords(static_cast<uint16_t>(target_owner), target_index, &u.target_fine_x,
                              &u.target_fine_y);
            u.goal_x = static_cast<uint8_t>(ground_fine_to_tile(u.target_fine_x));
            u.goal_y = static_cast<uint8_t>(ground_fine_to_tile(u.target_fine_y));
        }

        // 0x0048322d-0x0048328f: read through the ROSTER (the original does fresh
        // units[cur_player][cur_index] arithmetic here rather than going through CUR_UNIT; same
        // record) and skip the whole stand-off step for the two plane classes.
        const uint32_t proto_type =
            v.cfg_units[v.units[player * v.caps.units + unit_index].unit_proto_id].type;
        if (proto_type != UNIT_TYPE_A_PLANE &&
            proto_type != UNIT_TYPE_H_PLANE) {
            // 0x00483296-0x004832c1: ask for a stand-off tile inside weapon range of the goal.
            // uint32_t, not int32_t: llm_strat_unit_calc_range_approach_point's committed
            // out-params are `uint *` (TACT1-P C6 -- the boundary carries the committed pointee).
            uint32_t approach_col = u.goal_x;
            uint32_t approach_row = u.goal_y;
            if (c.unit_calc_range_approach_point(&approach_col, &approach_row) == 0) {
                // 0x00483326-0x0048338b: no reachable stand-off tile -- drop the target and fall
                // back to the unit class's default order.
                c.target_release_ref(player, unit_index, GROUP_STEP_GROUND_TARGET_RELEASE_MODE);
                u.target_ref   = 0;
                u.target_index = 0;
                c.unit_set_state_order(
                    static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].default_op_code),
                    static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].default_op_code));
                return;
            }
            // 0x004832c3-0x0048331f: commit the stand-off tile; if the unit is already standing on
            // it there is nothing to walk, so go straight to the in-place engage state.
            u.goal_x = static_cast<uint8_t>(approach_col);
            u.goal_y = static_cast<uint8_t>(approach_row);
            if (u.x == u.goal_x && u.y == u.goal_y) {
                c.unit_set_state(UNIT_STATE_HOVER_ENGAGE);
                return;
            }
        }
    }

    // ---- 0x00483390-0x004833dd: seed the group with this unit -------------------------------------
    int32_t       start_col = u.x;
    int32_t       start_row = u.y;
    const int32_t goal_col  = u.goal_x;
    const int32_t goal_row  = u.goal_y;

    c.group_scratch_add_unit_and_normalize_heading(player, unit_index, &group_count);

    // ---- 0x004833ed-0x00483593: gather the rest of the group ------------------------------------
    // Only LATER roster slots are considered (the scan starts at cur_index + 1), so "the group" is
    // always this unit plus its successors.
    for (int32_t i = unit_index + 1; i < v.caps.units; ++i) {
        auto other = [&]() -> const unit & { return v.units[player * v.caps.units + i]; };

        if (other().state != GROUP_STEP_GROUND_STATE_GROUP_STEP) continue;      // 0x00483416
        if (other().order != u.order) continue;                                 // 0x00483443
        if (other().order == GROUP_STEP_GROUND_ORDER_LANDING_REQUEST) continue; // 0x00483461
        const uint32_t other_type = v.cfg_units[other().unit_proto_id].type;    // 0x00483490/bc
        if (other_type == UNIT_TYPE_A_PLANE ||
            other_type == UNIT_TYPE_H_PLANE) {
            continue;
        }
        if (other().order == GROUP_STEP_GROUND_ORDER_ATTACK_BUILDING ||    // 0x004834e0
            other().order == GROUP_STEP_GROUND_ORDER_ATTACK_UNIT ||        // 0x00483500
            other().order == GROUP_STEP_GROUND_ORDER_ATTACK_UNIT_RETURN) { // 0x00483522
            continue;
        }
        if (other().goal_x != goal_col) continue; // 0x00483547
        if (other().goal_y != goal_row) continue; // 0x0048356d

        c.group_scratch_add_unit_and_normalize_heading(player, i, &group_count);
        // 0x0048358f: the scratch is full -- stop scanning. The member that filled it IS registered.
        if (group_count == GROUP_STEP_GROUND_SCRATCH_CAP) break;
    }

    // ---- 0x00483599-0x004835b7: the group's toroidal centroid becomes the path start -------------
    // Computed ONCE. Passes 2 and 3 do not get it back -- see the carry-over note in the header.
    int32_t centroid_col = 0;
    int32_t centroid_row = 0;
    c.group_scratch_compute_centroid(player, group_count, &centroid_col, &centroid_row);
    start_col = centroid_col;
    start_row = centroid_row;

    // ---- 0x004835ba-0x004835fd: a landing unit approaches along its home building's facing --------
    // The plane sibling's identical arm additionally clears the air-mode flag here; ground never set
    // it, so there is nothing to clear and the original correspondingly has no such write.
    if (u.order == GROUP_STEP_GROUND_ORDER_LANDING_REQUEST) {
        const int32_t home_building_idx = c.unit_get_ready_home_building();
        if (home_building_idx != 0) {
            const uint16_t building_id =
                v.buildings[player * v.caps.buildings + home_building_idx].building_id;
            facing_override = v.cfg_buildings[building_id].door_approach_route[0];
        }
    }

    // ---- PASS 1 (0x00483600-0x00483c31) -----------------------------------------------------------
    int32_t processed_count = 0; // 0x00483600
    // Slot 0 of the current heading's three, and ONLY slot 0 -- cand_facing is a per-heading
    // path-shape class in {1,2,3}, not a per-slot facing (0x00483607-0x0048361c).
    int32_t shape_class =
        v.heading_candidates[u.move_heading * GROUP_STEP_GROUND_CANDIDATE_SLOTS].cand_facing;

    int32_t chosen = ground_plan_pass(v, u, c, start_col, start_row, goal_col, goal_row,
                                      facing_override);
    if (ground_commit_pass(v, own, c, player, /*first_index=*/0, group_count, shape_class, chosen,
                           saved_activity_clock, /*write_state_directly=*/false, start_col,
                           start_row, processed_count)) {
        return;
    }

    // 0x00483c31-0x00483c37: everyone claimed -- return WITHOUT touching move_heading (it has not
    // been modified yet, so there is nothing to restore).
    if (processed_count >= group_count) return;

    // ---- PASS 2 (0x00483c3d-0x00484354) -----------------------------------------------------------
    const int32_t saved_heading = u.move_heading; // 0x00483c3d

    // Stack slot -0x38. The original NEVER initialises it and writes it only inside the switch below,
    // so a shape_class outside {1,2,3} would leave pass 3 branching on stack garbage. cand_facing is
    // documented as {1,2,3}, making that unreachable; 0 is chosen here because the pass-3 switch
    // treats it as "change nothing", which is the same outcome garbage produces for all but three
    // values. Declared as a divergence in the unreachable case, not claimed as equivalent.
    int32_t next_shape = 0;

    // 0x00483c4c-0x00483cb8. NOT the same mapping as pass 3's switch below -- do not fold them.
    if (shape_class == 1) {
        u.move_heading = 0;
        shape_class    = 2;
        next_shape     = 3;
    } else if (shape_class == 2) {
        u.move_heading = 4;
        shape_class    = 1;
        next_shape     = 3;
    } else if (shape_class == 3) {
        u.move_heading = 4;
        shape_class    = 1;
        next_shape     = 2;
    }

    chosen = ground_plan_pass(v, u, c, start_col, start_row, goal_col, goal_row, facing_override);
    // first_index 1 (scratch member 0 is cur_unit, only eligible in pass 1) and the extra direct
    // `state` write that only this pass performs.
    if (ground_commit_pass(v, own, c, player, /*first_index=*/1, group_count, shape_class, chosen,
                           saved_activity_clock, /*write_state_directly=*/true, start_col, start_row,
                           processed_count)) {
        return;
    }

    // ---- PASS 3 (0x00484354-0x004849f5) -----------------------------------------------------------
    if (processed_count < group_count) {
        shape_class = next_shape; // 0x00484360
        // 0x00484366-0x004843aa: a different heading per class from pass 2's table.
        if (shape_class == 1) {
            u.move_heading = 4;
        } else if (shape_class == 2) {
            u.move_heading = 0;
        } else if (shape_class == 3) {
            u.move_heading = 1;
        }

        chosen = ground_plan_pass(v, u, c, start_col, start_row, goal_col, goal_row,
                                  facing_override);
        if (ground_commit_pass(v, own, c, player, /*first_index=*/1, group_count, shape_class,
                               chosen, saved_activity_clock, /*write_state_directly=*/false,
                               start_col, start_row, processed_count)) {
            return;
        }
    }

    // 0x004849f5: put the driver unit's real heading back. Reached whether pass 3 ran or was skipped
    // by the count check -- but NOT on the no-free-path-slot bail-out above, which leaves the
    // synthetic heading in place. Faithful: the original's bail-out jumps past this store.
    u.move_heading = static_cast<uint8_t>(saved_heading);

    // 0x00484a04 reloads processed_count and compares it against group_count one last time with no
    // consumer of the flags -- dead code the Watcom epilogue left behind. Not emitted.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_group_step_ground() {
    sim_state st = state();
    detail::unit_group_step_ground(st.read, st.own, live_unit_group_step_ground_calls());
}


} // namespace mh::sim
