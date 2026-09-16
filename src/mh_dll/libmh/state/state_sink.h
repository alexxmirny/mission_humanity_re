//
// state/state_sink.h -- ONE canonical stream per state region, two modes (RI-STATE / ST6).
//
// THE PROBLEM. Until now every consumer of a state region dereferenced a base and read bytes:
// the determinism hash, the seed dump/inject blob, the poke that proves the gate can go red. That
// works exactly as long as a region IS a flat array at a fixed address. The moment a subsystem takes
// ownership of its state (Law 2), raw-byte reads stop being merely ugly and become WRONG -- a
// container's bytes include heap POINTER VALUES, which differ between peers by construction, plus
// padding and capacity slack. Hashing those is the `peer_horizon` failure shape (a gate red for a
// benign reason) arriving through the back door.
//
// THE ANSWER, settled with the user 2026-07-31: an owner exposes ONE emit(sink) describing its
// canonical state, and the consumers are sinks over it.
//
//   PERSIST  -> the save file. Everything the format carries.
//   VERDICT  -> the determinism hash. The same traversal, minus the fields the owner declares
//               peer-local or frame-rate-dependent.
//
// The rejected alternative was a separate hash_state() alongside save_state(). Two encodings of one
// decision with nothing checking they agree is already a recorded trap, it is what ST2M
// had just finished undoing for this very manifest, and here a disagreement does not merely confuse
// a report -- it means the determinism oracle silently stops watching part of the state.
//
// WHAT THIS REPLACES. The three bespoke masked hashers (hash_units' control-group byte,
// hash_tile_objects' fog bits, hash_rng_state's fx slot) were byte-offset masks over a raw array.
// As field-level `local()` / `masked()` calls they survive a layout change; as byte offsets they
// could not, and one of them was until today built on a HAND-TYPED offset (see the mirror-binding
// commit) with the verdict resting on it.
//
// HONEST LIMIT, PHASE 1. For a region still owned by the game, PERSIST and VERDICT are NOT one
// traversal differing by a mask, and this file does not pretend otherwise -- see region_view.h.
// They collapse into one traversal AT ownership, which is the point, not a property we already have.
//
#pragma once
#include <cstdint>

namespace mh::state {

enum class sink_mode : uint8_t {
    PERSIST, // the save file: everything the format carries
    VERDICT, // the determinism hash: minus what legitimately differs between peers
};

// ---- the emission rules, ONCE, as templates -----------------------------------------------------
//
// These hold the actual semantics of `bytes` / `local` / `masked`. `state_sink`'s members below
// forward to them, and the hot tactical path calls them directly on a CONCRETE sink type -- so the
// generic path keeps its virtual dispatch and the hash path gets a direct, inlinable call, from one
// body. Duplicating the rules instead would let the verdict hash and the seed blob drift apart on
// what "local" means, which is the one disagreement this file exists to prevent.
//
// `S` needs only `.mode` and `.raw(const void*, uint32_t)`.
template <class S>
inline void sink_bytes(S &s, const void *p, uint32_t n) {
    s.raw(p, n);
}

// NAMESPACE SCOPE, not a function-local static inside the template below. A function-local static
// in a template gets a thread-safe-init guard per instantiation, and MSVC 19.44 + /fsanitize=address
// hits an internal compiler error (C1001) on an UNRELATED translation unit when that combination is
// present -- the diagnosis cost a bisect because the file named in the error does not include this
// header at all. It is a constant; it never needed to be a static.
inline constexpr uint8_t SINK_ZEROS[8] = {0, 0, 0, 0, 0, 0, 0, 0};

template <class S>
inline void sink_local(S &s, const void *p, uint32_t n) {
    if (s.mode == sink_mode::PERSIST) {
        s.raw(p, n);
        return;
    }
    const uint8_t(&zeros)[8] = SINK_ZEROS;
    while (n) {
        const uint32_t k = n > sizeof(zeros) ? (uint32_t)sizeof(zeros) : n;
        s.raw(zeros, k);
        n -= k;
    }
}

template <class S>
inline void sink_masked(S &s, uint16_t v, uint16_t keep) {
    const uint16_t out   = (s.mode == sink_mode::PERSIST) ? v : (uint16_t)(v & keep);
    const uint8_t  le[2] = {(uint8_t)(out & 0xff), (uint8_t)(out >> 8)};
    s.raw(le, 2);
}

// A destination for a region's canonical content. Consumers implement `raw`; everything else is
// expressed in terms of it, so a new sink cannot forget to handle a mode.
struct state_sink {
    explicit state_sink(sink_mode m) : mode(m) {}
    virtual ~state_sink() = default;

    sink_mode mode;

    // The one primitive. Everything below decides WHAT to hand it.
    virtual void raw(const void *p, uint32_t n) = 0;

    // A CONCRETE forwarder, and it exists for a compiler bug rather than for taste. Forwarding
    // straight to the templates -- sink_local(*this, ...) -- instantiates them on `state_sink`,
    // which is ABSTRACT, and MSVC 19.44 with /fsanitize=address + LTCG then dies with an internal
    // compiler error (C1001) while compiling an UNRELATED translation unit that does not include
    // this header at all. Bisected 2026-08-25.
    //
    // Wrapping the reference in a concrete type keeps ONE copy of the emission rules -- which is
    // the whole point of the templates, and what a duplicated "fast path" would have thrown away --
    // while never instantiating them on an abstract class.
    struct virt_ref {
        state_sink &s;
        sink_mode   mode;
        void        raw(const void *p, uint32_t n) { s.raw(p, n); }
    };
    virt_ref as_ref() { return virt_ref{*this, mode}; }

