//
// config/gameplay.cpp -- PT-SET1: see gameplay.h.
//
#include "config/gameplay.h"

#include "config/config.h"
#include "desync/replay_record.h"         // replay_rec::current_step: the step a live heat change is logged at
#include "include/mh_net_export.h"        // MH_Net_IsStarted / MH_Net_PeerCount: the lockstep-session lock of the live heat controls
#include "include/mh_transport_present.h" // mh::net::transport_present
#include "lockstep/turn_engine.h"
#include "seams/heat_ui.h"
#include "seams/net_internal.h"
#include "state/region_runtime.h" // mh::state::ptr, RID_GAME_SESSION_MODE

namespace mh::gameplay {

using namespace mh_gameplay;

const char *source_name(Source s) {
    switch (s) {
        case Source::LOCAL: return "LOCAL";
        case Source::PUBLISHED: return "PUBLISHED";
        default: return "ADOPTED from host";
    }
}

static void log_line(const char *s) { seam_log(s); }

static char g_cur_sum[2048] = "";
static char g_cur_src[16]   = "local";

static const char *source_tag(Source s) { return s == Source::LOCAL ? "local" : (s == Source::PUBLISHED ? "published" : "adopted"); }

void current(char *summary, int summary_cap, char *src, int src_cap) {
    if (summary && summary_cap > 0) lstrcpynA(summary, g_cur_sum, summary_cap);
    if (src && src_cap > 0) lstrcpynA(src, g_cur_src, src_cap);
}

uint32_t current_hash() {
    uint32_t h = 2166136261u;
    for (const char *c = g_cur_sum; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
    return h;
}

void note(const char *text) {
    char line[320];
    snprintf(line, sizeof(line), "; gameplay: %s\n", text);
    seam_log(line);
}

void log_raise_refused() {
    seam_log("; [caps] host uses raised limits but this game cannot raise them (relocation refused) -- leaving\n");
}

void log_applied_hash() {
    char line[64];
    snprintf(line, sizeof(line), "; gameplay: applied hash=%08x\n", current_hash());
    seam_log(line);
}

mh_net_proto::GameplaySet local_set() {
    int         n;
    const spec *sp = host_specs(&n);
    values      v;
    read_local(sp, n, g_ini, v);
    mh_net_proto::GameplaySet s;
    to_set(sp, n, v, s);
    return s;
}

uint32_t local_digest() {
    int         n;
    const spec *sp = host_specs(&n);
    return digest(sp, n);
}

void summary_of(const mh_net_proto::GameplaySet &set, char *out, int cap) {
    int         n;
    const spec *sp = host_specs(&n);
    values      v;
    from_set(sp, n, set, v, nullptr);
    summary_text(sp, n, v, out, cap);
}

static void apply_impl(const mh_net_proto::GameplaySet &set, Source src, const char *why, bool record) {
    int         n;
    const spec *sp = host_specs(&n);
    values      v;
    from_set(sp, n, set, v, log_line);
    int                        nh;
    const hook_row            *hk  = hooks(&nh);
    mh::lockstep::reimpl_fixes fx  = mh::lockstep::fixes();
    const int                  ran = dispatch<mh::lockstep::reimpl_fixes>(sp, n, v, hk, nh, fx);
    if (mh::config::ours_run()) mh::lockstep::set_fixes(fx);
    char sum[2048];
    summary_text(sp, n, v, sum, sizeof(sum));
    char line[2400];
    snprintf(line, sizeof(line), "; gameplay: %s%s%s%s%s [%d key(s), %d hook(s)]\n", source_name(src), sum[0] ? " " : "", sum,
             why ? " -- " : "", why ? why : "", n, ran);
    seam_log(line);
    if (record) {
        lstrcpynA(g_cur_sum, sum, sizeof(g_cur_sum));
        lstrcpynA(g_cur_src, source_tag(src), sizeof(g_cur_src));
    }
}

// ---- mods:M1WL v3: live changes of the stream heat keys from the ImGui panel (seams/heat_ui.h) ---------------------------------------------------------
bool heat_locked(const char **why) {
    const bool mp = *mh::state::ptr<const uint8_t>(mh::state::RID_GAME_SESSION_MODE) == 3 || (mh::net::transport_present() && MH_Net_IsStarted() && MH_Net_PeerCount() > 0);
    if (mp && why) *why = "multiplayer, would desync";
    return mp;
}

bool heat_set_live(heat_key which, int val) {
    if (heat_locked(nullptr)) return false;
    static const char *const NAMES[] = {"move_stream_heat", "move_stream_heat_cost", "move_stream_heat_window", "move_stream_heat_min_mates", "move_stream_heat_scale", "move_stream_heat_retrace", "move_stream_heat_turn", "move_stream_heat_scale_cap", "move_stream_heat_margin"};
    if (static_cast<int>(which) < 0 || static_cast<int>(which) > 8) return false;
    mh::lockstep::reimpl_fixes fx = mh::lockstep::fixes();
    { // the values IN FORCE are the retail splice's (fixes() only follows them when this run publishes fixes): start from those, so a change of one key keeps the rest
        mh::heatui::live_values lv;
        mh::heatui::get_live(&lv);
        fx.move_stream_heat           = lv.on != 0;
        fx.move_stream_heat_cost      = lv.cost;
        fx.move_stream_heat_window    = lv.window;
        fx.move_stream_heat_min_mates = lv.min_mates;
        fx.move_stream_heat_scale     = lv.scale;
        fx.move_stream_heat_retrace   = lv.retrace;
        fx.move_stream_heat_turn      = lv.turn;
        fx.move_stream_heat_scale_cap = lv.scale_cap;
        fx.move_stream_heat_margin    = lv.margin;
    }
    const mh::lockstep::reimpl_fixes was = fx;
    int                              nh;
    const hook_row                  *hk = hooks(&nh);
    value                            v;
    v.i      = val < 0 ? 0 : val; // an explicit setting: -1 ("follow the preset") is not offered to the panel
    bool ran = false;
    for (int h = 0; h < nh; ++h)
        if (hk[h].sec && lstrcmpiA(hk[h].sec, "net") == 0 && lstrcmpiA(hk[h].key, NAMES[which]) == 0) {
            hk[h].fn(v, fx);
            ran = true;
            break;
        }
    if (!ran) return false;
    const bool changed = fx.move_stream_heat != was.move_stream_heat || fx.move_stream_heat_cost != was.move_stream_heat_cost || fx.move_stream_heat_window != was.move_stream_heat_window ||
                         fx.move_stream_heat_min_mates != was.move_stream_heat_min_mates || fx.move_stream_heat_scale != was.move_stream_heat_scale ||
                         fx.move_stream_heat_retrace != was.move_stream_heat_retrace || fx.move_stream_heat_turn != was.move_stream_heat_turn ||
                         fx.move_stream_heat_scale_cap != was.move_stream_heat_scale_cap || fx.move_stream_heat_margin != was.move_stream_heat_margin;
    if (!changed) return false;
    if (mh::config::ours_run()) mh::lockstep::set_fixes(fx);
    char line[256];
    snprintf(line, sizeof(line), "; [heat] live set cost=%d window=%d mates=%d scale=%d retrace=%d turn=%d cap=%d margin=%d at step %u on=%d\n", fx.move_stream_heat_cost, fx.move_stream_heat_window,
             fx.move_stream_heat_min_mates, fx.move_stream_heat_scale, fx.move_stream_heat_retrace, fx.move_stream_heat_turn, fx.move_stream_heat_scale_cap, fx.move_stream_heat_margin, (unsigned)mh::desync::replay_rec::current_step(), fx.move_stream_heat ? 1 : 0);
    seam_log(line);
    return true;
}

void apply(const mh_net_proto::GameplaySet &set, Source src, const char *why) { apply_impl(set, src, why, true); }

void apply_local(Source src, const char *why) { apply_impl(local_set(), src, why, true); }

void restore_local(const char *why) { apply_impl(local_set(), Source::LOCAL, why, false); }

} // namespace mh::gameplay
