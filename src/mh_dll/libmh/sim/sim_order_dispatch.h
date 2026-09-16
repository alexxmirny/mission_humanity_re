//
// sim/sim_order_dispatch.h -- llm_strat_order_queue_dispatch, the order-queue EXECUTOR
// (RI-SIM / SIM1C).
//
// 12302 bytes, 62% of batch C, and the single largest function in the sim migration set. It is the
// other half of the pair the order-container notes split: mh::orders puts records INTO the queue,
// this takes them out and makes them happen. sim_order_enqueue.{h,cpp} is the third piece -- the
// typed wrappers that DECIDE to enqueue.
//
// ---------------------------------------------------------------------------------------------
// WHAT IT DOES, IN ONE PARAGRAPH
// ---------------------------------------------------------------------------------------------
//
// It is a FILTER-IN-PLACE over the queue, not a drain. It walks slots 0..count-1; each record's
// `owner_and_kind` byte splits into a low-nibble PLAYER and a high-nibble KIND, and the kind routes
// to one of four paths (unit / building / storage / lockstep). Most paths execute the order and
// drop it. The unit path may instead RETAIN the record: it copies it down to a compaction cursor
// and advances that cursor, so retained orders end up packed at the front. At the end the cursor --
// not the original count -- becomes the new queue count. A "sensible" reimplementation that drained
// the queue and set the count to zero passes an idle test and loses every deferred order under
// load.
//
// ---------------------------------------------------------------------------------------------
// WHY THREE TRANSLATION UNITS
// ---------------------------------------------------------------------------------------------
//
// The body is one function but three separable regions, and the split follows the assembly's own
// seams rather than a size target:
//
//   sim_order_dispatch.cpp        the loop, the kind router, the unit and storage paths, the
//                                 lockstep-extend kind, and the tail. Owns the RETENTION logic --
//                                 no other path can retain, which is what makes the split clean.
//   sim_order_dispatch_bldg.cpp   the BUILDING table: 26 arms behind the first REPNE SCASB switch.
//   sim_order_dispatch_admin.cpp  the ADMIN table: 23 arms behind the second, keyed on the order
//                                 CODE rather than on param0 -- the god-mode/debug/economy verbs.
//
// The two table files are pure functions of (state, calls, ctx): they cannot retain, cannot touch
// the loop cursor, and every arm of both ends at the same place. That is why they can be written
// concurrently. THIS HEADER IS CONDUCTOR-OWNED -- it is the contract the three agree on, so no
// writer edits it and no writer's content depends on another's timing.
//
// ---------------------------------------------------------------------------------------------
// THE DECODE IS TRANSCRIBED, NOT RE-DERIVED
// ---------------------------------------------------------------------------------------------
//
// Both switches are Watcom's sparse form: a `REPNE SCASB` over a byte table, then an indirect jump
// through a pointer table indexed by the REMAINING ECX. Two consequences that a rewrite as a plain
// `switch (opcode)` gets wrong, and the reason `decode_*` below returns the original's JUMP-TABLE
// INDEX instead of an opcode:
//
//   * INDEX 0 IS THE DEFAULT, and the building table's default arm has a REAL BODY (an energy gate
//     and a full handler, 0x00469545) -- so every unrecognised param0 runs it. That is the part a
//     `switch (opcode)` rewrite has to remember.
//   * THE SCAN COUNT IS ONE LARGER THAN THE SCAN TABLE. `ECX` is 26 (resp. 23) over a table of 25
//     (resp. 22) bytes, so the last comparison reads one byte PAST it -- into the low byte of
//     jump-table slot 0. Harmless by construction: a "match" there leaves ECX == 0, the same target
//     a failed scan selects. The tables below therefore carry that overflow byte as their last
//     element, because it is what the hardware compares against; it is NOT a table entry and the
//     value it would decode to is NOT an opcode. See the note on the tables.
//   * THE GUARD IS 16-BIT, THE SCAN IS 8-BIT. `(uint16_t)(param0 - base) > max_delta` rejects
//     first, and only then is the LOW BYTE scanned.
//
// Switching on the index reproduces both for free. Switching on the opcode requires spelling the
// collision out by hand at the one place it matters, and there is no upside.
//
// ---------------------------------------------------------------------------------------------
// SHADOW SAFETY: THIS SITE CANNOT BE ARMED NAIVELY
// ---------------------------------------------------------------------------------------------
//
// The dispatcher reaches FOUR targets the SIM-CUT effect classification marks `effectful` --
// game_ui_PrintTextMessage, llm_snd_play, llm_net_send_lockstep_extend and
// llm_net_lockstep_extend_ui_enter. Shadow mode runs the original, restores the pre-state, then
// runs ours; restoring state does not un-send a packet or un-draw a message, so those four fire
// TWICE per compared call unless the effect seam suppresses them. The seam already covers all four
// (tools/data/sim_effect_classes.json) -- this note exists so nobody arms the site with the seam
// disarmed and reads the clean state comparison as a green.
//
// The `llm_progress_*` callees are classed `state`, NOT effectful: they are suppressed by nothing
// and the between-arms restore undoes them, which is correct.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the record fields the router splits ------------------------------------------------------
// `owner_and_kind` is one 16-bit field carrying two things. sim_order_enqueue.h names the KIND tags
// it writes (ORDER_KIND_UNIT 0x80 / ORDER_KIND_BUILDING 0x40); these are the masks the READER uses,
// plus the two kinds only the dispatcher ever sees.
inline constexpr uint32_t ORDER_OWNER_MASK = 0x000fu; // low nibble: the owning player
inline constexpr uint32_t ORDER_KIND_MASK  = 0x00f0u; // high nibble: which path executes it

