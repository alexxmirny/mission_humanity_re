#pragma once
#include <cstddef>
#include <cstdint>
#include "mh_strategic_binding_ids.gen.h"

namespace mh::strategic_bindings {
constexpr uint32_t profile_version = 2;
constexpr size_t   binding_slots   = 4;
// Reserve two bytes in the 96-byte resolved INI reader for NUL and unambiguous truncation detection.
constexpr size_t max_spec_bytes = 94;
enum modifier : uint8_t { ctrl  = 1,
                          alt   = 2,
                          shift = 4 };
enum class context : uint8_t { none,
                               main,
                               game_menu,
                               options,
                               info,
                               planet,
                               saveload,
                               diplomacy,
                               tutorial,
                               outcome,
                               wait };
enum class phase : uint8_t { pressed,
                             released };
enum class kind : uint8_t { event,
                            held };
enum class role_kind : uint8_t { modifier,
                                 button };
enum class button : uint8_t { left  = 1,
                              right = 2 };
// Identity is set-1 make scan plus E0 in bit7, NEVER the native masked scan alone.
struct chord {
    uint8_t physical = 0, modifiers = 0, ignored = 0;
};
constexpr bool operator==(chord a, chord b) { return a.physical == b.physical && a.modifiers == b.modifiers && a.ignored == b.ignored; }
struct action_row {
    const char *id, *label_en, *label_ru, *category, *contexts;
    kind        type;
    const char *primary, *alternate, *section, *key, *alt_key;
    uint8_t     ignored_modifiers, native_scan, native_modifiers;
    bool        repeat;
    bool        editable;
    const char *third = "none", *fourth = "none";
    const char *third_key = "", *fourth_key = "";
};
struct role_row {
    const char *id, *label_en, *label_ru, *default_value, *section, *key;
    role_kind   type;
};
struct key_row {
    const char *token;
    uint8_t     scan;
    bool        e0;
    uint16_t    vk;
};
struct reserved_row {
    const char *key, *contexts, *reason;
};
#define MH_STRATEGIC_ACTION(id, en, ru, cat, ctx, type, pri, sec, section, key, altkey, ignored, scan, mods, repeat, ...) +1
#define MH_STRATEGIC_ROLE(...)
#define MH_STRATEGIC_KEY(...)
#define MH_STRATEGIC_RESERVED(...)
#define MH_STRATEGIC_EXCLUSIVE(...)
constexpr size_t action_count = 0
#include "mh_strategic_bindings.def"
    ;
#undef MH_STRATEGIC_ACTION
#undef MH_STRATEGIC_ROLE
#undef MH_STRATEGIC_KEY
#undef MH_STRATEGIC_RESERVED
#undef MH_STRATEGIC_EXCLUSIVE
constexpr size_t    role_count = 6;
const action_row   *actions() noexcept;
const role_row     *roles() noexcept;
const key_row      *keys(size_t &count) noexcept;
const reserved_row *reserved(size_t &count) noexcept;
const char         *context_name(context ctx) noexcept;
bool                contexts_overlap(const char *a, const char *b) noexcept;
bool                parse_chord(const char *text, chord &out) noexcept;
bool                parse_role(const char *text, role_kind type, uint8_t &out) noexcept;
// Native keystate stores base and E0 twins in different bits; either-side modifiers are levels.
uint8_t native_modifiers(const uint8_t *state128) noexcept;
uint8_t native_physical(uint32_t scan, uint32_t event_type) noexcept;
struct profile {
    chord   bindings[action_count][binding_slots] = {};
    uint8_t role_values[role_count]               = {};
};
enum class error_code : uint8_t { none,
                                  malformed,
                                  reserved,
                                  collision,
                                  role_collision,
                                  wrong_version,
                                  wrong_size,
                                  migration_conflict,
                                  fixed_binding };
struct error {
    error_code  code   = error_code::none;
    size_t      action = 0, slot = 0, other = 0;
    const char *setting = nullptr;
};
// Reader must provide the already-resolved local INI value or the supplied default, NUL terminated.
// Return false on truncation/read failure. No file IO or globals in the common resolver.
using reader = bool (*)(void *, const char *, const char *, const char *, char *, size_t);
bool defaults(profile &out, error &why) noexcept;
// Import reads are separate so retired player keys are not hidden by a canonical-key gate.
// Only recognized v1 settings use this reader, and never after a canonical v2 marker.
bool     load(profile &out, reader read, void *user, error &why, reader import_read = nullptr) noexcept;
bool     validate(const profile &candidate, error &why) noexcept;
uint32_t fingerprint(const profile &p) noexcept;
// UI journal owns these exact effective bytes and version, not a hash of a mutable local file.
constexpr size_t journal_profile_bytes = 16 + action_count * binding_slots * 3 + role_count;
bool             encode_profile(const profile &p, uint8_t *out, size_t capacity, size_t &written) noexcept;
bool             decode_profile(const uint8_t *bytes, size_t length, profile &out, error &why) noexcept;
using sink = void (*)(void *, size_t action, phase event);
struct key_event {
    uint8_t physical = 0, modifiers = 0;
    bool    down = false, repeated = false;
};
class resolver {
public:
    explicit resolver(const profile &p) noexcept;
    // Always call on context/focus transition, including transition to text/tactical/ImGui/none.
    void set_context(context next, sink emit, void *user) noexcept;
    bool process(key_event ev, sink emit, void *user) noexcept;
    bool held(size_t action) const noexcept;
    void clear(sink emit, void *user) noexcept;

private:
    const profile *profile_;
    context        context_                       = context::none;
    uint16_t       owners_[256]                   = {}; // action+1, held only; no allocations, bounded at setup.
    uint8_t        counts_[action_count]          = {};
    bool           down_[256]                     = {};
    uint8_t        modifiers_                     = 0;
    uint16_t       action_contexts_[action_count] = {};
    uint16_t       reserved_contexts_[16]         = {};
    chord          reserved_chords_[16]           = {};
    size_t         reserved_count_                = 0;
    void           release(uint8_t physical, sink emit, void *user) noexcept;
};
enum role_index : size_t { shift_modifier,
                           patrol,
                           confirm,
                           toggle_selection,
                           select_button,
                           command_button };
bool role_held(const profile &p, role_index role, uint8_t mods) noexcept;
bool role_button(const profile &p, role_index role, button physical) noexcept;
enum class surface : uint8_t { world,
                               minimap,
                               bigmap };
enum class target : uint8_t { terrain,
                              own_unit,
                              own_storage,
                              own_building,
                              enemy_unit,
                              enemy_main_base,
                              enemy_building,
                              unresolved };
enum class gesture : uint8_t { native,
                               select,
                               place_or_cancel,
                               rally,
                               replace,
                               append,
                               patrol_replace,
                               patrol_append,
                               special_confirm,
                               force_attack,
                               reject_queued_variant };
struct click_context {
    surface where       = surface::world;
    target  hit         = target::terrain;
    button  physical    = button::right;
    uint8_t modifiers   = 0;
    bool    owned_by_ui = false, construction = false, selected_storage = false;
    bool    special_eligible = false, native_double = false, alternate_attack = false;
};
// Classifies input only. Actor/target eligibility/lifetime, ordering/admission and feedback stay canonical.
gesture classify_click(const profile &p, const click_context &click) noexcept;
} // namespace mh::strategic_bindings
