//
// lockstep/tx_emit_chat.h -- the CHAT pair + the flush primitive, reimplemented (RI-WIRE / W6-A).
//
// Scope: three rows of the nineteen-emitter inventory (the wire-emitter inventory) --
//
//   llm_net_chat_send_team   @0x0049d450  row 3  (MSG_CHAT (5), GUARDED, variable-length payload)
//   llm_net_chat_send_all    @0x0049d50b  row 4  (MSG_CHAT (5), GUARDED, variable-length payload)
//   llm_net_send_buf_flush   @0x0049d7ff  row 19 (NOT an emitter -- no tag, no payload; see below)
//
// TRANSLATED FROM tmp/decomp_wire/llm_net_chat_send_team_0049d450.asm,
// tmp/decomp_wire/llm_net_chat_send_all_0049d50b.asm and
// tmp/decomp_wire/llm_net_send_buf_flush_0049d7ff.asm -- NOT from the decompile beside each. Ghidra's
// C for the chat pair renders the incremental per-byte stack updates as more local variables/statements
// than instructions and, more importantly, silently loses the fact that the header's length BYTE is
// read as only the LOW 8 BITS of the 32-bit length argument while the copy count and the cursor
// advance both use the FULL 32-bit value -- the decompile prints `local_18 = (undefined1)param_2` for
// the byte store and `_G_LLM_NET_SEND_BUF_CURSOR + param_2` for the cursor advance as if they were
// unrelated, which reads as two different quantities; the disassembly shows they are the SAME stack
// slot read two different widths. See "THE LENGTH BYTE'S DOUBLE DUTY" below.
//
// THE ONE TEMPLATE, MOSTLY. Like tx_emit.h's five, the chat pair builds its record through
// mh::net::emit_ctrl (include/ctrl_emit.h) over the ONE shared buffer (include/packet_buffer.h). The
// one place this batch does NOT fit that template is `llm_net_send_buf_flush`, which is not an emitter
// at all -- see its own section below.
//
// THE CURSOR GUARD: BOTH chat functions are GUARDED (`reset_first = true`) -- they are two of only
// THREE guarded emitters in the whole inventory (the wire-emitter inventory "The cursor guard, and why it
// must not be normalised"), alongside `llm_net_send_lockstep_extend` (already in tx_emit.h). This is
// the LOSSY form: firing on a dirty cursor DISCARDS whatever order batch was pending rather than
// appending after it. Reproduced faithfully, not "fixed" -- see ctrl_emit.h's header comment for why a
// generic always-reset or never-reset `emit_ctrl` would be a silent wire-format change.
//
// THE LENGTH BYTE'S DOUBLE DUTY (do not "clean this up"). Both functions take `len` as a full 32-bit
// value (EDX at entry) and use it THREE different ways in three different widths:
//   1. truncated to its low 8 bits and written as the wire length-prefix byte (`MOV AL, byte ptr
//      [len]` -- an 8-bit load off a 32-bit stack slot, i.e. `len & 0xff`, NOT a saturating clamp);
//   2. used AT FULL WIDTH as the REP MOVSD/MOVSB copy count (so the actual bytes copied is `len`, not
//      the truncated byte);
//   3. used AT FULL WIDTH again to advance the cursor (`ADD [cursor], len`).
// For any `len <= 255` all three agree and nothing looks odd. For `len > 255` the wire's own length
// prefix UNDERSTATES how much text follows it -- a receiver trusting the prefix byte alone would
// mis-parse the record, while the sender still queues (and the receiver's dispatch loop still has to
// consume) the full `len` bytes. This is reproduced EXACTLY, not guarded against: see the single
// `std::memcpy(..., len)` in the .cpp, which moves the correct (full) byte count regardless of what
// the length-prefix byte says.
//
// THE INLINED memcpy'S SPLIT IS NOT THE "REMAINDER IS ALWAYS ZERO" CASE tx_emit.cpp DOCUMENTS FOR ITS
// FIVE FUNCTIONS. There, every payload is a fixed struct whose size is a multiple of 4, so the
// REP MOVSB tail always runs zero times and is a pure inlining artifact. HERE `len` is caller-supplied
// chat text and is NOT generally a multiple of 4, so `len & 3` (the REP MOVSB count -- `MOV CL, AL`
// reloads the length byte, `AND CL, 3` keeps only its low two bits, which equal `len`'s low two bits
// regardless of the earlier 8-bit truncation) is routinely NONZERO and carries genuine trailing text
// bytes. It is still not a SEPARATE field, though: REP MOVSD copying `len >> 2` dwords followed by
// REP MOVSB copying `len & 3` bytes moves exactly `len` contiguous bytes in total (the standard
// dword+remainder split of one copy), which is exactly what one `std::memcpy(dst, src, len)` does. So
// the translation below is still a single memcpy over the FULL `len` -- the correction here is only
// against reflexively repeating "the remainder is always zero" the way the sibling file could.
//
// MSG_CHAT'S SHAPE, AND WHY IT IS NOT AN "INNER TAG". The record on the wire is: outer tag `5`
// (MSG_CHAT), then a length byte, then a recipient byte, then `len` bytes of ANSI text. The length
// byte is NOT `has_inner` -- `has_inner` means specifically "outer == MSG_CONTROL (4), so a second tag
// byte follows selecting the CTL_* handler" everywhere else in this module, and MSG_CHAT's second byte
// is a length count, not a tag selecting a case. Modelled here as `has_inner = false` and a
// PRE-ASSEMBLED payload blob (`{len_byte, recipient_byte, text...}`) handed to emit_ctrl as one
// `payload`/`payload_len` -- see detail::chat_send below for why that blob is the one place in this
// whole RI-WIRE effort that needs a runtime-sized buffer instead of a fixed one.
//
// THE ONE-BYTE DIFFERENCE BETWEEN THE TWO CHAT FUNCTIONS. `llm_net_chat_send_team` and
// `llm_net_chat_send_all` are otherwise BYTE-IDENTICAL machine code (same prologue frame size, same
// instruction sequence, same guard, same copy, same send) -- they differ in exactly one instruction:
// the recipient byte is `MOV DL, byte ptr [0x00e5898b]` (a global read) in `_team` and a hardcoded
// `MOV byte ptr [...], 0xff` in `_all`. Factored through one shared `detail::chat_send(..., uint8_t
// recipient)` helper, the same way tx_emit.cpp factors `player_remove`/`_timeout` through
// `send_removal_record`/`apply_removal_and_branch`, so this one byte cannot drift between the two
// call sites.
//
// `llm_net_send_buf_flush` IS NOT AN EMITTER. It writes no tag and no payload -- it is exactly
// `transport_send(BUF, cursor); old = cursor; cursor = 0; return old;` (the wire-emitter inventory). It
// does NOT go through emit_ctrl (there is no record to build), and its only caller is
// `llm_strat_order_schedule` (already reimplemented in libmh/orders/order_queue.cpp, which currently binds
// its `send_buf_flush` call slot to the RAW `mh::call::llm_net_send_buf_flush` original-function
// wrapper -- see that file). This module's `detail::send_buf_flush` is a from-scratch translation of
// the ORIGINAL function's own body, offered for a future site that wants to promote it; nothing in
// this batch rewires order_queue.cpp's binding, and this task does not touch that file.
//
// THE STACK PROBE IS DROPPED, as already settled for every function in this module (see ctrl_emit.h's
// header and the reimpl-loop skill's "settled once" list): `assert_stack_capacity` (0x004cf46f) is
// Watcom's guard-page stack-touch helper, has no semantic effect, and touches zero tracked state.
//
// ARGUMENT CONVENTION (read off each .asm's own entry and RET): both chat functions are
// `void __watcall(void *text /*EAX*/, uint32_t len /*EDX*/)` -- plain `RET`, no stack cleanup, because
// both arguments arrive in registers. `llm_net_send_buf_flush` is `int32_t __watcall(void)` -- no
// arguments, plain `RET`, return value in EAX (the original's own comment already carries this
// prototype verbatim: "int __watcall llm_net_send_buf_flush(void)").
//
// NO PROMOTION/EXPORT WIRING IN THIS DRAFT, and this is a stated limitation, not an oversight:
//   * `llm_net_chat_send_team` and `llm_net_chat_send_all` have NO committed prototype in Ghidra yet --
//     `src/mh_dll/mh/addr/mh_export.gen.h` emits `MH_EXPORT_REPLACE_llm_net_chat_send_team(IMPL)` /
//     `_all` as `static_assert(false, "...MH_UNAVAILABLE__no_prototype_committed_in_Ghidra")`, i.e. a
//     build-time hard-stop. Nothing in this file attempts to call that macro. The generated call
//     wrapper (`mh::call::llm_net_chat_send_team`/`_all`) is equally unavailable, for the same reason.
//   * `llm_net_send_buf_flush` DOES have a working `MH_EXPORT_REPLACE_llm_net_send_buf_flush` macro
//     already generated, so promotion is mechanically possible for it alone -- but this task's brief
//     restricts the deliverable to exactly these two files, and wiring even one promotion seam means
//     touching the shadow manifest / regenerating the `.gen.h` headers, which are
//     out of scope here. Promoting one of three functions in this batch and not the other two would
//     also be a half-finished, inconsistent seam. Left for a follow-up once all three have gone
//     through reimpl-verify and a region set has been declared.
//
#pragma once
#include <cstdint>

