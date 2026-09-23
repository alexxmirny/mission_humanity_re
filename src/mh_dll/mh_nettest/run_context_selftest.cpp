//
// run_context_selftest.cpp -- `net_selftest.exe runctxtest`: WHERE the run context puts a process's
// logs (dist LA13). Two roots, one rule: `<exedir>\logs\` unless MH_LOG_ROOT was in the environment
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
//   1. no variable          -> "<exedir>logs", run dir under it, breadcrumb beside the exe   (3)
//   2. MH_LOG_ROOT=<dir>    -> that dir (created; trailing separator stripped), run dir under
//                              it, breadcrumb INSIDE it                                        (1)
//   3. MH_LOG_ROOT=<unmakeable> -> the hand-launch default again, never nothing
//
#include <stdio.h>
#include <string.h>
#include <windows.h>

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

} // namespace

int run_runctxtest(int argc, char **argv) {
    if (argc > 3 && strcmp(argv[2], "--child") == 0) return child_main(argv[3]);

    printf("=== runctxtest (LA13: <exedir>\\logs\\ by default, MH_LOG_ROOT when the launcher sets it) ===\n");

    // A stray MH_LOG_ROOT in the gate's environment would turn arm 1 into arm 2; say so rather than
    // silently testing the wrong thing.
    char stray[MAX_PATH];
    if (GetEnvironmentVariableA("MH_LOG_ROOT", stray, MAX_PATH) != 0) {
        printf("  MH_LOG_ROOT is set in this environment (%s) -- unset it and rerun\n", stray);
        return 1;
    }

    // ---- 1. the hand-launch default, in THIS process ------------------------------------------
    char expect_root[MAX_PATH];
    wsprintfA(expect_root, "%slogs", MH_ExeDir());
    check("no variable: logs root is <exedir>logs", strcmp(MH_LogsRoot(), expect_root) == 0);
    char expect_prefix[MAX_PATH];
    wsprintfA(expect_prefix, "%s\\", expect_root);
    check("no variable: run dir is under <exedir>logs\\", starts_with(MH_RunDir(), expect_prefix));
    check("no variable: run dir exists", GetFileAttributesA(MH_RunDir()) != INVALID_FILE_ATTRIBUTES);
    char bc[MAX_PATH], bc_text[MAX_PATH * 2];
    wsprintfA(bc, "%smh_run.txt", MH_ExeDir());
    check("no variable: mh_run.txt beside the exe", read_file(bc, bc_text, (int)sizeof(bc_text)));
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
            // The exe-side breadcrumb was not rewritten by the child: it still names OUR run dir.
            check("MH_LOG_ROOT: the exe-side mh_run.txt is untouched",
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
            check("unmakeable MH_LOG_ROOT: falls back to <exedir>logs", strcmp(a.logs_root, expect_root) == 0);
            check("unmakeable MH_LOG_ROOT: run dir under <exedir>logs\\", starts_with(a.run_dir, expect_prefix));
        }
        DeleteFileA(outfile);
        DeleteFileA(blocker);
    }

    // ---- 4. an EMPTY MH_LOG_ROOT is the same as none ---------------------------------------------
    {
        ChildAnswer a;
        bool        got = spawn_child("", outfile, &a);
        check("empty MH_LOG_ROOT: child answered", got);
        if (got) check("empty MH_LOG_ROOT: <exedir>logs", strcmp(a.logs_root, expect_root) == 0);
        DeleteFileA(outfile);
    }

    printf("=== runctxtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
