//
// tact/tact_mission_end_finalize.h -- TACT1A: a proven no-op.
//
//   llm_tact_mission_end_finalize_stub @0x0044da59 (0x4b)
//
// NOT `dead` IN THE LEDGER'S SENSE -- it has one real caller (callers_total=1, reachable_from_root)
// -- so this is a REACHABLE function with a body that has no observable effect, a different thing
// from the ledger's `todo:dead`/state=dead (settled-UNREACHABLE, the migration-ledger schema). Marked
// `reviewed`: static proof exists, no execution proof is possible or meaningful (see below).
//
// THE WHOLE BODY, verified instruction by instruction: PUSH/MOV/CMP/INC/JMP only, no CALL besides
// the boilerplate stack probe, and every memory access is a READ. The loop is
//   for (i = 0; i < 0x40; ++i) { (void)*(int32_t*)(0xe15e60 + i*0x10); }   // @0x0044da7b-0x0044da98
// -- i.e. it reads `.unit_proto_id` (offset +0x0) of every _G_LLM_SQUAD_STATUS slot -- and the CMP
// at 0x0044da91 is followed by an UNCONDITIONAL JMP at 0x0044da98, not a Jcc: the comparison's flags
// are never consumed. No local, global or return value carries anything out of this loop.
//
// This is NOT the "0 direct, 0 transitive write cells" trap recorded elsewhere (that
// trap is about a REAL write escaping through a computed address the sweep cannot see -- see
// tact_squad_status.h). Here there is no write to escape: the function reads a real, live channel
// and provably discards every bit of it.
//
// WHY NO SHADOW SITE. A function with zero writes and a void return compares NOTHING under a shadow
// arm -- gen_dll_shadow.py's own VACUOUS bucket exists for exactly this shape, and arming it would
// only prove "both arms do nothing", not equivalence. The proof above is the exhaustive one: it
// covers all 100% of the body's instructions, which a rig run sampling live game states cannot
// claim to do any more completely.
//
#pragma once

namespace mh::tact {

void mission_end_finalize_stub();

}
