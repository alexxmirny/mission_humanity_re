//
// info_guard_selftest.cpp -- `net_selftest.exe infoguardtest`: mp:U73, the entity-info-screen guard's
// decision (seams/ui_info_guard.h).
//
// THE FIELD STATE. A docked-unit info request names a row of the storage panel's list; the panel
// reads docked_units[row + scroll - 1] when it consumes it. After the list shrinks (launch / death /
// the sim's purge) that slot is stale or zero, units[side][0].unit_proto_id is 0, Unit[0].info_txt is
// "" and retail raises "Error: Cannot find info text:" with an empty key. The arms below drive the
// decision with exactly those states, then with the valid ones it must leave alone.
//
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "seams/ui_info_guard.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

} // namespace

int run_infoguardtest() {
    printf("=== infoguardtest (mp:U73: the entity info screen guard) ===\n");
    using namespace mh::info_guard;

    // A. KEY SHAPE: the blank slot is the field defect; a real key and a 64-byte unterminated field.
    {
        char blank[INFO_KEY_BYTES] = {0};
        check("blank slot -> key_empty", key_shape(blank) == verdict::key_empty);
        char good[INFO_KEY_BYTES] = {0};
        strcpy(good, "obcy_b\\O_PORT.INF");
        check("configured key -> ok", key_shape(good) == verdict::ok);
        char full[INFO_KEY_BYTES];
        memset(full, 'A', sizeof(full));
        check("64 bytes, no NUL -> key_unterminated", key_shape(full) == verdict::key_unterminated);
        char edge[INFO_KEY_BYTES];
        memset(edge, 'A', sizeof(edge));
        edge[INFO_KEY_BYTES - 1] = 0;
        check("63 chars + NUL -> ok", key_shape(edge) == verdict::ok);
    }

    // B. ID RANGE: the cfg tables are 100 deep; total is advisory (0 = unknown).
    {
        check("id 0 in range", id_in_range(KIND_UNIT, 0, 0));
        check("id 99 in range", id_in_range(KIND_BUILDING, 99, 0));
        check("id 100 out of range", !id_in_range(KIND_PROJECT, 100, 0));
        check("negative id out of range", !id_in_range(KIND_UNIT, -1, 0));
        check("id past a known total refused", !id_in_range(KIND_UNIT, 60, 40));
        check("id == total accepted (inclusive)", id_in_range(KIND_UNIT, 40, 40));
        check("projects have no total", id_in_range(KIND_PROJECT, 60, 40));
    }

    // C. THE STORAGE REQUEST against the docked list as it is NOW.
    {
        // 3 docked units, ids 7 / 9 / 12, all configured.
        check("row 1 of 3 ok", storage_request_check(1, 0, 3, 7, 100, 21) == storage_row::ok);
        check("row 3 of 3 ok", storage_request_check(3, 0, 3, 12, 100, 21) == storage_row::ok);
        check("scroll folded in: request 1 + scroll 2 = row 3", storage_request_check(1, 2, 3, 12, 100, 21) == storage_row::ok);
        // THE FIELD CASE: the click named row 3, a unit left, the list is now 2 long.
        check("row 3 of 2 -> past the list", storage_request_check(3, 0, 2, 0, 100, 0) == storage_row::row_past_list);
        check("list emptied -> past the list", storage_request_check(1, 0, 0, 0, 100, 0) == storage_row::row_past_list);
        check("scroll pushes the row past the list", storage_request_check(2, 2, 3, 12, 100, 21) == storage_row::row_past_list);
        check("row 0 refused", storage_request_check(0, 0, 3, 7, 100, 21) == storage_row::row_past_list);
        check("negative count clamps to 0", storage_request_check(1, 0, -4, 7, 100, 21) == storage_row::row_past_list);
        check("count over 50 clamps to the 50-entry list", storage_request_check(51, 0, 80, 7, 100, 21) == storage_row::row_past_list);
        check("row 50 of 80 ok (list is 50 deep)", storage_request_check(50, 0, 80, 7, 100, 21) == storage_row::ok);
        check("docked slot past the roster", storage_request_check(1, 0, 3, 100, 100, 21) == storage_row::unit_slot_range);
        check("docked slot negative", storage_request_check(1, 0, 3, -1, 100, 21) == storage_row::unit_slot_range);
        check("raised roster accepts slot 250", storage_request_check(1, 0, 3, 250, 500, 21) == storage_row::ok);
        check("slot names a blank unit (proto 0)", storage_request_check(1, 0, 3, 7, 100, 0) == storage_row::unit_proto_blank);
    }

    // D. THE RIG KNOB: the n-th call, once.
    {
        int c = 0;
        check("n=0 never fires", !test_hit(0, c) && c == 0);
        int s = 0;
        check("n=2: call 1 clean", !test_hit(2, s));
        check("n=2: call 2 fires", test_hit(2, s));
        check("n=2: call 3 clean again", !test_hit(2, s));
    }

    // E. names are stable (the log needles and tools/check_info_guard.py read them).
    check("verdict name key-empty", strcmp(verdict_name(verdict::key_empty), "key-empty") == 0);
    check("verdict name id-out-of-range", strcmp(verdict_name(verdict::id_range), "id-out-of-range") == 0);
    check("row name", strcmp(storage_row_name(storage_row::row_past_list), "row-past-docked-list") == 0);

    printf("infoguardtest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
