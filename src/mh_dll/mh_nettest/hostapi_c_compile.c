/* Compile-as-C proof for the generated host-callback tables (LIB-ABI done_when clause;
 * both tables since LIB-IFACE-SPLIT).
 *
 * This TU is deliberately plain C (.c => MSVC compiles it as C): it proves the generated
 * headers need no C++ to be included, and that the X-macro metadata pattern the binder and
 * the unbound-walks rely on actually expands -- name table, offsetof table, notify flags --
 * with each entry count agreeing with its LIBMH[_TACT]_HOST_API_ENTRY_COUNT at compile
 * time.
 */
#include <stddef.h>

#include "../libmh/include/libmh_host_api.gen.h"
#include "../libmh/include/libmh_tact_host_api.gen.h"

#define X(name, category, notify) {#name, offsetof(libmh_host_api, name), notify},
static const struct {
    const char *name;
    size_t      offset;
    int         notify;
} k_entries[] = {LIBMH_HOST_API_FOR_EACH(X)};
#undef X

#define X(name, category, notify) {#name, offsetof(libmh_tact_host_api, name), notify},
static const struct {
    const char *name;
    size_t      offset;
    int         notify;
} k_tact_entries[] = {LIBMH_TACT_HOST_API_FOR_EACH(X)};
#undef X

/* C89-portable static assert: array size goes negative if the counts disagree. */
typedef char libmh_hostapi_entry_count_matches
    [(sizeof(k_entries) / sizeof(k_entries[0]) == LIBMH_HOST_API_ENTRY_COUNT) ? 1 : -1];
typedef char libmh_tact_hostapi_entry_count_matches
    [(sizeof(k_tact_entries) / sizeof(k_tact_entries[0]) == LIBMH_TACT_HOST_API_ENTRY_COUNT)
         ? 1
         : -1];

/* Every entry is a function pointer, so each struct is exactly COUNT pointers. */
typedef char libmh_hostapi_struct_is_flat
    [(sizeof(libmh_host_api) == LIBMH_HOST_API_ENTRY_COUNT * sizeof(void (*)(void))) ? 1 : -1];
typedef char libmh_tact_hostapi_struct_is_flat
    [(sizeof(libmh_tact_host_api) == LIBMH_TACT_HOST_API_ENTRY_COUNT * sizeof(void (*)(void)))
         ? 1
         : -1];

/* Referenced so the arrays are used and the TU exports a symbol (avoids LNK4221). */
const char *libmh_hostapi_c_compile_probe(void) {
    return k_entries[0].name ? k_entries[0].name : k_tact_entries[0].name;
}
