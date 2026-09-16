//
// tact_mission_load_selftest.cpp -- offline oracle for
//   llm_tact_mission_load @0x0043717f (libmh/tact/tact_mission_load.h, libmh/tact/tact_mission_load.cpp)
//
// WHY OFFLINE: see tact_mission_load.h's banner -- this function's real callees include the
// resource-file layer (GetResourseFilePtr/rsr_GetFileRealSize), a real fatal/abort path, and a
// large mission-script keyword-dispatch surface, none of which is safe or practical to exercise
// for real in a selftest. All ~21 outward callees are indirected via mission_load_calls
// (translator brief 3b).
//
// MOCK STRATEGY (per the brief this file was written against): the resource-file layer
// (get_resource_file_ptr/rsr_get_file_real_size) is a small in-memory filename->buffer table this
// file assembles per case, fed with REAL POZ*.DAT text (tmp/decomp_tact/_POZ_SAMPLES.md), CR/LF
// line endings (the read loop @0x00437484-0x004374d4 scans for 0x0d and skips the CR+LF pair).
// The parser mocks are THIN REAL-ISH stand-ins where cheap -- cfg_keyword_token_match reproduces
// the real prefix-match/whitespace-skip/';'-stop semantics (@0x00439103's documented contract, see
// tact_cfg_keyword.h) so SECTION DISPATCH is genuinely exercised, not asserted by fiat; mission_
// parse_float locates the first digit run NOT immediately preceded by a letter (so "NUMBER GUN1
// 2" finds "2", not the "1" inside "GUN1") and hands it to strtod, clamped to max_value --
// deliberately not the original's digit-by-digit x87 accumulation (that fidelity is the sibling
// parser's own suite's job; this oracle tests the ORCHESTRATOR). mission_parse_quoted_string,
// mission_parse_coord_pair, mission_parse_keyword_int and mission_parse_disposition_spawn are
// similarly small real scanners over the literal text, cheap enough to keep real because several
// mandatory cases (the GROUND aliasing regression, the DISPOSITION persistence regression) turn on
// their actual output, not a canned value. mission_parse_command_token is a smaller real-ish stand-
// in (finds '-', matches a short keyword chain, parses a digit list) -- enough to prove the
// spawn-vs-command branch and one enqueue pass-through, not a full reproduction of the real
// twelve-keyword parser (that parser has its own suite, tact_cfg_keyword_selftest.cpp).
// character_parse_frame_table and door_parse_definition are pure RECORDERS (their own field-fill
// logic is out of this function's scope). unit_spawn/unit_enqueue_command/map_compute_bounds/
// tlo_*/gfx_convert_pixels_565_to_555/utils_free/utils_str_cmp/llm_fatal_cleanup/utils_abort are
// recorders.
//
// Every case builds its own mission-file text via `run_load()` (CR/LF joined, filename "MISSION.
// DAT" unless noted) and a resource table (`g_files`) for any secondary GetResourseFilePtr lookups
// (the MAP ground-data resource, texture/panel files). `ck`-family checks cite the exact .asm
// address range each assertion pins, per the checklist (the per-function RE checklist) briefed into this
// translation. Per-case `ck_eq`/`ck` blocks also assert the OTHER recorders stayed at zero so a
// stray call (wrong section wiring, a keyword matching when it should not) fails loudly.
//
#include "tact/tact_mission_load.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "tact/tact_cfg_keyword.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

// ---- the shared recorder -------------------------------------------------------------------------

struct spawn_call_t {
    int32_t char_type, col, row;
    uint8_t fd, ds;
    int32_t hp_pct;
};
struct enqueue_call_t {
    int32_t  unit_id, op;
    uint8_t  interrupt;
    int32_t  arg0;
    uint16_t arg1, arg2, arg3;
};

struct load_recorder {
    std::vector<std::string> call_order;

    int32_t             token_match_calls = 0;
    int32_t             float_calls       = 0;
    std::vector<double> float_max_args;
    std::vector<double> float_returns;
    int32_t             quoted_string_calls     = 0;
    int32_t             keyword_int_calls       = 0;
    int32_t             coord_pair_calls        = 0;
    int32_t             command_token_calls     = 0;
    int32_t             disposition_spawn_calls = 0;

    int32_t                  frame_table_calls = 0;
    std::vector<int32_t>     frame_table_char_idx;
    std::vector<std::string> frame_table_lines;

    int32_t                  door_parse_def_calls = 0;
    std::vector<int32_t>     door_parse_def_door_idx;
    std::vector<std::string> door_parse_def_lines;

    int32_t door_apply_to_map_calls = 0;

    int32_t                   unit_spawn_calls = 0;
    std::vector<spawn_call_t> spawn_calls;
    int32_t                   next_spawn_id = 1000;

    int32_t                     enqueue_calls = 0;
    std::vector<enqueue_call_t> enqueue_calls_v;

    int32_t map_compute_bounds_calls = 0;
    int32_t tlo_palette_calls        = 0;
    int32_t tlo_shade_calls          = 0;

    int32_t             gfx_convert_calls = 0;
    std::vector<void *> gfx_convert_args;

    int32_t                  get_resource_calls = 0;
    std::vector<std::string> get_resource_names;
    int32_t                  rsr_size_calls = 0;

    int32_t             utils_free_calls = 0;
    std::vector<void *> utils_free_args;
    int32_t             str_cmp_calls = 0;

    int32_t              fatal_cleanup_calls = 0;
    int32_t              abort_calls         = 0;
    std::vector<int32_t> abort_status_args;

    void reset() { *this = load_recorder{}; }
};
load_recorder g_rec;

// The per-case resource-file table: filename -> raw bytes. get_resource_file_ptr/rsr_get_file_
// real_size both key off this, matching the resource layer's real filename-indexed contract.
std::map<std::string, std::vector<uint8_t>> g_files;

void set_text_file(const std::string &name, const std::vector<std::string> &lines) {
    std::vector<uint8_t> buf;
    for (const std::string &l : lines) {
        buf.insert(buf.end(), l.begin(), l.end());
        buf.push_back(0x0d);
        buf.push_back(0x0a);
    }
    g_files[name] = std::move(buf);
}

std::vector<uint8_t> make_blank_map_resource() {
    // height sprites (0x40000) + passable bitmap (0x800) + tile-type grid (0x8000), matching the
    // three memcpy sections @0x004376f8-0x0043776c exactly.
    return std::vector<uint8_t>((size_t)0x48800, 0);
}
void set_u16le(std::vector<uint8_t> &buf, size_t byte_offset, uint16_t value) {
    buf[byte_offset]     = (uint8_t)(value & 0xffu);
    buf[byte_offset + 1] = (uint8_t)(value >> 8);
}

// ---- the mocks --------------------------------------------------------------------------------

// llm_tact_cfg_keyword_token_match @0x00439103's documented contract (tact_cfg_keyword.h): skip
// leading whitespace (<=0x20), then compare from the first non-ws byte, character-for-character,
// against `keyword`; return 0 the instant every char of `keyword` matched (a PREFIX match, not a
// whole-token test); return -1 on any mismatch, on hitting ';' at the scan position, or on
// reaching end-of-string without a non-whitespace byte. Kept REAL (not a canned dispatcher) so
// every section-dispatch assertion in this file is proving actual behaviour.
int32_t mock_token_match(char *line, char *keyword) {
    g_rec.token_match_calls++;
    unsigned char *p = reinterpret_cast<unsigned char *>(line);
    while (*p != 0 && *p <= 0x20) ++p;
    if (*p == 0 || *p == ';') return -1;
    unsigned char *k = reinterpret_cast<unsigned char *>(keyword);
    while (*k != 0) {
        if (*p != *k || *p == ';') return -1;
        ++p;
        ++k;
    }
    return 0;
}

// THIN REAL-ISH: locate the first digit run not immediately preceded by a letter (so a keyword
// carrying its own digit, e.g. "NUMBER GUN1      2", resolves to the trailing "2" and not the "1"
// inside "GUN1"), hand it to strtod, clamp to max_value. Deliberately not the original's
// digit-by-digit x87 accumulation -- numeric parser fidelity is mission_parse_float's own suite's
// job; this oracle is testing what mission_load DOES with the returned double.
double mock_parse_float(char *line, double max_value) {
    g_rec.float_calls++;
    g_rec.float_max_args.push_back(max_value);
    char *p     = line;
    char *found = nullptr;
    while (*p != 0) {
        if (*p >= '0' && *p <= '9') {
            char prev          = (p == line) ? ' ' : *(p - 1);
            bool prev_is_alpha = (prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z');
            if (!prev_is_alpha) {
                found = p;
                break;
            }
        }
        ++p;
    }
    double v = found ? std::strtod(found, nullptr) : 0.0;
    if (v > max_value) v = max_value;
    g_rec.float_returns.push_back(v);
    return v;
}

// REAL: copy the text between the first two '"' bytes (NUL-terminated), matching mission_parse_
// quoted_string's observable contract closely enough to drive the GROUND aliasing regression (an
// EMPTY quoted string, "" back-to-back) and the real MAP/GROUND/NAME/PANEL text.
void mock_quoted_string(char *line, char *out_buf) {
    g_rec.quoted_string_calls++;
    char *q1 = std::strchr(line, '"');
    if (q1 == nullptr) {
        out_buf[0] = 0;
        return;
    }
    char  *q2 = std::strchr(q1 + 1, '"');
    size_t n  = q2 ? (size_t)(q2 - (q1 + 1)) : std::strlen(q1 + 1);
    std::memcpy(out_buf, q1 + 1, n);
    out_buf[n] = 0;
}

// REAL: DIRECT_COLON's trailing digits and the four DEFENSE:* tokens are all found by substring
// search (each is called unconditionally per DISPOSITION line -- see the header's persistence
// note), matching mission_parse_keyword_int's contract (0 = matched, out_value set from any
// trailing digits; nonzero = not present, out_value untouched).
int32_t mock_keyword_int(char *line, char *keyword, int32_t *out_value) {
    g_rec.keyword_int_calls++;
    char *hit = std::strstr(line, keyword);
    if (hit == nullptr) return -1;
    char   *p   = hit + std::strlen(keyword);
    int32_t v   = 0;
    bool    any = false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
        any = true;
    }
    *out_value = any ? v : 0;
    return 0;
}

