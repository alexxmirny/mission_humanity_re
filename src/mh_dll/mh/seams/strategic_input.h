#pragma once
#include "mh_strategic_bindings.h"

namespace mh::seams::strategic_input {
namespace bindings = mh::strategic_bindings;
// Call ONCE after resolved mh_config_path_a is available, before input producers and replay start.
// A nonempty journal profile replaces local assignment values, never overlays them.
// On failure caller presents the named invalid binding and refuses to arm gameplay input.
bool initialize(const char *resolved_ini, const uint8_t *journal, size_t journal_size, bindings::error &why) noexcept;
// Replay preflight only, before the first normalized record. Refuses a profile change once input starts.
bool                     adopt_journal_profile(const uint8_t *journal, size_t journal_size, bindings::error &why) noexcept;
const bindings::profile *effective_profile() noexcept;
uint32_t                 effective_profile_fingerprint() noexcept;
bool                     write_journal_profile(uint8_t *out, size_t capacity, size_t &written) noexcept;
// Owner changes context from actual active gameplay/modal consumer, not merely a queued widget pointer.
void context_changed(bindings::context ctx, bindings::sink dispatch, void *user) noexcept;
// Admission barrier only; call for every normalized keyboard/mouse record, even in passthrough contexts.
// Does not dispatch actions or change held/resolver state.
void note_input_record() noexcept;
// Observe each NORMALIZED keyboard record exactly once, including records popped by nonstrategic drains.
// Only the appropriate consumer dispatches presses. Releases/focus loss clear ownership in all contexts.
bool              key_event(uint32_t scan, uint32_t event_type, uint8_t modifiers, bool repeated,
                            bindings::sink dispatch, void *user) noexcept;
void              focus_lost(bindings::sink dispatch, void *user) noexcept;
void              update_modifiers(uint8_t modifiers) noexcept;
bool              role_held(bindings::role_index role) noexcept;
bool              action_held(size_t action) noexcept;
bool              take_network_indicator_toggle() noexcept;
bindings::gesture classify_click(const bindings::click_context &click) noexcept;
} // namespace mh::seams::strategic_input
