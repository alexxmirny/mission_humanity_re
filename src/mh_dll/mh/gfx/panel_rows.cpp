//
// gfx/panel_rows.cpp -- see panel_rows.h.
//
#include "panel_rows.h"

#include <string.h>

namespace mh::gfx::panel_rows {

// top_y = the scroll list's y + its arrow row height; row heights from the descriptors
// (llm_ui_hud_scroll_list_define calls in llm_strat_ui_hud_widgets_init, 0x0041a013).
const list_geom LIST_GEOM[L_COUNT] = {
    {248, 52}, // L_BLDG  available / owned buildings (0x00510115, 0x00510146)
    {300, 46}, // L_UNITS a building's units (0x0050ffef)
    {318, 46}, // L_STORE storage (0x00510020)
    {300, 46}, // L_PROJ  projects (0x00510051)
    {272, 46}, // L_UA    control group 0 (0x00510082)
    {288, 16}, // L_SEL   ungrouped selection / selected control group (0x005100b3, 0x005100e4)
};

const range_def RANGES[R_COUNT] = {
    {0x47, 10, L_BLDG},
    {0x51, 10, L_BLDG},
    {0x5b, 10, L_BLDG},
    {0x65, 10, L_BLDG},
    {0x6f, 10, L_BLDG},
    {0x79, 10, L_BLDG},
    {0x83, 10, L_BLDG},
    {0x8d, 10, L_BLDG},
    {0x101, 10, L_UNITS},
    {0x10b, 10, L_UNITS},
    {0x115, 10, L_UNITS},
    {0x29, 10, L_STORE},
    {0x3d, 10, L_PROJ},
    {0x9d, 10, L_UA},
    {0xa7, 10, L_UA},
    {0xb1, 40, L_SEL},
    {0xd9, 40, L_SEL},
};

const group_rule RULES[RULE_COUNT] = {
    {4, 1, 10, L_BLDG},   // available: slots are nested groups 24..33
    {5, 1, 10, L_BLDG},   // owned: nested groups 34..43
    {7, 1, 20, L_UNITS},  // units: nested groups 44..53, then ten slots no stock window reaches
    {8, 3, 10, L_STORE},  // storage panels: slots are widgets 0x29..0x32
    {9, 3, 10, L_STORE},  //
    {11, 4, 10, L_STORE}, //
    {12, 1, 10, L_PROJ},  // projects: widgets 0x3d..0x46
    {54, 1, 20, L_UA},    // control group 0: widget PAIRS (0x9d+r, 0xa7+r), rebuilt as one nested group per row
    {55, 1, 80, L_SEL},   // selection 0xb1.. then control group 0xd9.., 40 + 40
};

namespace {

// The stock nested group a grown list's row r < 10 uses, and so the template of a new one.
constexpr int AVAIL_SUB0 = 24, OWNED_SUB0 = 34, UNITS_SUB0 = 44;

int sub_template(int rule) {
    return rule == G_AVAIL ? AVAIL_SUB0 : rule == G_OWNED ? OWNED_SUB0
                                      : rule == G_UNITS   ? UNITS_SUB0
                                                          : -1;
}

int max_i(int a, int b) { return a > b ? a : b; }

int range_of_widget(int id, int *row) {
    for (int k = 0; k < R_COUNT; ++k)
        if (id >= RANGES[k].base && id < RANGES[k].base + RANGES[k].stock_n) {
            *row = id - RANGES[k].base;
            return k;
        }
    return -1;
}

struct group_view {
    const uint8_t *rec; // first record
    int            n;   // records including the end marker
};

int32_t rd_i32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Walks the blob: the group of id `want`, or n = 0.
group_view find_group(const uint8_t *blob, int blob_len, int want) {
    for (int off = 0; off + 1 <= blob_len && blob[off] != BLOB_END_ID; off += BLOB_SLOT) {
        if (off + BLOB_SLOT > blob_len) break;
        if (blob[off] != want) continue;
        const uint8_t *r = blob + off + 1;
        int            n = 0;
        while ((n + 1) * ENTRY_BYTES <= BLOB_SLOT - 1) {
            ++n;
            if (r[(n - 1) * ENTRY_BYTES + 4] == K_END) return {r, n};
        }
        return {r, 0};
    }
    return {nullptr, 0};
}

struct emitter {
    uint8_t *out;
    int      cap, n;
    bool     ok = true;
    void     put(int32_t operand, uint8_t kind) {
        if (out) {
            if (n >= cap) {
                ok = false;
                return;
            }
            memcpy(out + (size_t)n * ENTRY_BYTES, &operand, 4);
            out[(size_t)n * ENTRY_BYTES + 4] = kind;
        }
        ++n;
    }
    void put_rec(const uint8_t *r) { put(rd_i32(r), r[4]); }
};

// The nested group for row `row` of a grown rule: the template group's widgets, each moved to that row.
bool emit_sub(emitter &e, const plan &p, const group_view &tmpl, int row) {
    for (int i = 0; i + 1 < tmpl.n; ++i) {
        const uint8_t *r = tmpl.rec + i * ENTRY_BYTES;
        int            r0;
        const int      rng = range_of_widget(rd_i32(r), &r0);
        if (r[4] != K_WIDGET || rng < 0 || r0 != 0) return false;
        const int id = widget_id(p, rng, row);
        if (id < 0) return false;
        e.put(id, K_WIDGET);
    }
    e.put(0, K_END);
    return true;
}

} // namespace

int rows_for(int h, int top, int rowh) {
    if (rowh <= 0 || h <= top) return 0;
    return (h - top) / rowh;
}

bool make_plan(int h_built, plan &p) {
    memset(&p, 0, sizeof(p));
    if (h_built < STOCK_ROWS_H) h_built = STOCK_ROWS_H;
    if (h_built > H_FALLBACK) h_built = H_FALLBACK;
    p.h_built = h_built;
    for (int l = 0; l < L_COUNT; ++l) p.rows[l] = rows_for(h_built, LIST_GEOM[l].top, LIST_GEOM[l].rowh);

    // Widget extensions, numbered from 332 in range order.
    int next_w = STOCK_WIDGETS;
    for (int k = 0; k < R_COUNT; ++k) {
        const int n   = p.rows[RANGES[k].list] - RANGES[k].stock_n;
        p.ext_n[k]    = n > 0 ? n : 0;
        p.ext_base[k] = p.ext_n[k] ? next_w : -1;
        next_w += p.ext_n[k];
    }
    p.n_widgets = next_w;

    // Row slots and new nested groups, numbered from 56 in rule order.
    int next_g = STOCK_GROUPS;
    for (int g = 0; g < RULE_COUNT; ++g) {
        const group_rule &r    = RULES[g];
        const int         rows = p.rows[r.list];
        p.sub_first[g]         = -1;
        switch (g) {
            case G_UA: // past 10 rows: every row a new nested group, the 20 stock pair records replaced
                p.ua_nested = rows > 10;
                p.slots[g]  = p.ua_nested ? rows : r.stock_slots;
                if (p.ua_nested) {
                    p.sub_row0[g] = 0;
                    p.sub_n[g]    = rows;
                }
                break;
            case G_SEL: // per half
                p.slots[g] = max_i(40, rows);
                break;
            default:
                p.slots[g] = max_i(r.stock_slots, rows);
                if (sub_template(g) >= 0 && rows > 10) {
                    p.sub_row0[g] = 10;
                    p.sub_n[g]    = rows - 10;
                }
                break;
        }
        if (p.sub_n[g] > 0) {
            p.sub_first[g] = next_g;
            next_g += p.sub_n[g];
        }
    }
    p.n_groups = next_g;
    // The selected control group's half of group 55 starts at base + slots (the `add edx,0x28` at 0x004199a7 and
    // the clear loop bound at 0x0041c0bb are imm8 operands).
    return p.slots[G_SEL] <= 127;
}

int widget_id(const plan &p, int range, int row) {
    if (range < 0 || range >= R_COUNT || row < 0) return -1;
    const range_def &d = RANGES[range];
    if (row < d.stock_n) return d.base + row;
    const int k = row - d.stock_n;
    return k < p.ext_n[range] ? p.ext_base[range] + k : -1;
}

int build_icon_table(const uint8_t *blob, int blob_len, const plan &p, uint8_t *entries, int entries_cap,
                     int32_t *gstart, int gstart_cap) {
    if (!blob || blob_len < BLOB_SLOT) return -1;
    if (gstart && gstart_cap < p.n_groups) return -1;
    emitter e{entries, entries_cap, 0};

    for (int off = 0; off < blob_len && blob[off] != BLOB_END_ID; off += BLOB_SLOT) {
        if (off + BLOB_SLOT > blob_len) return -1;
        const int id = blob[off];
        if (id >= STOCK_GROUPS) return -1;
        const group_view gv = find_group(blob, blob_len, id);
        if (gv.n == 0) return -1;
        if (gstart) gstart[id] = e.n;

        int rule = -1;
        for (int g = 0; g < RULE_COUNT; ++g)
            if (RULES[g].group == id) rule = g;
        if (rule < 0) { // not a list: verbatim
            for (int i = 0; i < gv.n; ++i) e.put_rec(gv.rec + i * ENTRY_BYTES);
            continue;
        }

        const group_rule &R     = RULES[rule];
        const int         stock = R.stock_slots;
        if (gv.n < R.lead + stock + 1 || gv.rec[4] != K_CALL) return -1;
        auto rec = [&](int i) { return gv.rec + i * ENTRY_BYTES; };
        for (int i = 0; i < R.lead; ++i) e.put_rec(rec(i));

        // Check the stock row slots are what the rule assumes, then emit the grown slots.
        if (rule == G_UA) {
            for (int i = 0; i < stock; ++i) { // pairs: 0x9d+r, 0xa7+r
                const uint8_t *s    = rec(R.lead + i);
                const int      want = (i & 1) ? RANGES[R_UA_CLICK].base + i / 2 : RANGES[R_UA_INFO].base + i / 2;
                if (s[4] != K_WIDGET || rd_i32(s) != want) return -1;
            }
            for (int r = 0; r < p.slots[rule]; ++r) {
                if (p.ua_nested)
                    e.put(p.sub_first[rule] + r, K_HIDDEN);
                else
                    e.put_rec(rec(R.lead + r));
            }
        } else if (rule == G_SEL) {
            for (int half = 0; half < 2; ++half) {
                const int rng = half ? R_CG : R_SEL;
                for (int r = 0; r < 40; ++r) {
                    const uint8_t *s = rec(R.lead + half * 40 + r);
                    if (s[4] != K_WIDGET || rd_i32(s) != RANGES[rng].base + r) return -1;
                }
                for (int r = 0; r < p.slots[rule]; ++r) {
                    if (r < 40)
                        e.put_rec(rec(R.lead + half * 40 + r));
                    else
                        e.put(widget_id(p, rng, r), K_HIDDEN);
                }
            }
        } else {
            const int tmpl = sub_template(rule);
            const int rng  = rule == G_PROJ ? R_PROJ : R_STORE;
            for (int r = 0; r < 10; ++r) {
                const uint8_t *s    = rec(R.lead + r);
                const int      want = tmpl >= 0 ? tmpl + r : RANGES[rng].base + r;
                if (s[4] != K_HIDDEN || rd_i32(s) != want) return -1;
            }
            for (int r = 0; r < p.slots[rule]; ++r) {
                const bool grown = r >= 10 && r < p.rows[R.list];
                if (!grown && r < stock)
                    e.put_rec(rec(R.lead + r));
                else if (tmpl >= 0)
                    e.put(p.sub_first[rule] + (r - p.sub_row0[rule]), K_HIDDEN);
                else
                    e.put(widget_id(p, rng, r), K_HIDDEN);
            }
        }
        // The trailing records (scroll widgets, panel buttons) and the end marker, unchanged.
        for (int i = R.lead + stock; i < gv.n; ++i) e.put_rec(rec(i));
    }

    // The new nested groups, in plan order.
    for (int g = 0; g < RULE_COUNT; ++g) {
        if (p.sub_n[g] <= 0) continue;
        group_view tmpl;
        if (g == G_UA) {
            static uint8_t ua_tmpl[3 * ENTRY_BYTES];
            const int32_t  a = RANGES[R_UA_INFO].base, b = RANGES[R_UA_CLICK].base, z = 0;
            memcpy(ua_tmpl, &a, 4);
            ua_tmpl[4] = K_WIDGET;
            memcpy(ua_tmpl + 5, &b, 4);
            ua_tmpl[9] = K_WIDGET;
            memcpy(ua_tmpl + 10, &z, 4);
            ua_tmpl[14] = K_END;
            tmpl        = {ua_tmpl, 3};
        } else {
            tmpl = find_group(blob, blob_len, sub_template(g));
            if (tmpl.n < 2) return -1;
        }
        for (int k = 0; k < p.sub_n[g]; ++k) {
            if (gstart) gstart[p.sub_first[g] + k] = e.n;
            if (!emit_sub(e, p, tmpl, p.sub_row0[g] + k)) return -1;
        }
    }
    return e.ok ? e.n : -1;
}

int row_slot(const plan &p, const int32_t *gstart, int rule, int sub, int r) {
    const int base = gstart[RULES[rule].group] + RULES[rule].lead;
    return base + (rule == G_SEL && sub ? p.slots[rule] : 0) + r;
}

bool extend_widgets(hud_widget *w, int cap, const plan &p) {
    if (!w || cap < p.n_widgets) return false;
    for (int k = 0; k < R_COUNT; ++k) { // check every range before writing any
        if (!p.ext_n[k]) continue;
        const range_def &d    = RANGES[k];
        const int        step = w[d.base + 1].y - w[d.base].y;
        if (step != LIST_GEOM[d.list].rowh) return false;
        for (int r = 0; r < d.stock_n; ++r)
            if (w[d.base + r].cb_arg != r || w[d.base + r].y != w[d.base].y + r * step) return false;
    }
    for (int k = 0; k < R_COUNT; ++k) {
        const range_def &d    = RANGES[k];
        const hud_widget last = w[d.base + d.stock_n - 1];
        const int        step = LIST_GEOM[d.list].rowh;
        for (int i = 0; i < p.ext_n[k]; ++i) {
            hud_widget nw = last;
            const int  dy = (i + 1) * step;
            nw.y += dy;
            nw.bottom += dy;
            nw.cb_arg += i + 1;
            w[p.ext_base[k] + i] = nw;
        }
    }
    return true;
}

void hide_rows_beyond(uint8_t *entries, const int32_t *gstart, const plan &p, int h) {
    for (int g = 0; g < RULE_COUNT; ++g) {
        if (g == G_UA && !p.ua_nested) continue; // stock pair layout: slot r is not row r
        const list_geom &lg   = LIST_GEOM[RULES[g].list];
        const int        from = rows_for(h, lg.top, lg.rowh);
        for (int sub = 0; sub < (g == G_SEL ? 2 : 1); ++sub)
            for (int r = from; r < p.slots[g]; ++r)
                entries[(size_t)row_slot(p, gstart, g, sub, r) * ENTRY_BYTES + 4] = K_HIDDEN;
    }
}

} // namespace mh::gfx::panel_rows
