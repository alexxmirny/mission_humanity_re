#pragma once

#include <cstdint>
#include <cstring>

// Both images may compile this implementation, but exactly one initialized HOST
// owns each live instance. libmh reaches it through the required synchronous port.
namespace mh::participant_boundary {

inline constexpr uint64_t READY            = 0x524f5354u;
inline constexpr uint32_t MAX_PLAYERS      = 8;
inline constexpr uint32_t PAYLOAD_CAPACITY = 1024;
inline constexpr uint32_t TAIL_CAPACITY    = 0x3f8;
inline constexpr uint32_t POLL_STOP        = 0x80000000u;

enum class operation : uint32_t {
    probe                = 0,
    begin                = 1,
    complete             = 2,
    poll                 = 3,
    world_invalidate     = 4,
    can_apply            = 5,
    pending              = 6,
    admission_generation = 7,
    body_active          = 8,
    new_match            = 9,
    end_match            = 10,
    receive_continuation = 11,
};
enum class admission : int32_t {
    invalid      = -1,
    execute_now  = 0,
    queued       = 1,
    duplicate    = 2,
    applied      = 3,
    applied_stop = 4
};
enum class transport_decision { wait,
                                cancel,
                                apply };

struct view {
    double clock;
    double interval;
    bool   multiplayer;
    bool   valid;
};
using reader_fn   = view (*)(void *);
using execute_fn  = bool (*)(const void *, uint32_t); // false preserves the original RX stop/discard-tail
using validate_fn = bool (*)(const void *, uint32_t);
using admit_fn    = int32_t (*)(const void *, uint32_t);

// Payload values are copied, not their pointees. Callback code and every pointee
// must outlive the match. Executors copy their typed payload locally before any
// reentrant call. A receive-backed consensus may validate/cancel its unsent emit;
// an accepted canonical removal has no validator and cannot be silently canceled.
struct request {
    uint32_t       struct_size;
    uint32_t       slot_index;
    uint64_t       horizon_bits;
    execute_fn     execute;
    validate_fn    validate;
    validate_fn    terminal; // original terminal branch: no continuing-body wait
    uint32_t       payload_size;
    const void    *payload;
    uint32_t       received;
    int32_t        tail_sender;
    uint32_t       tail_sender_slot;
    uint32_t       tail_size;
    const uint8_t *tail;
};

inline uint64_t bits(double v) {
    uint64_t r;
    std::memcpy(&r, &v, sizeof(r));
    return r;
}
inline double value(uint64_t v) {
    double r;
    std::memcpy(&r, &v, sizeof(r));
    return r;
}
// Explicit policy for pure detail callers; live factories bind the required port.
inline int32_t synchronous(const void *, uint32_t) { return int32_t(admission::execute_now); }

class authority {
public:
    // Do not inspect or certify an unbound world during host-table installation.
    bool initialize(reader_fn reader, void *context) {
        if (!reader || reader_) return false;
        reader_      = reader;
        context_     = context;
        match_epoch_ = world_epoch_ = 1;
        return true;
    }
    bool ready() const { return reader_ != nullptr; }
    bool pending() const {
        return pending_mask_ != 0 || tails_mask_ != 0 || transport_mask_ != 0 || transport_polling_ != 0;
    }
    bool has_removal(uint32_t index) const {
        return index < MAX_PLAYERS && (pending_mask_ & (1u << index)) != 0;
    }
    uint64_t match_epoch() const { return match_epoch_; }
    bool     body_active() const { return body_depth_ != 0; }
    bool     receive_continuation() const { return receive_continuation_; }
    uint64_t admission_generation() const { return admission_generation_; }

    // The caller resets the old match at its actual lifecycle boundary. At the
    // verified new-world PRE-DRIVER boundary certify the empty prefix, retaining
    // any new-match input already accepted during setup. Imports never call this.
    void new_match() {
        if (!ready()) return;
        const view v     = reader_(context_);
        session_active_  = v.valid && v.multiplayer;
        completed_clock_ = bits(v.clock);
        completed_       = session_active_;
    }
    void end_match() { clear_session(); }
    void invalidate_world() {
        advance(world_epoch_);
        completed_ = false;
        // Same-match publication/refusal does not erase accepted input.
    }
    uint64_t body_begin() {
        if (!ready()) return 0;
        const view v = reader_(context_);
        if (!v.valid) return 0;
        if (!session_active_ && v.multiplayer) session_active_ = true;
        ++body_depth_;
        return world_epoch_;
    }
    void body_complete(uint64_t token) {
        if (!ready() || !token || !body_depth_) return;
        if (--body_depth_) return;
        if (token != world_epoch_) {
            completed_ = false;
            return;
        }
        const view v     = reader_(context_);
        completed_clock_ = bits(v.clock);
        completed_       = v.valid && session_active_ && v.multiplayer;
    }
    bool can_apply(uint64_t horizon) const { return ready() && eligible(reader_(context_), horizon); }

