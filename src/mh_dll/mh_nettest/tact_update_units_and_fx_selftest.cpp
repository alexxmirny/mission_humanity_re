//
// tact_update_units_and_fx_selftest.cpp -- offline oracle for llm_tact_update_units_and_fx. See
// tact/tact_update_units_and_fx.h for the full derivation (two independent scans, the redundant
// double reset of MOVE_PATH_CACHE_VALID, and the two gate conditions).
//
// STRUCTURAL LIMIT, not fixed here: this function calls its two already-translated TACT1C siblings
// (unit_weapons_tick / fx_update_projectile) through their OWN PUBLIC production wrappers
// (translator-brief 3b's in-manifest-sibling exception), not through a mockable `_calls` struct.
// Those wrappers bind their outward calls to real game VAs (live_unit_weapons_tick_calls() /
// live_fx_update_projectile_calls()), and unit_weapons_tick's own first action on every call is an
// UNCONDITIONAL time_GetCurrentTime() through exactly such a binding -- so actually invoking either
// sibling in this standalone harness (net_selftest.exe: "no game, no DllMain", README.md) would
// crash the whole suite on entry, not fail one check, regardless of what unit/fx data is seeded.
// This oracle therefore proves only the SAFE side: both gates correctly EXCLUDE the boundary values
// (type==0, type==0x80, fx_type==0) so neither sibling is ever reached, and the two writes to
// move_path_cache_valid are observed. The INCLUSION side (type in [1,0x7f) calls unit_weapons_tick;
// fx_type!=0 calls fx_update_projectile) is proven only by the header's own byte-level re-derivation
// of the two CMP/Jcc pairs against the raw .asm, not at runtime here.
//
#include "tact/tact_update_units_and_fx.h"
#include "tact_test_support.h"

namespace mh::tact::test {

void run_update_units_and_fx_tests() {
    // T1: every unit type==0 (empty) and every fx_pool slot fx_type==0 (free) -- BOTH gates excluded
    // for the WHOLE roster/pool, so neither sibling call site is ever reached; safe to run for real.
    // move_path_cache_valid ends at 0 via the redundant double reset (both writes are 0 regardless).
    {
        tact_fixture fx;
        fx.move_path_cache_valid = 1; // distinct from the expected 0, so the reset is observable
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) fx.units[i].type = 0;
        for (int32_t i = 0; i < TACT_FX_POOL_SLOTS; ++i) fx.fx_pool[i].fx_type = 0;

        tact_store own = fx.store();
        detail::update_units_and_fx(fx.view(), own);

        ck_eq((uint32_t)own.move_path_cache_valid(), 0u,
              "T1: move_path_cache_valid reset to 0 (both writes), 0x0042b00a/0x0042b02e");
    }

    // T2: the two EXCLUSION boundaries for scan 1 -- type==0 (JBE skip) and type==0x80 (JC excludes,
    // unsigned >= 0x80) are the two edge values the gate's CONJUNCTION must reject; neither may reach
    // the sibling call. Every other slot stays type==0 too (both boundaries + the whole rest excluded
    // -> zero reachable calls, safe to run for real).
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) fx.units[i].type = 0;
        fx.units[TACT_UNIT_FIRST_SLOT].type     = 0;             // lower boundary: type==0 excluded by JBE
        fx.units[TACT_UNIT_FIRST_SLOT + 1].type = (uint8_t)0x80; // upper boundary: type==0x80 excluded by JC (unsigned)
        for (int32_t i = 0; i < TACT_FX_POOL_SLOTS; ++i) fx.fx_pool[i].fx_type = 0;

        tact_store own = fx.store();
        // The call under test does not crash for either boundary value -- if the gate's conjunction
        // were wrong (e.g. JBE/JC turned into JB/JBE, admitting 0 or 0x80), this line would attempt a
        // real-VA call and the whole suite would die here rather than print a clean failure. Reaching
        // the check below IS the passing evidence for this case.
        detail::update_units_and_fx(fx.view(), own);
        ck(true, "T2: type==0 and type==0x80 are both correctly EXCLUDED -- no crash, 0x0042b03f-0x0042b056");
    }

    // T3: fx-pool scan's exclusion boundary -- fx_type==0 (JBE skip, "0 = free slot" per
    // mh_llm_tact_fx::fx_type's own comment) is excluded across every slot; safe to run for real.
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) fx.units[i].type = 0;
        for (int32_t i = 0; i < TACT_FX_POOL_SLOTS; ++i) fx.fx_pool[i].fx_type = 0;

        tact_store own = fx.store();
        detail::update_units_and_fx(fx.view(), own);
        ck(true, "T3: fx_type==0 excluded across the whole pool -- no crash, 0x0042b082-0x0042b089");
    }
}

} // namespace mh::tact::test
