# State recording file format (`mh_match_state.bin`, `mh_desync_state.bin`)

Spec for mp:D40 (whole-match recording, `net-debug`), mp:D41 (the ship `net` ring, flushed on a
detected desync), mp:D42 (`tools/state_record.py`) and mp:D43 (launcher report carriage). Written
before any of them, so the writer, the reader and the launcher's tail cut are built against one
definition. **Version 1.** Anything that changes a layout below bumps `version`, and every reader
refuses a version it does not know, by name.

Why it exists and what it costs: measured 2026-09-27 with the desync watch's dirty-block probe
(`[desync] dirty_probe=1`) on a replayed 66,586-step field match. The match changes 432 bytes of the
2.8 MB hash manifest per sim step on average, so recording raw byte runs for every step of a whole
match is about 29 MB before compression.

## What is recorded

- The **raw bytes of every hash-manifest slice** (`mh::state::HASH_REGIONS`, read at
  `hash_base(i)`), in manifest order. Raw means no VERDICT masking: the local-only fields
  (`ctrl_group_id`, the fog byte, rng slot 1, soldier animation) are stored as they are in memory.
  A reader that compares two peers applies the VERDICT masks itself (the mp:D39 mask table).
- One state per **sim step**, taken at the step's PRE-BODY boundary: the same point the desync watch
  and the determinism harness sample at. `step` is the desync watch's step counter (`g_step`, reset
  to 0 at `session_begin_multi`; the first recorded step is 1). The writer logs the offset between
  this axis and the harness's match step axis (`mh_match_harness.log`, `step_base`) once, so the two
  can be joined.

## Layout

All integers little-endian. No padding anywhere; every structure is byte-packed.

### Header (once, at offset 0)

| Field | Type | Meaning |
|---|---|---|
| `magic` | u32 | `0x5253484D` (`"MHSR"` as bytes) |
| `version` | u16 | `1` |
| `flags` | u16 | bit 0: `RING` (a D41 desync ring dump, not a whole match). Others 0. |
| `header_len` | u32 | total header bytes including the region table; chunks start here |
| `manifest_fp` | u64 | the desync watch's manifest fingerprint (names + lens + excluded) |
| `keyframe_every` | u32 | steps between keyframes (the writer's setting; informational) |
| `region_count` | u32 | number of region-table entries |
| region table | | `region_count` entries of `{u32 len; u8 name_len; char name[name_len]}` |

The region table is what makes a file decodable **without a matching build**: a reader takes the
names and lengths from the file, never from its own manifest.

### Chunks (from `header_len` to end of file)

Every chunk is `{u32 tag; u32 payload_len; u32 crc32; u8 payload[payload_len]}`. `crc32` is the
standard zlib CRC-32 (polynomial 0xEDB88320, init/final xor 0xFFFFFFFF) of the payload only.

A reader walks chunks in order and **stops cleanly at the first incomplete chunk** (the file was cut
by a crash, a kill, or a tail cut) or at a CRC mismatch, reporting the last complete step. Neither is
an error unless nothing decodable remains.

| Tag | Value (`"...."` as bytes) | Payload |
|---|---|---|
| `KEYF` | `0x4659454B` | `u32 step`, then every region's raw bytes, concatenated in table order (sum of `len`) |
| `STEP` | `0x50455453` | `u32 step`, `u32 run_count`, then `run_count` runs of `{u16 region; u32 offset; u16 len; u8 bytes[len]}` |
| `END ` | `0x20444E45` | `u32 last_step`, `u32 steps_recorded`, `u64 raw_bytes_written` (the file's size before this chunk) |

Semantics:

- `KEYF` at step *s* = the full state at step *s*. The first chunk after the header is always a
  `KEYF`. Further keyframes come every `keyframe_every` steps (default 3000, one minute at 50 steps/s),
  counted from the first recorded step: steps *f*, *f*+K, *f*+2K, ....
- `STEP` at step *s* = the runs that changed between step *s−1* and step *s*. Applying them to the
  state at *s−1* gives the state at *s*. A step with no change still writes a `STEP` with
  `run_count = 0`, so every step is explicit and a gap means lost data, not "unchanged".
- A keyframe step also gets its `STEP` chunk (written first), so a reader can replay deltas across a
  keyframe without special-casing it; the `KEYF` then re-bases the reader's state (and a reader MAY
  check the rebuilt state equals it).
- Runs never cross a region; `offset + len <= region len`. Nearby changed bytes MAY be coalesced into
  one run (the writer merges gaps of up to 8 unchanged bytes); a reader must not assume runs are
  minimal.
- `END ` is written when recording stops normally. A file without it was cut short; that is
  reported, not refused. The mh.dll writer also leaves it out when it stops recording early (its
  writer fell behind, a write failed, or an allocation failed). It logs the step, and the file
  stays valid through its last complete chunk.

## The mh.dll writer (mp:D40)

`[desync] state_record=1` (and `state_keyframe_every`, default 3000) in `mh_net.ini`. One file per
match, opened at the match's first step in the match folder: `mh_match_state.bin`, or
`mh_match_state_<n>.bin` when that folder already holds one. It is closed at the session's end,
and steps after that ("Continue game") are not recorded. `mh_net.log` carries:

- `; [desync] STATE RECORD -> <path>: ...` when the file opens.
- `; [desync] STATE RECORD step axis: ...`. This line gives the offset between `step` and the
  harness's match step axis in `mh_match_harness.log`. The harness's match step 1 is the first sim
  step that sees the session folder. The line counts the sim steps that ran in that folder before
  `session_begin_multi` (0 = the same axis). With no session folder open there is no harness
  segment, and the line says so.
- `; [desync] STATE RECORD match end: ...` gives steps, keyframes, file bytes and the sim-thread
  cost per step. Then `; [desync] STATE RECORD writer ...` gives the writer thread's stats.

### Cutting a tail (mp:D43)

A file may be shortened for upload by keeping the **header plus every chunk from some `KEYF` chunk
onward**. The result is a valid file: its first chunk is a keyframe. The launcher finds `KEYF`
boundaries by walking the chunk headers (tag + length); it never needs to decode a payload.

### Ring dumps (mp:D41)

Same format, with `flags` bit 0 set and `keyframe_every` 0. The only `KEYF` is the first chunk, at the
ring's oldest step. `STEP` chunks follow, one per step, up to the recording's end: the desync plus
the configured tail after it, or less if the byte cap or the match end came first. `END` closes it,
with `steps_recorded` = last - first + 1. The mh.dll writer is `src/mh_dll/mh/desync/state_ring.cpp`
(`[desync] state_ring`, on in ship `net`), and it writes `mh_desync_state.bin` (`_<n>` if taken)
into the match folder once per match. Its `mh_net.log` lines name the path, the step range, how many
steps precede the mismatching step, and the bytes.
