//
// ai/ai_scr_parse.cpp -- see ai_scr_parse.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_scr_parse_004ddb31.asm), not from Ghidra's C, though the two turned out
// to agree once the call-argument-count artifact below is understood.
//
#include "ai/ai_scr_parse.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mh::ai {
namespace detail {

namespace {

// _G_LLM_STRAT_AI_SCR_KEYWORD_TABLE's row layout, read off the original's `[ECX]` / `[ECX+4]` /
// `[ECX+8]` accesses (0x004ddbd7/0x004ddc38/0x004ddc4b et al.) and already documented on
// ai_view::ai_scr_keyword_table. A LOCAL type, not a second declaration of the region: the view
// exposes the table only as `const int32_t *` because there is no known record count, only the
// zero-`type` sentinel that ends the walk -- this struct is purely a convenience cast onto that same
// storage, same as the original's own `piVar5[0]`/`piVar5[1]`/`piVar5[2]` indexing.
struct scr_keyword_record {
    int32_t type;   // 0 = end of table (sentinel); 1 = int, parsed via atoi; 2 = float, via strtod
    void   *target; // the process-global tunable this keyword feeds; write target, not tracked state
    char   *name;   // keyword string; matched case-insensitively against the file's token
};
static_assert(sizeof(scr_keyword_record) == 12,
              "must match the original's 3 x int32 (0xc-byte) record stride");

} // namespace

void scr_parse(const ai_view &v, const ai_calls &gc, char *filename) {
    // Load the file whole, exactly like the original: file size, malloc(size + 1), copy the
    // resource-bank bytes in, NUL-terminate. The original then freed the RESOURCE-BANK pointer
    // (never the copy) -- SIMABI-VFS took that free away from libmh, because the pointer was the
    // HOST's and freeing it through the vendored CRT is only correct while the host allocates from
    // the game's heap. `asset_read` copies straight into our buffer and the host releases its own.
    //
    // The old size call passed `filename` TWICE, reproducing what the assembly loads into EDX right
    // before it (0x004ddb46: MOV EDX,EAX, with EAX still holding `filename`). That was never data:
    // it was the committed prototype's second parameter picking up a register the caller had not
    // reloaded. The entry drops it, and mh.dll's binder supplies it at the thunk.
    //
    // ONE NARROWING, in the safe direction: the original memcpy'd `file_size` bytes out of the
    // resource buffer whatever the buffer's real length was; `asset_read` copies at most what the
    // asset actually has. The two differ only for an asset whose bank-reported size overruns it,
    // where the original read past the end.
    const int32_t file_size = gc.asset_size(filename);

    char *buf = static_cast<char *>(gc.utils_malloc(static_cast<uint32_t>(file_size) + 1));
    gc.asset_read(filename, buf, static_cast<uint32_t>(file_size));
    buf[file_size] = 0;

    char *p = buf;
    for (;;) {
        // Skip whitespace (space / LF / CR) ahead of the keyword -- a pretest loop, matching the
        // assembly's CMP-then-conditional-INC shape at 0x004ddb80 directly. The second whitespace
        // skip below (ahead of the value, 0x004ddbf6) is the identical shape in the assembly, even
        // though Ghidra's own decompile rendered THAT occurrence as a nested do-while; hand-traced
        // both to confirm they skip the same run of {' ', '\n', '\r'} either way, so both are written
        // the same simple way here.
        while (*p == ' ' || *p == '\n' || *p == '\r') ++p;
        if (*p == 0) break;

        // Scan the keyword: [a-zA-Z0-9_]+.
        char *keyword = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
               *p == '_') {
            ++p;
        }
        if (*p == 0) break; // truncated file: keyword ran to EOF with no delimiter
        *p = 0;
        ++p;

        // Look up the keyword. `rec->type == 0` (the sentinel) both ends a failed linear search and,
        // right after, is the "did we actually match" test -- the same dword, the same comparison,
        // matching the assembly's single CMP at 0x004ddbed serving both roles.
        const auto *rec = reinterpret_cast<const scr_keyword_record *>(v.ai_scr_keyword_table);
        while (rec->type != 0 && gc.utils_str_cmp_ci(keyword, rec->name) != 0) {
            ++rec;
        }
        if (rec->type == 0) break; // unmatched keyword: SILENTLY ABORTS the rest of the file

        // Skip whitespace ahead of the value.
        while (*p == ' ' || *p == '\n' || *p == '\r') ++p;
        if (*p == 0) break;

        // Scan the value token: [0-9.-]+. UNLIKE the keyword scan above, the original does not
        // re-check for end-of-buffer before terminating/advancing past this token (0x004ddc0f -
        // 0x004ddc29 has no analogue of the 0x004ddbc3 EOF check) -- it always writes the NUL and
        // advances, then falls straight into the store. Reproduced as-is: see uncertainties for the
        // one-byte-past-the-buffer read this creates when a file's last value token is not followed
        // by a delimiter before EOF.
        char *value = p;
        while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
            ++p;
        }
        *p = 0;
        ++p;

        if (rec->type == 1) {
            // reimpl-verify 2026-08-07: this calls the HOST toolchain's <cstdlib> atoi, not the
            // original binary's own atoi (0x004daa6e) -- neither has a committed __watcall prototype
            // (mh_calls.gen.h has no wrapper for either), so routing through gc.atoi is not currently
            // possible. Judged low-risk: for well-formed small decimal integers (every known AI.SCR
            // int keyword) atoi's behaviour is CRT-invariant.
            *static_cast<int32_t *>(rec->target) = atoi(value);
        } else {
            *static_cast<float *>(rec->target) = static_cast<float>(strtod(value, nullptr));
        }
    }

    gc.utils_free(buf);
}

} // namespace detail

void scr_parse(char *filename) {
    const ai_state st = state();
    detail::scr_parse(st.read, live_calls(), filename);
}

} // namespace mh::ai