// REAL: scan for the first digit run (col), then the next digit run (row) -- handles both the
// real "{ 23,40 }" brace form and the irregular alignment _POZ_SAMPLES.md documents (one to seven
// spaces, two-space right-aligned single digits).
void mock_coord_pair(char *text, uint8_t *out_col, uint8_t *out_row) {
    g_rec.coord_pair_calls++;
    char *p = text;
    while (*p != 0 && !(*p >= '0' && *p <= '9')) ++p;
    int32_t c = 0;
    while (*p >= '0' && *p <= '9') {
        c = c * 10 + (*p - '0');
        ++p;
    }
    while (*p != 0 && !(*p >= '0' && *p <= '9')) ++p;
    int32_t r = 0;
    while (*p >= '0' && *p <= '9') {
        r = r * 10 + (*p - '0');
        ++p;
    }
    *out_col = (uint8_t)c;
    *out_row = (uint8_t)r;
}

// THIN REAL-ISH stand-in for llm_tact_mission_parse_command_token @0x0043a1d9 (that parser's own
// full 12-keyword-chain fidelity is tact_cfg_keyword_selftest.cpp's job): all five out-pointers
// zeroed unconditionally at entry (matching the real contract); scan for '-'; if found, skip
// spaces and match a short keyword chain (REPEAT/MOVE/RUN/WALK/WAIT/TELE/DIRECT -- enough for this
// file's DISPOSITION-section cases), parse a trailing comma-digit list into out_arg0..3, return 1;
// if no '-' anywhere in the line, return 0 (a spawn/disposition definition line).
int32_t mock_command_token(char *line, int32_t *out_opcode, int32_t *out_arg0, int32_t *out_arg1,
                           int32_t *out_arg2, int32_t *out_arg3) {
    g_rec.command_token_calls++;
    int32_t *opcode  = out_opcode;
    int32_t *outs[4] = {out_arg0, out_arg1, out_arg2, out_arg3};
    *opcode          = 0;
    for (int32_t i = 0; i < 4; ++i) *outs[i] = 0;

    char *dash = std::strchr(line, '-');
    if (dash == nullptr) return 0;
    char *p = dash + 1;
    while (*p == ' ') ++p;

    struct chain_entry {
        const char *kw;
        int32_t     op;
    };
    static const chain_entry kChain[] = {
        {"REPEAT", 0x40},
        {"MOVE", 0x1},
        {"RUN", 0x1e},
        {"WALK", 0x1c},
        {"WAIT", 0x8},
        {"TELE", 0xa},
        {"DIRECT", 0x7},
    };
    for (const chain_entry &e : kChain) {
        size_t klen = std::strlen(e.kw);
        if (std::strncmp(p, e.kw, klen) != 0) continue;
        *opcode = e.op;
        char *q = p + klen;
        while (*q == ' ') ++q;
        int32_t oi = 0;
        while (oi < 4 && *q >= '0' && *q <= '9') {
            int32_t v = 0;
            while (*q >= '0' && *q <= '9') {
                v = v * 10 + (*q - '0');
                ++q;
            }
            *outs[oi++] = v;
            if (*q == ',') ++q;
            else break;
        }
        return 1;
    }
    return 0;
}

// REAL: leading integer (char_num), then the first digit run after '{' (col), then the next digit
// run (row) -- handles the real irregular DISPOSITION alignment.
int32_t mock_disposition_spawn(char *line, uint32_t *out_col, uint32_t *out_row) {
    g_rec.disposition_spawn_calls++;
    uint32_t *col = out_col;
    uint32_t *row = out_row;
    char     *p   = line;
    while (*p == ' ' || *p == '\t') ++p;
    int32_t char_num = 0;
    while (*p >= '0' && *p <= '9') {
        char_num = char_num * 10 + (*p - '0');
        ++p;
    }
    char *brace = std::strchr(p, '{');
    if (brace != nullptr) p = brace + 1;
    while (*p != 0 && !(*p >= '0' && *p <= '9')) ++p;
    uint32_t c = 0;
    while (*p >= '0' && *p <= '9') {
        c = c * 10 + (uint32_t)(*p - '0');
        ++p;
    }
    while (*p != 0 && !(*p >= '0' && *p <= '9')) ++p;
    uint32_t r = 0;
    while (*p >= '0' && *p <= '9') {
        r = r * 10 + (uint32_t)(*p - '0');
        ++p;
    }
    *col = c;
    *row = r;
    return char_num;
}

// RECORDER: FRAMES-list field filling is llm_tact_character_parse_frame_table's own scope.
void mock_frame_table(char *frames_directive_line, int32_t character_type_index) {
    g_rec.frame_table_calls++;
    g_rec.frame_table_char_idx.push_back(character_type_index);
    g_rec.frame_table_lines.push_back(frames_directive_line);
}

// RECORDER: LEFT/RIGHT leaf geometry filling is llm_tact_door_parse_definition's own scope.
void mock_door_parse_definition(char *door_def_line, int32_t door_index) {
    g_rec.door_parse_def_calls++;
    g_rec.door_parse_def_door_idx.push_back(door_index);
    g_rec.door_parse_def_lines.push_back(door_def_line);
}

void mock_door_apply_to_map() {
    g_rec.door_apply_to_map_calls++;
    g_rec.call_order.push_back("door_apply_to_map");
}

int32_t mock_unit_spawn(int32_t char_type, int32_t col, int32_t row, uint8_t fd, uint8_t ds,
                        int32_t hp_pct) {
    g_rec.unit_spawn_calls++;
    g_rec.spawn_calls.push_back({char_type, col, row, fd, ds, hp_pct});
    g_rec.call_order.push_back("unit_spawn");
    return g_rec.next_spawn_id++;
}

int32_t mock_unit_enqueue_command(int32_t unit_id, int32_t op, uint8_t interrupt_flag,
                                  int32_t arg0, uint16_t arg1, uint16_t arg2, uint16_t arg3) {
    g_rec.enqueue_calls++;
    g_rec.enqueue_calls_v.push_back({unit_id, op, interrupt_flag, arg0, arg1, arg2, arg3});
    g_rec.call_order.push_back("unit_enqueue_command");
    return 0;
}

void mock_map_compute_bounds() {
    g_rec.map_compute_bounds_calls++;
    g_rec.call_order.push_back("map_compute_bounds");
}
void mock_tlo_palette_convert_565_to_555() {
    g_rec.tlo_palette_calls++;
    g_rec.call_order.push_back("tlo_palette_convert_565_to_555");
}
void mock_tlo_shade_table_build_tact() {
    g_rec.tlo_shade_calls++;
    g_rec.call_order.push_back("tlo_shade_table_build_tact");
}
void mock_gfx_convert_pixels_565_to_555(int16_t *sprite_blob) {
    g_rec.gfx_convert_calls++;
    g_rec.gfx_convert_args.push_back(sprite_blob);
    g_rec.call_order.push_back("gfx_convert_pixels_565_to_555");
}

uint8_t *mock_get_resource_file_ptr(char *file_name) {
    g_rec.get_resource_calls++;
    g_rec.get_resource_names.push_back(file_name);
    g_rec.call_order.push_back(std::string("get_resource_file_ptr:") + file_name);
    auto it = g_files.find(file_name);
    return it != g_files.end() ? it->second.data() : nullptr;
}
uint32_t mock_rsr_get_file_real_size(char *file_name) {
    g_rec.rsr_size_calls++;
    auto it = g_files.find(file_name);
    return it != g_files.end() ? (uint32_t)it->second.size() : 0u;
}
void mock_utils_free(void *ptr) {
    g_rec.utils_free_calls++;
    g_rec.utils_free_args.push_back(ptr);
    g_rec.call_order.push_back("utils_free");
}
int32_t mock_utils_str_cmp(char *a, char *b) {
    g_rec.str_cmp_calls++;
    return std::strcmp(a, b);
}
void mock_fatal_cleanup() {
    g_rec.fatal_cleanup_calls++;
    g_rec.call_order.push_back("fatal_cleanup");
}
void mock_utils_abort(int32_t status) {
    g_rec.abort_calls++;
    g_rec.abort_status_args.push_back(status);
    g_rec.call_order.push_back("abort");
}

const mission_load_calls &rec_calls() {
    static const mission_load_calls c = {
        mock_token_match,
        mock_parse_float,
        mock_quoted_string,
        mock_keyword_int,
        mock_coord_pair,
        mock_command_token,
        mock_disposition_spawn,
        mock_frame_table,
        mock_door_parse_definition,
        mock_door_apply_to_map,
        mock_unit_spawn,
        mock_unit_enqueue_command,
        mock_map_compute_bounds,
        mock_tlo_palette_convert_565_to_555,
        mock_tlo_shade_table_build_tact,
        mock_gfx_convert_pixels_565_to_555,
        mock_get_resource_file_ptr,
        mock_rsr_get_file_real_size,
        mock_utils_free,
        mock_utils_str_cmp,
        mock_fatal_cleanup,
        mock_utils_abort,
    };
    return c;
}

