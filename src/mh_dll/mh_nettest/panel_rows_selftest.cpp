//
// panel_rows_selftest.cpp -- RES7's table layout (gfx/panel_rows.{h,cpp}), run inside `net_selftest.exe bgscaletest`
// next to P6's own cores (seams/video_mode_guards.h).
//
// The blob here is SYNTHETIC: the list groups and the nested groups they use have the stock structure (the records
// llm_ui_icon_table_unpack unpacks, as the EN exe holds them), every other group is a stand-in. The
// game's own unpack is reimplemented below as the reference.
//
//   1. h_built 768: the grown build is byte-identical to the stock unpack (records and group starts).
//   2. every list at every window height up to h_built: the rows the game writes (slot base + 1 + r, r < rows) land
//      on row slots only -- the trailing records and the end marker are never touched -- and walking the group the
//      way llm_ui_icon_group_dispatch does reaches exactly that row's widgets.
//   3. the widget extension: one row height further per row, cb_arg = the row, ids unique past 331.
//   4. hide_rows_beyond: rows past a smaller window's count are hidden, the rows it shows untouched.
//
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "gfx/panel_rows.h"

namespace {

namespace pr = mh::gfx::panel_rows;

int  g_n = 0, g_f = 0;
void ck(const char *what, bool ok) {
    ++g_n;
    if (!ok) {
        ++g_f;
        printf("  FAIL: %s\n", what);
    }
}

struct rec {
    int32_t op;
    uint8_t kind;
};
using group = std::vector<rec>;

std::vector<group> stock_groups() {
    std::vector<group> g(pr::STOCK_GROUPS);
    for (int id = 0; id < pr::STOCK_GROUPS; ++id) g[id] = {{0x400000 + id, 3}, {0x10, 4}, {0, 1}}; // stand-ins
    auto widgets = [](group &x, int base, int n, uint8_t kind) {
        for (int r = 0; r < n; ++r) x.push_back({base + r, kind});
    };
    // 4 available, 5 owned: callback, 10 hidden nested-group slots, 3 scroll widgets, end
    for (int k = 0; k < 2; ++k) {
        group &x = g[4 + k];
        x        = {{0x4163db + k, 3}};
        widgets(x, k ? 34 : 24, 10, 2);
        x.push_back({k ? 0x133 : 0x135, 4});
        x.push_back({k ? 0x134 : 0x136, 4});
        x.push_back({k ? 0x132 : 0x137, 4});
        x.push_back({0, 1});
    }
    // 7 units: callback, 10 nested-group slots + 10 never reached, scroll widgets, panel widgets
    {
        group &x = g[7];
        x        = {{0x416ec6, 3}};
        widgets(x, 0x2c, 10, 2);
        widgets(x, 0x33, 10, 2);
        for (int w : {0x139, 0x13a, 0x138, 0x9c, 0x9b, 0x9a, 0x99, 0x98, 0x97}) x.push_back({w, 4});
        x.push_back({0, 1});
    }
    // 9 / 8 / 11 storage: callback, 2 or 3 panel widgets, 10 hidden widget slots 0x29.., tail
    for (int id : {9, 8, 11}) {
        group &x       = g[id];
        x              = {{0x417c3b, 3}};
        const int lead = id == 11 ? 3 : 2;
        for (int i = 0; i < lead; ++i) x.push_back({0x1d + i, 4});
        widgets(x, 0x29, 10, 2);
        for (int w : {0x13c, 0x13d, 0x13b, 0x9c, 0x9b, 0x9a, 0x99, 0x98, 0x97}) x.push_back({w, 4});
        x.push_back({0, 1});
    }
    // 12 projects
    {
        group &x = g[12];
        x        = {{0x417fb3, 3}};
        widgets(x, 0x3d, 10, 2);
        for (int w : {0x13f, 0x140, 0x13e, 0x9c, 0x9b, 0x9a, 0x99, 0x98, 0x97}) x.push_back({w, 4});
        x.push_back({0, 1});
    }
    // 24..33, 34..43, 44..53 nested row groups
    for (int r = 0; r < 10; ++r) {
        g[24 + r] = {{0x47 + r, 4}, {0x5b + r, 4}, {0, 1}};
        g[34 + r] = {{0x51 + r, 4}, {0x5b + r, 4}, {0x65 + r, 4}, {0x6f + r, 4}, {0x79 + r, 4}, {0x83 + r, 4}, {0x8d + r, 4}, {0, 1}};
        g[44 + r] = {{0x101 + r, 4}, {0x10b + r, 4}, {0x115 + r, 4}, {0, 1}};
    }
    // 54 control group 0: widget pairs
    {
        group &x = g[54];
        x        = {{0x419549, 3}};
        for (int r = 0; r < 10; ++r) {
            x.push_back({0x9d + r, 4});
            x.push_back({0xa7 + r, 4});
        }
        for (int w : {0x129, 0x12a, 0x12b}) x.push_back({w, 4});
        x.push_back({0, 1});
    }
    // 55 selection + selected control group
    {
        group &x = g[55];
        x        = {{0x4197ba, 3}};
        widgets(x, 0xb1, 40, 4);
        widgets(x, 0xd9, 40, 4);
        for (int w : {0x12c, 0x12e, 0x12d, 0x12f, 0x131, 0x130}) x.push_back({w, 4});
        x.push_back({0, 1});
    }
    return g;
}

// The blob, in a stock-like order (not by id).
std::vector<uint8_t> make_blob(const std::vector<group> &g) {
    static const int     order[] = {0, 1, 2, 3, 22, 23, 54, 55, 4, 5, 6, 7, 9, 8, 11, 10, 13, 12, 14,
                                    15, 16, 17, 18, 19, 20, 21, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
                                    36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53};
    std::vector<uint8_t> b((size_t)pr::STOCK_GROUPS * pr::BLOB_SLOT + 1, 0xcc);
    int                  slot = 0;
    for (int id : order) {
        uint8_t *p = b.data() + (size_t)slot++ * pr::BLOB_SLOT;
        p[0]       = (uint8_t)id;
        for (size_t i = 0; i < g[id].size(); ++i) {
            memcpy(p + 1 + i * 5, &g[id][i].op, 4);
            p[1 + i * 5 + 4] = g[id][i].kind;
        }
    }
    b[(size_t)pr::STOCK_GROUPS * pr::BLOB_SLOT] = pr::BLOB_END_ID;
    return b;
}

// llm_ui_icon_table_unpack, reimplemented: the reference for h_built 768.
int stock_unpack(const std::vector<uint8_t> &b, uint8_t *out, int32_t *gs) {
    int n = 0;
    for (size_t off = 0; b[off] != pr::BLOB_END_ID; off += pr::BLOB_SLOT) {
        gs[b[off]]       = n;
        const uint8_t *p = b.data() + off + 1;
        do {
            memcpy(out + (size_t)n * 5, p, 5);
            ++n;
            p += 5;
        } while (p[-1] != 1);
    }
    return n;
}

int32_t op_at(const uint8_t *t, int i) {
    int32_t v;
    memcpy(&v, t + (size_t)i * 5, 4);
    return v;
}
uint8_t kind_at(const uint8_t *t, int i) { return t[(size_t)i * 5 + 4]; }

// The widgets llm_ui_icon_group_dispatch reaches from group `id` (kind 4 directly, kind 5 recursively).
void walk(const uint8_t *t, const int32_t *gs, int id, std::vector<int> &out, int depth = 0) {
    if (depth > 4) return;
    for (int i = gs[id];; ++i) {
        const uint8_t k = kind_at(t, i);
        if (k == 1) return;
        if (k == 4) out.push_back(op_at(t, i));
        if (k == 5) walk(t, gs, op_at(t, i), out, depth + 1);
    }
}

void test_identity(const std::vector<uint8_t> &blob) {
    pr::plan p;
    ck("plan 768", pr::make_plan(768, p));
    std::vector<uint8_t> ref(4000 * 5), got(4000 * 5);
    int32_t              gs_ref[256] = {}, gs_got[256] = {};
    const int            n_ref = stock_unpack(blob, ref.data(), gs_ref);
    const int            n_got = pr::build_icon_table(blob.data(), (int)blob.size(), p, got.data(), 4000, gs_got, 256);
    ck("768: the same record count as the stock unpack", n_got == n_ref && n_ref > 0);
    ck("768: byte-identical records", n_got == n_ref && memcmp(ref.data(), got.data(), (size_t)n_ref * 5) == 0);
    ck("768: identical group starts", memcmp(gs_ref, gs_got, sizeof(int32_t) * pr::STOCK_GROUPS) == 0);
    ck("768: no new groups or widgets", p.n_groups == pr::STOCK_GROUPS && p.n_widgets == pr::STOCK_WIDGETS);
}

void test_grown(const std::vector<group> &g, const std::vector<uint8_t> &blob, int h_built) {
    pr::plan p;
    ck("plan", pr::make_plan(h_built, p));
    const int            count = pr::build_icon_table(blob.data(), (int)blob.size(), p, nullptr, 0, nullptr, 0);
    std::vector<uint8_t> t((size_t)count * 5);
    std::vector<int32_t> gs((size_t)p.n_groups);
    char                 what[200];
    snprintf(what, sizeof what, "%d: build fills exactly the counted records", h_built);
    ck(what, count > 0 && pr::build_icon_table(blob.data(), (int)blob.size(), p, t.data(), count, gs.data(),
                                               p.n_groups) == count);
    snprintf(what, sizeof what, "%d: a short buffer is refused", h_built);
    ck(what, pr::build_icon_table(blob.data(), (int)blob.size(), p, t.data(), count - 1, gs.data(), p.n_groups) < 0);

    // Groups the rules do not touch are verbatim.
    bool same = true;
    for (int id = 0; id < pr::STOCK_GROUPS; ++id) {
        bool ruled = false;
        for (const pr::group_rule &r : pr::RULES) ruled |= r.group == id;
        if (ruled || (id >= 24 && id <= 53)) continue;
        for (size_t i = 0; i < g[id].size(); ++i)
            same &= op_at(t.data(), gs[id] + (int)i) == g[id][i].op && kind_at(t.data(), gs[id] + (int)i) == g[id][i].kind;
    }
    snprintf(what, sizeof what, "%d: the other groups are verbatim", h_built);
    ck(what, same);

    // Every rule, every height up to h_built: the rows the game writes stay on row slots, and each row reaches its
    // widgets. Row kinds per list: 5 for the nested-group lists (and group 54 rebuilt), 4 for the widget slots.
    static const pr::range_id AV[]  = {pr::R_AV_CLICK, pr::R_INFO};
    static const pr::range_id OW[]  = {pr::R_OW_CLICK, pr::R_INFO, pr::R_WK0, pr::R_WK1, pr::R_ST0, pr::R_ST1, pr::R_ST2};
    static const pr::range_id UN[]  = {pr::R_UN_INC, pr::R_UN_DEC, pr::R_UN_PEND};
    static const pr::range_id UA[]  = {pr::R_UA_INFO, pr::R_UA_CLICK};
    bool                      tails = true, reach = true;
    for (int rule = 0; rule < pr::RULE_COUNT; ++rule) {
        const pr::group_rule &R    = pr::RULES[rule];
        const group          &sg   = g[R.group];
        const int             tail = (int)sg.size() - (R.lead + R.stock_slots);
        for (int H = 480; H <= h_built; H += 32) {
            const int            rows = pr::rows_for(H, pr::LIST_GEOM[R.list].top, pr::LIST_GEOM[R.list].rowh);
            std::vector<uint8_t> w    = t;
            for (int sub = 0; sub < (rule == pr::G_SEL ? 2 : 1); ++sub) {
                const int base = gs[R.group] + (R.lead - 1) + (sub ? p.slots[rule] : 0); // the callback's index
                for (int r = 0; r < rows; ++r) {
                    const bool nested                 = rule == pr::G_AVAIL || rule == pr::G_OWNED || rule == pr::G_UNITS || (rule == pr::G_UA && p.ua_nested);
                    w[(size_t)(base + 1 + r) * 5 + 4] = nested ? 5 : 4; // llm_ui_scroll_icon_list_draw's write
                }
            }
            // the tail is intact
            const int tail0 = gs[R.group] + R.lead + (rule == pr::G_SEL ? 2 : 1) * p.slots[rule];
            for (int i = 0; i < tail; ++i) {
                const rec &s = sg[(size_t)(R.lead + R.stock_slots + i)];
                tails &= op_at(w.data(), tail0 + i) == s.op && kind_at(w.data(), tail0 + i) == s.kind;
            }
            // each row's widgets
            for (int r = 0; r < rows && rule != pr::G_SEL && !(rule == pr::G_UA && !p.ua_nested); ++r) {
                std::vector<int> got;
                const int        slot = gs[R.group] + R.lead + r;
                if (kind_at(w.data(), slot) == 5) walk(w.data(), gs.data(), op_at(w.data(), slot), got);
                if (kind_at(w.data(), slot) == 4) got.push_back(op_at(w.data(), slot));
                std::vector<int>    want;
                const pr::range_id *rs = rule == pr::G_AVAIL ? AV : rule == pr::G_OWNED ? OW
                                                                : rule == pr::G_UNITS   ? UN
                                                                : rule == pr::G_UA      ? UA
                                                                                        : nullptr;
                const int           nr = rule == pr::G_AVAIL ? 2 : rule == pr::G_OWNED ? 7
                                                               : rule == pr::G_UNITS   ? 3
                                                               : rule == pr::G_UA      ? 2
                                                                                       : 0;
                for (int k = 0; k < nr; ++k) want.push_back(pr::widget_id(p, rs[k], r));
                if (!rs) want.push_back(pr::widget_id(p, rule == pr::G_PROJ ? pr::R_PROJ : pr::R_STORE, r));
                reach &= got == want;
            }
            for (int r = 0; r < rows && rule == pr::G_SEL; ++r)
                for (int sub = 0; sub < 2; ++sub)
                    reach &= op_at(w.data(), pr::row_slot(p, gs.data(), rule, sub, r)) ==
                             pr::widget_id(p, sub ? pr::R_CG : pr::R_SEL, r);
        }
    }
    snprintf(what, sizeof what, "%d: every height: the rows never reach a trailing record or the end marker", h_built);
    ck(what, tails);
    snprintf(what, sizeof what, "%d: every height: each row reaches exactly its own widgets", h_built);
    ck(what, reach);

    // hide_rows_beyond: at 768 everything past the stock rows is hidden, the rows 768 shows are not touched.
    std::vector<uint8_t> w = t;
    for (int rule = 0; rule < pr::RULE_COUNT; ++rule)
        for (int sub = 0; sub < (rule == pr::G_SEL ? 2 : 1); ++sub)
            for (int r = 0; r < p.slots[rule]; ++r) w[(size_t)pr::row_slot(p, gs.data(), rule, sub, r) * 5 + 4] = 4;
    pr::hide_rows_beyond(w.data(), gs.data(), p, 768);
    bool hid = true;
    for (int rule = 0; rule < pr::RULE_COUNT; ++rule) {
        if (rule == pr::G_UA && !p.ua_nested) continue;
        const int rows = pr::rows_for(768, pr::LIST_GEOM[pr::RULES[rule].list].top, pr::LIST_GEOM[pr::RULES[rule].list].rowh);
        for (int sub = 0; sub < (rule == pr::G_SEL ? 2 : 1); ++sub)
            for (int r = 0; r < p.slots[rule]; ++r)
                hid &= kind_at(w.data(), pr::row_slot(p, gs.data(), rule, sub, r)) == (r < rows ? 4 : 2);
    }
    snprintf(what, sizeof what, "%d: back at 768 the rows past its count are hidden, its own rows untouched", h_built);
    ck(what, hid);
}

void test_widgets(int h_built) {
    pr::plan p;
    pr::make_plan(h_built, p);
    std::vector<pr::hud_widget> w((size_t)p.n_widgets);
    for (int k = 0; k < pr::R_COUNT; ++k) { // llm_ui_hud_widget_range_define's output for each stock range
        const pr::range_def &d = pr::RANGES[k];
        for (int r = 0; r < d.stock_n; ++r) {
            pr::hud_widget &x = w[(size_t)(d.base + r)];
            x.callback        = 0x413000u + (uint32_t)k;
            x.cb_arg          = r;
            x.x               = 2 + k;
            x.y               = 100 + r * pr::LIST_GEOM[d.list].rowh;
            x.right           = x.x + 40;
            x.bottom          = x.y + 30;
            x.text_id         = 0x100 + k;
        }
    }
    std::vector<pr::hud_widget> bad = w;
    bad[(size_t)pr::RANGES[pr::R_STORE].base + 3].y += 1;
    char what[160];
    snprintf(what, sizeof what, "%d: a range that does not step by its row height is refused, nothing written", h_built);
    if (p.ext_n[pr::R_STORE] > 0) // a range is only checked when it grows
        ck(what, !pr::extend_widgets(bad.data(), p.n_widgets, p) && bad[(size_t)p.ext_base[pr::R_STORE]].callback == 0);
    ck("extend", pr::extend_widgets(w.data(), p.n_widgets, p));
    bool             geo = true, uniq = true;
    std::vector<int> seen((size_t)p.n_widgets, 0);
    for (int k = 0; k < pr::R_COUNT; ++k) {
        const pr::range_def &d    = pr::RANGES[k];
        const int            rows = p.rows[d.list] > d.stock_n ? p.rows[d.list] : d.stock_n;
        for (int r = 0; r < rows; ++r) {
            const int id = pr::widget_id(p, k, r);
            if (id < 0 || id >= p.n_widgets) {
                geo = false;
                continue;
            }
            uniq &= ++seen[(size_t)id] == 1;
            const pr::hud_widget &x = w[(size_t)id];
            geo &= x.y == 100 + r * pr::LIST_GEOM[d.list].rowh && x.bottom == x.y + 30 && x.cb_arg == r &&
                   x.x == 2 + k && x.right == x.x + 40 && x.callback == 0x413000u + (uint32_t)k && x.text_id == 0x100 + k;
            if (r >= d.stock_n) geo &= id >= pr::STOCK_WIDGETS;
        }
        geo &= pr::widget_id(p, k, rows) == -1 || rows < p.rows[d.list];
    }
    snprintf(what, sizeof what, "%d: every row's widget one row height further down, cb_arg = the row, same callback", h_built);
    ck(what, geo);
    snprintf(what, sizeof what, "%d: no widget id serves two rows", h_built);
    ck(what, uniq);
}

} // namespace

