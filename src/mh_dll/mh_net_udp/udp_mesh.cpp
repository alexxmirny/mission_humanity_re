//
// udp_mesh.cpp -- mp:U61 (HM-M3): mesh probes, address brokering, the RTT matrix and SUCCESSION epochs
// (the host-migration plan (mp:U57) sections 3-5), as Endpoint methods. The socket-free half (elect(), the
// matrix, the codecs, the probe keys) is mesh.h; read that first, and the plan after it.
//
// THE SHAPE, in the order a match's first seconds go:
//
//   1. A client that has been admitted sends MK_CAND to the host: the addresses it might be reached at
//      (this machine's non-loopback IPv4 addresses with the endpoint's bound port) and whether it is
//      behind a relay tunnel.
//   2. The host (once it has two or more clients) mints a per-match MESH KEY and sends each client MK_BROKER:
//      the key plus every OTHER client's candidates -- the address the host itself observed that client at
//      (its public mapping) and what the client reported. Brokering exposes a client's address to the
//      other clients: a privacy change the user accepted (plan Q4).
//   3. Each client probes the others DIRECTLY from the endpoint's own socket, the very socket whose
//      mapping the host observed, so the brokered address is the mapping a peer will actually hit. A
//      sealed probe carries a nonce, the echo returns it, the prober times it (QPC). 250 ms for 3 s after
//      brokering (the warm-up burst that opens the pinholes from both ends), then 1 Hz; in steady state
//      only the LOWER id probes a pair (the echoes keep the higher side's pinhole warm), and the higher id
//      probes too only while it hears nothing from the lower. One direction measured is a round trip, so
//      the edge exists.
//   4. Each client reports its row (MK_ROW, 1 Hz for 10 s, then every 2 s) with its own relay-leg round
//      trip; the host merges the rows into the matrix.
//   5. The host publishes SUCCESSION epochs (MK_EPOCH): epoch number, the matrix, the ranked successor
//      list from the pure elect(), dial info per candidate, a fresh pre-minted relay room per candidate in
//      a relayed match. Every client recomputes elect() from the matrix it received and checks it against
//      the ranking, then acks (MK_ACK). Epoch e+1 goes out only after EVERY client acked e.
//
// mp:U62 (HM-M4) -- THE HANDOVER lives at the end of this file (hub_leave / hl_*): the same FLAG_MESH
// channel carries MK_LEAVING (hub -> every client) and MK_LEAVE_ACK (client -> hub), and the mesh machinery
// above RESUMES on a rehomed endpoint (the new hub brokers again under the SAME key and numbers its epochs
// above every epoch a survivor holds).
//
// RELAYED CLIENTS. A client behind the relay tunnel owns a LOOPBACK socket, so its endpoint cannot be
// probed from outside; its leg socket belongs to the tunnel. Such a client is flagged relayed, is neither
// probed nor probing, and contributes its relay-leg round trip instead (hook `leg_rtt_dms`); the
// election's relay tier (mesh.h) reads those. Probing client-to-client through the leg socket is a
// separate piece of work (see the host-migration plan (mp:U57) section 6.4).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <string.h>

#include "udp_endpoint.h"
#include "mh_net_key.h"

namespace mh {
namespace netudp {

using namespace mh_net_proto;
namespace U = mh_net_proto::udp;

namespace {

constexpr DWORD WARM_MS         = 3000;  // after brokering: probe every WARM_FAST_MS for this long
constexpr DWORD WARM_FAST_MS    = 250;
constexpr DWORD STEADY_MS       = 1000;  // plan Q6: 1 Hz
constexpr DWORD ECHO_FRESH_MS   = 5000;  // an edge whose last echo is older than this reads as unmeasured
constexpr DWORD BEST_FRESH_MS   = 3000;  // probe only the confirmed address while it answered this recently
constexpr DWORD HEARD_FRESH_MS  = 3000;  // the lower id's own probes drive the pair while heard this recently
constexpr DWORD ROW_WARM_MS     = 1000;
constexpr DWORD ROW_STEADY_MS   = 2000;
constexpr DWORD ROW_STALE_MS    = 6000;  // a client's row older than this is dropped from the host's snapshot
constexpr DWORD EPOCH_SETTLE_MS = 1000;  // a roster change must hold this long before an epoch is published
constexpr DWORD EPOCH_DEADLINE_MS = 4000; // ...and a first/changed epoch goes out by now even with a partial matrix
constexpr DWORD EPOCH_RESEND_MS = 1000;
constexpr DWORD BROKER_MIN_MS   = 200;
constexpr DWORD STAT_LOG_MS     = 10000;
// mp:U62 -- the handover's clocks. SUCC_ACK_MS: how long the leaving hub waits for the FIRST successor to
// acknowledge before it re-issues the list without it (an ack is one round trip; this is generous).
// HL_TIMEOUT_DEFAULT: the whole wait for every acknowledgement when the caller names none.
constexpr DWORD SUCC_ACK_MS        = 1500;
constexpr int   HL_TIMEOUT_DEFAULT = 2500;
constexpr DWORD HL_DIAL_BUDGET_MS  = 10000; // a survivor's re-dial of the successor keeps trying this long

int64_t qpc_now_m() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)t.QuadPart;
}
int64_t qpc_freq_m() {
    static int64_t f = 0;
    if (f == 0) {
        LARGE_INTEGER x;
        f = QueryPerformanceFrequency(&x) ? (int64_t)x.QuadPart : 1;
    }
    return f;
}
uint64_t unix_ms_m() {
    FILETIME       ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}
bool is_loopback(const sockaddr_in &a) { return (ntohl(a.sin_addr.s_addr) >> 24) == 127; }

mesh::Addr4 to_addr4(const sockaddr_in &a) {
    mesh::Addr4 r;
    memcpy(r.ip, &a.sin_addr, 4);
    r.port = ntohs(a.sin_port);
    return r;
}
sockaddr_in from_addr4(const mesh::Addr4 &a) {
    sockaddr_in s;
    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    memcpy(&s.sin_addr, a.ip, 4);
    s.sin_port = htons(a.port);
    return s;
}
void addr_text(const mesh::Addr4 &a, char *out) {
    wsprintfA(out, "%u.%u.%u.%u:%u", (unsigned)a.ip[0], (unsigned)a.ip[1], (unsigned)a.ip[2], (unsigned)a.ip[3],
              (unsigned)a.port);
}
// "1>3>2" -- the ranking as one token a log grep can compare across peers.
void rank_text(const uint8_t *rank, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o + 4 < cap; ++i) {
        if (i) out[o++] = '>';
        out[o++] = (char)('0' + (rank[i] & 7));
    }
    out[o] = '\0';
}
int rooms_in(const mesh::Epoch &e) {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if ((e.S >> i & 1u) && e.room[i] != 0) ++n;
    return n;
}
uint16_t clamp_dms(double v) {
    if (v < 1.0) return 1;
    if (v > 65534.0) return 65534;
    return (uint16_t)(v + 0.5);
}

} // namespace

uint8_t Endpoint::cap_byte() const {
    const LONG v = m_test_cap;
    return v ? (uint8_t)(v - 1) : osq::CAP_VERSION;
}

void Endpoint::mesh_send(int idx, const uint8_t *payload, int len, int dst) {
    send_frame(idx, FLAG_MESH, (int16_t)m_my_id, (int16_t)dst, payload, len);
    ++m_mesh.frames_tx;
    m_mesh.frame_bytes_tx += (long)(WIRE_HDR_SIZE + len);
}

void Endpoint::mesh_install_key(const uint8_t key[KEY_LEN]) {
    Mesh &m = m_mesh;
    if (m.key_valid && memcmp(m.key, key, KEY_LEN) == 0) return;
    const bool first = !m.key_valid;
    memcpy(m.key, key, KEY_LEN);
    m.key_valid = true;
    mesh::probe_keys(m.key, m_my_id, m.me);
    for (int i = 0; i < 8; ++i) {
        mesh::probe_keys(m.key, i, m.peer[i].keys);
        m.peer[i].rx_win.reset();
    }
    // The sequence IS the ChaCha20 nonce. A peer that restarts under the SAME key (a lobby relink) must not
    // reuse one and must not start below what the others' replay windows have seen: wall-clock ms << 10 plus
    // a counter is above any earlier life's last value (a life sends far fewer than 1024 datagrams a ms).
    if (first || m.tx_seq == 0) m.tx_seq = unix_ms_m() << 10;
}

void Endpoint::mesh_send_probe(int peer, const sockaddr_in &to, uint8_t kind, uint32_t nonce) {
    Mesh   &m = m_mesh;
    if (m_cfg.mesh_test_delay_ms > 0 && !m.flushing) { // mp:U64: a slow peer, on purpose (Config::mesh_test_delay_ms)
        if (m.defer_n < 64) {
            Mesh::Deferred &d = m.defer[m.defer_n++];
            d.due   = GetTickCount() + (DWORD)m_cfg.mesh_test_delay_ms;
            d.peer  = peer;
            d.to    = to;
            d.kind  = kind;
            d.nonce = nonce;
        }
        return;
    }
    uint8_t body[mesh::PROBE_BODY];
    mesh::probe_body(kind, m_my_id, peer, nonce, 0, body);
    if (send_sealed(to, U::PKT_DATA, m.me.cid, m.tx_seq++, m.me.enc, m.me.mac, body, sizeof(body))) {
        const long bytes = (long)(U::HDR_SIZE + sizeof(body) + U::TAG_SIZE);
        m.probe_bytes_tx += bytes;
        if (kind == mesh::PR_PROBE) ++m.probe_tx;
        else ++m.echo_tx;
    }
}

// ---- the datagram path ------------------------------------------------------------------------------
bool Endpoint::mesh_on_datagram(uint8_t *pkt, int len, const sockaddr_in &from, DWORD now) {
    U::Header peek;
    if (!U::hdr_peek(pkt, (size_t)len, peek) || peek.type != U::PKT_DATA) return false;
    EnterCriticalSection(&m_conn_cs);
    Mesh &m = m_mesh;
    int   s = -1;
    if (m.on && m.key_valid)
        for (int i = 0; i < 8; ++i)
            if (i != m_my_id && memcmp(m.peer[i].keys.cid, peek.conn_id, 8) == 0) {
                s = i;
                break;
            }
    if (s < 0) {
        LeaveCriticalSection(&m_conn_cs);
        return false;
    }
    MeshPeer  &q = m.peer[s];
    U::Header  h;
    size_t     blen = 0;
    const auto v    = U::packet_decode(pkt, (size_t)len, q.keys.cid, q.keys.enc, q.keys.mac, &q.rx_win, h, &blen);
    uint8_t    kind = 0, flags = 0;
    int        from_id = -1, to_id = -1;
    uint32_t   nonce = 0;
    // mp:U63 -- a FAILOVER datagram (13-byte body) rides the same sealed channel as a probe (8 bytes).
    uint32_t fo_epoch = 0;
    uint8_t  fo_arg   = 0;
    const bool is_fo  = v == U::Verdict::Ok && blen == (size_t)mesh::FO_BODY &&
                       mesh::fo_body_decode(pkt + U::HDR_SIZE, (int)blen, kind, from_id, to_id, nonce, flags, fo_epoch, fo_arg);
    if (v != U::Verdict::Ok ||
        (!is_fo && !mesh::probe_body_decode(pkt + U::HDR_SIZE, (int)blen, kind, from_id, to_id, nonce, flags)) ||
        from_id != s || to_id != m_my_id) {
        ++m.bad_probe;
        LeaveCriticalSection(&m_conn_cs);
        return true; // it carried a mesh connection id: it was ours to refuse, not the handshake's
    }
    m.probe_bytes_rx += len;
    if (is_fo) {
        q.heard_ms = now ? now : 1u; // any sealed datagram from the peer is a sign of life
        fo_on_datagram(s, kind, flags, fo_epoch, fo_arg, from, now);
        LeaveCriticalSection(&m_conn_cs);
        return true;
    }
    if (kind == mesh::PR_PROBE) {
        ++m.probe_rx;
        q.heard_ms = now ? now : 1u;
        if (!m_test_noprobe) mesh_send_probe(s, from, mesh::PR_ECHO, nonce);
    } else {
        ++m.echo_rx;
        int64_t sent = 0;
        for (int i = 0; i < 8; ++i)
            if (q.ring_nonce[i] == nonce && q.ring_qpc[i] != 0) {
                sent             = q.ring_qpc[i];
                q.ring_qpc[i]    = 0; // an echo answers one probe
                break;
            }
        if (sent != 0) {
            const double dms = (double)(qpc_now_m() - sent) * 10000.0 / (double)qpc_freq_m();
            q.srtt_dms       = q.samples == 0 ? dms : q.srtt_dms + (dms - q.srtt_dms) / 4.0;
            ++q.samples;
            q.last_echo_ms = now ? now : 1u;
            q.best         = from;
            q.have_best    = true;
            q.best_ms      = q.last_echo_ms;
            if (m.first_pair_ms[s] == 0) {
                m.first_pair_ms[s] = (DWORD)(now - m.broker_ms) | 1u;
                char at[40];
                addr_text(to_addr4(from), at);
                logf("net: mesh pair %d-%d first echo %u ms after brokering, rtt %u dms (%s)", m_my_id, s,
                     (unsigned)m.first_pair_ms[s], (unsigned)clamp_dms(dms), at);
            }
        }
    }
    LeaveCriticalSection(&m_conn_cs);
    return true;
}

