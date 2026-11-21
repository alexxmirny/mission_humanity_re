//
// mods/save_meta.cpp -- DATAEXT1: the outer .sav mod-metadata carrier (contract: mods/save_meta.h).
// Pure over the file_io and active_set_source seams; caps_install.cpp owns the hook sites.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "mods/save_meta.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "addr/mh_addrs.gen.h" // G_TEXT_TMP (the notice buffer the other seams use)
#include "addr/mh_calls.gen.h" // the game's CRT file primitives + the floating message
#include "mh_mod_catalog.h"    // ADAPTER: mh::mods::mounted_snapshot() -- see install_active_set_source
#include "ui/player_strings.h" // tr(Str::MODS_SAVE_*)

namespace mh::mods::save {

namespace {

void (*g_log)(const char *)               = nullptr;
const active_set_source *g_src            = nullptr;
bool                     g_hooks_ready    = false;
bool                     g_new_game_ready = false;
bool                     g_continue_ready = false;

void say(const char *s) {
    if (g_log) g_log(s);
}

// ---- the provenance state ------------------------------------------------------------------------------------
// set_record is ~6.5 KB at MAX_MODS: it lives only in file-scope statics, never on the game's stack.
enum class provenance { BOOT,
                        LOADED,
                        UNKNOWN };

struct set_record {
    uint16_t count                                = 0;
    uint8_t  digest[32]                           = {};
    char     id[MAX_MODS][MAX_ID_BYTES + 1]       = {};
    char     version[MAX_MODS][MAX_VER_BYTES + 1] = {};
};

set_record g_mounted; // re-read from the source on every use (no stale boot-time cache)
set_record g_loaded;  // valid when g_prov == LOADED (as read from the carrier)
provenance g_prov                       = provenance::BOOT;
bool       g_session_origin_established = false;

// the staged probe (one load in flight at a time; the game is single-threaded here)
bool       g_stage_valid = false;
bool       g_stage_known = false; // a carrier parsed (true) or absent/malformed (false)
set_record g_stage_set;
verdict    g_stage_verdict = verdict::NONE;

uint8_t g_blob[MAX_BLOB_BYTES]; // shared scratch: one serialization or one bounded parse at a time

void clear_stage() {
    g_stage_valid   = false;
    g_stage_known   = false;
    g_stage_set     = set_record{};
    g_stage_verdict = verdict::NONE;
}

// ---- strict field grammars ------------------------------------------------------------------------------------
bool valid_id(const char *s, size_t n) {
    if (n < 1 || n > MAX_ID_BYTES) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true; // the grammar excludes NUL, so strlen(s) == n holds by construction
}

// Opaque UTF-8, printable: no C0 control, no DEL, well-formed sequences only (no overlongs,
// surrogates, or values past U+10FFFF); strlen(s) == n (a NUL inside would fail the control test
// only at 0x00 -- checked explicitly).
bool valid_version(const char *s, size_t n) {
    if (n > MAX_VER_BYTES) return false;
    size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0 || c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        const int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3
                                    : c >= 0xC2   ? 2
                                                  : 0;
        uint32_t  cp  = 0;
        if (len == 0 || i + (size_t)len > n) return false;
        for (int k = 1; k < len; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        cp = len == 4   ? ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) | ((uint32_t)(s[i + 2] & 0x3F) << 6) | (uint32_t)(s[i + 3] & 0x3F)
             : len == 3 ? ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) | (uint32_t)(s[i + 2] & 0x3F)
                        : ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[i + 1] & 0x3F);
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false; // overlong
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp <= 0x9F)) return false;
        i += (size_t)len;
    }
    return true;
}

