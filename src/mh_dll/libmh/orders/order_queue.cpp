#include "orders/order_queue.h"
#include "state/host_api.h" // LIFT-TABLE S6/S7: these callees are host-API entries now

#include "addr/mh_export.gen.h"  // MH_EXPORT_REPLACE / the entry-thunk shapes
#include "addr/mh_calls.gen.h"   // typed callables for the original functions we still call OUT to
#include "addr/mh_regions.gen.h" // the state region REGISTRY -- where this module's state lives
#include "orders/order_codec.h"  // ST5: the ONE order-record layout, shared with the wire
#include "sim/rng_trace.h"       // LIB-REF step-5000: the ORDER-QUEUE COUNT LEDGER (see below)

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h> // _ReturnAddress -- the ledger's caller tag

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::orders {

const container_state &state() {
    // Addresses come from the state region REGISTRY (RI-STATE / ST1), never spelled here. Until
    // 2026-07-30 they came straight from mh_addrs.gen.h, which was already safe against a Ghidra
    // rename -- what it was NOT safe against is another consumer of the same bytes disagreeing:
    // SavePlanetToDisk's first block IS this queue, reached by an independent derivation that did
    // not even resolve the symbol. The registry makes those one derivation plus a static_assert.
    // Byte-identical addresses either way, which is what makes ST1 provably behaviour-free.
    using namespace mh::state;
    static const container_state st = {
        ptr<order>(RID_STRAT_ORDER_QUEUE),
        ptr<int32_t>(RID_STRAT_ORDER_QUEUE_COUNT),
        ptr<order>(RID_STRAT_ORDER_PENDING),
        ptr<int32_t>(RID_STRAT_ORDER_PENDING_COUNT),
        ptr<int32_t>(RID_STRAT_ORDER_SCRATCH_ARGS),
        ptr<order>(RID_STRAT_ORDER_STAGING),
        ptr<int32_t>(RID_STRAT_ORDER_STAGING_COUNT),
        ptr<double>(RID_STRAT_LOCKSTEP_HORIZON),
        mh::net::packet_buffer{ptr<uint8_t>(RID_NET_SEND_BUF), ptr<int32_t>(RID_NET_SEND_BUF_CURSOR)},
        ptr<const mh::game::mh_llm_strat_player_profile>(RID_STRAT_PLAYERS),
        ptr<const int32_t>(RID_GAME_SESSION_MODE),
        ptr<const uint8_t>(RID_PLAYERS),
        ptr<const double>(RID_STRAT_GAME_CLOCK),
        ptr<const double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
        ptr<mh::game::mh_map_object_unit>(RID_UNITS),
        ptr<const uint8_t>(RID_UNIT),
        live_roster_caps(), // SB-BIND T2: derived row capacities
    };
    return st;
}

// The order COUNTS, for consumers outside this module. The turn engine gates release_due/schedule on
// them and used to bind its own pointers to the same two addresses (RI-STATE / ST1 done_when: two
// owners of one region). It asks here now, so "where do the order counts live" has exactly one
// answer, and moving them later is this module's decision alone.
const int32_t *pending_count() { return state().pending_count; }
const int32_t *staging_count() { return state().staging_count; }

const game_calls &live_calls() {
    static const game_calls gc = {
        MH_LIBMH_BIND(llm_strat_order_integrity_check),
        MH_LIBMH_BIND(llm_net_lockstep_commit_horizon),
        &MH_PROMOTED_ROW(llm_net_send_order),
        MH_LIBMH_BIND(llm_net_send_buf_flush),
        MH_LIBMH_BIND(llm_unit_set_order_param),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_menu_force_return_to_main),
    };
    return gc;
}

const game_calls &inert_calls() {
    static const game_calls gc = {
        [](const order *, char *) {},
        []() {},
        [](const order *) -> int32_t { return 0; },
        []() -> int32_t { return 0; },
        [](int32_t, int32_t, int16_t) {},
        [](uint32_t, int32_t, uint32_t) {},
        [](uint32_t, int32_t, uint32_t) {},
        []() {},
    };
    return gc;
}

// ---- LIB-REF-SPLIT: the last original-image DATA POINTER in the migrated modules ---------------
//
// Hosted this must stay the ORIGINAL string's address and not a copy of the text, for the reason the
// header states: the callee is the game's own llm_strat_order_integrity_check, and it sees the same
// pointer VALUE the original passed it. Standalone there is no string at 0x005011a4 -- it was six
// immediates in order_queue.obj, the `s_PropozycjaRozkazu`-class dependency this item's scope names
// by name.
//
// THE STANDALONE ARM IS SAFE BECAUSE THE CALLEE IGNORES IT, and that is measured rather than hoped:
// the standalone binder resolves llm_strat_order_integrity_check to ::mh::orders::integrity_check,
// whose signature is `void integrity_check(const order *rec, char *)` -- the tag parameter is
// UNNAMED, so no standalone path dereferences or compares it. Our own copy of the text is therefore
// strictly more defined than the hosted pointer is: readable if anything ever logs it, and not an
// address into a binary that is not loaded.
//
// NOT a nullptr: `char *` here is passed through two call layers and a null would turn a harmless
// unused argument into a crash the day someone adds a %s.
#ifdef MH_LIBMH_BUILD
char *proposal_tag() {
    static char tag[] = "PropozycjaRozkazu";
    return tag;
}
#else
char *proposal_tag() { return reinterpret_cast<char *>(mh::addr::s_PropozycjaRozkazu); }
#endif

// Logging, mirroring mh::shadow's own shape: the module owns a logger pointer and the seam layer
// wires it to seam_log at arm time. Keeps mh/orders free of a dependency on mh/seams.
namespace {
void (*g_log)(const char *) = nullptr;
}

void say(const char *fmt, ...) {
    if (!g_log) return;
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(line);
}

void set_logger(void (*fn)(const char *)) { g_log = fn; }

