//
// orders_selftest.cpp -- `net_selftest.exe orderstest`: the order container's logic over heap
// buffers, with no game and no rig.
//
// WHY THIS EXISTS. Shadow mode proves equivalence only on paths a run actually reaches, and this
// container has paths no available scenario reaches at all:
//   * The IMMEDIATE lane (`enqueue`) is unreachable in a 2-peer lockstep run by construction --
//     order_dispatch routes every network-controlled player to the scheduled lane, and in lockstep
//     that includes the AI. A solo scenario reaches it exactly once.
//   * NEITHER enqueue's OVERFLOW branch has ever executed anywhere. Reaching them means pushing 300
//     (or 1000) orders into one queue inside a real match.
// Those are precisely the branches where a "sensible" reimplementation diverges -- the original
// EMPTIES the whole queue on overflow rather than rejecting the new order. So the cheap test is not
// a substitute for the oracle, it is the only thing that covers what the oracle cannot see.
//
// It works because mh::orders::detail::* takes the container state as a parameter; the live entry
// points are those functions applied to state(). Same code, different buffers.
//
#include "orders/order_queue.h"

#include "lockstep/tx_emit_order.h" // ST5: the WIRE half of the one-codec claim
#include "orders/order_codec.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

int g_checks, g_fails;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

using mh::orders::container_state;
using mh::orders::game_calls;
using mh::orders::order;
using profile = mh::game::mh_llm_strat_player_profile;
using unit    = mh::game::mh_map_object_unit;

// The outward calls, recorded. game_calls holds plain function pointers (it has to: in production
// they are the generated thunks), so the recorder is a file-scope singleton rather than a capture.
struct call_log {
    int                checks     = 0; // integrity_check calls
    char              *last_tag   = nullptr;
    int                commits    = 0; // commit_horizon calls
    int                flushes    = 0; // send_buf_flush calls
    int32_t            flush_each = 0; // what each flush reports back
    std::vector<order> sent;           // every record handed to send_order, as it was handed over
    // ---- batch B ----
    int                  set_param  = 0;
    int16_t              last_param = 0;
    std::vector<int32_t> release_modes; // the `mode` argument of each target_release_ref call
    int                  notifies = 0;
    int                  returns  = 0; // force_return_to_main calls
    void                 reset() { *this = call_log{}; }
};
call_log g_log;

const game_calls &recording_calls() {
    static const game_calls gc = {
        [](const order *, char *tag) {
            ++g_log.checks;
            g_log.last_tag = tag;
        },
        []() { ++g_log.commits; },
        [](const order *rec) -> int32_t {
            g_log.sent.push_back(*rec);
            return 0;
        },
        []() -> int32_t {
            ++g_log.flushes;
            return g_log.flush_each;
        },
        [](int32_t, int32_t, int16_t param) {
            ++g_log.set_param;
            g_log.last_param = param;
        },
        [](uint32_t, int32_t, uint32_t mode) {
            g_log.release_modes.push_back(static_cast<int32_t>(mode));
        },
        [](uint32_t, int32_t, uint32_t) { ++g_log.notifies; },
        []() { ++g_log.returns; },
    };
    return gc;
}

// Every array is allocated with SLACK past its cap. A reimplementation that lost a bound check
// would otherwise run off the end of the vector and take the process down with a heap corruption --
// and a crashed selftest prints nothing, which reads exactly like a pass to anything scraping its
// output. With slack, the same bug lands in valid memory and fails the count assertion by name.
// (Learned while mutation-testing this file: removing release_due's queue-cap check killed the exe
// at exit code 0xC0000374 instead of failing a check.)
inline constexpr int SLACK = 8;

struct fixture {
    std::vector<order>   queue{mh::orders::QUEUE_CAP + SLACK};
    std::vector<order>   pending{mh::orders::PENDING_CAP + SLACK};
    std::vector<order>   staging{mh::orders::STAGING_CAP + SLACK};
    std::vector<profile> players{mh::orders::MAX_PLAYERS};
    int32_t              queue_count   = 0;
    int32_t              pending_count = 0;
    int32_t              staging_count = 0;
    int32_t              scratch[mh::orders::ARG_SLOTS]{};
    double               horizon     = 0.0;
    int32_t              send_cursor = 0;
    // ---- batch B state ----
    int32_t              session_mode = 0;
    std::vector<uint8_t> net_players =
        std::vector<uint8_t>(static_cast<size_t>(mh::orders::MAX_PLAYERS) * mh::orders::PLAYERS_STRIDE);
    double            game_clock = 0.0;
    double            step_size  = 0.0;
    std::vector<unit> units =
        std::vector<unit>(static_cast<size_t>(mh::orders::MAX_PLAYERS) * mh::orders::UNITS_PER_PLAYER);
    std::vector<uint8_t> unit_types = // Unit[100] config prototypes
        std::vector<uint8_t>(static_cast<size_t>(100) * mh::orders::UNIT_TYPE_STRIDE);
    container_state st{};
    fixture() {
        std::memset(queue.data(), 0xAA, queue.size() * sizeof(order));
        std::memset(pending.data(), 0xAA, pending.size() * sizeof(order));
        std::memset(staging.data(), 0xAA, staging.size() * sizeof(order));
        std::memset(players.data(), 0, players.size() * sizeof(profile));
        std::memset(units.data(), 0, units.size() * sizeof(unit));
        for (auto &p : players) p.status_flags = mh::orders::PLAYER_ALIVE;
        std::memset(unit_types.data(), 0, unit_types.size());
        st = {queue.data(), &queue_count, pending.data(), &pending_count, scratch,
              staging.data(), &staging_count, &horizon,
              // bytes stays null on purpose: schedule() only ever READS the cursor, because the
              // bytes are appended by the original send_order() this fixture stubs out.
              mh::net::packet_buffer{nullptr, &send_cursor}, players.data(),
              &session_mode, net_players.data(), &game_clock, &step_size, units.data(),
              unit_types.data()};
        g_log.reset();
    }
    // The router's "this player is network-controlled" bit, in the SECOND player table.
    void set_net_controlled(int player, bool on) {
        uint8_t &b = net_players[player * mh::orders::PLAYERS_STRIDE + mh::orders::PLAYERS_FLAG_OFF];
        b          = on ? static_cast<uint8_t>(b | mh::orders::NET_CONTROLLED)
                        : static_cast<uint8_t>(b & ~mh::orders::NET_CONTROLLED);
    }
    unit &unit_at(int player, int idx) {
        return units[player * mh::orders::UNITS_PER_PLAYER + idx];
    }
};

// An order with every field distinct, so a mis-copied one is obvious in a failure.
order make_order(double t, uint16_t unit, uint16_t owner, int16_t p0, uint16_t code) {
    order o{};
    o.exec_time      = t;
    o.unit_index     = unit;
    o.owner_and_kind = owner;
    o.param0         = p0;
    o.order_code     = code;
    for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) o.args[i] = 900 + i;
    return o;
}

} // namespace

