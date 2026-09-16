//
// sim/resid/sim_table_resets.cpp -- see sim_table_resets.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_tech_tables_reset_00455b5a.asm,
// tmp/decomp_sim_resid/llm_strat_prod_reset_system_0048ffe9.asm), the Ghidra .c drafts beside each
// being drafts only.
//
#include "sim/resid/sim_table_resets.h"

#include "addr/mh_calls.gen.h"  // typed callables for the effectful/frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// ---- llm_strat_tech_tables_reset local constants -------------------------------------------------
// Projects[100] -- cfg_project capacity; the reset loop bound (`CMP ...,0x64`, 0x455c7c) matches the
// region's own canonical size (RID_PROJECTS: base 0xbe1c60, size==reach==20800==100*0xd0 -- no
// discrepancy, unlike Upgrades below).
constexpr int32_t PROJECTS_COUNT = 100;

// Upgrades -- the reset loop bound (`CMP ...,0x64`, 0x455d1d) is 100, but mh_addrs.gen.h documents
// the table as cfg_final_struct_Upgrade[99] and mh_regions.gen.h's RID_UPGRADES registry entry
// records size=10296 (99*0x68, canonical) vs reach=10400 (100*0x68) -- see the header's PRESERVE-BUG
// note. Named separately from PROJECTS_COUNT so the mismatch reads as deliberate, not a typo.
constexpr int32_t UPGRADES_RESET_COUNT = 100;

// Per-Invention depend[]/per-Upgrade objects[] sub-array counts, read straight off the inner loop
// bounds (`CMP ...,0x10` @0x455b97 and @0x455d54).
constexpr int32_t INVENTION_DEPEND_COUNT = 16; // cfg_t_invention_index_s[16], 2-byte-stride
constexpr int32_t UPGRADE_OBJECTS_COUNT  = 16; // cfg_t_upgrade_object_index[16], 4-byte-stride
constexpr int32_t RESOURCE_SLOTS         = 7;  // Project::resource[7] (`CMP ...,0x7` @0x455cd4)

// ---- llm_strat_prod_reset_system local constants --------------------------------------------------
constexpr int32_t PROD_QUEUE_SLOT_COUNT = 32; // player_profile::prod_queue_slot[32] (`CMP ...,0x20` @0x490026)
constexpr int32_t SHUTTLE_RELEASE_COUNT = 10; // shuttle slots released per player (`CMP ...,0xa` @0x490058)

} // namespace

// ---- llm_strat_prod_reset_system @0x0048ffe9 --------------------------------------------------------

const prod_reset_system_calls &live_prod_reset_system_calls() {
    static const prod_reset_system_calls c = {
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
    };
    return c;
}

