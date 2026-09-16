//
// sim/resid/sim_scenario_planet_clone.cpp -- see sim_scenario_planet_clone.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_scenario_planet_clone_0045ba25.asm); the Ghidra .c is
// a draft.
//
#include "sim/resid/sim_scenario_planet_clone.h"

#include <cstring>

#include "addr/mh_calls.gen.h" // typed callables for the effectful/frontier originals we still call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const scenario_planet_clone_calls &live_scenario_planet_clone_calls() {
    static const scenario_planet_clone_calls c = {
        MH_LIBMH_BIND(cfg_final_planet_Construct),
        MH_LIBMH_BIND(llm_str_char_subst),
        mh::host().ansi_to_wide,
        mh::state::evt::snd_ambient_planet_clone,
    };
    return c;
}

namespace detail {

// ---- llm_strat_scenario_planet_clone @0x0045ba25 -----------------------------------------------
void scenario_planet_clone(const sim_view &v, sim_store &own, const scenario_planet_clone_calls &c,
                           void *cfg_blob) {
    // 0x0045ba40-0x0045ba71: two LOCAL int[4] arrays (not state) whose ADDRESSES are pushed below as
    // the by-value struct's source_mul/source_add fields.
    int32_t source_mul[4] = {0xf, 0xf, 0xf, 0xf};
    int32_t source_add[4] = {0, 0, 0, 0};

    // 0x0045ba78-0x0045baa0: the by-value mh_cfg_pre_struct_Planet, field-by-field. See the header
    // banner for the full push-order -> field derivation; every offset here is
    // mh_structs.gen.h's own offsetof() static_assert, not a guessed name.
    mh::game::mh_cfg_pre_struct_Planet planet_data{};
    // 0x0045baa0: Planets[1].icon_index (base 0x00be6da0 + 1*0x427 + 0xc).
    planet_data.icon_index   = v.cfg_planets[1].icon_index;
    planet_data.x1           = 0;                                    // 0x0045ba9e
    planet_data.y1           = 0;                                    // 0x0045ba9c
    planet_data.x2           = 0;                                    // 0x0045ba9a
    planet_data.y2           = 0;                                    // 0x0045ba98
    planet_data.info_txt     = const_cast<char *>(v.empty_name_str); // 0x0045ba92-0x0045ba97
    planet_data.info_flc     = const_cast<char *>(v.empty_name_str); // 0x0045ba8c-0x0045ba91
    planet_data.source_mul   = source_mul;                           // 0x0045ba88-0x0045ba8b: &[EBP-0x28]
    planet_data.source_add   = source_add;                           // 0x0045ba84-0x0045ba87: &[EBP-0x38]
    planet_data.coordinate_x = 0;                                    // 0x0045ba82
    planet_data.coordinate_y = 0;                                    // 0x0045ba80
    planet_data.asteroids    = 0;                                    // 0x0045ba7e
    planet_data.turn_speed   = 0;                                    // 0x0045ba7c
    planet_data.enemy        = 0;                                    // 0x0045ba7a
    planet_data.index        = 0x1f;                                 // 0x0045ba78

    // cfg_blob stays an opaque caller-owned void* (declared_needs; same posture as
    // sim_session_begin_multi.h's declared_needs #12, which calls THIS function with the same
    // parameter). +0xfc is the name/map-name buffer; +0x18 is path_unc; +0x108 (used below) is
    // +0xfc plus 12, the truncation NUL.
    char *cfg_blob_bytes = static_cast<char *>(cfg_blob);
    char *map_name       = cfg_blob_bytes + 0xfc;
    char *path_unc       = cfg_blob_bytes + 0x18;

    // 0x0045baa8-0x0045babc: cfg_final_planet_Construct(define_index=0xa8, invention_index=0,
    // map_name, &planet_data, path_unc).
    c.cfg_final_planet_Construct(0xa8, 0, map_name, &planet_data, path_unc);

    // 0x0045bac1-0x0045bad5: llm_str_char_subst(map_name, mode=2, c1='.', c2='\0').
    c.str_char_subst(map_name, 2, '.', '\0');

    // 0x0045bada-0x0045baed: an inline strlen(map_name) (REPNE SCASB in the original, needle primed
    // by XOR EAX,EAX). std::strlen is byte-for-byte equivalent for a NUL-terminated buffer.
    std::size_t name_len = std::strlen(map_name);

    // 0x0045baed: CMP ECX,0xf; JBE skip -- truncate only when name_len is STRICTLY GREATER than 15.
    if (name_len > 0xf) {
        // 0x0045baf2-0x0045baf9: cap the name at 12 chars (a BYTE store at +0xfc+0xc == +0x108).
        cfg_blob_bytes[0x108] = '\0';

        // 0x0045bafc-0x0045bb12: a SECOND, independent inline strlen -- re-measures the
        // JUST-TRUNCATED string to find the append point (not a reuse of name_len above).
        std::size_t trunc_len = std::strlen(map_name);

        // LAB_0045bb13-0x0045bb29: an inline, unrolled-by-2 strcpy appending the ellipsis string at
        // 0x00501039 (Ghidra auto-label s_..._00501039; content "..." per this header's
        // declared_needs -- confirmed by the sibling sim_player_init.cpp/selftest that copies the
        // same label the same way). std::strcpy is byte-for-byte equivalent.
        std::strcpy(map_name + trunc_len, "...");
    }

    // 0x0045bb2c-0x0045bb3f: widen the (possibly truncated) name into the scenario-planet UTF-16
    // scratch buffer (an address escape -- the widening is performed by the original callee), then
    // publish the RETURNED pointer into G_TEXT_PTRS[0xa8].
    void *wide            = c.str_ansi_to_wide(own.scenario_planet_name_w(), map_name);
    own.text_ptr_at(0xa8) = static_cast<const wchar_t *>(wide);

    // 0x0045bb44: Planets[0x1f].system_index = 0. Base 0x00be6da0 + 0x1f*0x427 + 0x8 ==
    // 0x00beee61, matching the instruction operand exactly.
    own.cfg_planet_at(0x1f).system_index = 0;

    // 0x0045bb4e-0x0045bb53: llm_snd_ambient_planet_clone(1).
    c.snd_ambient_planet_clone(1);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void scenario_planet_clone(void *cfg_blob) {
    sim_state st = state();
    detail::scenario_planet_clone(st.read, st.own, live_scenario_planet_clone_calls(), cfg_blob);
}

} // namespace mh::sim
