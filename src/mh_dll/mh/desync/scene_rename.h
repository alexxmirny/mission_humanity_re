//
// desync/scene_rename.h -- keeps mh.dll's private copy of libmh's world-snapshot code OUT of the libmh
// contract (REPSCENE, 2026-10-17).
//
// mh.dll compiles libmh/state/{world_snapshot,world_fixup,nav_trailer,boot_snapshot}.cpp itself, with
// MH_NAV_CAPTURE_ONLY, only so the field-report SCENE DUMP (desync/scene_dump.cpp) can capture the world
// without libmh.dll (release ships none). Those files define mh::state::world::capture, capture_capacity,
// lockstep_hash, nav::capture/import, boot::session_begun ... -- the very symbols the libmh contract
// forwards to libmh.dll through weak /alternatename shims (seams/libmh_contract.gen.cpp). A STRONG local
// definition silently beats the shim, so every existing caller (world_sync's resync capture, the lockstep
// hash) would have started running mh.dll's copy -- which reads the stock addresses, not libmh's bound
// regions -- and the contract generator would have grown six rows for it.
//
// So the copy lives in its OWN namespaces: this header is force-included (/FI) into exactly those four
// TUs and into scene_dump.cpp, and renames the three namespaces they define. No symbol of the copy has
// the contract's name, the contract is untouched (tools/gen_libmh_contract.py --rederive --check), and
// only the scene dump calls it. Anything that includes the real headers normally is unaffected.
//
#pragma once
#define world mh_scene_world
#define nav   mh_scene_nav
#define boot  mh_scene_boot