int run_orderstest() {
    printf("=== orderstest (order container logic, no game) ===\n");
    namespace od = mh::orders::detail;

    // ---- scratch_reset: every slot to the sentinel ----
    {
        fixture f;
        std::memset(f.scratch, 0x5A, sizeof(f.scratch));
        od::scratch_reset(f.st);
        bool all = true;
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) all &= (f.scratch[i] == mh::orders::ARG_UNCHANGED);
        check("scratch_reset sets all 13 slots to -1", all);
    }

    // ---- scratch_set_field: one slot, no bounds check, neighbours untouched ----
    {
        fixture f;
        od::scratch_reset(f.st);
        od::scratch_set_field(f.st, 5, 0x1234);
        check("scratch_set_field writes the addressed slot", f.scratch[5] == 0x1234);
        check("scratch_set_field leaves neighbours alone",
              f.scratch[4] == -1 && f.scratch[6] == -1);
    }

    // ---- enqueue: the normal path ----
    {
        fixture f;
        od::scratch_reset(f.st);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) od::scratch_set_field(f.st, i, 1000 + i);
        // exec_time must survive untouched: the immediate lane never writes it (see the .cpp).
        std::memset(&f.queue[0], 0xAA, sizeof(order));
        const int32_t r = od::enqueue(f.st, 0x1111, 0x2222, 0x3333, 0x4444);

        check("enqueue returns the NEW count (1-based), not the index", r == 1);
        check("enqueue advances the count", f.queue_count == 1);
        check("enqueue stores unit_index", f.queue[0].unit_index == 0x1111);
        check("enqueue stores owner_and_kind", f.queue[0].owner_and_kind == 0x2222);
        check("enqueue stores param0", f.queue[0].param0 == 0x3333);
        check("enqueue stores order_code", f.queue[0].order_code == 0x4444);
        bool args = true;
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) args &= (f.queue[0].args[i] == 1000 + i);
        check("enqueue copies all 13 scratch slots into args[]", args);

        // THE ONE A HELPFUL REIMPLEMENTATION GETS WRONG: exec_time is left stale on purpose.
        uint64_t bits;
        std::memcpy(&bits, &f.queue[0].exec_time, sizeof(bits));
        check("enqueue does NOT write exec_time (stale by design)", bits == 0xAAAAAAAAAAAAAAAAull);
    }

    // ---- enqueue: the OVERFLOW branch -- unreachable in any rig scenario ----
    {
        fixture f;
        od::scratch_reset(f.st);
        f.queue_count   = mh::orders::QUEUE_CAP; // 300
        const int32_t r = od::enqueue(f.st, 1, 2, 3, 4);
        check("enqueue at the cap returns 0", r == 0);
        check("enqueue at the cap EMPTIES the whole queue (does not just reject)", f.queue_count == 0);

        f.queue_count = mh::orders::QUEUE_CAP - 1; // 299 -- the last accepting slot
        check("enqueue at cap-1 still accepts", od::enqueue(f.st, 1, 2, 3, 4) == mh::orders::QUEUE_CAP);
    }

    // ---- pending_enqueue: the low-byte masking, in place, before the copy ----
    {
        fixture f;
        order   in{};
        in.exec_time      = 1234.5;
        in.unit_index     = 0x1234;
        in.owner_and_kind = 0x5678;
        in.param0         = 0x0ABC;
        in.order_code     = 0x0DEF;
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) in.args[i] = 7000 + i;

        const int32_t r = od::pending_enqueue(f.st, &in);
        check("pending_enqueue returns 1 on success", r == 1);
        check("pending_enqueue advances the count", f.pending_count == 1);
        check("pending_enqueue masks unit_index to its low byte", f.pending[0].unit_index == 0x34);
        check("pending_enqueue masks owner_and_kind", f.pending[0].owner_and_kind == 0x78);
        check("pending_enqueue masks param0", f.pending[0].param0 == 0xBC);
        check("pending_enqueue masks order_code", f.pending[0].order_code == 0xEF);
        check("pending_enqueue MUTATES THE CALLER'S RECORD in place", in.unit_index == 0x34);
        check("pending_enqueue copies exec_time through", f.pending[0].exec_time == 1234.5);
        bool args = true;
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) args &= (f.pending[0].args[i] == 7000 + i);
        check("pending_enqueue copies all 68 bytes (args intact)", args);
    }

    // ---- pending_enqueue: its own overflow branch ----
    {
        fixture f;
        order   in{};
        f.pending_count = mh::orders::PENDING_CAP; // 1000
        check("pending_enqueue at the cap returns 0", od::pending_enqueue(f.st, &in) == 0);
        check("pending_enqueue at the cap EMPTIES pending", f.pending_count == 0);
    }

    // ================= the SCHEDULE lane (O2 batch A) =================

    // ---- stage_scheduled: THE BYTE DUPLICATION. Not the low-byte MASK its siblings apply -- the
    // low byte is written into BOTH halves, and llm_strat_order_integrity_check later verifies
    // exactly that. A translator that reused pending_enqueue's `& 0xff` would fail that check and
    // (in SESSION_MODE 3) drop the player back to the main menu.
    {
        fixture f;
        od::scratch_reset(f.st);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) od::scratch_set_field(f.st, i, 500 + i);

        char          tag[] = "tag";
        const int32_t r     = od::stage_scheduled(f.st, recording_calls(), tag, 0x1234, 0x5678, 0x0ABC,
                                                  0x0DEF, 4242.5);

        check("stage_scheduled returns the NEW count", r == 1);
        check("stage_scheduled advances the count", f.staging_count == 1);
        check("stage_scheduled DUPLICATES the low byte of unit_index", f.staging[0].unit_index == 0x3434);
        check("stage_scheduled DUPLICATES the low byte of owner_and_kind",
              f.staging[0].owner_and_kind == 0x7878);
        check("stage_scheduled DUPLICATES the low byte of param0",
              static_cast<uint16_t>(f.staging[0].param0) == 0xBCBC);
        check("stage_scheduled DUPLICATES the low byte of order_code", f.staging[0].order_code == 0xEFEF);
        check("stage_scheduled stores exec_time", f.staging[0].exec_time == 4242.5);
        bool sargs = true;
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) sargs &= (f.staging[0].args[i] == 500 + i);
        check("stage_scheduled copies the 13 scratch slots into args[]", sargs);
        check("stage_scheduled runs the integrity check exactly once", g_log.checks == 1);
        check("stage_scheduled passes the tag through", g_log.last_tag == tag);
    }

    // ---- stage_scheduled: a duplicated byte is what a SIGN-EXTENDING translation gets wrong.
    // 0x80 must become 0x8080, not 0xFF80.
    {
        fixture f;
        char    tag[] = "t";
        od::scratch_reset(f.st);
        od::stage_scheduled(f.st, recording_calls(), tag, 0x0080, 0x0080, static_cast<int16_t>(-128),
                            0x0080, 1.0);
        check("stage_scheduled duplicates 0x80 as 0x8080 (no sign extension)",
              f.staging[0].unit_index == 0x8080 &&
                  static_cast<uint16_t>(f.staging[0].param0) == 0x8080);
    }

    // ---- stage_scheduled OVERFLOW: it REJECTS. Its two sibling enqueues EMPTY their array; copying
    // that behaviour here would silently discard 300 staged orders mid-match.
    {
        fixture f;
        char    tag[] = "t";
        od::scratch_reset(f.st);
        f.staging_count = mh::orders::STAGING_CAP;
        const int32_t r = od::stage_scheduled(f.st, recording_calls(), tag, 1, 2, 3, 4, 1.0);
        check("stage_scheduled at the cap returns 0", r == 0);
        check("stage_scheduled at the cap KEEPS the array (does NOT empty it, unlike enqueue)",
              f.staging_count == mh::orders::STAGING_CAP);
        check("stage_scheduled at the cap does not run the integrity check", g_log.checks == 0);
    }

    // ---- schedule: the horizon block leaves exec_time == horizon in every case ----
    {
        fixture f; // order is PAST the horizon -> the horizon is pulled forward and committed
        f.horizon       = 100.0;
        f.staging[0]    = make_order(150.0, 1, 0, 0, 0);
        f.staging_count = 1;
        od::schedule(f.st, recording_calls());
        check("schedule advances the horizon to a later order", f.horizon == 150.0);
        check("schedule commits the advanced horizon to the peers", g_log.commits == 1);
        check("schedule leaves the order's own time alone when it led", f.pending[0].exec_time == 150.0);
    }
    {
        fixture f; // order is BEHIND the horizon -> the order is clamped up, no commit
        f.horizon       = 100.0;
        f.staging[0]    = make_order(50.0, 1, 0, 0, 0);
        f.staging_count = 1;
        od::schedule(f.st, recording_calls());
        check("schedule clamps an early order UP to the horizon", f.pending[0].exec_time == 100.0);
        check("schedule leaves the horizon alone when clamping", f.horizon == 100.0);
        check("schedule does NOT commit when it only clamped", g_log.commits == 0);
    }

    // ---- schedule: the wire gets the DUPLICATED form, PENDING gets the MASKED one ----
    {
        fixture f;
        f.horizon       = 0.0;
        f.staging[0]    = make_order(10.0, 0x3434, 0x7878, static_cast<int16_t>(0xBCBC), 0xEFEF);
        f.staging_count = 1;
        od::schedule(f.st, recording_calls());

        check("schedule masks unit_index into PENDING", f.pending[0].unit_index == 0x34);
        check("schedule masks owner_and_kind into PENDING", f.pending[0].owner_and_kind == 0x78);
        check("schedule masks param0 into PENDING", f.pending[0].param0 == 0xBC);
        check("schedule masks order_code into PENDING", f.pending[0].order_code == 0xEF);
        check("schedule advances the pending count", f.pending_count == 1);
        check("schedule sends exactly one order", g_log.sent.size() == 1);
        check("schedule sends the UNMASKED (duplicated) record -- masking it would fail the peer's "
              "integrity check",
              g_log.sent.size() == 1 && g_log.sent[0].unit_index == 0x3434 &&
                  g_log.sent[0].order_code == 0xEFEF);
        check("schedule empties staging after a full drain", f.staging_count == 0);
    }

    // ---- schedule: PENDING full -> the order is KEPT staged, not dropped and not sent ----
    {
        fixture f;
        f.pending_count = mh::orders::PENDING_CAP;
        f.staging[0]    = make_order(10.0, 0x11, 0x22, 0x33, 0x44);
        f.staging[1]    = make_order(20.0, 0x55, 0x66, 0x77, 0x88);
        f.staging_count = 2;
        od::schedule(f.st, recording_calls());
        check("schedule keeps both orders staged when pending is full", f.staging_count == 2);
        check("schedule compacts the kept orders in order", f.staging[0].unit_index == 0x11 &&
                                                                f.staging[1].unit_index == 0x55);
        check("schedule sends nothing when pending is full", g_log.sent.empty());
        check("schedule does not touch the pending count when full",
              f.pending_count == mh::orders::PENDING_CAP);
    }

    {
        fixture f;
        f.staging[0]      = make_order(10.0, 1, 0, 0, 0);
        f.staging_count   = 1;
        f.send_cursor     = 0x3f8 - 0x45; // exactly at the high-water mark -> flush inside the loop
        g_log.flush_each  = 7;
        const int32_t got = od::schedule(f.st, recording_calls());
        check("schedule flushes when one more order would not fit", g_log.flushes >= 1);
        check("schedule returns the bytes the flushes reported", got == 7 * g_log.flushes);
    }
    {
        fixture f; // nothing staged, but the buffer is dirty -> exactly one trailing flush
        f.send_cursor     = 1;
        g_log.flush_each  = 3;
        const int32_t got = od::schedule(f.st, recording_calls());
        check("schedule flushes a dirty buffer even with nothing to drain", g_log.flushes == 1);
        check("schedule returns that flush's byte count", got == 3);
    }
    {
        fixture f; // clean buffer, nothing staged -> no flush at all
        f.send_cursor = 0;
        check("schedule with a clean buffer and nothing staged returns 0",
              od::schedule(f.st, recording_calls()) == 0);
        check("schedule does not flush a clean buffer", g_log.flushes == 0);
    }

    // ---- release_due: only LIVE players are pumped ----
    {
        fixture f;
        f.players[0].status_flags = 0; // player 0 dead
        f.pending[0]              = make_order(10.0, 5, 0, 1, 2);
        f.pending_count           = 1;
        const int32_t r           = od::release_due(f.st, 100.0);
        check("release_due does not release a dead player's orders", f.queue_count == 0);
        check("release_due keeps them pending", f.pending_count == 1);
        check("release_due returns 0 while pending is non-empty", r == 0);
    }

    // ---- release_due: the three ways an order stays pending ----
    {
        fixture f;
        f.pending[0]              = make_order(500.0, 5, 0, 1, 2); // not due yet
        f.pending[1]              = make_order(10.0, 6, 3, 1, 2);  // owner 3, and only player 0..7 alive check
        f.pending_count           = 2;
        f.players[3].status_flags = 0; // player 3 dead, so its order is never even considered
        od::release_due(f.st, 100.0);
        check("release_due keeps an order whose time has not come", f.pending_count == 2);
        check("release_due queues nothing", f.queue_count == 0);
    }
    {
        fixture f;
        f.queue_count   = mh::orders::QUEUE_CAP; // queue full
        f.pending[0]    = make_order(10.0, 5, 0, 1, 2);
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due keeps an order when the queue is full", f.pending_count == 1);
        check("release_due does not overflow the queue", f.queue_count == mh::orders::QUEUE_CAP);
    }

    // ---- release_due: an UNORDERED (NaN) exec_time releases, because JBE does ----
    // The original compares with FCOMP/FNSTSW/SAHF/JBE, and JBE is taken for less, equal AND
    // unordered. A C++ `exec_time <= now` is false on a NaN and would strand the record in PENDING
    // forever. Unreachable in any rig scenario -- a NaN has to arrive off the wire or out of
    // corrupted state -- so this assertion is the only thing standing between us and that bug.
    {
        fixture      f;
        const double nan_t = std::numeric_limits<double>::quiet_NaN();
        f.pending[0]       = make_order(nan_t, 5, 0, 1, 2);
        f.pending_count    = 1;
        od::release_due(f.st, 100.0);
        check("release_due releases a NaN-timed order (JBE takes the unordered case)",
              f.queue_count == 1);
        check("release_due does not strand the NaN-timed order in pending", f.pending_count == 0);
    }

    // ---- release_due: the ordinary release ----
    {
        fixture f;
        f.pending[0]    = make_order(10.0, 5, 0, 1, 2);
        f.pending_count = 1;
        const int32_t r = od::release_due(f.st, 100.0);
        check("release_due moves a due order into the queue", f.queue_count == 1);
        check("release_due copies the whole record", f.queue[0].unit_index == 5 &&
                                                         f.queue[0].order_code == 2 &&
                                                         f.queue[0].args[0] == 900);
        check("release_due CONSUMES the released order", f.pending_count == 0);
        check("release_due returns 1 once pending is empty", r == 1);
    }

    // ---- release_due dedup: a BYTE-EQUAL order is NOT deduplicated. The scan skips it without
    // counting a match, so it is appended -- the same order can legitimately sit in the queue twice.
    // This is the opposite of what "dedup" suggests, and it is what the original does (0x00466672
    // jumps to the loop tail, past the dup_found=1 at 0x004666fd).
    {
        fixture f;
        f.queue[0]      = make_order(10.0, 5, 0, 1, 2);
        f.queue_count   = 1;
        f.pending[0]    = make_order(20.0, 5, 0, 1, 2); // same unit+owner+param0+order_code
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due APPENDS a byte-equal order rather than collapsing it", f.queue_count == 2);
        check("release_due leaves the existing entry untouched", f.queue[0].exec_time == 10.0);
        check("release_due consumes the incoming one from pending", f.pending_count == 0);
    }

    // ---- release_due dedup: a DIFFERENT order for the same unit REPLACES the weaker one ----
    {
        fixture f;
        f.queue[0]      = make_order(10.0, 5, 0, 1, 2); // earlier -> weaker
        f.queue_count   = 1;
        f.pending[0]    = make_order(20.0, 5, 0, 1, 3); // different order_code
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due replaces an earlier order for the same unit", f.queue_count == 1 &&
                                                                             f.queue[0].order_code == 3);
    }
    {
        fixture f;
        f.queue[0]      = make_order(50.0, 5, 0, 1, 2); // LATER -> stronger, must survive
        f.queue_count   = 1;
        f.pending[0]    = make_order(20.0, 5, 0, 1, 3);
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due keeps a LATER queued order and drops the incoming one",
              f.queue_count == 1 && f.queue[0].order_code == 2 && f.queue[0].exec_time == 50.0);
        check("release_due consumes the loser rather than re-queuing it", f.pending_count == 0);
    }
    {
        fixture f; // SAME instant -> the higher order_code wins
        f.queue[0]      = make_order(20.0, 5, 0, 1, 2);
        f.queue_count   = 1;
        f.pending[0]    = make_order(20.0, 5, 0, 1, 9);
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due breaks an exec_time tie on the higher order_code",
              f.queue[0].order_code == 9);
    }
    {
        fixture f; // same instant, LOWER incoming order_code -> the queued one stands
        f.queue[0]      = make_order(20.0, 5, 0, 1, 9);
        f.queue_count   = 1;
        f.pending[0]    = make_order(20.0, 5, 0, 1, 2);
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due leaves the queued entry when the tie-break says so",
              f.queue[0].order_code == 9);
    }

    // ---- release_due: a different unit is not a duplicate ----
    {
        fixture f;
        f.queue[0]      = make_order(10.0, 5, 0, 1, 2);
        f.queue_count   = 1;
        f.pending[0]    = make_order(20.0, 6, 0, 1, 2); // different unit
        f.pending_count = 1;
        od::release_due(f.st, 100.0);
        check("release_due queues an order for a different unit alongside", f.queue_count == 2);
    }

    // ---- release_due: mixed batch -- released orders are consumed, the rest compacted down ----
    {
        fixture f;
        f.pending[0]    = make_order(10.0, 1, 0, 1, 2);  // due
        f.pending[1]    = make_order(900.0, 2, 0, 1, 2); // not due -> kept
        f.pending[2]    = make_order(20.0, 3, 0, 1, 2);  // due
        f.pending_count = 3;
        const int32_t r = od::release_due(f.st, 100.0);
        check("release_due releases only the due orders", f.queue_count == 2);
        check("release_due compacts the survivors to the front", f.pending_count == 1 &&
                                                                     f.pending[0].unit_index == 2);
        check("release_due returns 0 while something is still pending", r == 0);
    }

    // ================= the ROUTER and the CANCEL path (O2 batch B) =================

    // ---- dispatch: the lane choice, and the argument RENAMING across the call ----
    // dispatch(unit_id, player, op_code, arg) becomes a record of
    // (unit_index, owner_and_kind, param0, order_code) -- the names differ on the two sides.
    {
        fixture f; // not MP -> immediate lane regardless of the controller bit
        f.session_mode = 0;
        f.set_net_controlled(0, true);
        char tag[] = "t";
        od::scratch_reset(f.st);
        const int32_t r = od::dispatch(f.st, recording_calls(), tag, 0x11, 0, 0x22, 0x33);
        check("dispatch outside MP takes the IMMEDIATE lane", f.queue_count == 1 && f.staging_count == 0);
        check("dispatch returns the immediate lane's result", r == 1);
        check("dispatch maps unit_id -> unit_index", f.queue[0].unit_index == 0x11);
        check("dispatch maps op_code -> param0", f.queue[0].param0 == 0x22);
        check("dispatch maps arg -> order_code", f.queue[0].order_code == 0x33);
    }
    {
        fixture f; // MP, but this player is NOT network-controlled -> still immediate
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        f.set_net_controlled(0, false);
        char tag[] = "t";
        od::scratch_reset(f.st);
        od::dispatch(f.st, recording_calls(), tag, 0x11, 0, 0x22, 0x33);
        check("dispatch in MP for a LOCAL player takes the immediate lane",
              f.queue_count == 1 && f.staging_count == 0);
    }
    {
        fixture f; // MP + network-controlled -> scheduled lane, exec_time = clock + step
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        f.set_net_controlled(2, true);
        f.game_clock = 100.0;
        f.step_size  = 0.25;
        f.horizon    = 0.0;
        char tag[]   = "t";
        od::scratch_reset(f.st);
        od::dispatch(f.st, recording_calls(), tag, 0x11, 2, 0x22, 0x33);
        check("dispatch in MP for a NET player takes the SCHEDULED lane",
              f.staging_count == 1 && f.queue_count == 0);
        check("dispatch schedules at clock + step", f.staging[0].exec_time == 100.25);
        check("dispatch routes through stage_scheduled (byte-duplicated fields)",
              f.staging[0].unit_index == 0x1111 && f.staging[0].owner_and_kind == 0x0202);
        // The renaming has to be asserted on BOTH lanes: mutation-testing showed that checking it
        // only on the immediate lane leaves a swap in the SCHEDULED call completely undetected.
        check("dispatch maps op_code -> param0 on the SCHEDULED lane too",
              static_cast<uint16_t>(f.staging[0].param0) == 0x2222);
        check("dispatch maps arg -> order_code on the SCHEDULED lane too",
              f.staging[0].order_code == 0x3333);
    }
    {
        fixture f; // a NONZERO player must reach its own flag byte -- catches a lost PLAYERS_STRIDE
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        f.set_net_controlled(5, true);
        char tag[] = "t";
        od::scratch_reset(f.st);
        od::dispatch(f.st, recording_calls(), tag, 1, 5, 2, 3);
        check("dispatch finds a nonzero player's flag at player*PLAYERS_STRIDE",
              f.staging_count == 1 && f.queue_count == 0);
    }
    {
        fixture f; // clock + step is BEHIND the horizon -> clamped up to it
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        f.set_net_controlled(0, true);
        f.game_clock = 10.0;
        f.step_size  = 0.5;
        f.horizon    = 500.0;
        char tag[]   = "t";
        od::scratch_reset(f.st);
        od::dispatch(f.st, recording_calls(), tag, 1, 0, 2, 3);
        check("dispatch clamps an early schedule up to the lockstep horizon",
              f.staging[0].exec_time == 500.0);
    }
    {
        fixture f; // the router reads the SECOND player table; a neighbour's bit must not leak in
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        f.set_net_controlled(1, true); // player 1 only
        char tag[] = "t";
        od::scratch_reset(f.st);
        od::dispatch(f.st, recording_calls(), tag, 1, 0, 2, 3);
        check("dispatch indexes the player table by PLAYERS_STRIDE (no neighbour bleed)",
              f.queue_count == 1 && f.staging_count == 0);
    }

    // ---- queue_find_index: owner nibble, kind nibble, unit index ----
    {
        fixture f;
        f.queue[0]    = make_order(1.0, 7, 0x41, 0, 0); // owner 1, kind 0x40
        f.queue[1]    = make_order(1.0, 9, 0x42, 0, 0); // owner 2, kind 0x40
        f.queue_count = 2;
        check("find_index finds the matching entry", od::queue_find_index(f.st, 2, 9, 0x40) == 1);
        check("find_index returns -1 when nothing matches",
              od::queue_find_index(f.st, 3, 9, 0x40) == -1);
        check("find_index rejects a KIND mismatch", od::queue_find_index(f.st, 2, 9, 0x80) == -1);
        // The kind test must use the HIGH nibble. Asking for kind 0x02 against an entry whose
        // owner_and_kind is 0x42 must MISS -- under the owner mask (& 0xf) it would MATCH, and the
        // 0x80 case above cannot tell the two masks apart because it misses under both.
        check("find_index's kind test uses the HIGH nibble, not the owner mask",
              od::queue_find_index(f.st, 2, 9, 0x02) == -1);
        check("find_index rejects a UNIT mismatch", od::queue_find_index(f.st, 2, 8, 0x40) == -1);
        f.queue_count = 0;
        check("find_index on an empty queue returns -1", od::queue_find_index(f.st, 2, 9, 0x40) == -1);
    }

    // ---- apply_and_dequeue: the args table, the sentinel, and the queue compaction ----
    {
        fixture f;
        f.queue[0] = make_order(1.0, 5, 0x40, 0x77, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) f.queue[0].args[i] = 10 + i;
        f.queue[1]    = make_order(2.0, 6, 0x40, 0, 0); // must survive and shift down
        f.queue_count = 2;

        unit &u = f.unit_at(3, 4);
        u.x     = 41;
        u.y     = 42;

        od::queue_apply_and_dequeue(f.st, recording_calls(), 3, 4, 0);

        check("apply passes param0 to unit_set_order_param",
              g_log.set_param == 1 && g_log.last_param == 0x77);
        check("apply writes args[0]/[1] -> goal_x/goal_y", u.goal_x == 10 && u.goal_y == 11);
        check("apply writes args[2] -> home_storage_slot", u.home_storage_slot == 12);
        check("apply writes args[3]/[4] -> target_fine_x/y",
              u.target_fine_x == 13 && u.target_fine_y == 14);
        check("apply writes args[5] -> target_ref", u.target_ref == 15);
        check("apply writes args[6] -> target_index", u.target_index == 16);
        check("apply writes args[7] -> selected_weapon", u.selected_weapon == 17);
        check("apply writes args[12] -> move_group_id", u.move_group_id == 22);
        check("apply snapshots home_x/home_y from the unit's position", u.home_x == 41 && u.home_y == 42);
        check("apply clears path_blocked_retry_count and order_queued",
              u.path_blocked_retry_count == 0 && u.order_queued == 0);
        check("apply decrements the queue count", f.queue_count == 1);
        check("apply compacts the survivor down", f.queue[0].unit_index == 6);
    }
    {
        fixture f; // the -1 sentinel must LEAVE A FIELD ALONE
        f.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) f.queue[0].args[i] = -1;
        f.queue_count   = 1;
        unit &u         = f.unit_at(0, 0);
        u.goal_x        = 99;
        u.move_group_id = 1234;
        od::queue_apply_and_dequeue(f.st, recording_calls(), 0, 0, 0);
        check("apply leaves a field untouched when its arg is the -1 sentinel",
              u.goal_x == 99 && u.move_group_id == 1234);
    }
    {
        // THE PAIRED GUARD: args[6] (target_index) is written under a test on args[5], not its own
        // (0x00469bf9 re-tests the args[5] slot). target_ref is the "is there a target"
        // discriminator, so the pair moves together -- a reimplementation that gave args[6] its own
        // guard would write target_index when the original does not.
        fixture f;
        f.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) f.queue[0].args[i] = -1;
        f.queue[0].args[6] = 77; // a real index, but its PAIR-MATE args[5] is the sentinel
        f.queue_count      = 1;
        unit &u            = f.unit_at(0, 0);
        u.target_index     = 5;
        od::queue_apply_and_dequeue(f.st, recording_calls(), 0, 0, 0);
        check("apply does NOT write target_index when args[5] is the sentinel (paired guard)",
              u.target_index == 5);

        fixture g;
        g.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) g.queue[0].args[i] = -1;
        g.queue[0].args[5] = 3;  // the guard slot only...
        g.queue[0].args[6] = 77; // ...lets THIS one through
        g.queue_count      = 1;
        od::queue_apply_and_dequeue(g.st, recording_calls(), 0, 0, 0);
        check("apply DOES write target_index once args[5] is set", g.unit_at(0, 0).target_index == 77);
    }
    {
        fixture f; // an existing target is released up front (mode 1) and the pair cleared
        f.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) f.queue[0].args[i] = -1;
        f.queue_count  = 1;
        unit &u        = f.unit_at(0, 0);
        u.target_ref   = 9;
        u.target_index = 8;
        od::queue_apply_and_dequeue(f.st, recording_calls(), 0, 0, 0);
        check("apply releases an existing target with mode 1",
              g_log.release_modes.size() == 1 && g_log.release_modes[0] == 1);
        check("apply clears the target pair after releasing", u.target_ref == 0 && u.target_index == 0);
    }
    {
        fixture f; // a NEWLY installed target is released again at the end, with mode 0
        f.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) f.queue[0].args[i] = -1;
        f.queue[0].args[5] = 4; // installs target_ref
        f.queue_count      = 1;
        od::queue_apply_and_dequeue(f.st, recording_calls(), 0, 0, 0);
        check("apply releases a newly-installed target with mode 0",
              g_log.release_modes.size() == 1 && g_log.release_modes[0] == 0);
    }
    {
        fixture f; // the status notify fires only when the live state equals the config byte
        f.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) f.queue[0].args[i] = -1;
        f.queue_count                                                                     = 1;
        unit &u                                                                           = f.unit_at(0, 0);
        u.unit_proto_id                                                                   = 2;
        u.state                                                                           = 0x5;
        f.unit_types[2 * mh::orders::UNIT_TYPE_STRIDE + mh::orders::UNIT_TYPE_STATE_BYTE] = 0x5;
        od::queue_apply_and_dequeue(f.st, recording_calls(), 0, 0, 0);
        check("apply notifies status when state matches the prototype byte", g_log.notifies == 1);

        fixture g;
        g.queue[0] = make_order(1.0, 5, 0x40, 0, 0);
        for (int i = 0; i < mh::orders::ARG_SLOTS; ++i) g.queue[0].args[i] = -1;
        g.queue_count                                                                     = 1;
        g.unit_at(0, 0).unit_proto_id                                                     = 2;
        g.unit_at(0, 0).state                                                             = 0x6;
        g.unit_types[2 * mh::orders::UNIT_TYPE_STRIDE + mh::orders::UNIT_TYPE_STATE_BYTE] = 0x5;
        od::queue_apply_and_dequeue(g.st, recording_calls(), 0, 0, 0);
        check("apply does not notify when the state differs", g_log.notifies == 0);
    }

    // ---- integrity_check: the byte-duplication verifier ----
    {
        fixture f;
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        order ok{};
        ok.unit_index     = 0x3434;
        ok.owner_and_kind = 0x7878;
        ok.param0         = static_cast<int16_t>(0xBCBC);
        ok.order_code     = 0xEFEF;
        od::integrity_check(f.st, recording_calls(), &ok);
        check("integrity_check passes a correctly duplicated record",
              g_log.returns == 0);
    }
    {
        fixture f; // a MASKED record (what a wrong stage_scheduled would produce) must FAIL
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        order bad{};
        bad.unit_index = 0x34; // high byte 0, low byte 0x34 -> mismatch
        od::integrity_check(f.st, recording_calls(), &bad);
        check("integrity_check tears down on a masked (not duplicated) field in MP",
              g_log.returns == 1);
    }
    {
        fixture f; // the SAME bad record outside MP must do nothing
        f.session_mode = 0;
        order bad{};
        bad.unit_index = 0x34;
        od::integrity_check(f.st, recording_calls(), &bad);
        check("integrity_check is silent outside MP", g_log.returns == 0);
    }
    {
        fixture f; // it checks all FOUR fields, and does not stop at the first bad one
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        order bad{};
        bad.unit_index = 0x0034;
        bad.order_code = 0x00EF;
        od::integrity_check(f.st, recording_calls(), &bad);
        check("integrity_check reports every mismatching field, not just the first",
              g_log.returns == 2);
    }

    // ---- ST4: the module as the OWNER of its save state ---------------------------------------
    // Two properties, and the second is the one nobody would otherwise have written down.
    {
        fixture f;
        for (int i = 0; i < mh::orders::QUEUE_CAP; ++i) f.queue[i].unit_index = (uint16_t)(i + 1);
        f.queue_count = 7;

        // (1) emit(PERSIST) reproduces the block path's byte stream EXACTLY: the whole 20400-byte
        //     array -- including the slots past the live count, which the original block-copies too,
        //     so a tidier "live prefix only" emit would NOT be byte-identical -- then the 4-byte
        //     count, in SavePlanetToDisk's order (queue is step 1, count is step 2). Reversing them
        //     would still round-trip through this module and still pass a self-consistency test,
        //     while producing a save the original cannot read.
        std::vector<uint8_t> blob;
        mh::state::fn_sink   sink(
            mh::state::sink_mode::PERSIST,
            [](void *ctx, const void *p, uint32_t n) {
                auto *v = static_cast<std::vector<uint8_t> *>(ctx);
                v->insert(v->end(), (const uint8_t *)p, (const uint8_t *)p + n);
            },
            &blob);
        od::emit(f.st, sink);

        const size_t         qbytes = (size_t)mh::orders::QUEUE_CAP * sizeof(order);
        std::vector<uint8_t> expect;
        expect.insert(expect.end(), (const uint8_t *)f.queue.data(),
                      (const uint8_t *)f.queue.data() + qbytes);
        expect.insert(expect.end(), (const uint8_t *)&f.queue_count,
                      (const uint8_t *)&f.queue_count + sizeof(int32_t));
        check("emit(PERSIST) == the block path's bytes (20400 + 4, queue then count)",
              blob.size() == qbytes + 4 && blob.size() == expect.size() &&
                  std::memcmp(blob.data(), expect.data(), blob.size()) == 0);

        // (2) load_state restores the queue AND LEAVES PENDING/STAGING ALONE. Measured in Ghidra
        //     2026-07-31: the load path never resets them. The only code that does is FillDefaults
        //     (three literal `MOV dword ptr [...],0x0`), reachable only from llm_strat_mode_init and
        //     ReadMap_pre, neither of which llm_game_load or LoadPlanetFromDisk reaches. So a
        //     load_state that "helpfully" cleared the container would silently diverge from the
        //     original: the ABSENCE of that clear is the property under test, not an omission.
        fixture g;
        std::memset(g.pending.data(), 0x5C, g.pending.size() * sizeof(order));
        std::memset(g.staging.data(), 0x3B, g.staging.size() * sizeof(order));
        g.pending_count = 5;
        g.staging_count = 3;
        struct cursor {
            const uint8_t *p;
        } cur{blob.data()};
        mh::state::fn_source src(
            [](void *ctx, void *p, uint32_t n) {
                auto *c = static_cast<cursor *>(ctx);
                std::memcpy(p, c->p, n);
                c->p += n;
            },
            &cur);
        od::load_state(g.st, src);
        check("load_state restores the queue array and its count",
              g.queue_count == 7 && std::memcmp(g.queue.data(), f.queue.data(), qbytes) == 0);

        bool untouched = g.pending_count == 5 && g.staging_count == 3;
        for (size_t i = 0; untouched && i < g.pending.size() * sizeof(order); ++i)
            untouched = ((const uint8_t *)g.pending.data())[i] == 0x5C;
        for (size_t i = 0; untouched && i < g.staging.size() * sizeof(order); ++i)
            untouched = ((const uint8_t *)g.staging.data())[i] == 0x3B;
        check("load_state leaves PENDING and STAGING untouched (fidelity, not an omission)",
              untouched);
    }


    // ==============================================================================================
    // ST5 -- ONE CODEC FOR THE ORDER RECORD. The wire and the save stop having private copies.
    //
    // THE POINT IS THE COUPLING, not the round-trips. Each path round-tripped against itself before
    // this item and would go on doing so with two divergent layouts -- which is exactly the failure:
    // the save keeps reading its own files while the wire disagrees with the peer, and each side's
    // test stays green. So the evidence that matters is the MUTATION on record: editing
    // orders/order_codec.h has to turn BOTH of the round-trips below red TOGETHER. Either one
    // surviving would mean it kept a copy.
    {
        // A record with a value in every field, chosen so no two fields share a byte pattern -- a
        // codec that swapped two same-width fields would otherwise round-trip perfectly.
        mh::orders::order o{};
        o.exec_time      = 1234.5;
        o.unit_index     = 0x1234;
        o.owner_and_kind = 0x0f42;
        o.param0         = -3;
        o.order_code     = 0x007e;
        for (int i = 0; i < 13; ++i) o.args[i] = 0x10203040 + i;
        o.args[3] = -1;          // the "leave unchanged" sentinel
        o.args[7] = -0x7fffffff; // a negative that is not the sentinel

        // THE RECORDED GOLDEN. Captured from the LAYOUT (the order-container notes' 0x44 record),
        // computed independently of the code under test -- a golden generated by the encoder would
        // only prove the encoder equals itself. This is the anchor that survives a future
        // representation change, when the raw struct image below stops being a valid reference.
        static const uint8_t GOLDEN[68] = {
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x4a,
            0x93,
            0x40,
            0x34,
            0x12,
            0x42,
            0x0f,
            0xfd,
            0xff,
            0x7e,
            0x00,
            0x40,
            0x30,
            0x20,
            0x10,
            0x41,
            0x30,
            0x20,
            0x10,
            0x42,
            0x30,
            0x20,
            0x10,
            0xff,
            0xff,
            0xff,
            0xff,
            0x44,
            0x30,
            0x20,
            0x10,
            0x45,
            0x30,
            0x20,
            0x10,
            0x46,
            0x30,
            0x20,
            0x10,
            0x01,
            0x00,
            0x00,
            0x80,
            0x48,
            0x30,
            0x20,
            0x10,
            0x49,
            0x30,
            0x20,
            0x10,
            0x4a,
            0x30,
            0x20,
            0x10,
            0x4b,
            0x30,
            0x20,
            0x10,
            0x4c,
            0x30,
            0x20,
            0x10,
        };

        uint8_t enc[mh::orders::codec::RECORD_BYTES];
        mh::orders::codec::encode(o, enc);
        check("ST5: the encoded record matches the RECORDED golden -- the bytes on the wire and in the "
              "file are unchanged by giving them a codec",
              std::memcmp(enc, GOLDEN, sizeof(GOLDEN)) == 0);

        // And the strong form, for THIS build: the encoding still equals the struct's raw image, so
        // the change is provably byte-identical for every value and not just the fixture. This is
        // the assertion that must be deleted the day the representation actually changes -- which is
        // the point at which the golden above becomes the only reference.
        check("ST5: while the in-memory record IS the wire record, encode() equals its raw image",
              std::memcmp(enc, &o, sizeof(o)) == 0);

        mh::orders::order back{};
        mh::orders::codec::decode(enc, back);
        check("ST5: decode(encode(x)) == x",
              std::memcmp(&back, &o, sizeof(o)) == 0);

        // ---- THE WIRE PATH, through the real emitter -------------------------------------------
        // Not codec::encode called twice: mh::lockstep::detail::send_order is the function the game
        // calls, and routing it through the codec is half of what this item claims.
        uint8_t                            wire[512]{};
        int32_t                            cursor = 0;
        const mh::lockstep::order_tx_state tx{wire, &cursor};
        mh::lockstep::detail::send_order(tx, mh::lockstep::inert_order_tx_calls(), &o);
        check("ST5 wire: the emitter advances by the tag plus one record",
              cursor == 1 + (int32_t)mh::orders::codec::RECORD_BYTES);
        check("ST5 wire: the record tag is written first",
              wire[0] == mh::lockstep::ORDER_RECORD_TAG);
        check("ST5 WIRE ROUND-TRIP: the bytes the emitter puts on the wire are the golden record",
              std::memcmp(wire + 1, GOLDEN, sizeof(GOLDEN)) == 0);

        mh::orders::order from_wire{};
        mh::orders::codec::decode(wire + 1, from_wire);
        check("ST5 WIRE ROUND-TRIP: and the receiver's decode reproduces the record exactly",
              std::memcmp(&from_wire, &o, sizeof(o)) == 0);

        // ---- THE SAVE PATH, through the real emit/load -----------------------------------------
        std::vector<mh::orders::order> qa(mh::orders::QUEUE_CAP), qb(mh::orders::QUEUE_CAP);
        int32_t                        ca = 7, cb = 0;
        for (int i = 0; i < mh::orders::QUEUE_CAP; ++i) {
            qa[i]            = o;
            qa[i].unit_index = (uint16_t)(0x1000 + i); // every slot distinguishable
            qa[i].args[0]    = 0x10203040 + i;
        }
        mh::orders::container_state sa{}, sb{};
        sa.queue       = qa.data();
        sa.queue_count = &ca;
        sb.queue       = qb.data();
        sb.queue_count = &cb;

        std::vector<uint8_t> file;
        struct vec_sink      final : mh::state::state_sink {
            explicit vec_sink(std::vector<uint8_t> &v)
                : mh::state::state_sink(mh::state::sink_mode::PERSIST), v(v) {}
            std::vector<uint8_t> &v;
            void                  raw(const void *p, uint32_t n) override {
                const uint8_t *b = (const uint8_t *)p;
                v.insert(v.end(), b, b + n);
            }
        } sink(file);
        mh::orders::detail::emit(sa, sink);
        check("ST5 save: the emitted stream is QUEUE_CAP records plus the count",
              file.size() == (size_t)mh::orders::QUEUE_CAP * mh::orders::codec::RECORD_BYTES + 4);
        check("ST5 SAVE ROUND-TRIP: routing the save through the codec is BYTE-IDENTICAL to the flat "
              "block it replaced -- which is what makes this item invisible to the save format",
              std::memcmp(file.data(), qa.data(),
                          (size_t)mh::orders::QUEUE_CAP * mh::orders::codec::RECORD_BYTES) == 0);

        struct vec_source final : mh::state::state_source {
            explicit vec_source(const std::vector<uint8_t> &v) : v(v) {}
            const std::vector<uint8_t> &v;
            size_t                      at = 0;
            void                        raw(void *p, uint32_t n) override {
                std::memcpy(p, v.data() + at, n);
                at += n;
            }
        } source(file);
        mh::orders::detail::load_state(sb, source);
        check("ST5 save: the count survives the round-trip",
              cb == ca);
        check("ST5 SAVE ROUND-TRIP: every one of the 300 slots comes back identical",
              std::memcmp(qb.data(), qa.data(),
                          (size_t)mh::orders::QUEUE_CAP * sizeof(mh::orders::order)) == 0);
    }

    // ---- O1b: THE REPLICATED LANE IS 8-BIT, AND THE FIELD CARRIES A *BUILDING* INDEX --------------
    //
    // This is O1b's failing observation, and it is a PRESERVED DEFECT, not a bug in this container:
    // every assertion below pins what the ORIGINAL does, so the suite stays green while the defect
    // stands. When the encoding is widened (tracker item O1c) these are the checks that must be
    // rewritten -- deliberately, not silently.
    //
    // THE CHAIN, each link measured rather than assumed:
    //   1. the building order wrappers call dispatch(unit_id = BUILDING roster index,
    //      player | KIND_BLDG(0x40), ...) -- libmh/orders/issue/issue_bldg_orders.cpp;
    //   2. in MP, for a net-controlled player, dispatch routes to stage_scheduled, which DUPLICATES
    //      each field's low byte (0x012c -> 0x2c2c);
    //   3. schedule() puts that duplicated record ON THE WIRE and the masked copy into local
    //      PENDING; the receiving peer's pending_enqueue masks the same way;
    //   4. llm_strat_order_queue_dispatch's kind-0x40 arm then indexes
    //      buildings[owner][record.unit_index] (@0x00466892, read 2026-08-30).
    // So a building order for roster index 300 is applied to building 44 -- on BOTH peers, which is
    // why it is not a desync and why no oracle in this project would have caught it.
    //
    // WHY IT IS REACHABLE TODAY, AND WHY THE ITEM'S OWN PREMISE WAS WRONG. O1b called this the
    // "500-unit build" risk. The UNIT roster was never raised -- it is 8 x 100 (state region `units`,
    // 186,400 B = 8 x 100 x 233) -- so a unit index cannot approach 255. The BUILDINGS roster IS the
    // array this project raised, 100 -> 500 (the hardcoded-limits survey), and 256..499 are exactly the
    // indices that truncate. Right defect, wrong array.
    {
        fixture f;
        f.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        f.set_net_controlled(0, true);
        f.game_clock = 10.0;
        f.step_size  = 0.25;
        f.horizon    = 0.0;
        char tag[]   = "t";
        od::scratch_reset(f.st);

        // Player 0, kind 0x40 (KIND_BLDG), building roster index 300 -- legal in the cap-raised
        // build, impossible in retail.
        const uint16_t BLDG_IDX  = 300; // 0x012c
        const uint16_t TRUNCATED = 44;  // 0x2c -- the building that actually gets the order
        od::dispatch(f.st, recording_calls(), tag, BLDG_IDX, 0x40, 0x6d, 0x6d);

        check("O1b: a building order for a net player takes the SCHEDULED lane",
              f.staging_count == 1 && f.queue_count == 0);
        check("O1b: stage_scheduled duplicates the LOW byte, discarding the high one (300 -> 0x2c2c)",
              f.staging[0].unit_index == 0x2c2c);
        check("O1b: the kind nibble survives -- this really is a BUILDING order",
              (f.staging[0].owner_and_kind & 0xf0) == 0x40);

        od::schedule(f.st, recording_calls());

        check("O1b: the LOCAL peer decodes building 300 as building 44",
              f.pending_count == 1 && f.pending[0].unit_index == TRUNCATED);
        check("O1b: the wire carries the duplicated form, so the peer's integrity check passes on a "
              "record that has ALREADY lost the high byte",
              g_log.sent.size() == 1 && g_log.sent[0].unit_index == 0x2c2c);

        // The receiving peer, driven through the very body the net path calls. NOTE THE ORDER:
        // the wire record is copied out BEFORE the second fixture exists, because fixture's
        // constructor resets g_log -- reading g_log.sent[0] afterwards is a read past the end of an
        // emptied vector, which is exactly how this block first crashed.
        order   wire = g_log.sent[0];
        fixture r;
        od::pending_enqueue(r.st, &wire);
        check("O1b: the REMOTE peer decodes the same record to the same wrong building",
              r.pending_count == 1 && r.pending[0].unit_index == TRUNCATED);
        check("O1b: both peers agree on the WRONG target -- a silent mis-target, not a desync, which "
              "is why the determinism oracle cannot see it",
              r.pending[0].unit_index == f.pending[0].unit_index && f.pending[0].unit_index != BLDG_IDX);

        // The boundary, pinned so a future fix has the exact edge: 255 survives, 256 does not.
        fixture e255;
        e255.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        e255.set_net_controlled(0, true);
        od::scratch_reset(e255.st);
        od::dispatch(e255.st, recording_calls(), tag, 255, 0x40, 0x6d, 0x6d);
        od::schedule(e255.st, recording_calls());
        check("O1b: 255 is the largest building index the replicated lane carries intact",
              e255.pending[0].unit_index == 255);

        fixture e256;
        e256.session_mode = mh::orders::SESSION_MP_LOCKSTEP;
        e256.set_net_controlled(0, true);
        od::scratch_reset(e256.st);
        od::dispatch(e256.st, recording_calls(), tag, 256, 0x40, 0x6d, 0x6d);
        od::schedule(e256.st, recording_calls());
        check("O1b: 256 collapses to 0 -- the first building the cap raise made unaddressable",
              e256.pending[0].unit_index == 0);
    }


    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
