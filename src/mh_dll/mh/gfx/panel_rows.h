//
// gfx/panel_rows.h -- the side-panel list tables sized for the tallest window (RES7).
//
// Pure layout code; no Windows, no game memory. The seam that arms it is seams/ui_panel_rows.cpp.
//
// THE STOCK TABLES. Every list in the strategic side panel (available / owned buildings, a building's units,
// storage, projects, control group 0, the ungrouped selection and the selected control group) shows
// (WindowHeight - top) / row_height rows, and each visible row is a slot in one flat icon table
// (_G_LLM_UI_ICON_ENTRIES, 0x0050e817: 562 records of {int operand; byte kind}, kinds 1 end, 2 hidden,
// 3 callback, 4 HUD widget, 5 nested group). llm_ui_icon_table_unpack (0x00419b98) packs it once at
// boot from a blob of 56 groups (0x005102e8, 0x1f5-byte slots, id byte + records, terminator id 0x38), back to
// back, and records each group's first index (int[56] at 0x0050bcf7). A list group is [kind-3 callback, leading
// records, row slots, trailing scroll widgets, end]; llm_ui_scroll_icon_list_draw writes row r's kind into
// slot base + 1 + r. The tables hold the rows of the 768-px mode (10 per list, 40 for the selection lists); past
// that a row overwrites the scroll widgets and the end marker, which is why P6 (seams/video_mode_guards.cpp) caps
// the row count at the 768 one.
//
// THE EXTENDED TABLES. build_icon_table() repacks the same blob with every list group's row slots grown to the
// rows of the tallest window the session can reach (h_built), inserted before the group's trailing records, so
// every record keeps its offset from the group's callback entry (the callbacks address their records relative to
// the index the dispatcher hands them). A new row needs its own HUD widgets: each per-row widget range (ten
// widgets of fixed geometry, y stepping by the row height, cb_arg = row) is extrapolated past its tenth row into
// new widget ids >= 332 (extend_widgets), and a list whose row slot is a nested group (kind 5: available, owned,
// units) gets one new group per extra row (ids >= 56) holding that row's widgets. Control group 0's list
// (group 54) is the one stock list whose slots hold widgets in PAIRS (row r's two widgets at slots 2r, 2r+1,
// while the draw toggles slot r): it cannot grow by inserting slots, so it is rebuilt as one nested group per
// row, which needs its descriptor's row kind changed from 4 to 5 (the seam does that).
//
// With h_built <= 768 every list keeps its stock slot count and the table is byte-identical to the stock unpack.
//
#pragma once
#include <stdint.h>

