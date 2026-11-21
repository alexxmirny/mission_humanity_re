// Session-scoped native queue inputs. MHOR v2 carries an exact HASH/ENTRY pair for every step,
// including empty queues. Native order bytes are unchanged; framing is mh_order_stream.h.
// The step hook owns the existing session axis; HASH is sampled beside the existing pre-body hash,
// ENTRY after AI and before the actual conditional dispatch. Dispatch itself is never forced.
// Sim-thread only. The existing reliable binary log sink owns all I/O and copies borrowed buffers.
#pragma once
#include <cstdint>
#include "mh_order_stream.h"

namespace mh::desync::orders {
inline constexpr int  ORDER_SIZE    = mh::order_stream::RECORD_BYTES;
inline constexpr int  ORDER_QCAP    = 4096;
inline constexpr char ORDERS_NAME[] = "mh_rec_orders.bin";
inline constexpr char CLOCK_NAME[]  = "mh_rec_clock.bin";
using log_fn                        = void (*)(const char *fmt, ...);
bool configure(int enabled, const char *refused, log_fn log);
bool enabled();
void set_compress(bool on);
void step(const char *session_dir, unsigned long gen, bool settled);
void hash(const uint8_t *queue, int count, uint64_t clock_bits);
void entry(const uint8_t *queue, int count);
void session_close(const char *why);
struct stats {
    uint32_t files;
    uint32_t steps;   // complete HASH/ENTRY pairs
    uint32_t records; // ENTRY records, preserving the existing repeat statistic
    uint32_t repeats;
    uint32_t calls; // ENTRY observations, including empty
    uint64_t bytes;
    int64_t  ticks;
    bool     open;
};
stats current();
} // namespace mh::desync::orders
