//
// state/host_bind.cpp -- the state ABI: the HOST tells libmh where the state is (SB-BIND T1).
//
// docs/state-boundary.md D4 is the decision this implements. The registry has carried a live base
// since ST2 and every dereferencing consumer already resolves through it; what did not exist was a
// way for someone OUTSIDE libmh to set it. That is the whole of this file.
//
// THREE THINGS WORTH KNOWING BEFORE EDITING:
//
// 1. THE DEFAULT TABLE IS DERIVED, NOT GENERATED SEPARATELY. `libmh_default_binds` reads REGIONS[]
//    -- the same array the registry header already generates from tools/data/state_regions.json.
//    Emitting a second generated table of 823 bases would be exactly the "silently-diverging
//    derivation" mh_regions.gen.h's own header comment exists to abolish, so the generated thing
//    is REGIONS[] and this is a view over it.
//
// 2. IT BINDS `reach`, NOT `size`, and that is a decision with a measurement behind it (D6.5).
//    `covering()` resolves a save block against reach, so the 10 blocks that overrun their symbol's
//    tail DO resolve to a region -- and translate() would then bounds-check the window against the
//    shorter `size` and hand back nullptr. Binding reach is what keeps that from firing. The two
//    numbers differ for 10 of 827 regions; none of them feeds a derived count, so nothing else
//    observes the difference -- EXCEPT bind_relocated() below, which refuses to move exactly those
//    ten, because a window that overruns its symbol overruns onto its NEIGHBOURS.
//
// 3. THE HOSTED BIND IS A NO-OP BY CONSTRUCTION, not by measurement. mh::state::bind() derives
//    `moved` from whether the base actually differs from the stock one, so binding the default
//    table writes the same bases back and leaves every `moved` flag false -- translate() stays on
//    its identity path, ai::island_move() can still claim its 48 regions later, and statetest's
//    "nothing has moved" premise still holds. The determinism/UI arm confirms that; it does not
//    establish it.
//
#include "state/host_bind.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "addr/mh_regions.gen.h"

namespace mh::state {

size_t default_binds(libmh_region_bind *out, size_t cap) {
#ifdef MH_LIBMH_BUILD
    // ---- THE STANDALONE REFUSAL (LIB-REF-SPLIT) -------------------------------------------------
    //
    // "The stock, in-binary answer" is the one thing a standalone artifact cannot give. There is no
    // binary, nothing is mapped at any of those addresses, and the stock base column is not even
    // carried standalone (MH_STOCK_BASE, addr/mh_regions.gen.h). Returning the table with base_of()
    // zeroed would hand a host 845 entries all pointing at address 0 and it would look like an
    // answer, so this returns the honest one: NOTHING, and no entries written.
    //
    // ZERO IS A DISTINCT CODE HERE, not an ambiguous one, and that is why the refusal composes with
    // the existing contract instead of colliding with it. `libmh_default_binds` returns the FULL
    // count and writes min(cap, count) entries, so the documented way to ask "how big a buffer do I
    // need" is to call it with cap 0 -- which hosted returns RID_COUNT and standalone returns 0.
    // A standalone host therefore learns "there is no default table" from the same call it would
    // have used to size one, and it cannot mistake that for "no regions": libmh_region_count() is
    // unchanged and still reports RID_COUNT. The host must then answer for every region itself
    // through libmh_bind_regions, and libmh_bound_count() stays below libmh_region_count() until it
    // does -- which is the existing "did every region get an answer?" check, unmodified.
    (void)out;
    (void)cap;
    return 0;
#else
    const size_t n = static_cast<size_t>(RID_COUNT);
    for (size_t i = 0; i < n && i < cap; ++i) {
        const region_id r = static_cast<region_id>(i);
        out[i].region_id  = static_cast<uint32_t>(i);
        out[i].base       = reinterpret_cast<void *>(static_cast<uintptr_t>(base_of(r)));
        out[i].size       = reach_of(r);
        // 0 = "the host declares no capacity". The registry carries no element stride, so the
        // stock table cannot honestly claim one; SB-BIND T2 is where a cap-raised host does.
        out[i].count = 0;
    }
    return n;
#endif // MH_LIBMH_BUILD
}

int bind_all(const libmh_region_bind *binds, size_t n) {
    if (binds == nullptr) return n == 0 ? 0 : -1;
    // VALIDATE THE WHOLE TABLE FIRST. A half-applied bind is worse than a rejected one: the
    // registry would be answering with a mix of host and stock addresses and nothing downstream
    // could tell, so the error paths below must not have written anything yet.
    for (size_t i = 0; i < n; ++i) {
        if (binds[i].region_id >= static_cast<uint32_t>(RID_COUNT)) return -2;
        if (binds[i].base == nullptr && binds[i].size != 0) return -3;
    }
    for (size_t i = 0; i < n; ++i) {
        const region_id r = static_cast<region_id>(binds[i].region_id);
        bind(r, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(binds[i].base)), binds[i].size,
             binds[i].count);
    }
    return static_cast<int>(n);
}

