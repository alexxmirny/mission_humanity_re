#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

#include "hook/detour.h"

#include "hook/promoted.h"

namespace mh::hook {

namespace {

// C9, generalised by U30. Same shape, same reason, and deliberately the same voice as
// patch_bytes_guarded's interlock in patch.cpp: the ONLY place that reliably holds the resolved
// target is the primitive itself, so that is where the question gets asked. Several detour
// installers are table-driven (shadow's site table, video's re-entrancy group,
// net_diag's ini-supplied trace VAs) and have no static address for a lint to read, which is exactly
// why a static check alone cannot close this.
//
// THREE reasons, THREE messages, no conflation -- because they demand three different actions:
// a promoted body means carry the fix in our body; an owned entry means hand it over or rebind; a
// byte mismatch means stop and check the build. Merging any two of them IS G68.
//
// Every refusal is also FILED (note_entry_refusal), because a line at the moment of refusal is not
// enough on its own: U30 is a default-ON fix that printed exactly such a line, 61 ms after the gate
// that beat it, into a 2 MB log, in every shipped run. report_entry_refusals() states them together
// at the end of arming.
bool refused(uintptr_t target, entry_claim claim, uint32_t expect_prologue, const char *how,
             const char *who, bool body_dies = false) {
    const refuse_reason why = detour_refusal(target, claim, expect_prologue, body_dies);
    if (why == refuse_reason::none) return false;
    note_entry_refusal(target, who, why);

    const char *what = who ? who : "an unnamed DLL detour";
    char        line[400];
    switch (why) {
        case refuse_reason::promoted:
            // WORDING UNCHANGED from C9 -- the ledger quotes it, and a reader who has seen
            // it once should recognise it verbatim.
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [interlock] %s at %08X DISPLACED -- that entry is inside %s, which is "
                        "PROMOTED in this run, so the detour was NOT installed and whatever fix it "
                        "carried IS NOT IN THIS RUN. Carry it in our body, or promote nothing here. "
                        "(This is NOT a wrong-build 'unexpected prologue' -- it is a "
                        "DISPLACED FIX.)\n",
                        how, (unsigned)target, promoted_owner_of(target));
            break;
        case refuse_reason::entry_owned:
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [interlock] %s at %08X REFUSED -- %s wanted this entry EXCLUSIVELY and %s "
                        "already owns it. ONE ENTRY, ONE OWNER (hook/promoted.h C4), so whatever fix "
                        "the loser carried IS NOT IN THIS RUN. Rebind what the owner falls through "
                        "to, or claim entry_claim::rebind if sharing it is the design. This is NOT a "
                        "wrong-build 'unexpected prologue' -- it is a DISPLACED FIX.\n",
                        how, (unsigned)target, what, entry_owner_of(target));
            break;
        case refuse_reason::patched: {
            // F4C-COMP. Says which MANIFEST and which FUNCTION, because the action is a compile-list
            // edit or a dropped detour -- neither of which a reader can take from an address alone.
            // It also says WHICH HALF of the body is contested, since the two mean different fixes:
            // a whole-body jmp can sometimes be re-expressed as a trampoline past the sites, while a
            // contested entry window cannot be moved at all.
            const char *fn = "an unnamed body";
            const char *mf = body_dies ? patched_body_of(target, &fn) : patched_entry_window_of(target, &fn);
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [interlock] %s at %08X REFUSED -- the in-memory static patch '%s' already "
                        "holds bytes inside %s, and this install would %s. ONE MECHANISM PER BODY "
                        "(hook/promoted.h F4C-COMP): whatever fix %s carried IS NOT IN THIS RUN. Drop "
                        "the manifest from the compile list, or drop this hook. This is NOT a "
                        "wrong-build 'unexpected prologue' -- it is a DISPLACED FIX.\n",
                        how, (unsigned)target, mf ? mf : "an unnamed manifest", fn,
                        body_dies ? "make every byte of it dead" : "overwrite the very bytes it patched",
                        what);
            break;
        }
        default:
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [interlock] %s at %08X REFUSED -- entry bytes %08X are not the %08X this "
                        "site was generated against, and NOTHING in this run claims that entry: a "
                        "wrong mh.exe build, or a hook nobody registered. %s is NOT armed.\n",
                        how, (unsigned)target, *(const uint32_t *)target, expect_prologue, what);
            break;
    }
    promotion_log(line);
    return true;
}

