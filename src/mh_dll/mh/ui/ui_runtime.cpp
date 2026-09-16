//
// mh/ui -- the arm-time wiring. Three slots, three setters, nothing else.
//
#include "ui/ui_internal.h"

namespace mh {
namespace ui {
namespace detail {

void (*g_log)(const char *)   = nullptr;
const bool *g_diag            = nullptr;
int (*g_client_session)(void) = nullptr;

} // namespace detail

void set_logger(void (*fn)(const char *)) {
    detail::g_log = fn;
}

void set_diag_flag(const bool *flag) {
    detail::g_diag = flag;
}

void set_client_session_gate(int (*fn)(void)) {
    detail::g_client_session = fn;
}

} // namespace ui
} // namespace mh
