#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "mh_strategic_bindings.h"
#include "mh_strategic_input_export.h"
namespace mh::harness::strategic_profile {
// Journal envelope 2 carries the exact effective binding-profile binary v2.
// Older profiles require explicit normalization with consumer equivalence evidence, never a runtime shim.
inline constexpr unsigned VERSION    = 2;
inline constexpr size_t   BYTES      = mh::strategic_bindings::journal_profile_bytes;
inline constexpr size_t   LINE_BYTES = 16 + BYTES * 2;
inline int                digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                                              : c >= 'A' && c <= 'F'   ? c - 'A' + 10
                                                                                                       : -1; }
inline bool               metadata(char (&line)[LINE_BYTES]) {
    uint8_t  profile[BYTES];
    unsigned written = 0;
    if (!MH_StrategicBindings_Profile(profile, unsigned(BYTES), &written) || written != BYTES) return false;
    char      *out      = line;
    const char prefix[] = "TJ B 2 ";
    std::memcpy(out, prefix, sizeof(prefix) - 1);
    out += sizeof(prefix) - 1;
    constexpr char hex[] = "0123456789abcdef";
    for (uint8_t byte : profile) {
        *out++ = hex[byte >> 4];
        *out++ = hex[byte & 15];
    }
    *out++ = '\n';
    *out   = 0;
    return true;
}
inline bool preflight(const char *text, size_t length) {
    if (!text) return false;
    uint8_t     profile[BYTES]{};
    bool        found = false, inputs = false;
    const char *line = text, *end = text + length;
    while (line < end) {
        const char *finish = line;
        while (finish < end && *finish != '\n') ++finish;
        const char *last = finish;
        if (last > line && last[-1] == '\r') --last;
        if (last > line && *line == 'B') {
            if (found || inputs || last - line != ptrdiff_t(4 + BYTES * 2) || line[1] != ' ' || line[2] != '2' || line[3] != ' ') return false;
            for (size_t i = 0; i < BYTES; ++i) {
                const int a = digit(line[4 + i * 2]), b = digit(line[5 + i * 2]);
                if (a < 0 || b < 0) return false;
                profile[i] = uint8_t(a * 16 + b);
            }
            // Validate all serialized values, collisions, catalog identity and reserved assignments
            // before asking the DLL to replace its current resolver. No mutation on malformed input.
            mh::strategic_bindings::profile decoded;
            mh::strategic_bindings::error   error;
            if (!mh::strategic_bindings::decode_profile(profile, BYTES, decoded, error)) return false;
            found = true;
        } else if (last - line >= 2 && line[1] == ' ' && std::strchr("MKCSP", line[0])) {
            if (!found) return false;
            inputs = true;
        }
        line = finish < end ? finish + 1 : end;
    }
    if (!found) return false;
    unsigned error = 0;
    return MH_StrategicBindings_AdoptProfile(profile, unsigned(BYTES), &error) != 0;
}
} // namespace mh::harness::strategic_profile