// Builds "MISSION.DAT" from `lines` (CR/LF joined, matching the real read loop's 0x0d/0x0a
// contract) and runs detail::mission_load against `fx`. Case bodies register any additional
// resource-file entries (MAP data, GROUND/PANEL textures) into g_files BEFORE calling this.
void run_load(tact_fixture &fx, const std::vector<std::string> &lines,
              const char *filename = "MISSION.DAT") {
    set_text_file(filename, lines);
    char fname_buf[64] = {};
    std::strncpy(fname_buf, filename, sizeof(fname_buf) - 1);
    tact_store own = fx.store();
    tact_view  tv  = fx.view();
    detail::mission_load(tv, own, rec_calls(), fname_buf);
}

// ==================================================================================================
// CASE 1 -- REGRESSION 1: the GROUND aliasing fix (review counterexample 1)
// ==================================================================================================
//
// 1A: MAP then a real GROUND line must NOT abort (the pre-fix draft aborted every such file, since
// it read a stale/aliased stack slot instead of the just-parsed ground-texture name's first byte).
// Cite 0x004379c9-0x004379d4 (the aliased read) + 0x004378da-0x0043794f (the MAP loop that leaves
// map_scratch_idx == 0x80, the derivation the header banner documents).
void case1_ground_regression() {
    {
        tact_fixture fx;
        g_rec.reset();
        g_files.clear();
        g_files["ALIEN_01.MAP"] = make_blank_map_resource();

        // Real POZ text (_POZ_SAMPLES.md "Header / global directives (POZ1L)" + "Quoted-string
        // operands" blocks).
        run_load(fx, {
                         "MAP    \"ALIEN_01.MAP\"",
                         "GROUND \"PODLOGA.TLO\"",
                     });

        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u,
              "1A: GROUND following a real MAP does NOT abort -- the aliased-read regression, "
              "0x004379c9-0x004379d4 (ALIASED read of ground_tex_name[0], not stale stack), "
              "derived from the MAP loop leaving map_scratch_idx==0x80, 0x004378da-0x0043794f");
        ck_eq((uint32_t)g_rec.abort_calls, 0u, "1A: no abort recorded, 0x004379a4-0x004379f7");
        ck_eq((uint32_t)g_rec.map_compute_bounds_calls, 1u,
              "1A: map_compute_bounds fires once after the MAP section, 0x0043794f");
        ck_eq((uint32_t)g_rec.tlo_palette_calls, 1u,
              "1A: tlo_palette_convert_565_to_555 fires once from GROUND, 0x004379f2");
        ck_eq((uint32_t)g_rec.tlo_shade_calls, 1u,
              "1A: tlo_shade_table_build_tact fires once from GROUND, 0x004379f7");
        ck_eq((uint32_t)g_rec.get_resource_calls, 3u,
              "1A: get_resource_file_ptr called for MISSION.DAT + ALIEN_01.MAP + PODLOGA.TLO");
        ck_eq((uint32_t)g_rec.utils_free_calls, 2u,
              "1A: utils_free called for the MAP resource (0x0043776c) + the EOF file free "
              "(0x00437471-0x00437477)");
    }

    // 1B: GROUND with an EMPTY quoted name ("") -> fatal_cleanup+abort recorded, matching the
    // header banner's ground_tex_name[0]==0 check verbatim.
    {
        tact_fixture fx;
        g_rec.reset();
        g_files.clear();
        g_files["ALIEN_02.MAP"] = make_blank_map_resource();

        run_load(fx, {
                         "MAP    \"ALIEN_02.MAP\"",
                         "GROUND \"\"",
                     });

        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 1u,
              "1B: an EMPTY GROUND quoted name triggers fatal_cleanup, "
              "0x004379c4-0x004379e2 (ground_tex_name[0]==0)");
        ck_eq((uint32_t)g_rec.abort_calls, 1u, "1B: ... and utils_abort");
        if (g_rec.abort_calls == 1) {
            ck_eq((uint32_t)g_rec.abort_status_args[0], 0u, "1B: utils_abort(0)");
        }
        // No early return follows the abort call in this translation (same "no early return"
        // shape mission_start.cpp's T2 documents) -- GROUND's own fallthrough still runs once.
        ck_eq((uint32_t)g_rec.tlo_palette_calls, 1u,
              "1B: tlo_palette_convert_565_to_555 STILL fires after the abort path (no early "
              "return in the translation), 0x004379f2");
        ck_eq((uint32_t)g_rec.tlo_shade_calls, 1u, "1B: tlo_shade_table_build_tact STILL fires");
    }
}

// ==================================================================================================
// CASE 2 -- REGRESSION 2: DISPOSITION facing_dir/def_stat persistence + the facing_dir clamp
// (review counterexample 2). Cite 0x004388ee (DIRECT: store), 0x00438904-0x00438967 (DEFENSE:*
// stores), 0x0043896e-0x0043897a (facing_dir clamp), 0x00438981-0x00438987 (def_stat clamp),
// 0x00438a8e-0x00438a92 (command-vs-spawn branch), 0x00438aae (unit_enqueue_command call).
// ==================================================================================================
void case2_disposition_persistence() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();

    // char_num=4, who left at the fixture default (0) -> squad path; not char_num==1, so this case
    // stays clear of the camera-snap logic (case 8's job). Real DISPOSITION shape from
    // _POZ_SAMPLES.md ("N { col,row }  DIRECT:n  DEFENSE:stance"), values adjusted for this
    // regression per the task brief; the no-keyword inheriting line is a documented hand-built
    // variation (no shipped sample omits DIRECT:/DEFENSE:, per _POZ_SAMPLES.md's own DEFENSE:NONE
    // caveat) and the trailing "- RUN" command line is verbatim real text (_POZ_SAMPLES.md's
    // "every distinct non-MOVE shape" list).
    fx.character_types[4].who = 0; // squad path (unrelated to this case's own focus)
    // The TU reads squad_status_at(squad_size()) and squad_size INCREMENTS per spawn line
    // (@0x00438a22) -- so each of the three lines consumes its OWN blackboard slot, and a slot
    // with energy_pct==0 is skipped (spawn gated @0x004389b0 on the slot's +4 -- see case8's own
    // note for why that is energy_pct and not unit_proto_id). Seed all three; is_commando==0 on all
    // of them so they take the plain (char_num) branch.
    fx.squad_status[0] = {77, 50, 1, 0};
    fx.squad_status[1] = {78, 60, 0, 0};
    fx.squad_status[2] = {79, 70, 0, 0};

    run_load(fx, {
                     "DISPOSITION",
                     "       4 { 88,25 }  DIRECT:5  DEFENSE:GUARD1",
                     "       4 { 90,25 }",
                     "       4 { 92,25 }  DIRECT:99",
                     "  - RUN",
                 });

    ck_eq((uint32_t)g_rec.unit_spawn_calls, 3u, "case2: three spawn lines, three unit_spawn calls");
    if (g_rec.unit_spawn_calls == 3) {
        ck_eq((uint32_t)g_rec.spawn_calls[0].fd, 5u,
              "case2 spawn1: facing_dir==5 from this line's own DIRECT:5, 0x004388ee");
        ck_eq((uint32_t)g_rec.spawn_calls[0].ds, 1u,
              "case2 spawn1: def_stat==1 (guard1) from this line's own DEFENSE:GUARD1, 0x00438922");
        ck_eq((uint32_t)g_rec.spawn_calls[0].col, 88u, "case2 spawn1: col from this line, 0x004388d1");
        ck_eq((uint32_t)g_rec.spawn_calls[0].row, 25u, "case2 spawn1: row from this line");

        ck_eq((uint32_t)g_rec.spawn_calls[1].fd, 5u,
              "case2 spawn2 (NO DIRECT:/DEFENSE: on this line): facing_dir INHERITED == 5, not "
              "reset to the fallback 1 -- the persistence regression, 0x0043896e-0x0043897a");
        ck_eq((uint32_t)g_rec.spawn_calls[1].ds, 1u,
              "case2 spawn2: def_stat INHERITED == 1, not reset to the fallback 0, "
              "0x00438981-0x00438987");
        ck_eq((uint32_t)g_rec.spawn_calls[1].col, 90u,
              "case2 spawn2: col/row are still THIS line's own (not inherited), proving fd/ds are "
              "the only persistent fields, 0x004388d1");

        ck_eq((uint32_t)g_rec.spawn_calls[2].fd, 1u,
              "case2 spawn3: facing_dir==99 (DIRECT:99) clamps to 1 (99 > 0x18), "
              "0x0043896e-0x0043897a");
        ck_eq((uint32_t)g_rec.spawn_calls[2].ds, 1u,
              "case2 spawn3: def_stat still INHERITED == 1 (no DEFENSE: on this line either)");
        ck_eq((uint32_t)g_rec.spawn_calls[2].col, 92u, "case2 spawn3: col from this line");
    }
    ck_eq((uint32_t)g_rec.keyword_int_calls, 15u,
          "case2: mission_parse_keyword_int called exactly 5x per spawn line (DIRECT_COLON + 4x "
          "DEFENSE:*), unconditionally, x3 lines == 15 total, 0x004388e7-0x0043895e");
    ck_eq((uint32_t)g_rec.command_token_calls, 4u,
          "case2: mission_parse_command_token called once per DISPOSITION-section line (3 spawns + "
          "1 command), 0x004388b8");
    ck_eq((uint32_t)g_rec.disposition_spawn_calls, 3u,
          "case2: mission_parse_disposition_spawn called only for the 3 non-command lines, "
          "0x004388d1");

    ck_eq((uint32_t)g_rec.enqueue_calls, 1u,
          "case2: the trailing '- RUN' command line enqueues once, 0x00438a8e-0x00438a92");
    if (g_rec.enqueue_calls == 1) {
        ck_eq((uint32_t)g_rec.enqueue_calls_v[0].unit_id, (uint32_t)(1000 + 2),
              "case2: unit_enqueue_command's unit_id is spawned_unit_id from the LAST spawn "
              "(spawn3, the 3rd mocked id), not the first, 0x00438a8e 'spawned_unit_id > 0'");
        ck_eq((uint32_t)g_rec.enqueue_calls_v[0].op, 0x1eu,
              "case2: opcode==RUN(0x1e), 0x00438aae");
        ck_eq((uint32_t)g_rec.enqueue_calls_v[0].interrupt, 1u,
              "case2: interrupt_flag hardcoded 1, 0x00438aae");
        ck_eq((uint32_t)g_rec.enqueue_calls_v[0].arg0, 0u, "case2: RUN carries no args -> arg0==0");
    }
    ck_eq((uint32_t)fx.squad_size, 3u, "case2: squad_size incremented once per spawn line, 0x00438a22");
    ck_eq((uint32_t)fx.enemy_count, 0u, "case2: enemy_count untouched (squad path throughout)");
    ck_eq((uint32_t)g_rec.door_apply_to_map_calls, 1u,
          "case2: DISPOSITION's own header line fires door_apply_to_map once, 0x00437b57");
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "case2: no stray abort");
    ck_eq((uint32_t)g_rec.abort_calls, 0u, "case2: no stray abort");
}

