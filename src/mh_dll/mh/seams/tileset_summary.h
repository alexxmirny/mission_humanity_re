#pragma once
#include <cstdio>
#include <cwchar>
#include <cstring>

namespace mh::tileset_runtime {
// Same nine cdecl arguments as both retail w_sprintf calls. dst is the game's
// 256-WCHAR summary. Overflow replaces the whole result, not part of a label.
inline int format_summary(wchar_t *dst, const wchar_t *fmt, int marker,
                          const wchar_t *map, const wchar_t *tileset, int width,
                          int height, const wchar_t *players_text, int players,
                          const wchar_t *overflow) {
    const int n = _snwprintf_s(dst, 256, _TRUNCATE, fmt, marker, map, tileset,
                               width, height, players_text, players);
    if (n >= 0) return n;
    size_t len = 0;
    while (len < 255 && overflow[len]) ++len;
    std::memcpy(dst, overflow, len * sizeof(wchar_t));
    dst[len] = 0;
    return static_cast<int>(len);
}
} // namespace mh::tileset_runtime
