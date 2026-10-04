//
// backend_null.cpp -- PT-GFX1/PT-GFX2: the presenter that presents nothing (gfx/present_backend.h).
//
// [video] backend=null. Frames are still composed -- the game renders into the owned back buffer
// exactly as before, and the UI harness reads _G_LLM_FRAMEBUFFER at llm_gfx_present_flip's entry,
// upstream of any presenter -- they are simply never put on a screen. The owner (ddraw_own.cpp) leaves
// the window alone under this backend, so a lane that also sets [video] no_window keeps it unmapped.
// The frame limiter still applies ([video] fps_limit; 0 = unlimited).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "gfx/present_backend.h"

namespace mh::gfx {
namespace {

class null_backend final : public present_backend {
public:
    const char *name() const override { return "null"; }
    bool        attach(HWND, const backend_config &) override { return true; }
    void        on_mode(int, int, pixel_format) override {}
    bool        present(const frame_view &) override { return true; }
    void        detach() override {}
};

} // namespace

present_backend *make_null_backend() { return new null_backend(); }

} // namespace mh::gfx
