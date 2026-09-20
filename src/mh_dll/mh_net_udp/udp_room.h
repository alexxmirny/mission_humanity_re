#pragma once
//
// udp_room.h -- the HOST's relay room, MINTED per tunnel start (tracker mp:R6).
//
// WHAT WENT WRONG WITH THE PORT. R1 made a host's room its own `[net] port`, and R2 kept that for
// the host while it taught the CLIENT to re-dial whatever room the directory named. On a rig that
// is one host per relay and the port is as good a code as any. On the shared VPS relay every player
// ships with `port=6501`, so the second player to host is refused `room_busy` until the first lobby
// ends -- and a host that crashed out of a lobby and relaunches inside the relay's idle window is
// refused by its OWN stale slot. Measured 2026-09-19 on the VPS: the second smoke run's host was
// refused 30 times over 16 s while the killed first run's slot aged out.
//
// THE FIX IS A NUMBER NOBODY HAS TO AGREE ON. A client never types the room -- it learns it from the
// SESSIONS answer (mp:R2), so the host's code can be anything that is unique among the hosts on one
// relay at one moment. A random 30-bit value is that: two hosts collide with probability 2^-30 per
// pair, and the collision that does happen is answered by re-minting (`minter_on_busy`) rather
// than by waiting for the other lobby to end. Thirty bits and not thirty-two because the room rides
// to mh.dll's browser inside the SESSION_INFO sender int (session_info.h's
// `session_sender_for_relay_room`: BASE | room, 30 bits); a room that did not fit would be an
// UNROUTABLE row, dropped on the client side with nothing in either log to say why.
//
// ZERO IS NEVER A HOST ROOM. Room 0 is the relay's DIRECTORY room (relay.rs DIRECTORY_ROOM), the one
// a browsing client registers in before it knows any code, and a host naming it is refused by
// name. So `mint()` returns 0 for exactly one thing: the RNG failed. A caller cannot mistake that
// for a code, and udp_transport.cpp refuses to start the transport on it, the same way the endpoint
// refuses to host without secure randomness.
//
// WHY THIS IS A HEADER WITH NO SOCKET IN IT. udp_relay.cpp owns a socket and a thread and is not
// linked into net_selftest.exe; the decision half here -- draw, reject 0 and the room being avoided,
// retry a bounded number of times on room_busy, then let the refusal stand -- is a pure function of
// an injected RNG, and `net_selftest.exe udproomtest` drives it as such. The rig cannot host two
// lobbies at once (one box, one exclusive port per lane), so the offline arm is the one that proves
// the retry bound and the "fresh room after a crash is not the stale one" clause deterministically.
//
#ifndef MH_NET_UDP_ROOM_H
#define MH_NET_UDP_ROOM_H

#include <stdint.h>

#include "mh_net_proto/session_info.h" // SESSION_RELAY_ROOM_MAX -- the 30 bits the browser's sender int carries

namespace mh {
namespace udproom {

// The RNG, in MH_Key_Random's shape: 1 = `out` holds `n` random bytes, 0 = no randomness available.
typedef int (*rand_fn)(void *ctx, uint8_t *out, unsigned n);

// The widest room the client-side browser can route (session_info.h). Rooms are drawn UNDER it,
// never masked down to it after the fact -- a mask would make two draws that differ only above the
// ceiling the same room, which is a collision the RNG did not produce.
constexpr uint32_t ROOM_MAX = mh_net_proto::SESSION_RELAY_ROOM_MAX;

// How many `room_busy` refusals a host answers with a fresh room before the refusal is allowed to
// stand (the R1 path: the "another host already holds this room code" line, and the HELLO retrying
// the same room every HELLO_RETRY_MS). Four is not a tuned number; it is a bound on a loop whose
// first iteration already succeeds with probability 1 - 2^-30 against any one other host. It exists
// so a relay that answers room_busy to EVERYTHING (a misbehaving or hostile one) produces a log
// with four re-mint lines and then the refusal, rather than a host that mints forever and never
// reports being refused.
constexpr int BUSY_RETRIES = 4;

// Draws per mint() before it gives up. A draw is rejected for being 0 or for being `avoid`; the
// chance of rejecting eight in a row from a working RNG is (2/2^30)^8, i.e. this bound is reached
// only by an RNG that has stopped being one.
constexpr int MINT_DRAWS = 8;

// One room code: non-zero, <= ROOM_MAX, and not `avoid` (the room a re-mint is moving away from, or
// 0 when there is none). Returns 0 when the RNG fails -- 0 is not a host room, see the header.
inline uint32_t mint(rand_fn rnd, void *ctx, uint32_t avoid = 0) {
    if (rnd == nullptr) return 0;
    for (int i = 0; i < MINT_DRAWS; ++i) {
        uint8_t b[4];
        if (!rnd(ctx, b, 4)) return 0;
        const uint32_t r = ((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                            ((uint32_t)b[3] << 24)) &
                           ROOM_MAX;
        if (r != 0 && r != avoid) return r;
    }
    return 0;
}

// The host's room across one tunnel life: the room it is asking for, and how many times room_busy
// has already been answered by moving to a fresh one.
struct Minter {
    rand_fn  rnd;
    void    *ctx;
    uint32_t room; // the room this host currently names in its HELLO; 0 = never minted
    int      busy; // re-mints so far, <= BUSY_RETRIES
};

// A fresh minter for a tunnel start: `room` is a new code (0 if the RNG failed, and the caller must
// then refuse to start). `avoid` is a room the caller does not want back -- there is no stale-slot
// memory across a crash (a relaunched process has none), so this is for a caller that DOES know a
// previous room, such as a restart inside one process. Returns whether a room was minted.
inline bool minter_init(Minter &m, rand_fn rnd, void *ctx, uint32_t avoid = 0) {
    m.rnd  = rnd;
    m.ctx  = ctx;
    m.busy = 0;
    m.room = mint(rnd, ctx, avoid);
    return m.room != 0;
}

// The relay answered `room_busy` for `m.room` before this host held a handle: move to a fresh room
// if the bound allows. Returns true when `m.room` changed (the caller re-HELLOs naming it) and false
// when the retries are spent or the RNG failed, in which case `m.room` is untouched and the R1
// refusal path stands.
inline bool minter_on_busy(Minter &m) {
    if (m.busy >= BUSY_RETRIES) return false;
    const uint32_t fresh = mint(m.rnd, m.ctx, m.room);
    if (fresh == 0) return false;
    ++m.busy;
    m.room = fresh;
    return true;
}

} // namespace udproom
} // namespace mh

#endif // MH_NET_UDP_ROOM_H