    // Consume all transport deaths even while an earlier, higher-H peer waits.
    // Reconciliation decides WHEN to inspect them; horizon/roster truth is read
    // by the caller then, not frozen before a new hub has reconciled it.
    admission admit_transport_death(uint32_t index, int32_t side) {
        if (!ready() || index >= MAX_PLAYERS) return admission::invalid;
        const auto v = reader_(context_);
        if (!v.valid || !v.multiplayer) return admission::invalid;
        const uint32_t bit = 1u << index;
        if (transport_mask_ & bit)
            return transport_deaths_[index].side == side ? admission::duplicate : admission::invalid;
        transport_deaths_[index] = {side, 0};
        transport_mask_ |= bit;
        advance(admission_generation_);
        return admission::queued;
    }
    bool transport_deaths_pending() const { return transport_mask_ != 0; }
    template <class Inspect, class Execute>
    void drain_transport_deaths(bool held, const Inspect &inspect, const Execute &execute) {
        if (held || transport_polling_ || !transport_mask_) return;
        ++transport_polling_;
        const uint64_t epoch = match_epoch_;
        for (uint32_t i = 0; i < MAX_PLAYERS && transport_mask_; ++i) {
            const uint32_t bit = 1u << i;
            if (!(transport_mask_ & bit)) continue;
            auto &death = transport_deaths_[i];
            ++death.wait;
            const auto decision = inspect(i, death.side);
            if (decision == transport_decision::wait) continue;
            const int32_t  side = death.side;
            const uint32_t wait = death.wait;
            transport_mask_ &= ~bit; // recursive polling must never apply this death twice
            if (decision == transport_decision::apply) execute(i, side, wait);
            if (match_epoch_ != epoch) break;
        }
        --transport_polling_;
    }

    admission admit(const request &r) {
        if (!ready() || r.struct_size != sizeof(request) || r.slot_index >= MAX_PLAYERS ||
            !r.execute || r.payload_size > PAYLOAD_CAPACITY || (r.payload_size && !r.payload) ||
            r.received > 1 || r.tail_size > TAIL_CAPACITY || (r.tail_size && !r.tail) ||
            (!r.received && r.tail_size) || (r.received && r.tail_sender_slot >= MAX_PLAYERS))
            return admission::invalid;
        const view v = reader_(context_);
        if (!v.valid) return admission::invalid;
        if (!v.multiplayer) return admission::execute_now;
        session_active_ = true; // never invents a completion certificate
        if (!r.received && executing_slot_ == r.slot_index + 1 && executing_epoch_ == match_epoch_)
            return admission::execute_now; // a complete continuation invokes its emitter

        uint32_t tail_index = MAX_PLAYERS;
        if (r.received) {
            // A sender's next frame must remain in the transport until this tail
            // has been consumed; accepting a second one would lose stream order.
            for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
                if (tails_[i].occupied && tails_[i].sender == r.tail_sender) return admission::invalid;
                if (!tails_[i].occupied && tail_index == MAX_PLAYERS) tail_index = i;
            }
            if (tail_index == MAX_PLAYERS) return admission::invalid;
        }
        slot      &s            = slots_[r.slot_index];
        const bool duplicate_rx = s.occupied && s.received && r.received;
        if (s.occupied && !r.received) return admission::duplicate;
        if (!duplicate_rx) {
            // Canonical RX supersedes a still-unbroadcast local intent. A second
            // received producer in this still-pending episode shares its one
            // application but retains its OWN sender tail.
            s.occupied     = true;
            s.received     = r.received != 0;
            s.horizon_bits = r.horizon_bits;
            s.execute      = r.execute;
            s.validate     = r.validate;
            s.terminal     = r.terminal;
            s.payload_size = r.payload_size;
            s.serial       = ++serial_;
            if (r.payload_size) std::memcpy(s.payload, r.payload, r.payload_size);
            pending_mask_ |= 1u << r.slot_index;
        }
        if (r.received) {
            tail &t       = tails_[tail_index];
            t.occupied    = true;
            t.ready       = false;
            t.target      = r.slot_index;
            t.dependency  = s.serial;
            t.serial      = ++serial_;
            t.sender      = r.tail_sender;
            t.sender_slot = r.tail_sender_slot;
            t.size        = r.tail_size;
            if (r.tail_size) std::memcpy(t.bytes, r.tail, r.tail_size);
            tails_mask_ |= 1u << tail_index;
        }
        advance(admission_generation_);
        const uint64_t epoch = match_epoch_, serial = s.serial;
        const uint32_t result = poll();
        if ((result & POLL_STOP) || match_epoch_ != epoch) return admission::applied_stop;
        if (!s.occupied || s.serial != serial) return admission::applied;
        return admission::queued;
    }