namespace {
int g_moved_before_bind = -1; // regions already relocated when the host answered; -1 = never bound
int g_moved_after_bind  = -1;
} // namespace

int moved_before_bind() {
    return g_moved_before_bind;
}

int moved_after_bind() {
    return g_moved_after_bind;
}

int bind_stock() {
    // Goes THROUGH the public entry point rather than calling bind() per region directly. That is
    // the point of having the hosted config bind at all: mh.exe drives the same path a standalone
    // host will, so it cannot rot while nothing standalone exists yet.
    //
    // THE TWO COUNTERS ARE AN ORDERING CHECK, and they exist because the first live run made the
    // coupling visible rather than because it was anticipated. `moved_before` MUST be zero: a region
    // already relocated when the host answers is about to be silently UN-relocated by this very
    // call, because the stock table says "it is at its .bss address" and bind believes the host.
    // That is not hypothetical -- under the ship default [promote] ai=1, ai::island_move() relocates
    // 48 regions and poisons the abandoned .bss with 0xCD, so binding after it would leave
    // mh::ai::state() reading poison. It happens not to occur because the bind runs before the arm
    // path -- it is the second statement of MH_Core_Arm_Early, which DllMain calls ahead of both
    // MH_Harness_Init and MH_Seam_Init (it was the head of MH_Harness_Init until fork F3B; same
    // position in the boot, a name that says whose work it is). But "happens not to" is the kind of
    // boot-order property G104 says to measure rather than reason about.
    //
    // `moved_after` is the no-op assertion itself. Both are sampled HERE rather than at report time
    // because by then the AI island legitimately HAS moved -- which is what made the first version
    // of the report line read its own success as a failure.
    int before = 0;
    for (int i = 0; i < RID_COUNT; ++i)
        if (is_rebased(static_cast<region_id>(i))) ++before;

    // The table is a function-local static, not a stack array: 823 entries is ~13 KB, and this runs
    // once at init on a thread nothing else has yet.
    static libmh_region_bind table[RID_COUNT];
    const size_t             n  = default_binds(table, RID_COUNT);
    const int                rc = bind_all(table, n);

    int after = 0;
    for (int i = 0; i < RID_COUNT; ++i)
        if (is_rebased(static_cast<region_id>(i))) ++after;

    g_moved_before_bind = before;
    g_moved_after_bind  = after;
    return rc;
}

// ---- the RELOCATING bind (SB-HOSTFREE) -------------------------------------------------------
//
// THE ONE THING THAT MAKES THIS DIFFERENT FROM bind_stock(): it can be WRONG, and loudly. Binding
// the stock table is a no-op by construction, so it cannot break a run and cannot prove much
// either. This moves real bytes, and every consumer that failed to follow the registry -- a cached
// pointer, a `mh::addr::` literal, a stale binder -- reads the abandoned range instead. That is
// exactly the population SB-BIND spent five tranches draining, and this is the arm that says
// whether the draining worked.
//
// WHICH REGIONS: `REGIONS[].relocatable`, generated from the accessor census. NOT a judgement made
// here, and deliberately not "all of them" -- an original accessor is a baked disp32 that cannot
// follow a bind, so 364 of 827 must stay put or the original half of the game ends up reading the
// .bss we just abandoned (docs/state-boundary.md D7.2).
//
// POISON, on ai::island_move's precedent (ai_state.cpp): after moving a region, fill its stock
// range with 0xCD. Without it a consumer that did not follow reads a plausible stale COPY of the
// right bytes and quietly agrees with itself, which is the vacuous-green shape this whole item
// exists to refuse; with it, it reads garbage and the determinism hash says so at the first step.

namespace {
bool     g_host_relocated[RID_COUNT];
int      g_relocated_count = -1; // -1 = bind_relocated() never ran; this run is on the stock bind
uint32_t align16(uint32_t v) {
    return (v + 15u) & ~15u;
}
} // namespace

bool host_relocated(region_id r) {
    return g_host_relocated[r];
}

