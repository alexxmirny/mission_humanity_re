//
// sim_scenario_planet_clone_selftest.cpp -- `simtest` oracle for llm_strat_scenario_planet_clone
// @0x0045ba25 (sim/resid/sim_scenario_planet_clone.h/.cpp, RI-SIM / sim_resid batch C+E second
// slice).
//
// arm_ready:false -- NO SHADOW SITE (see sim_scenario_planet_clone.h's "NO SHADOW SITE" banner).
// This offline oracle is its only evidence.
//
// EXPECTED BEHAVIOUR straight off the .asm
// (tmp/decomp_sim_resid/llm_strat_scenario_planet_clone_0045ba25.asm), NOT the Ghidra .c beside it
// (house rule: the .c in this project has silently lied before):
//   0x0045ba40-0x0045ba71: two LOCAL int[4] arrays -- source_mul = {0xf,0xf,0xf,0xf}, source_add =
//     {0,0,0,0}, at two DIFFERENT stack addresses.
//   0x0045ba78-0x0045babc: cfg_final_planet_Construct(define_index=0xa8, invention_index=0,
//     map_name=cfg_blob+0xfc, planet_data=&<by-value mh_cfg_pre_struct_Planet>, path_unc=cfg_blob+0x18)
//     -- the by-value struct's icon_index is Planets[1].icon_index (PUSH [0x00be71d3] @0x0045baa0),
//     info_txt/info_flc are both EMPTY_NAME_STR (0x50109d), index is 0x1f, everything else is 0.
//   0x0045bac1-0x0045bad5: llm_str_char_subst(cfg_blob+0xfc, mode=2, c1='.', c2='\0').
//   0x0045bada-0x0045baed: inline strlen(cfg_blob+0xfc) (REPNE SCASB), AFTER the char_subst call.
//   0x0045baed: CMP ECX,0xf; JBE 0x0045bb2c -- truncate only when length is STRICTLY > 15.
//   0x0045baf2-0x0045baf9 (truncated arm only): cfg_blob[0x108] (== +0xfc+0xc) = 0 (byte store,
//     caps the name at 12 chars); 0x0045bafc-0x0045bb12: a SECOND independent inline strlen re-measures
//     the just-truncated string; LAB_0045bb13-0x0045bb29: inline strcpy appends "..." (the label
//     0x00501039) at the truncated end -- final content is the first 12 chars plus "...".
//   0x0045bb2c-0x0045bb3f: llm_str_ansi_to_wide(dst=own.scenario_planet_name_w() [0x00e589c0],
//     src=cfg_blob+0xfc) -- AFTER any truncation, so it widens the (possibly truncated) name; its
//     RETURN VALUE (not dst) is stored into G_TEXT_PTRS[0xa8] (0x005846ac).
//   0x0045bb44: Planets[0x1f].system_index = 0 (0x00beee61 == base 0x00be6da0 + 0x1f*0x427 + 0x8).
//   0x0045bb4e-0x0045bb53: llm_snd_ambient_planet_clone(1), unconditional, last.
// No other stores, no other calls, no early-out.
//
#include "sim/resid/sim_scenario_planet_clone.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ONE shared sequence counter across ALL FOUR mocks, so a case can assert end-to-end call order
// (construct -> char_subst -> ansi_to_wide -> snd_ambient) as well as per-mock counts/arguments.
int g_seq = 0;

