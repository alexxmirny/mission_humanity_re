// Storage rally/Auto presentation and original-body adapters. Policy memory lives only in
// storage_policy.cpp. Original CALL sites are deliberately narrower than shared docking helpers.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include "seams/ui_storage_controls.h"
#include "seams/storage_policy.h"
#include "seams/storage_bulk_rally.h"
#include "gfx/panel_rows.h"
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "en_guard.h"
#include "ui/player_strings.h"
#include "state/region_runtime.h"
#include "seams/order_tasks_hosted.h"
#include "seams/order_tasks_native.h"

namespace mh::ui::storage_controls {
namespace {
namespace policy = mh::storage_policy;
namespace hosted = mh::storage_policy::hosted;
namespace pr     = mh::gfx::panel_rows;
using unit       = mh::game::mh_map_object_unit;
using order      = mh::game::mh_llm_strat_order;
using cfg_unit   = mh::game::mh_cfg_final_struct_Unit;
// These are native presentation cells, not new registry regions or spare fields.
constexpr uintptr_t SELECTED_BUILDING = 0x00e5813c;
constexpr uintptr_t LMB_RELEASE       = 0x00519d3c;
constexpr uintptr_t ICONS             = 0x0050e2b3;
constexpr uintptr_t STATUS_LINE_TEXT  = 0x005d00a4;
constexpr int       STATUS_LINE_UNITS = 120; // native wchar_t[120], not localization STR_MAX
constexpr uintptr_t HOVER_ACTIVE      = 0x00519d30;
constexpr uintptr_t CURSOR_OVERRIDE   = 0x00519d50;
constexpr int       ACTION_IDS[]      = {0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22};
constexpr int       ICON_IDS[]        = {0x45, 0x46, 0x47, 0x48, 0x4f, 0x50, 0x53, 0x54,
                                         0x59, 0x5a, 0x5b, 0x5c};
constexpr int       N_ICONS           = sizeof(ICON_IDS) / sizeof(ICON_IDS[0]);
struct bitmap {
    uint16_t w, h, pixels[54 * 18];
};
bitmap          g_compact[N_ICONS];
bitmap          g_auto[2];
pr::hud_widget *g_widgets = nullptr;
int             g_auto_id = -1;
uint32_t        g_action_callbacks[6]{};
uint32_t        g_group_callbacks[3]{};
bool            g_armed = false;
// This is synchronous call-pair scratch, not pending simulation authority. Only the repair
// body's PAY call writes it; the success-only SMOKE call consumes it before the next iteration.
thread_local double g_repair_before = 0;

template <class T>
T       &cell(uintptr_t address) { return *reinterpret_cast<T *>(address); }
uint32_t local_player() { return cell<uint16_t>(mh::addr::PlayerSide); }
uint32_t selected_building() { return cell<uint16_t>(SELECTED_BUILDING); }
unit    *live_unit(uint32_t p, uint32_t u) {
    const auto &g = mh::caps::g(mh::caps::UNITS);
    return p < g.players && u > 0 && u < g.limit ? mh::caps::rec<unit>(mh::caps::UNITS, p, u) : nullptr;
}
bool issue(uint32_t p, uint32_t b, policy::command_kind kind, uint32_t x, uint32_t y) {
    if (p != local_player() || !hosted::eligible_building(p, b)) return false;
    policy::command cmd{hosted::building_identity(p, b), kind, x, y};
    if (cmd.building.incarnation == 0 || b > 0xffffu) return false;
    int32_t args[13];
    policy::encode_command(cmd, args);
    // Preserve the issuer's hidden channel, including all sentinel words. Initialize EVERY word
    // in our new order, then restore the UI caller's scratch even on enqueue refusal.
    int32_t saved[13];
    auto   *scratch = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_SCRATCH_ARGS);
    if (!scratch || mh::state::live_size(mh::state::RID_STRAT_ORDER_SCRATCH_ARGS) < sizeof saved) return false;
    std::memcpy(saved, scratch, sizeof saved);
    std::memcpy(scratch, args, sizeof args);
    const uint16_t code   = kind == policy::command_kind::set_auto ? policy::AUTO_COMMAND : policy::RALLY_COMMAND;
    const int32_t  result = mh::call::llm_strat_order_dispatch(static_cast<uint16_t>(b), p, code, code);
    std::memcpy(scratch, saved, sizeof saved);
    return result != 0;
}

// Final-width native material. Six-pixel endcaps are copied ONE-TO-ONE. The entire interior
// ramp is evaluated at its FINAL width, independently for each native scanline: no cut-and-join
// gradient, stretched bevel, scaled glyph or screenshot fragment. Native action glyphs are the
// green-chroma foreground in the raw 80x18 P_*_[FN].GFX assets (loaded as RGB565).
uint16_t ramp(uint16_t a, uint16_t b, int n, int denominator) {
    const int r = (((a >> 11) & 31) * (denominator - n) + ((b >> 11) & 31) * n + denominator / 2) / denominator;
    const int g = (((a >> 5) & 63) * (denominator - n) + ((b >> 5) & 63) * n + denominator / 2) / denominator;
    const int z = ((a & 31) * (denominator - n) + (b & 31) * n + denominator / 2) / denominator;
    return static_cast<uint16_t>((r << 11) | (g << 5) | z);
}
void material(bitmap &out, const uint16_t *source, int width) {
    out.w = static_cast<uint16_t>(width);
    out.h = 18;
    for (int y = 0; y < 18; ++y) {
        const uint16_t *s = source + 2 + y * 80;
        for (int x = 0; x < width; ++x) {
            out.pixels[y * width + x] = x < 6 ? s[x] : x >= width - 6 ? s[80 - width + x]
                                                                      : ramp(s[6], s[73], x - 6, width - 13);
        }
    }
}
void glyph(bitmap &out, const uint16_t *source) {
    // Central 36px includes the widest cargo icon. Only coloured glyph pixels are transferred;
    // retaining its surrounding background would reintroduce the rejected internal patch edge.
    const int dx = (static_cast<int>(out.w) - 80) / 2;
    for (int y = 0; y < 18; ++y) {
        for (int x = 22; x < 58; ++x) {
            const uint16_t c = source[2 + y * 80 + x];
            const int      r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            if (g > r * 2 + 5 && g > b * 2 + 3) out.pixels[y * out.w + x + dx] = c;
        }
    }
}
bool build_materials() {
    auto **icons = reinterpret_cast<uint16_t **>(ICONS);
    for (int i = 0; i < N_ICONS; ++i) {
        const uint16_t *s = icons[ICON_IDS[i]];
        if (!s || s[0] != 80 || s[1] != 18) return false;
    }
    for (int i = 0; i < N_ICONS; ++i) {
        material(g_compact[i], icons[ICON_IDS[i]], 53);
        glyph(g_compact[i], icons[ICON_IDS[i]]);
    }
    material(g_auto[0], icons[0x4f], 54); // unlatched native frame
    material(g_auto[1], icons[0x50], 54); // depressed native frame
    return true;
}
struct icon_scope {
    uint16_t *saved[N_ICONS];
    icon_scope() {
        auto **icons = reinterpret_cast<uint16_t **>(ICONS);
        for (int i = 0; i < N_ICONS; ++i) {
            saved[i]           = icons[ICON_IDS[i]];
            icons[ICON_IDS[i]] = &g_compact[i].w;
        }
    }
    ~icon_scope() {
        auto **icons = reinterpret_cast<uint16_t **>(ICONS);
        for (int i = 0; i < N_ICONS; ++i) icons[ICON_IDS[i]] = saved[i];
    }
};
// Execute a native two-register HUD callback with its original all-register preservation.
void native_callback(uint32_t target, int a, int b) {
    __asm {
        pushad
        mov eax, a
        mov edx, b
        call target
        popad
    }
}
void __cdecl action_callback(int widget, int arg) {
    if (!g_widgets) return;
    for (int i = 0; i < 6; ++i)
        if (widget == ACTION_IDS[i]) {
            icon_scope scope;
            native_callback(g_action_callbacks[i], widget, arg);
            return;
        }
}
void draw_auto(int widget);
void __cdecl group_callback(int entry, int group) {
    icon_scope scope;
    native_callback(g_group_callbacks[group - 8], entry, 0);
    draw_auto(g_auto_id);
}
void __cdecl auto_callback(int widget) {
    const uint32_t p = local_player(), b = selected_building();
    if (!hosted::eligible_building(p, b)) return;
    const auto rally = hosted::read_rally(p, b);
    // Native TEXT hints use this shared scratch, consumed/cleared by the bottom-strip tick.
    // Publish only on current-cursor hover; never clear another widget's pending hint.
    if (mh::call::llm_ui_hud_widget_hit_test(widget)) {
        lstrcpynW(reinterpret_cast<wchar_t *>(STATUS_LINE_TEXT), mh::ui::tr(mh::ui::Str::STORAGE_AUTO_DISPATCH), STATUS_LINE_UNITS);
        cell<int>(HOVER_ACTIVE)    = 1;
        cell<int>(CURSOR_OVERRIDE) = 0xc;
    }
    const bool pressed_here = mh::call::llm_ui_icon_rect_hit_test(widget) != 0;
    if (cell<int>(LMB_RELEASE) != 0 && pressed_here && mh::call::llm_ui_icon_rect_hit_test_release_pos(widget)) {
        set_auto(p, b, !rally.automatic); // explicit desired value goes on the wire, never a toggle
        mh::call::llm_snd_play(0x44, 100);
    }
    draw_auto(widget);
}
void draw_auto(int widget) {
    if (!g_widgets || widget < 0) return;
    const uint32_t p = local_player(), b = selected_building();
    if (!hosted::eligible_building(p, b)) return;
    const auto  rally = hosted::read_rally(p, b);
    const auto &w     = g_widgets[widget];
    mh::call::llm_ui_icon_draw_vclipped(w.x, w.y, &g_auto[rally.automatic ? 1 : 0]);
    static uint16_t text[] = {'A', 'U', 'T', 'O', 0};
    mh::call::llm_gfx_font_select(1); // PFMENU0, not the fixed-width HUD counter font
    const int      width = mh::call::llm_gfx_font_measure_text_width(text, 4);
    const uint16_t color = mh::call::llm_gfx_pack_rgb16(rally.automatic ? 80 : 65, 20, rally.automatic ? 255 : 130);
    mh::call::llm_ui_text_draw_rgb16(w.x + (54 - width) / 2, w.y + 4, reinterpret_cast<char *>(text), color);
    if (rally.automatic) {
        // Native latched underline uses the same green as the lettering, inside the lower bevel.
        auto     *surface = cell<uint16_t *>(mh::addr::_G_LLM_GFX_DRAW_SURFACE);
        const int stride  = cell<int>(mh::addr::_G_LLM_GFX_PANEL_ROW_SKIP) / 2;
        if (surface && stride >= w.x + 54)
            for (int x = 9; x < 45; ++x) surface[(w.y + 14) * stride + w.x + x] = color;
    }
}
// Native widget callback: EAX=widget, EDX=cb_arg. All native callbacks preserve their callers'
// registers; the cdecl helpers do not, so the bridge explicitly preserves every one.
__declspec(naked) void action_thunk() {
    __asm {
    pushad
    push edx
    push eax
    call action_callback
    add esp, 8
    popad
    ret
    }
}
__declspec(naked) void auto_thunk() {
    __asm {
    pushad
    push eax
    call auto_callback
    add esp, 4
    popad
    ret
    }
}
__declspec(naked) void group8_thunk() {
    __asm {
    pushad
    push 8
    push eax
    call group_callback
    add esp, 8
    popad
    ret
    }
}
__declspec(naked) void group9_thunk() {
    __asm {
    pushad
    push 9
    push eax
    call group_callback
    add esp, 8
    popad
    ret
    }
}
__declspec(naked) void group10_thunk() {
    __asm {
    pushad
    push 10
    push eax
    call group_callback
    add esp, 8
    popad
    ret
    }
}

int __cdecl production_spawn(uint32_t type, uint32_t player, uint32_t probe) {
    const int id = mh::call::llm_strat_unit_spawn_docked(static_cast<uint16_t>(type), static_cast<uint16_t>(player), probe);
    if (id > 0) hosted::production_inserted(player, static_cast<uint32_t>(id));
    return id;
}
int __cdecl production_add(uint32_t type, uint32_t player, uint32_t probe) {
    const int id = mh::call::llm_strat_unit_add_docked(type, static_cast<uint16_t>(player), probe);
    if (id > 0) hosted::production_inserted(player, static_cast<uint32_t>(id));
    return id;
}
__declspec(naked) void production_spawn_thunk() {
    __asm {
    push ecx
    push edx
    push ebx
    push edx
    push eax
    call production_spawn
    add esp, 12
    pop edx
    pop ecx
    ret
    }
}
__declspec(naked) void production_add_thunk() {
    __asm {
    push ecx
    push edx
    push ebx
    push edx
    push eax
    call production_add
    add esp, 12
    pop edx
    pop ecx
    ret
    }
}
int __cdecl repair_pay(uint32_t player, uint32_t id) {
    const unit *u   = live_unit(player, id);
    g_repair_before = u ? u->energy : 0.0;
    return mh::call::llm_strat_unit_try_pay_action_cost(player, static_cast<int32_t>(id));
}
void __cdecl repair_complete(uint32_t player, uint32_t id) {
    mh::call::llm_strat_unit_update_damage_smoke(player, static_cast<int32_t>(id));
    hosted::repair_paid(player, id, g_repair_before);
}
__declspec(naked) void repair_pay_thunk() {
    __asm {
    push ecx
    push ebx
    push edx
    push edx
    push eax
    call repair_pay
    add esp, 8
    pop edx
    pop ebx
    pop ecx
    ret
    }
}
__declspec(naked) void repair_complete_thunk() {
    __asm {
    pushad
    push edx
    push eax
    call repair_complete
    add esp, 8
    popad
    ret
    }
}
void __cdecl prepare_queue_commit(int queue_index, int kept, uint32_t *saved_ebx) {
    const int n = static_cast<int>(mh::caps::orders::live_count(mh::caps::orders::QUEUE));
    if (queue_index < 0 || queue_index >= n) return;
    auto      *queue         = reinterpret_cast<order *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    auto      &q             = queue[queue_index];
    const bool retained_copy = kept > 0 && kept <= queue_index && kept <= n &&
                               std::memcmp(&queue[kept - 1], &q, sizeof q) == 0;
    q.param0   = before_unit_commit(q.owner_and_kind & 15, q.unit_index, q.order_code, q.param0, q.args);
    *saved_ebx = q.param0;
    // Native boarding orders may have been compacted AND applied. Snapshot that already-made
    // retained copy too, or its next application would overwrite the committed rally with the
    // old native scatter destination. No new record is created.
    if (retained_copy && q.order_code == 0x20) {
        queue[kept - 1].param0   = q.param0;
        queue[kept - 1].args[0]  = q.args[0];
        queue[kept - 1].args[1]  = q.args[1];
        queue[kept - 1].args[12] = q.args[12];
    }
}
uintptr_t              g_native_set_order = 0x0048696f;
__declspec(naked) void manual_commit_thunk() {
    __asm {
    pushfd
    pushad
    lea eax, [esp + 10h]
    push eax
    push dword ptr [ebp - 20h]
    push dword ptr [ebp - 24h]
    call prepare_queue_commit
    add esp, 12
    popad
    popfd
    jmp dword ptr [g_native_set_order]
    }
}
void __cdecl finish_rally_departure(uint32_t *saved, int ground) {
    mh::order_tasks::native::preparation_finished();
    auto *u = *mh::state::ptr<unit *>(mh::state::RID_STRAT_CUR_UNIT);
    if (!u) return;
    const auto &c = mh::state::ptr<const cfg_unit>(mh::state::RID_UNIT)[u->unit_proto_id];
    if (!policy::is_rally_departure(u->order, c.move_op_arg, c.default_op_code)) return;
    saved[7] = c.move_op_arg; // saved EAX: next state, including fresh path planning
    if (ground)
        saved[5] = c.move_op_code; // saved EDX: original set_state_order's order argument
    else
        u->order = c.move_op_code;
}
uintptr_t              g_native_takeoff_state    = 0x004866c9;
uintptr_t              g_native_exit_state_order = 0x00486657;
__declspec(naked) void takeoff_rally_thunk() {
    __asm {
    pushfd
    pushad
    mov eax, esp
    push 0
    push eax
    call finish_rally_departure
    add esp, 8
    popad
    popfd
    jmp dword ptr [g_native_takeoff_state]
    }
}
__declspec(naked) void ground_rally_thunk() {
    __asm {
    pushfd
    pushad
    mov eax, esp
    push 1
    push eax
    call finish_rally_departure
    add esp, 8
    popad
    popfd
    jmp dword ptr [g_native_exit_state_order]
    }
}
void __cdecl birth_unit_callback(uint32_t p, uint32_t id) { hosted::birth_unit(p, id); }
void __cdecl birth_building_callback(uint32_t p, uint32_t id) { hosted::birth_building(p, id); }
void __cdecl retire_unit_callback(uint32_t p, uint32_t id) {
    mh::order_tasks::hosted::retire_unit(p, id, false);
    hosted::retire_unit(p, id);
}
void __cdecl retire_building_callback(uint32_t p, uint32_t id) {
    mh::order_tasks::hosted::retire_building(p, id, false);
    hosted::retire_building(p, id);
}
void __cdecl cancel_callback(uint32_t p, uint32_t id) { hosted::cancel_pending(p, id); }
uintptr_t              g_admission_continue = 0x0046696e;
uintptr_t              g_order_skip         = 0x0046997f;
__declspec(naked) void explicit_order_thunk() {
    __asm {
    jnc dead_unit
    pushfd
    pushad
    push dword ptr [ebp - 30h]
    push dword ptr [ebp - 34h]
    call cancel_callback
    add esp, 8
    popad
    popfd
    jmp dword ptr [g_admission_continue]
dead_unit:
    jmp dword ptr [g_order_skip]
    }
}
int __cdecl consume_admin(int slot, uint32_t retained_prefix) {
    const uint32_t n = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    if (slot < 0 || static_cast<uint32_t>(slot) >= n) return 0;
    const auto &q = reinterpret_cast<const order *>(mh::caps::orders::base(mh::caps::orders::QUEUE))[slot];
    if (mh::order_tasks::hosted::consume_command(q.owner_and_kind, q.order_code,
                                                 static_cast<uint16_t>(q.param0), q.unit_index, q.args, retained_prefix)) return 1;
    if ((q.owner_and_kind & 0xf0) != 0 ||
        (q.order_code != policy::RALLY_COMMAND && q.order_code != policy::AUTO_COMMAND)) return 0;
    mh_storage_policy_request request{};
    mh_storage_policy_result  result{};
    request.operation = static_cast<uint32_t>(policy::operation::consume_setting);
    request.player    = q.owner_and_kind;
    request.slot      = q.unit_index;
    request.args[0]   = q.order_code;
    request.args[1]   = static_cast<uint16_t>(q.param0);
    std::memcpy(request.args + 2, q.args, sizeof q.args);
    hosted::invoke(&request, &result);
    return 1; // malformed recognized settings are consumed, never native-fallback commands
}
uintptr_t              g_admin_continue = 0x004696a2;
__declspec(naked) void admin_thunk() {
    __asm {
    pushfd
    pushad
    push dword ptr [ebp - 20h]
    push dword ptr [ebp - 24h]
    call consume_admin
    add esp, 8
    test eax, eax
    jnz consumed
    popad
    popfd
    sub eax, 34h
    mov dword ptr [ebp - 6ch], eax
    jmp dword ptr [g_admin_continue]
consumed:
    popad
    popfd
    jmp dword ptr [g_order_skip]
    }
}
void __cdecl retry_policy() {
    hosted::retry();
    mh::order_tasks::hosted::tick();
}
// Both optional-dispatch arms join at the scalar player-loop initialization. Preserve the
// COMPLETE caller state: 16-byte-aligned FXSAVE includes x87 registers/control/status and
// MXCSR/XMM state. The policy runs with an empty x87 stack but the caller's precision/rounding.
// Nothing is enqueued; accepted launches commit state20 and destination synchronously.
__declspec(naked) void retry_thunk() {
    __asm {
    pushfd
    pushad
    mov ebx, esp
    sub esp, 527
    and esp, -16
    fxsave [esp]
    fninit
    fldcw word ptr [esp]
    call retry_policy
    fxrstor [esp]
    mov esp, ebx
    popad
    popfd
    mov word ptr ds:[00e58144h], 0
    ret
    }
}
void __cdecl fresh_world_policy() {
    // Read real tile extents AFTER map_ReadMap_pre published them and cleared the old roster.
    // Same-sized rematches/system resets still need reset_world: setup intentionally preserves
    // a same-shape owner. No generic session clear is called (match speed stays frozen).
    const uint32_t width  = cell<uint32_t>(mh::addr::width);
    const uint32_t height = cell<uint32_t>(mh::addr::height);
    if (!hosted::setup(width, height) || !hosted::reset_world()) std::abort();
    const uint32_t system = cell<uint32_t>(mh::addr::CurrentSystem);
    const uint32_t planet = cell<uint8_t>(mh::addr::G_PLANET_INDEX);
    if (!mh::order_tasks::hosted::fresh_world(system, planet, width, height)) std::abort();
}
void __cdecl begin_world_replacement_policy() {
    if (mh::order_tasks::hosted::begin_load(mh::order_tasks::host_protocol::begin_load_reason::preserve_input_ring) == UINT32_MAX)
        std::abort();
}
__declspec(naked) void begin_world_replacement_thunk() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 527
        and esp, -16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        call begin_world_replacement_policy
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    }
}
uintptr_t g_native_map_read_pre = 0x004552d6;
// Native void __watcall(EAX=planet). Run the original exactly ONCE before observing dimensions;
// preserve its outgoing state rather than the incoming state, including every FP/SSE register.
__declspec(naked) void fresh_map_thunk() {
    __asm {
        call begin_world_replacement_thunk
    call dword ptr [g_native_map_read_pre]
    pushfd
    pushad
    mov ebx, esp
    sub esp, 527
    and esp, -16
    fxsave [esp]
    fninit
    fldcw word ptr [esp]
    call fresh_world_policy
    fxrstor [esp]
    mov esp, ebx
    popad
    popfd
    ret
    }
}
void __cdecl damaged_unit(uint32_t p, uint32_t id) {
    hosted::cancel_pending(p, id);
    mh::call::llm_strat_unit_update_damage_smoke(p, static_cast<int32_t>(id));
}
__declspec(naked) void damage_thunk() {
    __asm {
    pushad
    push edx
    push eax
    call damaged_unit
    add esp, 8
    popad
    ret
    }
}
void __cdecl proto_changed(uint32_t p, uint32_t id) {
    hosted::cancel_pending(p, id);
    mh::call::llm_strat_unit_soldiers_start_walk_anim(p, static_cast<int32_t>(id));
}
__declspec(naked) void proto_change_thunk() {
    __asm {
    pushad
    push edx
    push eax
    call proto_changed
    add esp, 8
    popad
    ret
    }
}
// Tail-splices preserve the original epilogue exactly, including CreateBuilding's RET 12.
// Every argument comes from its committed frame local, not a clobbered volatile register.
__declspec(naked) void birth_init_tail() {
    __asm {
    pushad
    push dword ptr [ebp - 28h]
    movzx eax, word ptr [ebp - 10h]
    push eax
    call birth_unit_callback
    add esp, 8
    popad
    lea esp, [ebp - 0ch]
    pop edi
    pop esi
    pop ecx
    pop ebp
    ret
    }
}
__declspec(naked) void birth_add_tail() {
    __asm {
    pushad
    push dword ptr [ebp - 1ch]
    movzx eax, word ptr [ebp - 10h]
    push eax
    call birth_unit_callback
    add esp, 8
    popad
    lea esp, [ebp - 0ch]
    pop edi
    pop esi
    pop ecx
    pop ebp
    ret
    }
}
__declspec(naked) void birth_building_tail() { __asm {
    pushad
    push dword ptr [ebp - 20h]
    movzx eax, word ptr [ebp + 0ch]
    push eax
    call birth_building_callback
    add esp, 8
    popad
    lea esp, [ebp - 8]
    pop edi
    pop esi
    pop ebp
    ret 0ch
} }
__declspec(naked) void retire_unit_tail() {
    __asm {
    pushad
    push dword ptr [ebp - 18h]
    movzx eax, word ptr [ebp - 14h]
    push eax
    call retire_unit_callback
    add esp, 8
    popad
    lea esp, [ebp - 10h]
    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    ret
    }
}
__declspec(naked) void retire_building_tail() {
    __asm {
    pushad
    push dword ptr [ebp - 18h]
    movzx eax, word ptr [ebp - 14h]
    push eax
    call retire_building_callback
    add esp, 8
    popad
    lea esp, [ebp - 10h]
    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    ret
    }
}
__declspec(naked) void dock_tail() {
    __asm {
    pushad
    push dword ptr [ebp - 18h]
    movzx eax, word ptr [ebp - 10h]
    push eax
    call cancel_callback
    add esp, 8
    popad
    lea esp, [ebp - 0ch]
    pop edi
    pop esi
    pop ecx
    pop ebp
    ret
    }
}
__declspec(naked) void undock_tail() {
    __asm {
    pushad
    push dword ptr [ebp - 1ch]
    movzx eax, word ptr [ebp - 10h]
    push eax
    call cancel_callback
    add esp, 8
    popad
    lea esp, [ebp - 0ch]
    pop edi
    pop esi
    pop ecx
    pop ebp
    ret
    }
}

