//
// mh_log_rotate.h -- the ONE size cap + one-generation rotation used by every bounded log stream
// (mp:SES2).
//
// WHY IT IS SHARED RATHER THAN COPIED. Until SES2 exactly one stream in this tree rotated --
// mh_temporal.log, capped by [trace] temporal_max_mb in mh/seams/net_diag.cpp -- and its ".log" ->
// ".prev.log" derivation was four lines written inline at the one call site. mh_net.log, which is the
// stream a bug report is actually read from and the one several images append to, was unbounded. Two
// rotating streams is the point at which the derivation stops being an implementation detail of one
// writer and becomes a FORMAT: the tools glob `*.prev.log`, so a second, slightly different spelling
// of the same idea is a silent tooling break rather than a compile error.
//
// THE POLICY, stated once so both callers cannot drift:
//
//   * ONE generation is kept. At the cap the current file is RENAMED over `<stem>.prev.log` and the
//     writer reopens an empty `<stem>.log`. Disk is therefore bounded at 2x the cap however long a
//     session runs. There is no `.1`/`.2`/`.N` ladder and there should not be: what a post-mortem
//     needs is the TAIL -- what happened just before the freeze or the desync -- and a ladder buys
//     older history at the price of a bound nobody can state.
//   * ZERO MEANS UNCAPPED. A cap of 0 (or negative) never rotates. This is the knob's "off", and it
//     is how an operator asks for a complete long-run trace at their own risk.
//   * THE SIZE COMES FROM THE FILE, NOT FROM A COUNTER, for any stream with more than one writer.
//     mh_net.log is appended to by mh.dll's seam_log, mh_net.dll's logf, mp_menu.cpp, libmh_bind.cpp
//     and module_bind.cpp -- five writers in three images -- so a per-writer byte counter of the
//     shape mh_temporal.log uses would each independently believe the file is a fifth of its real
//     size. mh_log_over_cap() asks the filesystem.
//   * A FAILED ROTATION IS NOT A LOST LINE. If the rename fails (a reader holding the file without
//     FILE_SHARE_DELETE is the realistic case), mh_log_rotate() returns false, the caller keeps
//     appending to the file it already has, and the next line tries again. Growing past the cap is a
//     far better failure than dropping the line that says why the session died.
//
// SES1 INTERACTION (the generation-follow). Every bounded writer here resolves its path through
// mh_run_path() FIRST, so the path handed to these helpers is always the CURRENT session
// directory's file. Rotation therefore acts inside the open match's folder and never reaches back
// into a previous one; a fresh session starts at zero bytes and carries no predecessor's size.
//
// CONCURRENCY. seam_log is called from the main thread and from the transport's receive thread. Two
// threads can both observe over-cap and both attempt the rename; the second fails harmlessly (the
// source is already gone and the replacement file is small). A line written by a thread that opened
// its handle just before another thread's rename lands in the .prev file. Both outcomes are a line in
// the wrong one of two files, which is why no lock is taken for this.
//
#ifndef MH_LOG_ROTATE_H
#define MH_LOG_ROTATE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus

// The suffix a rotated generation carries. Named because tools glob it.
#define MH_LOG_PREV_SUFFIX ".prev.log"

// "<dir>\mh_net.log" -> "<dir>\mh_net.prev.log". Returns false (and leaves dst empty) when `path`
// does not end in ".log" or when the result would not fit in `cap`.
//
// THE ".log" SUFFIX IS CHECKED. The pre-SES2 inline version in net_diag.cpp overwrote the last four
// characters on the sole strength of `n > 4`, which is true of every path in the tree and would have
// produced `mh_desync_snap_0.prev.log` out of `mh_desync_snap_0.bin` had anyone reused it. It never
// mattered with one caller; with two it is the difference between a helper and a trap.
inline bool mh_log_prev_path(char *dst, int cap, const char *path) {
    if (dst == nullptr || cap <= 0) return false;
    dst[0] = '\0';
    if (path == nullptr) return false;
    const int n = lstrlenA(path);
    if (n < 5) return false;
    if (lstrcmpiA(path + n - 4, ".log") != 0) return false;
    const int want = n - 4 + (int)sizeof(MH_LOG_PREV_SUFFIX) - 1; // ".log" dropped, suffix added
    if (want + 1 > cap) return false;
    lstrcpynA(dst, path, n - 4 + 1); // the stem, NUL-terminated (lstrcpynA's cap counts the NUL)
    lstrcatA(dst, MH_LOG_PREV_SUFFIX);
    return true;
}

// Is this file at or past the cap? `cap_bytes <= 0` is UNCAPPED and always answers false.
// A file that does not exist is not over any cap.
inline bool mh_log_over_cap(const char *path, long long cap_bytes) {
    if (cap_bytes <= 0 || path == nullptr || path[0] == '\0') return false;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return false;
    const long long size = ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
    return size >= cap_bytes;
}

// The same question asked of an ALREADY-OPEN handle, so a writer that has just opened the file for
// append does not pay a second path lookup. Same contract: cap <= 0 is uncapped.
inline bool mh_log_handle_over_cap(HANDLE h, long long cap_bytes) {
    if (cap_bytes <= 0 || h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return false;
    return li.QuadPart >= cap_bytes;
}

// Rename `path` over its .prev sibling, replacing any existing one. Returns true iff the rename
// happened -- false means the caller should carry on appending to the file it has.
inline bool mh_log_rotate(const char *path) {
    char prev[MAX_PATH];
    if (!mh_log_prev_path(prev, MAX_PATH, path)) return false;
    return MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING) != 0;
}

// The whole decision for an open-append-close writer, in one call:
//   over cap? -> close the handle, rotate, report true so the caller reopens an empty file.
// `*h` is set to INVALID_HANDLE_VALUE only when the rotation actually happened, so a failed rename
// leaves the caller's handle usable and its line still gets written.
inline bool mh_log_rotate_open_handle(HANDLE *h, const char *path, long long cap_bytes) {
    if (h == nullptr || !mh_log_handle_over_cap(*h, cap_bytes)) return false;
    CloseHandle(*h);
    if (!mh_log_rotate(path)) {
        // The rename lost. Reopen the SAME file and keep going: over the cap beats silent.
        *h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return false;
    }
    *h = INVALID_HANDLE_VALUE;
    return true;
}

// Read a megabyte cap out of an ini and return it in BYTES. `default_mb` is used when the key is
// absent or the ini path is empty (which is the DllMain window, before build_paths has run).
// A value of 0 disables the cap; the result is clamped so a silly-large ini value cannot overflow.
inline long long mh_log_cap_bytes(const char *ini, const char *section, const char *key, int default_mb) {
    int mb = default_mb;
    if (ini != nullptr && ini[0] != '\0') mb = (int)GetPrivateProfileIntA(section, key, default_mb, ini);
    if (mb <= 0) return 0;                  // explicit "off"
    if (mb > 1024 * 1024) mb = 1024 * 1024; // 1 TB; keeps the multiply inside long long by a mile
    return (long long)mb * 1024 * 1024;
}

#endif // __cplusplus
#endif // MH_LOG_ROTATE_H