// ---- the frame path -----------------------------------------------------------------------------------
void Endpoint::mesh_on_frame(int idx, const WireHdr &h, const uint8_t *payload, uint32_t len) {
    (void)h;
    Mesh &m = m_mesh;
    // A rehomed endpoint runs no mesh -- unless the HANDOVER (mp:U62) rehomed it: then the new hub
    // brokers/publishes and the re-dialled clients report, exactly as before the hub changed.
    if (!m.on || len < 1 || (m_rehomed && !m_hl.mesh_resume)) return;
    ++m.frames_rx;
    m.frame_bytes_rx += (long)(WIRE_HDR_SIZE + len);
    Conn       &c   = m_conns[idx];
    const DWORD now = GetTickCount();
    switch (payload[0]) {
        case mesh::MK_CAND: {
            if (m_role != 0 || c.player_id < 0 || c.player_id >= 8) return;
            mesh::CandList cl;
            bool           rel = false;
            if (!mesh::cand_frame_decode(payload, (int)len, rel, cl)) {
                ++m.bad_frame;
                return;
            }
            m.reported[c.player_id]         = cl;
            m.reported_relayed[c.player_id] = rel;
            m.have_cand[c.player_id]        = true;
            m.broker_dirty                  = true;
            if (m.first_cand_ms == 0) m.first_cand_ms = now ? now : 1u;
            return;
        }
        case mesh::MK_ROW: {
            if (m_role != 0 || c.player_id < 0 || c.player_id >= 8) return;
            mesh::Row r;
            if (!mesh::row_decode(payload, (int)len, r)) {
                ++m.bad_frame;
                return;
            }
            const int id = c.player_id;
            for (int j = 0; j < 8; ++j) m.M.rtt[id][j] = mesh::NONE;
            for (int i = 0; i < r.n; ++i) m.M.rtt[id][r.peer[i]] = r.rtt[i];
            m.M.leg[id]            = r.leg;
            m.reported_relayed[id] = r.relayed;
            m.row_ms[id]           = now ? now : 1u;
            m.have_row[id]         = true;
            return;
        }
        case mesh::MK_ACK: {
            if (m_role != 0 || c.player_id < 0 || c.player_id >= 8) return;
            uint32_t e = 0, d = 0;
            if (!mesh::ack_decode(payload, (int)len, e, d)) {
                ++m.bad_frame;
                return;
            }
            if (e != m.cur.epoch || d != m.cur.digest) return; // a stale ack, or one for another matrix
            const int id = c.player_id;
            ++m.ack_rx;
            m.acked_mask |= 1u << id;
            m.last_acked_epoch[id]  = e;
            m.last_acked_digest[id] = d;
            const uint32_t req      = (uint32_t)m.cur.S & m.S;
            if ((m.acked_mask & req) == req && m.all_acked_ms == 0) {
                m.all_acked_ms = (DWORD)(now - m.pub_ms) | 1u;
                logf("net: mesh epoch=%u acked by all %d client(s) after %u ms", e, mesh::popcnt(req),
                     (unsigned)m.all_acked_ms);
            }
            return;
        }
        case mesh::MK_BROKER: {
            if (m_role != 1 || idx != 0) return;
            mesh::Broker b;
            if (!mesh::broker_decode(payload, (int)len, b)) {
                ++m.bad_frame;
                return;
            }
            mesh_install_key(b.key);
            bool seen[8] = {false};
            for (int i = 0; i < b.n; ++i) {
                const int id = b.e[i].id;
                if (id == m_my_id) continue;
                MeshPeer &q  = m.peer[id];
                const bool changed = !q.known || q.relayed != b.e[i].relayed || q.cands.n != b.e[i].cands.n ||
                                     memcmp(q.cands.a, b.e[i].cands.a, sizeof(q.cands.a[0]) * q.cands.n) != 0;
                if (changed) {
                    q.have_best      = false;
                    q.first_probe_ms = now ? now : 1u;
                    q.last_probe_ms  = 0;
                }
                q.known   = true;
                q.relayed = b.e[i].relayed;
                q.cands   = b.e[i].cands;
                seen[id]  = true;
            }
            for (int id = 0; id < 8; ++id)
                if (!seen[id] && m.peer[id].known) {
                    m.peer[id].known = false; // that client left
                    m.first_pair_ms[id] = 0;
                }
            if (m.broker_ms == 0) m.broker_ms = now ? now : 1u;
            logf("net: mesh BROKER from the hub -- %d other client(s), key installed", b.n);
            return;
        }
        case mesh::MK_EPOCH: {
            if (m_role != 1 || idx != 0) return;
            mesh::Epoch e;
            if (!mesh::epoch_decode(payload, (int)len, e) || mesh::digest(e.M, e.S) != e.digest) {
                ++m.bad_frame;
                return;
            }
            // Every peer recomputes the ranking from the matrix it was handed. elect() is a pure function
            // of (matrix, S), so a disagreement is a bug or a corrupted frame, never a legitimate difference.
            mesh::Election el;
            mesh::elect(e.M, e.S, el);
            bool agree = el.n == e.n && (uint8_t)el.tier == e.tier;
            for (int i = 0; agree && i < el.n; ++i) agree = el.rank[i] == e.rank[i];
            if (!agree) {
                ++m.rank_mismatch;
                logf("net: mesh epoch=%u RANKING MISMATCH -- the published ranking is not elect() of the published "
                     "matrix (digest %08x)",
                     (unsigned)e.epoch, (unsigned)e.digest);
            }
            const bool fresh = !m.held_valid || e.epoch > m.held.epoch;
            if (fresh) {
                m.held       = e;
                m.held_valid = true;
                ++m.epochs_held;
                int have = 0, total = 0;
                mesh::coverage(e.M, e.S, &have, &total);
                char rk[24];
                rank_text(e.rank, e.n, rk, sizeof(rk));
                logf("net: mesh epoch=%u hub=%d tier=%d rank=%s digest=%08x cov=%d/%d rooms=%d role=client",
                     (unsigned)e.epoch, (int)e.hub, (int)e.tier, rk, (unsigned)e.digest, have, total, rooms_in(e));
            }
            if (!m_test_noack) {
                uint8_t ack[mesh::ACK_BYTES];
                mesh::ack_encode(e.epoch, e.digest, ack);
                mesh_send(0, ack, (int)sizeof(ack), BROADCAST);
            }
            return;
        }
        case mesh::MK_LEAVING: {
            if (m_role != 1 || idx != 0) return; // only a client, only from its hub
            mesh::Leaving L;
            if (!mesh::leaving_decode(payload, (int)len, L)) {
                ++m.bad_frame;
                return;
            }
            hl_on_leaving(idx, L);
            return;
        }
        case mesh::MK_LEAVE_ACK: {
            if (m_role != 0) return;
            hl_on_ack(idx, payload, len);
            return;
        }
        default: return; // a kind from a newer build
    }
}

// ---- the host ------------------------------------------------------------------------------------------
void Endpoint::mesh_publish(DWORD now, uint32_t S, const mesh::Matrix &snap, const mesh::Election &el) {
    Mesh        &m = m_mesh;
    mesh::Epoch  e;
    e.epoch  = m.cur.epoch + 1;
    e.hub    = (uint8_t)m_my_id;
    e.S      = (uint8_t)S;
    e.tier   = el.tier;
    e.n      = el.n;
    e.M      = snap;
    for (int i = 0; i < el.n; ++i) e.rank[i] = el.rank[i];
    e.digest = mesh::digest(snap, S);
    for (int id = 0; id < 8; ++id) {
        if (!(S >> id & 1u)) continue;
        const int ci = conn_of_player(id);
        const bool relayed = m.reported_relayed[id] || (m_leg_overhead > 0 && ci >= 0 && is_loopback(m_conns[ci].addr));
        if (!relayed) {
            if (ci >= 0) e.dial[id].add(to_addr4(m_conns[ci].addr));
            for (int k = 0; k < m.reported[id].n; ++k) e.dial[id].add(m.reported[id].a[k]);
        }
        // A relayed match: a FRESH room per candidate, minted now, that this candidate would register as the
        // room's host if it ever became the hub (plan section 6.2). A direct match has none.
        // mp:U64 -- STABLE across this hub's epochs: a candidate keeps the room it was given. The hub publishes e+1 only after
        // every ack of e, but may die inside that round, leaving survivors at e and others at e+1 (U61 risk 3); with a fresh
        // room per epoch they would dial DIFFERENT rooms for the same candidate and one of them would elect itself instead.
        // (An inherited epoch's rooms belong to the previous hub's life and are not reused.)
        if (snap.relay_ok && m_mesh_hooks.mint_room) {
            const uint32_t prev = (m.cur.epoch != 0 && !m_hl.epoch_inherited && id < 8) ? m.cur.room[id] : 0;
            e.room[id]          = prev != 0 ? prev : m_mesh_hooks.mint_room(m_mesh_hooks.ctx);
        }
    }
    uint8_t buf[mesh::MAX_FRAME];
    const int n = mesh::epoch_encode(e, buf, (int)sizeof(buf));
    if (n == 0) {
        logf("net: mesh epoch=%u NOT published -- the frame does not fit", (unsigned)e.epoch);
        return;
    }
    m.cur        = e;
    m_hl.epoch_inherited = false; // mp:U62 -- this hub has published its own epoch now
    m.acked_mask = 0;
    m.pub_ms     = now ? now : 1u;
    m.last_send_ms = m.pub_ms;
    m.all_acked_ms = 0;
    if (m.first_epoch_ms == 0 && m.first_cand_ms != 0) m.first_epoch_ms = (DWORD)(now - m.first_cand_ms) | 1u;
    ++m.epochs_published;
    for (int id = 0; id < 8; ++id) {
        if (!(S >> id & 1u)) continue;
        const int ci = conn_of_player(id);
        if (ci >= 0) mesh_send(ci, buf, n, id);
    }
    int have = 0, total = 0;
    mesh::coverage(snap, S, &have, &total);
    char rk[24];
    rank_text(e.rank, e.n, rk, sizeof(rk));
    logf("net: mesh epoch=%u hub=%d tier=%d rank=%s digest=%08x cov=%d/%d rooms=%d role=hub", (unsigned)e.epoch,
         (int)e.hub, (int)e.tier, rk, (unsigned)e.digest, have, total, rooms_in(e));
    // The matrix in one line, so a log can say which pair measured what: "a-b:dms" for every pair of S.
    char line[420];
    int  o = 0;
    for (int i = 0; i < 8; ++i)
        for (int j = i + 1; j < 8; ++j)
            if ((S >> i & 1u) && (S >> j & 1u) && o < (int)sizeof(line) - 24) {
                const uint16_t d = mesh::pair_rtt(snap, i, j);
                o += wsprintfA(line + o, " %d-%d:%d%s", i, j, d == mesh::NONE ? -1 : (int)d,
                               mesh::edge(snap, i, j) == mesh::NONE && d != mesh::NONE ? "r" : "");
            }
    // ...and the relay legs, when there are any ("legs id:dms").
    o += wsprintfA(line + o, " | legs");
    for (int i = 0; i < 8; ++i)
        if ((S >> i & 1u) && snap.leg[i] != mesh::NONE && o < (int)sizeof(line) - 16)
            o += wsprintfA(line + o, " %d:%d", i, (int)snap.leg[i]);
    line[o] = '\0';
    logf("net: mesh epoch=%u matrix (dms, r = relay-estimated):%s", (unsigned)e.epoch, line);
}

