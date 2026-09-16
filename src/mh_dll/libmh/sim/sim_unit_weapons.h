//
// sim/sim_unit_weapons.h -- the three unit-weapon queries (RI-SIM / SIM0 pilot).
//
//   llm_strat_unit_has_aa_weapon     @0x004d7311 (0x66)  batch E layer 0, 5 callers
//   llm_strat_unit_has_ground_weapon @0x004d7377 (0x66)  batch E layer 0, 4 callers
//   llm_strat_unit_max_weapon_range  @0x004d480f (0x79)  batch E layer 0, 9 callers
//
// Grouped in one TU because they are three reads of the same two tables -- but they are NOT three
// spellings of one function, and the pilot picked them partly to prove that the interface makes the
// difference visible rather than hiding it:
//
//   THE TWO PREDICATES READ THE UNIT INSTANCE'S OWN WEAPON SLOTS (`unit::weapons[4]`, stride 0x13,
//   fields weapon_id / enabled_2) -- the four weapons this particular unit is carrying right now.
//
//   max_weapon_range READS THE CFG TYPE RECORD'S SLOTS (`cfg_unit::weapons`, four (id, enabled)
//   BYTE PAIRS, via cfg_unit_weapon_id/_enabled) -- the four weapons units of this TYPE mount. It
//   never looks at the instance at all beyond `unit_proto_id`.
//
// Same count, adjacent addresses, near-identical decompiled shapes, different arrays. A translation
// that unified them would compile, pass a shallow test, and be wrong for any unit whose instance
// loadout has diverged from its type's.
//
// THE LOOP BOUNDS ALSO DIFFER, AND BOTH ARE REPRODUCED: the two predicates run `INC EDX / CMP
// EDX,4 / JL` -- a SIGNED compare (0x004d736c, 0x004d73d2) -- while max_weapon_range runs
// `CMP EDX,4 / JC`, UNSIGNED (0x004d487c). Over 0..3 the two are identical; they are written as
// they are because the listing is, not because anything depends on it.
//
// All three are pure reads -- `sim_view` only, no `sim_store`. NOT ARMED under SIM0 (see
// sim_bldg_alive.h); the shadow sites belong to SIM1E.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_unit_has_aa_weapon @0x004d7311 / llm_strat_unit_has_ground_weapon @0x004d7377.
//
// 1 if any of the unit INSTANCE's four weapon slots is enabled, carries a nonzero weapon_id, and
// that weapon's cfg `target` mask has the requested bit; 0 otherwise. The two originals differ in
// exactly one immediate -- TEST ...,0x2 at 0x004d7359 vs TEST ...,0x1 at 0x004d73bf -- so they are
// one helper here plus two wrappers, which is a merge of two identical bodies and not of two
// different tables (contrast max_weapon_range above).
//
// `unit_ref`'s LOW NIBBLE is the owning player; the high nibble carries caller-packed bits and is
// masked off before indexing (AND ECX,0xf at 0x004d7322 / 0x004d7388).
int32_t unit_has_weapon_vs(const sim_view &v, uint32_t unit_ref, int32_t unit_index,
                           uint8_t target_mask);

inline int32_t unit_has_aa_weapon(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    return unit_has_weapon_vs(v, unit_ref, unit_index, WEAPON_TARGET_AIR);
}
inline int32_t unit_has_ground_weapon(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    return unit_has_weapon_vs(v, unit_ref, unit_index, WEAPON_TARGET_GROUND);
}

// llm_strat_unit_max_weapon_range @0x004d480f.
//
// The largest `Weapon[id].range_max[player]` over the unit TYPE's four enabled weapon slots, 0 if
// none is enabled. Two things to keep:
//
//   THE RANGE TABLE IS PER-PLAYER. range_max is int32_t[9] and the index is the SAME masked player
//   nibble the roster lookup used (SHL EBX,2 off `unit_ref & 0xf` at 0x004d4863-0x004d486b), so two
//   players can get different answers for the same weapon.
//
//   THE MAXIMUM IS TAKEN WITH AN UNSIGNED COMPARE (CMP ECX,.. / JNC at 0x004d486d) over values the
//   table declares as int32_t. A negative range would therefore read as enormous. Nothing in the
//   shipped cfg is negative; the compare is reproduced as unsigned because the original's is, and
//   the return type is uint32_t for the same reason.
uint32_t unit_max_weapon_range(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

} // namespace detail

// Live wrappers: the logic applied to state().read. Signatures match the originals' __watcall shape.
int32_t  unit_has_aa_weapon(uint32_t unit_ref, int32_t unit_index);
int32_t  unit_has_ground_weapon(uint32_t unit_ref, int32_t unit_index);
uint32_t unit_max_weapon_range(uint32_t unit_ref, int32_t unit_index);


} // namespace mh::sim