// ---- the mounted set, freshly read through the seam on every use ----------------------------------------------
bool read_mounted(set_record *out) {
    *out = set_record{};
    if (!g_src) return true; // no snapshot mounted: stock (count 0)
    const unsigned n = g_src->count(g_src->ctx);
    if (n > MAX_MODS) { // the catalog rejects larger sets at mount; this line is the carrier-side backstop
        say("; [mods] save: the mounted set exceeds the carrier bound -- metadata NOT written\n");
        return false;
    }
    for (unsigned i = 0; i < n; ++i) {
        char id[MAX_ID_BYTES + 1]   = {};
        char ver[MAX_VER_BYTES + 1] = {};
        if (!g_src->id_of(g_src->ctx, i, id, sizeof(id)) || !valid_id(id, strlen(id)) ||
            !g_src->version_of(g_src->ctx, i, ver, sizeof(ver)) || !valid_version(ver, strlen(ver))) {
            say("; [mods] save: the mounted set violates the carrier bounds -- metadata NOT written\n");
            return false;
        }
        out->count = static_cast<uint16_t>(i + 1);
        memcpy(out->id[i], id, strlen(id) + 1);
        memcpy(out->version[i], ver, strlen(ver) + 1);
    }
    g_src->digest_of(g_src->ctx, out->digest);
    return true;
}

size_t serialize(const set_record &s, uint8_t *out) {
    size_t p = 0;
    out[p++] = (uint8_t)CARRIER_VERSION;
    out[p++] = (uint8_t)CARRIER_FLAGS;
    out[p++] = (uint8_t)(s.count & 0xff);
    out[p++] = (uint8_t)(s.count >> 8);
    memcpy(out + p, s.digest, 32);
    p += 32;
    for (unsigned i = 0; i < s.count; ++i) {
        const size_t idn = strlen(s.id[i]), vern = strlen(s.version[i]);
        out[p++] = (uint8_t)idn;
        out[p++] = (uint8_t)vern;
        memcpy(out + p, s.id[i], idn);
        p += idn;
        memcpy(out + p, s.version[i], vern);
        p += vern;
    }
    return p;
}