namespace {

// The original moves records with `MOV ECX,0x11 / REP MOVSD` -- 17 dwords, the whole 0x44. Both
// compaction loops can hand it dst == src (the keep-in-place case), which REP MOVSD tolerates and a
// struct assignment technically need not, so this is memmove rather than `*dst = *src`.
inline void copy_record(order *dst, const order *src) { std::memmove(dst, src, sizeof(order)); }

// stage_scheduled stores each ushort as its LOW BYTE DUPLICATED into both halves
// (XOR DH,DH / SHL EAX,8 / OR EDX,EAX @0x00466242-0x004662a7). This is NOT the plain low-byte mask
// pending_enqueue applies, and it is invisible in the decompile. It is an integrity code: the
// duplicated form travels on the wire, and both receiving paths (schedule's local copy into PENDING,
// and pending_enqueue on the remote side) collapse it back with `& 0xff`.
inline uint16_t dup_low_byte(uint16_t v) {
    const uint16_t b = static_cast<uint16_t>(v & 0xff);
    return static_cast<uint16_t>(b | (b << 8));
}

} // namespace

namespace detail {

// llm_strat_order_scratch_reset @0x00466812 -- all 13 slots to the "leave unchanged" sentinel.
void scratch_reset(const container_state &st) {
    for (int i = 0; i < ARG_SLOTS; ++i) st.scratch[i] = ARG_UNCHANGED;
}

// llm_strat_order_scratch_set_field @0x0046685d.
// NO BOUNDS CHECK, deliberately: the original has none (00466883 is a bare indexed store) and 238
// call sites depend on whatever it currently does. Adding one would be a behaviour change, and the
// shadow oracle would not even catch it -- an out-of-range write lands outside the declared region.
void scratch_set_field(const container_state &st, int32_t index, int32_t value) {
    st.scratch[index] = value;
}

// ---- LIB-REF step-5000: THE ORDER-QUEUE COUNT LEDGER --------------------------------------------
//
// WHAT IT IS FOR. At step 5000 the hosted replay holds order_queue_count = 25 and the standalone
// reference host holds 24, and every earlier step is masked because the replay injector rewrites
// [0, n) and SETS the count on every order-carrying step. One arm therefore does one more (or one
// fewer) thing to that integer during a step body than the other, and six guesses about WHICH have
// already cost a session each.
//
// So this stops guessing the same way the RNG draw trace did: it records EVERY write to
// order_queue_count, at every site that performs one, with the identity of the record involved --
// a per-step LEDGER whose entries must add up to the count the next step observes. Two arms'
// ledgers are directly diffable because the entries are order identities (order_code, unit_index,
// owner_and_kind, param0) and counts, none of which is module-relative.
//
// THE RECONCILIATION IS THE POINT, not the individual lines: `count after inject` plus the sum of
// the step's ledger deltas must equal the count the NEXT step's inject note reports at entry. A
// residual means a writer that is not on this list -- which is itself the answer, and a failure
// mode the instrument can state rather than hide.
//
// TAGS (shared numbering with sim/rng_trace.h's note stream; 1/2/4 are the step-290 hunt's):
//   10 enqueue APPEND       a=order_code b=unit_index c=owner_and_kind d=param0 e=new count f=site
//   11 enqueue OVERFLOW     a=count seen, e=0 (the original DISCARDS the queue, it does not reject)
//   12 release_due APPEND   a..d = the released record's identity, e=new count, f=player
//   13 apply_and_dequeue    a..c = the dequeued record, d=queue index, e=new count, f=player
//   14 dispatch TAIL        a=count at entry, e=`kept` (the compaction cursor that becomes the count)
//   15 enqueue SUPPRESSED   a..d = the refused record's identity, e=count (unchanged), f=site
//   16 injector             a=records injected, e=count after inject + tail-clear
//
// COST AND SAFETY: `rng_trace_add_note` is two integer compares when the step is outside the armed
// window, and the window is disarmed unless a host asks for it. The notes only READ state.
//
// `g_ledger_site` is the frame ABOVE the public wrapper -- for a hosted run that distinguishes a
// call arriving through the promotion wrapper (the game's own code) from one made by libmh's body,
// which is exactly the standalone-vs-hosted question. It is stale-by-design if a call ever reaches
// detail:: without passing a public wrapper, which is why the identity fields, not the address, are
// what the two arms are diffed on.
const void *g_ledger_site = nullptr;

// llm_strat_order_enqueue @0x00466094 -- the IMMEDIATE lane.
//
// Two things here are counter-intuitive and both are load-bearing:
//   * On overflow the original does NOT reject the new order -- it sets the count to 0, discarding
//     the ENTIRE queue, and returns 0 (00466153). Rejecting instead would be the "sensible" bug.
//   * exec_time (+0x00) is NEVER written. The immediate lane leaves whatever the slot's previous
//     occupant left there; the struct comment calls this "stale on direct local enqueues". Zeroing
//     it would diverge.
int32_t enqueue(const container_state &st, uint16_t unit_index, uint16_t owner_and_kind,
                int16_t param0, uint16_t order_code) {
    // ---- D18 / LIB-REF-REC: THE REPLAY SUPPRESSION GATE, AT THE SINK (user ruling, 2026-09-11) ---
    //
    // WHY IT IS HERE AND NOT IN THE PROMOTION WRAPPER, WHERE IT LIVED UNTIL TODAY. `[harness]
    // replay_suppress_enqueue=1` is documented -- in the fixture manifest, in order_queue.h and in
    // harness.cpp -- as "llm_strat_order_enqueue returns 0 and appends nothing, so the INJECTED
    // recording is the only thing that reaches the order queue". With the check in
    // promoted::enqueue that sentence was FALSE, and measurably so: the promotion wrapper only sees
    // calls that arrive through the GAME'S entry at 0x00466094. Every libmh-internal caller goes
    // through MH_LIBMH_BIND, and with the rebind row armed (measured: `[rebind] llm_strat_order_
    // enqueue -> OURS`, 715 of 718 rows armed) that binds straight to mh::orders::enqueue -- past the
    // gate. So did every caller routed through our own dispatch: detail::dispatch reaches this
    // function by a plain in-TU call on the non-mode-3 immediate lane and never crosses an entry at
    // all. The flag's coverage was therefore shrinking with every promotion, silently.
    //
    // That is not hypothetical: it is the whole of LIB-REF's step-5000 residual. The harness's
    // synth_move workload (re-armed at replay by the fixture contract) issued one mothership move per
    // step through llm_strat_unit_order_move -> our promoted issue body -> dispatch -> HERE, appending
    // an order the suppression was supposed to have stopped. The injector's `count = k` overwrite hid
    // it at 4940 steps; the one step that injects nothing exposed it.
    //
    // SCOPE, stated rather than implied: this is the gate on the IMMEDIATE lane, which is the only
    // path that APPENDS to ORDER_QUEUE in a non-lockstep session. The mode-3 lane reaches the queue
    // through stage_scheduled -> schedule -> pending -> release_due instead, and release_due is NOT
    // suppressed -- deliberately, because neutering it would break an MP replay's order delivery
    // rather than merely quieten a local workload. A future MP-mode replay fixture must state that.
    //
    // FAITHFULNESS: the flag defaults FALSE and nothing outside a replay lane sets it, so an ordinary
    // game, MP or determinism run executes exactly the body it executed before -- one predicate on a
    // file-static bool. Same contract as reimpl_fixes: a flag whose default changed what this body
    // does would make every unflagged run a different function.
    if (mh::orders::suppress_enqueue()) {
        // Ledger tag 15: the suppression is OBSERVABLE, not merely asserted. A gate nobody can see
        // fire is the same failure class as the one this move exists to fix -- the old placement also
        // "worked" in every log while covering less and less of the call graph.
        mh::sim::rng_trace_add_note(15u, (uint32_t)order_code, (uint32_t)unit_index,
                                    (uint32_t)owner_and_kind, (uint32_t)(uint16_t)param0,
                                    (uint32_t)*st.queue_count, (uint32_t)(uintptr_t)g_ledger_site);
        return 0;
    }

    if (*st.queue_count >= QUEUE_CAP) {
        mh::sim::rng_trace_add_note(11u, (uint32_t)*st.queue_count, (uint32_t)unit_index,
                                    (uint32_t)owner_and_kind, (uint32_t)(uint16_t)param0, 0u,
                                    (uint32_t)(uintptr_t)g_ledger_site);
        *st.queue_count = 0;
        return 0;
    }
    order &rec         = st.queue[*st.queue_count];
    rec.unit_index     = unit_index;
    rec.owner_and_kind = owner_and_kind;
    rec.param0         = param0;
    rec.order_code     = order_code;
    for (int i = 0; i < ARG_SLOTS; ++i) rec.args[i] = st.scratch[i];
    *st.queue_count += 1;
    mh::sim::rng_trace_add_note(10u, (uint32_t)order_code, (uint32_t)unit_index,
                                (uint32_t)owner_and_kind, (uint32_t)(uint16_t)param0,
                                (uint32_t)*st.queue_count, (uint32_t)(uintptr_t)g_ledger_site);
    return *st.queue_count; // the NEW count (00466149), not the index written
}

// llm_strat_order_pending_enqueue @0x00466790 -- the NET-delivered lane.
//
// Masks four ushort fields to their low byte IN PLACE before copying (004667b4-004667c6). The
// mutation is invisible to the caller because the record is a by-value stack copy, but it must
// happen before the copy or the stored record differs. See O1b: if unit_index really is a unit
// index, that 8-bit mask caps it at 255, which retail never reaches but the 500-unit build does.
int32_t pending_enqueue(const container_state &st, order *rec) {
    if (*st.pending_count >= PENDING_CAP) {
        *st.pending_count = 0;
        return 0;
    }
    order &in = *rec;
    in.unit_index &= 0xff;
    in.owner_and_kind &= 0xff;
    in.param0 = static_cast<int16_t>(in.param0 & 0xff);
    in.order_code &= 0xff;
    st.pending[*st.pending_count] = in; // 17 dwords, REP MOVSD in the original
    *st.pending_count += 1;
    return 1;
}

// llm_strat_order_stage_scheduled @0x00466211 -- the SCHEDULED lane's front door, taken whenever
// order_dispatch sees SESSION_MODE 3 and a network-controlled player.
//
// Three things a plausible translation gets wrong:
//   * On overflow it REJECTS (returns 0, count untouched). Its two sibling enqueues EMPTY their
//     array instead. Mirroring the siblings here would silently discard 300 staged orders.
//   * The four ushorts are stored byte-DUPLICATED, not masked (see dup_low_byte).
//   * exec_time is copied as raw bytes (two dword stores, 0x004662b8/0x004662c1), never through the
//     x87 stack -- so a signalling NaN passes through unchanged. `rec.exec_time = exec_time` in a
//     32-bit build can round-trip it through FLD/FSTP and quiet it. memcpy keeps the bits.
int32_t stage_scheduled(const container_state &st, const game_calls &gc, char *tag,
                        uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                        uint16_t order_code, double exec_time) {
    if (*st.staging_count >= STAGING_CAP) return 0;

    order &hdr         = st.staging[*st.staging_count];
    hdr.unit_index     = dup_low_byte(unit_index);
    hdr.owner_and_kind = dup_low_byte(owner_and_kind);
    hdr.param0         = static_cast<int16_t>(dup_low_byte(static_cast<uint16_t>(param0)));
    hdr.order_code     = dup_low_byte(order_code);
    std::memcpy(&hdr.exec_time, &exec_time, sizeof(double));

    // Called on the record AS IT STANDS -- header written, args NOT yet. The callee gets a by-value
    // copy whose args[] still hold whatever the slot's previous occupant left there, and that is what
    // the original checks (0x004662cd copies before 0x004662eb writes the args).
    gc.integrity_check(&hdr, tag);

    // Re-index rather than reuse `hdr`: the original recomputes `STAGING_COUNT * 0x44` after the call
    // (IMUL @0x00466308). integrity_check can bail to the main menu on failure, so the count it
    // reads afterwards is not provably the one it read before. Faithful is free here.
    order &tail = st.staging[*st.staging_count];
    for (int i = 0; i < ARG_SLOTS; ++i) tail.args[i] = st.scratch[i];

    *st.staging_count += 1;
    return *st.staging_count; // the NEW count (0x0046632b)
}

// llm_strat_order_schedule @0x00466348 -- drains STAGING into PENDING once per lockstep pump, moves
// the horizon, and broadcasts each order.
//
// The horizon block leaves exec_time == horizon in EVERY case: an order past the horizon PULLS the
// horizon forward (and commits it to the peers), an order behind it is CLAMPED up. That is the
// lockstep-safety invariant, and it is why the horizon only ever increases.
//
// The wire gets the STAGING record -- still byte-duplicated. Only the local PENDING copy is collapsed
// with `& 0xff`, which is the mirror of what pending_enqueue does to a remote one. Sending the masked
// form would break the receiver's integrity check.
int32_t schedule(const container_state &st, const game_calls &gc) {
    int32_t       flushed = 0;
    int32_t       out     = 0;                 // compaction cursor: where the NOT-drained orders end up
    const int32_t n       = *st.staging_count; // snapshotted before the loop (0x0046636e)

    for (int32_t i = 0; i < n; ++i) {
        if (*st.pending_count >= PENDING_CAP) {
            // Pending is full: keep this order staged for a later call rather than dropping it.
            copy_record(&st.staging[out], &st.staging[i]);
            ++out;
            continue;
        }

        order &s = st.staging[i];
        if (!(*st.horizon >= s.exec_time)) { // FCOMP/JNC -- also the unordered (NaN) case
            *st.horizon = s.exec_time;
            gc.commit_horizon();
        } else if (*st.horizon > s.exec_time) {
            s.exec_time = *st.horizon;
        }

        order &p = st.pending[*st.pending_count];
        copy_record(&p, &s);
        p.owner_and_kind &= 0xff; // masked in this order in the original, 0x00466411-0x00466492
        p.unit_index &= 0xff;
        p.param0 = static_cast<int16_t>(p.param0 & 0xff);
        p.order_code &= 0xff;
        *st.pending_count += 1;

        gc.send_order(&s); // return value DISCARDED by the original (0x004664b5)
        if (st.packet.order_would_overflow()) flushed += gc.send_buf_flush();
    }

    *st.staging_count = out;
    if (st.packet.dirty()) flushed += gc.send_buf_flush();
    return flushed;
}

// llm_strat_order_release_due @0x0046652e -- PENDING -> QUEUE for orders whose time has come.
//
// Walked PER PLAYER, and only for players whose PLAYER_ALIVE bit is set; a dead or absent player's
// pending orders are compacted and kept, never released. The pass rewrites PENDING in place: a
// released order is CONSUMED (not written back), everything else is compacted down.
//
// The dedup rule is the subtle part, and it is narrower than "one order per unit". A queued entry is
// only a candidate when it is for the same unit+owner AND is a DIFFERENT order (param0 or order_code
// differs). A BYTE-EQUAL order is skipped by the scan without being counted as a match, so it is
// appended -- the same order really can sit in the queue twice, and a reimplementation that "helpfully"
// collapsed those would drop work the original performs. When the candidate is different, the queued
// entry is OVERWRITTEN only if it is the weaker of the two: earlier exec_time, or the same exec_time
// and a lower order_code.
int32_t release_due(const container_state &st, double now) {
    int32_t out = 0;

    for (int32_t player = 0; player < MAX_PLAYERS && *st.pending_count > 0; ++player) {
        if ((st.players[player].status_flags & PLAYER_ALIVE) == 0) continue;

        const int32_t n = *st.pending_count; // snapshotted per player (0x00466584)
        out             = 0;

        for (int32_t i = 0; i < n; ++i) {
            order &p = st.pending[i];

            // `!(exec_time > now)`, NOT `exec_time <= now`. The original is FCOMP/FNSTSW/SAHF/JBE
            // (0x004665d1-0x004665d7): with CF=C0 and ZF=C3, JBE is taken for less, for equal AND
            // for UNORDERED -- only the strictly-ordered `>` falls through. C++ `<=` is false on a
            // NaN, so it would take the opposite branch and leave a NaN-timed order sitting in
            // PENDING that the original releases. `!(a > b)` funnels unordered the same way JBE
            // does. Same reasoning as the horizon comparison in schedule(); found by the
            // reimpl-verify review 2026-07-27, where the two sites had been written inconsistently.
            const bool release = (static_cast<int32_t>(p.owner_and_kind & 0xf) == player) &&
                                 !(p.exec_time > now) && (*st.queue_count < QUEUE_CAP);
            if (!release) {
                copy_record(&st.pending[out], &p);
                ++out;
                continue;
            }

            bool matched = false;
            for (int32_t q = 0; q < *st.queue_count; ++q) {
                order &e = st.queue[q];
                if (e.unit_index != p.unit_index || e.owner_and_kind != p.owner_and_kind) continue;
                if (e.param0 == p.param0 && e.order_code == p.order_code) continue;

                bool replace = false;
                if (!(e.exec_time >= p.exec_time))
                    replace = true; // queued one is older -> the newer wins
                else if (e.exec_time == p.exec_time && e.order_code < p.order_code)
                    replace = true; // same instant -> higher order_code wins (unsigned CMP/JNC)
                if (replace) copy_record(&e, &p);

                matched = true; // set even when the queued entry stood: the incoming one is consumed
            }

            if (!matched) {
                copy_record(&st.queue[*st.queue_count], &p);
                *st.queue_count += 1;
                mh::sim::rng_trace_add_note(12u, (uint32_t)p.order_code, (uint32_t)p.unit_index,
                                            (uint32_t)p.owner_and_kind,
                                            (uint32_t)(uint16_t)p.param0,
                                            (uint32_t)*st.queue_count, (uint32_t)player);
            }
        }

        *st.pending_count = out;
    }

    return *st.pending_count > 0 ? 0 : 1;
}

// llm_strat_order_dispatch @0x00465fdf -- the router the 63 replicated-lane wrappers call.
//
// Watch the argument RENAMING: this function's (unit_id, player, op_code, arg) are loaded into
// EAX/EDX/EBX/ECX and handed to a callee whose parameters are (unit_index, owner_and_kind, param0,
// order_code). So `player` becomes owner_and_kind and `op_code` becomes param0 -- the names differ
// on the two sides of one call, and swapping them would be silent and catastrophic.
//
// Both lanes MOVZX their arguments to 16 bits first (0x00466058-0x00466064, 0x00466072-0x0046607e),
// including `player`, which arrives as a 32-bit uint and is truncated to its low word.
int32_t dispatch(const container_state &st, const game_calls &gc, char *tag, uint16_t unit_id,
                 uint32_t player, uint16_t op_code, uint16_t arg) {
    const bool net_player =
        *st.session_mode == SESSION_MP_LOCKSTEP &&
        (st.net_players[static_cast<uint16_t>(player & 0xf) * PLAYERS_STRIDE + PLAYERS_FLAG_OFF] &
         NET_CONTROLLED) != 0;

    if (!net_player)
        return enqueue(st, unit_id, static_cast<uint16_t>(player), op_code, arg);

    double t = *st.game_clock + *st.step_size;
    // `!(t >= horizon)`, not `t < horizon`: FCOMP/JNC is taken (skipping the clamp) only when CF=0,
    // i.e. strictly ordered t >= horizon. Unordered sets CF and falls through to the clamp. Same
    // shape as the two comparisons in schedule() and the one in release_due().
    if (!(t >= *st.horizon)) std::memcpy(&t, st.horizon, sizeof(double)); // raw dword pair, 0x0046603f

    return stage_scheduled(st, gc, tag, unit_id, static_cast<uint16_t>(player),
                           static_cast<int16_t>(op_code), arg, t);
}

// llm_strat_order_queue_find_index @0x00469996 -- pure lookup, the first half of the cancel/replace
// pair. Returns the QUEUE index, or -1.
//
// The two nibble tests read the SAME field: `owner_and_kind & 0xf` is the owner, `& 0xf0` the kind.
// So the parameter (named `kind_tag` -- matches the Ghidra rename applied 2026-08-21, finding
// 2026-08-21-1329-6) is really the KIND nibble, already shifted into place by the caller -- it is
// compared against the masked field directly, not shifted here.
int32_t queue_find_index(const container_state &st, int32_t player, int32_t unit_idx,
                         int32_t kind_tag) {
    for (int32_t i = 0; i < *st.queue_count; ++i) {
        const order &e = st.queue[i];
        if (static_cast<uint16_t>(player) != (e.owner_and_kind & 0xf)) continue;
        if (static_cast<uint16_t>(kind_tag) != (e.owner_and_kind & 0xf0)) continue;
        if (static_cast<uint16_t>(unit_idx) != e.unit_index) continue; // CMP AX,word -- low 16 only
        return i;
    }
    return -1;
}

// llm_strat_order_queue_apply_and_dequeue @0x00469a37 -- the apply half: pour the queued order's
// args into the unit, then remove the entry and compact the queue.
//
// Every args[] slot is guarded by the -1 "leave unchanged" sentinel, and the destination offsets
// match the args[] field comment on llm_strat_order exactly ([0]/[1] -> goal_x/goal_y, [2] ->
// home_storage_slot, [3]/[4] -> target_fine_x/y, [5] -> target_ref, [6] -> target_index, [7] ->
// selected_weapon, [12] -> move_group_id). Slots 8..11 are NOT applied here.
//
// TWO THINGS TO REPRODUCE RATHER THAN TIDY:
//   * args[6] is written under a guard on args[5] (0x00469bf9 re-tests 0xbb4ef4, the args[5] slot,
//     and then stores args[6] from 0xbb4ef8). Reading it as a copy-paste slip is tempting, but
//     target_ref and target_index are a PAIR and target_ref is the "is there a target" discriminator
//     -- setting the index without the ref would be meaningless. Deliberate or not, ours does the
//     same, and a unit test pins it.
//   * The target release runs TWICE in the original -- once up front, clearing the old target, and
//     once at the end, after the args may have installed a NEW one. Both are guarded on target_ref
//     being non-zero, and they pass DIFFERENT mode arguments (1 then 0).
void queue_apply_and_dequeue(const container_state &st, const game_calls &gc, uint32_t player,
                             int32_t unit_idx, int32_t queue_idx) {
    const order &q = st.queue[queue_idx];
    auto        &u = st.units[static_cast<uint16_t>(player) * st.caps.units + unit_idx];

    gc.unit_set_order_param(static_cast<uint16_t>(player), unit_idx, q.param0);

    if (u.target_ref != 0) {
        gc.target_release_ref(static_cast<uint16_t>(player), unit_idx, 1);
        u.target_ref   = 0;
        u.target_index = 0;
    }

    if (q.args[0] != ARG_UNCHANGED_32) u.goal_x = static_cast<uint8_t>(q.args[0]);
    if (q.args[1] != ARG_UNCHANGED_32) u.goal_y = static_cast<uint8_t>(q.args[1]);
    if (q.args[2] != ARG_UNCHANGED_32) u.home_storage_slot = static_cast<uint8_t>(q.args[2]);
    if (q.args[3] != ARG_UNCHANGED_32) u.target_fine_x = q.args[3];
    if (q.args[4] != ARG_UNCHANGED_32) u.target_fine_y = q.args[4];
    if (q.args[5] != ARG_UNCHANGED_32) u.target_ref = static_cast<int16_t>(q.args[5]);
    if (q.args[5] != ARG_UNCHANGED_32) u.target_index = static_cast<int16_t>(q.args[6]); // [5], not [6]
    if (q.args[7] != ARG_UNCHANGED_32) u.selected_weapon = static_cast<uint8_t>(q.args[7]);
    if (q.args[12] != ARG_UNCHANGED_32) u.move_group_id = q.args[12];

    u.path_blocked_retry_count = 0;
    u.order_queued             = 0;
    u.home_x                   = u.x; // snapshot the rally point from the current position
    u.home_y                   = u.y;

    // The unit's live state is compared against an UNNAMED per-prototype byte in the unit config
    // table (Unit[proto] + 0xec). What that byte means is not established, so it gets no invented
    // name here; the comparison is reproduced literally. The state field is 16-bit and the config
    // byte is zero-extended (MOVZX), so this is a widened compare, not a byte compare.
    const uint8_t proto_state =
        st.unit_types[static_cast<int32_t>(u.unit_proto_id) * UNIT_TYPE_STRIDE + UNIT_TYPE_STATE_BYTE];
    if (static_cast<uint32_t>(u.state) == static_cast<uint32_t>(proto_state))
        gc.unit_notify_status(static_cast<uint16_t>(player), unit_idx, 0);

    if (u.target_ref != 0) gc.target_release_ref(static_cast<uint16_t>(player), unit_idx, 0);

    *st.queue_count -= 1;
    mh::sim::rng_trace_add_note(13u, (uint32_t)q.order_code, (uint32_t)q.unit_index,
                                (uint32_t)q.owner_and_kind, (uint32_t)queue_idx,
                                (uint32_t)*st.queue_count, (uint32_t)player);
    for (int32_t j = queue_idx; j < *st.queue_count; ++j) copy_record(&st.queue[j], &st.queue[j + 1]);
}

// llm_strat_order_integrity_check @0x0046616e -- the verifier for stage_scheduled's byte-duplication
// encoding. For each of the four identity fields it compares the high byte against the low byte;
// a mismatch means a corrupted or hostile record, and IN MP ONLY it tears the session down.
//
// It writes no state of its own, which is why it has no useful shadow region set: its entire
// observable behaviour is whether it makes that call. (The original makes TWO -- the empty
// llm_teardown_hook_stub first, dropped at SIMABI-HOOKS; see order_queue.h.)
void integrity_check(const container_state &st, const game_calls &gc, const order *rec) {
    const uint16_t fields[4] = {rec->unit_index, rec->owner_and_kind,
                                static_cast<uint16_t>(rec->param0), rec->order_code};
    for (int i = 0; i < 4; ++i) {
        const uint8_t lo = static_cast<uint8_t>(fields[i] & 0xff);
        // SAR EAX,0x8 after MOVZX (0x004661de-0x004661e5) -- the value is zero-extended first, so the
        // arithmetic shift can never bring in sign bits. A plain >> 8 of the byte is equivalent.
        const uint8_t hi = static_cast<uint8_t>((fields[i] >> 8) & 0xff);
        if (hi == lo) continue;
        if (*st.session_mode != SESSION_MP_LOCKSTEP) continue;
        gc.force_return_to_main();
    }
}

} // namespace detail