void Endpoint::mesh_host_tick(DWORD now) {
    Mesh    &m = m_mesh;
    uint32_t S = 0;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active && !m_conns[i].tx_dead && m_conns[i].player_id >= 0 && m_conns[i].player_id < 8 &&
            m_conns[i].player_id != m_my_id)
            S |= 1u << m_conns[i].player_id;
    if (S != m.S) {
        const uint32_t gone = m.S & ~S;
        for (int id = 0; id < 8; ++id)
            if (gone >> id & 1u) {
                m.have_cand[id] = false;
                m.have_row[id]  = false;
                m.reported[id]  = mesh::CandList();
                m.reported_relayed[id] = false;
                for (int j = 0; j < 8; ++j) {
                    m.M.rtt[id][j] = mesh::NONE;
                    m.M.rtt[j][id] = mesh::NONE;
                }
                m.M.leg[id] = mesh::NONE;
            }
        m.S            = S;
        m.roster_ms    = now ? now : 1u;
        m.broker_dirty = true;
        m.acked_mask &= S;
    }
    if (mesh::popcnt(S) < 2) return; // a 2-peer match has no survivors to elect among

    // ---- brokering ---------------------------------------------------------------------------------
    uint32_t have_mask = 0;
    for (int id = 0; id < 8; ++id)
        if (m.have_cand[id]) have_mask |= 1u << id;
    if (m.broker_dirty && (DWORD)(now - m.last_broker_ms) >= mesh_t(BROKER_MIN_MS) &&
        ((S & ~have_mask) == 0 || (DWORD)(now - m.roster_ms) >= mesh_t(1500))) {
        if (!m.key_valid) {
            uint8_t k[KEY_LEN];
            if (!MH_Key_Random(k, (unsigned)KEY_LEN)) {
                logf("net: mesh -- no secure randomness for the mesh key; no brokering");
                m.broker_dirty = false;
                return;
            }
            mesh_install_key(k);
        }
        for (int x = 0; x < 8; ++x) {
            if (!(S >> x & 1u)) continue;
            const int xi = conn_of_player(x);
            if (xi < 0) continue;
            mesh::Broker b;
            memset(&b, 0, sizeof(b));
            memcpy(b.key, m.key, KEY_LEN);
            for (int y = 0; y < 8; ++y) {
                if (y == x || !(S >> y & 1u)) continue;
                const int yi = conn_of_player(y);
                mesh::BrokerEntry &e = b.e[b.n++];
                e.id      = (uint8_t)y;
                e.relayed = m.reported_relayed[y] || (m_leg_overhead > 0 && yi >= 0 && is_loopback(m_conns[yi].addr));
                if (!e.relayed) {
                    if (yi >= 0) e.cands.add(to_addr4(m_conns[yi].addr)); // what the HOST observed: the public mapping
                    for (int k = 0; k < m.reported[y].n; ++k) e.cands.add(m.reported[y].a[k]);
                }
            }
            uint8_t buf[mesh::MAX_FRAME];
            const int n = mesh::broker_encode(b, buf, (int)sizeof(buf));
            if (n) mesh_send(xi, buf, n, x);
        }
        m.last_broker_ms = now ? now : 1u;
        m.broker_dirty   = false;
        logf("net: mesh BROKER sent to %d client(s)", mesh::popcnt(S));
    }
    if (!m.key_valid) return;

    // ---- the matrix snapshot ----------------------------------------------------------------------------
    mesh::Matrix snap = m.M;
    snap.relay_ok     = m_leg_overhead > 0 ? 1 : 0;
    for (int i = 0; i < 8; ++i) {
        const bool stale = !(S >> i & 1u) || !m.have_row[i] || (DWORD)(now - m.row_ms[i]) > mesh_t(ROW_STALE_MS);
        for (int j = 0; j < 8; ++j)
            if (stale || !(S >> j & 1u)) snap.rtt[i][j] = mesh::NONE;
        if (stale) snap.leg[i] = mesh::NONE;
    }

    // ---- the epoch ---------------------------------------------------------------------------------------
    const uint32_t req         = (uint32_t)m.cur.S & S;
    const bool     outstanding = m.cur.epoch != 0 && (m.acked_mask & req) != req;
    if (outstanding) {
        if ((DWORD)(now - m.last_send_ms) >= mesh_t(EPOCH_RESEND_MS)) {
            uint8_t buf[mesh::MAX_FRAME];
            const int n = mesh::epoch_encode(m.cur, buf, (int)sizeof(buf));
            for (int id = 0; id < 8 && n; ++id)
                if ((req >> id & 1u) && !(m.acked_mask >> id & 1u)) {
                    const int ci = conn_of_player(id);
                    if (ci >= 0) {
                        mesh_send(ci, buf, n, id);
                        ++m.epoch_resends;
                    }
                }
            m.last_send_ms = now ? now : 1u;
        }
        return; // never e+1 before every ack of e
    }
    int have = 0, total = 0;
    mesh::coverage(snap, S, &have, &total);
    const bool complete = have == total;
    mesh::Election el;
    mesh::elect(snap, S, el);
    const bool settled = (DWORD)(now - m.roster_ms) >= mesh_t(EPOCH_SETTLE_MS);
    bool       publish = false;
    if (m.cur.epoch == 0 || (uint32_t)m.cur.S != S) {
        publish = settled && (complete || (DWORD)(now - m.roster_ms) >= mesh_t(EPOCH_DEADLINE_MS));
    } else {
        int ch = 0, ct = 0;
        mesh::coverage(m.cur.M, m.cur.S, &ch, &ct);
        if (ch < ct && complete && (DWORD)(now - m.pub_ms) >= mesh_t(5000)) {
            publish = true; // the first epoch went out on a partial matrix; now every pair is measured
        } else if ((DWORD)(now - m.last_eval_ms) >= mesh_t(mesh::REELECT_MS)) {
            m.last_eval_ms = now ? now : 1u;
            if ((DWORD)(now - m.pub_ms) >= mesh_t(mesh::MIN_PUBLISH_MS) && el.winner >= 0 && el.winner != (int)m.cur.rank[0]) {
                uint32_t cur_cost = 0xFFFFFFFFu;
                for (int i = 0; i < el.n; ++i)
                    if (el.rank[i] == m.cur.rank[0]) cur_cost = el.cost[i];
                publish = mesh::beats_incumbent(cur_cost, el.cost[0]);
            }
        }
    }
    if (publish) mesh_publish(now, S, snap, el);
}

// ---- a client ------------------------------------------------------------------------------------------
void Endpoint::mesh_client_tick(DWORD now) {
    Mesh &m = m_mesh;
    if (!m_conns[0].active || m_conns[0].tx_dead) return;
    if (InterlockedCompareExchange(&m_id_assigned, 0, 0) == 0 || m_my_id < 0 || m_my_id >= 8) return;
    const bool relayed = m_leg_overhead > 0 && is_loopback(m_conns[0].addr);

    if (!m.cand_sent) {
        mesh::CandList cl;
        if (!relayed && m_sock != INVALID_SOCKET) {
            uint8_t buf[4096];
            DWORD   got = 0;
            const unsigned short port = bound_port();
            if (port != 0 && WSAIoctl(m_sock, SIO_ADDRESS_LIST_QUERY, nullptr, 0, buf, (DWORD)sizeof(buf), &got,
                                      nullptr, nullptr) == 0) {
                const SOCKET_ADDRESS_LIST *l = (const SOCKET_ADDRESS_LIST *)buf;
                for (int i = 0; i < l->iAddressCount; ++i) {
                    const sockaddr_in *a = (const sockaddr_in *)l->Address[i].lpSockaddr;
                    if (a == nullptr || a->sin_family != AF_INET) continue;
                    const unsigned long ip = ntohl(a->sin_addr.s_addr);
                    if ((ip >> 24) == 127 || ip == 0) continue; // loopback is never a candidate (the punch rule)
                    sockaddr_in s = *a;
                    s.sin_port    = htons(port);
                    cl.add(to_addr4(s));
                }
            }
        }
        uint8_t   b[64];
        const int n = mesh::cand_frame_encode(relayed, cl, b, (int)sizeof(b));
        if (n) mesh_send(0, b, n, BROADCAST);
        m.cand_sent = true;
        logf("net: mesh CAND sent -- %d local candidate(s)%s", (int)cl.n, relayed ? ", relayed (no direct probing)" : "");
    }
    if (m.broker_ms == 0) return; // no BROKER yet: nothing to probe, nothing to report

    // ---- probes -------------------------------------------------------------------------------------------
    if (!relayed && !m_test_noprobe) {
        for (int p = 0; p < 8; ++p) {
            MeshPeer &q = m.peer[p];
            if (!q.known || q.relayed || p == m_my_id || q.cands.n == 0) continue;
            const bool warm = q.first_probe_ms != 0 && (DWORD)(now - q.first_probe_ms) < mesh_t(WARM_MS);
            const bool heard = q.heard_ms != 0 && (DWORD)(now - q.heard_ms) < mesh_t(HEARD_FRESH_MS);
            // Steady state: the lower id probes. The higher id echoes (which keeps its own pinhole warm) and
            // probes too only while it hears nothing from the lower (the path may be one-way broken).
            if (!(warm || m_my_id < p || !heard)) continue;
            const DWORD every = warm ? mesh_t(WARM_FAST_MS) : mesh_t(STEADY_MS);
            if (q.last_probe_ms != 0 && (DWORD)(now - q.last_probe_ms) < every) continue;
            q.last_probe_ms = now ? now : 1u;
            const uint32_t nonce = ++q.nonce;
            q.ring_nonce[q.ring_at & 7] = nonce;
            q.ring_qpc[q.ring_at & 7]   = qpc_now_m();
            ++q.ring_at;
            if (q.have_best && (DWORD)(now - q.best_ms) < mesh_t(BEST_FRESH_MS)) {
                mesh_send_probe(p, q.best, mesh::PR_PROBE, nonce);
            } else {
                for (int k = 0; k < q.cands.n; ++k) {
                    const sockaddr_in to = from_addr4(q.cands.a[k]);
                    mesh_send_probe(p, to, mesh::PR_PROBE, nonce);
                }
            }
        }
    }

    // ---- the row ------------------------------------------------------------------------------------------
    const DWORD row_every = (DWORD)(now - m.broker_ms) < mesh_t(10000) ? mesh_t(ROW_WARM_MS) : mesh_t(ROW_STEADY_MS);
    if (m.last_row_ms == 0 || (DWORD)(now - m.last_row_ms) >= row_every) {
        m.last_row_ms = now ? now : 1u;
        mesh::Row r;
        memset(&r, 0, sizeof(r));
        for (int p = 0; p < 8; ++p) {
            const MeshPeer &q = m.peer[p];
            if (!q.known || p == m_my_id) continue;
            const bool fresh = q.samples > 0 && (DWORD)(now - q.last_echo_ms) < mesh_t(ECHO_FRESH_MS);
            r.peer[r.n]      = (uint8_t)p;
            r.rtt[r.n]       = fresh ? clamp_dms(q.srtt_dms) : mesh::NONE;
            ++r.n;
        }
        r.leg     = mesh::NONE;
        r.relayed = relayed;
        if (m_mesh_hooks.leg_rtt_dms) {
            const int leg = m_mesh_hooks.leg_rtt_dms(m_mesh_hooks.ctx);
            if (leg >= 0) r.leg = clamp_dms((double)leg);
        }
        uint8_t   b[mesh::MAX_FRAME];
        const int n = mesh::row_encode(r, b, (int)sizeof(b));
        if (n) mesh_send(0, b, n, BROADCAST);
    }
}

void Endpoint::mesh_tick(DWORD now) {
    Mesh &m = m_mesh;
    if (m.defer_n > 0) { // mp:U64: release the held probes/echoes whose time has come
        m.flushing = true;
        int keep   = 0;
        for (int i = 0; i < m.defer_n; ++i) {
            if ((int32_t)(now - m.defer[i].due) >= 0) mesh_send_probe(m.defer[i].peer, m.defer[i].to, m.defer[i].kind, m.defer[i].nonce);
            else m.defer[keep++] = m.defer[i];
        }
        m.defer_n  = keep;
        m.flushing = false;
    }
    if (!m.on || (m_rehomed && !m_hl.mesh_resume)) return; // a rehomed endpoint runs no mesh unless the handover resumed it
    if (m_hl.leaving) {
        hl_leave_tick(now); // mp:U62 -- a hub that is handing over is quiet: no brokering, no new epoch
        return;
    }
    if (m_role == 0) mesh_host_tick(now);
    else mesh_client_tick(now);
    if ((DWORD)(now - m.last_stat_log_ms) >= STAT_LOG_MS) {
        m.last_stat_log_ms = now ? now : 1u;
        const long moved   = m.probe_tx + m.echo_tx + m.probe_rx + m.echo_rx + m.frames_tx + m.frames_rx;
        if (moved != m.last_stat_moved) {
            m.last_stat_moved = moved;
            logf("net: mesh counters probes tx %ld rx %ld | echoes tx %ld rx %ld | probe bytes tx %ld rx %ld | "
                 "frames tx %ld rx %ld (bytes tx %ld rx %ld) | bad probe %ld frame %ld | epochs published %ld held %ld",
                 m.probe_tx, m.probe_rx, m.echo_tx, m.echo_rx, m.probe_bytes_tx, m.probe_bytes_rx, m.frames_tx,
                 m.frames_rx, m.frame_bytes_tx, m.frame_bytes_rx, m.bad_probe, m.bad_frame, m.epochs_published,
                 m.epochs_held);
        }
    }
}

