//
// tact/tact_mission_load.cpp -- see tact_mission_load.h for the file banner (the dead TIME/tail
// finding, the two uninitialised-stack reads, the second overrun beyond the one the batch brief
// named, and the full DECLARED NEEDS list). Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_mission_load_0043717f.asm), section by section; the exported .c draft's
// control flow and numeric literals cross-check cleanly against it (75 cfg_keyword_token_match sites,
// 52 mission_parse_float sites, 40 utils_math_trunc sites -- all three counts independently reconciled
// against the asm during translation), but its EOF tail is incomplete (see the header banner) and it
// does not surface the EXPLOSION section's stale-index bug documented at that section below.
//
#include "tact/tact_mission_load.h"

#include <cstdint>
#include <cstring>

#include "addr/mh_calls.gen.h"     // frontier callees
#include "state/mode_planes.h"     // mh::state::PASSABLE_BLOCKED
#include "tact/tact_cfg_keyword.h" // detail::defense_stance -- rule 17a: an existing enum for the
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_enqueue_command.h"
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"         // CRT-X87: the shared x87 truncation helpers
#include "fp/x87_shapes.h"  // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
// DISPOSITION line's DEFENSE:* domain, used instead of local ints

namespace mh::tact {

const mission_load_calls &live_mission_load_calls() {
    static const mission_load_calls c = {
        MH_LIBMH_BIND(llm_tact_cfg_keyword_token_match),
        MH_LIBMH_BIND(llm_tact_mission_parse_float),
        MH_LIBMH_BIND(llm_tact_mission_parse_quoted_string),
        // The four parse rows bind since TACT1-P C6 (2026-09-04) -- they were pinned to the
        // original while sig_<fn> spelled their out-params `void *`, which was the generator
        // blunting the committed `int *`/`uint *`/`uchar *`, not the ABI.
        MH_LIBMH_BIND(llm_tact_mission_parse_keyword_int),
        MH_LIBMH_BIND(llm_tact_mission_parse_coord_pair),
        MH_LIBMH_BIND(llm_tact_mission_parse_command_token),
        MH_LIBMH_BIND(llm_tact_mission_parse_disposition_spawn),
        MH_LIBMH_BIND(llm_tact_character_parse_frame_table),
        MH_LIBMH_BIND(llm_tact_door_parse_definition),
        MH_LIBMH_BIND(llm_tact_door_apply_to_map),
        MH_LIBMH_BIND(llm_tact_unit_spawn),
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command),
        MH_LIBMH_BIND(llm_tact_map_compute_bounds),
        mh::tact_host().llm_tlo_palette_convert_565_to_555,
        mh::tact_host().llm_tlo_shade_table_build_tact,
        mh::tact_host().llm_gfx_convert_pixels_565_to_555,
        mh::tact_host().GetResourseFilePtr,
        mh::tact_host().rsr_GetFileRealSize,
        MH_CRT(utils_free),
        MH_CRT(utils_str_cmp),
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
    };
    return c;
}

namespace {

// ---- utils_math_trunc @0x004d0596, reproduced as inline x87 asm (MH_UNAVAILABLE in mh_calls.gen.h)
// -- same shape as tact_fx_update_projectile.cpp / tact_calc_dir24.cpp. This function needs only the
// plain-truncate and truncate-of-a-sum shapes (40 sites total: see the file banner's count
// reconciliation); no mul/div/sub shape appears anywhere in the disassembly.

int32_t trunc_only(double x) {
    return ::mh::fp::trunc_i32(x);
}

// trunc(a + b) -- every FIRST_FRAME site (bank sprite base + parsed value) and the three bias-add
// sites (COLISION1/COLISION2/TELEPORT DEATH).
int32_t trunc_add(double a, double b) {
    return ::mh::fp::trunc_add(a, b);
}

// ---- the keyword table, BYTE-VERIFIED (conductor, ReVA read-memory 0x005004f0+720, 2026-08-26; see
// tmp/decomp_tact/_KW_TABLE.md). Trailing spaces are load-bearing token-match behaviour -- NOT
// trimmed. Only the subset this function actually tests is defined here (66 of the table's 73
// entries; MOVE/WALK/WAIT/TELE/DEFENSE:SNIPER belong to the sibling
// llm_tact_mission_parse_command_token, not to this function).
static char KW_STOP[]             = "STOP";             // @0x005004f0
static char KW_TIME[]             = "TIME ";            // @0x005004f5
static char KW_SEE_ENEMY[]        = "SEE ENEMY";        // @0x005004fb
static char KW_MAP[]              = "MAP";              // @0x00500505
static char KW_BANK[]             = "BANK";             // @0x00500509
static char KW_GROUND[]           = "GROUND";           // @0x0050050e
static char KW_CHARACTER[]        = "CHARACTER";        // @0x00500515
static char KW_GUN[]              = "GUN ";             // @0x0050051f
static char KW_EXPLOSION[]        = "EXPLOSION";        // @0x00500524
static char KW_DISPOSITION[]      = "DISPOSITION";      // @0x0050052e
static char KW_DOOR[]             = "DOOR";             // @0x0050053a
static char KW_TELEPORT[]         = "TELEPORT ";        // @0x0050053f
static char KW_DETONATION[]       = "DETONATION";       // @0x00500549
static char KW_QUIT[]             = "QUIT ";            // @0x00500554
static char KW_TARGET[]           = "TARGET ";          // @0x0050055a
static char KW_NAME[]             = "NAME";             // @0x00500562
static char KW_PANEL[]            = "PANEL";            // @0x00500567
static char KW_WHO[]              = "WHO";              // @0x00500593
static char KW_ANGLE_SEE[]        = "ANGLE SEE";        // @0x00500597
static char KW_DISTANCE_SEE[]     = "DISTANCE SEE";     // @0x005005a1
static char KW_FIRST_FRAME[]      = "FIRST FRAME";      // @0x005005ae
static char KW_NUMBER_GUN1[]      = "NUMBER GUN1";      // @0x005005ba
static char KW_NUMBER_GUN2[]      = "NUMBER GUN2";      // @0x005005c6
static char KW_HEIGHT_GUN1[]      = "HEIGHT GUN1";      // @0x005005d2
static char KW_HEIGHT_GUN2[]      = "HEIGHT GUN2";      // @0x005005de
static char KW_KNEEL_GUN1[]       = "KNEEL GUN1";       // @0x005005ea
static char KW_KNEEL_GUN2[]       = "KNEEL GUN2";       // @0x005005f5
static char KW_ENERGY[]           = "ENERGY";           // @0x00500600
static char KW_SPEED[]            = "SPEED";            // @0x00500607
static char KW_ROTATE[]           = "ROTATE";           // @0x0050060d
static char KW_KNEEL[]            = "KNEEL  ";          // @0x00500614
static char KW_DEATH[]            = "DEATH";            // @0x0050061c
static char KW_MINE[]             = "MINE";             // @0x00500622
static char KW_RUN[]              = "RUN";              // @0x00500627
static char KW_FRAMES[]           = "FRAMES";           // @0x0050062b
static char KW_SPEED_GUN[]        = "SPEED  ";          // @0x00500632
static char KW_SPEED_FIRE[]       = "SPEED FIRE";       // @0x0050063a
static char KW_BULLETS[]          = "BULLETS";          // @0x00500645
static char KW_REPEAT[]           = "REPEAT";           // @0x0050064d
static char KW_MAGAZINES[]        = "MAGAZINES";        // @0x00500654
static char KW_PRECISE[]          = "PRECISE  ";        // @0x0050065e
static char KW_PRECISE_KNEEL[]    = "PRECISE KNEEL";    // @0x00500668
static char KW_DIRECT[]           = "DIRECT";           // @0x00500676
static char KW_POWER[]            = "POWER";            // @0x0050067d
static char KW_COLISION1[]        = "COLISION1";        // @0x00500683
static char KW_COLISION2[]        = "COLISION2";        // @0x00500695
static char KW_RANGE_MAX[]        = "RANGE_MAX";        // @0x005006a7
static char KW_RANGE_MIN[]        = "RANGE_MIN";        // @0x005006b1
static char KW_SOUND[]            = "SOUND";            // @0x005006bb
static char KW_RANGE_KILL[]       = "RANGE_KILL";       // @0x005006c1
static char KW_DIRECT_COLON[]     = "DIRECT:";          // @0x005006cc
static char KW_DEFENSE_NONE[]     = "DEFENSE:NONE";     // @0x005006d4
static char KW_DEFENSE_GUARD1[]   = "DEFENSE:GUARD1";   // @0x005006e1
static char KW_DEFENSE_GUARD2[]   = "DEFENSE:GUARD2";   // @0x005006f0
static char KW_DEFENSE_ATTACK[]   = "DEFENSE:ATTACK";   // @0x005006ff
static char KW_DIRECT_LEFTRIGHT[] = "DIRECT LeftRight"; // @0x0050070e
static char KW_DIRECT_RIGHTLEFT[] = "DIRECT RightLeft"; // @0x0050071f
static char KW_DIRECT_NORMAL[]    = "DIRECT Normal";    // @0x00500730
static char KW_LEFT[]             = "LEFT";             // @0x0050073e
static char KW_RIGHT[]            = "RIGHT";            // @0x00500743
static char KW_XY[]               = "X,Y";              // @0x00500749
static char KW_RANDOM[]           = "RANDOM";           // @0x0050074d
static char KW_SEQUENCE[]         = "SEQUENCE";         // @0x00500754
static char KW_NO_ENEMY[]         = "NO_ENEMY";         // @0x0050075d
static char KW_START[]            = "START";            // @0x00500766
static char KW_WHERE[]            = "WHERE";            // @0x0050076c
static char KW_ASSOCIATION[]      = "ASSOCIATION";      // @0x00500772
static char KW_TIMER[]            = "TIMER";            // @0x00500786

// ---- the PANEL keyword's fixed fallback path string, NOT byte-verified by the conductor the way the
// KW table above was (Ghidra's own string decompile only, per rule 17b's caveat -- flagged in
// uncertainties). 0x00437d55: `s_panelb\ilp_kom.gfx_0050056d`, the value PANEL compares the parsed
// string against; 0x00437d74: `s_panelb\ilp_typ.gfx_00500580`, the 19-byte (18 chars + NUL)
// replacement copied in verbatim (4-dword REP MOVSD + 1 MOVSW + 1 MOVSB @0x00437d79-0x00437d7d --
// 16+2+1 = 19 bytes, matching "panelb\ilp_typ.gfx\0" exactly).
inline constexpr const char *kPanelKomPath = "panelb\\ilp_kom.gfx"; // s_panelb\ilp_kom.gfx_0050056d
inline constexpr const char *kPanelTypPath = "panelb\\ilp_typ.gfx"; // s_panelb\ilp_typ.gfx_00500580

} // namespace