inline constexpr uint32_t ORDER_KIND_STORAGE  = 0x20u; // the storage/dock path
inline constexpr uint32_t ORDER_KIND_BLDG     = 0x40u; // -> dispatch_building_order
inline constexpr uint32_t ORDER_KIND_UNIT_EX  = 0x80u; // the unit path -- THE ONLY ONE THAT RETAINS
inline constexpr uint32_t ORDER_KIND_LOCKSTEP = 0xf0u; // the horizon-extension request

// ---- the two sparse switches ------------------------------------------------------------------
//
// Both tables were read out of the image (mh.exe EN, 2026-08-08) rather than reconstructed from the
// listing's `case_N` labels, because the labels only say WHERE each arm is, not which opcode
// reaches it. All 49 arms were then cross-checked against those labels: all match.
//
// THE LAST ELEMENT OF EACH ARRAY IS NOT A TABLE ENTRY. The real tables are 25 and 22 bytes -- the
// jump tables begin immediately after them (0x004672c8 - 0x004672af = 25; 0x00469635 - 0x0046961f
// = 22) -- while `REPNE SCASB` runs 26 and 23 comparisons. The extra element below (0x45 / 0x7f) is
// the LOW BYTE OF JUMP-TABLE SLOT 0, read one past the end, and it is reproduced here because
// reproducing what the hardware compares is the whole point. It routes to index 0, which is the
// default, so it changes nothing -- which is presumably why Watcom emits it.
//
// AN EARLIER NOTE CALLED THAT BYTE "param0 0xaf" / "order_code 0xb3" AND TREATED THEM AS REAL
// OPCODES SHARING THE DEFAULT ARM. They are not opcodes at all, and the giveaway was there the
// whole time: neither /llm/E_BLDG_ORDER_PARAM0 nor /llm/E_ORDER_CODE has a member for them,
// because nothing enqueues a value that does not exist. Caught 2026-08-08 when a `byte[26]` data
// definition in Ghidra collided with the jump table's first pointer.
//
// Every byte in each table is distinct, so "first match" and "any match" agree -- stated because the
// loop below is a first-match loop and a duplicate would make that a real decision.

// The two scan tables are IMAGE BYTES in image order. The house style breaks a 26-element
// initializer one-per-line, which makes a transcription impossible to check against a hex dump --
// and checking it against a hex dump is the entire point, so the formatter is turned off across
// them. THE DIRECTIVE MUST BE THE WHOLE COMMENT: `// clang-format off -- because ...` is not a
// directive, it is a comment that happens to contain one, and clang-format ignores it silently.
// clang-format off
inline constexpr uint16_t BUILDING_ORDER_BASE                      = 0x6au; // SUB EAX,0x6a @0x0046733b
inline constexpr uint16_t BUILDING_ORDER_MAX_DELTA                 = 0x80u; // CMP word ptr,0x80 / JA @0x00467341
inline constexpr int32_t  BUILDING_ORDER_ARMS                      = 26;    // MOV ECX,0x1a
inline constexpr uint8_t  BUILDING_ORDER_SCAN[BUILDING_ORDER_ARMS] = {
    // DAT_004672af. Scanned as `param0 - BUILDING_ORDER_BASE`, low byte only.
    0x80, 0x7c, 0x75, 0x74, 0x73, 0x72, 0x71, 0x6c, 0x6a, 0x69, 0x68, 0x1f, 0x19,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x11, 0x0f, 0x0a, 0x05, 0x03, 0x01, 0x00, 0x45,
};

