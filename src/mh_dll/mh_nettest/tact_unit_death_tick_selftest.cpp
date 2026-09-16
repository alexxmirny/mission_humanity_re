//
// tact_unit_death_tick_selftest.cpp -- offline oracle for
//   llm_tact_unit_death_tick @0x0042fb0b (libmh/tact/tact_unit_death_tick.h)
//
// The sole outward call (llm_tact_unit_destroy) is mocked via the file's own
// unit_death_tick_calls table, the same shape as tact_unit_despawn_selftest.cpp's
// `unit_despawn_calls`. Derivation is tmp/decomp_tact/llm_tact_unit_death_tick_0042fb0b.asm /
// tact_unit_death_tick.h's banner:
//   1. @0x0042fb28-0x0042fb36: snapshot `type` = u.type BEFORE progress changes.
//   2. @0x0042fb39: a CMP whose flags nothing downstream reads -- confirmed dead, NOT exercised
//      here (there is nothing to observe: reproducing or dropping it is behaviourally identical).
//   3. @0x0042fb3d-0x0042fb44: byte-wide `INC progress`, wraps mod 256.
//   4. @0x0042fb4a-0x0042fb58: `CMP progress,0x69` / `JBE` on the POST-increment value -- "still
//      dying" iff progress<=0x69, i.e. progress<0x6a. Both sides of that boundary are pinned below
//      (T2/T3), plus the byte-wrap edge (T4) that a widening add would get wrong.
//      - still dying (@0x0042fb6b-0x0042fb88): `move_state_timer += character_types[type].death_time`
//        (FLD death_time / FADD move_state_timer / FSTP move_state_timer). Returns 0.
//      - threshold reached (@0x0042fb5a-0x0042fb69): `unit_destroy(unit_idx)`. Returns 1.
//
#include "tact/tact_unit_death_tick.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_ID = 13;

struct death_tick_recorder {
    std::vector<uint32_t> unit_destroy_args;
    void                  reset() { *this = death_tick_recorder{}; }
};
death_tick_recorder g_rec;

const unit_death_tick_calls &rec_calls() {
    static const unit_death_tick_calls c = {
        [](uint32_t unit_idx) { g_rec.unit_destroy_args.push_back(unit_idx); },
    };
    return c;
}

} // namespace

