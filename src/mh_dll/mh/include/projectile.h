#pragma once
#include <cstdint>

//
// The strategic-mode projectile record: mh.exe's `/llm llm_strat_projectile` type, 121 bytes,
// fully byte-packed (doubles sit at odd offsets -- no padding). Field names/offsets/semantics
// mirror the Ghidra type exactly (see the weapon-FX notes); the field comments are authoritative.
// Slot 0's `active` doubles as the pool's live-count header. Phase 2 (the DLL-side allocator
// redirect for llm_strat_projectile_spawn) will manage slots as these fields instead of raw
// byte offsets.
//
#pragma pack(push, 1)
struct llm_strat_projectile {
    int32_t  active;            // 0x00 1=live 0=free; slot 0 header: this dword = live count
    int32_t  weapon_id;         // 0x04 index into cfg::final::data::Weapon (stride 0x16c)
    uint16_t x;                 // 0x08 current x (map px, wraps mod big_width)
    uint16_t y;                 // 0x0a current y (elevation-adjusted, wraps mod big_height)
    uint16_t src_x;             // 0x0c
    uint16_t src_y_ground;      // 0x0e source y without elevation
    uint16_t src_y_vis;         // 0x10 source y minus shooter elevation (visual launch)
    uint16_t dst_x;             // 0x12 retargeted each tick if homing
    uint16_t dst_y_ground;      // 0x14
    uint16_t dst_y_vis;         // 0x16 target y minus target elevation (visual impact)
    uint8_t  owner_player;      // 0x18 passed into explosion FX and damage
    double   delta_x;           // 0x19 wrapped flight delta src->dst
    double   delta_y;           // 0x21
    double   launch_time;       // 0x29 _G_LLM_STRAT_GAME_CLOCK at fire
    double   duration;          // 0x31 sqrt(dist)*Weapon.speed/Weapon.length; recomputed when homing
    double   anim_clock;        // 0x39 last bullet-anim advance
    double   smoke_clock;       // 0x41 next smoke-puff emission
    double   explo_pulse_clock; // 0x49 next in-flight damage pulse (Weapon.explo_time)
    int32_t  scatter_x;         // 0x51 total miss offset (from Weapon.missing), applied over flight
    int32_t  scatter_y;         // 0x55
    int32_t  facing;            // 0x59 24-dir; Weapon.type 8: random index into cos/sin table 0xe15848
    int32_t  anim_frame;        // 0x5d index into cfg::final::data::Anim
    int32_t  smoke_sprite;      // 0x61 cached; 0 = no smoke trail
    int32_t  explo_pulse_flag;  // 0x65 explodes periodically along path
    int32_t  anim_loop_frame;   // 0x69 loop-restart base frame for this direction
    uint16_t homing_player;     // 0x6d 0 if not homing
    int32_t  homing_unit;       // 0x6f target unit index, 0 = none
    uint16_t shooter_ref;       // 0x73 low nibble = player, bit 0x80 = fired by unit (vs turret)
    int32_t  shooter_unit;      // 0x75 kill credit / self-damage exclusion
};
#pragma pack(pop)

// The exe indexes this record with a hardcoded stride 0x79 baked into 61 IMUL sites; if this
// static_assert ever fails the C++ layout has diverged from what the game's code assumes.
static_assert(sizeof(llm_strat_projectile) == 121,
              "llm_strat_projectile must match the game's 0x79-byte record stride");