inline constexpr uint16_t ADMIN_ORDER_BASE                   = 0x34u; // SUB EAX,0x34 @0x0046969c
inline constexpr uint16_t ADMIN_ORDER_MAX_DELTA              = 0xc7u; // CMP word ptr,0xc7 / JA @0x004696a2
inline constexpr int32_t  ADMIN_ORDER_ARMS                   = 23;    // MOV ECX,0x17
inline constexpr uint8_t  ADMIN_ORDER_SCAN[ADMIN_ORDER_ARMS] = {
    // DAT_0046961f. Scanned as `order_code - ADMIN_ORDER_BASE`, low byte only.
    0xc7, 0xc6, 0xc5, 0xc4, 0xc3, 0xc2, 0xc1, 0xc0, 0xbf, 0xbe, 0xbd, 0xbc,
    0xbb, 0xba, 0xb9, 0xb8, 0xb7, 0xb5, 0xb4, 0xb3, 0x01, 0x00, 0x7f,
};

// clang-format on

// ---- the arms, as ENUMS ------------------------------------------------------------------------
//
// `decode_*` returns the ORIGINAL'S JUMP-TABLE INDEX, and a bare `case 7:` says nothing about what
// arm 7 is. These name every arm, and the names are NOT invented: they are the members of Ghidra's
// /llm/E_BLDG_ORDER_PARAM0 and /llm/E_ORDER_CODE, which an earlier session built from the ENQUEUE
// call sites -- a completely separate derivation from this file's, which read the jump tables out
// of the image.
//
// THAT AGREEMENT IS EVIDENCE, and it is worth stating what it covers: all 25 building arms and all
// 22 admin arms resolve to exactly one enum member, and the generator that produced these blocks
// ASSERTS that (an arm with no member is a hard error, not a blank name). Two independent readings
// of a 49-way dispatch agreeing member-for-member is a much stronger check than either alone.
//
// The Ghidra enums have no member for 0xaf or 0xb3, and THAT ABSENCE WAS THE CLUE that those two
// are not opcodes at all -- they are what the scan's one-byte over-read decodes to. Do not "fill
// the gap": an enum built from enqueue sites cannot contain a value nothing enqueues.
//
// Switching on these rather than on `int32_t` also buys MSVC's C4061/C4062 coverage warning, so an
// arm that is dropped in a later edit is a diagnostic instead of a silent fall-through to default.

enum class bldg_arm : int32_t {
    // The arm `REPNE SCASB` selects on NO MATCH -- and equally where the scan's one-byte over-read
    // past the table lands (see the tables' note; that byte is not an opcode). It has a REAL BODY
    // at 0x00469545, so every unrecognised param0 runs a genuine handler.
    DEFAULT            = 0,
    PAY_CYCLE_UPKEEP   = 1,  // opcode 0x6a
    RESTART_CONSTRUCT  = 2,  // opcode 0x6b
    PROD_QUEUE_ACCUM   = 3,  // opcode 0x6d
    PROD_ITEM_COMPLETE = 4,  // opcode 0x6f
    MINE_RESCAN        = 5,  // opcode 0x74
    HANGAR_RECHARGE    = 6,  // opcode 0x79
    TURRET_ACQUIRE     = 7,  // opcode 0x7b
    ASSIGN_WORKERS     = 8,  // opcode 0x7e
    UNASSIGN_WORKERS   = 9,  // opcode 0x7f
    SET_STAFFED        = 10, // opcode 0x80
    CLEAR_STAFFED      = 11, // opcode 0x81
    START_UPGRADE      = 12, // opcode 0x82
    CANCEL_RESET       = 13, // opcode 0x83
    START_RESEARCH     = 14, // opcode 0x89
    SHUTTLE_DEPART     = 15, // opcode 0xd2
    PORT_DEPART        = 16, // opcode 0xd3
    MOTHER_DEPART      = 17, // opcode 0xd4
    CARGO_BIND_SLOT    = 18, // opcode 0xd6
    LOAD_PASSENGERS    = 19, // opcode 0xdb
    UNLOAD_PASSENGERS  = 20, // opcode 0xdc
    LOAD_RESOURCE      = 21, // opcode 0xdd
    UNLOAD_RESOURCE    = 22, // opcode 0xde
    FLUSH_CARGO_HOLD   = 23, // opcode 0xdf
    PURGE_DEAD_DOCKED  = 24, // opcode 0xe6
    INSTANT_CONSTRUCT  = 25, // opcode 0xea
};

