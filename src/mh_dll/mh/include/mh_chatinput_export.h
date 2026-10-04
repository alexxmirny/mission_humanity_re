//
// mh_chatinput_export.h -- mp:F3: NON-ENGLISH TYPED INPUT.
//
// THE DEFECT, IN TWO HALVES.
//
// (a) THE CODEC. `llm_input_key_dequeue_translate_ascii` (0x004cb91e) -- the translate step every
//     menu and lobby text field goes through -- resolves a keystroke with
//     `MapVirtualKeyA(sc,1)` -> `GetKeyboardState` -> `ToAscii`. ToAscii converts the layout's
//     UTF-16 result to a byte through the PROCESS ANSI codepage, not through the keyboard layout's.
//     On a CP1252 box with a Russian layout active that is a guaranteed loss: measured on the rig
//     (mp:F4, dead-ends G192 / the `type_cyrillic` scenario), `ToAsciiEx(vk,0,st,out,0,ru_hkl)`
//     returns 0xCF for U+041F while `ToAscii(vk,0,st,out,0)` returns 0x3F '?' -- same machine, same
//     layout, same virtual key. Six Cyrillic keystrokes stored `3f 3f 3f 3f 3f 3f`.
//
// (b) THE CHAT SWITCH. `llm_ui_chat_input_process_scancodes` (0x00414a4e) never reaches a codec at
//     all. The in-game chat's producers (`llm_strat_input_update`, `llm_net_chat_input_process`)
//     append RAW SCANCODES to a 40-byte queue at `_G_LLM_STRAT_CHAT_INPUT_LINE + 0x29`, and this
//     2246-byte function drains it through a hardcoded US-QWERTY `switch` -- 0x10 is always 'q',
//     0x1e is always 'a'. A Russian or Polish layout is not merely mis-encoded there, it is not
//     consulted. That is why the chat needs a seam and not just a codepage.
//
// THE FIX: ONE CODEC, THREE HOOKS, AND A PINNED CODEPAGE.
//
//   translate_sc(sc) = MapVirtualKeyExA(sc, MAPVK_VSC_TO_VK, hkl)
//                    -> ToUnicodeEx(vk, sc, keystate, hkl)          [the LAYOUT decides the char]
//                    -> WideCharToMultiByte(PINNED codepage)        [WE decide the byte]
//
//   H1  llm_input_key_dequeue_translate_ascii (0x004cb91e)  whole-body replace. The codec replaces
//       the ToAscii step; EVERYTHING ELSE IS THE ORIGINAL LINE FOR LINE, including the scancode
//       fallback table at 0x0065fb08/09 that carries the arrow/function keys the menus navigate
//       with. A key the codec cannot resolve falls into that table exactly as it does today.
//   H2  llm_ui_chat_input_process_scancodes (0x00414a4e)  run-before. For each queued scancode in
//       the TYPEWRITER BLOCK (the only ranges whose `switch` arms produce a character -- see
//       sc_is_typewriter) the seam inserts the codec's byte itself; every other scancode is handed
//       to the ORIGINAL BODY one at a time, so backspace, Enter, Esc, Delete, the two cursor keys
//       and the two history keys keep their exact retail behaviour and their exact ORDER relative
//       to the typed characters. Nothing of the control half is reimplemented.
//   H3  llm_str_ansi_to_wide (0x004cf379)  whole-body replace: the same pinned codepage on the
//       ANSI->UTF-16 widen every piece of game text is DISPLAYED through. Without it a correctly
//       stored CP1251 0xCF would still be widened as CP_ACP and land on U+00CF 'Ï' -- the right
//       byte drawn as the wrong glyph. H3 is what makes the F2 merged Cyrillic glyphs reachable.
//
// WHY PIN A CODEPAGE RATHER THAN GO UNICODE. The game's chat payload is `char[81]`, the lobby name
// is `char[32]`, the savegame and the wire carry those bytes verbatim, and the lockstep hash covers
// state those buffers sit in. Widening the storage is a format break; the ruling (plan D17) is KEEP
// the byte-level codec and make the codepage EXPLICIT instead of ambient. `[input] codepage`:
//
//     absent / `acp` / `0`  -> GetACP(), i.e. exactly today's behaviour (the ship default)
//     1250 1251 1252 ...    -> that codepage, on all three hooks, on this peer
//
// AND BOTH PEERS MUST AGREE (F3 -- SUPERSEDED for chat by MP-LANG, below). A pinned codepage was
// a property of the SESSION: if the host encodes `П` as 0xCF under CP1251 and the joiner renders 0xCF under CP1252, the chat line
// is silently corrupt and no error is raised anywhere. So the value is advertised in SESSION_INFO
// (v4) and echoed in JOIN (v4), and `mh_net_proto::join_admit()` REFUSES a mismatch by name
// (`JoinAdmit::RefusedCodepage`). That is the negative case: two peers that cannot agree about what
// a byte means do not start a match together.
//
// WHAT CHANGES ON A US/ASCII SETUP: nothing. ToUnicodeEx and ToAscii agree on every ASCII character
// of the US layout, and with the codepage at the process ACP the encode step is the identity ToAscii
// already performed. The `type_ascii` baseline is untouched by design and by measurement.
//
// ONE DELIBERATE DIFFERENCE, recorded rather than preserved: the original chat switch maps scancode
// 0x29 (the US backtick key) to '~' UNSHIFTED and '`' SHIFTED -- inverted with respect to every US
// layout. H2 routes that key through the layout like all the others, so it now types '`' and
// shift-'~'. The menu fields were always correct here (they went through ToAscii); this aligns the
// chat with them.
//
// MP-LANG (2026-09-29): LANGUAGE-AGNOSTIC MULTIPLAYER. The pinned-codepage design above could only
// ever agree on ONE codepage per session (F3c made the joiner adopt the host's), so a CP1251 player
// and a CP1250 player could not both type their own letters, and a CP1251 language pack's resource
// text had to share its codepage with the chat. The ruling: MP is language-agnostic INCLUDING chat;
// player names are Latin letters and digits. So the text is split by KIND, not by machine:
//
//   CHAT   is UTF-8 -- H2 encodes ToUnicodeEx's code point as 1..3 UTF-8 bytes into the same char[81]
//          line (room for the WHOLE character or none of it, so the line is never torn), the editing
//          keys the original handles (backspace / delete / left / right) are repeated for the length
//          of the sequence they step over, and H3 DECODES a chat source as UTF-8: the in-game edit
//          line (its draw and both local echoes), the lobby RX chat text, and the original lockstep
//          dispatch's received-chat widen (found by its call site). The promoted dispatch (libmh
//          rx_dispatch.cpp) decodes with the same codec itself. H4 re-expresses the chat caret's byte
//          index in UTF-16 units (llm_gfx_font_measure_text_width has that one caller).
//   LOBBY CHAT (follow-up, 2026-09-29) is UTF-8 too. It is a menu edit field, so H1 carries it: for a
//          keystroke the pump latches inside the edit tick while the lobby chat widget is being edited,
//          a non-ASCII character is inserted as its whole UTF-8 sequence (or not at all), and backspace /
//          delete / left / right step over a whole multi-byte character -- H1 applies those edits itself
//          and hands the tick a neutralized event; ASCII and every other key stay the original's. The
//          caret x is re-measured over the decoded prefix. H3 decodes the buffer (anywhere inside it --
//          the draw widens from its scroll start and from the cursor) and, while that field is open, its
//          Esc copy. H5 replaces the field draw's line fitter (llm_ui_text_wrap_find_break_ansi, one
//          caller), which treated a byte offset as a unit index both ways. H6 replaces
//          utils_WideStringToAscii for a lobby chat destination: on Enter the tick widens the buffer
//          and llm_ui_widget_set_text narrows it BACK into the buffer through CP_ACP -- measured to
//          reduce the line to its CP1252 residue at the moment it was sent -- so that narrow is UTF-8
//          too. The wire copy is the buffer itself (MH_ChatInput_LobbyToWire); if H1, H5 or H6 cannot
//          arm, the field falls back to the local codepage and the wire copy is converted, as before.
//   NAMES  are [A-Za-z0-9] -- H1 refuses any other printable keystroke while the pending edit field's
//          buffer is the player name or the game name; the first-run Polish placeholders become
//          "Player" / "Game"; the join path normalizes forged names (net_discovery.cpp, net_seams.cpp).
//   NO LATIN STAND-INS. A character the layout produces but this peer's codepage cannot hold (Cyrillic
//          under CP1252) types NOTHING in a menu field. It used to fall back to the scancode table --
//          the US-QWERTY map -- and type the Latin letter on that key (П -> 'g'), which in a name field
//          then passed the [A-Za-z0-9] rule. The table is still used for keys that produce no character.
//   EVERYTHING ELSE (resource text, Msgs.dat, map names, menu fields) keeps THIS PEER'S 8-bit
//          `[input] codepage` -- a purely local setting now, never advertised and never adopted.
//
// The wire says so: SESSION_INFO.codepage / JOIN.codepage carry CHAT_ENCODING_UTF8 (65001), so a
// pre-MP-LANG peer (whose value is its 8-bit codepage) is refused by name through the unchanged
// join_admit() equality test. `[input] chat_legacy_codepage=1` makes THIS peer behave like one on the
// wire -- a test emulation that exists only to be refused.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Arm the six hooks. Reads `[input] codepage` and `[input] chat_legacy_codepage` from mh_net.ini.
// Returns 1 if the seam is live (every hook that could be installed was), 0 if it disarmed --
// best-effort, like every other seam: a refused install leaves retail behaviour, it never fails the
// process.
int MH_ChatInput_Install(void);

