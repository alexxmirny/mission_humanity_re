//
// sim/libtrans/sim_lt_available_projects_clear.cpp -- see sim_lt_available_projects_clear.h.
// Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/game_ClearAvailableProjects_0041c02e.asm), not from Ghidra's C draft (the
// draft is a faithful transcription too, but the .asm is the checked spec).
//
#include "sim/libtrans/sim_lt_available_projects_clear.h"

#include "addr/mh_calls.gen.h" // typed callable for the effectful/frontier original we still call OUT to
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const available_projects_clear_calls &live_available_projects_clear_calls() {
    static const available_projects_clear_calls c = {
        MH_CRT(utils_fill_data),
    };
    return c;
}

namespace detail {

// ---- game_ClearAvailableProjects @0x0041c02e ---------------------------------------------------
void available_projects_clear(sim_store &own, const available_projects_clear_calls &c) {
    // 0x0041c046-0x0041c052: MOV EBX,0xc80 ; XOR EDX,EDX ; MOV EAX,0x50b077 ; CALL utils_fill_data
    // -- one memset-shaped call over the WHOLE RID_AVAILABLEPROJECTS region, not a per-bucket loop.
    // Size derived from the region's own extents (8 players * 2 types * 50 ints/bucket * 4 bytes)
    // rather than written as the literal 0xc80, though the two are the same 3200 bytes.
    constexpr uint32_t kAvailableProjectsBytes = static_cast<uint32_t>(MAX_PLAYERS) *
                                                 static_cast<uint32_t>(AVAILABLE_PROJECTS_TYPES) *
                                                 static_cast<uint32_t>(AVAILABLE_PROJECTS_BUCKET_INTS) *
                                                 sizeof(int32_t);
    static_assert(kAvailableProjectsBytes == 0xc80,
                  "AvailableProjects region size mismatch vs 0x0041c046's EBX=0xc80 literal");

    c.fill_data(own.available_projects_base(), kAvailableProjectsBytes,
                0); // 0x0041c052 -> 0x0050b077 AvailableProjects (whole region)
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------

void available_projects_clear() {
    sim_state st = state();
    detail::available_projects_clear(st.own, live_available_projects_clear_calls());
}

} // namespace mh::sim