namespace detail {

// ---- llm_strat_tech_tables_reset @0x00455b5a -------------------------------------------------------
void tech_tables_reset(const sim_view &v, sim_store &own) {
    // 0x00455b72: outer loop over Progress[]/progress[][] rows, inv = 0..299 (`CMP ...,0x12c`
    // @0x00455b79). PROGRESS_ROW_COUNT (300, sim_state.h) is the same capacity constant
    // sim_store::progress_at()'s row dimension already uses.
    for (int32_t inv = 0; inv < PROGRESS_ROW_COUNT; ++inv) {
        cfg_invention &invn = own.cfg_invention_at(inv);

        // 0x00455b93-0x00455bb6: Progress[inv].depend[0..15] = 0. Struct field is flattened
        // `uint8_t depend[32]` (2-byte-stride cfg_t_invention_index_s entries, per the struct's own
        // comment) -- each store below is a WORD write of 0 in the .asm
        // (`MOV word ptr [EAX+0xe162e4],0`, EAX = inv*0x67 + k*2); writing both bytes of the pair to
        // 0 reproduces that store byte-identically without a reinterpret_cast.
        for (int32_t k = 0; k < INVENTION_DEPEND_COUNT; ++k) {
            invn.depend[k * 2]     = 0;
            invn.depend[k * 2 + 1] = 0;
        }

        // 0x00455bbd-0x00455c0e: progress[j][inv].{available,acquired,f3} = false, j = 0..7. Address
        // derivation (0x00455bca-0x00455bd9): EDX = j*0x384(900) + inv*3, base 0x00c38750 -- row
        // stride 900 == 300*3 makes j the OUTER (player) axis and inv (0..299, this function's own
        // outer counter) the INNER (row) axis, exactly sim_store::progress_at(player, row)'s
        // `progress_[player*PROGRESS_ROW_COUNT + row]` layout.
        for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
            player_progress &pr = own.progress_at(static_cast<uint32_t>(j), inv);
            pr.available        = 0; // false
            pr.acquired         = 0; // false
            pr.f3               = 0; // false
        }

        // 0x00455c0e-0x00455c1c: Progress[inv].f2 = 0 (offset 0x63/99 -- EAX = inv*0x67 + 0xe16347;
        // 0xe16347-0xe162e4 == 0x63, the last 4 bytes of the 0x67-byte entry).
        invn.f2 = 0;

        // 0x00455c1c-0x00455c57: copy the "wynalazek" (Polish: "invention") placeholder name string
        // into Progress[inv].some_trash[64]. The original stages it through a 12-byte local stack
        // buffer (three unrolled MOVSD from 0x00501091) before the byte-pair copy loop; that hop does
        // not change the observable result -- the copy loop below stops at the source's first NUL
        // (byte 9 of "wynalazek\0"), so it never reaches the extra 2 bytes the stack buffer also
        // picked up from the next packed static in that blob -- so this reads directly from the
        // string. The binding is sim_view::invention_name_placeholder (0x00501091), added by
        // SIM-RESID-IF's third close -- this row was DEFERRED until it existed; see the header.
        {
            const char *src = v.invention_name_placeholder;
            char       *dst = invn.some_trash;
            // 0x00455c3f-0x00455c55: 2 bytes per iteration, checking for NUL after EACH byte
            // (not only every 2nd) -- transcribed literally, matching the asm's per-byte JZ.
            for (;;) {
                char c0 = src[0];
                dst[0]  = c0;
                if (c0 == '\0')
                    break;
                char c1 = src[1];
                src += 2;
                dst[1] = c1;
                dst += 2;
                if (c1 == '\0')
                    break;
            }
        }

        // 0x00455c57-0x00455c70: Progress[inv].type = UNDEFINED (cfg_enum_E_INVETION_TYPE member 0,
        // literal 0 in the .asm's `MOV byte ptr [...],0`), Progress[inv].index = 0.
        invn.type  = 0; // UNDEFINED
        invn.index = 0;
    }

    // 0x00455c75: second outer loop, Projects[0..99] (`CMP ...,0x64` @0x00455c7c).
    for (int32_t p = 0; p < PROJECTS_COUNT; ++p) {
        cfg_project &proj = own.cfg_project_at(p);

        // 0x00455c8c-0x00455ca4: invention (offset 0) = 0, type (offset 0xc) = 0. name/icon
        // (offsets 4/8) are NOT touched by the reset -- a real gap in the original, not an omission
        // here.
        proj.invention = 0;
        proj.type      = 0;

        // 0x00455cae-0x00455cbf: build_time (double, offset 0x48) = 0.0, written as two dword-zero
        // stores (@0xbe1ca8 low, @0xbe1cac high) -- both halves zero, so a plain double assignment
        // reproduces it byte-identically. (Written before the resource[] loop below in the .asm's own
        // instruction order; independent fields, so the C++ order does not matter.)
        proj.build_time = 0.0;

        // 0x00455cd0-0x00455d11: resource[0..6] = {id=UNDEFINED(0), val=0}.
        for (int32_t r = 0; r < RESOURCE_SLOTS; ++r) {
            proj.resource[r].id  = 0; // UNDEFINED (cfg_enum_E_RESOURCE member 0)
            proj.resource[r].val = 0;
        }
    }

