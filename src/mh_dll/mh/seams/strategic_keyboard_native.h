#pragma once
#include "mh_strategic_bindings.h"

namespace mh::seams::strategic_keyboard_native {
// Caller owns ONE dequeued local record, not the raw shared ring or keystate array.
// token: deliver returned scan/type to the native strategic ladder exactly once.
// consumed: clear local scan to zero, retaining the record's timestamp and every unrelated field.
// not_main: the active modal adapter must route this action to its native widget consumer instead.
enum class delivery { token,
                      consumed,
                      not_main };
delivery dispatch(size_t action, mh::strategic_bindings::phase phase, uint32_t &scan, uint32_t &type) noexcept;
} // namespace mh::seams::strategic_keyboard_native
