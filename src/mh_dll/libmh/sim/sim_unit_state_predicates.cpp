//
// sim/sim_unit_state_predicates.cpp -- see sim_unit_state_predicates.h. Translated from the
// DISASSEMBLY, not from Ghidra's C drafts:
//   tmp/decomp/llm_strat_unit_attack_target_is_dead_004d4226.asm
//   tmp/decomp/llm_strat_unit_state_is_in_transit_004d431e.asm
//   tmp/decomp/llm_strat_unit_is_idle_or_parked_004d4452.asm
//
#include "sim/sim_unit_state_predicates.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// The tile<-fine conversion llm_strat_unit_attack_target_is_dead's ATTACK_BUILDING arm performs on
// target_fine_x/y (0x004d42d2-0x004d42fe): `SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`.
// Value-for-value this is C's truncating `/ 32` on a signed int -- same idiom, same verification, as
// sim_order_enqueue.cpp's anonymous-namespace fine_to_tile(), re-derived locally here per the
// translator brief rather than shared across TUs.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// Membership test over llm_strat_unit_state values {GROUP_MARSHAL 0xa, MOVE_WALKER 0xf, MOVE_PATH
// 0x11, MOVE_PATH_12 0x12, EXIT_STORAGE_BEGIN..EXIT_WAIT 0x20-0x22, ENTER_STORAGE_BEGIN..PARKED_2B
// 0x24-0x2b} -- llm_strat_unit_state_is_in_transit's own branch tree, transcribed once and applied
// to both the state and order fields below (the asm repeats the SAME tree at 0x004d4340-0x004d437b
// for state and 0x004d4385-0x004d43cd for order, differing only in which field is loaded). No
// backing Ghidra enum on this field (see ai_hq_attack_commit.cpp's identical finding on the same
// `state`/`order` domain) -- names are the plate's own vocabulary, kept as comments only.
bool unit_state_in_transit_set(uint16_t v) {
    if (v < 0x11) return v == 0xa || v == 0xf; // GROUP_MARSHAL / MOVE_WALKER
    if (v <= 0x12) return true;                // MOVE_PATH / MOVE_PATH_12
    if (v < 0x20) return false;
    if (v <= 0x22) return true; // EXIT_STORAGE_BEGIN..EXIT_WAIT
    if (v < 0x24) return false;
    return v <= 0x2b; // ENTER_STORAGE_BEGIN..PARKED_2B
}

// Membership test over {PARKED..EXIT_WAIT 0x1f-0x22, ENTER_STORAGE_BEGIN..PARKED_2B 0x24-0x2b} --
// llm_strat_unit_is_idle_or_parked's own branch tree (0x004d447b-0x004d4493 for state,
// 0x004d44b1-0x004d44c7 for order), same shape as unit_state_in_transit_set above but a different,
// overlapping value set -- do not merge the two.
bool unit_state_in_idle_parked_set(uint16_t v) {
    return (v >= 0x1f && v <= 0x22) || (v >= 0x24 && v <= 0x2b);
}

} // namespace

namespace detail {

int32_t unit_attack_target_is_dead(const sim_view &v, int32_t player, int32_t unit_index) {
    const unit &u = unit_of(v, (uint32_t)player, unit_index);

    // 0x004d4243-0x004d4255: own.state == ATTACK_UNIT(0x1a) OR own.order == ATTACK_UNIT(0x1a).
    if (u.state == 0x1a || u.order == 0x1a) {
        // 0x004d4257-0x004d4281: target row = ref_owner(own.target_ref) * stride +
        // (ushort)own.target_index * stride. target_ref's low nibble is the owner player (see
        // sim_state.h's ref_owner() -- same packed-ref convention).
        const uint32_t target_owner = ref_owner((uint32_t)(uint16_t)u.target_ref);
        const int32_t  target_index = (int32_t)(uint16_t)u.target_index;
        const unit    &target       = unit_of(v, target_owner, target_index);

        // 0x004d4287-0x004d429e (FLD energy / FSUB pending_damage / FLDZ / FCOMPP / FNSTSW / SAHF /
        // JC). The original's JC branch ("not dead", return 0) fires when ST(0)=0.0 is LESS THAN
        // ST(1)=diff -- true for an ordinary positive diff -- OR when the compare is UNORDERED
        // (diff is NaN), because x87's unordered result also sets CF. So "dead" (JNC, return 1) is
        // exactly the ORDERED `diff <= 0.0` case, and NOT `!(0.0 < diff)` (which would read a NaN
        // diff as "dead", the opposite of the original). Same x87 idiom sim_bldg_alive.cpp's header
        // documents for its own FCOMP/JNC. No reachable state is expected to put a NaN here, but the
        // `<=` form costs nothing and needs no argument.
        const double diff = target.energy - target.pending_damage;
        return (diff <= 0.0) ? 1 : 0;
    }

    // 0x004d42af-0x004d42c1: own.state == ATTACK_BUILDING(0x1c) OR own.order == ATTACK_BUILDING(0x1c).
    if (u.state == 0x1c || u.order == 0x1c) {
        // 0x004d42c3-0x004d4304: truncating /32 on the fine coords, then the occupancy plane's
        // `building` field at that tile.
        const int32_t tile_x = fine_to_tile(u.target_fine_x);
        const int32_t tile_y = fine_to_tile(u.target_fine_y);
        return (tile_at(v, tile_x, tile_y).building == 0) ? 1 : 0;
    }

    // 0x004d4317-0x004d4319: neither state matched (on state or order).
    return 0;
}

int32_t unit_state_is_in_transit(const sim_view &v, uint32_t player, uint32_t unit_id) {
    const unit &u = unit_of(v, player, (int32_t)unit_id);
    if (unit_state_in_transit_set(u.state)) return 1;  // 0x004d4340-0x004d437b
    return unit_state_in_transit_set(u.order) ? 1 : 0; // 0x004d4385-0x004d43cd
}

int32_t unit_is_idle_or_parked(const sim_view &v, int32_t player, int32_t unit_index) {
    const unit &u = unit_of(v, (uint32_t)player, unit_index);
    if (unit_state_in_idle_parked_set(u.state)) return 1;  // 0x004d447b-0x004d4493
    return unit_state_in_idle_parked_set(u.order) ? 1 : 0; // 0x004d44b1-0x004d44c7
}

} // namespace detail

int32_t unit_attack_target_is_dead(int32_t player, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_attack_target_is_dead(v, player, unit_index);
}

int32_t unit_state_is_in_transit(uint32_t player, uint32_t unit_id) {
    const sim_view v = state().read;
    return detail::unit_state_is_in_transit(v, player, unit_id);
}

int32_t unit_is_idle_or_parked(int32_t player, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_is_idle_or_parked(v, player, unit_index);
}


} // namespace mh::sim