enum class admin_arm : int32_t {
    // The arm `REPNE SCASB` selects on NO MATCH -- and equally where the scan's one-byte over-read
    // past the table lands (see the tables' note; that byte is not an opcode). Its body is EMPTY:
    // a jump straight to the loop tail.
    DEFAULT                = 0,
    UNIT_STATUS_BIT_SET    = 1,  // opcode 0x34
    UNIT_STATUS_BIT_CLEAR  = 2,  // opcode 0x35
    DEBUG_ROLL_RANDOM      = 3,  // opcode 0xe7
    GAME_SPEED_INCREASE    = 4,  // opcode 0xe8
    GAME_SPEED_DECREASE    = 5,  // opcode 0xe9
    UNIT_CREATE            = 6,  // opcode 0xeb
    POPULATION_ADD_REMOVE  = 7,  // opcode 0xec
    RESOURCE_ADD_SPEND     = 8,  // opcode 0xed
    PROJECTS_COLLECT       = 9,  // opcode 0xee
    RECHECK_BUILDINGS      = 10, // opcode 0xef
    RECHECK_PROJECTS       = 11, // opcode 0xf0
    RECHECK_PLANET_ALL     = 12, // opcode 0xf1
    BLDG_QUEUE_CONSTRUCT   = 13, // opcode 0xf2
    UNIT_RECRUIT           = 14, // opcode 0xf3
    DIPLOMACY_SET_RELATION = 15, // opcode 0xf4
    PLAYER_SET_AI_HUMAN    = 16, // opcode 0xf5
    ENERGY_REFILL_FULL     = 17, // opcode 0xf6
    APPLY_SCALED_DAMAGE    = 18, // opcode 0xf7
    APPLY_LETHAL_DAMAGE    = 19, // opcode 0xf8
    FOW_REVEAL_FULL        = 20, // opcode 0xf9
    CREDIT_CONQUEST_KILLS  = 21, // opcode 0xfa
    UNIT_NOTIFY_STATUS     = 22, // opcode 0xfb
};

// The shared shape of both decodes: reject on the 16-bit delta, scan the low byte, return the
// REMAINING count. `arms - 1 - j` is what ECX holds after `REPNE SCASB` matched at position j, and 0
// is what it holds when nothing matched -- one expression covering both, as the hardware does.
inline constexpr int32_t decode_scan(uint16_t value, uint16_t base, uint16_t max_delta,
                                     const uint8_t *table, int32_t arms) {
    const uint16_t delta = (uint16_t)(value - base);
    if (delta > max_delta) return 0;
    const uint8_t needle = (uint8_t)delta;
    for (int32_t j = 0; j < arms; ++j)
        if (table[j] == needle) return arms - 1 - j;
    return 0;
}

// The arm of the BUILDING table, keyed on the order record's `param0`. See `bldg_arm` below for
// why the return is an enum and what its names come from.
inline constexpr bldg_arm decode_building_order(uint16_t param0) {
    return (bldg_arm)decode_scan(param0, BUILDING_ORDER_BASE, BUILDING_ORDER_MAX_DELTA,
                                 BUILDING_ORDER_SCAN, BUILDING_ORDER_ARMS);
}

// The arm of the ADMIN table, keyed on the order record's `order_code`, NOT on param0 -- the two
// switches read different fields, which is the whole reason there are two.
inline constexpr admin_arm decode_admin_order(uint16_t order_code) {
    return (admin_arm)decode_scan(order_code, ADMIN_ORDER_BASE, ADMIN_ORDER_MAX_DELTA,
                                  ADMIN_ORDER_SCAN, ADMIN_ORDER_ARMS);
}

// ---- the tables are ASSERTED, because hand-transcribing 49 bytes is how this goes wrong --------
//
// It went wrong on the first draft of this header: one byte (0xbb) dropped out of the admin table,
// which does not shorten anything visible -- the array is sized by the same literal list -- but
// shifts the index of every entry after it, so nine arms would have run the wrong handler. Nothing
// downstream could have caught it: the shadow oracle only sees the opcodes a run happens to send,
// and both tables would still have decoded *something* for every input.
//
// So the properties that make a transcription faithful are checked at COMPILE time:
//
//   (1) EVERY ENTRY IS DISTINCT. `REPNE SCASB` stops at the first match, so a duplicate would make
//       "which j matched" a real question; it is not one here, and this is what says so. It also
//       catches a byte accidentally typed twice.
//       It is also what catches the dropped byte specifically: `uint8_t t[23] = {22 values}`
//       ZERO-FILLS the tail, and 0x00 is already in the table, so the drop shows up as a duplicate.
//       That is why (1) is stated as a property rather than left implicit.
//   (2) EVERY ARM IS REACHABLE AND LANDS WHERE THE TABLE SAYS. For each entry, the opcode that
//       produces it must decode back to `arms-1-j`, and the entry must survive the 16-bit guard --
//       an entry above `max_delta` would be an arm no input can ever reach, which would mean the
//       guard or the base is wrong. Checked over the TABLE (arms^2 steps), not over the 16-bit
//       input domain: the domain sweep is the same property and ~42M constexpr steps.
//   (3) THE DEFAULT ARM, from both directions: an unrecognised opcode and the scan's over-read byte
//       must both land on arm 0. The second of those is the one that would break if the arrays were
//       ever "tidied" by dropping their last element without also dropping the scan count.

constexpr bool scan_entries_distinct(const uint8_t *t, int32_t n) {
    for (int32_t i = 0; i < n; ++i)
        for (int32_t j = i + 1; j < n; ++j)
            if (t[i] == t[j]) return false;
    return true;
}

