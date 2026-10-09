//
// run_context_selftest.cpp -- `net_selftest.exe runctxtest`: WHERE the run context puts a process's
// logs (dist LA13). Two roots, one rule: `<config dir>\logs\` (= `<exedir>\logs\` in portable mode) unless MH_LOG_ROOT was in the environment
// at start, in which case that directory -- and the breadcrumb (`mh_run.txt`) goes into whichever
// root won, never beside an exe that a non-elevated process cannot really write next to.
//
// WHY A CHILD PROCESS. mh_common/run_context.cpp decides ONCE per process (`ensure_init`, first
// caller wins), and the decision reads the environment. So the arms that need MH_LOG_ROOT set are
// run in a spawned copy of this exe (`runctxtest --child <outfile>`) that has never asked for its
// run dir: the child writes what it decided into the file and exits; the parent reads it back. The
// in-process arm is the hand-launch default, which is also what every other suite in this exe has
// always exercised without saying so.
//
// The three questions, each a clause of LA13's done_when:
//   1. no variable          -> "<cfgdir>logs", run dir under it, breadcrumb in the config dir  (3)
//   2. MH_LOG_ROOT=<dir>    -> that dir (created; trailing separator stripped), run dir under
//                              it, breadcrumb INSIDE it                                        (1)
//   3. MH_LOG_ROOT=<unmakeable> -> the hand-launch default again, never nothing
//
// RL3 (2026-10) ADDED THE OTHER HALF OF "WHERE": the config directory itself (mh_net.ini, mh_key.txt,
// the logs fallback, mh_run.txt -- include/mh_config_dir.h). <cfgdir> above is MH_CONFIG_DIR, else the
// exe directory when an mh_net.ini sits beside it (portable), else
// %LOCALAPPDATA%\MissionHumanity\games\<hash16>\. Five groups, all in `config_dir_arms()`:
//   A. SHA-256 against FIPS 180-4 / NIST vectors, incl. the 55/56/63/64/65-byte padding boundaries
//   B. hash16 against the SHARED VECTORS -- the same ten strings src/launcher/src/paths.rs asserts
//      (`game_dir_hash_shared_vectors`). If you change one table, change the other.
//   C. the resolution order, through the pure resolver with temp dirs standing in for the exe dir and
//      %LOCALAPPDATA% (env beats portable beats user storage; every unusable rule falls through)
//   D. the cached accessors: env override, "computed once", and 8 racing first callers agreeing
//   E. two children (the accessor is per-process): MH_CONFIG_DIR, and the hand launch with only
//      LOCALAPPDATA set -- whose logs root, breadcrumb and run dir must land under the config dir
//
// net_selftest's main() pins MH_CONFIG_DIR to its own directory (see there), so every OTHER suite is
// portable; arms C-E build their own environments and never depend on that pin.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <windows.h>

#include "mh_config_dir.h"
#include "mh_run_context.h"

extern "C" const char *MH_LogsRoot(void); // run_context.cpp; not in the shared header (see there)

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

bool starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool read_file(const char *path, char *dst, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(dst, 1, (size_t)cap - 1, f);
    fclose(f);
    dst[n] = '\0';
    return true;
}

// Split "<logs_root>\n<run_dir>\n<proc_dir>\n" as the child writes it.
struct ChildAnswer {
    char logs_root[MAX_PATH];
    char run_dir[MAX_PATH];
    char proc_dir[MAX_PATH];
};

bool parse_answer(const char *text, ChildAnswer *a) {
    char *fields[3] = {a->logs_root, a->run_dir, a->proc_dir};
    int   fi = 0, di = 0;
    for (const char *p = text; *p && fi < 3; ++p) {
        if (*p == '\n') {
            fields[fi][di] = '\0';
            ++fi;
            di = 0;
        } else if (*p != '\r' && di < MAX_PATH - 1) {
            fields[fi][di++] = *p;
        }
    }
    return fi == 3;
}

