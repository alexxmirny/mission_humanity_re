//
// config/gameplay.cpp -- PT-SET1: see gameplay.h.
//
#include "config/gameplay.h"

#include "config/config.h"
#include "lockstep/turn_engine.h"
#include "seams/net_internal.h"

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

void apply(const mh_net_proto::GameplaySet &set, Source src, const char *why) { apply_impl(set, src, why, true); }

void apply_local(Source src, const char *why) { apply_impl(local_set(), src, why, true); }

void restore_local(const char *why) { apply_impl(local_set(), Source::LOCAL, why, false); }

} // namespace mh::gameplay
