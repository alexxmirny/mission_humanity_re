//
// tact/tact_ambient_sound.cpp -- see tact_ambient_sound.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_ambient_sound_tick_0042ef7b.asm), not from Ghidra's .c.
//
#include "tact/tact_ambient_sound.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4): time_GetCurrentTime, llm_rand, llm_snd_play_matching_sample
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const ambient_sound_calls &live_ambient_sound_calls() {
    static const ambient_sound_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_CRT(llm_rand),
        [](uint32_t channel, const snd_cfg_entry *cfg, int32_t volume, int32_t pan,
           int32_t note_or_rate) {
            mh::tact_host().llm_snd_play_matching_sample(channel, const_cast<snd_cfg_entry *>(cfg), volume,
                                                         pan, note_or_rate);
        },
    };
    return c;
}

namespace detail {

void ambient_sound_tick(const tact_view &v, tact_store &own, const ambient_sound_calls &c) {
    // @0x0042ef93-0x0042efa3: no-op unless BOTH gates hold.
    if (*v.snd_enabled == 0 || *v.snd_master_volume == 0) return;

    // @0x0042efaa-0x0042efc9: local_24 == -4 deterministically -- see this header's own comment
    // (the Watcom stack-probe watermark, not call-history garbage).
    const int32_t volume_arg =
        (static_cast<int32_t>(v.snd_cfg_table[-4].volume) * 30 * (*v.snd_master_volume)) / 10000;

    // @0x0042efce-0x0042efe8: the rate-limit gate.
    const double deadline = own.snd_channel0_retrigger_time() + *v.ambient_snd_min_interval_sec;
    if (c.time_now() <= deadline) return;

    // @0x0042efea-0x0042f00b.
    const int32_t rand1          = c.rand_fn();
    const int32_t zone_table_idx = rand1 / (*v.ambient_snd_zone_count * 0x7fff);
    const int32_t cfg_index      = v.ambient_snd_zone_table[zone_table_idx];

    // @0x0042f014-0x0042f01e: read BEFORE the second rand() call, matching the .asm's own order
    // (the note_or_rate argument is PUSHed to the stack before llm_rand's second CALL).
    const int32_t note_or_rate = v.snd_cfg_table[cfg_index].note_or_rate;

    // @0x0042f01e-0x0042f02f: C++ truncating division reproduces the SAR/SHL/SBB bit trick exactly
    // (Watcom's standard signed-divide-by-128 idiom) -- see this header's own comment.
    const int32_t rand2 = c.rand_fn();
    const int32_t pan   = rand2 / 128;

    // @0x0042f046.
    c.snd_play(/*channel=*/0, &v.snd_cfg_table[cfg_index], volume_arg, pan, note_or_rate);

    // @0x0042f04b-0x0042f050.
    own.snd_channel0_retrigger_time() = c.time_now();
}

} // namespace detail

void ambient_sound_tick() {
    tact_state st = state();
    detail::ambient_sound_tick(st.read, st.own, live_ambient_sound_calls());
}


} // namespace mh::tact
