//
// sim_register_bldg_type_callbacks_selftest.cpp -- `simtest` oracle for OUR registrar
// (sim/sim_register_bldg_type_callbacks.h/.cpp, RI-SIM / SIM1-BLDGCB, 2026-08-23).
//
// WHAT THIS ORACLE IS FOR, AND WHY THE LIVE RUN IS NOT ENOUGH. Same argument as the state-table
// registrar's own oracle, one notch sharper. The golden A/B says whether the game still behaves the
// same with our 30 callbacks dispatched for real. It cannot say WHICH building got which callback:
// give the port kind the shuttle animation handler and the trajectory diverges -- but the
// divergence names a step and a region, not the swapped pair, and a scenario in which neither kind
// is ever built reports MATCHED with the swap still in place. The pairing is a table, and a table is
// what an offline check can read entry by entry.
//
// THE SHARPER PART: THIS REGISTRAR DOES NOT INDEX BY TYPE, IT SCANS BY IT. The original setters
// (llm_bldg_register_done_callback @0x0045f1ae, _tick2_ @0x0045f20c) loop over building ids 1..99
// and write the callback into every slot whose `Building[id].type` equals the type argument. So the
// property under test is not "slot N holds callback X" -- it is "the scan mapped a cfg table onto
// two tables the way the original's would". That needs a cfg table to scan, which is exactly what a
// live golden run cannot vary and this file can: every case below drives detail::
// fill_bldg_callback_tables over LOCAL arrays with a SYNTHETIC cfg. No game memory is touched here.
//
// FOUR MISTAKES THIS IS WRITTEN TO CATCH, each of which survives a green golden run:
//   * the pairing swapped or shifted           -> case D (and the by-name anchors, case J)
//   * the default fill run LAST, flattening the assignments -> case D2/D5
//   * the scan stopping at the first match, so only ONE of the several buildings sharing a type
//     gets the callback                        -> case K, which is the failure mode unique to this
//     registrar and has no counterpart in the state tables
//   * "helpfully" filling the ids whose type is outside the default range, which the original
//     leaves ALONE                             -> case F
//
// TWO ARMS SINCE FORK F5I S2, AND THE DIFFERENCE IS THE KEY, NOT THE CLAIM -- the sibling of the
// note in sim_register_state_handlers_selftest.cpp, and the same argument. `simtest` runs in the
// STANDALONE build now (libmh_selftest.exe, MH_LIBMH_BUILD), which cannot name an original address
// at all: mh_bldg_type_callbacks.gen.h compiles the VA-bearing rows out so libmh.lib's initialised
// data carries no original VA (LIB-REF-SPLIT's measured zero), and offers the SAME rows keyed by
// NAME. Every case below asserts the same pairing, type by type; the whole difference is confined to
// the arm-neutral vocabulary after this banner.
//
// EXPECTED CONTENT comes from addr/mh_bldg_type_callbacks.gen.h, extracted by
// tools/gen_state_handler_table.py --extract-bldg-type from llm_strat_register_bldg_type_callbacks
// @0x0045f76a. This file deliberately re-derives NOTHING from it: it asserts that the fill
// reproduces the generated table, and separately (case J) pins four pairings by NAME against the
// registrar's own listing, so that a wholesale corruption of the generated table is still caught.
//
#include "sim/sim_register_bldg_type_callbacks.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "addr/mh_bldg_type_callbacks.gen.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int ID_LIMIT = mh::addr::BLDG_TYPE_ID_LIMIT;
constexpr int FIRST_ID = mh::addr::BLDG_TYPE_FIRST_ID;

// A sentinel no thunk can ever equal, so "untouched" is distinguishable from "filled".
bldg_done_fn sentinel_fn() {
    return reinterpret_cast<bldg_done_fn>(static_cast<uintptr_t>(0xdeadbeefu));
}

// ---- THE ARM-NEUTRAL VOCABULARY (fork F5I S2) ---------------------------------------------------
//
// See the banner. The KEY is the original VA hosted and the callback NAME standalone; both generated
// row sets are the same rows in the same order, from one extraction.
#ifdef MH_LIBMH_BUILD
using bind_t = detail::sa_cb_binding_row;
using key_t  = const char *;
using slot_t = mh::addr::bldg_type_callback_slot_sa;

const slot_t *const DONE_ASSIGN    = mh::addr::BLDG_DONE_ASSIGN_SA;
constexpr int       DONE_ASSIGN_N  = mh::addr::BLDG_DONE_ASSIGN_SA_COUNT;
const slot_t *const TICK2_ASSIGN   = mh::addr::BLDG_TICK2_ASSIGN_SA;
constexpr int       TICK2_ASSIGN_N = mh::addr::BLDG_TICK2_ASSIGN_SA_COUNT;
constexpr key_t     DONE_DEFAULT   = mh::addr::BLDG_DONE_DEFAULT_NAME;
constexpr key_t     TICK2_DEFAULT  = mh::addr::BLDG_TICK2_DEFAULT_NAME;
// The two the scan cases name directly, by the only key this arm has.
constexpr key_t PORT_DONE_KEY  = "llm_strat_done_port";
constexpr key_t PORT_TICK2_KEY = "llm_strat_bldg_anim_state_port";
constexpr key_t NOT_A_CALLBACK = "llm_strat_definitely_not_a_bldg_type_callback";

