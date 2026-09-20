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
// AND BOTH PEERS MUST AGREE. A pinned codepage is a property of the SESSION, not of a machine: if
// the host encodes `П` as 0xCF under CP1251 and the joiner renders 0xCF under CP1252, the chat line
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
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Arm the three hooks. Reads `[input] codepage` from mh_net.ini. Returns 1 if the seam is live
// (every hook that could be installed was), 0 if it disarmed -- best-effort, like every other seam:
// a refused install leaves retail behaviour, it never fails the process.
int MH_ChatInput_Install(void);

// The codepage this peer has pinned, as advertised in SESSION_INFO / JOIN. Resolves to GetACP() if
// the seam never armed, so a caller never has to ask whether it did -- "no pin" and "pinned to the
// ACP" are the same statement about the wire.
unsigned int MH_ChatInput_Codepage(void);

// mp:F3c -- THE SESSION IS THE HOST'S. A joiner ADOPTS the host's codepage (from SESSION_INFO) at
// its JOIN and gets its own back when the session closes; the refusal F3 introduced then only
// remains for a peer that cannot switch. `OwnCodepage` is this peer's own resolution regardless of
// any adoption (the ini pin or the ACP); `Codepage` above answers the adopted value while one is
// live. `AdoptCodepage` returns 1 if the three hooks and the wire now use `cp` (or already did), 0
// if this peer will not or cannot switch -- `[input] codepage_adopt=0` or a codepage Windows has not
// installed -- in which case the caller sends its own value and the host refuses it by name.
// `RestoreCodepage` is idempotent. `Transcode` re-encodes a string typed under one codepage so it
// means the same characters under another (the player name, typed before the JOIN).
unsigned int MH_ChatInput_OwnCodepage(void);
int          MH_ChatInput_AdoptCodepage(unsigned int cp);
void         MH_ChatInput_RestoreCodepage(void);
void         MH_ChatInput_Transcode(char *s, int cap, unsigned int from, unsigned int to);

// Diagnostics for the offline arms: characters the codec resolved, characters it produced through
// the chat seam, and characters the pinned codepage could not represent (those fall back to the
// game's own scancode table and are counted, never silently dropped).
void MH_ChatInput_Stats(long *resolved, long *chat_chars, long *unmappable);

#ifdef __cplusplus
}
#endif
