//
// desync/state_compress.cpp -- mp:D46, gzip a finished state recording (state_compress.h).
//
#include "desync/state_compress.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

// miniz 3.0.2 (MIT, src/mh_dll/include/miniz/LICENSE.txt): the library is compiled as C by state_miniz.c;
// only its header is seen here, configured identically (state_miniz_config.h).
#include "desync/state_miniz_config.h"
#pragma warning(push, 0)
#include "../../include/miniz/miniz.h"
#pragma warning(pop)

namespace mh::desync::gz {
namespace {

constexpr size_t   IO_CHUNK     = 1u << 20;     // raw reads
constexpr size_t   OUT_BUF      = 256u * 1024u; // gzip writes
constexpr int      LEVEL        = 6;
constexpr unsigned PATH_MAX_LEN = MAX_PATH + 16;

volatile LONG g_active = 0;

double ms_since(const LARGE_INTEGER &t0) {
    LARGE_INTEGER t1, hz;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    return hz.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart : 0.0;
}

// ---- the deflate output sink: a buffered file writer -----------------------------------------------
struct sink {
    HANDLE   h;
    uint8_t *buf;
    size_t   n;
    uint64_t total;
    bool     bad;
};

bool sink_flush(sink &s) {
    if (s.n == 0 || s.bad) return !s.bad;
    DWORD w = 0;
    if (!WriteFile(s.h, s.buf, (DWORD)s.n, &w, nullptr) || w != (DWORD)s.n) {
        s.bad = true;
        return false;
    }
    s.n = 0;
    return true;
}

bool sink_put(sink &s, const void *p, size_t len) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    while (len && !s.bad) {
        const size_t room = OUT_BUF - s.n;
        const size_t k    = len < room ? len : room;
        memcpy(s.buf + s.n, b, k);
        s.n += k;
        s.total += k;
        b += k;
        len -= k;
        if (s.n == OUT_BUF && !sink_flush(s)) return false;
    }
    return !s.bad;
}

mz_bool put_cb(const void *p, int len, void *user) {
    return sink_put(*static_cast<sink *>(user), p, (size_t)len) ? MZ_TRUE : MZ_FALSE;
}

void le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// ---- verify: re-inflate the temp file and compare what it holds with what was read ------------------
// Returns null when the gzip member is whole and says the right CRC-32 and length, else why not.
const char *verify_gz(const char *path, uint64_t want_raw, uint32_t want_crc) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return "cannot reopen the compressed temp file to verify it";
    tinfl_decompressor *d    = static_cast<tinfl_decompressor *>(malloc(sizeof(tinfl_decompressor)));
    uint8_t            *in   = static_cast<uint8_t *>(malloc(IO_CHUNK));
    uint8_t            *dict = static_cast<uint8_t *>(malloc(TINFL_LZ_DICT_SIZE));
    const char         *why  = nullptr;
    if (!d || !in || !dict) {
        why = "out of memory verifying";
    } else {
        tinfl_init(d);
        size_t   have = 0, pos = 0, dict_ofs = 0;
        bool     eof = false, first = true, done = false;
        uint32_t crc = (uint32_t)mz_crc32(0, nullptr, 0);
        uint64_t out = 0;
        while (!why && !done) {
            if (pos == have && !eof) {
                DWORD got = 0;
                if (!ReadFile(h, in, (DWORD)IO_CHUNK, &got, nullptr)) {
                    why = "read error verifying the compressed temp file";
                    break;
                }
                have = got;
                pos  = 0;
                if (got == 0) eof = true;
                if (first && got) {
                    // RFC 1952 header as written below: 1f 8b 08 00, mtime 0, xfl 0, os 255.
                    static const uint8_t HDR[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
                    if (got < 10 || memcmp(in, HDR, 10) != 0) {
                        why = "the compressed temp file has no gzip header";
                        break;
                    }
                    pos   = 10;
                    first = false;
                }
            }
            size_t             in_bytes  = have - pos;
            size_t             out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
            const tinfl_status st =
                tinfl_decompress(d, in + pos, &in_bytes, dict, dict + dict_ofs, &out_bytes, eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
            pos += in_bytes;
            if (out_bytes) {
                crc = (uint32_t)mz_crc32(crc, dict + dict_ofs, out_bytes);
                out += out_bytes;
                dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (st == TINFL_STATUS_DONE) {
                done = true;
            } else if (st < 0) {
                why = "the compressed temp file does not inflate";
            } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && eof) {
                why = "the compressed temp file ends inside the deflate stream";
            }
        }
        if (!why) {
            // The 8-byte trailer: CRC-32 and length mod 2^32 of the uncompressed data. It may straddle
            // what was left in the buffer and the rest of the file.
            uint8_t tr[9];
            size_t  tn = 0;
            while (pos < have && tn < sizeof(tr)) tr[tn++] = in[pos++];
            while (tn < sizeof(tr)) {
                DWORD got = 0;
                if (!ReadFile(h, tr + tn, (DWORD)(sizeof(tr) - tn), &got, nullptr) || got == 0) break;
                tn += got;
            }
            if (tn != 8) {
                why = "the compressed temp file has no 8-byte gzip trailer (or trailing bytes)";
            } else {
                uint32_t tcrc = (uint32_t)tr[0] | ((uint32_t)tr[1] << 8) | ((uint32_t)tr[2] << 16) | ((uint32_t)tr[3] << 24);
                uint32_t tlen = (uint32_t)tr[4] | ((uint32_t)tr[5] << 8) | ((uint32_t)tr[6] << 16) | ((uint32_t)tr[7] << 24);
                if (out != want_raw || crc != want_crc) why = "the re-inflated bytes differ from the raw file";
                else if (tcrc != want_crc || tlen != (uint32_t)want_raw) why = "the gzip trailer disagrees with the raw file";
            }
        }
    }
    free(d);
    free(in);
    free(dict);
    CloseHandle(h);
    return why;
}

} // namespace

void compress_file(const char *raw_path, result *r) {
    *r = result{};
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    char gz[PATH_MAX_LEN], tmp[PATH_MAX_LEN + 8];
    if (strlen(raw_path) + 8 >= PATH_MAX_LEN) {
        r->why = "path too long";
        return;
    }
    wsprintfA(gz, "%s.gz", raw_path);
    wsprintfA(tmp, "%s.gz.tmp", raw_path);

    HANDLE in = CreateFileA(raw_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        r->why = "cannot open the raw file";
        return;
    }
    // CREATE_ALWAYS truncates a temp file a killed earlier run left behind.
    HANDLE out = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        r->why = "cannot create the .gz.tmp file";
        return;
    }
    tdefl_compressor *c   = static_cast<tdefl_compressor *>(malloc(sizeof(tdefl_compressor)));
    uint8_t          *buf = static_cast<uint8_t *>(malloc(IO_CHUNK));
    sink              s   = {out, static_cast<uint8_t *>(malloc(OUT_BUF)), 0, 0, false};
    uint32_t          crc = (uint32_t)mz_crc32(0, nullptr, 0);
    uint64_t          raw = 0;
    const char       *why = nullptr;

