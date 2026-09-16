#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The ~21 distinct outward callees this function makes, indirected for offline testability
// (translator brief 3b). `utils_math_trunc` is deliberately absent -- MH_UNAVAILABLE in
// mh_calls.gen.h, reproduced as TU-local inline x87 asm in the .cpp instead (same shape as
// tact_fx_update_projectile.cpp / tact_calc_dir24.cpp).
struct mission_load_calls {
    // ---- keyword/value scanners (the SIBLING originals -- frontier callees, never this batch's own
    // C++ translations of the same functions; translator brief's bit-independence rule) ------------
    int32_t (*cfg_keyword_token_match)(char *line, char *keyword);  // @0x00439103, 75 sites
    double (*mission_parse_float)(char *line, double max_value);    // @0x004391e7, 52 sites
    void (*mission_parse_quoted_string)(char *line, char *out_buf); // @0x00439fa8, 5 sites
    // The out-pointer parameters below carry the committed pointee type (TACT1-P C6, 2026-09-04):
    // mh_calls.gen.h's generated wrapper signature must match a function-pointer member's type
    // exactly (there is no implicit conversion at that point, unlike at an ordinary call site), and
    // the committed prototypes are `int *`/`uint *`/`uchar *`, not `void *` -- classify() used to
    // blunt every non-struct, non-char pointee down to `void *` before the fix that made this
    // sweep necessary.
    int32_t (*mission_parse_keyword_int)(char *line, char *keyword,
                                         int32_t *out_value); // @0x00439749, 5 sites (DISPOSITION only)
    void (*mission_parse_coord_pair)(char *text, uint8_t *out_col,
                                     uint8_t *out_row); // @0x00439382, 5 sites
    int32_t (*mission_parse_command_token)(char *mission_line, int32_t *out_opcode, int32_t *out_arg0,
                                           int32_t *out_arg1, int32_t *out_arg2,
                                           int32_t *out_arg3); // @0x0043a1d9
    int32_t (*mission_parse_disposition_spawn)(char *line, uint32_t *out_col,
                                               uint32_t *out_row);                                  // @0x00439526
    void (*character_parse_frame_table)(char *frames_directive_line, int32_t character_type_index); // @0x0043a649
    void (*door_parse_definition)(char *door_def_line, int32_t door_index);                         // @0x004398eb, 2 sites
    void (*door_apply_to_map)();                                                                    // @0x00439cf5

    // ---- table/gfx side effects -----------------------------------------------------------------
    int32_t (*unit_spawn)(int32_t char_type, int32_t col, int32_t row, uint8_t facing_dir,
                          uint8_t def_stat, int32_t hp_pct); // @0x0042b948, 3 sites (the ORIGINAL)
    int32_t (*unit_enqueue_command)(int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0,
                                    uint16_t arg1, uint16_t arg2, uint16_t arg3); // @0x0042b39d
    void (*map_compute_bounds)();                                                 // @0x0043a06f, after the MAP section
    void (*tlo_palette_convert_565_to_555)();                                     // @0x0043c78c, GROUND
    void (*tlo_shade_table_build_tact)();                                         // @0x0043a7cd, GROUND
    void (*gfx_convert_pixels_565_to_555)(int16_t *sprite_blob);                  // @0x0043c800, PANEL

    // ---- resource/file layer ----------------------------------------------------------------------
    uint8_t *(*get_resource_file_ptr)(char *file_name);  // GetResourseFilePtr @0x004cf069, 4 sites
    uint32_t (*rsr_get_file_real_size)(char *file_name); // @0x004cf15e
    void (*utils_free)(void *ptr);                       // @0x004d0244, 2 sites
    int32_t (*utils_str_cmp)(char *a, char *b);          // @0x004d16d0, PANEL

    // ---- fatal/abort path (STOP; GROUND's missing-file check; five duplicate-record-id checks) ----
    void (*llm_fatal_cleanup)();         // @0x0042613b, 7 sites
    void (*utils_abort)(int32_t status); // @0x004da944, 7 sites (always paired with the above)
};

const mission_load_calls &live_mission_load_calls();

namespace detail {

// llm_tact_mission_load @0x0043717f. See the file banner above for the tail/dead-store/overrun
// findings; see the .cpp for the section-by-section address citations.
void mission_load(const tact_view &tv, tact_store &own, const mission_load_calls &c, char *filename);

} // namespace detail

void mission_load(char *filename);


} // namespace mh::tact