struct construct_call {
    int32_t define_index;
    int32_t invention_index;
    char   *map_name;
    // A COPY, taken at call time -- the original mh_cfg_pre_struct_Planet is a caller-stack local
    // (see the header banner) that does not outlive this call.
    mh::game::mh_cfg_pre_struct_Planet planet_data;
    // THE COPY IS NOT ENOUGH FOR TWO OF ITS FIELDS. source_mul and source_add are POINTERS into the
    // same dead frame -- `int32_t source_mul[4]` / `source_add[4]` are locals of
    // detail::scenario_planet_clone, exactly as the original's `LEA EAX,[EBP-0x28]` / `[EBP-0x38]`
    // are locals of llm_strat_scenario_planet_clone. Copying the struct copies the pointers, so
    // dereferencing them after the call reads a reclaimed stack frame. The first draft of this
    // oracle did that and both array checks failed on garbage while the translation was correct.
    // Snapshot the CONTENTS here, where the frame is still alive.
    int32_t source_mul_snapshot[4];
    int32_t source_add_snapshot[4];
    bool    source_ptrs_distinct;
    char   *path_unc;
    int     seq;
};
std::vector<construct_call> g_construct_calls;

void rec_cfg_final_planet_Construct(int32_t define_index, int32_t invention_index, char *map_name,
                                    const mh::game::mh_cfg_pre_struct_Planet *planet_data,
                                    char                                     *path_unc) {
    construct_call rec{};
    rec.define_index    = define_index;
    rec.invention_index = invention_index;
    rec.map_name        = map_name;
    std::memcpy(&rec.planet_data, planet_data, sizeof(rec.planet_data));
    // Dereference the two source arrays HERE, while the caller's frame is still live (see the
    // struct's comment).
    rec.source_ptrs_distinct = rec.planet_data.source_mul != nullptr &&
                               rec.planet_data.source_add != nullptr &&
                               rec.planet_data.source_mul != rec.planet_data.source_add;
    if (rec.planet_data.source_mul != nullptr)
        std::memcpy(rec.source_mul_snapshot, rec.planet_data.source_mul, sizeof(rec.source_mul_snapshot));
    if (rec.planet_data.source_add != nullptr)
        std::memcpy(rec.source_add_snapshot, rec.planet_data.source_add, sizeof(rec.source_add_snapshot));
    rec.path_unc = path_unc;
    rec.seq      = ++g_seq;
    g_construct_calls.push_back(rec);
}

struct char_subst_call {
    char   *str;
    uint8_t mode;
    char    c1;
    char    c2;
    int     seq;
};
std::vector<char_subst_call> g_char_subst_calls;

void rec_str_char_subst(char *str, uint8_t mode, char c1, char c2) {
    g_char_subst_calls.push_back({str, mode, c1, c2, ++g_seq});
}

// A SEPARATE mock, used only by the order-sensitive T2 case below: it records exactly like
// rec_str_char_subst above, but ALSO shrinks `str` to 3 characters in place. This is a deliberately
// non-realistic effect (the real llm_str_char_subst substitutes a character, it does not truncate to
// a fixed length) -- its only purpose is to be OBSERVABLE downstream. The inline
// strlen()/truncation-decision logic at 0x0045bada-0x0045baed can only see this 3-char content if
// the translation truly calls str_char_subst BEFORE measuring the length, in the .asm's order. A
// (bug) translation that measured the length of the original, un-mutated buffer first would still see
// T2's 20-char seed and take the truncation branch regardless of this mock.
void rec_str_char_subst_and_shrink(char *str, uint8_t mode, char c1, char c2) {
    g_char_subst_calls.push_back({str, mode, c1, c2, ++g_seq});
    if (str != nullptr) str[3] = '\0';
}

struct ansi_to_wide_call {
    void       *dst;
    char       *src;
    std::string src_snapshot; // a COPY of *src taken AT CALL TIME -- proves what the SUT actually
                              // handed this call, independent of what the buffer holds afterward.
    int seq;
};
std::vector<ansi_to_wide_call> g_ansi_to_wide_calls;

// A distinctive return value that is NOT `dst` -- so a translation that (bug) stores the destination
// argument instead of this call's RETURN value into G_TEXT_PTRS[0xa8] fails visibly.
wchar_t g_wide_return_sentinel[4] = {L'S', L'E', L'N', L'\0'};

