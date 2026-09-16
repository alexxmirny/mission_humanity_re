//
// tact_ambient_sound_selftest.cpp -- offline oracle for llm_tact_ambient_sound_tick (TACT1E,
// 2026-08-27). See tact/tact_ambient_sound.h for the derivation.
//
// WHY OFFLINE: reaches TWO ungated shared/effectful callees (llm_rand, llm_snd_play_matching_sample
// -- tools/data/tact_shared_callees.json, both twice_test:YES) -- arm_ready:false, proven here only.
//
#include "tact/tact_ambient_sound.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    double               now               = 0.0;
    int32_t              rand_queue[2]     = {0, 0};
    int32_t              rand_calls        = 0;
    bool                 play_called       = false;
    uint32_t             play_channel      = 0xdeadbeef;
    const snd_cfg_entry *play_cfg          = nullptr;
    int32_t              play_volume       = -1;
    int32_t              play_pan          = 0x5a5a5a5a;
    int32_t              play_note_or_rate = -1;
};

call_log &log() {
    static call_log l;
    return l;
}

void reset_log() { log() = call_log{}; }

double  mock_time_now() { return log().now; }
int32_t mock_rand() { return log().rand_queue[log().rand_calls++]; }
void    mock_snd_play(uint32_t channel, const snd_cfg_entry *cfg, int32_t volume, int32_t pan,
                      int32_t note_or_rate) {
    log().play_called       = true;
    log().play_channel      = channel;
    log().play_cfg          = cfg;
    log().play_volume       = volume;
    log().play_pan          = pan;
    log().play_note_or_rate = note_or_rate;
}

ambient_sound_calls mock_calls() { return {mock_time_now, mock_rand, mock_snd_play}; }

} // namespace

