# mh_net_proto

Portable, cross-platform networking **protocol** library for the MH multiplayer restoration — the
shared foundation both the Windows injected DLL and the (future) headless-Linux relay link.

## What it is
**Pure logic only:** message formats, wire framing, and (de)serialization. It has **no platform or
socket dependencies** — no `<windows.h>`, no `<winsock2.h>`/`<sys/socket.h>`, no Boost. Each side
provides its own thin socket adapter and calls into this library to encode/decode:

- **Windows DLL** (`../mh_dll/mh_common/net_transport.cpp`) — WinSock adapter.
- **Relay** (headless Ubuntu, later) — Boost.Asio / POSIX adapter.

This is why the shared code lives here and not in the DLL: the relay can't use WinAPI, so anything
both sides need must be platform-neutral. The **MP-DISCOVERY** work (S0–S6) is tracked with the multiplayer restoration.

## Contents (grows with the discovery layer)
- `net_wire.{h,cpp}` — the 12-byte routing frame header (`magic/flags/src/dst/len`) + little-endian
  encode/decode. Byte-compatible with the transport's original x86 packed-struct layout, so existing
  peers interoperate unchanged. This is what the relay reads to route without parsing game payloads.
- `session_info.{h,cpp}` — **SESSION_INFO** (game name, random tag, host/protocol version, cur/max
  players, map) + its (de)serialization, and the **lobby-id** (`name + tag`) equality/format helpers.
  Carries identity, NOT address (no host IP — see the design note in the header). This is the record a
  host advertises (S2) and a client lists in the browser (S3/S5); the relay demuxes by lobby-id.
- `src/byteio.h` — private little-endian read/write helpers.
- *(next: join-by-lobby-id (S4), player identity (S6).)*

## Tests
`test/net_proto_test.cpp` — round-trips (wire header + SESSION_INFO), malformed-input rejection, and
lobby-id dedup. Runs via CTest: `cmake -S . -B build && cmake --build build && ctest --test-dir build`.

## Building
- **Windows:** `mh_net_proto.vcxproj` (static lib) is part of `../mh_dll/mh.sln`; the `mh` DLL links it.
- **Linux (relay):** `cmake -S . -B build && cmake --build build` → `libmh_net_proto.a`.

The two build systems compile the **same** sources; keep both in sync when adding files.
