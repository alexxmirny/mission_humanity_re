//
// config/ini_read.h -- TL-HARN4: the one place a hand-written ini's same-line `;comment` is
// stripped from a STRING read.
//
// GetPrivateProfileStringA already does the right thing for a `;`-led COMMENT LINE (the whole
// line is skipped, standard ini behaviour) and for a NUMERIC read (atoi/atof/strtol/
// GetPrivateProfileIntA all stop at the first non-numeric byte, so a trailing ` ;comment` is
// simply never parsed). What it does NOT do is strip a trailing same-line comment from the
// STRING it returns -- `key=value ;comment` reads back as the literal bytes `value ;comment`,
// comment and all. mh_net.example.ini documents every key with exactly such a trailing comment
// (that is the whole point of writing an example file), so a verbatim copy of it is booby-trapped
// for every STRING key: two sites used to fail CLOSED on it ([net] module, [config] mode -- both
// TERMINATE the process on an unrecognised value) and one used to fail open silently (`[net]
// relay=<vps>:7100 ; comment` dialled comment-and-all, 2026-09-19, before mp:R7a hand-fixed that
// one site). This header is the durable fix -- ONE helper, routed through every
// GetPrivateProfileStringA call in src/mh_dll, instead of a per-key patch.
//
// WHAT A `;` MEANS INSIDE A VALUE: nothing, by inspection of every key that reads through this
// helper (TL-HARN4 audit, 2026-09-21) -- hostnames/IPs, ports, hex colors, coordinate pairs,
// hotkey names, file paths under this repo, function-name lists, and free-text probe strings.
// None of them is ever written quoted, and `;` is not a legal byte in any of those domains, so
// cutting at the first UNCONDITIONAL `;` (no quoting rule needed) is safe for the whole roster.
// If a future key genuinely needs a literal `;` in its value, it needs its own escape convention
// and must NOT be routed through this helper -- that is a design decision for that key, not a
// special case to bolt on here.
//
// mh::config, not mh::config::detail: unlike the mode selector this is a plain utility with no
// STANDALONE-vs-hosted distinction (MH_LIBMH_BUILD sources call GetPrivateProfileStringA directly
// already -- e.g. libmh/lockstep/turn_engine.cpp's `[bisect] lockstep_seams` diagnostic read -- so
// this header includes <windows.h> unconditionally rather than only under config.h's `#ifndef
// MH_LIBMH_BUILD` half).
//
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mh::config {

// Strip a trailing same-line `;comment` (and the whitespace immediately before it) from `s` IN
// PLACE. Returns true if it actually cut something, so a caller that wants to log the before/
// after (net_discovery.cpp's mp:R7a relay notice) can do so without re-deriving "did it strip".
inline bool strip_ini_comment(char *s) {
    for (char *p = s; *p != '\0'; ++p) {
        if (*p != ';') continue;
        char *end = p;
        while (end > s && (end[-1] == ' ' || end[-1] == '\t')) --end;
        *end = '\0';
        return true;
    }
    return false;
}

// GetPrivateProfileStringA, with a trailing same-line `;comment` stripped from the STRING it
// returns (see strip_ini_comment above). Drop-in replacement for the raw API at a STRING read
// site -- same signature, same return value (the length actually written, post-strip).
//
// STRIPS WHATEVER ENDS UP IN `out`, not only bytes that came from the ini -- if the key is absent,
// GetPrivateProfileStringA copies `def` into `out` verbatim and this still scans that copy. None of
// the ~40 sites routed through this helper (TL-HARN4) write a `;` into their own compiled-in
// default, so this never fires in practice; it is simpler and more predictable than trying to strip
// only a FILE-sourced value, and a caller whose default genuinely needs a literal `;` should not use
// this helper for that key (call GetPrivateProfileStringA directly, as net_diag.cpp's `[trace]
// funcs` already does for an unrelated reason -- `;` is one of ITS key's own valid separators).
inline DWORD read_ini_string(const char *section, const char *key, const char *def, char *out,
                             DWORD out_cap, const char *ini) {
    DWORD n = GetPrivateProfileStringA(section, key, def, out, out_cap, ini);
    if (strip_ini_comment(out)) n = (DWORD)lstrlenA(out);
    return n;
}

} // namespace mh::config