void run_unit_death_tick_tests() {
    // T1: nominal "still dying" tick -- progress advances by one, the x87 FADD accumulates
    // character_types[type].death_time into move_state_timer (bit-exact, ck_eq_d), unit_destroy is
    // NOT called, and every field this function doesn't touch survives as a distinct sentinel.
    // 0x0042fb28-0x0042fb88.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u = fx.units[UNIT_ID];
        u.type       = 4;
        u.progress   = 10; // well below the 0x69/0x6a threshold
        // Distinct, non-symmetric sentinels on fields this function must NOT write.
        u.status                         = 0x11;
        u.owner                          = 0x22;
        u.anim_state                     = 0x07;
        u.hp                             = 4321;
        u.cmd_index                      = 5;
        const double timer_before        = 12.75;
        u.move_state_timer               = timer_before;
        const double death_time          = 3.5;
        fx.character_types[4].death_time = death_time;

        tact_store    own = fx.store();
        const int32_t ret = detail::unit_death_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)ret, 0u, "T1: still-dying tick returns 0, 0x0042fb88/0x0042fb8f");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 11u,
              "T1: progress incremented by one (byte INC), 0x0042fb44");
        ck_eq_d(own.unit_at(UNIT_ID).move_state_timer, timer_before + death_time,
                "T1: move_state_timer += character_types[type].death_time, bit-exact FADD, "
                "0x0042fb76-0x0042fb82");
        ck_eq((uint32_t)g_rec.unit_destroy_args.size(), 0u,
              "T1: unit_destroy NOT called on the still-dying path");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).status, 0x11u, "T1: status sentinel untouched");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).owner, 0x22u, "T1: owner sentinel untouched");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).anim_state, 0x07u,
              "T1: anim_state sentinel untouched -- this function never writes it");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).hp, 4321u, "T1: hp sentinel untouched");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).cmd_index, 5u, "T1: cmd_index sentinel untouched");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).type, 4u, "T1: type itself is only READ, survives unchanged");
    }

    // T2: the threshold's "still dying" side -- pre-increment progress 0x68 -> post 0x69, and
    // 0x69<=0x69 (JBE) is true, so still dying. 0x0042fb51/0x0042fb58.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                     = fx.units[UNIT_ID];
        u.type                           = 0;
        u.progress                       = 0x68;
        fx.character_types[0].death_time = 1.0;
        u.move_state_timer               = 0.0;

        tact_store    own = fx.store();
        const int32_t ret = detail::unit_death_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)ret, 0u,
              "T2: progress 0x68->0x69, 0x69<=0x69 is still dying, returns 0, 0x0042fb58");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0x69u,
              "T2: progress landed exactly on the boundary value");
        ck_eq((uint32_t)g_rec.unit_destroy_args.size(), 0u,
              "T2: unit_destroy NOT called at the boundary's still-dying side");
    }

    // T3: the threshold's "destroy" side -- pre-increment progress 0x69 -> post 0x6a, and
    // 0x6a<=0x69 (JBE) is false, so destroy triggers. Proves the compare is STRICT (progress<0x6a,
    // not <=0x6a) and that unit_destroy is called with unit_idx forwarded (as uint32_t, per the
    // header's note on the callee's parameter type). move_state_timer must NOT be touched on this
    // path -- the FADD block is skipped entirely. 0x0042fb51-0x0042fb69.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                     = fx.units[UNIT_ID];
        u.type                           = 0;
        u.progress                       = 0x69;
        const double timer_sentinel      = 999.25; // must survive: destroy path never reaches the FADD
        u.move_state_timer               = timer_sentinel;
        fx.character_types[0].death_time = 42.0; // must NOT be added on this path

        tact_store    own = fx.store();
        const int32_t ret = detail::unit_death_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)ret, 1u, "T3: progress 0x69->0x6a crosses the threshold, returns 1, 0x0042fb62");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0x6au,
              "T3: progress still incremented before the branch, 0x0042fb44");
        ck_eq((uint32_t)g_rec.unit_destroy_args.size(), 1u,
              "T3: unit_destroy called exactly once, 0x0042fb5d");
        if (g_rec.unit_destroy_args.size() == 1) {
            ck_eq(g_rec.unit_destroy_args[0], (uint32_t)UNIT_ID,
                  "T3: unit_destroy(unit_idx) forwarded as uint32_t, 0x0042fb5a/0x0042fb5d");
        }
        ck_eq_d(own.unit_at(UNIT_ID).move_state_timer, timer_sentinel,
                "T3: move_state_timer untouched on the destroy path -- the FADD block is skipped "
                "entirely");
    }

    // T4: byte-wide wraparound -- pre-increment progress 0xff -> post 0x00 (mod 256), which is
    // <=0x69, so still dying. A widening (16/32-bit) add would instead produce 0x100, which is NOT
    // <=0x69 and would wrongly destroy the unit here -- this pins the INC's actual width. 0x0042fb44.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                     = fx.units[UNIT_ID];
        u.type                           = 0;
        u.progress                       = 0xff;
        fx.character_types[0].death_time = 0.5;
        u.move_state_timer               = 0.0;

        tact_store    own = fx.store();
        const int32_t ret = detail::unit_death_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)ret, 0u,
              "T4: 0xff+1 wraps to 0x00 mod 256, still <=0x69, still dying, 0x0042fb44/0x0042fb58");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T4: progress wrapped to 0, not 0x100");
        ck_eq((uint32_t)g_rec.unit_destroy_args.size(), 0u,
              "T4: unit_destroy NOT called -- a widening add would have wrongly triggered it here");
    }

    // T5: character_type indexing -- two distinct fixture states with different `type` values and
    // distinct, non-symmetric death_time per slot (the SAME two slots, roles swapped between T5a/T5b),
    // proving move_state_timer accumulates the death_time of the SNAPSHOT type (read before progress
    // changes), not a hardcoded or off-by-one slot. 0x0042fb28-0x0042fb36 (snapshot),
    // 0x0042fb72/0x0042fb76 (character_types[type].death_time, struct offset +0x3c).
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                     = fx.units[UNIT_ID];
        u.type                           = 3;
        u.progress                       = 1;
        fx.character_types[3].death_time = 7.25;  // the slot actually used
        fx.character_types[9].death_time = 100.0; // decoy at a DIFFERENT slot -- must be ignored
        const double timer_before        = 2.0;
        u.move_state_timer               = timer_before;

        tact_store own = fx.store();
        detail::unit_death_tick(own, rec_calls(), UNIT_ID);

        ck_eq_d(own.unit_at(UNIT_ID).move_state_timer, timer_before + 7.25,
                "T5a: death_time read from character_types[3] (u.type), not a hardcoded/decoy slot");
    }
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                     = fx.units[UNIT_ID];
        u.type                           = 9;
        u.progress                       = 1;
        fx.character_types[3].death_time = 7.25;  // decoy at a DIFFERENT slot -- must be ignored
        fx.character_types[9].death_time = 100.0; // the slot actually used
        const double timer_before        = 5.0;
        u.move_state_timer               = timer_before;

        tact_store own = fx.store();
        detail::unit_death_tick(own, rec_calls(), UNIT_ID);

        ck_eq_d(own.unit_at(UNIT_ID).move_state_timer, timer_before + 100.0,
                "T5b: death_time read from character_types[9] (u.type) -- the same two decoy slots "
                "swapped roles vs T5a");
    }
}

} // namespace mh::tact::test
