//
// caps/ovf_save.cpp -- the raised-caps save: planet-file trailer, container stamp (patching:PT-CAP8 S3, slice G).
// Contract and rationale: caps/ovf_save.h. Pure: no game address is touched here.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "caps/ovf_save.h"

#include <cstdio>
#include <cstring>

namespace mh::caps::ovf {

namespace {

void (*g_log)(const char *) = nullptr;
uint8_t *g_buf              = nullptr;
bool     g_refused          = false;
uint32_t g_features         = 0;

void say(const char *s) {
    if (g_log) g_log(s);
}

} // namespace

void set_save_logger(void (*fn)(const char *)) { g_log = fn; }

uint8_t *scratch() {
    if (!g_buf) g_buf = static_cast<uint8_t *>(VirtualAlloc(nullptr, max_bytes_save() + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    return g_buf;
}
void scratch_release() {
    if (g_buf) VirtualFree(g_buf, 0, MEM_RELEASE);
    g_buf = nullptr;
}

bool refused_raised() { return g_refused; }
void set_refused_raised(bool v) { g_refused = v; }
bool take_refused_raised() {
    const bool v = g_refused;
    g_refused    = false;
    return v;
}

// ---- save tail --------------------------------------------------------------------------------------------
int save_tail(const file_io &io, void *f, int planet) {
    if (!raised()) return 0; // OFF: not a byte, not a call
    uint8_t *b = scratch();
    if (!b) {
        say("; [caps] save: trailer NOT written (no memory for the encode buffer)\n");
        return -1;
    }
    const size_t n = encode(b, max_bytes_save(), FOR_SAVE);
    if (n == 0) return 0; // nothing past the window: the file stays vanilla
    if (n == ENCODE_FAIL) {
        say("; [caps] save: trailer NOT written (encode refused)\n");
        return -1;
    }
    const uint32_t foot[2] = {static_cast<uint32_t>(n), FOOTER_MAGIC};
    if (io.write(f, b, static_cast<uint32_t>(n)) != n || io.write(f, foot, FOOTER_BYTES) != FOOTER_BYTES) {
        say("; [caps] save: trailer NOT written (short write)\n");
        return -1;
    }
    char       m[200];
    const int  paths = section_records(b, n, KIND_PATH_SLOT);
    ovf_header oh;
    memcpy(&oh, b, sizeof(oh));
    if (oh.version == MHOV_VERSION_1)
        _snprintf_s(m, sizeof(m), _TRUNCATE, "; [caps] save: planet %d trailer %d records (%u bytes) v1\n", planet, record_count(b, n),
                    static_cast<unsigned>(n + FOOTER_BYTES));
    else
        _snprintf_s(m, sizeof(m), _TRUNCATE, "; [caps] save: planet %d trailer %d records (%u bytes) v2 path %d orders %d\n", planet, record_count(b, n),
                    static_cast<unsigned>(n + FOOTER_BYTES), paths, section_records(b, n, KIND_ORDER_QUEUE));
    say(m);
    return static_cast<int>(n + FOOTER_BYTES);
}

// ---- load tail --------------------------------------------------------------------------------------------
namespace {

// Footer at END-8 and its blob start. Leaves the position wherever; the caller restores. false = no footer.
bool read_footer(const file_io &io, void *f, uint32_t *len, int32_t *blob_at) {
    if (io.seek(f, -static_cast<int32_t>(FOOTER_BYTES), 2) != 0) return false;
    const int32_t at = io.tell(f);
    if (at < 0) return false;
    uint32_t foot[2] = {0, 0};
    if (io.read(f, foot, FOOTER_BYTES) != FOOTER_BYTES || foot[1] != FOOTER_MAGIC) return false;
    *len     = foot[0];
    *blob_at = at - static_cast<int32_t>(foot[0]); // may be negative: the caller range-checks
    return true;
}

} // namespace

uint32_t last_load_features() { return g_features; }

int load_tail(const file_io &io, void *f, int32_t *fail) {
    g_features        = 0; // every planet load starts without trailer features
    const int32_t pos = io.tell(f);
    if (pos < 0) return LT_NONE; // cannot tell: behave as the vanilla reader
    int      rc   = LT_NONE;
    uint32_t len  = 0;
    int32_t  from = 0;
    if (read_footer(io, f, &len, &from)) {
        char m[220];
        if (!raised()) {
            int        records = -1;
            ovf_header h       = {};
            if (len >= sizeof(h) && from >= 0 && io.seek(f, from, 0) == 0 && io.read(f, &h, sizeof(h)) == sizeof(h) && h.magic == MHOV_MAGIC)
                records = static_cast<int>(h.records);
            ++*fail;
            set_refused_raised(true);
            _snprintf_s(m, sizeof(m), _TRUNCATE, "; [caps] load REFUSED: planet file carries %d overflow records and [caps] raise is OFF\n", records);
            say(m);
            rc = LT_REFUSED_OFF;
        } else {
            uint8_t *b = (len >= sizeof(ovf_header) && len <= max_bytes_save() && from >= 0) ? scratch() : nullptr;
            int      r = OVF_ERR_TRUNCATED;
            if (b && io.seek(f, from, 0) == 0 && io.read(f, b, len) == len) r = apply(b, len, &g_features);
            if (r == OVF_OK) {
                _snprintf_s(m, sizeof(m), _TRUNCATE, "; [caps] load: planet file overflow %d records applied\n", record_count(b, len));
                say(m);
                rc = LT_APPLIED;
            } else {
                ++*fail;
                _snprintf_s(m, sizeof(m), _TRUNCATE, "; [caps] load REFUSED: overflow trailer rejected (code %d, %u bytes)\n", r, (unsigned)len);
                say(m);
                rc = LT_REFUSED_BAD;
            }
        }
    }
    io.seek(f, pos, 0); // EVERY path
    return rc;
}

bool file_has_footer(const file_io &io, void *f) {
    const int32_t pos = io.tell(f);
    if (pos < 0) return false;
    uint32_t   len  = 0;
    int32_t    from = 0;
    const bool ok   = read_footer(io, f, &len, &from) && len >= sizeof(ovf_header) && len <= max_bytes_save() && from >= 0;
    io.seek(f, pos, 0);
    return ok;
}

// ---- the container stamp ----------------------------------------------------------------------------------
void make_stamp(const uint8_t *slot, uint8_t *out) {
    memcpy(out, slot, VERSION_BYTES);
    memcpy(out + STAMP_OFFSET, "MHC1", 4);
}

bool is_stamp(const uint8_t *hdr, const uint8_t *slot) {
    uint8_t s[VERSION_BYTES];
    make_stamp(slot, s);
    return memcmp(hdr, s, VERSION_BYTES) == 0;
}

bool member_included(const member_probe &mp, int i) {
    return mp.system_of(mp.ctx, i) == mp.current_system && (mp.status_of(mp.ctx, i) != 0 || mp.current_planet == i);
}

bool stamp_needed(const member_probe &mp) {
    if (!raised()) return false;
    for (int i = FIRST_MEMBER; i <= LAST_MEMBER; ++i)
        if (member_included(mp, i) && mp.footer_of(mp.ctx, i)) return true;
    return false;
}

int version_check(uint8_t *hdr, const uint8_t *slot) {
    g_refused = false;
    if (!is_stamp(hdr, slot)) return VC_PASS;
    if (raised()) {
        memcpy(hdr, slot, VERSION_BYTES);
        say("; [caps] load: raised save accepted\n");
        return VC_ACCEPTED;
    }
    g_refused = true;
    say("; [caps] load REFUSED: the save is a raised-caps save and [caps] raise is OFF\n");
    return VC_REFUSED;
}

} // namespace mh::caps::ovf
