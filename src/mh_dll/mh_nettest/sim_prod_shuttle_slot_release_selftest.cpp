//
// sim_prod_shuttle_slot_release_selftest.cpp -- `simtest` oracle for llm_strat_prod_shuttle_slot_release
// @0x0046318d (sim/sim_prod_shuttle_slot_release.h/.cpp, RI-SIM / SIM1D).
//
// arm_ready:false in spirit -- shadow_region_closure.py --depth 4 came back bounded (1 function, 1
// region) and the site WAS armed for real, but a 15000-step all-AI soak made ZERO calls into it: its
// callers (prod_shuttle_complete / bldg_state_destroyed / unit_on_destroyed /
// prod_shuttle_slot_bind_default) all need a unit/building holding a shuttle-bay slot to be destroyed,
// or a shuttle cycle to complete, and the soak never hit that. THIS OFFLINE ORACLE IS THE ONLY EVIDENCE
// THIS FUNCTION WILL EVER HAVE.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_prod_shuttle_slot_release.h):
//   Every store recomputes the SAME slot base (player*0x1f18 + slot*0x31c) -- no branches anywhere in
//   the 0x19b-byte body besides the two FIXED-COUNT zeroing loops. Eleven scalar stores, all
//   unconditional:
//     0x004631ba type_ref_id=0, 0x004631d3 src_building_type=0, 0x004631ec src_building_index=0,
//     0x00463205 status=0, 0x0046321e origin_planet=0, 0x00463237 is_planet_bound=0,
//     0x00463251 is_heli_mother_pending=0, 0x0046326b dest_planet=0,
//     0x00463284/0x0046328e travel_duration (both dword halves, one double)=0.0,
//     0x004632a8 passengers_reserved=0.
//   travel_duration_copy (offset 0x28) is DELIBERATELY left untouched -- no store anywhere in the body
//   targets it (banner "FIELD DELIBERATELY LEFT UNTOUCHED"). This is the load-bearing claim this
//   oracle exists to PIN: a case that would fail if travel_duration_copy were zeroed too.
//   Loop 1 (0x004632b9-0x004632ea, i<0xa): resources_reserved[i]=0 for i in [0,10), stride 4.
//   Loop 2 (0x004632ea-0x0046331f, i<0x32): ONE 2-byte MOV per iteration at i*0xe within
//     cargo_manifest_raw -- zeroes only the LEADING uint16_t of each of the first 50 conceptual
//     14-byte manifest entries; bytes 2-13 of every entry are left alone.
//   No reads anywhere in the body; no outward calls besides the inert stack-capacity probe. Nothing
//   past this slot's own record (0x31c bytes) is ever touched.
//
#include "sim/sim_prod_shuttle_slot_release.h"

#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t TARGET_PLAYER = 3;
constexpr int32_t  TARGET_SLOT   = 7;

// Raw sentinel byte for everything in the record (covers the _pad_0x00 header bytes and every byte of
// cargo_manifest_raw). Non-zero so "left untouched" is observable everywhere the explicit overrides
// below don't apply.
constexpr uint8_t RAW_SENTINEL = 0xCD;

// Fills one slot record with a non-zero, non-symmetric pattern: raw 0xCD everywhere, then explicit
// distinct values for every field the function is claimed to zero (so a skipped store is caught) plus
// travel_duration_copy (the one field claimed to survive).
void fill_slot_sentinel(prod_shuttle_slot &s) {
    std::memset(&s, RAW_SENTINEL, sizeof(s));
    s.travel_duration        = 4242.5;
    s.travel_duration_copy   = 8181.25; // must SURVIVE the call -- the deliberate non-touch field
    s.passengers_reserved    = 9001;
    s.is_planet_bound        = 9002;
    s.is_heli_mother_pending = 9003;
    for (int32_t i = 0; i < 10; ++i) s.resources_reserved[i] = 3000 + i * 7;
}

// Byte-for-byte snapshot of one record, for isolation checks that don't want to hand-reconstruct an
// expected value -- any deviation at all is a bug regardless of which field moved.
void snapshot(const prod_shuttle_slot &s, uint8_t *out) { std::memcpy(out, &s, sizeof(s)); }

bool unchanged(const prod_shuttle_slot &s, const uint8_t *snap) {
    return std::memcmp(&s, snap, sizeof(s)) == 0;
}

} // namespace

