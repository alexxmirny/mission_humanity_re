//
// sim_register_state_handlers_selftest.cpp -- `simtest` oracle for OUR registrar
// (sim/sim_register_state_handlers.h/.cpp, RI-SIM / SIM1-DISPATCH, 2026-08-22).
//
// WHAT THIS ORACLE IS FOR, AND WHY THE LIVE RUN IS NOT ENOUGH. The golden A/B says whether the game
// still behaves the same with 68 of our handlers dispatched for real. It cannot say WHICH slot got
// which handler: give unit state 0x0f the plot_turn_path handler instead of move_walker and the
// trajectory diverges -- but the divergence names a step and a region, not the swapped pair, and a
// scenario where neither state ever fires reports MATCHED with the swap still in place. The pairing
// is a table, and a table is exactly what an offline check can read entry by entry.
//
// So `detail::fill_tables` is deliberately separable from the live binding: it takes two arrays and
// fills them, and everything below drives it over LOCAL arrays. No game memory is touched here.
//
// THE ORDER MATTERS AND IS CHECKED. The original registrar fills 0..0xfe with a default and THEN
// overwrites 44 unit / 34 bldg states. Two mistakes survive a naive "every slot is one of ours"
// check: filling the assignments first and then flattening them with the default (case D would go
// red), and skipping the default fill so unassigned states keep whatever was in .bss (case C).
//
// TWO ARMS SINCE FORK F5I S2, AND THE DIFFERENCE IS THE KEY, NOT THE CLAIM. `simtest` runs in the
// STANDALONE build now (libmh_selftest.exe, MH_LIBMH_BUILD), and this configuration cannot name an
// original address at all: mh_state_handlers.gen.h compiles the VA-bearing rows out so that
// libmh.lib's initialised data carries no original VA (LIB-REF-SPLIT's measured zero), and offers
// the SAME rows keyed by NAME instead. So the registrar's table is keyed by `original_va` hosted and
// by `name` standalone, the fill resolves 63 handlers to their C++ body directly and the five
// register-parameter ones to a reviewed zero-argument adapter, and EVERY claim below is unchanged.
// The whole difference is confined to the arm-neutral vocabulary right after this banner; every
// case reads the same in both arms and asserts the same pairing, slot by slot.
//
// EXPECTED CONTENT comes from addr/mh_state_handlers.gen.h, extracted by
// tools/gen_state_handler_table.py from llm_strat_register_state_handlers @0x0045f26a. This file
// deliberately re-derives NOTHING from it: it asserts that fill_tables reproduces the generated
// table, and separately (case G) pins two well-known pairings by NAME against the strategic-sim notes,
// so that a wholesale corruption of the generated table itself is still caught here.
//
#include "sim/sim_register_state_handlers.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "addr/mh_state_handlers.gen.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int32_t UNIT_SLOTS = mh::addr::UNIT_STATE_TABLE_SLOTS;
constexpr int32_t BLDG_SLOTS = mh::addr::BLDG_STATE_TABLE_SLOTS;

// A sentinel no thunk can ever equal, so "untouched" is distinguishable from "filled".
unit_state_fn sentinel_fn() { return reinterpret_cast<unit_state_fn>(static_cast<uintptr_t>(0xdeadbeefu)); }

// ---- THE ARM-NEUTRAL VOCABULARY (fork F5I S2) ---------------------------------------------------
//
// Everything past this block is written once and means the same thing in both builds. What differs
// is the KEY the registrar's binding table is looked up by -- the ORIGINAL VA hosted, the handler
// NAME standalone -- plus which of the two generated row sets carries the assignments. Both sets are
// the same rows in the same order; mh_state_handlers.gen.h emits them from one extraction.
#ifdef MH_LIBMH_BUILD
using bind_t = detail::sa_binding;
using key_t  = const char *;
using slot_t = mh::addr::state_handler_slot_sa;

