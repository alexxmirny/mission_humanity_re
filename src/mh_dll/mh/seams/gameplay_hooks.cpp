//
// seams/gameplay_hooks.cpp -- PT-SET1: the apply hook of every F_HOSTSYNC row in ini_keys.def.
//
// ONE entry per synced row, in this shape (tools/gen_ini_registry.py --check parses it: exactly one entry
// per F_HOSTSYNC row and vice versa):
//
//   {"net", "foo_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { MH_Foo_SetRuntime(v.i); fx.foo_fix = v.i != 0; }},
//
// A hook sets BOTH the retail-stub runtime global (the byte-patch carrier reads it) and the libmh
// `reimpl_fixes` field (the promoted body reads it). A setter that REFUSES (its splice is MISMATCHED, so the byte-patch
// carrier is absent) keeps the fx field off too.
//
#include "config/gameplay.h"
#include "lockstep/turn_engine.h"
#include "seams/net_internal.h"

namespace mh::gameplay {

static const hook_row k_hooks[] = {
    {"net", "turret_reload_realtime_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.turret_reload_realtime_fix = mh_set_turret_reload_realtime(v.i) != 0 && v.i != 0; }},
    {"net", "turret_fire_rate_mult", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { mh_set_turret_fire_rate_mult(v.f); fx.turret_fire_rate_mult = v.f; }},
    {"net", "turret_clip_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.turret_clip_fix = mh_set_turret_clip_fix(v.i) != 0 && v.i != 0; }},
    {"net", "turret_fire_poll_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.turret_fire_poll_fix = mh_set_turret_fire_poll_fix(v.i) != 0 && v.i != 0; }},
    {nullptr, nullptr, nullptr}, // sentinel: keeps the array non-empty if every synced row is ever removed
};

const hook_row *hooks(int *count) {
    *count = (int)(sizeof(k_hooks) / sizeof(k_hooks[0])) - 1;
    return k_hooks;
}

} // namespace mh::gameplay