namespace mh::gfx::panel_rows {

constexpr int STOCK_ROWS_H  = 768;  // the tallest stock mode: what the stock tables hold rows for
constexpr int H_FALLBACK    = 2160; // the tallest window the tables are ever sized for
constexpr int STOCK_WIDGETS = 332;  // llm_ui_hud_widget[332] at 0x0050be63
constexpr int STOCK_GROUPS  = 56;   // ids 0..55
constexpr int BLOB_SLOT     = 0x1f5;
constexpr int BLOB_END_ID   = 0x38;
constexpr int ENTRY_BYTES   = 5;

enum kind : uint8_t { K_END    = 1,
                      K_HIDDEN = 2,
                      K_CALL   = 3,
                      K_WIDGET = 4,
                      K_GROUP  = 5 };

// The lists, by geometry (stock values from llm_strat_ui_hud_widgets_init; the seam checks the descriptors).
enum list_id { L_BLDG,
               L_UNITS,
               L_STORE,
               L_PROJ,
               L_UA,
               L_SEL,
               L_COUNT };
struct list_geom {
    int top, rowh;
};
extern const list_geom LIST_GEOM[L_COUNT];

// The per-row widget ranges that grow: stock ids base .. base+stock_n-1, one per row.
enum range_id {
    R_AV_CLICK, // 0x47 available: start placement
    R_OW_CLICK, // 0x51 owned: select
    R_INFO,     // 0x5b available / owned: info screen (RMB)
    R_WK0,      // 0x65 owned: worker +
    R_WK1,      // 0x6f owned: worker -
    R_ST0,      // 0x79 owned: status icon 0
    R_ST1,      // 0x83 owned: status icon 1
    R_ST2,      // 0x8d owned: status icon 2
    R_UN_INC,   // 0x101 units: queue +
    R_UN_DEC,   // 0x10b units: queue -
    R_UN_PEND,  // 0x115 units: info screen
    R_STORE,    // 0x29 storage row
    R_PROJ,     // 0x3d project row
    R_UA_INFO,  // 0x9d control group 0: info screen
    R_UA_CLICK, // 0xa7 control group 0: select
    R_SEL,      // 0xb1 ungrouped selection row
    R_CG,       // 0xd9 selected control group row
    R_COUNT
};
struct range_def {
    int base, stock_n, list;
};
extern const range_def RANGES[R_COUNT];

// The list groups that grow, in the order their new nested groups are numbered.
enum rule_id { G_AVAIL,
               G_OWNED,
               G_UNITS,
               G_STORE8,
               G_STORE9,
               G_STORE11,
               G_PROJ,
               G_UA,
               G_SEL,
               RULE_COUNT };
struct group_rule {
    int group;       // icon group id
    int lead;        // records before the first row slot (the callback included)
    int stock_slots; // row-slot records in the stock group (group 54: its 20 widget records; group 55: 40 + 40)
    int list;        // list_id: the geometry its row count follows
};
extern const group_rule RULES[RULE_COUNT];

// (h - top) / rowh, 0 below the top.
int rows_for(int h, int top, int rowh);

struct plan {
    int  h_built;               // the window height the tables hold rows for
    int  rows[L_COUNT];         // rows of each list at h_built
    int  slots[RULE_COUNT];     // row slots of each grown group (per sub-list for G_SEL)
    int  sub_first[RULE_COUNT]; // first new nested-group id of a rule (-1: none)
    int  sub_n[RULE_COUNT];     // how many
    int  sub_row0[RULE_COUNT];  // the row the first new nested group serves
    int  ext_base[R_COUNT];     // first new widget id of a range (-1: none)
    int  ext_n[R_COUNT];
    int  n_widgets; // STOCK_WIDGETS + every extension
    int  n_groups;  // STOCK_GROUPS + every new nested group
    bool ua_nested; // group 54 rebuilt as one nested group per row (its descriptor's row kind must be 5)
};

// Fills `p` for a table holding the rows of an h_built-tall window (clamped to [STOCK_ROWS_H, H_FALLBACK]).
// Returns false when a count would not fit the code that indexes it (the selection-list stride is an imm8).
bool make_plan(int h_built, plan &p);

// The widget a range uses for row r: the stock id below stock_n, else the extension. -1 past the plan.
int widget_id(const plan &p, int range, int row);

// Repacks the stock blob for the plan. `entries` (ENTRY_BYTES per record) and `gstart` may be null to count only.
// Returns the record count, or -1 when the blob does not have the stock shape the rules expect (the seam then
// arms nothing): every grown group is checked record by record against what the rules assume.
int build_icon_table(const uint8_t *blob, int blob_len, const plan &p, uint8_t *entries, int entries_cap,
                     int32_t *gstart, int gstart_cap);

// Index of row r's slot in a grown group (sub = 1: the selected control group's half of group 55).
int row_slot(const plan &p, const int32_t *gstart, int rule, int sub, int r);

// The HUD widget record (llm_ui_hud_widget, 28 bytes).
#pragma pack(push, 1)
struct hud_widget {
    uint32_t callback;
    int32_t  cb_arg, x, y, right, bottom, text_id;
};
#pragma pack(pop)
static_assert(sizeof(hud_widget) == 28, "llm_ui_hud_widget is 28 bytes");

// Defines every extension widget from its range's stock rows: the tenth (or fortieth) row's widget, moved down by
// whole row steps, cb_arg counting on. Returns false (nothing written) when a stock range does not step by its
// list's row height or its cb_arg does not count from 0 -- the geometry the extrapolation assumes.
bool extend_widgets(hud_widget *w, int cap, const plan &p);

// Hides (kind 2) every row slot at or past the rows a window of height h shows: a slot a taller window made
// visible stays dispatched otherwise (its widgets sit below the new window, but its handlers still run).
void hide_rows_beyond(uint8_t *entries, const int32_t *gstart, const plan &p, int h);

} // namespace mh::gfx::panel_rows
