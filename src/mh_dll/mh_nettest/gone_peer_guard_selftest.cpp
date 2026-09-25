//
// gone_peer_guard_selftest.cpp -- mp:U19i, gone_peer_frame_guard's BYTE-PATCH CARRIER, off the rig.
//
// The fix has two carriers now: the promoted dispatch_packet's own save/restore (libmh, proven by
// libmh_selftest.exe lockstest's test_u19g_gone_peer_frame_guard_arm), and mh::gone_peer_guard::thunk
// (mh.dll, spliced over `call llm_net_send_lockstep_kick` @0x0049c330 in the ORIGINAL dispatch, for
// configuration (1)). This suite is the second one's oracle. It drives the REAL naked thunk -- the
// one net_lockstep.cpp installs, not a copy -- with the three things it touches replaced: the buffer
// (a heap array instead of _G_LLM_NET_SEND_BUF), the emitter (a fake that writes the kick's 6-byte
// record exactly where llm_net_send_lockstep_kick would) and the caller's frame (a fake EBP whose
// [ebp-0x38] holds `len`, as llm_net_lockstep_dispatch's does).
//
// The datagram is U19g's: a bare MSG_HORIZON whose byte[6] is 0xff (so a clobbered parse lands in
// handle_garbled), then the sender's own CTL_PLAYER_LEFT. The CONTROL arm calls the fake emitter the
// way the unpatched original does -- straight, no thunk -- and shows the U19e damage: the head of the
// datagram is the kick's record. The CARRIER arm shows the same emit leaving the datagram
// byte-identical. Together those are the both-ways claim; the lockstest arm then owns what the
// parse does with each buffer.
//
#include "seams/gone_peer_guard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gpg = mh::gone_peer_guard;

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// ---- the fake emitter: llm_net_send_lockstep_kick's effect on the shared buffer ----------------
uint8_t *g_target      = nullptr;
int      g_kick_calls  = 0;
int32_t  g_kick_side   = 0;
bool     g_kick_saw_dg = false; // at entry, the buffer still held the datagram (the save did not write it)
uint8_t  g_dg_head[6];

void __cdecl fake_kick_body(int32_t side_id) {
    ++g_kick_calls;
    g_kick_side   = side_id;
    g_kick_saw_dg = g_target && memcmp(g_target, g_dg_head, sizeof(g_dg_head)) == 0;
    if (g_target) {
        // MSG_CONTROL(4), CTL_KICK(10), side_id -- written at cursor 0, as emit_ctrl does on an empty batch
        g_target[0] = 4;
        g_target[1] = 10;
        memcpy(g_target + 2, &side_id, sizeof(side_id));
    }
}

// Watcom-shaped: side_id in EAX, every other register preserved, EAX clobbered on return (a void
// __watcall callee owes the caller nothing in EAX).
// clang-format off
__declspec(naked) void fake_kick() {
    __asm {
        pushad
        push eax
        call fake_kick_body
        add  esp, 4
        popad
        mov  eax, 0xdeadbeef
        ret
    }
}
// clang-format on

// ---- a fake llm_net_lockstep_dispatch frame, and the call the splice makes ------------------------
uint32_t g_frame[32];
uint32_t g_regs_out[4]; // ebx, esi, edi, edx after the call

// C4731 is the point: the call is made with EBP pointing at a FAKE dispatch frame, restored after.
#pragma warning(push)
#pragma warning(disable : 4731)
void call_site(void (*target)(), int32_t side_id, uint32_t len) {
    memset(g_frame, 0, sizeof(g_frame));
    const int slot           = (0x40 + gpg::LEN_EBP_OFF) / 4; // fake ebp = &g_frame + 0x40
    g_frame[slot]            = len;
    const uintptr_t fake_ebp = (uintptr_t)g_frame + 0x40;
    void (*t)()              = target;
    // clang-format off
    __asm {
        push ebx
        push esi
        push edi
        mov  ecx, t
        mov  edx, fake_ebp
        mov  eax, side_id
        push ebp
        mov  ebp, edx
        mov  ebx, 0x11111111
        mov  esi, 0x22222222
        mov  edi, 0x33333333
        mov  edx, 0x44444444
        call ecx
        pop  ebp
        mov  g_regs_out[0], ebx
        mov  g_regs_out[4], esi
        mov  g_regs_out[8], edi
        mov  g_regs_out[12], edx
        pop  edi
        pop  esi
        pop  ebx
    }
    // clang-format on
}
#pragma warning(pop)

bool regs_intact() {
    return g_regs_out[0] == 0x11111111u && g_regs_out[1] == 0x22222222u && g_regs_out[2] == 0x33333333u &&
           g_regs_out[3] == 0x44444444u;
}

} // namespace