key_t key_of(const slot_t &r) { return r.name; }
bool  key_eq(key_t a, key_t b) { return strcmp(a, b) == 0; }
// RESOLVED rows, never the raw ones: the eight register-parameter callbacks are null in the raw
// table and resolve to their reviewed zero-argument adapter here, which is what the fill installs.
const bind_t *bind_of(key_t k) { return detail::sa_bldg_type_binding_for(k); }
int           binding_count() { return detail::sa_bldg_type_binding_count(); }
const char   *binding_key_name(int i) { return detail::sa_bldg_type_binding_name(i); }
const bind_t *binding_at(int i) { return bind_of(binding_key_name(i)); }
key_t         binding_key(int i) { return binding_key_name(i); }
bldg_done_fn  binding_fn(const bind_t *b) { return reinterpret_cast<bldg_done_fn>(b->ours); }
#else
using bind_t = bldg_type_callback_binding;
using key_t  = uintptr_t;
using slot_t = mh::addr::bldg_type_callback_slot;

const slot_t *const DONE_ASSIGN    = mh::addr::BLDG_DONE_ASSIGN;
constexpr int       DONE_ASSIGN_N  = mh::addr::BLDG_DONE_ASSIGN_COUNT;
const slot_t *const TICK2_ASSIGN   = mh::addr::BLDG_TICK2_ASSIGN;
constexpr int       TICK2_ASSIGN_N = mh::addr::BLDG_TICK2_ASSIGN_COUNT;
constexpr key_t     DONE_DEFAULT   = mh::addr::BLDG_DONE_DEFAULT_VA;
constexpr key_t     TICK2_DEFAULT  = mh::addr::BLDG_TICK2_DEFAULT_VA;
constexpr key_t     PORT_DONE_KEY  = 0x00470198u; // llm_strat_done_port
constexpr key_t     PORT_TICK2_KEY = 0x004786f5u; // llm_strat_bldg_anim_state_port
constexpr key_t     NOT_A_CALLBACK = 0x00400000u;

key_t         key_of(const slot_t &r) { return r.original_va; }
bool          key_eq(key_t a, key_t b) { return a == b; }
const bind_t *bind_of(key_t k) { return detail::bldg_type_binding_for(k); }
int           binding_count() { return detail::bldg_type_callback_binding_count(); }
const char   *binding_key_name(int i) { return detail::bldg_type_callback_bindings()[i].name; }
const bind_t *binding_at(int i) { return &detail::bldg_type_callback_bindings()[i]; }
key_t         binding_key(int i) { return detail::bldg_type_callback_bindings()[i].original_va; }
bldg_done_fn  binding_fn(const bind_t *b) { return b->ours; }
#endif

// The body the fill is expected to put in a slot: the binding's, in either arm.
bldg_done_fn expected_fn(key_t k) { return binding_fn(bind_of(k)); }

// The name of whichever binding owns `fn`, or "<none>" -- so a failure message says WHICH callback
// landed in the slot rather than only that the pointer differed.
const char *name_of(bldg_done_fn fn) {
    for (int i = 0; i < binding_count(); ++i)
        if (binding_fn(binding_at(i)) == fn) return binding_key_name(i);
    return "<none>";
}

// Is this pointer one of OUR callbacks? Hosted that is is_our_bldg_type_callback(), a membership
// test over the VA-keyed table. Standalone that table is empty by construction ("is this one of the
// ORIGINAL-keyed bindings" has no standalone meaning), so the same membership question is asked of
// the name-keyed set -- the same 30 bodies, reached the only way this arm can reach them.
bool ours(const void *p) {
#ifdef MH_LIBMH_BUILD
    for (int i = 0; i < binding_count(); ++i)
        if (reinterpret_cast<const void *>(binding_fn(binding_at(i))) == p) return true;
    return false;
#else
    return is_our_bldg_type_callback(p);
#endif
}

bool done_type_is_assigned(int ty) {
    for (int i = 0; i < DONE_ASSIGN_N; ++i)
        if (DONE_ASSIGN[i].type == ty) return true;
    return false;
}
bool tick2_type_is_assigned(int ty) {
    for (int i = 0; i < TICK2_ASSIGN_N; ++i)
        if (TICK2_ASSIGN[i].type == ty) return true;
    return false;
}

