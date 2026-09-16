
All credits to @dupan_80025 (aka PC Vandalus)

# BNK Sprite Bank Format

Technical reference for the sprite bank files used in **Exterminacja** (also released as **Mission Humanity**) by Techland.

---

## Table of Contents

- [1. Overview](#1-overview)
- [2. BANKI.DAT — Bank Index](#2-bankidat--bank-index)
- [3. BNK File — V1 (Palette-Based)](#3-bnk-file--v1-palette-based)
- [4. BNK File — V2 (Inline RGB565)](#4-bnk-file--v2-inline-rgb565)
- [5. V1 Sprite RLE Stream](#5-v1-sprite-rle-stream)
- [6. V2 Sprite RLE Stream](#6-v2-sprite-rle-stream)
- [7. Color Format — RGB565](#7-color-format--rgb565)
- [8. Extraction (BNK → PNG)](#8-extraction-bnk--png)
- [9. Repacking (PNG → BNK)](#9-repacking-png--bnk)
- [10. Extracted File Layout](#10-extracted-file-layout)
- [11. Lossless Round-Trip Guarantee](#11-lossless-round-trip-guarantee)

---

## 1. Overview

The sprite system uses a two-level architecture:

- **`BANKI.DAT`** — a flat index file describing the size of each bank's data sections.
- **`BANK_XX.BNK`** — individual bank files containing RLE-encoded sprites with palettes.

Banks are divided into three categories:

| Range | Role | Loading |
|-------|------|---------|
| 0–49 | Base banks (UI, terrain, shared effects) | Always loaded at startup |
| 50–99 | Character banks (unit sprites per faction) | Loaded conditionally based on active unit configuration |
| 100–120 | Alternative banks (enemy base interiors) | Loaded when entering a base |

Two format versions exist:

- **V1** — palette-based sprites. Each sprite references an external 256-color RGB565 palette. The BNK file contains five data sections.
- **V2** — inline RGB565 sprites. Colors are embedded directly in the RLE stream. The BNK file contains only three data sections (no palette, no palette indices). Identified by `paletteDataSize == 0` in the BANKI.DAT entry.

---

## 2. BANKI.DAT — Bank Index

A flat binary array of 12-byte records, one per bank slot. No file header. Total bank count is derived from the file size: `numBanks = fileSize / 12`.

### Record: `BankiEntry` (12 bytes)

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| +0x00 | 4 | uint32 LE | `indicesSize` | Byte size of each index array. Number of sprites = `indicesSize / 4`. |
| +0x04 | 4 | uint32 LE | `spriteDataSize` | Byte size of the RLE sprite data section. |
| +0x08 | 4 | uint32 LE | `paletteDataSize` | Byte size of the palette data section. If 0, this is a V2 bank. |

Empty banks have all three fields set to zero.

### Derived values

- **Number of sprites**: `indicesSize / 4`
- **V1 payload size**: `spriteDataSize + indicesSize × 3 + metadataSize + paletteDataSize` where `metadataSize = indicesSize × 6`
- **V2 payload size**: same, but without `paletteDataSize` and the second index array

### Bank-to-filename mapping

The game uses format strings to locate BNK files:

- Banks 0–99: `BANK_%02d.BNK` (e.g., `BANK_05.BNK`)
- Banks 100+: `BANK_%03d.BNK` (e.g., `BANK_120.BNK`)
- Exception: banks 47 and 48 always use `BANK_%02d.BNK` even when loaded as alternative banks

---

## 3. BNK File — V1 (Palette-Based)

All data is little-endian. Sections are laid out sequentially with no padding.

```
┌───────────────────────────────┐
│  File Header       (0x14 B)   │  Opaque resource header, preserved verbatim
├───────────────────────────────┤
│  Section 1: spriteData        │  RLE-encoded sprites concatenated back-to-back
│  (spriteDataSize bytes)       │
├───────────────────────────────┤
│  Section 2: indices1          │  uint32 offsets into spriteData, one per sprite
│  (indicesSize bytes)          │
├───────────────────────────────┤
│  Section 3: metadata          │  24 bytes per sprite (hotspot, bounding box, etc.)
│  (indicesSize × 6 bytes)      │
├───────────────────────────────┤
│  Section 4: paletteData       │  Per-sprite RGB565 palettes (uint16 LE arrays)
│  (paletteDataSize bytes)      │
├───────────────────────────────┤
│  Section 5: indices2          │  uint32 offsets into paletteData, one per sprite
│  (indicesSize bytes)          │
└───────────────────────────────┘
```

### Accessing sprite data

For sprite *i*:

- **RLE data**: `spriteData[indices1[i] .. indices1[i+1])` (last sprite ends at `spriteDataSize`)
- **Palette**: `paletteData[indices2[i] ..)` — size determined by the next distinct offset in `indices2`, or `paletteDataSize` for the last entry
- **Metadata**: `metadata[i × 24 .. i × 24 + 24)`

### Shared palettes

Multiple sprites frequently share a single palette. In this case, `indices2` contains identical values (e.g., `[0, 0, 0, ...]`). The palette size for any sprite is computed by finding the next strictly greater offset in `indices2`, not simply the next entry.

### Sprite header (14 bytes, at start of each sprite in spriteData)

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| +0x00 | 1 | uint8 | `unknown0` | Unknown, preserved during round-trip |
| +0x01 | 1 | uint8 | `version` | Format version. If ≥ 2, game converts inline colors on load |
| +0x02 | 1 | uint8 | `unknown2` | Unknown |
| +0x03 | 1 | uint8 | `unknown3` | Unknown |
| +0x04 | 2 | uint16 LE | `bboxX` | Bounding box X offset from anchor |
| +0x06 | 2 | uint16 LE | `bboxY` | Bounding box Y offset from anchor |
| +0x08 | 2 | uint16 LE | `extent` | Visual extent (used for clipping) |
| +0x0A | 2 | uint16 LE | `offsetY` | Additional Y offset for clipping |
| +0x0C | 2 | uint16 LE | `yStart` | Y offset where pixel rendering begins |

The RLE stream follows immediately after the header at offset +0x0E.

### Metadata (24 bytes per sprite)

Stored in section 3 as a flat array. Known fields:

| Offset | Type | Description |
|--------|------|-------------|
| +0x04 | int16 LE | Hotspot X — anchor point for positioning |
| +0x06 | int16 LE | Hotspot Y — anchor point for positioning |

Remaining 20 bytes are unknown and preserved verbatim.

---

## 4. BNK File — V2 (Inline RGB565)

V2 banks are identified by `paletteDataSize == 0` in the BANKI.DAT entry. They lack palette and palette index sections entirely.

```
┌───────────────────────────────┐
│  File Header       (0x14 B)   │
├───────────────────────────────┤
│  Section 1: spriteData        │
│  (spriteDataSize bytes)       │
├───────────────────────────────┤
│  Section 2: indices1          │
│  (indicesSize bytes)          │
├───────────────────────────────┤
│  Section 3: metadata          │
│  (indicesSize × 6 bytes)      │
└───────────────────────────────┘
```

No `paletteData` section. No `indices2` section. Sprite access via `indices1` works identically to V1.

### V2 sprite header (7 bytes)

The V2 sprite header is 7 bytes (compared to 14 for V1). Its internal layout is not fully documented; it is preserved verbatim during conversion.

---

## 5. V1 Sprite RLE Stream

The RLE stream begins at offset +0x0E within each sprite's data (after the 14-byte header) and ends with byte `0xFF`.

### Command byte structure

```
  Bit 7  6  5  4  3  2  1  0
     ┌──┬──┬──┬──┬──┬──┬──┬──┐
     │ TYPE (3b) │ COUNT (5b) │
     └──┴──┴──┴──┴──┴──┴──┴──┘
      mask 0xE0   mask 0x1F
```

Two special values are not split into type/count:
- `0xFE` — end of row (advance to next scanline)
- `0xFF` — end of sprite

### Command types

| Type | Name | Data per pixel | Description |
|------|------|---------------|-------------|
| 0x00 | SKIP | 0 bytes | Skip *N* pixels (transparent) |
| 0x20 | OPAQUE | 1 byte | *N* opaque pixels. Each byte is an index into the sprite's palette → RGB565 color |
| 0x40 | BLEND_PAL | 2 bytes | *N* pixels: palette color blended with background (index + alpha) |
| 0x60 | BLEND_COMPLEX | 2 bytes | *N* pixels: complex blending through multiple LUT tables |
| 0x80 | TINT | 1 byte | *N* pixels: tint/shadow via LUT blending with background |
| 0xA0 | DARKEN | 0 bytes | Darken *N* background pixels: `(pixel & mask) >> 1` |
| 0xC0 | SKIP_LINES | 0 bytes | Skip *N* entire rows |

Maximum count per command: 31 (5 bits). Longer runs are split across multiple commands.

### Effect commands (0x40–0xA0)

Commands 0x40, 0x60, 0x80, and 0xA0 are runtime blending effects. They operate on the current background beneath the sprite and cannot be represented as static pixel colors. The game uses four LUT (Look-Up Table) arrays for these operations.

During extraction, these are stored as binary effect entries in a sidecar file. In the PNG output, they appear as **magenta marker pixels** (index 255).

---

## 6. V2 Sprite RLE Stream

V2 sprites embed RGB565 colors directly instead of palette indices. The stream begins at offset +7 (after the 7-byte header) and uses a different command structure.

### Special bytes (unchanged)
- `0xFE` — end of row
- `0xFF` — end of sprite

### Command pairs

Every non-special byte begins a two-byte command pair:

| Byte | Meaning |
|------|---------|
| `byte0` | Number of transparent pixels to skip before drawing |
| `byte1` | Draw command (see below) |

**If `byte1 < 0x80`** — Opaque pixels:
- Count = `byte1`
- Followed by `count × 2` bytes of inline RGB565 values (little-endian uint16)

**If `byte1 ≥ 0x80`** — Blended pixels:
- Count = `byte1 & 0x7F`
- Followed by `count × 3` bytes: one blend parameter byte + one RGB565 value (2 bytes LE) per pixel
- Blend parameter range: 0–31 (maps to transparency level)

---

## 7. Color Format — RGB565

All colors in BNK files are stored as 16-bit RGB565 (little-endian on disk):

```
  Bit 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
     ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
     │   Red (5 bits)   │  Green (6 bits)  │  Blue (5b) │
     └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
```

### Conversion formulas

**RGB565 → RGB888** (for PNG):
```
r8 = (r5 × 255 + 15) / 31
g8 = (g6 × 255 + 31) / 63
b8 = (b5 × 255 + 15) / 31
```

**RGB888 → RGB565** (from PNG):
```
r5 = (r8 × 31 + 127) / 255
g6 = (g8 × 63 + 127) / 255
b5 = (b8 × 31 + 127) / 255
```

> **Note:** This conversion is not perfectly reversible due to different bit depths. The tool preserves original RGB565 values for unchanged colors to avoid round-trip losses.

### RGB555 variant

When the game runs in a non-RGB565 display mode, palettes are converted from RGB565 to RGB555 on load (green channel shifted from 6 to 5 bits). Files on disk always store RGB565.

---

## 8. Extraction (BNK → PNG)

### Step 1: Read BANKI.DAT

Parse the flat array of 12-byte records. Identify the target bank and determine whether it is V1 (`paletteDataSize > 0`) or V2 (`paletteDataSize == 0`).

### Step 2: Split BNK into sections

Read the BNK file and split it into sections according to the sizes from BANKI.DAT:

1. File header (0x14 bytes) — saved as `header.bin`
2. spriteData — raw RLE data for all sprites
3. indices1 — uint32 offset array
4. metadata — 24 bytes per sprite
5. paletteData (V1 only) — palette color data
6. indices2 (V1 only) — uint32 palette offset array

### Step 3: Extract each sprite

**For V1 banks:**

1. Locate sprite RLE data using `indices1[i]`
2. Locate palette using `indices2[i]` (find next greater offset for shared palettes)
3. Save raw binary files (`rle.bin`, `palette.bin`, `meta.bin`) for lossless round-trip
4. Decode the V1 RLE stream into a pixel grid:
   - SKIP → transparent (PNG index 0)
   - OPAQUE → palette index + 1 (PNG indices 1–254)
   - Effect commands → magenta marker (PNG index 255), data saved to `effects.bin`
5. Save as **indexed PNG** (8-bit palette mode) with the following mapping:
   - Index 0: transparent (alpha = 0)
   - Indices 1–254: game palette colors 0–253 (RGB888 from RGB565 conversion)
   - Index 255: effect marker (magenta #FF00FF, alpha = 128)
6. Save decoded pixel indices as `pixels.bin` (for change detection during repack)

**For V2 banks:**

1. Locate sprite data using `indices1[i]`
2. Save raw data files (`rle.bin`, `meta.bin`, `spritedata.bin`, `indices1.bin`, `metadata.bin`)
3. Decode the V2 RLE stream:
   - Skip bytes → transparent pixels
   - Opaque pixels → RGB565 converted to RGBA (alpha = 255)
   - Blended pixels → RGB565 with blend parameter mapped to alpha (0–31 → 0–255)
4. Save as **RGBA PNG** (32-bit, not indexed — V2 has no palette)

---

## 9. Repacking (PNG → BNK)

### Step 1: Change detection (V1 only)

For each sprite, compare the current PNG pixel data against the saved `pixels.bin`:

- **If identical**: use the original `rle.bin` verbatim — guarantees byte-for-byte reproduction
- **If different**: re-encode the RLE stream from the modified PNG data

### Step 2: Re-encode RLE (V1, when changed)

1. Preserve the original 14-byte sprite header from `rle.bin`
2. Load effect entries from `effects.bin`
3. Scan each row of the pixel grid:
   - Consecutive transparent rows → emit SKIP_LINES commands
   - Transparent pixels → emit SKIP commands
   - Opaque pixels (PNG indices 1–254) → emit OPAQUE commands with game palette index (PNG index − 1)
   - Effect markers (PNG index 255) → emit original effect commands from `effects.bin`, grouped by type
   - End of row → emit END_LINE (0xFE)
4. End of sprite → emit END_SPRITE (0xFF)
5. Runs exceeding 31 pixels are split across multiple commands

### Step 3: Reconstruct palette (V1, when changed)

Read the PNG palette entries and convert back to RGB565. For each color:
- If it matches the original RGB565 value (after RGB565→RGB888→RGB565 round-trip), use the original value verbatim
- If it differs, convert the new RGB888 color to RGB565

### Step 4: Repack V2 banks

V2 banks are repacked from the saved raw binary data (`spritedata.bin`, `indices1.bin`, `metadata.bin`). PNG files are generated for viewing only; editing V2 sprites requires modifying the raw data directly.

### Step 5: Assemble BNK file

1. Concatenate all sprite RLE data → new `spriteData`
2. Build `indices1` from accumulated offsets
3. Concatenate all palette data → new `paletteData` (V1 only)
4. Build `indices2` from accumulated palette offsets (V1 only)
5. Write: `header.bin` + spriteData + indices1 + metadata + paletteData + indices2

### Step 6: Update BANKI.DAT

Write the new `indicesSize`, `spriteDataSize`, and `paletteDataSize` values to the corresponding bank entry in BANKI.DAT.

---

## 10. Extracted File Layout

### Top-level structure

```
output/
└── BANKI/                          ← named after the DAT file stem
    ├── info.txt                    ← global metadata
    ├── BANK_00/                    ← one directory per BNK file
    │   ├── header.bin              ← 0x14-byte BNK file header
    │   ├── info.txt                ← bank metadata
    │   ├── spritedata.bin          ← (V2 only) raw concatenated sprite data
    │   ├── indices1.bin            ← (V2 only) raw uint32 index array
    │   ├── metadata.bin            ← (V2 only) raw metadata block
    │   ├── 0000/                   ← sprite 0
    │   │   ├── sprite.png          ← viewable/editable image
    │   │   ├── rle.bin             ← original RLE data (for lossless round-trip)
    │   │   ├── meta.bin            ← 24-byte sprite metadata
    │   │   ├── palette.bin         ← (V1) original palette slice (RGB565)
    │   │   ├── pixels.bin          ← (V1) decoded pixel indices (for change detection)
    │   │   └── effects.bin         ← (V1) effect command entries
    │   ├── 0001/
    │   │   └── ...
    │   └── ...
    ├── BANK_01/
    │   └── ...
    └── ...
```

### Global info.txt

```
datFilename=BANKI.DAT
numBanks=200
```

| Key | Description |
|-----|-------------|
| `datFilename` | Original DAT filename (used during repack to name the output) |
| `numBanks` | Total number of bank slots (determines BANKI.DAT file size) |

### Per-bank info.txt

```
bankIndex=5
bnkFilename=BANK_05.BNK
numSprites=44
indicesSize=176
spriteDataSize=275519
paletteDataSize=512
```

| Key | Description |
|-----|-------------|
| `bankIndex` | Bank slot number in BANKI.DAT (0-based) |
| `bnkFilename` | Original BNK filename |
| `numSprites` | Number of sprites in this bank |
| `indicesSize` | Size of index arrays in bytes |
| `spriteDataSize` | Size of RLE sprite data in bytes |
| `paletteDataSize` | Size of palette data in bytes (0 for V2 banks) |

### Per-sprite files

**V1 sprites:**

| File | Format | Purpose |
|------|--------|---------|
| `sprite.png` | Indexed PNG, 8-bit | Viewable and editable sprite image |
| `rle.bin` | Binary | Original RLE stream (used verbatim if PNG unchanged) |
| `meta.bin` | Binary, 24 bytes | Sprite metadata (hotspot, bounding box) |
| `palette.bin` | Binary, uint16 LE array | Original RGB565 palette slice |
| `pixels.bin` | Binary, uint8 array | Decoded pixel indices (for change detection) |
| `effects.bin` | Binary | Effect command entries for blend/shadow/darken pixels |

**V2 sprites:**

| File | Format | Purpose |
|------|--------|---------|
| `sprite.png` | RGBA PNG, 32-bit | Viewable sprite image (with alpha for blended pixels) |
| `rle.bin` | Binary | Original RLE stream |
| `meta.bin` | Binary, 24 bytes | Sprite metadata |

### Effects file format

`effects.bin` stores non-opaque RLE commands that depend on the background:

```
┌──────────────────────────┐
│ uint32 count             │  Number of effect entries
├──────────────────────────┤
│ EffectEntry[0]  (8 B)    │
│ EffectEntry[1]  (8 B)    │
│ ...                      │
└──────────────────────────┘
```

Each `EffectEntry` (8 bytes):

| Offset | Size | Type | Field |
|--------|------|------|-------|
| +0 | 2 | uint16 | Row position |
| +2 | 2 | uint16 | Column position |
| +4 | 1 | uint8 | Command type (0x40, 0x60, 0x80, or 0xA0) |
| +5 | 1 | uint8 | Data size per pixel (0, 1, or 2) |
| +6 | 2 | uint8[2] | Raw data bytes (zero-padded) |

### PNG palette mapping (V1)

| PNG Index | Game Meaning | Alpha | Visual |
|-----------|-------------|-------|--------|
| 0 | Transparent pixel | 0 | Invisible |
| 1–254 | Game palette color 0–253 | 255 | Opaque color from RGB565 palette |
| 255 | Effect marker | 128 | Magenta (#FF00FF) |

The +1 shift reserves index 0 for transparency. Image editors that support indexed PNG mode (GIMP, Aseprite, GraphicsGale) can edit these files while preserving palette indices.

---

## 11. Lossless Round-Trip Guarantee

The conversion pipeline guarantees that extracting and repacking without any edits produces output files identical byte-for-byte to the originals. This relies on three mechanisms:

### 1. Binary RLE preservation

The original RLE stream is saved as `rle.bin` during extraction. If the PNG is unmodified, this exact byte sequence is used during repacking — no re-encoding occurs.

### 2. Change detection via pixels.bin

During extraction, the decoded pixel indices are saved as `pixels.bin`. During repacking, the PNG is decoded and compared byte-for-byte against `pixels.bin`. Only if they differ does re-encoding take place.

### 3. Palette value preservation

Original RGB565 palette values are saved in `palette.bin`. During repacking, each color in the PNG palette is compared against the original: if the RGB888 value matches the original RGB565→RGB888 conversion, the original RGB565 value is used verbatim, avoiding conversion losses.

### Scenario matrix

| Scenario | Path | Bytes identical? |
|----------|------|:----------------:|
| PNG not edited | `rle.bin` + `palette.bin` verbatim | ✓ Yes |
| PNG pixels edited | Re-encoded RLE + original palette | ✗ No (new RLE) |
| PNG palette edited | Re-encoded RLE + reconstructed palette | ✗ No (new palette + RLE) |
| Only `effects.bin` edited | Not detected as PNG change → verbatim | ✓ Yes |

> **Note:** Editing `effects.bin` alone does not trigger re-encoding because the effect data is not represented in the PNG pixels. To apply modified effects, you must also change at least one pixel in the PNG to force re-encoding.

---

*Documentation based on reverse engineering of the Exterminacja game executable (Techland, ~2001).*

---

## 12. Implementation errata (mh_tools, 2026-07-05)

The C++ implementation in `src/mh_dll/` (`mh_lib`'s `mh::bnk`/`mh::bmp` + the `mh_tools`
CLI) validated this document against the full retail corpus (`BANKI.DAT`, 200 slots, 64
non-empty banks = 63 V1 with 6841 sprites + `BANK_120.BNK` V2 with 2261 slots). Everything
above holds **except** the corrections below, plus a few facts the document leaves open.
Acceptance result: unpack → repack with no edits reproduces **all 65 files byte-for-byte**,
and a forced re-encode of all 8622 sprites decodes back semantically identical (91% even
byte-identical).

1. **The 0x14 file header is not opaque.** It is five LE `uint32` section sizes:
   `{spriteDataSize, indices1Size, metadataSize, paletteDataSize, indices2Size}` — verified
   byte-exact on all 64 banks. The tool regenerates it from the section sizes on repack
   (and warns if a stored header ever disagrees with the formula).
2. **§6 has the V2 blended-pixel triple backwards.** The 3 bytes per blended pixel are
   `[RGB565 lo][RGB565 hi][blendParam]` — the parameter is the **third** byte (strictly
   0–31 across all 60,233 shipped blend pixels), not the first.
3. **V2 `indices1` uses `0xFFFFFFFF` sentinels** for absent sprites (480 of `BANK_120`'s
   2261 slots). A sprite's slice ends at the next **non-sentinel** offset. Absent sprites
   still have a 24-byte metadata row.
4. **V1 canvas geometry** (the document never defines the pixel-grid size): with the
   14-byte header fields `xLeft@+4, yTop@+6, w1@+8, h1@+0xA, yStart@+0xC` (int16 LE),
   `canvasW = xLeft + w1 + 1` and `canvasH = yStart + h1 + 1`; RLE row 0 renders at canvas
   row `yStart`, every row starts at canvas column 0. `yStart == yTop` and the rule holds
   with zero violations for all 6841 sprites. Header bytes 0/2 sometimes look like
   dimensions but contradict the streams in 57 sprites — do not use them.
5. **The §8/§10 indexed-image scheme (index 0 = transparent, 1–254 = colors, 255 =
   marker) cannot represent the retail data.** Every shipped V1 palette slice is exactly
   256 colors, opaque runs reference indices 0–255 inclusive (2718 sprites use index ≥
   254; index 0 is a real color in 2646 sprites), and 6799/6841 sprites contain effect
   commands — that is 258 logical pixel values, two more than an 8-bit image can hold.
   `mh_tools` therefore emits **32-bit BGRA BMPs** instead: alpha 0 = transparent, 255 =
   opaque (true color); background-dependent effect pixels live in **per-type mask BMPs**
   (see the layout notes below), and a 16×16 `palette.bmp` swatch is the editable palette.
   On repack, edited opaque pixels map back to indices by exact color match (preferring
   the pixel's original index when the palette has duplicate colors).
6. **V2 blend params map to BMP alpha as `A = 8·p + 4`** (range 4–252): bijective
   (`p = A div 8`), and never 0 or 255, so the three pixel classes stay unambiguous in the
   BMP. **Direction verified in the blitter (2026-07-05): higher param = more opaque**, so
   the alpha mapping is directionally correct. The in-game math (from
   `llm_gfx_blit_sprite_rle_v2` + the LUT builder `llm_gfx_init_blend_luts`) is
   `dst = sprite565 + background × (31−p)/31` — the LUT builder stores brightness level *k*
   into row *31−k*, and the blitter indexes rows with `p` directly. So `p = 31` renders the
   pure sprite color and `p = 0` adds the full sprite color onto the untouched background.
   Note this is **additive** blending, not source-over: the sprite term never fades — low
   params make a glow/tint over the background, they don't make the sprite vanish.
7. Minor: the empty-sprite encoding is an all-zero 14-byte header + a bare `0xFF` (8
   occurrences); content rows always end with an explicit `0xFE` before a final `0xFF`
   everywhere else; original streams encode trailing blank rows out to exactly `canvasH`.
   The only V1 sprite with header `version >= 2` is bank 47 sprite 64 (not sprite 0, so no
   V1 bank trips the game's on-load V2 conversion).

File-layout deltas vs §10 (**extracted-layout v2**, `layout=2` in the root `info.txt`):
`sprite.png` → `sprite.bmp` (BGRA32, opaque art only), plus per-sprite `palette.bmp` (V1).
**Effect commands are stored as one mask BMP per RLE type** instead of the doc's
`effects.bin` + marker pixels — `fx_darken.bmp` (0xA0: white = shadow pixel, no payload),
`fx_tint.bmp` (0x80: `R=G=B = level×8`, level 1–31), `fx_blend.bmp` (0x40: `R` = palette
index 1–255, `G` = level×8) and `fx_blend2.bmp` (0x60, same encoding); `R = 0` means "no
effect here" (safe: every shipped payload byte0 is ≥ 1), a pixel may appear in at most one
mask, and repack **synthesizes** the commands from the masks — so shadows are freely
paintable (adding a DARKEN pixel = painting white on `fx_darken.bmp`). Masks may be
re-saved as 24-bit BMPs by editors; the reader accepts that. There are **no
`pixels.bin`/`effects.bin` sidecars** — change detection decodes the verbatim `rle.bin`
instead. Banks also keep raw `indices1.bin`/`indices2.bin` (palette-sharing groups survive
repack); V2 absent sprites are directories holding only `meta.bin`. V2 banks are **fully
re-encodable** (not view-only as §9 step 4 assumes) under the same change-detection rule.