const slot_t *const UNIT_ASSIGN   = mh::addr::UNIT_STATE_ASSIGN_SA;
constexpr int       UNIT_ASSIGN_N = mh::addr::UNIT_STATE_ASSIGN_SA_COUNT;
const slot_t *const BLDG_ASSIGN   = mh::addr::BLDG_STATE_ASSIGN_SA;
constexpr int       BLDG_ASSIGN_N = mh::addr::BLDG_STATE_ASSIGN_SA_COUNT;
constexpr key_t     UNIT_DEFAULT  = mh::addr::UNIT_STATE_DEFAULT_NAME;
constexpr key_t     BLDG_DEFAULT  = mh::addr::BLDG_STATE_DEFAULT_NAME;
// A key that is well-formed and is NOT a handler's, for the "the lookup is a lookup, not a
// fallback" arm. Hosted that is an in-image address nothing is registered at; here it is a name
// nothing is registered under, which is the same statement about the same function.
constexpr key_t NOT_A_HANDLER = "llm_strat_definitely_not_a_state_handler";

key_t key_of(const slot_t &r) { return r.name; }
bool  key_eq(key_t a, key_t b) { return strcmp(a, b) == 0; }
// RESOLVED rows, never the raw ones: the five register-parameter handlers are null in the raw table
// and resolve to their reviewed zero-argument adapter here, which is precisely what the fill
// installs, so this is the same view of the set the fill has.
const bind_t *bind_of(key_t k) { return detail::sa_handler_binding_for(k); }
int           binding_count() { return detail::sa_handler_binding_count(); }
const bind_t *binding_at(int i) { return bind_of(detail::sa_handler_binding_name(i)); }
key_t         binding_key(int i) { return detail::sa_handler_binding_name(i); }
const char   *binding_key_name(int i) { return detail::sa_handler_binding_name(i); }
unit_state_fn binding_fn(const bind_t *b) { return reinterpret_cast<unit_state_fn>(b->ours); }
#else
using bind_t = handler_binding;
using key_t  = uintptr_t;
using slot_t = mh::addr::state_handler_slot;

const slot_t *const UNIT_ASSIGN   = mh::addr::UNIT_STATE_ASSIGN;
constexpr int       UNIT_ASSIGN_N = mh::addr::UNIT_STATE_ASSIGN_COUNT;
const slot_t *const BLDG_ASSIGN   = mh::addr::BLDG_STATE_ASSIGN;
constexpr int       BLDG_ASSIGN_N = mh::addr::BLDG_STATE_ASSIGN_COUNT;
constexpr key_t     UNIT_DEFAULT  = mh::addr::UNIT_STATE_DEFAULT_VA;
constexpr key_t     BLDG_DEFAULT  = mh::addr::BLDG_STATE_DEFAULT_VA;
constexpr key_t     NOT_A_HANDLER = 0x00400000u;

key_t         key_of(const slot_t &r) { return r.original_va; }
bool          key_eq(key_t a, key_t b) { return a == b; }
const bind_t *bind_of(key_t k) { return detail::binding_for(k); }
int           binding_count() { return detail::handler_binding_count(); }
const bind_t *binding_at(int i) { return &detail::handler_bindings()[i]; }
key_t         binding_key(int i) { return detail::handler_bindings()[i].original_va; }
const char   *binding_key_name(int i) { return detail::handler_bindings()[i].name; }
unit_state_fn binding_fn(const bind_t *b) { return b->ours; }
#endif

// The body the fill is expected to put in a slot: the binding's, in either arm.
unit_state_fn expected_fn(key_t k) { return binding_fn(bind_of(k)); }

// The name of whichever binding owns `fn`, or "<none>" -- so a failure message says WHICH handler
// landed in the slot rather than only that the pointer differed.
const char *name_of(unit_state_fn fn) {
    for (int i = 0; i < binding_count(); ++i)
        if (binding_fn(binding_at(i)) == fn) return binding_key_name(i);
    return "<none>";
}

// Is this pointer one of OUR handlers? Hosted that is is_our_handler(), a membership test over the
// VA-keyed table. Standalone that table is empty by construction ("is this one of the ORIGINAL-keyed
// bindings" has no standalone meaning), so the same membership question is asked of the name-keyed
// set -- the same 68 bodies, reached the only way this arm can reach them.
bool ours(const void *p) {
#ifdef MH_LIBMH_BUILD
    for (int i = 0; i < binding_count(); ++i)
        if (reinterpret_cast<const void *>(binding_fn(binding_at(i))) == p) return true;
    return false;
#else
    return is_our_handler(p);
#endif
}

