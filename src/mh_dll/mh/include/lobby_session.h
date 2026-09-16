#pragma once
#include <cstdint>

//
// mh.exe's `/llm llm_net_session_entry` -- one record in the retail lobby "session list"
// (_G_LLM_NET_LOBBY_SESSION_LIST @0x5d5368, count @0x5d54b0) that llm_lobby_host_net_dispatch's
// admin loop drains each frame: per entry it announces + allocates/removes a lobby slot. Byte-packed,
// stride 0x29 (player_id/aux_value sit at odd offsets -- no padding). Field names/offsets mirror the
// Ghidra type exactly; see the Extermination cross-version reference for the full JOIN/LEFT/DROP delta lifecycle (recovered
// from extermin's live twin). mp_sync_host_peer_table is the DLL's stand-in for the dead retail
// roster-pump: it WRITES these delta records; the game CONSUMES them (it owns slot alloc/removal).
//
#pragma pack(push, 1)
struct llm_net_session_entry {
    char    name[0x20]; // 0x00 ANSI display name, null-padded (the admin loop announces off this)
    uint8_t event_tag;  // 0x20 1 = LEFT (departure); 2 = JOINED. The loop only tests == 1 (LEFT) vs not.
    int32_t player_id;  // 0x21 network player id
    int32_t aux_value;  // 0x25 net_udp per-player datapack length; game gates >= 2 accepted / < 2 kick
};
#pragma pack(pop)

static_assert(sizeof(llm_net_session_entry) == 0x29,
              "llm_net_session_entry must match the game's 0x29-byte session-list record stride");
