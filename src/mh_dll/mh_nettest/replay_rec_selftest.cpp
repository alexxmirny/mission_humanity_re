//
// replay_rec_selftest.cpp -- hotfix rc9.4: the replay record's SEGMENTS (desync/replay_record.h) and the
// seed SANITY scan (desync/seed_sanity.h), offline.
//
//   replay_rec  a session folder in %TEMP%, fake hooks (seed = a small marker file, hash = a counter, a
//               settable clock and planet). A noted save load becomes segment 1 at the NEXT step with the
//               source file's FNV-1a 64; a planet change nobody noted and a clock that goes backwards are
//               cut by the poll (kind=unhooked, hash_before=unknown); a note with no session open is
//               dropped; the segment's post-boundary hash is a checkpoint at its first step; the close
//               counts the segments.
//   sanity      the pure scan over a synthetic seed blob: a clean blob has no finding; each planted bad
//               field (the crash's pip_level, an out-of-range door mutex, a bad target ref, idle > total)
//               is one finding with its own key; free slots are skipped. The async half reports a finding
//               NEW once per session and again in a new session.
//
// No log sink is published in this process, so mh_logq_* writes synchronously and every line is on disk
// when the call returns.
//
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "desync/replay_record.h"
#include "desync/seed_sanity.h"
#include "addr/mh_structs.gen.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

namespace rr = mh::desync::replay_rec;
namespace sn = mh::desync::sanity;

std::string g_log_all;
void        test_log(const char *fmt, ...) {
    char    line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log_all += line;
}

