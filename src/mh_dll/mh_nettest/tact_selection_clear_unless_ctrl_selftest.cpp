#include "tact/tact_selection_clear_unless_ctrl.h"
#include "tact_test_support.h"

namespace mh::tact::test {

void run_selection_clear_unless_ctrl_tests() {
    // T1: LCtrl held via bit 0x1 -> no-op. Seed every unit's status with bit 0 SET plus other bits,
    // so a spurious clear would be visible. 0x0042af85-0x0042afa9.
    {
        tact_fixture fx;
        fx.key_lctrl_held = 0x1;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].status = 0xff;
        }

        tact_store own = fx.store();
        detail::selection_clear_unless_ctrl(fx.view(), own);

        bool all_untouched = true;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            if (own.unit_at(i).status != 0xff) all_untouched = false;
        }
        ck(all_untouched, "T1: LCtrl bit 0x1 held -> gate returns before the loop, 0x0042af85");
    }

    // T2: LCtrl held via bit 0x2 (the second encoding the same byte packs) -> also a no-op.
    {
        tact_fixture fx;
        fx.key_lctrl_held = 0x2;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].status = 0xff;
        }

        tact_store own = fx.store();
        detail::selection_clear_unless_ctrl(fx.view(), own);

        bool all_untouched = true;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            if (own.unit_at(i).status != 0xff) all_untouched = false;
        }
        ck(all_untouched, "T2: LCtrl bit 0x2 held -> gate returns before the loop, 0x0042af9x");
    }

    // T3: LCtrl held via BOTH bits set at once -> still a no-op (the gate ORs the two masks).
    {
        tact_fixture fx;
        fx.key_lctrl_held                     = 0x3;
        fx.units[TACT_UNIT_FIRST_SLOT].status = 0xff;

        tact_store own = fx.store();
        detail::selection_clear_unless_ctrl(fx.view(), own);

        ck_eq((uint32_t)own.unit_at(TACT_UNIT_FIRST_SLOT).status, 0xffu,
              "T3: both LCtrl bits set -> still a no-op");
    }

    // T4: LCtrl NOT held (byte reads 0) -> clear bit 0 on EVERY slot in [FIRST_SLOT, LAST_SLOT]
    // inclusive, and ONLY bit 0 -- seed 0xff so a wider clear (e.g. `status = 0`) is caught, and
    // seed slot 0 / TACT_UNIT_SLOTS (one past LAST_SLOT) distinctly to prove the loop bound is
    // exact, not off-by-one in either direction. 0x0042afaf-0x0042afe6.
    {
        tact_fixture fx;
        fx.key_lctrl_held                = 0x0;
        fx.units[0].status               = 0xff; // one BEFORE the loop's first slot
        fx.units[TACT_UNIT_SLOTS].status = 0xff; // one AFTER the loop's last slot (TACT_UNIT_SLOTS == 129)
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].status = 0xff;
        }

        tact_store own = fx.store();
        detail::selection_clear_unless_ctrl(fx.view(), own);

        bool in_range_cleared_bit0_only = true;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            if (own.unit_at(i).status != 0xfe) in_range_cleared_bit0_only = false;
        }
        ck(in_range_cleared_bit0_only,
           "T4: every slot in [FIRST_SLOT, LAST_SLOT] has ONLY bit 0 cleared (0xff -> 0xfe), "
           "0x0042afaf-0x0042afe6");
        ck_eq((uint32_t)own.unit_at(0).status, 0xffu, "T4: slot 0 (before the loop) is untouched");
        ck_eq((uint32_t)own.unit_at(TACT_UNIT_SLOTS).status, 0xffu,
              "T4: slot TACT_UNIT_SLOTS (after the loop) is untouched");
    }
}

} // namespace mh::tact::test
