#pragma once
//
// mh/ui/lobby_ping.h -- mp:L1b: the lobby slot-row panel's per-slot SRTT column.
//
namespace mh {
namespace ui {

// Refreshes the per-slot ping cell for every OCCUPIED HUMAN slot row (skips the local player's own
// row and any AI/open/closed slot). Reuses the color-spinner widget's spare secondary `label` field
// on each row -- llm_ui_widget_draw_content already paints it, right after the swatch, when it is
// non-NULL, so retail's own per-frame widget-list draw is what actually renders the text; this
// function only keeps the pointer/buffer current. Driven by the lobby's OWN per-frame tick
// (net_seams.cpp on_lobby_dispatch), not the present hook -- see lobby_ping.cpp for why a value
// retail's draw reads is the right mechanism here and a pixel blit issued from the tick would not
// be. No-op off the lobby screen, or before the slot widgets exist. Cheap when idle.
void lobby_ping_tick();

// mp:L1f -- THE HOST'S PUBLISHED PER-SLOT SUMMARY HAS ARRIVED (a FLAG_ANNOUNCE of kind
// ANNOUNCE_PING; net_seams.cpp on_announce_recv decodes it and calls this). `buf`/`len` are the
// announce payload verbatim, decoded here so the wire format has exactly one reader.
//
// WHY A CLIENT NEEDS IT AT ALL: the transport is a client-server STAR, so a client holds one
// connection -- to the host -- and can never measure another client. Every other occupied row is
// blank on its screen without this. See session_info.h's ANNOUNCE_PING block.
//
// CALLED ON THE RECEIVE THREAD. The table it fills is one aligned LONG per player id, written and
// read with Interlocked ops, so lobby_ping_tick() on the main thread can never see a torn entry and
// neither side takes a lock on a datagram path. DISPLAY ONLY -- nothing else in the DLL reads it.
void lobby_ping_on_published(const unsigned char *buf, int len);

// mp:L1f -- HOW MANY occupied human rows currently carry a real number on THIS peer's screen (its
// own row is never one of them). The `lobbyping <N>` script predicate (seams/ui_drive.cpp) waits on
// it, so a 3-peer scenario can gate its capture on the mechanism having actually run instead of on a
// frame budget: on a client `>= 2` is only reachable through the host's publication, because the
// star topology gives it exactly ONE row it can measure itself. Updated by lobby_ping_tick; 0 off
// the lobby screen.
int lobby_ping_measured_rows();

} // namespace ui
} // namespace mh