int run_gpfgtest() {
    printf("=== gpfgtest (mp:U19i: gone_peer_frame_guard's byte-patch carrier, the real thunk) ===\n");

    // ---- (1) the guard bytes, re-derived rather than trusted ------------------------------------
    {
        int32_t rel;
        memcpy(&rel, gpg::EXPECT + 1, sizeof(rel));
        ck(gpg::EXPECT[0] == 0xE8, "the site is a CALL rel32");
        ck(gpg::SITE + 5 + (uintptr_t)(intptr_t)rel == gpg::KICK,
           "its rel32 lands on llm_net_send_lockstep_kick (0x0049dc16)");
        ck(gpg::EXPECT[5] == 0x8B && gpg::EXPECT[6] == 0x45,
           "the next instruction is `mov eax,[ebp+disp8]` (the len read the thunk relies on)");
        ck((int8_t)gpg::EXPECT[7] == gpg::LEN_EBP_OFF, "...and its disp8 is the offset the thunk reads");
    }

    // The U19g datagram: MSG_HORIZON(2) + 8 payload bytes (byte[6] = 0xff), then CTL_PLAYER_LEFT.
    const uint8_t  dg[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0xff, 0x77, 0x88, 0x04, 0x01};
    const uint32_t dlen = (uint32_t)sizeof(dg);
    static uint8_t buf[gpg::CAPACITY];
    auto           reset = [&]() {
        memset(buf, 0xcd, sizeof(buf));
        memcpy(buf, dg, sizeof(dg));
        memcpy(g_dg_head, dg, sizeof(g_dg_head));
        g_target      = buf;
        g_kick_calls  = 0;
        g_kick_side   = 0;
        g_kick_saw_dg = false;
    };
    uint8_t *const  save_buf  = gpg::g_buf;
    const uintptr_t save_kick = gpg::g_kick;
    gpg::g_buf                = buf;
    gpg::g_kick               = (uintptr_t)&fake_kick;

    // ---- (2) CONTROL: what the unpatched original does -- the call straight to the emitter ------
    reset();
    call_site(&fake_kick, 101, dlen);
    ck(g_kick_calls == 1 && g_kick_side == 101, "control: the kick fires for the gone sender");
    ck(memcmp(buf, dg, sizeof(dg)) != 0, "control: the datagram is DAMAGED (U19e) -- this is the bug");
    ck(buf[0] == 4 && buf[1] == 10 && buf[6] == 0xff,
       "control: head = the kick's own record, byte 6 = the horizon's 0xff (the rig's garbled shape)");

    // ---- (3) CARRIER: the same emit through the thunk -----------------------------------------
    reset();
    const unsigned fires0 = gpg::g_fires;
    call_site(&gpg::thunk, 101, dlen);
    ck(g_kick_calls == 1 && g_kick_side == 101, "carrier: the kick still fires, once, for the same side_id");
    ck(g_kick_saw_dg, "carrier: the emitter ran on the untouched datagram (the save only reads)");
    ck(memcmp(buf, dg, sizeof(dg)) == 0, "carrier: the datagram is byte-identical after the emit");
    ck(buf[dlen] == 0xcd, "carrier: nothing past len was written");
    ck(gpg::g_last_len == dlen, "carrier: len was read from the caller's [ebp-0x38]");
    ck(gpg::g_fires == fires0 + 1, "carrier: the FIRED counter moved (the boot/log evidence)");
    ck(regs_intact(), "carrier: EBX/ESI/EDI/EDX reach the caller exactly as the emitter left them");

    // ---- (4) len SHORTER than the kick's record: parity with the reimpl's n = len ---------------
    // dispatch_packet restores exactly `len` bytes, so bytes [len, 6) keep the kick's record. The
    // carrier must do the same -- restoring more would make the two carriers disagree.
    reset();
    call_site(&gpg::thunk, 0x01020365, 3);
    ck(memcmp(buf, dg, 3) == 0, "short len: [0, len) restored");
    ck(buf[3] == 0x03 && buf[4] == 0x02 && buf[5] == 0x01,
       "short len: [len, 6) still the kick's side_id bytes, as the reimpl leaves them");

    // ---- (5) an oversized len is clamped to the buffer, as the reimpl clamps it -----------------
    reset();
    call_site(&gpg::thunk, 101, 0x10000u);
    ck(gpg::g_last_len == 0x10000u, "clamp: the raw len was read");
    ck(memcmp(buf, dg, sizeof(dg)) == 0 && buf[gpg::CAPACITY - 1] == 0xcd,
       "clamp: the whole CAPACITY restored, nothing written past it");

    gpg::g_buf  = save_buf;
    gpg::g_kick = save_kick;
    g_target    = nullptr;
    printf("=== gpfgtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