int panel_rows_checks(int *checks) {
    printf("  [RES7 panel list tables]\n");
    const std::vector<group>   g    = stock_groups();
    const std::vector<uint8_t> blob = make_blob(g);
    test_identity(blob);
    for (int h : {800, 960, 1056, 1152, 1440, 1600, 2160}) {
        test_grown(g, blob, h);
        test_widgets(h);
    }
    pr::plan p;
    ck("the cap: 2160 fits the imm8 selection stride", pr::make_plan(2160, p) && p.slots[pr::G_SEL] <= 127);
    ck("past the cap: clamped to 2160", pr::make_plan(5000, p) && p.h_built == pr::H_FALLBACK);
    ck("1280x960: 13 building rows (the user's report)", pr::make_plan(960, p) && p.rows[pr::L_BLDG] == 13);
    ck("1920x1440: 72 selection rows, 25 control-group-0 rows",
       pr::make_plan(1440, p) && p.rows[pr::L_SEL] == 72 && p.rows[pr::L_UA] == 25);
    std::vector<uint8_t> broken                       = blob;
    broken[(size_t)8 * pr::BLOB_SLOT + 1 + 5 * 3 + 4] = 4; // group 4's 3rd row slot (blob slot 8) not hidden
    ck("a blob without the stock shape is refused", pr::build_icon_table(broken.data(), (int)broken.size(), p, nullptr, 0, nullptr, 0) < 0);
    *checks += g_n;
    return g_f;
}