void *rec_str_ansi_to_wide(void *dst, char *src) {
    ansi_to_wide_call rec{};
    rec.dst          = dst;
    rec.src          = src;
    rec.src_snapshot = (src != nullptr) ? std::string(src) : std::string();
    rec.seq          = ++g_seq;
    g_ansi_to_wide_calls.push_back(rec);
    return g_wide_return_sentinel;
}

struct snd_call {
    uint32_t planet_id;
    int      seq;
};
std::vector<snd_call> g_snd_calls;

void rec_snd_ambient_planet_clone(uint32_t planet_id) { g_snd_calls.push_back({planet_id, ++g_seq}); }

const scenario_planet_clone_calls g_calls = {
    &rec_cfg_final_planet_Construct,
    &rec_str_char_subst,
    &rec_str_ansi_to_wide,
    &rec_snd_ambient_planet_clone,
};

void reset_recorders() {
    g_seq = 0;
    g_construct_calls.clear();
    g_char_subst_calls.clear();
    g_ansi_to_wide_calls.clear();
    g_snd_calls.clear();
}

} // namespace

void run_scenario_planet_clone_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- happy path: exhaustive pin of the whole call chain on a SHORT (not-truncated) name.
    // Covers cfg_final_planet_Construct's five arguments incl. every field of the by-value
    // mh_cfg_pre_struct_Planet (0x0045ba78-0x0045baa0); the Planets[1].icon_index read (0x0045baa0);
    // the Planets[0x1f].system_index write + neighbour survival (0x0045bb44); G_TEXT_PTRS[0xa8]
    // receiving the WIDENING CALL'S RETURN VALUE, not its destination argument (0x0045bb3f);
    // llm_snd_ambient_planet_clone(1) firing last (0x0045bb53); and end-to-end call order via the
    // one shared sequence counter.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        // cfg_blob: an opaque caller-owned buffer (declared_needs). +0xfc is the name/map_name field
        // this function reads/writes; +0x18 is path_unc (read-only here, pointer-identity checked
        // only); +0x108 is +0xfc+0xc, the truncation NUL. Zeroed, then the two fields seeded with
        // DISTINCT, non-zero content so a pointer swap between them would also show up on content.
        char cfg_blob[0x140];
        std::memset(cfg_blob, 0, sizeof(cfg_blob));
        std::memcpy(cfg_blob + 0x18, "PATHUNC_MARK", 12);
        const char *name1 = "HAPPY_OK"; // 8 chars, well under the 15-char boundary -- not truncated
        std::memcpy(cfg_blob + 0xfc, name1, std::strlen(name1) + 1);

        // Planets[1].icon_index: seeded distinctive/non-zero so a wrong index into Planets fails.
        fx.cfg_planets[1].icon_index = 0x4321;

        // Planets[0x1e] and Planets[0x1f]: full-struct sentinel fill (byte patterns), then
        // Planets[0x1f].system_index overwritten with its own distinct non-zero seed -- lets a single
        // post-call byte-diff prove BOTH "only system_index changed" and "slot 0x1e untouched".
        std::memset(&fx.cfg_planets[0x1e], 0xA5, sizeof(cfg_planet));
        std::memset(&fx.cfg_planets[0x1f], 0x5A, sizeof(cfg_planet));
        fx.cfg_planets[0x1f].system_index = 0x77777777;

        cfg_planet before_1e = fx.cfg_planets[0x1e];
        cfg_planet before_1f = fx.cfg_planets[0x1f];

        sim_store own = fx.store();
        detail::scenario_planet_clone(fx.view(), own, g_calls, cfg_blob);

        // ---- cfg_final_planet_Construct: all five arguments, 0x0045baa8-0x0045babc -----------------
        ck_eq((uint32_t)g_construct_calls.size(), 1u, "T1: cfg_final_planet_Construct called exactly once, 0x0045babc");
        if (!g_construct_calls.empty()) {
            const construct_call &cc = g_construct_calls[0];
            ck_eq((uint32_t)cc.define_index, 0xa8u, "T1: define_index == 0xa8, MOV EAX,0xa8 @0x0045baae");
            ck_eq((uint32_t)cc.invention_index, 0u, "T1: invention_index == 0, XOR ECX,ECX @0x0045baa6");
            ck(cc.map_name == cfg_blob + 0xfc,
               "T1: map_name == cfg_blob+0xfc, ADD EDX,0xfc @0x0045bab1 (not swapped with path_unc)");
            ck(cc.path_unc == cfg_blob + 0x18,
               "T1: path_unc == cfg_blob+0x18, ADD EBX,0x18 @0x0045baab (not swapped with map_name)");

            // ---- the by-value mh_cfg_pre_struct_Planet, EVERY field -----------------------------
            ck_eq((uint32_t)cc.planet_data.icon_index, 0x4321u,
                  "T1: planet_data.icon_index == Planets[1].icon_index (seeded 0x4321), PUSH [0x00be71d3] @0x0045baa0");
            ck_eq((uint32_t)cc.planet_data.x1, 0u, "T1: planet_data.x1 == 0, PUSH 0 @0x0045ba9e");
            ck_eq((uint32_t)cc.planet_data.y1, 0u, "T1: planet_data.y1 == 0, PUSH 0 @0x0045ba9c");
            ck_eq((uint32_t)cc.planet_data.x2, 0u, "T1: planet_data.x2 == 0, PUSH 0 @0x0045ba9a");
            ck_eq((uint32_t)cc.planet_data.y2, 0u, "T1: planet_data.y2 == 0, PUSH 0 @0x0045ba98");
            ck(cc.planet_data.info_txt == (void *)&fx.empty_name_str,
               "T1: planet_data.info_txt == &empty_name_str (0x50109d), PUSH EMPTY_NAME_STR @0x0045ba97");
            ck(cc.planet_data.info_flc == (void *)&fx.empty_name_str,
               "T1: planet_data.info_flc == &empty_name_str (0x50109d), PUSH EMPTY_NAME_STR @0x0045ba91");
            ck(cc.source_ptrs_distinct,
               "T1: source_mul and source_add point at two DIFFERENT local arrays, LEA @0x0045ba84/0x0045ba88");
            {
                // Read from the CALL-TIME snapshots, not through the recorded pointers -- both aim
                // into detail::scenario_planet_clone's own (now dead) frame.
                const int32_t *mul = cc.source_mul_snapshot;
                const int32_t *add = cc.source_add_snapshot;
                ck(mul[0] == 0xf && mul[1] == 0xf && mul[2] == 0xf && mul[3] == 0xf,
                   "T1: source_mul == {0xf,0xf,0xf,0xf}, four MOVs @0x0045ba40/0x0045ba4e/0x0045ba5c/0x0045ba6a");
                ck(add[0] == 0 && add[1] == 0 && add[2] == 0 && add[3] == 0,
                   "T1: source_add == {0,0,0,0}, four MOVs @0x0045ba47/0x0045ba55/0x0045ba63/0x0045ba71");
            }
            ck_eq((uint32_t)cc.planet_data.coordinate_x, 0u, "T1: planet_data.coordinate_x == 0, PUSH 0 @0x0045ba82");
            ck_eq((uint32_t)cc.planet_data.coordinate_y, 0u, "T1: planet_data.coordinate_y == 0, PUSH 0 @0x0045ba80");
            ck_eq((uint32_t)cc.planet_data.asteroids, 0u, "T1: planet_data.asteroids == 0, PUSH 0 @0x0045ba7e");
            ck_eq((uint32_t)cc.planet_data.turn_speed, 0u, "T1: planet_data.turn_speed == 0, PUSH 0 @0x0045ba7c");
            ck_eq((uint32_t)cc.planet_data.enemy, 0u, "T1: planet_data.enemy == 0, PUSH 0 @0x0045ba7a");
            ck_eq((uint32_t)cc.planet_data.index, 0x1fu, "T1: planet_data.index == 0x1f, PUSH 0x1f @0x0045ba78");
        }

        // ---- llm_str_char_subst: exactly once, right arguments, 0x0045bad5 --------------------------
        ck_eq((uint32_t)g_char_subst_calls.size(), 1u, "T1: llm_str_char_subst called exactly once, 0x0045bad5");
        if (!g_char_subst_calls.empty()) {
            const char_subst_call &sc = g_char_subst_calls[0];
            ck(sc.str == cfg_blob + 0xfc, "T1: llm_str_char_subst's str == cfg_blob+0xfc, ADD EAX,0xfc @0x0045bad0");
            ck_eq((uint32_t)sc.mode, 2u, "T1: llm_str_char_subst's mode == 2, MOV EDX,0x2 @0x0045bac8");
            ck_eq((uint32_t)(uint8_t)sc.c1, (uint32_t)(uint8_t)'.', "T1: llm_str_char_subst's c1 == '.', MOV EBX,0x2e @0x0045bac3");
            ck_eq((uint32_t)(uint8_t)sc.c2, 0u, "T1: llm_str_char_subst's c2 == 0, XOR ECX,ECX @0x0045bac1");
        }

        // ---- not truncated: buffer unchanged, +0x108 untouched (8 chars << the "> 15" boundary) ----
        ck(std::memcmp(cfg_blob + 0xfc, name1, std::strlen(name1) + 1) == 0,
           "T1: name buffer unchanged (8 chars <= 15), CMP ECX,0xf / JBE @0x0045baed taken");
        ck_eq((uint32_t)(uint8_t)cfg_blob[0x108], 0u,
              "T1: byte +0x108 still the zero-fill (never written -- truncation branch not taken), CMP ECX,0xf / JBE @0x0045baed");

        // ---- llm_str_ansi_to_wide: dest == own.scenario_planet_name_w(), src content == the name ----
        ck_eq((uint32_t)g_ansi_to_wide_calls.size(), 1u, "T1: llm_str_ansi_to_wide called exactly once, 0x0045bb3a");
        if (!g_ansi_to_wide_calls.empty()) {
            const ansi_to_wide_call &wc = g_ansi_to_wide_calls[0];
            ck(wc.dst == (void *)fx.scenario_planet_name_w.data(),
               "T1: llm_str_ansi_to_wide's dst == scenario_planet_name_w() (0x00e589c0), MOV EAX,0xe589c0 @0x0045bb35");
            ck(wc.src == cfg_blob + 0xfc, "T1: llm_str_ansi_to_wide's src == cfg_blob+0xfc, ADD EDX,0xfc @0x0045bb2f");
            ck(wc.src_snapshot == name1, "T1: llm_str_ansi_to_wide widens the (unmodified) name \"HAPPY_OK\"");
        }

        // ---- G_TEXT_PTRS[0xa8]: receives the RETURN VALUE of str_ansi_to_wide, NOT its dst arg ------
        ck(fx.text_ptrs[0xa8] == (const wchar_t *)g_wide_return_sentinel,
           "T1: G_TEXT_PTRS[0xa8] == str_ansi_to_wide's RETURN value (a sentinel != dst), MOV [0x005846ac],EAX @0x0045bb3f");
        ck(fx.text_ptrs[0xa8] != (const wchar_t *)fx.scenario_planet_name_w.data(),
           "T1: G_TEXT_PTRS[0xa8] is NOT the destination buffer pointer (would mean the translation "
           "stored dst instead of the call's return value), 0x0045bb3f");

        // ---- Planets[0x1f].system_index == 0, neighbours untouched, 0x0045bb44 ----------------------
        ck_eq((uint32_t)fx.cfg_planets[0x1f].system_index, 0u,
              "T1: Planets[0x1f].system_index == 0, MOV dword ptr [0x00beee61],0x0 @0x0045bb44");
        {
            cfg_planet after_1f   = fx.cfg_planets[0x1f];
            after_1f.system_index = before_1f.system_index; // the one field this store legitimately changed
            ck(std::memcmp(&after_1f, &before_1f, sizeof(cfg_planet)) == 0,
               "T1: every OTHER field of Planets[0x1f] unchanged (no wider write than the single system_index dword), 0x0045bb44");
        }
        ck(std::memcmp(&fx.cfg_planets[0x1e], &before_1e, sizeof(cfg_planet)) == 0,
           "T1: Planets[0x1e] (the neighbouring slot) completely untouched -- rules out a wrong index "
           "into Planets, 0x00beee61 == base + 0x1f*0x427 + 0x8");

        // ---- llm_snd_ambient_planet_clone(1): exactly once, unconditional, LAST, 0x0045bb53 ---------
        ck_eq((uint32_t)g_snd_calls.size(), 1u, "T1: llm_snd_ambient_planet_clone called exactly once, 0x0045bb53");
        if (!g_snd_calls.empty())
            ck_eq(g_snd_calls[0].planet_id, 1u, "T1: llm_snd_ambient_planet_clone(1), MOV EAX,0x1 @0x0045bb4e");

        // ---- end-to-end call order: construct(1) -> char_subst(2) -> ansi_to_wide(3) -> snd(4) ------
        if (!g_construct_calls.empty() && !g_char_subst_calls.empty() && !g_ansi_to_wide_calls.empty() &&
            !g_snd_calls.empty()) {
            ck(g_construct_calls[0].seq < g_char_subst_calls[0].seq &&
                   g_char_subst_calls[0].seq < g_ansi_to_wide_calls[0].seq &&
                   g_ansi_to_wide_calls[0].seq < g_snd_calls[0].seq,
               "T1: call order is cfg_final_planet_Construct -> llm_str_char_subst -> llm_str_ansi_to_wide -> "
               "llm_snd_ambient_planet_clone, the .asm's straight-line order 0x0045babc/0x0045bad5/0x0045bb3a/0x0045bb53");
        }
    }

    // =================================================================================================
    // T2 -- llm_str_char_subst fires BEFORE the length measurement, not after. The mock both records
    // AND shrinks its argument to 3 characters in place (see rec_str_char_subst_and_shrink's comment)
    // -- a 20-char seed name would cross the ">15" boundary and truncate UNLESS the inline strlen at
    // 0x0045bada-0x0045baed reads the buffer AFTER str_char_subst has run, in which case it sees only
    // 3 characters and skips truncation entirely.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        scenario_planet_clone_calls order_calls = g_calls;
        order_calls.str_char_subst              = &rec_str_char_subst_and_shrink;

        char cfg_blob[0x140];
        std::memset(cfg_blob, 0, sizeof(cfg_blob));
        const char *name = "TWENTY_CHARACTERS!!!"; // 20 chars, well past the 15-char boundary if unmutated
        std::memcpy(cfg_blob + 0xfc, name, std::strlen(name) + 1);

        sim_store own = fx.store();
        detail::scenario_planet_clone(fx.view(), own, order_calls, cfg_blob);

        ck_eq((uint32_t)g_char_subst_calls.size(), 1u, "T2: llm_str_char_subst called exactly once, 0x0045bad5");
        // If the truncation branch had (bug) fired, +0x108 would hold '.' (the first appended
        // ellipsis character -- see T3b below). It holds the SEED's 13th character instead, because
        // the mock's mutation is already in the buffer by the time the SUT measures the length, so
        // the length reads 3 and nothing is written past index 3.
        //
        // 'C' AND NOT 0: the untaken branch leaves the byte ALONE, it does not clear it. The seed
        // "TWENTY_CHARACTERS!!!" has 'C' at index 12 (0x108 == 0xfc + 12), and the mock only writes a
        // NUL at index 3. Asserting 0 here -- as this oracle's first draft did -- would have been
        // asserting that the branch wrote something, which is the opposite of the claim.
        ck_eq((uint32_t)(uint8_t)cfg_blob[0x108], (uint32_t)'C',
              "T2: truncation branch NOT taken -- +0x108 still holds the SEED's 13th char 'C', proving the "
              "inline strlen @0x0045bada-0x0045baed read the buffer AFTER llm_str_char_subst ran (mock "
              "shrank it to 3 chars) rather than the original 20-char seed, CMP ECX,0xf / JBE @0x0045baed");
        ck(std::memcmp(cfg_blob + 0xfc, "TWE", 3) == 0 && cfg_blob[0xfc + 3] == 0,
           "T2: map_name reads back the mock-shrunk 3-char content, confirming str_char_subst ran before "
           "this function's own length measurement re-read the buffer");
    }

    // =================================================================================================
    // T3 -- the truncation branch (`CMP ECX,0xf; JBE 0x0045bb2c` @0x0045baed), both arms and its exact
    // boundary: length <= 15 does NOT truncate, length > 15 DOES.
    // =================================================================================================
    // T3a: 15 chars -- the boundary's low side. NOT truncated.
    {
        fx.reset();
        reset_recorders();
        char cfg_blob[0x140];
        std::memset(cfg_blob, 0, sizeof(cfg_blob));
        const char *name = "ABCDEFGHIJKLMNO";   // exactly 15 chars, A..O
        std::memcpy(cfg_blob + 0xfc, name, 16); // 15 chars + NUL

        sim_store own = fx.store();
        detail::scenario_planet_clone(fx.view(), own, g_calls, cfg_blob);

        ck(std::memcmp(cfg_blob + 0xfc, "ABCDEFGHIJKLMNO", 16) == 0,
           "T3a: 15-char name (== boundary) left byte-for-byte UNCHANGED -- CMP ECX,0xf / JBE @0x0045baed "
           "taken (15 <= 15), truncation skipped");
        ck_eq((uint32_t)(uint8_t)cfg_blob[0x108], (uint32_t)'M',
              "T3a: byte +0x108 (the 13th char, index 12 of the 15-char name) still 'M' -- not zeroed, "
              "CMP ECX,0xf / JBE @0x0045baed");
    }
    // T3b: 16 chars -- the boundary's high side, by exactly one. TRUNCATED.
    {
        fx.reset();
        reset_recorders();
        char cfg_blob[0x140];
        std::memset(cfg_blob, 0, sizeof(cfg_blob));
        const char *name = "ABCDEFGHIJKLMNOP";  // exactly 16 chars, A..P
        std::memcpy(cfg_blob + 0xfc, name, 17); // 16 chars + NUL

        sim_store own = fx.store();
        detail::scenario_planet_clone(fx.view(), own, g_calls, cfg_blob);

        ck(std::memcmp(cfg_blob + 0xfc, "ABCDEFGHIJKL...", 16) == 0 && cfg_blob[0xfc + 16] == 0,
           "T3b: 16-char name (== boundary + 1) TRUNCATED to the first 12 chars plus \"...\" -- CMP ECX,0xf "
           "/ JBE @0x0045baed NOT taken (16 > 15), byte +0x108 zeroed @0x0045baf5 then overwritten by the "
           "appended ellipsis's first '.' @LAB_0045bb13");
        ck_eq((uint32_t)(uint8_t)cfg_blob[0x108], (uint32_t)'.',
              "T3b: byte +0x108 == '.' (the truncation NUL @0x0045baf5 was immediately overwritten by the "
              "appended \"...\"'s first character)");

        // llm_str_ansi_to_wide must widen the TRUNCATED string, not the original 16-char one.
        ck_eq((uint32_t)g_ansi_to_wide_calls.size(), 1u, "T3b: llm_str_ansi_to_wide called once, 0x0045bb3a");
        if (!g_ansi_to_wide_calls.empty())
            ck(g_ansi_to_wide_calls[0].src_snapshot == "ABCDEFGHIJKL...",
               "T3b: llm_str_ansi_to_wide widens the TRUNCATED name \"ABCDEFGHIJKL...\", not the original "
               "16-char seed -- proves the widen call (0x0045bb3a) happens AFTER truncation (0x0045baed-0x0045bb29)");
    }
    // T3c: a much longer name (30 chars) -- same truncation result regardless of how far past the
    // boundary the original length is.
    {
        fx.reset();
        reset_recorders();
        char cfg_blob[0x140];
        std::memset(cfg_blob, 0, sizeof(cfg_blob));
        const char *name = "0123456789ABCDEFGHIJKLMNOPQRST"; // 30 chars, first 12 == "0123456789AB"
        std::memcpy(cfg_blob + 0xfc, name, std::strlen(name) + 1);

        sim_store own = fx.store();
        detail::scenario_planet_clone(fx.view(), own, g_calls, cfg_blob);

        // THE TAIL IS NOT CLEARED, and this is the difference from T3b rather than an oversight. The
        // truncation writes exactly FOUR bytes past index 11 -- the NUL at +0x108 (index 12), then
        // the appended "..." plus its own NUL over indices 12..15. Everything from index 16 on is
        // whatever the seed left there. In T3b the seed was exactly 16 chars, so index 16 was its
        // terminator and happened to be 0; here the seed is 30 chars, so index 16 is still 'G'.
        // Asserting 0 -- as this oracle's first draft did -- would have been asserting a clear the
        // original never performs.
        ck(std::memcmp(cfg_blob + 0xfc, "0123456789AB...", 16) == 0,
           "T3c: a 30-char name truncates to the same shape -- first 12 chars plus \"...\" and its NUL, "
           "0x0045baed/0x0045baf5/LAB_0045bb13");
        ck_eq((uint32_t)(uint8_t)cfg_blob[0xfc + 16], (uint32_t)'G',
              "T3c: index 16 still holds the SEED's 17th char 'G' -- the truncation writes only indices "
              "12..15 and never clears the tail, LAB_0045bb13-0x0045bb29");
        ck_eq((uint32_t)g_ansi_to_wide_calls.size(), 1u, "T3c: llm_str_ansi_to_wide called once, 0x0045bb3a");
        if (!g_ansi_to_wide_calls.empty())
            ck(g_ansi_to_wide_calls[0].src_snapshot == "0123456789AB...",
               "T3c: llm_str_ansi_to_wide widens the truncated 15-char result, not the 30-char seed");
    }
    // T3d: a short name (5 chars) -- well under the boundary. NOT truncated.
    {
        fx.reset();
        reset_recorders();
        char cfg_blob[0x140];
        std::memset(cfg_blob, 0, sizeof(cfg_blob));
        const char *name = "SHORT"; // 5 chars
        std::memcpy(cfg_blob + 0xfc, name, std::strlen(name) + 1);

        sim_store own = fx.store();
        detail::scenario_planet_clone(fx.view(), own, g_calls, cfg_blob);

        ck(std::memcmp(cfg_blob + 0xfc, "SHORT", 6) == 0,
           "T3d: 5-char name left byte-for-byte UNCHANGED, CMP ECX,0xf / JBE @0x0045baed taken (5 <= 15)");
        ck_eq((uint32_t)(uint8_t)cfg_blob[0x108], 0u,
              "T3d: byte +0x108 still the zero-fill (5-char name doesn't reach that far, and the "
              "truncation store never ran), CMP ECX,0xf / JBE @0x0045baed");
        ck_eq((uint32_t)g_ansi_to_wide_calls.size(), 1u, "T3d: llm_str_ansi_to_wide called once, 0x0045bb3a");
        if (!g_ansi_to_wide_calls.empty())
            ck(g_ansi_to_wide_calls[0].src_snapshot == "SHORT", "T3d: llm_str_ansi_to_wide widens the unmodified 5-char name");
    }
}

} // namespace mh::sim::test
