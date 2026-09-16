// The mh.dll thin implementation of the host-callback ABI (LIB-ABI stage B; sim/tact
// split at LIB-IFACE-SPLIT).
//
// mhdll_table() / mhdll_tact_table() are the GENERATED tables (addr/mh_hostapi_bind.gen.cpp)
// whose every entry forwards to its mh::call:: thunk -- the hosted configuration's host.
// The harness binds both at arm time via libmh_set_host_api() / libmh_set_tact_host_api()
// and reports both unbound-walks (zero by construction: the same generator emits the
// structs and the fills, so a gap is a generator bug, and the startup line is what would
// catch it).
//
// HARNESS header: the tables it declares name fixed VAs. Module (libmh) code must never
// include this -- modules see the host only through libmh/state/host_api.h.
#pragma once

#include "../../libmh/include/libmh_host_api.gen.h"
#include "../../libmh/include/libmh_tact_host_api.gen.h"

namespace mh::hostapi {
const libmh_host_api      &mhdll_table();
const libmh_tact_host_api &mhdll_tact_table();

// The eight RESHAPED `io` entries (SIMABI-VFS). Their ABI shape is not the original thunk's, so
// there is no one-line forward to generate: seams/hostapi_io_bind.cpp implements them by hand and
// the generator drops these names straight into the table. A ledger row asks for this by naming the
// function in its `abi.bind`, so adding one is a ledger edit plus a definition here -- the generated
// binder never needs touching.
namespace io {
int32_t vfs_open(const char *path, int32_t mode);
int32_t vfs_read(int32_t handle, void *dst, uint32_t n);
int32_t vfs_write(int32_t handle, const void *src, uint32_t n);
void    vfs_close(int32_t handle);
int32_t vfs_seek(int32_t handle, int32_t offset, int32_t whence);
int32_t vfs_tell(int32_t handle);
int32_t asset_size(const char *name);
int32_t asset_read(const char *name, void *dst, uint32_t dst_cap);
} // namespace io

} // namespace mh::hostapi