// The scan's INPUT. `cfg_building` is 0x842 bytes, so the 100 records live on the heap (a stack
// array would be 211 KB); `sim_fixture` already owns one for the same reason and is reused rather
// than allocating a second.
//
// THE TYPE LAYOUT USED BY EVERY CASE BELOW, chosen so each property has a witness:
//   id 0                 -- type 0x0c, and the id loop starts at 1, so it must stay UNTOUCHED
//   ids 1..0x27          -- type == id: one building per type across the whole default-fill range,
//                           which makes every ASSIGN row reachable and every unassigned type too
//   ids 0x28..0x2b       -- types 0, 0x28, 0x29, 0xff: all OUTSIDE the default range, so the
//                           original never writes them (case F)
//   ids 0x2c, 0x2d       -- type 0x0c again, i.e. THREE ids share one type (0x0c, 0x2c, 0x2d).
//                           A scan that stops at the first match fills 0x0c and leaves these two
//                           at the sentinel (case K)
//   ids 0x2e..0x63       -- type 0x01, harmless filler that also proves the loop runs to the end
void seed_cfg(sim_fixture &fx) {
    for (int id = 0; id < ID_LIMIT; ++id) fx.cfg_buildings[id].type = 0x01;
    fx.cfg_buildings[0].type = 0x0c;
    for (int id = 1; id <= 0x27; ++id) fx.cfg_buildings[id].type = (uint8_t)id;
    fx.cfg_buildings[0x28].type = 0x00;
    fx.cfg_buildings[0x29].type = 0x28;
    fx.cfg_buildings[0x2a].type = 0x29;
    fx.cfg_buildings[0x2b].type = 0xff;
    fx.cfg_buildings[0x2c].type = 0x0c;
    fx.cfg_buildings[0x2d].type = 0x0c;
}

// Assemble the binder struct over local tables + the fixture's cfg. `done_slots`/`tick2_slots`/
// `cfg_count` are passed explicitly so the refusal cases can shrink one of them.
sim_bldg_callback_tables tables_over(sim_fixture &fx, std::vector<bldg_done_fn> &done,
                                     std::vector<bldg_tick2_fn> &tick2, int32_t done_slots,
                                     int32_t tick2_slots, int32_t cfg_count) {
    sim_bldg_callback_tables t{};
    t.done        = done.empty() ? nullptr : done.data();
    t.tick2       = tick2.empty() ? nullptr : tick2.data();
    t.cfg         = fx.cfg_buildings.data();
    t.done_slots  = done_slots;
    t.tick2_slots = tick2_slots;
    t.cfg_count   = cfg_count;
    return t;
}


// ---- F5I R7: THE EXPECTED ALIAS GROUPS, ENUMERATED -------------------------------------------
//
// The sibling of the list in sim_register_state_handlers_selftest.cpp, and the same argument.
// Hosted, `ours` is the per-original MH_EXPORT_REPLACE thunk, so 39 rows hold 39 distinct addresses
// and A6 is pure injectivity. Standalone the slot holds the C++ function itself, and MSVC's
// identical-COMDAT folding gives two callbacks whose bodies compile to the same bytes the same
// address -- which they must, because the binary really does contain distinct done-callbacks that
// do exactly the same thing. So the grouping is written out here, by name, FROM THE SOURCE BODIES,
// and A6 asserts the measured grouping equals it in both directions.
//
// THE THREE GROUPS, each with the reason its members legitimately share one body:
//
//   G0 -- THE EMPTY ONES. All three originals have no calls and no writes at all:
//         llm_strat_done_default          0x0045f1ae-ish, the done table's default
//         llm_strat_done_shuttle          0x0047014f  ("no calls, no writes", per its own comment)
//         llm_strat_bldg_anim_state_turret            the tick2 callback for type 0x05
//         Our wrappers are literally `{}` (sim_bldg_done_handlers.cpp, sim_bldg_anim_tick.cpp).
//
//   G1 -- THE SINGLE power_consume() ONES. Four originals whose whole body is one call to
//         llm_strat_bldg_power_consume:
//         llm_strat_done_relay      0x00470171 -> 0x00470189
//         llm_strat_done_production 0x004700ae -> 0x004700c6
//         llm_strat_done_turret     0x004700fc -> 0x00470114
//         llm_strat_done_lab        0x004700d5 -> 0x004700ed
//
//   G2 -- THE add_storage_capacity() + power_consume() PAIR:
//         llm_strat_done_mine       0x0047005f
//         llm_strat_done_silos      0x00470123 -> 0x0047013b, 0x00470140
//
// Each is a separate C++ function on purpose -- they are separate originals with separate names and
// separate call sites, and collapsing them in SOURCE would lose that -- so the folding is a link
// step, not a translation shortcut. What must not happen silently is a FOURTH group appearing, or a
// name moving between groups: either means a callback's body changed and nobody noticed.
#ifdef MH_LIBMH_BUILD
const char *const ALIAS_G0[] = {
    "llm_strat_done_default",
    "llm_strat_done_shuttle",
    "llm_strat_bldg_anim_state_turret",
};
const char *const ALIAS_G1[] = {
    "llm_strat_done_relay",
    "llm_strat_done_production",
    "llm_strat_done_turret",
    "llm_strat_done_lab",
};
const char *const ALIAS_G2[] = {
    "llm_strat_done_mine",
    "llm_strat_done_silos",
};