// ==================================================================================================
// CASE 3 -- the two init-clear overruns (+ the exact-bound table clears), all unconditional at the
// TOP of mission_load, before the file is even opened -- so a 0-byte file still exercises all of
// them. Cite 0x0043721c (units JLE 0x80), 0x0043727e/0x00437288 (fx_pool JLE 0x400 -- the
// SECOND overrun), 0x004372bb (character_types JL 0x10, exact), 0x004372ee (fx_type_table JL 0x40,
// exact), 0x00437321 (door_table JLE 0x10 -- the FIRST/batch-brief overrun), 0x00437354
// (teleport_zones JL 0x42, exact).
// ==================================================================================================
void case3_init_overruns() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();
    g_files["EMPTY.DAT"] = {}; // 0-byte "file": EOF triggers immediately, but the clears above
                               // already ran (they precede "open the mission file")

    // Sentinels distinct across the families so a wrong-index write is observable, not lucky-zero.
    fx.fx_type_table_mut[0].id  = 0xab;
    fx.fx_type_table_mut[31].id = 0xac;
    fx.fx_type_table_mut[63].id = 0xad; // last REAL slot (TACT_FX_TYPE_SLOTS-1)
    fx.fx_type_table_mut[64].id = 0xae; // the +1 SLACK slot the fixture carries for this overrun

    fx.door_table[0].id  = 0xba;
    fx.door_table[8].id  = 0xbb;
    fx.door_table[15].id = 0xbc; // last REAL slot
    fx.door_table[16].id = 0xbd; // the +1 SLACK slot (the batch-brief-named overrun)

    fx.fx_pool[0].fx_type    = 0xca;
    fx.fx_pool[1023].fx_type = 0xcb; // last REAL slot (TACT_FX_POOL_SLOTS-1)
    fx.fx_pool[1024].fx_type = 0xcc; // the +1 SLACK slot (the second, banner-flagged overrun)

    fx.character_types[0].id  = 0xda;
    fx.character_types[15].id = 0xdb; // last of the exact 16, no slack

    fx.teleport_zones[0].id  = 0xea;
    fx.teleport_zones[65].id = 0xeb; // last of the exact 66, no slack

    fx.units[0].type   = 0xff;
    fx.units[128].type = 0xff; // last of the exact 129 (0..0x80 inclusive)

    fx.squad_size  = 7;
    fx.enemy_count = 9;

    run_load(fx, {}, "EMPTY.DAT");

    ck_eq((uint32_t)fx.fx_type_table_mut[0].id, 0u, "case3: fx_type_table[0] cleared, 0x004372ee");
    ck_eq((uint32_t)fx.fx_type_table_mut[31].id, 0u, "case3: fx_type_table[31] cleared");
    ck_eq((uint32_t)fx.fx_type_table_mut[63].id, 0u, "case3: fx_type_table[63] (last real slot) cleared");
    ck_eq((uint32_t)fx.fx_type_table_mut[64].id, 0xaeu,
          "case3: NO overrun on fx_type_table -- JL 0x40 is exact (0..63): the fixture's slack "
          "index 64 keeps its 0xae sentinel, 0x004372ee");

    ck_eq((uint32_t)fx.door_table[0].id, 0u, "case3: door_table[0] cleared, 0x00437321");
    ck_eq((uint32_t)fx.door_table[8].id, 0u, "case3: door_table[8] cleared");
    ck_eq((uint32_t)fx.door_table[15].id, 0u, "case3: door_table[15] (last real slot) cleared");
    ck_eq((uint32_t)fx.door_table[16].id, 0u,
          "case3: the FIRST overrun -- JLE 0x10 writes indices 0..16 inclusive, one record past "
          "the 16-slot table -- door_table[16] (the fixture's +1 slack slot) WAS written, 0x00437321");

    ck_eq((uint32_t)fx.fx_pool[0].fx_type, 0u, "case3: fx_pool[0] cleared, 0x0043727e/0x00437288");
    ck_eq((uint32_t)fx.fx_pool[1023].fx_type, 0u, "case3: fx_pool[1023] (last real slot) cleared");
    ck_eq((uint32_t)fx.fx_pool[1024].fx_type, 0u,
          "case3: the SECOND overrun -- JLE 0x400 writes indices 0..1024 inclusive, one record "
          "past the 1024-slot table -- fx_pool[1024] (the fixture's +1 slack slot) WAS written, "
          "0x0043727e/0x00437288");

    ck_eq((uint32_t)fx.character_types[0].id, 0u, "case3: character_types[0] cleared, 0x004372bb");
    ck_eq((uint32_t)fx.character_types[15].id, 0u,
          "case3: character_types[15] (last of the exact 16, JL 0x10) cleared");

    ck_eq((uint32_t)fx.teleport_zones[0].id, 0u, "case3: teleport_zones[0] cleared, 0x00437354");
    ck_eq((uint32_t)fx.teleport_zones[65].id, 0u,
          "case3: teleport_zones[65] (last of the exact 66, JL 0x42) cleared");

    ck_eq((uint32_t)fx.units[0].type, 0u,
          "case3: units[0].type cleared -- the @0x00437201 pre-loop 0xff write is immediately "
          "clobbered by the loop's own i==0 pass, net no-op, 0x00437201/0x0043721c");
    ck_eq((uint32_t)fx.units[128].type, 0u, "case3: units[128] (last of the exact 129) cleared");

    ck_eq((uint32_t)fx.squad_size, 0u, "case3: squad_size reset, 0x0043743a");
    ck_eq((uint32_t)fx.enemy_count, 0u, "case3: enemy_count reset, 0x00437444");

    // TOTAL recorder counts: a 0-byte file exercises ONLY the resource-open + EOF-free calls.
    ck_eq((uint32_t)g_rec.get_resource_calls, 1u, "case3: get_resource_file_ptr(EMPTY.DAT) once");
    ck_eq((uint32_t)g_rec.rsr_size_calls, 1u, "case3: rsr_get_file_real_size(EMPTY.DAT) once");
    ck_eq((uint32_t)g_rec.utils_free_calls, 1u, "case3: the EOF free, 0x00437471-0x00437477");
    ck_eq((uint32_t)g_rec.token_match_calls, 0u, "case3: no lines -> no keyword dispatch at all");
    ck_eq((uint32_t)g_rec.float_calls, 0u, "case3: no stray float parse");
    ck_eq((uint32_t)g_rec.unit_spawn_calls, 0u, "case3: no stray spawn");
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "case3: no stray abort");
    ck_eq((uint32_t)g_rec.abort_calls, 0u, "case3: no stray abort");
}