// A truncation-safe append cursor: _snprintf_s/_TRUNCATE returns -1 when it truncates, so a raw
// accumulate would wrap the offset. On the first truncation the buffer already holds a valid,
// NUL-terminated prefix; stop there.
struct text {
    char  *p;
    size_t cap, n = 0;
    bool   cut = false;
    void   add(const char *fmt, ...) {
        if (cut || n + 1 >= cap) {
            cut = true;
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        const int r = _vsnprintf_s(p + n, cap - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (r < 0) { // truncated: the prefix is valid, nothing more fits
            cut        = true;
            p[cap - 1] = 0;
            n          = cap - 1;
        } else {
            n += (size_t)r;
        }
    }
};

void fmt_set(text &t, const set_record &s) {
    t.add("[");
    for (unsigned i = 0; i < s.count && i < 4; ++i) t.add("%s%s@%s", i ? ", " : "", s.id[i], s.version[i]);
    if (s.count > 4) t.add(", +%u more", s.count - 4u);
    t.add("]");
}

bool same_set(const set_record &a, const set_record &b) {
    if (a.count != b.count) return false;
    for (unsigned i = 0; i < a.count; ++i)
        if (strcmp(a.id[i], b.id[i]) != 0 || strcmp(a.version[i], b.version[i]) != 0) return false;
    if (a.count && memcmp(a.digest, b.digest, 32) != 0) return false;
    return true;
}

// Parser: strict. Every check runs BEFORE the caller may have read anything (the caller bounds the
// length first); the blob itself is fully in memory here. false = malformed (treated as absent).
bool parse_blob(const uint8_t *b, uint32_t len, set_record *out, const char **why) {
    *out = set_record{};
    if (len < MIN_BLOB_BYTES) {
        *why = "length below minimum";
        return false;
    }
    uint32_t p = 0;
    if (b[p++] != CARRIER_VERSION) {
        *why = "version";
        return false;
    }
    if (b[p++] != CARRIER_FLAGS) { // unknown bits are malformed: a variant bumps the version instead
        *why = "flags";
        return false;
    }
    const uint16_t count = (uint16_t)(b[p] | (b[p + 1] << 8));
    p += 2;
    if (count > MAX_MODS) {
        *why = "entry count";
        return false;
    }
    memcpy(out->digest, b + p, 32);
    p += 32;
    for (unsigned i = 0; i < count; ++i) {
        if (p + 2 > len) {
            *why = "truncated entry header";
            return false;
        }
        const uint8_t idn = b[p++], vern = b[p++];
        if (idn < 1 || idn > MAX_ID_BYTES || vern > MAX_VER_BYTES || (uint64_t)p + idn + vern > len) {
            *why = "entry bounds";
            return false;
        }
        if (!valid_id((const char *)b + p, idn) || !valid_version((const char *)b + p + idn, vern)) {
            *why = "entry grammar";
            return false;
        }
        for (unsigned k = 0; k < i; ++k)
            if (memcmp(out->id[k], b + p, idn) == 0 && out->id[k][idn] == 0) {
                *why = "duplicate id";
                return false;
            }
        memcpy(out->id[i], b + p, idn);
        out->id[i][idn] = 0;
        p += idn;
        memcpy(out->version[i], b + p, vern);
        out->version[i][vern] = 0;
        p += vern;
    }
    if (p != len) { // strict: the entries exactly consume the blob (forward variants get a new version)
        *why = "trailing bytes";
        return false;
    }
    out->count = count;
    return true;
}

// Footer at END-8: {u32 blob_len, u32 magic}. Wide arithmetic only: blob_pos is computed in int64 so
// an attacker-sized len cannot overflow the int32 domain. Leaves the position wherever; the caller
// restores and validates BEFORE reading the blob. false = no footer.
bool read_footer(const file_io &io, void *f, uint32_t *len, int64_t *blob_pos) {
    if (io.seek(f, -(int32_t)FOOTER_BYTES, 2) != 0) return false;
    const int32_t at = io.tell(f);
    if (at < (int32_t)FOOTER_BYTES) return false;
    uint8_t foot[FOOTER_BYTES];
    if (io.read(f, foot, FOOTER_BYTES) != FOOTER_BYTES) return false;
    uint32_t magic = 0, n = 0;
    memcpy(&magic, foot + 4, 4);
    memcpy(&n, foot, 4);
    if (magic != FOOTER_MAGIC) return false;
    *len      = n;
    *blob_pos = (int64_t)at - (int64_t)n; // may be negative / overlap the header: the caller range-checks
    return true;
}

// The file position is restored by the caller on EVERY path. The mounted set is already read.
verdict probe_absent() {
    g_stage_valid   = true;
    g_stage_known   = false;
    g_stage_set     = set_record{};
    g_stage_verdict = g_mounted.count ? verdict::LEGACY_UNCERTAIN : verdict::NONE;
    return g_stage_verdict;
}

} // namespace

void set_logger(void (*fn)(const char *)) { g_log = fn; }
void set_hooks_ready(bool armed) { g_hooks_ready = armed; }
void set_new_game_ready(bool armed) { g_new_game_ready = armed; }
void set_continue_ready(bool armed) { g_continue_ready = armed; }
bool hooks_ready() { return g_hooks_ready && g_new_game_ready && g_continue_ready; }

void set_active_set_source(const active_set_source *s) { g_src = s; }

verdict probe_container_io(const file_io &io, void *f) {
    clear_stage();
    if (!read_mounted(&g_mounted)) { // the mounted set violates the carrier bounds: nothing to stage
        g_stage_valid   = true;
        g_stage_verdict = verdict::NONE;
        return verdict::NONE;
    }
    const int32_t pos = io.tell(f);
    if (pos < 0) { // cannot tell: behave as the vanilla reader
        g_stage_valid   = true;
        g_stage_verdict = verdict::NONE;
        return verdict::NONE;
    }
    uint32_t len     = 0;
    int64_t  blob_at = 0;
    verdict  v       = verdict::NONE;
    if (!read_footer(io, f, &len, &blob_at)) {
        v = probe_absent();
    } else if (blob_at < 40 /* past the version header */ || len < MIN_BLOB_BYTES || len > MAX_BLOB_BYTES) {
        // validated BEFORE any seek/read of the blob: an attacker-sized length can never reach g_blob
        say("; [mods] load: mod metadata malformed (bounds) -- treated as absent\n");
        v = probe_absent();
    } else if (io.seek(f, (int32_t)blob_at, 0) != 0 || io.read(f, g_blob, len) != len) {
        say("; [mods] load: mod metadata malformed (short read) -- treated as absent\n");
        v = probe_absent();
    } else {
        const char *why = nullptr;
        if (!parse_blob(g_blob, len, &g_stage_set, &why)) {
            char m[160];
            _snprintf_s(m, sizeof(m), _TRUNCATE, "; [mods] load: mod metadata malformed (%s, %u bytes) -- treated as absent\n", why, (unsigned)len);
            say(m);
            v = probe_absent();
        } else {
            g_stage_known   = true;
            g_stage_valid   = true;
            g_stage_verdict = same_set(g_stage_set, g_mounted) ? verdict::MATCH : verdict::MISMATCH;
            v               = g_stage_verdict;
            char abuf[420], bbuf[420], m[900];
            text a{abuf, sizeof(abuf)}, b{bbuf, sizeof(bbuf)};
            fmt_set(a, g_stage_set);
            fmt_set(b, g_mounted);
            if (v == verdict::MISMATCH) {
                text mm{m, sizeof(m)};
                mm.add("; [mods] load: save mods ");
                mm.add("%s", a.p);
                mm.add(" != active ");
                mm.add("%s", b.p);
                mm.add(" -- warning queued\n");
                say(mm.cut ? "; [mods] load: save mods differ from active -- warning queued\n" : m);
            } else if (v == verdict::LEGACY_UNCERTAIN) {
                text mm{m, sizeof(m)};
                mm.add("; [mods] load: no mod metadata in the save, active ");
                mm.add("%s", b.p);
                mm.add(" -- warning queued\n");
                say(mm.cut ? "; [mods] load: no mod metadata in the save, active mods present -- warning queued\n" : m);
            }
        }
    }
    io.seek(f, pos, 0); // EVERY path
    return v;
}

verdict commit_staged() {
    if (!g_stage_valid) return verdict::NONE;
    const verdict v = g_stage_verdict;
    if (g_stage_known) {
        g_prov   = provenance::LOADED;
        g_loaded = g_stage_set;
    } else {
        g_prov = provenance::UNKNOWN; // uncertainty persists: the writer appends nothing
    }
    g_session_origin_established = true;
    clear_stage();
    return v;
}

int append_origin(const file_io &io, void *f) {
    const set_record *out = nullptr;
    if (g_prov == provenance::UNKNOWN) return 0; // absence is the on-disk marker for unknown
    if (g_prov == provenance::LOADED) {
        out = &g_loaded;
    } else {
        if (!read_mounted(&g_mounted)) return -1;
        out = &g_mounted;
    }
    if (out->count == 0) return 0; // known-empty: the ordinary save stays byte-vanilla
    const size_t  n                  = serialize(*out, g_blob);
    const uint8_t foot[FOOTER_BYTES] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24),
                                        (uint8_t)FOOTER_MAGIC, (uint8_t)(FOOTER_MAGIC >> 8), (uint8_t)(FOOTER_MAGIC >> 16),
                                        (uint8_t)(FOOTER_MAGIC >> 24)};
    if (io.write(f, g_blob, (uint32_t)n) != n || io.write(f, foot, FOOTER_BYTES) != FOOTER_BYTES) {
        // A torn tail (blob without footer) has no magic, so every reader treats it as absent.
        say("; [mods] save: mod metadata NOT written (short write)\n");
        return -1;
    }
    char m[128];
    _snprintf_s(m, sizeof(m), _TRUNCATE, "; [mods] save: mod metadata appended (%u bytes, %u mod(s))\n", (unsigned)(n + FOOTER_BYTES), (unsigned)out->count);
    say(m);
    return (int)(n + FOOTER_BYTES);
}

