//
// hook/detour.h -- x86 inline-detour primitives for patching mh.exe (32-bit Watcom).
//
// The injected DLL redirects game functions by overwriting their entry with a 5-byte `E9 rel32`
// jump. Two shapes:
//   * install_jmp        -- whole-body replace: the original never runs (used for the dead net
//                           stubs we fully substitute).
//   * install_trampoline -- steal the prologue into an executable thunk that jumps back to
//                           target+stolen, so the detour can run the original as a subroutine or
//                           run-before-and-continue.
//
// mh.exe has a fixed image base (0x00400000, no ASLR), so raw VAs are stable. Every hookable
// function opens with the Watcom frame prologue (WATCOM_PROLOGUE), and THE PRIMITIVES CHECK IT
// THEMSELVES (`expect_prologue`, U30) -- so a wrong build / already-hooked entry is a safe no-op,
// not corruption, and the primitive is the one that gets to say which of those it was.
//
// ---- U30: why the byte compare lives IN HERE and not at the call site ---------------------------
//
// It used to live at the call site, ~20 times, always in this shape:
//
//     if (*(const uint32_t *)ADDR == PROLOGUE && install_trampoline(ADDR, ...))
//
// and the `&&` is the bug. It SHORT-CIRCUITS PAST the primitive's own guards, so on a contested
// entry install_trampoline is never reached, the C1/C4/C9 registries are never asked, and the
// else-branch prints the only cause its author had in mind -- "NOT armed (unexpected prologue)",
// which blames the build. C9 already knew this (see report_if_displaced below, which exists purely
// to give such an else-branch a second chance to ask) and still left the compare outside; that
// half-measure was taken at ONE site out of twenty, and the nineteenth cost a default-ON endgame
// fix its entire shipped life (U30: `[net] sync_gameover` defaults to 1 and has
// never once armed, because an effects gate takes llm_ui_outcome_dialog's entry 61 ms earlier).
//
// So the compare moved in. A caller now writes `if (install_trampoline(ADDR, ..., who))` and cannot
// skip the adjudication, because there is no longer an expression in front of it. Every refusal --
// promoted body, owned entry, byte mismatch -- goes through ONE decision (detour_refusal) that
// names its reason and files it in the registry report_entry_refusals() enumerates at the end of
// arming. `expect_prologue == 0` is the opt-out for the targets that legitimately do not open with
// a Watcom frame (llm_rand, utils_abort) or that already checked their exact 8 entry bytes against
// a generated constant, which is a STRICTLY STRONGER guard than this one.
//
#pragma once
#include "hook/promoted.h" // refuse_reason -- the refusal vocabulary and its registry

#include <cstdint>