// ---- the public entry points: the logic above, applied to the live game state -------------------
void    scratch_reset() { detail::scratch_reset(state()); }
void    scratch_set_field(int32_t i, int32_t v) { detail::scratch_set_field(state(), i, v); }
int32_t enqueue(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0, uint16_t order_code) {
    detail::g_ledger_site = _ReturnAddress(); // the count ledger's caller tag -- see g_ledger_site
    return detail::enqueue(state(), unit_index, owner_and_kind, param0, order_code);
}
int32_t pending_enqueue(order *rec) { return detail::pending_enqueue(state(), rec); }
int32_t stage_scheduled(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                        uint16_t order_code, double exec_time) {
    return detail::stage_scheduled(state(), live_calls(), proposal_tag(), unit_index, owner_and_kind,
                                   param0, order_code, exec_time);
}
int32_t schedule() { return detail::schedule(state(), live_calls()); }
int32_t release_due(double now) { return detail::release_due(state(), now); }

// ---- ST4: the module as the owner of its own state -------------------------------------------
//
// SCOPE IS THE SAVE FORMAT'S SCOPE, not the container's. QUEUE + QUEUE_COUNT are the two blocks
// Save/LoadPlanetFromDisk carry; PENDING, STAGING, the scratch args and the horizon are NOT in the
// file and are deliberately absent here. See the header for why load must not clear them.
//
// ORDER MATTERS AND IS THE FILE'S ORDER: SavePlanetToDisk writes the QUEUE first (step 1) and the
// COUNT second (step 2). Reversing them would still round-trip through this module and would still
// pass a self-consistency test, while producing a save the ORIGINAL cannot read -- so the ordering
// is pinned by the byte-identity A/B against the block path, not by inspection.
//
// WHY THE WHOLE ARRAY AND NOT `*st.queue_count` RECORDS: the original block-copies all 20400 bytes
// including the slots past the live count, so the file carries whatever stale records were there.
// Emitting only the live prefix would be tidier and would NOT be byte-identical.
//
// ST5: THROUGH THE CODEC, RECORD BY RECORD, not one flat block. Byte-identical -- orderstest pins
// that -- but it means the save and the wire now share one written-down layout instead of two
// independent memcpys that agree by accident. The flat block was the cheaper instruction sequence
// and the more expensive coupling.
namespace detail {
void emit(const container_state &st, mh::state::state_sink &s) {
    uint8_t rec[codec::RECORD_BYTES];
    for (int32_t i = 0; i < QUEUE_CAP; ++i) {
        codec::encode(st.queue[i], rec);
        s.bytes(rec, codec::RECORD_BYTES);
    }
    s.bytes(st.queue_count, (uint32_t)sizeof(int32_t));
}

void load_state(const container_state &st, mh::state::state_source &src) {
    uint8_t rec[codec::RECORD_BYTES];
    for (int32_t i = 0; i < QUEUE_CAP; ++i) {
        src.bytes(rec, codec::RECORD_BYTES);
        codec::decode(rec, st.queue[i]);
    }
    src.bytes(st.queue_count, (uint32_t)sizeof(int32_t));
    // PENDING / STAGING deliberately untouched -- see the header. This is the line whose ABSENCE of
    // a sibling is the fidelity requirement, so it is asserted by orderstest rather than trusted.
}
} // namespace detail

