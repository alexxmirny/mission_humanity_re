//
// sim/sim_pathtrace_dir_table.cpp -- see sim_pathtrace_dir_table.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_pathtrace_dirs_get_0066b415.asm,
// tmp/decomp_sim/llm_strat_pathtrace_normalize_repeat_dir_table_0066b4a1.asm), not from any Ghidra .c
// draft.
//
#include "sim/sim_pathtrace_dir_table.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

#include <cstdint>

namespace mh::sim {
namespace detail {

// 0x0066b415-0x0066b41a: MOV EAX,0x66a380 (_G_LLM_STRAT_PATHTRACE_DIRS); RET. Same accessor idiom as
// trace_greedy_path's own return statement (sim_pathtrace_greedy.cpp:277).
uint8_t *pathtrace_dirs_get(sim_store &own) { return &own.pathtrace_dir_at(0); }

// llm_strat_pathtrace_normalize_repeat_dir_table @0x0066b4a1. See the header banner for the full
// derivation.
void pathtrace_normalize_repeat_dir_table(const sim_view &v, sim_store &own) {
    // 0x0066b4a1-0x0066b4aa: ECX = pathtrace_len - 2; JLE done. Signed compare on a small (<=512)
    // unsigned length never overflows int32_t here.
    int32_t remaining = static_cast<int32_t>(*v.pathtrace_len) - 2;
    if (remaining <= 0) return;

    int32_t i = 0; // EDI, 0x0066b4b0 XOR EDI,EDI

    // The do-while shape (entry-guarded above, loop-back guarded by the same JG test at 0x0066b51d) is
    // behaviourally a plain while loop since both guards are the identical `remaining > 0` test.
    while (remaining > 0) {
        const uint8_t dir_i = own.pathtrace_dir_at(i); // 0x0066b4b2 MOVZX EAX, dirs[i]

        // 0x0066b4b9 CMP AL,0x8; JC (unsigned, dir_i < 8) -- and, only if that passes,
        // 0x0066b4c1 CMP AL, dirs[i+2]; JNZ. Both failures fall through to the advance-by-1 path.
        if (dir_i >= 8u && dir_i == own.pathtrace_dir_at(i + 2)) {
            // 0x0066b4cd: LEA ESI,[EAX*8 + 0x66b3e1]. The literal base 0x66b3e1 is NOT a real data
            // address (it aliases two preceding functions' code bytes) -- it is 8*8=64 bytes before
            // the REAL table start 0x0066b421 (sim_view::pathtrace_dir_split_table), which is exactly
            // where the caller's own `dir_i >= 8` guard (just above) lands on the first reachable
            // index. So the real table is indexed by `dir_i - 8`, matching Ghidra's own
            // llm_strat_pathtrace_dir_split[16] array bounds. See
            // tools/data/dll_addr_manifest.json's note on _G_LLM_STRAT_PATHTRACE_DIR_SPLIT_TABLE.
            const llm_strat_pathtrace_dir_split &entry = v.pathtrace_dir_split_table[dir_i - 8u];

            // 0x0066b4d4: dirs[i+1] = AL, still the OLD dir_i (equal to the old dirs[i+2] too).
            own.pathtrace_dir_at(i + 1) = dir_i;
            // 0x0066b4da/0x0066b4e0: dirs[i] = new_dir_a.
            own.pathtrace_dir_at(i) = entry.new_dir_a;
            // 0x0066b4dd/0x0066b4e6: dirs[i+2] = new_dir_b.
            own.pathtrace_dir_at(i + 2) = entry.new_dir_b;

            // 0x0066b4ec-0x0066b506: pos[i+1]'s ROW byte (low) += delta_row; its COL byte (high) +=
            // delta_col; recombine and mask against the packed pathtrace_coord_mask in one AND,
            // matching sim_state.h's (col<<8)|row packing note.
            const uint16_t old_pos = own.pathtrace_pos_at(i + 1);
            const uint8_t  old_row = static_cast<uint8_t>(old_pos & 0xffu);
            const uint8_t  old_col = static_cast<uint8_t>(old_pos >> 8);
            const uint8_t  new_row =
                static_cast<uint8_t>(entry.delta_row + old_row); // 8-bit wraparound add
            const uint8_t new_col =
                static_cast<uint8_t>(entry.delta_col + old_col); // 8-bit wraparound add
            const uint32_t packed       = (static_cast<uint32_t>(new_col) << 8) | new_row;
            own.pathtrace_pos_at(i + 1) = static_cast<uint16_t>(packed & *v.pathtrace_coord_mask);

            i += 3;         // 0x0066b50e
            remaining -= 3; // 0x0066b511
        } else {
            i += 1;         // 0x0066b519
            remaining -= 1; // 0x0066b51a
        }
    }
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

uint8_t *pathtrace_dirs_get() {
    sim_state st = state();
    return detail::pathtrace_dirs_get(st.own);
}

void pathtrace_normalize_repeat_dir_table() {
    sim_state st = state();
    detail::pathtrace_normalize_repeat_dir_table(st.read, st.own);
}


} // namespace mh::sim
