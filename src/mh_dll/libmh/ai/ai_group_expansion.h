//
// ai/ai_group_expansion.h -- llm_strat_ai_group_expansion_form_or_repurpose (RI-AI / AI1C layer 2).
//
// llm_strat_ai_group_expansion_form_or_repurpose @0x004e769e (0x29a bytes)
//
// Runs once per tick per AI player. First SCANS the player's existing groups for one already on
// the expansion mission (goal == 3), stopping at the first match. What happens next depends on
// ai_resource_shortage_state (a small 0..3 code the sim itself recomputes elsewhere):
//
//   - a match found, shortage_state == 2  -> BAIL entirely, no action at all (the group already
//     out there is left to keep going while resources are still critically short).
//   - a match found, shortage_state == 3  -> REPURPOSE that group: drain its whole task queue
//     (group_task_dequeue while task_queue_count != 0), then push rally-formup (task 2), advance-
//     to-home-anchor (task 0xc, args = ai_home_tile_x/y) and finally DISBAND (task 0x14). Returns
//     after the disband enqueue.
//   - a match found, shortage_state == 0 or 1 -> the match doesn't trigger anything; the scan just
//     keeps going (0x004e76c2/0x004e76de both fall through to the INC-and-continue at 0x004e7780).
//   - no match anywhere in 0..ai_group_count -> falls out of the loop to the SAME shortage-state
//     test the "no group yet" case reads, but this time only shortage_state == 2 proceeds; anything
//     else (including 3, with no group to repurpose) is a silent no-op return.
//
// FORM PATH (reached only on "no repurposable group found" AND shortage_state == 2):
//
//   Pick the weakest HOSTILE opponent: for every OTHER active player q with
//   ai_player_relation[q] <= -1 (the original spells the test as `> -1` and skips -- the field
//   comment on ai_player_relation confirms -1/+1 are the only two values it is ever initialised to
//   and that every hostility test in this module is a sign test), take the first such q
//   unconditionally and thereafter replace the running "weakest" only when a candidate's
//   total_unit_power is UNSIGNED-less than the current weakest's -- the identical selection idiom
//   ai_invasion.cpp and ai_army_milestone.cpp already document. If no candidate qualifies, return
//   with nothing formed.
//
//   target_member_count = (ai_groups[0].member_count + ai_groups[2].member_count) * 80 / 100 --
//   BOTH READ MOVZX even though member_count is a signed int16_t field; these are two of
//   llm_strat_spawn_ai_base's five SEED groups (see ai_group_form.h). A zero result bails with
//   nothing formed.
//
//   Two OUTPUT-POINTER calls fill four locals: bldg_find_mother_position_indexed(weakest) fills
//   (mother_x, mother_y); pick_owned_tile_or_home(player_id) fills (home_x, home_y). Then
//   group_create; -1 is a real early return here (0x004e787f). On success the new group is stamped
//   goal=3, target_player_id=weakest (a FULL DWORD -- see the struct field's comment, corroborated
//   by a raw-pointer scan finding the old "padding" bytes a total orphan) and
//   active_member_count=(uint16_t)target_member_count, THEN four tasks are enqueued: muster-from-
//   pool (0x11, args = home_x/home_y, p9 = target_member_count), advance-to-anchor (3, args =
//   mother_x/mother_y), nudge-stragglers (6, args = mother_x/mother_y, p9 = 15), and finally
//   recall-home (8, all zero).
//
// THE ONE THING THAT MUST NOT BE OPTIMISED AWAY: between the first and second FORM enqueue calls,
// the original calls rand_below_ai(4) and DISCARDS the result (0x004e78c9-0x004e78ce; EAX is
// overwritten four instructions later at 0x004e78eb). It is pure side effect -- it advances the AI
// PRNG (channel 2) -- and dropping it would consume one fewer draw than the original and desync the
// sim this translation is measured inside.
//
// THE SHARED TAIL. The form path's LAST enqueue (task 8) and the repurpose path's LAST enqueue
// (task 0x14) both go through the SAME physical instruction pair at 0x004e7925 (MOV EDX,EDI /
// MOV EAX,ESI / CALL group_task_enqueue) -- the task_code (EBX) and param_4 (ECX) are already loaded
// before the JMP into it. The exact same tail is ALSO where llm_strat_ai_invasion_launch_attack_group
// (libmh/ai/ai_invasion.cpp) lands, with task_code 0. Nothing about the sharing is observable in this
// translation -- the arguments are already assembled at each call site -- but it is why the two
// listings interleave physically in the image.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID. Most calls return having written nothing beyond a possible drain of an
// existing group's task queue -- these fields are the anti-vacuity evidence for the shadow arm.
struct group_expansion_report {
    // ---- outcome of the "is one already on the expansion mission" scan ----
    int32_t groups_scanned     = 0;     // groups examined by the scan (stops at the first goal==3 match)
    bool    repurpose_blocked  = false; // matched goal==3, shortage_state==2: bail, no action at all
    bool    repurposed         = false; // matched goal==3, shortage_state==3: drained + rally/anchor/disband
    int32_t repurposed_group   = -1;
    int32_t repurpose_dequeues = 0; // group_task_dequeue calls the drain loop made

    // ---- the FORM path (no repurposable group found) ----
    bool no_existing_match_wrong_state = false; // scan found nothing actionable AND
                                                // shortage_state != 2 at the second gate: no-op
    int32_t opponents_scanned   = 0;            // non-self candidates evaluated by the weakest-opponent scan
    bool    no_opponent_found   = false;        // no candidate had ai_player_relation <= -1
    int32_t weakest_opponent    = -1;
    int32_t target_member_count = 0;     // (ai_groups[0] + ai_groups[2]).member_count * 80 / 100
    bool    zero_member_target  = false; // target_member_count computed as 0: bail, nothing formed
    bool    create_failed       = false; // group_create returned -1
    bool    formed              = false; // a new group was created, stamped and tasked
    int32_t group_index         = -1;
    int32_t tasks_enqueued      = 0; // group_task_enqueue calls made, either path
};

// llm_strat_ai_group_expansion_form_or_repurpose @0x004e769e.
group_expansion_report group_expansion_form_or_repurpose(const ai_view &v, const ai_store &own,
                                                         const ai_calls &gc, uint32_t player_id);

} // namespace detail

void group_expansion_form_or_repurpose(uint32_t player_id);

} // namespace mh::ai