constexpr bool scan_arms_reachable(uint16_t base, uint16_t max_delta, const uint8_t *t,
                                   int32_t arms) {
    for (int32_t j = 0; j < arms; ++j) {
        if (t[j] > max_delta) return false; // an arm behind the guard: unreachable by any input
        if (decode_scan((uint16_t)(base + t[j]), base, max_delta, t, arms) != arms - 1 - j)
            return false;
    }
    return true;
}

static_assert(scan_entries_distinct(BUILDING_ORDER_SCAN, BUILDING_ORDER_ARMS));
static_assert(scan_entries_distinct(ADMIN_ORDER_SCAN, ADMIN_ORDER_ARMS));
static_assert(scan_arms_reachable(BUILDING_ORDER_BASE, BUILDING_ORDER_MAX_DELTA,
                                  BUILDING_ORDER_SCAN, BUILDING_ORDER_ARMS));
static_assert(scan_arms_reachable(ADMIN_ORDER_BASE, ADMIN_ORDER_MAX_DELTA, ADMIN_ORDER_SCAN,
                                  ADMIN_ORDER_ARMS));

// (3) the anchors.
static_assert(decode_building_order(0x00) == bldg_arm::DEFAULT,
              "an unrecognised param0 takes the default arm");
static_assert(decode_building_order(0xaf) == bldg_arm::DEFAULT,
              "and so does the scan's over-read byte, which is not an opcode");
static_assert(decode_admin_order(0x00) == admin_arm::DEFAULT,
              "an unrecognised order_code takes the default arm");
static_assert(decode_admin_order(0xb3) == admin_arm::DEFAULT,
              "and so does the scan's over-read byte, which is not an opcode");

// ---- what the loop hands to a table handler ---------------------------------------------------
//
// The four values the original keeps in stack slots for the whole of one iteration and every arm
// re-reads. Passed as a struct rather than four parameters so an arm cannot silently take them in
// the wrong order -- `player` and `object_index` are both small ints and both plausible first.
//
// It carries NO cursor. The loop's compaction state ([EBP-0x20]) never leaves
// sim_order_dispatch.cpp, which is the property that lets the two table files be written
// independently: an arm has no way to retain, because it is not given the means.
struct dispatch_ctx {
    int32_t  slot;         // [EBP-0x24] -- which queue record is executing
    uint32_t player;       // [EBP-0x34] -- owner_and_kind & ORDER_OWNER_MASK
    uint32_t kind;         // [EBP-0x38] -- owner_and_kind & ORDER_KIND_MASK
    int32_t  object_index; // [EBP-0x30] -- the record's `unit_index` field, WHICH IS NOT ALWAYS A
                           // UNIT: the building path indexes `buildings` with it and the storage
                           // path a dock slot. The field name is the container's; the meaning is
                           // the kind's.
};

