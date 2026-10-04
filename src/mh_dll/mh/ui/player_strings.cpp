//
// ui/player_strings.cpp -- mods:LANG4: the compiled-in English table and the per-pack loader.
// Contract: ui/player_strings.h. Rows: ui/player_strings.def.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "ui/player_strings.h"
#include "include/mh_run_context.h" // mh_run_path -- mh_video.log, the lang pack's advisory channel
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "mh_net_proto/text_utf8.h" // utf8_to_utf16: the pack file is UTF-8

#pragma comment(lib, "user32.lib") // wsprintfW / wvsprintfA

namespace mh {
namespace ui {
namespace {

struct Row {
    const char    *key;
    const wchar_t *en;
};

const Row kRows[STR_COUNT] = {
#define MH_STR(id, key, en) {key, en},
#include "ui/player_strings.def"
#undef MH_STR
};

wchar_t g_pool[STR_COUNT][STR_MAX]; // the loaded translations
bool    g_have[STR_COUNT];          // g_pool[i] is valid
int     g_loaded = 0;
bool    g_done   = false;

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

// mh_video.log: the channel lang_pack.cpp already uses for the pack's own lines. NOT mh_net.log,
// whose arm-window order tools/check_arm_order.py gates -- and a stock run writes nothing here.
void ps_log(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[400];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_log, line, lstrlenA(line));
}

int find_key(const char *k, size_t n) {
    for (int i = 0; i < STR_COUNT; ++i)
        if (strlen(kRows[i].key) == n && memcmp(kRows[i].key, k, n) == 0) return i;
    return -1;
}

// The next printf conversion in s at or after *i: 's' 'd' 'u' 'L' (= %lu), or 0 at the end. %% skips.
wchar_t next_conv(const wchar_t *s, int *i) {
    while (s[*i]) {
        if (s[*i] != L'%') {
            ++*i;
            continue;
        }
        const wchar_t c = s[*i + 1];
        if (c == L'%') {
            *i += 2;
            continue;
        }
        if (c == L'l' && s[*i + 2] == L'u') {
            *i += 3;
            return L'L';
        }
        *i += (c ? 2 : 1);
        return c ? c : L'?';
    }
    return 0;
}

// Match an ASCII line against an English pattern whose only conversions are %u: literal text equal,
// each %u one run of digits. Returns the number of values (0..2) or -1.
int match_u(const wchar_t *pat, const char *s, unsigned *vals) {
    int nv = 0;
    for (; *pat; ++pat) {
        if (pat[0] == L'%' && pat[1] == L'u') {
            if (*s < '0' || *s > '9' || nv >= 2) return -1;
            unsigned v = 0;
            for (; *s >= '0' && *s <= '9'; ++s) v = v * 10u + (unsigned)(*s - '0');
            vals[nv++] = v;
            ++pat;
            continue;
        }
        if ((wchar_t)(unsigned char)*s != *pat) return -1;
        ++s;
    }
    return *s ? -1 : nv;
}

int localize(const Str *ids, int nids, const char *ascii, wchar_t *out, int cap, bool *matched) {
    if (matched) *matched = false;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!ascii) return 0;
    for (int k = 0; k < nids; ++k) {
        unsigned  v[2] = {0, 0};
        const int nv   = match_u(str_en(ids[k]), ascii, v);
        if (nv < 0) continue;
        wchar_t buf[1100];
        wsprintfW(buf, tr(ids[k]), v[0], v[1]); // same shape as the English: validated at load
        lstrcpynW(out, buf, cap);
        if (matched) *matched = true;
        return lstrlenW(out);
    }
    int n = 0; // not one of ours: the received text, widened as-is (it is ASCII by protocol)
    for (const char *p = ascii; *p && n < cap - 1; ++p) out[n++] = (wchar_t)(unsigned char)*p;
    out[n] = 0;
    return n;
}

} // namespace

const wchar_t *tr(Str s) {
    const int i = (int)s;
    if (i < 0 || i >= STR_COUNT) return L"";
    return g_have[i] ? g_pool[i] : kRows[i].en;
}
const char    *str_key(Str s) { return ((int)s >= 0 && (int)s < STR_COUNT) ? kRows[(int)s].key : ""; }
const wchar_t *str_en(Str s) { return ((int)s >= 0 && (int)s < STR_COUNT) ? kRows[(int)s].en : L""; }
int            strings_loaded_count() { return g_loaded; }