std::string read_all(const std::string &p) {
    std::string s;
    FILE       *f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return s;
    char   buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

bool write_all(const std::string &p, const void *d, size_t n) {
    FILE *f = nullptr;
    if (fopen_s(&f, p.c_str(), "wb") != 0 || !f) return false;
    fwrite(d, 1, n, f);
    fclose(f);
    return true;
}

// ---- fake hooks ------------------------------------------------------------------------------------
double   g_clock  = 10.0;
int      g_planet = 3;
uint64_t g_hash   = 0x1000;
int      g_seeds  = 0;

uint64_t fake_seed(const char *path, int segment, const char *, unsigned long, uint32_t, const char *) {
    char      body[32];
    const int n = _snprintf_s(body, sizeof(body), _TRUNCATE, "SEED%d", segment);
    write_all(path, body, (size_t)n);
    ++g_seeds;
    return (uint64_t)n;
}
uint64_t fake_hash() { return ++g_hash; }
double   fake_clock() { return g_clock; }
int      fake_planet() { return g_planet; }

void test_segments() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "mh_replayrec_selftest\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    for (const char *leaf : {"mh_rec_seed.bin", "mh_rec_seed_1.bin", "mh_rec_seed_2.bin", "mh_rec_seed_3.bin",
                             "mh_rec_hash.txt", "mh_rec_segments.txt", "x.sav"})
        DeleteFileA((dir + leaf).c_str());
    const std::string sav = dir + "x.sav";
    write_all(sav, "abc", 3); // FNV-1a 64("abc") = E71FA2190541574B

    uint64_t fnv = 0, bytes = 0;
    check("file_fnv64 of 'abc' is the FNV-1a 64 reference value",
          rr::file_fnv64(sav.c_str(), &fnv, &bytes) && fnv == 0xE71FA2190541574Bull && bytes == 3);

    rr::hooks h     = {};
    h.seed          = &fake_seed;
    h.hash          = &fake_hash;
    h.configuration = +[]() -> int { return 1; };
    h.clock         = &fake_clock;
    h.planet        = &fake_planet;
    rr::set_compress(false);
    rr::configure(1, 4, h, &test_log);

    g_log_all.clear();
    rr::note_boundary(rr::boundary::save_load, "early", sav.c_str(), -1);
    check("a note with no session folder open is dropped (the step-1 seed covers it)",
          g_log_all.find("no session folder open") != std::string::npos);

    const unsigned long gen = 77;
    for (int s = 1; s <= 5; ++s) {
        g_clock += 0.5;
        rr::step(dir.c_str(), gen, true);
    }
    check("5 steps, no boundary -> no segment", rr::segments() == 0 && rr::current_step() == 5);
    rr::note_boundary(rr::boundary::save_load, "x", sav.c_str(), -1); // the load: clock goes back
    g_clock = 4.0;
    rr::step(dir.c_str(), gen, true); // step 6 = segment 1
    check("a noted save load is segment 1 at the next step", rr::segments() == 1);
    g_clock += 0.5;
    rr::step(dir.c_str(), gen, true); // step 7: the clock moves forward again, no new segment
    g_planet = 5;                     // a planet change nobody noted (a promoted SwitchToPlanet)
    g_clock += 0.5;
    rr::step(dir.c_str(), gen, true); // step 8 = segment 2 (unhooked)
    g_clock -= 2.0;                   // a rewind nobody noted (a resync)
    rr::step(dir.c_str(), gen, true); // step 9 = segment 3 (unhooked)
    rr::step(dir.c_str(), gen, true); // step 10: unchanged
    check("the poll cuts an unnoted planet change and an unnoted rewind", rr::segments() == 3);
    rr::session_close("test");

    const std::string seg = read_all(dir + "mh_rec_segments.txt");
    check("segments file: header + segment 0 at step 1",
          seg.find("; mh_rec_segments v1") == 0 && seg.find("segment 0 step=1 kind=session_start") != std::string::npos);
    check("segment 1: step 6, save_load, clock 8.500 -> 4.000, the save's size and FNV",
          seg.find("segment 1 step=6 kind=save_load prev_step=5 clock_before=12.500 clock_after=4.000") !=
                  std::string::npos &&
              seg.find("file_bytes=3 file_fnv=E71FA2190541574B") != std::string::npos &&
              seg.find("seed=mh_rec_seed_1.bin") != std::string::npos);
    check("segment 2: step 8, unhooked planet change 3 -> 5, no pre-boundary hash",
          seg.find("segment 2 step=8 kind=unhooked prev_step=7") != std::string::npos &&
              seg.find("planet_before=3 planet_after=5") != std::string::npos &&
              seg.find("hash_before=unknown") != std::string::npos);
    check("segment 3: step 9, unhooked rewind", seg.find("segment 3 step=9 kind=unhooked") != std::string::npos &&
                                                    seg.find("the game clock went backwards") != std::string::npos);
    check("the close counts 3 segments", seg.find("; end at step 10: 3 segment(s)") != std::string::npos);
    check("segment seeds written with the step-1 seed", read_all(dir + "mh_rec_seed.bin") == "SEED0" &&
                                                            read_all(dir + "mh_rec_seed_1.bin") == "SEED1" &&
                                                            read_all(dir + "mh_rec_seed_3.bin") == "SEED3");
    const std::string hs = read_all(dir + "mh_rec_hash.txt");
    // checkpoints: 1, 4, 6 (segment), 8 (segment = also the every-4 one, written once), 9 (segment)
    int n8 = 0;
    for (size_t at = 0; (at = hs.find("\n8 ", at)) != std::string::npos; ++at) ++n8;
    check("checkpoints at steps 1, 4 and every segment start; step 8 once",
          hs.find("\n1 ") != std::string::npos && hs.find("\n4 ") != std::string::npos &&
              hs.find("\n6 ") != std::string::npos && hs.find("\n9 ") != std::string::npos && n8 == 1);
    check("the hash file's checkpoint at a segment start is the post-boundary hash on its segment line",
          [&] {
              const size_t      a  = seg.find("segment 1 ");
              const size_t      b  = seg.find("hash_after=", a);
              const std::string ha = seg.substr(b + 11, 16);
              return hs.find("\n6 " + ha) != std::string::npos;
          }());
    rr::configure(0, 0, h, nullptr);
}

// ---- the sanity scan -------------------------------------------------------------------------------
using mh::game::mh_map_object_building;
using mh::game::mh_map_object_unit;
using mh::game::mh_map_object_unit_storage;

struct blob_t {
    std::vector<uint8_t> b;
    sn::layout           L;
};

blob_t clean_blob() {
    blob_t       t;
    const size_t nb = sizeof(mh_map_object_building) * 800, nu = sizeof(mh_map_object_unit) * 800,
                 ns  = sizeof(mh_map_object_unit_storage) * 200;
    t.L.buildings    = 16;
    t.L.units        = 16 + (int64_t)nb;
    t.L.unit_storage = 16 + (int64_t)(nb + nu);
    t.b.assign(16 + nb + nu + ns + 16, 0);
    // one live building and one live unit per player, every field legal
    for (int p = 0; p < 8; ++p) {
        auto *bd             = reinterpret_cast<mh_map_object_building *>(&t.b[(size_t)t.L.buildings]) + p * 100 + 10;
        bd->building_id      = 7;
        bd->state            = 100;
        bd->pip_active_count = 3;
        bd->pip_level[0]     = 4;
        auto *u              = reinterpret_cast<mh_map_object_unit *>(&t.b[(size_t)t.L.units]) + p * 100 + 5;
        u->unit_proto_id     = 3;
        u->state             = 1;
        u->home_storage_slot = 0xff;
        u->path_slot_id      = 0xff;
        u->selected_weapon   = 100;
        u->ai_group_index    = 0xffff;
        u->target_ref        = 0x41;
        u->target_index      = 10;
    }
    return t;
}

