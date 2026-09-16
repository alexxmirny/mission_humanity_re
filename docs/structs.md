# Struct definitions (generated)

This file's table block is **fully
generated** from the live Ghidra database by the Ghidra-side struct dumper (run in Ghidra as
`mh_dump_structs.py`) — it dumps every structure in the `/llm` and `/Manual`
data-type categories with field comments. Never hand-edit the block; rerun the
script after retyping anything.

<!-- BEGIN generated-structs -->
#### `LZW_data` (size 0x1c, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `compressed_size` | `uint` |  |
| `+0x04` | `word_L` | `undefined4` |  |
| `+0x08` | `word_H` | `undefined4` |  |
| `+0x0c` | `bit_count` | `undefined4` |  |
| `+0x10` | `mask` | `undefined4` |  |
| `+0x14` | `shift` | `undefined4` |  |
| `+0x18` | `dict_size` | `undefined4` |  |

#### `fow_temp_struct_for_point` (size 0x4, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `p` | `map_t_coord_r` |  |
| `+0x02` | `_3` | `byte` |  |
| `+0x03` | `_4` | `byte` |  |

#### `llm_lobby_player_slot` (size 0x39, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `reserved_0x0` | `undefined1` |  |
| `+0x01` | `player_id` | `int` | assigned network player id -- matched vs _G_LLM_NET_LOCAL_PLAYER_INDEX (0x5d55ac) to find own slot, and by slot_for_player/slot_find_or_alloc. For a HUMAN it is copied to Players[].scenario_side_id (its map side); for an AI the side is auto (-1) so the id is gameplay-irrelevant and stays 0. Host = 0. |
| `+0x05` | `race` | `byte` | race/faction selector (2 options). build_players_step memcpy's slot+0x05 -> Players[].race_or_faction (descriptor+0x00). 'open' was a misnomer -- open/closed is slot_status. |
| `+0x06` | `color_index` | `byte` | feeds the 8-color spinner |
| `+0x07` | `f5` | `uint` | build_players_step memcpy's slot+0x07 -> Players[].ui_sprite_index (descriptor+0x02), but build_players_finish OVERWRITES that with a sprite/colour index -> this slot value is discarded; own role unconfirmed (no observed writer). |
| `+0x0b` | `slot_status` | `llm_lobby_slot_status` | occupancy/state (enum): OPEN=0, HUMAN=1, AI=2, CLOSED=3 |
| `+0x0c` | `reserved_0xc` | `undefined1` |  |
| `+0x0d` | `relation` | `byte[8]` | diplomacy vs each player slot; 2 = self/ally, else enemy |
| `+0x15` | `name` | `char[32]` | player display name (host stamps it from the peer table) |
| `+0x35` | `reserved_0x35` | `undefined1[4]` |  |

#### `llm_lobby_slot_cursor` (size 0x24, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `widget_state` | `pointer` | open/AI/closed state spinner widget (slot_cycle_state_cb; its value == slot_status) |
| `+0x04` | `widget_name` | `pointer` | player-name label + kick button (llm_ui_dlg_lobby_kick_confirm_trigger); name ptr @+0x38 |
| `+0x08` | `widget_race` | `pointer` | race spinner widget (slot_race_cycle_cb; drives slot.race -> Players.race_or_faction) |
| `+0x0c` | `widget_color` | `pointer` | 8-color spinner widget (slot_color_cycle_cb; drives color_index) |
| `+0x10` | `spin_state` | `pointer` | state spinner block; value(+0xc) mirrors slot_status (open/AI/closed), NOT a race |
| `+0x14` | `spin_race` | `pointer` | race spinner block (2 options); value(+0xc) = slot.race |
| `+0x18` | `spin_color` | `pointer` | {0, 8, 0, slot&7, 0x14} = 8 colors, default = slot index |
| `+0x1c` | `name_buf` | `pointer` | 0x20-byte per-slot player-name buffer (@0x6531eb) |
| `+0x20` | `slot_state` | `pointer` | llm_lobby_player_slot* |

#### `llm_net_session_desc` (size 0x400, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `session_handle` | `pointer` | matched against listbox row on join |
| `+0x04` | `name_blob` | `byte[32]` | session name/address, formatted for display |
| `+0x24` | `total_slots` | `int` | printed; in-progress shows total-current = free |
| `+0x28` | `max_players` | `int` | printed (unverified) |
| `+0x30` | `state` | `char` | -1 = enumerating/open, 0 = joinable, else 'game in progress' (text 0x311) |
| `+0x31` | `player_count` | `byte` |  |
| `+0x32` | `map_header` | `cfg_struct_map_header` | host's map header (cfg::struct::map_data_1); checksum field4_0x10 verified vs local map, mismatch renames local file away for refetch (MoveFileA) |
| `+0x25a` | `-` | `undefined4` |  |
| `+0x25e` | `protocol_version` | `int` | entry listed only if <= DAT_00654e12 |

#### `llm_panel_icon_file_entry` (size 0x14, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `filename` | `char[16]` | panel resource name (no path); table terminated by filename[0]==0, cap 313 |
| `+0x10` | `icon_slot` | `uint` | destination index in G_ICON_PTRS (LoadPanelData) |

#### `llm_snd_ambient_event` (size 0x24, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `sound_id` | `uint` | 1st arg to llm_snd_play |
| `+0x04` | `volume` | `uint` |  |
| `+0x08` | `replay_delay` | `double` | added to next_time after the sound plays (4-13s in init data) |
| `+0x10` | `retry_delay` | `double` | added when the chance roll fails (30s) |
| `+0x18` | `chance_pct` | `uint` | play probability % |
| `+0x1c` | `next_time` | `double` | next eligible game time (vs time::g::LAST_GAME_TIME) |

#### `llm_snd_ambient_planet_list` (size 0x2d4, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `count` | `int` |  |
| `+0x04` | `events` | `llm_snd_ambient_event[20]` |  |

#### `llm_tlo_registry_entry` (size 0x9, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char *` | tileset filename (Jungle.tlo, Kam256.tlo, ...) |
| `+0x04` | `unused0` | `uint` | always 0 |
| `+0x08` | `tlo_index` | `byte` | 1..8; cfg::GetTloIndex defaults to 1 (Jungle) for unknown names |

#### `rsr_file_entry` (size 0x40, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `filename` | `char[47]` |  |
| `+0x2f` | `magic` | `char[5]` |  |
| `+0x34` | `offset` | `uint` |  |
| `+0x38` | `compressed` | `uint` |  |
| `+0x3c` | `decompressed` | `uint` |  |

#### `rsr_rsr_file_data` (size 0xc, category `/Manual`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `file_handler` | `util_struct_file_handle *` |  |
| `+0x04` | `file_entry_ptr` | `rsr_file_entry *` |  |
| `+0x08` | `file_count` | `uint` |  |

#### `cfg_dynamic_struct_Anim` (size 0x11c, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `length` | `int` | Created by Rename Structure Field action |
| `+0x104` | `time` | `double` |  |
| `+0x10c` | `step` | `int` |  |
| `+0x110` | `repeat` | `int` | Created by Rename Structure Field action |
| `+0x114` | `frame_index` | `cfg_t_frame_index` |  |
| `+0x118` | `cyclic` | `int` |  |

#### `cfg_dynamic_struct_Bank` (size 0x84, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `index` | `int` | Created by Rename Structure Field action |

#### `cfg_dynamic_struct_Building` (size 0x2c48, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `-` | `MH_STR` |  |
| `+0x100` | `invention` | `MH_STR` |  |
| `+0x180` | `anim` | `MH_STR[12]` |  |
| `+0x780` | `component` | `MH_STR[8]` |  |
| `+0xb80` | `component_quant` | `int` |  |
| `+0xb84` | `anim_p` | `int[8]` |  |
| `+0xba4` | `info_txt` | `MH_STR` |  |
| `+0xc24` | `info_flc` | `MH_STR` |  |
| `+0xca4` | `sprite_quantity` | `int` |  |
| `+0xca8` | `area` | `int[10][10]` |  |
| `+0xe38` | `height` | `int` |  |
| `+0xe3c` | `weapon` | `MH_STR` |  |
| `+0xebc` | `frame` | `MH_STR` |  |
| `+0xf3c` | `frame_2` | `MH_STR` |  |
| `+0xfbc` | `equivalent` | `MH_STR` |  |
| `+0x103c` | `type` | `MH_STR` | Created by Rename Structure Field action |
| `+0x10bc` | `ai_build` | `MH_STR` |  |
| `+0x113c` | `parametr` | `int` |  |
| `+0x1140` | `energy` | `int` | Created by Rename Structure Field action |
| `+0x1144` | `electric_power` | `int` |  |
| `+0x1148` | `human` | `int` |  |
| `+0x114c` | `builder` | `int` |  |
| `+0x1150` | `upgrade` | `MH_STR` |  |
| `+0x11d0` | `resource_name` | `MH_STR[7]` |  |
| `+0x1550` | `resource_val` | `int[7]` |  |
| `+0x156c` | `build_time` | `double` |  |
| `+0x1574` | `unit_name` | `MH_STR[20]` |  |
| `+0x1f74` | `unit_quant` | `double[20]` |  |
| `+0x2014` | `capacity_name` | `MH_STR[10]` |  |
| `+0x2514` | `extract_fuel_name` | `MH_STR[10]` |  |
| `+0x2a14` | `capacity_val` | `double[10]` |  |
| `+0x2a64` | `extract_fuel_val` | `double[10]` |  |
| `+0x2ab4` | `sight` | `int` | Created by Rename Structure Field action |
| `+0x2ab8` | `velocity` | `double` |  |
| `+0x2ac0` | `ability` | `int` |  |
| `+0x2ac4` | `icon` | `MH_STR` |  |
| `+0x2b44` | `sound_explo` | `MH_STR` |  |
| `+0x2bc4` | `sound_click` | `MH_STR` |  |
| `+0x2c44` | `trace` | `int` |  |

#### `cfg_dynamic_struct_Define` (size 0x84, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char[128]` |  |
| `+0x80` | `index` | `cfg_t_define_index` |  |

#### `cfg_dynamic_struct_Icon` (size 0x84, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `index` | `int` | Created by Rename Structure Field action |

#### `cfg_dynamic_struct_Planet` (size 0x3a8, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `map` | `MH_STR` |  |
| `+0x100` | `bank` | `bool[100]` |  |
| `+0x164` | `invention` | `MH_STR` |  |
| `+0x1e4` | `icon` | `MH_STR` |  |
| `+0x264` | `info_txt` | `MH_STR` |  |
| `+0x2e4` | `info_flc` | `MH_STR` |  |
| `+0x364` | `x1` | `int` |  |
| `+0x368` | `y1` | `int` |  |
| `+0x36c` | `x2` | `int` |  |
| `+0x370` | `y2` | `int` |  |
| `+0x374` | `coordinate_x` | `int` |  |
| `+0x378` | `coordinate_y` | `int` |  |
| `+0x37c` | `asteroids` | `uint` |  |
| `+0x380` | `turn_speed` | `int` |  |
| `+0x384` | `enemy` | `uint` |  |
| `+0x388` | `source_mul` | `int[4]` |  |
| `+0x398` | `source_add` | `int[4]` |  |

#### `cfg_dynamic_struct_Progress` (size 0x904, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `depend` | `MH_STR[16]` |  |
| `+0x880` | `depend_count` | `int` | Created by Rename Structure Field action |
| `+0x884` | `type` | `MH_STR` | Created by Rename Structure Field action |

#### `cfg_dynamic_struct_Project` (size 0x518, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `invention` | `MH_STR` |  |
| `+0x100` | `icon` | `MH_STR` |  |
| `+0x180` | `type` | `MH_STR` |  |
| `+0x200` | `build_time` | `double` |  |
| `+0x208` | `info_txt` | `MH_STR` |  |
| `+0x288` | `info_flc` | `MH_STR` |  |
| `+0x308` | `resource_name` | `MH_STR[4]` |  |
| `+0x508` | `resource_val` | `int[4]` |  |

#### `cfg_dynamic_struct_Sprite` (size 0x84, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `frame_index` | `cfg_t_frame_index` |  |

#### `cfg_dynamic_struct_Stone` (size 0x180, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x80` | `sprite` | `MH_STR` | Created by Rename Structure Field action |

#### `cfg_dynamic_struct_System` (size 0x1180, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char[128]` |  |
| `+0x80` | `invention` | `char[128]` |  |
| `+0x100` | `icon` | `char[128]` |  |
| `+0x180` | `planets` | `MH_STR[32]` |  |

#### `cfg_dynamic_struct_Text` (size 0x174, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `value` | `MH_STR` |  |
| `+0x80` | `text` | `WCHAR[120]` |  |

#### `cfg_dynamic_struct_Tree` (size 0x1e8, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `hz` | `MH_STR` |  |
| `+0x180` | `area` | `int[5][5]` |  |
| `+0x1e4` | `-` | `int` |  |

#### `cfg_dynamic_struct_Unit` (size 0xee4, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `type` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `invention` | `MH_STR` | Created by Rename Structure Field action |
| `+0x180` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x200` | `sprite_shadow` | `MH_STR` | Created by Rename Structure Field action |
| `+0x280` | `sprite_type` | `MH_STR` | Created by Rename Structure Field action |
| `+0x300` | `frame` | `MH_STR` | Created by Rename Structure Field action |
| `+0x380` | `frame_2` | `MH_STR` | Created by Rename Structure Field action |
| `+0x400` | `equivalent` | `MH_STR` | Created by Rename Structure Field action |
| `+0x480` | `anim_explo` | `MH_STR` | Created by Rename Structure Field action |
| `+0x500` | `info_txt` | `MH_STR` |  |
| `+0x580` | `info_flc` | `MH_STR` |  |
| `+0x600` | `step_speed` | `double` |  |
| `+0x608` | `turn_speed` | `double` |  |
| `+0x610` | `armor` | `int` |  |
| `+0x614` | `energy` | `int` |  |
| `+0x618` | `p` | `int[5]` |  |
| `+0x62c` | `build_time` | `double` |  |
| `+0x634` | `resource_count` | `int` |  |
| `+0x638` | `resource_name` | `MH_STR[7]` |  |
| `+0x9b8` | `resource_val` | `int[7]` |  |
| `+0x9d4` | `weapon_name` | `MH_STR[4]` |  |
| `+0xbd4` | `independent` | `MH_STR` |  |
| `+0xc54` | `sight` | `int` | Created by Rename Structure Field action |
| `+0xc58` | `icon` | `MH_STR` | Created by Rename Structure Field action |
| `+0xcd8` | `sound_explo` | `MH_STR` | Created by Rename Structure Field action |
| `+0xd58` | `sound_move` | `MH_STR` |  |
| `+0xdd8` | `human` | `int` |  |
| `+0xddc` | `soldier_type` | `MH_STR` |  |
| `+0xe5c` | `trace` | `int` |  |
| `+0xe60` | `ai_unit` | `MH_STR` |  |
| `+0xee0` | `ai_level` | `int` |  |

#### `cfg_dynamic_struct_Upgrade` (size 0x9a8, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `invention` | `MH_STR` |  |
| `+0x100` | `type` | `MH_STR` |  |
| `+0x180` | `object` | `MH_STR[16]` |  |
| `+0x980` | `range_min` | `int` |  |
| `+0x984` | `range_max` | `int` |  |
| `+0x988` | `power` | `int` |  |
| `+0x98c` | `missing` | `int` |  |
| `+0x990` | `step_speed` | `double` |  |
| `+0x998` | `turn_speed` | `double` |  |
| `+0x9a0` | `energy` | `int` |  |
| `+0x9a4` | `sight` | `int` |  |

#### `cfg_dynamic_struct_Weapon` (size 0x6e4, category `/Manual/cfg/dynamic`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `type` | `MH_STR` |  |
| `+0x100` | `ammo` | `int` |  |
| `+0x104` | `pocket` | `int` |  |
| `+0x108` | `short_time` | `double` |  |
| `+0x110` | `long_time` | `double` |  |
| `+0x118` | `range_min` | `int` |  |
| `+0x11c` | `range_max` | `int` |  |
| `+0x120` | `fire_explo` | `MH_STR` |  |
| `+0x1a0` | `target_explo` | `MH_STR` |  |
| `+0x220` | `power` | `int` |  |
| `+0x224` | `speed` | `double` |  |
| `+0x22c` | `length` | `int` |  |
| `+0x230` | `missing` | `int` |  |
| `+0x234` | `bullet_anim` | `MH_STR[4]` |  |
| `+0x434` | `probability` | `int[4]` |  |
| `+0x444` | `fire_range` | `int` |  |
| `+0x448` | `hz` | `undefined1` |  |
| `+0x450` | `explo_time` | `double` |  |
| `+0x458` | `smoke_time` | `double` |  |
| `+0x460` | `smoke_sprite` | `MH_STR` |  |
| `+0x4e0` | `homing` | `MH_STR` |  |
| `+0x560` | `target` | `MH_STR` |  |
| `+0x5e0` | `sound_fire` | `MH_STR` |  |
| `+0x660` | `sound_target` | `MH_STR` |  |

#### `cfg_final_struct_Anim` (size 0x10, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `sprite_id` | `cfg_t_sprite_index` | Created by Rename Structure Field action |
| `+0x04` | `next` | `cfg_t_frame_index` |  |
| `+0x08` | `time` | `double` |  |

#### `cfg_final_struct_Building` (size 0x842, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `id` | `cfg_t_define_index` |  |
| `+0x04` | `invention` | `cfg_t_invention_index_s` |  |
| `+0x06` | `ai_build` | `cfg_enum_ai_E_BUILD` |  |
| `+0x08` | `type` | `cfg_enum_E_BUILDING` |  |
| `+0x09` | `height` | `undefined1` | Created by Rename Structure Field action |
| `+0x0a` | `width` | `byte` | Created by Rename Structure Field action |
| `+0x0b` | `area` | `byte[10][10]` | Created by Rename Structure Field action |
| `+0x6f` | `area_2` | `byte[10][10]` |  |
| `+0xd3` | `area_3` | `byte[10][10]` |  |
| `+0x137` | `sound_explo` | `cfg_t_define_index` | Created by Rename Structure Field action |
| `+0x13b` | `sound_click` | `cfg_t_define_index` |  |
| `+0x13f` | `frame` | `cfg_t_frame_index` | Created by Rename Structure Field action |
| `+0x143` | `frame_2` | `cfg_t_frame_index` |  |
| `+0x147` | `width_2` | `uint` | Created by Rename Structure Field action |
| `+0x14b` | `width_3` | `uint` | Created by Rename Structure Field action |
| `+0x14f` | `anim` | `cfg_t_frame_index[12]` |  |
| `+0x17f` | `trace` | `int` | Created by Rename Structure Field action |
| `+0x183` | `sprite_quantity` | `byte` | Created by Rename Structure Field action |
| `+0x184` | `component_quant` | `int` | Created by Rename Structure Field action |
| `+0x188` | `component` | `cfg_t_frame_index[7]` | Created by Rename Structure Field action |
| `+0x1a4` | `pip_slot_count` | `int` | SIM1B (2026-08-12; building_tick machinery slice). Per-building-TYPE count of 'pip' status-icon slots -- llm_strat_bldg_tick_pip_anim's outer loop bound (0x0047928d: CMP EDX, dword ptr [building_id*sizeof(cfg_building)+0xd9ee24]), compared against the building INSTANCE's fixed 4-entry pip_level/pip_frame/pip_timer arrays. Was the leading int32 of an undecoded 36-byte undef_block; split out because the decompiler was already resolving it as a distinct dword read. |
| `+0x1a8` | `undef_block_tail` | `byte[32]` | SIM1B (2026-08-12). undef_block's remaining 32 bytes (offset 0x1a8..0x1c7), still undecoded -- no reader found yet. |
| `+0x1c8` | `info_txt` | `MH_STR_s` |  |
| `+0x208` | `info_flc` | `MH_STR_s` |  |
| `+0x248` | `icon` | `cfg_t_define_index` | Created by Rename Structure Field action |
| `+0x24c` | `energy` | `double` | Building type's MAX energy (HP/charge cap). ENERGY = the HP-like charge stat, NOT the POWER resource; energy_refill_full copies this into map_object_building.energy to max it out. Power generation/consumption is power_stats (generated/consumed), a separate concept. |
| `+0x254` | `electric_power` | `int` | Created by Rename Structure Field action |
| `+0x258` | `worker_count` | `int` | Created by Rename Structure Field action |
| `+0x25c` | `builder_count` | `int` |  |
| `+0x260` | `sight` | `byte` |  |
| `+0x261` | `upgrade_index` | `cfg_t_building_index` | Created by Rename Structure Field action |
| `+0x265` | `upgrade_lvl` | `int` |  |
| `+0x269` | `kill_score` | `int` | Points added to the KILLER's runtime units[killer].experience (+0x28) when a building of THIS TYPE is destroyed by a different player -- llm_strat_bldg_kill_credit reads it @0x0044c81e and adds it @0x0044c824, the same destination address (0xdd8c70) the unit-side credit path uses. Currently HARDCODED to 10 for EVERY building type by cfg_final_building_Construct @0x004570da (unconditional). Sibling of cfg_final_struct_Unit.kill_score. Named 2026-08-22 (RI-SIM SIM-READY prep). |
| `+0x26d` | `state_transition_ids` | `int[4]` | SIM1-G4 first slice (2026-08-22). Per-building-TYPE array; only index [1] (absolute offset 0x271, cfg_buildings_base+building_id*0x842+0x271) has a confirmed reader in this batch -- its LOW 16 BITS are read as a llm_strat_bldg_state value: the building's next tick-state after a completed LAND (llm_strat_bldg_state_land_activate, MOVZX AX @0x004712e4) or a completed CHARGE cycle (llm_strat_bldg_state_charge_gate, two equivalent reads at 0x004727b0 and 0x004728a5). Indices [0]/[2]/[3] and the upper 16 bits of [1] have no reader found in this batch -- named the whole array rather than guess-splitting further, since only one element and one half-width is evidenced. |
| `+0x27d` | `unit_quant` | `double[100]` |  |
| `+0x59d` | `extract_id` | `int[4]` |  |
| `+0x5ad` | `extract_val` | `int[4]` |  |
| `+0x5bd` | `extract_val_2` | `int[10]` |  |
| `+0x5e5` | `project_type` | `int` |  |
| `+0x5e9` | `capacity` | `int[10]` |  |
| `+0x611` | `unit_housing_capacity` | `int` | Per-building unit-housing capacity by class (soldiers/vehicles/planes/helis); read by llm_strat_add_unit_capacity_* -> UNIT_HOUSING_STATS.cap_accum_*, gated in prod_try_start_unit (used_X vs cap_prev_X). HARDCODED to 50 at cfg-load: cfg::final::building::Construct @0x00457e8e does `=0x32` right after `=building.parametr`, so the cfg PARAMETR field is a DEAD STORE (every housing building = 50). Revive per-building control by NOPing the 0x32 overwrite + setting PARAMETR per building. (was prob_total_unit_capacity) |
| `+0x615` | `unit_capacity` | `byte[100]` |  |
| `+0x679` | `capacity_2` | `int[10]` |  |
| `+0x6a1` | `human_transport` | `int` | Created by Rename Structure Field action |
| `+0x6a5` | `interplanetary_capable` | `int` | Per-building-TYPE flag, tested only as != 0: nonzero lets this building act on a planet OTHER than the one currently being simulated. Set by cfg_final_building_Construct -- defaulted 1 then immediately 0 before the type switch, then hardcoded 1 on the A/H_MOTHER and A/H_PORT branches; A/H_SHUTTLE is the ONLY branch that takes it from the config, copying the cfg `ability` int verbatim @0x00456f8a. Readers: llm_strat_order_queue_dispatch @0x00468036, whose case-0xf gate is (interplanetary_capable != 0) OR (target_planet == G_PLANET_INDEX) -- i.e. a shuttle order aimed at another planet is normally deferred unless this is set; and llm_ui_info_text_build @0x004ca21e, which uses it to decide whether to show a travel-time stat row. NAME CAVEAT: the mechanism is settled, but the designer-facing label is not -- the cfg column this comes from is called `ability`, and whether that is a richer bitmask elsewhere was not established. Named 2026-08-22 (RI-SIM SIM-READY prep). |
| `+0x6a9` | `fuel` | `cfg_struct_resource[7]` | Per-building-TYPE fuel deltas applied on interplanetary departure (llm_prod_shuttle_fuel_apply/llm_prod_shuttle_fuel_check), {id,val} pairs like resource/resource_2. OFFSET+SIZE CORRECTED 2026-08-14 (was declared cfg_struct_resource[5] at +0x6a5 -- 4 bytes short and 2 slots short): true base +0x6a9, confirmed via fuel[0].id's raw read address (0xd9f329) against the independently-confirmed Building[] table base (0xd9ec80, from the already-correct resource@+0x6f6/resource_2@+0x736 siblings); true slot count 7, matching both functions' own `i<7` loop bound (the CFG_RESOURCE_SLOTS(7) convention resource/resource_2 already use). See tasks/tooling.md. |
| `+0x6e1` | `velocity` | `double` |  |
| `+0x6e9` | `weapon_id` | `int` | Index into the Weapon[] config table of the weapon this building type mounts; 0 = unarmed. Recovered 2026-08-01 (AI1A layer 1) from the seven readers: llm_strat_ai_target_ref_has_ground_weapon (Weapon[id].+1 & 1), llm_strat_bldg_has_aa_weapon (& 2), llm_strat_ai_building_defense_weapon_range and llm_strat_ai_turret_threat_rescan (Weapon[id].range_max[player]), llm_strat_ai_group_classify_target_object, Construct, CreateBuilding. Read as a DWORD by every AI reader; CreateBuilding copies only the low byte. |
| `+0x6ed` | `per_shot_cost` | `double` | Per-building-TYPE turret action cost drawn from _G_LLM_STRAT_TICK_BUDGET once per turret action: llm_strat_bldg_state_turret_scan spends it directly per idle aim-step (FCOMP @0x00471b2b, FSUB @0x00471d8f), llm_strat_bldg_state_turret_attack spends it scaled by _G_LLM_STRAT_TURRET_ATTACK_INTERVAL_SCALE (FLD @0x00471e95, FMUL @0x00471e9b). HARDCODED to 0.1 for every building type by cfg_final_building_Construct, written as two dwords 0x9999999a/0x3fb99999 @0x00457eb5/0x00457ebf = IEEE-754 0.1. The .c showing two reads into two doubles is a DECOMPILER ARTIFACT: the .asm copies ONE double via two 32-bit MOVs into one stack slot and reads that slot twice. Named 2026-08-22 (RI-SIM SIM-READY prep). |
| `+0x6f5` | `staffs_workers` | `byte` | Per-building-TYPE flag (0/1): 'staffs workers during NORMAL operation'. Sole reader: llm_strat_bldg_uses_workers (0x004988b0). Written only by cfg_final_building_Construct (0x00459f6d/0x00459f80) as a literal 1 or 0, selected from the cfg source record's building-kind byte: 1 for kinds {1,2,3,5,0x15-0x17,0x19,0x1f}, else 0. Caveat: flag==0 does NOT mean the building never holds workers -- llm_strat_bldg_add_workers still adds builder_count workers during CONSTRUCTION/CHARGE_STEP/UPGRADING/DISMANTLING regardless of this flag. Named 2026-08-13 (RI-SIM SIM1B fifth slice, uses_workers translation). |
| `+0x6f6` | `resource` | `cfg_struct_resource[7]` |  |
| `+0x72e` | `build_time_2` | `double` |  |
| `+0x736` | `resource_2` | `cfg_struct_resource[7]` |  |
| `+0x76e` | `build_time_d` | `double` |  |
| `+0x776` | `energy_d` | `double` |  |
| `+0x77e` | `park_offset_x` | `int` | SIM1F fourth slice (2026-08-17; map_CreateBuilding). Storage-building parking-position tile offset (dx from the building's own origin), used for a docked unit's park_x -- distinct from shuttle_pad_offset_x (+0x782, the exit/launch tile). map_CreateBuilding is the sole reader: `unit_storage[player][sub_id].park_x = x_b + Building[building_id].park_offset_x & general.width_mask`. Was inside reserved_0x77e[4], undifferentiated padding since the 2026-08-12 shuttle_pad_offset_x split. |
| `+0x782` | `shuttle_pad_offset_x` | `int` | SIM1B (2026-08-12). Landing-pad tile offset (dx from the building's own origin) for A_SHUTTLE/H_SHUTTLE building types -- llm_bldg_placement_check_and_preview's shuttle arm reads this alongside shuttle_pad_offset_y to preview/legality-check the pad tile separately from the 10x10 footprint mask. Was inside the undifferentiated 0x77e..0x82d pad; split out because the decompiler was already resolving it as a distinct field_0x782 access. |
| `+0x786` | `park_offset_y` | `int` | SIM1F fourth slice (2026-08-17; map_CreateBuilding). Paired dy offset for park_offset_x above -- feeds unit_storage[...].park_y. Was inside reserved_0x786[4]. |
| `+0x78a` | `shuttle_pad_offset_y` | `int` | SIM1B (2026-08-12). See shuttle_pad_offset_x -- the paired dy offset. |
| `+0x78e` | `dock_lift_offset_x` | `int` | RD-A readers readiness (2026-08-24). Per-building-TYPE X pixel offset for a docked AIR unit's sprite while it is LANDING/TAKING OFF: llm_strat_render_docked_unit_air FILDs it (0x00450e9c), multiplies by the unit's current elevation ratio and adds the truncated result into the sprite's X pixel coordinate. Signed -- llm_strat_bldg_init_defaults stores 0xffffffdd (-35) at 0x0045aae3. Baked per building category at cfg-load by llm_strat_bldg_init_defaults (13 write sites), not parsed from the cfg file. X half of the (dock_lift_offset_x, dock_lift_offset_y) pair; was the head of the undifferentiated _pad_0x78e. |
| `+0x792` | `dock_lift_offset_y` | `int` | RD-A readers readiness (2026-08-24). Y half of dock_lift_offset_x: FILD'd at 0x00450eff, scaled by the same elevation ratio and added into the docked air unit's Y pixel coordinate. Written in lockstep with the X half by llm_strat_bldg_init_defaults (e.g. 0x0045aaf7 stores 0xf alongside the X half's -35). |
| `+0x796` | `heading_pixel_offset_x` | `int[8]` | SIM1-H (2026-09-10). Per-heading PIXEL x offset, one int32 per heading 0..7 (stride 4). Written ONLY by llm_strat_bldg_init_defaults (0x0045a3c7 and the sibling loop at 0x0045a7b2), which is the sole writer of this region in the whole sim set. Value = (sprite_meta[Anim[anim[0]+1].sprite_id].origin_x + sprite_anchor_offset_x) - ((width-1)/2*32 + MOVE_MICROSTEPS[heading][0].x_off). It is the microstep-0 (ENTRY) counterpart of dock_lift_offset_x @0x78e, which uses microstep 31 (the EXIT) at one fixed heading. Split out of _pad_0x796[128]; the heading count comes from the loop bound, not from the pad size. |
| `+0x7b6` | `heading_pixel_offset_y` | `int[8]` | SIM1-H (2026-09-10). The paired per-heading PIXEL y offset -- see heading_pixel_offset_x @0x796. Written at 0x0045a432 and 0x0045a81d, same address form, same heading index, reading MOVE_MICROSTEPS[heading][0].y_off (+0xae3741) and sprite_meta origin_y (MOVSX word [.. + 0x714126], read SIGNED). |
| `+0x7d6` | `_pad_0x7d6` | `byte[64]` | SIM1-H (2026-09-10). The REMAINDER of the old _pad_0x796[128] after its first 64 bytes were decoded into heading_pixel_offset_x/y. Still undecoded, and deliberately so: llm_strat_bldg_init_defaults -- the region's only writer in the sim set -- never stores here, so there is no evidence to name it from. Ends at door_exit_route @0x816. |
| `+0x816` | `door_exit_route` | `byte[10]` | Per-building-TYPE short byte sequence read by llm_strat_storage_setup_exit_path (indexed [building_id*sizeof(cfg_final_struct_Building)+entry], 0xff-terminated, up to 300 entries per the loop bound -- but only 10 bytes are guaranteed before door_approach_route begins; an unterminated route longer than 10 bytes reads on into door_approach_route/its padding, matching the original's own unbounded read). Was the tail of the undifferentiated 0x78e..0x81f (146-byte) pad. SIM1-G3 third slice (2026-08-21). |
| `+0x820` | `door_approach_route` | `byte[14]` | Absorbs the former single-byte 'facing' field (element [0] is byte-identical to it -- read as a scalar orientation by llm_strat_locate_active_port/llm_strat_storage_get_approach_tile/llm_strat_unit_group_step_ground/_plane, update those 4 call sites to door_approach_route[0]) plus the 13 bytes of reserved_0x821 padding immediately after it. llm_strat_storage_setup_approach_path reads the whole span as a 0xff-terminated byte sequence, same shape as door_exit_route above. SIM1-G3 third slice (2026-08-21). |
| `+0x82e` | `equivalent` | `cfg_t_define_index` | Created by Rename Structure Field action |
| `+0x832` | `sprite_offset_x` | `int` | baked (x,y) screen-position bias applied to every Building.anim[]/undef_block decoration sprite layer when drawn (llm_strat_render_tile_object) -- was anonymous padding |
| `+0x836` | `sprite_offset_y` | `int` | see sprite_offset_x |
| `+0x83a` | `selection_marker_offset_x` | `int` | RD-A readers readiness (2026-08-24). Per-building-TYPE X correction used ONLY when drawing the selection-marker sprite (.frame/.frame_2) over a building. Baked at cfg-load as sprite_offset_x minus llm_gfx_bldg_frame_center_offset's out_dx (write 0x0045b669), i.e. it re-centres the marker on the frame's true visual centre rather than the building's normal draw anchor. Read as a plain dword (never FILD'd) into EDX = llm_gfx_draw_sprite's dst_x by llm_strat_render_selection_markers at 0x0044f2df and 0x0044f540. |
| `+0x83e` | `selection_marker_offset_y` | `int` | RD-A readers readiness (2026-08-24). Y half of selection_marker_offset_x: sprite_offset_y minus llm_gfx_bldg_frame_center_offset's out_dy (write 0x0045b68c), read into EBX = llm_gfx_draw_sprite's dst_y at 0x0044f2b0 and 0x0044f511. Last field of the record (struct ends at 0x842). |

#### `cfg_final_struct_Invention` (size 0x67, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `depend` | `cfg_t_invention_index_s[16]` |  |
| `+0x20` | `type` | `cfg_enum_E_INVETION_TYPE` |  |
| `+0x21` | `index` | `cfg_t_invention_index_s` |  |
| `+0x23` | `some_trash` | `MH_STR_s` |  |
| `+0x63` | `f2` | `int` |  |

#### `cfg_final_struct_Planet` (size 0x427, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `invention_index` | `cfg_t_invention_index` |  |
| `+0x04` | `name` | `cfg_t_define_index` |  |
| `+0x08` | `system_index` | `int` |  |
| `+0x0c` | `icon_index` | `cfg_t_icon_index` |  |
| `+0x10` | `path_unc` | `char[128]` |  |
| `+0x10f` | `map_name` | `MH_STR` |  |
| `+0x20e` | `tlo_file` | `MH_STR` | TLO/tileset filename for this planet's map, NUL-terminated. Retyped 2026-09-01 (was a lone `undefined1` + 254 undefined bytes, which modelled a 1-byte field where a real string lives). THE SLOT IS A STRING, three ways: (1) offset arithmetic -- path_unc @+0x10, map_name @+0x10f, tlo_file @+0x20e, i.e. 0xff between each, so this is the THIRD of three equal string slots; (2) llm_game_start_tutorial @0x004bb1b1-0x004bb21c copies current_map_data().path_unc, .map_name and .tlo_name into this planet's three slots with the SAME byte-pair loop (strcpy shape, stops at and includes the first NUL) -- a 1-byte destination cannot receive it; (3) typed MH_STR (char[128]) to match map_name exactly rather than the full 0xff span, so all three slots keep one shape and the 127-byte tail stays untyped as it already is for path_unc/map_name. |
| `+0x30d` | `info_txt` | `char[64]` |  |
| `+0x34d` | `info_flc` | `char[64]` |  |
| `+0x38d` | `bank` | `bool[100]` |  |
| `+0x3f1` | `source_mul` | `int[4]` |  |
| `+0x401` | `source_add` | `int[4]` |  |
| `+0x411` | `coordinate_x` | `uint` |  |
| `+0x415` | `coordinate_y` | `uint` |  |
| `+0x419` | `asteriods` | `uint` |  |
| `+0x41d` | `turn_speed` | `uint` |  |
| `+0x421` | `enemy` | `uint` | Created by retype action |
| `+0x425` | `tlo_index` | `cfg_t_tlo_index` |  |
| `+0x426` | `soldier_sprite_bank_offset` | `byte` | RD-A readers readiness (2026-08-24). Per-planet frame-BANK offset added directly to `Unit[proto].sprite + <crew soldier anim frame, 0..0x3f>` when the strategic renderers draw a building's/unit's crew soldiers (llm_strat_render_bldg_docked_unit 0x004519af, llm_strat_render_tile_object 0x00452517). Selects one of up to three alternate 64-frame soldier-sprite banks: values are 0x00 / 0x40 / 0x80. NOT a cfg column -- cfg_final_planet_Construct computes it at planet-load as (n-1)*0x40 where n in {1,2,3} comes from a switch on this planet's tlo_index (write 0x0045b9dd, SHL AL,0x6 at 0x0045b9d1); tlo_index itself is resolved from the loaded .MP map's TLO/tileset name via cfg_GetTloIndex. Last byte of the record. |

#### `cfg_final_struct_Project` (size 0xd0, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `invention` | `cfg_t_invention_index` |  |
| `+0x04` | `name` | `cfg_t_define_index` | Created by Rename Structure Field action |
| `+0x08` | `icon` | `cfg_t_icon_index` |  |
| `+0x0c` | `type` | `int` |  |
| `+0x10` | `resource` | `cfg_struct_resource[7]` |  |
| `+0x48` | `build_time` | `double` |  |
| `+0x50` | `info_txt` | `MH_STR_s` |  |
| `+0x90` | `info_flc` | `MH_STR_s` |  |

#### `cfg_final_struct_System` (size 0x8c, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `invention` | `cfg_t_invention_index` | cfg keyword INVENTION -> cfg_dynamic_GetInventionIndex. Index into cfg_final_data_Progress: the invention this star system grants. cfg_final_system_Construct back-writes Progress[invention].index = this system's own index (0x0045e4d1). Read as a ushort at 0x00498745 and passed to game_UpdateProgress (0x004402b0), whose SYSTEM case grants and finalizes it to every active player. |
| `+0x04` | `name` | `cfg_t_define_index` | The `SYSTEM <name>` section-header token -> cfg_dynamic_GetDefineIndex. A display-name id: every reader shifts it left 2 and indexes the localized string table, `[name*4 + 0x0058440c]` = G_TEXT_PTRS -- llm_ui_main_panel_draw (0x004159be/0x004159c7), llm_game_notify_system_available (0x004401d6), llm_strat_player_presence_lost (0x00498708). |
| `+0x08` | `icon` | `cfg_t_icon_index` | cfg keyword ICON -> cfg_dynamic_GetIconIndex. Written only by cfg_final_system_Construct (0x0045e8a8); its one non-constructor reader is cfg_ConstructSystems at 0x004b3368, which copies System[1].icon into the sentinel System[0]. No render site reads it by constant displacement, so it has no confirmed drawing consumer -- but the cfg keyword is decisive about what it IS, and a `[reg+8]` read through a held &System[i] would be invisible to the scan that looked. NOT a home-planet index: the entry planet is planets[1]. Verified 2026-08-24 (finding 2026-08-24-0228-20, which suspected all three scalar names and was REFUTED -- see the plate at 0x00be1a30). |
| `+0x0c` | `planets` | `cfg_t_planet_index[32]` | cfg keyword `PLANET <k> <name>`, k = 0..31 -> cfg_GetPlanetIndex; slot k maps 1:1 to planets[k]. Indices into Planets[]; 0 terminates the list (Construct's tail scan at 0x0045eb34 walks k = 1..31 for the first zero). planets[1] is the system's ENTRY planet: llm_strat_session_state_reset copies it into G_PLANET_INDEX on session start and on system advance (0x00454009 / 0x00454136), llm_game_land_players_on_planet and llm_strat_input_update compare against it, and llm_strat_session_begin_multi forces System[0].planets[1] = 31 (0x004543cd). Construct back-fills Planets[planets[k]].system_index for every member. NOTE the parser does not bounds-check k: `PLANET 32` in a cfg would write past the 32-slot scratch template into the INVENTION scratch (SHL EAX,0x7 at 0x004afec0, no clamp); only the copy loop is bounded. |

#### `cfg_final_struct_Tree` (size 0x2c, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `define_index` | `cfg_t_define_index` |  |
| `+0x04` | `frame_index` | `cfg_t_frame_index` |  |
| `+0x08` | `area` | `byte[6][6]` |  |

#### `cfg_final_struct_Unit` (size 0x23f, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x02` | `invention` | `cfg_t_invention_index_s` |  |
| `+0x04` | `frame` | `cfg_t_frame_index` |  |
| `+0x08` | `frame_2` | `cfg_t_frame_index` |  |
| `+0x0c` | `energy_bar_frame_selected` | `cfg_t_frame_index` | RD-A readers readiness (2026-08-24). Healthbar-backdrop sprite/frame index for this unit's ENERGY bar (the HP-like stat, NOT the POWER resource) in the OWN/SELECTED visual state -- read at 0x0044f158 by llm_strat_render_selection_markers and passed as the frame argument to llm_gfx_draw_sprite_with_bar together with the red bar colour 0xf800. NOT per-unit-type cfg data: cfg_final_unit_Construct writes the literal 0x1c unconditionally for every unit type. 0x1c is exactly cfg_final_struct_Building.width_2's formula (footprint width + 0x1b) evaluated at width == 1 -- units are single-tile, so this is the width-1 slot of the same footprint-indexed healthbar-frame strip buildings index by their own width. |
| `+0x10` | `energy_bar_frame_click_target` | `cfg_t_frame_index` | RD-A readers readiness (2026-08-24). Sibling of energy_bar_frame_selected for the CLICK/HOVER-TARGET visual state -- read at 0x0044f9ed and paired with the green bar colour 0x7e0 in the same llm_gfx_draw_sprite_with_bar call shape. Also an unconditional literal (0x22) from cfg_final_unit_Construct, matching cfg_final_struct_Building.width_3 (footprint width + 0x21) at width == 1. |
| `+0x14` | `weapons` | `cfg_struct_weapon[4]` |  |
| `+0x1c` | `sight` | `byte` |  |
| `+0x1d` | `step_speed` | `double[9]` |  |
| `+0x65` | `turn_speed` | `double[9]` |  |
| `+0xad` | `-` | `double` |  |
| `+0xb5` | `armor_prob` | `int[9]` |  |
| `+0xd9` | `-` | `byte[5]` |  |
| `+0xde` | `type` | `cfg_enum_E_UNIT_TYPE` |  |
| `+0xe2` | `build_time` | `double` | Created by Rename Structure Field action |
| `+0xea` | `independent` | `undefined1` | Created by Rename Structure Field action |
| `+0xeb` | `move_op_code` | `byte` | The unit class's MOVE ORDER OPCODE -- llm_strat_order_dispatch's `op_code` (EBX) at every move/attack issuer: llm_strat_unit_order_move @0x0046a07b, _move_enqueue @0x0046a16a, _move_auto @0x0046a5f5, llm_strat_order_queue_dispatch @0x00466d99. Also compared directly as a CLASS TEST -- ==0xf (ground) by llm_unit_recruit @0x0046408e, llm_strat_order_issue_0xf_adjacent @0x0046a262 and llm_strat_bldg_completion_dispatch @0x0047a2ba; ==0x11 (heli) by llm_strat_storage_get_approach_tile @0x0048b3ee; ==0x12 (plane) by llm_strat_unit_fire_at_target @0x0048652d. Parser-derived from `type` by cfg_final_unit_Construct's class switch at 0x0045680c-0x00456890 (EN): type 1..0xa and 0xb..0xe -> {0xf, 0xa, 1}; type 0x11..0x12 (heli) -> {0x11, 0xb, 0x13}; type 0xf..0x10 and 0x13..0x18 (plane) -> {0x12, 0xb, 0x13}; any other type leaves all three at their zero-init value. Named 2026-08-05 (RI-AI AI1C layer 2, llm_strat_ai_unit_order_move_with_bump); previously three undefined bytes. |
| `+0xec` | `move_op_arg` | `byte` | The `arg` (ECX) paired with move_op_code in the same llm_strat_order_dispatch calls, and the same value llm_strat_ai_unit_order_move_with_bump @0x0046ae67 passes to llm_strat_order_enqueue alongside a LITERAL 0x18 opcode. 0xa = ground mover, 0xb = air; the storage/attack issuers test it as `== 0xa` to mean ground (llm_strat_unit_order_exit_storage @0x0046a7cf, llm_strat_unit_order_attack_target @0x0046b0ac and eight siblings). Parser-derived from `type` by cfg_final_unit_Construct's class switch at 0x0045680c-0x00456890 (EN): type 1..0xa and 0xb..0xe -> {0xf, 0xa, 1}; type 0x11..0x12 (heli) -> {0x11, 0xb, 0x13}; type 0xf..0x10 and 0x13..0x18 (plane) -> {0x12, 0xb, 0x13}; any other type leaves all three at their zero-init value. Named 2026-08-05 (RI-AI AI1C layer 2, llm_strat_ai_unit_order_move_with_bump); previously three undefined bytes. |
| `+0xed` | `default_op_code` | `byte` | The unit class's DEFAULT/IDLE order code, passed as BOTH `op_code` and `arg` to llm_strat_order_dispatch (llm_strat_unit_order_dispatch_default @0x00469e25/0x00469e4d loads it twice into EBX and ECX; same in llm_strat_unit_issue_default_order, _reset_order_and_target, _order_auto_launch_from_storage(_enqueue), llm_unit_force_disembark, llm_strat_unit_state_stop_to_default, _state_group_marshal, _group_step_ground). 1 = ground, 0x13 = air. Parser-derived from `type` by cfg_final_unit_Construct's class switch at 0x0045680c-0x00456890 (EN): type 1..0xa and 0xb..0xe -> {0xf, 0xa, 1}; type 0x11..0x12 (heli) -> {0x11, 0xb, 0x13}; type 0xf..0x10 and 0x13..0x18 (plane) -> {0x12, 0xb, 0x13}; any other type leaves all three at their zero-init value. Named 2026-08-05 (RI-AI AI1C layer 2, llm_strat_ai_unit_order_move_with_bump); previously three undefined bytes. |
| `+0xee` | `energy` | `double` | Unit type's MAX energy (HP cap). ENERGY = hit points, NOT the POWER resource (power_stats is the separate generated/consumed economy). |
| `+0xf6` | `sprite` | `cfg_t_frame_index` |  |
| `+0xfa` | `sprite_shadow` | `cfg_t_frame_index` |  |
| `+0xfe` | `anim_explo` | `cfg_t_frame_index` |  |
| `+0x102` | `debris_anim_row` | `int` | SIM1E (2026-08-14; llm_strat_unit_state_die_explode). Nonzero gates an optional debris-burst FX spawn on unit death; when nonzero, combined with a 0..3 random roll (field<<4 + roll*4) to index _G_LLM_STRAT_DEATH_ANIM_TABLE as [field*4+roll] -- the same row*4+roll scheme cfg_final_struct_Building's `trace` field uses (sim_bldg_state_destroyed.cpp). Was reserved_0x102[4] (undifferentiated padding immediately after anim_explo@0xfe); re-derived from the raw disassembly (0x004855b6/0x00485627: `*(int*)&Unit[proto].field_0x102`, then `<<4` combined with `llm_rand_below(4)*4` to index DEATH_ANIM_TABLE). |
| `+0x106` | `sprite_type` | `byte` |  |
| `+0x107` | `dmg_smoke_enabled` | `int` | gates llm_strat_unit_update_anim's damage-smoke animation; nonzero enables the chain |
| `+0x10b` | `kill_score` | `int` | Points added to the KILLER's runtime units[killer].experience (+0x28) when a unit of THIS TYPE is destroyed by a different player -- llm_strat_unit_kill_credit reads it @0x0044cc4a and adds it @0x0044cc50. Only on the just-died transition and only for a real cross-player attacker (killer_info&0x80 && killer_info&0xf != victim_player). Despite living on the per-type record it is currently HARDCODED to 1 for EVERY unit type by cfg_final_unit_Construct @0x00456551 (unconditional, no branch) -- the slot supports per-type variation, this binary does not use it. Named 2026-08-22 (RI-SIM SIM-READY prep); matches the term docs/structs.md already uses for the destination field ('Earned per kill (cfg kill_score)'). |
| `+0x10f` | `name` | `cfg_t_define_index` |  |
| `+0x113` | `info_txt` | `MH_STR_s` |  |
| `+0x153` | `info_flc` | `MH_STR_s` |  |
| `+0x193` | `sound_explo` | `cfg_t_define_index` |  |
| `+0x197` | `sound_move` | `cfg_t_define_index` |  |
| `+0x19b` | `elevation` | `int` |  |
| `+0x19f` | `elevation_2` | `int` |  |
| `+0x1a3` | `equivalent` | `cfg_t_building_index` |  |
| `+0x1a7` | `resource` | `cfg_struct_resource[7]` | Build cost, one (id, val) pair per entry, index = slot NOT resource id -- a walker reads .id to key the cost and stops at .id == 0. SEVEN entries, not four: every consumer in the binary bounds its walk at 7 (llm_strat_econ_track_unit_resource_spend CMP ECX,0x7 @0x004e21d9; llm_unit_can_afford_resources CMP EBX,0x7 @0x004e2407; llm_strat_unit_refund_build_cost_by_health @0x0048d78e; llm_strat_prod_try_start_unit @0x004929ef and @0x00492aed; llm_unit_apply_production_completion @0x00492c33; llm_strat_ai_queue_train_unit CMP EDX,0x7 @0x004e27fe), and 0x1a7 + 7*8 == 0x1df lands exactly on resource_2. The cfg parser (Construct) only ever fills FOUR (ids @0x1a7/0x1af/0x1b7/0x1bf) because the Unit section's grammar exposes four resource keywords -- the array is still 7, and entries 4..6 read back zero, which is what stops every walk early. Same shape and same size as cfg::final::struct::Building.resource. |
| `+0x1df` | `resource_2` | `cfg_struct_resource[7]` | Second per-unit resource list (the sibling of Building.resource_2), same (id, val) shape and the same SEVEN entries -- llm_strat_unit_try_pay_action_cost walks it with CMP ..,0x7 at 0x004932ba and 0x00493343. 0x1df + 7*8 == 0x217 lands exactly on energy_2, so the 24 bytes Ghidra previously left undefined after a [4] were this array's tail, not padding. |
| `+0x217` | `energy_2` | `double` |  |
| `+0x21f` | `icon` | `cfg_t_icon_index` |  |
| `+0x223` | `human` | `int` |  |
| `+0x227` | `soldier_count` | `int` | Created by Rename Structure Field action |
| `+0x22b` | `soldier_type` | `cfg_enum_E_SOLDIER_TYPE` |  |
| `+0x22f` | `weapon_facing_tolerance` | `uint` | Per-unit-class weapon firing-facing tolerance, in heading units (a circle is 0x18=24 headings, 15 deg each). A shot fires when the bearing to the target lies within +-this of facing_current, tested as a CIRCULAR window: (target-cur)<=tol \|\| (0x18-tol)<=(target-cur). Readers: llm_strat_unit_fire_at_target @0x004865a7, _if_aimed @0x00486336, 2_if_aimed @0x00486456, and llm_strat_unit_state_attack_unit @0x004852ea, which HALVES it first (SAR/SUB/SAR @0x004852f6, i.e. C /2). HARDCODED by cfg_final_unit_Construct's movement-class switch @0x004568db, not cfg-authored: 4 (loose) for ground vehicles and helis, 1 (tight) for foot/walker units and planes. Named 2026-08-22 (RI-SIM SIM-READY prep). |
| `+0x233` | `-` | `uint` |  |
| `+0x237` | `ai_unit` | `cfg_enum_ai_E_UNIT` |  |
| `+0x23b` | `ai_level` | `int` |  |

#### `cfg_final_struct_Upgrade` (size 0x68, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `invention` | `cfg_t_invention_index` |  |
| `+0x04` | `type` | `cfg_enum_E_UPGRADE_TYPE` |  |
| `+0x08` | `objects` | `cfg_t_upgrade_object_index[16]` |  |
| `+0x48` | `energy` | `int` |  |
| `+0x4c` | `step_speed` | `double` |  |
| `+0x54` | `range_min` | `int` | Created by Rename Structure Field action |
| `+0x58` | `range_max` | `int` |  |
| `+0x5c` | `missing` | `int` |  |
| `+0x60` | `fire_range` | `int` |  |
| `+0x64` | `power` | `int` |  |

#### `cfg_final_struct_Weapon` (size 0x16c, category `/Manual/cfg/final/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `type` | `cfg_t_weapon_type` |  |
| `+0x01` | `target` | `cfg_t_target_type` | Created by Rename Structure Field action |
| `+0x02` | `short_time` | `double` |  |
| `+0x0a` | `long_time` | `double` |  |
| `+0x12` | `range_min` | `int[9]` |  |
| `+0x36` | `range_max` | `int[9]` |  |
| `+0x5a` | `missing` | `int[9]` |  |
| `+0x7e` | `name` | `cfg_t_define_index` |  |
| `+0x82` | `ammo` | `int` |  |
| `+0x86` | `pocket` | `int` | Created by Rename Structure Field action |
| `+0x8a` | `fite_explo` | `cfg_t_frame_index` |  |
| `+0x8e` | `target_explo` | `cfg_t_frame_index` |  |
| `+0x92` | `power` | `double[9]` |  |
| `+0xda` | `fire_range` | `double[9]` |  |
| `+0x122` | `speed` | `double` |  |
| `+0x12a` | `smoke_time` | `double` | Created by Rename Structure Field action |
| `+0x132` | `explo_time` | `double` |  |
| `+0x13a` | `explo_pulse_initial_delay` | `double` | Delay added to _G_LLM_STRAT_GAME_CLOCK to seed a freshly-spawned projectile's explo_pulse_clock. Read at llm_strat_projectile_spawn 0x00464e6b as `FADD double ptr [EAX + 0xc3a65a]` with EAX = weapon_id * 0x16c, i.e. Weapon base 0xc3a520 + 0x13a. Was an 8-byte HOLE between explo_time (+0x132) and smoke_sprite (+0x142). NOTE the projectile record's own explo_pulse_clock field comment credits `Weapon.explo_time` for this value -- that comment names the WRONG field: the seed comes from +0x13a, not +0x132. |
| `+0x142` | `smoke_sprite` | `cfg_t_frame_index` | Created by Rename Structure Field action |
| `+0x146` | `length` | `int` |  |
| `+0x14a` | `explo_time_` | `int` |  |
| `+0x14e` | `power_` | `int` |  |
| `+0x152` | `homing` | `cfg_t_homing_type` | Created by Rename Structure Field action |
| `+0x153` | `area_damage_owner_filter` | `byte` | SIM1D fourth slice (2026-08-14; llm_strat_projectile_tick). Per-weapon-TYPE byte passed as llm_strat_apply_area_damage's owner-filter argument at BOTH of projectile_tick's call sites (impact and periodic explo-pulse damage) -- NOT always zero, unlike the one existing caller (sim_bldg_state_destroyed.cpp) whose own local parameter name `owner_filter_zeroed` reflects only that caller's always-0 usage, not this field's general meaning. Was `reserved_0x153[1]`, immediately after `homing`; split out because projectile_tick's disassembly resolves it as a distinct byte read. |
| `+0x154` | `bullet_anim` | `cfg_t_frame_index` |  |
| `+0x164` | `sound_fire` | `cfg_t_define_index` |  |
| `+0x168` | `sound_target` | `cfg_t_define_index` |  |

#### `cfg_pre_struct_Anim` (size 0x14, category `/Manual/cfg/pre/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `time` | `double` |  |
| `+0x08` | `repeat` | `int` |  |
| `+0x0c` | `step` | `int` |  |
| `+0x10` | `cyclic` | `int` |  |

#### `cfg_pre_struct_Building` (size 0x2cc, category `/Manual/cfg/pre/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `anim` | `cfg_t_frame_index[12]` |  |
| `+0x30` | `info_txt` | `LPSTR` |  |
| `+0x34` | `info_flc` | `cfg_t_define_index` |  |
| `+0x38` | `component_quant` | `int` |  |
| `+0x3c` | `component` | `cfg_t_frame_index[7]` |  |
| `+0x58` | `sprite_quantity` | `int` |  |
| `+0x5c` | `area` | `int[10] *` |  |
| `+0x60` | `weapon` | `cfg_t_weapon_index` |  |
| `+0x64` | `frame` | `cfg_t_frame_index` |  |
| `+0x68` | `frame_2` | `cfg_t_frame_index` |  |
| `+0x6c` | `type` | `cfg_t_define_index` |  |
| `+0x70` | `ai_build` | `int` |  |
| `+0x74` | `parametr` | `int` |  |
| `+0x78` | `energy` | `int` |  |
| `+0x7c` | `electric_power` | `int` |  |
| `+0x80` | `worker_count` | `int` |  |
| `+0x84` | `builder_count` | `int` |  |
| `+0x88` | `resource_id` | `cfg_t_define_index[7]` |  |
| `+0xa4` | `resource_val` | `int[7]` |  |
| `+0xc0` | `build_time` | `double` |  |
| `+0xc8` | `unit_id` | `cfg_t_unit_index[20]` |  |
| `+0x118` | `unit_quant` | `double[20]` |  |
| `+0x1b8` | `capacity_index` | `cfg_t_define_index[10]` |  |
| `+0x1e0` | `extract_fuel_index` | `cfg_t_define_index[10]` |  |
| `+0x208` | `capacity_val` | `double[10]` |  |
| `+0x258` | `extract_fuel_val` | `double[10]` |  |
| `+0x2a8` | `sight` | `int` |  |
| `+0x2ac` | `icon` | `cfg_t_define_index` |  |
| `+0x2b0` | `sound_explo` | `cfg_t_define_index` |  |
| `+0x2b4` | `sound_click` | `cfg_t_define_index` |  |
| `+0x2b8` | `trace` | `int` |  |
| `+0x2bc` | `velocity` | `double` |  |
| `+0x2c4` | `ability` | `int` |  |
| `+0x2c8` | `equivalent` | `cfg_t_define_index` |  |

#### `cfg_pre_struct_Planet` (size 0x3c, category `/Manual/cfg/pre/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `icon_index` | `cfg_t_icon_index` |  |
| `+0x04` | `x1` | `uint` |  |
| `+0x08` | `y1` | `uint` |  |
| `+0x0c` | `x2` | `uint` |  |
| `+0x10` | `y2` | `uint` |  |
| `+0x14` | `info_txt` | `LPSTR` |  |
| `+0x18` | `info_flc` | `LPSTR` |  |
| `+0x1c` | `source_mul` | `int *` |  |
| `+0x20` | `source_add` | `int *` |  |
| `+0x24` | `coordinate_x` | `uint` |  |
| `+0x28` | `coordinate_y` | `uint` |  |
| `+0x2c` | `asteroids` | `uint` |  |
| `+0x30` | `turn_speed` | `uint` |  |
| `+0x34` | `enemy` | `uint` |  |
| `+0x38` | `index` | `cfg_t_planet_index` |  |

#### `cfg_pre_struct_Project` (size 0x34, category `/Manual/cfg/pre/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `build_time` | `double` |  |
| `+0x08` | `info_txt` | `LPSTR` |  |
| `+0x0c` | `info_flc` | `LPSTR` |  |
| `+0x10` | `resource_name` | `cfg_enum_E_RESOURCE[4]` |  |
| `+0x20` | `resource_val` | `int[4]` |  |
| `+0x30` | `index` | `cfg_t_project_id` |  |

#### `cfg_pre_struct_Unit` (size 0xd4, category `/Manual/cfg/pre/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `sprite_shadow` | `cfg_t_frame_index` |  |
| `+0x04` | `sprite_type` | `cfg_t_define_index` |  |
| `+0x08` | `frame` | `cfg_t_frame_index` |  |
| `+0x0c` | `frame_2` | `cfg_t_frame_index` |  |
| `+0x10` | `anim_explo` | `cfg_t_frame_index` |  |
| `+0x14` | `trace` | `int` |  |
| `+0x18` | `info_txt` | `LPSTR` |  |
| `+0x1c` | `info_flc` | `LPSTR` |  |
| `+0x20` | `step_speed` | `double` |  |
| `+0x28` | `turn_speed` | `double` |  |
| `+0x30` | `energy` | `int` |  |
| `+0x34` | `p` | `int[5]` |  |
| `+0x48` | `build_time` | `double` |  |
| `+0x50` | `resource_index` | `cfg_enum_E_RESOURCE[7]` |  |
| `+0x6c` | `resource_val` | `int[7]` |  |
| `+0x88` | `weapon_index` | `cfg_t_weapon_index[4]` |  |
| `+0x98` | `weapon_enabled` | `int[4]` |  |
| `+0xa8` | `independent` | `cfg_t_define_index` |  |
| `+0xac` | `type` | `cfg_enum_E_UNIT_TYPE` |  |
| `+0xb0` | `sight` | `int` |  |
| `+0xb4` | `icon` | `cfg_t_icon_index` |  |
| `+0xb8` | `human` | `int` |  |
| `+0xbc` | `soldier_type` | `cfg_enum_E_SOLDIER_TYPE` |  |
| `+0xc0` | `sound_explo` | `cfg_t_define_index` |  |
| `+0xc4` | `sound_move` | `cfg_t_define_index` |  |
| `+0xc8` | `ai_unit` | `cfg_t_define_index` |  |
| `+0xcc` | `ai_level` | `int` |  |
| `+0xd0` | `equivalent` | `cfg_t_building_index` |  |

#### `cfg_pre_struct_Upgrade` (size 0x5c, category `/Manual/cfg/pre/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `objects` | `cfg_t_upgrade_object_index[14]` |  |
| `+0x38` | `range_min` | `int` |  |
| `+0x3c` | `range_max` | `int` |  |
| `+0x40` | `power` | `int` |  |
| `+0x44` | `missing` | `int` |  |
| `+0x48` | `step_speed` | `int` |  |
| `+0x4c` | `turn_speed` | `int` |  |
| `+0x50` | `energy` | `int` |  |
| `+0x54` | `sight` | `int` |  |
| `+0x58` | `index` | `cfg_t_unit_index` |  |

#### `cfg_static_struct_Anim` (size 0x124, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `length` | `int` |  |
| `+0x104` | `anim_length_total` | `int` |  |
| `+0x108` | `time` | `double` | Created by Rename Structure Field action |
| `+0x110` | `step` | `int` | Created by Rename Structure Field action |
| `+0x114` | `repeat` | `int` | Created by Rename Structure Field action |
| `+0x118` | `counter` | `int` |  |
| `+0x11c` | `total` | `dword` |  |
| `+0x120` | `cyclic` | `int` |  |

#### `cfg_static_struct_Bank` (size 0x8c, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `index` | `int` | Created by Rename Structure Field action |
| `+0x84` | `counter` | `int` |  |
| `+0x88` | `total` | `dword` |  |

#### `cfg_static_struct_Building` (size 0x2c54, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `-` | `MH_STR` |  |
| `+0x100` | `invention` | `MH_STR` |  |
| `+0x180` | `anim` | `MH_STR[12]` |  |
| `+0x780` | `component` | `MH_STR[8]` |  |
| `+0xb80` | `component_quant` | `int` |  |
| `+0xb84` | `anim_p` | `int[8]` |  |
| `+0xba4` | `info_txt` | `MH_STR` |  |
| `+0xc24` | `info_flc` | `MH_STR` |  |
| `+0xca4` | `sprite_quantity` | `int` |  |
| `+0xca8` | `area` | `int[10][10]` |  |
| `+0xe38` | `height` | `int` |  |
| `+0xe3c` | `weapon` | `MH_STR` |  |
| `+0xebc` | `frame` | `MH_STR` |  |
| `+0xf3c` | `frame_2` | `MH_STR` |  |
| `+0xfbc` | `equivalent` | `MH_STR` |  |
| `+0x103c` | `type` | `MH_STR` |  |
| `+0x10bc` | `ai_build` | `MH_STR` |  |
| `+0x113c` | `parametr` | `int` |  |
| `+0x1140` | `energy` | `int` |  |
| `+0x1144` | `electric_power` | `int` |  |
| `+0x1148` | `human` | `int` |  |
| `+0x114c` | `builder` | `int` |  |
| `+0x1150` | `resource_name` | `MH_STR[7]` |  |
| `+0x14d0` | `resource_val` | `int[7]` |  |
| `+0x14ec` | `resource_count` | `int` |  |
| `+0x14f0` | `upgrade` | `MH_STR` |  |
| `+0x1570` | `build_time` | `double` |  |
| `+0x1578` | `unit_name` | `MH_STR[20]` |  |
| `+0x1f78` | `unit_quant` | `double[20]` |  |
| `+0x2018` | `capacity_name` | `MH_STR[10]` |  |
| `+0x2518` | `extract_fuel_name` | `MH_STR[10]` |  |
| `+0x2a18` | `capacity_val` | `double[10]` |  |
| `+0x2a68` | `extract_fuel_val` | `double[10]` |  |
| `+0x2ab8` | `sight` | `int` | Created by Rename Structure Field action |
| `+0x2abc` | `velocity` | `double` |  |
| `+0x2ac4` | `ability` | `int` |  |
| `+0x2ac8` | `icon` | `MH_STR` |  |
| `+0x2b48` | `sound_explo` | `MH_STR` |  |
| `+0x2bc8` | `sound_click` | `MH_STR` |  |
| `+0x2c48` | `trace` | `int` |  |
| `+0x2c4c` | `counter` | `dword` |  |
| `+0x2c50` | `total` | `uint` | Number of BUILDING types the loaded cfg defines (label G_BUILDING_COUNT_TOTAL @0xe5f634). Compared UNSIGNED and used as an INCLUSIVE upper bound (JBE). Its one AI referrer is llm_strat_ai_plan_unit_training @0x004e6d6c, which uses it to bound the clear loop over player_data::ai_train_source_state -- an array indexed by UNIT type, not building type. That mismatch is the original's and is documented on the field it clears; do not read this field's name as evidence about what that loop walks. Was typed `dword`; retyped to uint 2026-08-02 so the generated DLL view reads it as a scalar rather than a uint8_t[4] byte array -- the identical fix made to the sibling cfg::static::struct::Unit.total on 2026-08-01, and for the identical reason. |

#### `cfg_static_struct_Define` (size 0x8c, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `index` | `int` |  |
| `+0x84` | `counter` | `int` |  |
| `+0x88` | `total` | `int` |  |

#### `cfg_static_struct_Icon` (size 0x8c, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `index` | `int` |  |
| `+0x84` | `counter` | `dword` |  |
| `+0x88` | `total` | `dword` |  |

#### `cfg_static_struct_Planet` (size 0x3b0, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `map` | `MH_STR` |  |
| `+0x100` | `bank` | `bool[100]` |  |
| `+0x164` | `invention` | `MH_STR` |  |
| `+0x1e4` | `icon` | `MH_STR` |  |
| `+0x264` | `x1` | `int` |  |
| `+0x268` | `y1` | `int` |  |
| `+0x26c` | `x2` | `int` |  |
| `+0x270` | `y2` | `int` |  |
| `+0x274` | `info_txt` | `MH_STR` |  |
| `+0x2f4` | `info_flc` | `MH_STR` |  |
| `+0x374` | `source_mul` | `int[4]` |  |
| `+0x384` | `source_add` | `int[4]` |  |
| `+0x394` | `coordinate_x` | `int` |  |
| `+0x398` | `coordinate_y` | `int` |  |
| `+0x39c` | `asteroids` | `int` |  |
| `+0x3a0` | `turn_speed` | `int` |  |
| `+0x3a4` | `enemy` | `int` |  |
| `+0x3a8` | `counter` | `int` |  |
| `+0x3ac` | `total` | `int` |  |

#### `cfg_static_struct_Progress` (size 0x90c, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `depend` | `MH_STR[16]` |  |
| `+0x880` | `depend_count` | `int` |  |
| `+0x884` | `type` | `MH_STR` |  |
| `+0x904` | `counter` | `int` |  |
| `+0x908` | `total` | `int` |  |

#### `cfg_static_struct_Project` (size 0x524, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `invention` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `icon` | `MH_STR` | Created by Rename Structure Field action |
| `+0x180` | `type` | `MH_STR` | Created by Rename Structure Field action |
| `+0x200` | `build_time` | `double` | Created by Rename Structure Field action |
| `+0x208` | `info_txt` | `MH_STR` | Created by Rename Structure Field action |
| `+0x288` | `info_flc` | `MH_STR` | Created by Rename Structure Field action |
| `+0x308` | `resource_name` | `MH_STR[4]` |  |
| `+0x508` | `resource_val` | `int[4]` |  |
| `+0x518` | `resource_count` | `int` |  |
| `+0x51c` | `counter` | `int` |  |
| `+0x520` | `total` | `int` |  |

#### `cfg_static_struct_Sprite` (size 0x90, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `index` | `cfg_t_define_index` |  |
| `+0x84` | `frame_index` | `dword` |  |
| `+0x88` | `counter` | `int` |  |
| `+0x8c` | `total` | `int` |  |

#### `cfg_static_struct_Stone` (size 0x188, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x180` | `counter` | `int` |  |
| `+0x184` | `total` | `int` |  |

#### `cfg_static_struct_System` (size 0x1188, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `planets` | `MH_STR[32]` |  |
| `+0x1080` | `invention` | `MH_STR` |  |
| `+0x1100` | `icon` | `MH_STR` |  |
| `+0x1180` | `counter` | `int` |  |
| `+0x1184` | `total` | `int` |  |

#### `cfg_static_struct_Text` (size 0x178, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `text` | `WCHAR[120]` |  |
| `+0x170` | `counter` | `int` |  |
| `+0x174` | `total` | `dword` |  |

#### `cfg_static_struct_Tree` (size 0x1f0, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `hz` | `MH_STR` |  |
| `+0x180` | `area` | `int[5][5]` |  |
| `+0x1e4` | `-` | `int` |  |
| `+0x1e8` | `counter` | `int` |  |
| `+0x1ec` | `total` | `int` |  |

#### `cfg_static_struct_Unit` (size 0xeec, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `type` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `invention` | `MH_STR` | Created by Rename Structure Field action |
| `+0x180` | `sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x200` | `sprite_shadow` | `MH_STR` |  |
| `+0x280` | `sprite_type` | `MH_STR` |  |
| `+0x300` | `frame` | `MH_STR` |  |
| `+0x380` | `frame_2` | `MH_STR` |  |
| `+0x400` | `equivalent` | `MH_STR` |  |
| `+0x480` | `anim_explo` | `MH_STR` |  |
| `+0x500` | `info_txt` | `MH_STR` |  |
| `+0x580` | `info_flc` | `MH_STR` |  |
| `+0x600` | `step_speed` | `double` | Created by Rename Structure Field action |
| `+0x608` | `turn_speed` | `double` |  |
| `+0x610` | `armor` | `int` | Created by Rename Structure Field action |
| `+0x614` | `energy` | `int` | Created by Rename Structure Field action |
| `+0x618` | `p_1` | `int` | Created by Rename Structure Field action |
| `+0x61c` | `p_2` | `int` | Created by Rename Structure Field action |
| `+0x620` | `p_3` | `int` | Created by Rename Structure Field action |
| `+0x624` | `p_4` | `int` | Created by Rename Structure Field action |
| `+0x628` | `p_5` | `int` | Created by Rename Structure Field action |
| `+0x62c` | `build_time` | `double` | Created by Rename Structure Field action |
| `+0x634` | `resource_count` | `int` |  |
| `+0x638` | `resource_name` | `MH_STR[7]` | Created by Rename Structure Field action |
| `+0x9b8` | `resource_val` | `int[7]` |  |
| `+0x9d4` | `weapon_name` | `MH_STR[4]` |  |
| `+0xbd4` | `independent` | `MH_STR` | Created by Rename Structure Field action |
| `+0xc54` | `sight` | `int` | Created by Rename Structure Field action |
| `+0xc58` | `icon` | `MH_STR` | Created by Rename Structure Field action |
| `+0xcd8` | `sound_explo` | `MH_STR` | Created by Rename Structure Field action |
| `+0xd58` | `sound_move` | `MH_STR` |  |
| `+0xdd8` | `human` | `int` | Created by Rename Structure Field action |
| `+0xddc` | `soldier_type` | `MH_STR` | Created by Rename Structure Field action |
| `+0xe5c` | `trace` | `int` | Created by Rename Structure Field action |
| `+0xe60` | `counter` | `uint` |  |
| `+0xe64` | `total` | `uint` | Number of unit types the loaded cfg defines (label G_UNIT_COUNT_TOTAL @0xe6049c). Compared UNSIGNED and used as an INCLUSIVE upper bound: llm_strat_ai_is_worker_priority_candidate scans production queue slots 1..total. Was typed `dword`; retyped to uint 2026-08-01 so the generated DLL view reads it as a scalar rather than a byte array. |
| `+0xe68` | `ai_unit` | `MH_STR` |  |
| `+0xee8` | `ai_level` | `int` | Created by Rename Structure Field action |

#### `cfg_static_struct_Upgrade` (size 0x9b0, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `invention` | `MH_STR` |  |
| `+0x100` | `type` | `MH_STR` |  |
| `+0x180` | `object` | `MH_STR[16]` |  |
| `+0x980` | `range_min` | `int` |  |
| `+0x984` | `range_max` | `int` |  |
| `+0x988` | `power` | `int` |  |
| `+0x98c` | `missing` | `int` |  |
| `+0x990` | `step_speed` | `double` |  |
| `+0x998` | `turn_speed` | `double` |  |
| `+0x9a0` | `energy` | `int` |  |
| `+0x9a4` | `sight` | `int` | Created by Rename Structure Field action |
| `+0x9a8` | `counter` | `dword` |  |
| `+0x9ac` | `total` | `dword` |  |

#### `cfg_static_struct_Weapon` (size 0x6e0, category `/Manual/cfg/static`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `MH_STR` |  |
| `+0x80` | `type` | `MH_STR` | Created by Rename Structure Field action |
| `+0x100` | `ammo` | `int` |  |
| `+0x104` | `pocket` | `int` | Created by Rename Structure Field action |
| `+0x108` | `short_time` | `double` |  |
| `+0x110` | `long_time` | `double` | Created by Rename Structure Field action |
| `+0x118` | `range_min` | `int` | Created by Rename Structure Field action |
| `+0x11c` | `range_max` | `int` | Created by Rename Structure Field action |
| `+0x120` | `fire_explo` | `MH_STR` | Created by Rename Structure Field action |
| `+0x1a0` | `target_explo` | `MH_STR` | Created by Rename Structure Field action |
| `+0x220` | `power` | `int` | Created by Rename Structure Field action |
| `+0x224` | `speed` | `double` | Created by Rename Structure Field action |
| `+0x22c` | `length` | `int` | Created by Rename Structure Field action |
| `+0x230` | `missing` | `int` | Created by Rename Structure Field action |
| `+0x234` | `bullet_anim` | `MH_STR[4]` | Created by Rename Structure Field action |
| `+0x434` | `probability` | `int[4]` |  |
| `+0x444` | `fire_range` | `int` | Created by Rename Structure Field action |
| `+0x448` | `explo_time` | `double` | Created by Rename Structure Field action |
| `+0x450` | `smoke_time` | `double` |  |
| `+0x458` | `smoke_sprite` | `MH_STR` | Created by Rename Structure Field action |
| `+0x4d8` | `homing` | `MH_STR` | Created by Rename Structure Field action |
| `+0x558` | `target` | `MH_STR` | Created by Rename Structure Field action |
| `+0x5d8` | `sound_fire` | `MH_STR` | Created by Rename Structure Field action |
| `+0x658` | `sound_target` | `MH_STR` | Created by Rename Structure Field action |
| `+0x6d8` | `counter` | `dword` |  |
| `+0x6dc` | `total` | `dword` |  |

#### `cfg_struct_map_2` (size 0x5c, category `/Manual/cfg/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `width` | `uint` |  |
| `+0x04` | `height` | `uint` |  |
| `+0x08` | `tlo_name` | `char[12]` |  |
| `+0x30` | `f1` | `undefined4` |  |
| `+0x34` | `f2` | `undefined4` |  |
| `+0x3c` | `f3` | `int` |  |

#### `cfg_struct_map_header` (size 0x17c, category `/Manual/cfg/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `width` | `uint` | map width in tiles |
| `+0x04` | `height` | `uint` | map height in tiles |
| `+0x08` | `player_count` | `int` | map's DESIGNED player/side count; bounds build_slot_widgets + build_players_from_slots. MP overrides it to the live N (launch.cpp MD_PCOUNT). |
| `+0x0c` | `file_size` | `uint` | map file size in bytes (.MP) |
| `+0x10` | `checksum` | `uint` | map file checksum (dword from the .MP header); on MP map transfer the client verifies it vs its local map, and on mismatch renames the local file away for refetch (was field4_0x10) |
| `+0x14` | `seed` | `byte` | map RNG seed byte (read from the .MP header on load; feeds deterministic map/mission setup) |
| `+0x15` | `reserved_0x15` | `undefined1[3]` |  |
| `+0x18` | `path_unc` | `char[128]` | map file path (ANSI, e.g. Maps\<name>.mpm) |
| `+0xfc` | `map_name` | `char[32]` | map display name |
| `+0x11c` | `tlo_name` | `char[12]` | tileset / tlo name |
| `+0x12c` | `footer` | `byte[80]` | Created by retype action |

#### `cfg_struct_resource` (size 0x8, category `/Manual/cfg/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `id` | `cfg_enum_E_RESOURCE` |  |
| `+0x04` | `val` | `int` |  |

#### `cfg_struct_weapon` (size 0x2, category `/Manual/cfg/struct`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `id` | `cfg_t_weapon_index_s` |  |
| `+0x01` | `enabled` | `bool` |  |

#### `game_player_data` (size 0x288fc, category `/Manual/game`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `ai_housing_candidate_heli` | `int` | AI housing-building type id for HELI units (ai_build category 0x22). ALIASED FIELD: llm_strat_ai_init_build_candidate_priorities writes it, and llm_strat_ai_maintain_unit_housing reads it, as player_data[p+1].+0x00 -- i.e. player p's value physically lives in player p+1's record. Verified 2026-08-01: writer and all readers use the same base+p*0x288fc+0xe967bc form, so the aliasing is self-consistent; player_data[0]'s copy is never written or read, and player 7's write lands at 0xfb2640, past the 8-element array, in .bss that nothing else references (scan_raw_pointers: TOTAL-ORPHAN). |
| `+0x04` | `ai_housing_candidate_plane` | `int` | AI housing-building type id for PLANE units (ai_build category 0x23). Same player+1 aliasing as ai_housing_candidate_heli. |
| `+0x08` | `ai_housing_candidate_vehicle` | `int` | AI housing-building type id for VEHICLE units (ai_build category 0x21). Same player+1 aliasing as ai_housing_candidate_heli. |
| `+0x0c` | `ai_housing_candidate_soldier` | `int` | AI housing-building type id for SOLDIER units (ai_build category 0x20). Same player+1 aliasing as ai_housing_candidate_heli. |
| `+0x10` | `ai_established` | `int` | one-shot latch: set once this AI player's starting unit is spawned; gates all further per-tick AI phases in llm_strat_ai_player_tick |
| `+0x14` | `ai_phase_flags` | `byte` | AI phase-enable bitmask (llm_strat_ai_player_tick): 0x1=construction planner (+repair/upgrade scan), 0x2=llm_strat_ai_plan_unit_training, 0x4=unit-group task machine |
| `+0x18` | `ai_enabled` | `int` | master gate for this player's strategic AI tick pipeline: set 1 by llm_strat_spawn_ai_base/llm_strat_spawn_invasion_force, cleared 0 by llm_strat_init_human_player_data. Read by llm_strat_ai_players_tick (gates llm_strat_ai_player_tick + the FUN_004eadec/FUN_004ede0b-vs-FUN_004ee00e sibling phase pair) and by most AI notification hooks (llm_strat_ai_notify_bldg_constructed, llm_strat_ai_notify_object_removed, llm_ai_notify_unit_lifecycle, llm_strat_ai_group_member_count_adjust, llm_strat_ai_queue_release_order) to skip AI-only bookkeeping for non-AI player slots |
| `+0x1c` | `ai_invasion_force` | `int` | 0 = full AI base (runs the construction/economy planner + unit-group tasks in llm_strat_ai_player_tick; classifies units by combat type in llm_ai_notify_unit_lifecycle); 1 = invasion-force-only AI (llm_strat_ai_player_tick skips straight to FUN_004e8273/FUN_004e851f instead, and llm_ai_notify_unit_lifecycle skips unit-type classification). Set by llm_strat_spawn_ai_base (0) / llm_strat_spawn_invasion_force (1) |
| `+0x20` | `ai_invasion_points` | `int` | reinforcement-spawn budget for an invasion-force-only AI player (ai_invasion_force != 0): initialized from llm_strat_spawn_invasion_force's 5th parameter, decremented by 1 per unit spawned in llm_strat_ai_invasion_spawn_reinforcements until it reaches 0 (then llm_strat_ai_invasion_launch_attack_group takes over) |
| `+0x24` | `ai_home_tile_x` | `int` | this player's home/base tile X, set at base creation by llm_strat_spawn_ai_base / llm_strat_spawn_invasion_force. Origin of every AI toroidal distance-to-home computation (llm_strat_sort_sites_by_dist, the three site scanners, the group task handlers). |
| `+0x28` | `ai_home_tile_y` | `int` | this player's home/base tile Y, paired with ai_home_tile_x. |
| `+0x2c` | `ai_attack_orders_issued` | `int` | Running count of attack orders this player has committed. INC'd from exactly two sites -- llm_strat_ai_commit_attack_order (0x004d56fa, on the common tail after llm_strat_unit_order_attack_target_enqueue) and llm_strat_unit_order_attack_dispatch (0x004d5854) -- and zeroed by all three player-init paths (spawn_ai_base, spawn_invasion_force, init_human_player_data). NOTHING READS IT: a full instruction sweep for 0xe6deec over the whole image finds those five sites and no read. Pure statistic as far as the code goes. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x30` | `ai_reposition_cached_member_count` | `int` | Change-detector for llm_strat_ai_group_reposition_members, its only referrer besides the two spawn initialisers. Holds the ai_groups[g].member_count (a SHORT, zero-extended) that the last reposition was computed for: the function returns early when the current count still matches (CMP at 0x004d62f4), stores the fresh count on a completed pass (0x004d65e5), and writes -1 to invalidate (0x004d624a). Note it is keyed by PLAYER, not by group, so repositioning a second group of the same player overwrites the memo. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x34` | `ai_map_changed_pending` | `int` | one-shot 'the map layout changed near me' trigger, set 1 for every AI-enabled player by llm_strat_ai_notify_map_changed/_2 whenever a building is placed/queued/completed; consumed and cleared by llm_strat_ai_recompute_map_influence |
| `+0x38` | `ai_turret_rescan_pending` | `int` | one-shot 'rescan for enemy turret threats' trigger, set 1 by llm_strat_ai_notify_bldg_constructed/llm_strat_ai_notify_object_removed when the affected building is an enemy turret; consumed and cleared by llm_strat_ai_turret_threat_rescan (this is the 'turret-threat rescan trigger' already described in the AI notification hooks writeup) |
| `+0x3c` | `ai_tile_flags_grid` | `byte[65536]` | Per-tile AI flag/influence grid: 256x256 BYTES indexed (x << 8) \| y (x is the HIGH byte -- column-major), one byte of flag bits per map tile. Extent measured 2026-08-01 (AI1A layer 1) from llm_strat_ai_grid_clear_threat_bit's index math (BL = inner/height counter, BH = outer/width counter, address = base + ((BH<<8)\|BL)), not from the gap to the next field -- the 20 bytes at +0x1003c are NOT part of it and stay undefined. Bit 0x40 = 'an enemy turret can reach this tile': stamped in a radius by llm_strat_ai_grid_stamp_threat_ring (which wraps on the torus via a ((width-1)<<8)\|(height-1) mask) and cleared wholesale by llm_strat_ai_grid_clear_threat_bit; both are driven per player by llm_strat_ai_turret_threat_rescan. Other bits belong to other AI passes (see ai_map_changed_pending / llm_strat_ai_recompute_map_influence) and are not classified here. |
| `+0x1003c` | `ai_clock` | `float` | The AI's per-player accumulating clock (FLOAT), the time base every AI period/milestone is measured against. Seeded to 0.001f by llm_strat_spawn_ai_base / _spawn_invasion_force, advanced and compared by llm_strat_ai_players_tick against the float at +0x10040, divided into a rate by llm_strat_ai_plan_mine_construction, snapshotted into llm_strat_ai_unit_group::task_start_time (as a double) by llm_strat_ai_group_task_activate, and latched into ai_attack_milestone_clock by llm_strat_ai_army_milestone_advance_or_attack. NOT the game clock. |
| `+0x10040` | `ai_clock_s` | `float` | Second of the four AI timers the game's own debug line prints as `timemark play:%f s:%f t:%f m:%f` (llm_strat_ai_tick_all_groups @0x004eece4 pushes 0x1003c/40/44/48 in that order). `play` is ai_clock; what s/t/m stand for is not established -- the letters are the only evidence. |
| `+0x10044` | `ai_clock_t` | `float` | Third of the four AI timers in the `timemark play/s/t/m` debug line; see ai_clock_s. |
| `+0x10048` | `ai_clock_m` | `float` | Fourth of the four AI timers in the `timemark play/s/t/m` debug line; see ai_clock_s. |
| `+0x1004c` | `ai_expand_gate_value` | `int` | AI expansion / perimeter RADIUS IN TILES -- not a tick count, despite the _MIN_TICKS name of the constant it is compared against. PRODUCED once per player per AI tick by llm_strat_ai_player_tick: 0x004e8ccc stores trunc(sqrt(llm_strat_bldg_max_defense_radius_sq(player, ai_home_tile_x, ai_home_tile_y))) (FUN_004da9f0 is the domain-checked FSQRT wrapper -- FTST/SAHF then FSQRT at 0x004daa1f); 0x004e8cd4 stores the constant 5 on the other arm of the `CMP dword ptr [ECX + 0xe96568],0x0` at 0x004e8c93, i.e. when pd[player].ai_score_cat_1 is 0. CONSUMED at six sites in four functions: llm_strat_ai_scan_construction_sites (0x004e42e3) and llm_strat_ai_expand_adjacent_mine_relay (0x004e4af7) both bail when it is < _G_LLM_STRAT_AI_EXPAND_MIN_TICKS (unsigned); expand_adjacent_mine_relay again at 0x004e4d0c as radius+5 around ai_home_tile; llm_strat_ai_group_form_perimeter_patrol at 0x004e79f9 and 0x004e7a4f, added to DAT_00669340 and scaled by the quadrant unit vectors _G_LLM_STRAT_AI_QUADRANT_D*; and llm_strat_ai_bldg_register_visible_building at 0x004db3c2, SQUARED on the spot (IMUL EAX,EAX) and compared against llm_strat_toroidal_dist_sq to set ai_intel_flags bit 0x8. CORRECTED 2026-08-02 (RI-AI AI1B prep): this comment previously recorded TWO READS AND NO WRITER and reasoned that both expansion paths might therefore be dead on a fresh game. Both halves were wrong. An instruction-level sweep of all 252238 instructions for the little-endian dword 0x00e7df0c -- independent of Ghidra's reference manager -- returns 8 sites: 6 reads and 2 writes. A missing writer in a generated index is the classic false negative (docs/dead-ends.md). |
| `+0x10050` | `ai_patrol_quadrant_cursor` | `int` | Rotating 0..3 cursor selecting the map QUADRANT for the next perimeter-patrol waypoint pair. SOLE CONSUMER llm_strat_ai_group_form_perimeter_patrol: MOV EBX,[ESI+0xe7df10] / AND EBX,0x3 (quadrant of waypoint 1) / INC dword [ESI+0xe7df10] / MOV EAX,[ESI+0xe7df10] / AND EAX,0x3 (quadrant of waypoint 2) at 0x004e79d4-0x004e79e9; both masked values are then scaled by 4 and used to index _G_LLM_STRAT_AI_QUADRANT_DX1 (0x0066f418) / _G_LLM_STRAT_AI_QUADRANT_DY1 (0x0066f428) at 0x004e7a0b/0x004e7a21 (first waypoint) and 0x004e7a5e/0x004e7a75 (second), each scaled by ai_expand_gate_value + DAT_00669340 and added to ai_home_tile_x/y -- so successive patrol groups are dispatched to successive quadrants around the home tile. The name rests on that indexing and on nothing else; the counter is never bounded, only masked. SPLIT OUT OF resource_spent[0] on 2026-08-03 (finding 2026-08-02-1731-2, user-approved). IT WAS NEVER A RESOURCE TOTAL, and the ORIGINAL'S OWN INIT CODE SAYS SO: both spawn paths store this slot with a dedicated scalar MOV dword [EAX+0xe7df10],0 (0x004dcdf3, 0x004dd522) inside the same run of single-scalar stores as ai_group_count (0xe7e424) and ai_bldg_queue_count (0xe935ec), while the resource row is cleared by a SEPARATE four-iteration loop based at slot [1] -- MOV dword [EBX + EDX*0x4 + 0xe7df14],0 / INC EAX / CMP EAX,0x4 / JB (0x004dcf23-0x004dcf32, and 0x004dd646 in the sibling) -- which never touches +0x10050. Second, independent derivation: the generic ledger writer UpdateResourceStats @0x004dbeec refuses id 0 outright, CMP EDX,0x4 / JA and TEST EDX,EDX / JBE at 0x004dbef9-0x004dbf00, before ADD dword [EDX+EAX*0x4+0xe7df10],EBX @0x004dbf1e. |
| `+0x10054` | `resource_spent` | `int[4]` | Per-resource-id RESERVATION/EXPENDITURE ledger. RESHAPED 2026-08-03 (finding 2026-08-02-1731-2, user-approved): was int[5] at +0x10050, whose slot [0] was never a resource total; that slot is now the separate scalar ai_patrol_quadrant_cursor and this array is int[4] at +0x10054. INDEXING CHANGED WITH THE RESHAPE: element [i] is RESOURCE ID i+1, so ids 1..4 map to [0..3] and there is no id-0 element. The original's own code still addresses the row from the OLD [0] base in one place -- UpdateResourceStats @0x004dbeec does ADD dword [EDX+EAX*0x4+0xe7df10],EBX @0x004dbf1e with EAX = the resource id -- so a reimplementation of THAT function must write resource_spent[id-1]. Every other site already indexes from the [1] base and needs no adjustment: llm_strat_ai_bldg_queue compares CMP ESI,[EDX+EAX*0x4+0xe7df14] @0x004e83d1 and CMP EBX,[EAX+0xe7df14] @0x004e8433. The ledger is symmetric: llm_strat_ai_notify_bldg_constructed ADDs (0x004db1c5/d2/df/ec) and llm_strat_ai_queue_release_order (0x004dbfd2/e0/ee/fc), llm_strat_ai_bldg_queue_process_entry (0x004e7fb3), _handle_recruit_state (0x004e819f) and _handle_upgrade_or_cancel (0x004e8231/3e/4b/58) SUB it back; llm_strat_ai_bldg_queue_process only COMPARES it, treating 'the queue entry reserved more than this' as 'cannot proceed'. NOT AI-ONLY -- UpdateResourceStats (above) adds into it for any id 1..4 with a positive amount (TEST EBX,EBX / JLE @0x004dbf02). Distinct from the lifetime tally resource_spend_total (int[8] at +0x10084). CORRECTION TO THE PREVIOUS COMMENT, which said 'Both spawn functions zero the whole row (0x004dcf23, 0x004dd646)': those two sites are the four-iteration loop over ids 1..4 ONLY, based at 0xe7df14 with CMP EAX,0x4 -- they do not clear +0x10050, which is initialised separately at 0x004dcdf3 / 0x004dd522. Site list re-derived 2026-08-03 by an image-wide byte scan for the absolutes 0xe7df10..0xe7df20 over the executable sections of mh_en_clean.bak.exe (24 sites), independent of Ghidra's reference manager. |
| `+0x10064` | `ai_mine_yield_by_resource` | `int[8]` | Estimated mine yield per RESOURCE ID, filled by llm_strat_ai_mine_portfolio_rebalance, which takes it as an out-pointer (`llm_strat_ai_mine_portfolio_rebalance(player, &player_data[p].field_0x10064)`) and zeroes indices 0..4 on entry. THAT POINTER IS WHY AN INSTRUCTION SWEEP FINDS NO WRITER -- the three sites that name 0xe7df24 are all reads (llm_strat_ai_plan_mine_construction x2, llm_strat_ai_resource_site_meets_threshold), and concluding 'never written' from that would be the docs/dead-ends.md trap in its exact form. Sized int[8] by symmetry with the sibling total at +0x10084 and because it makes the 0x45c region between resource_spent and is_alien_race tile EXACTLY; only ids 0..4 are ever touched. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x10084` | `resource_spend_total` | `int[8]` | Lifetime resource expenditure per RESOURCE ID -- accumulated by llm_strat_econ_track_unit_resource_spend (0x004e21ba) and llm_strat_bldg_record_resource_expenditure_stats (0x004e68b4), each adding the cfg record's resource[].val for every resource slot of a produced unit/building. INITIALISED TO 1, not 0, by both spawn paths over indices 0..7 (0x004dcfd3, loop bound CMP EAX,0x8) -- i.e. it is divide-safe by construction, which is the tell that it is used as a denominator; llm_strat_ai_plan_mine_construction reads it alongside ai_mine_yield_by_resource. Ghidra renders every access to this array as `resource_spent[id + 0xd]`, an overrun of the int[5] at +0x10050 -- that is the undersized-array artifact, not a real aliasing. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x100a4` | `ai_spend_ring_cursor` | `int` | Write cursor, 0..31, for the two 32-slot spend rings below. llm_strat_ai_resource_spend_rate_update increments it and wraps at 32 at the END of each pass (0x004e7ccb / CMP 0x20 / reset); llm_strat_econ_track_unit_resource_spend and llm_strat_bldg_record_resource_expenditure_stats read it to pick the slot they accumulate into (`cursor * 0x10` -- 0x10 because a slot is 4 ints). Zeroed at spawn. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x100a8` | `ai_spend_rate_denom_ring` | `int[128]` | The DENOMINATOR ring of the AI spend-rate ratio: logically int[32][4] -- 32 slots indexed by ai_spend_ring_cursor, each holding 4 per-resource ints -- which is how both spawn paths initialise it (nested loop, outer 0..0x20, inner 0..4, address player + slot*0x10 + res*4 at 0x004dcf67). Declared flat as int[128] because the field grammar has no nested-array form; index it as [slot*4 + res]. llm_strat_ai_resource_spend_rate_update sums it weighted by _G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS into ai_spend_rate_denom. NO WRITER IS VISIBLE anywhere in the image beyond those two zeroing loops -- the only other site naming 0xe7df68 is the read in rate_update. Stated as an observation, NOT as 'always zero': an instruction sweep cannot see a store made through a pointer, a memcpy or the save loader, which is exactly how ai_mine_yield_by_resource above turned out to be written. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x102a8` | `ai_spend_rate_numer_ring` | `int[128]` | The NUMERATOR ring, same int[32][4] shape and same cursor as ai_spend_rate_denom_ring above (index [slot*4 + res]). This is the one that actually gets filled: llm_strat_econ_track_unit_resource_spend (0x004e21d1) and llm_strat_bldg_record_resource_expenditure_stats accumulate each produced unit's/building's resource[].val into `cursor*0x10 + id*4` off a displacement of 0xe7e164 -- which is +0x102a4, i.e. the array base biased by -4 because resource IDs are 1-based. llm_strat_ai_resource_spend_rate_update sums it weighted into ai_spend_rate_numer. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x104a8` | `ai_spend_rate_numer` | `int` | Weighted sum of ai_spend_rate_numer_ring over all 32 slots and 4 resources, using _G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS. Recomputed from scratch (zeroed first) on every llm_strat_ai_resource_spend_rate_update pass; not persisted between passes in any other sense. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x104ac` | `ai_spend_rate_denom` | `int` | Weighted sum of ai_spend_rate_denom_ring, computed in the same loop as ai_spend_rate_numer and with the same weights. When it comes out ZERO the ratio below is set to a sentinel instead of dividing -- and since no writer of the denominator ring has been found, that is the branch to expect. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x104b0` | `ai_spend_rate` | `double` | ai_spend_rate_numer / ai_spend_rate_denom as a double, recomputed per llm_strat_ai_resource_spend_rate_update pass. When the denominator is 0 the function stores the raw bit pattern 0x42a2309c_e5400000 instead of dividing -- that is exactly 1e13, i.e. an 'effectively infinite rate' sentinel rather than a NaN or an error code. Occupies +0x104b0..+0x104b8; the dword at +0x104b4 is this field's HIGH half, which is why the sentinel is written as two 32-bit stores. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x104b8` | `ai_labor_utilization` | `double` | Labour-utilisation ratio computed by llm_strat_ai_rebalance_building_workers (see that function's plate), clamped into 0..1: values below _DAT_00669378 are flushed to 0.0, and values above 1.0 are also reset. Occupies +0x104b8..+0x104c0; the dword at +0x104bc is this field's HIGH half -- which is why the clamp writes appear as paired 32-bit stores of 0x00000000 and 0x3ff00000 (the high word of 1.0), and why one site tests it with `TEST dword [..+0x104bc],0x7fffffff` (a sign-ignoring is-nonzero check on the exponent/mantissa half). Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x104c0` | `is_alien_race` | `int` | 0 = human race, non-0 = alien race. Written from llm_strat_spawn_ai_base's is_alien argument. Selects the race-specific building-type constant at ~50 sites (H_MINE/A_MINE, H_TURRET/A_TURRET, H_RELAY/A_RELAY, ... and every ai_build lookup code in llm_strat_ai_init_build_candidate_priorities). |
| `+0x104c4` | `ai_build_plan_cursor` | `int` | index of the next unconsumed entry in the precomputed build-order plan ai_build_plan[]; advanced by llm_strat_ai_plan_construction |
| `+0x104c8` | `ai_build_plan_len_and_flag` | `uint` | low 31 bits = ai_build_plan[] length; bit 0x80000000 (== the high bit of this dword's MSB byte, formerly documented as a separate field_0x104cb) = ring/spiral-scan-around-threat mode for llm_strat_ai_scan_construction_sites, preserved across plan-length updates in llm_strat_load_base_layout_dmp |
| `+0x104cc` | `ai_build_plan` | `int[32]` | Precomputed starting build-order plan (building-type ids), populated at base spawn (llm_strat_ai_build_plan_push @0x004dc781 appends at ai_build_plan_len_and_flag and increments it, with NO cap check; <=6 entries seen in practice) and consumed via ai_build_plan_cursor / ai_build_plan_len_and_flag. SHRUNK [37]->[35] on 2026-08-01 (AI1A layer 2, the two attack-milestone scalars) and [35]->[32] on 2026-08-02 (AI1B layer 2): slots 32/33/34 are three independent scalars, now named below. That was already suspected in this comment; it is settled by llm_strat_spawn_ai_base @0x004dcebd-0x004dcecf, which writes them ONE AT A TIME with different values (0xe7e40c and 0xe7e410 <- _G_LLM_STRAT_AI_CFG_START_UNITS, 0xe7e414 <- 0), and by their three disjoint readers. NOTHING in the image indexes this array with a runtime index >= 32. |
| `+0x1054c` | `ai_start_units_remaining` | `int` | Countdown of the AI's scripted STARTING UNITS still to be QUEUED for training. Initialised to _G_LLM_STRAT_AI_CFG_START_UNITS (@0x00669398, 12 in the shipped script) by llm_strat_spawn_ai_base @0x004dcec3 and llm_strat_spawn_invasion_force @0x004dd5ec. llm_strat_ai_plan_unit_training decrements it once per queued training and loops while it is non-zero (CMP @0x004e6efb, DEC @0x004e6f04, CMP @0x004e6f20), so one planning pass drains it completely. While it is non-zero llm_strat_ai_queue_train_unit SKIPS the per-unit-type queue cap check against _G_LLM_STRAT_AI_TRAIN_QUEUE_PER_UNIT_CAP entirely (CMP @0x004e26c8, JNZ past the scan). Those are its only three referrers besides the two spawn initialisers. Was ai_build_plan[32] until 2026-08-02 (AI1B layer 2). |
| `+0x10550` | `ai_start_units_pending_spawn` | `int` | Second countdown initialised from the SAME _G_LLM_STRAT_AI_CFG_START_UNITS value at base spawn (llm_strat_spawn_ai_base @0x004dcec9, llm_strat_spawn_invasion_force @0x004dd5f2). Its ONLY other referrer in the image is llm_ai_notify_unit_lifecycle, which decrements it when non-zero at 0x004dbbd7-0x004dbbe0, in the branch that also resets the unit's AI-tracking words at 0xdd8d1c/0xdd8d1e/0xdd8d20. So it counts scripted starting units still to ARRIVE, as against ai_start_units_remaining's still to be QUEUED. WHICH lifecycle mode(s) reach that DEC was not traced -- llm_ai_notify_unit_lifecycle is mode-dispatched and only mode 1 has been read. Was ai_build_plan[33] until 2026-08-02 (AI1B layer 2). |
| `+0x10554` | `ai_promo_credit` | `int` | AI FREE-UNIT SUBSIDY METER. Named for the two AI.SCR keys that move it (nPromoAdd / nPromoSub) rather than for its sole consumer; renamed from ai_recruit_credit 2026-08-03 (ghidra_findings 2026-08-02-1922-12). Signed; initialised to 0 at base spawn (@0x004dcecf / @0x004dd5f8). Its only other referrer is llm_strat_ai_bldg_queue_handle_recruit_state, reached once llm_strat_bldg_find_idle_producer_for_unit has returned a producer. While the meter is > 0 (CMP/JLE @0x004e8136 -- SIGNED) the queued unit is taken FOR FREE: llm_strat_order_recruit_unit_enqueue is issued, NO resource moves anywhere in that arm, the meter is charged _G_LLM_STRAT_AI_CFG_PROMO_ADD (@0x004e8153) and the entry is stamped 0xc0. Otherwise the PAYING production path runs -- llm_strat_bldg_order_production_add_enqueue (misnamed; it issues order 0x6d, START PRODUCTION -- see finding 2026-08-02-1922-10), then llm_strat_econ_track_unit_resource_spend and resource_spent[1..4] -= queue_entry.reserved[0..3] (@0x004e818e-0x004e819f) -- and the meter EARNS _G_LLM_STRAT_AI_CFG_PROMO_SUB (@0x004e81cd), entry stamped 0x80. So PAYING BANKS CREDIT AND CREDIT BUYS A FREE UNIT. Both keys default to 1 and the meter starts at 0, so a stock AI alternates pay / free / pay / free -- a 50% discount on units. NOT DERIVED, deliberately: whether the free arm is also INSTANT (that is a property of llm_strat_order_recruit_unit_enqueue vs the paying order path, neither opened). Was ai_build_plan[34] until 2026-08-02 (AI1B layer 2). |
| `+0x10558` | `ai_attack_milestone_clock` | `float` | ai_clock latched at the last attack-milestone advance (FLOAT, raw dword copy). llm_strat_ai_army_milestone_advance_or_attack gates the next milestone on (ai_clock - ai_attack_milestone_clock) >= _G_LLM_STRAT_AI_ATTACK_TIME_TABLE[ai_attack_milestone_index]. Was ai_build_plan[0x23]. |
| `+0x1055c` | `ai_attack_milestone_index` | `int` | Index of the next attack-wave milestone, 0.._G_LLM_STRAT_AI_ATTACK_MILESTONE_COUNT-1 (AI.SCR nMaxAttacks); the whole milestone phase is skipped once it reaches the count. Indexes both _G_LLM_STRAT_AI_ATTACK_TIME_TABLE and _G_LLM_STRAT_AI_ATTACK_STRENGTH_TABLE. Was ai_build_plan[0x24]. |
| `+0x10560` | `next_group_serial` | `int` | AI unit-group serial counter |
| `+0x10564` | `ai_group_count` | `int` | active AI unit groups, max 0x20 |
| `+0x10568` | `ai_groups` | `llm_strat_ai_unit_group[32]` | AI task forces (old US_5[4] was misapplied 0xa bytes late and undersized) |
| `+0x25228` | `ai_target_list_count` | `int` | count for the AI's tracked-enemy-target list (array of up to 0x40 entries, stride 0x14, base absolute 0xe930ec -- confirmed to exactly fill the gap up to ai_bldg_queue_count, array itself not yet formally typed -- see llm_strat_ai_target_list_remove's existing plate comment and FUN_004d6867 the add-side counterpart). Also reset to 0 by llm_strat_spawn_ai_base/_invasion_force/llm_strat_init_human_player_data and, oddly, by llm_strat_ai_active_unit_tick as an unexplained side effect of its ai_groups[0] handling |
| `+0x2522c` | `ai_target_list` | `llm_strat_ai_target_entry[64]` | The AI's tracked-enemy-target list; live entry count is ai_target_list_count immediately above (+0x25228), capped at 0x40 by target_list_add. Extent verified: exactly fills the 0x500 gap to ai_bldg_queue_count. Formally typed 2026-08-01 (AI-PREP); it was raw DAT_/field_ columns before, which is why the AI functions touching it carried todo:struct. |
| `+0x2572c` | `ai_bldg_queue_count` | `int` | count of live entries in ai_bldg_queue[], capped at 0x40 (real compile-time bound, guarded at every enqueue site) |
| `+0x25730` | `ai_bldg_queue` | `llm_strat_ai_bldg_queue_entry[64]` | pending AI construction/repair/upgrade queue -- see llm_strat_ai_queue_train_unit/_bldg_repair/_bldg_upgrade/_release_order/_remove_at/_rotate_newest_to_front |
| `+0x25bb0` | `ai_building_type_available` | `byte[100]` | Per-player availability flag for each BUILDING TYPE, indexed by cfg::final::data::Building[] index; the only value ever tested is == 1. Recovered 2026-08-01 (AI1A layer 2) from its 20 code referrers -- llm_strat_bldg_find_by_type_for_player and _by_ai_build_for_player scan it to pick a type the player may build, and llm_strat_ai_scan_construction_sites / _plan_mine_construction / _react_resource_shortage / _expand_adjacent_mine_relay / _scan_bldg_repair_upgrade all gate on it (the last one via cfg Building[].upgrade_index, i.e. 'is the UPGRADED type available'). It was `undefined` padding before, which is why a translation of scan_bldg_repair_upgrade had to reach it through the pad array. LENGTH IS THE INDEX DOMAIN, NOT THE GAP: Building[] is [100], so 100 entries are reachable. The 100 bytes that follow (to +0x25c78) are a separate unclassified run with no observed referrer -- deliberately NOT folded into this array. |
| `+0x25c14` | `ai_train_source_state` | `byte[100]` | Per-UNIT-TYPE production-slot state, rebuilt each llm_strat_ai_plan_unit_training pass: 0 = no production slot is making this unit type; 1 = a slot is making it with progress[player][slot].f3 CLEAR; 2 = the same with f3 SET. The index is a unit type, proved by the neighbouring `Unit[idx].ai_unit` lookup on the same value (0x004e6dfd path) -- note the CLEARING loop is nonetheless bounded by G_BUILDING_COUNT_TOTAL (0x004e6d6c) rather than the unit count, which looks like a copied bound and is harmless only while it covers the unit-type range. Read back at 0x004e6e8d as a plain non-zero test. Sits immediately after ai_building_type_available and fills the 0x64 gap to ai_resource_site_count exactly. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x25c78` | `ai_resource_site_count` | `int` | number of live entries in ai_resource_sites[]; zeroed then filled by llm_strat_spawn_ai_base's coarse map sweep. |
| `+0x25c7c` | `ai_resource_sites` | `llm_strat_ai_resource_site[1024]` | the AI's tracked resource-deposit sites, count in ai_resource_site_count immediately above. Populated by llm_strat_spawn_ai_base, re-ordered nearest-first by llm_strat_sort_sites_by_dist, consumed by llm_strat_ai_bldg_scan_resource_site_candidates. Extent verified by adjacency: 1024 * 0xa = 0x2800 exactly fills the gap to ai_resource_need_score at +0x2847c. |
| `+0x2847c` | `ai_resource_need_score` | `int` | computed need score for the currently-shortage-flagged resource (FUN_004e5186); compared against ai_resource_need_threshold |
| `+0x28480` | `ai_resource_shortage_state` | `int` | small state code (values 0-3 observed) written by FUN_004e803f; llm_strat_ai_plan_construction only reacts to a shortage when this == 0 |
| `+0x28484` | `ai_resource_need_threshold` | `int` | threshold ai_resource_need_score is compared against; initialized to 6 |
| `+0x28488` | `ai_intel_seen_count` | `int[8]` | AI per-OTHER-PLAYER THREAT counter: how many times that player has damaged something of MINE. `INC dword ptr [EAX + 0xe96348]` at 0x004db290 in llm_strat_ai_bldg_register_visible_building, once per call, AFTER the same-owner early-out at 0x004db26e -- so the diagonal (self-damage) is never counted -- and, unlike its sibling ai_intel_flags, NOT gated on the 0x40 (victim-is-a-building) bit, so unit victims count too. ROW = the player whose property was hit, COLUMN = the aggressor. SEMANTIC CORRECTED 2026-08-02 (finding 2026-08-02-0405-3): the first version of this comment read the table as RECONNAISSANCE ('sighting counter ... indexed by the sighted object's owner') because the callee alone cannot tell observer from victim. The call sites can: llm_strat_bldg_kill_credit (0x0044caa4-0x0044cab9) and llm_strat_unit_kill_credit (0x0044cdee-0x0044ce03) pass their OWN victim_player/victim_index in EDX/EAX and the killer's ref/index in ECX/EBX. The ROW/COLUMN DIRECTION was right and is unchanged: the row is still the player forming the opinion (ai_player_relation[col] = -1 at 0x004db472, target_list_add called with the row at 0x004db48b); only the identity of the row -- 'the observer' vs 'the player who was hit' -- changed. FIRST of three consecutive int[8] per-opponent blocks -- this, ai_intel_flags (+0x284a8), ai_player_relation (+0x284c8) -- exactly 0x20 apart and zeroed in the same init loop. Named 2026-08-02 (RI-AI AI1B); previously undefined bytes. |
| `+0x284a8` | `ai_intel_flags` | `int[8]` | AI per-OTHER-PLAYER THREAT bitset -- what kind of MY OWN property that player has hit. Same row/column convention as ai_intel_seen_count above (row = the player who was hit, column = the aggressor). ONLY THE LOW BYTE is ever written -- five `OR byte ptr [.. + col*4 + 0xe96368]` sites in llm_strat_ai_bldg_register_visible_building, all inside the `TEST byte ptr [victim_ref],0x40` gate at 0x004db296, i.e. only when the DAMAGED object was a building: 0x1 = a building of mine was hit (0x004db2a0, unconditional within the gate); 0x2 = a MINE of mine (0x004db2e8); 0x4 = a TURRET of mine (0x004db346); 0x10 = a MOTHER of mine (0x004db3a4); 0x8 = the hit landed within ai_expand_gate_value of MY ai_home_tile (0x004db432, llm_strat_toroidal_dist_sq compared against the SQUARED gate). SEMANTIC CORRECTED 2026-08-02 (finding 2026-08-02-0405-3) from a sighting reading; see ai_intel_seen_count for the call-site derivation. TWO THINGS A READER OF THE KIND BITS WILL TRIP ON. (a) The three type tests pick a race-paired cfg Building member off pd[ROW].is_alien_race (2/0x16 mine, 5/0x19 turret, 6/0x1a mother) and read buildings[ROW][...] -- i.e. the row player's OWN race and OWN roster, which is the callee-side proof that the classified object belongs to the row player. (b) The cfg Building record is selected with the VICTIM'S ROSTER INDEX (IMUL EAX,EDI,0x842 at 0x004db2bc/0x004db31a/0x004db378) rather than with buildings[owner][index].building_id, which is how every other AI site reaches a cfg record; both index spaces are 0..99 so it is self-consistent, but the bits are keyed on Building[roster_slot].type and are unlikely to mean what the author intended. TYPE IS int[8], NOT byte[32], and that does not rest on the stride-4 indexing here: llm_strat_ai_recompute_shortage_state compares a full `dword ptr [EAX + EBX*0x4 + 0xe96368]` at 0x004e85a0 and clears a full dword at 0x004e85cd. Named 2026-08-02 (RI-AI AI1B); previously undefined bytes. |
| `+0x284c8` | `ai_player_relation` | `int[8]` | diplomatic relation toward each player index 0..7: +1 = self/friendly, -1 = hostile. Initialised by llm_strat_spawn_ai_base / _spawn_invasion_force / llm_strat_init_human_player_data as (i == self) ? 1 : -1. Every AI hostility test is a sign test (< 0 = hostile), e.g. llm_strat_ai_group_area_scan_hostile indexing it by the tile owner nibble. THIRD of three consecutive int[8] per-opponent blocks -- ai_intel_seen_count (+0x28488), ai_intel_flags (+0x284a8), this (+0x284c8) -- exactly 0x20 apart and zeroed in the same init loop. (The two siblings were undefined bytes until they were named 2026-08-02; this sentence used to say so.) |
| `+0x284e8` | `ai_train_queued_by_unit_type` | `int[100]` | How many training entries the AI currently has queued for each UNIT TYPE: INC'd by llm_strat_ai_queue_train_unit (0x004e282e) and DEC'd by llm_strat_ai_queue_flush_unit_train_entries (0x004e2c3f), both indexing by the unit type the caller passed. Zeroed at spawn over 0..G_UNIT_COUNT_TOTAL inclusive. Extent int[100] is fixed by adjacency: +0x284e8 + 100*4 lands exactly on ai_train_queued_by_ai_unit below. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x28678` | `ai_train_queued_by_ai_unit` | `int[12]` | The same queued-training count aggregated by AI ROLE instead of by type -- indexed by `cfg::final::struct::Unit[type].ai_unit` (+0x237 of the 0x23f-byte cfg Unit record), maintained in lockstep with ai_train_queued_by_unit_type by the same INC/DEC pair (0x004e2837 / 0x004e2c5f). llm_strat_ai_plan_unit_training reads it to pick the role with the FEWEST queued (running-minimum at 0x004e6e68). Extent int[12] is the remaining gap to ai_score_cat_1. Named 2026-08-02 (RI-AI); previously undefined bytes. |
| `+0x286a8` | `ai_score_cat_1` | `int` | build-category score cache (llm_strat_ai_score_build_categories): built+queued count for ai_build category 1 |
| `+0x286ac` | `ai_score_cat_0x20` | `int` | built+queued count for ai_build category 0x20 |
| `+0x286b0` | `ai_score_cat_0x21` | `int` | built+queued count for ai_build category 0x21 |
| `+0x286b4` | `ai_score_cat_0x23` | `int` | built+queued count for ai_build category 0x23 (stored before 0x22, out of numeric order) |
| `+0x286b8` | `ai_score_cat_0x22` | `int` | built+queued count for ai_build category 0x22 |
| `+0x286bc` | `ai_score_cat_0x10` | `int` | built+queued count for ai_build category 0x10 |
| `+0x286c0` | `ai_score_cat_0x11` | `int` | built+queued count for ai_build category 0x11 |
| `+0x286c4` | `ai_score_cat_0x12` | `int` | built+queued count for ai_build category 0x12 |
| `+0x286c8` | `ai_score_cat_0x13` | `int` | built+queued count for ai_build category 0x13 |
| `+0x286cc` | `ai_score_bldg_type_a` | `int` | built-count + queue-pending flag for the race-selected TURRET type (BLDG_TYPE_A_TURRET 5 / H_TURRET 0x19), written by llm_strat_ai_score_build_categories @0x004e55fd as bldg_count_by_type + bldg_type_queue_has_pending. IT HAS EXACTLY ONE REFERRER IN THE IMAGE AND THAT REFERRER IS THAT WRITE -- an instruction sweep for 0xe9658c (2026-08-02) returns nothing else. The previous comment here claimed llm_strat_ai_plan_mine_construction reads it 'as an owned-mine-count-like cap'; it does not, it reads ai_score_bldg_type_b at +0x286d0, and so does llm_strat_ai_storage_capacity_short_and_cap_check. |
| `+0x286d0` | `ai_score_bldg_type_b` | `int` | same shape as ai_score_bldg_type_a but for the race-selected MINE type (BLDG_TYPE_A_MINE 2 / H_MINE 0x16): count_by_type + queue-pending, written by llm_strat_ai_score_build_categories @0x004e567a. THIS is the field the mine logic gates on -- llm_strat_ai_plan_mine_construction reads it twice (@0x004e546d, @0x004e54a0) as the owned-mine count in both of its queue gates, llm_strat_ai_storage_capacity_short_and_cap_check compares a count against it @0x004e3904, and three llm_strat_ai_group_task_* functions test it against 0. |
| `+0x286d4` | `ai_score_cat_0x30` | `int` | built+queued count for ai_build category 0x30 |
| `+0x286d8` | `ai_opponent_assessments` | `llm_strat_ai_opponent_assessment[8]` | per-opponent military/economic assessment cache, one slot per player id (0-7), refreshed by llm_strat_ai_update_opponent_relations from llm_strat_ai_player_tick for every other active player; offset/size confirmed to exactly fill the gap directly after the ai_score_cat_* build-category cache |
| `+0x288b8` | `ai_mother_building_type` | `uint` | cfg::final::data::Building[] index of this player's race MOTHER building (A_MOTHER/H_MOTHER), cached at base setup by FUN_004dc2be via an ai_build==1 lookup; 0xffffffff if none. llm_strat_load_base_layout_dmp skips re-queueing this type from the starting-base script since the landing sequence already places it |
| `+0x288bc` | `ai_build_candidate_primary` | `int` | cached candidate building-type id for the construction planner's primary live-priority slot; compared against a just-completed building's id in llm_strat_ai_notify_bldg_constructed |
| `+0x288c0` | `ai_build_candidate_secondary` | `int` | cached candidate building-type id, construction planner's secondary live-priority slot |
| `+0x288c4` | `ai_build_candidate_shortage` | `int` | cached candidate building-type id, gated by llm_strat_ai_storage_capacity_short_and_cap_check |
| `+0x288c8` | `ai_build_candidate_cat_0x31` | `int` | cached candidate building-type id for ai_build category 0x31, written by llm_strat_ai_init_build_candidate_priorities. WRITE-ONLY: that store is its ONLY referrer in the whole image (scan_raw_pointers 2026-08-01) -- nothing reads it, unlike its sibling ai_build_candidate_cat_0x30. |
| `+0x288cc` | `ai_build_candidate_cat_0x30` | `int` | cached candidate building-type id for ai_build category 0x30, gated by the matching ai_score_cat_0x30 counter. llm_strat_ai_react_resource_shortage queues it as a final fallback after the four ai_resource_shortage_candidates; llm_strat_ai_is_worker_priority_candidate treats it as a shortage-priority type. |
| `+0x288d0` | `ai_mine_candidate_tier1` | `int` | cached candidate mine-type building id (ai_build 0x50), -1 if none researched/available; llm_strat_ai_plan_mine_construction |
| `+0x288d4` | `ai_mine_candidate_tier2` | `int` | cached candidate mine-type building id (ai_build 0x51), -1 if none; takes priority over ai_mine_candidate_tier1 when both are available |
| `+0x288d8` | `ai_mine_alt_candidates` | `int[4]` | 4 alternate mine-type building candidates (ai_build categories 0x40-0x43, race MINE-type filter via FUN_004d31c4), populated by FUN_004dc2be; -1 if none. llm_strat_ai_scan_construction_sites picks one via FUN_004d3b63(4) (random index 0-3), retrying until it lands on an available (index != -1 and researched-not-built) candidate -- a genuine uniformly-indexed array, unlike the sibling ai_resource_shortage_candidates group which is only ever accessed via 4 unrolled reads |
| `+0x288e8` | `ai_turret_candidate` | `int` | cached candidate turret/defense building-type id, -1 if none researched; gates llm_strat_ai_plan_turret_upgrade entirely |
| `+0x288ec` | `ai_resource_shortage_candidates` | `int[4]` | one cached candidate production-building-type id per basic resource (ai_build 0x10-0x13, index = resource id 0-3), -1 if none; read by llm_strat_ai_react_resource_shortage and llm_strat_ai_is_worker_priority_candidate as 4 unrolled slots rather than an indexed loop |

#### `llm_strat_player_desc` (size 0x34, category `/Manual/game`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `race_or_faction` | `byte` | copied to DAT_00e58361 for the local side |
| `+0x01` | `color_or_team` | `byte` | Per-player colour/team index. CONFIRMED 2026-08-24 (was 'passed to per-player init CHECK_FUN_00454985 (unverified)'): llm_strat_session_begin_multi PUSHes it (MOVZX at 0x00454485) as the 6th argument of llm_strat_player_profile_init (0x00454985), whose body reads it at [EBP+0x10] three times (0x00454a68 / 0x00454a78 / 0x00454a86) and passes it as the second argument of llm_strat_player_set_color. |
| `+0x02` | `ui_sprite_index` | `uint` | NOT credits -- a UI sprite/swatch index. llm_lobby_build_players_finish (0x004beb33) writes _G_LLM_GFX_UI_SPRITE_BASE_INDEX + _G_LLM_LOBBY_SLOTS[slot].color_index + 0x1c here, overwriting the slot+0x07 value build_players_step memcpy'd in. That writer is the ONLY reference to the field (xrefs, 2026-08-29): nothing reads it back. The old name 'start_credits' and its 'scenario f4' addend were both wrong -- the addend is the slot's colour index -- and the wrong name is already quoted as 'credits=' in the D23 PLAYERDUMP evidence. |
| `+0x06` | `controller_flags` | `byte` | 7 = active human/AI, 0xb = neutral 'Mercenaries'; nonzero = slot in use; bit 2 checked on duplicate-side merge |
| `+0x07` | `-` | `byte` |  |
| `+0x08` | `relation` | `byte[8]` | diplomacy vs each player; 2 = self/ally |
| `+0x10` | `name` | `char[32]` | player name |
| `+0x30` | `scenario_side_id` | `int` | race/side id from map header f2; -1 for the neutral slot |

#### `tact_unit_record` (size 0x5f4, category `/Manual/game`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `type` | `byte` | building type id; 0=empty (guard <=0x80); indexes anim descriptor DAT_008742d8 + type*0x6c |
| `+0x01` | `owner` | `byte` | side/faction (character WHO); projectile with equal fx_record.owner does no damage. Was 'hidden_flag'. |
| `+0x02` | `status` | `byte` | bits: 1=owned/selection marker, 8=FIRE, 0x60=active/animated |
| `+0x03` | `pos_col` | `byte` | anchor tile column (checked in destroy: tile_objects[pos_col][pos_row].building == id) |
| `+0x04` | `pos_row` | `byte` | anchor tile row; building occupies a 2x2 tile footprint |
| `+0x05` | `anim_state` | `byte` | 0/1 timed-move, 2/3/4 progress anims, 0x1f special |
| `+0x06` | `def_stat` | `byte` | 'def' value shown in debug overlay |
| `+0x07` | `vision_angle` | `ushort` | = character.angle_see at spawn |
| `+0x09` | `vision_dist` | `byte` | = character.distance_see at spawn |
| `+0x0a` | `facing_dir` | `byte` | facing 0..0x17 (0x18->1); selects directional sprite |
| `+0x0b` | `move_state_timer` | `double` | Multi-purpose per-tick state clock used by llm_tact_unit_move_tick, and it means three different things. NORMAL path: accumulates the dt parameter every call (FLD 0x00430349 / FADD [EBP+0x8] 0x0043034f / FSTP 0x00430352; same shape at 0x0042fcbc-0x0042fcc5). KNEEL path (anim_state 2 or 3, tested 0x0042fbc6/0x0042fbd6): advanced by the character type's fixed kneel_time instead of dt (FLD [EDX*0x6c+0x873c38] 0x0042fc02, FADD 0x0042fc08, FSTP 0x0042fc0e) then jumps past the normal trailer at 0x0042fc14. DOOR path: OVERWRITTEN with the absolute time_GetCurrentTime() value, not an add (0x00430207-0x00430213). Elapsed time in two modes, a timestamp in the third -- do not assume one reading. |
| `+0x13` | `weapon_timer` | `double` | last-fire timestamp (llm_tact_unit_fire_weapon turret update) |
| `+0x1b` | `wander_check_time` | `double` | Timestamp of this unit's last idle-wander evaluation. llm_tact_unit_owner_tick re-rolls a wander direction only once time_GetCurrentTime() exceeds wander_check_time + _G_LLM_TACT_UNIT_WANDER_RETRY_INTERVAL (FLD 0x00433b42 / FADD [0x005004ac] 0x00433b48 / FCOMP 0x00433b56 / JBE 0x00433b5c), then restamps it (0x00433b62-0x00433b6e) and enqueues a FACE/TURN command (op 6) at 0x00433bf9. |
| `+0x23` | `cmd_wait_until_time` | `double` | Absolute time_GetCurrentTime() deadline for a pending timed-WAIT command (queue op 8). Set to now + (double)cmd_queue[cmd_index].arg0 at 0x0042f708-0x0042f71d, and the dispatch loop keeps re-issuing the wait -- skipping the op switch entirely -- while now is below it (FLDZ/FCOMP-vs-0 guard at 0x0042f599, FCOMP-vs-now at 0x0042f5b2), all in llm_tact_unit_weapons_tick. Zeroed by two back-to-back 32-bit MOV-0 stores -- the compiler's zero-a-double idiom, NOT two fields -- when a MOVE (op 1) is dispatched (0x0042f767 + 0x0042f771) and at spawn (0x0042babd + 0x0042bac7). This subsumes what the decompiler rendered as field_0x23 and field_0x27; the FSTP at 0x0042f71d proves the 8-byte width. |
| `+0x2b` | `anim_frame_time` | `double` | timestamp of last frame advance |
| `+0x33` | `frame_index` | `byte` | current animation frame 0..0x1f |
| `+0x34` | `anim_cycle_time` | `double` | timestamp of animation cycle start |
| `+0x3c` | `frame_interval` | `double` | seconds per animation frame |
| `+0x44` | `progress` | `byte` | action/movement progress; drives 2/3/4 anim states and move offset |
| `+0x45` | `sprite_id` | `ushort` | current display sprite id (output; drawn by llm_tact_unit_render) |
| `+0x47` | `move_path_slot` | `short` | Index of the global pathfinder path slot assigned to this unit's current move (mirrors _G_LLM_TACT_MOVE_PATH_SLOT_ID), 0 = none. While nonzero llm_tact_unit_move_tick skips re-pathing and goes straight to consuming the existing path (CMP 0x0042ff5b / JNZ 0x0042ff63 -> 0x00430091). Assigned from the global at 0x0042feed-0x0042fef4 and 0x00430031; cleared on arrival at 0x00430155 with the slot-flag release at 0x00430147. Sentinel -1 in the GLOBAL means no path found (tested 0x0042feb8). |
| `+0x49` | `move_path_step` | `short` | Step index within the slot named by move_path_slot: the per-tick facing byte is fetched at _G_LLM_STRAT_PATH_BUFFERS + move_path_slot*0x258 + move_path_step*2 (MOVZX 0x0042fd3c / ADD EAX,EAX 0x0042fd43 / MOVZX [EAX+0xaee8c0] 0x0042fd47) and fed to llm_tact_facing_to_delta at 0x0042fd5a. Zeroed whenever a new slot is assigned (0x0042ff13, 0x00430050). The INCREMENT site is NOT in move_tick -- believed to be llm_tact_unit_move_advance (0x00430f5a), unverified. |
| `+0x4b` | `hp` | `ushort` | hit points; llm_tact_fx_update_projectile subtracts fx_type.damage; <=damage -> anim_state 0x1f (dying) |
| `+0x4d` | `move_retry_wait` | `short` | Inner countdown in ticks before the next fast pathfinding retry. Decremented once per llm_tact_unit_move_tick call while above 0 (DEC 0x0042fcae, guarded 0x0042fca5). Reset to 0x10 at every site that also resets move_retry_attempts to 0xc: 0x0042fcec, 0x0042fed8, 0x00430005, 0x00430268. Also serves as the has-a-live-move-command gate in llm_tact_unit_owner_tick (CMP 0x00433670 / JZ 0x00433678). |
| `+0x4f` | `move_retry_attempts` | `short` | Retries remaining for a blocked move; decremented once each time move_retry_wait reaches 0, i.e. once per real retry (DEC 0x0042fcfc, guarded 0x0042fcd7/0x0042fcdf which falls through to move_stuck_countdown when already 0). Reset to 0xc at 0x0042fed8, 0x00430005 and 0x00430278, always paired with move_retry_wait = 0x10. |
| `+0x51` | `move_stuck_countdown` | `short` | Outer give-up budget for a stuck move command, consumed once per exhausted fast-retry cycle (guard 0x0042fddf/0x0042fde7, DEC 0x0042fdf4, re-test 0x0042fe02/0x0042fe0a). On reaching 0 the command is ABORTED: the low byte of the current queue entry's op is snapshotted into move_aborted_op (MOV AL,[EAX+0x826125] 0x0042fe20 / MOV [EDX+0x8266ba],AL 0x0042fe26) and llm_tact_unit_cmd_advance is called (0x0042fe32). Reset to 3 at 0x00430015 and 0x00430288 alongside move_retry_wait/attempts. |
| `+0x53` | `cmd_index` | `byte` | current command-queue index (was 'weapon_subindex') |
| `+0x54` | `cmd_queue` | `llm_tact_unit_cmd_entry[128]` | Circular command queue: 0x80 entries x 0xb bytes, spanning +0x54..+0x5d3 with NO slack. Head is cmd_index (+0x53), advanced with wrap at 0x80 by llm_tact_unit_cmd_advance (0x0043133c-0x00431359); every wrap in the file tests `0x7f < idx`, so index 0x80 is unreachable and +0x5d4 is a separate field rather than entry 128. Written by llm_tact_unit_enqueue_command (0x0042b39d), consumed by llm_tact_unit_owner_tick / llm_tact_unit_move_tick, re-submitted by llm_tact_unit_cmd_queue_resubmit_run (0x0043069e).  BASE CORRECTED FROM +0x55 TO +0x54 on 2026-08-24 (finding 2026-08-24-1151-11). The old model documented 0xb-byte entries carrying only 10 bytes of named content and left an unexplained spare at the END of every entry; the leading byte was modelled separately as `cmd_slot_flag` with the other five fields as 'ENTRY-0 ALIAS' scalars. Both models predict the SAME addresses for every field that was already documented, so no earlier reading depended on +0x55. Five independent sites decide it, all indexing base 0x826124 (= unit 0 +0x54) with stride 0xb at ONE index: the enqueue's six-store block (0x0042b88f-0x0042b90a), the resubmit's six-read reconstruction (0x004306f5-0x0043076e), the two occupancy guards (0x00433641/0x00433658 and 0x0042b400/0x0042b416), and the debug overlay that prints flag and op for the same slot. The far-end objection dissolves too: +0x5d4..+0x5de and +0x5df..+0x5e9 are two MORE records of this identical 0xb-byte shape -- the immediate ATTACK/AIM and FACE/TURN slots the enqueue writes wholesale on its op==2 and op==6 short-circuits (0x0042b79f-0x0042b7f2, 0x0042b72c-0x0042b77a) -- and the third of them ends exactly where move_aborted_op begins at +0x5ea. Three records tile perfectly with zero orphan bytes. See docs/dead-ends.md L1: derive a record's base from the LOWEST offset any instruction touches with the stride, not from the first field you named. |
| `+0x5d4` | `attack_interrupt_flag` | `byte` | interrupt_flag of the immediate ATTACK/AIM command record at +0x5d4 (the same 0xb-byte shape as mh_llm_tact_unit_cmd_entry; its op is attack_cmd_op at +0x5d5). Set from llm_tact_unit_enqueue_command's interrupt_flag parameter at 0x0042b79f on the op==2 short-circuit. LEAD, not settled: no reader was found -- the role is inferred from the position/shape parallel with the FACE record's face_interrupt_flag, which DOES have one. |
| `+0x5d5` | `attack_cmd_op` | `ushort` | op field of an IMMEDIATE (non-queued) command record at +0x5d4 that has the SAME 0xb-byte llm_tact_unit_cmd_entry shape: interrupt_flag +0x5d4, op +0x5d5, arg0 +0x5d7, arg1 +0x5d9, arg2 = aim_x +0x5db, arg3 = aim_y +0x5dd, ending +0x5de. Written wholesale by llm_tact_unit_enqueue_command's op==2 ATTACK/AIM short-circuit (0x0042b79f-0x0042b7f2) and reset to 0 by the op 0x46 CLEAR (0x0042b65b). A twin record for FACE/TURN occupies +0x5df..+0x5e9 (written 0x0042b72c-0x0042b77a, its op at +0x5e0), landing exactly on move_aborted_op at +0x5ea. These two immediate slots are why the queue's far end at +0x5d4 is not a hole. Deliberately NOT retyped as llm_tact_unit_cmd_entry: doing so would fold the hand-named, separately documented aim_x/aim_y into arg2/arg3 (finding 2026-08-24-1151-11, recommendation (a)). Renamed from field_5d5 2026-08-24. |
| `+0x5d7` | `attack_gun_toggle` | `ushort` | arg0 of the immediate ATTACK/AIM record. Passed as fire_arg to llm_tact_unit_fire_weapon (load 0x0042f401, call 0x0042f40e), which XORs it with active_gun at 0x00430925 to pick the firing gun -- so 0 fires the currently active gun and 1 fires the other. Written from the enqueue's arg0 at 0x0042b7bf; zeroed at spawn (0x0042bd5f). |
| `+0x5d9` | `attack_cmd_arg1` | `ushort` | arg1 slot of the immediate ATTACK/AIM record, written wholesale with the rest of it (0x0042b7d0) and zeroed at spawn (0x0042bd6f). NO READER FOUND anywhere in the exported tactical set: it exists because the record shares the 0xb-byte cmd_entry shape and the enqueue fills every arg slot. Scaffolding name -- a LEAD, not a reading. |
| `+0x5db` | `aim_x` | `ushort` | current target/aim X (weapon logic) |
| `+0x5dd` | `aim_y` | `ushort` | current target/aim Y |
| `+0x5df` | `face_interrupt_flag` | `byte` | interrupt_flag of the immediate FACE/TURN command record at +0x5df (0xb-byte cmd_entry shape; its op is face_cmd_op at +0x5e0). Set from the enqueue's interrupt_flag parameter at 0x0042b72c, and READ as part of the re-entry guard that refuses to re-issue a FACE onto an already-armed slot (CMP at 0x0042b6f3, paired with the face_cmd_op==6 test at 0x0042b6e0). |
| `+0x5e0` | `face_cmd_op` | `ushort` | op of the immediate FACE/TURN record: 6 = turn pending, 0 = free/consumed. Armed to 6 by llm_tact_unit_enqueue_command's op==6 short-circuit (0x0042b739) and by the op-7 turn-then-attack path in llm_tact_unit_weapons_tick (0x0042f477). Cleared to 0 by llm_tact_unit_rotate_tick once facing_dir reaches face_cmd_target_dir, or immediately if status bit 0x8 (FIRE) is set (0x004307f9, 0x00430843), and by the op 0x46 CLEAR in both enqueue_command (0x0042b64b) and llm_tact_group_issue_order (0x0042b258). Read as the 'is it turning' gate before calling rotate_tick (0x0042f4e4 -> call 0x0042f504). |
| `+0x5e2` | `face_cmd_target_dir` | `ushort` | arg0 of the immediate FACE/TURN record: the target dir24 heading (1..0x18) to rotate to. Written from the enqueue's arg0 (0x0042b74c) or from cmd_queue[idx].arg0 on the op-7 path (0x0042f49b). llm_tact_unit_rotate_tick compares it against facing_dir (load 0x00430819, CMP 0x00430820) and passes it to llm_tact_unit_rotate_step (0x0043082b/0x00430835) until they match. |
| `+0x5e4` | `face_cmd_arg1` | `ushort` | arg1 slot of the immediate FACE/TURN record. Written 0 by every writer found (0x0042b75a, 0x0042f4a9); no reader anywhere in the exported tactical set. Unused payload of the shared 0xb-byte cmd_entry shape -- scaffolding name, a LEAD. |
| `+0x5e6` | `face_cmd_arg2` | `ushort` | arg2 slot of the immediate FACE/TURN record. Written 0 at 0x0042b76a and 0x0042f4b9; no reader found. See face_cmd_arg1. |
| `+0x5e8` | `face_cmd_arg3` | `ushort` | arg3 slot of the immediate FACE/TURN record. Written 0 at 0x0042b77a and 0x0042f4c9; no reader found. It completes the record at +0x5df..+0x5e9 with zero orphan bytes, landing exactly on move_aborted_op at +0x5ea. See face_cmd_arg1. |
| `+0x5ea` | `move_aborted_op` | `byte` | Low byte of cmd_queue[cmd_index].op snapshotted at the moment move_stuck_countdown expires and the command is force-dequeued (MOV AL,[EAX+0x826125] 0x0042fe20 / MOV [EDX+0x8266ba],AL 0x0042fe26) -- which op was interrupted by a stuck move. Sole write site; no reader found in llm_tact_unit_move_tick, llm_tact_unit_owner_tick or llm_tact_render_view. |
| `+0x5eb` | `active_gun` | `byte` | Selected weapon slot, 0 = gun1, 1 = gun2. Used as a +0/+1 bias onto llm_tact_character_type.height_gun1 (+0x1e) when computing the firing-line height: MOVZX type 0x004337ed / IMUL 0x6c 0x004337ea / MOVZX this 0x004337f4 / ADD 0x004337fb / MOVZX [EAX+0x873c22] 0x004337fd, where 0x873c22 = _G_LLM_TACT_CHARACTER_TYPES (0x00873c04, llm_tact_character_type[16]) + 0x1e; repeated at 0x004338dd and 0x00433a44. Printed raw as the gun:%1d field of llm_tact_render_view's debug overlay (0x0042d94d, sprintf 0x0042d96e). NOTE the decompiled C renders this as character_type.name[field_0x5eb + 0x1e] because name is at +0 and Ghidra folds the +0x1e into the index -- it is NOT a name-table lookup. No write site found in the three readers. |
| `+0x5ec` | `gun1_bullets` | `byte` | = fx_type[gun1].bullets at spawn; decremented per shot |
| `+0x5ed` | `gun2_bullets` | `byte` | = fx_type[gun2].bullets at spawn |
| `+0x5ee` | `gun1_magazines` | `byte` | = fx_type[gun1].magazines - 1 at spawn |
| `+0x5ef` | `gun2_magazines` | `byte` | = fx_type[gun2].magazines - 1 at spawn |
| `+0x5f0` | `squad_group_id` | `byte` | Player-assigned squad / control-group id, 0..7, with 0xff = unassigned (the spawn default, set at 0x0042bdad). llm_tact_ui_sel_panel_multi_mode_tick writes the active group (_G_LLM_TACT_SIDEBAR_ACTIVE_GROUP_ID) into every selected owner-0 unit at 0x00436336 ('assign to group') and re-selects every unit whose value matches it at 0x00436390/0x00436397 ('recall group'). llm_tact_sidebar_dispatch sets it from a sidebar icon click (0x00435adc) or resets it to 0xff (0x00435b01). llm_tact_squad_roster_refresh buckets every active unit by it (0x0043575f/0x00435769) into _G_LLM_TACT_UNASSIGNED_UNIT_ROSTER or _G_LLM_TACT_GROUP_UNIT_ROSTER[id]. ENUM candidate (8 groups + the 0xff sentinel). |
| `+0x5f1` | `move_redirect_col` | `byte` | Scratch tile COLUMN for re-targeting an in-flight MOVE. Snapshotted from pos_col whenever the command queue is CLEARed (op 0x46) -- 0x0042b5ce in enqueue_command, 0x0042b1f1 in llm_tact_group_issue_order. group_issue_order then uses it (with move_redirect_row) as the current-position input to llm_tact_move_step_attempt (0x0042b2c7/0x0042b2ce) and llm_tact_move_path_preview_walk (0x0042b2fe/0x0042b309), overwriting it with the walked-forward column at 0x0042b318. llm_tact_frame also reads it at 0x0042a873 to compute the click-preview facing. Confidence MEDIUM on the name -- the mechanism is traced, the intended concept is not. |
| `+0x5f2` | `move_redirect_row` | `byte` | Row counterpart of move_redirect_col, snapshotted from pos_row. Written 0x0042b5e8, 0x0042b20b, 0x0042b328; read 0x0042b2b9, 0x0042b2f0, 0x0042a884. |
| `+0x5f3` | `click_preview_facing` | `byte` | dir24 heading (0..0x17) computed per frame by llm_tact_frame's click-preview overlay for selected units with status bits 0x41, from llm_tact_calc_dir24(camera+cursor vs move_redirect_col/row) -- call 0x0042a8ba, store 0x0042a8e6. Consumed by llm_tact_group_issue_order at 0x0042b108 as an OVERRIDE for the order's target-direction argument when the order code is 7 (turn-then-attack), i.e. the live UI preview direction becomes the actual attack facing at issue time. |

#### `gfx_sprite_meta` (size 0x18, category `/Manual/gfx`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `bnk_reserved_0x00` | `byte[4]` | The first 4 bytes of the 24-byte .BNK per-sprite metadata record, loaded VERBATIM from the asset and read by NOTHING in the binary. Named 2026-08-24 (finding 2026-08-24-1348-3) to retire the placeholder `f1` + _pad_0x01[3]; the name records provenance, not meaning. EVIDENCE FOR 'no reader': the five references to the array base 0x00714120 are all the SAME pointer setup in the bank loaders -- `MOV [EBP-N],0x714120` then `IMUL EAX,[bank*4 + _G_LLM_BANK_SPRITE_BASE],0x18` / `ADD [EBP-N],EAX` (llm_gfx_load_banks 0x00465399-0x004653ad, and identically in llm_gfx_load_sprite_bank_files, llm_gfx_load_planet_extra_sprite_banks, llm_gfx_planet_bank_load, llm_tact_gfx_load_banks_alt) -- i.e. computing a destination for the bulk copy, not reading a field. The addresses 0x00714121/22/23 occur ZERO times image-wide (Memory.findBytes), while +0x04 (origin_x, 0x00714124) has 40+ referencing renderers. CAVEAT, stated because the scan cannot close it: a renderer holding &meta[i] in a register and reading `[reg]` would be invisible to an address scan, since the displacement would be 0. BNK_FORMAT.md documents +0x04 / +0x06 as the hotspot and calls the remaining 20 bytes unknown-and-preserved, so there is no external name to borrow either. |
| `+0x04` | `origin_x` | `short` | sprite anchor/hotspot X (subtracted from tile draw position by the renderer) |
| `+0x06` | `origin_y` | `short` | sprite anchor/hotspot Y (subtracted from tile draw position by the renderer) |
| `+0x08` | `mount1_x` | `short` | weapon mount slot 1 X anchor offset (added to origin_x-based fine pos; see llm_strat_unit_calc_mount_fine_pos/_render_pos). Also read generically as a per-frame draw offset by llm_strat_unit_soldier_get_sprite_screen_pos |
| `+0x0a` | `mount1_y` | `short` | weapon mount slot 1 Y anchor offset (added to origin_y-based fine pos). Also read generically as a per-frame draw offset by llm_strat_unit_soldier_get_sprite_screen_pos |
| `+0x0c` | `mount2_x` | `short` | weapon mount slot 2 X anchor offset |
| `+0x0e` | `mount2_y` | `short` | weapon mount slot 2 Y anchor offset |
| `+0x10` | `submount_x` | `short` | independent-turret sub-sprite base-frame X anchor offset (read at Unit.sprite+facing_current+0x17, alongside mount1/mount2 on the base frame) |
| `+0x12` | `submount_y` | `short` | independent-turret sub-sprite base-frame Y anchor offset |
| `+0x14` | `pip_anchor4_x` | `short` | RD-A readers readiness (2026-08-24). FOURTH X,Y anchor pair of the 24-byte BNK sprite-metadata record, after origin / mount1 / mount2 / submount. EVIDENCE IS POSITIONAL, NOT SEMANTIC, and that is the honest limit: unlike submount_x/y -- which two renderers read independently for real turret sub-sprite placement -- this pair has exactly ONE reader in the whole binary, llm_strat_bldg_init_defaults, which treats it exactly as it treats submount: guard both halves > 0 (CMP 0x0045b4ec / 0x0045b510), then MOVSX and add the X half into the pip X accumulator (0x0045b540) and the Y half into the pip Y accumulator (0x0045b576), writing pip slot 4 and setting Building[].pip_slot_count = 4. Loaded verbatim from the .BNK metadata section (all three bank loaders memcpy the whole 24-byte record; nothing computes it), so it is art-asset data. Deliberately NOT named submount2_* -- nothing supports a second turret sub-sprite. |
| `+0x16` | `pip_anchor4_y` | `short` | RD-A readers readiness (2026-08-24). Y half of pip_anchor4_x; same single reader, same guard, same positional-only evidence. MOVSX at 0x0045b576 into the pip Y accumulator. |

#### `gfx_struct_banki_row` (size 0xc, category `/Manual/gfx`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `offsets_size` | `int` | = BNK_FORMAT.md indicesSize: byte size of each index array; sprite count = /4 |
| `+0x04` | `data_size` | `int` | = BNK_FORMAT.md spriteDataSize: RLE sprite data section size |
| `+0x08` | `size_2_p` | `int` | = BNK_FORMAT.md paletteDataSize: palette section size; 0 => V2 (inline RGB565) bank |

#### `gfx_struct_cursor_anim_state` (size 0x10, category `/Manual/gfx`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `frame` | `cfg_t_frame_index` |  |
| `+0x04` | `base_frame` | `cfg_t_frame_index` | The Anim[] frame this animation restarts from: llm_cursor_anim_tick copies it into `frame` both when the chain wraps (Anim[frame+1].next == 0) and when the active cursor animation is switched. Read at 0x0044555c/0x00445562 and 0x0044563e, stored into `frame` at 0x00445568 and 0x00445644; nothing in that function writes it, so it is set by whoever installs the animation. |
| `+0x08` | `last_time` | `double` | Timestamp of the last frame advance, in CURRENT_GAME_TIME units. llm_cursor_anim_tick takes elapsed = CURRENT_GAME_TIME - last_time, restamps last_time = CURRENT_GAME_TIME, then walks the Anim[] chain consuming Anim[frame+1].time per step until the remainder fits, subtracting the leftover back off last_time so the catch-up carries into the next call. Also restamped outright when the active animation is switched. |

#### `gfx_t_pixel` (size 0x2, category `/Manual/gfx`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `r` | `ushort:5` |  |
| `+0x00` | `g` | `ushort:5` |  |
| `+0x01` | `b` | `ushort:5` |  |

#### `game_progress` (size 0x3, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `available` | `bool` |  |
| `+0x01` | `acquired` | `bool` |  |
| `+0x02` | `f3` | `bool` |  |

#### `llm_map_bfs_entry` (size 0x4, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `byte` | tile X (wrapped) |
| `+0x01` | `y` | `byte` |  |
| `+0x02` | `depth` | `ushort` | BFS depth from seed; expansion stops at >0xe or fill-rate heuristic |

#### `llm_map_proximity_stencil_entry` (size 0x4, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `bit_index` | `byte` | 1<<bit stamped into region_cell.terrain_flags; concentric rings 4 (adjacent) / 3 / 2 / 0; 13x13 table, index i -> (i%13-6, i/13-6) |

#### `llm_map_region` (size 0x420, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `index` | `ushort` | region id (index into _G_LLM_MAP_REGION_BY_INDEX) |
| `+0x02` | `x` | `byte` | flood-fill seed tile |
| `+0x03` | `y` | `byte` |  |
| `+0x04` | `cell_count` | `uint` | cells claimed by this region (merge picks the smaller region) |
| `+0x08` | `neighbor_count` | `uint` | entries in both 128-slot arrays |
| `+0x0c` | `neighbors` | `llm_map_region *[128]` | adjacent regions (serialized as indices, re-resolved on load) |
| `+0x20c` | `neighbor_data` | `uint[128]` | shared-BORDER LENGTH per neighbor, in tiles -- PROVEN 2026-08-07, was 'border length / link cost? unproven'. llm_map_region_add_adjacency_edge is called once per right/down tile pair that crosses a region boundary and increments this by 1 on BOTH sides, so the value counts tile-edge crossings. Parallel to neighbors[]; only the first neighbor_count entries are live. |
| `+0x40c` | `next` | `llm_map_region *` | intrusive list link (active list or free list) |
| `+0x410` | `route_bfs_dist` | `int` | Scratch BFS-from-source distance/visited marker for llm_map_region_find_route: reset to 0 for every region at each find_route call, source=1, each newly-enqueued neighbor = parent+1; 0 doubles as 'unvisited'. (llm) |
| `+0x414` | `route_parent` | `llm_map_region *` | BFS predecessor / route-chain link: written by find_route during expansion (neighbor.route_parent=current), walked BACKWARD from goal to reconstruct the path and FORWARD as a 'next' by route_mark_shared_nodes over the reconstructed chain. (llm) |
| `+0x418` | `route_mark` | `int` | Multi-pass scratch flag (same 4 bytes reused by two non-concurrent subsystems): (a) find_route post-process sets 1 for regions on the discovered path, route_mark_shared_nodes sets 2 for a shared neighbor of two adjacent path regions; (b) pathfind_mark_group_member_regions seeds 2 for member-occupied regions and flood_reachable gates its flood on (route_mark!=0). 0=off. [confidence: med -- the reset discipline BETWEEN the two passes is unverified] (llm) |
| `+0x41c` | `prev` | `llm_map_region *` | set to 0, never seen read |

#### `llm_map_region_cell` (size 0x8, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `region` | `llm_map_region *` | owning region node; 0xffffffff = flood-fill pending, 0 = no region |
| `+0x04` | `terrain_flags` | `uint` | low byte from g::passable; bits 2/3/4 = obstacle within ~5/~3/~1 tiles (13x13 stencil) |

#### `llm_strat_crew_soldier` (size 0x1d, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `owner_unit` | `short` | roster index of owning unit; 0 = free slot; in record [0]: used-slot count |
| `+0x02` | `next_soldier` | `ushort` | next record in the unit's soldier chain (0 = end; head in unit.unit_above) |
| `+0x04` | `cur_x` | `schar` | interpolated offset = start + round((end-start)*t) |
| `+0x05` | `cur_y` | `schar` |  |
| `+0x06` | `sprite_frame` | `schar` | interpolated offset = start + round((end-start)*t) |
| `+0x07` | `start_x` | `schar` |  |
| `+0x08` | `start_y` | `schar` | interpolated offset = start + round((end-start)*t) |
| `+0x09` | `end_x` | `schar` |  |
| `+0x0a` | `end_y` | `schar` | interpolated offset = start + round((end-start)*t) |
| `+0x0b` | `walk_elapsed` | `double` |  |
| `+0x13` | `walk_duration` | `double` | 0.0 = idle (arrival zeroes both) |
| `+0x1b` | `anim_change_count` | `byte` | idle-animation nudge counter |
| `+0x1c` | `idle_wander_flag` | `byte` | set on head soldier when unit idle + 1% wander roll fires |

#### `llm_strat_ctrl_group` (size 0x194, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `count` | `int` | number of unit ids in the group |
| `+0x04` | `unit_ids` | `ushort[200]` | roster indices into map::units[side]; removal = swap-with-last or shift |

#### `llm_strat_landing_spot` (size 0xc, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `int` | tile X (clamped to map dims on load) |
| `+0x04` | `y` | `int` | tile Y |
| `+0x08` | `status` | `int` | -1 = end-of-list/unused, -2 = taken by a player, else free; spots are paired (idx^1) |

#### `llm_strat_map_geom` (size 0x28, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `bw_mask` | `uint` | pixel-space X wrap mask = big_width-1 (planet maps wrap; dims are powers of two) |
| `+0x04` | `bh_mask` | `uint` | pixel-space Y wrap mask = big_height-1 |
| `+0x08` | `width_mask` | `uint` | tile X wrap mask = width-1 |
| `+0x0c` | `pathfinder_params` | `llm_strat_pathfinder_params *` | malloc'd 0x20-byte packed pathfinder parameter block (utils_malloc_struct_array(1,0x20) at 0x00461594), wired up by llm_strat_pathfinder_init and re-wired each map load by llm_strat_pathfinder_ctx_link; freed by llm_strat_pathfinder_shutdown. TYPED 2026-08-24 (RD-A readers readiness) -- was `byte[32] *`, whose raw offset dereferences were llm_strat_pathfinder_ctx_link's todo:struct. The old comment named FUN_00487d97 / FUN_00494ab5 as the consumers; those addresses now hold unrelated functions and the claim was stale. The real chain is llm_strat_pathfind_next_step -> llm_pf_run -> {llm_pf_select_resolution, llm_pf_seed_goals, llm_pf_expand_wavefront}, and llm_strat_pathfind_dispatch_route_order -> llm_strat_pathfind_route_start_adjust / llm_strat_pathfind_flood_search_driver. |
| `+0x10` | `big_width` | `uint` | width*32 (map width in pixels) |
| `+0x18` | `big_height` | `uint` | height*32 |
| `+0x1c` | `pathfinder_workbuf` | `byte[533632] *` | malloc(0x82480) zero-filled per-cell pathfinder working state |
| `+0x20` | `height_mask` | `uint` | tile Y wrap mask = height-1 |
| `+0x24` | `change_flag` | `short` | zeroed before unit/building walks in sim_step; nonzero (with +0x26) fires events 6/8; llm_strat_bldg_notify_ui sets 1 |
| `+0x26` | `change_flag2` | `short` | second change flag, checked together with +0x24 as one dword |

#### `llm_strat_player_profile` (size 0x740, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `status_flags` | `E_STRAT_PLAYER_STATUS` | bit0 slot enabled; bit1 alive/has presence (cleared on elimination); bit2 human-controlled; bit3 AI-controlled (gates DMP AI scripts); bit4 net-dropped/AI-takeover. OWNERSHIP SPLIT (measured 2026-07-26, docs/state-flow.md): b1 ALIVE is SIM-OWNED -- it gates llm_strat_sim_step's master per-player loop (TEST at 0x43f580), llm_strat_order_release_due's pump (0x466577) and the whole AI tick chain, AND is read by llm_net_lockstep_commit_horizon (0x49c1ce) and llm_strat_bldg_compute_state_checksum (0x4a0125), so a b1 divergence is BOTH a pacing change and a lockstep checksum mismatch. b2/b3/b4 are NET-OWNED: llm_net_lockstep_dispatch (0x49c623+, x10), llm_net_player_remove (0x49dd6c) and llm_net_player_remove_timeout (0x49de8a) write them as one atomic triple AND 0xfb; OR 0x10; OR 0x08, and write NO other field of this struct. NOTE the b1 clear is CONDITIONAL, not part of the ordinary drop: llm_net_player_remove calls llm_strat_player_presence_lost (which clears b1 + downgrades SESSION_MODE 3->2) ONLY when llm_net_lockstep_count_active_players() < 2 -- the last-man-standing/game-over path. A 3+-survivor drop flips b2/b3/b4 only and leaves b1 intact, which is why the heavyweight sim consumers are untouched there. b3 also gates AI base-layout .DMP injection at llm_strat_bldg_completion_dispatch 0x479908; b2 feeds the game_speed product (llm_game_speed_recompute 0x49766d) that scales GAME_TIME_DELTA. Ordering: the RETAIL drop is safe because both triggers fire only while every peer is parked at the dead peer's frozen horizon (see llm_net_player_remove's plate); tracker/mp.yaml D4. |
| `+0x04` | `race` | `game_e_race` |  |
| `+0x08` | `color_index` | `int` | 0-7 -> RGB565 via llm_strat_player_set_color into DAT_00ae1948[player] |
| `+0x0c` | `flag_sprite_id` | `int` | ownership marker sprite = color_index + 0x453f |
| `+0x10` | `landing_x` | `int[32]` | per-planet landing-site tile X (from _G_LLM_STRAT_LANDING_SPOTS via llm_strat_set_landing_site; invasion fallback = camera pos) |
| `+0x90` | `landing_y` | `int[32]` |  |
| `+0x110` | `landing_spot_index` | `int[32]` | which landing spot was claimed; 2nd %02d in the AI base-layout script name init\{A,H}_%02d%02d.DMP |
| `+0x190` | `mother_established` | `int` | 1 once a MOTHER building completed on the current planet (gates the one-shot completion event + DMP script); reset on system advance; 0 triggers the build-your-mother hint |
| `+0x194` | `prod_queue_slot` | `int[32]` | production-queue slot (1..9) whose output is routed to this planet; bind llm_strat_prod_bind_planet, unbind on carrier death llm_strat_prod_unbind_planet |
| `+0x214` | `primary_mother_bldg` | `int[32]` | tracked primary mother BUILDING index (deployed form), 0 = none; re-elected by llm_strat_mother_reelect_primary; powers the 0x8a primary check |
| `+0x294` | `primary_mother_unit` | `int[32]` | tracked primary mother UNIT index (mobile heli-mother form); exact dual of primary_mother_bldg - one of the pair is nonzero while the mother lives |
| `+0x314` | `units_alive` | `int[32]` | per-planet live unit count; 0 with buildings_alive 0 -> llm_strat_player_presence_lost |
| `+0x394` | `buildings_alive` | `int[32]` | per-planet live building count |
| `+0x414` | `units_built_total` | `int[32]` | cumulative, never decremented (stats) |
| `+0x494` | `buildings_built_total` | `int[32]` | cumulative; >5 on planet>3 fires an AI event in CreateBuilding |
| `+0x514` | `units_lost_total` | `int[32]` | own units lost (strategic deaths + tactical results FUN_0044d81d) |
| `+0x594` | `buildings_lost_total` | `int[32]` | own buildings lost |
| `+0x614` | `units_killed_total` | `int[32]` | kill credit (attacker-indexed, with cfg Unit score value) |
| `+0x694` | `buildings_killed_total` | `int[32]` | kill credit (with cfg Building score value) |
| `+0x714` | `name` | `char[32]` | player name; init truncates >12 chars to 9+'...'; used in elimination message (text 0xa7) and diplomacy UI |
| `+0x734` | `prod_check_clock` | `double` | last production-progress check timestamp; sim_step steps it by 1.0s checking queue slots for status 200 |
| `+0x73c` | `side_id` | `int` | scenario/network side id (-1 campaign); llm_strat_player_by_side_id reverse-maps |

#### `llm_strat_pop_stats` (size 0x34, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `pop_total` | `int` | displayed population (rounded from pop_fraction) |
| `+0x04` | `workers_employed` | `int` | workers employed in buildings; invariant: total = employed + human + human_in_field |
| `+0x08` | `human_in_field` | `int` | crew aboard deployed units |
| `+0x0c` | `human` | `int` | idle (unemployed) population at base |
| `+0x10` | `housing_prev` | `int` | housing capacity latched at step start |
| `+0x14` | `housing_accum` | `int` | housing capacity accumulator (zeroed per sim step) |
| `+0x18` | `pop_fraction` | `double` | fractional population accumulator (growth/decay math) |
| `+0x20` | `subtick_a_clock` | `double` | sub-tick A clock |
| `+0x28` | `colony_hp_sum` | `int` | sum round(colony current HP) - growth modifier |
| `+0x2c` | `colony_hp_max_sum` | `int` | sum round(colony max HP) - growth modifier |
| `+0x30` | `layoff_cursor` | `int` | round-robin cursor for worker layoffs |

#### `llm_strat_storage_stats` (size 0x58, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `cap_accum` | `int[10]` | storage capacity accumulator per resource (zeroed per step) |
| `+0x28` | `cap_prev` | `int[10]` | storage capacity latched at step start |
| `+0x50` | `subtick_b_clock` | `double` | sub-tick B clock (storage overflow checks) |

#### `map_resources` (size 0x10, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `value` | `short[8]` |  |

#### `map_struct_fog_of_war` (size 0x90000, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `visible_by_count` | `byte[256][256][8]` |  |
| `+0x80000` | `discovered` | `byte[256][256]` |  |

#### `map_t_coord` (size 0x2, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `byte` |  |
| `+0x01` | `y` | `byte` |  |

#### `map_t_coord_r` (size 0x2, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `y` | `byte` |  |
| `+0x01` | `x` | `byte` |  |

#### `map_t_tile_coord` (size 0x3, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `byte` |  |
| `+0x01` | `y` | `byte` |  |
| `+0x02` | `len` | `byte` |  |

#### `map_t_tile_flags` (size 0x2, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `f1` | `byte` |  |
| `+0x01` | `f2` | `byte` |  |

#### `map_tile_object_data` (size 0x8, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `flags` | `map_u_tile_flags` |  |
| `+0x02` | `building` | `ushort` |  |
| `+0x04` | `unit` | `llm_map_tile_unit_slot` | Two incompatible readings of the same 2 bytes, by game mode -- see llm_map_tile_unit_slot. `._` = strategic unit_id/player (the default); `.tact` = the tactical two-layer sprite overlay; `.f` = the raw word. |
| `+0x06` | `class_owner` | `byte` | hi nibble = object class (0x10=tree/decoration, 0x40=building, 0x80=unit); lo nibble = owner/player id |
| `+0x07` | `visibility` | `byte` |  |

#### `map_unit_weapon` (size 0x13, category `/Manual/map`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `weapon_id` | `byte` | index into cfg::final::data::Weapon |
| `+0x01` | `enabled_2` | `bool` |  |
| `+0x02` | `enabled` | `bool` | Created by Rename Structure Field action |
| `+0x03` | `reload_timer` | `double` | countdown; on expiry ammo refilled from Weapon.ammo and ready flag set (llm_strat_unit_weapon_reload_tick) |
| `+0x0b` | `ammo` | `int` | Created by Rename Structure Field action |
| `+0x0f` | `pocket` | `int` | Created by Rename Structure Field action |

#### `map_object_building` (size 0x111, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `index` | `short` |  |
| `+0x02` | `building_id` | `cfg_t_building_index_s` |  |
| `+0x04` | `built_flags` | `byte` | Operational bitflags: bit 0x1 = connected/reached (flood-fill from Mother; llm_bldg_set_connected_flag), bit 0x2 = staffed/has-workers (llm_strat_bldg_set_staffed_flag). The ubiquitous '== 3' test means connected AND staffed = fully operational. |
| `+0x05` | `last_tick_time` | `double` | Game-clock timestamp of the last processed tick; _G_LLM_STRAT_TICK_BUDGET = GAME_CLOCK - last_tick_time, then refreshed. Set to GAME_CLOCK at creation. |
| `+0x0d` | `state` | `llm_strat_bldg_state` | Building state-machine index into _G_LLM_STRAT_BLDG_STATE_FUNCS (dispatched each tick while budget>0; also gates the DONE callback via state != 4). Init 100 (under-construction); 2 = destroyed. |
| `+0x0f` | `cycle_progress` | `double` | Generic per-state progress accumulator, reused by the active state handler (construction progress vs Building.build_time_2; mine extraction cycle; etc.); reset to 0 when a cycle restarts. |
| `+0x17` | `online_state` | `short` | Primary role: 'operational/ready' gate (nonzero required by storage-capacity/power/housing/unit-capacity accumulators and order dispatch). Overloaded per building family as a small sub-phase counter (1-4 dock/depart anim phases; last-produced unit-type id for production buildings). (medium-confidence overload.) |
| `+0x19` | `energy` | `double` | HP-like CHARGE stat (the CLAUDE.md ENERGY concept: units' hit points, buildings' construction-progress/charge climbing toward max during construction/repair) -- NOT the POWER resource. apply_damage subtracts pending_damage from it and fires death (state 2) at <=0; energy_refill_full maxes it from cfg Building.energy. The generated/consumed POWER economy is _G_LLM_STRAT_POWER_STATS (power_stats.generated/consumed), a SEPARATE concept. |
| `+0x21` | `pending_damage` | `double` | Accumulated pending damage; >0 triggers llm_strat_bldg_apply_damage, which subtracts it from energy and zeros it. |
| `+0x29` | `efficiency` | `double` | Output ratio 0.0-1.0 recomputed each tick by llm_strat_refresh_building from workers x (energy/max-energy) x player power ratio (PLANT skips the power factor since it produces power). Scales anim speed, construction/production rate, and power output. NOTE: this is a ratio, NOT the power resource itself. |
| `+0x31` | `current_workers` | `ushort` |  |
| `+0x33` | `anim_dur` | `double[12]` |  |
| `+0x93` | `anim` | `cfg_t_frame_index[12]` |  |
| `+0xc3` | `x` | `byte` | Created by Rename Structure Field action |
| `+0xc4` | `y` | `byte` | Created by Rename Structure Field action |
| `+0xc5` | `shuttle_slot` | `byte` | Production/shuttle cargo-slot link: 0 = unbound, 1-9 indexes _G_LLM_PROD_SHUTTLE_SLOTS[player*10 + slot] for reserved resources/passengers while an order is in flight. |
| `+0xc6` | `sub_id` | `byte` | Created by Rename Structure Field action |
| `+0xc7` | `pip_active_count` | `int` | Count of currently-lit auxiliary 'pip' sprite slots (damage/charge indicator), stepped one at a time toward round(pip_slot_count * (max_energy-energy)/max_energy). Gates whether the pip-anim tick (FUN_00479244) runs. See pip_level[4]/pip_frame[4]/pip_timer[4]. |
| `+0xcb` | `pip_frame` | `int[4]` | Per-pip-slot [4] current cfg::final::data::Anim frame index; advanced via the anim .next chain, re-seeded from a level->frame table on level change. The 4 slots are anonymous interchangeable pip instances (also the generic 'aux decoration/turret sprite' slots per llm_strat_render_tile_object). |
| `+0xdb` | `pip_timer` | `double[4]` | Per-pip-slot [4] last-update game-clock timestamp; elapsed = GAME_CLOCK - pip_timer[i] drives that slot's frame advance. Init GAME_CLOCK at creation. |
| `+0xfb` | `pip_level` | `int[4]` | Per-pip-slot [4] activation level (0 = inactive, 1-4 = stacked); a random slot is incremented/decremented each step toward a damage-fraction target. >0 gates the frame-advance. |
| `+0x10f` | `incoming_damage_tally` | `short` | Estimated committed damage from AI attack orders targeting this building (mirrors the unit record's incoming_threat_damage); read by llm_strat_ai_engage_select_and_commit to avoid overkill. |

#### `map_object_lab` (size 0x8, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `b_index` | `int` |  |
| `+0x04` | `active_project_id` | `int` | The project id this lab is currently researching, or the requested one while idle. llm_strat_order_queue_dispatch's Table-A start-project case (0x0046899b / 0x00468a26) compares it against order.args[7] to reject a duplicate request, then stores args[7] here when game_TryStartProject succeeds and flips the owning building to state 0x89. Indexed labs[player][building.sub_id]; row stride 0xc8 = 25 x 8. |

#### `map_object_mine` (size 0x38, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `b_index` | `int` | Roster-slot chain. SLOT 0 holds the running count of this player's active mine sub-objects; slots 1..31 hold the owning buildings[player][] index, 0 = free slot. int32. Incremented and assigned in map_CreateBuilding (0x004622cb, `INC dword ptr [EAX+0xd03480]` / `MOV dword ptr [EDX+0xd03480],EAX`), zeroed at init by map_FillDefaults (0x0045603e), cleared-then-decremented by llm_strat_bldg_unmap_footprint (0x0047b36b), free-slot-tested by llm_strat_bldg_instant_construct_find_slot (0x0046d229), and walked as count-then-index by llm_strat_bldg_sum_player_mines_resource / llm_strat_bldg_player_mine_extraction_total. Matches map_object_lab.b_index / map_object_production.b_index / unit_storage[].b_index. Renamed from the placeholder `f1` 2026-08-24 (finding 2026-08-24-0228-18). |
| `+0x04` | `deposit_slot` | `llm_mine_deposit_slot[4]` | The four resource-deposit bindings this mine works, assigned by llm_strat_mine_scan_deposit_slot and consumed by llm_strat_bldg_completion_dispatch's MINE_EXTRACTING arm (which walks them under _G_LLM_STRAT_BLDG_COMPLETION_SLOT_COUNT). Reached via the BUILDING record's .sub_id -- mines[player][sub_id], NOT indexed by building_id. 4 slots x 13 bytes exactly fills the former reserved_0x4[52]. Split 2026-08-22 (RI-SIM SIM-READY prep). |

#### `map_object_production` (size 0x195, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `b_index` | `int` |  |
| `+0x04` | `active_unit_type` | `byte` | The unit-type id this production building is currently BUILDING, or 0. Read back by llm_strat_bldg_prod_get_active_unit_type (0x00449f1c) and llm_strat_bldg_get_production_substatus (0x0044a362), both gated on buildings[..].state == PROD_WORKING, and compared against a queued order's unit-type argument in llm_strat_order_queue_dispatch. |
| `+0x05` | `queued_count` | `int[100]` | Per-unit-type queue depth, indexed by cfg Unit type id. SLOT 0 IS NOT A UNIT TYPE -- it is the RUNNING TOTAL, and the building-order handlers maintain it: order 0x6d adds args[1] to both queued_count[args[0]] (@0x004686b4) and queued_count[0] (@0x0046866f) and clamps that total to 0x32 (@0x004685e2-0x00468628), and order 0x6f decrements both (@0x0046881c / @0x00468848). Corrected 2026-08-23: this comment used to say slot 0 is UNUSED, which is true only of the AI's own scan and not of the array -- llm_strat_ai_is_worker_priority_candidate (0x004e3a77) scans 1..UNIT.total (G_UNIT_COUNT_TOTAL) for any nonzero as 'this production building still has demand'. Reads are register-indexed. |

#### `map_object_turret` (size 0x37, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `b_index` | `short` | Roster-slot chain, same role as map_object_mine.b_index and the lab/production siblings (slot 0 = active-turret count, slots 1..31 = owning buildings[player][] index, 0 = free) -- BUT genuinely int16, NOT int32 like every sibling. Every access is a word-sized op, read straight off the disassembly: map_FillDefaults `MOV word ptr [EAX+0xcc0fe0],0x0` (0x0045635a), map_CreateBuilding `INC word ptr` / `MOV word ptr [EDX+0xcc0fe0],AX` (0x004629ab-0x004629cf), llm_strat_bldg_unmap_footprint `MOV word` then `DEC word` (0x0047b6b3-0x0047b6d6); free-slot-tested by llm_bldg_construct_finalize (0x00462e66) and llm_strat_bldg_instant_construct_find_slot (0x0046d229). DO NOT widen it to int: the record is unpadded (size 55 = the exact sum of its fields) and aim_heading follows immediately at +0x2. Renamed from the placeholder `f1` 2026-08-24 (finding 2026-08-24-0228-18); the width was verified independently rather than copied from the mine. |
| `+0x02` | `aim_heading` | `int` | SIM1-G4 first slice (2026-08-22). The turret's own barrel-facing angle, quantised into 24 steps (1..24) -- read/written by both llm_strat_bldg_state_turret_scan (idle-scan cadence: increments while a threat/sight flag is set comparing to 0x19=25 wrapping to 1, decrements otherwise comparing to 0 wrapping to 0x18=24 -- MOV/CMP/reset sequence at 0x00471b66/0x00471b7f/0x00471b9b/0x00471ba7/0x00471bd3/0x00471bef) and llm_strat_bldg_state_turret_attack (rotates step-by-step toward llm_strat_dir_from_to's target bearing each tick-budget-consuming loop iteration, wrapping mod 0x18=24 in both directions at 0x00472239-0x00472265, stored back at 0x0047230a). Was inside the undifferentiated reserved_0x2[20] pad; split 2026-08-22 (RI-SIM SIM1-G4 first slice). |
| `+0x06` | `aim_heading_boot_scratch` | `int` | SIM1-G4 first slice (2026-08-22), gap fix. A real (non-padding) field: map_CreateBuilding's turret-creation arm (0x004629d6-0x00462a72) writes it to 1 alongside aim_heading(+0x2)/aim_step_dir(+0xa) (also both boot-seeded to 1) and acquire_retry_seed/acquire_retry_counter(+0xe/+0x12, both boot-seeded to 0x1e=30, cross-validating the turret_scan re-seed formula's own '+30'). No reader found anywhere in the SIM1-G4 batch (llm_strat_bldg_state_turret_scan/_attack never touch +0x6) or in map_CreateBuilding's own body beyond this write. Was silently absorbed into the old undifferentiated reserved_0x2[20]/then this session's own reserved_0x6[4] gap between aim_heading and aim_step_dir -- purpose unconfirmed, but it is definitely NOT padding. |
| `+0x0a` | `aim_step_dir` | `int` | SIM1-G4 first slice (2026-08-22). The last rotation direction sign (+1/-1) turret_attack's aim-tracking loop converged on, stored at 0x00472307-0x0047230a; llm_strat_bldg_state_turret_scan negates it (IMUL EDX,[+0xa],-1 @0x00471cfc-0x00471d16) when re-arming after a failed target acquire, so the next idle scan sweeps the opposite way. Was inside reserved_0x2[20]; split 2026-08-22 (RI-SIM SIM1-G4 first slice). |
| `+0x0e` | `acquire_retry_seed` | `int` | SIM1-G4 first slice (2026-08-22). A persistent scratch scalar whose only writer found in this batch is its own self-referential update in llm_strat_bldg_state_turret_scan ((old % 40) + 30, read then written back at 0x00471d34-0x00471d5b) and which is copied into acquire_retry_counter (+0x12) on the same failed-acquire path (0x00471d7a). No writer/reader outside this pair of functions found; the value a fresh turret record boots with is unconfirmed. Was inside reserved_0x2[20]; split 2026-08-22 (RI-SIM SIM1-G4 first slice). |
| `+0x12` | `acquire_retry_counter` | `int` | SIM1-G4 first slice (2026-08-22). Target-acquire retry throttle: decremented every idle-scan tick (0x00471c0c), divided by 5 with the remainder gating a periodic widen-and-retry (0x00471c2a-0x00471c3d), and refreshed from acquire_retry_seed (+0xe) whenever a fresh target acquire fails (0x00471d7a). Was inside reserved_0x2[20]; split 2026-08-22 (RI-SIM SIM1-G4 first slice). |
| `+0x16` | `counter_ref` | `ushort` | Packed owner\|kind ref (target_ref encoding) of whoever/whatever this turret slot is tracking as its own counter-target. Read by llm_strat_ai_group_classify_target_object's BUILDING arm as the turret's own counter-attacker identity. |
| `+0x18` | `counter_target_slot` | `int` | 0 = not engaging; nonzero = engaging. The asm tests it as a dword != 0 but only the LOW 16 BITS are read back out elsewhere (llm_strat_ai_group_classify_target_object). |
| `+0x1c` | `cached_sight` | `uint` | Sight radius copied once from cfg Building.sight (MOVZX from Building[building_id]+0x260 @0x004629dd, stored @0x004629f4) by map_CreateBuilding's turret arm. NO READER FOUND in the sim migration set; that is NOT a claim of deadness -- Ghidra does not track register+displacement effective addresses as data xrefs (docs/dead-ends.md trap A), so an indexed reader would be invisible. Split 2026-08-22 (RI-SIM SIM-READY prep); was reserved_0x1c[4]. |
| `+0x20` | `attack_range` | `int` | The turret's engagement radius in TILES. llm_strat_order_queue_dispatch's building-table turret arm (0x0046782c) loads it and passes it to llm_strat_dist_out_of_range as the threshold for the building->target tile distance, rejecting the order when the target is outside it. Read at turrets[player][building.sub_id]; 0x20 confirmed as 0xcc1000 - 0xcc0fe0 against the array base. |
| `+0x24` | `weapon_id` | `byte` | SIM1B (2026-08-12; building_tick machinery slice). Index into cfg Weapon[] this turret slot mounts -- llm_strat_bldg_turret_reload_tick reads Weapon[weapon_id].ammo to refill on reload expiry. Was inside the undifferentiated 0x24..0x36 pad; split out because the decompiler was already resolving it as a distinct byte access (0x0047c63b). |
| `+0x25` | `resupply_available` | `byte` | Set to 1 beside reload_ready_flag when the turret is armed (weapon_id != 0) at creation (map_CreateBuilding @0x00462ab9); cleared to 0 by llm_strat_turret_fire @0x0047c0f1 when resupply_cycles_remaining (+0x33) is decremented to exactly 0. What re-arms it is NOT established -- the name is a reading of the two writes, carried at MEDIUM confidence. Split 2026-08-22 (RI-SIM SIM-READY prep); was reserved_0x25[1]. |
| `+0x26` | `reload_ready_flag` | `byte` | SIM1B (2026-08-12). 0 = actively counting down reload_timer; set to 1 once reload_timer expires and ammo is refilled (llm_strat_bldg_turret_reload_tick), which also gates the whole function -- a tick where this is already 1 skips the countdown/refill entirely. Cleared elsewhere (outside this batch) when the turret fires and depletes ammo. |
| `+0x27` | `reload_timer` | `double` | SIM1B (2026-08-12). Countdown to the next reload/ammo-refill, decremented by elapsed dt each tick (llm_strat_bldg_turret_reload_tick); on reaching <=0.0 the turret's ammo is refilled from Weapon[weapon_id].ammo (only if ammo was exactly 0), the timer is reset to 0.0, and reload_ready_flag is set. Ghidra's decompiler had been mis-splitting this 8-byte double's second dword (the zero-store idiom at 0x0047c704/0x0047c70e) as a separate field_0x2b -- confirmed a single FP field from the .asm, not two. |
| `+0x2f` | `ammo` | `int` | SIM1B (2026-08-12). Current ammo count for this turret slot. llm_strat_bldg_turret_reload_tick only refills it from Weapon[weapon_id].ammo when it reads exactly 0 -- consumed elsewhere (outside this batch) when the turret fires. |
| `+0x33` | `resupply_cycles_remaining` | `int` | Countdown of full-reload/resupply cycles left once ammo empties, in llm_strat_turret_fire: compared against -1 @0x0047c084 (which skips the decrement, i.e. -1 reads as unlimited), decremented @0x0047c09d, and compared against 0 @0x0047c0b3 to clear resupply_available. NO initialiser found in this batch, so the -1-means-unlimited reading is a LEAD from the consumer, not a proven boot contract. Split 2026-08-22 (RI-SIM SIM-READY prep); was reserved_0x33[4]. |

#### `map_object_unit` (size 0xe9, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `unit_above` | `map_t_unit_full_id` |  |
| `+0x02` | `unit_proto_id` | `cfg_t_unit_index_s` |  |
| `+0x04` | `order` | `llm_strat_unit_state` | Pending/queued state, committed to `state` once the current move/transition completes; written together with `state` by llm_strat_unit_set_state_order. |
| `+0x06` | `state` | `llm_strat_unit_state` | Current unit state-machine state; dispatch index into _G_LLM_STRAT_UNIT_STATE_FUNCS. llm_strat_unit_set_state writes only this. |
| `+0x08` | `activity_clock` | `double` | Game-clock timestamp of this unit's last per-tick budget sample (_G_LLM_STRAT_TICK_BUDGET = GAME_CLOCK - activity_clock), refreshed to GAME_CLOCK each tick. |
| `+0x10` | `rotation_clock` | `double` | Game-clock timestamp of the last facing/turn tick; diffed against GAME_CLOCK and gated by cfg Unit.turn_speed to budget facing steps. |
| `+0x18` | `energy` | `double` | Unit's HP (the CLAUDE.md ENERGY concept = hit points), NOT the POWER resource. Maxed from cfg Unit.energy by energy_refill_full's unit arm. |
| `+0x20` | `pending_damage` | `double` | Accumulated pending damage; subtracted from energy and zeroed by llm_strat_unit_apply_damage (which fires death -> state 2 when energy<=0). |
| `+0x28` | `experience` | `int` | Accumulated combat experience / veterancy (signed). Earned per kill (cfg kill_score), summed on squad merge, split among soldiers on troop unload; reduces weapon scatter (feeds llm_strat_weapon_scatter_offset as an accuracy divisor). NOT a position snapshot -- the cargo-manifest code just byte-copies this same int32. |
| `+0x2c` | `facing_target` | `byte` | Target facing (24-direction compass) the unit is rotating toward; facing_current chases it one step per rotation_clock tick. |
| `+0x2d` | `facing_current` | `byte` | Actual current facing (24-direction); stepped toward facing_target each rotation tick, drives soldier heading. |
| `+0x2e` | `order_queued` | `byte` | Set to 1 when llm_strat_order_queue_dispatch defers/requeues a player order for this unit; cleared to 0 once the order is applied. Checked by storage/exit code to avoid scrapping a unit with an order still pending. (semantics medium-confidence -- verify vs the enter_wait states if relied upon.) |
| `+0x2f` | `ctrl_group_id` | `byte` | Assigned Ctrl+digit control-group index (0 = none). Distinct from move_group_id(+0xc6) and ai_group_index(+0xd8) -- three separate group concepts. |
| `+0x30` | `home_storage_slot` | `byte` | Index into map::g::unit_storage[player][] -- the storage/dock slot this unit is docked in or launched from. |
| `+0x31` | `shuttle_slot` | `byte` | Index into _G_LLM_PROD_SHUTTLE_SLOTS[player*10 + shuttle_slot] -- the interplanetary-transfer/production slot this unit occupies as a shuttle/cargo carrier (the slot record holds dest/origin planet, not this field). |
| `+0x32` | `elevation` | `int` | Created by Rename Structure Field action |
| `+0x36` | `selected_weapon` | `byte` | Currently-selected weapon slot (0-3 index into weapons[4]); 100 (0x64) sentinel = no weapon selected. |
| `+0x37` | `weapons` | `map_unit_weapon[4]` |  |
| `+0x84` | `x` | `byte` | Created by Rename Structure Field action |
| `+0x85` | `y` | `byte` | Created by Rename Structure Field action |
| `+0x86` | `goal_x` | `byte` | Move-order destination tile X (the game's own tooltip labels the 0x86/0x87 pair 'X2 Y2'). |
| `+0x87` | `goal_y` | `byte` | Move-order destination tile Y. |
| `+0x88` | `home_x` | `byte` | Return-to/rally tile X, snapshotted from current x when an attack-move order issues; state 0x1b (attack-then-return) sends the unit back here. |
| `+0x89` | `home_y` | `byte` | Return-to/rally tile Y. |
| `+0x8a` | `target_index` | `short` | PRIMARY target roster/building slot index (used directly as (ushort) array subscript). Paired with target_ref at +0x8c. |
| `+0x8c` | `target_ref` | `short` | PRIMARY target owner\|kind ref (low byte): low nibble = owner player, 0x40 = building, 0x80/0x20 (checked as &0xa0) = unit; 0 = no target. |
| `+0x8e` | `target_fine_x` | `fine_coord` | PRIMARY target fine (sub-tile) X coordinate; >>5 gives the tile X. |
| `+0x92` | `target_fine_y` | `fine_coord` | PRIMARY target fine (sub-tile) Y coordinate. |
| `+0x96` | `target2_index` | `short` | SECONDARY target slot index (same semantics as target_index); a second concurrent target checked alongside the primary. |
| `+0x98` | `target2_ref` | `short` | SECONDARY target owner\|kind ref (same bit layout as target_ref); 0 = no secondary target. |
| `+0x9a` | `target2_fine_x` | `fine_coord` | SECONDARY target fine X coordinate (>>5 = tile). |
| `+0x9e` | `target2_fine_y` | `fine_coord` | SECONDARY target fine Y coordinate. |
| `+0xa2` | `move_microstep` | `int` | Sub-tile movement micro-step counter 0..0x1f indexing _G_LLM_STRAT_MOVE_MICROSTEPS (counts down as the unit slides into a tile). Overloaded: while a corpse (state corpse_fow_decay) it is a decrementing saved sight radius for FoW cleanup, not a movement index. |
| `+0xa6` | `path_cursor` | `int` | Current step index into this unit's active path buffer (ARRAY_00aee8c0, 2 bytes/step); reset to 0 when a new path attaches. |
| `+0xaa` | `path_slot_id` | `byte` | Index (0-99) of this unit's path buffer among the player's 100 slots; 0xff = no path assigned. |
| `+0xab` | `path_blocked_retry_count` | `byte` | Consecutive blocked-move retry counter; compared < 4 to decide retry vs re-route. Reset on new order/path. |
| `+0xac` | `move_heading` | `byte` | Current compass-direction index; seeds initial facing at spawn and indexes per-direction dx/dy tables during formation marching. |
| `+0xad` | `origin_tile_was_passable` | `byte` | Cached map::g::passable[x][y] at placement time; restored to the passable grid when the unit leaves the tile (dies/boards). |
| `+0xae` | `move_step_speed_scale` | `double` | Multiplier on cfg step_speed for tile-to-tile movement interpolation; initialized to (double)origin_tile_was_passable (0.0 stalls if spawned on a blocked tile), default 1.0. |
| `+0xb6` | `dmg_smoke_level` | `int` | Damage-smoke overlay tier (0 = none); index derived from fraction of energy lost, selects the starting smoke-anim id. Gates the smoke render/tick. |
| `+0xba` | `dmg_smoke_anim_timer` | `double` | Countdown until the damage-smoke overlay advances a frame; decremented by GAME_CLOCK delta each tick. |
| `+0xc2` | `dmg_smoke_anim_id` | `int` | Current cfg::final::data::Anim index for the damage-smoke overlay; advanced via .next, re-seeded from dmg_smoke_level when the chain ends. |
| `+0xc6` | `move_group_id` | `int` | Formation/marshalling 'move-together' group tag from move-order arg[0xc]; group_marshal matches units with same destination+order+group. Distinct from ctrl_group_id(+0x2f) and ai_group_index(+0xd8). |
| `+0xd0` | `passive_engage_target_index` | `ushort` | Slot index of the opportunistic/passive auto-fire target; paired with passive_engage_target_ref at +0xd2 (this pair is ordered index-then-ref). |
| `+0xd2` | `passive_engage_target_ref` | `ushort` | Packed owner\|kind ref (same convention as target_ref) for the passive-engage target; paired with the index at +0xd0. |
| `+0xd4` | `ai_group_next` | `ushort` | Next unit index in this player's AI-group doubly-linked member list (0 = tail). Links via game::g::player_data[].ai_groups[]. |
| `+0xd6` | `ai_group_prev` | `ushort` | Previous unit index in the AI-group member list (0 = head). |
| `+0xd8` | `ai_group_index` | `ushort` | Which AI tactical group (player_data[player].ai_groups[], 0..0x1f -- ai_group_count caps at 0x20) this unit belongs to; 0xffff = none. A third group concept, distinct from ctrl_group_id (+0x2f) and move_group_id. WRITTEN by straight assignment only: llm_strat_ai_group_member_link @0x004d4a4e (stores the group index), llm_strat_ai_group_remove @0x004d4f98, llm_strat_ai_group_member_unlink @0x004d49fc (0xffff), llm_strat_ai_notify_unit_lifecycle @0x004dbbae / @0x004dbbce / @0x004dbd5b, and cleared to 0 by four sim teardown writers (llm_strat_unit_remove_from_map @0x004874d8, llm_strat_unit_teardown_mapped @0x004876d3, llm_strat_unit_on_destroyed @0x004879b8, llm_strat_unit_teardown @0x00487d61). READ as a plain index/id by llm_strat_unit_get_ai_group_index, llm_strat_ai_unit_in_group, llm_strat_ai_route_unit_to_home_storage, llm_strat_ai_group_classify_target_object, llm_strat_unit_passive_engage_tick and llm_strat_ai_target_list_add/_invalidate_by_id. Every AI writer is gated on player_data[p].ai_enabled, so all of them are no-ops for a human player. KNOWN ALIASING HAZARD (2026-08-29, finding 2026-08-28-2315-1; confirmed by disassembly, NOT observed live): the SAME word is also read-modify-written bit-by-bit by llm_unit_status_bit_set @0x0044b0dd (`OR word ptr [EDX + 0xdd8d20],AX`) and llm_unit_status_bit_clear @0x0044b132 (`AND ...,AX`) -- the only two RMW sites on this field and the only ones whose operand is a variable mask. Those are not AI code: they are order codes 0x34 ORDER_SYS_UNIT_STATUS_BIT_SET / 0x35 ORDER_SYS_UNIT_STATUS_BIT_CLEAR dispatched out of llm_strat_order_queue_dispatch, enqueued only by llm_strat_order_ctrlgrp_select_member @0x0046f1de and llm_strat_order_ctrlgrp_flash_member @0x0046f278 -- i.e. the player's Ctrl+digit CONTROL-GROUP feature (llm_strat_unit_ctrlgroup_add_member /_remove_member/_leave, llm_unit_ctrl_group_activate/_flash_observer, llm_strat_selection_drag_finish, llm_ui_hud_unassigned_unit_row_click_handler). bit_index is the control-group number 0..9, the same value stored in ctrl_group_id at +0x2f. THERE IS NO INDEX-BITS / FLAG-BITS SPLIT: AI indices occupy bits 0-4 and the ctrl-group range 0..9 covers all of them, so every possible index value collides, and a sweep of all 25 other accesses of this word (17 through the named field, 8 through the raw 0xdd8d20 + player*0x5b04 + unit*0xe9 form) found every one treating it as a plain scalar and NONE reading the bits back as flags -- the two writers are write-only. So a collision's only observable effect is silent corruption of the AI index. Practically inert today because the AI writers require ai_enabled while the ctrl-group path always targets PlayerSide; the one path that could make it reachable is ORDER_SYS_PLAYER_SET_AI_HUMAN (0xf5) handing a slot between AI and human while a unit still carries a live index -- NOT traced. A reimplementation must PRESERVE the raw unmasked OR/AND across the whole word and must NOT give the two features separate storage. |
| `+0xdc` | `incoming_threat_damage` | `short` | Estimated committed damage from all attackers locked onto this unit; read by AI target selection to avoid overkill. += attacker.committed_weapon_damage_est on commit, -= on release. |
| `+0xde` | `committed_weapon_damage_est` | `short` | This unit's cached weapon-damage estimate at attack-commit time (from llm_strat_unit_estimate_weapon_damage); the value added to/subtracted from the target's incoming_threat_damage. |
| `+0xe2` | `engagement_flags` | `byte` | Combat-engagement flags; bit 0x1 = has an active attack commitment. (medium-confidence; written jointly with order_status_flags via a compiler-merged 16-bit store.) |
| `+0xe3` | `order_status_flags` | `byte` | Status flags; bit 0x40 = order-notify pending (set/cleared by llm_strat_unit_notify_status), bit 0x80 mirrors the engagement-committed flag. (medium-confidence.) |
| `+0xe4` | `ai_contact_ref` | `ushort` | Packed owner\|kind ref to a spotted enemy contact cached for AI-group tactical logic; paired with ai_contact_index at +0xe6. NOTE: this pair is ordered ref-then-index (opposite of the +0xd0/+0xd2 pair) -- verified against the actual arg order of llm_strat_ai_target_ref_is_alive(ref,index). |
| `+0xe6` | `ai_contact_index` | `ushort` | Slot index paired with ai_contact_ref at +0xe4 (see the ref-then-index ordering note there). |
| `+0xe8` | `order_notify_status` | `byte` | Last order/notification outcome status code (observed 0,1,2,3,5,6); set by llm_strat_unit_notify_status and AI move/flag helpers. Drives AI bookkeeping and likely UI order-feedback. (label medium-confidence.) |

#### `map_object_unit_storage` (size 0xf4, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `b_index` | `int` |  |
| `+0x04` | `docked_count` | `int` | number of units currently in docked_units[] |
| `+0x08` | `docked_units` | `int[50]` | embedded docked-unit index list [50]; unit indices (<500). int[50]->short[100] repack target |
| `+0xd0` | `occupancy` | `int` | occupancy weight: soldiers weighted by soldier_count, vehicles=1; gated <50 |
| `+0xd4` | `door_mutex_unit` | `int` | storage door mutex: unit idx holding the door, or 0 |
| `+0xd8` | `door_waiter_count` | `int` | count of units waiting on the door |
| `+0xdc` | `park_x` | `int` | Building park-position tile X. WIDENED byte -> int on 2026-08-22: the original moves all four bytes in ONE 32-bit access, so the old byte typing was a width defect and the three bytes after it were never padding. Writer map_CreateBuilding @0x0046290f (MOV dword ptr [EAX+0xc7289c],EDX; value (x_b + Building[id].park_offset_x) & general.width_mask, AND @0x004628f6); readers llm_strat_unit_state_enter_walk_in @0x00480418 and llm_strat_storage_place_exit_ground @0x0048a011, both MOV r32,dword ptr. The .c's (char)(v>>8)/(v>>0x10)/(v>>0x18) byte-splatter rendering was a decompiler artifact of the too-narrow type, not three stores. This widening is what retires the 'park_x/park_y READ AS DWORD' hazard recorded in sim_storage_dock.h. |
| `+0xe0` | `park_y` | `int` | Building park-position tile Y; paired with park_x and widened byte -> int the same way and for the same reason. Writer map_CreateBuilding @0x0046293e (MOV dword ptr [EAX+0xc728a0],EBX; value (y_b + Building[id].park_offset_y) & general.height_mask, AND @0x00462925); readers llm_strat_unit_state_enter_walk_in @0x004803f9 and llm_strat_storage_place_exit_ground @0x00489ff5. |
| `+0xe4` | `exit_tile_x` | `int` | tile the unit appears at when it leaves this storage: llm_strat_tile_is_storage_door_exit compares an arriving unit's tile against this pair; written once by map_CreateBuilding, read as the default exit target by llm_strat_unit_order_exit_storage(_enqueue)/_auto and llm_strat_unit_order_auto_launch_from_storage(_enqueue) |
| `+0xe8` | `exit_tile_y` | `int` | see exit_tile_x |
| `+0xec` | `reserved_0xec` | `byte[8]` | undefined padding 0xec-0xf3 |

#### `map_t_unit_full_id` (size 0x2, category `/Manual/map/object`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `unit_id` | `ushort:12` |  |
| `+0x01` | `player` | `ushort:4` |  |

#### `llm_strat_ai_unit_group` (size 0xa66, category `/Manual/unknown`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `serial_id` | `int` | from player_data next_group_serial |
| `+0x04` | `member_count` | `short` |  |
| `+0x06` | `reinforce_pending` | `ushort` | PENDING REINFORCEMENT DELTA -- units this group has lost (or been promised) that have not yet been folded into its live recruit request. Distinct from member_count (+4, the live roster) and active_member_count (+8). Mechanics, all addresses EN: ZEROED at group create (llm_strat_ai_group_create @0x004d4e06), at group remove (llm_strat_ai_group_remove @0x004d5025) and by the disband task (llm_strat_ai_group_task_disband @0x004eaad0). INCREMENTED by llm_strat_ai_notify_object_removed @0x004db8d9, immediately after llm_strat_ai_group_member_unlink has DECREMENTED member_count @0x004d4a22 -- i.e. one tick per member lost. ADDED to by llm_strat_ai_group_redistribute_units @0x004e6bcf (a clamped unit count). CONSUMED read-then-zero by the three pool handlers, each of which folds it into active_sub_code (+0x2d, task slot 0's sub_code) and clears it in the same three instructions: llm_strat_ai_group_task_muster_from_pool @0x004ea49e-0x004ea4ac, _recruit_from_pool3 @0x004ea610-0x004ea61e, _recruit_from_pool4 @0x004ea6d3-0x004ea6e1. Read (not cleared) by llm_strat_ai_group_task_step @0x004e974a, which sums it with active_sub_code and, when the sum is non-zero, clears active_flag so the task re-activates. Read UNSIGNED (MOVZX @0x004e6b8f / 0x004e974a). The name is an inference from that flush-into-sub_code protocol, not from a shipped string; the mechanics above are what is proven. Named 2026-08-05 (RI-AI AI1C); previously an unnamed 2-byte hole. |
| `+0x08` | `active_member_count` | `ushort` | operationally-active members (excludes boarded/stored); adjusted by llm_strat_ai_group_member_count_adjust |
| `+0x0a` | `head_unit` | `ushort` | intrusive doubly-linked member list via map::units[].f1/.f2; unit group word +4 = 0xffff when removed |
| `+0x0c` | `tail_unit` | `ushort` |  |
| `+0x0e` | `current_param` | `int` | copied from pending_param on task activation |
| `+0x12` | `task_queue_count` | `short` | # task records in use incl active slot 0; cap 0x40(64); 0 = idle group |
| `+0x14` | `goal` | `short` | static mission-type tag set once at formation (seen 3,4,5,8,10,0xb); scanned to census/rate-limit groups; distinct from the live task_code at +0x2a |
| `+0x16` | `reserved_0x16` | `ushort` | RESERVED / role unknown, and PROVEN UNREAD image-wide (2026-08-29, R10 readiness drain). The only access anywhere in the image is a single unconditional 16-bit zero-store at group allocation: llm_strat_ai_group_create @0x004d4e3c `MOV word ptr [EAX + 0xe7e43e],0x0`, reached on every successful allocation (the pool-full arm returns -1 at 0x004d4db9 before it). THREE instruments, and the negative is only as good as its controls: (1) image-wide instruction scan of all 252238 instructions for the folded disp32 0x00e7e43e -> 1 hit, the store -- VALIDATED by running the same scan on the neighbours, +0x18 link_target_group (0xe7e440) 9 hits and +0x14 goal (0xe7e43c) 39 hits, so the scan demonstrably sees raw-offset reads of this struct; (2) search-decompilation program-wide for field_0x16/field_0x17 -> the same store only (a second textual hit is map::object::turret's unrelated field of the same auto-name); (3) grep of the freshly exported 210-function AI cluster. No allocator bypasses it: llm_strat_ai_group_split_off_create @0x004e68dd allocates through group_create and then writes only link_target_group/goal/active_member_count, and llm_strat_ai_group_split_half @0x004d4e6b is dead. llm_strat_ai_group_remove's swap-compaction moves these bytes inside a whole-struct copy without inspecting them. So the word is 0 for every group's entire lifetime and a reimplementation need only zero it. NOT called padding: it receives a DEDICATED zero-store, which Watcom alignment padding does not -- 'reserved but never wired' fits the evidence at least as well, and neither can be told from the other without a consumer. The name keeps the offset because there is nothing else honest to call it; the R10 blocker it leaves on llm_strat_ai_group_create is acknowledged in tools/data/ai_ready_ack.json rather than dressed up as a resolved name. |
| `+0x18` | `link_target_group` | `ushort` | cross-group reference (valid for certain goal/link values); fixed up on group-index compaction by FUN_004d4a16 |
| `+0x1a` | `target_player_id` | `int` | Opponent player index this group's attack mission targets (set together with goal 3 / 0xb; the weakest-known-opponent pick). WIDENED from ushort to int on 2026-08-01 (AI1A layer 2): all THREE writers store a full DWORD -- llm_strat_ai_army_milestone_advance_or_attack @0x004e75d1, llm_strat_ai_group_expansion_form_or_repurpose @0x004e7899 and llm_strat_ai_invasion_launch_attack_group @0x004e8b30, each `MOV dword ptr [reg+0xe7e442],reg` with no 0x66 prefix -- so the two bytes that used to be _pad_0x1c are this field's upper half, not padding. Corroborated: a whole-image raw-pointer scan finds 0x00e7e444 (the old pad) a TOTAL-ORPHAN, and llm_strat_ai_group_create zeroes only up to +0x16, so nothing else can be relying on those bytes. Caught by the reimpl-verify review of the army-milestone translation, which wrote 16 bits where the original writes 32 -- inside a shadow-COMPARED region. |
| `+0x1e` | `resolved_target_ref` | `int` | cached owner/type-encoded target ref once validated alive (low nibble=owner, 0xa0=unit-vs-bldg marker) |
| `+0x22` | `resolved_target_index` | `int` | cached target building/unit index, paired with resolved_target_ref |
| `+0x26` | `pending_param` | `int` | task parameter (target id?) |
| `+0x2a` | `task_code` | `short` | live active-task code 2..0x18; activation dispatch llm_strat_ai_group_task_activate, per-tick step llm_strat_ai_group_task_step. These flat fields (+0x26..+0x4e) are slot 0 of task_queue |
| `+0x2c` | `active_flag` | `byte` | set 1 on activation |
| `+0x2d` | `active_sub_code` | `short` | record[0].sub_code (see llm_strat_ai_group_task) |
| `+0x2f` | `active_param_a` | `int` | record[0].param_a: anchor X or target owner/type ref (-1 = centroid) |
| `+0x33` | `active_param_b` | `int` | record[0].param_b: anchor Y or target index |
| `+0x37` | `active_param_c` | `int` | record[0].param_c: secondary anchor X |
| `+0x3b` | `active_param_d` | `int` | record[0].param_d: secondary anchor Y |
| `+0x3f` | `active_queued_time` | `double` | record[0].queued_time |
| `+0x47` | `task_start_time` | `double` | AI clock (player_data+0x1003c) at activation |
| `+0x4f` | `task_queue_backlog` | `llm_strat_ai_group_task[63]` | queued task records [1..63]; slot 0 is the flat active-task fields at +0x26. FIFO: enqueue-back llm_strat_ai_group_task_enqueue, preempt-front _preempt, pop-front _dequeue |

#### `llm_strat_move_microstep` (size 0x3, category `/Manual/unknown`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x_off` | `byte` | pixel X offset inside the 32px tile (final = tile*0x20 + x_off) |
| `+0x01` | `y_off` | `byte` |  |
| `+0x02` | `facing` | `byte` | heading 1..0x18, interpolated with wraparound; doubles as rotation sprite frame |

#### `llm_strat_order` (size 0x44, category `/Manual/unknown`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `exec_time` | `double` | scheduled execution game-clock; clamped to the lockstep horizon by llm_strat_order_schedule; release when <= now (llm_strat_order_release_due; dedup: same unit+owner keeps earlier time). Stale on direct local enqueues. |
| `+0x08` | `unit_index` | `ushort` | into map::units[player] (or building index for kind 0x40) |
| `+0x0a` | `owner_and_kind` | `ushort` | low nibble = player; 0x20/0x80 = unit target, 0x40 = building, 0xf0 = global event, else jumptable on order code |
| `+0x0c` | `param0` | `short` | buildings: the order itself (0x6a..0xea -> building state +0xd); units: facing/secondary param (FUN_0048671b); kind 0xf0: event id |
| `+0x0e` | `order_code` | `E_ORDER_CODE` | E_ORDER_CODE. The verb this record carries. TWO FAMILIES IN ONE DOMAIN, and which one applies is decided by owner_and_kind's KIND nibble, not by this field: for the unit kinds (0x20/0x80) it is a unit STATE id fed to llm_strat_unit_set_state_of, and for every other kind it is one of the ORDER_SYS_* admin verbs that llm_strat_order_queue_dispatch's second switch decodes (order_code - 0x34, REPNE SCASB over _G_LLM_STRAT_ORDER_ADMIN_SWITCH_SCAN). The enum holds both families, which is why it is applied here and E_BLDG_ORDER_PARAM0 is deliberately NOT applied to param0 -- that one covers only the kind-0x40 reading of a field that is polymorphic the same way. |
| `+0x10` | `args` | `int[13]` | -1 = leave unchanged; [0]/[1]->goal x/y +0x86/87, [2]->home slot +0x30, [3]/[4]->target fine +0x8e/+0x92, [5]/[6]->primary target idx/kind +0x8c/+0x8a, [7]->weapon +0x36, [8]/[9]->+0x9a/+0x9e, [10]/[11]->secondary target +0x98/+0x96, [12]->group +0xc6 |

#### `util_struct_file_handle` (size 0x22, category `/Manual/util`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x0c` | `-` | `uint` |  |
| `+0x10` | `-` | `undefined4` |  |

#### `llm_DUP_DELETE_ME_tact_unit_record_2026_08_24` (size 0x5f4, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x23` | `cmd_wait_until_time` | `double` | Absolute time_GetCurrentTime() deadline for a pending timed-WAIT command (queue op 8). Set to now + (double)cmd_queue[cmd_index].arg0 at 0x0042f708-0x0042f71d, and the dispatch loop keeps re-issuing the wait -- skipping the op switch entirely -- while now is below it (FLDZ/FCOMP-vs-0 guard at 0x0042f599, FCOMP-vs-now at 0x0042f5b2), all in llm_tact_unit_weapons_tick. Zeroed by two back-to-back 32-bit MOV-0 stores -- the compiler's zero-a-double idiom, NOT two fields -- when a MOVE (op 1) is dispatched (0x0042f767 + 0x0042f771) and at spawn (0x0042babd + 0x0042bac7). This subsumes what the decompiler rendered as field_0x23 and field_0x27; the FSTP at 0x0042f71d proves the 8-byte width. |
| `+0x5d4` | `attack_interrupt_flag` | `byte` | interrupt_flag of the immediate ATTACK/AIM command record at +0x5d4 (the same 0xb-byte shape as mh_llm_tact_unit_cmd_entry; its op is attack_cmd_op at +0x5d5). Set from llm_tact_unit_enqueue_command's interrupt_flag parameter at 0x0042b79f on the op==2 short-circuit. LEAD, not settled: no reader was found -- the role is inferred from the position/shape parallel with the FACE record's face_interrupt_flag, which DOES have one. |
| `+0x5d7` | `attack_gun_toggle` | `ushort` | arg0 of the immediate ATTACK/AIM record. Passed as fire_arg to llm_tact_unit_fire_weapon (load 0x0042f401, call 0x0042f40e), which XORs it with active_gun at 0x00430925 to pick the firing gun -- so 0 fires the currently active gun and 1 fires the other. Written from the enqueue's arg0 at 0x0042b7bf; zeroed at spawn (0x0042bd5f). |
| `+0x5d9` | `attack_cmd_arg1` | `ushort` | arg1 slot of the immediate ATTACK/AIM record, written wholesale with the rest of it (0x0042b7d0) and zeroed at spawn (0x0042bd6f). NO READER FOUND anywhere in the exported tactical set: it exists because the record shares the 0xb-byte cmd_entry shape and the enqueue fills every arg slot. Scaffolding name -- a LEAD, not a reading. |
| `+0x5df` | `face_interrupt_flag` | `byte` | interrupt_flag of the immediate FACE/TURN command record at +0x5df (0xb-byte cmd_entry shape; its op is face_cmd_op at +0x5e0). Set from the enqueue's interrupt_flag parameter at 0x0042b72c, and READ as part of the re-entry guard that refuses to re-issue a FACE onto an already-armed slot (CMP at 0x0042b6f3, paired with the face_cmd_op==6 test at 0x0042b6e0). |
| `+0x5e0` | `face_cmd_op` | `ushort` | op of the immediate FACE/TURN record: 6 = turn pending, 0 = free/consumed. Armed to 6 by llm_tact_unit_enqueue_command's op==6 short-circuit (0x0042b739) and by the op-7 turn-then-attack path in llm_tact_unit_weapons_tick (0x0042f477). Cleared to 0 by llm_tact_unit_rotate_tick once facing_dir reaches face_cmd_target_dir, or immediately if status bit 0x8 (FIRE) is set (0x004307f9, 0x00430843), and by the op 0x46 CLEAR in both enqueue_command (0x0042b64b) and llm_tact_group_issue_order (0x0042b258). Read as the 'is it turning' gate before calling rotate_tick (0x0042f4e4 -> call 0x0042f504). |
| `+0x5e2` | `face_cmd_target_dir` | `ushort` | arg0 of the immediate FACE/TURN record: the target dir24 heading (1..0x18) to rotate to. Written from the enqueue's arg0 (0x0042b74c) or from cmd_queue[idx].arg0 on the op-7 path (0x0042f49b). llm_tact_unit_rotate_tick compares it against facing_dir (load 0x00430819, CMP 0x00430820) and passes it to llm_tact_unit_rotate_step (0x0043082b/0x00430835) until they match. |
| `+0x5e4` | `face_cmd_arg1` | `ushort` | arg1 slot of the immediate FACE/TURN record. Written 0 by every writer found (0x0042b75a, 0x0042f4a9); no reader anywhere in the exported tactical set. Unused payload of the shared 0xb-byte cmd_entry shape -- scaffolding name, a LEAD. |
| `+0x5e6` | `face_cmd_arg2` | `ushort` | arg2 slot of the immediate FACE/TURN record. Written 0 at 0x0042b76a and 0x0042f4b9; no reader found. See face_cmd_arg1. |
| `+0x5e8` | `face_cmd_arg3` | `ushort` | arg3 slot of the immediate FACE/TURN record. Written 0 at 0x0042b77a and 0x0042f4c9; no reader found. It completes the record at +0x5df..+0x5e9 with zero orphan bytes, landing exactly on move_aborted_op at +0x5ea. See face_cmd_arg1. |
| `+0x5f0` | `squad_group_id` | `byte` | Player-assigned squad / control-group id, 0..7, with 0xff = unassigned (the spawn default, set at 0x0042bdad). llm_tact_ui_sel_panel_multi_mode_tick writes the active group (_G_LLM_TACT_SIDEBAR_ACTIVE_GROUP_ID) into every selected owner-0 unit at 0x00436336 ('assign to group') and re-selects every unit whose value matches it at 0x00436390/0x00436397 ('recall group'). llm_tact_sidebar_dispatch sets it from a sidebar icon click (0x00435adc) or resets it to 0xff (0x00435b01). llm_tact_squad_roster_refresh buckets every active unit by it (0x0043575f/0x00435769) into _G_LLM_TACT_UNASSIGNED_UNIT_ROSTER or _G_LLM_TACT_GROUP_UNIT_ROSTER[id]. ENUM candidate (8 groups + the 0xff sentinel). |
| `+0x5f1` | `move_redirect_col` | `byte` | Scratch tile COLUMN for re-targeting an in-flight MOVE. Snapshotted from pos_col whenever the command queue is CLEARed (op 0x46) -- 0x0042b5ce in enqueue_command, 0x0042b1f1 in llm_tact_group_issue_order. group_issue_order then uses it (with move_redirect_row) as the current-position input to llm_tact_move_step_attempt (0x0042b2c7/0x0042b2ce) and llm_tact_move_path_preview_walk (0x0042b2fe/0x0042b309), overwriting it with the walked-forward column at 0x0042b318. llm_tact_frame also reads it at 0x0042a873 to compute the click-preview facing. Confidence MEDIUM on the name -- the mechanism is traced, the intended concept is not. |
| `+0x5f2` | `move_redirect_row` | `byte` | Row counterpart of move_redirect_col, snapshotted from pos_row. Written 0x0042b5e8, 0x0042b20b, 0x0042b328; read 0x0042b2b9, 0x0042b2f0, 0x0042a884. |
| `+0x5f3` | `click_preview_facing` | `byte` | dir24 heading (0..0x17) computed per frame by llm_tact_frame's click-preview overlay for selected units with status bits 0x41, from llm_tact_calc_dir24(camera+cursor vs move_redirect_col/row) -- call 0x0042a8ba, store 0x0042a8e6. Consumed by llm_tact_group_issue_order at 0x0042b108 as an OVERRIDE for the order's target-direction argument when the order code is 7 (turn-then-attack), i.e. the live UI preview direction becomes the actual attack facing at issue time. |

#### `llm_avi_player_ctx` (size 0x214, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x04` | `active_flag` | `int` | nonzero => async playback running; gates boot-async critical-section init (read at boot tail) |
| `+0x9c` | `fmt_header` | `void *` | ptr to video format header (BITMAPINFOHEADER-like: bitcount@+0xe, +2, width@+4) used to size audio prebuffer |
| `+0x154` | `cur_frame` | `void *` | current decoded video frame (dims: width@+4, height@+8 stored top-down/negative) |
| `+0x1d8` | `frame_rect_work[4]` | `int` | working copy of frame rect {0,0,w,-h} (16 bytes) |
| `+0x1e8` | `frame_rect[4]` | `int` | frame source rect {left=0,top=0,right=w,bottom=-h} (16 bytes) |
| `+0x1f8` | `surface_bits` | `void *` | locked DirectDraw surface data ptr for the decoded frame |
| `+0x200` | `audio_chunk_bytes` | `int` | audio prebuffer size = bitcount*w*h/8*500/2000 |
| `+0x210` | `field_210` | `int` | zeroed at init; unknown |

#### `llm_bitflags8` (size 0x1, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `bit0` | `byte:1` |  |
| `+0x00` | `bit1` | `byte:1` |  |
| `+0x00` | `bit2` | `byte:1` |  |
| `+0x00` | `bit3` | `byte:1` |  |
| `+0x00` | `bit4` | `byte:1` |  |
| `+0x00` | `bit5` | `byte:1` |  |
| `+0x00` | `bit6` | `byte:1` |  |
| `+0x00` | `bit7` | `byte:1` |  |

#### `llm_ddraw_surface_desc` (size 0x6c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dwSize` | `int` | DDSURFACEDESC.dwSize. `MOV dword [0x0065f69f],0x6c` at 0x004cb2e2 -- 0x6c IS sizeof(DDSURFACEDESC), and the utils_fill_data call two instructions earlier (EBX=0x6c at 0x004cb2d1, call at 0x004cb2dd) zeroes exactly that many bytes. Two independent sources for the same number. |
| `+0x04` | `dwFlags` | `uint` | DDSURFACEDESC.dwFlags. FOUR bytes, not one: `MOV dword ptr [0x0065f6a3],0x7` at 0x004cb2ec. 7 = DDSD_CAPS\|DDSD_HEIGHT\|DDSD_WIDTH. (Previously modelled as a byte plus 3 pad bytes named `flags_planes`, which rendered the wrong width in every decompile.) |
| `+0x08` | `dwHeight` | `int` | DDSURFACEDESC.dwHeight. `MOV dword [0x0065f6a7],0xf0` at 0x004cb30a = 240. |
| `+0x0c` | `dwWidth` | `int` | DDSURFACEDESC.dwWidth. `MOV dword [0x0065f6ab],0x140` at 0x004cb300 = 320. |
| `+0x10` | `lPitch` | `int` | DDSURFACEDESC.lPitch (union with dwLinearSize). Filled by IDirectDrawSurface::Lock, not by the game. Offset from the DDSURFACEDESC layout; no in-binary write observed. |
| `+0x14` | `dwBackBufferCount` | `uint` | DDSURFACEDESC.dwBackBufferCount. Layout offset; zeroed by the utils_fill_data at 0x004cb2dd and never set. |
| `+0x18` | `dwMipMapCount` | `uint` | DDSURFACEDESC union {dwMipMapCount, dwZBufferBitDepth, dwRefreshRate}. Layout offset; zeroed and never set. |
| `+0x1c` | `dwAlphaBitDepth` | `uint` | DDSURFACEDESC.dwAlphaBitDepth. Layout offset; zeroed and never set. |
| `+0x20` | `dwReserved` | `uint` | DDSURFACEDESC.dwReserved. Layout offset; zeroed and never set. |
| `+0x24` | `lpSurface` | `void *` | DDSURFACEDESC.lpSurface -- the LOCKED pixel pointer IDirectDrawSurface::Lock writes back. Read at 0x004cb354 (`MOV EAX,[0x0065f6c3]`, i.e. base+0x24) immediately after the Lock call at 0x004cb34e, and stashed into llm_ui_info_media_ctx.locked_surface_ptr at 0x004cb359. This read is the strongest single confirmation of the DDSURFACEDESC identification: +0x24 is exactly where lpSurface lives. |
| `+0x28` | `ddckCKDestOverlay` | `uint[2]` | DDSURFACEDESC.ddckCKDestOverlay (DDCOLORKEY: dwColorSpaceLowValue, dwColorSpaceHighValue). Layout offset; zeroed and never set. |
| `+0x30` | `ddckCKDestBlt` | `uint[2]` | DDSURFACEDESC.ddckCKDestBlt (DDCOLORKEY). Layout offset; zeroed and never set. |
| `+0x38` | `ddckCKSrcOverlay` | `uint[2]` | DDSURFACEDESC.ddckCKSrcOverlay (DDCOLORKEY). Layout offset; zeroed and never set. |
| `+0x40` | `ddckCKSrcBlt` | `uint[2]` | DDSURFACEDESC.ddckCKSrcBlt (DDCOLORKEY). Layout offset; zeroed and never set. |
| `+0x48` | `ddpfPixelFormat` | `uint[8]` | DDSURFACEDESC.ddpfPixelFormat (DDPIXELFORMAT, 32 bytes: dwSize, dwFlags, dwFourCC, dwRGBBitCount, dwRBitMask, dwGBitMask, dwBBitMask, dwRGBAlphaBitMask). Layout offset; zeroed and never set -- DDSD_PIXELFORMAT is absent from dwFlags, so DirectDraw ignores it. |
| `+0x68` | `ddsCaps` | `uint` | DDSURFACEDESC.ddsCaps.dwCaps -- the LAST member, which is what makes the struct 0x6c. `MOV dword [0x0065f707],0x840` at 0x004cb2f6; 0x840 = DDSCAPS_SYSTEMMEMORY(0x800) \| DDSCAPS_OFFSCREENPLAIN(0x40), i.e. a plain system-memory offscreen surface. (Previously named `data_size`, which was actively misleading.) |

#### `llm_dir24_delta` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dx` | `int` | column delta in tiles for this 15-degree sector (range -10..+10) |
| `+0x04` | `dy` | `int` | row delta in tiles for this 15-degree sector (range -10..+10) |

#### `llm_dlg_state_record` (size 0x4, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `flags0` | `llm_bitflags8` | 8 individually-used flag bits, confirmed via ~50 referencing functions doing single-byte AND/OR/TEST on this byte alone. |
| `+0x01` | `field_1` | `byte` | At least bit1 individually confirmed live (5 xrefs); rest of this byte under-sampled, likely also individual flags. |
| `+0x02` | `field_2` | `byte` | No individual byte-level xrefs found -- only ever touched via the whole-record bulk OR (see llm_dlg_state_flags32.all). Meaning unconfirmed. |
| `+0x03` | `field_3` | `byte` | Same as field_2: no individual xrefs, only touched via bulk OR. Meaning unconfirmed. |

#### `llm_facing_trig` (size 0x14, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `cos` | `double` | cos of this facing's heading angle (radians); unit direction-vector X. Runtime-filled by llm_strat_unit_facing_offset_init loop3. NOTE off-by-one: rec[N].cos is fcos of rec[N-1].angle_deg (loop reads angle[i-1], writes trig[i]); rec[0].cos is left uninitialized. |
| `+0x08` | `sin` | `double` | sin of this facing's heading angle (radians); unit direction-vector Y. Same off-by-one pairing as cos. |
| `+0x10` | `angle_deg` | `int` | heading angle in DEGREES for this 24-dir facing. Init sets rec0..rec23 = 270,255,240,...,30,15,0,345,330,315,300,285 (15-deg steps = 360/24, full circle). |

#### `llm_gfx_ddraw_device` (size 0xd0, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `present` | `undefined4` | non-zero gate: DirectDraw device/window present |
| `+0x04` | `flags` | `byte` | 0x01 exclusive fullscreen (DDSCL_EXCLUSIVE\|FULLSCREEN + SetDisplayMode, no clipper) \| 0x04 back buffer in sysmem (DDSCAPS_SYSTEMMEMORY instead of VIDEOMEMORY) \| 0x08 WaitForVerticalBlank before the Blt present \| 0x10 primary is a COMPLEX\|FLIP chain with 1 backbuffer -> present via Flip; when CLEAR: plain primary + clipper + separate offscreen back buffer -> present via Blt (CORRECTED 2026-07-31: this bit was previously commented 'windowed', which is inverted) \| 0x20 make video surf(+0x58) \| 0x40 make surf(+0x5c) \| 0x80 DDSCL_ALLOWREBOOT. Only ever written as 7 (llm_gfx_set_window_resolution); llm_gfx_ddraw_create_surfaces ORs in 0x04, already set. So 0x08/0x10/0x80 are never set in the shipped build. |
| `+0x07` | `status` | `byte` | 0x40 keep-ddraw-alive (device-lost recreate) \| 0x80 mode-applied (surfaces live) |
| `+0x08` | `hwnd` | `pointer` | HWND target window |
| `+0x0c` | `width` | `int` | back-buffer / client width |
| `+0x10` | `height` | `int` | back-buffer / client height |
| `+0x1c` | `bpp` | `int` | display bit depth; 8=palettized, >=9 uses the RGB masks |
| `+0x20` | `r_mask` | `uint` | pixel-format red bitmask (bpp>=9) |
| `+0x24` | `g_mask` | `uint` | green bitmask |
| `+0x28` | `b_mask` | `uint` | blue bitmask |
| `+0x2c` | `video_surf_bpp` | `int` | bit depth of the +0x58 video surface (default 0x10) |
| `+0x30` | `video_vram_size` | `uint` | computed VRAM span of the video surface |
| `+0x3c` | `locked_mask` | `uint` | surface-id bits currently locked |
| `+0x40` | `dirty_mask` | `uint` | surface-id bits needing re-lock after a restore |
| `+0x44` | `ddraw` | `IDirectDraw *` | IDirectDraw* (== _G_LLM_GFX_DDRAW_DEVICE label) |
| `+0x48` | `clipper` | `IDirectDrawClipper *` | IDirectDrawClipper* |
| `+0x4c` | `surf_primary` | `IDirectDrawSurface *` | IDirectDrawSurface* id 1 (== _G_LLM_GFX_DDRAW_AUX label) |
| `+0x50` | `surf_back` | `IDirectDrawSurface *` | IDirectDrawSurface* id 2 (back buffer) |
| `+0x54` | `surf_id4` | `IDirectDrawSurface *` | IDirectDrawSurface* id 4 |
| `+0x58` | `surf_video` | `IDirectDrawSurface *` | IDirectDrawSurface* id 0x20 (video/overlay; FUN_004d90b0) |
| `+0x5c` | `surf_id40` | `IDirectDrawSurface *` | IDirectDrawSurface* id 0x40 (FUN_004d92ec) |
| `+0x7c` | `surf_id8` | `IDirectDrawSurface *` | IDirectDrawSurface* id 8 |
| `+0x80` | `palette` | `IDirectDrawPalette *` | IDirectDrawPalette* |
| `+0x84` | `lock_slots` | `llm_gfx_ddraw_lock_slot[6]` | per-surface-type lock cache; idx 0=primary(1),1=back(2),2=id4,3=video(0x20),4=id0x40,5=id8 |
| `+0xcc` | `flip_count` | `int` | count of SUCCESSFUL IDirectDrawSurface::Flip calls (incremented when Flip returns DD_OK); zeroed by llm_gfx_ddraw_create_surfaces, bumped by llm_gfx_ddraw_present. WRITE-ONLY -- no reader anywhere in the binary, and the Flip path itself is unreachable at the shipped flags=7. Diagnostic/leftover frame counter. |

#### `llm_gfx_ddraw_lock_slot` (size 0xc, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `bits` | `pointer` | locked surface memory (DDSURFACEDESC.lpSurface) |
| `+0x04` | `pitch` | `int` | row pitch / valid flag (set to 1 on lock) |
| `+0x08` | `rsv` | `int` | reserved (0) |

#### `llm_gfx_font_desc` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `handle` | `pointer` | Font handle; passed to the font-select primitive at 0x004a2f9d on entry (push) and exit (pop). |
| `+0x04` | `glyph_table` | `pointer *` | Per-glyph record-pointer table, indexed by char code (*4). Each entry points to a glyph record whose first byte is its advance width. |

#### `llm_input_key_event` (size 0x38, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `scancode` | `uint` | (lParam >> 16) & 0x7f |
| `+0x04` | `event_type` | `uint` | 0x80 up / 0x100 down, OR 0x200 when sourced from DirectInput (llm_input_di_keyboard_poll) instead of a raw WM_KEY* message |
| `+0x08` | `reserved_0x08` | `undefined1[24]` | unused for keyboard events |
| `+0x20` | `timestamp` | `uint` | GetTickCount() - _G_LLM_INPUT_TIME_EPOCH |
| `+0x24` | `reserved_0x24` | `undefined1[20]` | unused tail of the 0x38 stride |

#### `llm_input_mouse_event` (size 0x38, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `buttons` | `uint` | snapshot of the button/modifier bitmask at event time |
| `+0x04` | `event_type` | `uint` | 1 move, 2 LBUTTONDOWN, 4 LBUTTONUP, 8 RBUTTONDOWN, 0x10 RBUTTONUP |
| `+0x08` | `dx` | `int` | delta x since the previous recorded event |
| `+0x0c` | `dy` | `int` | delta y since the previous recorded event |
| `+0x10` | `wheel_delta` | `int` | signed wheel rotation for this event (0 except WM_MOUSEWHEEL) |
| `+0x14` | `x` | `uint` | absolute x (LOWORD of lParam) |
| `+0x18` | `y` | `uint` | absolute y (HIWORD of lParam) |
| `+0x1c` | `wheel_total` | `int` | running wheel-rotation accumulator at event time |
| `+0x20` | `timestamp` | `uint` | GetTickCount() - _G_LLM_INPUT_TIME_EPOCH |
| `+0x24` | `reserved_0x24` | `undefined1[20]` | unused tail of the 0x38 stride |

#### `llm_map_object` (size 0x18, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `kind` | `int` | 1-based index into cfg::final::data::Tree[1..48] (.frame_index=sprite); trees are named '01'..'48' = the index, so kind = decoration-variant number. STONE class empty. |
| `+0x04` | `px_x` | `int` | sub-tile pixel X offset 0..31; drawn at tile_x*32 + px_x - sprite.origin_x |
| `+0x08` | `px_y` | `int` | sub-tile pixel Y offset 0..31; drawn at tile_y*32 + px_y - sprite.origin_y |
| `+0x0c` | `next_on_tile` | `int` | next object stacked on the same tile (linked list; tile.building=head, 0=end) |
| `+0x10` | `x` | `int` | tile X (0..width) |
| `+0x14` | `y` | `int` | tile Y (0..height) |

#### `llm_mine_deposit_slot` (size 0xd, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `resource_id` | `byte` | cfg Building.extract_id[slot] bound to this deposit slot by llm_strat_mine_scan_deposit_slot (write @0x0047abc1). 0 = slot unbound / no deposit in range. |
| `+0x01` | `tile_x_q4` | `int` | Bound resource tile COLUMN, stored pre-divided by 4 (>>2) at write time (@0x0047afd6) -- map-region units, NOT full tile units. Read by llm_strat_bldg_completion_dispatch @0x0047a834. |
| `+0x05` | `tile_y_q4` | `int` | Bound resource tile ROW, same >>2 scaling as tile_x_q4 (write @0x0047b008, read @0x0047a853). |
| `+0x09` | `extract_rate` | `int` | Per-cycle extraction amount: cfg Building.extract_val[slot] scaled by a distance multiplier (1.0 exact cell / 0.5 near / 0.1 far) and rounded. Writes: 0x0047b09e (assigned), 0x0047abf2 (=0, no candidate), 0x0047b0c9 (=0, site lost). Consumed each MINE_EXTRACTING tick by llm_strat_bldg_completion_dispatch @0x0047a872. |

#### `llm_net_lobby_peer` (size 0x404, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `session_handle` | `void *` | opaque session handle, copied from the joined llm_net_session_desc.session_handle staging buffer (DAT_005d0f50) |
| `+0x04` | `player_id` | `int` | read by llm_lobby_slot_cycle_state_cb / llm_lobby_peer_slot_remove; NOT written by add_ai -- must already hold a valid value at alloc time (semantics unresolved) |
| `+0x08` | `name` | `char[32]` | display name; add_ai writes only 6 bytes ('dummy\0') for the AI default; 32B sized by analogy to llm_net_session_desc.name_blob |
| `+0x28` | `reserved_0x28` | `undefined1[16]` | zeroed via utils::fill_data on entry creation; no further evidence |
| `+0x38` | `peer_type` | `int` | 2 = AI (only confirmed value) |
| `+0x3c` | `peer_flags` | `byte` | 0xff on AI add (only confirmed value) |
| `+0x3d` | `reserved_tail` | `undefined1[967]` | no evidence; explicit padding to the 0x404 stride, not a single real field |

#### `llm_net_lockstep_peer_timing` (size 0xc, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `order_marker` | `int` | Peer's order-sequence marker, travelling with its advertised horizon. record_peer_horizon (0x0049ffa2) stores it; commit-side agreement is tested by comparing every active peer's marker against the LOCAL side's (0x004a006b) -- a mismatch means the peers are at the same time but not on the same order stream. peer_timing_reset zeroes it at session start. |
| `+0x04` | `horizon` | `double` | Highest lockstep horizon this peer has advertised, in game-seconds. -1.0 = nothing reported yet (peer_timing_reset). record_peer_horizon REJECTS a non-increasing value (returns 1 without storing), so this is monotonic within a session. Distinct from _G_LLM_NET_PEER_HORIZON (0x5d54cc), which is the horizon the barrier commits against. |

#### `llm_net_roster_entry` (size 0x400, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `reserved_0x0` | `undefined4` | zeroed by llm_net_lobby_scan_state_reset; no reader observed |
| `+0x04` | `player_id` | `int` | peer/player id. The strided view (&player_id)[k*0x100] over the LIVE array (_G_LLM_NET_ROSTER_LIVE @0x5d3368) IS the peer-id table _G_LLM_NET_HOST_PLAYER_ID -- scanned by llm_net_player_slot_from_id, populated by the injected DLL (U8). Stride 0x400 (0x400-byte record). |
| `+0x08` | `name` | `char[48]` | ANSI display name. Only llm_net_session_list_merge_refresh reads it -> UNPOPULATED in the live game (the retail roster-scan path is dead). |
| `+0x38` | `aux_value` | `int` | merge_refresh-only; copied to llm_net_session_entry.aux_value (host_net_dispatch admin loop treats >=2 as accepted, <2 as kick). |
| `+0x3c` | `active_flag` | `byte` | 0xff/-1 = empty slot (merge_refresh skips it); else present. Unpopulated in the live game. |

#### `llm_net_session_entry` (size 0x29, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char[32]` | ANSI display name, null-padded; confirmed via llm_lobby_announce_line reading arg directly as an ansi string |
| `+0x20` | `event_tag` | `byte` | 1=seen-in-scan-not-in-roster (departure: consumer removes slot + sends pkt 0x14); 2=in-roster-not-in-scan (rejoin/resync: allocs slot + sends 0xd kick or 0x13 update per aux_value) |
| `+0x21` | `player_id` | `int` | network player id copied from the matching source roster record |
| `+0x25` | `aux_value` | `undefined4` | event_tag==2 only; sourced from host-roster record +0x34; no other referrer in the binary -- semantics UNCONFIRMED |

#### `llm_prod_shuttle_slot` (size 0x31c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x04` | `prev_slot_transit_x` | `int` | RD-A readers readiness (2026-08-24). NOT this slot's own datum -- it is the in-flight interpolated star-map X of the PRECEDING slot (index i-1), parked in this slot's dead leading pad. llm_strat_prod_set_transfer_destination is the only referrer: when rerouting a transfer that is already in transit (status 0xc8) with origin_planet == 0 it reads the cache back as the leg's origin (0x0048f065 / 0x0048f1a4) and, after re-lerping between origin and destination planet coordinates, writes it again (0x0048f120), all at slot-relative +0x320 = SLOTS[i+1] + 4. CAUTION, and it is real: for player 7 slot 9 (i = 79) that address is 4 bytes PAST the declared 80-element array, at 0x00be1a20; harmless in this build only because the next global starts 0x18 bytes further on. Bytes [0,4) of this same pad are a DIFFERENT overrun (the 50th cargo-manifest entry's trailing int -- see sim_unit_load_into_shuttle_cargo.h). |
| `+0x08` | `prev_slot_transit_y` | `int` | RD-A readers readiness (2026-08-24). Y half of prev_slot_transit_x, same owner (slot i-1), same sole referrer: reads at 0x0048f081 / 0x0048f18b, write at 0x0048f13c, slot-relative +0x324. |
| `+0x14` | `src_building_index` | `short` | source building's index within its owner's building array; written at bind time from the slot-request's building_index arg |
| `+0x16` | `type_ref_id` | `ushort` | config-table index whose meaning depends on transfer stage: at bind time (llm_prod_shuttle_slot_bind_default) holds the SOURCE building's building_id (cfg::final::data::Building[] index); for a heli-mothership departure, llm_prod_bldg_depart_finalize overwrites it with the newly-created UNIT's unit_proto_id instead. Doubles as the free/used flag (0 = slot free). |
| `+0x18` | `src_building_type` | `ushort` | source building's cfg::final::data::Building[].type, snapshotted at bind time |
| `+0x1a` | `status` | `short` | transfer state code; observed values 0xca (just bound), 0xc9 (arrived, ready to spawn -- checked in the still-unnamed FUN_0048f086), 0xcc (unit spawned/delivered), 0xc8/200 (set by llm_strat_unit_state_production_ready) |
| `+0x1c` | `origin_planet` | `short` | planet the transfer started from; defaults to G_PLANET_INDEX at bind time |
| `+0x1e` | `dest_planet` | `short` | destination planet; set at actual departure by llm_prod_bldg_depart_finalize (was 0 / unset before then) |
| `+0x20` | `travel_duration` | `double` | REMAINING interplanetary travel time in ticks, decremented as the shuttle flies (reaches 0 on arrival). Initialized by llm_prod_bldg_depart_finalize / llm_strat_prod_set_transfer_destination to Building[equivalent].velocity * planet-to-planet distance. Journey-progress fraction = (travel_duration_copy - travel_duration)/travel_duration_copy (llm_strat_prod_transfer_progress). |
| `+0x28` | `travel_duration_copy` | `double` | FULL original travel time (the ETA at departure), never decremented -- the denominator for the journey-progress fraction (llm_strat_prod_transfer_progress). Written alongside travel_duration; not reset by the slot-free routine. Purpose confirmed 2026-07-04: it is the total-time reference for position interpolation, not merely a UI display copy. |
| `+0x30` | `resources_reserved` | `int[10]` | per-resource-id amount reserved/loaded for this transfer (indices 0-6 used by the fuel and cargo load/unload functions); llm_prod_shuttle_load_resource/_unload_resource increment/decrement one slot at a time |
| `+0x58` | `passengers_reserved` | `int` | colonists (human population) currently loaded for this transfer; llm_prod_shuttle_load_passengers/_unload_passengers increment/decrement |
| `+0x5c` | `is_planet_bound` | `int` | 1 while this slot holds a reservation lock on the destination planet's map::_G_LLM_STRAT_PLAYERS[].prod_queue_slot reverse-mapping; set/cleared by llm_strat_prod_bind_planet/_unbind_planet |
| `+0x60` | `is_heli_mother_pending` | `int` | 1 when the unit being produced is a heli-mothership needing special mobile-conversion handling at spawn time; set by llm_strat_unit_state_production_ready, consumed (reset to 0) by the still-unnamed FUN_0048f086 right before spawning |
| `+0x64` | `cargo_manifest_raw` | `byte[696]` | cargo manifest: ~50 conceptual entries strided 0xe(14) bytes apart (llm_strat_production_spawn_unit's reset loop touches offset+0 of each as a short); only those first 2 bytes per entry are confirmed, the rest of each entry's layout and the manifest's exact semantics are unresolved -- see tasks/strategic-sim.md |

#### `llm_snd_cfg_entry` (size 0x2c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x21` | `priority` | `byte` | Sound priority 0-0xff (higher wins channel contention when voices are exhausted). |
| `+0x23` | `volume` | `byte` | Base volume 0-0xff, scaled by _G_LLM_SND_MASTER_VOLUME at play time. |
| `+0x24` | `note_or_rate` | `int` | NOTE pitch (0x35-0x6b0) or SPEED sample-rate; bit31 flags which. |
| `+0x28` | `wave_ptr` | `pointer` | Loaded wave-data pointer (rsr::GetResourseFilePtr); 0 = not loaded. |

#### `llm_squad_placement_offset` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `offset_a` | `int` | written by llm_strat_squad_placement_offset_set from col_class (squad size class 1/2/3 remapped to 8/0x10/0x18); read as a byte (low byte) by llm_ui_cursor_lookup_offset_pair out_a. Which of a/b is dx vs dy is NOT settled -- llm_strat_squad_pick_free_formation_anchor's (x,y) candidates come out (24,8),(8,8),(16,16),(24,24),(8,24) for col=5 rows 1..5 |
| `+0x04` | `offset_b` | `int` | written from row_class (soldier-slot class remap, same 8/0x10/0x18 domain); read as a byte by llm_ui_cursor_lookup_offset_pair out_b |

#### `llm_squad_status_slot` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `unit_proto_id` | `int` | 0 = slot unused. Written by llm_strat_bldg_gather_nearby_squad_status (0x0044d707); read/compared by llm_tact_mission_end_finalize_stub (0x0044da91, result discarded -- dead code). |
| `+0x04` | `energy_pct` | `int` | Soldier HP/energy percent, floored at 1. Set on squad assembly by llm_strat_bldg_gather_nearby_squad_status (0x0044d6e4); refreshed by llm_tact_squad_sync_hp (0x00438fde/0x00438fec) ONCE, at the mission-exit-confirm moment (both of llm_tact_frame's call sites -- 0x00429c7d, 0x00429de9 -- sit immediately before llm_tact_mission_end_return_to_strategic), not every tactical frame -- the ONLY channel that survives the excursion (docs/dead-ends.md G44). |
| `+0x08` | `unit_slot_index` | `int` | Index into units[scan_player][] for this soldier. Written 0x0044d716; consumed by llm_strat_squad_assault_resolve (0x0044d854) to look up unit_proto_id for the damage table. |
| `+0x0c` | `is_commando` | `int` | 1 if Unit[unit_proto_id].soldier_type == COMMANDO else 0. Written 0x0044d748/0x0044d75a. |

#### `llm_strat_ai_attack_candidate` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `unit_index` | `ushort` | Roster index of the candidate unit, written by llm_strat_ai_attack_candidate_add. |
| `+0x02` | `score` | `short` | Running suitability score, zeroed then INC/DEC'd by llm_strat_ai_active_unit_tick; one branch subtracts 0x64. NOT written by attack_candidate_add. |
| `+0x04` | `dist_sq` | `int` | Squared distance from the candidate's (x, y) to the point under consideration; computed and stored by llm_strat_ai_active_unit_tick, then compared against weapon_range_sq. |
| `+0x08` | `weapon_range_sq` | `uint` | SQUARE of llm_strat_unit_max_weapon_range for this unit. attack_candidate_add stores the raw range to this slot first and immediately overwrites it with the square -- the first store is dead, and the field holds the square. |
| `+0x0c` | `x` | `ushort` | Candidate unit's tile X (map::object::unit::x), zero-extended from the byte field. |
| `+0x0e` | `y` | `ushort` | Candidate unit's tile Y (map::object::unit::y), zero-extended from the byte field. |

#### `llm_strat_ai_bldg_queue_entry` (size 0x12, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `status` | `byte` | low nibble = entry kind, dispatched by llm_strat_ai_bldg_queue_process: 0=train/recruit (llm_strat_ai_bldg_queue_handle_recruit_state), 1=construction (llm_strat_ai_bldg_queue_process_entry), 2=llm_strat_ai_bldg_queue_handle_state2_empty (an 11-byte no-op), 3/4=llm_strat_ai_bldg_queue_handle_upgrade_or_cancel -- and that handler splits them: nibble 4 debits resource_spent[1..4] by resource_reserved[1..4] and issues llm_strat_bldg_order_upgrade_enqueue, nibble 3 issues llm_strat_bldg_order_repair_cycle_start_enqueue with no resource movement (CMP CL,0x4 / JNZ @0x004e8225-0x004e8228). Bits: 0x20 = affordability check waived -> take the normal construction-order path (llm_strat_ai_bldg_queue_process_entry clears the bit and calls llm_strat_order_queue_construction_enqueue; when the bit is CLEAR it instead instant-constructs and debits resource_spent[] directly); 0x40 = entry removed, compacted out by the queue_process prologue; 0x80 = entry finished/committed, skipped by queue_process (0x83/0x84 = the committed repair/upgrade forms llm_strat_ai_queue_release_order matches on). |
| `+0x01` | `tick_or_unit_id` | `byte` | THREE meanings, selected by status's low nibble. train entries (kind 0): the spawned unit's unit_id, i.e. a cfg Unit TYPE id. CONSTRUCTION entries (kind 1): a cfg BUILDING TYPE id -- llm_strat_bldg_queue_construction stores its building_type argument here (`MOV byte ptr [EDX + EAX*0x1 + 0xe935f1],CL` @0x004e25a6) in the same breath as stamping status=1 (@0x004e2594), and llm_strat_ai_bldg_queue_process_entry multiplies it by the 0x842 cfg Building stride at 0x004e7eb0 / 0x004e7f3f / 0x004e7ff8 / 0x004e8043. repair entries: tick counter counting up to Building.energy_d, decremented by llm_strat_ai_queue_release_order on cancel; upgrade entries: unused. |
| `+0x02` | `build_tile_x` | `short` | CONSTRUCTION entries (kind 1): cached resolved build tile X, -1/0xffff = not yet resolved. llm_strat_ai_bldg_queue_process_entry resolves it from the site-candidate scan and re-invalidates it to 0xffff when the tile is no longer free. Explicitly zeroed (with resource_reserved[0]) by llm_strat_ai_queue_train_unit, i.e. unused for train entries. Was field_0x2_unk. |
| `+0x04` | `resource_reserved` | `short[5]` | reserved resource costs, index = RESOURCE ID (llm_strat_ai_queue_train_unit accrues Unit[].resource[].val at +0x04 + id*2). Only ids 1..4 are ever costs, so slot [0] is dead as a cost -- and llm_strat_ai_bldg_queue_process_entry REUSES it as the cached resolved build tile Y, paired with build_tile_x at +0x02. Refunded by llm_strat_ai_queue_release_order on cancel. |
| `+0x0e` | `building_index` | `int` | target building roster index for repair/upgrade entries; unused for train entries |

#### `llm_strat_ai_engage_candidate` (size 0xc, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `target_ref` | `uint` | Packed target reference: LOW NIBBLE (&0xf) = owning player index; the HIGH bits are a class code. TWO DIFFERENT ROSTER TESTS EXIST OVER THE SAME FIELD and both are live -- preserve each where it appears rather than normalising them: (a) (ref & 0xa0) == 0 -> BUILDING, else UNIT -- used by llm_strat_ai_engage_candidate_add, llm_strat_ai_target_ref_is_alive, llm_strat_ai_engage_sort_candidates_by_dist and the attack-order commit branch of llm_strat_ai_engage_filter_and_commit_target; (b) (ref & 0x40) != 0 -> BUILDING -- used by llm_strat_ai_engage_partition_turret_candidates (which then reads buildings[].building_id and tests Building[].type == A_TURRET/H_TURRET, so 0x40 really does denote a building) and by the survivability scan in both engage_select_and_commit and engage_filter_and_commit_target. The two agree on class nibbles 2 (unit) and 4 (building) and disagree on nibble 0, so a class-0 entry is classified oppositely by the two halves of the SAME function. Bit 0x80 is OR'd in by callers onto the SOURCE ref they hand to the sort helper, where test (a) reads it as UNIT. |
| `+0x04` | `target_index` | `int` | Index of the target within its owner's roster -- units[owner][target_index] when bit 0x20 of target_ref is set, buildings[owner][target_index] otherwise. |
| `+0x08` | `dist_sq` | `uint` | Toroidal squared distance from the querying entity, filled by llm_strat_ai_engage_sort_candidates_by_dist which then bubble-sorts the array ascending on it. UNDEFINED until that helper runs -- llm_strat_ai_engage_candidate_add writes only the two fields above. |

#### `llm_strat_ai_group_task` (size 0x29, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `pending_param` | `int` | order parameter (target id / packed value; task-code dependent) |
| `+0x04` | `task_code` | `short` | task state-machine code 2..0x18 |
| `+0x06` | `active_flag` | `byte` | 1 when this record is the active (front) task |
| `+0x07` | `sub_code` | `short` | aux param: leg/loiter counter, units-to-recruit, etc (task-code dependent) |
| `+0x09` | `param_a` | `int` | polymorphic: anchor X, or target owner/type ref (-1 = use group centroid) |
| `+0x0d` | `param_b` | `int` | polymorphic: anchor Y, or target index |
| `+0x11` | `param_c` | `int` | secondary anchor X (e.g. task 5 shuttle far end) |
| `+0x15` | `param_d` | `int` | secondary anchor Y |
| `+0x19` | `queued_time` | `double` | AI clock when enqueued |
| `+0x21` | `task_start_time` | `double` | AI clock when activated (dispatcher prologue re-stamps) |

#### `llm_strat_ai_mine_kernel_cell` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dx` | `int` | Coarse-grid (tile>>2) column offset from the mine's own cell. Range -2..2 in the shipped table. |
| `+0x04` | `dy` | `int` | Coarse-grid (tile>>2) row offset. Range -2..2 in the shipped table. |
| `+0x08` | `weight` | `double` | Multiplier applied to cfg Building[].extract_val when the sampled cell holds the resource. Shipped values: 1.0 for the centre cell, 0.5 for the 8 ring-1 cells, 0.1 for the 16 ring-2 cells. llm_strat_ai_calc_mine_yield_estimate walks the table IN ORDER and stops a resource's scan at the first hit, so a resource's score is the weight of its NEAREST cell; the three thresholds it then buckets on (0.99 / 0.499 / 0.099 @0x004e32a1/0x004e32b6/0x004e32cc) are these three values with an epsilon. |

#### `llm_strat_ai_mine_quality` (size 0xc, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `near_count` | `int` | How many of resource ids 1..4 have their nearest deposit at kernel weight >= 0.99, i.e. on the mine's own coarse cell (ring 0). Incremented @0x004e32b2. |
| `+0x04` | `mid_count` | `int` | ... at weight >= 0.499, i.e. Chebyshev ring 1. Incremented @0x004e32c7. |
| `+0x08` | `far_count` | `int` | ... at weight >= 0.099, i.e. Chebyshev ring 2. Incremented @0x004e32dd. A resource with no deposit anywhere in the 5x5 kernel is counted in none of the three. |

#### `llm_strat_ai_mine_yield` (size 0x20, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `by_resource` | `int[8]` | Estimated extraction per resource id. [1..4] are the resource ids and are zeroed then filled by llm_strat_ai_calc_mine_yield_estimate (@0x004e3164 / @0x004e323b) as trunc(cfg Building[].extract_val * the nearest-cell kernel weight). [0] is a TOTAL that the estimator only ADDS into (@0x004e329f) and never zeroes, so it holds stale carry until llm_strat_ai_mine_portfolio_rebalance resets and re-sums it @0x004e3618-0x004e3639. [5..7] are never touched; the 32-byte row stride is what the SHL 5 addressing at 0x004e34ee gives. |

#### `llm_strat_ai_opponent_assessment` (size 0x3c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `soldier_count` | `int` | opponent's live soldier unit count |
| `+0x04` | `soldier_power` | `int` | summed llm_strat_ai_unit_weapon_power across soldier units |
| `+0x08` | `ground_count` | `int` | ground vehicle (non-soldier IsAiGround) unit count |
| `+0x0c` | `ground_power` | `int` | summed weapon power across ground vehicle units |
| `+0x10` | `heli_count` | `int` | helicopter unit count |
| `+0x14` | `heli_power` | `int` | summed weapon power across helicopter units |
| `+0x18` | `plane_count` | `int` | plane unit count |
| `+0x1c` | `plane_power` | `int` | summed weapon power across plane units |
| `+0x20` | `total_unit_power` | `int` | soldier_power + ground_power + heli_power + plane_power |
| `+0x24` | `building_count` | `int` | opponent's total building count |
| `+0x28` | `mine_count` | `int` | opponent's H_MINE/A_MINE building count |
| `+0x2c` | `turret_count` | `int` | opponent's H_TURRET/A_TURRET building count |
| `+0x30` | `aa_capability_flags` | `int` | bit 0x1 = has a unit with llm_strat_unit_has_aa_weapon; bit 0x2 = has a turret with llm_strat_bldg_has_aa_weapon (only the low byte is meaningful) |
| `+0x34` | `weighted_resource_score` | `int` | weighted resource stockpile: sum over k=1..4 of map::player_resources[assessed_player*10 + k] * _G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS[k] (weights {4,2,2,1}). Zeroed at llm_strat_ai_update_opponent_relations @0x004d757d; accumulated @0x004d77b6 (`IMUL EDX,dword ptr [EDI*0x4 + 0x6616a8]`) / @0x004d77be (`ADD dword ptr [ESI + 0x34],EDX`). NAMED 2026-08-29 (R10 readiness drain) -- the mechanism was already derived and only the field name was missing. CORRECTION (2026-07-03): index 0 of the weight array is NOT part of a fixed {0,4,2,2,1} table as earlier prose described -- it is a dynamically-growing 'highest claimed player index + 1' counter (llm_strat_spawn_ai_base / _invasion_force / llm_strat_init_human_player_data) that merely shares storage with the byte before the real weights. Nothing ever reads index 0 as a weight, so the computed score is unaffected; the description was what was wrong. |
| `+0x38` | `weighted_mine_yield_score` | `int` | weighted estimated total mine yield: for each H_MINE/A_MINE building the assessed player owns, llm_strat_ai_calc_mine_yield_estimate (called @0x004d7741) fills a 5-slot buffer and slots 1..4 are accumulated as yield[k] * _G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS[k] (weights {4,2,2,1}). Zeroed at llm_strat_ai_update_opponent_relations @0x004d7584; accumulated @0x004d7751 (`IMUL EBX,dword ptr [EDX*0x4 + 0x6616a8]`) / @0x004d7759 (`ADD dword ptr [ESI + 0x38],EBX`). Same weight-array index-0 correction as weighted_resource_score. NAMED 2026-08-29 (R10 readiness drain). |

#### `llm_strat_ai_resource_site` (size 0xa, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `grid_x` | `short` | COARSE (quarter-tile) grid column of the resource site. Consumers convert to tile coords as grid_x*4 (+2 to centre): llm_strat_sort_sites_by_dist does <<2, llm_strat_ai_bldg_scan_resource_site_candidates does *4+2. Written by llm_strat_spawn_ai_base's map sweep. |
| `+0x02` | `grid_y` | `short` | coarse (quarter-tile) grid row; same *4 (+2) convention as grid_x. |
| `+0x04` | `status` | `short` | TRI-STATE, and the third state is a BUILDING INDEX -- the comment here listed only the first two until 2026-08-05. 0 = unresolved, still a live build candidate (the only value llm_strat_ai_bldg_scan_resource_site_candidates will act on, CMP @0x004e63bd). 0xffff = invalidated, written by that scanner when no neighbouring tile can host the mine (@0x004e6558) and by llm_strat_ai_notify_object_removed's HARD-remove arm (@0x004db866). ANY OTHER VALUE = the buildings[player][] roster index of the mine occupying this site: llm_strat_ai_notify_bldg_constructed stores the new mine's roster index here (@0x004db084) once it matches the site's cached build_tile_x/build_tile_y, and llm_strat_ai_notify_object_removed searches the site list for status == the removed building's index (@0x004db855) and resets it to 0 (soft remove, site becomes a candidate again) or 0xffff (hard remove) @0x004db874 / @0x004db866. Roster indices are 1..99 so they never collide with either sentinel. Initialised 0 by llm_strat_spawn_ai_base. |
| `+0x06` | `build_tile_x` | `short` | cached resolved build tile X for this site, written by llm_strat_ai_bldg_scan_resource_site_candidates on a successful placement scan; 0xffff at init. |
| `+0x08` | `build_tile_y` | `short` | cached resolved build tile Y, paired with build_tile_x; 0xffff at init. |

#### `llm_strat_ai_scan_target_entry` (size 0x16, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `target_ref` | `ushort` | Packed owner\|kind ref of the target, same layout as map::object::unit::target_ref: low nibble = owning player, (ref & 0xa0)==0 selects the BUILDING roster and !=0 the UNIT roster. NOT the object id -- llm_strat_ai_scan_target_list_add takes it in EDX (its committed param name said `target_id` until 2026-08-01, which was wrong). |
| `+0x02` | `target_index` | `ushort` | Roster slot index of the target within the owner selected by target_ref. Deduped on jointly with target_ref by llm_strat_ai_scan_target_list_add. |
| `+0x04` | `priority_score` | `ushort` | Running priority score. Producers zero it; llm_strat_ai_group_classify_target_object increments it once per rule that matches (turret / counter-attacker / mother-building / in-threat-ring). |
| `+0x06` | `class_flags` | `ushort` | Classification bitmask, byte-OR'd by llm_strat_ai_group_classify_target_object alongside each priority_score bump: 0x01 the counter-target is a UNIT, 0x02 it is a BUILDING, 0x04 that building is the player's ai_mother_building_type, 0x08 the target stands inside this player's threat ring, 0x20 the target is a turret, 0x40 the target is a mother building. |
| `+0x08` | `reserved_0x08` | `ushort` | RESERVED / role unknown, and PROVEN UNREAD image-wide (2026-08-29, R10 readiness drain). NEVER WRITTEN INDEPENDENTLY: every write is the upper half of a DWORD store aimed at class_flags (+6) -- llm_strat_ai_scan_target_list_add @0x004ec334 `MOV dword ptr [EAX + 0x101731a],0x0`, and the two UNREACHABLE producers inside llm_strat_ai_group_scan_building_targets @0x004ec067 (dword 0x10, i.e. class_flags=0x10 and this word 0) and @0x004ec1cd (dword 0). So it is always 0. NEVER READ: find-cross-references program-wide on the array base 0x01017314 plus the two interior anchors enumerates 7 referrer functions in total, and the 3 live ones (llm_strat_ai_scan_target_list_add, llm_strat_ai_active_unit_tick, llm_strat_ai_log_scan_targets) contain NO register-relative `+ 0x8` access at all in their .asm -- nor does llm_strat_ai_group_classify_target_object, which fills every other field past +0x04 and whose OR-byte writes all target class_flags' low byte at +6. The 4 dead referrers carry the word only inside a whole-record 0x14-byte memcpy. An image-wide disp32 scan for base+8 (0x0101731c) and for the +1024-entry interior +8 (0x0101cb1c) returns 0 hits, but that instrument is WEAK here and is corroboration only: its control at +0xa tile_x also returns 0 while the classifier demonstrably writes tile_x through a register base. The .asm read of the referrer set is the load-bearing evidence. Most likely the low half of a convenient two-word zero-store rather than a field at all -- but that is inference, not proof. The R10 blocker it leaves on llm_strat_ai_scan_target_list_add is acknowledged in tools/data/ai_ready_ack.json rather than dressed up as a resolved name. |
| `+0x0a` | `tile_x` | `ushort` | Target tile X, copied from buildings[owner][idx].x or units[owner][idx].x by the classifier. [map::t::tile_coord] |
| `+0x0c` | `tile_y` | `ushort` | Target tile Y. [map::t::tile_coord] |
| `+0x0e` | `range_sq` | `ushort` | SQUARE of the target's own weapon range (the classifier writes the range then overwrites it with range*range), so it compares directly against llm_strat_toroidal_dist_sq. 0 when the target is an unarmed building. |
| `+0x10` | `counter_target_ai_group` | `ushort` | AI group index of the unit this target is itself attacking (units[owner][counter_target_index].ai_group_index). Producers preset it to 0xffff = none, and the classifier overwrites it only when the counter-target is a UNIT. |
| `+0x12` | `counter_target_ref` | `ushort` | Packed owner\|kind ref of what this target is currently attacking (0 = nothing); same bit layout as target_ref. |
| `+0x14` | `counter_target_index` | `ushort` | Roster slot index of what this target is currently attacking. |

#### `llm_strat_ai_site_candidate` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `tile_x` | `int` | candidate build tile X. Written by all three AI site scanners (llm_strat_ai_scan_build_site_candidates, _bldg_scan_grid_candidates, _bldg_scan_resource_site_candidates). |
| `+0x04` | `tile_y` | `int` | candidate build tile Y. |
| `+0x08` | `kind` | `int` | scan that produced the entry: 0 = free-area/grid fit (scan_build_site_candidates + bldg_scan_grid_candidates), 1 = resource-adjacent (bldg_scan_resource_site_candidates). WRITE-ONLY as of 2026-08-01: no reader of this column exists in the image. |
| `+0x0c` | `dist_sq` | `uint` | toroidal squared distance from the player's base (ai_home_tile_x/y) to (tile_x,tile_y), from llm_strat_toroidal_dist_sq. Sole sort key of llm_strat_ai_sort_site_candidates_by_dist. |

#### `llm_strat_ai_spiral_offset` (size 0x2, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dx` | `char` | signed tile offset applied to the anchor X. Built by llm_strat_ai_spiral_table_init over dx,dy in [-127,127] keeping cells with dx*dx+dy*dy < 0x3f02, then qsorted by radius so a prefix of the array is a disc of that radius. |
| `+0x01` | `dy` | `char` | signed tile offset applied to the anchor Y. |

#### `llm_strat_ai_spread_tile` (size 0x6, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `short` |  |
| `+0x02` | `y` | `short` |  |
| `+0x04` | `used` | `short` |  |

#### `llm_strat_ai_target_entry` (size 0x14, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `victim_index` | `int` | Roster index of the AI-side object that was attacked. Renamed from target_id 2026-08-03 (finding 2026-08-02-1731-3). Written by llm_strat_ai_target_list_add from its `victim_index` parameter (EBX, spilled to [EBP-0xc] @0x004d6d7e, stored @0x004d6de4) and handed to llm_strat_unit_get_ai_group_index to fill ai_group_index below. Confirmed as an INDEX and not an id by llm_strat_ai_unit_should_abandon_target, which compares it against the caller's own unit index (@0x004ec8f0) and then indexes units[player][idx] with the 0xe9 record stride (@0x004ec92a). Pairs with victim_ref at +0x04 exactly as aggressor_index pairs with aggressor_ref. |
| `+0x04` | `victim_ref` | `uint` | PACKED OBJECT REF for the victim -- same encoding as aggressor_ref at +0x10, which is why the two fields now share a suffix. Renamed from kind_flags 2026-08-03 (finding 2026-08-02-1731-3). Written whole (dword) by llm_strat_ai_target_list_add from its `victim_ref` parameter (EDX, spilled to [EBP-0x10] @0x004d6d7b, stored @0x004d6df5), but every reader tests it as a BYTE. Bit 0xa0 selects the roster: llm_strat_ai_unit_should_abandon_target requires victim_ref & 0xa0 before treating victim_index as a UNIT index (@0x004ec8f9), and applies the identical 0xa0 test to aggressor_ref @0x004ec939 to choose between the units and buildings rosters. refresh_mothers tests &0x40 ('mother' class). Individual bit meanings beyond 0xa0 and the low player nibble are still unresolved. |
| `+0x08` | `ai_group_index` | `int` | Owning AI group index, from llm_strat_unit_get_ai_group_index(player, target_id); explicitly written 0 when player_data[].ai_enabled == 0. CAUTION (unresolved, AI-PREP 2026-08-01): llm_strat_ai_target_list_invalidate_by_id compares THIS column against its second argument, which its (low-confidence) name calls a target id. Writer and that one reader disagree about what the column holds; the writer is authoritative here. Resolve before relying on either reading -- see tasks/ai.md. |
| `+0x0c` | `aggressor_index` | `int` | Roster index of the ATTACKING object -- NOT a map position. Renamed position -> aggressor_index 2026-08-03 (finding 2026-08-02-1731-3, whose own proposal `aggressor_unit_index` is deliberately NOT used: see the roster note below). Written by llm_strat_ai_target_list_add from its `aggressor_index` stack parameter (EDI, [EBP+0x8] @0x004d6d81, stored @0x004d6e4f); together with aggressor_ref it forms the list's dedupe key (CMP @0x004d6d9a / @0x004d6da2). NOT A UNIT INDEX SPECIFICALLY -- the roster is selected by aggressor_ref & 0xa0: llm_strat_ai_unit_should_abandon_target indexes units[aggressor_ref & 0xf][aggressor_index] with the 0xe9 stride when the bit is set (@0x004ec95c-0x004ec96f) and buildings[aggressor_ref & 0xf][aggressor_index] with the 0x111 stride when it is clear (LAB_004ec99a, @0x004ec9aa-0x004ec9db). Three further sites read it as a WORD (0x004ebbc4, 0x004ee22c, 0x004ee294), consistent with a small roster index and not with a packed position. |
| `+0x10` | `aggressor_ref` | `uint` | PACKED OBJECT REF for the attacker, pairing with aggressor_index at +0x0c. Renamed from owner_class 2026-08-03 (finding 2026-08-02-1731-3). Written by llm_strat_ai_target_list_add from its `aggressor_ref` parameter (ECX, stored @0x004d6e5d); second half of the (aggressor_index, aggressor_ref) dedupe key. LOW NIBBLE IS THE OWNING PLAYER: target_list_add returns without appending when (aggressor_ref & 0xf) == player (@0x004d6d84-0x004d6d8b), i.e. the AI never records itself as an aggressor, and consumers use the same nibble as the roster's player index (@0x004ec95c, @0x004ec9b0). BIT 0xa0 SELECTS THE ROSTER for aggressor_index -- set means units, clear means buildings (@0x004ec939). Same encoding as victim_ref at +0x04. |

#### `llm_strat_dir8_offset` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dx` | `int` | signed tile-column delta for this compass direction (8-direction order matches llm_strat_dir_from_to's return codes) |
| `+0x04` | `dy` | `int` | signed tile-row delta for this compass direction, paired with dx at +4 |

#### `llm_strat_dir_remap_row` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `step_primary` | `int` | primary remapped step-offset index (0..31) into _G_LLM_STRAT_DIR_STEP_OFFSET_TABLE for this heading; used by llm_strat_tile_neighbor_in_dir (forward step) and FUN_0048b308 (reverse step) |
| `+0x04` | `step_alt1` | `int` | collinear alternate step index (= step_primary+16); one of the 4 heading-equivalent step dirs compared in llm_strat_path_write_from_solver |
| `+0x08` | `step_alt2` | `int` | collinear alternate step index (base rows: step_primary+8; diagonal-base rows cycle 12..15) |
| `+0x0c` | `step_alt3` | `int` | collinear alternate step index (= step_primary+24) |

#### `llm_strat_fx_anim` (size 0x15, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `ushort` | map px, >>5 = tile |
| `+0x02` | `y` | `ushort` |  |
| `+0x04` | `layer` | `byte` | draw pass 0/1/2 in llm_strat_render_view |
| `+0x05` | `live` | `int` | 1 = active; slot 0 header: this dword = live count (cap 9999) |
| `+0x09` | `timestamp` | `double` | last-update game clock |
| `+0x11` | `anim_frame` | `int` | index into cfg::final::data::Anim (chained via .next; .time==0 = permanent) |

#### `llm_strat_group_member` (size 0x6, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `unit_handle` | `uint` | member unit/object handle; passed to path-slot alloc (FUN_0049a1ce) and free (llm_strat_path_free_slot) |
| `+0x04` | `cur_col` | `byte` | member current tile column (X / major axis, index*0x100 into passable) |
| `+0x05` | `cur_row` | `byte` | member current tile row (Y / minor axis) |

#### `llm_strat_group_scratch_member` (size 0x14, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `tile_col` | `int` | Member's tile column. Written by llm_strat_group_move_register_member from units[p][i].x at registration; on a plain multi-member move llm_strat_group_move_order_commit overwrites it with the planner's DESTINATION column (_G_LLM_STRAT_GROUP_MEMBER_TILE) once the wave is committed, so the field means 'current' before the commit and 'target' after it. |
| `+0x04` | `tile_row` | `int` | Member's tile row; same lifecycle as tile_col (start row, then the planner's destination row). Read as a byte in places -- only the low byte is ever significant, tile coords are 0..255. |
| `+0x08` | `wave_rank` | `int` | Movement-wave / cluster rank, -1 = unassigned. llm_strat_pathfind_route_leg_group_and_sort seeds every row to -1, then walks the members in ascending distance-to-goal order and, for each still-unassigned one, bumps a running rank and calls llm_strat_claim_free_slots_within_dist to stamp that rank onto EVERY unassigned member within a distance threshold of it. The array is then qsorted on this field, so llm_strat_unit_state_group_marshal can commit the group one rank at a time -- these are the 'heading-classed waves' of the 0xa->0xb->0xc chain. |
| `+0x0c` | `unit_idx` | `int` | Index into units[player][] for this member. This is the field the former _G_LLM_STRAT_GROUP_MEMBER_UNIT_SCRATCH symbol pointed at (it was the array's 4th column mistaken for a standalone dword at 0x00ae2e54); consumers index it as (&SCRATCH)[i*5]. |
| `+0x10` | `saved_passable` | `int` | The member tile's passable[] value as it was BEFORE registration. llm_strat_group_move_register_member saves it here and then writes units[p][i].origin_tile_was_passable over passable[x][y], i.e. it temporarily lifts every group member off the collision map so the group pathfinder does not treat its own members as obstacles; llm_strat_unit_state_group_marshal restores this value once the wave is committed. Losing this field would make a group deadlock on itself. |

#### `llm_strat_heading_slot` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `cand_facing` | `int` | SLOT 0 of each heading's 3 is the only one read for this field. Small class id in {1,2,3} (init 1; the initializer raises some headings to 2 or 3). llm_strat_unit_group_step_ground reads TABLE[cur_unit->heading*3].cand_facing into a local (0x00483616) and (a) compares it for EQUALITY against TABLE[other_unit->heading*3].cand_facing to test whether two units' headings are compatible (0x004838c0), (b) switches on ==1/==2/==3 and REASSIGNS the local (0x00483c4c..0x00483caa). So it is a per-heading PATH-SHAPE CLASS, not a facing -- the name predates that reading and is not yet re-derived. NOT a per-slot field. |
| `+0x04` | `turn_delta` | `int` | Per-slot. Passed VERBATIM as llm_strat_trace_greedy_path's 3rd argument, `mode` (push at 0x004836b7/0x00483d62/0x00484454). -1 is the INACTIVE-SLOT SENTINEL: llm_strat_unit_group_step_ground guards each of the 3 slots with `> -1` (0x00483685 CMP / 0x0048368c JG) and skips the slot otherwise -- the slot loop bound is a fixed 3 (0x00483668), so this field, not a count, is what ends the candidate list. Initializer values are -1 (default) and 0..6. Whether `turn delta` or `path mode` is the better reading is OPEN -- it depends on trace_greedy_path's own `mode` parameter, not re-derived here. |
| `+0x08` | `start_col_delta` | `int` | Per-slot X (COLUMN) delta applied to the greedy-path START COLUMN. Re-derived 2026-08-20: the previous comment `unused/filler (init 0)` was WRONG -- llm_strat_unit_group_step_ground reads it at NINE sites (0x00483700, 0x004837cb, 0x00483a54, 0x00483db1, 0x00483e97, 0x00484126, 0x004844a3, 0x00484589, 0x00484818), each as `start_col = (local + start_col_delta) & general.width_mask` pushed as llm_strat_trace_greedy_path's 1st argument `start_col`. Symmetric with start_row_delta on the row axis; the WIDTH_MASK vs HEIGHT_MASK pairing is what fixes which axis is which. llm_strat_group_step_heading_table_init writes it 0/+1/-1 -- a tile step, not filler. |
| `+0x0c` | `start_row_delta` | `int` | Per-slot Y (ROW) delta applied to the greedy-path START ROW -- the twin of start_col_delta. Read at nine sites (0x004836d7, 0x004837fb, 0x00483a98, 0x00483d85, 0x00483eca, 0x0048416a, 0x00484477, 0x004845bc, 0x0048485c), each as `start_row = (local + start_row_delta) & general.height_mask` pushed as llm_strat_trace_greedy_path's 2nd argument `start_row`. Renamed from `override_val` 2026-08-20: that name recorded only that the initializer overrides it per heading, not what it means. Initializer values 0/+1/-1. |

#### `llm_strat_job_result_entry` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `path_steps` | `byte *` | pointer to the pathfinder job's packed path-step array (3 bytes/step: [0]=dir code (+1), [1]=aux; step byte 0xff terminates) |
| `+0x04` | `aux` | `undefined4` | unconfirmed second word of the 8-byte record |

#### `llm_strat_move_dir_step` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dir_code` | `byte` | 8-way heading code (0..7) this record represents |
| `+0x01` | `opposite_idx` | `byte` | record index of the reverse heading (not read by the tracer) |
| `+0x02` | `dcol` | `char` | column step, signed (-1/0/+1) |
| `+0x03` | `drow` | `char` | row step, signed (-1/0/+1) |
| `+0x04` | `next_straight` | `byte` | record index to continue straight ahead |
| `+0x05` | `turn_a` | `byte` | record index for turn-option A (one of the two flanking headings) |
| `+0x06` | `turn_b` | `byte` | record index for turn-option B |
| `+0x07` | `is_cardinal` | `byte` | 1=orthogonal N/E/S/W, 0=diagonal; 0 gates the air-mode altitude test |

#### `llm_strat_path_waypoint` (size 0x2, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `heading` | `byte` | direction/heading step code |
| `+0x01` | `run_length` | `byte` | consecutive-step count for this heading |

#### `llm_strat_pathfinder_params` (size 0x20, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `job_count` | `short` | RD-A readers readiness (2026-08-24). Number of goal/job entries in job_result_table -- always a THRESHOLD or a loop bound, never an index: CMP word [EAX],0x2 at 0x0043dc00 and the loop bound MOVSX at 0x0043dc5e in llm_strat_pathfind_route_start_adjust; CMP word [EAX],0x0 / 0x1 at 0x0043df3f / 0x0043df54 and MOVSX loop bounds at 0x0043dfa4 / 0x0043e063 / 0x0043e28c in llm_strat_pathfind_dispatch_route_order; the != 0 gate at 0x0055e2b6 in llm_pf_seed_goals, which then walks that many 8-byte table entries. |
| `+0x02` | `seed_row` | `ushort` | RD-A readers readiness (2026-08-24). Row of the cell the flood fill is ROOTED at. llm_pf_seed_goals reads it with +0x04 at 0x0055e2c0 / 0x0055e2c4 and packs the pair into the single BFS seed cell. Deliberately NOT named goal_y: llm_strat_group_move_request_build writes the shared destination here (0x00494f86) but llm_strat_pathfind_next_step writes the QUERYING UNIT's own position (0x0043cfa3) and searches in reverse, so `goal` would be wrong for half the call sites. |
| `+0x04` | `seed_col` | `ushort` | RD-A readers readiness (2026-08-24). Column half of seed_row; writes at 0x00494f93 (group move, the destination) and 0x0043cfaa (single-step wrapper, the unit's own column). |
| `+0x06` | `passable` | `pointer` | RD-A readers readiness (2026-08-24). -> map::g::passable (0x00b64bb0, byte[256][256]). Written by every producer -- llm_strat_pathfinder_init 0x004615b3, llm_strat_pathfinder_ctx_link 0x0046160c, llm_strat_group_move_request_build 0x00494f76, llm_strat_pathfind_next_step 0x0043cfcc -- and read by llm_strat_pathfind_dispatch_route_order (0x0043e194) and llm_pf_seed_goals (0x0055e30a, the source for the working-plane copy). UNALIGNED at +6: the whole record is a packed 0x20-byte malloc. |
| `+0x0a` | `job_result_table` | `pointer` | RD-A readers readiness (2026-08-24). -> _G_LLM_STRAT_PATH_JOB_RESULT_TABLE (0x00ae1958), 100 entries of 8 bytes, each with a packed goal col:row at entry+0x06. Written at 0x004615cd / 0x00461626, read back at 0x00494fc0, and consumed per-entry by llm_pf_seed_goals (0x0055e33b, iterated job_count times) and by llm_strat_pathfind_route_start_adjust (0x0043dc15). The next_step wrapper writes an entry's goal sub-field at 0x0043cfc8, closing the round trip. |
| `+0x0e` | `workbuf` | `pointer` | RD-A readers readiness (2026-08-24). -> map::g::general.pathfinder_workbuf, 0x82480 bytes, zeroed at init and again by llm_strat_pathfinder_ctx_link. Written at 0x004615c5 / 0x0046161e; read at 0x0043e1ac and at 0x0055e2d1, where llm_pf_seed_goals uses it as the base for five working planes at +0x20200 / +0x40400 / +0x50400 / +0x60400. |
| `+0x12` | `flags` | `uint` | RD-A readers readiness (2026-08-24). Mode/dispatch flags, derived by the caller from its own flags & 0x8000 / & 0x4000 (write 0x00494fb8). llm_strat_pathfind_dispatch_route_order branches on it (direct flood search vs route_start_adjust first) and resets it to 0 at 0x0043df35 / 0x0043df5d. Note Ghidra's decompiler renders this access as local_30[10]; that index is wrong -- the disassembly displacement is +0x12. |
| `+0x16` | `width_mask` | `byte` | RD-A readers readiness (2026-08-24). Map width MINUS ONE, written by llm_map_setup_dimensions (MOV byte [EAX+0x16],DL at 0x00498a2d after DEC DL). Named mask rather than width_minus_one because it is genuinely used as one: llm_strat_pathfind_route_start_adjust ANDs both adjusted coordinates with it before packing its return value (0x0043dd96 / 0x0043dd99 and 0x0043dd9f / 0x0043dda2) -- a toroidal power-of-two wrap. Also read as a size-class switch key by llm_pf_select_resolution (CMP against 0x0f / 0x1f / 0x3f / 0x7f / 0xff, five LOD tables 0x400 apart -- which only works if width is a power of two), as a symmetric delta-wrap bound in the same route_start_adjust, and INC'd back to a plain width by llm_pf_seed_goals at 0x0055e31a. |
| `+0x17` | `mode_flag` | `byte` | RD-A readers readiness (2026-08-24). 0/1, written at 0x00494fac from the same caller flag decode as `flags`. Gates group-scatter vs direct/individual dispatch in llm_strat_pathfind_dispatch_route_order (CMP byte [EAX+0x17],0x1 at 0x0043df2c / 0x0043df67 / 0x0043dfe6 / 0x0043e01f / 0x0043e248) and is read again by the search engine itself, llm_pf_run at 0x0055e1ed. |
| `+0x18` | `start_pos_packed` | `ushort` | RD-A readers readiness (2026-08-24). IN/OUT search cursor, packed high byte = col, low byte = row (same packing as next_step_packed). Written by llm_strat_pathfind_dispatch_route_order at 0x0043e040 from llm_strat_pathfind_route_start_adjust's return -- the wrap-corrected start nudged toward the next waypoint -- then read at entry (0x00583419) and written back at exit by llm_strat_pathfind_flood_search_driver. |
| `+0x1a` | `next_step_packed` | `ushort` | RD-A readers readiness (2026-08-24). OUT: the computed next step, high byte = col, low byte = row. Written by llm_pf_run at 0x0055e229; llm_strat_pathfind_next_step decodes it through llm_strat_pathfind_result_col ((w>>8)&0xff) and llm_strat_pathfind_result_row (w&0xff). |
| `+0x1c` | `status` | `uint` | RD-A readers readiness (2026-08-24). OUT: 0 = no path, nonzero = success. Zeroed at engine entry (0x0055e1dd) and set from the search's success flag at engine exit (0x0055e234), both in llm_pf_run; read by llm_strat_pathfind_next_step at 0x0043cfdb to drive its own success/failure return. Covers 0x1c..0x1f in full -- there is no separate field at 0x1e. |

#### `llm_strat_pathtrace_dir_split` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `src_dir` | `byte` | source diagonal heading (echo of the index dir_code) |
| `+0x01` | `reverse_dir` | `byte` | source heading + reverse offset (unused by normalize) |
| `+0x02` | `src_dir_2` | `byte` | echo of src_dir |
| `+0x03` | `new_dir_a` | `byte` | first replacement orthogonal heading -> PATHTRACE_DIRS[i] |
| `+0x04` | `src_dir_3` | `byte` | echo of src_dir |
| `+0x05` | `new_dir_b` | `byte` | second replacement orthogonal heading -> PATHTRACE_DIRS[i+2] |
| `+0x06` | `delta_col` | `char` | signed column delta added to PATHTRACE_POS[i+1] |
| `+0x07` | `delta_row` | `char` | signed row delta added to PATHTRACE_POS[i+1] |

#### `llm_strat_power_stats` (size 0x18, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `generated` | `int` | SIM1B (2026-08-12; building_tick machinery slice). This tick's total power generated by player's power plants, accumulated elsewhere (outside this batch) before llm_strat_power_recompute reads it. Derived from tmp/decomp/llm_strat_power_recompute_00491260.asm (base 0xbf4fc0, stride 0x18). |
| `+0x04` | `consumed` | `int` | SIM1B (2026-08-12). This tick's total power consumed by player's buildings, accumulated elsewhere before llm_strat_power_recompute reads it. |
| `+0x08` | `prev_generated` | `int` | SIM1B (2026-08-12). Latched copy of `generated`, written by llm_strat_power_recompute at the end of each recompute (its own read-then-write, not consumed by it). |
| `+0x0c` | `prev_consumed` | `int` | SIM1B (2026-08-12). Latched copy of `consumed`, same lifecycle as prev_generated. |
| `+0x10` | `ratio` | `double` | SIM1B (2026-08-12). The brownout factor: (generated+1)/(consumed+1), clamped to a maximum of 1.0. llm_strat_refresh_all_buildings -> llm_strat_refresh_building applies this per building to gate operational/efficiency state. |

#### `llm_strat_projectile` (size 0x79, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `active` | `int` | 1 = live, 0 = free; slot 0 header: this dword = live count |
| `+0x04` | `weapon_id` | `int` | index into cfg::final::data::Weapon (stride 0x16c) |
| `+0x08` | `x` | `ushort` | current x (map px, wraps mod big_width) |
| `+0x0a` | `y` | `ushort` | current y (elevation-adjusted, wraps mod big_height) |
| `+0x0c` | `src_x` | `ushort` |  |
| `+0x0e` | `src_y_ground` | `ushort` | source y without elevation |
| `+0x10` | `src_y_vis` | `ushort` | source y minus shooter elevation (visual launch) |
| `+0x12` | `dst_x` | `ushort` | retargeted each tick if homing |
| `+0x14` | `dst_y_ground` | `ushort` |  |
| `+0x16` | `dst_y_vis` | `ushort` | target y minus target elevation (visual impact) |
| `+0x18` | `owner_player` | `byte` | passed into explosion FX and damage |
| `+0x19` | `delta_x` | `double` | wrapped flight delta src->dst |
| `+0x21` | `delta_y` | `double` |  |
| `+0x29` | `launch_time` | `double` | _G_LLM_STRAT_GAME_CLOCK at fire |
| `+0x31` | `duration` | `double` | sqrt(dist)*Weapon.speed/Weapon.length; recomputed when homing |
| `+0x39` | `anim_clock` | `double` | last bullet-anim advance |
| `+0x41` | `smoke_clock` | `double` | next smoke-puff emission |
| `+0x49` | `explo_pulse_clock` | `double` | next in-flight damage pulse (Weapon.explo_time) |
| `+0x51` | `scatter_x` | `int` | total miss offset (from Weapon.missing), applied over flight |
| `+0x55` | `scatter_y` | `int` |  |
| `+0x59` | `facing` | `int` | 24-dir; for Weapon.type 8: random index into cos/sin table 0xe15848 |
| `+0x5d` | `anim_frame` | `int` | index into cfg::final::data::Anim |
| `+0x61` | `smoke_sprite` | `int` | cached; 0 = no smoke trail |
| `+0x65` | `explo_pulse_flag` | `int` | explodes periodically along path |
| `+0x69` | `anim_loop_frame` | `int` | loop-restart base frame for this direction |
| `+0x6d` | `homing_player` | `ushort` | 0 if not homing |
| `+0x6f` | `homing_unit` | `int` | target unit index, 0 = none |
| `+0x73` | `shooter_ref` | `ushort` | low nibble = player, bit 0x80 = fired by unit (vs turret) |
| `+0x75` | `shooter_unit` | `int` | kill credit / self-damage exclusion |

#### `llm_strat_route_step` (size 0x2, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dir_code` | `byte` | direction code; index into the compact dir-step (dx,dy) table at 0x0051de40 (DAT_0051de40=dx, DAT_0051de41=dy, stride 2) |
| `+0x01` | `run_length` | `byte` | consecutive steps taken in this direction; a run_length of 0 terminates the route list |

#### `llm_strat_squad_formation_anchor_scratch` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `x` | `int` |  |
| `+0x04` | `y` | `int` |  |

#### `llm_strat_ui_base_marker_coord` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `cam_col` | `int` | Saved minimap camera COLUMN for one HUD go-to-base marker slot. -1 = unset (all 8 slots are initialised to -1 by llm_ui_bldg_panel_open @0x0041c096 and llm_strat_ui_panel_init @0x0041ae55). |
| `+0x04` | `cam_row` | `int` | Saved minimap camera ROW for the same slot. Written beside cam_col at 0x00479b08 (llm_strat_bldg_completion_dispatch, slot 0, masked by general.height_mask) and 0x004556c9 (llm_game_land_players_on_planet, slot 0 = this planet's landing spot); read by llm_strat_minimap_cam_goto's jump-to-marker arm @0x00427b90. |

#### `llm_strat_unit_housing_stats` (size 0x40, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `used_vehicles` | `int` | ground vehicles alive (unit types 11-14) |
| `+0x04` | `used_soldiers` | `int` | soldiers alive (types 1-10, weighted by Unit.soldier_count) |
| `+0x08` | `used_planes` | `int` | planes alive (types 17-18) |
| `+0x0c` | `used_helis` | `int` | helicopters alive (types 15-16) |
| `+0x10` | `cap_prev_vehicles` | `int` | latched capacity |
| `+0x14` | `cap_prev_soldiers` | `int` | latched capacity |
| `+0x18` | `cap_prev_planes` | `int` | latched capacity |
| `+0x1c` | `cap_prev_helis` | `int` | latched capacity |
| `+0x20` | `cap_accum_vehicles` | `int` | fed by llm_strat_add_unit_capacity_vehicles (bldg 7 KOSZARYp) |
| `+0x24` | `cap_accum_soldiers` | `int` | fed by llm_strat_add_unit_capacity_soldiers (bldg 8 KOSZARYz) |
| `+0x28` | `cap_accum_planes` | `int` | fed by llm_strat_add_unit_capacity_planes (airfield) |
| `+0x2c` | `cap_accum_helis` | `int` | fed by llm_strat_add_unit_capacity_helis (helipad) |
| `+0x30` | `rr_cursor_vehicles` | `int` | SIM1D third slice (2026-08-14; llm_strat_storage_find_home_for_unit). Persistent per-player storage-slot round-robin cursor for VEHICLE-class units (cfg unit type 11-14, matching used_vehicles/cap_*_vehicles above). Independently re-derived from the raw range-comparison chain over Unit[unit_type].type: [1,10]=soldiers (+0x34), [11,14]=vehicles (this field), [15,16]=helis (+0x3c), [17,18]=planes (+0x38), [19,24]=none (early-bail path) -- NOT the same field order as the used_*/cap_*_* quads (vehicle,soldier,plane,heli); this quad is vehicle/soldier/heli/plane in memory order. Wraps at slot 24 -> 1 (0x463429); see docs/hardcoded-limits.md 'The split round-robin' for the known scan-budget desync bug this cursor is part of. |
| `+0x34` | `rr_cursor_soldiers` | `int` | SIM1D third slice (2026-08-14). See rr_cursor_vehicles. SOLDIER-class round-robin cursor (cfg unit type 1-10, matching used_soldiers/cap_*_soldiers above). Derived from CMP unit_type_val,0x1 (JC skip=UNDEFINED) / CMP unit_type_val,0xa; JBE -> this field. |
| `+0x38` | `rr_cursor_planes` | `int` | SIM1D third slice (2026-08-14). See rr_cursor_vehicles. PLANE-class round-robin cursor (cfg unit type 17-18, matching used_planes/cap_*_planes above). Derived from CMP unit_type_val,0x12(18); JBE -> this field, reached only after the type-15/16 (heli) branch already failed. |
| `+0x3c` | `rr_cursor_helis` | `int` | SIM1D third slice (2026-08-14). See rr_cursor_vehicles. HELI-class round-robin cursor (cfg unit type 15-16, matching used_helis/cap_*_helis above). Derived from CMP unit_type_val,0xf(15); JC skip / CMP unit_type_val,0x10(16); JBE -> this field. |

#### `llm_strat_unitq_search_node` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `tile` | `uint` | packed tile linear index = x*256 + y (byte0=y, byte1=x); flat index into passable[] and tile_objects[0][] |
| `+0x04` | `dir` | `ushort` | chosen 8-neighbor direction 0..7; indexes _G_LLM_UNITQ_NEIGHBOR_DX / _DY |
| `+0x06` | `unit` | `ushort` | unit slot id at this node (order param_2 handed to llm_strat_order_issue_0xf_adjacent_by_offset) |

#### `llm_tact_anim_frame_range` (size 0x3, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `start` | `ushort` | First sprite frame of this animation state. Slot 0 is seeded from the character type's first_frame; every later slot accumulates start[i] = start[i-1] + count[i-1]*8 (llm_tact_character_parse_frame_table 0x0043a649, the *8 being the eight facing directions per animation frame). |
| `+0x02` | `count` | `byte` | Number of animation frames in this state, atoi'd straight from the mission-script FRAMES list token. |

#### `llm_tact_character_type` (size 0x6c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char[21]` | CHARACTER NAME token |
| `+0x15` | `id` | `byte` | record index / in-use marker (0=empty) |
| `+0x16` | `who` | `byte` | WHO: allegiance side; 0=player squad, else enemy. Stored value XOR _G_LLM_TACT_WHO_XOR_KEY |
| `+0x17` | `angle_see` | `short` | ANGLE SEE |
| `+0x19` | `distance_see` | `byte` | DISTANCE SEE |
| `+0x1a` | `first_frame` | `short` | FIRST FRAME (BANK sprite base + value) |
| `+0x1c` | `number_gun1` | `byte` | NUMBER GUN1 |
| `+0x1d` | `number_gun2` | `byte` | NUMBER GUN2 |
| `+0x1e` | `height_gun1` | `byte` | HEIGHT GUN1 |
| `+0x1f` | `height_gun2` | `byte` | HEIGHT GUN2 |
| `+0x20` | `kneel_gun1` | `byte` | KNEEL GUN1 |
| `+0x21` | `kneel_gun2` | `byte` | KNEEL GUN2 |
| `+0x22` | `energy` | `short` | ENERGY (unit HP) |
| `+0x24` | `speed` | `double` | SPEED |
| `+0x2c` | `rotate` | `double` | ROTATE |
| `+0x34` | `kneel_time` | `double` | KNEEL |
| `+0x3c` | `death_time` | `double` | DEATH |
| `+0x44` | `mine_time` | `double` | MINE |
| `+0x4c` | `run_speed` | `double` | RUN |
| `+0x54` | `frames` | `llm_tact_anim_frame_range[8]` | Per-animation-state frame ranges, 8 x 3 bytes = the 24 bytes previously typed byte[24]. llm_tact_character_parse_frame_table (0x0043a649) fills SIX of the eight from a brace-delimited mission-script list: slot 0's start comes from first_frame (+0x1a), each later start accumulates prev_start + prev_count*8, and every slot's count is atoi'd from its token. Slots 6 and 7 are left as loaded. The 3-byte stride is what the parser's `frames + i*3`, `frames[i*3 - 1]` and `frames[i*3 + 2]` arithmetic was expressing. Typed 2026-08-24; the byte[24] form is what the todo:struct lead on the parser was about. |

#### `llm_tact_door` (size 0x68, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `id` | `byte` | record index / in-use marker |
| `+0x01` | `direct_mode` | `byte` | DIRECT_LeftRight=0 / RightLeft=1 / Normal=2 |
| `+0x02` | `left_frame_count` | `ushort` | Animation frame count of the LEFT leaf = (entries parsed - 2) / left_col_count, computed at end-of-parse by llm_tact_door_parse_definition (fill 0x00439abb/0x00439ae6, finalize 0x00439b99-0x00439bb1) and zeroed at mission load (0x00437be2). NO READER FOUND anywhere in the exported tactical set: door_tick bounds the animation on right_frame_count (+0x26) only, so the left leaf's own count is vestigial. Structural twin of right_frame_count -- named symmetrically on purpose. |
| `+0x04` | `left_col_count` | `ushort` | Columns per row of left_rows, and the raw first token of a LEFT line; also the IDIV divisor that produces left_frame_count (0x00439b71/0x00439b99). Loop bound in llm_tact_door_apply_to_map (0x00439e83), llm_tact_door_update_tile_state (0x00432788/0x004327a1) and llm_tact_door_path_clear (0x00432af1). |
| `+0x06` | `left_rows` | `ushort[16]` | Flat per-row tile/sprite index list for the LEFT leaf, row selected as (frame_index-1)*left_col_count + j. Read at 0x00439ea7 (apply_to_map) and 0x004327ca (update_tile_state). Shape is certain; that the entries are door-leaf silhouette indices rather than a generic wall mask is a LEAD (the sprite bank 20 asset was not traced). |
| `+0x26` | `right_frame_count` | `ushort` | Animation frame count of the RIGHT leaf, same computation as left_frame_count (fill 0x00439a4a-0x00439a67, finalize 0x00439b02-0x00439b4d, zeroed 0x00437bc8) -- AND the one the animation actually uses: llm_tact_door_tick loads it at 0x00432332 and compares it against frame_index at 0x00432339 to detect 'fully open' (state 1 -> 2). |
| `+0x28` | `right_col_count` | `ushort` | Columns per row of right_rows. Loop bound in llm_tact_door_apply_to_map (0x00439d62), llm_tact_door_update_tile_state (0x0043251f) and llm_tact_door_path_clear (0x00432a47). Zeroed at mission load (0x00437bd5). |
| `+0x2a` | `right_rows` | `ushort[16]` | Flat per-row tile/sprite index list for the RIGHT leaf; same shape and indexing as left_rows. Read at 0x00439d86 (apply_to_map) and 0x00432548 (update_tile_state). PROVEN to be a field of THIS record, not of a map-tile struct: every access is IMUL reg,door_idx,0x68 then [reg + 0x8742ee] (= table base + 0x2a) with the base folded into the displacement -- the Watcom global-array-index idiom. |
| `+0x4a` | `speed` | `double` | SPEED |
| `+0x52` | `tile_x` | `ushort` | X (from 'X,Y') |
| `+0x54` | `tile_y` | `ushort` | Y (from 'X,Y') |
| `+0x56` | `state` | `byte` | Door animation state machine: 0 idle/closed, 1 opening, 2 open/holding, 3 closing. llm_tact_door_anim_start requires state==0 && frame_index==0 (0x00432285/0x00432292) then sets 1 (0x004322a1). llm_tact_door_tick: state==1 (0x00432316) advances and sets 2 on completion (0x00432341); state==2 (0x004323ba) waits out _G_LLM_TACT_DOOR_OPEN_HOLD_TIME_SEC then INCs to 3 (0x004323e7); state==3 (0x00432400) retreats and resets to 0 (0x00432442). No other value is ever compared. Renamed from field_73_0x56 2026-08-24; ENUM candidate (llm_tact_door_state). |
| `+0x57` | `frame_index` | `byte` | Current animation step. 0 while idle; INC toward right_frame_count while opening (0x00432389); DEC back toward 1 while closing (0x00432473); reset to 0 on close-complete (0x00432437). Also the ROW selector into left_rows/right_rows in llm_tact_door_update_tile_state (0x004324fb, 0x0043277d). Renamed from field_57; the old comment 'init 0 -- unverified' is now settled. |
| `+0x58` | `last_step_time` | `double` | time_GetCurrentTime() at the last frame advance (or at opening start). Written by llm_tact_door_anim_start (0x004322b1) and at every advance in llm_tact_door_tick (0x004323ab, 0x004323f6, 0x00432495, 0x004324a6); the next advance is gated on last_step_time + speed vs now (0x0043236f, 0x00432459). |
| `+0x60` | `opened_at_time` | `double` | time_GetCurrentTime() at the moment the door reached fully-open (state 1 -> 2), stored at 0x00432351. Compared against now plus _G_LLM_TACT_DOOR_OPEN_HOLD_TIME_SEC (0x005004a4) at 0x004323cd to decide when the auto-close (state 2 -> 3) begins. |

#### `llm_tact_fx` (size 0x52, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `fx_type` | `byte` | FX/effect type id; indexes _G_LLM_TACT_FX_TYPE_TABLE. ALSO the slot-occupied flag: 0 = free slot (alloc scan looks for the first 0). type<0x20 = moving projectile, >=0x20 = pure animation effect |
| `+0x01` | `owner` | `byte` | owning player/side (from firing unit's .who); used for friendly-fire exclusion in collision |
| `+0x02` | `pos_x` | `double` | current position, fine X coord (world). Advances by vel_x each move step |
| `+0x0a` | `pos_y` | `double` | current position, fine Y coord (world). Advances by vel_y each move step |
| `+0x12` | `travel_dx` | `double` | accumulated X displacement since spawn (+= vel_x per step); sqrt(travel_dx^2+travel_dy^2) checked vs fx_type.range_max. Init 0 by llm_tact_fx_spawn (0x0042bdce), and ONLY ADVANCED for fx_type<0x20 (llm_tact_fx_update_projectile @0x00431654, gated 0x004316b7). For fx_type>=0x20 (pure animation effect) it is written once at spawn and never again, so it stays 0x00 for the entry's whole life. llm_tact_tile_rebuild_occupancy_layer (0x00433dfc) reads this field's FIRST THREE RAW BYTES (+0x12/+0x13/+0x14) as (col,row,stamp) for tile_objects[col][row].unit stamping (0x00433e41-0x00433e4c), for EVERY active fx entry owned by the reload map_id regardless of fx_type -- there is no evidence this is deliberate col/row encoding rather than an incidental byte-scavenge off an accumulator that happens to be 0 for the animation-effect population and near-zero-magnitude mantissa bytes for the projectile population. Preserved literally (Law 2), not corrected. |
| `+0x1a` | `travel_dy` | `double` | accumulated Y displacement since spawn (+= vel_y per step). Init 0 |
| `+0x22` | `tile_col` | `byte` | current tile column = pos_x >> 5 (recomputed each tick). map tile coord stored as byte |
| `+0x23` | `tile_row` | `byte` | current tile row = pos_y / 24 (recomputed each tick) |
| `+0x24` | `dir24` | `byte` | facing/travel direction 1..24 (llm_tact_calc_dir24; 1 when spawn==target). Drives directional anim frame + ricochet turn |
| `+0x25` | `altitude` | `byte` | projectile altitude/height class; gates whether it collides with ground units (<0x1e passes over some states) |
| `+0x26` | `move_clock` | `double` | next-move timestamp (spawn = time::GetCurrentTime); each step += fx_type.speed while move_clock+speed < now (catch-up stepping) |
| `+0x2e` | `frame_counter` | `short` | animation frame counter; ++ per tick, wraps to 0 at fx_type.frames |
| `+0x30` | `sprite_frame` | `ushort` | computed sprite frame index = (dir-based row)*frames + frame_counter for directional fx, else = frame_counter |
| `+0x32` | `target_x` | `double` | destination fine X coord (x2) |
| `+0x3a` | `target_y` | `double` | destination fine Y coord (y2) |
| `+0x42` | `vel_x` | `double` | per-step X velocity = normalized (target_x-pos_x)/dist (unit direction); 0 when spawn==target |
| `+0x4a` | `vel_y` | `double` | per-step Y velocity = normalized (target_y-pos_y)/dist; 0 when spawn==target |

#### `llm_tact_fx_type` (size 0x4a, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char[21]` | GUN/EXPLOSION NAME token |
| `+0x15` | `id` | `byte` | record index (guns 0..0x1f; explosions 0x20..0x3f) |
| `+0x16` | `speed_fire` | `double` | SPEED_FIRE |
| `+0x1e` | `speed` | `double` | SPEED (projectile) |
| `+0x26` | `bullets` | `byte` | BULLETS |
| `+0x27` | `magazines` | `byte` | MAGAZINES |
| `+0x28` | `repeat` | `double` | REPEAT |
| `+0x30` | `precise` | `byte` | PRECISE |
| `+0x31` | `precise_kneel` | `byte` | PRECISE_KNEEL |
| `+0x32` | `first_frame` | `short` | FIRST FRAME (sprite base + value) |
| `+0x34` | `direct` | `byte` | DIRECT |
| `+0x35` | `frames` | `short` | FRAMES |
| `+0x37` | `power` | `byte` | POWER |
| `+0x38` | `colision1` | `byte` | COLISION1 (+32.0 bias) |
| `+0x39` | `colision2` | `byte` | COLISION2 (+32.0 bias) |
| `+0x3a` | `range_max` | `int` | RANGE_MAX |
| `+0x3e` | `range_min` | `int` | RANGE_MIN |
| `+0x42` | `sound` | `int` | SOUND |
| `+0x46` | `range_kill` | `int` | RANGE_KILL |

#### `llm_tact_panel_icon_name` (size 0xa, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `char[10]` | NUL-padded resource-name suffix; llm_tact_ui_sel_panel_init formats it into "panelb\%s.gfx" (0x00433ed4) and loads the result via GetResourseFilePtr. Read-memory confirmed: gora, dol, nowe_pol, zaklad_1, ... |

#### `llm_tact_teleport` (size 0x32, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `id` | `byte` | record index / in-use marker |
| `+0x01` | `mode` | `byte` | RANDOM=1 / SEQUENCE=2 |
| `+0x02` | `no_enemy` | `byte` | 1 if NO_ENEMY flagged |
| `+0x03` | `field_03` | `byte` | cleared at init -- unverified |
| `+0x04` | `start_col` | `ushort` | START tile col |
| `+0x06` | `start_row` | `ushort` | START tile row |
| `+0x08` | `dest_col` | `ushort[8]` | WHERE destination cols |
| `+0x18` | `dest_row` | `ushort[8]` | WHERE destination rows |
| `+0x28` | `field_28` | `byte` | cleared at init -- unverified |
| `+0x29` | `association` | `byte[8]` | ASSOCIATION ids |
| `+0x31` | `death` | `byte` | DEATH (+32.0 bias) |

#### `llm_tact_tile_overlay` (size 0x2, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `strat_lo` | `byte` | NOT part of this view -- the low 8 bits of the strategic map_t_unit_full_id.unit_id. No tactical overlay code reads or writes it. |
| `+0x01` | `layers` | `byte` | Two 4-bit overlay sprite indices, read BYTE-WIDE with varying masks (which is why a nibble bitfield would not match the code). 0 = nothing drawn. LOW nibble (&0x0f): ground-bank sprite, id = n + 0x1e (llm_tact_render_view 0x0042d563). HIGH nibble (&0xf0)>>4: top-bank sprite, id = n + 0x28 (0x0042d5c6). ESCAPE: if the top TWO bits are both set ((b&0xc0)==0xc0, tested 0x0042d4c2) the byte is instead one 6-bit id, sprite = (b&0x3f) + 0x32 (0x0042d4fa) -- so high-nibble values 0xc..0xf are reserved and the nibble reading does not apply. WRITER: llm_tact_move_path_preview_walk stamps a movement arrow -- high nibble = (heading/3)+1 (0x0042eb42..), low nibble ORed with the same value for headings >= 10; heading/3 folds the 24-direction compass to 8, so 1..8 and never reaches the 0xc escape. |

#### `llm_tact_unit_cmd_entry` (size 0xb, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `interrupt_flag` | `byte` | Per-entry 'who issued this / may it be overridden' byte -- the entry's LEADING member, which is why the queue is based at tact_unit_record+0x54 and not +0x55. Written from llm_tact_unit_enqueue_command's 3rd param (register BL) at 0x0042b898, in the same six-store block that writes op and arg0..arg3 at the SAME slot index. Read back verbatim by llm_tact_unit_cmd_queue_resubmit_run (0x00430712). Both enqueue (0x0042b400) and llm_tact_unit_owner_tick (0x0043365e) refuse to act on the current slot when it is 0 while op != 0 and move_retry_wait == 0, so 0 marks a protected/uninterruptible command. The shipped debug overlay calls it `who`: llm_tact_render_view prints it beside op with the format at 0x00500338, "%2d. nr.com:%2d com:%2d who:%1d". Call-site polarity reads as issuer rather than a plain flag (player input -> 0, AI owner tick and mission load -> 1), which is why the name is the parameter's and the reading is left stated rather than settled. NOT cleared by llm_tact_unit_cmd_advance, which zeroes op and the four args only (0x004312b4-0x0043130c) -- so this byte is STALE whenever op == 0. Harmless today because every guard pairs it with op != 0, but a reimplementation that zero-fills the whole entry on dequeue would diverge on a byte-compare of the queue region. |
| `+0x01` | `op` | `ushort` | Command opcode; 0 = slot free, and that is the occupancy test everywhere. 1 move, 2 attack/aim, 4 kneel, 5 stand, 6 face/turn, 7 turn-then-attack, 8 timed wait, 9 mine-arm, 0xa teleport-jump, 0xb advance-with-defstat, 0x1c/0x1e markers, 0x1d/0x1f stance toggle, 0x40 run, 0x46 clear, 0x47 advance/repeat, 0x7f stop/interrupt. Full table: docs/fx-weapons.md 'Command queue op codes'. |
| `+0x03` | `arg0` | `ushort` | Per-op. op 1 (MOVE): destination tile COLUMN (read at 0x0042fe66, passed to llm_tact_move_step_attempt). op 6: direction 1..0x18. op 0x47: the run length written at 0x0042b55b and read back as the resubmit count at 0x004306d5. |
| `+0x05` | `arg1` | `ushort` | Per-op. op 1 (MOVE): destination tile ROW (read at 0x0042fe52). |
| `+0x07` | `arg2` | `ushort` | Per-op. For op 1 (MOVE) this is SCRATCH, not an input: rewritten from _G_LLM_TACT_MOVE_FLOOD_RESULT_COL on each pathfind (0x0042ff1c/0x0042ff31, 0x0043006e-0x0043008a). arg0 == arg2 && arg1 == arg3 is the ARRIVAL test (0x004300e7-0x00430118, which then calls llm_tact_unit_cmd_advance). |
| `+0x09` | `arg3` | `ushort` | Per-op. For op 1 (MOVE) the scratch counterpart of arg2, from _G_LLM_TACT_MOVE_FLOOD_RESULT_ROW (0x0042ff38/0x0042ff4d). |

#### `llm_tact_unit_roster_slot` (size 0x100, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `count` | `int` | How many unit ids follow. Incremented in place by llm_tact_squad_roster_refresh (INC at 0x00435772 / 0x00435792) before the id is stored, so ids[] is 1-based off this header. |
| `+0x04` | `ids` | `int[63]` | Unit ids (indices into _G_LLM_TACT_UNITS). Stored at 0x00435784 / 0x004357b2. The whole slot is zeroed by utils_fill_data before each rebuild. |

#### `llm_tutorial_step` (size 0x60c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `title` | `wchar_t[64]` | inline caption from the '%d <title>' step-header line (loader FUN_004de1fd wide-copy) |
| `+0x80` | `body` | `wchar_t[512]` | 'Tekst' section dialog body (loader FUN_004b8cb1); shown by driver widget 650f63 -- NOTE the driver reads it with a buggy 0x306 half-stride (correct only for step 1) |
| `+0x480` | `reakcje` | `llm_tutorial_step_op[4]` | objective/completion-condition ops ('Reakcje' section); driver detects step completion |
| `+0x504` | `panel` | `llm_tutorial_step_op[4]` | UI gate/limit ops ('Panel' section); driver applies autopage/card-allow-list/rmb limits |
| `+0x588` | `komenda` | `llm_tutorial_step_op[4]` | scripted AI-command ops ('Komenda' section); driver fires AI_ATTACK (opcode 0xe) |

#### `llm_tutorial_step_op` (size 0x21, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `opcode` | `byte` | list-specific opcode, 0=end-of-list. Reakcje:2 select,7 produced,8 isout,9 killed,0xa exist,0xb build,0xc placing,0xd destroyed. Panel:1 sel_limit,3 bld_limit,4 card_limit,5 rmb_limit,6 back_sel. Komenda:0xe ai_attack |
| `+0x01` | `operands` | `cfg_enum_E_UNIT_TYPE[8]` | unaligned (starts at +1); 0(UNDEFINED)-terminated list of building/unit type codes the op applies to |

#### `llm_ui_avi_stream_info` (size 0xb8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `stream` | `void *` | AVI stream handle (PAVISTREAM); set by llm_ui_avi_open_stream, closed by llm_ui_avi_stream_end |
| `+0x04` | `_pad1` | `undefined1[32]` | unmapped |
| `+0x24` | `rate_divisor` | `uint` | divisor against stream_length_time in llm_ui_avi_stream_begin |
| `+0x28` | `_pad2` | `undefined1[104]` | unmapped |
| `+0x90` | `format_buf_a` | `void *` | malloc'd format chunk (1st AVIStreamReadFormat); freed in llm_ui_avi_stream_end |
| `+0x94` | `format_buf_a_size` | `uint` | size for format_buf_a |
| `+0x98` | `format_buf_b` | `void *` | malloc'd format chunk (2nd AVIStreamReadFormat); freed in stream_end |
| `+0x9c` | `format_buf_b_size` | `uint` | size for format_buf_b |
| `+0xa0` | `decoded_buffer` | `void *` | malloc'd decode buffer (audio path feeds ACMSTREAMHEADER.pbSrc) |
| `+0xa4` | `decoded_buffer_size` | `uint` | size of decoded_buffer |
| `+0xa8` | `stream_length_time` | `int` | AVIStreamSampleToTime() result |
| `+0xac` | `stream_length_units` | `int` | stream_length_time / rate_divisor |
| `+0xb0` | `_pad3` | `undefined1[4]` | unmapped |
| `+0xb4` | `playback_position` | `int` | zeroed at AVIStreamBeginStreaming |

#### `llm_ui_bldg_worker_adjust_btn` (size 0x14, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `direction` | `int` | worker delta direction: >=1 => assign workers, <1 => unassign (read by the apply helper) |
| `+0x04` | `icon_x_offset` | `int` | x offset added to the panel base position for the button icon draw |
| `+0x08` | `icon_id` | `int` | G_ICON_PTRS index of the button glyph |
| `+0x0c` | `pending_worker_amount` | `int` | worker count to (un)assign, computed from the click/hold; consumed once by the apply helper |
| `+0x10` | `pending_row` | `int` | clicked active-list row +1 (0=idle); set on click, reset to 0 after the tick applies it |

#### `llm_ui_click_sound_slot` (size 0x1a, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `format_flags` | `uint` | WAV/RIFF format flags |
| `+0x04` | `pcm_data` | `void *` | GlobalAlloc'd PCM sample data (loaded by FUN_004ce0ce) |
| `+0x08` | `reserved` | `byte[18]` | remaining WAV descriptor fields (unresolved) |

#### `llm_ui_dlg_table` (size 0x34, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `title` | `pointer` | Dialog message/title text ptr; copied to _G_LLM_UI_DLG_SCRATCH_WIDGETS[3].label |
| `+0x04` | `item_labels` | `pointer[4]` | 0-terminated list of menu-item label ptrs; the builder makes one widget per non-null entry (menu dialogs). Empty (all 0) for keyboard Yes/No confirm boxes. |
| `+0x14` | `item_callbacks` | `pointer[4]` | Per-item action_cb (parallel to item_labels). For Yes/No confirm boxes: [0]=confirm/Yes, [1]=cancel/No handler. |
| `+0x24` | `item_values` | `int[4]` | Per-item value -> widget.value (menu selection id / option value). |

#### `llm_ui_fade_transition_state` (size 0x20, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `elapsed_ms` | `int` | ms elapsed within the current fade phase; recomputed each tick from llm_time_get_ticks_ms() - phase_start_ticks |
| `+0x04` | `phase_start_ticks` | `int` | llm_time_get_ticks_ms() timestamp when the current phase (fade-out/fade-in) started |
| `+0x08` | `state` | `int` | fade state machine: 0=arm+start timer, 1=fade from src, 2=fade to dst, 3=finish+invoke done_cb |
| `+0x0c` | `src_screen_id` | `int` | screen/framebuffer id to fade FROM; 0 = skip fade-out phase |
| `+0x10` | `dst_screen_id` | `int` | screen/framebuffer id to fade TO; 0 = skip fade-in phase |
| `+0x14` | `done_cb` | `intCallback *` | completion callback invoked with 0 args when state reaches 3 |
| `+0x18` | `tick_cb` | `intCallback *` | optional per-tick callback invoked with 0 args during the fade-out phase (state 1) |
| `+0x1c` | `duration_ms` | `int` | total fade duration in ms (observed default 0x1f4=500); blend-level divisor |

#### `llm_ui_flc_anim_state` (size 0x534, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `file_ptr` | `void *` | raw FLC resource pointer (GetResourseFilePtr); NULL = no anim, draw placeholder box |
| `+0x04` | `decode_buf` | `void *` | malloc'd palette-index pixel buffer (width*height bytes); freed with file_ptr in llm_dlg_close_and_resume |
| `+0x08` | `hdr_unk1` | `undefined4` | opaque embedded FLC frame-header field (unverified) |
| `+0x0c` | `width` | `int` | preview width px; copied into the planet widget.width |
| `+0x10` | `height` | `int` | preview height px; copied into the planet widget.height |
| `+0x14` | `pixel_count` | `int` | width*height; used directly as the malloc size for decode_buf |
| `+0x18` | `hdr_unk2` | `undefined4` | opaque embedded FLC frame-header field (unverified) |
| `+0x1c` | `hdr_unk3` | `undefined4` | opaque FLC frame-header field, zeroed by FUN_004de59d (unverified) |
| `+0x20` | `hdr_unk4` | `undefined4` | opaque (unverified) |
| `+0x24` | `palette_dirty` | `int` | nonzero => new palette chunk decoded this frame, needs RGB565 conversion (FUN_004c4f29) |
| `+0x28` | `palette_rgb888` | `byte[768]` | 256-entry raw RGB triplet palette parsed from the FLC palette chunk |
| `+0x328` | `frame_counter` | `int` | running animation tick accumulator, advanced by _G_LLM_UI_ANIM_FRAME_DELTA |
| `+0x32c` | `frame_delay` | `int` | per-planet playback threshold = cfg Planet.turn_speed; advance when frame_counter exceeds this |
| `+0x330` | `palette_rgb565` | `ushort[256]` | 256-entry display-format palette (per G_COLOR_FORMAT), converted from palette_rgb888 by FUN_004c4f29 |
| `+0x530` | `reserved_tail` | `undefined4` | unverified; last dword of the 0x14d-int stride |

#### `llm_ui_hud_color_slot` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `color` | `uint` | rgb565 packed via llm_gfx_pack_rgb16 (low 16 bits) |
| `+0x04` | `font_id` | `int` | font selector for text drawn in this style (0/1); read by llm_ui_panel_text_draw & panel draw fns |

#### `llm_ui_hud_widget` (size 0x1c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `action_callback` | `void *` | code*; invoked as (cb_arg, &desc) on click via the widget dispatch/handlers |
| `+0x04` | `callback_arg` | `int` | arg passed to action_callback (or a shared-handler mode/action code) |
| `+0x08` | `x` | `int` | left px |
| `+0x0c` | `y` | `int` | top px |
| `+0x10` | `right` | `int` | right px (x+width) |
| `+0x14` | `bottom` | `int` | bottom px (y+height) |
| `+0x18` | `text_id` | `int` | G_TEXT_PTRS label index (0 = none / plain rect) |

#### `llm_ui_icon_entry` (size 0x5, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `operand` | `int` | Meaning is selected by `kind`: kind 3 = a FUNCTION POINTER, called as operand(entry_index) by CALL dword ptr [EBP-0x1c] at 0x0041b07e; kind 4 = an index into _G_LLM_UI_HUD_WIDGETS (hit-tested at 0x0041b096..0x0041b0da, then the widget's action_callback at CALL dword ptr [EBX+0x50be63], 0x0041b198); kind 5 = a nested icon-group id, recursed at CALL 0x0041afcf / 0x0041b1b8. The HUD panel layer OVERWRITES this field at runtime to re-point a row: llm_ui_bldg_panel_refresh_apply writes 4/5/6 (0x00415c2f/0x00415c42/0x00415c55), llm_ui_bldg_selected_panel_tick 7..0x15 (0x00416ad2, 0x00416c24, ...), llm_ui_hud_mainpanel_tab_row_label_refresh 0x16/0x17 (0x0041591c/0x00415909), llm_ui_group_tab_on_click 0x36/0x37 (0x0041941b, ...), llm_ui_hud_topbar_tick 1/2/3 (0x00414479/0x00414496/0x004144b3). |
| `+0x04` | `kind` | `byte` | Entry kind, dispatched by llm_ui_icon_group_dispatch's switch (function base 0x0041afcf). 1 = END of this group (the walk returns; exactly 56 of these in the unpacked array, one per group); 2 = inert/hidden entry (llm_ui_group_tab_on_click writes 2 at 0x0041931c/0x0041932c/0x00419377 to hide tab rows); 3 = direct callback; 4 = HUD widget hit-test + hover tooltip + click dispatch; 5 = recurse into a nested icon sub-group. Unpacked verbatim from the source blob by llm_util_unpack_static_category_table (0x00419b98). |

#### `llm_ui_icon_widget_desc` (size 0x20, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `base_icon_frame` | `int` | G_ICON_PTRS base frame index |
| `+0x04` | `defer_redraw_flag` | `int` | if set, toggles _G_LLM_STRAT_UI_EVENT_DEFER_ACTIVE around the hover redraw |
| `+0x08` | `hover_frame_delta` | `int` | hover draws G_ICON_PTRS[base_icon_frame + this + 1] |
| `+0x0c` | `draw_frame_delta` | `int` | normal draw uses G_ICON_PTRS[base_icon_frame + this] |
| `+0x10` | `click_sound_id` | `int` | llm_snd_play id on click; default 0x44 set by llm_ui_hud_icon_widget_define |
| `+0x14` | `draw_x_offset` | `int` | normal icon drawn at widget.x + this |
| `+0x18` | `callback_arg` | `int` | arg passed to action_callback (runtime-filled by the define fn) |
| `+0x1c` | `action_callback` | `void *` | code*; called as action_callback(callback_arg, &desc) on click (runtime-filled) |

#### `llm_ui_info_media_ctx` (size 0x274, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `hfile` | `void *` | AVIFileOpenA/AVIFileRelease target (PAVIFILE) |
| `+0x04` | `audio_stream` | `llm_ui_avi_stream_info` | embedded sub-record, fourcc 'auds' |
| `+0xbc` | `video_stream` | `llm_ui_avi_stream_info` | embedded sub-record, fourcc 'vids' |
| `+0x174` | `codec_handle` | `int` | video decompressor handle (HIC) from ICLocate(), closed via ICClose |
| `+0x178` | `_pad1` | `undefined1[12]` | unmapped |
| `+0x184` | `acm_hdr` | `undefined1[84]` | embedded ACMSTREAMHEADER (0x54): cbStruct@+0=0x54, pbSrc@+0xc, cbSrcLength@+0x10 |
| `+0x1d8` | `dest_rect` | `RECT` | {0,0,w,h} |
| `+0x1e8` | `frame_rect_staging` | `RECT` | The RECT temporary Watcom materialises in static storage for the struct assignment `dest_rect = {0, 0, biWidth, biHeight}`. Built field-by-field out of order at 0x004cb290-0x004cb2bb -- right=+8 and bottom=+12 come from `[format_buf_a+4]` and `[format_buf_a+8]` (video_stream.format_buf_a is the AVI video stream's BITMAPINFOHEADER, so those are biWidth/biHeight), then top=+4 is zeroed and left=+0 is copied from it -- and then `REP MOVSD` with ECX=4 copies all 16 bytes from here (ESI=0x0065fa17) into dest_rect (EDI=0x0065fa07) at 0x004cb2c0-0x004cb2cf. Not player state: a compiler-owned staging slot. |
| `+0x1f8` | `locked_surface_ptr` | `void *` | The locked pixel pointer captured out of _G_LLM_UI_INFO_DDRAW_SURFACE_DESC.lpSurface (base+0x24) immediately after IDirectDrawSurface::Lock: read at 0x004cb354, stored here at 0x004cb359 (`MOV [0x0065fa27],EAX`). |
| `+0x1fc` | `_pad2` | `undefined1[8]` | unmapped residue of the old 28-byte _pad2 blob; its first 20 bytes are now frame_rect_staging + locked_surface_ptr. |
| `+0x204` | `flag_a` | `int` | zeroed by llm_ui_avi_init_codecs at setup |
| `+0x208` | `_pad3` | `undefined1[4]` | unmapped |
| `+0x20c` | `flag_seek_pending` | `int` | toggled around AVIStreamFindSample in llm_ui_avi_decode_frame |
| `+0x210` | `flag_more_data` | `int` | loop-continue condition in llm_ui_avi_decode_frame |
| `+0x214` | `playback_pos_ms` | `int` | playback position in ms. Advanced EVERY frame by the frame delta in llm_ui_info_media_frame_tick and wrapped modulo video_stream.stream_length_time to loop. (Zeroed on screen open -- the old name flag_b recorded only that one fact.) |
| `+0x218` | `last_tick_ms` | `int` | tick-counter (DAT_00e654f8) copy from the PREVIOUS frame -- rewritten to now on every frame in llm_ui_info_media_frame_tick, and the subtrahend for the frame delta. NOT an open-time stamp: the old name open_timestamp was inferred from the open path alone and misled a 2026-07-20 naming pass (see docs/dead-ends.md B). |
| `+0x21c` | `saved_game_mode` | `int` | restores _G_LLM_GAME_MODE on close |
| `+0x220` | `ddraw_surface` | `IDirectDrawSurface *` | The offscreen DirectDraw surface video frames are decoded into. Created by IDirectDraw::CreateSurface -- vtable slot +0x18 on _G_LLM_GFX_DDRAW_DEVICE, called at 0x004cb32f with (this, &_G_LLM_UI_INFO_DDRAW_SURFACE_DESC, &this_field, NULL), so this field is CreateSurface's out-parameter. Then IDirectDrawSurface::Lock at vtable +0x64 (0x004cb34e) and ::Unlock at vtable +0x80 (0x004cb383) -- both offsets match the IDirectDrawSurface vtable exactly, which is the third independent confirmation of the interface identity. (Was `void *com_interface`.) |
| `+0x224` | `_pad4` | `undefined1[80]` | Unmapped residue of the record, 0x50 bytes. Two offsets inside it ARE touched -- +0x25c (0x0065fa8b, written by llm_ui_info_media_draw_p1/p2 at 0x004caca2/0x004cace2) and +0x270 (0x0065fa9f, address-taken at 0x004caca7/0x004cacb1/0x004cace7/0x004cacf1) -- and +0x270 is what fixes this record's measured extent at 0x274. |

#### `llm_ui_info_screen_workspace` (size 0x820, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `text` | `WCHAR[1030]` | accumulated wide-char description text; written by llm_ui_info_text_build/_append/_emit_stat/_emit_labeled_stat |
| `+0x80c` | `entity_id` | `int` | = param_1 (entity id) at top of llm_ui_info_text_build |
| `+0x810` | `text_color` | `int` | scratch value shared across the text helpers (init 0x6e) |
| `+0x814` | `cur_building` | `cfg_final_struct_Building *` | = Building+entity_id in the building-kind branch |
| `+0x818` | `upgrade_base_building` | `cfg_final_struct_Building *` | the building an in-progress upgrade belongs to, else NULL |
| `+0x81c` | `cur_unit` | `cfg_final_struct_Unit *` | = Unit+entity_id in the unit-kind branch |

#### `llm_ui_list_row_icon_style` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `font_slot` | `int` | Passed as llm_ui_panel_text_draw's 5th argument (font_slot) for the row label; read at 0x00419528 (PUSH dword ptr [EAX] after EAX = base + mode*4). |
| `+0x04` | `icon_id` | `int` | Index into G_ICON_PTRS for the row icon (read at 0x00419508, scaled at 0x0041950b, loaded from G_ICON_PTRS at 0x0041950e) and passed to llm_ui_icon_draw_vclipped at 0x0041951a. |

#### `llm_ui_menu_list_item` (size 0x44, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `name` | `wchar_t[20]` | wide item name (w_sprintf) |
| `+0x28` | `sublabel` | `wchar_t[10]` | wide sublabel line ("%S %d") |
| `+0x3c` | `layout_y_in` | `int` | incoming stacked-layout Y accumulator |
| `+0x40` | `layout_y_out` | `int` | outgoing stacked-layout Y (in + row height) |

#### `llm_ui_outcome_stat_row` (size 0x18, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `bldgs_built` | `int` | Buildings built, summed over the planets of the current system. Accumulated by llm_ui_outcome_stat_cell_add from _G_LLM_STRAT_PLAYERS[p].buildings_built_total[planet] (call at 0x004c6a3a, cell offset +0). |
| `+0x04` | `bldgs_lost` | `int` | Buildings lost (buildings_lost_total); third call, cell offset +4 (0x004c6a7a). |
| `+0x08` | `bldgs_killed` | `int` | Buildings destroyed by this player (buildings_killed_total); second call, cell offset +8 (0x004c6a5a). |
| `+0x0c` | `units_built` | `int` | Units built (units_built_total); fourth call, cell offset +0xc (0x004c6a9a). llm_ui_outcome_dlg_open then does DEC dword ptr [EAX+0xc] at 0x004c6b7c for every player whose bldgs_built is > 0 -- the starting unit is not counted as built. |
| `+0x10` | `units_lost` | `int` | Units lost (units_lost_total); sixth call, cell offset +0x10 (0x004c6ada). |
| `+0x14` | `units_killed` | `int` | Units destroyed by this player (units_killed_total); fifth call, cell offset +0x14 (0x004c6aba). |

#### `llm_ui_row_icon_style` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `icon_id` | `int` | G_ICON_PTRS index drawn for this row-state (values seen: 0x1f/0x20/0x23/0x24) |
| `+0x04` | `text_color` | `undefined4` | color/style id passed to llm_ui_panel_text_draw for this row-state (values seen: 4/5) |

#### `llm_ui_scroll_arrow_desc` (size 0x10, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `scroll_list` | `llm_ui_scroll_list_descriptor *` | back-ptr to the owning llm_ui_scroll_list_descriptor |
| `+0x04` | `direction` | `int` | scroll direction: +1 = up (toward top), -1 = down |
| `+0x08` | `icon_base_frame` | `int` | G_ICON_PTRS base frame for the arrow sprite (up/down variants) |
| `+0x0c` | `widget_id` | `int` | the arrow's widget-table id; runtime-filled by llm_ui_hud_scroll_list_define (list_id+1/+2) |

#### `llm_ui_scroll_list_descriptor` (size 0x31, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `row_height` | `int` | per-row vertical pitch/divisor (confirmed: llm_ui_hud_scroll_list_body_handler uses (WinH-top_y)/row_height for the hit row) |
| `+0x04` | `event_handle` | `int` | game event signalled via game::SetEvent on a row click/scroll (llm_ui_hud_scroll_list_body_handler) |
| `+0x08` | `row_draw_callback` | `void *` | per-row draw callback, invoked as (*cb)(row) by llm_ui_scroll_icon_list_draw |
| `+0x0c` | `field_0xc` | `undefined1` | unresolved 1-byte field |
| `+0x0d` | `default_row_icon_id` | `int` | index into G_ICON_PTRS for the default (unhighlighted) row icon |
| `+0x11` | `highlight_icon_id` | `int` | index into G_ICON_PTRS for the highlight/empty-row icon |
| `+0x15` | `scrollbar_up` | `llm_ui_scroll_arrow_desc *` | -> up-arrow llm_ui_scroll_arrow_desc (set by llm_ui_hud_scroll_list_define) |
| `+0x19` | `scrollbar_down` | `llm_ui_scroll_arrow_desc *` | -> down-arrow llm_ui_scroll_arrow_desc |
| `+0x1d` | `icon_x` | `int` | row icon x-coord (set by constructor; read by the panel draw-row fn) |
| `+0x21` | `top_y` | `int` | first-row top Y (constructor: base_y + row_height) |
| `+0x25` | `subrow_scroll_px` | `int` | smooth-scroll pixel offset; self-mutated each draw by llm_ui_scroll_icon_list_draw |
| `+0x29` | `scroll_row_offset` | `int` | current top row index; clamped each draw against item_count |
| `+0x2d` | `item_count` | `int` | row count; written by the owning panel-tick each frame, read as the scroll clamp bound |

#### `llm_ui_spinner` (size 0x14, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `min` | `int` |  |
| `+0x04` | `count` | `int` |  |
| `+0x08` | `f8` | `int` |  |
| `+0x0c` | `value` | `int` |  |
| `+0x10` | `f10` | `int` |  |

#### `llm_ui_text_bind_entry` (size 0xc, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `label_target` | `void * *` | -> the .label field of a target llm_ui_widget; filled from G_TEXT_PTRS[text_index] at boot |
| `+0x04` | `text_index` | `int` | index into G_TEXT_PTRS for the source wide-string pointer |
| `+0x08` | `value_target` | `void * *` | -> the .value field of a target llm_ui_widget (0 = none) |

#### `llm_ui_widget` (size 0x44, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `nav_target` | `llm_ui_widget_list *` |  |
| `+0x04` | `action_cb` | `pointer` | Activation callback invoked when the widget is triggered; set from the builder table (param[item+5]) in llm_ui_dlg_build_from_table. PTR_FUN_00653a5f is item-0's slot in the shared scratch dialog table. |
| `+0x08` | `flags` | `uint` | Widget flag bits (generic builder inits 0xa001). 0x40 = DISABLED (hit-test and hotkey both skip it). Low-byte bit 0x80 = hidden (skipped by llm_ui_widget_list_draw). 0x100 = HIT-TEST AGAINST A 1BPP STENCIL, not the rect: llm_ui_widget_input_tick indexes _G_LLM_UI_MENU_MASK_BITMAPS[hit_mask_idx] and tests bit (dx + DRAW_W*dy) -- a clear bit means the cursor is NOT over this widget even though it is inside its rectangle. 0x400/0x800 activate on the mouse-DOWN edge (0x800 also makes a merely-SELECTED widget activate); without either, activation waits for the mouse-UP (event kind 4). 0x1000 = hotkey matches the SCANCODE rather than the ASCII; 0x2000 / 0x4000 = also triggered by Enter / Esc; 0x10000000 = a submenu request; 0x40000000 = activates on a drag/repeat event kind. Other bits are byte-accessed by llm_ui_widget_draw: +0x9&0x80 / +0x9&0x2 / +0xa&0x1 select label vs highlight variants, +0xb&0x80 = draw the disp index as a sprite id rather than a label-table lookup, and word bits &0x18000 gate the highlight-frame overlay. |
| `+0x0c` | `draw_cb` | `pointer` | Per-widget draw callback (llm_ui_widget_draw for widgets built by llm_ui_dlg_build_from_table); invoked once per frame by llm_ui_widget_list_draw unless the hidden flag is set. |
| `+0x10` | `disp_idx` | `int` | Display index in the DEFAULT (unhighlighted) visual state; used as an index into the label-pointer table (&DAT_0065419f) or, with flags bit +0xb&0x80, as a sprite id. |
| `+0x14` | `disp_idx_alt` | `int` | Display index used when this widget is the SECONDARY cursor (widget == DAT_006542a1). |
| `+0x18` | `disp_idx_sel` | `int` | Display index used when this widget is the PRIMARY highlighted cursor (widget == DAT_0065429d). |
| `+0x1c` | `x` | `int` | Laid-out left/X pixel position (centered within the viewport by the builder and adjusted at draw). |
| `+0x20` | `y` | `int` | Laid-out top/Y pixel position. |
| `+0x24` | `width` | `int` | Widget width in px (builder default 0x60; overwritten with the actual rendered label/sprite width at draw time). |
| `+0x28` | `height` | `int` | Widget height in px (builder default INT_00654263; overwritten with the rendered height at draw time). |
| `+0x2c` | `value` | `int` | Per-item value (param[item+9] in the builder); meaning is dialog-specific (menu selection id / option value / ...). |
| `+0x30` | `param_block` | `pointer` | e.g. spinner parameter block; 0 for plain widgets |
| `+0x34` | `hit_mask_idx` | `int` | Index into _G_LLM_UI_MENU_MASK_BITMAPS (12 MENUMASK*.GFX 1bpp stencils built by llm_ui_main_menu_screen_load). Only consulted when flags&0x100. THIS IS WHAT TELLS TWO IDENTICAL RECTS APART: the new-game race picker's Human and Alien buttons are byte-identical widgets at (160,72) 304x208 -- same x/y/w/h, same action_cb (llm_menu_race_select_cb), same flags -- differing ONLY in value ('h'/'a') and this index (7 = RACEHM.GFX, 8 = RACEAM.GFX). A click is attributed by which stencil has the bit set under the cursor, so the rect centre is not a valid target for such a widget. Was 4 bytes of undefined padding until 2026-09-07. |
| `+0x38` | `label` | `char *` | Label text pointer (0 = none); param[item+1] in the builder. Rendered at the widget's position. |
| `+0x40` | `user_data` | `uint` | row/slot index |

#### `llm_ui_widget_list` (size 0x4c, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `children` | `llm_ui_widget * *` | NULL-terminated array of llm_ui_widget* drawn in order (llm_ui_widget_list_draw skips hidden). |
| `+0x04` | `field4_layout_state` | `int` | Layout/scroll state: draw runs a re-layout pass (FUN_004b42f3) when this is >= 0. |
| `+0x08` | `frame` | `llm_ui_widget *` | Frame/background widget; drives centering (llm_ui_widget_list_center). 0 -> full-window bounds. |
| `+0x0c` | `field_0xc` | `int` | Unresolved container field (+0xc). |
| `+0x10` | `field_0x10` | `int` | Unresolved container field (+0x10). |
| `+0x14` | `parent` | `llm_ui_widget_list *` | Back-navigation parent: llm_ui_menu_push_screen saves the previous screen's list here (menu stack). |
| `+0x18` | `origin_x` | `int` | Centered draw-origin X (copied into _G_LLM_UI_WIDGET_DRAW_X by the draw helper). |
| `+0x1c` | `origin_y` | `int` | Centered draw-origin Y. |
| `+0x20` | `width` | `int` | Laid-out width (frame widget's sprite width, or WindowWidth when frameless). |
| `+0x24` | `height` | `int` | Laid-out height (frame widget's sprite height, or WindowHeight). |
| `+0x28` | `field_0x28` | `int` | Saved screen build-config block (+0x28..+0x48): llm_ui_menu_push_screen's helper snapshots the current screen's build scratch (title text / action callback / ...) here on navigate-away so the parent back-nav can restore it. Individual field roles unresolved. |
| `+0x2c` | `saved_action_cb` | `int` | Saved screen action/build callback (from DAT_00650a5b, e.g. llm_mp_map_picker_open_action). |
| `+0x30` | `field_0x30` | `int` |  |
| `+0x34` | `field_0x34` | `int` |  |
| `+0x38` | `field_0x38` | `int` |  |
| `+0x3c` | `field_0x3c` | `int` |  |
| `+0x40` | `field_0x40` | `int` |  |
| `+0x44` | `field_0x44` | `int` |  |
| `+0x48` | `field_0x48` | `int` |  |

#### `llm_vec2i` (size 0x8, category `/llm`)

| Offset | Field | Type | Comment |
| --- | --- | --- | --- |
| `+0x00` | `dx` | `int` | x / column delta |
| `+0x04` | `dy` | `int` | y / row delta |
<!-- END generated-structs -->