    if (!c || !buf || !s.buf) {
        why = "out of memory";
    } else if (tdefl_init(c, put_cb, &s, (int)tdefl_create_comp_flags_from_zip_params(LEVEL, -15 /* raw deflate: no zlib header */, 0)) !=
               TDEFL_STATUS_OKAY) {
        why = "deflate init failed";
    } else {
        static const uint8_t HDR[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
        sink_put(s, HDR, sizeof(HDR));
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(in, buf, (DWORD)IO_CHUNK, &got, nullptr)) {
                why = "read error on the raw file";
                break;
            }
            if (got == 0) break;
            crc = (uint32_t)mz_crc32(crc, buf, got);
            raw += got;
            if (tdefl_compress_buffer(c, buf, got, TDEFL_NO_FLUSH) == TDEFL_STATUS_PUT_BUF_FAILED) {
                why = "write error on the .gz.tmp file";
                break;
            }
        }
        if (!why) {
            if (tdefl_compress_buffer(c, nullptr, 0, TDEFL_FINISH) == TDEFL_STATUS_PUT_BUF_FAILED) {
                why = "write error on the .gz.tmp file";
            } else {
                uint8_t tr[8];
                le32(tr, crc);
                le32(tr + 4, (uint32_t)raw);
                sink_put(s, tr, sizeof(tr));
                if (!sink_flush(s) || s.bad) why = "write error on the .gz.tmp file";
            }
        }
    }
    CloseHandle(in);
    // Flush to the platter before the rename can make this the file of record.
    if (!why && !FlushFileBuffers(out)) why = "FlushFileBuffers failed on the .gz.tmp file";
    CloseHandle(out);
    free(c);
    free(buf);
    free(s.buf);
    if (!why && raw == 0) why = "the raw file is empty";
    if (!why) why = verify_gz(tmp, raw, crc);
    if (!why && !MoveFileExA(tmp, gz, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        why = "cannot rename the .gz.tmp file to .gz";
    if (why) {
        DeleteFileA(tmp);
        r->why = why;
        r->raw = raw;
        r->ms  = ms_since(t0);
        return;
    }
    // The compressed file is complete, verified and under its final name; only now does the raw go.
    r->raw  = raw;
    r->comp = s.total;
    r->ok   = true;
    if (!DeleteFileA(raw_path)) r->why = "compressed, but the raw file could not be deleted (both kept)";
    r->ms = ms_since(t0);
}

namespace {

struct job {
    char   path[PATH_MAX_LEN];
    log_fn log;
};

DWORD WINAPI compress_main(LPVOID arg) {
    job *j = static_cast<job *>(arg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    result r;
    compress_file(j->path, &r);
    if (j->log) {
        if (r.ok)
            j->log("; [desync] STATE RECORD compressed %s: %llu raw bytes -> %llu gzip bytes (%.1f%%), %.0f ms (deflate "
                   "level %d, verified by re-inflating; the raw file is %s)\n",
                   j->path, (unsigned long long)r.raw, (unsigned long long)r.comp,
                   r.raw ? 100.0 * (double)r.comp / (double)r.raw : 0.0, r.ms, LEVEL,
                   r.why ? "KEPT" : "removed");
        else
            j->log("; [desync] STATE RECORD compress FAILED for %s after %.0f ms: %s -- the raw file is kept as it is\n",
                   j->path, r.ms, r.why ? r.why : "?");
    }
    free(j);
    InterlockedDecrement(&g_active);
    return 0;
}

} // namespace

void compress_async(const char *raw_path, log_fn log) {
    job *j = static_cast<job *>(malloc(sizeof(job)));
    if (!j) return;
    if (strlen(raw_path) >= sizeof(j->path)) {
        free(j);
        return;
    }
    strcpy_s(j->path, raw_path);
    j->log = log;
    InterlockedIncrement(&g_active);
    HANDLE t = CreateThread(nullptr, 0, compress_main, j, 0, nullptr);
    if (!t) {
        InterlockedDecrement(&g_active);
        if (log) log("; [desync] STATE RECORD compress thread not started (err %lu): the raw file is kept\n", GetLastError());
        free(j);
        return;
    }
    CloseHandle(t);
}

bool wait_idle(unsigned ms) {
    const DWORD t0 = GetTickCount();
    while (g_active != 0) {
        if (GetTickCount() - t0 > ms) return false;
        Sleep(5);
    }
    return true;
}

} // namespace mh::desync::gz