void run_ambient_sound_tests() {
    // T1: snd_enabled == 0 -> no-op, even with everything else primed to fire.
    {
        tact_fixture fx;
        fx.snd_enabled                       = 0;
        fx.snd_master_volume                 = 10000;
        fx.snd_channel_next_retrigger_time_v = 0.0;
        reset_log();
        log().now           = 999.0;
        log().rand_queue[0] = 5;
        log().rand_queue[1] = 5;

        tact_store own = fx.store();
        detail::ambient_sound_tick(fx.view(), own, mock_calls());

        ck_eq(log().play_called, false, "T1: snd_enabled==0 -> no play, 0x0042ef9a");
        ck_eq((uint32_t)log().rand_calls, 0u, "T1: no-op means llm_rand is never drawn at all");
        ck_eq_d(own.snd_channel0_retrigger_time(), 0.0, "T1: retrigger time untouched");
    }

    // T2: snd_enabled != 0 but snd_master_volume == 0 -> no-op.
    {
        tact_fixture fx;
        fx.snd_enabled       = 1;
        fx.snd_master_volume = 0;
        reset_log();
        log().now = 999.0;

        tact_store own = fx.store();
        detail::ambient_sound_tick(fx.view(), own, mock_calls());

        ck_eq(log().play_called, false, "T2: snd_master_volume==0 -> no play, 0x0042efa3");
        ck_eq((uint32_t)log().rand_calls, 0u, "T2: no-op means llm_rand is never drawn at all");
    }

    // T3: both gates pass, but the rate-limit has NOT elapsed (now <= retrigger + min_interval) --
    // no play, no rand draw, but this is NOT the same code path as T1/T2 (it reaches the volume_arg
    // computation first, i.e. the OOB read at snd_cfg_table[-4] DOES fire -- see T5 for why that
    // matters and is safe to exercise).
    {
        tact_fixture fx;
        fx.snd_enabled                       = 1;
        fx.snd_master_volume                 = 10000;
        fx.ambient_snd_min_interval_sec      = 20.0;
        fx.snd_channel_next_retrigger_time_v = 100.0;
        reset_log();
        log().now = 119.0; // 100+20=120, not yet elapsed (119 <= 120)

        tact_store own = fx.store();
        detail::ambient_sound_tick(fx.view(), own, mock_calls());

        ck_eq(log().play_called, false, "T3: rate limit not elapsed -> no play, 0x0042efe8");
        ck_eq((uint32_t)log().rand_calls, 0u, "T3: rate limit gate is BEFORE any rand() draw");
        ck_eq_d(own.snd_channel0_retrigger_time(), 100.0, "T3: retrigger time untouched when gated");
    }

    // T3b: EXACT boundary (now == deadline) -- the gate is `now <= deadline -> no-op` (JBE @0x0042efe8),
    // so equality must NOT fire. Distinguishes <= from < (T3's now=119 < deadline=120 satisfies both).
    {
        tact_fixture fx;
        fx.snd_enabled                       = 1;
        fx.snd_master_volume                 = 10000;
        fx.ambient_snd_min_interval_sec      = 20.0;
        fx.snd_channel_next_retrigger_time_v = 100.0;
        reset_log();
        log().now = 120.0; // == 100+20 exactly

        tact_store own = fx.store();
        detail::ambient_sound_tick(fx.view(), own, mock_calls());

        ck_eq(log().play_called, false, "T3b: now==deadline is NOT elapsed (JBE, not JB), 0x0042efe8");
    }

    // T4/T5/T6/T7: the full fire path. One case, many assertions, since they all depend on the same
    // primed state (splitting them would just re-prime the same fixture).
    //
    // snd_cfg_table_storage layout: index 0..3 are the "PADDING" (real index -4..-1), index 4 is the
    // real table's entry 0. T5 pins the OOB read by giving storage[0] (== snd_cfg_table[-4]) a
    // DISTINCT volume from every other entry the fire path touches.
    {
        tact_fixture fx;
        fx.snd_enabled                       = 1;
        fx.snd_master_volume                 = 10000;
        fx.ambient_snd_min_interval_sec      = 20.0;
        fx.snd_channel_next_retrigger_time_v = 100.0;
        fx.ambient_snd_zone_count            = 3;
        fx.ambient_snd_zone_table            = {7, 41, 99}; // index 1 is the one this case selects

        // T5: snd_cfg_table[-4].volume = storage[0].volume. Distinct from every other entry's volume
        // so a case that (wrongly) reads a different entry is observable.
        fx.snd_cfg_table_storage[0].volume = 200;
        // The REAL entry 0 (storage[4]) must NOT be what feeds volume_arg -- poison it differently.
        fx.snd_cfg_table_storage[4].volume = 1;
        // cfg_index 41 (zone_table[1]) is what T6's rand1 selects -- storage[4+41] = storage[45].
        fx.snd_cfg_table_storage[45].note_or_rate = 555;
        fx.snd_cfg_table_storage[45].volume       = 77; // must NOT be read as the volume source

        reset_log();
        log().now           = 121.0;            // 100+20=120, elapsed (121 > 120)
        log().rand_queue[0] = 32767 * 3 + 1000; // T6: /(zone_count*32767) with zone_count=3 -> idx 1
        log().rand_queue[1] = -200;             // T7: pan = -200/128 (truncating) = -1

        tact_store own = fx.store();
        detail::ambient_sound_tick(fx.view(), own, mock_calls());

        ck_eq(log().play_called, true, "T4: rate limit elapsed -> play fires, 0x0042efe8");
        ck_eq((uint32_t)log().rand_calls, 2u, "T4: exactly two llm_rand() draws (zone pick, then pan)");
        ck_eq((uint32_t)log().play_channel, 0u, "T4: channel arg is always 0, 0x0042f044");

        // T5: volume_arg = (snd_cfg_table[-4].volume(200) * 30 * master_volume(10000)) / 10000
        //                = (200*30*10000)/10000 = 200*30 = 6000.
        ck_eq((uint32_t)log().play_volume, 6000u,
              "T5: volume_arg derives from the OOB snd_cfg_table[-4].volume (200), NOT entry 0's "
              "(poisoned to 1) -- G28 deterministic watermark, 0x0042efaa-0x0042efc9");

        // T6: zone_table_idx = 32767*3+1000 / (3*32767) = 1 (integer division) -> zone_table[1]=41
        //     -> cfg_index=41 -> note_or_rate = storage[4+41].note_or_rate = 555.
        ck_eq((uint32_t)log().play_note_or_rate, 555u,
              "T6: zone selection resolves rand1 -> zone_table[1]=41 -> cfg_index 41's note_or_rate, "
              "0x0042efea-0x0042f01e");
        ck_eq((uint32_t)(uintptr_t)log().play_cfg, (uint32_t)(uintptr_t)&fx.snd_cfg_table_storage[45],
              "T6: cfg pointer passed to snd_play is &snd_cfg_table[cfg_index], not zone_table_idx, "
              "0x0042f03e");

        // T7: pan = rand2(-200) / 128, truncating toward zero = -1 (NOT floor, which would be -2).
        ck_eq((uint32_t)log().play_pan, (uint32_t)-1,
              "T7: pan truncates toward zero like IDIV, reproducing the SAR/SHL/SBB divide-by-128 "
              "idiom for a NEGATIVE rand draw, 0x0042f01e-0x0042f02f");

        ck_eq_d(own.snd_channel0_retrigger_time(), 121.0,
                "T4: retrigger_time_ch0 = time_now() AFTER the play call, 0x0042f04b-0x0042f050");
    }

    // T8: a POSITIVE pan draw truncates the same way (300/128 = 2.34 -> 2, not 3).
    {
        tact_fixture fx;
        fx.snd_enabled                       = 1;
        fx.snd_master_volume                 = 10000;
        fx.ambient_snd_min_interval_sec      = 20.0;
        fx.snd_channel_next_retrigger_time_v = 0.0;
        fx.ambient_snd_zone_count            = 1;
        fx.ambient_snd_zone_table            = {0};
        reset_log();
        log().now           = 999.0;
        log().rand_queue[0] = 0;   // zone_table_idx = 0 / (1*32767) = 0
        log().rand_queue[1] = 300; // pan = 300/128 = 2 (truncating)

        tact_store own = fx.store();
        detail::ambient_sound_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().play_pan, 2u, "T8: positive pan truncates toward zero (300/128=2), 0x0042f01e-0x0042f02f");
    }
}

} // namespace mh::tact::test
