#include "hook/export.h"

#include "hook/detour.h"
#include "hook/promoted.h"

#include <cstdio>
#include <cstring>

namespace mh::hook {
namespace {
void (*g_log)(const char *) = nullptr;

void report(const char *name, const char *why) {
    if (!g_log) return;
    char line[256];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "; [export] %s NOT armed -- %s", name, why);
    g_log(line);
}
} // namespace

void set_export_logger(void (*fn)(const char *)) { g_log = fn; }

const char *export_result_str(export_result r) {
    switch (r) {
        case export_result::ok: return "ok";
        case export_result::bad_entry: return "entry bytes differ (wrong build, or already hooked)";
        case export_result::install_failed: return "install_jmp failed (VirtualProtect?)";
        case export_result::entry_owned: return "a DLL detour already owns this entry";
    }
    return "unknown";
}

export_result install_export(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8) {
    // ONE OWNER PER ENTRY (C4). Asked BEFORE the byte guard, because a detour-owned entry would fail
    // that guard too -- with "entry bytes differ (wrong build, or already hooked)", a wrong-build
    // message for a perfectly good build. The two situations need different words because they need
    // different actions: a byte mismatch means stop, a claimed entry means rebind.
    if (const char *owner = entry_owner_of(target)) {
        char why[224];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "entry is OWNED by %s. Promote by REBINDING what that detour falls through to, not "
                    "by installing a second entry patch (C4)",
                    owner);
        report(name, why);
        return export_result::entry_owned;
    }
    uint64_t seen = 0;
    std::memcpy(&seen, (const void *)target, sizeof(seen));
    if (seen != expect_entry8) {
        char why[160];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "entry bytes %016llX != expected %016llX (wrong build, or already hooked)",
                    (unsigned long long)seen, (unsigned long long)expect_entry8);
        report(name, why);
        return export_result::bad_entry;
    }
    // entry_claim::rebind (C9): this call IS the promotion. The registry it feeds must never be
    // allowed to refuse the code that populates it.
    // expect_prologue 0 (U30): install_export's guard is the per-function expected BYTES checked
    // just above, deliberately not WATCOM_PROLOGUE -- "plenty of worthwhile leaf targets have no
    // frame prologue at all" (hook/export.h). Handing the primitive a prologue expectation here would
    // refuse every one of them.
    if (!install_jmp(target, thunk, entry_claim::rebind, name, 0)) {
        report(name, export_result_str(export_result::install_failed));
        return export_result::install_failed;
    }
    // The whole body from here to the function's end is now dead code. Tell the interlock, so a byte
    // patch aimed inside it is refused rather than written into instructions nothing will reach (C1).
    note_promoted(target);
    return export_result::ok;
}

bool install_export_ok(uintptr_t target, void *thunk, const char *name, uint64_t expect_entry8) {
    return install_export(target, thunk, name, expect_entry8) == export_result::ok;
}

} // namespace mh::hook
