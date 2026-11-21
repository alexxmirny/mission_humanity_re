// Standalone FILE-backed storage carrier. The caller owns handles, arena and canonical policy bytes.
#pragma once
#include "include/mh_storage_policy_save.h"
#include <cstdint>
#include <cstdio>

namespace mh::refhost::storage_save {
using file_resolver = FILE *(*)(int32_t handle);
// Call after binding the actual arena/policy region and before opening the replay surface.
bool initialize(file_resolver resolve);
// Only native .sav containers and saveNN.dat/qsaveNN.dat members belong to this carrier.
bool is_save_output(const char *path);
bool preflight_open(FILE *file, const char *path);

int32_t write_planet(int32_t handle);
int32_t preflight_planet(int32_t handle);
int32_t begin_load();
int32_t load_planet(int32_t handle);
int32_t preflight_container(int32_t handle);
void    bind_order_context(const mh::storage_policy::savefile::order_context &);
int32_t load_container(int32_t handle);
int32_t finish_container(int32_t handle);
int32_t normalize_version(void *header40);
// This host has native fixed-window storage, not the hosted relocation/MHOV backend. The prepare
// gate refuses wider bindings before serialization; direct native windows need no gather/restore.
int32_t planet_save_prepare();
int32_t planet_load_extensions(int32_t handle);
void    planet_save_restore();
} // namespace mh::refhost::storage_save
