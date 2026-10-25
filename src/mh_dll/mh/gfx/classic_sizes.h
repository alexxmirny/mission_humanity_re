// gfx/classic_sizes.h -- mods:RES25: the ONE predicate behind the launcher's "Classic sizes only" ([video] resolution=stock, the default).
//
// Classic sizes = no resolution work and no Interface size scaling: the in-game Screen size control keeps the game's own three entries, no
// generated list / crop / ceil32 render exists, and [video] ui_scale acts as 100 whatever the ini says. What stays on regardless (the presenter's scaling of a
// stock frame, the mode-change crash guards, borderless sizing) is not resolution work and is untouched.
//
// A DEV size key (`modes=`, `picker=1`, `width`/`height`) is an explicit developer override of the very thing classic turns off, and the launcher
// never writes one; with any of them set the build keeps its old behaviour (the rig's fixed-size rows depend on it). `size_mode=` is NOT a dev
// size key here: 0..2 pin a stock entry, which classic keeps.
//
// Read ONCE (the first call, with the ini path); later calls take no path. Pure core: classic_from().
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "config/ini_read.h"
#include "mh_ini_gate.h"

namespace mh::gfx {

// resolution: the raw `[video] resolution` value; dev_size_key: any of modes= / picker=1 / width / height is set.
inline bool classic_from(const char *resolution, bool dev_size_key) {
    const bool stock = resolution == nullptr || resolution[0] == 0 || lstrcmpiA(resolution, "stock") == 0;
    return stock && !dev_size_key;
}

inline bool classic_sizes(const char *ini = nullptr) {
    static int state = -1; // -1 = not read yet
    if (state < 0) {
        if (!ini) return false; // asked before the install read it: nothing is classic yet (never happens: install_owned_ddraw is first)
        char res[32], modes[8];
        mh::config::read_ini_string("video", "resolution", "stock", res, sizeof(res), ini);
        mh::config::read_ini_string("video", "modes", "", modes, sizeof(modes), ini);
        const bool dev = modes[0] || mh_ini_get_int("video", "picker", 0, ini) != 0 || mh_ini_get_int("video", "width", 0, ini) > 0 ||
                         mh_ini_get_int("video", "height", 0, ini) > 0;
        state = classic_from(res, dev) ? 1 : 0;
    }
    return state == 1;
}

} // namespace mh::gfx
