#include "strategic_input.h"
#include "config/ini_read.h"
#include "include/mh_strategic_input_export.h"
#include <new>

namespace mh::seams::strategic_input {
namespace {
bindings::profile local_profile;
alignas(bindings::resolver) unsigned char resolver_storage[sizeof(bindings::resolver)];
bindings::resolver *input            = nullptr;
uint8_t             held_modifiers   = 0;
bool                started          = false;
bool                input_started    = false;
bool                indicator_toggle = false;
struct dispatch_context {
    bindings::sink downstream;
    void          *user;
};
void dispatch_action(void *opaque, size_t action, bindings::phase phase) {
    auto &ctx = *static_cast<dispatch_context *>(opaque);
    if (action == size_t(bindings::action_id::network_indicator)) {
        if (phase == bindings::phase::pressed) indicator_toggle = !indicator_toggle;
    } else if (action != size_t(bindings::action_id::expanded_map) && ctx.downstream) ctx.downstream(ctx.user, action, phase);
}
bool ini_reader(void *user, const char *section, const char *key, const char *def, char *out, size_t capacity) {
    if (!user || capacity < 2 || capacity > MAXDWORD) return false;
    const DWORD n = mh::config::read_ini_string(section, key, def, out, DWORD(capacity), static_cast<const char *>(user));
    return n < capacity - 1;
}
bool import_reader(void *user, const char *section, const char *key, const char *def, char *out, size_t capacity) {
    if (!user || capacity < 2 || capacity > MAXDWORD) return false;
    const DWORD n = mh::config::read_ini_player_import_string(section, key, def, out, DWORD(capacity), static_cast<const char *>(user));
    return n < capacity - 1;
}
} // namespace
bool initialize(const char *resolved_ini, const uint8_t *journal, size_t journal_size, bindings::error &why) noexcept {
    // Initialization is deliberately not reloadable: changing profiles with held keys loses release ownership.
    if (started) {
        why = {bindings::error_code::wrong_version, 0, 0, 0};
        return false;
    }
    bindings::profile candidate;
    if (journal || journal_size) {
        if (!bindings::decode_profile(journal, journal_size, candidate, why)) return false;
    } else if (!bindings::load(candidate, ini_reader, const_cast<char *>(resolved_ini), why, import_reader)) return false;
    local_profile = candidate;
    input         = new (resolver_storage) bindings::resolver(local_profile);
    started       = true;
    return true;
}
bool adopt_journal_profile(const uint8_t *journal, size_t journal_size, bindings::error &why) noexcept {
    if (!started || input_started) {
        why = {bindings::error_code::wrong_version, 0, 0, 0};
        return false;
    }
    bindings::profile candidate;
    if (!bindings::decode_profile(journal, journal_size, candidate, why)) return false;
    input->~resolver();
    local_profile    = candidate;
    input            = new (resolver_storage) bindings::resolver(local_profile);
    held_modifiers   = 0;
    indicator_toggle = false;
    return true;
}
const bindings::profile *effective_profile() noexcept { return started ? &local_profile : nullptr; }
uint32_t                 effective_profile_fingerprint() noexcept { return started ? bindings::fingerprint(local_profile) : 0; }
bool                     write_journal_profile(uint8_t *out, size_t capacity, size_t &written) noexcept {
    written = 0;
    return started && bindings::encode_profile(local_profile, out, capacity, written);
}
void context_changed(bindings::context ctx, bindings::sink dispatch, void *user) noexcept {
    dispatch_context delivery = {dispatch, user};
    if (input) input->set_context(ctx, dispatch_action, &delivery);
}
void note_input_record() noexcept { input_started = true; }
bool key_event(uint32_t scan, uint32_t type, uint8_t modifiers, bool repeated, bindings::sink dispatch, void *user) noexcept {
    held_modifiers = modifiers & 7;
    if (!input || !(type & 0x180)) return false;
    note_input_record();
    dispatch_context ctx = {dispatch, user};
    return input->process({bindings::native_physical(scan, type), held_modifiers, (type & 0x100) != 0, repeated}, dispatch_action, &ctx);
}
void focus_lost(bindings::sink dispatch, void *user) noexcept {
    held_modifiers       = 0;
    dispatch_context ctx = {dispatch, user};
    if (input) input->clear(dispatch_action, &ctx);
}
void update_modifiers(uint8_t modifiers) noexcept { held_modifiers = modifiers & 7; }
bool role_held(bindings::role_index role) noexcept { return started && bindings::role_held(local_profile, role, held_modifiers); }
bool action_held(size_t action) noexcept { return input && input->held(action); }
bool take_network_indicator_toggle() noexcept {
    bool value       = indicator_toggle;
    indicator_toggle = false;
    return value;
}
bindings::gesture classify_click(const bindings::click_context &click) noexcept {
    return started ? bindings::classify_click(local_profile, click) : bindings::gesture::native;
}
} // namespace mh::seams::strategic_input

extern "C" int MH_StrategicBindings_Profile(void *bytes, unsigned capacity, unsigned *written) {
    namespace b = mh::strategic_bindings;
    if (written) *written = unsigned(b::journal_profile_bytes);
    if (!mh::seams::strategic_input::effective_profile()) return 0;
    if (!bytes) return capacity == 0 ? 1 : 0;
    size_t count = 0;
    if (!mh::seams::strategic_input::write_journal_profile(static_cast<uint8_t *>(bytes), capacity, count)) return 0;
    if (written) *written = unsigned(count);
    return 1;
}
extern "C" int MH_StrategicBindings_AdoptProfile(const void *bytes, unsigned size, unsigned *error_code) {
    mh::strategic_bindings::error why;
    const bool                    ok = mh::seams::strategic_input::adopt_journal_profile(static_cast<const uint8_t *>(bytes), size, why);
    if (error_code) *error_code = unsigned(why.code);
    return ok ? 1 : 0;
}