sn::extras states_ok() {
    sn::extras X;
    memset(X.unit_state_ok, 1, sizeof(X.unit_state_ok));
    memset(X.bldg_state_ok, 1, sizeof(X.bldg_state_ok));
    X.unit_state_ok[255] = X.bldg_state_ok[255] = 0;
    X.have_states                               = true;
    memset(X.pop, 0, sizeof(X.pop));
    X.have_pop = true;
    return X;
}

std::vector<std::string> g_keys;
void                     collect(void *, const char *key, const char *) { g_keys.push_back(key); }

void test_sanity() {
    blob_t     t = clean_blob();
    sn::extras X = states_ok();
    g_keys.clear();
    check("a clean seed has no finding", sn::scan(t.b.data(), (uint32_t)t.b.size(), t.L, X, &collect, nullptr) == 0);

    auto *bd                = reinterpret_cast<mh_map_object_building *>(&t.b[(size_t)t.L.buildings]) + 0 * 100 + 10;
    bd->pip_level[0]        = 0x0384DE82; // the crash_7908b value
    auto *free_slot         = reinterpret_cast<mh_map_object_building *>(&t.b[(size_t)t.L.buildings]) + 0 * 100 + 11;
    free_slot->pip_level[1] = 99; // building_id 0: a free slot, not scanned
    auto *st                = reinterpret_cast<mh_map_object_unit_storage *>(&t.b[(size_t)t.L.unit_storage]) + 1 * 25 + 2;
    st->door_mutex_unit     = 4000;
    auto *u                 = reinterpret_cast<mh_map_object_unit *>(&t.b[(size_t)t.L.units]) + 2 * 100 + 5;
    u->target_index         = 300;
    X.pop[3 * 0x34 + 0x0c]  = 50; // player 3 idle 50, total 0
    g_keys.clear();
    const int n   = sn::scan(t.b.data(), (uint32_t)t.b.size(), t.L, X, &collect, nullptr);
    auto      has = [&](const char *k) {
        for (auto &s : g_keys)
            if (s == k) return true;
        return false;
    };
    check("four planted fields -> four findings (the free slot is skipped)", n == 4);
    check("the crash's pip_level is found by its key", has("buildings:0:10:pip_level0"));
    check("door_mutex_unit 4000 is found", has("unit_storage:1:2:door_mutex_unit"));
    check("a target index of 300 is found", has("units:2:5:target"));
    check("idle > total is found", has("pop_stats:3:0:idle_gt_total"));

    // the async half: NEW once per session, again in a new session
    sn::configure(1, &test_log);
    for (int round = 0; round < 3; ++round) {
        uint8_t *copy = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, t.b.size()));
        memcpy(copy, t.b.data(), t.b.size());
        sn::scan_async(copy, (uint32_t)t.b.size(), t.L, X, round < 2 ? 5 : 6, round, "save_load", 100 + round, "");
        check("the scan finishes", sn::wait_idle(5000));
        if (round == 0) check("first seed of a session: 4 NEW", sn::last_findings() == 4 && sn::last_new() == 4);
        if (round == 1) check("second seed, same session: 4 found, 0 new", sn::last_findings() == 4 && sn::last_new() == 0);
        if (round == 2) check("a new session reports them NEW again", sn::last_new() == 4);
    }
    check("the NEW line names the seed's segment and boundary",
          g_log_all.find("sanity NEW seg=0 step=100 boundary=save_load: buildings p0 i10 id 7: pip_level[0]=59039362") !=
              std::string::npos);
    check("every scan logs its time off the game thread", g_log_all.find("ms off the game thread") != std::string::npos);
    sn::configure(0, nullptr);
}

} // namespace

int run_replayrectest() {
    g_checks = g_fails = 0;
    test_segments();
    test_sanity();
    printf("replayrectest: %d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) printf("--- log ---\n%s", g_log_all.c_str());
    return g_fails ? 1 : 0;
}