struct patch {
    uintptr_t site;
    uint8_t   old[12], replacement[12];
    int       n;
    bool      active;
};
patch g_patches[24];
int   g_patch_count = 0;
bool  g_patch_error = false;
bool  add_jump(uintptr_t site, const uint8_t *expected, int n, const void *target) {
    if (g_patch_count >= static_cast<int>(sizeof g_patches / sizeof *g_patches) ||
        n < 5 || n > static_cast<int>(sizeof g_patches[0].old)) {
        g_patch_error = true;
        return false;
    }
    patch &p = g_patches[g_patch_count++];
    p.site   = site;
    p.n      = n;
    p.active = !mh::hook::promoted_owner_of(site);
    std::memcpy(p.old, expected, n);
    std::memset(p.replacement, 0x90, n);
    p.replacement[0]  = 0xe9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - site - 5);
    std::memcpy(p.replacement + 1, &rel, 4);
    return true;
}
void add_call(uintptr_t site, uintptr_t original, const void *target) {
    uint8_t       bytes[5] = {0xe8};
    const int32_t rel      = static_cast<int32_t>(original - site - 5);
    std::memcpy(bytes + 1, &rel, 4);
    if (add_jump(site, bytes, 5, target))
        g_patches[g_patch_count - 1].replacement[0] = 0xe8;
}
void write_entry(uint8_t *p, int32_t operand, uint8_t kind) {
    std::memcpy(p, &operand, 4);
    p[4] = kind;
}
} // namespace