// Bound to the live container, like every other entry point here. The unbound detail:: forms above
// are what orderstest drives over heap buffers.
void emit(mh::state::state_sink &s) { detail::emit(state(), s); }
void load_state(mh::state::state_source &src) { detail::load_state(state(), src); }

// ---- the per-region face, and the ownership claim ---------------------------------------------
//
// TWO REGIONS, not one stream. QUEUE and QUEUE_COUNT are separate file blocks with separate headers,
// and separate entries in the determinism hash manifest, so the unit both consumers key on is the
// REGION. Emitting them as one 20404-byte stream would produce ONE block and would NOT be
// byte-identical. Anything else this module touches (PENDING, STAGING, the scratch args, the
// horizon) is deliberately NOT claimed: they are absent from the save format, and claiming a region
// means promising a canonical stream for it -- a promise worth making only where a consumer asks.
// HASH-INPUT BEGIN orders_emit_region (tools/data/hash_input_epoch.json)
void emit_region(mh::state::region_id rid, mh::state::state_sink &s) {
    const container_state &st = state();
    if (rid == mh::state::RID_STRAT_ORDER_QUEUE) {
        // THE DEAD SLOTS ARE local() (mp:D33 follow-up, user 2026-09-25). Only [0, count) is live --
        // the dispatcher reads nothing past it -- so the stale records above it are process history:
        // PERSIST still writes them (the original block-copies all 20400 bytes, so the save stays
        // byte-identical), VERDICT hashes them as zeros. That is the same byte stream the harness's
        // old per-step memset produced, so no recorded hash moves, and a peer running the harness no
        // longer reads as desynced against one that is not. CLAMPED: the count has been measured > 300.
        int32_t live = *st.queue_count;
        if (live < 0) live = 0;
        if (live > QUEUE_CAP) live = QUEUE_CAP;
        uint8_t rec[codec::RECORD_BYTES]; // ST5: one codec, shared with the wire
        for (int32_t i = 0; i < QUEUE_CAP; ++i) {
            codec::encode(st.queue[i], rec);
            if (i < live)
                s.bytes(rec, codec::RECORD_BYTES);
            else
                s.local(rec, codec::RECORD_BYTES);
        }
    } else if (rid == mh::state::RID_STRAT_ORDER_QUEUE_COUNT)
        s.bytes(st.queue_count, (uint32_t)sizeof(int32_t));
}
// HASH-INPUT END orders_emit_region

