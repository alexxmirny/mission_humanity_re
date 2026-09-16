#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "en_guard.h"
#include "addr/mh_addrs.gen.h"

namespace {

// Three well-separated EN function entries; each carries the Watcom `push ebp; mov ebp, esp`
// prologue on the EN image and unrelated mid-function bytes on any other build (same probe
// set the retired mh_port used, EN side).
constexpr uintptr_t PROBES[] = {
    mh::addr::llm_net_send_packet,
    mh::addr::llm_net_poll_recv,
    mh::addr::llm_lobby_join_head,
};

// SEH leaf (no C++ unwinding in this function): read 3 bytes, tolerate an unmapped VA.
bool has_prologue(uintptr_t va) {
    __try {
        const uint8_t *p = (const uint8_t *)va;
        return p[0] == 0x55 && p[1] == 0x89 && p[2] == 0xE5;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool probe_all() {
    for (uintptr_t va : PROBES)
        if (!has_prologue(va)) return false;
    return true;
}

} // namespace

namespace mh {

bool en_build_ok() {
    static const bool ok = probe_all();
    return ok;
}

} // namespace mh
