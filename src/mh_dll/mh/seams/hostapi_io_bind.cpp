//
// seams/hostapi_io_bind.cpp -- mh.dll's HAND-WRITTEN half of the host-callback ABI: the eight
// reshaped `io` entries (SIMABI-VFS, 2026-09-10; docs/libmh-sim-abi.md sec 2b).
//
// Every other entry in the two tables is a generated one-line forward onto its mh::call:: thunk,
// because every other entry's ABI shape IS the thunk's. These eight are not: the io group was
// reshaped into a surface a headless host implements from its contract sentence, and the difference
// between that surface and the original thunks is exactly the set of quirks below. They live HERE
// rather than in libmh on purpose -- that is what "reshape" bought. libmh no longer knows that:
//
//   * open takes a C-runtime mode STRING ("rb"/"wb" -- the only two any libmh site ever passed);
//   * read is fread(dst, elem_size, count, file) and needs elem_size pinned to 1 for the return to
//     be a byte count at all;
//   * write is fwrite(data, size, count=1, handle) and returns the ITEM count, so 1 means "all n";
//   * the resource-bank size call takes the filename TWICE (a register-convention artifact);
//   * the resource loader hands back the GAME's allocation, which somebody then has to free.
//
// That last one is the load-bearing change rather than a tidy-up. Two sim sites -- ai_scr_parse.cpp
// and sim_load_base_layout_dmp.cpp -- used to free the returned pointer through the vendored CRT,
// which is correct only while libmh and the host share one heap. `asset_read` copies and frees here,
// at the same instant retail's own callers do (llm_tact_view_metrics_init 0x0042999c,
// llm_ui_main_menu_screen_load and the rest), so R5 is untouched and no libmh site owns a host
// pointer any more.
//
// HARNESS TU (mh.vcxproj only): it names fixed VAs through mh::call and can never join libmh.
//
#include "include/mh_hostapi_bind.h"

#include "../../libmh/include/libmh.h" // MH_VFS_READ / MH_VFS_WRITE -- the mode enum's home
#include "addr/mh_calls.gen.h"

#include <cstring>

namespace mh::hostapi::io {

// The two mode strings, materialised once. `utils_open_file` takes a non-const char *, and the
// spelling is retail's own (0x00500cda "rb" is the one the disassembly cites at the load sites).
namespace {
char g_mode_rb[] = "rb";
char g_mode_wb[] = "wb";
} // namespace

int32_t vfs_open(const char *path, int32_t mode) {
    // MH_VFS_WRITE is create/truncate, everything else is read -- and there IS nothing else: the
    // enum has two values because the six libmh sites had two mode strings between them.
    return mh::call::utils_open_file(const_cast<char *>(path),
                                     mode == MH_VFS_WRITE ? g_mode_wb : g_mode_rb);
}

int32_t vfs_read(int32_t handle, void *dst, uint32_t n) {
    // ELEM_SIZE 1, COUNT n. The thunk's tail (0x004cff48 XOR EDX,EDX / MOV EAX,[ESP+8] / DIV ESI)
    // divides the bytes read by elem_size, so pinning elem_size to 1 is what makes the return a
    // BYTE count and a short read recoverable. With the original's (n, 1) a short read returns 0
    // and the figure is gone -- which is the property the entry's "must be REPORTED" depends on.
    return mh::call::utils_read_from_file(dst, 1u, n, reinterpret_cast<void *>(static_cast<uintptr_t>(handle)));
}

int32_t vfs_write(int32_t handle, const void *src, uint32_t n) {
    // fwrite(data, size, count, handle), which the generated wrapper exposes as
    // (file_h, count, data, size). Count 1, so the return is 1 for "the whole block went" and 0
    // otherwise; the entry promises a byte count, so that is what comes back.
    //
    // FAILURE IS -1, NOT 0, AND THE ZERO-LENGTH CASE IS WHY. fwrite writes zero ITEMS when the item
    // size is zero, so the original answers 0 -- a FAILURE to every caller of this thunk, including
    // write_extension's zero-length guard, whose mutation is unobservable without it. Mapping that
    // to a 0 byte count would make `bytes == n` true for n == 0 and turn the original's failure into
    // a success, which is a behaviour change the entry's shape would have smuggled in for free.
    return mh::call::utils_write_to_file(static_cast<uint32_t>(handle), 1u, const_cast<void *>(src), n) == 1
               ? static_cast<int32_t>(n)
               : -1;
}

void vfs_close(int32_t handle) {
    // The thunk returns a status every libmh site already discarded (and the original discards at
    // 0x004480e0 too), so the entry is void and the value is dropped here.
    mh::call::utils_close_file(reinterpret_cast<void *>(static_cast<uintptr_t>(handle)));
}

int32_t vfs_seek(int32_t handle, int32_t offset, int32_t whence) {
    return mh::call::file_seek(reinterpret_cast<void *>(static_cast<uintptr_t>(handle)), offset,
                               static_cast<uint32_t>(whence));
}

int32_t vfs_tell(int32_t handle) {
    return mh::call::file_tell(reinterpret_cast<void *>(static_cast<uintptr_t>(handle)));
}

int32_t asset_size(const char *name) {
    // The second argument is the filename AGAIN. It is not data: at the one original call site
    // (0x004ddb46 MOV EDX,EAX, EAX still holding `filename`) the callee's second register parameter
    // simply picks up what the caller had not reloaded, and the committed __watcall prototype
    // records it. Reproduced here so the hosted arm calls the thunk exactly as retail does.
    return mh::call::llm_res_bank_get_file_size(const_cast<char *>(name),
                                                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(name)));
}

int32_t asset_read(const char *name, void *dst, uint32_t dst_cap) {
    // rsr_GetFileRealSize 0x004cf15e IS the game's own answer to "how big is the buffer
    // GetResourseFilePtr just returned": 14 call sites, and nearly every one of them is literally
    // that pair on the same name (llm_tact_mission_load, llm_planet_tlo_load, cfg_ReadMapFile,
    // llm_ui_text_viewer_open, cfg_Init, llm_snd_load_sound_cfg ...), several then sizing a
    // mem_realloc from it. So this is the game's idiom rather than our inference. 0 means the name
    // is in no loaded pack -- the same condition GetResourseFilePtr answers with NULL.
    const uint32_t len = mh::call::rsr_GetFileRealSize(const_cast<char *>(name));
    if (dst_cap == 0) {
        // The size query. It deliberately does NOT load: a caller that has to size a buffer before
        // it can receive the bytes would otherwise pay for a whole LZW decompress twice.
        return len == 0 ? -1 : static_cast<int32_t>(len);
    }
    uint8_t *p = mh::call::GetResourseFilePtr(const_cast<char *>(name));
    if (p == nullptr) return -1;
    const uint32_t n = len < dst_cap ? len : dst_cap;
    if (n != 0 && dst != nullptr) std::memcpy(dst, p, n);
    // THE CROSS-HEAP FREE, moved to the side that allocated. The game's malloc, the game's free.
    mh::call::utils_free(p);
    return static_cast<int32_t>(len);
}

} // namespace mh::hostapi::io