// ---- status -------------------------------------------------------------------------------------------
void Endpoint::mesh_status(MeshStatus &o) {
    memset(&o, 0, sizeof(o));
    o.M.clear();
    if (!m_cs_ready) return;
    EnterCriticalSection(&m_conn_cs);
    const Mesh &m = m_mesh;
    o.role        = (int)m_role;
    o.key_valid   = m.key_valid;
    const mesh::Epoch *e = nullptr;
    if (m_role == 0 && m.cur.epoch != 0) e = &m.cur;
    if (m_role == 1 && m.held_valid) e = &m.held;
    if (e) {
        o.epoch  = e->epoch;
        o.digest = e->digest;
        o.hub    = e->hub;
        o.S      = e->S;
        o.tier   = e->tier;
        o.rank_n = e->n;
        for (int i = 0; i < 8; ++i) o.rank[i] = e->rank[i];
        o.M = e->M;
        for (int i = 0; i < 8; ++i) {
            o.dial[i] = e->dial[i];
            o.room[i] = e->room[i];
        }
    } else if (m_role == 0) {
        o.M = m.M;
    }
    o.acked_mask = m.acked_mask;
    for (int i = 0; i < 8; ++i) {
        o.last_acked_epoch[i]  = m.last_acked_epoch[i];
        o.last_acked_digest[i] = m.last_acked_digest[i];
        o.first_pair_ms[i]     = m.first_pair_ms[i];
    }
    if (m_role == 0) {
        mesh::Matrix snap = m.M;
        snap.relay_ok     = m_leg_overhead > 0 ? 1 : 0;
        mesh::coverage(snap, m.S, &o.cov_have, &o.cov_total);
    } else if (e) {
        mesh::coverage(e->M, e->S, &o.cov_have, &o.cov_total);
    }
    o.probe_tx = m.probe_tx;
    o.probe_rx = m.probe_rx;
    o.echo_tx = m.echo_tx;
    o.echo_rx = m.echo_rx;
    o.probe_bytes_tx = m.probe_bytes_tx;
    o.probe_bytes_rx = m.probe_bytes_rx;
    o.frames_tx = m.frames_tx;
    o.frames_rx = m.frames_rx;
    o.frame_bytes_tx = m.frame_bytes_tx;
    o.frame_bytes_rx = m.frame_bytes_rx;
    o.bad_probe = m.bad_probe;
    o.bad_frame = m.bad_frame;
    o.rank_mismatch = m.rank_mismatch;
    o.epochs_published = m.epochs_published;
    o.epochs_held = m.epochs_held;
    o.ack_rx = m.ack_rx;
    o.epoch_resends = m.epoch_resends;
    o.all_acked_ms = m.all_acked_ms;
    o.first_epoch_ms = m.first_epoch_ms;
    LeaveCriticalSection(&m_conn_cs);
}

bool Endpoint::mesh_held_epoch(mesh::Epoch &out) {
    if (!m_cs_ready) return false;
    EnterCriticalSection(&m_conn_cs);
    const bool ok = m_mesh.held_valid;
    if (ok) out = m_mesh.held;
    LeaveCriticalSection(&m_conn_cs);
    return ok;
}

// =================================================================================================
// mp:U62 (HM-M4) -- THE PLANNED HANDOVER OF THE HUB (plan section 5.4)
//
//   THE LEAVING HUB   hub_leave(): MK_LEAVING to every client (the survivors in successor-preference order, each
//                     with its dial info), keep forwarding, wait for every MK_LEAVE_ACK, then retire the
//                     connections quietly. The first successor not acknowledging within SUCC_ACK_MS gets the
//                     list re-issued without it (generation + 1).
//   A SURVIVOR        hl_on_leaving (recv thread, under the lock): acknowledge, queue. hl_service (timer thread,
//                     NO lock): the first listed id rehomes AS THE HUB, every other id re-dials it with its OLD
//                     id (Endpoint::rehome, M2), the reconcile (M1) runs, and the mesh resumes.
//
// WHY THE ACKNOWLEDGEMENT GOES OUT BEFORE THE REHOME AND ON THE OLD CONN. rehome() retires the old hub
// connection in place, and an acknowledgement sent after that has nowhere to go; stream_write puts it on the
// wire inside this call, so by the time the conn is retired it has been sent.
//
// WHY hl_service IS A SEPARATE, UNLOCKED STEP. In a relayed match the tunnel's role switch (udprelay::rehome)
// joins the tunnel's pump thread, and that thread asks Endpoint::knows_addr -- under m_conn_cs -- once a
// second. Calling the switch from the recv thread (which holds the lock) is a 2 s wait that ends in a refusal.
//
// WHAT THIS DOES NOT DO: the leaving player is NOT removed from any roster here. That is the game's own
// pinned flip (U19b's park step for a quit, the pinned elimination for a defeat), which has already run on
// every peer before the game calls hub_leave(); this layer moves only the transport hub.
// =================================================================================================
namespace {
// "1>3>2"-style list for the log (same shape as the epoch's ranking token).
void list_text(const mesh::Leaving &L, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < L.n && o + 4 < cap; ++i) {
        if (i) out[o++] = '>';
        out[o++] = (char)('0' + (L.e[i].id & 7));
    }
    out[o] = '\0';
}
} // namespace

// The survivors in preference order, with dial info. Caller holds m_conn_cs.
void Endpoint::hl_build_leaving(uint32_t S, mesh::Leaving &L, DWORD now) {
    Mesh &m = m_mesh;
    L       = mesh::Leaving();
    L.hub   = (uint8_t)m_my_id;
    L.gen   = 1;
    const bool have_epoch = m.cur.epoch != 0;
    L.epoch               = have_epoch ? m.cur.epoch : 0;
    // The order: the published epoch's ranking restricted to who is still here (the survivors all hold that
    // epoch, so it is the order they already expect), then anyone it did not cover, ascending. With no epoch
    // yet, elect() over the matrix as it stands (a hub that has measured nothing answers the lowest id).
    uint8_t order[mesh::MAXP];
    int     n = 0;
    uint32_t placed = 0;
    if (have_epoch) {
        for (int i = 0; i < m.cur.n; ++i) {
            const int id = m.cur.rank[i];
            if ((S >> id & 1u) && !(placed >> id & 1u)) {
                order[n++] = (uint8_t)id;
                placed |= 1u << id;
            }
        }
    } else {
        mesh::Matrix snap = m.M;
        snap.relay_ok     = m_leg_overhead > 0 ? 1 : 0;
        mesh::Election el;
        mesh::elect(snap, S, el);
        for (int i = 0; i < el.n; ++i) {
            order[n++] = el.rank[i];
            placed |= 1u << el.rank[i];
        }
    }
    for (int id = 0; id < mesh::MAXP; ++id)
        if ((S >> id & 1u) && !(placed >> id & 1u)) order[n++] = (uint8_t)id;
    for (int i = 0; i < n; ++i) {
        const int  id  = order[i];
        const int  ci  = conn_of_player(id);
        mesh::LeaveEntry &e = L.e[L.n++];
        e.id      = (uint8_t)id;
        e.relayed = m.reported_relayed[id] || (m_leg_overhead > 0 && ci >= 0 && is_loopback(m_conns[ci].addr));
        if (!e.relayed) {
            if (ci >= 0) e.cands.add(to_addr4(m_conns[ci].addr)); // what the HUB observed: the public mapping
            for (int k = 0; k < m.reported[id].n; ++k) e.cands.add(m.reported[id].a[k]);
        }
        e.room = 0;
        if (m_leg_overhead > 0) {
            if (have_epoch && (m.cur.S >> id & 1u) && m.cur.room[id] != 0) e.room = m.cur.room[id];
            else if (m_mesh_hooks.mint_room) e.room = m_mesh_hooks.mint_room(m_mesh_hooks.ctx);
        }
    }
    (void)now;
}

int Endpoint::hub_leave(int timeout_ms) {
    if (!m_started || !m_cs_ready) return HL_NOTHING;
    const DWORD t0 = GetTickCount();
    EnterCriticalSection(&m_conn_cs);
    HubLeaveState &h = m_hl;
    if (m_cfg.no_hub_migration || m_role != 0 || h.leaving) {
        const char *why = m_cfg.no_hub_migration ? "[net] hub_migration=0" : (m_role != 0 ? "this endpoint is not the hub" : "a handover is already running");
        h.state         = HL_NOTHING;
        LeaveCriticalSection(&m_conn_cs);
        logf("net: hub-leave -- nothing to hand over (%s) (mp:U62)", why);
        return HL_NOTHING;
    }
    uint32_t S = 0;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active && !m_conns[i].tx_dead && m_conns[i].player_id >= 0 && m_conns[i].player_id < 8 &&
            m_conns[i].player_id != m_my_id)
            S |= 1u << m_conns[i].player_id;
    if (mesh::popcnt(S) < 2) {
        // One client left is not a match anyone can play on: it ends with the leaver (the 2-peer rule).
        h.state = HL_NOTHING;
        LeaveCriticalSection(&m_conn_cs);
        logf("net: hub-leave -- %d client(s) connected: fewer than two survivors, nothing to hand over (mp:U62)",
             mesh::popcnt(S));
        return HL_NOTHING;
    }
    mesh::Leaving L;
    hl_build_leaving(S, L, t0);
    uint8_t buf[mesh::MAX_FRAME];
    const int n = mesh::leaving_encode(L, buf, (int)sizeof(buf));
    if (n == 0) {
        h.state = HL_NOTHING;
        LeaveCriticalSection(&m_conn_cs);
        logf("net: hub-leave -- the LEAVING frame does not fit (mp:U62)");
        return HL_NOTHING;
    }
    h.leaving    = true;
    h.state      = HL_WAITING;
    h.want       = S;
    h.ack        = 0;
    h.removed    = 0;
    h.succ_acked = false;
    h.go_sent    = false;
    memset(h.ackc, 0, sizeof(h.ackc));
    h.started_ms = t0 ? t0 : 1u;
    h.sent_ms    = h.started_ms;
    h.cur        = L;
    for (int id = 0; id < 8; ++id) {
        if (!(S >> id & 1u)) continue;
        const int ci = conn_of_player(id);
        if (ci >= 0) mesh_send(ci, buf, n, id);
    }
    char lt[24];
    list_text(L, lt, sizeof(lt));
    logf("net: hub-leave -- player %d is LEAVING; HUB_LEAVING epoch=%u gen=%d successors=%s sent to %d client(s); "
         "forwarding until they acknowledge (mp:U62)",
         m_my_id, (unsigned)L.epoch, (int)L.gen, lt, mesh::popcnt(S));
    LeaveCriticalSection(&m_conn_cs);

    const int budget = timeout_ms > 0 ? timeout_ms : HL_TIMEOUT_DEFAULT;
    bool      done   = false;
    for (;;) {
        Sleep(2);
        EnterCriticalSection(&m_conn_cs);
        done = h.succ_acked && h.go_sent && (h.ack & h.want) == h.want;
        LeaveCriticalSection(&m_conn_cs);
        if (done || (int)(GetTickCount() - t0) >= budget) break;
    }
    EnterCriticalSection(&m_conn_cs);
    h.state = done ? HL_HANDED : HL_TIMEOUT;
    logf("net: hub-leave -- %s after %u ms: %d of %d client(s) acknowledged, successor %s (mp:U62)",
         done ? "HANDED OVER" : "TIMED OUT", (unsigned)(GetTickCount() - t0), mesh::popcnt(h.ack & h.want),
         mesh::popcnt(h.want), h.succ_acked ? "confirmed hub-up" : "NOT confirmed");
    hl_finish_leave(done ? "handed over" : "timed out");
    const int r = h.state;
    LeaveCriticalSection(&m_conn_cs);
    return r;
}

// Retire every connection quietly (a DISCONNECT each, so a client that never acknowledged learns promptly that
// the hub is gone) and forget the hub-side session state. Caller holds m_conn_cs.
void Endpoint::hl_finish_leave(const char *why) {
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active && !m_conns[i].tx_dead) refuse_conn(i, "the hub left the match (planned handover, mp:U62)");
    InterlockedExchange(&m_dead_peer, -1); // the departures above are OURS, not a peer dying
    m_hl.leaving = false;
    mesh_reset_hub_state(GetTickCount());
    logf("net: hub-leave -- connections retired (%s); this endpoint no longer carries the match (mp:U62)", why);
}

// A hub with no clients forgets its epoch/roster so a later lobby starts clean (a rematch on this endpoint must
// not find "outstanding" acks from a match whose clients are gone). Caller holds m_conn_cs.
void Endpoint::mesh_reset_hub_state(DWORD now) {
    Mesh &m = m_mesh;
    m.cur   = mesh::Epoch();
    m.acked_mask = 0;
    m.S          = 0;
    m.roster_ms  = now ? now : 1u;
    m.broker_dirty = false;
    m.last_broker_ms = 0;
    m.pub_ms = m.last_send_ms = m.last_eval_ms = m.all_acked_ms = m.first_epoch_ms = m.first_cand_ms = 0;
    for (int i = 0; i < 8; ++i) {
        m.have_cand[i] = m.have_row[i] = m.reported_relayed[i] = false;
        m.reported[i]  = mesh::CandList();
        m.row_ms[i]    = 0;
        m.last_acked_epoch[i] = m.last_acked_digest[i] = 0;
    }
    m.M.clear();
}

