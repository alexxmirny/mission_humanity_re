//
// seams/ui_info_guard.h -- mp:U73: the PURE half of the entity-info-screen guard, the part a selftest
// can drive without a game (mh_nettest/info_guard_selftest.cpp, `net_selftest.exe infoguardtest`).
//
// The seam (ui_info_guard.cpp) hooks the ENTRY of llm_ui_entity_info_screen_open (EN 0x004cad31) and
// splices the storage panel's info-request consume (EN 0x00417902). Mechanism and evidence:
// include/mh_infoguard_export.h. What lives here is the decision: which (kind, entity_id, key)
// triples the retail function can turn into a screen, and which it can only turn into the
// "Error: Cannot find info text:" modal.
//
#pragma once
#include <cstddef>
#include <cstdint>

namespace mh::info_guard {

// llm_ui_info_kind
enum : uint32_t { KIND_UNIT     = 0,
                  KIND_BUILDING = 1,
                  KIND_PROJECT  = 2 };

// cfg_enum_E_INVETION_TYPE members the PROJECT fallback reads.
enum : uint8_t { INV_UNDEFINED = 0,
                 INV_BUILDING  = 1,
                 INV_UNIT      = 2 };

// Unit[100] / Building[100] / Projects[100]: the cfg record tables are 100 deep (measured from the
// DB: cfg_final_struct_{Unit,Building,Project}[100]), and every record's info_txt is char[64].
inline constexpr int32_t CFG_TABLE_DEPTH = 100;
inline constexpr size_t  INFO_KEY_BYTES  = 64;

enum class verdict : uint8_t {
    ok = 0,
    kind_range,       // kind is not UNIT/BUILDING/PROJECT: retail falls through all three arms and reads
                      // an uninitialised key
    id_range,         // entity_id outside the configured record table
    key_empty,        // the resolved INFO_TXT key is "" -- the field modal, with an empty key
    key_unterminated, // 64 bytes without a NUL: not a key retail's own strcpy could have stored
    key_not_indexed,  // non-empty, but absent from the INFO.Txt index: retail's modal with a key
};

inline const char *verdict_name(verdict v) {
    switch (v) {
        case verdict::ok: return "ok";
        case verdict::kind_range: return "kind-out-of-range";
        case verdict::id_range: return "id-out-of-range";
        case verdict::key_empty: return "key-empty";
        case verdict::key_unterminated: return "key-unterminated";
        case verdict::key_not_indexed: return "key-not-in-info-txt";
    }
    return "?";
}

// `total` is the cfg section's own .total (G_BUILDING_COUNT_TOTAL / G_UNIT_COUNT_TOTAL, the number of
// types the loaded cfg defines; the AI scans it as an INCLUSIVE bound, so ids run 1..total). 0 means
// "unknown / not loaded": only the table depth applies then.
inline bool id_in_range(uint32_t kind, int32_t id, int32_t total) {
    if (id < 0 || id >= CFG_TABLE_DEPTH) return false;
    if ((kind == KIND_UNIT || kind == KIND_BUILDING) && total > 0 && total < CFG_TABLE_DEPTH && id > total)
        return false;
    return true;
}

// The key's own shape: non-empty, NUL inside the 64-byte field.
inline verdict key_shape(const char *key) {
    if (key[0] == '\0') return verdict::key_empty;
    for (size_t i = 0; i < INFO_KEY_BYTES; ++i)
        if (key[i] == '\0') return verdict::ok;
    return verdict::key_unterminated;
}

// The storage panel's request: the widget callback latched `request` (= row_index + 1, 1-based) and
// the panel adds the list's scroll offset at CONSUME time, then reads docked_units[row - 1]. The row
// is usable only while it names a live entry of the docked list as it is NOW, and that entry names a
// unit with a configured prototype. `docked_count` is clamped to the 50-entry list.
enum class storage_row : uint8_t { ok = 0,
                                   row_past_list,
                                   unit_slot_range,
                                   unit_proto_blank };

inline storage_row storage_request_check(int32_t request, int32_t scroll, int32_t docked_count,
                                         int32_t docked_unit_id, int32_t per_player_units,
                                         uint32_t unit_proto_id) {
    const int32_t row = request + scroll;
    const int32_t n   = docked_count < 0 ? 0 : (docked_count > 50 ? 50 : docked_count);
    if (row < 1 || row > n) return storage_row::row_past_list;
    if (docked_unit_id < 0 || docked_unit_id >= per_player_units) return storage_row::unit_slot_range;
    if (unit_proto_id == 0) return storage_row::unit_proto_blank;
    return storage_row::ok;
}

inline const char *storage_row_name(storage_row v) {
    switch (v) {
        case storage_row::ok: return "ok";
        case storage_row::row_past_list: return "row-past-docked-list";
        case storage_row::unit_slot_range: return "docked-slot-out-of-range";
        case storage_row::unit_proto_blank: return "docked-unit-proto-blank";
    }
    return "?";
}

// `[net] info_guard_test_*` -- the rig's knobs. `n` is the 1-based call to corrupt, 0 = off.
inline bool test_hit(int32_t n, int32_t &counter) {
    if (n <= 0) return false;
    return ++counter == n;
}

} // namespace mh::info_guard
