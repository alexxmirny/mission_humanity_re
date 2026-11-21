#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "strategic_input_native.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "en_guard.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_export.gen.h"
#include "ui/player_strings.h"
#include <cstdint>
#include <cstring>
namespace mh::seams::strategic_input_native {
namespace {
uintptr_t                                 native_pop        = 0x004d1030u;
uintptr_t                                 native_modal_pump = 0x004b528fu;
static mh::exp::sig_llm_input_wndproc_tap input_tap_impl    = &wndproc_record;
MH_EXPORT_THUNK_e_void_EAX_EDX_EBX_ECX__callee(input_tap_thunk, input_tap_impl)
    __declspec(naked) void widget_modal_pump_return() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp+28]
        call filter_modal_pump
        add esp,4
        mov dword ptr [esp+28],eax
        popad
        popfd
        ret
    }
}
__declspec(naked) void widget_modal_pump_thunk() {
    __asm {
        push offset widget_modal_pump_return
        jmp dword ptr [native_modal_pump]
    }
}
bool armed = false;
bool strategic() { return main_context_active(); }
int  add_held() { return strategic() ? (held(mh::strategic_bindings::shift_modifier) ? 1 : 0) : ((*reinterpret_cast<volatile const uint8_t *>(0x00e69d3au) | *reinterpret_cast<volatile const uint8_t *>(0x00e69d46u)) & 1); }
int  toggle_held() { return strategic() ? (held(mh::strategic_bindings::toggle_selection) ? 1 : 0) : (*reinterpret_cast<volatile const uint8_t *>(0x00e69d2du) & 3); }
int  selection_button_held() { return (*reinterpret_cast<volatile const uint8_t *>(0x00e5899bu) & (strategic() ? button_mask(mh::strategic_bindings::select_button) : 1u)) ? 1 : 0; }
bool minimap_select_left() { return !strategic() || button_mask(mh::strategic_bindings::select_button) == 1; }
int  minimap_hit(int widget) {
    return minimap_select_left()
                ? mh::call::llm_ui_icon_rect_hit_test(widget)
                : mh::call::llm_ui_icon_rect_hit_test_rmb(widget);
}
int minimap_press() { return *reinterpret_cast<volatile const int32_t *>(minimap_select_left() ? 0x00519d40u : 0x00519d4cu); }
int minimap_release() { return *reinterpret_cast<volatile const int32_t *>(minimap_select_left() ? 0x00519d3cu : 0x00519d48u); }
// Raw absolute TEST predicates are replaced ONLY inside native strategic selection consumers.
// CALL+NOP2 is the exact original 7-byte instruction footprint; GP registers survive and ZF
// reports the role, so the original JZ/JNZ and both original branches remain authoritative.
__declspec(naked) void add_test() {
    __asm {
        pushad
        call add_held
        test eax,eax
        popad
        ret
    }
}
__declspec(naked) void toggle_test() {
    __asm {
        pushad
        call toggle_held
        test eax,eax
        popad
        ret
    }
}
__declspec(naked) void selection_test() {
    __asm {
        pushad
        call selection_button_held
        test eax,eax
        popad
        ret
    }
}
__declspec(naked) void minimap_hit_thunk() {
    __asm {
        push ebx
        push ecx
        push edx
        push esi
        push edi
        push eax
        call minimap_hit
        add esp,4
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret
    }
}
__declspec(naked) void minimap_press_test() {
    __asm {
        pushad
        call minimap_press
        test eax,eax
        popad
        ret
    }
}
__declspec(naked) void minimap_release_test() {
    __asm {
        pushad
        call minimap_release
        test eax,eax
        popad
        ret
    }
}
__declspec(naked) void main_pop_thunk() {
    __asm {
        push eax
        pushfd
        pushad
        call begin_main_pop
        popad
        popfd
        call dword ptr [native_pop]
        pushfd
        pushad
        mov ebx,esp
        sub esp,527
        and esp,-16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push dword ptr [ebx+36]
        call main_record
        add esp,4
        fxrstor [esp]
        mov esp,ebx
        popad
        popfd
        lea esp,[esp+4]
        ret
    }
}
__declspec(naked) void begin_mouse_pump_thunk() {
    __asm {
        pushfd
        pushad
        call begin_mouse_pump
        popad
        popfd
        mov byte ptr [ebp-030h],0
        mov byte ptr [ebp-028h],0
        ret
    }
}
__declspec(naked) void physical_buttons_thunk() {
    __asm {
        push ebx
        push ecx
        push edx
        push esi
        push edi
        call physical_buttons
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret
    }
}
__declspec(naked) void mouse_frame_thunk() {
    __asm {
        pushfd
        pushad
        mov ebx,esp
        sub esp,527
        and esp,-16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push ebp
        call mouse_frame
        add esp,4
        fxrstor [esp]
        mov esp,ebx
        popad
        popfd
        mov eax,dword ptr ds:[005204a1h]
        ret
    }
}
__declspec(naked) void world_order_thunk() {
    __asm {
        pushfd
        pushad
        mov ebx,esp
        sub esp,527
        and esp,-16
        fxsave [esp]
        fninit
        fldcw word ptr [esp]
        cld
        push ebp
        call mouse_world_order
        add esp,4
        fxrstor [esp]
        mov esp,ebx
        popad
        popfd
        mov dword ptr [esp],004434deh
        ret
    }
}
struct patch {
    uintptr_t site;
    uint8_t   size, expected[10], replacement[10];
};
patch    patches[24]{};
unsigned count = 0;
void     call_patch(uintptr_t site, const uint8_t *bytes, uint8_t size, uintptr_t target) {
    auto &p = patches[count++];
    p.site  = site;
    p.size  = size;
    std::memcpy(p.expected, bytes, size);
    std::memset(p.replacement, 0x90, size);
    p.replacement[0]       = 0xe8;
    const int32_t relative = int32_t(target - (site + 5));
    std::memcpy(p.replacement + 1, &relative, 4);
}
void test_patch(uintptr_t site, uint32_t address, uint8_t bit, bool toggle) {
    const uint8_t bytes[] = {0xf6, 0x05, uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16), uint8_t(address >> 24), bit};
    call_patch(site, bytes, 7, reinterpret_cast<uintptr_t>(toggle ? &toggle_test : &add_test));
}
} // namespace
const uintptr_t modal_widget_return = reinterpret_cast<uintptr_t>(&widget_modal_pump_return);
bool            install() {
    if (armed) return true;
    if (!mh::en_build_ok() || !observing() || !prepare_mouse()) return false;
    count                     = 0;
    const uint8_t input_tap[] = {0xe8, 0x49, 0x0c, 0x03, 0x00};
    call_patch(0x004a0546u, input_tap, 5, reinterpret_cast<uintptr_t>(&input_tap_thunk));
    const uint8_t modal[] = {0xe8, 0x39, 0xde, 0xff, 0xff};
    call_patch(0x004b7451u, modal, 5, reinterpret_cast<uintptr_t>(&widget_modal_pump_thunk));
    const uint8_t key[] = {0xe8, 0x33, 0xf4, 0x08, 0x00};
    call_patch(0x00441bf8u, key, 5, reinterpret_cast<uintptr_t>(&main_pop_thunk));
    const uint8_t mouse[] = {0xa1, 0xa1, 0x04, 0x52, 0x00};
    call_patch(0x00442da4u, mouse, 5, reinterpret_cast<uintptr_t>(&mouse_frame_thunk));
    const uint8_t world[] = {0x80, 0x7d, 0xd8, 0x00, 0x0f, 0x84, 0xde, 0x05, 0x00, 0x00};
    call_patch(0x00442ef6u, world, 10, reinterpret_cast<uintptr_t>(&world_order_thunk));
    test_patch(0x00446012u, 0x00e69d2du, 1, true);
    test_patch(0x0044601bu, 0x00e69d2du, 2, true);
    test_patch(0x00446026u, 0x00e69d46u, 1, false);
    test_patch(0x0044602fu, 0x00e69d3au, 1, false);
    test_patch(0x00446379u, 0x00e69d46u, 1, false);
    test_patch(0x00446382u, 0x00e69d3au, 1, false);
    test_patch(0x004465f1u, 0x00e69d2du, 1, true);
    test_patch(0x004465fau, 0x00e69d2du, 2, true);
    test_patch(0x0044661bu, 0x00e69d46u, 1, false);
    test_patch(0x00446624u, 0x00e69d3au, 1, false);
    test_patch(0x00445e43u, 0x00e69d46u, 1, false);
    test_patch(0x00445e4cu, 0x00e69d3au, 1, false);
    // Continuous world-drag viewport compensation tests the semantic SELECTION button's level.
    const uint8_t selection[] = {0xf6, 0x05, 0x9b, 0x89, 0xe5, 0x00, 0x01};
    call_patch(0x00442ba5u, selection, 7, reinterpret_cast<uintptr_t>(&selection_test));
    // The side-minimap HUD callback has its OWN physical-LMB pan/drag path, before the strategic
    // pump. Map only these two hit calls and edge predicates; all other HUD/menu hit tests stay raw.
    const uint8_t hit1[] = {0xe8, 0x9c, 0x9e, 0x00, 0x00}, hit2[] = {0xe8, 0x68, 0x9e, 0x00, 0x00};
    const uint8_t press[] = {0x83, 0x3d, 0x40, 0x9d, 0x51, 0x00, 0x00}, release[] = {0x83, 0x3d, 0x3c, 0x9d, 0x51, 0x00, 0x00};
    call_patch(0x0041183au, hit1, 5, reinterpret_cast<uintptr_t>(&minimap_hit_thunk));
    call_patch(0x0041186eu, hit2, 5, reinterpret_cast<uintptr_t>(&minimap_hit_thunk));
    call_patch(0x00411843u, press, 7, reinterpret_cast<uintptr_t>(&minimap_press_test));
    call_patch(0x0041187bu, release, 7, reinterpret_cast<uintptr_t>(&minimap_release_test));
    const uint8_t buttons[] = {0xe8, 0xc3, 0x3c, 0xfe, 0xff};
    call_patch(0x00442a4au, buttons, 5, reinterpret_cast<uintptr_t>(&physical_buttons_thunk));
    const uint8_t begin[] = {0xc6, 0x45, 0xd0, 0x00, 0xc6, 0x45, 0xd8, 0x00};
    call_patch(0x00441ba0u, begin, 8, reinterpret_cast<uintptr_t>(&begin_mouse_pump_thunk));
    // No installation can leave a half-remapped input policy behind.
    for (unsigned i = 0; i < count; ++i)
        if (mh::hook::promoted_owner_of(patches[i].site) || std::memcmp(reinterpret_cast<const void *>(patches[i].site), patches[i].expected, patches[i].size)) return false;
    unsigned done = 0;
    for (; done < count; ++done)
        if (!mh::hook::patch_bytes_guarded(patches[done].site, patches[done].expected, patches[done].replacement, patches[done].size)) break;
    if (done != count) {
        while (done) {
            --done;
            if (!mh::hook::patch_bytes_guarded(patches[done].site, patches[done].replacement, patches[done].expected, patches[done].size)) FatalAppExitW(0, mh::ui::tr(mh::ui::Str::STRATEGIC_INPUT_UNAVAILABLE));
        }
        return false;
    }
    armed = true;
    return true;
}
} // namespace mh::seams::strategic_input_native