void Endpoint::hl_on_ack(int idx, const uint8_t *payload, uint32_t len) {
    HubLeaveState &h = m_hl;
    Conn          &c = m_conns[idx];
    uint32_t       epoch = 0;
    uint8_t        gen = 0, flags = 0;
    if (!mesh::leave_ack_decode(payload, (int)len, epoch, gen, flags)) {
        ++m_mesh.bad_frame;
        return;
    }
    if (!h.leaving || c.player_id < 0 || c.player_id >= 8 || !(h.want >> c.player_id & 1u)) return;
    if (gen != h.cur.gen) return; // an ack of a generation already superseded counts for nothing
    ++h.acks_rx;
    if (h.ackc[c.player_id] < 255) ++h.ackc[c.player_id];
    // `ack` = "heard the list AND (once the GO is out) the GO". The GO is the list repeated: a survivor only dials
    // the successor after it, because a survivor that dialled a successor which never came up could no longer hear
    // the hub re-issue the list without it (the retired connection).
    if (h.go_sent && h.ackc[c.player_id] >= 2) h.ack |= 1u << c.player_id;
    if ((flags & mesh::LA_SUCCESSOR) && h.cur.n > 0 && h.cur.e[0].id == c.player_id && !h.succ_acked) {
        h.succ_acked = true;
        h.ack |= 1u << c.player_id; // the successor switches roles on the first ack: it never sees (or acks) the GO
        logf("net: hub-leave -- successor player %d acknowledged and is now the hub (gen %d) (mp:U62)",
             c.player_id, (int)gen);
        hl_send_go();
    }
}

// The successor acknowledged: repeat the CURRENT list once to every client (the survivors treat a repeat of the
// generation they hold as "dial now"). Caller holds m_conn_cs.
void Endpoint::hl_send_go() {
    HubLeaveState &h = m_hl;
    uint8_t buf[mesh::MAX_FRAME];
    const int n = mesh::leaving_encode(h.cur, buf, (int)sizeof(buf));
    if (n == 0) return;
    h.go_sent = true;
    for (int id = 0; id < 8; ++id) {
        if (!(h.want >> id & 1u)) continue;
        const int ci = conn_of_player(id);
        if (ci >= 0) mesh_send(ci, buf, n, id);
    }
    logf("net: hub-leave -- GO sent to %d client(s) (mp:U62)", mesh::popcnt(h.want));
}

// The leaving hub's timer-thread step: re-issue the list without a successor that never acknowledged.
void Endpoint::hl_leave_tick(DWORD now) {
    HubLeaveState &h = m_hl;
    if (!h.leaving || h.succ_acked || h.cur.n < 2) return;
    if ((DWORD)(now - h.sent_ms) < mesh_t(SUCC_ACK_MS)) return;
    const int gone = h.cur.e[0].id;
    h.removed |= 1u << gone;
    h.want &= ~(1u << gone); // a client we gave up on is not waited for
    h.ack = 0;
    h.go_sent = false;
    memset(h.ackc, 0, sizeof(h.ackc));
    for (int i = 1; i < h.cur.n; ++i) h.cur.e[i - 1] = h.cur.e[i];
    --h.cur.n;
    ++h.cur.gen;
    uint8_t buf[mesh::MAX_FRAME];
    const int n = mesh::leaving_encode(h.cur, buf, (int)sizeof(buf));
    if (n == 0) return;
    for (int id = 0; id < 8; ++id) {
        if (!(h.want >> id & 1u)) continue;
        const int ci = conn_of_player(id);
        if (ci >= 0) mesh_send(ci, buf, n, id);
    }
    h.sent_ms = now ? now : 1u;
    ++h.retargets;
    char lt[24];
    list_text(h.cur, lt, sizeof(lt));
    logf("net: hub-leave -- successor player %d did not acknowledge within %u ms: RE-ISSUED without it, gen=%d "
         "successors=%s (mp:U62)",
         gone, (unsigned)SUCC_ACK_MS, (int)h.cur.gen, lt);
}

// ---- the surviving side ---------------------------------------------------------------------------------
void Endpoint::hl_on_leaving(int idx, const mesh::Leaving &L) {
    HubLeaveState &h = m_hl;
    ++h.leaving_rx;
    if (m_cfg.no_hub_migration) {
        logf("net: HUB_LEAVING from player %d ignored -- [net] hub_migration=0 (mp:U62)", (int)L.hub);
        return;
    }
    if (m_hl_test_mute) {
        logf("net: HUB_LEAVING gen=%d from player %d heard and MUTED (test hook): no ack, no rehome (mp:U62)",
             (int)L.gen, (int)L.hub);
        return;
    }
    bool me_in = false;
    for (int i = 0; i < L.n; ++i)
        if (L.e[i].id == m_my_id) me_in = true;
    if (!me_in) {
        logf("net: HUB_LEAVING gen=%d does not list this player (%d) -- ignored (mp:U62)", (int)L.gen, m_my_id);
        return;
    }
    const int succ = L.e[0].id;
    uint8_t   ack[mesh::LEAVE_ACK_BYTES];
    mesh::leave_ack_encode(L.epoch, L.gen, succ == m_my_id ? mesh::LA_SUCCESSOR : 0, ack);
    if (L.gen <= h.seen_gen) { // a repeat of a generation already acted on = the hub's GO: say "got it" and dial
        mesh_send(idx, ack, (int)sizeof(ack), BROADCAST);
        ++h.acks_tx;
        if (L.gen == h.seen_gen && h.todo_valid && !h.todo_go) {
            h.todo_go = true;
            logf("net: HUB_LEAVING gen=%d GO from player %d -- the successor is up, re-dialling it (mp:U62)", (int)L.gen,
                 (int)L.hub);
        }
        return;
    }
    h.seen_gen = L.gen;
    mesh_send(idx, ack, (int)sizeof(ack), BROADCAST); // BEFORE the conn is retired (see the block comment)
    ++h.acks_tx;
    h.todo       = L;
    h.todo_valid = true;
    h.todo_go    = (succ == m_my_id); // the successor switches at once; the others wait for the GO
    char lt[24];
    list_text(L, lt, sizeof(lt));
    logf("net: HUB_LEAVING from player %d -- epoch=%u gen=%d successors=%s; acknowledged, %s (mp:U62)", (int)L.hub,
         (unsigned)L.epoch, (int)L.gen, lt, succ == m_my_id ? "this player becomes the hub" : "waiting for the GO before re-dialling the successor");
}

// After a rehome, the mesh state a survivor carries has to point at the new hub. Caller holds m_conn_cs.
void Endpoint::hl_become_hub_state(const mesh::Leaving &L, DWORD now) {
    Mesh &m = m_mesh;
    const uint32_t held_epoch = m.held_valid ? m.held.epoch : 0;
    m_hl.mesh_resume          = true;
    m_hl.epoch_inherited      = true;
    // The next epoch this hub publishes is numbered ABOVE every epoch any survivor can hold (the old hub sent
    // e+1 only after every ack of e, so a survivor holds the new hub's own held epoch or one above it).
    m.cur = m.held_valid ? m.held : mesh::Epoch();
    m.cur.epoch      = mesh::successor_first_epoch(held_epoch) - 1; // mesh_publish adds one
    m.cur.hub        = (uint8_t)m_my_id;
    m.acked_mask     = 0xFFu; // nothing is outstanding: every survivor already holds what it holds
    m.S              = 0;
    m.roster_ms      = now ? now : 1u;
    m.broker_dirty   = true;
    m.last_broker_ms = 0;
    m.pub_ms         = now ? now : 1u;
    m.last_eval_ms   = now ? now : 1u;
    m.all_acked_ms = m.first_epoch_ms = m.first_cand_ms = 0;
    for (int i = 0; i < 8; ++i) {
        m.have_cand[i] = m.have_row[i] = m.reported_relayed[i] = false;
        m.reported[i]  = mesh::CandList();
        m.row_ms[i]    = 0;
        for (int j = 0; j < 8; ++j) m.M.rtt[i][j] = mesh::NONE;
        m.M.leg[i] = mesh::NONE;
    }
    ++m_hl.changes;
    m_hl.old_hub      = L.hub;
    m_hl.new_hub      = m_my_id;
    m_hl.hub_id       = m_my_id;
    m_hl.change_epoch = L.epoch;
    logf("net: hub-handover -- player %d is the HUB now (was %d); mesh restarts under the same key, next epoch %u, "
         "roster to re-form (mp:U62)",
         m_my_id, (int)L.hub, (unsigned)(m.cur.epoch + 1));
}

// The unlocked half (timer thread). See the block comment above.
void Endpoint::hl_service() {
    mesh::Leaving L;
    int           me = -1;
    bool          have_best = false;
    sockaddr_in   best;
    memset(&best, 0, sizeof(best));
    EnterCriticalSection(&m_conn_cs);
    if (!m_hl.todo_valid) {
        LeaveCriticalSection(&m_conn_cs);
        return;
    }
    L                  = m_hl.todo;
    m_hl.todo_valid    = false;
    me                 = m_my_id;
    const int succ_id  = L.e[0].id;
    if (m_role != 1) { // already the hub (a re-issue naming someone else): nothing to switch
        LeaveCriticalSection(&m_conn_cs);
        logf("net: HUB_LEAVING gen=%d: this endpoint is already the hub -- ignored (mp:U62)", (int)L.gen);
        return;
    }
    if (succ_id != me) {
        const MeshPeer &q = m_mesh.peer[succ_id];
        if (q.have_best && (DWORD)(GetTickCount() - q.best_ms) < 10000) {
            have_best = true;
            best      = q.best;
        }
    }
    LeaveCriticalSection(&m_conn_cs);

    const mesh::LeaveEntry &S      = L.e[0];
    const bool              relay  = m_leg_overhead > 0;
    const DWORD             t0     = GetTickCount();
    if (S.id == me) {
        // ---- THIS PLAYER IS THE NEW HUB
        unsigned short dp = 0;
        if (relay) {
            if (!m_mesh_hooks.tunnel_rehome || S.room == 0 ||
                !m_mesh_hooks.tunnel_rehome(m_mesh_hooks.ctx, 0, S.room, bound_port(), &dp)) {
                logf("net: hub-handover -- the relay tunnel could not switch to hosting room %u: this player cannot "
                     "become the hub (mp:U62)",
                     (unsigned)S.room);
                return;
            }
        }
        RehomeSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.as_hub = true;
        for (int i = 0; i < L.n; ++i)
            if (L.e[i].id != me && sp.roster_n < MH_NET_MAX_PEERS) sp.roster[sp.roster_n++] = L.e[i].id;
        if (!rehome(sp)) return;
        EnterCriticalSection(&m_conn_cs);
        hl_become_hub_state(L, GetTickCount());
        LeaveCriticalSection(&m_conn_cs);
        logf("net: hub-handover -- switched to hub in %u ms (mp:U62)", (unsigned)(GetTickCount() - t0));
        return;
    }
    // ---- A SURVIVOR: re-dial the successor, keeping our id
    RehomeSpec sp;
    memset(&sp, 0, sizeof(sp));
    sp.as_hub         = false;
    sp.dial_budget_ms = (int)mesh_t(HL_DIAL_BUDGET_MS);
    if (relay) {
        unsigned short dp = 0;
        if (!m_mesh_hooks.tunnel_rehome || S.room == 0 ||
            !m_mesh_hooks.tunnel_rehome(m_mesh_hooks.ctx, 1, S.room, 0, &dp) || dp == 0) {
            logf("net: hub-handover -- the relay tunnel could not move to room %u: cannot follow the new hub (mp:U62)",
                 (unsigned)S.room);
            return;
        }
        lstrcpynA(sp.host, "127.0.0.1", (int)sizeof(sp.host));
        sp.port = (int)dp;
    } else {
        mesh::Addr4 a;
        if (have_best) a = to_addr4(best);
        else if (S.cands.n > 0) a = S.cands.a[0];
        else {
            logf("net: hub-handover -- no address for the successor (player %d): cannot follow it (mp:U62)", S.id);
            return;
        }
        wsprintfA(sp.host, "%u.%u.%u.%u", (unsigned)a.ip[0], (unsigned)a.ip[1], (unsigned)a.ip[2], (unsigned)a.ip[3]);
        sp.port = (int)a.port;
    }
    if (!rehome(sp)) return;
    EnterCriticalSection(&m_conn_cs);
    m_hl.mesh_resume = true;
    m_mesh.cand_sent   = false; // the new hub has not seen our candidates
    m_mesh.last_row_ms = 0;     // ...nor our row
    ++m_hl.changes;
    m_hl.old_hub      = L.hub;
    m_hl.new_hub      = S.id;
    m_hl.hub_id       = S.id;
    m_hl.change_epoch = L.epoch;
    LeaveCriticalSection(&m_conn_cs);
    logf("net: hub-handover -- player %d re-dials the new hub (player %d at %s:%d) keeping its id (mp:U62)", me,
         S.id, sp.host, sp.port);
}