// ==================================================================================================
// CASE 4 -- the EXPLOSION section's wrong-index original bug: the unconditional trailing reset
// indexes fx_type_table with GUN's OWN index (gun_idx), not this section's expl_idx, so it lands
// on gun_idx+0x20, NOT expl_idx+0x20. Cite 0x00438862-0x0043888b.
// ==================================================================================================
void case4_explosion_wrong_index() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();

    // gun_idx will be 3 (from "GUN 3"); expl_idx will be 5 (from "EXPLOSION 5"). The bug's actual
    // write target is gun_idx+0x20 == 0x23; the "should be" (but is NOT) target is
    // expl_idx+0x20 == 0x25. Seed BOTH with distinct sentinels so either polarity fails.
    fx.fx_type_table_mut[0x23].direct    = 0xaa; // gun_idx+0x20 -- the ACTUAL bug target
    fx.fx_type_table_mut[0x23].colision1 = 0xab;
    fx.fx_type_table_mut[0x23].colision2 = 0xac;
    fx.fx_type_table_mut[0x25].direct    = 0xbb; // expl_idx+0x20 -- the "should be" target (untouched)
    fx.fx_type_table_mut[0x25].colision1 = 0xbc;
    fx.fx_type_table_mut[0x25].colision2 = 0xbd;

    // "FRAMES 3" is the real single-integer GUN/EXPLOSION-section shape _POZ_SAMPLES.md documents
    // ("the unrelated FRAMES 3 / FRAMES 5 single-integer form in the GUN/EXPLOSION sections"). It
    // both exercises a legitimate EXPLOSION field write (fx.frames) AND, on the SAME iteration,
    // triggers the unconditional trailing reset (both live in the section==3 body together).
    run_load(fx, {
                     "GUN 3",
                     "EXPLOSION 5",
                     "FRAMES 3",
                 });

    ck_eq((uint32_t)fx.fx_type_table_mut[3].id, 3u, "case4: GUN 3 sets fx_type_table[3].id, 0x00437aa8");
    ck_eq((uint32_t)fx.fx_type_table_mut[3].range_kill, 0u,
          "case4: GUN's own unconditional range_kill=0, 0x00437ab5-0x00437ac3");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x25].id, 0x25u,
          "case4: EXPLOSION 5 sets fx_type_table[0x25].id, 0x00437b2a");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x25].frames, 3u,
          "case4: FRAMES 3 legitimately sets fx_type_table[expl_idx+0x20].frames, 0x004387be");

    ck_eq((uint32_t)fx.fx_type_table_mut[0x23].direct, 0u,
          "case4 THE BUG: fx_type_table[gun_idx+0x20 == 0x23].direct clobbered to 0 by the "
          "unconditional trailing reset, 0x0043886b (MOV EAX,[EBP-0x70] == gun_idx, not expl_idx)");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x23].colision1, 0u,
          "case4 THE BUG: ...[0x23].colision1 clobbered, 0x0043887b");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x23].colision2, 0u,
          "case4 THE BUG: ...[0x23].colision2 clobbered, 0x0043888b");

    ck_eq((uint32_t)fx.fx_type_table_mut[0x25].direct, 0xbbu,
          "case4 THE OTHER POLARITY: fx_type_table[expl_idx+0x20 == 0x25].direct is the sentinel "
          "0xbb, UNTOUCHED -- a 'fixed' translation that used expl_idx instead of gun_idx here "
          "would zero this and this check would fail, 0x0043886b");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x25].colision1, 0xbcu,
          "case4 THE OTHER POLARITY: ...[0x25].colision1 untouched sentinel survives, 0x0043887b");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x25].colision2, 0xbdu,
          "case4 THE OTHER POLARITY: ...[0x25].colision2 untouched sentinel survives, 0x0043888b");

    ck_eq((uint32_t)g_rec.float_calls, 3u,
          "case4: 3 float parses total -- GUN 3's index parse (0x00437a61), EXPLOSION 5's index "
          "parse (0x00437acf), and FRAMES 3's own field parse inside the section==3 body "
          "(0x00438793) -- the section-entry keywords (GUN/EXPLOSION) and the per-record field "
          "keywords are mutually exclusive per line (else-if chain), so GUN 3/EXPLOSION 5 each "
          "contribute exactly their own index parse, nothing more");
    ck_eq((uint32_t)g_rec.utils_free_calls, 1u, "case4: EOF free only, no MAP section in this file");
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "case4: no stray abort (both ids fresh, no dup)");
}

// ==================================================================================================
// CASE 5 -- the dead TIME keyword: parsed + truncated (a real, observable side effect: the mock
// call is recorded), but never persisted anywhere. Real POZ text verbatim (_POZ_SAMPLES.md's
// header block, including the inline ';' comment mission_parse_float must scan past). Cite
// 0x00437549-0x00437577 (TIME's own parse+trunc+discard) and the header banner's "THE .c DRAFT
// ALREADY LIED HERE" / "THE TIME KEYWORD'S TARGET IS DEAD" sections (the clamp+free+RET tail at
// 0x00438ee9-0x00438f08 is documented, not reproduced -- there is nothing observable to preserve).
// ==================================================================================================
void case5_time_dead_keyword() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();

    run_load(fx, {"TIME = 100                    ; uplyw czasu w %"});

    ck_eq((uint32_t)g_rec.float_calls, 1u,
          "case5: TIME is recognised and consumed -- mission_parse_float called once, 0x00437559");
    if (g_rec.float_calls == 1) {
        ck_eq_d(g_rec.float_max_args[0], 1000.0, "case5: TIME's own max clamp is 1000.0, 0x00437554");
        ck_eq_d(g_rec.float_returns[0], 100.0,
                "case5: the real value scanned past the inline ';' comment is 100 (the trailing "
                "'; uplyw czasu w %' text does not confuse the scan)");
    }
    // TOTAL: exactly 2 keyword tests for this single line (STOP mismatches first, TIME matches
    // second, then the else-if chain short-circuits -- section stays 0, so no per-section body
    // ever runs).
    ck_eq((uint32_t)g_rec.token_match_calls, 2u,
          "case5: STOP (mismatch) then TIME (match) -- the chain short-circuits on match, "
          "0x00437529/0x00437549");
    ck_eq((uint32_t)g_rec.get_resource_calls, 1u, "case5: only the main file is opened");
    ck_eq((uint32_t)g_rec.utils_free_calls, 1u, "case5: EOF free only");
    ck_eq((uint32_t)g_rec.unit_spawn_calls, 0u, "case5: TIME has NOTHING persisted -- no side effect "
                                                "anywhere else in the fixture");
    ck_eq((uint32_t)fx.squad_size, 0u, "case5: squad_size untouched by TIME (init-clear value only)");
    ck_eq((uint32_t)fx.enemy_count, 0u, "case5: enemy_count untouched by TIME");
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "case5: no stray abort");
}

// ==================================================================================================
// CASE 6 -- section dispatch (the 15 global keywords) + /* */ comment tracking + the five
// duplicate-record-id fatal-abort checks. Cite each global keyword's own dispatch address (the
// KW_* table comments in tact_mission_load.cpp) plus 0x004374e0-0x0043751c (comment tracking).
// ==================================================================================================

// 6A: the 15 global keywords, one real-shape line each, in one file (real header text verbatim for
// STOP/TIME/SEE_ENEMY/MAP/GROUND/BANK/QUIT/TARGET; hand-built single-index lines for CHARACTER/GUN/
// EXPLOSION/DISPOSITION/DOOR/TELEPORT/DETONATION -- no full block exists in _POZ_SAMPLES.md for
// those, matching the file's own precedent for DEFENSE:NONE/GUARD1). STOP is placed LAST so its
// abort doesn't interfere with the earlier routing checks (no early return follows it either).
void case6a_global_keyword_routing() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();
    g_files["ALIEN_01.MAP"] = make_blank_map_resource();
    // DOOR 7's own explicit field-zeroing (0x00437bbd-0x00437bef) is verified by seeding garbage
    // into fields the init-clear loop (@0x00437321, id-only) does NOT touch.
    fx.door_table[7].frame_index       = 0xaa;
    fx.door_table[7].right_frame_count = 0xbbbb;
    fx.door_table[7].right_col_count   = 0xcccc;
    fx.door_table[7].left_frame_count  = 0xdddd;
    fx.door_table[7].left_col_count    = 0xeeee;

    run_load(fx, {
                     ";--------------------------------------------------------------------------",
                     "; UWAGA !!!  Nie deklarowac obiektow,strzalow,broni,drzwi o numerach 0 !!!",
                     ";--------------------------------------------------------------------------",
                     "",
                     "TIME = 100                    ; uplyw czasu w %",
                     ";SEE ENEMY                    ; obcy sa widoczni (odkrywaja teren)",
                     "",
                     "MAP    \"ALIEN_01.MAP\"",
                     "GROUND \"PODLOGA.TLO\"",
                     "",
                     "TARGET { 23,40 }",
                     "QUIT   { 56,45 }",
                     "",
                     "BANK 5",
                     "CHARACTER 2",
                     "GUN 4",
                     "EXPLOSION 6",
                     "DISPOSITION",
                     "DOOR 7",
                     "TELEPORT 9",
                     "DETONATION",
                     "STOP",
                 });

    ck_eq((uint32_t)fx.see_enemy_flag, 0u,
          "6A: the COMMENTED-OUT ';SEE ENEMY' line does NOT set see_enemy_flag -- "
          "cfg_keyword_token_match stops at ';' at the scan position for every keyword tested "
          "against that line, 0x0043757c / the token-match contract's ';' rule");
    ck_eq((uint32_t)fx.target_tile_col, 23u, "6A: TARGET { 23,40 } -> target_tile_col, 0x00437cc0/0x00437ce9");
    ck_eq((uint32_t)fx.target_tile_row, 40u, "6A: ... target_tile_row, 0x00437cf2");
    ck_eq((uint32_t)fx.quit_tile_col, 56u, "6A: QUIT { 56,45 } -> quit_tile_col, 0x00437c84/0x00437cad");
    ck_eq((uint32_t)fx.quit_tile_row, 45u, "6A: ... quit_tile_row, 0x00437cb6");

    ck_eq((uint32_t)fx.character_types[2].id, 2u, "6A: CHARACTER 2 -> section=1, id=2, 0x00437a01/0x00437a48");
    ck_eq((uint32_t)fx.fx_type_table_mut[4].id, 4u, "6A: GUN 4 -> section=2, id=4, 0x00437a61/0x00437aa8");
    ck_eq((uint32_t)fx.fx_type_table_mut[4].range_kill, 0u, "6A: ... range_kill=0, 0x00437ab5-0x00437ac3");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x26].id, 0x26u,
          "6A: EXPLOSION 6 -> section=3, id at expl_idx+0x20, 0x00437acf/0x00437b2a");
    ck_eq((uint32_t)g_rec.door_apply_to_map_calls, 1u,
          "6A: DISPOSITION -> section=4 AND fires door_apply_to_map on the SAME line, 0x00437b3c/0x00437b57");
    ck_eq((uint32_t)fx.door_table[7].id, 7u, "6A: DOOR 7 -> section=5, id=7, 0x00437b61/0x00437bb3");
    ck_eq((uint32_t)fx.door_table[7].frame_index, 0u,
          "6A: DOOR's own explicit field-zeroing overwrites the pre-seeded garbage, 0x00437bbd");
    ck_eq((uint32_t)fx.door_table[7].right_frame_count, 0u, "6A: ... right_frame_count, 0x00437bc8");
    ck_eq((uint32_t)fx.door_table[7].right_col_count, 0u, "6A: ... right_col_count, 0x00437bd5");
    ck_eq((uint32_t)fx.door_table[7].left_frame_count, 0u, "6A: ... left_frame_count, 0x00437be2");
    ck_eq((uint32_t)fx.door_table[7].left_col_count, 0u, "6A: ... left_col_count, 0x00437bef");
    ck_eq((uint32_t)fx.teleport_zones[9].id, 9u, "6A: TELEPORT 9 -> section=6, id=9, 0x00437c04/0x00437c52");
    // DETONATION (section=7) has NO id field and NO duplicate check at all -- it is a global
    // mine-blast parameter block, not an indexed table; only the section switch is observable,
    // 0x00437c64-0x00437c78.

    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 1u, "6A: STOP (the last line) fires fatal_cleanup, 0x00437529");
    ck_eq((uint32_t)g_rec.abort_calls, 1u, "6A: ... and utils_abort");
    if (g_rec.abort_calls == 1) ck_eq((uint32_t)g_rec.abort_status_args[0], 0u, "6A: utils_abort(0)");

    ck_eq((uint32_t)g_rec.map_compute_bounds_calls, 1u, "6A: MAP -> map_compute_bounds once, 0x0043794f");
    ck_eq((uint32_t)g_rec.tlo_palette_calls, 1u, "6A: GROUND -> tlo_palette_convert_565_to_555 once");
    ck_eq((uint32_t)g_rec.tlo_shade_calls, 1u, "6A: GROUND -> tlo_shade_table_build_tact once");
    ck_eq((uint32_t)g_rec.quoted_string_calls, 2u, "6A: MAP + GROUND each call mission_parse_quoted_string");
    ck_eq((uint32_t)g_rec.coord_pair_calls, 2u, "6A: TARGET + QUIT each call mission_parse_coord_pair");
    ck_eq((uint32_t)g_rec.command_token_calls, 0u, "6A: DISPOSITION has no spawn/command lines in this file");
    ck_eq((uint32_t)g_rec.disposition_spawn_calls, 0u, "6A: ditto");
    ck_eq((uint32_t)g_rec.unit_spawn_calls, 0u, "6A: no stray spawn");
    ck_eq((uint32_t)g_rec.enqueue_calls, 0u, "6A: no stray enqueue");
    ck_eq((uint32_t)g_rec.frame_table_calls, 0u, "6A: no FRAMES line in this file");
    ck_eq((uint32_t)g_rec.door_parse_def_calls, 0u, "6A: no LEFT/RIGHT line in this file");
    ck_eq((uint32_t)g_rec.gfx_convert_calls, 0u, "6A: no PANEL line in this file");
    ck_eq((uint32_t)g_rec.str_cmp_calls, 0u, "6A: utils_str_cmp is PANEL-only, unused here");
    ck_eq((uint32_t)g_rec.get_resource_calls, 3u, "6A: main file + ALIEN_01.MAP + PODLOGA.TLO");
    ck_eq((uint32_t)g_rec.utils_free_calls, 2u, "6A: MAP resource free + EOF free");
}