int bind_relocated(void *arena, size_t bytes, const relocation_opts &opts,
                   relocation_report *out) {
    relocation_report rep    = {};
    const auto        finish = [&](int n) {
        if (out) *out = rep;
        return n;
    };
    if (arena == nullptr) {
        rep.refused = "no arena";
        return finish(0);
    }

    // MEASURE FIRST, BIND SECOND -- bind_all's rule, for bind_all's reason. A half-applied
    // relocation leaves the registry answering with a mix of arena and stock addresses and nothing
    // downstream can tell; refusing whole is recoverable, refusing halfway is not.
    uint32_t need = 0;
    for (int i = 0; i < RID_COUNT; ++i) {
        const region_id r = static_cast<region_id>(i);
        if (!movable_under(r, opts.armed_walkers) || is_rebased(r)) continue;
        if (opts.pin_stock && std::strcmp(REGIONS[i].name, opts.pin_stock) == 0) continue;
        need = align16(need) + REGIONS[i].reach;
    }
    if (need > bytes) {
        rep.refused = "arena too small";
        rep.bytes   = need;
        return finish(0);
    }

    // A pin that names nothing is a MUTATION THAT DID NOT HAPPEN, and it would present as a clean
    // relocated run -- i.e. as the very result the mutation exists to disprove. Refuse it.
    //
    // AGAINST `is_movable`, NOT `relocatable`, and the difference is the whole point of checking at
    // all: a region the census clears but the layout refuses (under an overrunning window, D7.3)
    // would never reach the pin branch in the loop below -- the refusal comes first -- so the run
    // would come out clean and be read as "the relocation survived the mutation". The stricter
    // predicate is the one that makes this guard mean anything.
    if (opts.pin_stock) {
        bool found = false;
        for (int i = 0; i < RID_COUNT && !found; ++i)
            if (movable_under(static_cast<region_id>(i), opts.armed_walkers) &&
                std::strcmp(REGIONS[i].name, opts.pin_stock) == 0)
                found = true;
        if (!found) {
            rep.refused = "pin_stock names no region THIS CONFIGURATION would move";
            return finish(0);
        }
        rep.pinned      = 1;
        rep.pinned_name = opts.pin_stock;
    }

    // Same guard, same reason: a corruption target that names nothing produces a CLEAN relocated
    // run, which is precisely the result the mutation exists to disprove.
    if (opts.corrupt_arena) {
        bool found = false;
        for (int i = 0; i < RID_COUNT && !found; ++i)
            if (movable_under(static_cast<region_id>(i), opts.armed_walkers) &&
                std::strcmp(REGIONS[i].name, opts.corrupt_arena) == 0)
                found = true;
        if (!found) {
            rep.refused = "corrupt_arena names no region THIS CONFIGURATION would move";
            return finish(0);
        }
        rep.corrupted      = 1;
        rep.corrupted_name = opts.corrupt_arena;
    }

    // A relocation REDEFINES where things are, so the previous answer is not carried forward. This
    // matters only to a fixture that relocates twice (the live host does it once, at init), but a
    // stale `true` here would tell ai::island_move a region is in an arena that no longer exists.
    std::memset(g_host_relocated, 0, sizeof(g_host_relocated));

    uint8_t *const base = static_cast<uint8_t *>(arena);
    uint32_t       off  = 0;
    for (int i = 0; i < RID_COUNT; ++i) {
        const region_id r = static_cast<region_id>(i);
        // A HOST ANSWERS FOR EVERY REGION, including the ones it does not move. Binding the stock
        // base for those is a no-op by construction (bind() derives `moved` from whether the base
        // differs) and it is what keeps `bound_count() == RID_COUNT` true -- i.e. what keeps "did
        // every region get an answer?" answerable. Leaving them unbound would make a relocating
        // host look like a host that forgot 364 regions, which is a different and much worse thing.
        const auto bind_stock_here = [&](region_id rr) {
            bind(rr, REGIONS[rr].base, REGIONS[rr].reach);
        };
        if (!REGIONS[i].relocatable) {
            ++rep.blocked;
            bind_stock_here(r);
            continue;
        }
        if (REGIONS[i].reach == 0) {
            ++rep.zero_size;
            bind_stock_here(r);
            continue;
        }
        // A REGION THAT OVERRUNS ITS OWN SYMBOL MAY NOT MOVE, and this refusal is here now because
        // it costs nothing now. `reach > size` means some manifest claims bytes PAST this symbol --
        // _G_LLM_GAME_SESSION_MODE's save block is 1623 bytes over a 4-byte symbol, spanning 49
        // other live regions. Relocating such a region copies a window over its NEIGHBOURS into the
        // arena and then poisons those neighbours' live storage, which corrupts regions this bind
        // never claimed and would present as an unrelated desync somewhere else entirely.
        //
        // ALL TEN SUCH REGIONS ARE ALREADY BLOCKED by an original accessor, so today this branch is
        // unreachable -- which is exactly why it is written down. `relocatable` will grow as
        // promotion retires original accessors, and the day one of these ten becomes eligible is a
        // day nobody will be looking for a 1623-byte poison. Untangling the overruns is
        // SB-HOSTFREE's save-block decomposition tranche (docs/state-boundary.md D6.6); until that
        // lands, they stay put.
        if (REGIONS[i].reach > REGIONS[i].size) {
            ++rep.overrunning;
            bind_stock_here(r);
            continue;
        }
        // THE SIBLING HAZARD, AND WHY IT IS NO LONGER A REFUSAL. A block that runs past its symbol
        // lands on whatever is next, and that neighbour is a region in its own right -- so moving
        // the neighbour used to mean the block kept reading the stock address after the bytes left.
        // Measured: 21 relocatable regions sit under _G_LLM_GAME_SESSION_MODE's 1623-byte save
        // block, and the failure DOES NOT DIVERGE, because the damage is in bytes only the save
        // format reads (dead-ends G135).
        //
        // Those blocks are DECOMPOSED now (save_table.gen.h SLICED_BLOCKS; save_driver gathers and
        // scatters them run by run, each run through its own region's live base), and
        // tools/check_save_block_slices.py refuses a new block that spans regions without one. So
        // these regions move like any other, and the count is kept only to say how many are relying
        // on that decomposition -- a number worth seeing in the report, not a refusal.
        if (under_overrunning_window(r)) ++rep.under_overrun;

        // AN ORIGINAL BLOCK-TABLE WALKER STILL NAMES THESE BYTES, and this configuration has not
        // promoted it (mh_regions.gen.h SAVE_WALKER_MASK). Its destination for this block is an
        // immediate in its own instruction stream, so it would read or write the stock address
        // after the bytes left -- and, being confined to a save or a load, only on the step that
        // saves or loads. That is what dead-ends G139 measured: the strategic and tactical soaks
        // relocate `order_queue` and match for 3000 steps, and the same run through a LOADGAME
        // diverges at exactly the load step, because that is the only step at which the original
        // walker runs.
        //
        // LEFT AT ITS STOCK BASE, not merely skipped: the original and every registry consumer then
        // agree about where the bytes are, which is correct rather than lucky. Promoting the walker
        // (`[promote] save/load/container/container_load`) hands the entry point to our body, whose
        // block resolution goes through translate(), and the region becomes movable in the same run.
        if (!movable_under(r, opts.armed_walkers)) {
            ++rep.walker_held;
            bind_stock_here(r);
            continue;
        }

        if (opts.pin_stock && std::strcmp(REGIONS[i].name, opts.pin_stock) == 0) {
            if (opts.poison)
                std::memset(reinterpret_cast<void *>(static_cast<uintptr_t>(REGIONS[i].base)), 0xCD,
                            REGIONS[i].reach);
            bind_stock_here(r);
            continue;
        }

        off                  = align16(off);
        uint8_t *const dst   = base + off;
        const uint32_t sz    = REGIONS[i].reach;
        void *const    stock = reinterpret_cast<void *>(static_cast<uintptr_t>(REGIONS[i].base));
        if (opts.copy) std::memcpy(dst, stock, sz);
        // THE MUTATION: the arena copy is filled with 0xCD after the copy and before the bind, so
        // every consumer that correctly follows the registry gets garbage. If the determinism hash
        // is reading the relocated bytes at all, it says so at the first step that touches them.
        if (opts.corrupt_arena && std::strcmp(REGIONS[i].name, opts.corrupt_arena) == 0)
            std::memset(dst, 0xCD, sz);
        bind(r, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dst)), sz);
        if (opts.poison) std::memset(stock, 0xCD, sz);
        g_host_relocated[i] = true;
        off += sz;
        ++rep.relocated;
    }
    rep.bytes = off;

    // THE NON-VACUITY COUNT. A relocation that moved nothing the determinism hash reads is green
    // over bytes nobody watches -- so the run reports how many slices actually sit on moved
    // regions, and the acceptance asserts on it rather than on `relocated` alone.
    for (int i = 0; i < HASH_REGION_COUNT; ++i)
        if (is_rebased(HASH_REGIONS[i].rid)) ++rep.hash_slices;
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i)
        if (is_rebased(TACT_HASH_REGIONS[i].rid)) ++rep.tact_slices;

    g_relocated_count = rep.relocated;
    return finish(rep.relocated);
}

int relocated_count() {
    return g_relocated_count;
}

} // namespace mh::state

extern "C" size_t libmh_region_count(void) {
    return static_cast<size_t>(mh::state::RID_COUNT);
}

extern "C" size_t libmh_default_binds(libmh_region_bind *out, size_t cap) {
    if (out == nullptr) return static_cast<size_t>(mh::state::RID_COUNT);
    return mh::state::default_binds(out, cap);
}

extern "C" int libmh_bind_regions(const libmh_region_bind *binds, size_t n) {
    return mh::state::bind_all(binds, n);
}

extern "C" int libmh_bound_count(void) {
    return mh::state::bound_count();
}