namespace detail {

void mission_load(const tact_view &tv, tact_store &own, const mission_load_calls &c, char *filename) {
    // ---- clear the runtime tables, @0x00437197-0x0043743a --------------------------------------
    //
    // The @0x00437201 pre-loop write of _G_LLM_TACT_UNITS[0].type = 0xff is transcribed for
    // fidelity even though the loop immediately below overwrites index 0 again on its own first
    // pass (i==0) -- it has zero net effect, but Law 2 preserves the original's instruction
    // sequence rather than silently dropping a store that turns out to be immediately clobbered.
    own.unit_at(0).type = 0xff; // @0x00437201

    for (int32_t i = 0; i <= 0x80; ++i) { // JLE @0x0043721c -- 129 iterations, matches TACT_UNIT_SLOTS
        auto &u    = own.unit_at(i);
        u.type     = 0;
        u.pos_col  = 0;
        u.pos_row  = 0;
        u.progress = 0;
    }
    // SECOND CONFIRMED ONE-RECORD OVERRUN (beyond the DOOR_TABLE one the batch brief named -- see the
    // header banner): JLE @0x00437288 means indices 0..1024 INCLUSIVE against a 1024-slot region.
    // own.fx_at(1024) genuinely indexes one mh_llm_tact_fx record past the declared array, matching
    // the original bit-for-bit.
    for (int32_t i = 0; i <= 0x400; ++i) { // JLE @0x00437288
        own.fx_at(i).fx_type = 0;
    }
    for (int32_t i = 0; i < 0x10; ++i) own.character_type_at(i).id = 0; // JL @0x004372bb, exact 16
    for (int32_t i = 0; i < 0x40; ++i) own.fx_type_table_at(i).id = 0;  // JL @0x004372ee, exact 64
    // DOOR_TABLE's overrun (the one the batch brief flagged): JLE @0x00437321 -- 17 iterations
    // against a 16-slot/0x68-stride region. own.door_table_at(16) is one record past the array.
    for (int32_t i = 0; i <= 0x10; ++i) own.door_table_at(i).id = 0; // JLE @0x00437321
    for (int32_t i = 0; i < 0x42; ++i) {                             // JL @0x00437354, exact 66
        auto &t     = own.teleport_zone_at(i);
        t.id        = 0;
        t.mode      = 0;
        t.field_03  = 0;
        t.no_enemy  = 0;
        t.start_col = 0;
        t.start_row = 0;
        for (int32_t k = 0; k < 8; ++k) {
            t.dest_col[k]    = 0;
            t.dest_row[k]    = 0;
            t.association[k] = 0;
        }
        t.field_28 = 0;
        t.death    = 0;
    }
    own.squad_size()  = 0; // @0x0043743a
    own.enemy_count() = 0; // @0x00437444

    // ---- open the mission file, @0x0043744e-0x0043746a -----------------------------------------
    void          *file_data = c.get_resource_file_ptr(filename);
    char          *file_text = static_cast<char *>(file_data);
    const uint32_t file_size = c.rsr_get_file_real_size(filename);
    uint32_t       read_pos  = 0;

    // ---- persistent state (function-scope, matching the original's single 0x89d4-byte frame) ----
    //
    // The prologue (0x0043719d-0x004371fa) zeroes THIRTEEN dword slots -- including
    // bank_sprite_base_current (EBP-0x90), which the first draft of this banner miscounted as
    // uninitialised. The genuinely-NOT-prologue-zeroed slots this function later READS are
    // map_scratch_idx (EBP-0x80, GROUND's aliased check -- see that arm) and the two persistent
    // DISPOSITION spawn parameters (facing_dir/def_stat, EBP-0x48/-0x44) -- see the header
    // banner's "GENUINELY-UNINITIALISED STACK READS" section (corrected by the adversarial
    // review, wf_d34b0eeb-a5b) for why each gets a deterministic start here.
    int32_t section = 0;                  // @EBP-0x78 -- 0=global, 1..7=CHARACTER/GUN/EXPLOSION/DISPOSITION/
                                          // DOOR/TELEPORT/DETONATION
    int32_t char_idx                 = 0; // @EBP-0x74
    int32_t gun_idx                  = 0; // @EBP-0x70
    int32_t expl_idx                 = 0; // @EBP-0x6c
    int32_t door_idx                 = 0; // @EBP-0x68
    int32_t tele_idx                 = 0; // @EBP-0x64
    int32_t spawned_unit_id          = 0; // @EBP-0x40 -- the DISPOSITION section's "unit just spawned" id
    int32_t bank_sprite_base_current = 0; // @EBP-0x90 -- BANK's own result. Prologue-ZEROED by the
                                          // original too (MOV dword [EBP-0x90],0 @0x0043719d); the
                                          // adversarial review corrected the first banner's
                                          // "uninitialised" claim -- this zero MATCHES the original.
    int32_t map_scratch_idx = 0;          // @EBP-0x80 -- MAP's persistent row counter, genuinely NOT
                                          // prologue-zeroed in the original. Deterministic zero here;
                                          // the GROUND check that consumed it stale is written via
                                          // its ALIASING semantics instead (see the GROUND arm).
    // The DISPOSITION spawn parameters are PERSISTENT across lines: a spawn line without
    // DIRECT:/DEFENSE:* INHERITS the previous line's values (writes are keyword-gated
    // @0x004388ee/0x00438904..0x00438967; only the range clamps run per line). Neither slot is
    // prologue-zeroed in the original -- these defaults are the same deterministic stand-in
    // map_scratch_idx gets, chosen to equal the clamps' own out-of-range fallbacks (1 / 0).
    int32_t facing_dir  = 1;     // @EBP-0x48
    int32_t def_stat    = 0;     // @EBP-0x44
    int32_t line_number = 0;     // @EBP-0x94 -- incremented, never read again; kept for fidelity
    bool    in_comment  = false; // @EBP-0x98 -- the /* */ tracker

    // The line buffer at its REAL 128-byte extent. (An earlier draft padded it to 136 to make the
    // GROUND check's stale-index read defined C++; the adversarial review proved that read is an
    // ALIASED read of the ground-texture name's first byte, now written that way, so the padding
    // is gone.)
    char line[128] = {};

    // MAP's own scratch buffers, sized to the exact byte counts the three real memcpy calls move
    // (@0x00437706-0x0043776b -- each REP MOVSD+MOVSB pair's trailing MOVSB runs zero times because
    // all three counts are multiples of 4, exactly like tact_mission_start.cpp's framebuffer copy).
    uint8_t  passable_bitmap[0x800] = {}; // local_9bc -- row-major (row*0x80+col) BIT map
    uint16_t tile_type_grid[0x4000] = {}; // local_89bc -- row-major (row*0x80+col) tile-type shorts

    for (;;) {
        // ---- EOF check, @0x00437471-0x00437477 --------------------------------------------------
        if (read_pos >= file_size) {
            c.utils_free(file_data);
            return;
            // See the header banner's "THE .c DRAFT ALREADY LIED HERE" section: the real assembly
            // clamps the (already-dead) TIME-keyword local into [1,10000] AFTER this free and
            // BEFORE the epilogue, but never stores it anywhere observable -- RET follows with no
            // intervening EAX load. Reproducing that clamp here would be dead code with zero
            // behavioural difference, so it is documented rather than transcribed.
        }

        // ---- read one CR-terminated line, dropping the CR/LF pair, @0x00437484-0x004374d4 -------
        int32_t line_len = 0;
        for (; line_len < static_cast<int32_t>(file_size - read_pos); ++line_len) {
            if (file_text[read_pos + line_len] == 0x0d) {
                line[line_len] = 0;
                read_pos += line_len + 2; // skip the CR and the LF right after it
                goto line_ready;
            }
            line[line_len] = file_text[read_pos + line_len];
        }
        // Ran off the end of the file without ever finding a CR: read_pos is NOT advanced and
        // `line` is NOT NUL-terminated by this pass, exactly as the original does -- no real POZ
        // file lacks a trailing CR on its last line, so this path is believed unreached; see
        // uncertainties.
    line_ready:
        ++line_number; // @0x004374d4 -- dead (see above), kept for fidelity

        // ---- /* */ comment tracking, @0x004374e0-0x0043751c -------------------------------------
        if (line[0] == '/' && line[1] == '*') in_comment = true;  // @0x004374e0-0x004374f4
        if (line[0] == '*' && line[1] == '/') in_comment = false; // @0x004374fe-0x00437512
        if (in_comment) continue;                                 // @0x0043751c-0x00438edc (the outer do-while's `while (local_9c==1)`)

        // ================================================================================
        // GLOBAL SECTION (@0x00437529-0x00438e02): one keyword resolves the whole line, in this
        // exact tested order (rule: call order is behaviour). Falling through all 15 tests without a
        // match drops into the per-record-section dispatch below, guarded by `section`.
        // ================================================================================
        if (c.cfg_keyword_token_match(line, KW_STOP) == 0) { // @0x00437529
            c.llm_fatal_cleanup();
            c.utils_abort(0);
        } else if (c.cfg_keyword_token_match(line, KW_TIME) == 0) { // @0x00437549
            // See the header banner's "THE TIME KEYWORD'S TARGET IS DEAD" section: the parse+trunc
            // both genuinely happen (the oracle can assert the TIME line was consumed correctly),
            // but the result is provably never read by anything in this function.
            const double parsed = c.mission_parse_float(line, 1000.0);
            (void)trunc_only(parsed);                                    // @0x00437569-0x00437577 -- result discarded, see banner
        } else if (c.cfg_keyword_token_match(line, KW_SEE_ENEMY) == 0) { // @0x0043757c
            own.see_enemy_flag() = 1;                                    // @0x00437590
        } else if (c.cfg_keyword_token_match(line, KW_MAP) == 0) {       // @0x0043759f
            // ---- MAP: load the 128x128 ground/height/passable data, @0x004375b7-0x0043794f ------
            char map_name[64] = {};
            c.mission_parse_quoted_string(line, map_name); // @0x004375b7-0x004375c3

            uint16_t *height_sprites = own.map_tile_height_sprites();

            // Pre-clear the top-left 128x128 sub-block's 8 height-sprite slots and its
            // tile_objects record, @0x004375c8-0x004376f8. `map_scratch_idx` is the row/outer
            // counter, deliberately the SAME persistent slot GROUND reads stale later on -- see
            // the header banner.
            for (map_scratch_idx = 0; map_scratch_idx < 0x80; ++map_scratch_idx) { // row
                for (int32_t col = 0; col < 0x80; ++col) {
                    for (int32_t slot = 0; slot < 8; ++slot) {
                        height_sprites[col * 1024 + map_scratch_idx * 8 + slot] = 0;
                    }
                    auto &t       = own.planes().tile_object_at(col, map_scratch_idx);
                    t.flags[0]    = 0;
                    t.flags[1]    = 0;
                    t.building    = 0;
                    t.unit[0]     = 0;
                    t.unit[1]     = 0;
                    t.class_owner = 0;
                    t.visibility  = 0;
                }
            }

            // Load the ground-map resource and copy its three sections -- height sprites
            // (0x40000 B), the passable bitmap (0x800 B), the tile-type grid (0x8000 B) --
            // @0x004376f8-0x0043776c. All three REP MOVSD+MOVSB pairs' trailing MOVSB runs zero
            // times (each count is a multiple of 4); folded here into one memcpy each, matching
            // tact_mission_start.cpp's identical pattern.
            void    *map_res   = c.get_resource_file_ptr(map_name);
            uint8_t *map_bytes = static_cast<uint8_t *>(map_res);
            std::memcpy(height_sprites, map_bytes, 0x40000u);
            std::memcpy(passable_bitmap, map_bytes + 0x40000, 0x800u);
            std::memcpy(tile_type_grid, map_bytes + 0x40800, 0x8000u);
            c.utils_free(map_res); // @0x0043776c-0x00437774

            // Apply the passable bitmap (row-major bit i = col + row*0x80 in the RAW file data,
            // written into the column-major passable_at(col,row)) and subtract 100 from every
            // nonzero height-sprite slot, @0x00437774-0x00437877.
            for (map_scratch_idx = 0; map_scratch_idx < 0x80; ++map_scratch_idx) { // row
                for (int32_t col = 0; col < 0x80; ++col) {
                    const int32_t bit_index = col + map_scratch_idx * 0x80;
                    if ((passable_bitmap[bit_index >> 3] >> (bit_index & 7)) & 1u) {
                        own.planes().passable_at(col, map_scratch_idx) = mh::state::PASSABLE_BLOCKED;
                    }
                    for (int32_t slot = 0; slot < 8; ++slot) {
                        uint16_t &h = height_sprites[col * 1024 + map_scratch_idx * 8 + slot];
                        if (h != 0) h = static_cast<uint16_t>(h - 100);
                    }
                }
            }

            // Reset the FULL 256x256 shared grid's flags word to 0x4000 (not just the tactical
            // 128x128 sub-block), @0x0043787c-0x004378da.
            for (map_scratch_idx = 0; map_scratch_idx < 0x100; ++map_scratch_idx) { // row
                for (int32_t col = 0; col < 0x100; ++col) {
                    auto &t    = own.planes().tile_object_at(col, map_scratch_idx);
                    t.flags[0] = 0x00;
                    t.flags[1] = 0x40; // word 0x4000, little-endian
                }
            }

            // Apply the tile-type grid's own flags encoding: low byte verbatim, high byte biased
            // by 0x40 ('@'), over the top-left 128x128 sub-block, @0x004378da-0x0043794f. This is
            // the loop that leaves `map_scratch_idx` at exactly 0x80 for GROUND's later stale read.
            for (map_scratch_idx = 0; map_scratch_idx < 0x80; ++map_scratch_idx) { // row
                for (int32_t col = 0; col < 0x80; ++col) {
                    const uint16_t raw = tile_type_grid[map_scratch_idx * 0x80 + col];
                    auto          &t   = own.planes().tile_object_at(col, map_scratch_idx);
                    t.flags[0]         = static_cast<uint8_t>(raw & 0xFFu);
                    t.flags[1]         = static_cast<uint8_t>((raw >> 8) + 0x40);
                }
            }

            c.map_compute_bounds();                                 // @0x0043794f
        } else if (c.cfg_keyword_token_match(line, KW_BANK) == 0) { // @0x00437959
            const double  parsed     = c.mission_parse_float(line, 99.0);
            const int32_t idx        = trunc_only(parsed);            // @0x0043797f-0x00437984
            bank_sprite_base_current = own.bank_sprite_base_at(idx);  // @0x00437993-0x00437999
        } else if (c.cfg_keyword_token_match(line, KW_GROUND) == 0) { // @0x004379a4
            char ground_tex_name[64] = {};
            c.mission_parse_quoted_string(line, ground_tex_name); // @0x004379b8-0x004379c4
            // THE GROUND CHECK IS AN ALIASED READ, NOT STACK GARBAGE (adversarial review,
            // wf_d34b0eeb-a5b): `line` sits at EBP-0x1b8 and the quoted-string out-buffer at
            // EBP-0x138 = line + 0x80 exactly -- and MAP's loops always leave the persistent index
            // at 0x80. So the original's `line[map_scratch_idx]` @0x004379c9-0x004379cc IS
            // ground_tex_name[0]: the check aborts when the just-parsed texture name is EMPTY.
            // Written that way here (frame-layout-independent); only a malformed file with GROUND
            // before any MAP (index = entry garbage in the original) could tell the two apart.
            if (ground_tex_name[0] == 0) {
                c.llm_fatal_cleanup();
                c.utils_abort(0);
            }
            void *ground_tex = c.get_resource_file_ptr(ground_tex_name); // @0x004379e2-0x004379ed
            own.file_ptr()   = ground_tex;
            c.tlo_palette_convert_565_to_555();                          // @0x004379f2
            c.tlo_shade_table_build_tact();                              // @0x004379f7
        } else if (c.cfg_keyword_token_match(line, KW_CHARACTER) == 0) { // @0x00437a01
            const double parsed = c.mission_parse_float(line, 32.0);
            char_idx            = trunc_only(parsed);      // @0x00437a27-0x00437a2c
            if (own.character_type_at(char_idx).id != 0) { // @0x00437a33
                c.llm_fatal_cleanup();
                c.utils_abort(0);
            }
            own.character_type_at(char_idx).id = static_cast<uint8_t>(char_idx); // @0x00437a48
            section                            = 1;                              // @0x00437a55
        } else if (c.cfg_keyword_token_match(line, KW_GUN) == 0) {               // @0x00437a61
            const double parsed = c.mission_parse_float(line, 32.0);
            gun_idx             = trunc_only(parsed);    // @0x00437a87-0x00437a8c
            if (own.fx_type_table_at(gun_idx).id != 0) { // @0x00437a93
                c.llm_fatal_cleanup();
                c.utils_abort(0);
            }
            own.fx_type_table_at(gun_idx).id         = static_cast<uint8_t>(gun_idx); // @0x00437aa8
            own.fx_type_table_at(gun_idx).range_kill = 0;                             // @0x00437ab5-0x00437ac3
            section                                  = 2;                             // @0x00437ac3
        } else if (c.cfg_keyword_token_match(line, KW_EXPLOSION) == 0) {              // @0x00437acf
            const double parsed = c.mission_parse_float(line, 32.0);
            expl_idx            = trunc_only(parsed);            // @0x00437af5-0x00437afa
            if (own.fx_type_table_at(expl_idx + 0x20).id != 0) { // @0x00437b06
                c.llm_fatal_cleanup();
                c.utils_abort(0);
            }
            own.fx_type_table_at(expl_idx + 0x20).id = static_cast<uint8_t>(expl_idx + 0x20); // @0x00437b2a
            section                                  = 3;                                     // @0x00437b30
        } else if (c.cfg_keyword_token_match(line, KW_DISPOSITION) == 0) {                    // @0x00437b3c
            section = 4;                                                                      // @0x00437b50
            c.door_apply_to_map();                                                            // @0x00437b57 -- odd but confirmed: fired the INSTANT the
                                                                                              // DISPOSITION section header itself is recognised, not once
                                                                                              // per DOOR record. Preserved verbatim.
        } else if (c.cfg_keyword_token_match(line, KW_DOOR) == 0) {                           // @0x00437b61
            const double parsed = c.mission_parse_float(line, 32.0);
            door_idx            = trunc_only(parsed);  // @0x00437b8b-0x00437b90
            if (own.door_table_at(door_idx).id != 0) { // @0x00437b97
                c.llm_fatal_cleanup();
                c.utils_abort(0);
            }
            own.door_table_at(door_idx).id                = static_cast<uint8_t>(door_idx); // @0x00437bb3
            own.door_table_at(door_idx).frame_index       = 0;                              // @0x00437bbd
            own.door_table_at(door_idx).right_frame_count = 0;                              // @0x00437bc8
            own.door_table_at(door_idx).right_col_count   = 0;                              // @0x00437bd5
            own.door_table_at(door_idx).left_frame_count  = 0;                              // @0x00437be2
            own.door_table_at(door_idx).left_col_count    = 0;                              // @0x00437bef
            section                                       = 5;                              // @0x00437bf8
        } else if (c.cfg_keyword_token_match(line, KW_TELEPORT) == 0) {                     // @0x00437c04
            const double parsed = c.mission_parse_float(line, 32.0);
            tele_idx            = trunc_only(parsed);     // @0x00437c2a-0x00437c2f
            if (own.teleport_zone_at(tele_idx).id != 0) { // @0x00437c36
                c.llm_fatal_cleanup();
                c.utils_abort(0);
            }
            own.teleport_zone_at(tele_idx).id = static_cast<uint8_t>(tele_idx); // @0x00437c52
            section                           = 6;                              // @0x00437c58
        } else if (c.cfg_keyword_token_match(line, KW_DETONATION) == 0) {       // @0x00437c64
            section = 7;                                                        // @0x00437c78
        } else if (c.cfg_keyword_token_match(line, KW_QUIT) == 0) {             // @0x00437c84
            uint8_t col = 0, row = 0;
            c.mission_parse_coord_pair(line, &col, &row);             // @0x00437c9e-0x00437ca4
            own.quit_tile_col() = col;                                // @0x00437cad
            own.quit_tile_row() = row;                                // @0x00437cb6
        } else if (c.cfg_keyword_token_match(line, KW_TARGET) == 0) { // @0x00437cc0
            uint8_t col = 0, row = 0;
            c.mission_parse_coord_pair(line, &col, &row); // @0x00437cda-0x00437ce0
            own.target_tile_col() = col;                  // @0x00437ce9
            own.target_tile_row() = row;                  // @0x00437cf2
        }

        // ================================================================================
        // PER-RECORD SECTIONS (only reached if none of the 15 global keys above matched). Each
        // section's own field checks are INDEPENDENT `if`s (not else-if): every real mission line
        // matches at most one, but the original tries them all in this exact order, and so does this
        // translation.
        // ================================================================================
        else if (section == 1) { // ---- CHARACTER, @0x00437d06-0x00438241 -----------------------
            auto &ch = own.character_type_at(char_idx);
            if (c.cfg_keyword_token_match(line, KW_NAME) == 0) { // @0x00437d06
                c.mission_parse_quoted_string(line, ch.name);
            }
            if (c.cfg_keyword_token_match(line, KW_PANEL) == 0) { // @0x00437d30
                char panel_path[64] = {};
                c.mission_parse_quoted_string(line, panel_path);                           // @0x00437d44-0x00437d50
                if (c.utils_str_cmp(panel_path, const_cast<char *>(kPanelKomPath)) == 0) { // @0x00437d5a-0x00437d65
                    // 19-byte fixed-content copy (16+2+1 REP MOVSD/MOVSW/MOVSB), @0x00437d69-0x00437d7d.
                    std::memcpy(panel_path, kPanelTypPath, 19u);
                }
                void *panel_gfx                 = c.get_resource_file_ptr(panel_path); // @0x00437d7e-0x00437d84
                own.char_panel_gfx_at(char_idx) = panel_gfx;                           // @0x00437d91
                // own.char_panel_gfx_at() stays `void *&` (RID_TACT_CHAR_PANEL_GFX, tact_state.h,
                // owned elsewhere); reinterpret_cast to the committed `int16_t *` pointee for the
                // call boundary -- same slot, same bytes, no other conversion.
                c.gfx_convert_pixels_565_to_555(
                    reinterpret_cast<int16_t *>(own.char_panel_gfx_at(char_idx))); // @0x00437d9d-0x00437da3
            }
            if (c.cfg_keyword_token_match(line, KW_WHO) == 0) { // @0x00437da8
                const double parsed = c.mission_parse_float(line, 1.0);
                ch.who              = static_cast<uint8_t>(trunc_only(parsed) ^ *tv.who_xor_key); // @0x00437ddf
            }
            if (c.cfg_keyword_token_match(line, KW_ANGLE_SEE) == 0) { // @0x00437def
                const double parsed = c.mission_parse_float(line, 360.0);
                ch.angle_see        = static_cast<int16_t>(trunc_only(parsed)); // @0x00437e1a
            }
            if (c.cfg_keyword_token_match(line, KW_DISTANCE_SEE) == 0) { // @0x00437e3e
                const double parsed = c.mission_parse_float(line, 20.0);
                ch.distance_see     = static_cast<uint8_t>(trunc_only(parsed)); // @0x00437e69
            }
            if (c.cfg_keyword_token_match(line, KW_FIRST_FRAME) == 0) { // @0x00437e8b
                const double parsed = c.mission_parse_float(line, 10000.0);
                ch.first_frame      = static_cast<int16_t>(
                    trunc_add(static_cast<double>(bank_sprite_base_current), parsed)); // @0x00437eb1-0x00437ebe
            }
            if (c.cfg_keyword_token_match(line, KW_NUMBER_GUN1) == 0) { // @0x00437ee2
                const double parsed = c.mission_parse_float(line, 32.0);
                ch.number_gun1      = static_cast<uint8_t>(trunc_only(parsed)); // @0x00437f0d
            }
            if (c.cfg_keyword_token_match(line, KW_NUMBER_GUN2) == 0) { // @0x00437f2f
                const double parsed = c.mission_parse_float(line, 32.0);
                ch.number_gun2      = static_cast<uint8_t>(trunc_only(parsed)); // @0x00437f5a
            }
            if (c.cfg_keyword_token_match(line, KW_HEIGHT_GUN1) == 0) { // @0x00437f7c
                const double parsed = c.mission_parse_float(line, 100.0);
                ch.height_gun1      = static_cast<uint8_t>(trunc_only(parsed)); // @0x00437fa7
            }
            if (c.cfg_keyword_token_match(line, KW_HEIGHT_GUN2) == 0) { // @0x00437fc9
                const double parsed = c.mission_parse_float(line, 100.0);
                ch.height_gun2      = static_cast<uint8_t>(trunc_only(parsed)); // @0x00437ff4
            }
            if (c.cfg_keyword_token_match(line, KW_KNEEL_GUN1) == 0) { // @0x00438016
                const double parsed = c.mission_parse_float(line, 100.0);
                ch.kneel_gun1       = static_cast<uint8_t>(trunc_only(parsed)); // @0x00438041
            }
            if (c.cfg_keyword_token_match(line, KW_KNEEL_GUN2) == 0) { // @0x00438063
                const double parsed = c.mission_parse_float(line, 100.0);
                ch.kneel_gun2       = static_cast<uint8_t>(trunc_only(parsed)); // @0x0043808e
            }
            if (c.cfg_keyword_token_match(line, KW_ENERGY) == 0) { // @0x004380b0
                const double parsed = c.mission_parse_float(line, 255.0);
                ch.energy           = static_cast<int16_t>(trunc_only(parsed)); // @0x004380db
            }
            if (c.cfg_keyword_token_match(line, KW_SPEED) == 0) { // @0x004380ff
                ch.speed = c.mission_parse_float(line, 10.0);     // @0x00438120-0x00438129, no trunc
            }
            if (c.cfg_keyword_token_match(line, KW_ROTATE) == 0) { // @0x0043812f
                ch.rotate = c.mission_parse_float(line, 10.0);     // @0x00438159
            }
            if (c.cfg_keyword_token_match(line, KW_KNEEL) == 0) {  // @0x0043815f
                ch.kneel_time = c.mission_parse_float(line, 10.0); // @0x00438189
            }
            if (c.cfg_keyword_token_match(line, KW_DEATH) == 0) {  // @0x0043818f
                ch.death_time = c.mission_parse_float(line, 10.0); // @0x004381b9
            }
            if (c.cfg_keyword_token_match(line, KW_MINE) == 0) {  // @0x004381bf
                ch.mine_time = c.mission_parse_float(line, 10.0); // @0x004381e9
            }
            if (c.cfg_keyword_token_match(line, KW_RUN) == 0) {  // @0x004381ef
                ch.run_speed = c.mission_parse_float(line, 8.0); // @0x00438219
            }
            if (c.cfg_keyword_token_match(line, KW_FRAMES) == 0) { // @0x0043821f
                c.character_parse_frame_table(line, char_idx);     // @0x0043823c
            }
        }

        else if (section == 2) { // ---- GUN, @0x00438250-0x00438728 -----------------------------
            auto &fx = own.fx_type_table_at(gun_idx);
            if (c.cfg_keyword_token_match(line, KW_NAME) == 0) { // @0x00438250
                c.mission_parse_quoted_string(line, fx.name);
            }
            if (c.cfg_keyword_token_match(line, KW_SPEED_GUN) == 0) { // @0x0043827a
                fx.speed = c.mission_parse_float(line, 10.0);         // @0x004382a4
            }
            if (c.cfg_keyword_token_match(line, KW_SPEED_FIRE) == 0) { // @0x004382aa
                fx.speed_fire = c.mission_parse_float(line, 10.0);     // @0x004382d4
            }
            if (c.cfg_keyword_token_match(line, KW_BULLETS) == 0) { // @0x004382da
                const double parsed = c.mission_parse_float(line, 255.0);
                fx.bullets          = static_cast<uint8_t>(trunc_only(parsed)); // @0x00438305
            }
            if (c.cfg_keyword_token_match(line, KW_REPEAT) == 0) { // @0x00438327
                fx.repeat = c.mission_parse_float(line, 10.0);     // @0x00438351
            }
            if (c.cfg_keyword_token_match(line, KW_MAGAZINES) == 0) { // @0x00438357
                const double parsed = c.mission_parse_float(line, 255.0);
                fx.magazines        = static_cast<uint8_t>(trunc_only(parsed)); // @0x00438382
            }
            if (c.cfg_keyword_token_match(line, KW_PRECISE) == 0) { // @0x004383a4
                const double parsed = c.mission_parse_float(line, 255.0);
                fx.precise          = static_cast<uint8_t>(trunc_only(parsed)); // @0x004383cf
            }
            if (c.cfg_keyword_token_match(line, KW_PRECISE_KNEEL) == 0) { // @0x004383f1
                const double parsed = c.mission_parse_float(line, 255.0);
                fx.precise_kneel    = static_cast<uint8_t>(trunc_only(parsed)); // @0x0043841c
            }
            if (c.cfg_keyword_token_match(line, KW_FIRST_FRAME) == 0) { // @0x0043843e
                const double parsed = c.mission_parse_float(line, 10000.0);
                fx.first_frame      = static_cast<int16_t>(
                    trunc_add(static_cast<double>(bank_sprite_base_current), parsed)); // @0x00438464-0x00438471
            }
            if (c.cfg_keyword_token_match(line, KW_FRAMES) == 0) { // @0x00438495
                const double parsed = c.mission_parse_float(line, 255.0);
                fx.frames           = static_cast<int16_t>(trunc_only(parsed)); // @0x004384c0
            }
            if (c.cfg_keyword_token_match(line, KW_DIRECT) == 0) { // @0x004384e4
                const double parsed = c.mission_parse_float(line, 1.0);
                fx.direct           = static_cast<uint8_t>(trunc_only(parsed)); // @0x0043850f
            }
            if (c.cfg_keyword_token_match(line, KW_POWER) == 0) { // @0x00438531
                const double parsed = c.mission_parse_float(line, 255.0);
                fx.power            = static_cast<uint8_t>(trunc_only(parsed)); // @0x0043855c
            }
            if (c.cfg_keyword_token_match(line, KW_COLISION1) == 0) { // @0x0043857e
                const double parsed = c.mission_parse_float(line, 32.0);
                fx.colision1        = static_cast<uint8_t>(trunc_add(parsed, *tv.colision1_bias)); // @0x004385a4-0x004385af
            }
            if (c.cfg_keyword_token_match(line, KW_COLISION2) == 0) { // @0x004385d1
                const double parsed = c.mission_parse_float(line, 32.0);
                fx.colision2        = static_cast<uint8_t>(trunc_add(parsed, *tv.colision2_bias)); // @0x004385f7-0x00438602
            }
            if (c.cfg_keyword_token_match(line, KW_RANGE_MAX) == 0) { // @0x00438624
                const double parsed = c.mission_parse_float(line, 32000.0);
                fx.range_max        = trunc_only(parsed); // @0x0043864f
            }
            if (c.cfg_keyword_token_match(line, KW_RANGE_MIN) == 0) { // @0x00438665
                const double parsed = c.mission_parse_float(line, 32000.0);
                fx.range_min        = trunc_only(parsed); // @0x00438690
            }
            if (c.cfg_keyword_token_match(line, KW_SOUND) == 0) { // @0x004386a6
                const double parsed = c.mission_parse_float(line, 999.0);
                fx.sound            = trunc_only(parsed); // @0x004386d1
            }
            if (c.cfg_keyword_token_match(line, KW_RANGE_KILL) == 0) { // @0x004386e7
                const double parsed = c.mission_parse_float(line, 10.0);
                fx.range_kill       = trunc_only(parsed); // @0x00438712
            }
        }

        else if (section == 3) { // ---- EXPLOSION, @0x00438737-0x00438892 -----------------------
            auto &fx = own.fx_type_table_at(expl_idx + 0x20);
            if (c.cfg_keyword_token_match(line, KW_FIRST_FRAME) == 0) { // @0x00438737
                const double parsed = c.mission_parse_float(line, 10000.0);
                fx.first_frame      = static_cast<int16_t>(
                    trunc_add(static_cast<double>(bank_sprite_base_current), parsed)); // @0x0043875d-0x0043876a
            }
            if (c.cfg_keyword_token_match(line, KW_FRAMES) == 0) { // @0x00438793
                const double parsed = c.mission_parse_float(line, 64.0);
                fx.frames           = static_cast<int16_t>(trunc_only(parsed)); // @0x004387be
            }
            if (c.cfg_keyword_token_match(line, KW_SPEED) == 0) { // @0x004387e7
                fx.speed = c.mission_parse_float(line, 10.0);     // @0x00438816, no trunc
            }
            if (c.cfg_keyword_token_match(line, KW_SOUND) == 0) { // @0x0043881c
                const double parsed = c.mission_parse_float(line, 999.0);
                fx.sound            = trunc_only(parsed); // @0x00438847
            }
            // UNCONDITIONAL trailing reset, @0x00438862-0x00438892 -- and a REAL, faithfully
            // preserved original bug: the disassembly indexes these three writes with GUN's OWN
            // index (`gun_idx`, raw EBP-0x70), NOT this section's `expl_idx` (raw EBP-0x6c). If an
            // EXPLOSION record is the first one this call processes -- no GUN keyword yet seen in
            // this file -- `gun_idx` is still its zero-initialised default, and this clobbers
            // fx_type_table[0x20].direct/colision1/colision2 regardless of which EXPLOSION slot is
            // actually being defined. Verified against the raw asm (0x00438862/0x00438872/
            // 0x00438882 all `MOV EAX,[EBP-0x70]`), not merely against the .c (which shows the same
            // thing: `_G_LLM_TACT_FX_TYPE_TABLE[local_74 + 0x20]`, local_74 being GUN's index).
            own.fx_type_table_at(gun_idx + 0x20).direct    = 0; // @0x0043886b
            own.fx_type_table_at(gun_idx + 0x20).colision1 = 0; // @0x0043887b
            own.fx_type_table_at(gun_idx + 0x20).colision2 = 0; // @0x0043888b
        }

        else if (section == 4) { // ---- DISPOSITION, @0x004388a1-0x00438ab3 --------------------
            int32_t opcode = 0, arg0 = 0, arg1 = 0, arg2 = 0, arg3 = 0;
            if (c.mission_parse_command_token(line, &opcode, &arg0, &arg1, &arg2, &arg3) == 0) { // @0x004388b8
                // Not a '-'-command token: a spawn/disposition definition line, @0x004388c5-0x00438a8c.
                uint32_t      spawn_col = 0, spawn_row = 0;
                const int32_t char_num =
                    c.mission_parse_disposition_spawn(line, &spawn_col, &spawn_row); // @0x004388d1
                // facing_dir/def_stat are the PERSISTENT function-scope slots (see their
                // declarations up top): a spawn line without DIRECT:/DEFENSE:* INHERITS the
                // previous line's values -- only the clamps below run unconditionally
                // (adversarial review wf_d34b0eeb-a5b; writes gated @0x004388ee/0x00438904..0x00438967).
                int32_t kw_int_out = 0;                                                     // discarded output of every mission_parse_keyword_int call
                                                                                            // below except DIRECT_COLON's
                if (c.mission_parse_keyword_int(line, KW_DIRECT_COLON, &kw_int_out) == 0) { // @0x004388e7
                    facing_dir = kw_int_out;                                                // @0x004388f0-0x004388f3
                }
                if (c.mission_parse_keyword_int(line, KW_DEFENSE_NONE, &kw_int_out) == 0) { // @0x00438904
                    def_stat = static_cast<int32_t>(detail::defense_stance::none);
                }
                if (c.mission_parse_keyword_int(line, KW_DEFENSE_GUARD1, &kw_int_out) == 0) { // @0x00438922
                    def_stat = static_cast<int32_t>(detail::defense_stance::guard1);
                }
                if (c.mission_parse_keyword_int(line, KW_DEFENSE_GUARD2, &kw_int_out) == 0) { // @0x00438940
                    def_stat = static_cast<int32_t>(detail::defense_stance::guard2);
                }
                if (c.mission_parse_keyword_int(line, KW_DEFENSE_ATTACK, &kw_int_out) == 0) { // @0x0043895e
                    def_stat = static_cast<int32_t>(detail::defense_stance::attack);
                }
                if (facing_dir < 1 || facing_dir > 0x18) facing_dir = 1; // @0x0043896e-0x0043897a
                if (def_stat > 3) def_stat = 0;                          // @0x00438981-0x00438987

                if (char_num > 0 && char_num < 0x1f) { // @0x0043898e-0x0043899f
                    const uint8_t fd = static_cast<uint8_t>(facing_dir);
                    const uint8_t ds = static_cast<uint8_t>(def_stat);
                    if (own.character_type_at(char_num).who == 0) { // @0x004389a3-0x004389aa: player squad
                        // THE FIELDS ARE energy_pct AND is_commando, not unit_proto_id and
                        // unit_slot_index (fixed 2026-09-04, TACT1-P C4 -- the first thing that
                        // broke when this body was made LIVE). The original addresses the record as
                        // base + i*0x10 with the FIELD FOLDED INTO THE DISPLACEMENT, so the slot
                        // identity is in the constant: squad_blackboard is 0x00e15e60, and
                        // [EAX+0xe15e64] is +4 = energy_pct while [EAX+0xe15e6c] is +0xc =
                        // is_commando. Read as +0 and +8 this spawned NOTHING for the player squad
                        // on a rig whose blackboard carries energy_pct=100 and unit_proto_id=0 --
                        // every DISPOSITION line fell through to the enemy branch and the mission
                        // came up with 16 enemies and no squad, with the strategic screen still
                        // rendering behind it.
                        auto &slot = own.squad_status_at(own.squad_size());
                        if (slot.energy_pct > 0) {       // @0x004389b0-0x004389bf, [EAX+0xe15e64]
                            if (slot.is_commando == 1) { // @0x004389c1-0x004389d0, [EAX+0xe15e6c]
                                spawned_unit_id = c.unit_spawn(7, static_cast<int32_t>(spawn_col),
                                                               static_cast<int32_t>(spawn_row), fd, ds,
                                                               slot.energy_pct); // @0x004389d2-0x004389f7
                            } else {
                                spawned_unit_id = c.unit_spawn(char_num, static_cast<int32_t>(spawn_col),
                                                               static_cast<int32_t>(spawn_row), fd, ds,
                                                               slot.energy_pct); // @0x004389fc-0x00438a1f
                            }
                        }
                        own.squad_size() = own.squad_size() + 1; // @0x00438a22
                    } else {                                     // @0x00438a2a-0x00438a44: enemy
                        spawned_unit_id   = c.unit_spawn(char_num, static_cast<int32_t>(spawn_col),
                                                         static_cast<int32_t>(spawn_row), fd, ds, 100);
                        own.enemy_count() = own.enemy_count() + 1; // @0x00438a44
                    }
                    if (char_num == 1) {                                          // @0x00438a4a-0x00438a8c: camera snap to the player's spawn
                        own.map_cam_col() = static_cast<int32_t>(spawn_col) - 7;  // @0x00438a56
                        if (own.map_cam_col() < 0) own.map_cam_col() = 0;         // @0x00438a64
                        own.map_cam_row() = static_cast<int32_t>(spawn_row) - 10; // @0x00438a74
                        if (own.map_cam_row() < 0) own.map_cam_row() = 0;         // @0x00438a82
                    }
                }
            } else if (spawned_unit_id > 0) { // @0x00438a8e-0x00438a92: a command-token line
                c.unit_enqueue_command(spawned_unit_id, opcode, 1, arg0, static_cast<uint16_t>(arg1),
                                       static_cast<uint16_t>(arg2), static_cast<uint16_t>(arg3)); // @0x00438aae
            }
        }

        else if (section == 5) { // ---- DOOR, @0x00438ac2-0x00438bd8 ----------------------------
            auto &dr = own.door_table_at(door_idx);
            if (c.cfg_keyword_token_match(line, KW_DIRECT_LEFTRIGHT) == 0) { // @0x00438ac2
                dr.direct_mode = 0;                                          // @0x00438ada
            }
            if (c.cfg_keyword_token_match(line, KW_DIRECT_RIGHTLEFT) == 0) { // @0x00438ae1
                dr.direct_mode = 1;                                          // @0x00438af9
            }
            if (c.cfg_keyword_token_match(line, KW_DIRECT_NORMAL) == 0) { // @0x00438b00
                dr.direct_mode = 2;                                       // @0x00438b18
            }
            if (c.cfg_keyword_token_match(line, KW_SPEED) == 0) { // @0x00438b1f
                dr.speed = c.mission_parse_float(line, 10.0);     // @0x00438b49
            }
            if (c.cfg_keyword_token_match(line, KW_LEFT) == 0) { // @0x00438b4f
                c.door_parse_definition(line, door_idx);         // @0x00438b6c
            }
            if (c.cfg_keyword_token_match(line, KW_RIGHT) == 0) { // @0x00438b71
                c.door_parse_definition(line, door_idx);          // @0x00438b8e
            }
            if (c.cfg_keyword_token_match(line, KW_XY) == 0) { // @0x00438b93
                uint8_t col = 0, row = 0;
                c.mission_parse_coord_pair(line, &col, &row); // @0x00438bad-0x00438bb3
                dr.tile_x = col;                              // @0x00438bc1
                dr.tile_y = row;                              // @0x00438bd1
            }
        }

        else if (section == 6) { // ---- TELEPORT, @0x00438be7-0x00438dfd --------------------------
            auto &tz = own.teleport_zone_at(tele_idx);
            if (c.cfg_keyword_token_match(line, KW_RANDOM) == 0) { // @0x00438be7
                tz.mode = 1;                                       // @0x00438bff
            }
            if (c.cfg_keyword_token_match(line, KW_SEQUENCE) == 0) { // @0x00438c06
                tz.mode = 2;                                         // @0x00438c1e
            }
            if (c.cfg_keyword_token_match(line, KW_NO_ENEMY) == 0) { // @0x00438c25
                tz.no_enemy = 1;                                     // @0x00438c3d
            }
            if (c.cfg_keyword_token_match(line, KW_START) == 0) { // @0x00438c44
                uint8_t col = 0, row = 0;
                c.mission_parse_coord_pair(line, &col, &row); // @0x00438c5e-0x00438c64
                tz.start_col = col;                           // @0x00438c72
                tz.start_row = row;                           // @0x00438c82
            }
            if (c.cfg_keyword_token_match(line, KW_WHERE) == 0) { // @0x00438c89
                uint8_t col = 0, row = 0;
                c.mission_parse_coord_pair(line, &col, &row); // @0x00438ca7-0x00438cad
                for (int32_t i = 0; i < 8; ++i) {             // @0x00438cbc-0x00438d25: first empty slot wins
                    if (tz.dest_col[i] == 0) {
                        tz.dest_col[i] = col; // @0x00438d00
                        tz.dest_row[i] = row; // @0x00438d1a
                        break;
                    }
                }
            }
            if (c.cfg_keyword_token_match(line, KW_ASSOCIATION) == 0) { // @0x00438d25
                const double  parsed = c.mission_parse_float(line, 99.0);
                const uint8_t assoc  = static_cast<uint8_t>(trunc_only(parsed)); // @0x00438d4f-0x00438d54
                for (int32_t i = 0; i < 8; ++i) {                                // @0x00438d5a-0x00438daa: first empty slot wins
                    if (tz.association[i] == 0) {
                        tz.association[i] = assoc; // @0x00438da0
                        break;
                    }
                }
            }
            if (c.cfg_keyword_token_match(line, KW_DEATH) == 0) { // @0x00438daa
                const double parsed = c.mission_parse_float(line, 32.0);
                tz.death            = static_cast<uint8_t>(trunc_add(parsed, *tv.teleport_death_bias)); // @0x00438dd0-0x00438ddb
            }
        }

        else if (section == 7) {                                        // ---- DETONATION (the mine-blast quad), @0x00438e0c-0x00438ed3 --
            if (c.cfg_keyword_token_match(line, KW_FIRST_FRAME) == 0) { // @0x00438e0c
                const double parsed          = c.mission_parse_float(line, 10000.0);
                own.mine_blast_first_frame() = trunc_add(
                    static_cast<double>(bank_sprite_base_current), parsed); // @0x00438e32-0x00438e3f
            }
            if (c.cfg_keyword_token_match(line, KW_FRAMES) == 0) { // @0x00438e45
                const double parsed          = c.mission_parse_float(line, 64.0);
                own.mine_blast_frame_count() = trunc_only(parsed); // @0x00438e70
            }
            if (c.cfg_keyword_token_match(line, KW_TIMER) == 0) {              // @0x00438e76
                own.mine_blast_duration() = c.mission_parse_float(line, 60.0); // @0x00438e9c, no trunc
            }
            if (c.cfg_keyword_token_match(line, KW_SOUND) == 0) { // @0x00438ea2
                const double parsed       = c.mission_parse_float(line, 999.0);
                own.mine_blast_sound_id() = trunc_only(parsed); // @0x00438ecd
            }
        }
        // else: no global key and no active section field matched -- the original's own trailing
        // `CMP byte ptr [line],0` @0x00438ed5 sets flags nothing branches on (no JZ/JNZ follows
        // before the unconditional `JMP 0x00437471` @0x00438edc); confirmed inert, not reproduced.
    }
}

} // namespace detail

void mission_load(char *filename) {
    tact_state st = state();
    detail::mission_load(st.read, st.own, live_mission_load_calls(), filename);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