// 6B: /* */ comment tracking -- a real keyword line, a hand-built /* */ block (no shipped sample
// uses this form) swallowing two further keyword lines whole (their gun_idx/expl_idx MUST NOT be
// touched -- in-bounds indices chosen deliberately so a comment-tracking regression corrupts
// observable fixture state rather than merely going unindexed), then a real keyword line resumes
// after '*/'. Cite 0x004374e0-0x0043751c.
void case6b_comment_tracking() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();

    run_load(fx, {
                     "CHARACTER 3",
                     "/* this whole block is commented out per the mission author",
                     "GUN 20",
                     "EXPLOSION 20",
                     "*/",
                     "DOOR 2",
                 });

    ck_eq((uint32_t)fx.character_types[3].id, 3u, "6B: CHARACTER 3 (before the comment) processed normally");
    ck_eq((uint32_t)fx.fx_type_table_mut[20].id, 0u,
          "6B: 'GUN 20' inside /* */ never dispatched -- fx_type_table[20].id stays at its "
          "init-clear 0, not 20, 0x004374e0-0x0043751c (in_comment -> continue, before ANY "
          "cfg_keyword_token_match call)");
    ck_eq((uint32_t)fx.fx_type_table_mut[0x34].id, 0u,
          "6B: 'EXPLOSION 20' inside /* */ never dispatched -- fx_type_table[0x34] (20+0x20) stays "
          "0, not 0x34, same comment-skip proof");
    ck_eq((uint32_t)fx.door_table[2].id, 2u,
          "6B: DOOR 2 (after the closing '*/') resumes normal dispatch, 0x004374fe-0x00437512");

    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "6B: no stray abort");
    ck_eq((uint32_t)g_rec.abort_calls, 0u, "6B: no stray abort");
    ck_eq((uint32_t)g_rec.unit_spawn_calls, 0u, "6B: no stray spawn");
    ck_eq((uint32_t)g_rec.enqueue_calls, 0u, "6B: no stray enqueue");
    ck_eq((uint32_t)g_rec.quoted_string_calls, 0u, "6B: no stray quoted-string parse");
    ck_eq((uint32_t)g_rec.coord_pair_calls, 0u, "6B: no stray coord-pair parse");
    ck_eq((uint32_t)g_rec.command_token_calls, 0u, "6B: no stray DISPOSITION-section activity");
    ck_eq((uint32_t)g_rec.disposition_spawn_calls, 0u, "6B: ditto");
    ck_eq((uint32_t)g_rec.frame_table_calls, 0u, "6B: no stray FRAMES call");
    ck_eq((uint32_t)g_rec.door_parse_def_calls, 0u, "6B: no stray LEFT/RIGHT call");
    ck_eq((uint32_t)g_rec.map_compute_bounds_calls, 0u, "6B: no MAP line -> no stray bounds call");
    ck_eq((uint32_t)g_rec.tlo_palette_calls, 0u, "6B: no GROUND line -> no stray tlo call");
    ck_eq((uint32_t)g_rec.gfx_convert_calls, 0u, "6B: no PANEL line -> no stray gfx_convert call");
}

// 6C: the five duplicate-record-id fatal-abort checks (CHARACTER/GUN/EXPLOSION/DOOR/TELEPORT). Each
// is a tiny "create id N, then repeat id N" file; none of the five returns early after the abort
// call (matching mission_start.cpp's T2 "no early return" precedent), so the id write and section
// assignment still happen a second time -- checked too, since a translation that DID add a return
// would silently pass a check that only looks at fatal_cleanup/abort counts.
void run_dup_id_case(const char *label, const std::vector<std::string> &lines, const char *addr) {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();
    run_load(fx, lines);
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 1u,
          (std::string(label) + ": duplicate record id triggers fatal_cleanup, " + addr).c_str());
    ck_eq((uint32_t)g_rec.abort_calls, 1u, (std::string(label) + ": ... and utils_abort").c_str());
    if (g_rec.abort_calls == 1)
        ck_eq((uint32_t)g_rec.abort_status_args[0], 0u,
              (std::string(label) + ": utils_abort(0)").c_str());
}
void case6c_duplicate_id_checks() {
    run_dup_id_case("6C-CHARACTER", {"CHARACTER 5", "CHARACTER 5"}, "0x00437a33");
    run_dup_id_case("6C-GUN", {"GUN 5", "GUN 5"}, "0x00437a93");
    run_dup_id_case("6C-EXPLOSION", {"EXPLOSION 5", "EXPLOSION 5"}, "0x00437b06");
    // DOOR's dup case also folds in door_parse_definition's own LEFT-line call (mock every
    // mission_load_calls member): the second DOOR line before the duplicate exercises LEFT.
    {
        tact_fixture fx;
        g_rec.reset();
        g_files.clear();
        run_load(fx, {"DOOR 3", "LEFT 2,1,10,11", "DOOR 3"});
        ck_eq((uint32_t)g_rec.door_parse_def_calls, 1u,
              "6C-DOOR: LEFT dispatches to door_parse_definition once, 0x00438b4f/0x00438b6c");
        if (g_rec.door_parse_def_calls == 1) {
            ck_eq((uint32_t)g_rec.door_parse_def_door_idx[0], 3u,
                  "6C-DOOR: door_parse_definition receives the active door_idx, 0x00438b6c");
        }
        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 1u,
              "6C-DOOR: the second 'DOOR 3' triggers fatal_cleanup, 0x00437b97");
        ck_eq((uint32_t)g_rec.abort_calls, 1u, "6C-DOOR: ... and utils_abort");
    }
    run_dup_id_case("6C-TELEPORT", {"TELEPORT 5", "TELEPORT 5"}, "0x00438c36");
}