// This peer's 8-bit codepage for everything that is NOT chat (resource text, menu fields). Resolves to
// GetACP() if the seam never armed. Local only since MP-LANG -- nothing adopts or advertises it.
unsigned int MH_ChatInput_Codepage(void);

// What SESSION_INFO / JOIN carry: CHAT_ENCODING_UTF8 (65001), or this peer's codepage under the
// `chat_legacy_codepage=1` emulation.
unsigned int MH_ChatInput_WireEncoding(void);

// The lobby chat line as the wire carries it: UTF-8, at most cap-1 bytes, cut on a sequence boundary,
// NUL-terminated. The field normally already holds UTF-8 (a copy); if its UTF-8 path could not arm it
// holds this peer's codepage and is converted. Returns the byte count. (Legacy emulation: a copy.)
int MH_ChatInput_LobbyToWire(const char *src, char *out, int cap);

// Normalize the two local name globals (player name, game name) to [A-Za-z0-9] in place; the first-
// run placeholders become "Player" / "Game". Returns a bitmask of what changed (1 = player, 2 = game).
int MH_ChatInput_NormalizeLocalNames(void);

// Normalize any received name buffer in place (cap bytes incl. NUL; empty-after-normalize -> "Player").
// Returns 1 iff it changed.
int MH_ChatInput_NormalizeName(char *s, int cap);

// TEST ONLY: `[input] test_join_name=<text | hex:..>` -- the bytes the JOIN carries INSTEAD of the local
// name, un-normalized, so the host-side normalization of a forged name can be staged on the rig (the
// name_forged row). NULL when unset, which is every real configuration.
const char *MH_ChatInput_TestJoinName(void);

// Diagnostics for the offline arms: characters the codec resolved, characters it produced through
// the chat seam, and characters the codepage could not represent in a menu field (those type nothing
// -- never the scancode table's Latin letter -- and are counted, never silently dropped).
void MH_ChatInput_Stats(long *resolved, long *chat_chars, long *unmappable);
// MP-LANG diagnostics: widens decoded as UTF-8, chat characters refused for lack of room, and
// keystrokes refused in a name field.
void MH_ChatInput_LangStats(long *utf8_widens, long *chat_full, long *name_rejected);

#ifdef __cplusplus
}
#endif
