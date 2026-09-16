//
// Shared surface between the generated stub table (proxy_stubs.gen.cpp) and the resolver
// (proxy_core.cpp). This DLL is the msvfw32 proxy shim that force-loads mh.dll.
//
// Deliberately does NOT include <windows.h>: several of the names we re-export are MACROS in the
// Windows headers (MCIWndCreate -> MCIWndCreateA/W in vfw.h being the obvious one), and a macro
// expansion over a generated function definition would silently rename the export.
//
#pragma once

#include "proxy_table.gen.h"

// One slot per export, in the generated table's order. Zero until first call.
extern "C" void *g_mh_proxy_real[MH_PROXY_COUNT];

// Export names for GetProcAddress, same order. An ordinal-only export stores the ordinal cast to
// a pointer, which is exactly what GetProcAddress expects for the by-ordinal form.
extern "C" const char *const g_mh_proxy_names[MH_PROXY_COUNT];

// Called from a stub when its slot is still empty. Returns the resolved target (never NULL -- a
// failure to resolve is fatal and does not return). __cdecl because the stubs push one argument
// and clean the stack themselves.
extern "C" void *__cdecl mh_proxy_resolve(int index);
