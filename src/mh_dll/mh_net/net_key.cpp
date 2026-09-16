//
// net_key.cpp -- see include/mh_net_key.h. Loads/creates mh_key.txt next to the exe.
//
// Style matches the rest of mh_common: raw Win32, fixed buffers, no CRT stdio -- this runs inside
// the injected DLL.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "include/mh_net_key.h"
#include "mh_net_proto/net_crypto.h"

namespace {

// RtlGenRandom (advapi32!SystemFunction036) -- the CSPRNG every Windows since XP has, without
// dragging in the CryptoAPI/CNG headers. Resolved dynamically because it is an undocumented-by-name
// export; if it is somehow absent we FAIL rather than fall back to a clock-seeded PRNG, since a
// predictable session key looks secure and is not.
typedef BOOLEAN(WINAPI *PFN_RTLGENRANDOM)(PVOID, ULONG);

bool secure_random(unsigned char *buf, unsigned len) {
    HMODULE h = LoadLibraryA("advapi32.dll");
    if (!h) return false;
    PFN_RTLGENRANDOM gen = (PFN_RTLGENRANDOM)GetProcAddress(h, "SystemFunction036");
    bool             ok  = gen && gen(buf, len);
    FreeLibrary(h);
    return ok;
}

bool read_all(const char *path, char *buf, int cap) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    BOOL  ok  = ReadFile(h, buf, (DWORD)(cap - 1), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    buf[got] = '\0';
    return true;
}

bool write_all(const char *path, const char *text) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, text, lstrlenA(text), &w, nullptr);
    CloseHandle(h);
    return true;
}

// Copy the first MEANINGFUL line (not blank, not a ';' comment) of `text` into `out`. The file we
// generate carries the key on line 1 followed by an explanation, and a human who edits it will add
// or keep comments -- so the value is "the first real line", not "the whole file". key_from_hex
// itself stays strict (it rejects trailing junk), which is what makes a truncated or mangled key an
// error instead of a silently different key.
void first_line(const char *text, char *out, int cap) {
    out[0] = '\0';
    for (const char *p = text; *p;) {
        const char *e = p;
        while (*e && *e != '\r' && *e != '\n') ++e;
        const char *t = p;
        while (t < e && (*t == ' ' || *t == '\t')) ++t;
        if (t < e && *t != ';') {
            int n = (int)(e - t);
            if (n > cap - 1) n = cap - 1;
            for (int i = 0; i < n; ++i) out[i] = t[i];
            out[n] = '\0';
            return;
        }
        p = e;
        while (*p == '\r' || *p == '\n') ++p;
    }
}

// "open" (any case, any surrounding whitespace) is the explicit opt-out marker.
bool says_open(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    const char *w = "open";
    int         i = 0;
    for (; w[i]; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != w[i]) return false;
    }
    for (const char *t = s + i; *t; ++t)
        if (*t != ' ' && *t != '\t' && *t != '\r' && *t != '\n') return false;
    return true;
}

} // namespace

extern "C" int MH_Key_Random(unsigned char *buf, unsigned len) {
    return (buf && secure_random(buf, len)) ? 1 : 0;
}

extern "C" int MH_Key_Load(const char *dir, unsigned char *out_key, char *out_hex, int *out_generated) {
    if (out_generated) *out_generated = 0;
    if (!dir || !out_key || !out_hex) return MH_KEY_INVALID;

    // MH_KEY_FILE overrides the location entirely. Operationally that lets several installs (or a
    // dedicated host box) share one key file instead of copying it around; it is also what lets the
    // auth selftest run two peers with DIFFERENT keys in one directory.
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("MH_KEY_FILE", path, MAX_PATH))
        wsprintfA(path, "%smh_key.txt", dir);

    char text[1024];
    if (read_all(path, text, sizeof(text))) {
        // Skip a UTF-8 BOM. This file gets pasted into Notepad by players, and "Save as UTF-8" there
        // prepends EF BB BF -- which would otherwise read as a corrupt key and refuse to start.
        const char *body = text;
        if ((unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
            body += 3;
        char line[128];
        first_line(body, line, sizeof(line));
        if (says_open(line)) return MH_KEY_OPEN;
        if (mh_net_proto::key_from_hex(line, out_key)) {
            mh_net_proto::key_to_hex(out_key, out_hex);
            return MH_KEY_SECURE;
        }
        return MH_KEY_INVALID; // present but garbage -> fail closed, never silently downgrade
    }

    // First run: mint a key and persist it. The file doubles as the thing the host copy-pastes to
    // its players, so it carries the explanation with it.
    if (!secure_random(out_key, MH_KEY_LEN)) return MH_KEY_INVALID;
    mh_net_proto::key_to_hex(out_key, out_hex);
    char body[1024]; // the explanatory block below is ~600 bytes -- wsprintfA does NOT bounds-check
    wsprintfA(body,
              "%s\r\n"
              "; ^ This line is your multiplayer key. Everyone playing together must have the SAME\r\n"
              "; one, so the host sends this file (or just that line) to the other players and they\r\n"
              "; replace their own mh_key.txt with it. It authenticates and encrypts the connection:\r\n"
              "; without it, anyone who can reach your port can join or crash the game.\r\n"
              ";\r\n"
              "; Replace the whole file with the single word  open  to turn authentication and\r\n"
              "; encryption OFF (LAN testing, or playing with an older build). Do not do that on a\r\n"
              "; port that is reachable from the internet.\r\n",
              out_hex);
    if (!write_all(path, body)) {
        // Unwritable folder (read-only install): the key still works for THIS run, but it would be a
        // different key next launch, so treat it as unusable rather than confusing.
        return MH_KEY_INVALID;
    }
    if (out_generated) *out_generated = 1;
    return MH_KEY_SECURE;
}
