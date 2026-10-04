/*
 * desync/state_miniz.c -- mp:D46: compiles the vendored miniz 3.0.2 (MIT, src/mh_dll/include/miniz/
 * LICENSE.txt) as C. It is not C++-clean (a tentative `static const` array definition), so it cannot be
 * included from a .cpp; state_compress.cpp calls it through miniz.h, configured by state_miniz_config.h.
 */
#include "state_miniz_config.h"
#pragma warning(push, 0)
#include "../../include/miniz/miniz.c"
#pragma warning(pop)
