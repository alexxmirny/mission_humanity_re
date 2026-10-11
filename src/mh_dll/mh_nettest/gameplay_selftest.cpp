//
// gameplay_selftest.cpp -- `net_selftest.exe gameplaytest`: PT-SET1 S3, the host-synced gameplay-settings
// machinery (mh_common/include/mh_gameplay.h) driven with a FAKE registry/hook table, so it is proven
// before any real row carries F_HOSTSYNC (the live table is empty until S5):
//   * build_specs keeps only the F_HOSTSYNC bool/int/float rows, in order, with the parsed range + key_id
//   * read_local: defaults for an absent ini key, the ini value when present, the float clamp
//     (low / high / NaN / garbage / trailing ;comment), bool normalisation
//   * local -> wire -> set: encode/decode/from_set returns exactly the values (float bits exact)
//   * from_set: an unknown key is skipped and logged, a missing key takes the default and is logged,
//     a type mismatch is a default + log
//   * dispatch runs the hooks in registry order, once each, skips a spec without a hook
//   * summary_text: "key=value;..." with %.17g floats; the digest is order/value independent
//   * the LIVE registry table is self-consistent (vacuous while it has no F_HOSTSYNC row)
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mh_gameplay.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

char g_lines[32][200];
int  g_nlines = 0;
void capture(const char *s) {
    if (g_nlines < 32) lstrcpynA(g_lines[g_nlines++], s, 200);
}
int count_lines(const char *needle) {
    int c = 0;
    for (int i = 0; i < g_nlines; ++i)
        if (strstr(g_lines[i], needle)) ++c;
    return c;
}

using namespace mh_ini_detail; // T_* / F_*

const mh_gameplay::reg FAKE[] = {
    {"net", "gp_flag", T_BOOL, "1", "", F_HOSTSYNC},
    {"net", "plain_dev", T_INT, "5", "", F_NONE}, // not synced: must be filtered out
    {"net", "gp_mult", T_FLOAT, "2.0", "0.1..10", F_HOSTSYNC},
    {"Gameplay", "Gp_Count", T_INT, "7", "", F_HOSTSYNC | F_RESTART},
    {"net", "gp_name", T_STRING, "", "", F_HOSTSYNC}, // a string row cannot be synced: filtered out
};

struct fake_fx {
    int    order[8];
    int    n    = 0;
    int    flag = -1, count = -1;
    double mult = -1;
};

bool write_file(const char *path, const char *bytes) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      w  = 0;
    const int  n  = (int)strlen(bytes);
    const bool ok = WriteFile(h, bytes, (DWORD)n, &w, nullptr) != 0 && (int)w == n;
    CloseHandle(h);
    return ok;
}

void ini_path(char *out, const char *tag) {
    char base[MAX_PATH];
    GetTempPathA(MAX_PATH, base);
    wsprintfA(out, "%smh_gameplay_%s_%lu.ini", base, tag, GetCurrentProcessId());
}

} // namespace

