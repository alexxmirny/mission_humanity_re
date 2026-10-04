//
// ui/text_wrap.h -- word-wrap a UTF-16 status line to a pixel width (the lobby/browser status field).
//
// PURE: the width of a run of text comes from the caller's measure callback (in mh.dll the game's own
// llm_ui_text_measure_width over the widget's font -- real glyph advances, not a character count), so
// the selftests drive it with a fixed-advance font.
//
// Rules: existing '\n' are kept (each paragraph wraps on its own); a paragraph that fits is copied
// VERBATIM (so any text that fitted before is byte-identical -- the EN baselines that never overflowed
// do not move); otherwise lines break at spaces (the breaking run of spaces is dropped), and a single
// word wider than the field is hard-broken at the last character that fits. If the result needs more
// lines than max_lines, the last visible line ends in "..." (trimmed until the ellipsis fits): the
// START of a message is where its meaning is, and a status line has no scroll affordance.
//
#pragma once
#include <cstdint>

namespace mh {
namespace ui {

// Pixel width of the n units at s (not NUL-terminated).
using measure_fn = int (*)(void *ctx, const wchar_t *s, int n);

struct WrapReport {
    int  lines;     // lines in the output
    int  breaks;    // line breaks inserted
    int  hard;      // of which inside a word (a word wider than the field)
    bool truncated; // more lines than max_lines: the last one ends in "..."
};

// Wrap `in` into `out` (cap units, NUL included). width <= 0 or max_lines <= 0 copies verbatim.
// Returns the output length in units.
int wrap_text(const wchar_t *in, wchar_t *out, int cap, int width, int max_lines, measure_fn measure, void *ctx,
              WrapReport *rep);

} // namespace ui
} // namespace mh
