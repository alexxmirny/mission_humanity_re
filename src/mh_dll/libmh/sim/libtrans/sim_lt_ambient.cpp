//
// sim/libtrans/sim_lt_ambient.cpp -- see sim_lt_ambient.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_snd_ambient_reseed_planet_event_times_0045e277.asm), not from the
// Ghidra .c draft (translator-brief rule 1).
//
#include "sim/libtrans/sim_lt_ambient.h"

#include <cmath>   // std::trunc -- the utils_math_trunc substitution, see the header banner
#include <cstring> // std::memcpy -- byte-offset double read/write over the untyped ambient table

#include "sim/libtrans/sim_lt_rng_draws.h" // mh::sim::detail::rand_below_fx -- LT1A batch A, already ours

namespace mh::sim::detail {

namespace {

// The per-planet-record / per-event-record geometry, derived from the instruction operands -- see
// the header banner for the full derivation of every constant here.
constexpr std::size_t AMBIENT_PLANET_STRIDE = 0x2d4; // 724 B/planet
constexpr std::size_t AMBIENT_EVENT_STRIDE  = 0x24;  // 36 B/event
constexpr int32_t     AMBIENT_EVENT_COUNT   = 20;    // events/planet (`CMP ...,0x14` loop bound)

constexpr std::size_t AMBIENT_EVENT_REPLAY_DELAY_OFFSET = 0xc;  // double, @planet+event+0xc
constexpr std::size_t AMBIENT_EVENT_RETRY_DELAY_OFFSET  = 0x14; // double, @planet+event+0x14
constexpr std::size_t AMBIENT_EVENT_NEXT_TIME_OFFSET    = 0x20; // double, @planet+event+0x20

// Byte offset of one event's record within the whole-region base -- the shared indexing arithmetic
// factored out of the three field accesses below so it is written (and can be reviewed) exactly once.
std::size_t ambient_event_byte_offset(int32_t planet_index, int32_t event_index) {
    return static_cast<std::size_t>(planet_index) * AMBIENT_PLANET_STRIDE +
           static_cast<std::size_t>(event_index) * AMBIENT_EVENT_STRIDE;
}

// The table has no Ghidra struct (see the header banner's DECLARED NEED), so reads/writes go through
// the whole-region byte pointer `sim_store::snd_ambient_by_planet_base()` returns, at a named field
// offset -- std::memcpy rather than a `reinterpret_cast<double *>` dereference because the field
// offsets are not all 8-byte-aligned relative to a `double`'s natural alignment (event stride 0x24 is
// not a multiple of 8), matching the byte-fill idiom sim/resid/sim_map_fill_defaults.cpp already uses
// for this same untyped-region shape.
double read_ambient_double(sim_store &own, int32_t planet_index, int32_t event_index, std::size_t field_offset) {
    double            value;
    const std::size_t byte_offset = ambient_event_byte_offset(planet_index, event_index) + field_offset;
    std::memcpy(&value, own.snd_ambient_by_planet_base() + byte_offset, sizeof(double));
    return value;
}

void write_ambient_double(sim_store &own, int32_t planet_index, int32_t event_index, std::size_t field_offset,
                          double value) {
    const std::size_t byte_offset = ambient_event_byte_offset(planet_index, event_index) + field_offset;
    std::memcpy(own.snd_ambient_by_planet_base() + byte_offset, &value, sizeof(double));
}

} // namespace

// ---- llm_snd_ambient_reseed_planet_event_times @0x0045e277 --------------------------------------
void ambient_reseed_planet_event_times(sim_store &own, int32_t planet_index, double current_time) {
    // 0x0045e292-0x0045e2f8: 20 events, in order (loop counter 0..0x14 exclusive, incremented once
    // per iteration at the tail -- LAB_0045e2a1). One RNG draw per iteration, never hoisted or
    // reordered (translator-brief rule 13).
    for (int32_t event_index = 0; event_index < AMBIENT_EVENT_COUNT; ++event_index) {
        const double replay_delay = read_ambient_double(own, planet_index, event_index, AMBIENT_EVENT_REPLAY_DELAY_OFFSET);
        const double retry_delay  = read_ambient_double(own, planet_index, event_index, AMBIENT_EVENT_RETRY_DELAY_OFFSET);

        // 0x0045e2c0-0x0045e2d1: FLD replay_delay; FADD retry_delay; CALL utils_math_trunc (NOT
        // marshallable, see header banner); FISTP the result to a signed int32. Substituted with
        // std::trunc -- see the header banner's derivation of why this is exact, not approximate.
        const int32_t window = static_cast<int32_t>(std::trunc(replay_delay + retry_delay));

        // 0x0045e2d4-0x0045e2d7: EAX = window (the truncated, now-integer sum) passed to
        // llm_rand_below_fx. Batch A's mh::sim::detail::rand_below_fx has the committed
        // uint32_t(uint32_t) signature (addr/mh_calls.gen.h:1362); the cast is the same
        // bit-pattern-preserving two's-complement round-trip that sibling's own header documents.
        const uint32_t draw = rand_below_fx(own, static_cast<uint32_t>(window));

        // 0x0045e2df-0x0045e2e2: FILD the draw (a signed int32 -- the bytes in EAX reinterpreted,
        // regardless of rand_below_fx's unsigned return type) THEN FADD current_time, in that order.
        const double next_time = static_cast<double>(static_cast<int32_t>(draw)) + current_time;

        // 0x0045e2f2: FSTP next_time back into the SAME event record just read from.
        write_ambient_double(own, planet_index, event_index, AMBIENT_EVENT_NEXT_TIME_OFFSET, next_time);
    }
}

} // namespace mh::sim::detail

// ---- the public wrapper --------------------------------------------------------------------------

namespace mh::sim {

void ambient_reseed_planet_event_times(int32_t planet_index, double current_time) {
    sim_state st = state();
    detail::ambient_reseed_planet_event_times(st.own, planet_index, current_time);
}

} // namespace mh::sim