bool     set_auto(uint32_t p, uint32_t b, bool desired) { return issue(p, b, policy::command_kind::set_auto, desired ? 1u : 0u, 0); }
bool     set_rally(uint32_t p, uint32_t b, uint32_t x, uint32_t y) { return issue(p, b, policy::command_kind::set_rally, x, y); }
bool     clear_rally(uint32_t p, uint32_t b) { return issue(p, b, policy::command_kind::clear_rally, 0, 0); }
uint16_t before_unit_commit(uint32_t p, uint32_t id, uint16_t code, uint16_t param0, int32_t *args) {
    unit *u = live_unit(p, id);
    if (!u) return param0;
    // Snapshot before the native state setter changes parked state to exit-begin. Never rewrite
    // later deferred applications; the queue itself carries the snapshotted destination.
    if (code == 0x20 && u->state == 0x1f && args) {
        uint32_t x = 0, y = 0;
        if (hosted::manual_rally(p, id, x, y)) {
            args[0]       = static_cast<int32_t>(x);
            args[1]       = static_cast<int32_t>(y);
            const auto &c = mh::state::ptr<const cfg_unit>(mh::state::RID_UNIT)[u->unit_proto_id];
            param0        = c.move_op_arg;
            args[12]      = 0;
        }
    }
    hosted::cancel_pending(p, id);
    return param0;
}

