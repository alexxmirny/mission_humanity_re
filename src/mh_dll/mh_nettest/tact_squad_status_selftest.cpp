//
// tact_squad_status_selftest.cpp -- offline oracle for llm_tact_squad_sync_hp (TACT1A batch A,
// 2026-08-26). See tact/tact_squad_status.h for the derivation.
//
// WHY OFFLINE, NOT RIG: the rig arms this site correctly (region size fixed, see the 2026-08-26
// finding in ghidra_findings.json) but the ONLY caller sits on the mission-exit-confirm arm of
// llm_tact_frame, which a force-entered no-input scenario never drives -- migration_sweep.py
// reports "0 call(s), 0 divergence(s)" against tact-save 11's 15000-frame run, confirmed
// reproducible. TACT1A's own done_when calls for exactly this: "a case that asserts the blackboard
// CONTENT after the call... a mutation to the stride or the base is caught."
//
#include "tact/tact_squad_status.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

// Seeds every squad-status slot to a KNOWN, DISTINCT sentinel so an untouched slot is
// distinguishable from one the function legitimately zeroed or left alone.
void seed_untouched(tact_fixture &fx) {
    for (int32_t i = 0; i < TACT_SQUAD_STATUS_SLOTS; ++i) {
        auto &s           = fx.squad_status[(size_t)i];
        s.unit_proto_id   = 900 + i; // never read by this function; must survive untouched
        s.energy_pct      = -1;      // <=0 -- the "slot unused" sentinel this function must SKIP
        s.unit_slot_index = 700 + i; // never read/written; must survive untouched
        s.is_commando     = (i % 2); // never read/written; must survive untouched
    }
}

} // namespace

void run_squad_status_tests() {
    // T1: energy_pct <= 0 at entry -- skip entirely, no write of any field.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size     = 4;
        fx.units[1].hp    = 50; // would matter if slot 0 were processed -- it must not be
        fx.units[1].owner = 0;
        fx.units[1].type  = 1;
        tact_view  v      = fx.view();
        tact_store own    = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)(int32_t)fx.squad_status[0].energy_pct, (uint32_t)-1,
              "T1: energy_pct<=0 slot is skipped entirely, 0x00438f46/0x0043 8f4d");
        ck_eq((uint32_t)fx.squad_status[0].unit_proto_id, 900u,
              "T1: skipped slot's unit_proto_id is untouched");
    }

    // T2: units[slot+1].hp == 0 -> energy_pct := 0 (0x00438f5d-0x00438f6d), regardless of owner/type.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 2;
        fx.squad_status[0].energy_pct = 42; // > 0, so the slot is processed
        fx.units[1].hp                = 0;
        fx.units[1].owner             = 0;
        fx.units[1].type              = 3;
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 0u, "T2: hp==0 -> energy_pct := 0, 0x00438f6d");
    }

    // T3: hp != 0 AND owner != 0 -> energy_pct := 0 (0x00438f86-0x00438fec) -- a DIFFERENT
    // instruction than T2's zero, but the SAME observable value, which is why T2 and T3 need
    // DIFFERENT hp/owner combinations to tell apart which arm actually ran.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 2;
        fx.squad_status[0].energy_pct = 42;
        fx.units[1].hp                = 77; // nonzero -- proves this is NOT the T2 arm
        fx.units[1].owner             = 5;  // nonzero -- selects this arm
        fx.units[1].type              = 3;
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 0u,
              "T3: hp!=0, owner!=0 -> energy_pct := 0, 0x00438fe6-0x00438fec");
    }

    // T4: hp != 0, owner == 0 -> energy_pct := hp*100 / character_types[type].energy, no floor
    // needed (the division does not truncate to 0). hp=50, energy=200 -> 25.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 2;
        fx.squad_status[0].energy_pct = 42;
        fx.units[1].hp                = 50;
        fx.units[1].owner             = 0;
        fx.units[1].type              = 6;
        fx.character_types[6].energy  = 200;
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 25u,
              "T4: energy_pct := hp*100/character_types[type].energy, 0x00438fc3/0x00438fde");
    }

    // T5: the SAME division truncating to 0 is floored to 1 by a SEPARATE branch
    // (0x00438fc8-0x00438fd5), not by the IDIV itself. hp=1, energy=1000 -> 1*100/1000 == 0 -> 1.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 2;
        fx.squad_status[0].energy_pct = 42;
        fx.units[1].hp                = 1;
        fx.units[1].owner             = 0;
        fx.units[1].type              = 9;
        fx.character_types[9].energy  = 1000;
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 1u,
              "T5: a division truncating to 0 is floored to 1, 0x00438fce");
    }

    // T6: 1-based roster indexing -- slot i reads units[i+1], NOT units[i]. Seed slot 0's own
    // units[0] entry with a value that would produce a DIFFERENT result if the function read the
    // 0-based index by mistake.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 1;
        fx.squad_status[0].energy_pct = 42;
        fx.units[0].hp                = 999; // if read (wrong index), would not floor to 1
        fx.units[0].owner             = 0;
        fx.units[0].type              = 0;
        fx.character_types[0].energy  = 1;
        fx.units[1].hp                = 5;
        fx.units[1].owner             = 0;
        fx.units[1].type              = 2;
        fx.character_types[2].energy  = 500; // 5*100/500 == 1, distinguishable from units[0]'s math
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 1u,
              "T6: slot 0 reads units[1], not units[0] (1-based roster, TACT_UNIT_FIRST_SLOT)");
    }

    // T7: the loop bound is *v.squad_size, not a fixed 64 -- a slot at or beyond squad_size must
    // NOT be touched even if it holds energy_pct > 0.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 1;
        fx.squad_status[1].energy_pct = 77; // > 0, but slot 1 is beyond squad_size=1
        fx.units[2].hp                = 0;  // would zero it if the loop reached slot 1
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[1].energy_pct, 77u,
              "T7: loop bound is *squad_size (0x00438f2b), a slot beyond it is never touched");
    }

    // T8: a mutation to the WRITE STRIDE or BASE (own.squad_status_at's indexing, or a wrong RID)
    // would move every write here -- this case's neighbour-untouched assertions (slots -1/+1 via
    // the T7 fixture's independent slot 1) are what catches it. Explicit here for the record:
    // slot 0's write must land in slot 0, not slot 1.
    {
        tact_fixture fx;
        seed_untouched(fx);
        fx.squad_size                 = 1;
        fx.squad_status[0].energy_pct = 42;
        fx.units[1].hp                = 0;
        tact_view  v                  = fx.view();
        tact_store own                = fx.store();
        detail::squad_sync_hp(v, own);
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 0u, "T8a: slot 0 IS written");
        ck_eq((uint32_t)(int32_t)fx.squad_status[1].energy_pct, (uint32_t)-1,
              "T8b: slot 1 is NOT written -- pins the stride/base");
    }
}

} // namespace mh::tact::test
