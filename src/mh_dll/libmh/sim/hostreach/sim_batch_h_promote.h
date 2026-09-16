//
// sim/hostreach/sim_batch_h_promote.h -- SIM1-H (2026-09-10): the PROMOTION install glue for batch
// H, the 15 sim bodies this batch translated.
//
// WHAT BATCH H IS, and why it is not more of sim_hostreach_promote.cpp. That module is organised by
// a MEASUREMENT: `report_promotion_reconciliation.py` section 5 named 18 leaves that ORIGINAL
// FRONT-END code still enters, they are leaves of unrelated sub-systems, and installing one is the
// permanent pre-fork answer for a caller M6 never translates. This module is organised by a
// TRANSLATION BATCH. Its rows are the other third of that same section-5 classification -- the
// TRANSLATE-LATER callers, which are genuine sim/game logic -- plus everything their translation
// pulled in:
//
//    8  THE HOST-SIDE CALLERS (wave 1). SwitchToPlanet, llm_strat_bldg_init_defaults, the two
//       ctrl-group order issuers, the two map-region entry points, the shuttle-arrival dialog
//       handler and llm_diplomacy_restore_relations. They sit ONE LEVEL ABOVE llm_strat_sim_step,
//       so the sim domain's closure-minus-cuts walk never reached them -- they are its callers.
//       Promoting them is what stops 24 verified sim rows being reachable as originals.
//
//    7  WHAT WAVE 1 EXPOSED (wave 2). Translating llm_map_build_regions -- the nav-region
//       pipeline's entry point -- turned five of its callees into `mh::call::` sites that
//       gen_va_census could route to NO OWNING ITEM, because they are in no migration ledger at
//       all, while their siblings llm_map_merge_small_regions and llm_map_region_pick_smaller are
//       verified rows. THE PIPELINE WAS HALF-OWNED and nothing had measured it. A sixth,
//       llm_map_try_seed_region_at, came out of the pre-translation STOP check; a seventh,
//       llm_gfx_bldg_frame_center_offset, was adjudicated in (pure, 210 B, its only caller in the
//       image is a wave-1 row, and its twin llm_strat_bldg_sprite_anchor_offset was already a
//       verified sim row -- a prefix is not ownership).
//
// ONE INI KEY FOR THE BATCH: `[promote] sim_batch_h` / SHIP_PROMOTE_SIM_BATCH_H, with
// `[promote_skip] <original name>` as the per-row ladder -- the ai_promote.h / sim_lt_promote.h /
// sim_hostreach_promote.h precedent.
//
// BUT READ THE LADDER CORRECTLY, BECAUSE THIS SET DIFFERS FROM ITS SIBLING MODULE'S IN EXACTLY THE
// WAY THAT MATTERS. sim_hostreach_promote.h can say "a refused seam really is just one row staying
// original" because its 18 rows never call each other. Batch H is PARTLY A CALL CLOSURE: the
// nav-region pipeline is a connected subtree (build_regions -> seed_regions_multires ->
// try_seed_region_at -> region_flood_fill; assign_remaining_tiles -> region_flood_fill), and our
// bodies call each other DIRECTLY in C++ (translator-brief rules 3a/3c) rather than through the
// original entries. So:
//
//    * skipping a LEAF is exactly "this row stays original" -- SwitchToPlanet,
//      llm_strat_bldg_init_defaults, both group-order issuers, llm_diplomacy_restore_relations,
//      llm_strat_prod_shuttle_slot_spawn_arrival.
//    * skipping a PIPELINE MEMBER rolls back its ENTRY only. Original callers reach the original
//      body; our promoted llm_map_build_regions still calls OUR seed/flood bodies, because that
//      edge is a compile-time C++ call and no ini can reach it. That is not a defect -- it is what
//      "the batch owns the pipeline" means -- but a per-row skip is NOT a behavioural bisect for
//      those rows, and reading it as one would waste a rig run. Use `[promote] sim_batch_h=0` for a
//      real bisect; that one is unambiguous.
//
// PER-ROW PROTOTYPE CHECK, done before any of the 15 was written: all 15 decompiles were exported at
// EN v400 and scanned for `unaff_*`, `extraout_*`, bare `in_<REG>`, unrecovered jump tables and
// baddata. All 15 came back clean. THREE carry a NON-DEFAULT calling convention that the generated
// naked thunk owns and the adapters must not second-guess -- they forward POSITIONALLY:
//   llm_strat_group_issue_attack_order         __mh_watcall_ebx_volatile
//   llm_strat_group_issue_enter_building_order __mh_watcall_ebx_volatile
//   llm_map_region_flood_fill                  __mh_watcall_ebx_volatile   (found by its translator;
//                                              the batch context's rule-2 table named only the first
//                                              two, so this is a third occurrence, not a surprise)
// Each adapter matches its `::mh::exp::sig_<orig>` typedef EXACTLY, so a wrong parameter list is a
// compile error rather than a stack that comes apart at runtime.
//
// PER-ROW FIRST-CALL LIVENESS. Each adapter logs, once, on its own first invocation:
//     "; [promote] sim_batch_h: <orig_name> call #1 (OURS is live)"
// The "(OURS is live)" suffix is exact -- tools/test_ui.py greps it -- do not reword it. A count
// alone cannot say WHICH rows a run entered, and for a batch whose rows fire at very different
// rates (bldg_init_defaults once per boot per building type; region_flood_fill thousands of times
// per planet load; SwitchToPlanet only on a planet transition) which ones a run reached is the
// interesting half.
//
#pragma once

namespace mh::sim {

// True once install_promotion_batch_h() has installed at least one seam this process.
bool batch_h_promotion_active();

// Install the 15 seams. `default_on` is the SHIPPING DEFAULT for `[promote] sim_batch_h`, PASSED IN
// by the seams layer (mh/seams/reimpl_probe.cpp, SHIP_PROMOTE_SIM_BATCH_H) rather than read from a
// header here: this module must not include a seams header (the layering lint keeps binary-bound
// seam code out of the reimplementation), so the one call site is where "what ships" is answerable.
// Returns the number of seams installed.
int install_promotion_batch_h(int default_on);

} // namespace mh::sim
