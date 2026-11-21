#include "include/mh_strategic_bindings.h"
#include <cstring>

namespace mh::strategic_bindings {
namespace {
#define MH_STRATEGIC_ACTION(id, en, ru, cat, ctx, type, pri, sec, section, key, altkey, ignored, scan, mods, repeat, ...) \
    {id, en, ru, cat, ctx, type[0] == 'h' ? kind::held : kind::event, pri, sec, section, key, altkey, ignored, scan, mods, repeat != 0, type[0] != 'f', __VA_ARGS__},
#define MH_STRATEGIC_ROLE(...)
#define MH_STRATEGIC_KEY(...)
#define MH_STRATEGIC_RESERVED(...)
#define MH_STRATEGIC_EXCLUSIVE(...)
constexpr action_row action_rows[] = {
#include "include/mh_strategic_bindings.def"
};
#undef MH_STRATEGIC_ACTION
#undef MH_STRATEGIC_ROLE
#define MH_STRATEGIC_ACTION(...)
#define MH_STRATEGIC_ROLE(id, en, ru, def, section, key, type) {id, en, ru, def, section, key, type[0] == 'm' ? role_kind::modifier : role_kind::button},
constexpr role_row role_rows[] = {
#include "include/mh_strategic_bindings.def"
};
#undef MH_STRATEGIC_ROLE
#undef MH_STRATEGIC_KEY
#define MH_STRATEGIC_ROLE(...)
#define MH_STRATEGIC_KEY(token, scan, e0, vk) {token, scan, e0 != 0, vk},
constexpr key_row key_rows[] = {
#include "include/mh_strategic_bindings.def"
};
#undef MH_STRATEGIC_KEY
#undef MH_STRATEGIC_RESERVED
#define MH_STRATEGIC_KEY(...)
#define MH_STRATEGIC_RESERVED(key, ctx, reason) {key, ctx, reason},
constexpr reserved_row reserved_rows[] = {
#include "include/mh_strategic_bindings.def"
};
#undef MH_STRATEGIC_RESERVED
#undef MH_STRATEGIC_EXCLUSIVE
#define MH_STRATEGIC_RESERVED(...)
#define MH_STRATEGIC_EXCLUSIVE(left, right) {left, right},
constexpr const char *exclusive_roles[][2] = {
#include "include/mh_strategic_bindings.def"
};
#undef MH_STRATEGIC_ACTION
#undef MH_STRATEGIC_ROLE
#undef MH_STRATEGIC_KEY
#undef MH_STRATEGIC_RESERVED
#undef MH_STRATEGIC_EXCLUSIVE
static_assert(sizeof(action_rows) / sizeof(*action_rows) == action_count);
static_assert(size_t(action_id::count) == action_count);
static_assert(sizeof(role_rows) / sizeof(*role_rows) == role_count);
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
bool same(const char *a, const char *b) {
    while (*a && *b && lower(*a) == lower(*b)) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}
bool token_in(const char *list, const char *token) {
    if (same(list, "*")) return true;
    const size_t n = std::strlen(token);
    for (const char *p = list; *p;) {
        const char *end = p;
        while (*end && *end != '|') ++end;
        if (size_t(end - p) == n && !std::strncmp(p, token, n)) return true;
        p = *end ? end + 1 : end;
    }
    return false;
}
bool good_key(uint8_t physical) {
    const uint8_t base = physical & 0x7f;
    if (!base || base == 0x1d || base == 0x38 || base == 0x2a || base == 0x36) return false;
    // Windows/application keys are OS/UI-owned, not strategic gameplay keys.
    return physical != 0xdb && physical != 0xdc && physical != 0xdd;
}
int hex(char c) {
    c = lower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                                 : -1;
}
bool byte_hex(const char *p, uint8_t &v) {
    if (!p[0] || !p[1] || p[2]) return false;
    int a = hex(p[0]), b = hex(p[1]);
    if (a < 0 || b < 0) return false;
    v = uint8_t(a * 16 + b);
    return true;
}
bool virtual_hex(const char *p, uint8_t &v) {
    if (!*p) return false;
    unsigned value = 0;
    for (; *p; ++p) {
        int digit = hex(*p);
        if (digit < 0 || value > 15) return false;
        value = value * 16 + unsigned(digit);
    }
    if (!value || value > 255) return false;
    v = uint8_t(value);
    return true;
}
bool prefix(const char *s, const char *p) {
    while (*p)
        if (!*s || lower(*s++) != lower(*p++)) return false;
    return true;
}
bool normalized_spec(const char *text, char *out) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t') ++text;
    const char *end = text + std::strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) --end;
    const size_t n = size_t(end - text);
    if (n > max_spec_bytes) return false;
    std::memcpy(out, text, n);
    out[n] = 0;
    return true;
}
bool matches(chord c, uint8_t physical, uint8_t mods) {
    return c.physical && c.physical == physical && (mods & c.modifiers) == c.modifiers &&
           !(mods & uint8_t(~(c.modifiers | c.ignored)));
}
unsigned specificity(chord c) { return unsigned((c.modifiers & 1) != 0) + unsigned((c.modifiers & 2) != 0) + unsigned((c.modifiers & 4) != 0); }
uint16_t context_mask(const char *contexts) {
    uint16_t mask = 0;
    for (unsigned i = 1; i <= unsigned(context::wait); ++i)
        if (token_in(contexts, context_name(context(i)))) mask |= uint16_t(1u << i);
    return mask;
}
bool fail(error &why, error_code code, size_t action = 0, size_t slot = 0, size_t other = 0) {
    why = {code, action, slot, other};
    return false;
}
void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}
const char *binding_default(const action_row &row, size_t slot) {
    switch (slot) {
        case 0: return row.primary;
        case 1: return row.alternate;
        case 2: return row.third;
        default: return row.fourth;
    }
}
const char *binding_key(const action_row &row, size_t slot) {
    switch (slot) {
        case 0: return row.key;
        case 1: return row.alt_key;
        case 2: return row.third_key;
        default: return row.fourth_key;
    }
}
const char *binding_section(const action_row &row, size_t slot) {
    return slot == 0 ? row.section : "keys";
}
bool fail_setting(error &why, error_code code, const char *key) {
    why = {code, action_count, role_count, 0, key};
    return false;
}
struct legacy_navigation_row {
    const char *key, *default_value;
};
#define MH_STRATEGIC_LEGACY_NAV(key, value) {key, value},
#define MH_STRATEGIC_LEGACY_SHIFT(key)
constexpr legacy_navigation_row legacy_navigation[] = {
#include "include/mh_strategic_legacy.gen.def"
};
#undef MH_STRATEGIC_LEGACY_NAV
#undef MH_STRATEGIC_LEGACY_SHIFT
#define MH_STRATEGIC_LEGACY_NAV(key, value)
#define MH_STRATEGIC_LEGACY_SHIFT(key) key,
constexpr const char *legacy_shift_keys[] = {
#include "include/mh_strategic_legacy.gen.def"
};
#undef MH_STRATEGIC_LEGACY_NAV
#undef MH_STRATEGIC_LEGACY_SHIFT