// ---- the manual driver (the MH_Net_Rehome export) ---------------------------------------------------------
bool Endpoint::rehome_ex(const RehomeEx &x) {
    if (!m_started || !m_cs_ready) return false;
    RehomeSpec     sp;
    unsigned short dp = 0;
    memset(&sp, 0, sizeof(sp));
    const bool relay = x.relay_room != 0;
    if (relay) {
        if (!m_mesh_hooks.tunnel_rehome ||
            !m_mesh_hooks.tunnel_rehome(m_mesh_hooks.ctx, x.as_hub ? 0 : 1, x.relay_room,
                                        x.as_hub ? bound_port() : (unsigned short)0, &dp))
            return false;
    }
    sp.as_hub         = x.as_hub;
    sp.dial_budget_ms = x.dial_budget_ms;
    if (x.as_hub) {
        for (int id = 0; id < MH_NET_MAX_PEERS; ++id)
            if ((x.roster_mask >> id & 1u) && id != m_my_id) sp.roster[sp.roster_n++] = id;
    } else if (relay) {
        lstrcpynA(sp.host, "127.0.0.1", (int)sizeof(sp.host));
        sp.port = (int)dp;
    } else {
        lstrcpynA(sp.host, x.host, (int)sizeof(sp.host));
        sp.port = x.port;
    }
    if (!rehome(sp)) return false;
    EnterCriticalSection(&m_conn_cs);
    if (x.as_hub) {
        mesh::Leaving L;
        L.hub = 0xFF & (uint8_t)(m_hl.hub_id >= 0 ? m_hl.hub_id : 0);
        hl_become_hub_state(L, GetTickCount());
    } else {
        m_hl.mesh_resume   = true;
        m_mesh.cand_sent   = false;
        m_mesh.last_row_ms = 0;
    }
    LeaveCriticalSection(&m_conn_cs);
    return true;
}

void Endpoint::hub_status(HubStatus &o) {
    memset(&o, 0, sizeof(o));
    o.role = -1;
    o.hub_id = -1;
    o.local_id = -1;
    o.old_hub = o.new_hub = -1;
    if (!m_cs_ready) return;
    EnterCriticalSection(&m_conn_cs);
    const Mesh &m = m_mesh;
    o.enabled     = !m_cfg.no_hub_migration;
    o.role        = (int)m_role;
    o.local_id    = m_my_id;
    const mesh::Epoch *e = nullptr;
    if (m_role == 0) {
        if (m_hl.epoch_inherited) {
            if (m.held_valid) e = &m.held;
        } else if (m.cur.epoch != 0) {
            e = &m.cur;
        }
    } else if (m.held_valid) {
        e = &m.held;
    }
    if (e) {
        o.epoch     = e->epoch;
        o.rank_n    = e->n;
        o.survivors = e->S;
        for (int i = 0; i < 8; ++i) o.rank[i] = e->rank[i];
    }
    if (m_role == 0) o.hub_id = m_my_id;
    else if (m_hl.hub_id >= 0) o.hub_id = m_hl.hub_id;
    else if (m.held_valid) o.hub_id = m.held.hub;
    o.leave_state    = m_hl.state;
    o.rehomed        = m_rehomed;
    o.reconciling    = m_rc.active;
    o.reconcile_done = m_rc.done;
    o.unrecoverable  = m_rc.unrecoverable;
    o.aborted        = m_rc.aborted;
    o.changes        = m_hl.changes;
    if (m_hl.changes > 0) {
        o.old_hub      = m_hl.old_hub;
        o.new_hub      = m_hl.new_hub;
        o.change_epoch = m_hl.change_epoch;
    }
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active && !m_conns[i].tx_dead && m_conns[i].player_id != m_my_id) ++o.clients;
    o.leaving_rx = m_hl.leaving_rx;
    o.acks_tx    = m_hl.acks_tx;
    o.acks_rx    = m_hl.acks_rx;
    o.retargets  = m_hl.retargets;
    o.failover        = m_fo.phase;
    o.failover_active = m_fo.phase == FO_SUSPECT || m_fo.phase == FO_ELECT;
    o.failover_ms     = (m_fo.phase == FO_IDLE || m_fo.start_ms == 0)
                            ? 0
                            : (int)((o.failover_active ? GetTickCount() : m_fo.done_ms) - m_fo.start_ms);
    o.change_crash    = m_hl.change_crash;
    o.fo_redirects_rx = m_fo.redirects_rx;
    o.fo_redirects_tx = m_fo.redirects_tx;
    o.fo_group        = mesh::popcnt(m_fo.group);
    LeaveCriticalSection(&m_conn_cs);
}

// =================================================================================================
// mp:U63 (HM-M5) -- CRASH FAILOVER (plan sections 2, 5.2, 5.3, 7.1)
//
// THE SHAPE. The hub's process is gone and nothing said so. Every survivor holds the last succession epoch it acked
// (the ranked candidate list, the dial info, a pre-minted relay room per candidate), so what is left to do is local:
//
//   1. DETECT.   The hub link has heard NOTHING (not DATA only: pings count) for T_suspect = max(2 s, 4 SRTT + 4 RTTVAR)
//                -> SUSPECT. From here a peer sends FO_STATE (sealed on the mesh channel) every 250 ms to the
//                other survivors, carrying "my hub is silent", its held epoch and the candidate it has in mind.
//   2. CORROBORATE. One peer's silence is not enough (a flaky link of one successor must not hijack a live hub):
//                direct match -- another survivor's FO_STATE says "hub silent" too; relayed match -- survivors cannot
//                hear each other at all (the relay pairs host<->client only), so the corroboration is the peer's OWN
//                relay leg being alive: every route runs through the relay, a live leg and a silent hub is the hub.
//                -> ELECT.
//   3. ELECT.    Walk the held ranking: the first candidate that is not dead to this peer. If that is THIS peer it
//                rehomes as the hub (a relayed match: registers the pre-minted room); otherwise it re-dials the
//                candidate keeping its id, trying EVERY address it has for it (the verified FO_STATE path, the best
//                probe path, the brokered ones), and for a relayed match the candidate's room. A candidate that does
//                not answer within T_redial is skipped; one that is up but not the hub answers REDIRECT (a
//                better-ranked candidate it can still reach, or the hub it already follows).
//   4. AGREE.    Everyone holds the same epoch (or at most two apart: a peer that sees a higher epoch's first choice
//                adopts it), so the walk is the same list on every survivor, and the same hub is the result.
//   5. QUORUM.   Plan Q1: only a strict majority of the seated human peers of that epoch plays on (mesh::quorum_ok).
//                Direct match: judged at corroboration + 1.5 s from the survivors this peer can hear; every match:
//                judged again by the new hub when its roster has re-dialled (or after FO_QUORUM_MS). A minority ends.
//   6. BOUND.    The whole failover is bounded (failover_budget_ms, 20 s): past it a true partition ends
//                through U55's own path. The game suspends its silence timers while `failover_active` (SUSPECT/ELECT).
//
// WHAT IT DOES NOT DO: the dead hub is not removed from any ROSTER here. The new hub latches it dead (m_dead_peer,
// U17's latch) and the game's pinned fast-drop removes it at the PARKED step (D5/G335) -- never at receive time. A
// survivor that never re-dialled is latched the same way, after the quorum judgement.
//
// RISK, STATED: a relayed match with survivors at epochs e and e+1 would dial different pre-minted rooms (the rooms
// belong to the epoch); the hub publishes e+1 only after every ack of e, so this needs the hub to die inside one ack
// round. Direct matches adopt the higher epoch's first choice (FO_STATE carries it).
// =================================================================================================

void Endpoint::set_spectator(int id, bool on) {
    if (id < 0 || id > 7) return;
    if (on) InterlockedOr(&m_spectators, 1L << id);
    else InterlockedAnd(&m_spectators, ~(1L << id));
}

void Endpoint::set_in_match(bool on) {
    InterlockedExchange(&m_in_match, on ? 1 : 0);
    if (on || !m_cs_ready) return;
    EnterCriticalSection(&m_conn_cs);
    if (m_fo.phase == FO_FAILED || m_fo.phase == FO_MINORITY || m_fo.phase == FO_DONE) {
        memset(&m_fo, 0, sizeof(m_fo)); // the match is over: the next one starts armed and clean
        m_fo.hub_id = m_fo.cand = m_fo.forced = -1;
    }
    LeaveCriticalSection(&m_conn_cs);
}

namespace {
// The addresses at which a survivor may be reached, best evidence first: where its last FO_STATE came from (a path
// that just worked), the confirmed probe path, then what the old hub brokered. Deduplicated, at most 8.
int fo_collect_addrs(const mesh::CandList &brokered, bool have_fo, const sockaddr_in &fo_addr, bool have_best,
                     const sockaddr_in &best, mesh::Addr4 *out) {
    int  n   = 0;
    auto add = [&](const mesh::Addr4 &a) {
        for (int i = 0; i < n; ++i)
            if (mesh::addr_eq(out[i], a)) return;
        if (n < 8) out[n++] = a;
    };
    if (have_fo) add(to_addr4(fo_addr));
    if (have_best) add(to_addr4(best));
    for (int i = 0; i < brokered.n; ++i) add(brokered.a[i]);
    return n;
}
} // namespace

DWORD Endpoint::fo_peer_age(int peer, DWORD now) {
    DWORD best = 0xFFFFFFFFu;
    auto  upd  = [&](DWORD t) {
        if (t == 0) return;
        const DWORD a = (DWORD)(now - t);
        if ((int32_t)a < 0) return;
        if (a < best) best = a;
    };
    const FoPeer   &fp = m_fo.p[peer];
    const MeshPeer &q  = m_mesh.peer[peer];
    upd(fp.ms);
    upd(q.heard_ms);
    upd(q.last_echo_ms);
    return best;
}

uint32_t Endpoint::fo_alive_mask(DWORD now) {
    uint32_t mask = 0;
    for (int p = 0; p < 8; ++p)
        if (p != m_my_id && (m_fo.ep.S >> p & 1u) && fo_peer_age(p, now) <= mesh_t(mesh::FO_FRESH_MS)) mask |= 1u << p;
    return mask;
}

void Endpoint::fo_send(int peer, uint8_t kind, uint8_t flags, uint8_t arg, DWORD now) {
    (void)now;
    if (m_fo_test_mute || peer < 0 || peer >= 8 || peer == m_my_id || !m_mesh.key_valid) return;
    Mesh   &m = m_mesh;
    uint8_t body[mesh::FO_BODY];
    mesh::fo_body(kind, m_my_id, peer, ++m_fo.seq, flags, m.held_valid ? m.held.epoch : m_fo.ep.epoch, arg, body);
    const FoPeer   &fp = m_fo.p[peer];
    const MeshPeer &q  = m.peer[peer];
    mesh::Addr4     a[8];
    const int       n = fo_collect_addrs(m_fo.ep.dial[peer], fp.have_addr, fp.addr, q.have_best, q.best, a);
    for (int i = 0; i < n; ++i) {
        const sockaddr_in to = from_addr4(a[i]);
        send_sealed(to, U::PKT_DATA, m.me.cid, m.tx_seq++, m.me.enc, m.me.mac, body, sizeof(body));
        m.probe_bytes_tx += (long)(U::HDR_SIZE + sizeof(body) + U::TAG_SIZE);
    }
    if (kind == mesh::PR_FO_STATE) ++m_fo.states_tx;
    else ++m_fo.redirects_tx;
}

// What this peer would dial / become right now: -1 = nobody (every candidate failed or is dead to it), *undecided
// set when a better-ranked candidate has not had time to prove it is alive or dead.
int Endpoint::fo_my_choice(DWORD now, bool *undecided) {
    Failover &f = m_fo;
    if (undecided) *undecided = false;
    const mesh::Epoch &e = f.ep;
    auto live = [&](int id) { return id >= 0 && id < 8 && (e.S >> id & 1u) && !(f.failed >> id & 1u); };
    if (f.forced >= 0 && live(f.forced)) return f.forced; // a REDIRECT (or a higher epoch's choice) outranks the list
    const int tf = (int)m_fo_test_first;
    if (tf >= 0 && live(tf)) return tf;
    for (int i = 0; i < e.n; ++i) {
        const int id = e.rank[i];
        if (!live(id)) continue;
        if (id == m_my_id) return id;
        if (f.relayed) return id; // nothing can be known about a peer behind the relay: try it, its room answers or not
        if (fo_peer_age(id, now) <= mesh_t(mesh::FO_FRESH_MS)) return id;
        if (f.corrob_ms == 0 || (DWORD)(now - f.corrob_ms) < mesh_t(mesh::FO_DEAD_MS)) {
            if (undecided) *undecided = true; // it may still answer: do not skip a better candidate on silence this young
            return -1;
        }
        // silent for T_redial since the loss was corroborated: dead to me, the next candidate
    }
    return -1;
}

