//
// tact/tact_unit_death_tick.h -- TACT1C: the per-tick driver for a unit in the dying state, run
// until its decay progress crosses the threshold and it is destroyed.
//
//   llm_tact_unit_death_tick @0x0042fb0b (0x90)
//   int __watcall llm_tact_unit_death_tick(int unit_idx)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call this function makes, indirected for offline testability (also called by
// llm_tact_unit_weapons_tick, translated in parallel -- irrelevant here, both call the ORIGINAL).
struct unit_death_tick_calls {
    void (*unit_destroy)(uint32_t unit_idx); // llm_tact_unit_destroy @0x00431d8d
};

const unit_death_tick_calls &live_unit_death_tick_calls();

namespace detail {

// llm_tact_unit_death_tick @0x0042fb0b.
//
// 1. @0x0042fb28-0x0042fb36: snapshot `type` = unit.type (the character-class index used below) --
//    BEFORE `progress` is touched. The two fields don't alias, so the read order has no observable
//    effect, but it is reproduced literally anyway.
// 2. @0x0042fb39: `CMP dword ptr [local_18],0x0` -- computes a comparison whose flags nothing
//    downstream reads: the very next instruction is an unrelated IMUL (address arithmetic, which
//    doesn't consume flags), and the branch a few instructions later is gated by a FRESH CMP against
//    `progress` (step 4). Verified inert against every instruction between it and the next branch;
//    not reproduced.
// 3. @0x0042fb3d-0x0042fb44: `++progress` -- a byte-wide INC, wraps mod 256 like the field's own
//    uint8_t type (INC does not consume incoming flags, so step 2's dead CMP couldn't have fed it
//    even if something downstream had read it).
// 4. @0x0042fb4a-0x0042fb58: a FRESH `CMP progress,0x69` / `JBE` on the POST-increment value --
//    "still dying" when progress <= 0x69, i.e. progress < 0x6a.
//    - still dying (@0x0042fb6b-0x0042fb88): `move_state_timer += character_types[type].death_time`
//      (FLD death_time / FADD move_state_timer / FSTP move_state_timer -- x87 addition is
//      commutative bit-for-bit, so `+=` reproduces this sequence exactly). Returns 0.
//    - threshold reached (@0x0042fb5a-0x0042fb69): `unit_destroy(unit_idx)`. Returns 1.
//
// `character_types` is read through the STORE (`own.character_type_at`), not the view, matching
// tact_unit_mine_arm_tick.h's precedent for the same table: the mission loader writes it (so it is a
// store member), and this function only reads it -- not a contradiction of tact_state.h's W1 (W1 is
// about what is reachable ONLY through the view).
int32_t unit_death_tick(tact_store &own, const unit_death_tick_calls &c, int32_t unit_idx);

} // namespace detail

int32_t unit_death_tick(int32_t unit_idx);


} // namespace mh::tact