bool str_format_compatible(const wchar_t *a, const wchar_t *b) {
    int ia = 0, ib = 0;
    for (;;) {
        const wchar_t ca = next_conv(a, &ia), cb = next_conv(b, &ib);
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

void strings_parse(const char *text, size_t n, wchar_t (*out)[STR_MAX], bool *out_have, StrLoadReport *rep) {
    StrLoadReport r = {};
    for (int i = 0; i < STR_COUNT; ++i) out_have[i] = false;
    size_t p = 0;
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        p = 3; // a UTF-8 BOM
    while (p < n) {
        size_t e = p;
        while (e < n && text[e] != '\n') ++e;
        size_t a = p, b = e;
        p = e + 1;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
        while (b > a && (text[b - 1] == '\r' || text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
        if (a == b || text[a] == '#') continue;
        ++r.lines;
        size_t eq = a;
        while (eq < b && text[eq] != '=') ++eq;
        size_t kb = eq;
        while (kb > a && (text[kb - 1] == ' ' || text[kb - 1] == '\t')) --kb;
        if (eq == b || kb == a) {
            ++r.malformed;
            continue;
        }
        size_t vb = eq + 1;
        while (vb < b && (text[vb] == ' ' || text[vb] == '\t')) ++vb;
        const int id = find_key(text + a, kb - a);
        if (id < 0) {
            ++r.unknown;
            continue;
        }
        if (out_have[id]) {
            ++r.duplicate;
            continue;
        }
        mh_net_proto::utf8_to_utf16(text + vb, b - vb, (uint16_t *)out[id], STR_MAX);
        if (!out[id][0] || !str_format_compatible(out[id], kRows[id].en)) {
            ++r.bad_format; // an empty value counts here too: it would print nothing
            continue;
        }
        out_have[id] = true;
        ++r.loaded;
    }
    if (rep) *rep = r;
}

void strings_load(const char *exe_dir, const char *id, const char *drop_key) {
    if (g_done) return;
    g_done = true;
    if (!exe_dir || !id || !id[0]) return; // stock: English, nothing read
    char path[MAX_PATH];
    wsprintfA(path, "%slang\\%s\\mh_strings.txt", exe_dir, id);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ps_log("; [lang] strings (%s): no lang\\%s\\mh_strings.txt -- mh.dll's own text stays English", id, id);
        return;
    }
    static char buf[64 * 1024];
    DWORD       rd = 0;
    const BOOL  ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd >= sizeof(buf) - 1) {
        ps_log("; [lang] strings (%s): mh_strings.txt unreadable or over %u KB -- English", id,
               (unsigned)(sizeof(buf) / 1024));
        return;
    }
    StrLoadReport r = {};
    strings_parse(buf, rd, g_pool, g_have, &r);
    if (drop_key && drop_key[0]) {
        const int k = find_key(drop_key, strlen(drop_key));
        if (k >= 0 && g_have[k]) {
            g_have[k] = false;
            --r.loaded;
            ps_log("; [lang] strings (%s): TEST [lang] test_drop_string=%s -- that key shows its English", id,
                   drop_key);
        }
    }
    g_loaded = r.loaded;
    ps_log("; [lang] strings (%s): %d of %d keys translated (%d unknown, %d bad format -> English, %d duplicate, "
           "%d malformed line(s))",
           id, r.loaded, STR_COUNT, r.unknown, r.bad_format, r.duplicate, r.malformed);
    for (int i = 0; i < STR_COUNT; ++i)
        if (!g_have[i]) ps_log("; [lang] strings (%s):   %s -> English", id, kRows[i].key);
}

int tr_refusal_reason(const char *ascii, wchar_t *out, int cap, bool *matched) {
    static const Str ids[] = {Str::REFUSE_OLD_CLIENT_CP, Str::REFUSE_OLD_HOST_CP, Str::REFUSE_CODEPAGE,
                              Str::REFUSE_NEWER_PROTOCOL, Str::REFUSE_OLDER_PROTOCOL, Str::REFUSE_MALFORMED,
                              Str::REFUSE_NO_MATCH_ID, Str::REFUSE_NEWER_THAN_HOST, Str::REFUSE_NOT_OUR_LOBBY,
                              Str::REFUSE_CP_MISMATCH, Str::REFUSE_UNKNOWN};
    return localize(ids, (int)(sizeof(ids) / sizeof(ids[0])), ascii, out, cap, matched);
}

int tr_relay_line(const char *ascii, wchar_t *out, int cap, bool *matched) {
    static const Str ids[] = {Str::RELAY_OUTDATED};
    return localize(ids, 1, ascii, out, cap, matched);
}

} // namespace ui
} // namespace mh