void Endpoint::fo_broadcast_state(DWORD now) {
    Failover &f   = m_fo;
    f.state_tx_ms = now ? now : 1u;
    if (f.relayed || m_fo_test_mute) return;
    uint8_t flags = 0;
    if ((f.phase == FO_SUSPECT || f.phase == FO_ELECT) && m_role == 1) flags |= mesh::FOF_SUSPECT;
    if (f.hub_acted) flags |= mesh::FOF_HUB;
    bool      u  = false;
    const int ch = f.phase == FO_ELECT && !f.hub_acted ? fo_my_choice(now, &u) : (f.hub_acted ? m_my_id : -1);
    for (int p = 0; p < 8; ++p)
        if (p != m_my_id && (f.ep.S >> p & 1u))
            fo_send(p, mesh::PR_FO_STATE, flags, ch >= 0 ? (uint8_t)ch : mesh::FO_NONE_ARG, now);
}

void Endpoint::fo_enter(DWORD now, const char *why) {
    Failover &f = m_fo;
    FoPeer    keep[8];
    memcpy(keep, f.p, sizeof(keep)); // what the others already said (they may have been first)
    memset(&f, 0, sizeof(f));
    memcpy(f.p, keep, sizeof(keep));
    f.hub_id   = f.cand = f.forced = -1;
    f.phase    = FO_SUSPECT;
    f.start_ms = now ? now : 1u;
    f.ep       = m_mesh.held;
    f.relayed  = m_leg_overhead > 0 && is_loopback(m_conns[0].addr);
    char rk[24];
    rank_text(f.ep.rank, f.ep.n, rk, sizeof(rk));
    logf("net: failover SUSPECT -- %s; epoch=%u hub=%d rank=%s%s (mp:U63)", why, (unsigned)f.ep.epoch, (int)f.ep.hub, rk,
         f.relayed ? " (relayed: corroborated by the live relay leg)" : "");
}

void Endpoint::fo_finish(int phase, const char *why) {
    Failover &f = m_fo;
    f.phase     = phase;
    f.done_ms   = GetTickCount();
    f.act       = FA_NONE;
    if (phase == FO_MINORITY) {
        // This side ends: it carries nobody (a hub drops its roster; a client that already re-dialled leaves the hub).
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_conns[i].active && !m_conns[i].tx_dead) refuse_conn(i, "minority group: this side ends (mp:U63)");
    }
    const char *nm = phase == FO_DONE ? "DONE" : phase == FO_FAILED ? "FAILED" : phase == FO_MINORITY ? "MINORITY (ends)" : "IDLE";
    logf("net: failover %s -- %s (after %u ms) (mp:U63)", nm, why, (unsigned)(f.done_ms - f.start_ms));
}

void Endpoint::fo_client_done(int hub, DWORD now) {
    Failover &f        = m_fo;
    m_hl.mesh_resume   = true;
    m_mesh.cand_sent   = false; // the new hub has not seen our candidates
    m_mesh.last_row_ms = 0;     // ...nor our row
    ++m_hl.changes;
    m_hl.old_hub      = f.ep.hub;
    m_hl.new_hub      = hub;
    m_hl.hub_id       = hub;
    m_hl.change_epoch = f.ep.epoch;
    m_hl.change_crash = true;
    f.hub_id          = hub;
    logf("net: hub elected %d epoch %u -- crash failover after %u ms: re-dialled player %d keeping id %d (mp:U63)", hub,
         (unsigned)f.ep.epoch, (unsigned)(now - f.start_ms), hub, m_my_id);
    fo_finish(FO_DONE, "re-dialled the elected hub");
}

void Endpoint::fo_on_datagram(int from_id, uint8_t kind, uint8_t flags, uint32_t epoch, uint8_t arg,
                              const sockaddr_in &from, DWORD now) {
    Failover &f = m_fo;
    if (m_cfg.no_hub_migration) return;
    FoPeer &p = f.p[from_id];
    if (kind == mesh::PR_FO_REDIRECT) {
        ++f.redirects_rx;
        if (f.phase == FO_ELECT && !f.hub_acted && arg < 8 && arg != m_my_id && arg != from_id && (f.ep.S >> arg & 1u)) {
            f.failed |= 1u << from_id; // it is up but is not the hub: not this round
            f.forced = arg;
            if (f.cand == from_id) {
                f.cand      = -1;
                f.dialing   = false;
                f.dialed_ok = false;
            }
            logf("net: failover REDIRECT from player %d -> player %d (mp:U63)", from_id, (int)arg);
        }
        return;
    }
    // PR_FO_STATE
    ++f.states_rx;
    p.ms        = now ? now : 1u;
    p.flags     = flags;
    p.epoch     = epoch;
    p.arg       = arg;
    p.addr      = from;
    p.have_addr = true;
    // a peer that holds a HIGHER epoch knows a ranking this peer does not: adopt its first choice
    if (f.phase == FO_ELECT && !f.hub_acted && epoch > f.ep.epoch && arg < 8 && arg != m_my_id && (f.ep.S >> arg & 1u) &&
        !(f.failed >> arg & 1u) && f.forced != arg) {
        f.forced = arg;
        logf("net: failover adopts player %d's first choice %d (its epoch %u > held %u) (mp:U63)", from_id, (int)arg,
             (unsigned)epoch, (unsigned)f.ep.epoch);
    }
    if (flags & mesh::FOF_REPLY) return; // an answer: never answered in turn
    // ---- answer: always (a peer that has not noticed the loss yet is still alive and must be HEARD)
    if (!m_fo_test_mute && (DWORD)(now - p.reply_ms) >= 200) {
        p.reply_ms = now ? now : 1u;
        uint8_t rf = mesh::FOF_REPLY;
        if (m_role == 1 && (f.phase == FO_SUSPECT || f.phase == FO_ELECT)) rf |= mesh::FOF_SUSPECT;
        if (f.hub_acted) rf |= mesh::FOF_HUB;
        bool      u  = false;
        const int ch = (f.phase == FO_ELECT && !f.hub_acted) ? fo_my_choice(now, &u) : (f.hub_acted ? m_my_id : -1);
        fo_send(from_id, mesh::PR_FO_STATE, rf, ch >= 0 ? (uint8_t)ch : mesh::FO_NONE_ARG, now);
    }
    // ---- REDIRECT: it intends to dial ME as the hub; I am not the hub, and I can still reach a better candidate (or I
    // already follow a hub): tell it where to go instead (plan 5.3 rule 2).
    if (arg == m_my_id && !f.hub_acted && !m_fo_test_mute && (DWORD)(now - p.redir_tx_ms) >= 250) {
        int target = -1;
        if (f.phase == FO_DONE && f.hub_id >= 0 && f.hub_id != m_my_id) target = f.hub_id;
        else if (f.phase == FO_ELECT) {
            bool      u  = false;
            const int ch = fo_my_choice(now, &u);
            if (ch >= 0 && ch != m_my_id && ch != from_id) target = ch;
        }
        if (target >= 0) {
            p.redir_tx_ms = now ? now : 1u;
            fo_send(from_id, mesh::PR_FO_REDIRECT, 0, (uint8_t)target, now);
            logf("net: failover REDIRECT sent to player %d -> player %d (it dialled me; I am not the hub) (mp:U63)",
                 from_id, target);
        }
    }
}