    // This drains only certified work. The host must resume its existing RX
    // dispatcher when receive_ready() is true, BEFORE another driver increment.
    uint32_t poll() {
        if (!ready() || polling_ || !pending_mask_) return 0;
        ++polling_;
        uint32_t applied = 0;
        for (uint32_t n = 0; n < MAX_PLAYERS && pending_mask_; ++n) {
            const view v = reader_(context_);
            if (!v.valid) break;
            if (!v.multiplayer) {
                clear_session();
                break;
            }
            uint32_t chosen        = MAX_PLAYERS;
            bool     cancel_chosen = false;
            for (uint32_t i = 0; i < MAX_PLAYERS; ++i) {
                const slot &s = slots_[i];
                if (!s.occupied) continue;
                const bool cancel = s.validate && !s.validate(s.payload, s.payload_size);
                const bool run    = !cancel && (eligible(v, s.horizon_bits) ||
                                             (s.terminal && s.terminal(s.payload, s.payload_size)));
                if ((cancel || run) && (chosen == MAX_PLAYERS || s.serial < slots_[chosen].serial)) {
                    chosen        = i;
                    cancel_chosen = cancel;
                }
            }
            if (chosen == MAX_PLAYERS) break;
            slot          &s     = slots_[chosen];
            const uint64_t epoch = match_epoch_, serial = s.serial;
            if (cancel_chosen) {
                if (match_epoch_ != epoch) break;
                if (s.serial == serial) erase(chosen);
                continue;
            }
            if (match_epoch_ != epoch || s.serial != serial) break;
            executing_slot_   = chosen + 1;
            executing_epoch_  = epoch;
            const bool resume = s.execute(s.payload, s.payload_size);
            executing_slot_   = 0;
            executing_epoch_  = 0;
            ++applied;
            if (match_epoch_ != epoch) break;
            if (s.serial == serial) erase(chosen, resume);
            if (!resume) {
                applied |= POLL_STOP;
                break;
            }
        }
        --polling_;
        return applied;
    }