// C9(c). An EXCLUSIVE install now records the claim, so a later promotion of the same function is
// refused BY NAME instead of by a byte compare that can only say "wrong build, or already hooked".
// A `rebind` install records nothing: it is either a promoter (note_promoted is its record) or a
// C4/C6 participant, where sharing the entry is the design rather than a conflict.
//
// Only on SUCCESS, and only from the two primitives, so the registry describes entries that were
// actually taken. The default name is deliberately unhelpful-but-honest: an unnamed claim still turns
// a wrong-build message into an ownership one, which is most of the value, and it reads as a prompt
// to name the site properly.
void claim_entry(uintptr_t target, entry_claim claim, const char *who) {
    // SIM1-P clause 6: a ::rebind claim is now RECORDED (flagged shared) instead of discarded. It used
    // to early-return, and that lost the only fact harness.cpp needed to derive which rows must yield
    // their libmh rebind -- so the answer was hand-kept as kHarnessOwned[], two string literals plus a
    // comment asking future authors to remember. Behaviour of detour_refusal is unchanged: it asks
    // entry_owner_of, which still answers only for exclusive claims.
    note_entry_claim(target, who ? who : "an unnamed DLL detour", claim != entry_claim::exclusive);
}

// ---- U31: the page ops, and the two failures that used to be silent -------------------------------
//
// Production leaves g_ops null and gets the real Win32 calls; interlock_selftest installs a table so
// the failure arms -- which no run in this tree has ever taken -- can be driven. See detour.h.
const page_ops *g_ops = nullptr;

void *alloc_thunk(unsigned int bytes) {
    if (g_ops) return g_ops->alloc(bytes);
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

void release_thunk(void *p) {
    if (g_ops) {
        g_ops->release(p);
        return;
    }
    VirtualFree(p, 0, MEM_RELEASE);
}

bool protect_rwx(void *p, unsigned int bytes, unsigned long want, unsigned long *old_out) {
    if (g_ops) return g_ops->protect(p, bytes, want, old_out);
    return VirtualProtect(p, bytes, want, reinterpret_cast<DWORD *>(old_out)) != 0;
}

// The counterpart of refused() for the failures that happen AFTER the adjudication says yes. Same
// discipline for the same reason: file it so the end-of-arming summary enumerates it, and say what
// state the entry was left in -- which for both of these is "untouched", and that is the fact a
// reader needs, because an idempotent installer will otherwise never retry.
void note_install_failure(uintptr_t target, const char *who, const char *how, refuse_reason why) {
    note_entry_refusal(target, who, why);
    char line[400];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; [interlock] %s at %08X REFUSED -- %s. This is the OS declining, not a collision "
                "and not a wrong build: NOTHING was patched, the entry is untouched and %s is NOT "
                "armed.\n",
                how, (unsigned)target,
                why == refuse_reason::alloc_failed
                    ? "VirtualAlloc could not obtain the 64-byte executable thunk"
                    : "VirtualProtect would not make the entry writable",
                who ? who : "an unnamed DLL detour");
    promotion_log(line);
}

} // namespace

void set_page_ops(const page_ops *ops) { g_ops = ops; }

bool detour_would_be_displaced(uintptr_t target, entry_claim claim) {
    return claim != entry_claim::rebind && promoted_owner_of(target) != nullptr;
}