#include "include/ctrl_emit.h"
#include "lockstep/turn_engine.h" // MSG_CHAT (outer_tag enum)

namespace mh::lockstep {

// Everything these three functions read or write, as typed pointers -- deliberately its own small
// struct rather than a reuse of tx_emit.h's `emit_state` (eleven fields, ten of which nothing here
// touches) or turn_engine.h's `engine_state`/`dispatch_state` (entirely unrelated to chat). Two fields
// is genuinely everything the chat pair and the flush primitive need between them.
struct emit_chat_state {
    mh::net::packet_buffer packet; // _G_LLM_NET_SEND_BUF (0x005d55cc) + _CURSOR (0x005d59c4) -- the
                                   // SAME object tx_emit.h's emit_state::packet and turn_engine.h's
                                   // dispatch_state::packet wrap; see include/packet_buffer.h.

    const uint8_t *chat_target_mask; // 0x00e5898b -- read ONLY by chat_send_team (0x0049d4ac: `MOV DL,
                                     // byte ptr [0x00e5898b]`); chat_send_all never touches it. Named
                                     // `_G_LLM_CHAT_TARGET_MASK` in docs/symbols.md (an existing
                                     // human/LLM label, not one this task invented) -- see the UNCERTAIN
                                     // SEMANTICS note in tx_emit_chat.cpp for why "the local team id"
                                     // (this task's framing) and "a chat-recipient selection mask"
                                     // (what the symbol's own name and its neighbours
                                     // `llm_ui_chat_target_add`/`_remove` at 0x0049d5c1/0x0049d5f8
                                     // suggest) are two different readings of the same byte that this
                                     // draft does NOT adjudicate between. ALSO NOTE: this address is
                                     // NOT YET in `mh_addrs.gen.h` / `dll_addr_manifest.json` (checked
                                     // 2026-07-29), so it is bound from a literal in the .cpp rather
                                     // than `mh::addr::_G_LLM_CHAT_TARGET_MASK` -- see that binding site
                                     // for the full explanation. This is the one place in this module
                                     // that breaks the "addresses come from mh_addrs.gen.h, never a
                                     // literal" rule, and it is a stated gap, not a choice.
};

// Outward calls. Just the one: every one of these three functions' only externally-observable effect
// (besides the shared buffer, which `packet` already owns) is handing bytes to the transport.
struct emit_chat_calls {
    mh::net::transport_send_fn transport_send; // llm_net_transport_send @0x0049ba8d -- a STUB in the
                                               // retail image; our DLL replaces it (see ctrl_emit.h).
};

emit_chat_state        chat_estate();
const emit_chat_calls &live_chat_calls();

// The hardcoded sentinel `llm_net_chat_send_all` writes in place of the team byte (0x0049d567: `MOV
// byte ptr [...], 0xff`). The wire-emitter inventory and the turn-engine notes both describe "all" as a
// RECEIVER-SIDE broadcast sentinel, not a per-peer fan-out -- neither chat function loops; exactly one
// record is built and sent per call, same as every other emitter in this inventory.
inline constexpr uint8_t CHAT_RECIPIENT_ALL = 0xff;

namespace detail {

// ---- shared helper: rows 3 & 4 differ by exactly this one parameter -------------------------------
//
// Builds `{len&0xff, recipient, text[0..len)}` as one contiguous blob and hands it to emit_ctrl as a
// single payload -- see the header comment above ("THE LENGTH BYTE'S DOUBLE DUTY") for why the first
// byte and the copy/cursor-advance use `len` at two different widths, and reproduces that exactly:
// the length PREFIX is `static_cast<uint8_t>(len)` (a truncating cast, matching the original's 8-bit
// load off the 32-bit argument) while `payload_len` -- what emit_ctrl actually copies and advances the
// cursor by -- is `2 + len` at FULL width, matching the original's `cursor += len` (0x0049d4df.. /
// 0x0049d595..) rather than the truncated byte.
//
// `text`/`len` mirror the original's own two register arguments (EAX, EDX) verbatim; `recipient` is
// NOT part of the original signature -- it is the parameter this helper introduces so the two
// call sites (`chat_send_team`, `chat_send_all`) can share this body without duplicating it.
void chat_send(const emit_chat_state &es, const emit_chat_calls &calls, const void *text, uint32_t len,
               uint8_t recipient);

// ---- 1. llm_net_chat_send_team @0x0049d450 (row 3) -------------------------------------------------
//
// void __watcall(void *text /*EAX*/, uint32_t len /*EDX*/) -- plain RET. MSG_CHAT (5), GUARDED
// (reset_first = true), recipient byte = *chat_target_mask (the global at 0x00e5898b). Wire = 3+len.
void chat_send_team(const emit_chat_state &es, const emit_chat_calls &calls, const void *text,
                    uint32_t len);

// ---- 2. llm_net_chat_send_all @0x0049d50b (row 4) --------------------------------------------------
//
// Identical shape to chat_send_team except the recipient byte is the hardcoded CHAT_RECIPIENT_ALL
// (0xff) sentinel rather than a global read. Wire = 3+len.
void chat_send_all(const emit_chat_state &es, const emit_chat_calls &calls, const void *text,
                   uint32_t len);

// ---- 3. llm_net_send_buf_flush @0x0049d7ff (row 19) ------------------------------------------------
//
// int32_t __watcall(void) -- plain RET, return in EAX. NOT an emitter: no tag, no payload, does not
// call emit_ctrl. `transport_send(bytes, queued()); const old = queued(); reset(); return old;` --
// unconditionally, even if queued() == 0 (there is no dirty-check guard here, unlike the chat pair).
int32_t send_buf_flush(const emit_chat_state &es, const emit_chat_calls &calls);

} // namespace detail

// ---- production entry points (bound to the live game state) ----------------------------------------
void    chat_send_team(const void *text, uint32_t len);
void    chat_send_all(const void *text, uint32_t len);
int32_t send_buf_flush();

} // namespace mh::lockstep
