// Folded into authtest: actual TCP endpoints, with no UI JOIN on either side.
// A raw peer speaks the pre-admission wire so both plaintext and sealed links
// exercise malformed/legacy/future/missing feature controls before any callback.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mh_net_export.h"
#include "mh_net_proto/net_crypto.h"
#include "mh_net_proto/net_wire.h"
#include "mh_net_proto/session_info.h"

namespace {
namespace P             = mh_net_proto;
volatile LONG callbacks = 0;
void          control(int, const unsigned char *, int) { InterlockedIncrement(&callbacks); }
bool          check(bool good, const char *name) {
    printf("  [%s] TCP generation: %s\n", good ? "ok" : "FAIL", name);
    return good;
}
template <class F>
bool until(F predicate, DWORD ms = 1000) {
    const DWORD began = GetTickCount();
    do {
        if (predicate()) return true;
        Sleep(5);
    } while (GetTickCount() - began < ms);
    return predicate();
}
bool io(SOCKET s, void *buf, int len, bool writing) {
    char *p = static_cast<char *>(buf);
    while (len) {
        const int n = writing ? send(s, p, len, 0) : recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}
struct Peer {
    SOCKET         s      = INVALID_SOCKET;
    bool           secure = false, server = false;
    P::SessionKeys keys{};
    uint64_t       tx = 0, rx = 0;
    ~Peer() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
    bool auth() {
        if (!secure) return true;
        uint8_t psk[P::KEY_LEN], cn[P::NONCE_LEN], sn[P::NONCE_LEN];
        memset(psk, 0x11, sizeof(psk));
        memset(cn, 0x42, sizeof(cn));
        memset(sn, 0x53, sizeof(sn));
        uint8_t  hello[P::HS_HELLO_LEN], challenge[P::HS_CHALLENGE_LEN], response[P::HS_RESPONSE_LEN];
        uint16_t version = 0;
        if (server) {
            if (!io(s, hello, sizeof(hello), false) || !P::hs_parse_hello(hello, cn, version)) return false;
            P::hs_build_challenge(psk, cn, sn, challenge);
            if (!io(s, challenge, sizeof(challenge), true) || !io(s, response, sizeof(response), false) ||
                !P::hs_check_response(psk, cn, sn, response)) return false;
        } else {
            P::hs_build_hello(cn, hello);
            if (!io(s, hello, sizeof(hello), true) || !io(s, challenge, sizeof(challenge), false) ||
                !P::hs_check_challenge(psk, cn, challenge, sn, version)) return false;
            P::hs_build_response(psk, cn, sn, response);
            if (!io(s, response, sizeof(response), true)) return false;
        }
        P::hs_derive_keys(psk, cn, sn, keys);
        return true;
    }
    bool frame(uint16_t flags, int16_t src, const uint8_t *data, size_t len) {
        uint8_t          plain[P::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD];
        const P::WireHdr h{P::WIRE_MAGIC, flags, src, P::BROADCAST, static_cast<uint32_t>(len)};
        P::wire_hdr_encode(h, plain);
        if (len) memcpy(plain + P::WIRE_HDR_SIZE, data, len);
        if (!secure) return io(s, plain, static_cast<int>(P::WIRE_HDR_SIZE + len), true);
        uint8_t      record[P::REC_LEN_SIZE + sizeof(plain) + P::MAC_LEN];
        const size_t n = P::rec_seal(server ? keys.enc_s2c : keys.enc_c2s,
                                     server ? keys.mac_s2c : keys.mac_c2s, tx++, plain,
                                     static_cast<uint32_t>(P::WIRE_HDR_SIZE + len), record);
        return io(s, record, static_cast<int>(n), true);
    }
    bool receive(P::WireHdr &h, uint8_t *out, size_t &len) {
        if (!secure) {
            uint8_t header[P::WIRE_HDR_SIZE];
            if (!io(s, header, sizeof(header), false) || !P::wire_hdr_decode(header, h) ||
                h.len > MH_NET_MAX_PAYLOAD) return false;
            len = h.len;
            return !len || io(s, out, static_cast<int>(len), false);
        }
        uint8_t size[P::REC_LEN_SIZE];
        if (!io(s, size, sizeof(size), false)) return false;
        const uint32_t n = uint32_t(size[0]) | uint32_t(size[1]) << 8 | uint32_t(size[2]) << 16 | uint32_t(size[3]) << 24;
        if (n < P::WIRE_HDR_SIZE || n > P::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD) return false;
        uint8_t body[P::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD], mac[P::MAC_LEN];
        if (!io(s, body, n, false) || !io(s, mac, sizeof(mac), false) ||
            !P::rec_open(server ? keys.enc_c2s : keys.enc_s2c, server ? keys.mac_c2s : keys.mac_s2c,
                         rx++, body, n, mac) ||
            !P::wire_hdr_decode(body, h) || h.len != n - P::WIRE_HDR_SIZE) return false;
        len = h.len;
        if (len) memcpy(out, body + P::WIRE_HDR_SIZE, len);
        return true;
    }
    bool connect_to(int port, bool sealed) {
        secure = sealed;
        s      = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        DWORD timeout = 6500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 && auth();
    }
    bool offer(int id) {
        uint8_t bytes[P::GAME_FEATURE_BYTES];
        P::game_feature_encode(P::GameFeatureStage::Offer, bytes);
        return frame(P::FLAG_HELLO, static_cast<int16_t>(id), bytes, sizeof(bytes));
    }
    bool ack() {
        P::WireHdr h{};
        size_t     n = 0;
        uint8_t    bytes[MH_NET_MAX_PAYLOAD];
        return receive(h, bytes, n) && P::game_feature_accept(h.flags, bytes, n, P::GameFeatureStage::Ack);
    }
    bool confirm(int id) {
        uint8_t bytes[P::GAME_FEATURE_BYTES];
        P::game_feature_encode(P::GameFeatureStage::Confirm, bytes);
        return frame(P::FLAG_HELLO, static_cast<int16_t>(id), bytes, sizeof(bytes));
    }
};
bool pristine() {
    int     sender = -1, len = 32;
    uint8_t bytes[32];
    int     ids[MH_NET_MAX_PEERS];
    return MH_Net_PeerCount() == 0 && MH_Net_ActivePeerIds(ids, MH_NET_MAX_PEERS) == 0 &&
           InterlockedCompareExchange(&callbacks, 0, 0) == 0 && !MH_Net_Recv(&sender, bytes, &len);
}
struct FakeHost {
    SOCKET listener;
    bool   secure;
    int    kind;
    bool   spoke = false;
};
DWORD WINAPI fake_host(LPVOID arg) {
    FakeHost &f = *static_cast<FakeHost *>(arg);
    Peer      p;
    p.server = true;
    p.secure = f.secure;
    p.s      = accept(f.listener, nullptr, nullptr);
    if (p.s == INVALID_SOCKET) return 1;
    DWORD timeout = 3000;
    setsockopt(p.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    if (!p.auth()) return 2;
    P::WireHdr h{};
    size_t     n = 0;
    uint8_t    bytes[MH_NET_MAX_PAYLOAD];
    if (!p.receive(h, bytes, n) || !P::game_feature_accept(h.flags, bytes, n, P::GameFeatureStage::Offer)) return 3;
    f.spoke = true;
    P::game_feature_encode(P::GameFeatureStage::Ack, bytes);
    n = P::GAME_FEATURE_BYTES;
    if (f.kind == 0) n = 0; // real legacy empty WELCOME
    if (f.kind == 1) {
        bytes[4] = 9; // previous QORD1 peer, equal key/content
        bytes[5] = 0;
    }
    if (f.kind == 2) {
        constexpr uint16_t future = P::GAME_GENERATION + 1;
        bytes[4]                  = static_cast<uint8_t>(future);
        bytes[5]                  = static_cast<uint8_t>(future >> 8);
    }
    if (f.kind == 3) bytes[7] = 1; // malformed reserved byte
    if (f.kind == 4) {
        Sleep(2300);
        return 0;
    } // missing ACK (client deadline)
    p.frame(P::FLAG_WELCOME, 0, bytes, n);
    const uint8_t unsupported[] = {0xfe, 1, 2, 3};
    p.frame(P::FLAG_SESSION_INFO, 0, unsupported, sizeof(unsupported));
    p.frame(P::FLAG_START, 0, unsupported, sizeof(unsupported));
    p.frame(P::FLAG_DATA, 0, unsupported, sizeof(unsupported));
    return 0;
}
} // namespace

int run_tcp_generation_checks(int port, const char *secure_key, const char *open_key) {
    int failures = 0;
    MH_Net_SetSessionInfoHandler(control);
    MH_Net_SetJoinHandler(control);
    MH_Net_SetStartHandler(control);
    MH_Net_SetAnnounceHandler(control);
    for (int sealed = 0; sealed != 2; ++sealed) {
        SetEnvironmentVariableA("MH_KEY_FILE", sealed ? secure_key : open_key);
        for (int assigned = 0; assigned != 2; ++assigned) {
            MH_NetConfig cfg{};
            cfg.role          = 0;
            cfg.player_id     = 0;
            cfg.port          = port;
            cfg.host_assign   = assigned;
            cfg.ping_ms       = -1;
            cfg.rx_timeout_ms = -1;
            if (!check(MH_Net_InitEx(&cfg) != 0, "actual host starts")) {
                ++failures;
                continue;
            }
            for (int kind = 0; kind != 5; ++kind) {
                Peer    p;
                bool    ok = p.connect_to(port, sealed != 0);
                uint8_t bytes[P::GAME_FEATURE_BYTES];
                P::game_feature_encode(P::GameFeatureStage::Offer, bytes);
                size_t n = sizeof(bytes);
                if (kind == 0) n = 0;
                if (kind == 1) {
                    bytes[4] = 9;
                    bytes[5] = 0;
                }
                if (kind == 2) {
                    constexpr uint16_t future = P::GAME_GENERATION + 1;
                    bytes[4]                  = static_cast<uint8_t>(future);
                    bytes[5]                  = static_cast<uint8_t>(future >> 8);
                }
                if (kind == 3) bytes[7] = 1;
                ok = ok && p.frame(P::FLAG_HELLO, 1, bytes, n);
                if (kind == 4) {
                    ok = ok && p.ack() && pristine(); // no CONFIRM: no public seat
                } else {
                    const uint8_t unsupported[] = {0xfe, 0, 0, 0};
                    p.frame(P::FLAG_JOIN, 1, unsupported, sizeof(unsupported));
                    p.frame(P::FLAG_START, 1, unsupported, sizeof(unsupported));
                    p.frame(P::FLAG_DATA, 1, unsupported, sizeof(unsupported));
                }
                char      byte     = 0;
                const int received = recv(p.s, &byte, 1, 0);
                ok                 = ok && received <= 0 && (received == 0 || WSAGetLastError() != WSAETIMEDOUT) && pristine();
                if (!check(ok, "old/future/malformed/missing-confirm force peer never publishes state")) ++failures;
            }
            // Concurrent unconfirmed offers reserve NO public IDs. Allocation is
            // after CONFIRM under the transport lock, so the real peer gets 1.
            {
                Peer a, b, good;
                bool ok = a.connect_to(port, sealed != 0) && a.offer(1) && a.ack() &&
                          b.connect_to(port, sealed != 0) && b.offer(1) && b.ack() && pristine() &&
                          good.connect_to(port, sealed != 0) && good.offer(1) && good.ack() && good.confirm(1);
                ok = ok && until([] { return MH_Net_PeerCount() == 1; });
                if (assigned) {
                    P::WireHdr h{};
                    uint8_t    bytes[MH_NET_MAX_PAYLOAD];
                    size_t     n = 0;
                    ok           = ok && good.receive(h, bytes, n) && h.flags == P::FLAG_WELCOME && n == 0 && h.dst == 1;
                }
                const uint8_t payload[] = {0xfe, 4, 5};
                ok                      = ok && good.frame(P::FLAG_DATA, 1, payload, sizeof(payload));
                int     from = -1, n = 32;
                uint8_t got[32];
                ok = ok && until([&] { n = sizeof(got); return MH_Net_Recv(&from, got, &n) != 0; }) &&
                     from == 1 && n == sizeof(payload) && memcmp(got, payload, sizeof(payload)) == 0;
                if (!check(ok, "concurrent offers stay provisional; confirmed current peer alone carries tasks")) ++failures;
            }
            if (!check(until([] { return MH_Net_PeerCount() == 0; }), "confirmed peer retires normally")) ++failures;
        }
        for (int kind = 0; kind != 5; ++kind) {
            FakeHost fake{};
            fake.secure   = sealed != 0;
            fake.kind     = kind;
            fake.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family      = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bool ok           = fake.listener != INVALID_SOCKET && bind(fake.listener, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 &&
                      listen(fake.listener, 1) == 0;
            int alen            = sizeof(a);
            ok                  = ok && getsockname(fake.listener, reinterpret_cast<sockaddr *>(&a), &alen) == 0;
            HANDLE       thread = ok ? CreateThread(nullptr, 0, fake_host, &fake, 0, nullptr) : nullptr;
            MH_NetConfig cfg{};
            cfg.role        = 1;
            cfg.player_id   = 1;
            cfg.host_assign = 1;
            cfg.port        = ntohs(a.sin_port);
            strcpy(cfg.host, "127.0.0.1");
            cfg.ping_ms       = -1;
            cfg.rx_timeout_ms = -1;
            const int started = thread ? MH_Net_InitEx(&cfg) : 1;
            ok                = ok && thread && started == 0 && pristine() && !MH_Net_IdAssigned();
            if (thread) {
                const DWORD waited = WaitForSingleObject(thread, 5000);
                ok                 = ok && waited == WAIT_OBJECT_0 && fake.spoke;
                if (waited != WAIT_OBJECT_0) {
                    closesocket(fake.listener);
                    fake.listener = INVALID_SOCKET;
                    WaitForSingleObject(thread, INFINITE);
                }
                CloseHandle(thread);
            }
            if (fake.listener != INVALID_SOCKET) closesocket(fake.listener);
            if (!check(ok, "new force client refuses legacy/future/malformed/missing ACK before IDs/start/data")) ++failures;
        }
    }
    MH_Net_SetSessionInfoHandler(nullptr);
    MH_Net_SetJoinHandler(nullptr);
    MH_Net_SetStartHandler(nullptr);
    MH_Net_SetAnnounceHandler(nullptr);
    SetEnvironmentVariableA("MH_KEY_FILE", nullptr);
    return failures;
}