namespace mh::hook {

// `push ebp; mov ebp, esp; push imm` = 55 89 e5 68, little-endian dword. The stealable 8-byte
// Watcom prologue every mh.exe function this DLL hooks begins with; the first 5 hold no rel
// operand, so E9-splicing at the entry is instruction-boundary safe.
inline constexpr uint32_t WATCOM_PROLOGUE = 0x68e58955u;

// ---- C9: what this detour means to do with an entry that is already PROMOTED --------------------
//
// The prologue guard every caller writes -- `*(uint32_t *)ADDR == WATCOM_PROLOGUE` -- answers "are
// these the bytes I expected". A promoted entry fails it, correctly, and for a completely different
// reason than a wrong build does; the compare cannot tell the two apart, so the caller's else-branch
// names the only cause its author had in mind ("already hooked?") and a displaced fix reads as a
// version mismatch. That is how the D14 resync-order clamp shipped inert for weeks (see
// D17).
//
// So the primitive asks the promotion registry itself, the way patch_bytes_guarded already does --
// and, like it, refuses LOUDLY rather than writing bytes nobody will execute. The claim below is how
// a caller says which of the two legitimate intents it has. It is a required argument at the sites
// that need `rebind` and defaulted everywhere else, so the safe answer is the one you get by not
// thinking about it.
enum class entry_claim {
    // The detour expects to OWN this entry. If the entry is inside a promoted body, that is a
    // collision: refuse, and say so by name.
    exclusive,
    // The detour legitimately shares an entry with a promotion, under the C4/C6 protocol: the detour
    // keeps the entry and the promotion REBINDS what it falls through to (llm_strat_time_tick, and
    // sim_tick/sim_step via MH_Harness_RebindSimStep). Also what a PROMOTER itself passes -- the code
    // installing our body must never be refused by the registry that records it.
    rebind,
};

// Explain an install refusal in the interlock's own vocabulary, for callers that PROLOGUE-CHECK
// FIRST. Guarding the primitive is not enough for them and this was measured, not predicted: the
// resync-order clamp reads `if (*(uint32_t *)ADDR == PROLOGUE && install_trampoline(...))`, and a
// promoted entry fails the compare, so the `&&` short-circuits and the installer -- with its guard --
// is never reached. The refusal still printed "unexpected prologue ... already hooked?" on a run of
// the very build that fixed the primitive.
//
// So the else-branch has to ask too. Returns true if the target is inside a PROMOTED body, having
// written the DISPLACED line; false when the mismatch is a genuine wrong-build/unknown one, and the
// caller's own message is then the correct thing to print.
//
//     if (*(const uint32_t *)ADDR == PROLOGUE && install_trampoline(ADDR, ...))
//         seam_log("; <what> armed\n");
//     else if (!mh::hook::report_if_displaced(ADDR, "<what>"))
//         seam_log("; <what> NOT armed (unexpected prologue)\n");
//
bool report_if_displaced(uintptr_t target, const char *what);

// ---- C9(c): who owns this entry -----------------------------------------------------------------
//
// The mirror of the refusal above, and the half that was missing until now. `promoted_owner_of` stops
// a detour landing on a body we promoted; `entry_owner_of` is what stops a PROMOTION landing on an
// entry a detour already holds -- and it could only ever answer for entries somebody had registered,
// which in practice was ONE (time_tick's, by an explicit call). Everywhere else the promotion lost
// the race silently and `install_export` reported "entry bytes ... (wrong build, or already hooked)":
// G68 again, roles swapped. Measured instances: the order recorder vs [promote] sim_dispatch, and
// replay_suppress_enqueue vs [promote] orders, the latter turning a run into "8/9 seams installed --
// PARTIAL, treat this run as invalid".
//
// So an `exclusive` install now claims its entry as a side effect of succeeding. `who` is the name
// that refusal will print; pass something a reader can act on ("order_record dispatch detour", not
// "detour"). A `rebind` install claims NOTHING -- it is either a PROMOTER (whose own note_promoted is
// the right record) or a participant in the C4/C6 protocol, where sharing the entry is the design.
//
// Registration happens only on SUCCESS: an entry nobody actually took must not be reported as owned.

// The decision the two installers make, WITHOUT the write. Exposed for the same reason C1's
// ownership decision is: both halves of this interlock are about something NOT happening, and a green
// run of the real installer looks exactly like a run that never reached the check. Testing the
// predicate is testing the part that can be wrong -- interlock_selftest.cpp drives it both ways.
// True = `install_jmp`/`install_trampoline` will refuse this target under this claim.
//
// PROMOTION ONLY -- this is C9's original question and it keeps its original answer. The full
// decision (which also asks who owns the entry, and what bytes are there) is detour_refusal below;
// this stays as the narrow predicate its existing callers and tests mean by it.
bool detour_would_be_displaced(uintptr_t target, entry_claim claim);

// THE WHOLE refusal decision, WITHOUT the write -- what both primitives ask before touching a byte.
// Asked in this order, and the order is the point: a promoted body, an owned entry and a wrong build
// are three different situations needing three different actions, and whichever is asked first is
// the one that gets to explain itself. Bytes go LAST because a contested entry fails the byte
// compare too, and letting it answer first is exactly how G68 happened.
//
//   (i)   the target is inside a body PROMOTED in this run   -> refuse_reason::promoted
//   (ii)  another detour already OWNS this entry (C4)        -> refuse_reason::entry_owned
//   (iii) the entry bytes are not `expect_prologue`          -> refuse_reason::prologue
//   otherwise                                                -> refuse_reason::none (install away)
//
// `entry_claim::rebind` is exempt from (i) and (ii) exactly as it always was -- it is either a
// PROMOTER (the code that populates the registry must not be refused by it) or a C4/C6 participant,
// for which sharing an entry is the design. It is NOT exempt from (iii): whoever you are, the bytes
// are the bytes.
//
// `expect_prologue == 0` means NO BYTE EXPECTATION, and only then is `target` not dereferenced.
// `body_dies` (F4C-COMP) says what this install does to the ORIGINAL BODY, which is the only thing
// that decides how much of it an in-memory static patch may still own: a JMP install (and so every
// promotion) redirects the entry and kills the whole body, while a TRAMPOLINE install steals only
// its first `stolen` bytes and the body runs on. The default is the trampoline answer -- the
// conservative one for the advisory callers (hookpoint's `available`, the pre-flight checks in
// mp_menu / net_diag / video), all of which install trampolines.
refuse_reason detour_refusal(uintptr_t target, entry_claim claim, uint32_t expect_prologue,
                             bool body_dies = false);

// Whole-body replace: write `E9 rel32` to `dest` over the first 5 bytes of `target` and NOP the
// next 3 (the orphaned prologue tail, never executed). The original body is unreachable after
// this. Returns false -- having LOGGED and REGISTERED the reason -- on any refusal detour_refusal
// names, AND (U31) if VirtualProtect declines the entry: that path used to return false in silence.
//
// `who` is what the refusal and the end-of-arming summary will print: pass something a reader can
// act on ("the D15 exit witness", not "detour"). `expect_prologue` is the byte guard the caller used
// to write itself; pass 0 for a target that does not open with a Watcom frame, or whose exact 8
// entry bytes the caller already checked against a generated constant.
bool install_jmp(uintptr_t target, const void *dest, entry_claim claim = entry_claim::exclusive,
                 const char *who = nullptr, uint32_t expect_prologue = WATCOM_PROLOGUE);

// Trampoline hook: allocate an executable thunk holding `stolen` copied prologue bytes followed
// by `E9 rel32` back to target+stolen, then overwrite the entry with `E9 rel32` to `detour` and
// NOP-pad to `stolen`. The thunk address is stored in *tramp_out -- a run-before detour ends with
// `jmp [tramp]` (original runs after), a wrap detour does `call [tramp]` (original runs mid-body).
// `stolen` must be >= 5 and land on an instruction boundary (8 for the Watcom prologue). Returns
// false -- having LOGGED and REGISTERED the reason -- on any refusal detour_refusal names, and (U31)
// on VirtualAlloc / VirtualProtect failure, which used to return false in silence.
//
// U31: IT FAILS ATOMICALLY. `*tramp_out` is published LAST, only once the entry is actually patched,
// and a thunk allocated before a failed VirtualProtect is freed. It used to publish the pointer
// before it owned the entry, so a failed install left the caller holding a live thunk over an
// UNPATCHED prologue -- and launch.cpp's installers are idempotent by testing that very pointer
// (`if (g_tramp) return;`), so they would neither retry nor notice.
bool install_trampoline(uintptr_t target, void *detour, void **tramp_out, int stolen,
                        entry_claim claim = entry_claim::exclusive, const char *who = nullptr,
                        uint32_t expect_prologue = WATCOM_PROLOGUE);

// ---- U31: the page operations, injectable ONLY so the failure arms can be driven off-rig ---------
//
// The two paths this seam exists for are paths NO RUN IN THIS TREE HAS EVER TAKEN: mh.exe's sections
// are writable to us in every environment tested, and no VirtualAlloc of 64 bytes has ever failed.
// That is precisely why they need a test -- an untaken path is where a fix that "obviously works"
// goes wrong, and U31's whole claim is about what the primitives leave behind WHEN they fail. There
// is no way to make the real VirtualProtect decline a live game entry on demand, so the calls go
// through here and the test supplies its own.
//
// Production never sets this: a null table means the real Win32 calls, so the shipped path has no
// behaviour to get wrong. The test also gets to point `target` at its own buffer, which is how the
// SUCCESS arm can be asserted at all -- the primitives write E9 bytes, and until now that made them
// untestable off-rig (interlock_selftest deliberately called neither).
struct page_ops {
    void *(*alloc)(unsigned int bytes);                                  // VirtualAlloc RWX
    void (*release)(void *p);                                            // VirtualFree
    bool (*protect)(void *p, unsigned int bytes, unsigned long want_rwx, // VirtualProtect
                    unsigned long *old_out);
};

// Install a page-op table, or nullptr to restore the real Win32 calls. Not thread-safe and not meant
// to be: installs happen once, on the injection thread, before anything else runs.
void set_page_ops(const page_ops *ops);

} // namespace mh::hook
