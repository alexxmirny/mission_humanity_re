//
// mh_gameplay.h -- PT-SET1: the HOST-SYNCED gameplay settings, as DATA.
//
// Every ini_keys.def row flagged F_HOSTSYNC is a sim-affecting setting whose HOST value wins: the host
// reads its own ini, publishes the values in the FLAG_START frame (mh_net_proto/gameplay_set.h), and each
// client adopts them. This header is the one place those rows are READ from the ini (gen_ini_registry.py
// forbids a direct mh_ini_get_* of an F_HOSTSYNC key anywhere else) and converted to / from the wire set.
//
//   * the registry rows -> a spec table (sec/key/type/default/range/key_id) built from ini_keys.def
//   * read_local   the gated ini read, clamped (T_FLOAT "lo..hi"); an absent/garbled key = the default
//   * to_set       values -> mh_net_proto::GameplaySet     from_set   set -> values (unknown key skipped
//                  and logged, missing key = default and logged)
//   * digest       gameplay_set_digest of the key set     summary_text   "key=value;..." (%.17g floats)
//   * dispatch     run the apply hooks in registry order (the hook table is mh/seams/gameplay_hooks.cpp)
//
// Header-only and free of libmh: the spec/values/hook machinery takes the row table (and the hook's
// fixes type) as parameters so net_selftest can drive it with a fake table. With NO F_HOSTSYNC rows (the
// state until PT-SET1 S5) every function here is vacuously correct: zero specs, an empty set.
//
#ifndef MH_GAMEPLAY_H
#define MH_GAMEPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh_ini_gate.h"
#include "mh_net_proto/gameplay_set.h"

namespace mh_gameplay {

enum { MAX_SPECS = mh_net_proto::GAMEPLAY_MAX_ENTRIES };

// One registry row, as ini_keys.def spells it (only the columns this layer needs).
struct reg {
    const char *sec;
    const char *key;
    int         type; // mh_ini_detail::T_*
    const char *dflt;
    const char *options; // T_FLOAT: "lo..hi"
    int         flags;   // mh_ini_detail::F_*
};

// One synced setting.
struct spec {
    const char *sec;
    const char *key;
    int         type; // T_BOOL / T_INT / T_FLOAT
    const char *dflt;
    double      lo, hi; // T_FLOAT clamp range
    uint32_t    key_id; // mh_ini_detail::hash_key("sec.key")
};

// One value. T_BOOL / T_INT live in `i`; T_FLOAT in `f`.
struct value {
    int    i = 0;
    double f = 0.0;
};

struct values {
    value v[MAX_SPECS];
};

inline bool is_float(const spec &s) { return s.type == mh_ini_detail::T_FLOAT; }

// ---- the registry -> spec table ---------------------------------------------------------------------
inline const reg *all_regs(int *count) {
    using namespace mh_ini_detail;
#define MH_INI_KEY(sec, key, type, dflt, cls, label, subtab, options, flags) {sec, key, (int)(type), dflt, options, (int)(flags)},
    static const reg rows[] = {
#include "../../mh/config/ini_keys.def"
    };
#undef MH_INI_KEY
    *count = (int)(sizeof(rows) / sizeof(rows[0]));
    return rows;
}

// Parse "lo..hi" (both finite, lo <= hi). False = malformed.
inline bool parse_range(const char *s, double *lo, double *hi) {
    if (s == nullptr) return false;
    char *e1 = nullptr;
    *lo      = strtod(s, &e1);
    if (e1 == s || e1[0] != '.' || e1[1] != '.') return false;
    char *e2 = nullptr;
    *hi      = strtod(e1 + 2, &e2);
    if (e2 == e1 + 2) return false;
    return *lo == *lo && *hi == *hi && *lo <= *hi;
}

// Filter `regs` to the F_HOSTSYNC bool/int/float rows, in registry order. Returns the count (<= cap).
inline int build_specs(const reg *regs, int nregs, spec *out, int cap) {
    using namespace mh_ini_detail;
    int n = 0;
    for (int i = 0; i < nregs && n < cap; ++i) {
        const reg &g = regs[i];
        if (!(g.flags & F_HOSTSYNC)) continue;
        if (g.type != T_BOOL && g.type != T_INT && g.type != T_FLOAT) continue;
        spec &s = out[n];
        s.sec   = g.sec;
        s.key   = g.key;
        s.type  = g.type;
        s.dflt  = g.dflt;
        s.lo = s.hi = 0.0;
        if (g.type == T_FLOAT && !parse_range(g.options, &s.lo, &s.hi)) {
            s.lo = -1e300; // a float row without a valid range is a registry bug (the generator rejects it)
            s.hi = 1e300;
        }
        s.key_id = hash_key(g.sec, g.key);
        ++n;
    }
    return n;
}

// The live registry's synced specs (built once).
inline const spec *host_specs(int *count) {
    static spec s_specs[MAX_SPECS];
    static int  s_n = -1;
    if (s_n < 0) {
        int        nr;
        const reg *r = all_regs(&nr);
        s_n          = build_specs(r, nr, s_specs, MAX_SPECS);
    }
    *count = s_n;
    return s_specs;
}

// ---- values -----------------------------------------------------------------------------------------
inline double clamp_float(const spec &s, double x) {
    if (!(x == x)) return strtod(s.dflt ? s.dflt : "0", nullptr);
    return x < s.lo ? s.lo : (x > s.hi ? s.hi : x);
}

inline value default_value(const spec &s) {
    value v;
    if (is_float(s)) v.f = clamp_float(s, strtod(s.dflt ? s.dflt : "0", nullptr));
    else if (s.type == mh_ini_detail::T_BOOL) v.i = atoi(s.dflt ? s.dflt : "0") != 0;
    else v.i = atoi(s.dflt ? s.dflt : "0");
    return v;
}

// The gated ini read of every spec. An absent key / unparsable float = the registry default; a float is
// clamped to its range. `ini` empty/null = every default (the gate treats it as "no ini").
inline void read_local(const spec *sp, int n, const char *ini, values &out) {
    for (int k = 0; k < n; ++k) {
        const spec &r = sp[k];
        value       d = default_value(r);
        if (is_float(r)) {
            char buf[64];
            buf[0] = 0;
            mh_ini_get_str(r.sec, r.key, "", buf, sizeof(buf), ini);
            for (char *p = buf; *p; ++p)
                if (*p == ';') {
                    *p = 0;
                    break;
                }
            char  *e   = nullptr;
            double x   = strtod(buf, &e);
            out.v[k].f = (e == buf) ? d.f : clamp_float(r, x);
        } else if (r.type == mh_ini_detail::T_BOOL) {
            out.v[k].i = (int)mh_ini_get_int(r.sec, r.key, d.i, ini) != 0;
        } else {
            out.v[k].i = (int)mh_ini_get_int(r.sec, r.key, d.i, ini);
        }
    }
}

inline uint64_t f64_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}
inline double f64_from(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}