// ---- the outward calls ------------------------------------------------------------------------
//
// Indirected for the same reason as every other module here: a direct `mh::call::` inside a
// `detail::` body reaches into the live game image, which makes the body untestable by
// `net_selftest.exe simtest` and unusable in the offline fixture. Production binds these to
// `mh::call::*` (live_dispatch_calls()); simtest binds recording stubs.
//
// GENERATED, NOT TRANSCRIBED. The member types must match the thunks in addr/mh_calls.gen.h exactly
// -- an aggregate initializer does not convert -- so both this block and live_dispatch_calls()'s initializer
// come from `python tools/gen_module_calls.py --from-asm <the .asm> --exclude utils_assert_stack_
// capacity --exclude w_sprintf --sort`. Re-run it rather than hand-editing a signature.
//
// ONE DELIBERATE OMISSION: `utils_assert_stack_capacity` is Watcom's stack probe in the prologue,
// not a callee of the logic. There is nothing to reproduce.
//
// `w_sprintf` IS A MEMBER, and the reasoning that once kept it out was wrong. The generator excludes
// it because a *variadic* callee has no single signature -- true of `w_sprintf`, false of
// `w_sprintf__vss`, which is one measured SHAPE (tools/data/varargs_shapes.json) and therefore an
// ordinary function pointer like every other member. The old note claimed "a shape thunk is not
// something a recording stub can stand in for"; a fixed-arity stub stands in for it exactly. What
// the omission actually cost was the offline oracle: `mh::call::w_sprintf__vss` calls a game image
// address, which is unmapped in net_selftest.exe, so the SIX arms that print a message could not be
// reached by `simtest` at all -- they would have taken an access violation. The member is appended
// BY HAND rather than by the generator (which reads mh_calls.gen.h and correctly refuses a varargs
// name); keep it LAST so the rest of the table stays regenerable in place.
struct dispatch_calls {
    void (*game_SpendResource)(int32_t, int32_t, int32_t);
    int32_t (*game_TryStartProject)(uint16_t, int32_t, uint32_t);
    uint32_t (*game_ui_PrintTextMessage)(void *);
    int32_t (*llm_bldg_construct_finalize)(uint32_t, int32_t, uint16_t, char, uint32_t, uint32_t);
    void (*llm_bldg_finish_current_order)(uint32_t, uint32_t);
    int32_t (*llm_bldg_footprint_is_clear)(int32_t, int32_t, int32_t, uint32_t);
    void (*llm_bldg_load_resource_tail_noop)();
    int32_t (*llm_bldg_pay_build_cost)(uint32_t, int32_t);
    int32_t (*llm_bldg_queue_construction)(int32_t, int32_t, int32_t, int32_t);
    void (*llm_bldg_reset_construction_anim)(uint32_t, int32_t);
    void (*llm_bldg_scrap_stored_units)(uint32_t, int32_t);
    void (*llm_bldg_set_connected_flag)(uint16_t, int32_t);
    void (*llm_bldg_transfer_notify_noop)();
    void (*llm_combat_credit_planet_conquest_kills)(uint32_t);
    void (*llm_debug_roll_random)();
    void (*llm_diplomacy_set_relation)(int32_t, int32_t, uint8_t);
    void (*llm_game_player_set_ai)(uint8_t);
    void (*llm_game_player_set_human)(uint8_t);
    void (*llm_game_speed_decrease)(uint32_t);
    void (*llm_game_speed_increase)(uint32_t);
    void (*llm_map_fow_reveal_full)(uint32_t);
    // LIFT-SCREEN: llm_net_lockstep_extend_ui_enter was SPLIT here. What is left of it as an
    // outward call is the wait overlay it showed; its two GAME_MODE stores are libmh's, done at
    // the call site. The return is discarded (the original always leaves 1 in EAX).
    int32_t (*llm_net_lockstep_wait_player_overlay_show)(int32_t);
    // GAME_MODE_SAVED = mode -- the split's first statement. A call slot rather than a store
    // through `own` because that global has no sim_store binding: hosted it is the live global
    // (resync_complete_local reads it back through the same hoist ops), and offline it is a stub,
    // which is exactly the modelling the mp_leave site already has.
    void (*ovl_mode_saved_set)(uint8_t);
    void (*llm_net_send_lockstep_extend)(double);
    int32_t (*llm_prod_bldg_depart_finalize)(uint16_t, int32_t, int32_t);
    int32_t (*llm_prod_shuttle_depart)(uint16_t, int32_t, int32_t);
    int32_t (*llm_prod_shuttle_fuel_apply)(uint16_t, int32_t, int32_t);
    int32_t (*llm_prod_shuttle_fuel_check)(uint16_t, int32_t, int32_t);
    uint32_t (*llm_prod_shuttle_load_passengers)(uint16_t, int32_t, uint32_t);
    int32_t (*llm_prod_shuttle_load_resource)(uint32_t, int32_t, uint32_t, uint32_t);
    int32_t (*llm_prod_shuttle_slot_bind_default)(uint32_t, int32_t);
    int32_t (*llm_prod_shuttle_unload_passengers)(uint16_t, int32_t, int32_t);
    uint8_t (*llm_prod_shuttle_unload_resource)(uint16_t, int32_t, uint16_t, int32_t);
    void (*llm_progress_collect_available_projects)(uint32_t);
    void (*llm_progress_recheck_buildings)(uint32_t);
    void (*llm_progress_recheck_planet_system_all_players)();
    void (*llm_progress_recheck_projects)(uint32_t);
    void (*llm_resource_add)(int32_t, int32_t, int32_t);
    void (*llm_snd_play)(int32_t, int32_t);
    void (*llm_storage_cancel_pending_docked)(uint32_t, int32_t);
    void (*llm_strat_ai_notify_bldg_constructed)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                 uint32_t);
    void (*llm_strat_ai_notify_unit_lifecycle)(uint16_t, uint16_t, uint32_t, uint32_t);
    void (*llm_strat_ai_queue_release_order)(int32_t, int32_t, int32_t);
    int32_t (*llm_strat_bldg_assign_workers)(uint32_t, uint32_t, int32_t);
    void (*llm_strat_bldg_clear_staffed_flag)(uint16_t, uint32_t);
    void (*llm_strat_bldg_flush_cargo_hold)(uint32_t, int32_t);
    void (*llm_strat_bldg_get_coords)(uint16_t, int32_t, int32_t *, int32_t *); // fine_coord * (TACT1-P C6, 2026-09-04)
    void (*llm_strat_bldg_notify_ui)(uint16_t, uint32_t);
    int32_t (*llm_strat_bldg_pay_cycle_inputs)(uint32_t, uint32_t);
    void (*llm_strat_bldg_set_staffed_flag)(uint16_t, int32_t);
    int32_t (*llm_strat_bldg_unassign_workers)(uint16_t, uint32_t, uint32_t);
    uint32_t (*llm_strat_dist_out_of_range)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    void (*llm_strat_population_add)(uint16_t, int32_t);
    void (*llm_strat_population_remove)(uint32_t, int32_t);
    int32_t (*llm_strat_prod_bind_planet)(int32_t, int32_t, int32_t);
    void (*llm_strat_refresh_building)(uint16_t, int32_t);
    void (*llm_strat_storage_purge_dead_docked)(int32_t, int32_t);
    void (*llm_strat_target_release_ref)(uint32_t, int32_t, uint32_t);
    uint32_t (*llm_strat_unit_create)(uint32_t, uint32_t, uint16_t, uint16_t, uint8_t);
    void (*llm_strat_unit_get_coords)(uint16_t, int32_t, int32_t *, int32_t *); // fine_coord * (TACT1-P C6, 2026-09-04)
    void (*llm_strat_unit_notify_status)(uint32_t, int32_t, uint32_t);
    void (*llm_strat_unit_set_state_of)(int32_t, int32_t, int16_t);
    void (*llm_unit_apply_production_completion)(uint32_t, int32_t);
    void (*llm_unit_bldg_apply_lethal_damage)(uint32_t, int32_t);
    void (*llm_unit_bldg_apply_scaled_damage)(uint32_t, int32_t);
    void (*llm_unit_bldg_energy_refill_full)(uint32_t, uint32_t);
    uint32_t (*llm_unit_create_soldier)(uint32_t, uint32_t, uint16_t, uint16_t, char);
    void (*llm_unit_force_disembark)(uint32_t, int32_t);
    int32_t (*llm_unit_recruit)(uint32_t, uint32_t);
    void (*llm_unit_set_order_param)(int32_t, int32_t, int16_t);
    int32_t (*llm_unit_state_is_boarding)(int32_t);
    void (*llm_unit_status_bit_clear)(int32_t, int32_t, uint8_t);
    void (*llm_unit_status_bit_set)(int32_t, int32_t, uint8_t);
    void (*llm_unit_transport_unload_docked)(uint32_t, uint32_t);
    void (*llm_unit_transport_unload_field)(uint32_t, uint32_t);
    // Hand-appended, not generated -- see the note above. One measured varargs SHAPE, so an ordinary
    // pointer: (dst, format, name, reason).
    int32_t (*w_sprintf__vss)(void *, const wchar_t *, const wchar_t *, const wchar_t *);
};

