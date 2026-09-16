//
// sim/sim_hostreach_promote.h -- SIM-HOSTREACH Phase A (2026-09-10): the PROMOTION install glue for
// the 21 sim rows that ORIGINAL FRONT-END CODE still enters as originals. (18 at SIM-HOSTREACH;
// LIB-REF-IN added the three ex-register-hazard rows once their prototypes were committed -- see
// the adapter block in the .cpp for why that is an RE result and not a re-judged risk.)
//
// WHAT THIS FILE IS, and why it is not "more of SIM1-P". Every other promote module in this tree is
// organised by MIGRATION BATCH -- resid_promote.cpp is sim_resid's 32 rows, sim_lt_promote.cpp is
// lib_trans's 43, ai_promote.cpp is the AI closure. This one is organised by a MEASUREMENT instead:
// The promotion-reconciliation walk, section 5, takes the ORIGINAL binary's call graph with
// every installed entry cut, and reports which bodies we own that host code can still walk into.
// The sim domain has 530 verified rows and only 9 installed (the spine seams: sim_step,
// order_dispatch, unit_tick, building_tick, bldg_tick_animation_state), so the other 521 are reached
// by REBIND from inside those spine entries and are never entered from outside -- except for the
// leaves that also have a caller OUTSIDE the spine. Those leaves are this file.
//
// THE ROW SELECTION IS DERIVED, NOT CHOSEN. Section 5 named 60 such rows; 29 of them are "forced"
// (a closure ROOT calls them directly, so no ancestor cut can reach them and only their own
// installation removes them). Those 29 split three ways, per the caller's own nature:
//
//   18  FRONT-END-FORCED -- the reaching caller is render/HUD/panel/input/camera/dialog code that
//                           M6 deliberately leaves in the binary (the reimplementation plan's revised
//                           exit). That caller is NEVER going to be translated, so an installation
//                           is the permanent pre-fork answer. THIS FILE.
//    8  TRANSLATE-LATER  -- the reaching caller is original sim/logic code. Those rows stop being
//                           host-reachable when batch SIM1-H translates and promotes the caller;
//                           installing around them would be work the translation undoes. NOT here.
//    3  REGISTER-HAZARD  -- llm_strat_order_ctrlgrp_flash_member, ..._select_member and
//                           llm_strat_planet_distance read an `unaff_EBX` the declared prototype
//                           does not cover. Installing a redirect serves EVERY original caller,
//                           including register state the prototype cannot see (this is
//                           the REBIND-AI-ESI row), so these stay ORIGINAL and are recorded as
//                           named residue. NOT here, and -- because every edge forcing them is
//                           front-end -- SIM1-H will not liquidate them either.
//
// PER-ROW PROTOTYPE CHECK, done before any of the 18 was written here: all 18 decompiles were
// exported at EN v400 and scanned for `unaff_*`, `extraout_*`, bare `in_<REG>` reads, unrecovered
// jump tables and baddata markers. All 18 came back clean; the three that did not are the residue
// above. Two of the 18 carry a NON-DEFAULT calling convention that the generated thunk handles and
// the adapter therefore must not second-guess -- llm_strat_unit_ctrlgroup_{add,remove}_member are
// `__mh_watcall_ebx_volatile` and llm_strat_locate_active_port is `__mh_watcall_ecx_ebx_volatile`.
// Each adapter matches its `::mh::exp::sig_<orig>` typedef EXACTLY, so a wrong parameter list is a
// compile error rather than a stack that comes apart at runtime; none of the 18 needs a cast at the
// boundary (game_SetEvent's `game_e_event` is a `uint32_t` typedef, sim_game_set_event.h:46).
//
// ONE INI KEY FOR THE BATCH, per the ai_promote.h / sim_lt_promote.h precedent: `[promote]
// sim_hostreach`, shipping default passed in by the seams layer as SHIP_PROMOTE_SIM_HOSTREACH. The
// per-row red ladder is `[promote_skip] <original name>`, the same override every sibling module
// reads, so a single misbehaving row can be rolled back without rebuilding or disarming the other
// 17. Grouping choice stated explicitly because it is a judgement: these 18 do NOT form a call
// closure (they are leaves of unrelated sub-systems -- control groups, storage, tile geometry,
// building orders), so the "whole domain arms together or the internal calls hit pre-patch bytes"
// argument ai_promote.h makes does NOT apply here. They are grouped because one MEASUREMENT selects
// them and one gate run proves them, not because they need each other.
//
// PER-ROW FIRST-CALL LIVENESS. Each adapter logs, once, on its own first invocation:
//     "; [promote] sim_hostreach: <orig_name> call #1 (OURS is live)"
// The "(OURS is live)" suffix is exact -- tools/test_ui.py greps it -- do not reword it. A count
// alone cannot say WHICH rows a run actually entered, and for a set selected by "host code can
// still reach this", which ones the host really reaches is the interesting half.
//
#pragma once

namespace mh::sim {

// True once install_promotion_hostreach() has installed at least one seam this process.
bool hostreach_promotion_active();

// Install the 21 seams. `default_on` is the SHIPPING DEFAULT for `[promote] sim_hostreach`, PASSED
// IN by the seams layer (mh/seams/reimpl_probe.cpp, SHIP_PROMOTE_SIM_HOSTREACH) rather than read
// from a header here: this module must not include a seams header (the layering lint keeps
// binary-bound seam code out of the reimplementation), so the one call site is where "what ships"
// is answerable -- same discipline as mh::sim::install_promotion_resid,
// mh::sim::install_promotion_lib_trans and mh::orders::install_promotion.
// Returns the number of seams installed.
int install_promotion_hostreach(int default_on);

} // namespace mh::sim