void on_new_game(bool boot_config_confirmed) {
    g_prov                       = boot_config_confirmed ? provenance::BOOT : provenance::UNKNOWN;
    g_session_origin_established = true;
    clear_stage();
}

void on_campaign_continue() {
    if (!g_session_origin_established) g_prov = provenance::UNKNOWN;
    g_session_origin_established = true;
    clear_stage();
}

void reset_for_test() {
    g_log                        = nullptr;
    g_src                        = nullptr;
    g_hooks_ready                = false;
    g_new_game_ready             = false;
    g_continue_ready             = false;
    g_mounted                    = set_record{};
    g_loaded                     = set_record{};
    g_prov                       = provenance::BOOT;
    g_session_origin_established = false;
    clear_stage();
}

// ---- production wiring ---------------------------------------------------------------------------------------
namespace {
int32_t  crt_tell(void *f) { return mh::call::file_tell(f); }
int      crt_seek(void *f, int32_t off, int whence) { return mh::call::file_seek(f, off, (uint32_t)whence); }
uint32_t crt_read(void *f, void *dst, uint32_t n) {
    return n && mh::call::utils_read_from_file(dst, n, 1, f) == 1 ? n : 0;
}
uint32_t crt_write(void *f, const void *src, uint32_t n) {
    return n && mh::call::utils_write_to_file((uint32_t)(uintptr_t)f, 1, const_cast<void *>(src), n) == 1 ? n : 0;
}
const file_io CRT_IO = {&crt_tell, &crt_seek, &crt_read, &crt_write};

// ADAPTER over the catalog's immutable snapshot. A NULL snapshot (an ordinary no-mod boot, before or
// without any mount) is stock: count 0, the stock identity -- exactly what a failure boot publishes;
// no fake snapshot object.
unsigned src_count(void *) {
    const auto *s = mh::mods::mounted_snapshot();
    return s ? (unsigned)s->mods.size() : 0u;
}
bool src_string(void *ctx, unsigned i, char *out, unsigned cap, bool id) {
    const auto *s = mh::mods::mounted_snapshot();
    if (!s || i >= s->mods.size() || !out || cap == 0) return false;
    const std::string &v = id ? s->mods[i].id : s->mods[i].version;
    if (v.size() + 1 > cap) return false;
    memcpy(out, v.c_str(), v.size() + 1);
    return true;
}
bool src_id(void *c, unsigned i, char *o, unsigned cap) { return src_string(c, i, o, cap, true); }
bool src_version(void *c, unsigned i, char *o, unsigned cap) { return src_string(c, i, o, cap, false); }
void src_digest(void *, unsigned char out[32]) {
    const auto *s = mh::mods::mounted_snapshot();
    // the stock identity when nothing is mounted (never consumed while count == 0)
    memcpy(out, s ? s->digest : mh::mods::nomod_digest(), 32);
}
const active_set_source PRODUCTION_SOURCE = {nullptr, &src_count, &src_id, &src_version, &src_digest};

void show_notice(const wchar_t *text, const char *what) {
    wsprintfW((wchar_t *)mh::addr::G_TEXT_TMP, L"%s", text);
    mh::call::llm_ui_print_floating_msg_red((void *)mh::addr::G_TEXT_TMP);
    char m[128];
    _snprintf_s(m, sizeof(m), _TRUNCATE, "; [mods] load notice shown (%s)\n", what);
    say(m);
}
} // namespace

void install_active_set_source() { set_active_set_source(&PRODUCTION_SOURCE); } // called once by the resource boot arm (post-mount)

// ---- the naked-stub entry points -----------------------------------------------------------------------------
extern "C" {

uintptr_t g_hdr_read_file = 0; // both header-read stubs store the FILE here before the read clobbers ECX


void mh_mods_save_on_header_probe(void) {
    if (!g_hdr_read_file) return;
    probe_container_io(CRT_IO, (void *)g_hdr_read_file);
}

void mh_mods_save_commit_and_notify(void) {
    switch (commit_staged()) {
        case verdict::MISMATCH: show_notice(mh::ui::tr(mh::ui::Str::MODS_SAVE_MISMATCH), "mismatch"); break;
        case verdict::LEGACY_UNCERTAIN: show_notice(mh::ui::tr(mh::ui::Str::MODS_SAVE_LEGACY), "legacy"); break;
        default: break; // MATCH / NONE: silent
    }
}
}

} // namespace mh::mods::save