    // The three below FORWARD to the free templates above -- they are not a second copy of the
    // rules. Calling them on a `state_sink&` costs one virtual dispatch per emitted field; the hot
    // tactical path calls the templates directly on a concrete sink and pays none.

    // Canonical content: part of the state in both modes.
    void bytes(const void *p, uint32_t n) {
        virt_ref r = as_ref(); // named: the templates take an lvalue reference
        sink_bytes(r, p, n);
    }

    // Content that PERSISTS but is not part of the determinism verdict -- a peer-local field, a
    // frame-rate-dependent counter, a UI echo. Substituted by n zero bytes in VERDICT rather than
    // skipped, so the stream stays positionally aligned and a field cannot silently disappear.
    void local(const void *p, uint32_t n) {
        virt_ref r = as_ref();
        sink_local(r, p, n);
    }

    // A field whose LOW BITS are state and whose high bits are not (the fog bits packed into
    // tile_objects' flags word). PERSIST keeps the whole value; VERDICT keeps `v & keep`.
    void masked(uint16_t v, uint16_t keep) {
        virt_ref r = as_ref();
        sink_masked(r, v, keep);
    }
};

// ---- the two sinks phase 1 needs ------------------------------------------------------------

// FNV-1a-64 over whatever the owner emits. `h` is seeded by the caller so a slice can be hashed
// standalone (the per-region `R` columns) or folded into a running combined hash.
// MH_HASH_WIDE: process the stream 8 bytes at a time instead of 1.
//
// The narrow form is FNV-1a: xor a byte, multiply by the 64-bit prime. On a 32-BIT build every one
// of those multiplies is a software __allmul, and the profile put ~55% of a tactical frame in that
// loop (the tactical-probe work 9r). Eight bytes per multiply is ~8x fewer of them.
//
// SPLIT-INVARIANCE IS THE HARD PART, and it is why `carry` exists. `raw()` is called with lengths
// 1, 2, 7, 0x5f4 -- whatever the emitter happens to hand it -- and the byte-serial form gives the
// same hash however a logical stream is chopped into those calls. A block hash that consumed only
// whole blocks per call would not: the same bytes split differently would hash differently, making
// the verdict a function of the EMITTER'S SHAPE rather than of the state. That is not theoretical
// -- packing tile_objects' six per-element calls into one (same bytes, same order) would silently
// change every tactical hash under a naive block hash. So partial blocks are carried across calls
// and the stream is hashed as one sequence, exactly as before.
#ifndef MH_HASH_WIDE
#define MH_HASH_WIDE 1
#endif

struct hash_sink final : state_sink {
    explicit hash_sink(sink_mode m = sink_mode::VERDICT, uint64_t seed = 1469598103934665603ULL)
        : state_sink(m), h(seed) {}
    uint64_t h;
#if MH_HASH_WIDE
    uint64_t block  = 0; // partial block carried between raw() calls
    uint32_t filled = 0; // bytes valid in `block`, 0..7

    void raw(const void *p, uint32_t n) override {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        uint32_t       i = 0;
        // Finish a block left over from the previous call.
        while (filled && i < n) {
            block |= (uint64_t)b[i++] << (filled * 8);
            if (++filled == 8) {
                mix(block);
                block  = 0;
                filled = 0;
            }
        }
        // Whole blocks. Byte-wise assembly rather than a cast: the source is an arbitrary address
        // inside a game struct, so it is not guaranteed 8-aligned, and a misaligned 64-bit read is
        // a portability bug waiting for a compiler that cares.
        for (; i + 8 <= n; i += 8) {
            uint64_t w = 0;
            for (uint32_t k = 0; k < 8; ++k) w |= (uint64_t)b[i + k] << (k * 8);
            mix(w);
        }
        // Keep the tail for the next call.
        for (; i < n; ++i) block |= (uint64_t)b[i] << (filled++ * 8);
    }

    // The stream may END mid-block, so the tail has to be folded before `h` is read. Callers that
    // read `h` directly must call this first; `finish()` is idempotent.
    uint64_t finish() {
        if (filled) {
            mix(block | ((uint64_t)filled << 56)); // length-tag the tail so "ab" != "ab\0"
            block  = 0;
            filled = 0;
        }
        return h;
    }

private:
    void mix(uint64_t w) {
        h ^= w;
        h *= 1099511628211ULL;
    }
#else
    void raw(const void *p, uint32_t n) override {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (uint32_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
    }
    uint64_t finish() { return h; }
#endif
};

// The LOAD half. Deliberately PERSIST-only and deliberately smaller than state_sink: you never
// "load a verdict", so there is no mode to get wrong here. This is the shape ST4's
// mh::orders::load_state(src) takes, arriving early because seed_inject is already a deserialize --
// it just did not know it, having been written as a ReadFile straight over a region's bytes.
struct state_source {
    virtual ~state_source()               = default;
    virtual void raw(void *p, uint32_t n) = 0;
    void         bytes(void *p, uint32_t n) { raw(p, n); }
};

struct fn_source final : state_source {
    fn_source(void (*f)(void *, void *, uint32_t), void *c) : fn(f), ctx(c) {}
    void (*fn)(void *, void *, uint32_t);
    void *ctx;
    void  raw(void *p, uint32_t n) override { fn(ctx, p, n); }
};

// A sink over a caller-supplied byte consumer -- the seed blob writes through this. Kept as a
// function pointer rather than a template so the header stays free of <functional> and the DLL
// keeps its no-CRT-surprises profile.
struct fn_sink final : state_sink {
    fn_sink(sink_mode m, void (*f)(void *, const void *, uint32_t), void *c)
        : state_sink(m), fn(f), ctx(c) {}
    void (*fn)(void *, const void *, uint32_t);
    void *ctx;
    void  raw(const void *p, uint32_t n) override { fn(ctx, p, n); }
};

} // namespace mh::state