    // 0x00455d16: third outer loop, Upgrades[0..99] INCLUSIVE -- 100 entries, see the header's
    // PRESERVE-BUG note (UPGRADES_RESET_COUNT above).
    for (int32_t u = 0; u < UPGRADES_RESET_COUNT; ++u) {
        cfg_upgrade &up = own.cfg_upgrade_at(u);

        // 0x00455d2d-0x00455d3f: invention (offset 0) = 0, type (offset 4) = UNIT
        // (cfg_enum_E_UPGRADE_TYPE member 0, literal 0 in the .asm).
        up.invention = 0;
        up.type      = 0; // UNIT

        // 0x00455d50-0x00455d75: objects[0..15] = 0. Struct field is flattened `uint8_t objects[64]`
        // at 4-byte stride (cfg_t_upgrade_object_index[16] -- see cfg_upgrade_object_id()'s comment
        // in sim_state.h for the same stride on the READ side); each store below is a dword write of
        // 0 in the .asm (`MOV dword ptr [EAX+0xbcf8d8],0`), so zeroing all four bytes reproduces it
        // without a reinterpret_cast.
        for (int32_t k = 0; k < UPGRADE_OBJECTS_COUNT; ++k) {
            up.objects[k * 4 + 0] = 0;
            up.objects[k * 4 + 1] = 0;
            up.objects[k * 4 + 2] = 0;
            up.objects[k * 4 + 3] = 0;
        }

        // 0x00455d75-0x00455dd7: energy/step_speed/range_min/range_max/missing/power/fire_range = 0.
        // step_speed (double, offset 0x4c) is two dword-zero stores (@0xbcf91c low, @0xbcf920 high),
        // both halves zero -- a plain double assignment reproduces it byte-identically. `power`
        // (offset 0x64) is stored BEFORE `fire_range` (offset 0x60) in the .asm's own instruction
        // order (@0x455dc9 vs @0x455dd7); independent fields, order does not matter for the result.
        up.energy     = 0;
        up.step_speed = 0.0;
        up.range_min  = 0;
        up.range_max  = 0;
        up.missing    = 0;
        up.power      = 0;
        up.fire_range = 0;
    }
}

void prod_reset_system(sim_store &own, const prod_reset_system_calls &c) {
    // 0x00490001: outer loop, player = 0..7 (`CMP ...,0x8` @0x00490008).
    for (int32_t player = 0; player < MAX_PLAYERS; ++player) {
        // 0x00490022-0x0049004d: _G_LLM_STRAT_PLAYERS[player].prod_queue_slot[0..31] = 0. Address
        // derivation: EAX = player*0x740 + slot*4 + 0xcff1f4; 0xcff1f4-0xcff060(_G_LLM_STRAT_PLAYERS
        // base) == 0x194 (404), exactly player_profile::prod_queue_slot's struct offset.
        player_profile &prof = own.profile_at(player);
        for (int32_t slot = 0; slot < PROD_QUEUE_SLOT_COUNT; ++slot)
            prof.prod_queue_slot[slot] = 0;

        // 0x00490054-0x00490071: llm_strat_prod_shuttle_slot_release(player, slot) for slot = 0..9
        // (`CMP ...,0xa` @0x00490058). Register mapping read straight off the call site
        // (0x00490064 `MOV EDX,[slot]`, 0x00490067 `MOV EAX,[player]`) and confirmed against
        // mh_calls.gen.h's own binding for this callee (`s_void_EAX_EDX`, param_1=EAX=player,
        // param_2=EDX=slot).
        for (int32_t slot = 0; slot < SHUTTLE_RELEASE_COUNT; ++slot)
            c.prod_shuttle_slot_release(player, slot);
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void tech_tables_reset() {
    sim_state st = state();
    detail::tech_tables_reset(st.read, st.own);
}

void prod_reset_system() {
    sim_state st = state();
    detail::prod_reset_system(st.own, live_prod_reset_system_calls());
}

} // namespace mh::sim
