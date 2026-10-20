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
#include "addr/mh_patches.gen.h" // registered_patches: the carrier column the move-carrier twin table must agree with
#include "seams/move_carrier.h"  // mods:M1H: the pure carrier rules
#include "seams/walker_frames.h" // the walker facing-frame table fill rule (render only)

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
        int        nr;
        const reg *rr = all_regs(&nr);
        const int  ne = eligible_specs(rr, nr);
        check("live: every synced registry row fits the wire cap (none silently dropped)", ne <= MAX_SPECS && ln == ne);
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

    // ---- mods:M1H lane A: the movement/combat carrier rules (seams/move_carrier.h), driven with FAKE dispatch tables ----
    {
        using namespace mh::seams::mv;
        constexpr uint32_t WALKER = 0x0047c903, MARSHAL = 0x00482451, SQUAD_MERGE = 0x0047ea62, LIBMH_THUNK = 0x10001000;
        uint32_t           retail[255] = {}, libmh[255] = {}, empty[255] = {};
        retail[0x0f] = WALKER;
        retail[0x0a] = MARSHAL;
        retail[0x33] = SQUAD_MERGE;
        libmh[0x0f]  = LIBMH_THUNK; // libmh's register_state_handlers fills the same slots with its own entry thunks
        libmh[0x0a]  = LIBMH_THUNK + 0x10;
        libmh[0x33]  = LIBMH_THUNK + 0x20;
        check("carrier: retail table dispatches the retail walker", table_has(retail, 255, WALKER));
        check("carrier: libmh table does not dispatch the retail walker", !table_has(libmh, 255, WALKER));
        check("carrier: libmh table does not dispatch the retail marshal or squad_merge", !table_has(libmh, 255, MARSHAL) && !table_has(libmh, 255, SQUAD_MERGE));
        check("carrier: an empty (unfilled BSS) table dispatches nothing", !table_has(empty, 255, WALKER));
        check("carrier: table_has refuses a null table and a zero handler", !table_has(nullptr, 255, WALKER) && !table_has(retail, 255, 0));
        check("carrier: the last slot (254) is scanned, slot 255 is not", [&] {
            uint32_t t[255] = {};
            t[254]          = 0x1234;
            return table_has(t, 255, 0x1234) && !table_has(t, 254, 0x1234);
        }());
        check("carrier: filled = walker and marshal slots set", table_filled(retail, 255) && table_filled(libmh, 255) && !table_filled(empty, 255));
        {
            uint32_t half[255] = {};
            half[0x0f]         = WALKER;
            check("carrier: a half-filled table is not filled", !table_filled(half, 255));
        }
        // the chain rule
        const bool all_armed[3] = {true, true, true}, one_unarmed[3] = {true, false, true}, none_armed[3] = {false, false, false};
        check("carrier chain: a retail root reaches the site whatever the rows say", chain_reaches_retail(true, all_armed, 3) && chain_reaches_retail(true, nullptr, 0));
        check("carrier chain: a libmh root with every row armed does NOT reach it", !chain_reaches_retail(false, all_armed, 3));
        check("carrier chain: a libmh root with ONE unarmed row reaches it (the call lands in retail code)", chain_reaches_retail(false, one_unarmed, 3));
        check("carrier chain: a libmh root, nothing armed, reaches it", chain_reaches_retail(false, none_armed, 3));
        check("carrier chain: a site inside a libmh handler's own body (no rows) is not reached", !chain_reaches_retail(false, nullptr, 0));
        // verdicts
        const site_fact ok1[1] = {{1, true}}, shadow1[1] = {{1, false}}, mism1[1] = {{-1, false}}, none1[1] = {{0, false}};
        check("carrier verdict: reached site, no twin = retail splice", key_verdict(ok1, 1, false).v == verdict::splice);
        check("carrier verdict: reached site, twin exists, nothing displaced = still retail splice", key_verdict(ok1, 1, true).v == verdict::splice);
        check("carrier verdict: unreached site, no twin = NOT ACTIVE (no twin)", key_verdict(shadow1, 1, false).v == verdict::no_twin);
        check("carrier verdict: unreached site, twin = libmh twin", key_verdict(shadow1, 1, true).v == verdict::twin);
        check("carrier verdict: mismatched = mismatched", key_verdict(mism1, 1, true).v == verdict::mismatched);
        check("carrier verdict: not installed", key_verdict(none1, 1, false).v == verdict::not_installed);
        const site_fact mixed[3] = {{1, true}, {1, false}, {-2, false}};
        check("carrier verdict: some sites reached, some displaced, a twin = splice + twin", key_verdict(mixed, 3, true).v == verdict::splice_and_twin);
        verdict_info mi = key_verdict(mixed, 3, false);
        check("carrier verdict: ... without a twin it is a partial splice with the counts", mi.v == verdict::splice && mi.reached == 1 && mi.displaced == 2 && mi.sites == 3);
        check("carrier verdict: a claimed (-2) site counts as displaced", key_verdict(&mixed[2], 1, false).v == verdict::no_twin);
        // the twin table and the manifest agree: `carrier: "migrated:<key>"` on some patch row exactly when the key has a twin
        bool twins_ok = true;
        int  twins    = 0;
        for (int k = 0; k < KEY_COUNT; ++k) {
            const key_id id   = static_cast<key_id>(k);
            const char  *name = key_name(id);
            char         want[96];
            wsprintfA(want, "migrated:%s", name);
            bool in_manifest = false;
            // mods:M1H lane C: a row that rides several keys (move_blocked_protocol: wait / sidestep / cooldown) lists them:
            // `migrated:a,b,c`. A key is carried when it is the whole carrier or one entry of that list.
            for (int i = 0; i < mh::addr::registered_patch_count; ++i) {
                const char *car = mh::addr::registered_patches[i].carrier;
                if (!car || strncmp(car, "migrated:", 9) != 0) continue;
                const size_t nlen = strlen(want) - 9;
                for (const char *c = car + 9; *c;) {
                    const char  *e = strchr(c, ',');
                    const size_t l = e ? static_cast<size_t>(e - c) : strlen(c);
                    if (l == nlen && strncmp(c, want + 9, l) == 0) in_manifest = true;
                    c += l;
                    if (*c == ',') ++c;
                }
            }
            if (key_has_twin(id)) ++twins;
            if (in_manifest != key_has_twin(id)) {
                twins_ok = false;
                printf("  carrier table vs manifest: %s has_twin=%d manifest_carrier=%d\n", name, key_has_twin(id) ? 1 : 0, in_manifest ? 1 : 0);
            }
        }
        check("carrier: the twin table agrees with the manifest's migrated: carriers", twins_ok);
        check("carrier: key names are distinct", [] {
            for (int a = 0; a < KEY_COUNT; ++a)
                for (int b = a + 1; b < KEY_COUNT; ++b)
                    if (strcmp(key_name(static_cast<key_id>(a)), key_name(static_cast<key_id>(b))) == 0) return false;
            return true;
        }());
        printf("  carrier: %d key(s), %d with a libmh twin\n", (int)KEY_COUNT, twins);
    }

    // ---- the walker facing-frame table fill (seams/walker_frames.h): render only ----
    {
        namespace wf = mh::seams::walker_frames;
        // The retail llm_strat_walker_frame_blend_index (0x00460a93), transcribed from its decompile branch for branch.
        auto retail_index = [](unsigned g, unsigned ms) -> uint32_t {
            int base = 0;
            if (g < 10) {
                if (g < 4) {
                    if (g == 1) base = 0;
                } else if (g < 5)
                    base = 5;
                else if (g == 7)
                    base = 10;
            } else if (g < 0xb)
                base = 0xf;
            else if (g < 0x10) {
                if (g == 0xd) base = 0x14;
            } else if (g < 0x11)
                base = 0x19;
            else if (0x12 < g) {
                if (g < 0x14)
                    base = 0x1e;
                else if (g == 0x16)
                    base = 0x23;
            }
            if (ms < 0xf) {
                if (4 < ms) {
                    if (9 < ms) return base + 0x19;
                    return base + 0x18;
                }
            } else {
                if (ms < 0x14) return base + 0x1a;
                if (ms < 0x19) return base + 0x1b;
                if (ms < 0x1f) return base + 0x1c;
                if (ms != 0x1f) return 0xdeadbeef;
            }
            return g - 1;
        };
        // 29 rows: the game's table is 0xC98 bytes (25 rows + a tail); the guard rows 0 and 25..28 must stay untouched
        constexpr int ROWS                          = 29;
        uint32_t      retail[ROWS * wf::MICROSTEPS] = {}, t[ROWS * wf::MICROSTEPS] = {};
        for (unsigned g = 1; g < 0x19; g += 3) // the retail init loop
            for (unsigned ms = 0; ms < 32; ++ms) retail[g * 32 + ms] = retail_index(g, ms);
        memcpy(t, retail, sizeof(t));
        check("walker frames: the retail-init guard accepts the retail table", wf::looks_like_retail_init(t));
        uint32_t zero[ROWS * wf::MICROSTEPS] = {};
        check("walker frames: the retail-init guard refuses an unfilled table", !wf::looks_like_retail_init(zero));
        const int n = wf::fill_unfilled(t);
        check("walker frames: 16 facings x 32 microsteps = 512 cells written", n == 512);
        bool filled_same = true, all24 = true, rest_ok = true, stride_ok = true, guard_ok = true;
        int  nfilled = 0;
        for (int f = 1; f <= 24; ++f) {
            for (int ms = 0; ms < 32; ++ms) {
                const uint32_t v = t[f * 32 + ms];
                if (wf::retail_fills(f)) {
                    ++nfilled;
                    if (v != retail[f * 32 + ms]) filled_same = false; // a retail-filled facing is byte-identical
                } else {
                    if (v == 0 && !(f == 1 && wf::is_rest_ms(ms))) all24 = false; // no cell is left 0 (f-1 >= 1 for f >= 2)
                    if (wf::is_rest_ms(ms)) {
                        if (v != static_cast<uint32_t>(f - 1)) rest_ok = false; // its own rest image
                    } else {
                        // a stride frame, from the bank of a filled facing at distance 1 on the 24-cycle
                        const int d1 = (f % 24) + 1, d2 = ((f + 22) % 24) + 1; // f+1 and f-1, wrapped into 1..24
                        const int dn = wf::retail_fills(d1) ? d1 : d2;
                        if (!(wf::retail_fills(d1) != wf::retail_fills(d2)) || v != retail[dn * 32 + ms]) stride_ok = false;
                    }
                }
            }
        }
        for (int i = 0; i < 32; ++i) {
            if (t[i] != 0) guard_ok = false; // row 0
            for (int r = 25; r < ROWS; ++r)
                if (t[r * 32 + i] != 0) guard_ok = false;
        }
        check("walker frames: the 8 retail-filled facings are byte-identical to the retail table", filled_same && nfilled == 8 * 32);
        check("walker frames: every cell of every facing 1..24 x 32 microsteps is filled", all24);
        check("walker frames: an unfilled facing f shows rest frame f-1 at the rest microsteps (0..4, 31)", rest_ok);
        check("walker frames: an unfilled facing's stride microsteps are the nearest filled facing's (unique, distance 1)", stride_ok);
        check("walker frames: row 0 and rows 25.. are untouched", guard_ok);
        // a full 180-degree turn in place (12 notches, rest microstep) shows 12 distinct images, rotating monotonically
        bool distinct = true, mono = true;
        for (int s = 0; s < 12; ++s) {
            const int f = 1 + s; // north -> south
            for (int s2 = s + 1; s2 < 12; ++s2) {
                if (t[f * 32] == t[(1 + s2) * 32]) distinct = false;
            }
            if (s > 0 && t[f * 32] != t[(f - 1) * 32] + 1) mono = false;
        }
        check("walker frames: a 12-notch turn at rest shows 12 distinct rest frames", distinct);
        check("walker frames: ... and they advance by one frame per notch", mono);
        // the retail table shows the bug: 2 of 3 rest cells are the north frame
        int north = 0;
        for (int f = 1; f <= 24; ++f)
            if (retail[f * 32] == 0) ++north;
        check("walker frames: retail baseline -- 17 of 24 facings draw frame 0 at rest (the bug)", north == 17);
        // idempotent
        uint32_t t2[ROWS * wf::MICROSTEPS];
        memcpy(t2, t, sizeof(t2));
        wf::fill_unfilled(t2);
        check("walker frames: filling twice changes nothing", memcmp(t, t2, sizeof(t)) == 0);
        // the donor rule
        check("walker frames: donors are 2->1 3->4 5->4 6->7 ... 23->22 24->1", wf::donor_facing(2) == 1 && wf::donor_facing(3) == 4 && wf::donor_facing(5) == 4 &&
                                                                                    wf::donor_facing(6) == 7 && wf::donor_facing(23) == 22 && wf::donor_facing(24) == 1);
    }

    DeleteFileA(ini);
    printf("gameplaytest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