void run_prod_shuttle_slot_release_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the eight scalar identity/state fields, all unconditional, all zeroed regardless of prior
    // content. Each check cites its own store address so a mutation that drops or mis-targets ONE
    // store is caught by name.
    // =================================================================================================
    {
        fx.reset();
        sim_store          own = fx.store();
        prod_shuttle_slot &s   = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT);
        fill_slot_sentinel(s);

        detail::prod_shuttle_slot_release(own, TARGET_PLAYER, TARGET_SLOT);

        ck_eq((uint32_t)s.type_ref_id, 0u, "T1: type_ref_id = 0, 0x004631ba");
        ck_eq((uint32_t)(uint16_t)s.src_building_type, 0u, "T1: src_building_type = 0, 0x004631d3");
        ck_eq((uint32_t)(uint16_t)s.src_building_index, 0u, "T1: src_building_index = 0, 0x004631ec");
        ck_eq((uint32_t)(uint16_t)s.status, 0u, "T1: status = 0, 0x00463205");
        ck_eq((uint32_t)(uint16_t)s.origin_planet, 0u, "T1: origin_planet = 0, 0x0046321e");
        ck_eq((uint32_t)s.is_planet_bound, 0u, "T1: is_planet_bound = 0, 0x00463237");
        ck_eq((uint32_t)s.is_heli_mother_pending, 0u, "T1: is_heli_mother_pending = 0, 0x00463251");
        ck_eq((uint32_t)(uint16_t)s.dest_planet, 0u, "T1: dest_planet = 0, 0x0046326b");
    }

    // =================================================================================================
    // T2 -- travel_duration (both dword halves, one bit-pattern-zero double) is zeroed, but
    // travel_duration_copy (offset 0x28, the FULL original travel time) is DELIBERATELY left untouched
    // -- no store anywhere in the 0x19b-byte body targets displacement 0xbd2184. This is the header
    // banner's load-bearing claim: reversed (i.e. copy zeroed too), this case fails. passengers_reserved
    // is also checked here as the last of the plain scalar stores (0x004632a8).
    // =================================================================================================
    {
        fx.reset();
        sim_store          own = fx.store();
        prod_shuttle_slot &s   = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT);
        fill_slot_sentinel(s);

        detail::prod_shuttle_slot_release(own, TARGET_PLAYER, TARGET_SLOT);

        ck_eq_d(s.travel_duration, 0.0, "T2: travel_duration = 0.0 (both dwords), 0x00463284/0x0046328e");
        ck_eq_d(s.travel_duration_copy, 8181.25,
                "T2: travel_duration_copy PRESERVED (offset 0x28 never stored to) -- deliberate non-touch");
        ck_eq((uint32_t)s.passengers_reserved, 0u, "T2: passengers_reserved = 0, 0x004632a8");
    }

    // =================================================================================================
    // T3 -- resources_reserved[10], stride 4, loop 1 (0x004632b9-0x004632ea). All ten distinctly-seeded
    // elements go to zero; index 9 is checked explicitly as the loop's own boundary (a `i<9` mutation
    // would leave it at its seeded 3063).
    // =================================================================================================
    {
        fx.reset();
        sim_store          own = fx.store();
        prod_shuttle_slot &s   = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT);
        fill_slot_sentinel(s);

        detail::prod_shuttle_slot_release(own, TARGET_PLAYER, TARGET_SLOT);

        for (int32_t i = 0; i < 10; ++i) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "T3: resources_reserved[%d] = 0, loop 0x004632b9-0x004632ea",
                          i);
            ck_eq((uint32_t)s.resources_reserved[i], 0u, msg);
        }
    }

    // =================================================================================================
    // T4 -- cargo_manifest_raw loop 2 (0x004632ea-0x0046331f, i<0x32, stride 0xe). Only the LEADING
    // uint16_t of each conceptual 14-byte entry is a 2-byte MOV; bytes 2-13 of an entry are never
    // touched. Entry 0 pins "only 2 bytes, not the whole entry"; entry 49 (the LAST of the 50, at
    // 49*14=686) pins the loop's own upper boundary (a `i<0x31` mutation would leave entry 49's word at
    // its seeded 0xCDCD).
    // =================================================================================================
    {
        fx.reset();
        sim_store          own = fx.store();
        prod_shuttle_slot &s   = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT);
        fill_slot_sentinel(s);

        detail::prod_shuttle_slot_release(own, TARGET_PLAYER, TARGET_SLOT);

        uint16_t entry0_word = 0xffff, entry49_word = 0xffff;
        std::memcpy(&entry0_word, &s.cargo_manifest_raw[0], sizeof(entry0_word));
        std::memcpy(&entry49_word, &s.cargo_manifest_raw[49 * 14], sizeof(entry49_word));

        ck_eq((uint32_t)entry0_word, 0u, "T4: cargo_manifest_raw entry 0 leading word = 0, 0x004632de");
        ck_eq((uint32_t)entry49_word, 0u,
              "T4: cargo_manifest_raw entry 49 (loop upper boundary, i=0x31) leading word = 0");
        ck_eq((uint32_t)s.cargo_manifest_raw[2], (uint32_t)RAW_SENTINEL,
              "T4: cargo_manifest_raw entry 0 byte[2] untouched -- only 2 bytes per entry are stored");
        ck_eq((uint32_t)s.cargo_manifest_raw[13], (uint32_t)RAW_SENTINEL,
              "T4: cargo_manifest_raw entry 0 byte[13] (last byte of the 14-byte entry) untouched");
    }

    // =================================================================================================
    // T5 -- addressing isolation: the slot base is player*0x1f18 + slot*0x31c, recomputed fresh before
    // every store. Fills EVERY slot with the sentinel pattern, calls for (TARGET_PLAYER, TARGET_SLOT),
    // then full-byte-snapshots five OTHER records to prove none of them moved: the two same-player
    // neighbor slots (catches a stride/off-by-one in the slot term), the two same-slot neighbor players
    // (catches a stride/off-by-one in the player term), and the ARGUMENT-SWAPPED index
    // (slot,player)=(7,3) (catches a player/slot parameter-order swap at the call site, since 0x1f18 !=
    // 0x31c makes a swap land on a genuinely different record). A sixth snapshot of the target's own
    // record BEFORE the call proves the call actually changed something (a no-op oracle would pass all
    // the "untouched" checks vacuously).
    // =================================================================================================
    {
        fx.reset();
        sim_store own = fx.store();
        for (uint16_t p = 0; p < 8; ++p) {
            for (int32_t sl = 0; sl < 10; ++sl) fill_slot_sentinel(own.prod_shuttle_slot_at(p, sl));
        }

        prod_shuttle_slot &target = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT);
        uint8_t            target_before[sizeof(prod_shuttle_slot)];
        snapshot(target, target_before);

        prod_shuttle_slot &slot_minus   = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT - 1);
        prod_shuttle_slot &slot_plus    = own.prod_shuttle_slot_at(TARGET_PLAYER, TARGET_SLOT + 1);
        prod_shuttle_slot &player_minus = own.prod_shuttle_slot_at(TARGET_PLAYER - 1, TARGET_SLOT);
        prod_shuttle_slot &player_plus  = own.prod_shuttle_slot_at(TARGET_PLAYER + 1, TARGET_SLOT);
        prod_shuttle_slot &swapped      = own.prod_shuttle_slot_at(TARGET_SLOT, TARGET_PLAYER);
        uint8_t            slot_minus_snap[sizeof(prod_shuttle_slot)];
        uint8_t            slot_plus_snap[sizeof(prod_shuttle_slot)];
        uint8_t            player_minus_snap[sizeof(prod_shuttle_slot)];
        uint8_t            player_plus_snap[sizeof(prod_shuttle_slot)];
        uint8_t            swapped_snap[sizeof(prod_shuttle_slot)];
        snapshot(slot_minus, slot_minus_snap);
        snapshot(slot_plus, slot_plus_snap);
        snapshot(player_minus, player_minus_snap);
        snapshot(player_plus, player_plus_snap);
        snapshot(swapped, swapped_snap);

        detail::prod_shuttle_slot_release(own, TARGET_PLAYER, TARGET_SLOT);

        ck(!unchanged(target, target_before),
           "T5: the target record itself DID change -- sanity check against a vacuous pass");
        ck(unchanged(slot_minus, slot_minus_snap),
           "T5: slot-1 (same player) fully untouched -- slot-term addressing, base player*0x1f18+slot*0x31c");
        ck(unchanged(slot_plus, slot_plus_snap),
           "T5: slot+1 (same player) fully untouched -- also rules out a loop-2 overrun past 50 entries");
        ck(unchanged(player_minus, player_minus_snap),
           "T5: player-1 (same slot) fully untouched -- player-term addressing (stride 0x1f18)");
        ck(unchanged(player_plus, player_plus_snap),
           "T5: player+1 (same slot) fully untouched -- player-term addressing (stride 0x1f18)");
        ck(unchanged(swapped, swapped_snap),
           "T5: (slot,player) swapped index untouched -- catches a player/slot argument-order swap");
    }
}

} // namespace mh::sim::test