// Bound in sim_order_dispatch.cpp -- the loop TU owns it, and the two table TUs receive it.
const dispatch_calls &live_dispatch_calls();

// ---- the logic, over an explicit state --------------------------------------------------------
namespace detail {

// The whole function. Walks the queue, executes each record, compacts the retained ones to the
// front, and stores the retained count.
void order_queue_dispatch(const sim_view &v, sim_store &own, const dispatch_calls &c);

// One BUILDING order (kind 0x40). 26 arms; see decode_building_order.
void dispatch_building_order(const sim_view &v, sim_store &own, const dispatch_calls &c,
                             const dispatch_ctx &x);

// One ADMIN order -- the fall-through path for every kind the router does not recognise, plus the
// unit/storage kinds' own default. 23 arms; see decode_admin_order.
void dispatch_admin_order(const sim_view &v, sim_store &own, const dispatch_calls &c,
                          const dispatch_ctx &x);

// The kind-0xf0 body @0x004695d4: the lockstep HORIZON-EXTENSION request. It belongs to no table --
// the router jumps straight to it -- and it lives with the admin arms only because it is small and
// adjacent in the listing.
//
// IT MUST NOT BE FOLDED INTO dispatch_admin_order. `decode_admin_order(0xf0)` is a real arm (11), so
// routing kind 0xf0 through the order-code scan would run a genuinely wrong handler rather than
// failing. Raised by the loop TU's translator, which declared this extern rather than guessing --
// exactly the right move, and the reason this declaration is here now.
void dispatch_lockstep_extend(const sim_view &v, sim_store &own, const dispatch_calls &c,
                              const dispatch_ctx &x);

} // namespace detail

// The public wrapper: detail::order_queue_dispatch over state() and live_dispatch_calls().
void order_queue_dispatch();

