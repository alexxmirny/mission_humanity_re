#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hook/patch.h"

#include "hook/promoted.h"

namespace mh::hook {

bool patch_bytes_guarded(uintptr_t target, const uint8_t *expect, const uint8_t *repl, int len) {
    // THE INTERLOCK (C1), and it lives here rather than at the call sites on purpose. Several patch
    // sites -- video's view-array relocation, the mode-2 immediates -- DISCOVER their addresses by
    // scanning .text at runtime, so there is no static list of them to annotate. Only the place that
    // already holds the resolved address can cover every writer without exception.
    if (const char *owner = promoted_owner_of(target)) {
        char line[224];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; [interlock] patch at %08X SUPPRESSED -- it is inside %s, which is PROMOTED in this "
                    "run. Those bytes never execute; the fix must be carried by our implementation.\n",
                    (unsigned)target, owner);
        promotion_log(line);
        return false;
    }
    uint8_t *t = reinterpret_cast<uint8_t *>(target);
    if (memcmp(t, expect, len) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(t, repl, len);
    VirtualProtect(t, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, len);
    return true;
}

} // namespace mh::hook