    bool receive_ready() const {
        if (!tails_mask_) return false;
        for (const tail &t : tails_)
            if (t.occupied && t.ready) return true;
        return false;
    }
    bool blocks_sender(int32_t sender) const {
        if (!tails_mask_) return false;
        for (const tail &t : tails_)
            if (t.occupied && t.sender == sender) return true;
        return false;
    }
    bool holds_player(uint32_t player) const {
        if (!tails_mask_) return false;
        for (const tail &t : tails_)
            if (t.occupied && t.sender_slot == player) return true;
        return false;
    }
    uint32_t blocked_senders(int32_t *out, uint32_t capacity) const {
        uint32_t n = 0;
        if (!tails_mask_) return n;
        for (const tail &t : tails_) {
            if (!t.occupied) continue;
            if (n < capacity) out[n] = t.sender;
            ++n;
        }
        return n;
    }
    // Briefly block recursive RX during a complete suffix, never while waiting
    // for its body. Otherwise allow OTHER origins to deliver funding horizons.
    bool receive(int32_t *sender, void *buffer, int32_t *length) {
        receive_continuation_ = false;
        if (executing_slot_) {
            if (length) *length = 0;
            return true;
        }
        for (uint32_t n = 0; n < MAX_PLAYERS; ++n) {
            uint32_t chosen = MAX_PLAYERS;
            for (uint32_t i = 0; tails_mask_ && i < MAX_PLAYERS; ++i) {
                const tail &t = tails_[i];
                if (t.occupied && t.ready &&
                    (chosen == MAX_PLAYERS || t.serial < tails_[chosen].serial)) chosen = i;
            }
            if (chosen == MAX_PLAYERS) return false;
            tail &t = tails_[chosen];
            if (!length) return true;
            if (t.size && (!buffer || *length < 0 || static_cast<uint32_t>(*length) < t.size)) {
                *length = 0;
                return true; // retain accepted input rather than truncate it
            }
            if (t.size) {
                std::memcpy(buffer, t.bytes, t.size);
                if (sender) *sender = t.sender;
                *length               = static_cast<int32_t>(t.size);
                receive_continuation_ = true;
            }
            const bool delivered = t.size != 0;
            t.occupied           = false;
            tails_mask_ &= ~(1u << chosen);
            if (delivered) return true;
        }
        return false;
    }
    uint64_t boundary(uint32_t op, uint64_t argument) {
        switch (static_cast<operation>(op)) {
            case operation::probe: return ready() ? READY : 0;
            case operation::begin: return body_begin();
            case operation::complete: body_complete(argument); return 0;
            case operation::poll: return poll();
            case operation::world_invalidate: invalidate_world(); return 0;
            case operation::can_apply: return can_apply(argument) ? 1 : 0;
            case operation::pending: return pending() ? 1 : 0;
            case operation::admission_generation: return admission_generation();
            case operation::body_active: return body_active() ? 1 : 0;
            case operation::new_match: new_match(); return ready() ? READY : 0;
            case operation::end_match: end_match(); return 0;
            case operation::receive_continuation: return receive_continuation_ ? 1 : 0;
        }
        return 0;
    }

private:
    struct slot {
        bool        occupied = false, received = false;
        uint64_t    horizon_bits = 0, serial = 0;
        execute_fn  execute      = nullptr;
        validate_fn validate     = nullptr;
        validate_fn terminal     = nullptr;
        uint32_t    payload_size = 0;
        uint8_t     payload[PAYLOAD_CAPACITY]{};
    };
    struct tail {
        bool     occupied = false, ready = false;
        uint32_t target = 0, sender_slot = 0, size = 0;
        uint64_t dependency = 0, serial = 0;
        int32_t  sender = 0;
        uint8_t  bytes[TAIL_CAPACITY]{};
    };
    struct transport_death {
        int32_t  side = 0;
        uint32_t wait = 0;
    };
    static void advance(uint64_t &v) {
        if (++v == 0) ++v;
    }
    bool eligible(const view &v, uint64_t horizon) const {
        // Same double addition and <= as the fixed-step driver. Unordered exits
        // that loop too. No rounded millisecond conversion invents a funded step.
        return v.valid && v.multiplayer && completed_ && !body_depth_ && completed_clock_ == bits(v.clock) &&
               !(v.clock + v.interval <= value(horizon));
    }
    void erase(uint32_t i, bool resume = true) {
        for (uint32_t j = 0; j < MAX_PLAYERS; ++j) {
            tail &t = tails_[j];
            if (t.occupied && t.target == i && t.dependency == slots_[i].serial) {
                if (resume) t.ready = true;
                else {
                    t.occupied = false;
                    tails_mask_ &= ~(1u << j);
                }
            }
        }
        slots_[i].occupied = false;
        pending_mask_ &= ~(1u << i);
    }
    void clear_session() {
        advance(match_epoch_);
        advance(world_epoch_);
        advance(admission_generation_);
        for (slot &s : slots_) s.occupied = false;
        for (tail &t : tails_) t.occupied = false;
        pending_mask_ = tails_mask_ = transport_mask_ = executing_slot_ = 0;
        executing_epoch_                                                = 0;
        completed_ = session_active_ = receive_continuation_ = false;
        // Physical body depth and polling belong to active C++ invocations, even across reset.
    }

    reader_fn       reader_  = nullptr;
    void           *context_ = nullptr;
    slot            slots_[MAX_PLAYERS]{};
    tail            tails_[MAX_PLAYERS]{};
    transport_death transport_deaths_[MAX_PLAYERS]{};
    uint32_t        transport_mask_ = 0, transport_polling_ = 0;
    uint64_t        match_epoch_ = 0, world_epoch_ = 0, admission_generation_ = 0;
    uint64_t        serial_ = 0, completed_clock_ = 0, executing_epoch_ = 0;
    uint32_t        pending_mask_ = 0, tails_mask_ = 0, body_depth_ = 0, polling_ = 0, executing_slot_ = 0;
    bool            session_active_ = false, completed_ = false, receive_continuation_ = false;
};

} // namespace mh::participant_boundary
