// The mh.dll event sink (seams/host_event_sink.cpp) -- harness-side header, mirroring
// include/mh_hostapi_bind.h's role for the generated table: the harness binds, libmh emits.
#pragma once

#include <cstdint>

namespace mh::hook {

// Bind the routing sink (synchronous at emit). Returns libmh_set_event_sink's rc: 0 ok,
// -1 version mismatch (visible red at the arm report, not a silent skew).
int bind_host_event_sink();

// Records that reached the sink with an unroutable (channel, kind) -- zero by construction
// while the version handshake holds; reported beside the [hostapi] line.
uint32_t host_event_sink_unknown_count();

// Same injection shape as set_export_logger (this dir carries no feature knowledge): once a
// logger is installed, the sink logs ONE '[hostevt] first dispatch' line per (channel, kind)
// -- the per-row first-call anti-vacuity instrument, rig-visible where a shutdown-only count
// is not (a killed lane never reaches shutdown; D24's g_dropped lesson).
void set_host_event_sink_logger(void (*logger)(const char *line));

} // namespace mh::hook
