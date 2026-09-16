//
// run_context.cpp -- see include/mh_run_context.h. Computes the shared per-run log directory once.
//
#include <windows.h>
#include "include/mh_run_context.h"

namespace {

char          g_exe_dir[MAX_PATH] = {0}; // "...\"
char          g_run_dir[MAX_PATH] = {0}; // "...\logs\<runid>_<role>\"
volatile LONG g_state             = 0;   // 0=uninit, 1=initialising, 2=ready

// case-insensitive substring test (no CRT dependence in the injected context)
bool contains_ci(const char *hay, const char *needle) {
    for (const char *h = hay; *h; ++h) {
        int i = 0;
        while (needle[i]) {
            char a = h[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            ++i;
        }
        if (!needle[i]) return true;
    }
    return false;
}

void compute_exe_dir() {
    GetModuleFileNameA(nullptr, g_exe_dir, MAX_PATH);
    char *slash = g_exe_dir;
    for (char *p = g_exe_dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0'; // keep trailing backslash
}

// Role: cmdline verb first (authoritative per peer), then mh_net.ini [net] role, else "solo".
const char *detect_role() {
    const char *cl = GetCommandLineA();
    if (cl) {
        if (contains_ci(cl, "--mp-host")) return "host";
        if (contains_ci(cl, "--mp-join")) return "client";
    }
    char ini[MAX_PATH], role[32] = {0};
    wsprintfA(ini, "%smh_net.ini", g_exe_dir);
    GetPrivateProfileStringA("net", "role", "", role, sizeof(role), ini);
    if (role[0] == 'h' || role[0] == 'H') return "host";
    if (role[0] == 'c' || role[0] == 'C') return "client";
    return "solo";
}

void do_init() {
    compute_exe_dir();

    SYSTEMTIME st;
    GetLocalTime(&st);
    char runid[32];
    wsprintfA(runid, "%04d%02d%02d_%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const char *role = detect_role();

    char logs_root[MAX_PATH];
    wsprintfA(logs_root, "%slogs", g_exe_dir);
    CreateDirectoryA(logs_root, nullptr); // ok if it already exists
    wsprintfA(g_run_dir, "%s\\%s_%s\\", logs_root, runid, role);
    // strip the trailing backslash for the create call, then keep it in g_run_dir
    char mk[MAX_PATH];
    lstrcpynA(mk, g_run_dir, MAX_PATH);
    for (int i = lstrlenA(mk) - 1; i >= 0 && (mk[i] == '\\' || mk[i] == '/'); --i) mk[i] = '\0';
    BOOL  ok  = CreateDirectoryA(mk, nullptr);
    DWORD err = GetLastError();
    if (!ok && err != ERROR_ALREADY_EXISTS) {
        lstrcpynA(g_run_dir, g_exe_dir, MAX_PATH); // fallback: never lose logs
    } else {
        // breadcrumb at exe dir so a human/pull finds the latest run folder
        char bc[MAX_PATH];
        wsprintfA(bc, "%smh_run.txt", g_exe_dir);
        HANDLE h = CreateFileA(bc, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, g_run_dir, lstrlenA(g_run_dir), &w, nullptr);
            WriteFile(h, "\r\n", 2, &w, nullptr);
            CloseHandle(h);
        }
    }
}

void ensure_init() {
    if (g_state == 2) return;
    if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
        do_init();
        g_state = 2;
    } else {
        while (g_state != 2) Sleep(0); // another thread is initialising
    }
}

} // namespace

extern "C" const char *MH_RunDir(void) {
    ensure_init();
    return g_run_dir;
}
extern "C" const char *MH_ExeDir(void) {
    ensure_init();
    return g_exe_dir;
}