// ---- the timer thread, under m_conn_cs ------------------------------------------------------------------
void Endpoint::fo_tick(DWORD now) {
    Failover &f = m_fo;
    Mesh     &m = m_mesh;
    if (m_cfg.no_hub_migration || !m.on) return;
    const DWORD budget = m_cfg.failover_budget_ms > 0 ? (DWORD)m_cfg.failover_budget_ms : mesh_t(mesh::FO_BUDGET_MS);
    const bool  match  = InterlockedCompareExchange(&m_in_match, 0, 0) != 0;

    if (f.phase == FO_DONE && m_role == 1 && (DWORD)(now - f.done_ms) > 3000 && m_conns[0].active &&
        !m_conns[0].tx_dead) {
        f.phase = FO_IDLE; // the new hub carries us: armed again for the NEXT loss
        f.ep    = mesh::Epoch();
        f.failed = 0;
        f.forced = f.cand = -1;
        f.hub_acted = f.dialing = false;
    }
    if (f.phase == FO_IDLE) {
        if (!match || m_role != 1 || !m_started) return;
        if (InterlockedCompareExchange(&m_id_assigned, 0, 0) == 0 || m_my_id < 0 || m_my_id >= 8) return;
        if (m_pend[0].used || m_hl.leaving || m_hl.todo_valid) return; // a handshake / a planned handover is in flight
        if (!m.held_valid || mesh::popcnt(m.held.S) < 2) return;       // no ranking, or nobody to corroborate with
        const int cur_hub = m_hl.hub_id >= 0 ? m_hl.hub_id : (int)m.held.hub;
        if (cur_hub != (int)m.held.hub) return; // the held epoch ranks a hub that is no longer ours: nothing to walk
        const Conn &c = m_conns[0];
        if (!c.bound && !c.active && c.last_rx == 0) return;
        DWORD silent = (DWORD)(now - c.last_rx);
        if ((int32_t)silent < 0) silent = 0;
        const DWORD T = mesh_t(mesh::fo_suspect_ms(c.stats.rtt.samples > 0 ? c.stats.rtt.srtt_ms : 0.0,
                                                 c.stats.rtt.samples > 0 ? c.stats.rtt.rttvar_ms : 0.0));
        bool others = false;
        for (int p = 0; p < 8; ++p)
            if (p != m_my_id && (m.held.S >> p & 1u) && (f.p[p].flags & mesh::FOF_SUSPECT) && f.p[p].ms != 0 &&
                (DWORD)(now - f.p[p].ms) <= mesh_t(mesh::FO_FRESH_MS))
                others = true;
        if (silent >= T) {
            char w[96];
            wsprintfA(w, "the hub link heard nothing for %u ms (T_suspect %u ms)", (unsigned)silent, (unsigned)T);
            fo_enter(now, w);
        } else if (others && silent >= mesh_t(mesh::FO_CORROB_MS)) {
            char w[96];
            wsprintfA(w, "the hub link heard nothing for %u ms and another survivor already says so", (unsigned)silent);
            fo_enter(now, w);
        }
        return;
    }
    if (f.phase != FO_SUSPECT && f.phase != FO_ELECT) return; // DONE / FAILED / MINORITY: nothing to run
    if (!match) {
        fo_finish(FO_IDLE, "the match ended");
        return;
    }
    if ((DWORD)(now - f.start_ms) >= budget) {
        fo_finish(FO_FAILED, "the failover budget ran out -- a true partition ends through U55's path");
        return;
    }
    if (m_role == 1 && !f.hub_acted && (DWORD)(now - f.state_tx_ms) >= mesh_t(mesh::FO_STATE_MS)) fo_broadcast_state(now);

    // ---- the hub came back? (only before corroboration; after it the walk is committed)
    if (f.phase == FO_SUSPECT && m_role == 1) {
        const Conn &c = m_conns[0];
        DWORD silent  = (DWORD)(now - c.last_rx);
        if ((int32_t)silent < 0) silent = 0;
        if (c.active && !c.tx_dead && silent < mesh_t(mesh::FO_CORROB_MS)) {
            fo_finish(FO_IDLE, "the hub is heard again");
            return;
        }
    }

    // ---- corroboration
    if (f.phase == FO_SUSPECT) {
        bool ok       = false;
        int  leg_age  = -1; // mp:U64: kept for the isolation verdict below
        if (f.relayed) {
            const int age = m_mesh_hooks.leg_age_ms ? m_mesh_hooks.leg_age_ms(m_mesh_hooks.ctx) : -1;
            leg_age       = age;
            ok            = age >= 0 && age < (int)mesh_t(mesh::FO_FRESH_MS);
        } else {
            for (int p = 0; p < 8; ++p)
                if (p != m_my_id && (f.ep.S >> p & 1u) && (f.p[p].flags & mesh::FOF_SUSPECT) && f.p[p].ms != 0 &&
                    (DWORD)(now - f.p[p].ms) <= mesh_t(mesh::FO_FRESH_MS))
                    ok = true;
        }
        if (!ok) {
            // mp:U64 (HM-M6): a RELAYED survivor whose own relay leg is dead cannot reach ANY peer: it is a group of one, a
            // minority of every match with a second human in it (U63 left it to run out the 20 s budget and report FAILED).
            // A short flap is not this: the leg must have been silent FO_ISOLATED_MS, counted from the suspicion.
            if (f.relayed && (DWORD)(now - f.start_ms) >= mesh_t(mesh::FO_ISOLATED_MS) && (leg_age < 0 || leg_age >= (int)mesh_t(mesh::FO_ISOLATED_MS))) {
                const uint32_t mem = mesh::quorum_members(f.ep, (uint32_t)m_spectators);
                if (!mesh::quorum_ok(mem, 1u << m_my_id)) {
                    f.group = 1u << m_my_id;
                    char w[160];
                    wsprintfA(w, "minority: cut off from the relay (its leg is silent), alone against %d other seated human(s) of epoch %u (Q1)",
                              mesh::popcnt(mem) - 1, (unsigned)f.ep.epoch);
                    fo_finish(FO_MINORITY, w);
                }
            }
            return;
        }
        f.phase     = FO_ELECT;
        f.corrob_ms = now ? now : 1u;
        logf("net: failover CORROBORATED after %u ms -- %s (mp:U63)", (unsigned)(now - f.start_ms),
             f.relayed ? "the relay leg is alive and the hub is silent" : "another survivor also says the hub is silent");
    }

    // ---- ELECT
    const uint32_t members = mesh::quorum_members(f.ep, (uint32_t)m_spectators);
    if (!f.relayed && !f.quorum_judged && (DWORD)(now - f.corrob_ms) >= mesh_t(mesh::FO_DEAD_MS)) {
        f.quorum_judged = true;
        f.group         = fo_alive_mask(now) | (1u << m_my_id);
        if (!mesh::quorum_ok(members, f.group)) {
            char w[160];
            wsprintfA(w, "minority: this side holds %d of the %d seated humans of epoch %u (Q1: a strict majority plays on)",
                      mesh::popcnt(members & f.group), mesh::popcnt(members), (unsigned)f.ep.epoch);
            fo_finish(FO_MINORITY, w);
            return;
        }
        logf("net: failover quorum ok -- %d of %d seated humans are on this side (mp:U63)",
             mesh::popcnt(members & f.group), mesh::popcnt(members));
    }
    if (f.hub_acted) {
        // THIS peer is the new hub: judge the quorum when the roster has re-dialled (or the wait ran out)
        const uint32_t roster = (uint32_t)f.ep.S & ~(1u << m_my_id);
        uint32_t       seated = 0;
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_conns[i].active && !m_conns[i].tx_dead && m_conns[i].player_id >= 0 && m_conns[i].player_id < 8)
                seated |= 1u << m_conns[i].player_id;
        seated &= roster;
        uint32_t dead_known = 0;
        if (!f.relayed && f.corrob_ms != 0 && (DWORD)(now - f.corrob_ms) >= mesh_t(mesh::FO_DEAD_MS))
            dead_known = roster & ~fo_alive_mask(now) & ~seated;
        const bool all  = (seated | dead_known) == roster;
        const bool late = (int32_t)(now - f.quorum_deadline) >= 0;
        if (!all && !late) return;
        const uint32_t group = seated | (1u << m_my_id);
        f.group              = group;
        if (!mesh::quorum_ok(members, group)) {
            char w[160];
            wsprintfA(w, "minority: the new hub seated %d of the %d seated humans of epoch %u (Q1)",
                      mesh::popcnt(members & group), mesh::popcnt(members), (unsigned)f.ep.epoch);
            fo_finish(FO_MINORITY, w);
            return;
        }
        const uint32_t missing = roster & ~seated;
        if (missing) {
            m_dead_extra |= missing; // never re-dialled: removed at a pinned step, like any dead client
            logf("net: failover -- survivor(s) mask %02x never re-dialled: latched dead for the pinned removal (mp:U63)",
                 (unsigned)missing);
        }
        if (f.change_pending) {
            ++m_hl.changes;
            f.change_pending = false;
        }
        fo_finish(FO_DONE, "the roster re-dialled; quorum ok");
        return;
    }
    // ---- a client: dial progress
    if (f.cand >= 0 && f.dialing) {
        if (m_conns[0].active && !m_conns[0].tx_dead) {
            if (!f.dialed_ok) {
                f.dialed_ok = true;
                logf("net: failover -- re-dialled player %d (%u ms after the loss) (mp:U63)", f.cand, (unsigned)(now - f.start_ms));
            }
            // A DIRECT match is DONE only once the quorum pre-check passed (it ran above): a minority must not announce a hub
            // change it will not keep. A relayed match cannot know its group, so it is DONE at once (the hub judges).
            if (f.relayed || f.quorum_judged) fo_client_done(f.cand, now);
            return;
        }
        if (f.dialed_ok) { // the hub we re-dialled is gone again (a minority hub ends its side, or it died): not this candidate
            logf("net: failover -- player %d, re-dialled, dropped the link again (mp:U63)", f.cand);
            f.dialed_ok = false;
            f.failed |= 1u << f.cand;
            f.cand    = -1;
            f.dialing = false;
            return;
        }
        if ((DWORD)(now - f.cand_ms) >= f.cand_window) {
            logf("net: failover -- player %d did not answer within %u ms: next candidate (mp:U63)", f.cand,
                 (unsigned)f.cand_window);
            f.failed |= 1u << f.cand;
            f.cand    = -1;
            f.dialing = false;
        } else if (!f.relayed && f.addr_n > 1 && f.act == FA_NONE) {
            const DWORD per = f.cand_window / (DWORD)f.addr_n;
            if ((DWORD)(now - f.addr_ms) >= per && f.addr_i + 1 < f.addr_n) {
                f.act        = FA_DIAL;
                f.act_cand   = f.cand;
                f.act_addr_i = ++f.addr_i;
            }
        }
        return;
    }
    if (f.cand >= 0 || f.act != FA_NONE) return; // a dial is being started
    bool      undecided = false;
    const int ch        = fo_my_choice(now, &undecided);
    if (ch < 0) {
        if (!undecided && f.failed != 0) {
            f.failed = 0; // a new round: every candidate again, until the budget ends it
            ++f.rounds;
            logf("net: failover -- every candidate failed this round: round %ld again (mp:U63)", f.rounds + 1);
        }
        return;
    }
    if (ch == m_my_id) {
        f.act      = FA_BECOME_HUB;
        f.act_cand = ch;
        return;
    }
    // dial candidate `ch`
    f.cand    = ch;
    f.cand_ms = now ? now : 1u;
    f.addr_i  = 0;
    f.dialing = false;
    if (f.relayed) {
        f.addr_n = 1;
    } else {
        const FoPeer   &fp = f.p[ch];
        const MeshPeer &q  = m.peer[ch];
        f.addr_n           = fo_collect_addrs(f.ep.dial[ch], fp.have_addr, fp.addr, q.have_best, q.best, f.addrs);
        if (f.addr_n == 0) {
            logf("net: failover -- no address known for candidate %d: skipped (mp:U63)", ch);
            f.failed |= 1u << ch;
            f.cand = -1;
            return;
        }
    }
    DWORD per = mesh_t(mesh::FO_REDIAL_MS);
    if (!f.relayed) {
        per = mesh_t(mesh::FO_REDIAL_MS) / (DWORD)f.addr_n;
        if (per < 400u) per = 400u;
    }
    f.cand_window = f.relayed ? mesh_t(mesh::FO_REDIAL_MS) + mesh_t(1000u) : per * (DWORD)f.addr_n; // a relay switch costs a round trip
    f.addr_ms     = now ? now : 1u;
    f.act         = FA_DIAL;
    f.act_cand    = ch;
    f.act_addr_i  = 0;
    logf("net: failover -- dialling candidate %d (%d address(es)%s) (mp:U63)", ch, f.addr_n,
         f.relayed ? ", via its pre-minted relay room" : "");
}

// ---- the unlocked half (timer thread) ---------------------------------------------------------------------
void Endpoint::fo_service() {
    int         act, cand, addr_i, me, addr_n;
    DWORD       window;
    mesh::Epoch ep;
    mesh::Addr4 addr;
    bool        relayed;
    memset(&addr, 0, sizeof(addr));
    EnterCriticalSection(&m_conn_cs);
    act     = m_fo.act;
    cand    = m_fo.act_cand;
    addr_i  = m_fo.act_addr_i;
    me      = m_my_id;
    ep      = m_fo.ep;
    relayed = m_fo.relayed;
    addr_n  = m_fo.addr_n;
    window  = m_fo.cand_window;
    if (act == FA_DIAL && !relayed && addr_i >= 0 && addr_i < addr_n) addr = m_fo.addrs[addr_i];
    m_fo.act = FA_NONE;
    if (m_role != 1) act = FA_NONE; // already a hub (a handover won the race): nothing to switch
    LeaveCriticalSection(&m_conn_cs);
    if (act == FA_NONE) return;

    const DWORD t0 = GetTickCount();
    if (act == FA_BECOME_HUB) {
        unsigned short dp = 0;
        bool           ok = true;
        if (relayed) {
            const uint32_t room = ep.room[me & 7];
            ok = m_mesh_hooks.tunnel_rehome && room != 0 &&
                 m_mesh_hooks.tunnel_rehome(m_mesh_hooks.ctx, 0, room, bound_port(), &dp);
            if (!ok)
                logf("net: failover -- the relay tunnel could not switch to hosting room %u: this player cannot become "
                     "the hub (mp:U63)",
                     (unsigned)room);
        }
        RehomeSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.as_hub = true;
        for (int i = 0; i < 8; ++i)
            if ((ep.S >> i & 1u) && i != me && sp.roster_n < MH_NET_MAX_PEERS) sp.roster[sp.roster_n++] = i;
        sp.reconcile_wait_ms = (int)mesh_t(1500); // the survivors were corroborated alive already; a late report replans (rc_tick)
        ok = ok && rehome(sp);
        EnterCriticalSection(&m_conn_cs);
        if (!ok) {
            m_fo.failed |= 1u << me;
        } else {
            mesh::Leaving L;
            L.hub   = ep.hub;
            L.epoch = ep.epoch;
            hl_become_hub_state(L, GetTickCount());
            m_hl.change_crash = true;
            --m_hl.changes; // counted (and shown to the player) only once the quorum is judged: a minority is no hub change
            m_fo.change_pending = true;
            InterlockedExchange(&m_dead_peer, (LONG)ep.hub); // the dead hub: U17's pinned fast-drop removes it at the parked step
            m_fo.hub_acted       = true;
            m_fo.hub_id          = me;
            m_fo.quorum_deadline = GetTickCount() + mesh_t(mesh::FO_QUORUM_MS);
            logf("net: hub elected %d epoch %u -- crash failover after %u ms: this player is the hub now (switched in "
                 "%u ms) (mp:U63)",
                 me, (unsigned)ep.epoch, (unsigned)(GetTickCount() - m_fo.start_ms), (unsigned)(GetTickCount() - t0));
            fo_broadcast_state(GetTickCount());
        }
        LeaveCriticalSection(&m_conn_cs);
        return;
    }
    // FA_DIAL
    RehomeSpec sp;
    memset(&sp, 0, sizeof(sp));
    sp.as_hub = false;
    bool ok   = true;
    if (relayed) {
        unsigned short dp   = 0;
        const uint32_t room = ep.room[cand & 7];
        ok = m_mesh_hooks.tunnel_rehome && room != 0 && m_mesh_hooks.tunnel_rehome(m_mesh_hooks.ctx, 1, room, 0, &dp) &&
             dp != 0;
        if (ok) {
            lstrcpynA(sp.host, "127.0.0.1", (int)sizeof(sp.host));
            sp.port = (int)dp;
        } else {
            logf("net: failover -- the relay tunnel could not move to candidate %d's room %u (mp:U63)", cand, (unsigned)room);
        }
        sp.dial_budget_ms = (int)window + 500;
    } else {
        wsprintfA(sp.host, "%u.%u.%u.%u", (unsigned)addr.ip[0], (unsigned)addr.ip[1], (unsigned)addr.ip[2],
                  (unsigned)addr.ip[3]);
        sp.port           = (int)addr.port;
        sp.dial_budget_ms = (int)(window / (DWORD)(addr_n > 0 ? addr_n : 1)) + 200;
    }
    ok = ok && rehome(sp);
    EnterCriticalSection(&m_conn_cs);
    if (!ok) {
        m_fo.failed |= 1u << cand;
        if (m_fo.cand == cand) m_fo.cand = -1;
        m_fo.dialing = false;
    } else if (m_fo.cand == cand) {
        m_fo.dialing = true;
        m_fo.addr_ms = GetTickCount();
    }
    LeaveCriticalSection(&m_conn_cs);
}

} // namespace netudp
} // namespace mh
