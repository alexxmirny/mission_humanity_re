//
// save/save_format.cpp -- SV1 batch A: the save file's version gate.
//
// Spec: /eng/mh.exe 0x004476b9-0x00447778. The header is in save_format.h; this file is the
// translation, and the comments are about what the disassembly does that the decompiler's shape hides.
//
#include "save/save_format.h"

#include "save/save_state.h" // SB-BIND T5: the live arm's non-block addresses

#include <cstring>

namespace mh::save {

namespace {
// REPE CMPSB over exactly VERSION_STRING_LEN bytes -- NOT a string compare. The stored strings are
// space-padded to 40 and are not nul-terminated within the table, so strcmp/strncmp would stop at the
// first difference in padding on some entries and read past the slot on others. memcmp is the same
// operation the original performs.
bool slot_matches(const version_table &vt, int32_t slot, const char *header) {
    return std::memcmp(vt.slots + static_cast<size_t>(slot) * VERSION_STRING_LEN, header,
                       VERSION_STRING_LEN) == 0;
}
} // namespace

int32_t detect_version(const version_table &vt, const char *header) {
    // 0x004476b9: CMP [_G_LLM_GAME_MISSION_VERSION_INDEX],0 / JNZ. The demo build takes a completely
    // separate path -- an EXACT match against its own slot, with no table scan and no compatibility
    // with anything else.
    if (*vt.build_index == 0) {
        if (!slot_matches(vt, *vt.build_index, header)) return VERSION_REJECT;
        return VERSION_DEMO_OK; // the original leaves this undefined; see the header
    }

    // 0x00447702: the detected slot starts at -1 and stays there unless something matches, which is
    // exactly what makes "no match" a refusal rather than a fall-through to slot 0.
    int32_t detected = VERSION_REJECT;

    // 0x00447709-0x0044775c: `for (i = 1; i < VERSION_SLOTS; ++i)`. Starts at ONE -- slot 0 (demo) is
    // unreachable here -- and has NO early exit, so the last match wins. Both faithful; see the header.
    for (int32_t i = 1; i < VERSION_SLOTS; ++i)
        if (slot_matches(vt, i, header)) detected = i;

    return detected;
}

const version_table &live_version_table() {
    static const version_table vt = {
        binds().version_strings,
        binds().version_index,
    };
    return vt;
}

int32_t detect_version_live(const char *header) { return detect_version(live_version_table(), header); }

} // namespace mh::save
