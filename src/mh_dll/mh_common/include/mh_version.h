#pragma once
//
// mh_version.h -- the build stamp, as strings, for both cl.exe and rc.exe.
//
// The two macros MH_VERSION_STAMP and MH_GIT_SHA_STAMP are pushed in from src/mh_dll/mh_version.props
// as BARE TOKENS (`/D MH_VERSION_STAMP=0.1.0-rc1`), never as quoted strings. That is deliberate and
// measured: a quoted /D value has to survive MSBuild, a response file and the compiler's own
// command-line parser, and it does not reliably survive all three. A bare token has no quoting layer
// to lose, and the two-step stringify below turns it into a literal identically for the C++ compiler
// and the resource compiler.
//
// A build with no properties set stamps 0.0.0-dev + the git short sha, so a development DLL can
// never be mistaken for a released one -- see the header comment of mh_version.props.
//
// WHO PRINTS IT: mh.dll writes MH_VERSION_FULL as the first line of mh_net.log (the `; [build] mh
// ...` banner in mh/seams/module_bind.cpp), so a run's log names the build it came from. The same
// string is in every shipping module's VERSIONINFO resource, which is what Explorer's Details tab
// and tools/release_package.py read.

// The classic two-step: the inner macro stringifies, the outer one forces its argument to expand
// first. One step alone would yield the literal "MH_VERSION_STAMP".
#define MH_VERSION_STRINGIFY_(x) #x
#define MH_VERSION_STRINGIFY(x)  MH_VERSION_STRINGIFY_(x)

#ifndef MH_VERSION_STAMP
// A TU compiled without the property sheet still has to build (mh_common is also linked into the
// selftest hosts, which do not import it). It says `0.0.0-unstamped` rather than borrowing the
// dev default, so "nobody stamped this" and "this is a dev build" stay distinguishable. It is
// spelled with an underscore rather than `0.0.0-unstamped` because a `-` in a macro body is a
// binary operator to clang-format, which would respace it and change what stringification yields.
#define MH_VERSION_STAMP 0.0.0_unstamped
#endif

#ifndef MH_GIT_SHA_STAMP
#define MH_GIT_SHA_STAMP unknown
#endif

// "0.1.0-rc1"
#define MH_VERSION_STRING MH_VERSION_STRINGIFY(MH_VERSION_STAMP)
// "abc12345" or "unknown"
#define MH_GIT_SHA_STRING MH_VERSION_STRINGIFY(MH_GIT_SHA_STAMP)
// "0.1.0-rc1+abc12345" -- the one form that is quoted in a bug report.
#define MH_VERSION_FULL MH_VERSION_STRING "+" MH_GIT_SHA_STRING