// 6D: the real CHARACTER 1 block verbatim (_POZ_SAMPLES.md), proving ALL 19 CHARACTER-section field
// keywords route correctly in one pass, INCLUDING the KNEEL vs KNEEL_GUN1/KNEEL_GUN2 whitespace
// trap the samples doc calls out (KW_KNEEL = "KNEEL  ", two trailing spaces, so it does NOT match
// "KNEEL GUN1      25" which has only one space before "GUN1") and the bogus "DIRECT           1"
// line (DIRECT is a GUN-section keyword, not a CHARACTER-section one -- a real no-op here, proven
// by every field below matching its OWN keyword's value and nothing else).
void case6d_real_character_block() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();
    fx.who_xor_key = 0; // fixture default; WHO 0 -> ch.who == 0 ^ 0 == 0

    run_load(fx, {
                     "CHARACTER 1",
                     "       NAME \"0\"",
                     "       PANEL \"panelb\\ilp_typ.gfx\"",
                     "       WHO 0",
                     "       ANGLE SEE      120",
                     "       DISTANCE SEE    11",
                     "       FIRST FRAME      0",
                     "       NUMBER GUN1      2",
                     "       NUMBER GUN2      1",
                     "       HEIGHT GUN1     40",
                     "       HEIGHT GUN2     40",
                     "       KNEEL GUN1      25",
                     "       KNEEL GUN2      25",
                     "       ENERGY         120",
                     "       SPEED        0.010",
                     "       ROTATE       0.025",
                     "       KNEEL        0.040",
                     "       DEATH        0.030",
                     "       MINE         0.040",
                     "       RUN              2",
                     "       DIRECT           1",
                     "       FRAMES { 8,8,8,8,8,8 }",
                 });

    auto &ch = fx.character_types[1];
    ck((std::string(ch.name) == "0"), "6D: NAME \"0\" -> ch.name, 0x00437d06 (mission_parse_quoted_string)");
    ck_eq((uint32_t)ch.who, 0u, "6D: WHO 0 -> ch.who == trunc(0)^who_xor_key(0) == 0, 0x00437ddf");
    ck_eq((uint32_t)(uint16_t)ch.angle_see, 120u, "6D: ANGLE SEE 120 -> ch.angle_see, 0x00437e1a");
    ck_eq((uint32_t)ch.distance_see, 11u, "6D: DISTANCE SEE 11 -> ch.distance_see, 0x00437e69");
    ck_eq((uint32_t)(uint16_t)ch.first_frame, 0u,
          "6D: FIRST FRAME 0 -> ch.first_frame == trunc(bank_sprite_base_current(0)+0), 0x00437eb1-0x00437ebe");
    ck_eq((uint32_t)ch.number_gun1, 2u,
          "6D: NUMBER GUN1 2 -> ch.number_gun1 == 2, NOT the '1' embedded in the keyword itself, 0x00437f0d");
    ck_eq((uint32_t)ch.number_gun2, 1u, "6D: NUMBER GUN2 1 -> ch.number_gun2, 0x00437f5a");
    ck_eq((uint32_t)ch.height_gun1, 40u, "6D: HEIGHT GUN1 40 -> ch.height_gun1, 0x00437fa7");
    ck_eq((uint32_t)ch.height_gun2, 40u, "6D: HEIGHT GUN2 40 -> ch.height_gun2, 0x00437ff4");
    ck_eq((uint32_t)ch.kneel_gun1, 25u,
          "6D: KNEEL GUN1 25 -> ch.kneel_gun1 -- the double-space KW_KNEEL('KNEEL  ') does NOT "
          "match this line (single space before 'GUN1'), so KNEEL_GUN1 alone claims it, 0x00438041");
    ck_eq((uint32_t)ch.kneel_gun2, 25u, "6D: KNEEL GUN2 25 -> ch.kneel_gun2, 0x0043808e");
    ck_eq((uint32_t)(uint16_t)ch.energy, 120u, "6D: ENERGY 120 -> ch.energy, 0x004380db");
    ck_eq_d(ch.speed, 0.010, "6D: SPEED 0.010 -> ch.speed, no trunc, 0x00438120-0x00438129");
    ck_eq_d(ch.rotate, 0.025, "6D: ROTATE 0.025 -> ch.rotate, 0x00438159");
    ck_eq_d(ch.kneel_time, 0.040,
            "6D: the PLAIN 'KNEEL        0.040' line (many spaces, satisfying KW_KNEEL's double-"
            "space requirement) -> ch.kneel_time, NOT ch.kneel_gun1/2 -- the samples doc's own "
            "'a keyword matcher that stops at the first word gets the wrong one' trap, 0x00438189");
    ck_eq_d(ch.death_time, 0.030, "6D: DEATH 0.030 -> ch.death_time, 0x004381b9");
    ck_eq_d(ch.mine_time, 0.040, "6D: MINE 0.040 -> ch.mine_time, 0x004381e9");
    ck_eq_d(ch.run_speed, 2.0,
            "6D: RUN 2 -> ch.run_speed accepts a bare INTEGER (no decimal point) on a float-typed "
            "field, per the samples doc's 'RUN 2 is an integer on a key whose sibling floats are "
            "not' note, 0x00438219");

    ck_eq((uint32_t)g_rec.frame_table_calls, 1u, "6D: FRAMES { 8,8,8,8,8,8 } -> character_parse_frame_table once, 0x0043823c");
    if (g_rec.frame_table_calls == 1) {
        ck_eq((uint32_t)g_rec.frame_table_char_idx[0], 1u, "6D: ... with char_idx==1");
        ck((g_rec.frame_table_lines[0].find("8,8,8,8,8,8") != std::string::npos),
           "6D: ... and the real brace-delimited list forwarded verbatim");
    }

    ck_eq((uint32_t)g_rec.gfx_convert_calls, 1u,
          "6D: PANEL's non-'ilp_kom.gfx' path -> gfx_convert_pixels_565_to_555 once, 0x00437d9d-0x00437da3");
    ck_eq((uint32_t)g_rec.str_cmp_calls, 1u,
          "6D: PANEL compares the parsed path against the ilp_kom.gfx fallback name once, 0x00437d5a");
    ck_eq((uint32_t)g_rec.get_resource_calls, 2u,
          "6D: main file + the PANEL texture (\"panelb\\ilp_typ.gfx\", NOT swapped -- the real "
          "sample is already the .._typ.gfx path, so the kom->typ substitution does not fire)");

    // The bogus "DIRECT           1" line is not a CHARACTER-section keyword at all (DIRECT only
    // exists in GUN's section-2 body) -- every field above already proves it did nothing; this is
    // the explicit statement of that fact for the reader.
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "6D: no abort anywhere in this block");
    ck_eq((uint32_t)g_rec.unit_spawn_calls, 0u, "6D: no DISPOSITION activity in this file");
}

// ==================================================================================================
// CASE 7 -- MAP: the pre-clear of the 128x128 sub-block, the three memcpy'd sections landing at the
// right offsets, the passable-BLOCKED bit application, the -100 height-sprite bias, and the FULL
// 256x256 flags reset vs. the 128x128-only tile-type/pre-clear steps. Cite 0x004375c8-0x004376f8
// (pre-clear), 0x004376f8-0x0043776c (the three memcpy offsets), 0x00437774-0x00437877 (passable +
// height bias), 0x0043787c-0x004378da (FULL 256x256 flags reset), 0x004378da-0x0043794f (tile-type
// grid apply over the 128x128 sub-block only), 0x0043794f (map_compute_bounds).
// ==================================================================================================
void case7_map_semantics() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();

    std::vector<uint8_t> map_res = make_blank_map_resource();
    // height_sprites: index = col*1024 + row*8 + slot (uint16 units); (col=5,row=10,slot=0) -> raw
    // 300 (subtract 100 -> 200); (col=0,row=0,slot=0) left 0 (must NOT be decremented: the code
    // guards `if (h != 0)`).
    set_u16le(map_res, (size_t)(5 * 1024 + 10 * 8 + 0) * 2, 300);
    // passable_bitmap: bit_index = col + row*0x80; (col=5,row=10) -> bit 1285 set (BLOCKED); (col=6,
    // row=10) left clear.
    {
        size_t bit_index                    = 5 + 10 * 0x80;
        map_res[0x40000 + (bit_index >> 3)] = (uint8_t)(1u << (bit_index & 7));
    }
    // tile_type_grid: index = row*0x80+col; (row=10,col=5) raw 0x0102 -> flags[0]=0x02 (low byte
    // verbatim), flags[1]=0x01+0x40=0x41 (high byte biased by 0x40).
    set_u16le(map_res, 0x40800 + (size_t)(10 * 0x80 + 5) * 2, 0x0102);
    g_files["ALIEN_03.MAP"] = std::move(map_res);

    // Pre-existing garbage the pre-clear step must zero (inside the 128x128 sub-block) and must
    // NOT touch (outside it) -- (5,10) is inside, (200,200) is outside.
    fx.tile_objects[(5u << 8) | 10u].building       = 0xbeef;
    fx.tile_objects[(5u << 8) | 10u].unit[0]        = 0xaa;
    fx.tile_objects[(5u << 8) | 10u].unit[1]        = 0xbb;
    fx.tile_objects[(5u << 8) | 10u].class_owner    = 0xcc;
    fx.tile_objects[(5u << 8) | 10u].visibility     = 0xdd;
    fx.tile_objects[(200u << 8) | 200u].building    = 0xbeef;
    fx.tile_objects[(200u << 8) | 200u].unit[0]     = 0xaa;
    fx.tile_objects[(200u << 8) | 200u].unit[1]     = 0xbb;
    fx.tile_objects[(200u << 8) | 200u].class_owner = 0xcc;
    fx.tile_objects[(200u << 8) | 200u].visibility  = 0xdd;
    // Distinct sentinel so "still BLOCKED by chance" can't hide a bug (PASSABLE_BLOCKED==0).
    for (size_t i = 0; i < fx.passable.size(); ++i) fx.passable[i] = mh::state::PASSABLE_DEFAULT;

    run_load(fx, {"MAP    \"ALIEN_03.MAP\""}, "MISSION.DAT");

    ck_eq((uint32_t)fx.tile_height_sprites[(size_t)(5 * 1024 + 10 * 8 + 0)], 200u,
          "case7: height_sprites(col=5,row=10,slot=0): raw 300 - 100 == 200, 0x00437774-0x00437877");
    ck_eq((uint32_t)fx.tile_height_sprites[0], 0u,
          "case7: height_sprites(col=0,row=0,slot=0): raw 0 stays 0 (the `if (h!=0)` guard), 0x00437774");

    ck_eq((uint32_t)fx.passable[(5u << 8) | 10u], (uint32_t)mh::state::PASSABLE_BLOCKED,
          "case7: passable(5,10) -> BLOCKED, the bit WAS set in the raw bitmap, 0x00437774-0x00437877");
    ck_eq((uint32_t)fx.passable[(6u << 8) | 10u], (uint32_t)mh::state::PASSABLE_DEFAULT,
          "case7: passable(6,10) unchanged (bit not set), still the DEFAULT sentinel, not BLOCKED");

    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].flags[0], 0x02u,
          "case7: tile_type_grid(row=10,col=5) low byte verbatim -> flags[0], 0x004378da-0x0043794f");
    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].flags[1], 0x41u,
          "case7: ... high byte + 0x40 -> flags[1] (0x01+0x40), 0x004378da-0x0043794f");
    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].building, 0u,
          "case7: the pre-clear zeroed building inside the sub-block, 0x004375c8-0x004376f8");
    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].unit[0], 0u, "case7: ... unit[0] zeroed");
    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].unit[1], 0u, "case7: ... unit[1] zeroed");
    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].class_owner, 0u, "case7: ... class_owner zeroed");
    ck_eq((uint32_t)fx.tile_objects[(5u << 8) | 10u].visibility, 0u, "case7: ... visibility zeroed");

    ck_eq((uint32_t)fx.tile_objects[(200u << 8) | 200u].flags[0], 0x00u,
          "case7: (200,200) IS covered by the FULL 256x256 flags reset (0x0043787c-0x004378da) -- "
          "flags[0]==0x00");
    ck_eq((uint32_t)fx.tile_objects[(200u << 8) | 200u].flags[1], 0x40u,
          "case7: ... flags[1]==0x40 (word 0x4000 LE) -- but NOT re-touched by the tile-type-grid "
          "step, which only covers the 128x128 sub-block, 0x004378da-0x0043794f");
    ck_eq((uint32_t)fx.tile_objects[(200u << 8) | 200u].building, 0xbeefu,
          "case7: (200,200) is OUTSIDE the sub-block -- the pre-clear step never reaches it, so "
          "building's pre-seeded garbage SURVIVES, 0x004375c8-0x004376f8 (128x128 bound, not 256x256)");
    ck_eq((uint32_t)fx.tile_objects[(200u << 8) | 200u].unit[0], 0xaau, "case7: ... unit[0] garbage survives");
    ck_eq((uint32_t)fx.tile_objects[(200u << 8) | 200u].class_owner, 0xccu,
          "case7: ... class_owner garbage survives");

    ck_eq((uint32_t)g_rec.map_compute_bounds_calls, 1u, "case7: map_compute_bounds fires once, 0x0043794f");
    ck_eq((uint32_t)g_rec.get_resource_calls, 2u, "case7: main file + ALIEN_03.MAP");
    ck_eq((uint32_t)g_rec.utils_free_calls, 2u, "case7: the MAP resource free + the EOF free");
}