bool bind_panel_tables(uint8_t *entries, int count, int capacity, int32_t *starts, int groups,
                       pr::hud_widget *widgets, int widget_count, int auto_widget) {
    if (!entries || !starts || !widgets || groups < 56 || capacity < count + EXTRA_ENTRIES ||
        auto_widget < pr::STOCK_WIDGETS || auto_widget >= widget_count || !build_materials()) return false;
    const int expected_first[] = {0x1f, 0x1d, 0x21};
    for (int g = 8; g <= 10; ++g) {
        const int first = starts[g];
        if (first < 0 || first + 2 >= count || entries[first * 5 + 4] != pr::K_CALL) return false;
        int action;
        std::memcpy(&action, entries + (first + 1) * 5, 4);
        if (entries[(first + 1) * 5 + 4] != pr::K_WIDGET || action != expected_first[g - 8]) return false;
        bool end = false;
        for (int i = first; i < count; ++i)
            if (entries[i * 5 + 4] == pr::K_END) {
                end = true;
                break;
            }
        if (!end) return false;
    }
    // The native tables are freshly rebuilt on every call; remember originals from THAT rebuild.
    // Widget originals are installed once, since subsequent rebuilds retain the relocated records.
    g_widgets = widgets;
    g_auto_id = auto_widget;
    for (int i = 0; i < 6; ++i) {
        auto &w = widgets[ACTION_IDS[i]];
        if (w.callback != reinterpret_cast<uint32_t>(&action_thunk)) g_action_callbacks[i] = w.callback;
        const bool second = (i & 1) != 0;
        w.x               = second ? 53 : 0;
        w.right           = second ? 106 : 53;
        w.y               = 276;
        w.bottom          = 294;
        w.callback        = reinterpret_cast<uint32_t>(&action_thunk);
    }
    widgets[auto_widget]   = {reinterpret_cast<uint32_t>(&auto_thunk), 0, 106, 276, 160, 294, 0};
    const void *wrappers[] = {reinterpret_cast<void *>(&group8_thunk), reinterpret_cast<void *>(&group9_thunk), reinterpret_cast<void *>(&group10_thunk)};
    for (int g = 8; g <= 10; ++g) {
        const int first = starts[g];
        std::memcpy(&g_group_callbacks[g - 8], entries + first * 5, 4);
        write_entry(entries + first * 5, static_cast<int32_t>(reinterpret_cast<uintptr_t>(wrappers[g - 8])), pr::K_CALL);
        int end = first;
        while (entries[end * 5 + 4] != pr::K_END) ++end;
        std::memmove(entries + (end + 1) * 5, entries + end * 5, static_cast<size_t>(count - end) * 5);
        write_entry(entries + end * 5, auto_widget, pr::K_WIDGET);
        for (int i = 0; i < groups; ++i)
            if (starts[i] > end) ++starts[i];
        ++count;
    }
    return true;
}
bool install_original_hooks() {
    if (g_armed) return true;
    if (!mh::en_build_ok()) return false;
    g_patch_count = 0;
    g_patch_error = false;
    add_call(0x0047a304, 0x0046434c, reinterpret_cast<void *>(&production_spawn_thunk));
    add_call(0x0047a53a, 0x0046443a, reinterpret_cast<void *>(&production_add_thunk));
    add_call(0x0047b28e, 0x00493254, reinterpret_cast<void *>(&repair_pay_thunk));
    add_call(0x0047b358, 0x004870d8, reinterpret_cast<void *>(&repair_complete_thunk));
    add_call(0x00466ddc, 0x0048696f, reinterpret_cast<void *>(&manual_commit_thunk));
    add_call(0x0046cb4a, 0x0046d0cd, reinterpret_cast<void *>(&mh::storage_policy::bulk_rally::native_force_disembark_thunk));
    add_call(0x004814f4, 0x004866c9, reinterpret_cast<void *>(&takeoff_rally_thunk));
    add_call(0x0047f690, 0x00486657, reinterpret_cast<void *>(&ground_rally_thunk));
    add_call(0x0047e5f0, 0x004870d8, reinterpret_cast<void *>(&damage_thunk));
    add_call(0x00489586, 0x004897c7, reinterpret_cast<void *>(&proto_change_thunk));
    // Hook the HP-gate branch BEFORE the cap-rewritten order-code operand. No overlap with
    // caps/orders' ON/OFF manifest, and no baked stock queue pointer in a replayed instruction.
    const uint8_t admission[] = {0x0f, 0x83, 0x11, 0x30, 0, 0};
    add_jump(0x00466968, admission, sizeof admission, reinterpret_cast<void *>(&explicit_order_thunk));
    // SUB/MOV only: the preceding queue-code load remains owned by the cap toggle.
    const uint8_t admin[] = {0x83, 0xe8, 0x34, 0x89, 0x45, 0x94};
    add_jump(0x0046969c, admin, sizeof admin, reinterpret_cast<void *>(&admin_thunk));
    const uint8_t retry[] = {0x66, 0xc7, 0x05, 0x44, 0x81, 0xe5, 0, 0, 0};
    if (!add_jump(0x0043f552, retry, sizeof retry, reinterpret_cast<void *>(&retry_thunk))) return false;
    g_patches[g_patch_count - 1].replacement[0] = 0xe8;
    // Fresh generation only, before profiles/landing/internal first tick. Known-planet load,
    // tactical return, snapshot/resync and replay imports are deliberately NOT reset here.
    add_call(0x0045421c, 0x004552d6, reinterpret_cast<void *>(&fresh_map_thunk));
    add_call(0x0045443d, 0x004552d6, reinterpret_cast<void *>(&fresh_map_thunk));
    add_call(0x0044cfb6, 0x004552d6, reinterpret_cast<void *>(&fresh_map_thunk));
    const uint8_t unit_tail[] = {0x8d, 0x65, 0xf4, 0x5f, 0x5e, 0x59, 0x5d, 0xc3};
    add_jump(0x00461e96, unit_tail, sizeof unit_tail, reinterpret_cast<void *>(&birth_init_tail));
    add_jump(0x0046227c, unit_tail, sizeof unit_tail, reinterpret_cast<void *>(&birth_add_tail));
    add_jump(0x00463858, unit_tail, sizeof unit_tail, reinterpret_cast<void *>(&dock_tail));
    add_jump(0x00489f4c, unit_tail, sizeof unit_tail, reinterpret_cast<void *>(&undock_tail));
    const uint8_t building_tail[] = {0x8d, 0x65, 0xf8, 0x5f, 0x5e, 0x5d, 0xc2, 0x0c, 0};
    add_jump(0x00462d80, building_tail, sizeof building_tail, reinterpret_cast<void *>(&birth_building_tail));
    const uint8_t free_tail[] = {0x8d, 0x65, 0xf0, 0x5f, 0x5e, 0x59, 0x5b, 0x5d, 0xc3};
    add_jump(0x00487b9c, free_tail, sizeof free_tail, reinterpret_cast<void *>(&retire_unit_tail));
    add_jump(0x0047baf0, free_tail, sizeof free_tail, reinterpret_cast<void *>(&retire_building_tail));
    if (g_patch_error) return false;
    for (int i = 0; i < g_patch_count; ++i) {
        const patch &p = g_patches[i];
        if (p.active && std::memcmp(reinterpret_cast<void *>(p.site), p.old, p.n) != 0) return false;
    }
    int applied = 0;
    for (; applied < g_patch_count; ++applied) {
        const patch &p = g_patches[applied];
        if (p.active && !mh::hook::patch_bytes_guarded(p.site, p.old, p.replacement, p.n)) break;
    }
    if (applied != g_patch_count) {
        while (applied-- > 0) {
            const patch &p = g_patches[applied];
            if (p.active) mh::hook::patch_bytes_guarded(p.site, p.replacement, p.old, p.n);
        }
        return false;
    }
    g_armed = true;
    return true;
}
} // namespace mh::ui::storage_controls