// ---- F5I R7: THE EXPECTED ALIAS GROUPS, ENUMERATED -------------------------------------------
//
// WHY THIS LIST EXISTS. Hosted, each binding's `ours` is the naked marshalling thunk
// MH_EXPORT_REPLACE generates PER ORIGINAL, so 68 rows hold 68 distinct addresses and A6's "no two
// originals share an entry thunk" is a pure injectivity claim. Standalone there is no Watcom caller
// to marshal for, so the slot holds the C++ FUNCTION ITSELF (sim_register_state_handlers.cpp says
// why) -- and MSVC's identical-COMDAT folding then gives two handlers whose bodies compile to the
// same bytes the same address. So the standalone body set is NOT injective, and cannot be: the
// binary really does contain distinct handlers that do exactly the same thing.
//
// WHAT REPLACES INJECTIVITY. Not "aliasing is allowed" -- that would delete the check. The set of
// alias groups is written out HERE, by name, from the SOURCE bodies rather than from a run, and A6
// asserts the measured grouping EQUALS it. A copy-paste that bound a second original to an existing
// wrapper adds a name to a group and reds A6; a handler that silently lost its body (became empty,
// and so folded onto the noops) joins a group it is not in and reds A6; and a group listed here
// whose members stop sharing a body reds it too, in the other direction.
//
// THE ONE GROUP, and why every member of it is legitimate -- all three originals are the SAME
// three instructions, `_G_LLM_STRAT_TICK_BUDGET = 0.0` and return, and our three wrappers are
// each the single statement `own.tick_budget() = 0.0;`:
//   llm_strat_unit_state_default_noop  0x0047e2c4  the unit table's default
//   llm_strat_unit_state_parked_noop   0x0047e28e  assigned to unit states 0x1f / 0x28 / 0x2b
//   llm_strat_bldg_state_idle_noop     0x0047121b  the building table's idle state
// sim_unit_state_budget_noop.cpp and sim_bldg_state_reset_idle.cpp both already record the identity
// at their definitions; this is the same fact, asserted instead of noted.
#ifdef MH_LIBMH_BUILD
const char *const ALIAS_G0[] = {
    "llm_strat_unit_state_default_noop",
    "llm_strat_unit_state_parked_noop",
    "llm_strat_bldg_state_idle_noop",
};

struct alias_group {
    const char *const *names;
    int                n;
};
const alias_group ALIAS_GROUPS[] = {{ALIAS_G0, 3}};
constexpr int     ALIAS_GROUP_N  = (int)(sizeof(ALIAS_GROUPS) / sizeof(ALIAS_GROUPS[0]));

// Which enumerated group a handler NAME belongs to, or -1 for the 65 that share with nobody.
int alias_group_of(const char *name) {
    for (int g = 0; g < ALIAS_GROUP_N; ++g)
        for (int k = 0; k < ALIAS_GROUPS[g].n; ++k)
            if (strcmp(ALIAS_GROUPS[g].names[k], name) == 0) return g;
    return -1;
}
#endif

bool state_is_assigned_unit(int s) {
    for (int i = 0; i < UNIT_ASSIGN_N; ++i)
        if (UNIT_ASSIGN[i].state == s) return true;
    return false;
}
bool state_is_assigned_bldg(int s) {
    for (int i = 0; i < BLDG_ASSIGN_N; ++i)
        if (BLDG_ASSIGN[i].state == s) return true;
    return false;
}

} // namespace