void load_region(mh::state::region_id rid, mh::state::state_source &src) {
    const container_state &st = state();
    if (rid == mh::state::RID_STRAT_ORDER_QUEUE) {
        uint8_t rec[codec::RECORD_BYTES]; // ST5: one codec, shared with the wire
        for (int32_t i = 0; i < QUEUE_CAP; ++i) {
            src.bytes(rec, codec::RECORD_BYTES);
            codec::decode(rec, st.queue[i]);
        }
    } else if (rid == mh::state::RID_STRAT_ORDER_QUEUE_COUNT)
        src.bytes(st.queue_count, (uint32_t)sizeof(int32_t));
}

int claim_regions() {
    static const mh::state::region_provider me = {&emit_region, &load_region, "mh::orders"};
    mh::state::claim(mh::state::RID_STRAT_ORDER_QUEUE, &me);
    mh::state::claim(mh::state::RID_STRAT_ORDER_QUEUE_COUNT, &me);
    return 2;
}

namespace {
// Claimed at load time rather than from an init seam, so there is no window in which a consumer
// could ask before the owner exists. Idempotent, so an explicit claim_regions() later is harmless.
const int g_claimed = claim_regions();
} // namespace
int32_t dispatch(uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg) {
    detail::g_ledger_site = _ReturnAddress(); // the count ledger's caller tag -- the router's own
                                              // immediate lane reaches detail::enqueue from here
    return detail::dispatch(state(), live_calls(), proposal_tag(), unit_id, player, op_code, arg);
}
int32_t queue_find_index(int32_t player, int32_t unit_idx, int32_t kind_tag) {
    return detail::queue_find_index(state(), player, unit_idx, kind_tag);
}
void queue_apply_and_dequeue(uint32_t player, int32_t unit_idx, int32_t queue_idx) {
    detail::queue_apply_and_dequeue(state(), live_calls(), player, unit_idx, queue_idx);
}
void integrity_check(const order *rec, char *) { detail::integrity_check(state(), live_calls(), rec); }

} // namespace mh::orders