int run_gameplaytest() {
    g_checks = g_fails = 0;
    using namespace mh_gameplay;
    using mh_net_proto::GameplaySet;

    // ---- build_specs ----
    spec      sp[MAX_SPECS];
    const int n = build_specs(FAKE, (int)(sizeof(FAKE) / sizeof(FAKE[0])), sp, MAX_SPECS);
    check("build_specs keeps the 3 synced bool/int/float rows", n == 3);
    check("order is registry order", n == 3 && !strcmp(sp[0].key, "gp_flag") && !strcmp(sp[1].key, "gp_mult") && !strcmp(sp[2].key, "Gp_Count"));
    check("float range parsed", n == 3 && sp[1].lo == 0.1 && sp[1].hi == 10.0);
    check("key_id is the registry hash", n == 3 && sp[1].key_id == mh_ini_detail::hash_key("net", "gp_mult") &&
                                             sp[2].key_id == mh_ini_detail::hash_key("gameplay", "gp_count"));
    check("key_id matches the wire codec's", n == 3 && sp[0].key_id == mh_net_proto::gameplay_key_id("net", "gp_flag"));
    {
        double lo, hi;
        check("range: ok", parse_range("0.1..10", &lo, &hi) && lo == 0.1 && hi == 10);
        check("range: reversed rejected", !parse_range("10..0.1", &lo, &hi));
        check("range: garbage rejected", !parse_range("abc", &lo, &hi) && !parse_range("", &lo, &hi) && !parse_range(nullptr, &lo, &hi));
    }

    // ---- read_local: absent ini / absent keys -> defaults ----
    char absent[MAX_PATH];
    ini_path(absent, "absent");
    DeleteFileA(absent);
    values dv;
    read_local(sp, n, absent, dv);
    check("absent ini: bool default 1", dv.v[0].i == 1);
    check("absent ini: float default 2.0", dv.v[1].f == 2.0);
    check("absent ini: int default 7", dv.v[2].i == 7);

    // ---- read_local: ini values, clamp ----
    char ini[MAX_PATH];
    ini_path(ini, "main");
    struct {
        const char *body;
        double      want_f;
        int         want_i, want_b;
        const char *what;
    } cases[] = {
        {"[net]\r\ngp_flag=0\r\ngp_mult=3.5 ; the host's rate\r\n[Gameplay]\r\nGP_COUNT=42\r\n", 3.5, 42, 0, "plain values + same-line comment + case-folded section/key"},
        {"[net]\r\ngp_flag=9\r\ngp_mult=0.001\r\n", 0.1, 7, 1, "float below the range clamps to lo; bool 9 -> 1"},
        {"[net]\r\ngp_mult=999\r\n", 10.0, 7, 1, "float above the range clamps to hi"},
        {"[net]\r\ngp_mult=nan\r\n", 2.0, 7, 1, "NaN text -> default"},
        {"[net]\r\ngp_mult=banana\r\n", 2.0, 7, 1, "garbage float -> default"},
        {"[net]\r\ngp_mult=\r\n", 2.0, 7, 1, "empty float -> default"},
        {"[net]\r\ngp_mult=-4\r\n[gameplay]\r\ngp_count=-3\r\n", 0.1, -3, 1, "negative float clamps to lo; negative int kept"},
    };
    for (auto &c : cases) {
        write_file(ini, c.body);
        values v;
        read_local(sp, n, ini, v);
        char msg[160];
        snprintf(msg, sizeof(msg), "read_local: %s", c.what);
        // gp_flag is only in some bodies: compare it only when the body sets it
        const bool flag_ok = strstr(c.body, "gp_flag") ? v.v[0].i == c.want_b : v.v[0].i == 1;
        const bool cnt_ok  = (strstr(c.body, "ameplay]") ? v.v[2].i == c.want_i : v.v[2].i == 7);
        check(msg, v.v[1].f == c.want_f && flag_ok && cnt_ok);
    }

    // ---- local -> wire -> set equality (exact f64 bits) ----
    {
        write_file(ini, "[net]\r\ngp_flag=0\r\ngp_mult=0.30000000000000004\r\n[gameplay]\r\ngp_count=-123456\r\n");
        values loc;
        read_local(sp, n, ini, loc);
        GameplaySet s;
        to_set(sp, n, loc, s);
        check("to_set: 3 entries, bool/int INT and float F64", s.n == 3 && s.e[0].type == mh_net_proto::GAMEPLAY_T_INT &&
                                                                   s.e[1].type == mh_net_proto::GAMEPLAY_T_F64 && s.e[2].type == mh_net_proto::GAMEPLAY_T_INT);
        uint8_t      buf[mh_net_proto::GAMEPLAY_MAX_ENCODED];
        const size_t len = mh_net_proto::gameplay_set_encode(s, buf);
        check("encode ok", len > 0);
        GameplaySet back;
        check("decode ok", mh_net_proto::gameplay_set_decode(buf, len, back) == mh_net_proto::GameplayErr::Ok);
        values rt;
        g_nlines     = 0;
        const int an = from_set(sp, n, back, rt, capture);
        check("round trip: no anomalies, no log lines", an == 0 && g_nlines == 0);
        check("round trip: values equal", rt.v[0].i == loc.v[0].i && rt.v[2].i == loc.v[2].i && f64_bits(rt.v[1].f) == f64_bits(loc.v[1].f));
        char sum[200];
        summary_text(sp, n, rt, sum, sizeof(sum));
        check("summary: 'key=value;...' with %.17g float", !strcmp(sum, "gp_flag=0;gp_mult=0.30000000000000004;Gp_Count=-123456"));
        char tiny[8];
        summary_text(sp, n, rt, tiny, sizeof(tiny));
        check("summary: truncates, stays terminated", strlen(tiny) < sizeof(tiny));
        check("summary: zero specs -> empty", (summary_text(sp, 0, rt, sum, sizeof(sum)), sum[0] == 0));
    }

    // ---- from_set anomalies ----
    {
        GameplaySet s;
        s.n           = 3;
        s.e[0].key_id = sp[1].key_id; // gp_mult only
        s.e[0].type   = mh_net_proto::GAMEPLAY_T_F64;
        s.e[0].f_bits = f64_bits(3.0);
        s.e[1].key_id = 0xDEADBEEFu; // unknown to this build
        s.e[1].type   = mh_net_proto::GAMEPLAY_T_INT;
        s.e[1].i      = 5;
        s.e[2].key_id = sp[2].key_id; // right key, WRONG type
        s.e[2].type   = mh_net_proto::GAMEPLAY_T_F64;
        s.e[2].f_bits = f64_bits(1.0);
        values v;
        g_nlines     = 0;
        const int an = from_set(sp, n, s, v, capture);
        check("from_set: host value used", v.v[1].f == 3.0);
        check("from_set: missing key -> registry default", v.v[0].i == 1);
        check("from_set: type mismatch -> registry default", v.v[2].i == 7);
        check("from_set: 3 anomalies (missing, mismatch, unknown)", an == 3);
        check("from_set: unknown key logged", count_lines("unknown key DEADBEEF") == 1);
        check("from_set: missing key logged", count_lines("[net] gp_flag") == 1);
        // out-of-range / NaN from a peer is clamped like a local read
        GameplaySet w;
        w.n           = 1;
        w.e[0].key_id = sp[1].key_id;
        w.e[0].type   = mh_net_proto::GAMEPLAY_T_F64;
        w.e[0].f_bits = f64_bits(1e9);
        from_set(sp, n, w, v, nullptr);
        check("from_set: an over-range host float is clamped", v.v[1].f == 10.0);
        w.e[0].f_bits = f64_bits(nan(""));
        from_set(sp, n, w, v, nullptr);
        check("from_set: a NaN host float -> default", v.v[1].f == 2.0);
    }

    // ---- digest ----
    {
        const uint32_t d = digest(sp, n);
        check("digest nonzero", d != 0);
        check("digest ignores values", d == digest(sp, n));
        spec rev[MAX_SPECS];
        for (int i = 0; i < n; ++i) rev[i] = sp[n - 1 - i];
        check("digest is order independent", d == digest(rev, n));
        check("digest changes with the key set", d != digest(sp, n - 1));
    }

    // ---- dispatch ----
    {
        typedef hook_row<fake_fx> H;
        const H                   hooks[] = {
            {"net", "gp_mult", [](const value &v, fake_fx &fx) { fx.order[fx.n++] = 2; fx.mult = v.f; }},
            {"NET", "GP_FLAG", [](const value &v, fake_fx &fx) { fx.order[fx.n++] = 1; fx.flag = v.i; }},
            {"net", "orphan_hook", [](const value &, fake_fx &fx) { fx.order[fx.n++] = 99; }},
            // no hook for gameplay.gp_count: skipped
        };
        values v;
        v.v[0].i = 0;
        v.v[1].f = 4.25;
        v.v[2].i = 11;
        fake_fx   fx;
        const int ran = dispatch<fake_fx>(sp, n, v, hooks, 3, fx);
        check("dispatch: ran 2 hooks (the spec without a hook is skipped, the orphan never runs)", ran == 2 && fx.n == 2);
        check("dispatch: registry order (flag before mult), case-insensitive match", fx.order[0] == 1 && fx.order[1] == 2);
        check("dispatch: values delivered", fx.flag == 0 && fx.mult == 4.25);
        fake_fx none;
        check("dispatch: empty hook table is a no-op", dispatch<fake_fx>(sp, n, v, nullptr, 0, none) == 0 && none.n == 0);
    }

    // ---- the LIVE registry (vacuous with no F_HOSTSYNC row) ----
    {
        int         ln;
        const spec *ls = host_specs(&ln);
        check("live: spec count within the wire cap", ln >= 0 && ln <= MAX_SPECS);
        values lv;
        read_local(ls, ln, absent, lv);
        GameplaySet s;
        to_set(ls, ln, lv, s);
        values rt;
        check("live: defaults round trip with no anomalies", from_set(ls, ln, s, rt, nullptr) == 0);
        check("live: digest nonzero", digest(ls, ln) != 0);
        bool distinct = true;
        for (int i = 0; i < ln; ++i)
            for (int j = i + 1; j < ln; ++j)
                if (ls[i].key_id == ls[j].key_id) distinct = false;
        check("live: key_ids distinct", distinct);
        printf("  live registry: %d synced key(s)\n", ln);
    }

    DeleteFileA(ini);
    printf("gameplaytest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
