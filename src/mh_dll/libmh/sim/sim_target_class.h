//
// sim/sim_target_class.h -- classifies a packed target reference as ground or air (RI-SIM / SIM1F).
//
//   llm_strat_target_class @0x004495f9 (0x64 B), `int __watcall
//   llm_strat_target_class(uint owner_and_kind_flag, int roster_slot)` (committed prototype,
//   addr/mh_export.gen.h's sig_llm_strat_target_class).
//
// Takes ONLY a `sim_view` -- no store, no callees at all beyond the inert stack-probe
// (utils_assert_stack_capacity, translator brief rule 6), same posture as
// sim_bldg_has_aa_weapon.h/sim_bldg_roster_queries.h's siblings: a pure roster-read predicate.
//
// ---- THE ASSEMBLY (0x004495f9-0x0044965c) -------------------------------------------------------
//   `MOV [ebp-0x18],1` -- default result is GROUND (1). owner_and_kind_flag arrives in EAX,
//   roster_slot in EDX (per the committed .asm header, __watcall EAX/EDX -- trusted over the .c
//   draft's plain-int signature per house rule 5).
//
//   `TEST owner_and_kind_flag,0xa0; JZ <return default>` (0x0044961d-0x00449624) -- if NEITHER of
//   bits 0x80/0x20 is set, skip straight to returning GROUND without ever reading `units`. Per the
//   original plate comment, 0x80/0x20 are the two "this ref is a UNIT" bits in the
//   map::units[].+0x8a/+0x8c roster-reference convention (as opposed to 0x40, "this ref is a
//   BUILDING", which this function never tests at all -- a building ref falls straight through to
//   the default GROUND return, matching the plate's "covers buildings ... without even reading
//   map::units").
//
//   `AND owner_and_kind_flag,0xf` then the roster address arithmetic (0x00449626-0x00449639:
//   `IMUL owner,0x5b04` [UNITS_PER_PLAYER row stride] `+ IMUL roster_slot,0xe9` [sizeof(unit)]) --
//   exactly `unit_of(v, owner_and_kind_flag & 0xf, roster_slot)`. The low nibble is the owning
//   player, matching sim_state.h's own `ref_owner()`/REF_OWNER_MASK helper (used verbatim here per
//   house rule 17a rather than re-deriving `& 0xf` locally).
//
//   `CMP units[...].elevation,0x0; JZ <return default>` (0x0044963b-0x00449642) -- if elevation ==
//   0, GROUND stays. Only when BOTH the kind-bit test passed AND elevation != 0 does the result
//   become AIR (2) (0x00449644-0x00449651). This is a single `&&`, not two independent early-outs:
//   both guards, taken, land at the same LAB_0044964b return with the default GROUND value intact.
//
// The two literal results (1/2) are not local magic numbers -- they are sim_state.h's own,
// already-documented WEAPON_TARGET_GROUND/WEAPON_TARGET_AIR constants, and that comment explicitly
// names this function as the scheme's origin ("cfg_weapon::target is a bitmask ... per
// llm_strat_target_class's ground=1 / air=2 scheme"). Used verbatim per house rule 17a.
//
// No floats, no writes, no calls beyond the inert stack probe (translator brief rule 6, not
// reproduced).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_target_class @0x004495f9. GROUND (WEAPON_TARGET_GROUND, 1) unless the reference is a
// UNIT (owner_and_kind_flag & 0xa0 != 0) whose roster entry has nonzero elevation, in which case
// AIR (WEAPON_TARGET_AIR, 2). See the header banner for the full derivation.
int32_t target_class(const sim_view &v, uint32_t owner_and_kind_flag, int32_t roster_slot);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_target_class) exactly.
int32_t target_class(uint32_t owner_and_kind_flag, int32_t roster_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
