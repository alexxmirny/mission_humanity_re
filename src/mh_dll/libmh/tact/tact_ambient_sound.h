//
// tact/tact_ambient_sound.h -- TACT1E: the per-frame ambient-sound roll.
//
//   llm_tact_ambient_sound_tick @0x0042ef7b (0xe5)
//
// Rate-limited (every _G_LLM_TACT_AMBIENT_SND_MIN_INTERVAL_SEC == 20.0s, per-channel-0 timestamp)
// pick of a random ambient zone/sample, played via llm_snd_play_matching_sample. No-op if sound is
// disabled or the master volume is 0.
//
// UNINITIALISED-READ HAZARD (re-derived and cross-checked against the stack-probe imprint before
// writing this file -- do not "fix" it):
//
//   The very first cfg-table access, `_G_LLM_SND_CFG_TABLE[local_24].volume` @0x0042efaa-0x0042efb2,
//   reads a stack slot ([EBP-0x20]) that NOTHING in this function writes before that read. G28's
//   mechanism applies and gives a DETERMINISTIC answer, not "unknowable residue":
//
//     R = ESP at function entry (before PUSH EBP). This function's post-probe pushes are
//     EBX/ECX/EDX/ESI/EDI (5 dwords) @0x0042ef88-0x0042ef8c, landing at R-0x10..R-0x20. [EBP-0x20]
//     converts to R-relative as R + (-0x20 - 4) = R-0x24 -- exactly the LAST row of G28's imprint
//     table ("R-0x24, MOV [ESP+EBX],EBX @0x004cf4b9 EBX=-4, value 0xfffffffc"), one slot BELOW the 5
//     pushes' reach (which stop at R-0x20). So local_24 == -4 on EVERY call, deterministically --
//     not a function of call history.
//
//   -4 * sizeof(llm_snd_cfg_entry)=44 = -176 bytes from the table base (0x0070d5c0, i.e. entry 0's
//   OWN base minus 176), landing at 0x0070d533 -- an address 35 bytes into that phantom "entry -4",
//   which the struct's own layout (offsetof(volume)==0x23==35) puts exactly on `.volume`. This is
//   itself a real OOB read of whatever static data precedes _G_LLM_SND_CFG_TABLE in the image (the
//   table lives in .bss -- confirmed unreadable via Ghidra's static image, i.e. runtime-filled data,
//   not a compile-time constant this file could hardcode). PRESERVED LITERALLY (Law 2): the
//   translation computes `snd_cfg_table[-4].volume` via the SAME pointer arithmetic the original
//   uses, which lands on the SAME live byte when running in-process against the real game (the
//   region is simply whatever the sound-init code left there) -- this file does not, and cannot,
//   know or assert its value.
//
// PROOF PATH: proof:OFFLINE(tacttest, shared:1) per tools/data/tact_migration.json -- this function
// reaches TWO ungated shared/effectful callees (llm_rand, llm_snd_play_matching_sample -- both
// confirmed in tools/data/tact_shared_callees.json: llm_rand's PRNG draw and
// llm_snd_play_matching_sample's DirectSound Play are both "twice_test: YES", i.e. arming this site
// would double-draw the PRNG AND double-play the sample), so it is NEVER rig-armed
// (arm_ready:false) -- proven by net_selftest.exe tacttest only.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable callee seam -- time/rand/the actual play call are all non-deterministic or effectful in
// the real game, so an offline oracle must mock all three (the `door_calls` shape, tact_door.h).
struct ambient_sound_calls {
    double (*time_now)();
    int32_t (*rand_fn)();
    void (*snd_play)(uint32_t channel, const snd_cfg_entry *cfg, int32_t volume, int32_t pan,
                     int32_t note_or_rate);
};

namespace detail {

// llm_tact_ambient_sound_tick @0x0042ef7b.
//
//  0. @0x0042ef93-0x0042efa3: no-op unless snd_enabled != 0 AND snd_master_volume != 0.
//  1. @0x0042efaa-0x0042efc9: volume_arg = (snd_cfg_table[-4].volume * 30 * snd_master_volume) /
//     10000 -- see this header's own comment on the OOB read.
//  2. @0x0042efce-0x0042efe8: no-op unless time_now() > retrigger_time_ch0 + min_interval_sec (the
//     rate-limit gate).
//  3. @0x0042efea-0x0042f00b: zone_table_idx = rand() / (zone_count * 0x7fff); cfg_index =
//     ambient_snd_zone_table[zone_table_idx].
//  4. @0x0042f014-0x0042f01e: note_or_rate = snd_cfg_table[cfg_index].note_or_rate (read+pushed
//     BEFORE the second rand() call, not after -- matches the .asm's own instruction order).
//  5. @0x0042f01e-0x0042f02f: pan = rand() / 128 (C++ truncating division reproduces the original's
//     SAR/SHL/SBB divide-by-128-with-negative-rounding-correction bit trick exactly -- the Watcom
//     optimizer's standard signed-division-by-power-of-2 idiom, not independent logic).
//  6. @0x0042f046: llm_snd_play_matching_sample(channel=0, &snd_cfg_table[cfg_index], volume_arg,
//     pan, note_or_rate).
//  7. @0x0042f04b-0x0042f050: retrigger_time_ch0 = time_now().
void ambient_sound_tick(const tact_view &v, tact_store &own, const ambient_sound_calls &c);

} // namespace detail

void ambient_sound_tick();

// Declared here per the module convention; DEFINED in tact_ambient_sound.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
