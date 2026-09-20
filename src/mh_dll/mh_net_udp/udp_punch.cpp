//
// udp_punch.cpp -- see udp_punch.h. The decision half of mp:R3, with no socket in it.
//
// Every function here takes `now_ms` as an argument and none of them reads a clock, allocates, or
// logs. That is not style: it is what lets `net_selftest.exe udppunchtest` drive a whole punch --
// probe, echo, promotion, a path going dark, demotion, a second promotion -- as a table of numbers
// in milliseconds, on a machine with no second peer and no firewall rule.
//
#include "udp_punch.h"

#include <string.h>

namespace mh {
namespace udppunch {

namespace {

int addr_len(uint8_t fam) { return fam == FAM_V6 ? 16 : 4; }

bool fam_ok(uint8_t fam) { return fam == FAM_V4 || fam == FAM_V6; }

// The 32-bit millisecond clock wraps every 49.7 days, and a subtraction in unsigned arithmetic is
// correct ACROSS the wrap while a comparison of two stamps is not. Every elapsed-time test in this
// file goes through here for that reason; `GetTickCount()` is the caller's source and it wraps.
bool elapsed(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }

} // namespace

// ---- the wire codec ------------------------------------------------------------------------------

bool cand_eq(const Cand &a, const Cand &b) {
    if (a.fam != b.fam || a.port != b.port) return false;
    return memcmp(a.addr, b.addr, (size_t)addr_len(a.fam)) == 0;
}

int cands_encode(const Cand *list, int n, uint8_t *out, int cap) {
    if (list == nullptr || out == nullptr || cap < 1) return 0;
    if (n < 0) n = 0;
    if (n > MAX_CANDS) n = MAX_CANDS;
    int need = 1;
    for (int i = 0; i < n; ++i) need += 4 + addr_len(list[i].fam);
    if (need > cap) return 0;
    out[0] = (uint8_t)n;
    int at = 1;
    for (int i = 0; i < n; ++i) {
        const int alen = addr_len(list[i].fam);
        out[at++]      = list[i].fam;
        out[at++]      = list[i].flags;
        out[at++]      = (uint8_t)(list[i].port & 0xff);
        out[at++]      = (uint8_t)(list[i].port >> 8);
        memcpy(out + at, list[i].addr, (size_t)alen);
        at += alen;
    }
    return at;
}

int cands_decode(const uint8_t *p, int len, Cand *out, int cap) {
    if (p == nullptr || out == nullptr || len < 1) return -1;
    const int n = p[0];
    if (n > MAX_CANDS || n > cap) return -1;
    int at = 1;
    for (int i = 0; i < n; ++i) {
        if (at + 4 > len) return -1;
        const uint8_t fam = p[at];
        if (!fam_ok(fam)) return -1;
        const int alen = addr_len(fam);
        if (at + 4 + alen > len) return -1;
        memset(&out[i], 0, sizeof(out[i]));
        out[i].fam   = fam;
        out[i].flags = p[at + 1];
        out[i].port  = (uint16_t)(p[at + 2] | ((uint16_t)p[at + 3] << 8));
        memcpy(out[i].addr, p + at + 4, (size_t)alen);
        at += 4 + alen;
    }
    // Trailing bytes are a shape we do not understand, not padding to skip. Refusing keeps the two
    // implementations of this codec honest: a field added on one side is a refusal on the other,
    // never a silent mis-parse of the entry after it.
    if (at != len) return -1;
    return n;
}

// ---- the state machine ----------------------------------------------------------------------------

void reset(Punch &p, uint64_t seed) {
    memset(&p, 0, sizeof(p));
    p.state      = ST_IDLE;
    p.chosen     = -1;
    p.next_token = seed | 1u; // never 0: 0 is "no probe outstanding"
}

int add_cands(Punch &p, const Cand *list, int n, uint32_t now_ms) {
    if (list == nullptr || n <= 0 || p.is_forced) return 0;
    int added = 0;
    for (int i = 0; i < n && p.n < MAX_CANDS; ++i) {
        if (!fam_ok(list[i].fam)) continue;
        bool known = false;
        for (int j = 0; j < p.n; ++j)
            if (cand_eq(p.path[j].cand, list[i])) {
                // A path already under test keeps its probe state. Overwriting would un-ack a
                // working path once a second, since the peer re-sends its whole list on a timer.
                p.path[j].cand.flags |= list[i].flags;
                known = true;
                break;
            }
        if (known) continue;
        Path &np = p.path[p.n++];
        memset(&np, 0, sizeof(np));
        np.cand = list[i];
        ++added;
    }
    if (added > 0 && p.state == ST_IDLE) {
        p.state      = ST_PROBING;
        p.started_ms = now_ms;
        // The first pump after a candidate arrives probes AT ONCE rather than after FAST_MS. The
        // acceptance clause is a wall-clock budget from match start, so the cheapest tenth of a
        // second to save is the one before the first probe.
        p.last_probe_ms = now_ms - FAST_MS;
    }
    return added;
}

int due_probes(Punch &p, uint32_t now_ms, int *out_idx, uint64_t *out_token, int cap) {
    if (p.is_forced || p.n == 0 || out_idx == nullptr || out_token == nullptr || cap <= 0) return 0;

    if (p.state == ST_DIRECT) {
        // Promoted: one keepalive on the chosen path and nothing else. Continuing to probe the
        // losers would keep every NAT binding this pair ever opened alive for no purpose, and it
        // would make a demotion indistinguishable from ordinary punching in a packet capture.
        if (p.chosen < 0 || !elapsed(now_ms, p.last_probe_ms, KEEP_MS)) return 0;
        p.last_probe_ms        = now_ms;
        p.path[p.chosen].token = p.next_token++;
        if (p.path[p.chosen].token == 0) p.path[p.chosen].token = p.next_token++;
        p.path[p.chosen].sent_ms = now_ms;
        out_idx[0]               = p.chosen;
        out_token[0]             = p.path[p.chosen].token;
        return 1;
    }

    const uint32_t every =
        elapsed(now_ms, p.started_ms, FAST_WINDOW_MS) ? SLOW_MS : FAST_MS;
    if (!elapsed(now_ms, p.last_probe_ms, every)) return 0;
    p.last_probe_ms = now_ms;

    int out = 0;
    for (int i = 0; i < p.n && out < cap; ++i) {
        Path &t = p.path[i];
        t.token = p.next_token++;
        if (t.token == 0) t.token = p.next_token++;
        t.sent_ms      = now_ms;
        out_idx[out]   = i;
        out_token[out] = t.token;
        ++out;
    }
    return out;
}

bool on_ack(Punch &p, uint64_t token, uint32_t now_ms) {
    if (token == 0) return false;
    for (int i = 0; i < p.n; ++i) {
        if (p.path[i].token != token) continue;
        p.path[i].acked = true;
        p.path[i].token = 0; // consumed: a replayed echo credits nothing a second time
        // THE ONLY THING THAT KEEPS A DIRECT PATH ALIVE. See the header's demotion note: an echo of
        // OUR OWN nonce is the only event that proves both halves of the path still work.
        if (p.state == ST_DIRECT && i == p.chosen) p.last_ack_ms = now_ms;
        return true;
    }
    return false;
}

int on_peer_probe(Punch &p, const Cand &from, uint32_t now_ms) {
    if (!fam_ok(from.fam)) return -1;
    for (int i = 0; i < p.n; ++i)
        if (cand_eq(p.path[i].cand, from)) {
            p.path[i].heard = true;
            // DELIBERATELY NOT A LIVENESS SIGNAL, and this line is where the bug was. An inbound
            // probe proves peer -> us and says nothing about us -> peer; treating it as liveness
            // pins a peer to a path whose outbound half is dead, forever. Measured on the rig.
            return i;
        }
    if (p.n >= MAX_CANDS) return -1;
    // AN ADDRESS NOBODY ADVERTISED, and the most valuable kind of candidate there is: a datagram
    // came FROM it, so it exists and it can reach us, which no advertised candidate proves. This is
    // the arm that makes punching work when one side's NAT maps a different port toward the peer
    // than the one the relay observed -- the advertised candidate is then wrong for everybody and
    // only the inbound probe carries the right one.
    Path &np = p.path[p.n++];
    memset(&np, 0, sizeof(np));
    np.cand  = from;
    np.heard = true;
    if (p.state == ST_IDLE) {
        p.state         = ST_PROBING;
        p.started_ms    = now_ms;
        p.last_probe_ms = now_ms - FAST_MS;
    }
    return p.n - 1;
}

int tick(Punch &p, uint32_t now_ms) {
    if (p.is_forced) return EV_NONE;

    if (p.state == ST_DIRECT) {
        if (p.chosen >= 0 && !elapsed(now_ms, p.last_ack_ms, DEAD_MS)) return EV_NONE;
        // The direct path went quiet. Back to the relay, and back to searching -- including on the
        // path that just died, because the commonest reason for this is a NAT rebinding, which the
        // next exchange of candidates repairs.
        p.state  = ST_PROBING;
        p.chosen = -1;
        for (int i = 0; i < p.n; ++i) {
            p.path[i].acked = false;
            p.path[i].heard = false;
            p.path[i].token = 0;
        }
        p.started_ms    = now_ms; // a fresh fast window: this is a new search, not the old one
        p.last_probe_ms = now_ms - FAST_MS;
        ++p.demotions;
        return EV_DEMOTED;
    }

    if (p.state != ST_PROBING) return EV_NONE;
    for (int i = 0; i < p.n; ++i) {
        if (!p.path[i].acked || !p.path[i].heard) continue;
        p.state       = ST_DIRECT;
        p.chosen      = i;
        p.last_ack_ms = now_ms; // the probe that validated it IS the first echo
        p.promoted_ms = now_ms;
        ++p.promotions;
        return EV_PROMOTED;
    }
    return EV_NONE;
}

const Cand *direct_target(const Punch &p) {
    if (p.state != ST_DIRECT || p.chosen < 0 || p.chosen >= p.n) return nullptr;
    return &p.path[p.chosen].cand;
}

void set_forced(Punch &p, bool forced_on, uint32_t now_ms) {
    if (p.is_forced == forced_on) return;
    p.is_forced = forced_on;
    if (!forced_on) return;
    // Pinning takes effect NOW, not at the next transition: the knob exists to be believed.
    if (p.state == ST_DIRECT) ++p.demotions;
    p.state         = ST_IDLE;
    p.chosen        = -1;
    p.n             = 0;
    p.last_probe_ms = now_ms;
}

bool forced(const Punch &p) { return p.is_forced; }

} // namespace udppunch
} // namespace mh