void run_register_state_handlers_tests() {
    printf("-- llm_strat_register_state_handlers (the two state-machine dispatch tables) --\n");

    // ---- A: the binding table itself (68 rows, one per distinct handler in the binary) ----------
    {
        const int n = binding_count();
        ck_eq((uint32_t)n, (uint32_t)mh::addr::STATE_HANDLER_DISTINCT_COUNT,
              "A1: one binding per distinct handler the registrar installs, 0x0045f26a");
        int nulls = 0, unresolved = 0, unnamed = 0;
        for (int i = 0; i < n; ++i) {
            const bind_t *b = bind_of(binding_key(i));
            if (b == nullptr) {
                ++unresolved;
                continue;
            }
            if (binding_fn(b) == nullptr) ++nulls;
            if (binding_key_name(i) == nullptr || binding_key_name(i)[0] == '\0') ++unnamed;
        }
        ck_eq((uint32_t)nulls, 0u, "A2: every binding has an entry thunk (a null slot would be a "
                                   "CALL to 0 the first time that state ticks)");
#ifdef MH_LIBMH_BUILD
        // A3's hosted form is "the key column is populated" (a zero VA would be a row the fill
        // cannot look up). The standalone key is the NAME, and the way it fails to be usable is that
        // the lookup refuses it -- a handler on the table but off the reviewed zero-parameter
        // allow-list resolves to nullptr, which is exactly the case the registrar's static_assert
        // and this arm exist to make loud rather than silent.
        ck_eq((uint32_t)unresolved, 0u,
              "A3: every binding's NAME resolves through the standalone lookup -- the name is this "
              "arm's key, and an unresolvable one is what a zero VA is hosted");
#else
        ck_eq((uint32_t)unresolved, 0u,
              "A3: every binding carries its original VA (mh::exp::addr_<fn>)");
#endif
        ck_eq((uint32_t)unnamed, 0u, "A4: every binding carries the original's symbol name");

        // Distinctness both ways. Two rows sharing a KEY means the X-macro emitted a handler twice;
        // two rows sharing a BODY means two originals were bound to one C++ wrapper -- which is the
        // copy-paste failure this whole file exists to catch, and it is invisible in a live run.
        int dup_key = 0, dup_fn = 0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                if (key_eq(binding_key(i), binding_key(j))) ++dup_key;
                if (binding_fn(binding_at(i)) == binding_fn(binding_at(j))) ++dup_fn;
            }
#ifdef MH_LIBMH_BUILD
        ck_eq((uint32_t)dup_key, 0u,
              "A5: no two bindings share a NAME -- the standalone fill is keyed by it, so a "
              "duplicate is what a duplicate original VA is hosted");
#else
        ck_eq((uint32_t)dup_key, 0u, "A5: no two bindings share an original VA");
#endif
#ifdef MH_LIBMH_BUILD
        // A6, this arm: the aliasing is EXPECTED but it is not free-form -- the measured grouping
        // must equal the enumerated one above, in both directions. `unexpected` is a pair that
        // shares a body and is not in one listed group (the copy-paste this file exists to catch);
        // `missing` is a listed pair that has stopped sharing one (the list has gone stale, or a
        // body changed and the note above no longer describes it).
        // ONE DIRECTION, AND THE REASON IS MEASURED. The aliasing is a LINK step -- MSVC's
        // identical-COMDAT folding -- so whether two identical bodies actually share an address is
        // a property of the build, not of the translation: the plain Release link folds all of
        // these, and the /fsanitize=address link folds NONE of them (the instrumentation makes the
        // bodies differ before the linker ever compares them). Measured both ways at fork F5I S2.
        // An equality against the list would therefore be red in exactly one of the two builds the
        // gate runs, which is why the claim is CONTAINMENT: a pair that shares a body must be in
        // one enumerated group. That is the direction the check exists for -- a copy-paste that
        // bound a second original to an existing wrapper creates an alias the list does not name,
        // and reds this in every configuration. The other direction (a listed group whose members
        // stopped sharing) cannot be asserted here for the reason above, and it is the harmless
        // one: it would mean a body diverged, which D1's slot-by-slot pairing and the C cases
        // already answer for.
        int unexpected = 0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                if (binding_fn(binding_at(i)) != binding_fn(binding_at(j))) continue;
                const int gi = alias_group_of(binding_key_name(i));
                const int gj = alias_group_of(binding_key_name(j));
                if (gi < 0 || gi != gj) ++unexpected;
            }
        char a6[224];
        std::snprintf(a6, sizeof(a6),
                      "A6: every pair of handlers that shares a BODY is inside one of the %d "
                      "enumerated alias group(s) -- %d pair(s) share one, %d of them unlisted",
                      ALIAS_GROUP_N, dup_fn, unexpected);
        ck_eq((uint32_t)unexpected, 0u, a6);