// ==================================================================================================
// CASE 8 -- squad vs enemy spawn routing, and the char_num==1 camera-snap clamp. Cite
// 0x004389a3-0x004389aa (who==0 check), 0x004389b0-0x004389bf (slot.energy_pct>0 gate),
// 0x004389c1-0x004389d0 (is_commando==1 -> char_type forced to 7), the two spawn call sites
// (0x004389d2-0x004389f7 / 0x004389fc-0x00438a1f), 0x00438a22 (squad_size+1, UNCONDITIONAL within
// the who==0 branch), 0x00438a2a-0x00438a44 (enemy branch, hp_pct hardcoded 100, enemy_count+1),
// 0x00438a4a-0x00438a8c (camera snap + the two clamp-to-0 sites at 0x00438a64/0x00438a82).
// ==================================================================================================
void case8_squad_enemy_routing() {
    tact_fixture fx;
    g_rec.reset();
    g_files.clear();

    // char_num 1/2/3 default who==0 (squad); char_num 9 gets who!=0 (enemy).
    fx.character_types[9].who = 4;

    // squad_status slots consumed in squad_size order (0, then 1, then 2).
    //
    // THE GATE IS energy_pct AND THE FORCE-TO-7 TEST IS is_commando -- corrected 2026-09-04
    // (TACT1-P C4) together with the body. Both this case and the TU had read the slot's +0 and +8
    // (unit_proto_id / unit_slot_index), because the original folds the FIELD into the displacement
    // and the listing shows a bare base: [EAX+0xe15e64] is squad_blackboard(0xe15e60)+4 = energy_pct,
    // [EAX+0xe15e6c] is +0xc = is_commando. The rig settled it independently -- on a blackboard with
    // energy_pct=100 and unit_proto_id=0 the old reading spawned NO squad at all and the mission came
    // up with only its 16 enemies. Slot 0 now carries is_commando=1 so the forced-to-7 branch keeps a
    // case; slot 2 stays all-zero so the "empty slot does not spawn" gate still fires on energy_pct.
    fx.squad_status[0] = {/*unit_proto_id*/ 55, /*energy_pct*/ 50, /*unit_slot_index*/ 1, /*is_commando*/ 1};
    fx.squad_status[1] = {/*unit_proto_id*/ 77, /*energy_pct*/ 60, /*unit_slot_index*/ 3, /*is_commando*/ 0};
    fx.squad_status[2] = {/*unit_proto_id*/ 0, /*energy_pct*/ 0, /*unit_slot_index*/ 0, /*is_commando*/ 0};

    // Real DISPOSITION brace shape; only line A carries DIRECT:/DEFENSE: (persists through B/C/D,
    // per case2's own regression coverage -- this case's focus is routing, not persistence).
    run_load(fx, {
                     "DISPOSITION",
                     "       1 { 3,5 }  DIRECT:2  DEFENSE:GUARD2",
                     "       2 { 20,20 }",
                     "       3 { 30,30 }",
                     "       9 { 40,41 }",
                 });

    ck_eq((uint32_t)g_rec.unit_spawn_calls, 3u,
          "case8: 3 unit_spawn calls -- lines A,B,D spawn; line C's empty blackboard slot does NOT "
          "(energy_pct==0), 0x004389b0-0x004389bf");
    if (g_rec.unit_spawn_calls == 3) {
        ck_eq((uint32_t)g_rec.spawn_calls[0].char_type, 7u,
              "case8 A (who==0, is_commando==1): char_type is FORCED to 7, not char_num(1), "
              "0x004389c1-0x004389d0");
        ck_eq((uint32_t)g_rec.spawn_calls[0].hp_pct, 50u,
              "case8 A: hp_pct == slot.energy_pct (50), 0x004389d2-0x004389f7");
        ck_eq((uint32_t)g_rec.spawn_calls[0].fd, 2u, "case8 A: facing_dir from this line's DIRECT:2");
        ck_eq((uint32_t)g_rec.spawn_calls[0].ds, 2u, "case8 A: def_stat from this line's DEFENSE:GUARD2");

        ck_eq((uint32_t)g_rec.spawn_calls[1].char_type, 2u,
              "case8 B (who==0, is_commando==0): char_type == char_num(2), NOT forced to 7, "
              "0x004389fc-0x00438a1f");
        ck_eq((uint32_t)g_rec.spawn_calls[1].hp_pct, 60u, "case8 B: hp_pct == slot.energy_pct (60)");

        ck_eq((uint32_t)g_rec.spawn_calls[2].char_type, 9u,
              "case8 D (who!=0, enemy): char_type == char_num(9)");
        ck_eq((uint32_t)g_rec.spawn_calls[2].hp_pct, 100u,
              "case8 D: hp_pct hardcoded 100 for the enemy branch, IGNORING any blackboard value, "
              "0x00438a2a-0x00438a44");
        ck_eq((uint32_t)g_rec.spawn_calls[2].fd, 2u, "case8 D: fd still inherited from line A");
        ck_eq((uint32_t)g_rec.spawn_calls[2].ds, 2u, "case8 D: ds still inherited from line A");
    }

    ck_eq((uint32_t)fx.squad_size, 3u,
          "case8: squad_size incremented for A, B AND C (3 total) -- UNCONDITIONAL within the "
          "who==0 branch, even for C's empty slot that spawned nothing, 0x00438a22");
    ck_eq((uint32_t)fx.enemy_count, 1u, "case8: enemy_count incremented once, for D only, 0x00438a44");

    ck_eq((uint32_t)fx.map_cam_col, 0u,
          "case8: char_num==1 (line A) triggers the camera snap; col-7 == 3-7 == -4 clamps to 0, "
          "0x00438a56/0x00438a64");
    ck_eq((uint32_t)fx.map_cam_row, 0u,
          "case8: ... row-10 == 5-10 == -5 clamps to 0, 0x00438a74/0x00438a82");

    ck_eq((uint32_t)g_rec.disposition_spawn_calls, 4u, "case8: 4 spawn-shaped lines, no command lines");
    ck_eq((uint32_t)g_rec.command_token_calls, 4u, "case8: command_token tried on all 4, none match (no '-')");
    ck_eq((uint32_t)g_rec.enqueue_calls, 0u, "case8: no command lines -> no enqueue calls");
    ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "case8: no stray abort");
}

} // namespace

void run_mission_load_tests() {
    case1_ground_regression();
    case2_disposition_persistence();
    case3_init_overruns();
    case4_explosion_wrong_index();
    case5_time_dead_keyword();
    case6a_global_keyword_routing();
    case6b_comment_tracking();
    case6c_duplicate_id_checks();
    case6d_real_character_block();
    case7_map_semantics();
    case8_squad_enemy_routing();
}

} // namespace mh::tact::test
