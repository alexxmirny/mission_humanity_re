//
// desync/state_compress.h -- mp:D46: gzip a finished state recording on a background thread.
//
// THE RULE (docs/state-record.md "The compressed file"). Once a match's mh_match_state.bin is closed,
// a low-priority thread streams it through deflate (level 6, 1 MiB reads, no whole-file buffer) into
// `<raw>.gz.tmp`, then VERIFIES that temp file by re-inflating it (CRC-32 and byte count must equal
// what was read from the raw file), flushes it to disk, renames it to `<raw>.gz` and only then
// deletes the raw file. The file is a plain gzip member (RFC 1952), so Python's `gzip` module, gzip
// and 7-zip read it as is; the payload is the unchanged docs/state-record.md v1 byte stream.
//
// WHY A QUIT OR CRASH CANNOT LEAVE ONLY A TRUNCATED FILE. The raw file is the last thing touched,
// after the compressed one is complete, verified and under its final name. A process that exits
// anywhere before that leaves the raw file whole (and a `.gz.tmp` that no reader looks at -- its
// name matches neither `*.bin` nor `*.bin.gz`; the next compress of that name truncates it). A
// process that dies between the rename and the delete leaves both, each complete. So at every
// instant at least one complete file exists, with no join at shutdown: ExitProcess kills the thread,
// and the accepted cost is that a match followed by an immediate quit keeps its recording raw
// (user ruling 2026-10-04). A bounded join would only delay the quit by seconds and could not make
// a 240 MB file finish.
//
// The compressed bytes depend only on the raw bytes and the library, never on timing, so a rerun on
// the same raw file gives the same `.gz`.
//
#pragma once
#include <cstdint>

namespace mh::desync::gz {

using log_fn = void (*)(const char *fmt, ...);

struct result {
    bool        ok;
    const char *why;  // when !ok: what failed; the raw file is untouched
    uint64_t    raw;  // bytes read from the raw file
    uint64_t    comp; // bytes of the gzip file
    double      ms;
};

// Synchronous: raw_path -> raw_path + ".gz.tmp" -> verify -> raw_path + ".gz", then the raw file is
// deleted. On any failure the temp file is removed, the raw file is kept and `r->why` names the step.
// Safe to call from any thread; `r` may not be null.
void compress_file(const char *raw_path, result *r);

// Run compress_file on a new below-normal-priority thread and log one line (success or failure) when it
// ends. The path is copied. `log` may be null.
void compress_async(const char *raw_path, log_fn log);

// Tests: wait until no compress_async thread is running. True if idle within `ms`.
bool wait_idle(unsigned ms);

} // namespace mh::desync::gz