// Run this exe as `runctxtest --child <outfile>` with MH_LOG_ROOT=<root> (or unset when root is
// null), wait for it, read its answer.
bool spawn_child(const char *root, const char *outfile, ChildAnswer *answer) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    SetEnvironmentVariableA("MH_LOG_ROOT", root); // nullptr deletes the variable
    char cmd[MAX_PATH * 3];
    wsprintfA(cmd, "\"%s\" runctxtest --child \"%s\"", exe, outfile);
    STARTUPINFOA        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    BOOL                ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableA("MH_LOG_ROOT", nullptr);
    if (!ok) {
        printf("  cannot spawn %s (err %lu)\n", cmd, GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    char text[MAX_PATH * 3 + 8];
    if (code != 0 || !read_file(outfile, text, (int)sizeof(text))) {
        printf("  child exit %lu, answer file %s\n", code, outfile);
        return false;
    }
    return parse_answer(text, answer);
}

int child_main(const char *outfile) {
    FILE *f = fopen(outfile, "wb");
    if (!f) return 2;
    fprintf(f, "%s\n%s\n%s\n", MH_LogsRoot(), MH_RunDir(), MH_ProcessDir());
    fclose(f);
    return 0;
}

// RL3's child: what the config accessors and the run context decided, one field per line --
// source, config dir, logs root, run dir. Asking MH_RunDir() is what makes run_context compose its
// logs root (and write its breadcrumb) from the config dir, which is half of what arm E asserts.
int cfg_child_main(const char *outfile) {
    FILE *f = fopen(outfile, "wb");
    if (!f) return 2;
    fprintf(f, "%d\n%s\n", (int)mh::cfgdir::source(), mh::cfgdir::config_dir());
    fprintf(f, "%s\n%s\n", MH_LogsRoot(), MH_RunDir());
    fclose(f);
    return 0;
}

// ---- RL3: the config directory --------------------------------------------------------------------

std::string hex_of(const unsigned char *d, int n) {
    static const char hx[] = "0123456789abcdef";
    std::string       s;
    for (int i = 0; i < n; ++i) {
        s += hx[d[i] >> 4];
        s += hx[d[i] & 15];
    }
    return s;
}

std::string sha_hex(const std::string &msg) {
    unsigned char out[32];
    mh::cfgdir::detail::sha256((const unsigned char *)msg.data(), (unsigned)msg.size(), out);
    return hex_of(out, 32);
}

std::string hash16(const char *utf8) {
    char h[17];
    if (!mh::cfgdir::game_dir_hash16(utf8, h)) return "";
    return h;
}

// Best-effort recursive delete: the scratch tree holds a handful of files and a created logs\ tree.
void rmtree(const std::string &dir) {
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                rmtree(p);
            } else {
                SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(p.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

bool is_dir(const std::string &p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_ascii(const std::string &s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

void touch(const std::string &p) {
    HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return L"";
    const int    n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

// A.  SHA-256 -- the FIPS 180-4 / NIST examples plus the padding boundaries (55 bytes is the last
// length whose pad fits one block, 56 the first that needs two, 64 a whole block).
void sha256_arm() {
    check("sha256(\"\")", sha_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("sha256(\"abc\")", sha_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("sha256(448-bit NIST message)",
          sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    struct {
        int         n;
        const char *want;
    } const a_runs[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (const auto &r : a_runs) {
        char what[48];
        wsprintfA(what, "sha256('a' x %d)", r.n);
        check(what, sha_hex(std::string((size_t)r.n, 'a')) == r.want);
    }
}

// B.  THE SHARED VECTORS. src/launcher/src/paths.rs `game_dir_hash_shared_vectors` asserts these exact
// pairs against the Rust implementation; any edit here is an edit there. Non-ASCII rows are spelled
// as UTF-8 byte escapes so the table is independent of this file's source encoding:
//   D:\<Cyrillic "Igry">\Mission Humanity\   and   C:\Games\Caf<e-acute>\MH (+ its upper-case twin)
struct vec_row {
    const char *path;
    const char *want;
};
const vec_row kVectors[] = {
    {"C:\\Games\\Mission Humanity", "a28ebc0acf5d4635"},
    {"c:\\games\\mission humanity\\", "a28ebc0acf5d4635"},
    {"C:\\Program Files (x86)\\Mission Humanity", "74afbc828fb7580a"},
    {"D:\\\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\\Mission Humanity\\", "d5c79400d9f5c14a"},
    {"D:\\\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\\MISSION humanity", "d5c79400d9f5c14a"},
    {"C:\\", "826c0d7d3c42f5c5"},
    {"C:/Games/MH//", "5bb58c0657eced3f"},
    {"\\\\server\\share\\MH", "55845b4645068bc3"},
    {"C:\\Games\\Caf\xC3\xA9\\MH", "1eb55528359e1719"},
    {"C:\\Games\\CAF\xC3\x89\\MH", "137c4033bad61292"}, // non-ASCII upper case is NOT folded
};

void hash_vectors_arm() {
    for (const vec_row &v : kVectors) {
        const std::string got = hash16(v.path);
        check((std::string("hash16 ") + v.path).c_str(), got == v.want);
    }
    // The rule's two negatives, so a rewrite cannot "agree" by folding too much: ONLY ASCII case is
    // folded (the Cyrillic row above would otherwise change), and a different folder differs.
    check("hash16: different folders differ", hash16("C:\\Games\\A") != hash16("C:\\Games\\B"));
    check("hash16: a NULL path is refused", [] {
        char h[17];
        return !mh::cfgdir::game_dir_hash16(nullptr, h);
    }());
}

// C.  The resolution order through the pure resolver. Temp directories stand in for the exe directory
// and for %LOCALAPPDATA%; nothing here reads the real environment or touches the real profile.
void resolve_order_arm(const std::string &base) {
    const std::string exe_plain  = base + "\\exe_plain\\";  // no mh_net.ini
    const std::string exe_ini    = base + "\\exe_ini\\";    // mh_net.ini beside it -> portable
    const std::string exe_dirini = base + "\\exe_dirini\\"; // a DIRECTORY named mh_net.ini
    const std::string localapp   = base + "\\local";
    const std::string envdir     = base + "\\env\\nested\\cfg\\";
    CreateDirectoryA((base + "\\exe_plain").c_str(), nullptr);
    CreateDirectoryA((base + "\\exe_ini").c_str(), nullptr);
    CreateDirectoryA((base + "\\exe_dirini").c_str(), nullptr);
    CreateDirectoryA((base + "\\exe_dirini\\mh_net.ini").c_str(), nullptr);
    CreateDirectoryA(localapp.c_str(), nullptr);
    touch(exe_ini + "mh_net.ini");
    const std::wstring wlocal = widen(localapp);

    auto run = [&](const std::string &exe, const char *env, const wchar_t *local, mh::cfgdir::result &r) {
        mh::cfgdir::inputs in;
        in.exe_dir       = exe.c_str();
        in.game_dir_utf8 = exe.c_str(); // ASCII temp paths: ANSI == UTF-8
        in.env_dir       = env;
        in.local_appdata = local;
        return mh::cfgdir::resolve(in, r);
    };
    mh::cfgdir::result r;

    // 1. env beats everything -- even a portable exe. Created, nested, trailing separator normalised.
    check("env: SRC_ENV", run(exe_ini, envdir.c_str(), wlocal.c_str(), r) == mh::cfgdir::SRC_ENV);
    check("env: dir is the variable with ONE trailing backslash", envdir == r.dir);
    check("env: directory was created", is_dir(base + "\\env\\nested\\cfg"));
    std::string no_slash = envdir.substr(0, envdir.size() - 1);
    run(exe_ini, no_slash.c_str(), wlocal.c_str(), r);
    check("env: a value without a trailing separator gets one", envdir == r.dir);

    // 2. no env, ini beside the exe -> portable: the exe's own directory, nothing created elsewhere.
    check("portable: SRC_PORTABLE", run(exe_ini, nullptr, wlocal.c_str(), r) == mh::cfgdir::SRC_PORTABLE);
    check("portable: dir is the exe dir", exe_ini == r.dir);
    check("portable: an empty env value is no env", run(exe_ini, "", wlocal.c_str(), r) == mh::cfgdir::SRC_PORTABLE);
    check("portable: user storage was NOT created for a portable run",
          !is_dir(localapp + "\\MissionHumanity"));

    // 3. neither -> %LOCALAPPDATA%\MissionHumanity\games\<hash16>\, created, hash = the launcher's.
    const std::string want_hash = hash16(exe_plain.c_str());
    const std::string want_user = localapp + "\\MissionHumanity\\games\\" + want_hash + "\\";
    check("user: SRC_USER", run(exe_plain, nullptr, wlocal.c_str(), r) == mh::cfgdir::SRC_USER);
    check("user: dir is LOCALAPPDATA\\MissionHumanity\\games\\<hash16>\\", want_user == r.dir);
    check("user: directory was created", is_dir(want_user.substr(0, want_user.size() - 1)));
    check("user: hash16 is 16 hex digits", want_hash.size() == 16);
    // The hash is spelling-blind (the launcher's rule): same folder, different case/separator -> same dir.
    {
        std::string alt = exe_plain.substr(0, exe_plain.size() - 1); // no trailing backslash
        for (char &c : alt) c = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        mh::cfgdir::inputs in;
        in.exe_dir       = exe_plain.c_str();
        in.game_dir_utf8 = alt.c_str();
        in.env_dir       = nullptr;
        in.local_appdata = wlocal.c_str();
        mh::cfgdir::result r2;
        mh::cfgdir::resolve(in, r2);
        check("user: the game-dir spelling (case, trailing separator) does not change the folder",
              want_user == r2.dir);
    }
    mh::cfgdir::result r3;
    run(exe_ini + "x\\", nullptr, wlocal.c_str(), r3); // a different game dir (no ini there)
    check("user: a different game dir gets a different folder", want_user != r3.dir && r3.source == mh::cfgdir::SRC_USER);

    // 4. a DIRECTORY called mh_net.ini is not an ini: not portable.
    check("a directory named mh_net.ini is not portable",
          run(exe_dirini, nullptr, wlocal.c_str(), r) == mh::cfgdir::SRC_USER);

    // 4b. RL4: one-time adoption of a legacy exe-side key by a hand-launch player (user storage only).
    {
        auto put = [](const std::string &p, const char *text) {
            FILE *f = fopen(p.c_str(), "wb");
            if (f) {
                fputs(text, f);
                fclose(f);
            }
        };
        auto get = [](const std::string &p) {
            char buf[64] = {0};
            read_file(p.c_str(), buf, sizeof(buf));
            return std::string(buf);
        };
        const std::string  legacy = exe_plain + "mh_key.txt";
        mh::cfgdir::result ru;
        run(exe_plain, nullptr, wlocal.c_str(), ru);
        const std::string adopted = std::string(ru.dir) + "mh_key.txt";
        DeleteFileA(adopted.c_str());
        DeleteFileA(legacy.c_str());
        check("adopt: no exe-side key, nothing adopted", !mh::cfgdir::adopt_legacy_key(exe_plain.c_str(), ru));
        put(legacy, "legacy-key");
        check("adopt: the exe-side key is copied into user storage",
              mh::cfgdir::adopt_legacy_key(exe_plain.c_str(), ru) && get(adopted) == "legacy-key");
        check("adopt: a copy, not a move (the launcher's migration owns removal)", get(legacy) == "legacy-key");
        put(adopted, "new-key");
        check("adopt: a key already in the config dir is never overwritten",
              !mh::cfgdir::adopt_legacy_key(exe_plain.c_str(), ru) && get(adopted) == "new-key");
        DeleteFileA(adopted.c_str());
        mh::cfgdir::result rp;
        run(exe_ini, nullptr, wlocal.c_str(), rp); // portable
        check("adopt: not for portable mode (the config dir IS the exe dir)",
              !mh::cfgdir::adopt_legacy_key(exe_ini.c_str(), rp));
        mh::cfgdir::result re;
        run(exe_plain, envdir.c_str(), wlocal.c_str(), re); // explicit env override
        check("adopt: not for an explicit MH_CONFIG_DIR", !mh::cfgdir::adopt_legacy_key(exe_plain.c_str(), re));
        check("adopt: nothing was written by the refusals", get(adopted).empty());
        DeleteFileA(legacy.c_str());
    }

    // 5. every rule that cannot be honoured falls through to the next.
    {
        const std::string blocker = base + "\\blocker";
        touch(blocker);
        const std::string  under_file = blocker + "\\sub";
        const std::wstring wblock     = widen(blocker);
        check("unusable env (under a file) falls through to portable",
              run(exe_ini, under_file.c_str(), wlocal.c_str(), r) == mh::cfgdir::SRC_PORTABLE);
        check("unusable env falls through to user storage",
              run(exe_plain, under_file.c_str(), wlocal.c_str(), r) == mh::cfgdir::SRC_USER);
        check("unusable LOCALAPPDATA (under a file) falls back to the exe dir",
              run(exe_plain, nullptr, wblock.c_str(), r) == mh::cfgdir::SRC_EXE && exe_plain == r.dir);
        check("no LOCALAPPDATA at all falls back to the exe dir",
              run(exe_plain, nullptr, nullptr, r) == mh::cfgdir::SRC_EXE && exe_plain == r.dir);
        check("empty LOCALAPPDATA falls back to the exe dir",
              run(exe_plain, nullptr, L"", r) == mh::cfgdir::SRC_EXE && exe_plain == r.dir);
        DeleteFileA(blocker.c_str());
    }
}

struct race_ctx {
    const char *volatile seen;
    volatile LONG go;
};
DWORD WINAPI race_thread(LPVOID p) {
    race_ctx *c = (race_ctx *)p;
    while (!c->go) Sleep(0);
    c->seen = mh::cfgdir::config_dir();
    return 0;
}

// D.  The cached accessors, in THIS process. The pin net_selftest main() set is saved and restored.
void accessor_arm(const std::string &base) {
    char              saved[MAX_PATH];
    const DWORD       sn     = GetEnvironmentVariableA("MH_CONFIG_DIR", saved, MAX_PATH);
    const std::string first  = base + "\\acc_one\\";
    const std::string second = base + "\\acc_two\\";

    SetEnvironmentVariableA("MH_CONFIG_DIR", first.c_str());
    mh::cfgdir::reset_cache_for_test();
    check("accessor: MH_CONFIG_DIR is honoured", first == mh::cfgdir::config_dir());
    check("accessor: source() is SRC_ENV", mh::cfgdir::source() == mh::cfgdir::SRC_ENV);
    check("accessor: an env override is not 'portable'", !mh::cfgdir::portable());
    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini);
    check("accessor: ini_path() is <config dir>mh_net.ini", (first + "mh_net.ini") == ini);
    char key[MAX_PATH];
    check("accessor: join() composes a leaf", mh::cfgdir::join(key, MAX_PATH, "mh_key.txt") &&
                                                  (first + "mh_key.txt") == key);
    char tiny[8];
    check("accessor: join() refuses a leaf that does not fit", !mh::cfgdir::join(tiny, (int)sizeof(tiny), "mh_key.txt"));

    // COMPUTED ONCE: changing the environment afterwards does not move a cached answer.
    SetEnvironmentVariableA("MH_CONFIG_DIR", second.c_str());
    check("accessor: the answer is cached (environment change ignored)", first == mh::cfgdir::config_dir());
    mh::cfgdir::reset_cache_for_test();
    check("accessor: a reset re-resolves", second == mh::cfgdir::config_dir());

    // G104: any number of FIRST callers racing agree, and none sees a half-built answer.
    mh::cfgdir::reset_cache_for_test();
    race_ctx ctx[8];
    HANDLE   th[8];
    for (int i = 0; i < 8; ++i) {
        ctx[i].seen = nullptr;
        ctx[i].go   = 0;
        th[i]       = CreateThread(nullptr, 0, race_thread, &ctx[i], 0, nullptr);
    }
    for (int i = 0; i < 8; ++i) ctx[i].go = 1;
    WaitForMultipleObjects(8, th, TRUE, 10000);
    bool agree = true;
    for (int i = 0; i < 8; ++i) {
        CloseHandle(th[i]);
        agree = agree && ctx[i].seen != nullptr && second == ctx[i].seen;
    }
    check("accessor: 8 racing first callers all see the same complete directory", agree);

    // put the world back the way main() left it
    SetEnvironmentVariableA("MH_CONFIG_DIR", sn ? saved : nullptr);
    mh::cfgdir::reset_cache_for_test();
}

struct CfgAnswer {
    int         source = -1;
    std::string cfg, logs, run;
};

// Run this exe as `runctxtest --cfgchild <outfile>` with the given MH_CONFIG_DIR / LOCALAPPDATA
// (nullptr = unset), MH_LOG_ROOT unset. The caller's environment is restored afterwards.
bool spawn_cfg_child(const char *config_dir_env, const char *localappdata, const std::string &outfile, CfgAnswer *a) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);

    struct saved_var {
        const char *name;
        char        old[MAX_PATH];
        bool        had;
    } vars[3] = {{"MH_CONFIG_DIR", {0}, false}, {"LOCALAPPDATA", {0}, false}, {"MH_LOG_ROOT", {0}, false}};
    for (saved_var &v : vars) v.had = GetEnvironmentVariableA(v.name, v.old, MAX_PATH) != 0;
    SetEnvironmentVariableA("MH_CONFIG_DIR", config_dir_env);
    if (localappdata != nullptr) SetEnvironmentVariableA("LOCALAPPDATA", localappdata);
    SetEnvironmentVariableA("MH_LOG_ROOT", nullptr);

    char cmd[MAX_PATH * 3];
    wsprintfA(cmd, "\"%s\" runctxtest --cfgchild \"%s\"", exe, outfile.c_str());
    STARTUPINFOA        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    const BOOL          ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (saved_var &v : vars) SetEnvironmentVariableA(v.name, v.had ? v.old : nullptr);
    if (!ok) {
        printf("  cannot spawn %s (err %lu)\n", cmd, GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    char text[MAX_PATH * 4 + 16];
    if (code != 0 || !read_file(outfile.c_str(), text, (int)sizeof(text))) {
        printf("  child exit %lu, answer file %s\n", code, outfile.c_str());
        return false;
    }
    // "<source>\n<cfg>\n<logs>\n<run>\n"
    std::string lines[4];
    int         li = 0;
    for (const char *p = text; *p && li < 4; ++p) {
        if (*p == '\n') ++li;
        else if (*p != '\r') lines[li] += *p;
    }
    if (li < 4) return false;
    a->source = atoi(lines[0].c_str());
    a->cfg    = lines[1];
    a->logs   = lines[2];
    a->run    = lines[3];
    return true;
}

// E.  Two children, because the accessors and run_context both decide once per process.
void child_arms(const std::string &base) {
    const std::string outfile = base + "\\cfg_answer.txt";

    // E1. MH_CONFIG_DIR: config dir = the variable; the logs root, the session dir and mh_run.txt all
    // follow it (run_context composes from the config dir, not from the exe dir).
    {
        const std::string envdir = base + "\\child_env\\cfg\\";
        CfgAnswer         a;
        const bool        got = spawn_cfg_child(envdir.c_str(), nullptr, outfile, &a);
        check("child MH_CONFIG_DIR: answered", got);
        if (got) {
            check("child MH_CONFIG_DIR: source is SRC_ENV", a.source == mh::cfgdir::SRC_ENV);
            check("child MH_CONFIG_DIR: config dir is the variable", a.cfg == envdir);
            check("child MH_CONFIG_DIR: logs root is <config dir>logs", a.logs == envdir + "logs");
            check("child MH_CONFIG_DIR: run dir is under it", starts_with(a.run.c_str(), (envdir + "logs\\").c_str()));
            std::string bc_text(MAX_PATH * 2, '\0');
            const bool  have = read_file((envdir + "mh_run.txt").c_str(), &bc_text[0], (int)bc_text.size());
            check("child MH_CONFIG_DIR: mh_run.txt is in the config dir", have);
            check("child MH_CONFIG_DIR: the breadcrumb names the run dir", have && starts_with(bc_text.c_str(), a.run.c_str()));
        }
        DeleteFileA(outfile.c_str());
    }

    // E2. THE HAND LAUNCH: no MH_CONFIG_DIR, no mh_net.ini beside the exe, only LOCALAPPDATA. Its ini
    // key and logs belong under <LOCALAPPDATA>\MissionHumanity\games\<hash16>\ and NOT beside the exe.
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string exe_dir = exe;
        exe_dir.resize(exe_dir.find_last_of("\\/") + 1);
        if (GetFileAttributesA((exe_dir + "mh_net.ini").c_str()) != INVALID_FILE_ATTRIBUTES) {
            printf("  hand-launch child: SKIPPED -- an mh_net.ini sits beside this exe, so it is portable\n");
        } else if (!is_ascii(base) || !is_ascii(exe_dir)) {
            printf("  hand-launch child: SKIPPED -- a non-ASCII path (the expected ANSI spelling is not portable here)\n");
        } else {
            const std::string local = base + "\\child_local";
            CreateDirectoryA(local.c_str(), nullptr);
            CfgAnswer  a;
            const bool got = spawn_cfg_child(nullptr, local.c_str(), outfile, &a);
            check("child hand launch: answered", got);
            if (got) {
                // The child hashes its own GetModuleFileNameW path; ask the same real_inputs() for the
                // expectation, so a change in how the hash input is read shows here AND in arm B.
                mh::cfgdir::real_buffers rb;
                const mh::cfgdir::inputs in = mh::cfgdir::real_inputs(rb);
                const std::string        want =
                    local + "\\MissionHumanity\\games\\" + hash16(in.game_dir_utf8) + "\\";
                check("child hand launch: source is SRC_USER", a.source == mh::cfgdir::SRC_USER);
                check("child hand launch: config dir is LOCALAPPDATA\\MissionHumanity\\games\\<hash16>\\",
                      a.cfg == want);
                check("child hand launch: logs root is <config dir>logs", a.logs == want + "logs");
                check("child hand launch: run dir is under <config dir>logs\\",
                      starts_with(a.run.c_str(), (want + "logs\\").c_str()));
                std::string bc_text(MAX_PATH * 2, '\0');
                const bool  have = read_file((want + "mh_run.txt").c_str(), &bc_text[0], (int)bc_text.size());
                check("child hand launch: mh_run.txt is in the config dir", have);
            }
            DeleteFileA(outfile.c_str());
        }
    }
}

void config_dir_arms() {
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    char base_c[MAX_PATH];
    wsprintfA(base_c, "%smh_cfgdir_%lu", temp, (unsigned long)GetCurrentProcessId());
    std::string base = base_c;
    rmtree(base);
    CreateDirectoryA(base.c_str(), nullptr);

    sha256_arm();
    hash_vectors_arm();
    if (is_ascii(base)) {
        resolve_order_arm(base);
        accessor_arm(base);
    } else {
        printf("  resolver arms C/D: SKIPPED -- the temp path is not ASCII (%s)\n", base.c_str());
    }
    child_arms(base);
    rmtree(base);
}

} // namespace

int run_runctxtest(int argc, char **argv) {
    if (argc > 3 && strcmp(argv[2], "--child") == 0) return child_main(argv[3]);
    if (argc > 3 && strcmp(argv[2], "--cfgchild") == 0) return cfg_child_main(argv[3]);

    printf("=== runctxtest (LA13 + RL3: <config dir>\\logs\\ by default, MH_LOG_ROOT when the launcher sets it;\n"
           "    the config dir itself: MH_CONFIG_DIR, portable, %%LOCALAPPDATA%% hash dir) ===\n");

    // A stray MH_LOG_ROOT in the gate's environment would turn arm 1 into arm 2; say so rather than
    // silently testing the wrong thing.
    char stray[MAX_PATH];
    if (GetEnvironmentVariableA("MH_LOG_ROOT", stray, MAX_PATH) != 0) {
        printf("  MH_LOG_ROOT is set in this environment (%s) -- unset it and rerun\n", stray);
        return 1;
    }

    // ---- 1. the hand-launch default, in THIS process ------------------------------------------
    // RL3: "the hand-launch default" is <config dir>logs. net_selftest pins the config dir to its own
    // directory, so on the gate this IS <exedir>logs -- the shape the arm has always asserted.
    char expect_root[MAX_PATH];
    wsprintfA(expect_root, "%slogs", mh::cfgdir::config_dir());
    check("no variable: logs root is <config dir>logs", strcmp(MH_LogsRoot(), expect_root) == 0);
    char expect_prefix[MAX_PATH];
    wsprintfA(expect_prefix, "%s\\", expect_root);
    check("no variable: run dir is under <config dir>logs\\", starts_with(MH_RunDir(), expect_prefix));
    check("no variable: run dir exists", GetFileAttributesA(MH_RunDir()) != INVALID_FILE_ATTRIBUTES);
    char bc[MAX_PATH], bc_text[MAX_PATH * 2];
    wsprintfA(bc, "%smh_run.txt", mh::cfgdir::config_dir());
    check("no variable: mh_run.txt in the config dir", read_file(bc, bc_text, (int)sizeof(bc_text)));
    check("no variable: mh_run.txt names the run dir", starts_with(bc_text, MH_RunDir()));

    // ---- a scratch tree for the child arms ----------------------------------------------------
    char temp[MAX_PATH], base[MAX_PATH], outfile[MAX_PATH], root[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    wsprintfA(base, "%smh_runctx_%lu", temp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(base, nullptr);
    wsprintfA(outfile, "%s\\answer.txt", base);

    // ---- 2. MH_LOG_ROOT names a directory that does not exist yet, with a trailing separator --
    wsprintfA(root, "%s\\owned\\", base);
    {
        char parent[MAX_PATH];
        wsprintfA(parent, "%s\\owned", base);
        ChildAnswer a;
        bool        got = spawn_child(root, outfile, &a);
        check("MH_LOG_ROOT: child answered", got);
        if (got) {
            check("MH_LOG_ROOT: logs root is the variable, trailing separator stripped",
                  strcmp(a.logs_root, parent) == 0);
            char prefix[MAX_PATH];
            wsprintfA(prefix, "%s\\", parent);
            check("MH_LOG_ROOT: run dir is under the root", starts_with(a.run_dir, prefix));
            check("MH_LOG_ROOT: process dir == run dir at boot", strcmp(a.run_dir, a.proc_dir) == 0);
            check("MH_LOG_ROOT: the run dir was created",
                  GetFileAttributesA(a.run_dir) != INVALID_FILE_ATTRIBUTES);
            char cbc[MAX_PATH], cbc_text[MAX_PATH * 2];
            wsprintfA(cbc, "%s\\mh_run.txt", parent);
            bool have = read_file(cbc, cbc_text, (int)sizeof(cbc_text));
            check("MH_LOG_ROOT: mh_run.txt is INSIDE the root", have);
            check("MH_LOG_ROOT: that breadcrumb names the run dir", have && starts_with(cbc_text, a.run_dir));
            // The config-dir breadcrumb was not rewritten by the child: it still names OUR run dir.
            check("MH_LOG_ROOT: the config-dir mh_run.txt is untouched",
                  read_file(bc, bc_text, (int)sizeof(bc_text)) && starts_with(bc_text, MH_RunDir()));
        }
        DeleteFileA(outfile);
    }

    // ---- 3. MH_LOG_ROOT names something that cannot be created: a path under a FILE ------------
    {
        char blocker[MAX_PATH];
        wsprintfA(blocker, "%s\\blocker", base);
        HANDLE h = CreateFileA(blocker, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        wsprintfA(root, "%s\\blocker\\logs", base);
        ChildAnswer a;
        bool        got = spawn_child(root, outfile, &a);
        check("unmakeable MH_LOG_ROOT: child answered", got);
        if (got) {
            check("unmakeable MH_LOG_ROOT: falls back to <config dir>logs", strcmp(a.logs_root, expect_root) == 0);
            check("unmakeable MH_LOG_ROOT: run dir under <config dir>logs\\", starts_with(a.run_dir, expect_prefix));
        }
        DeleteFileA(outfile);
        DeleteFileA(blocker);
    }

    // ---- 4. an EMPTY MH_LOG_ROOT is the same as none ---------------------------------------------
    {
        ChildAnswer a;
        bool        got = spawn_child("", outfile, &a);
        check("empty MH_LOG_ROOT: child answered", got);
        if (got) check("empty MH_LOG_ROOT: <config dir>logs", strcmp(a.logs_root, expect_root) == 0);
        DeleteFileA(outfile);
    }

    // ---- RL3: the config directory itself ------------------------------------------------------
    config_dir_arms();

    printf("=== runctxtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
