// seams/ui_diplomacy_echo.cpp -- mp:U39: NOP the diplomacy dialog's optimistic relation echo so the
// 0xf4 order handler is the hashed cell's only writer. The mechanism, the measured skew and the
// cost are in include/mh_diploecho_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#include "include/mh_diploecho_export.h"
#include "addr/mh_addrs.gen.h" // mh::addr::diplo_echo_write_site
#include "hook/patch.h"        // patch_bytes_guarded
#include "net_internal.h"      // seam_log; g_ini
#include "en_guard.h"          // EN-only build gate

namespace {

constexpr int ECHO_LEN = 22;
// The exact span (docs/symbols.md llm_ui_diplomacy_apply_and_resume, EN v406): a mismatch REFUSES
// -- patch_bytes_guarded writes nothing unless every byte matches, which is what keeps a shifted or
// already-patched function intact.
const uint8_t ECHO_EXPECT[ECHO_LEN] = {0x0F, 0xB7, 0x15, 0x54, 0x83, 0xE5, 0x00, 0x6B, 0xD2, 0x34, 0x03,
                                       0x55, 0xD4, 0x8A, 0x45, 0xD8, 0x88, 0x82, 0xF1, 0x87, 0xE5, 0x00};
const uint8_t ECHO_NOPS[ECHO_LEN]   = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                       0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

bool g_applied = false;

} // namespace

extern "C" int MH_DiploEcho_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_applied) return 1;
    const bool want = GetPrivateProfileIntA("net", "diplo_echo_nop", 1, g_ini) != 0;
    if (!want) {
        seam_log("; U39: diplomacy relation echo KEPT ([net] diplo_echo_nop=0): the clicking peer writes "
                 "Players[PlayerSide].relation[j] ahead of the 0xf4 commit -- the reproduction arm\n");
        return 0;
    }
    g_applied = mh::hook::patch_bytes_guarded(mh::addr::diplo_echo_write_site, ECHO_EXPECT, ECHO_NOPS, ECHO_LEN);
    char m[240];
    // clang-format off
    const char *fmt = g_applied
        ? "; U39: diplomacy relation echo NOPed at %08X (22 bytes) -- the 0xf4 order handler is the cell's only writer\n"
        : "; U39: diplomacy relation echo NOT patched at %08X -- bytes differ from the expected span or the parent is promoted (see any [interlock] line); retail echo kept\n";
    // clang-format on
    wsprintfA(m, fmt, (unsigned)mh::addr::diplo_echo_write_site);
    seam_log(m);
    return g_applied ? 1 : 0;
}