// ---- PROMOTION (O3): install this module as the LIVE implementation ------------------------------
//
// The bodies below wrap the public entry points with a FIRST-CALL liveness line. That shape is not
// decoration: P0-EXPORT shipped a replacement whose counter only logged at 1000 calls, the real
// volume was lower, and a full 3000-step run produced no line at all -- indistinguishable from a
// replacement that was never installed, and silent in the SAFE-LOOKING direction. So log #1, then
// widen (1, 100, 1000, 10000): #1 proves reachability and no single guess about the rate can hide it.
namespace mh::orders::promoted {

long g_calls[9];
bool g_any_installed = false;

inline void live(int slot, const char *name) {
    const long n = ++g_calls[slot];
    if (n == 1 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        mh::orders::say("; [promote] %s: call #%ld (OURS is live)\n", name, n);
}

// clang-format off
void    scratch_reset()                        { live(0, "scratch_reset");     mh::orders::scratch_reset(); }
void    scratch_set_field(int32_t i, int32_t v) { live(1, "scratch_set_field"); mh::orders::scratch_set_field(i, v); }
// D18's suppression check USED TO SIT HERE. It moved to the sink (mh::orders::detail::enqueue) on
// 2026-09-11 -- see the long note there for why a gate on this wrapper covered only the game's own
// entry and silently lost coverage with every promotion. This wrapper now simply delegates, and a
// suppressed call still returns 0 because the sink returns 0.
int32_t enqueue(uint16_t u, uint16_t o, int16_t p, uint16_t c) { live(2, "enqueue");        return mh::orders::enqueue(u, o, p, c); }
int32_t pending_enqueue(order *rec)             { live(3, "pending_enqueue");  return mh::orders::pending_enqueue(rec); }
int32_t schedule()                              { live(4, "schedule");         return mh::orders::schedule(); }
int32_t release_due(double now)                 { live(5, "release_due");      return mh::orders::release_due(now); }
int32_t dispatch(uint16_t u, uint32_t pl, uint16_t op, uint16_t a) { live(6, "dispatch");   return mh::orders::dispatch(u, pl, op, a); }
int32_t queue_find_index(int32_t pl, int32_t ui, int32_t oc) { live(7, "queue_find_index"); return mh::orders::queue_find_index(pl, ui, oc); }
void    queue_apply_and_dequeue(uint32_t pl, int32_t ui, int32_t qi) { live(8, "queue_apply_and_dequeue"); mh::orders::queue_apply_and_dequeue(pl, ui, qi); }
// clang-format on

} // namespace mh::orders::promoted

// The NINE live seams. stage_scheduled and integrity_check are deliberately absent: they are reached
// only from inside the container, so with dispatch and enqueue ours they are plain C++ calls. Every
// seam is an ABI contract maintained forever (Law 4), so two we do not have to keep is two we do not.
MH_EXPORT_REPLACE(llm_strat_order_scratch_reset, mh::orders::promoted::scratch_reset)
MH_EXPORT_REPLACE(llm_strat_order_scratch_set_field, mh::orders::promoted::scratch_set_field)
MH_EXPORT_REPLACE(llm_strat_order_enqueue, mh::orders::promoted::enqueue)
MH_EXPORT_REPLACE(llm_strat_order_pending_enqueue, mh::orders::promoted::pending_enqueue)
MH_EXPORT_REPLACE(llm_strat_order_schedule, mh::orders::promoted::schedule)
MH_EXPORT_REPLACE(llm_strat_order_release_due, mh::orders::promoted::release_due)
MH_EXPORT_REPLACE(llm_strat_order_dispatch, mh::orders::promoted::dispatch)
MH_EXPORT_REPLACE(llm_strat_order_queue_find_index, mh::orders::promoted::queue_find_index)
MH_EXPORT_REPLACE(llm_strat_order_queue_apply_and_dequeue, mh::orders::promoted::queue_apply_and_dequeue)

namespace mh::orders {

bool promotion_active() { return promoted::g_any_installed; }

// C8-f: `default_on` is the SHIPPING DEFAULT for `[promote] orders`, PASSED IN rather than read
// from a header here. The value lives in mh/seams/net_internal.h (SHIP_PROMOTE_*) with the rest of
// the ship defaults, and this module must not include a seams header -- the layering lint exists to
// keep binary-bound seam code out of the reimplementation. So the seams layer supplies it at the one
// call site, which also keeps "what ships" answerable by reading one block instead of two.
// D18. Default false, and false is the faithful behaviour -- same contract as reimpl_fixes: a flag
// whose default changes what our body does would make every unflagged run a different function.
namespace {
bool g_suppress_enqueue = false;
} // namespace
void set_suppress_enqueue(bool on) { g_suppress_enqueue = on; }
bool suppress_enqueue() { return g_suppress_enqueue; }

int install_promotion(int default_on) {
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        bool (*install)();
    };
    const entry seams[] = {
        {"scratch_reset", mh_export_install_llm_strat_order_scratch_reset},
        {"scratch_set_field", mh_export_install_llm_strat_order_scratch_set_field},
        {"enqueue", mh_export_install_llm_strat_order_enqueue},
        {"pending_enqueue", mh_export_install_llm_strat_order_pending_enqueue},
        {"schedule", mh_export_install_llm_strat_order_schedule},
        {"release_due", mh_export_install_llm_strat_order_release_due},
        {"dispatch", mh_export_install_llm_strat_order_dispatch},
        {"queue_find_index", mh_export_install_llm_strat_order_queue_find_index},
        {"queue_apply_and_dequeue", mh_export_install_llm_strat_order_queue_apply_and_dequeue},
    };

    int ok = 0;
    for (const entry &e : seams) {
        if (e.install()) {
            ++ok;
        } else {
            // install_export_ok already logged WHY (entry-byte guard mismatch = the DLL was built
            // against a different image). Refusing loudly beats a half-promoted closure.
            say("; [promote] orders: seam %s REFUSED -- container is NOT promoted\n", e.name);
        }
    }
    if (ok != (int)(sizeof(seams) / sizeof(seams[0]))) {
        say("; [promote] orders: %d/%d seams installed -- PARTIAL, treat this run as invalid\n", ok,
            (int)(sizeof(seams) / sizeof(seams[0])));
    } else {
        say("; [promote] orders: ALL %d seams installed -- mh::orders is LIVE\n", ok);
    }
    promoted::g_any_installed = ok > 0;
    return ok;
}


} // namespace mh::orders
