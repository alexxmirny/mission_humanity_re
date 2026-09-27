//
// seams/ui_info_avi.h -- mp:X2g: the PURE half of the info-screen AVI fix, the part a selftest can
// drive without a game (mh_nettest/info_avi_selftest.cpp, `net_selftest.exe infoavitest`).
//
// The seam itself (ui_info_avi.cpp) replaces llm_ui_info_media_frame_tick (EN 0x004caa46) and
// splices the AVI-open failure branch of llm_ui_entity_info_screen_open (EN 0x004cad31). Mechanism,
// addresses and evidence: include/mh_infoavi_export.h. What lives here is the arithmetic the
// replaced tick does every frame, because that is where the retail crash was: the tick advances
// `playback_pos_ms` by the frame delta and wraps it `% stream_length_time`, and after a FAILED open
// llm_ui_avi_open has zero-filled the context, so the divisor is 0 -- `IDIV EBX` at 0x004cab52,
// the rc4 field crash (0xc0000094).
//
#pragma once
#include <cstdint>

namespace mh::info_avi {

// Does this frame touch the video at all? Retail always did (Lock / decode / Unlock / BltFast). The
// guard needs BOTH halves: a NULL surface is the state a failed open leaves on a first open (close
// releases and NULLs it), and a zero stream length is the state a failed open leaves on EVERY open
// (llm_ui_avi_open zero-fills ctx[0..0x214), which holds the video stream record). Either one alone
// means there is no clip to draw.
inline bool video_on(bool surface_present, int32_t stream_length_time) {
    return surface_present && stream_length_time > 0;
}

struct tick_step {
    int32_t pos;     // the new playback_pos_ms
    int32_t delta;   // what the tick stores to _G_LLM_UI_INFO_SCREEN_FRAME_DELTA_MS
    bool    wrapped; // retail's loop branch ran (it also stores -1 to video_stream+0xb0, 0x0065f99b)
};

// Retail's clock step, line for line (0x004caaf7..0x004cab54), with the one divisor guard:
//   delta = now - last;  pos += delta;
//   if (len <= pos) { <-1 store>; pos %= len; }       -- retail: IDIV by len, even when len == 0
// Guarded: when len <= 0 there is no clip to loop, so the position is parked at 0 and the -1 store
// (a decoder state reset) is skipped -- nothing reads it without a clip. Signed compare, as the
// original's `JL` is.
inline tick_step plan_tick(int32_t stream_length_time, int32_t pos, uint32_t now_ms,
                           uint32_t last_ms) {
    tick_step s;
    s.delta   = (int32_t)(now_ms - last_ms);
    s.pos     = (int32_t)((uint32_t)pos + (uint32_t)s.delta); // two's-complement add, as the ADD is
    s.wrapped = false;
    if (stream_length_time <= s.pos) {
        if (stream_length_time > 0) {
            s.wrapped = true;
            s.pos     = s.pos % stream_length_time;
        } else {
            s.pos = 0;
        }
    }
    return s;
}

// The `[net] info_avi_test_absent=<list>` matcher: `*` matches every clip, otherwise a comma list of
// file names compared case-insensitively against the name's last path component. Empty never
// matches. ASCII only, like the retail file names (INIT.CFG's INFO_FLC entries).
inline bool name_listed(const char *list, const char *path) {
    if (list == nullptr || path == nullptr || list[0] == '\0') return false;
    if (list[0] == '*' && list[1] == '\0') return true;
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    auto        low  = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; };
    const char *item = list;
    while (*item) {
        while (*item == ' ' || *item == ',') ++item;
        const char *end = item;
        while (*end && *end != ',') ++end;
        const char *tail = end;
        while (tail > item && tail[-1] == ' ') --tail;
        const size_t n = (size_t)(tail - item);
        if (n > 0) {
            size_t i = 0;
            while (i < n && base[i] && low(base[i]) == low(item[i])) ++i;
            if (i == n && base[i] == '\0') return true;
        }
        item = end;
    }
    return false;
}

} // namespace mh::info_avi