struct alias_group {
    const char *const *names;
    int                n;
};
const alias_group ALIAS_GROUPS[] = {{ALIAS_G0, 3}, {ALIAS_G1, 4}, {ALIAS_G2, 2}};
constexpr int     ALIAS_GROUP_N  = (int)(sizeof(ALIAS_GROUPS) / sizeof(ALIAS_GROUPS[0]));

int alias_group_of(const char *name) {
    for (int g = 0; g < ALIAS_GROUP_N; ++g)
        for (int k = 0; k < ALIAS_GROUPS[g].n; ++k)
            if (strcmp(ALIAS_GROUPS[g].names[k], name) == 0) return g;
    return -1;
}
#endif

} // namespace

void run_register_bldg_type_callbacks_tests() {
    printf("-- llm_strat_register_bldg_type_callbacks (the two per-building-TYPE tables) --\n");

    sim_fixture fx;
    fx.reset();
    seed_cfg(fx);

    // ---- A: the binding table itself (30 rows, one per distinct callback in the binary) ---------
    {
        const int n = binding_count();
        ck_eq((uint32_t)n, (uint32_t)mh::addr::BLDG_TYPE_CALLBACK_DISTINCT_COUNT,
              "A1: one binding per distinct callback the registrar installs, 0x0045f76a");
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
        ck_eq((uint32_t)nulls, 0u,
              "A2: every binding has an entry thunk (a null slot would be a CALL to 0 the first "
              "time a building of that kind ticks)");
#ifdef MH_LIBMH_BUILD
        // A3's hosted form is "the key column is populated". The standalone key is the NAME, and the
        // way it fails to be usable is that the lookup refuses it -- a callback on the table but off
        // the reviewed zero-parameter allow-list resolves to nullptr, which is the case the
        // registrar's static_assert and this arm exist to make loud rather than silent.
        ck_eq((uint32_t)unresolved, 0u,
              "A3: every binding's NAME resolves through the standalone lookup -- the name is this "
              "arm's key, and an unresolvable one is what a zero VA is hosted");
#else
        ck_eq((uint32_t)unresolved, 0u,
              "A3: every binding carries its original VA (mh::exp::addr_<fn>)");
#endif
        ck_eq((uint32_t)unnamed, 0u, "A4: every binding carries the original's symbol name");

        // Distinctness both ways. Two rows sharing a KEY means the X-macro emitted a callback twice;
        // two rows sharing a BODY means two originals were bound to one C++ wrapper -- the
        // copy-paste failure this file exists to catch, invisible in a live run.
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
        // A6, this arm: the measured grouping must EQUAL the enumerated one above, both ways.
        // `unexpected` is a pair that shares a body and is in no listed group -- the copy-paste
        // this file exists to catch; `missing` is a listed pair that has stopped sharing one.
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
                      "A6: every pair of callbacks that shares a BODY is inside one of the %d "
                      "enumerated alias group(s) -- %d pair(s) share one, %d of them unlisted",
                      ALIAS_GROUP_N, dup_fn, unexpected);
        ck_eq((uint32_t)unexpected, 0u, a6);
#else
        ck_eq((uint32_t)dup_fn, 0u, "A6: no two originals are bound to the SAME entry thunk");
#endif
    }

    // ---- B: every VA the generated tables name is bindable ---------------------------------------
    {
        ck(bind_of(DONE_DEFAULT) != nullptr,
           "B1: the done default (llm_strat_done_default @0x0046ff2c) has a binding");
        ck(bind_of(TICK2_DEFAULT) != nullptr,
           "B2: the tick2 default (llm_strat_bldg_anim_tick @0x00476447) has a binding");
        int missing = 0;
        for (int i = 0; i < DONE_ASSIGN_N; ++i)
            if (bind_of(key_of(DONE_ASSIGN[i])) == nullptr) ++missing;
        for (int i = 0; i < TICK2_ASSIGN_N; ++i)
            if (bind_of(key_of(TICK2_ASSIGN[i])) == nullptr) ++missing;
        ck_eq((uint32_t)missing, 0u,
              "B3: every assigned type's callback VA resolves to a binding (50 assignments)");
        ck(bind_of(NOT_A_CALLBACK) == nullptr,
           "B4: an address that is NOT a bldg-type callback does not resolve -- "
           "bldg_type_binding_for is a lookup, not a fallback");
    }

    // ---- C: the fill, over local arrays and the synthetic cfg -------------------------------------
    std::vector<bldg_done_fn>  done(ID_LIMIT, sentinel_fn());
    std::vector<bldg_tick2_fn> tick2(ID_LIMIT, (bldg_tick2_fn)sentinel_fn());
    int                        filled_done = -1, filled_tick2 = -1;
    {
        sim_bldg_callback_tables t = tables_over(fx, done, tick2, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        ck(detail::fill_bldg_callback_tables(t, &filled_done, &filled_tick2),
           "C1: fill_bldg_callback_tables accepts two correctly-sized tables + a full cfg table");

        // The cfg seeded above puts 0x27 types in range at ids 1..0x27, plus ids 0x2c/0x2d (type
        // 0x0c) and ids 0x2e..0x63 (type 0x01) -- 39 + 2 + 54 = 95 ids. The four
        // out-of-range ids (0x28..0x2b) and id 0 are NOT among them. Spelled out rather than
        // recomputed from the same loop the fill uses, so a fill that scanned the wrong id range
        // cannot agree with its own arithmetic here.
        ck_eq((uint32_t)filled_done, 95u,
              "C2: the done table now holds OUR callback at exactly the 95 ids whose cfg type is in "
              "0x01..0x27 -- a CFG-DEPENDENT count, which is why the registrar logs one");
        ck_eq((uint32_t)filled_tick2, 95u, "C3: ...and the tick2 table at the same 95");
    }

    // ---- D: each id holds the callback ITS type is paired with, and not the default ---------------
    {
        int wrong = 0, flattened = 0;
#ifdef MH_LIBMH_BUILD
        int mis_aliased = 0;
#endif
        for (int i = 0; i < DONE_ASSIGN_N; ++i) {
            const slot_t &a  = DONE_ASSIGN[i];
            const int     id = a.type; // the seed puts type==id for 1..0x27
            if (done[id] != expected_fn(key_of(a))) ++wrong;
            // The default is a legitimate callback for a type only if the binary assigns it there;
            // it does not, for any of the 30 done assignments. A fill that ran the default loop
            // LAST would pass C2 and fail here.
            if (done[id] == expected_fn(DONE_DEFAULT)) ++flattened;
#ifdef MH_LIBMH_BUILD
            // D2, this arm. `== the default` cannot mean "flattened" here: llm_strat_done_shuttle
            // is an EMPTY callback and so compiles to the default's body (group G0 above), and the
            // two types it is assigned to -- 0x0d and 0x21 -- are then indistinguishable from the
            // default by pointer alone. The claim becomes an IFF against the enumerated list: a
            // type may compare equal to the default exactly when the callback the registrar pairs
            // with it shares the default's body, and must compare unequal otherwise. A fill that
            // ran the default loop LAST still reds this -- it would flatten the other 28
            // assignments, none of which are in G0.
            const int  gdflt   = alias_group_of(DONE_DEFAULT);
            const int  gslot   = alias_group_of(key_of(a));
            const bool allowed = (gdflt >= 0 && gslot == gdflt);
            // One direction, for A6's measured reason -- the fold is a link step, and the ASan
            // link does not perform it. What is asserted is that a type comparing equal to the
            // default is one the list explains; the other 28 are held to the original claim.
            if (done[id] == expected_fn(DONE_DEFAULT) && !allowed) ++mis_aliased;
#endif
        }
        ck_eq((uint32_t)wrong, 0u,
              "D1: every one of the 30 assigned done types holds the callback the registrar pairs "
              "with it (the pairing, type by type)");
#ifdef MH_LIBMH_BUILD
        ck_eq((uint32_t)mis_aliased, 0u,
              "D2: no assigned done type was flattened back to llm_strat_done_default -- an assigned "
              "type may compare equal to it only when the callback paired with it is one of the "
              "enumerated names that share the default's body -- the default fill runs FIRST "
              "(0x0045f782-0x0045f7a3), the assignments after");
#else
        ck_eq((uint32_t)flattened, 0u,
              "D2: no assigned done type was flattened back to llm_strat_done_default -- the "
              "default fill runs FIRST (0x0045f782-0x0045f7a3), the assignments after");
#endif

        wrong = flattened = 0;
        for (int i = 0; i < TICK2_ASSIGN_N; ++i) {
            const slot_t &a  = TICK2_ASSIGN[i];
            const int     id = a.type;
            if ((bldg_done_fn)tick2[id] != expected_fn(key_of(a))) ++wrong;
            if ((bldg_done_fn)tick2[id] == expected_fn(TICK2_DEFAULT))
                ++flattened;
        }
        ck_eq((uint32_t)wrong, 0u,
              "D4: every one of the 20 assigned tick2 types holds its own callback");
        ck_eq((uint32_t)flattened, 0u,
              "D5: no assigned tick2 type was flattened back to llm_strat_bldg_anim_tick "
              "(0x0045f967-0x0045f988)");
    }

    // ---- E: every UNassigned in-range type holds the default -------------------------------------
    {
        int wrong_done = 0, wrong_tick2 = 0, seen_done = 0, seen_tick2 = 0;
        for (int ty = mh::addr::BLDG_DONE_DEFAULT_TYPE_LO; ty <= mh::addr::BLDG_DONE_DEFAULT_TYPE_HI;
             ++ty) {
            if (done_type_is_assigned(ty)) continue;
            ++seen_done;
            if (done[ty] != expected_fn(DONE_DEFAULT)) ++wrong_done;
        }
        for (int ty = mh::addr::BLDG_TICK2_DEFAULT_TYPE_LO;
             ty <= mh::addr::BLDG_TICK2_DEFAULT_TYPE_HI; ++ty) {
            if (tick2_type_is_assigned(ty)) continue;
            ++seen_tick2;
            if ((bldg_done_fn)tick2[ty] != expected_fn(TICK2_DEFAULT))
                ++wrong_tick2;
        }
        // Non-vacuity for E itself: if the ASSIGN tables ever covered the whole range, the two loops
        // above would examine nothing and report clean.
        ck(seen_done > 0 && seen_tick2 > 0,
           "E1: (premise) both tables have types the registrar leaves on the default");
        ck_eq((uint32_t)wrong_done, 0u,
              "E2: an in-range done type with no assignment holds llm_strat_done_default -- it is a "
              "real call into the default, not a hole");
        ck_eq((uint32_t)wrong_tick2, 0u,
              "E3: an in-range tick2 type with no assignment holds llm_strat_bldg_anim_tick");
    }

    // ---- F: an id whose type is OUTSIDE the default range is LEFT ALONE ---------------------------
    //
    // This is the behaviour that differs from the state registrar, and the one a "robust"
    // reimplementation would quietly break by filling every slot. The original's default fill loops
    // over TYPES 1..0x27 (0x0045f789 CMP ...,0x28), so a building whose cfg type is 0 or >= 0x28 is
    // never named by any setter call and its slot keeps whatever was there.
    {
        ck(done[0x28] == sentinel_fn() && (bldg_done_fn)tick2[0x28] == sentinel_fn(),
           "F1: id 0x28 (cfg type 0x00, below the default range) is UNTOUCHED in both tables");
        ck(done[0x29] == sentinel_fn() && (bldg_done_fn)tick2[0x29] == sentinel_fn(),
           "F2: id 0x29 (cfg type 0x28, one past the range's 0x27) is UNTOUCHED");
        ck(done[0x2a] == sentinel_fn() && done[0x2b] == sentinel_fn(),
           "F3: ids 0x2a/0x2b (cfg types 0x29 and 0xff) are UNTOUCHED -- the type byte is MOVZX'd, "
           "so 0xff is 255 and out of range, never -1 and in it (0x0045f1e6)");
    }

    // ---- G: id 0 is never written -- the setters' id loop starts at 1 ----------------------------
    {
        ck_eq((uint32_t)FIRST_ID, 1u,
              "G1: (premise) the extracted id loop starts at 1 (0x0045f1cb MOV [EBP-0x14],0x1)");
        ck(done[0] == sentinel_fn() && (bldg_done_fn)tick2[0] == sentinel_fn(),
           "G2: id 0 is UNTOUCHED even though its cfg type (0x0c) is assigned -- slot 0 is outside "
           "the loop, not merely unmatched");
    }

    // ---- H: the two tables are genuinely different ------------------------------------------------
    //
    // A fill that wrote the done table twice (a copy-paste in fill_bldg_callback_tables, the
    // plausible bug) passes A through G for the done half and would leave the tick2 half looking
    // self-consistent if every check only compared a table against itself.
    {
        ck((bldg_done_fn)tick2[0x26] != done[0x26],
           "H1: at type 0x26 the two tables differ -- tick2 has an assignment there and done does "
           "not");
        ck(tick2_type_is_assigned(0x26) && !done_type_is_assigned(0x26),
           "H2: (premise of H1) 0x26 is assigned in the tick2 table (online_toggle, 0x0045fa57) and "
           "NOT in the done one");
    }

    // ---- I: THE SCAN IS A LOOP, NOT A LOOKUP -- every id sharing a type gets the callback ---------
    //
    // The failure mode unique to this registrar and with no counterpart in the state tables: an
    // implementation that resolved "the id for this type" instead of scanning would fill id 0x0c and
    // leave 0x2c/0x2d on the sentinel. Every check above would still pass, because every check above
    // looks at the id that happens to equal its type.
    {
        const bldg_done_fn port_done = expected_fn(PORT_DONE_KEY); // llm_strat_done_port
        ck(done[0x0c] == port_done && done[0x2c] == port_done && done[0x2d] == port_done,
           "I1: ALL THREE ids with cfg type 0x0c hold llm_strat_done_port -- the setter loops over "
           "every id (0x0045f1d2-0x0045f201), it does not resolve one");
        const bldg_done_fn port_tick2 = expected_fn(PORT_TICK2_KEY); // llm_strat_bldg_anim_state_port
        ck((bldg_done_fn)tick2[0x0c] == port_tick2 && (bldg_done_fn)tick2[0x2c] == port_tick2 &&
               (bldg_done_fn)tick2[0x2d] == port_tick2,
           "I2: ...and the same three in the tick2 table hold llm_strat_bldg_anim_state_port");
        // The default fill has to be a loop for the same reason. ids 0x2e..0x63 all carry type 0x01,
        // which IS assigned in done (done_production) and is NOT in tick2 -- so the tick2 side of
        // this is a default-fill-loop witness and the done side an assignment-loop one.
        int dflt_wrong = 0;
        for (int id = 0x2e; id < ID_LIMIT; ++id)
            if ((bldg_done_fn)tick2[id] != expected_fn(TICK2_DEFAULT))
                ++dflt_wrong;
        ck_eq((uint32_t)dflt_wrong, 0u,
              "I3: the 54 ids with cfg type 0x01 all hold the tick2 DEFAULT -- the default fill "
              "scans every id too, not just the first of each type");
    }

    // ---- J: four pairings pinned BY NAME against the registrar's own listing ----------------------
    //
    // Everything above compares the fill against the generated header. If the extraction itself were
    // wrong, all of it would agree and all of it would be wrong together. These four are the
    // independent anchor, each read off llm_strat_register_bldg_type_callbacks' disassembly.
    {
        ck_eq((uint32_t)(strcmp(name_of(done[0x0c]), "llm_strat_done_port") == 0), 1u,
              "J1: building type 0x0c gets llm_strat_done_port (MOV EDX,0x470198 / MOV EAX,0xc / "
              "CALL 0x0045f1ae at 0x0045f7af)");
#ifdef MH_LIBMH_BUILD
        // J2 and J3 ASK THE SAME QUESTION THROUGH THE OTHER DIRECTION in this arm, because
        // `name_of` cannot answer it here: it reports the FIRST binding whose body matches, and
        // both of these callbacks are inside an alias group (G1 and G0 above), so it names a
        // sibling -- llm_strat_done_relay for J2, llm_strat_done_default for J3. That is a property
        // of the reverse lookup, not of the fill. So the slot is compared against the body the
        // NAMED binding resolves to, which is exactly the pairing the case is pinning, and is what
        // the hosted form means when it spells the name. J1 and J4 keep the hosted form unchanged:
        // llm_strat_done_port and llm_strat_bldg_anim_state_online_toggle are in no alias group, so
        // name_of is unambiguous for them and the stronger "and no other binding shares this body"
        // reading is worth keeping where it is available.
        ck_eq((uint32_t)(done[0x15] == expected_fn("llm_strat_done_production")), 1u,
              "J2: type 0x15 -- the second race's production kind -- gets llm_strat_done_production "
              "(0x0045f8ea), the same callback as type 0x01");
        ck_eq((uint32_t)((bldg_done_fn)tick2[0x05] ==
                         expected_fn("llm_strat_bldg_anim_state_turret")),
              1u,
              "J3: type 0x05 gets llm_strat_bldg_anim_state_turret in the tick2 table (0x0045faa2) "
              "-- the callback that was FUN_00476605 when SIM1-BLDGCB opened");
#else
        ck_eq((uint32_t)(strcmp(name_of(done[0x15]), "llm_strat_done_production") == 0), 1u,
              "J2: type 0x15 -- the second race's production kind -- gets llm_strat_done_production "
              "(0x0045f8ea), the same callback as type 0x01");
        ck_eq((uint32_t)(strcmp(name_of((bldg_done_fn)tick2[0x05]),
                                "llm_strat_bldg_anim_state_turret") == 0),
              1u,
              "J3: type 0x05 gets llm_strat_bldg_anim_state_turret in the tick2 table (0x0045faa2) "
              "-- the callback that was FUN_00476605 when SIM1-BLDGCB opened");
#endif
        ck_eq((uint32_t)(strcmp(name_of((bldg_done_fn)tick2[0x12]),
                                "llm_strat_bldg_anim_state_online_toggle") == 0),
              1u,
              "J4: type 0x12 gets llm_strat_bldg_anim_state_online_toggle (0x0045fa48) -- one of "
              "the six types that share it");
    }

    // ---- K: the refusals. the fill writes NOTHING when it refuses --------------------------------
    //
    // The failure this guards is a region that is unrebased or short: a fill that got half way would
    // leave a LIVE dispatch table part ours and part stale .bss, strictly worse than not installing.
    // So each refusal is checked for its return value AND for the arrays being untouched.
    {
        std::vector<bldg_done_fn>  d2(ID_LIMIT, sentinel_fn());
        std::vector<bldg_tick2_fn> t2(ID_LIMIT, (bldg_tick2_fn)sentinel_fn());
        int                        fd = -1, ft = -1;

        sim_bldg_callback_tables t = tables_over(fx, d2, t2, ID_LIMIT - 1, ID_LIMIT, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K1: a done table one slot short is REFUSED");
        ck(d2[1] == sentinel_fn() && d2[ID_LIMIT - 2] == sentinel_fn(),
           "K2: ...and nothing was written before the refusal");
        ck(fd == 0 && ft == 0, "K3: ...and the reported fill counts are 0, not left stale");

        t = tables_over(fx, d2, t2, ID_LIMIT, ID_LIMIT - 1, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K4: a tick2 table one slot short is REFUSED -- the size check covers BOTH tables");
        ck(d2[1] == sentinel_fn() && (bldg_done_fn)t2[1] == sentinel_fn(),
           "K5: ...and NEITHER table was written (the done half is filled first, so a fill that "
           "wrote it before validating the tick2 size would fail here)");

        // The cfg table is an input, and a short one is the same class of failure: the original's
        // loop reads Building[1..99] unconditionally, so a 50-record cfg means reading past it.
        t = tables_over(fx, d2, t2, ID_LIMIT, ID_LIMIT, ID_LIMIT - 1);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K6: a cfg table shorter than the id loop is REFUSED -- it is scanned, so it is sized "
           "too");
        ck(d2[1] == sentinel_fn(), "K7: ...and nothing was written");

        std::vector<bldg_done_fn>  none_d;
        std::vector<bldg_tick2_fn> none_t;
        t = tables_over(fx, none_d, t2, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft),
           "K8: a null done table is REFUSED rather than dereferenced");
        t = tables_over(fx, d2, none_t, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft), "K9: a null tick2 table is REFUSED");
        t     = tables_over(fx, d2, t2, ID_LIMIT, ID_LIMIT, ID_LIMIT);
        t.cfg = nullptr;
        ck(!detail::fill_bldg_callback_tables(t, &fd, &ft), "K10: a null cfg table is REFUSED");
        ck(d2[1] == sentinel_fn() && (bldg_done_fn)t2[1] == sentinel_fn(),
           "K11: ...and still nothing written by any of the three");

        // Tables LARGER than the binary's are fine -- the registrar fills what the binary fills and
        // does not run off the end of its own table.
        std::vector<bldg_done_fn>  d3(ID_LIMIT + 8, sentinel_fn());
        std::vector<bldg_tick2_fn> t3(ID_LIMIT + 8, (bldg_tick2_fn)sentinel_fn());
        t = tables_over(fx, d3, t3, ID_LIMIT + 8, ID_LIMIT + 8, ID_LIMIT);
        ck(detail::fill_bldg_callback_tables(t, &fd, &ft), "K12: an over-sized table is accepted");
        ck(d3[ID_LIMIT] == sentinel_fn() && (bldg_done_fn)t3[ID_LIMIT] == sentinel_fn(),
           "K13: ...and the slots past the binary's 100 are NOT written -- the setter's own bound "
           "is `id < 0x64` (0x0045f1d2)");
    }

    // ---- L: is_our_bldg_type_callback is a membership test, not a range test ----------------------
    {
        ck(ours(reinterpret_cast<const void *>(done[0x0c])),
           "L1: a filled slot reads as ours");
        ck(!ours(reinterpret_cast<const void *>(sentinel_fn())),
           "L2: the sentinel does not");
        ck(!ours(nullptr), "L3: nullptr does not");
        ck(!ours(
               reinterpret_cast<const void *>(&run_register_bldg_type_callbacks_tests)),
           "L4: an unrelated DLL function does not -- membership against the 30, not an address "
           "range, so nearby DLL code cannot be mistaken for a callback of ours");
    }

    // ---- M: the cfg is READ, never written -------------------------------------------------------
    //
    // `sim_bldg_callback_tables::cfg` is a const pointer, so this cannot fail by a direct store; it
    // can by a const_cast, which is exactly the sort of thing a later edit adds "to normalise" a
    // type byte. Cheap to pin, and it states the direction of the dependency.
    {
        int changed = 0;
        for (int id = 1; id <= 0x27; ++id)
            if (fx.cfg_buildings[id].type != (uint8_t)id) ++changed;
        ck_eq((uint32_t)changed, 0u, "M1: the fill left the cfg type bytes untouched -- cfg is the "
                                     "scan's INPUT, never its output");
    }
}

} // namespace mh::sim::test