refuse_reason detour_refusal(uintptr_t target, entry_claim claim, uint32_t expect_prologue,
                             bool body_dies) {
    if (claim != entry_claim::rebind) {
        if (promoted_owner_of(target)) return refuse_reason::promoted;
        if (entry_owner_of(target)) return refuse_reason::entry_owned;
    }
    // F4C-COMP / Q6, and NOT gated on `claim` -- deliberately. `entry_claim::rebind` means "sharing
    // the ENTRY is the design", which is a statement about the other DLL detour at that address; it
    // says nothing about bytes an in-memory manifest already wrote further into the body. A
    // promotion is exactly such a rebind claim (install_export passes ::rebind), and a promotion is
    // the single most destructive thing that can happen to a static patch, so reading the claim here
    // would exempt the one case that most needs refusing.
    if (body_dies) {
        if (patched_body_of(target)) return refuse_reason::patched;
    } else if (patched_entry_window_of(target)) {
        return refuse_reason::patched;
    }
    // LAST, and only if the caller declared an expectation. Note the ordering also keeps this the
    // only dereference in the function, so a caller with no byte expectation can ask about an
    // address it has not proved is mapped.
    if (expect_prologue && *(const uint32_t *)target != expect_prologue) return refuse_reason::prologue;
    return refuse_reason::none;
}

bool report_if_displaced(uintptr_t target, const char *what) {
    const char *owner = promoted_owner_of(target);
    if (!owner) return false;
    char line[288];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; %s NOT armed -- DISPLACED: its target %08X is inside %s, which is PROMOTED in this "
                "run. This is NOT a wrong-build prologue mismatch. THE FIX IS NOT IN THIS RUN unless "
                "our body carries it.\n",
                what, (unsigned)target, owner);
    promotion_log(line);
    return true;
}

bool install_jmp(uintptr_t target, const void *dest, entry_claim claim, const char *who,
                 uint32_t expect_prologue) {
    // `body_dies = true`: the entry becomes an unconditional E9, so every byte after it is dead.
    if (refused(target, claim, expect_prologue, "install_jmp", who, /*body_dies=*/true)) return false;
    uint8_t      *t   = reinterpret_cast<uint8_t *>(target);
    unsigned long old = 0;
    if (!protect_rwx(t, 8, PAGE_EXECUTE_READWRITE, &old)) {
        note_install_failure(target, who, "install_jmp", refuse_reason::protect_failed);
        return false;
    }
    t[0]                                = 0xE9;
    *reinterpret_cast<int32_t *>(t + 1) = static_cast<int32_t>((uintptr_t)dest - (target + 5));
    t[5]                                = 0x90;
    t[6]                                = 0x90;
    t[7]                                = 0x90; // nop the orphaned imm tail (never executed)
    protect_rwx(t, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 8);
    claim_entry(target, claim, who);
    return true;
}

bool install_trampoline(uintptr_t target, void *detour, void **tramp_out, int stolen,
                        entry_claim claim, const char *who, uint32_t expect_prologue) {
    if (refused(target, claim, expect_prologue, "install_trampoline", who)) return false;
    uint8_t *t  = reinterpret_cast<uint8_t *>(target);
    uint8_t *tr = static_cast<uint8_t *>(alloc_thunk(64));
    if (!tr) {
        note_install_failure(target, who, "install_trampoline", refuse_reason::alloc_failed);
        return false;
    }
    for (int i = 0; i < stolen; ++i) tr[i] = t[i];
    tr[stolen] = 0xE9;
    *reinterpret_cast<int32_t *>(tr + stolen + 1) =
        static_cast<int32_t>((target + stolen) - (uintptr_t)(tr + stolen + 5));

    // U31: THE THUNK IS NOT PUBLISHED YET, and that ordering is the whole fix. *tramp_out used to be
    // written here, before the entry was owned -- so a failed VirtualProtect returned false with the
    // caller holding a live, executable thunk whose stolen bytes copy a prologue NOBODY PATCHED.
    // Calling through it runs the original prologue twice, and launch.cpp's installers are idempotent
    // by testing that same pointer (`if (g_tramp) return;`), so a failed install read as a completed
    // one and was never retried. Publish LAST, roll back on failure.
    unsigned long old = 0;
    if (!protect_rwx(t, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        release_thunk(tr);
        note_install_failure(target, who, "install_trampoline", refuse_reason::protect_failed);
        return false;
    }
    t[0]                                = 0xE9;
    *reinterpret_cast<int32_t *>(t + 1) = static_cast<int32_t>((uintptr_t)detour - (target + 5));
    for (int i = 5; i < stolen; ++i) t[i] = 0x90;
    protect_rwx(t, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, stolen);
    *tramp_out = tr;
    claim_entry(target, claim, who);
    return true;
}

} // namespace mh::hook