#else
        ck_eq((uint32_t)dup_fn, 0u, "A6: no two originals are bound to the SAME entry thunk");
#endif
    }

    // ---- B: every VA the generated tables name is bindable ---------------------------------------
    {
        ck(bind_of(UNIT_DEFAULT) != nullptr,
           "B1: the unit default (llm_strat_unit_state_default_noop @0x0047e2ac) has a binding");
        ck(bind_of(BLDG_DEFAULT) != nullptr,
           "B2: the bldg default (llm_strat_bldg_state_default_reset @0x004711c3) has a binding");
        int missing = 0;
        for (int i = 0; i < UNIT_ASSIGN_N; ++i)
            if (bind_of(key_of(UNIT_ASSIGN[i])) == nullptr) ++missing;
        for (int i = 0; i < BLDG_ASSIGN_N; ++i)
            if (bind_of(key_of(BLDG_ASSIGN[i])) == nullptr) ++missing;
        ck_eq((uint32_t)missing, 0u,
              "B3: every assigned state's handler VA resolves to a binding (78 assignments)");
        ck(bind_of(NOT_A_HANDLER) == nullptr,
           "B4: an address that is NOT a state handler does not resolve -- binding_for is a lookup, "
           "not a fallback");
    }

    // ---- C: the fill, over local arrays ----------------------------------------------------------
    std::vector<unit_state_fn> unit(UNIT_SLOTS, sentinel_fn());
    std::vector<bldg_state_fn> bldg(BLDG_SLOTS, sentinel_fn());
    {
        ck(detail::fill_tables(unit.data(), UNIT_SLOTS, bldg.data(), BLDG_SLOTS),
           "C1: fill_tables accepts two correctly-sized tables");

        int unit_foreign = 0, bldg_foreign = 0;
        for (int i = 0; i < UNIT_SLOTS; ++i)
            if (!ours(reinterpret_cast<const void *>(unit[i]))) ++unit_foreign;
        for (int i = 0; i < BLDG_SLOTS; ++i)
            if (!ours(reinterpret_cast<const void *>(bldg[i]))) ++bldg_foreign;
        // This is the clause SIM1-DISPATCH's done_when turns on: not "the assigned states are ours"
        // but EVERY slot, because llm_strat_unit_tick indexes by a byte it does not range-check
        // (0x0047c8d6 MOVZX / 0x0047c8dd CALL) and an unfilled slot is a call into stale .bss.
        ck_eq((uint32_t)unit_foreign, 0u,
              "C2: all 255 unit slots hold one of OUR handlers -- the default fill really ran");
        ck_eq((uint32_t)bldg_foreign, 0u, "C3: all 255 bldg slots hold one of OUR handlers");
    }

    // ---- D: each assigned state holds ITS OWN handler, and not the default -----------------------
    {
        int wrong = 0, flattened = 0;
#ifdef MH_LIBMH_BUILD
        int mis_aliased = 0;
#endif
        unit_state_fn unit_default = expected_fn(UNIT_DEFAULT);
        for (int i = 0; i < UNIT_ASSIGN_N; ++i) {
            const slot_t &s = UNIT_ASSIGN[i];
            if (unit[s.state] != expected_fn(key_of(s))) ++wrong;
            // The default is a legitimate handler for a state only if the binary assigns it there;
            // it does not, for any of the 44. A fill that ran the default loop LAST would pass C2.
            if (unit[s.state] == unit_default) ++flattened;
#ifdef MH_LIBMH_BUILD
            // D2, this arm. `== unit_default` cannot mean "flattened" here, because three handlers
            // legitimately COMPILE TO the default's body (the alias group above) -- an assigned
            // state holding llm_strat_unit_state_parked_noop compares equal to the default and is
            // nonetheless exactly right. So the claim becomes an IFF against the enumerated list:
            // a state is allowed to compare equal to the default precisely when the handler the
            // registrar assigns it is one of the names that share the default's body, and is
            // required to compare unequal otherwise. A fill that ran the default loop LAST still
            // reds this -- it would flatten the other 41 assignments, none of which are on the list.
            const int  gdflt   = alias_group_of(UNIT_DEFAULT);
            const int  gslot   = alias_group_of(key_of(s));
            const bool allowed = (gdflt >= 0 && gslot == gdflt);
            // One direction, for A6's measured reason: whether the fold happened is a property of
            // the link (plain Release folds, the ASan link does not), so "allowed" cannot be turned
            // into "must". What IS asserted is that a state comparing equal to the default is one
            // the list explains -- which is the whole of the original claim for the other 41.
            if (unit[s.state] == unit_default && !allowed) ++mis_aliased;
#endif
        }
        ck_eq((uint32_t)wrong, 0u,
              "D1: every one of the 44 assigned unit states holds the handler the registrar puts "
              "there (the pairing, slot by slot)");
#ifdef MH_LIBMH_BUILD
        ck_eq((uint32_t)mis_aliased, 0u,
              "D2: no assigned unit state was flattened back to the default -- an assigned state may "
              "compare equal to it only when its handler is one of the enumerated names that share "
              "the default's body -- the default fill runs "
              "FIRST (0x0045f282-0x0045f2a6), the assignments after");
#else
        ck_eq((uint32_t)flattened, 0u,
              "D2: no assigned unit state was flattened back to the default -- the default fill runs "
              "FIRST (0x0045f282-0x0045f2a6), the assignments after");
#endif

        wrong = flattened          = 0;
        bldg_state_fn bldg_default = reinterpret_cast<bldg_state_fn>(expected_fn(BLDG_DEFAULT));
        for (int i = 0; i < BLDG_ASSIGN_N; ++i) {
            const slot_t &s = BLDG_ASSIGN[i];
            if (bldg[s.state] != reinterpret_cast<bldg_state_fn>(expected_fn(key_of(s)))) ++wrong;
            if (bldg[s.state] == bldg_default) ++flattened;
        }
        ck_eq((uint32_t)wrong, 0u,
              "D3: every one of the 34 assigned bldg states holds its own handler");
        ck_eq((uint32_t)flattened, 0u,
              "D4: no assigned bldg state was flattened back to the default (0x0045f53c-0x0045f560)");
    }

    // ---- E: every UNassigned state holds the default ---------------------------------------------
    {
        int wrong = 0;
        for (int s = 0; s < UNIT_SLOTS; ++s)
            if (!state_is_assigned_unit(s) && unit[s] != expected_fn(UNIT_DEFAULT)) ++wrong;
        ck_eq((uint32_t)wrong, 0u,
              "E1: the 211 unassigned unit states hold llm_strat_unit_state_default_noop -- an "
              "unassigned state is a real call into the default, not a hole");
        wrong = 0;
        for (int s = 0; s < BLDG_SLOTS; ++s)
            if (!state_is_assigned_bldg(s) &&
                bldg[s] != reinterpret_cast<bldg_state_fn>(expected_fn(BLDG_DEFAULT)))
                ++wrong;
        ck_eq((uint32_t)wrong, 0u,
              "E2: the 221 unassigned bldg states hold llm_strat_bldg_state_default_reset");
    }

    // ---- F: the two tables are genuinely different -----------------------------------------------
    //
    // A fill that wrote the unit table twice (a copy-paste in fill_tables, the plausible bug) passes
    // A through E for the unit half and would leave the building half looking self-consistent too if
    // the checks above only compared each table against itself.
    {
        ck(unit[0x0f] != bldg[0x0f],
           "F1: the unit and bldg tables are not the same content -- state 0x0f differs");
        ck(!state_is_assigned_bldg(0x0f),
           "F2: (premise of F1) 0x0f is assigned in the unit table and NOT in the bldg one");
    }

    // ---- G: two pairings pinned BY NAME against the strategic-sim notes ----------------------------
    //
    // Everything above compares fill_tables against the generated header. If the extraction itself
    // were wrong, all of it would agree and all of it would be wrong together. These two are the
    // independent anchor: both are documented in prose, from before this generator existed.
    {
        ck_eq((uint32_t)(strcmp(name_of(unit[0x0f]), "llm_strat_unit_state_move_walker") == 0), 1u,
              "G1: unit state 0x0f is the walker move handler (the strategic-sim notes; 0x0045f2a8)");
        ck_eq((uint32_t)(strcmp(name_of(reinterpret_cast<unit_state_fn>(bldg[0x64])),
                                "llm_strat_bldg_state_construction") == 0),
              1u,
              "G2: bldg state 0x64 is the construction handler (the strategic-sim notes; 0x0045f562)");
        ck_eq((uint32_t)(strcmp(name_of(reinterpret_cast<unit_state_fn>(bldg[0x8b])),
                                "llm_strat_bldg_state_power_generate") == 0),
              1u,
              "G3: bldg state 0x8b is power_generate (the strategic-sim notes)");
    }

    // ---- H: the refusals. fill_tables writes NOTHING when it refuses -----------------------------
    //
    // The failure this guards is a region that is unrebased or short: a fill that got half way would
    // leave a LIVE dispatch table part ours and part stale .bss, which is strictly worse than not
    // installing at all. So each refusal is checked for its return value AND for the arrays being
    // untouched.
    {
        std::vector<unit_state_fn> u2(UNIT_SLOTS, sentinel_fn());
        std::vector<bldg_state_fn> b2(BLDG_SLOTS, sentinel_fn());

        ck(!detail::fill_tables(u2.data(), UNIT_SLOTS - 1, b2.data(), BLDG_SLOTS),
           "H1: a unit table one slot short is REFUSED");
        ck(u2[0] == sentinel_fn() && u2[UNIT_SLOTS - 2] == sentinel_fn(),
           "H2: ...and nothing was written before the refusal");

        ck(!detail::fill_tables(u2.data(), UNIT_SLOTS, b2.data(), BLDG_SLOTS - 1),
           "H3: a bldg table one slot short is REFUSED -- the size check covers BOTH tables");
        ck(u2[0] == sentinel_fn() && b2[0] == sentinel_fn(),
           "H4: ...and neither table was written (the unit half is checked first, so a fill that "
           "wrote it before validating the bldg size would fail here)");

        ck(!detail::fill_tables(nullptr, UNIT_SLOTS, b2.data(), BLDG_SLOTS),
           "H5: a null unit table is REFUSED rather than dereferenced");
        ck(!detail::fill_tables(u2.data(), UNIT_SLOTS, nullptr, BLDG_SLOTS),
           "H6: a null bldg table is REFUSED");
        ck(b2[0] == sentinel_fn(), "H7: ...and still nothing written");

        // A table LARGER than the binary's is fine -- the registrar fills what the binary fills and
        // does not run off the end of its own table.
        std::vector<unit_state_fn> u3(UNIT_SLOTS + 8, sentinel_fn());
        std::vector<bldg_state_fn> b3(BLDG_SLOTS + 8, sentinel_fn());
        ck(detail::fill_tables(u3.data(), UNIT_SLOTS + 8, b3.data(), BLDG_SLOTS + 8),
           "H8: an over-sized table is accepted");
        ck(u3[UNIT_SLOTS] == sentinel_fn() && b3[BLDG_SLOTS] == sentinel_fn(),
           "H9: ...and the slots past the binary's 255 are NOT written -- the setter's own bound is "
           "`id < 0xff` (0x0045f13f), so slot 255 is not part of the table");
    }

    // ---- I: is_our_handler is a membership test, not a range test --------------------------------
    {
        ck(ours(reinterpret_cast<const void *>(unit[0x0f])), "I1: a filled slot reads as ours");
        ck(!ours(reinterpret_cast<const void *>(sentinel_fn())), "I2: the sentinel does not");
        ck(!ours(nullptr), "I3: nullptr does not");
        ck(!ours(reinterpret_cast<const void *>(&run_register_state_handlers_tests)),
           "I4: an unrelated DLL-resident function does not -- so the first-dispatch note cannot be "
           "fooled by any of our own code that happens to sit near a thunk");
    }
}

} // namespace mh::sim::test