inline void to_set(const spec *sp, int n, const values &vals, mh_net_proto::GameplaySet &out) {
    out   = mh_net_proto::GameplaySet();
    out.n = n;
    for (int k = 0; k < n; ++k) {
        mh_net_proto::GameplayEntry &e = out.e[k];
        e.key_id                       = sp[k].key_id;
        if (is_float(sp[k])) {
            e.type   = mh_net_proto::GAMEPLAY_T_F64;
            e.f_bits = f64_bits(vals.v[k].f);
        } else {
            e.type = mh_net_proto::GAMEPLAY_T_INT;
            e.i    = vals.v[k].i;
        }
    }
}

// Set -> values. `log` (may be null) gets one line per anomaly: an unknown key (skipped), a missing key
// (default used), a type mismatch (default used). Returns the number of anomalies.
inline int from_set(const spec *sp, int n, const mh_net_proto::GameplaySet &s, values &out, void (*log)(const char *)) {
    int  anomalies = 0;
    char line[160];
    bool used[mh_net_proto::GAMEPLAY_MAX_ENTRIES] = {false};
    for (int k = 0; k < n; ++k) {
        const spec &r     = sp[k];
        int         found = -1;
        for (int j = 0; j < s.n; ++j)
            if (s.e[j].key_id == r.key_id) {
                found = j;
                break;
            }
        const bool want_f = is_float(r);
        if (found >= 0) used[found] = true;
        if (found < 0 || (s.e[found].type == mh_net_proto::GAMEPLAY_T_F64) != want_f) {
            out.v[k] = default_value(r);
            ++anomalies;
            if (log) {
                snprintf(line, sizeof(line), "; gameplay: %s [%s] %s -> registry default\n",
                         found < 0 ? "key missing from the host's set:" : "key type mismatch:", r.sec, r.key);
                log(line);
            }
            continue;
        }
        if (want_f) out.v[k].f = clamp_float(r, f64_from(s.e[found].f_bits));
        else if (r.type == mh_ini_detail::T_BOOL) out.v[k].i = s.e[found].i != 0;
        else out.v[k].i = s.e[found].i;
    }
    for (int j = 0; j < s.n; ++j) {
        if (used[j]) continue;
        ++anomalies;
        if (log) {
            snprintf(line, sizeof(line), "; gameplay: unknown key %08X in the host's set -- skipped\n", (unsigned)s.e[j].key_id);
            log(line);
        }
    }
    return anomalies;
}

inline uint32_t digest(const spec *sp, int n) {
    values                    zero;
    mh_net_proto::GameplaySet s;
    to_set(sp, n, zero, s);
    return mh_net_proto::gameplay_set_digest(s);
}

// "key=value;key=value" -- ints as %d, floats as %.17g (round-trip exact). Empty with no specs.
inline int summary_text(const spec *sp, int n, const values &vals, char *out, int cap) {
    int len = 0;
    if (cap > 0) out[0] = 0;
    for (int k = 0; k < n && len < cap - 1; ++k) {
        int w;
        if (is_float(sp[k])) w = snprintf(out + len, (size_t)(cap - len), "%s%s=%.17g", k ? ";" : "", sp[k].key, vals.v[k].f);
        else w = snprintf(out + len, (size_t)(cap - len), "%s%s=%d", k ? ";" : "", sp[k].key, vals.v[k].i);
        if (w < 0 || w >= cap - len) {
            len = cap - 1;
            break;
        }
        len += w;
    }
    return len;
}

// ---- hooks ------------------------------------------------------------------------------------------
// One apply hook per synced row, matched by (sec, key). `Fx` is the caller's fixes struct
// (mh::lockstep::reimpl_fixes in the DLL, anything in a selftest).
template <class Fx>
struct hook_row {
    const char *sec;
    const char *key;
    void (*fn)(const value &, Fx &);
};

// Run each spec's hook in spec (registry) order. A spec with no hook is skipped. Returns hooks run.
template <class Fx>
inline int dispatch(const spec *sp, int n, const values &vals, const hook_row<Fx> *hooks, int nhooks, Fx &fx) {
    int ran = 0;
    for (int k = 0; k < n; ++k)
        for (int h = 0; h < nhooks; ++h)
            if (hooks[h].sec && lstrcmpiA(hooks[h].sec, sp[k].sec) == 0 && lstrcmpiA(hooks[h].key, sp[k].key) == 0) {
                hooks[h].fn(vals.v[k], fx);
                ++ran;
                break;
            }
    return ran;
}

} // namespace mh_gameplay

#endif
