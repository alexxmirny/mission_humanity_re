//
// state/region_owner.h -- a module CLAIMS a state region and serves it (RI-STATE / ST4, the owner
// half inherited from ST6).
//
// ST6 phase 1 made every whole-region consumer ask a SLICE to emit itself. This is the other end:
// a module declares "this region is mine", and from then on the slice's bytes come from the module
// rather than from an address. Two consumers route through it today -- the determinism hash and the
// save driver's block steps -- which is the point: one owner, one canonical stream, two readers.
//
// WHAT CLAIMING DOES *NOT* MEAN, and the distinction matters because the opposite is easy to assume:
// claiming is not a MOVE. While Law 1 freezes a layout (llm_strat_order_queue_dispatch reads the
// order queue 132 times), the owner's own pointer IS the frozen .bss address, so the bytes are
// identical and the whole change is provably behaviour-free. Claiming moves the QUESTION -- "what is
// this region's content?" is now answered by the module instead of by an address. Only after that is
// the address free to change, which is ST2's synthetic rebase and the island moves after it.
//
// A CONSEQUENCE WORTH STATING: "the hash follows the module and a poke at the stale address does not
// change it" cannot be demonstrated for orders while Law 1 holds, because the module's address IS
// the address a poke would hit. That test belongs with the first region that actually moves; it is
// not evidence this layer can produce today, and claiming otherwise would be a gate that cannot fail.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "state/state_sink.h"

namespace mh::state {

// Per-region, not per-module: the save driver walks BLOCKS and the hash walks SLICES, and the one
// key both share is the registry region id.
//
// NAMED `region_provider`, not `region_owner`, because mh_regions.gen.h already spends that name on
// the WRITE-OWNERSHIP enum (OWN_ISLAND / OWN_SHARED, as measured by the state matrix). Two different
// senses of "owner": that one says who WRITES a region and therefore whether Law 2 lets it move,
// this one says who SERVES its bytes. Worth keeping distinct rather than collapsing.
struct region_provider {
    void (*emit)(region_id, state_sink &);
    void (*load)(region_id, state_source &);
    const char *who; // for diagnostics; never parsed
};

// Fixed table on purpose. There will never be many owners, and a fixed table keeps this header-only
// and allocation-free inside the game's address space, where a heap grab at the wrong moment is its
// own class of bug. Raised 16 -> 80 on 2026-09-01: AI1-P's island move claims its 48 regions with
// one shared provider (one entry per REGION, since that is the key both consumers ask by), which
// the old cap would have silently truncated at 16 -- the exact silently-full failure the comment
// at the claim() overflow return warns about.
inline constexpr int MAX_OWNED = 80;

struct owner_slot {
    uint16_t               rid;
    const region_provider *owner;
};

// ONE TABLE PER PROCESS, NOT PER IMAGE -- the same fork-F4D rule as mh::state::live(), and for the
// same measured reason. `claim()` is called from roster TUs (ai_state.cpp's island move, 48 regions;
// order_queue.cpp, 2) and `owner_of` / `owner_serves` are read from mh.dll's side (desync_watch.cpp,
// harness.cpp). Two images, two tables, and the hash silently takes the raw-bytes path for every
// region a module has claimed -- green everywhere, wrong. The image that compiles the roster owns
// the storage; any other image declares these and reaches them across the boundary. See the note in
// addr/mh_regions.gen.h for why MH_SPINE_IN_IMAGE needs no gate of its own.
#ifdef MH_SPINE_IN_IMAGE
inline owner_slot *owner_table() {
    static owner_slot t[MAX_OWNED]{};
    return t;
}
inline int &owner_count() {
    static int n = 0;
    return n;
}
#else
owner_slot *owner_table(); // seams/libmh_bind.cpp
int        &owner_count(); // seams/libmh_bind.cpp
#endif

// Idempotent: re-claiming a region replaces the owner rather than appending a second entry, so a
// double-registration (two static initializers, a re-init) cannot make `owner_of` order-dependent.
inline void claim(region_id rid, const region_provider *o) {
    owner_slot *t = owner_table();
    for (int i = 0; i < owner_count(); ++i) {
        if (t[i].rid == (uint16_t)rid) {
            t[i].owner = o;
            return;
        }
    }
    if (owner_count() >= MAX_OWNED) return; // silently full is wrong; see the assert at the call site
    t[owner_count()].rid   = (uint16_t)rid;
    t[owner_count()].owner = o;
    ++owner_count();
}

inline const region_provider *owner_of(region_id rid) {
    const owner_slot *t = owner_table();
    for (int i = 0; i < owner_count(); ++i)
        if (t[i].rid == (uint16_t)rid) return t[i].owner;
    return nullptr;
}

// The owner is only asked when the consumer's window is the WHOLE region. A sub-window (a hash slice
// that is one player's interior slot, a save block displaced by a field offset) has no meaning in
// terms of a module's canonical stream, so it falls back to raw bytes rather than being served a
// stream that does not line up. This is the guard that keeps the routing honest as more regions are
// claimed -- without it, the first owned region that some consumer reads a sub-window of would get
// silently mis-served.
inline bool owner_serves(region_id rid, uint32_t offset, uint32_t len) {
    return owner_of(rid) != nullptr && offset == 0 && len == REGIONS[rid].size;
}

} // namespace mh::state