// ---- the rig's work indicator -----------------------------------------------------------------
//
// SIM1C's acceptance test asks the armed run to report the OPCODE MIX, not the call total, because
// this site is fifty handlers behind two switches: a scenario that only ever sends two opcodes
// leaves the other 47 arms exactly as unverified after a clean 100 000-call run as before it, and
// nothing in `calls=100000 divergences=0` says so.
//
// It lives here, as a pure function over one record, for one reason: IT IS THE INSTRUMENT THAT
// PRODUCES THE EVIDENCE, so it has to be checkable itself. A tally buried in the shadow arm's static
// state can only be read from a rig log, which means a mask that is silently always zero reads as
// "the scenario was weak" -- the same shape as a real answer. As a pure function `simtest` pins it
// against hand-built records in a few lines.
//
// The bits are ARMS, not opcodes. A value the scan rejects sets bit 0, the default arm, which is
// where the dispatcher actually sends it; recording raw opcodes would report 200 distinct
// unrecognised values as 200 kinds of coverage of one arm.
struct dispatch_coverage {
    uint16_t kinds;      // bit k: kind nibble (k << 4) was dispatched
    uint32_t bldg_arms;  // bit n: building-table arm n ran (bit 0 = its default arm, a REAL body)
    uint32_t admin_arms; // bit n: admin-table arm n ran (bit 0 = its default arm, which is empty)
};

// Fold one queue record into the masks. Mirrors the router's nibble decisions -- the unit/storage
// path and the lockstep body are not table arms, so for them the kind bit is the whole story.
void tally_coverage(const order &q, dispatch_coverage &cov);

// Arm the differential oracle over this TU group. Defined in sim_order_dispatch.cpp and wired into
// the module's reimpl_probe_install() by the conductor -- see sim_order_enqueue.h's install_shadow
// for the convention and the SIM1C progress note for why it is per-module, not per-TU.
//
// REFUSES if promotion took the entry first. Install promotion BEFORE this.

// ---- the other oracle: PROMOTE, then compare two RUNS ------------------------------------------
//
// `[promote] sim_dispatch=1` replaces the original outright, so there is no per-call A/B and
// therefore no snapshot, no restore, and no region set to under-declare -- the failure mode that
// cost this site eight extra_regions and five rig runs. The comparison moves
// up a level instead: run the same scenario twice, unpromoted then promoted, and diff the per-step
// state-hash trajectory (`test_ui.py --soak --soak-golden <path>`, which reports the FIRST diverging
// step and region).
//
// The two oracles answer different questions and both are worth having. Shadow localises -- it names
// the call and the region. Promotion integrates -- it runs our body for thousands of steps with
// every downstream consequence live, which is the only way to catch an error that only matters once
// it has propagated. Returns 1 if the entry was taken, 0 if the gate was off or the guard refused.
int install_promotion_dispatch(int default_on);

// The generated entry thunk, for the harness-mediated rebind (D18). See sim_step_entry_thunk().
void *order_queue_dispatch_entry_thunk();

// Mark the promoted arm live when it was reached by REBIND rather than by an entry install (D18).
void mark_promoted_dispatch_installed(bool on);

// ---- carrying an OBSERVER across promotion (D18) ------------------------------------------------
//
// Promotion displaces INSTRUMENTS as readily as it displaces fixes, and it is worse when it does:
// a displaced fix changes behaviour, a displaced instrument changes the NUMBER and the run still
// looks valid. The order RECORDER was that -- a run-before trampoline on this function's entry
// (harness.cpp's dispatch_detour), which under `[promote] orders` could not arm, so the harness
// disabled order recording and said so in a line nobody was reading.
//
// Rather than teach the recorder to re-enter through a rebind (the C4/C6 protocol, which exists
// because those detours must run in BOTH configurations), the promoted entry simply offers the one
// thing the detour provided: a call at the same point, before the body. The harness registers it
// INSTEAD of the trampoline when the entry is promoted, so exactly one mechanism is live per
// configuration and the recorder cannot double-record.
//
// LAYERING, and why this is a setter rather than a direct call: a reimplementation TU must not
// include a seams header (see install_promotion_dispatch's own note). Same arrangement as
// mh::lockstep::set_fixes and mh::hook::set_promotion_logger -- the seam pushes its function in.
// Pass nullptr to clear. NOT called on the shadow arm: shadow runs the body twice per call across a
// snapshot/restore, and an observer with a side effect (this one writes a file) must fire once.
void set_dispatch_observer(void (*fn)());

// What is registered, or nullptr. Exposed so a test can assert the wiring rather than the pointer's
// existence -- the D17 lesson: a pure-function test cannot see whether anything calls it.
void (*dispatch_observer())();

// Call the registered observer, if any. The promoted entry's FIRST statement, split out as its own
// function for one reason: the promoted entry itself reads live game state through `state()` and so
// cannot be driven offline, while this can. Testing it is therefore not the whole wiring -- the
// residual "does the promoted entry call this" is one line, and the rig closes it by producing (or
// not producing) an actual recording under `[promote] orders`. Said plainly because the alternative
// is the D17 trap: a test that covers the arithmetic and not the call.
void fire_dispatch_observer();

} // namespace mh::sim