bool check_legacy_navigation(reader read, void *user, error &why) {
    for (const auto &row : legacy_navigation) {
        char  text[96]{};
        chord actual, expected;
        if (!read(user, "keys", row.key, row.default_value, text, sizeof(text)) ||
            !parse_chord(text, actual) ||
            !parse_chord(row.default_value, expected))
            return fail_setting(why, error_code::malformed, row.key);
        if (!(actual == expected))
            return fail_setting(why, error_code::migration_conflict, row.key);
    }
    return true;
}
} // namespace
const action_row *actions() noexcept { return action_rows; }
const role_row   *roles() noexcept { return role_rows; }
const key_row    *keys(size_t &count) noexcept {
    count = sizeof(key_rows) / sizeof(*key_rows);
    return key_rows;
}
const reserved_row *reserved(size_t &count) noexcept {
    count = sizeof(reserved_rows) / sizeof(*reserved_rows);
    return reserved_rows;
}
const char *context_name(context ctx) noexcept {
    constexpr const char *names[] = {"none", "main", "game_menu", "options", "info", "planet", "saveload", "diplomacy", "tutorial", "outcome", "wait"};
    const auto            index   = size_t(ctx);
    return index < sizeof(names) / sizeof(*names) ? names[index] : "none";
}
bool contexts_overlap(const char *a, const char *b) noexcept {
    if (same(a, "*") || same(b, "*")) return true;
    for (unsigned i = 1; i <= unsigned(context::wait); ++i) {
        const char *n = context_name(context(i));
        if (token_in(a, n) && token_in(b, n)) return true;
    }
    return false;
}
bool parse_chord(const char *text, chord &out) noexcept {
    out = {};
    char buf[max_spec_bytes + 2];
    if (!normalized_spec(text, buf)) return false;
    if (!buf[0] || same(buf, "none")) return true;
    char *p = buf;
    for (;;) {
        uint8_t bit  = 0;
        size_t  skip = 0;
        if (prefix(p, "ctrl+")) {
            bit  = ctrl;
            skip = 5;
        } else if (prefix(p, "control+")) {
            bit  = ctrl;
            skip = 8;
        } else if (prefix(p, "alt+")) {
            bit  = alt;
            skip = 4;
        } else if (prefix(p, "shift+")) {
            bit  = shift;
            skip = 6;
        }
        if (!bit) break;
        if (out.modifiers & bit) return false;
        out.modifiers |= bit;
        p += skip;
    }
    if (prefix(p, "scan:")) {
        p += 5;
        bool e0 = false;
        if (prefix(p, "e0:")) {
            e0 = true;
            p += 3;
        }
        uint8_t scan;
        if (!byte_hex(p, scan) || !scan || scan >= 0x80) return false;
        out.physical = uint8_t(scan | (e0 ? 0x80 : 0));
    } else {
        const bool vk_prefix = prefix(p, "vk_");
        if (vk_prefix) p += 3;
        uint16_t vk = 0;
        // Legacy VK_F1/VK_A/VK_TAB keep their named meaning; only an otherwise unknown
        // VK_NN suffix is interpreted as a virtual-key hex byte.
        if (vk_prefix)
            for (const auto &k : key_rows)
                if (same(p, k.token)) {
                    out.physical = uint8_t(k.scan | (k.e0 ? 0x80 : 0));
                    return good_key(out.physical);
                }
        if (prefix(p, "0x") || vk_prefix) {
            uint8_t value;
            if (!virtual_hex(prefix(p, "0x") ? p + 2 : p, value)) return false;
            vk = value;
        }
        for (const auto &k : key_rows)
            if ((vk && vk == k.vk) || (!vk && same(p, k.token))) {
                out.physical = uint8_t(k.scan | (k.e0 ? 0x80 : 0));
                break;
            }
    }
    return good_key(out.physical);
}
bool parse_role(const char *text, role_kind type, uint8_t &out) noexcept {
    out = 0;
    char buf[max_spec_bytes + 2];
    if (!normalized_spec(text, buf)) return false;
    text = buf;
    if (type == role_kind::modifier) {
        if (same(text, "none")) return true;
        if (same(text, "Ctrl") || same(text, "Control")) out = ctrl;
        else if (same(text, "Alt")) out = alt;
        else if (same(text, "Shift")) out = shift;
    } else {
        if (same(text, "MouseLeft")) out = uint8_t(button::left);
        else if (same(text, "MouseRight")) out = uint8_t(button::right);
    }
    return out != 0;
}
uint8_t native_modifiers(const uint8_t *state) noexcept {
    if (!state) return 0;
    return uint8_t(((state[0x1d] & 3) ? ctrl : 0) | ((state[0x38] & 3) ? alt : 0) |
                   (((state[0x2a] | state[0x36]) & 1) ? shift : 0));
}
uint8_t native_physical(uint32_t scan, uint32_t type) noexcept {
    return uint8_t((scan & 0x7f) | ((scan & 0x80) || (type & 0x200) ? 0x80 : 0));
}
bool defaults(profile &out, error &why) noexcept {
    profile candidate;
    for (size_t i = 0; i < action_count; ++i) {
        const auto &row = action_rows[i];
        for (size_t slot = 0; slot < binding_slots; ++slot) {
            auto &value = candidate.bindings[i][slot];
            if (!parse_chord(binding_default(row, slot), value))
                return fail(why, error_code::malformed, i, slot);
            if (value.physical) value.ignored = row.ignored_modifiers;
        }
    }
    for (size_t i = 0; i < role_count; ++i)
        if (!parse_role(role_rows[i].default_value, role_rows[i].type,
                        candidate.role_values[i]))
            return fail(why, error_code::malformed, action_count, i);
    if (!validate(candidate, why)) return false;
    out = candidate;
    return true;
}
bool load(profile &out, reader read, void *user, error &why, reader import_read) noexcept {
    if (!read) return fail(why, error_code::malformed);
    if (!import_read) import_read = read;

    char version[96]{};
    if (!read(user, "keys", "bindings_version", "", version, sizeof(version)))
        return fail_setting(why, error_code::malformed, "bindings_version");

    char normalized_version[max_spec_bytes + 2];
    if (!normalized_spec(version, normalized_version))
        return fail_setting(why, error_code::malformed, "bindings_version");

    const bool canonical = same(normalized_version, "2");
    if (!canonical && normalized_version[0] && !same(normalized_version, "1"))
        return fail_setting(why, error_code::wrong_version, "bindings_version");
    if (!canonical && !check_legacy_navigation(import_read, user, why)) return false;

    profile candidate;
    if (!defaults(candidate, why)) return false;

    for (size_t i = 0; i < action_count; ++i) {
        const auto &row = action_rows[i];
        if (!row.editable) continue;

        for (size_t slot = 0; slot < binding_slots; ++slot) {
            const char *key = binding_key(row, slot);
            if (!key[0]) continue;

            const char *def = binding_default(row, slot);
            char        text[96]{};

            // The missing-value sentinel distinguishes absence from an explicit
            // empty value, which is a deliberate unbinding.
            if (!read(user, binding_section(row, slot), key, "\1",
                      text, sizeof(text)))
                return fail_setting(why, error_code::malformed, key);

            if (same(text, "\1")) {
                const char *legacy_key = nullptr;
                if (!canonical && slot >= 2) {
                    if (same(row.id, "chat_team"))
                        legacy_key = slot == 2
                                         ? "chat_team_numpad"
                                         : "chat_team_numpad_alt";
                    else if (same(row.id, "chat_all"))
                        legacy_key = slot == 2
                                         ? "chat_all_numpad"
                                         : "chat_all_numpad_alt";
                }
                const reader source = legacy_key ? import_read : read;
                if (!source(user, legacy_key ? "keys" : binding_section(row, slot),
                            legacy_key ? legacy_key : key, def,
                            text, sizeof(text)))
                    return fail_setting(why, error_code::malformed,
                                        legacy_key ? legacy_key : key);
            }

            chord value, original;
            if (!parse_chord(text, value) || !parse_chord(def, original))
                return fail_setting(why, error_code::malformed, key);
            if (value.physical &&
                value.physical == original.physical &&
                value.modifiers == original.modifiers)
                value.ignored = row.ignored_modifiers;
            candidate.bindings[i][slot] = value;
        }
    }

    for (size_t i = 0; i < role_count; ++i) {
        const auto &row = role_rows[i];
        char        text[96]{};

        if (!read(user, row.section, row.key, "\1", text, sizeof(text)))
            return fail_setting(why, error_code::malformed, row.key);

        if (same(text, "\1") && !canonical && i == size_t(shift_modifier)) {
            uint8_t imported = 0;
            bool    first    = true;
            for (const char *old_key : legacy_shift_keys) {
                char    old_text[96]{};
                uint8_t old_value = 0;
                if (!import_read(user, "keys", old_key, "Shift",
                                 old_text, sizeof(old_text)) ||
                    !parse_role(old_text, role_kind::modifier, old_value))
                    return fail_setting(why, error_code::malformed, old_key);
                if (!first && imported != old_value)
                    return fail_setting(why, error_code::migration_conflict,
                                        old_key);
                imported = old_value;
                first    = false;
            }
            candidate.role_values[i] = imported;
            continue;
        }

        if (same(text, "\1")) {
            if (!read(user, row.section, row.key, row.default_value,
                      text, sizeof(text)))
                return fail_setting(why, error_code::malformed, row.key);
        }
        if (!parse_role(text, row.type, candidate.role_values[i]))
            return fail_setting(why, error_code::malformed, row.key);
    }

    if (!validate(candidate, why)) return false;
    out = candidate;
    return true;
}
bool validate(const profile &p, error &why) noexcept {
    why = {};
    for (size_t i = 0; i < action_count; ++i)
        for (size_t slot = 0; slot < binding_slots; ++slot) {
            chord c = p.bindings[i][slot];
            if (!action_rows[i].editable) {
                chord expected;
                if (!parse_chord(binding_default(action_rows[i], slot), expected))
                    return fail(why, error_code::malformed, i, slot);
                if (expected.physical)
                    expected.ignored = action_rows[i].ignored_modifiers;
                if (!(c == expected))
                    return fail(why, error_code::fixed_binding, i, slot);
            }
            if ((c.modifiers | c.ignored) > 7 || (!c.physical && (c.modifiers || c.ignored)) || (c.physical && !good_key(c.physical))) return fail(why, error_code::malformed, i, slot);
            if (!c.physical) continue;
            for (const auto &r : reserved_rows)
                if (contexts_overlap(action_rows[i].contexts, r.contexts)) {
                    chord reserved_chord;
                    if (!parse_chord(r.key, reserved_chord)) return fail(why, error_code::malformed, i, slot);
                    if (c.physical == reserved_chord.physical && (c.modifiers & reserved_chord.modifiers) == reserved_chord.modifiers) return fail(why, error_code::reserved, i, slot);
                }
            for (size_t j = i + 1; j < action_count; ++j) {
                if (!contexts_overlap(action_rows[i].contexts, action_rows[j].contexts)) continue;
                for (chord d : p.bindings[j])
                    if (c.physical == d.physical && specificity(c) == specificity(d)) {
                        for (uint8_t mods = 0; mods < 8; ++mods)
                            if (matches(c, c.physical, mods) && matches(d, c.physical, mods)) return fail(why, error_code::collision, i, slot, j);
                    }
            }
        }
    for (size_t i = 0; i < role_count; ++i) {
        uint8_t value = p.role_values[i];
        bool    good  = role_rows[i].type == role_kind::button ? (value == 1 || value == 2) : (value == 0 || value == 1 || value == 2 || value == 4);
        if (!good) return fail(why, error_code::malformed, action_count, i);
    }
    for (const auto &pair : exclusive_roles) {
        size_t left = role_count, right = role_count;
        for (size_t i = 0; i < role_count; ++i) {
            if (same(role_rows[i].id, pair[0])) left = i;
            if (same(role_rows[i].id, pair[1])) right = i;
        }
        if (left == role_count || right == role_count) return fail(why, error_code::malformed, action_count);
        if (p.role_values[left] && p.role_values[left] == p.role_values[right]) return fail(why, error_code::role_collision, action_count, left, right);
    }
    return true;
}
uint32_t fingerprint(const profile &p) noexcept {
    uint32_t hash = 2166136261u;
    auto     add  = [&](uint8_t b) { hash = (hash ^ b) * 16777619u; };
    for (unsigned i = 0; i < 4; ++i) add(uint8_t(profile_version >> (i * 8)));
    for (unsigned i = 0; i < 4; ++i) add(uint8_t(catalog_identity >> (i * 8)));
    for (const auto &row : p.bindings)
        for (chord c : row) {
            add(c.physical);
            add(c.modifiers);
            add(c.ignored);
        }
    for (uint8_t r : p.role_values) add(r);
    return hash;
}
bool encode_profile(const profile &p, uint8_t *out, size_t capacity, size_t &written) noexcept {
    written = 0;
    error why;
    if (!out || capacity < journal_profile_bytes || !validate(p, why)) return false;
    put32(out, profile_version);
    put32(out + 4, uint32_t(action_count));
    put32(out + 8, uint32_t(role_count));
    put32(out + 12, catalog_identity);
    size_t n = 16;
    for (const auto &row : p.bindings)
        for (chord c : row) {
            out[n++] = c.physical;
            out[n++] = c.modifiers;
            out[n++] = c.ignored;
        }
    for (uint8_t role : p.role_values) out[n++] = role;
    written = n;
    return true;
}
bool decode_profile(const uint8_t *bytes, size_t length, profile &out, error &why) noexcept {
    if (!bytes || length != journal_profile_bytes) return fail(why, error_code::wrong_size);
    if (get32(bytes) != profile_version || get32(bytes + 4) != action_count || get32(bytes + 8) != role_count || get32(bytes + 12) != catalog_identity) return fail(why, error_code::wrong_version);
    profile candidate;
    size_t  n = 16;
    for (auto &row : candidate.bindings)
        for (chord &c : row) {
            c = {bytes[n], bytes[n + 1], bytes[n + 2]};
            n += 3;
        }
    for (auto &role : candidate.role_values) role = bytes[n++];
    if (!validate(candidate, why)) return false;
    out = candidate;
    return true;
}
resolver::resolver(const profile &p) noexcept : profile_(&p) {
    static_assert(sizeof(reserved_rows) / sizeof(*reserved_rows) <= 16);
    for (size_t i = 0; i < action_count; ++i) action_contexts_[i] = context_mask(action_rows[i].contexts);
    reserved_count_ = sizeof(reserved_rows) / sizeof(*reserved_rows);
    for (size_t i = 0; i < reserved_count_; ++i) {
        parse_chord(reserved_rows[i].key, reserved_chords_[i]);
        reserved_contexts_[i] = context_mask(reserved_rows[i].contexts);
    }
}
void resolver::release(uint8_t physical, sink emit, void *user) noexcept {
    const uint16_t owner = owners_[physical];
    owners_[physical]    = 0;
    if (!owner) return;
    const size_t action = owner - 1;
    if (counts_[action] && !--counts_[action] && emit) emit(user, action, phase::released);
}
void resolver::clear(sink emit, void *user) noexcept {
    for (unsigned p = 0; p < 256; ++p) {
        release(uint8_t(p), emit, user);
        down_[p] = false;
    }
    modifiers_ = 0;
}
void resolver::set_context(context next, sink emit, void *user) noexcept {
    if (next != context_) {
        clear(emit, user);
        context_ = next;
    }
}
bool resolver::held(size_t action) const noexcept { return action < action_count && counts_[action] != 0; }
bool resolver::process(key_event ev, sink emit, void *user) noexcept {
    modifiers_ = ev.modifiers & 7;
    // Modifier-up can terminate a chord before its main key is released. Ownership survives other drains.
    for (unsigned p = 0; p < 256; ++p)
        if (owners_[p]) {
            size_t action  = owners_[p] - 1;
            bool   matched = false;
            for (chord value : profile_->bindings[action])
                matched = matched || matches(value, uint8_t(p), modifiers_);
            if (!matched) release(uint8_t(p), emit, user);
        }
    if (!ev.physical) return false;
    const bool was_down = down_[ev.physical];
    down_[ev.physical]  = ev.down;
    if (!ev.down) {
        bool claimed = owners_[ev.physical] != 0;
        release(ev.physical, emit, user);
        return claimed;
    }
    if (context_ == context::none) return false;
    const uint16_t ctx = uint16_t(1u << unsigned(context_));
    for (size_t i = 0; i < reserved_count_; ++i)
        if ((reserved_contexts_[i] & ctx) && ev.physical == reserved_chords_[i].physical && (modifiers_ & reserved_chords_[i].modifiers) == reserved_chords_[i].modifiers) return false;
    size_t   chosen = action_count;
    unsigned best   = 0;
    for (size_t i = 0; i < action_count; ++i)
        if (action_contexts_[i] & ctx)
            for (chord c : profile_->bindings[i])
                if (matches(c, ev.physical, modifiers_)) {
                    unsigned score = specificity(c);
                    if (chosen == action_count || score > best) {
                        chosen = i;
                        best   = score;
                    }
                }
    if (chosen == action_count) return false;
    const auto &row = action_rows[chosen];
    if (row.type == kind::held) {
        if (!owners_[ev.physical] && !was_down) {
            owners_[ev.physical] = uint16_t(chosen + 1);
            if (!counts_[chosen]++ && emit) emit(user, chosen, phase::pressed);
        }
    } else if ((!was_down && !ev.repeated) || row.repeat) {
        if (emit) emit(user, chosen, phase::pressed);
    }
    return true;
}
bool role_held(const profile &p, role_index role, uint8_t mods) noexcept {
    return size_t(role) < role_count && role_rows[size_t(role)].type == role_kind::modifier && (mods & p.role_values[size_t(role)]) != 0;
}
bool role_button(const profile &p, role_index role, button physical) noexcept {
    return size_t(role) < role_count && role_rows[size_t(role)].type == role_kind::button && p.role_values[size_t(role)] == uint8_t(physical);
}
gesture classify_click(const profile &p, const click_context &c) noexcept {
    if (c.owned_by_ui) return gesture::native;
    const bool select = role_button(p, select_button, c.physical), command = role_button(p, command_button, c.physical);
    if (c.construction) return (select || command) ? gesture::place_or_cancel : gesture::native;
    if (select) return gesture::select;
    if (!command) return gesture::native;
    if (c.selected_storage) return gesture::rally;
    const bool queued     = role_held(p, shift_modifier, c.modifiers),
               patrolling = role_held(p, patrol, c.modifiers),
               confirmed  = role_held(p, confirm, c.modifiers);
    const bool own        = c.hit == target::own_unit || c.hit == target::own_storage || c.hit == target::own_building;
    // The compatibility exception follows effective confirm+queue roles (defaults Ctrl+Shift).
    if (own && confirmed && queued) return gesture::force_attack;
    if (c.special_eligible && (confirmed || (!queued && c.native_double))) return gesture::special_confirm;
    const bool enemy = c.hit == target::enemy_unit || c.hit == target::enemy_main_base || c.hit == target::enemy_building;
    if (queued && enemy && (patrolling || c.alternate_attack)) return gesture::reject_queued_variant;
    if (c.hit == target::terrain && patrolling) return queued ? gesture::patrol_append : gesture::patrol_replace;
    return queued ? gesture::append : gesture::replace;
}
} // namespace mh::strategic_bindings
