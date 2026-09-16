# Symbols — generated tables

All `llm_` functions and `_G_LLM_` globals in `mh.exe`, dumped from the live Ghidra program.
**Do not hand-edit the block below** — regenerate it by running the Ghidra-side symbol dumper
(via ReVA: `run-script {programPath: "/eng/mh.exe", scriptName: "mh_dump_symbols.py"}`).
The `llm_` / `_G_LLM_` marker convention these names follow is in
[conventions.md](conventions.md).

<!-- BEGIN generated-symbols -->
### Named functions
| Address | Name | Signature | Tags |
| --- | --- | --- | --- |
| `0x004108bd` | `llm_debug_console_print_reset` | `undefined llm_debug_console_print_reset(void)` | todo:globals |
| `0x00410a4e` | `llm_log_debug_dir_session_init` | `void llm_log_debug_dir_session_init(void)` | conf:high todo:globals |
| `0x00410b1a` | `llm_log_write_file_count` | `void llm_log_write_file_count(int file_count)` | conf:med todo:globals |
| `0x00410bcc` | `llm_log_read_file_count` | `int llm_log_read_file_count(void)` | conf:high |
| `0x00410c97` | `llm_log_dump_all_files` | `void llm_log_dump_all_files(void)` | conf:high todo:globals |
| `0x004110a2` | `llm_ui_bldg_stat_value_draw` | `void llm_ui_bldg_stat_value_draw(int style_sel, int x, int y, int value)` | conf:med |
| `0x0041111e` | `llm_ui_bldg_progress_status_text_select` | `void llm_ui_bldg_progress_status_text_select(int bldg_idx, int text_x, int text_y)` | conf:med |
| `0x0041143f` | `llm_ui_bldg_resource_amount_draw` | `void llm_ui_bldg_resource_amount_draw(int resource_index)` | conf:high |
| `0x004115ba` | `llm_ui_bldg_resource_row_draw` | `void llm_ui_bldg_resource_row_draw(void)` | conf:med todo:globals |
| `0x0041172f` | `llm_ui_selection_count_row_draw` | `void llm_ui_selection_count_row_draw(int index_a, int index_b, int count_value, int y_baseline)` | conf:low |
| `0x0041181a` | `llm_ui_hud_minimap_click_handler` | `void llm_ui_hud_minimap_click_handler(int widget_id)` | conf:med |
| `0x00411a43` | `llm_ui_hud_base_marker_button_handler` | `void llm_ui_hud_base_marker_button_handler(int widget_id, int marker_slot)` | conf:high |
| `0x00411c12` | `llm_ui_hud_ctrl_group_button_handler` | `void llm_ui_hud_ctrl_group_button_handler(int widget_id, int ctrl_group_index)` | conf:high todo:globals |
| `0x00411d36` | `llm_ui_hud_fire_event_button_handler` | `void llm_ui_hud_fire_event_button_handler(int widget_id, game_e_event event)` | conf:high |
| `0x00411d8d` | `llm_ui_hud_storage_depart_type_select_handler` | `void llm_ui_hud_storage_depart_type_select_handler(void)` | conf:med |
| `0x00411dbf` | `llm_ui_hud_planet_select_button_handler` | `void llm_ui_hud_planet_select_button_handler(int widget_id, int screen_arg)` | conf:med |
| `0x00411e22` | `llm_ui_hud_icon_widget_hover_draw` | `void llm_ui_hud_icon_widget_hover_draw(int widget_id, int icon_base_index)` | conf:med todo:globals |
| `0x00411f12` | `llm_ui_unassigned_row_state_clear` | `void llm_ui_unassigned_row_state_clear(int row_index)` | conf:med |
| `0x00411f7c` | `llm_strat_ctrlgrp_assign_unit_by_row_click` | `void llm_strat_ctrlgrp_assign_unit_by_row_click(int row_click_index)` | conf:med todo:globals |
| `0x00411fba` | `llm_ui_ctrlgroup_member_scroll_row_click_remove` | `void llm_ui_ctrlgroup_member_scroll_row_click_remove(int row_index)` | conf:high todo:globals |
| `0x00411ff2` | `llm_ui_scroll_list_autorepeat_tick` | `int llm_ui_scroll_list_autorepeat_tick(int * scroll_state)` | conf:med todo:globals todo:struct |
| `0x00412129` | `llm_ui_hud_scroll_list_body_handler` | `void llm_ui_hud_scroll_list_body_handler(int widget_id, llm_ui_scroll_list_descriptor * scroll_desc)` | conf:high |
| `0x00412360` | `llm_bldg_available_unit_type_by_slot` | `int llm_bldg_available_unit_type_by_slot(int target_slot, int building_type)` | conf:med |
| `0x00412418` | `llm_strat_bldg_get_nth_active_building_index` | `int llm_strat_bldg_get_nth_active_building_index(int n)` |  |
| `0x004124ba` | `llm_ui_bldg_stat_adjust_row_click` | `uint llm_ui_bldg_stat_adjust_row_click(int widget_id, int resource_row, int direction_load_flag)` | conf:med |
| `0x004126c8` | `llm_ui_hud_bldg_load_button_handler` | `void llm_ui_hud_bldg_load_button_handler(int widget_id, int cargo_slot)` | conf:high todo:enum |
| `0x0041275d` | `llm_ui_hud_bldg_unload_button_handler` | `void llm_ui_hud_bldg_unload_button_handler(int widget_id, int cargo_slot)` | conf:high todo:enum |
| `0x004127ef` | `llm_ui_bldg_units_panel_row_count_update` | `void llm_ui_bldg_units_panel_row_count_update(void * widget, int row_idx, int direction, int target_count)` | rev:verified |
| `0x004129a8` | `llm_ui_hud_bldg_units_count_inc_handler` | `void llm_ui_hud_bldg_units_count_inc_handler(int widget_id, int row_idx)` | rev:verified |
| `0x004129e3` | `llm_ui_hud_bldg_units_count_dec_handler` | `void llm_ui_hud_bldg_units_count_dec_handler(int widget_id, int row_idx)` | rev:verified |
| `0x00412a1b` | `llm_ui_unit_unassigned_list_draw_row` | `void llm_ui_unit_unassigned_list_draw_row(int row_index, int row_state)` | conf:med |
| `0x00412b43` | `llm_ui_ctrlgroup_member_icon_row_draw` | `void llm_ui_ctrlgroup_member_icon_row_draw(int row_index, int icon_style_index)` | conf:med |
| `0x00412c25` | `llm_ui_unit_unassigned_list_scroll_row_draw` | `void llm_ui_unit_unassigned_list_scroll_row_draw(int row_index)` | conf:high |
| `0x00412c5e` | `llm_ui_ctrlgroup_member_icon_scroll_row_draw` | `void llm_ui_ctrlgroup_member_icon_scroll_row_draw(int row_index)` | conf:high todo:globals |
| `0x00412c97` | `llm_ui_icon_drag_range_process` | `void llm_ui_icon_drag_range_process(void * drag_state, int icon_index, int hover_index)` | conf:high todo:struct |
| `0x00412f78` | `llm_ui_hud_row_drag_toggle_handler` | `void llm_ui_hud_row_drag_toggle_handler(int widget_id, int row_idx)` | conf:low todo:struct |
| `0x00412fae` | `llm_ui_hud_icon_drag_select_bind` | `void llm_ui_hud_icon_drag_select_bind(int icon_index_a, int icon_index_b)` | conf:low todo:globals todo:struct |
| `0x00412fe4` | `llm_ui_hud_scroll_arrow_handler` | `void llm_ui_hud_scroll_arrow_handler(int widget_id, llm_ui_scroll_arrow_desc * arrow_desc)` | conf:high |
| `0x0041315f` | `llm_ui_build_menu_item_select` | `void llm_ui_build_menu_item_select(void * widget, int item_idx)` | conf:med |
| `0x004131f9` | `llm_ui_hud_icon_click_select` | `void llm_ui_hud_icon_click_select(int icon_rect_id, int item_index)` | conf:med todo:globals |
| `0x00413251` | `llm_ui_hud_slot_rmb_click_gate` | `int llm_ui_hud_slot_rmb_click_gate(int widget_id, int row_index)` | conf:high todo:globals |
| `0x004132c2` | `llm_ui_info_screen_request_pending_row` | `void llm_ui_info_screen_request_pending_row(int widget_id, int row_index)` | conf:high |
| `0x004132f8` | `llm_ui_bldg_units_panel_request_pending_row` | `void llm_ui_bldg_units_panel_request_pending_row(int widget_id, int row_index)` | conf:high |
| `0x0041332e` | `llm_ui_storage_panel_request_show_info_slot` | `void llm_ui_storage_panel_request_show_info_slot(int widget_id, int row_index)` | conf:high |
| `0x00413364` | `llm_ui_bldg_worker_adjust_btn_tick` | `undefined llm_ui_bldg_worker_adjust_btn_tick(void)` | conf:med |
| `0x004134b1` | `llm_ui_bldg_worker_adjust_apply` | `void llm_ui_bldg_worker_adjust_apply(llm_ui_bldg_worker_adjust_btn * adj_btn, uint building_index, int target_is_index)` | conf:med |
| `0x0041352d` | `llm_ui_bldg_worker_adj_btn0_tick` | `undefined llm_ui_bldg_worker_adj_btn0_tick(void)` | conf:high |
| `0x00413563` | `llm_ui_bldg_worker_adj_btn1_tick` | `undefined llm_ui_bldg_worker_adj_btn1_tick(void)` | conf:high |
| `0x00413599` | `llm_ui_hud_icon_widget_dispatch` | `void llm_ui_hud_icon_widget_dispatch(int widget_id, llm_ui_icon_widget_desc * icon_desc)` | conf:high |
| `0x004136aa` | `llm_ui_hud_minimap_zoom_inc_handler` | `void llm_ui_hud_minimap_zoom_inc_handler(void)` | conf:med |
| `0x00413707` | `llm_ui_hud_minimap_zoom_dec_handler` | `void llm_ui_hud_minimap_zoom_dec_handler(void)` | conf:med |
| `0x00413764` | `llm_ui_hud_ingame_menu_button_handler` | `void llm_ui_hud_ingame_menu_button_handler(void)` | conf:high |
| `0x00413799` | `llm_ui_hud_storage_panel_open_request` | `void llm_ui_hud_storage_panel_open_request(void)` | conf:med |
| `0x004137c9` | `llm_ui_hud_storage_recharge_request_set` | `void llm_ui_hud_storage_recharge_request_set(void)` | conf:high |
| `0x004137f9` | `llm_ui_hud_storage_slot1_set` | `void llm_ui_hud_storage_slot1_set(int widget_id, int callback_arg)` | conf:med todo:globals |
| `0x00413828` | `llm_ui_hud_storage_slot2_set` | `void llm_ui_hud_storage_slot2_set(int widget_id, int callback_arg)` | conf:med todo:globals |
| `0x00413857` | `llm_ui_hud_storage_slot1_icon_click` | `void llm_ui_hud_storage_slot1_icon_click(int widget_id, int callback_arg)` | conf:med todo:globals |
| `0x004138af` | `llm_ui_hud_storage_slot2_icon_click` | `void llm_ui_hud_storage_slot2_icon_click(int widget_id, int callback_arg)` | conf:med todo:globals |
| `0x00413907` | `llm_ui_hud_storage_slot0_icon_click` | `void llm_ui_hud_storage_slot0_icon_click(int widget_id, int callback_arg)` | conf:med todo:globals |
| `0x0041395f` | `llm_ui_hud_research_row_click_handler` | `void llm_ui_hud_research_row_click_handler(int widget_id, int row_index)` | conf:med |
| `0x004139e5` | `llm_ui_hud_bldg_panel_slot_click_handler` | `void llm_ui_hud_bldg_panel_slot_click_handler(int widget_id, int row_index)` | conf:high todo:globals |
| `0x00414313` | `llm_ui_hud_panel_title_draw` | `void llm_ui_hud_panel_title_draw(char * title_text)` | conf:med |
| `0x00414395` | `llm_ui_hud_topbar_tick` | `void llm_ui_hud_topbar_tick(int panel_target_index)` | conf:med |
| `0x00414a4e` | `llm_ui_chat_input_process_scancodes` | `void llm_ui_chat_input_process_scancodes(void)` | conf:high |
| `0x00415475` | `llm_chat_history_push` | `void llm_chat_history_push(void)` |  |
| `0x00415565` | `llm_chat_history_reset_cursor` | `void llm_chat_history_reset_cursor(void)` |  |
| `0x00415591` | `llm_ui_chat_input_char_insert` | `void llm_ui_chat_input_char_insert(char ch)` | conf:high todo:globals |
| `0x00415650` | `llm_ui_chat_input_delete_at_cursor` | `void llm_ui_chat_input_delete_at_cursor(void)` | conf:high |
| `0x004156c2` | `llm_ui_chat_input_backspace` | `void llm_ui_chat_input_backspace(void)` | conf:high |
| `0x0041575a` | `llm_ui_chat_input_char_insert_unbounded` | `void llm_ui_chat_input_char_insert_unbounded(char ch)` | conf:high todo:globals |
| `0x0041580f` | `llm_ui_chat_input_erase_char` | `void llm_ui_chat_input_erase_char(void)` | conf:med |
| `0x004158a7` | `llm_ui_hud_mainpanel_tab_row_label_refresh` | `void llm_ui_hud_mainpanel_tab_row_label_refresh(int row_index)` | conf:med |
| `0x00415930` | `llm_ui_main_panel_draw` | `void llm_ui_main_panel_draw(void)` | conf:high |
| `0x00415b41` | `llm_strat_bldg_order_depart_confirm_dispatch` | `void llm_strat_bldg_order_depart_confirm_dispatch(void)` |  |
| `0x00415bc5` | `llm_ui_bldg_panel_refresh_apply` | `void llm_ui_bldg_panel_refresh_apply(int slot_id)` | conf:med |
| `0x00415c69` | `llm_ui_scroll_icon_list_draw` | `void llm_ui_scroll_icon_list_draw(void * scroll_desc, int icon_entry_base)` | conf:med |
| `0x00415f3b` | `llm_strat_ui_bldg_panel_draw` | `void llm_strat_ui_bldg_panel_draw(int x, int y, int icon_idx, int name_icon_idx, cfg_final_struct_Building * type_ptr, int detail_level, int b_index, map_object_building * building_ptr, int cmp_id)` |  |
| `0x00416379` | `llm_strat_bldg_available_build_menu_count` | `undefined llm_strat_bldg_available_build_menu_count(void)` |  |
| `0x004163db` | `llm_ui_bldg_available_projects_tab_process_events` | `undefined llm_ui_bldg_available_projects_tab_process_events(void)` | conf:med todo:globals |
| `0x004164e8` | `llm_ui_bldg_build_menu_icon_draw` | `undefined llm_ui_bldg_build_menu_icon_draw(void)` |  |
| `0x004165ea` | `llm_ui_bldg_owned_tab_process_events` | `void llm_ui_bldg_owned_tab_process_events(int y_base)` |  |
| `0x0041687c` | `llm_ui_bldg_list_icon_draw` | `void llm_ui_bldg_list_icon_draw(int slot)` | conf:med |
| `0x0041695e` | `llm_ui_bldg_selected_panel_tick` | `void llm_ui_bldg_selected_panel_tick(int panel_slot)` |  |
| `0x00416ec6` | `llm_ui_bldg_units_panel_tick` | `void llm_ui_bldg_units_panel_tick(int panel_target_index)` | rev:verified |
| `0x004170ce` | `llm_ui_panel_text_pair_draw_centered` | `void llm_ui_panel_text_pair_draw_centered(void * text_a, void * text_b, int x, int y, int width)` | conf:med todo:globals |
| `0x00417175` | `llm_ui_panel_int_pair_draw_centered` | `void llm_ui_panel_int_pair_draw_centered(int value_a, int value_b, int x, int y, int width)` | conf:med |
| `0x004171cf` | `llm_ui_bldg_stat_adjust_row_draw` | `void llm_ui_bldg_stat_adjust_row_draw(int icon_index, int value, int max_value, int x, int * y_inout, int width, int left_hit_id, int right_hit_id)` | conf:med |
| `0x00417303` | `llm_ui_bldg_stat_rows_draw` | `void llm_ui_bldg_stat_rows_draw(void)` | conf:high |
| `0x00417409` | `llm_strat_ui_storage_bldg_panel` | `void llm_strat_ui_storage_bldg_panel(int panel_kind, int y_base)` |  |
| `0x00417c3b` | `llm_ui_storage_bldg_panel_kind1` | `void llm_ui_storage_bldg_panel_kind1(int y_base)` | conf:med |
| `0x00417c6d` | `llm_ui_storage_bldg_panel_kind0` | `void llm_ui_storage_bldg_panel_kind0(int y_base)` | conf:med |
| `0x00417c9c` | `llm_ui_storage_bldg_panel_kind3` | `void llm_ui_storage_bldg_panel_kind3(int y_base)` | conf:med |
| `0x00417cce` | `llm_ui_bldg_shuttle_dock_panel_tick` | `void llm_ui_bldg_shuttle_dock_panel_tick(void)` | conf:high |
| `0x00417e2e` | `llm_ui_build_panel_project_row_draw` | `undefined llm_ui_build_panel_project_row_draw(void)` |  |
| `0x00417f3e` | `llm_strat_bldg_count_available_projects` | `int llm_strat_bldg_count_available_projects(int category)` |  |
| `0x00417fb3` | `llm_ui_bldg_project_panel_tick` | `void llm_ui_bldg_project_panel_tick(int panel_target_index)` |  |
| `0x0041817c` | `llm_ui_bldg_panel_tick` | `void llm_ui_bldg_panel_tick(void)` | conf:high |
| `0x00418320` | `llm_ui_hud_label_value_row_draw` | `int llm_ui_hud_label_value_row_draw(int text_id, int value, int y)` | conf:high |
| `0x0041840f` | `llm_ui_hud_bldg_panel_same_type_count_row_draw` | `int llm_ui_hud_bldg_panel_same_type_count_row_draw(int row_slot)` | conf:high |
| `0x0041848e` | `llm_ui_hud_pop_panel_draw` | `void llm_ui_hud_pop_panel_draw(void)` | conf:high |
| `0x004185a8` | `llm_ui_bldg_mine_resource_stats_draw` | `void llm_ui_bldg_mine_resource_stats_draw(int y_base, int * cursor_y, int value_style, int stat_mode, int header_text_id, double preview_delta)` | conf:high |
| `0x0041885d` | `llm_ui_hud_bldg_mine_panel_draw` | `void llm_ui_hud_bldg_mine_panel_draw(void)` | conf:high |
| `0x00418909` | `llm_ui_hud_bldg_network_status_panel_draw` | `void llm_ui_hud_bldg_network_status_panel_draw(void)` | conf:high |
| `0x0041899c` | `llm_ui_panel_text_draw_centered` | `void llm_ui_panel_text_draw_centered(int y, void * text, int style_id)` | conf:high todo:struct |
| `0x00418a37` | `llm_ui_hud_bldg_count_only_panel_draw` | `void llm_ui_hud_bldg_count_only_panel_draw(void)` | conf:low |
| `0x00418a68` | `llm_ui_hud_bldg_same_type_count_row_draw_by_race` | `void llm_ui_hud_bldg_same_type_count_row_draw_by_race(void)` | conf:med |
| `0x00418ab0` | `llm_ui_hud_power_panel_draw` | `void llm_ui_hud_power_panel_draw(void)` | conf:high |
| `0x00418b43` | `llm_ui_hud_bldg_panel_refresh_tick` | `void llm_ui_hud_bldg_panel_refresh_tick(void)` | conf:med todo:review |
| `0x00418b85` | `llm_ui_hud_bldg_panel_refresh_tick_dup` | `void llm_ui_hud_bldg_panel_refresh_tick_dup(void)` | conf:med todo:review |
| `0x00418bc7` | `llm_ui_panel_row_icon_label_draw` | `void llm_ui_panel_row_icon_label_draw(int style_flags, int x, int y, void * item_def)` | conf:med todo:struct |
| `0x00418c4a` | `llm_ui_bldg_row_unit_effect_draw` | `void llm_ui_bldg_row_unit_effect_draw(uint side, int x, int y, map_object_unit * unit)` |  |
| `0x00418d97` | `llm_ui_unit_ctrlgroup_panel_draw_row` | `void llm_ui_unit_ctrlgroup_panel_draw_row(int row)` | conf:med |
| `0x00418e50` | `llm_ui_unit_panel_populate_selected` | `void llm_ui_unit_panel_populate_selected(int player, int unit_idx)` | conf:med |
| `0x00418ed7` | `llm_ui_bldg_prod_queue_slot_draw` | `void llm_ui_bldg_prod_queue_slot_draw(int slot_index)` |  |
| `0x00419077` | `llm_strat_ui_storage_panel_draw_row` | `void llm_strat_ui_storage_panel_draw_row(int row)` |  |
| `0x00419166` | `llm_ui_hud_unassigned_unit_row_click_handler` | `void llm_ui_hud_unassigned_unit_row_click_handler(int widget_id, int row_index)` | conf:high |
| `0x00419221` | `llm_ui_group_tab_on_click` | `void llm_ui_group_tab_on_click(int tab_index)` |  |
| `0x0041944c` | `llm_ui_list_item_icon_label_draw` | `void llm_ui_list_item_icon_label_draw(int slot_id, int mode)` |  |
| `0x00419549` | `llm_strat_ui_unit_panel_tick` | `void llm_strat_ui_unit_panel_tick(int icon_entry_base)` |  |
| `0x004197ba` | `llm_ui_selection_panel_refresh` | `void llm_ui_selection_panel_refresh(int icon_entry_base)` |  |
| `0x00419b98` | `llm_util_unpack_static_category_table` | `void llm_util_unpack_static_category_table(void)` | conf:low |
| `0x00419c16` | `llm_gfx_palette_scale_rgb` | `void llm_gfx_palette_scale_rgb(ushort * dst_palette, ushort * src_palette, int * rgb_scale)` | conf:med todo:struct |
| `0x00419cff` | `llm_ui_hud_widget_define` | `void llm_ui_hud_widget_define(int widget_id, int x, int y, int right, int bottom, void * callback, int cb_arg, int text_id)` | conf:high |
| `0x00419d84` | `llm_ui_hud_color_slot_set` | `void llm_ui_hud_color_slot_set(int r, int g, int b, int font_id, int slot_index)` | conf:high |
| `0x00419dd9` | `llm_ui_hud_widget_range_define` | `void llm_ui_hud_widget_range_define(int id_start, int id_end, int text_id, int text_id_step, int x, int y, int w, int h, int x_step, int y_step, void * callback, int cb_arg)` | conf:med |
| `0x00419e5c` | `llm_ui_hud_scroll_list_define` | `void llm_ui_hud_scroll_list_define(int base_id, int text_id, int x, int y_top, int width, int height, int row_h, llm_ui_scroll_list_descriptor * scroll_desc, llm_ui_scroll_arrow_desc * up_arrow, llm_ui_scroll_arrow_desc * down_arrow)` | conf:med |
| `0x00419f79` | `llm_ui_hud_icon_widget_define` | `void llm_ui_hud_icon_widget_define(int id_start, int id_end, int text_id, int x, int y, int w, int h, int x_step, int y_step, llm_ui_icon_widget_desc * desc, void * callback, int cb_arg)` | conf:med |
| `0x0041a013` | `llm_strat_ui_hud_widgets_init` | `void llm_strat_ui_hud_widgets_init(void)` | conf:med todo:globals |
| `0x0041adfe` | `llm_strat_ui_panel_init` | `void llm_strat_ui_panel_init(void)` |  |
| `0x0041af27` | `llm_debug_console_scroll_stub` | `undefined llm_debug_console_scroll_stub(void)` |  |
| `0x0041af49` | `llm_ui_hud_widget_contains_test_point` | `int llm_ui_hud_widget_contains_test_point(int widget_index)` | conf:high todo:globals |
| `0x0041afcf` | `llm_ui_icon_group_dispatch` | `void llm_ui_icon_group_dispatch(int icon_group_id)` | conf:med todo:globals todo:struct |
| `0x0041b1cc` | `llm_ui_frame_tick` | `void llm_ui_frame_tick(void)` |  |
| `0x0041b43c` | `llm_input_mouse_button_hold_update` | `void llm_input_mouse_button_hold_update(void)` | conf:med todo:globals todo:struct |
| `0x0041b64b` | `llm_ui_hud_widget_hit_test` | `int llm_ui_hud_widget_hit_test(int widget_id)` | conf:high |
| `0x0041b6db` | `llm_ui_icon_rect_hit_test` | `int llm_ui_icon_rect_hit_test(int icon_index)` | conf:high todo:globals todo:struct |
| `0x0041b76c` | `llm_ui_icon_rect_hit_test_cached` | `int llm_ui_icon_rect_hit_test_cached(int icon_index)` | conf:med todo:globals todo:struct |
| `0x0041b7fd` | `llm_ui_icon_rect_hit_test_rmb` | `int llm_ui_icon_rect_hit_test_rmb(int widget_id)` | conf:high |
| `0x0041b88e` | `llm_ui_icon_rect_hit_test_rmb_cached` | `int llm_ui_icon_rect_hit_test_rmb_cached(int widget_id)` | conf:high |
| `0x0041b91f` | `llm_ui_slot_icon_draw` | `void llm_ui_slot_icon_draw(int slot_idx, int icon_idx)` | conf:high |
| `0x0041b9a4` | `llm_ui_icon_draw_vclipped` | `void llm_ui_icon_draw_vclipped(int x, int y, undefined2 * icon)` | conf:high |
| `0x0041ba63` | `llm_ui_hud_widget_icon_blit` | `void llm_ui_hud_widget_icon_blit(int widget_id, int icon_id)` | conf:high |
| `0x0041bb63` | `llm_str_itoa_pad_left` | `ushort * llm_str_itoa_pad_left(int value, int min_width, byte pad_char)` | conf:med todo:globals |
| `0x0041bbd2` | `llm_ui_hud_panel_column_fill_draw` | `void llm_ui_hud_panel_column_fill_draw(int y_start)` | conf:high |
| `0x0041bd25` | `llm_ui_panel_background_tile_draw` | `void llm_ui_panel_background_tile_draw(int y_start)` | conf:med todo:globals |
| `0x0041be17` | `llm_ui_panel_text_draw` | `void llm_ui_panel_text_draw(int surface_flag, int x, int y, void * text, int font_slot)` | conf:high |
| `0x0041be9d` | `llm_ui_text_draw_rgb16` | `void llm_ui_text_draw_rgb16(int x, int y, char * text, ushort color)` | conf:high |
| `0x0041beeb` | `llm_gfx_icon_sprite_pixel_width` | `ushort llm_gfx_icon_sprite_pixel_width(int icon_index)` | conf:med |
| `0x0041bf38` | `llm_gfx_draw_icon_progress_bar` | `void llm_gfx_draw_icon_progress_bar(int icon_family, int x, int y, uint value, int scale)` | conf:low todo:globals |
| `0x0041bfe3` | `llm_ui_hud_icon_draw_clipped` | `void llm_ui_hud_icon_draw_clipped(int x, int y, int clip_width, int icon_index)` | conf:med |
| `0x0041c061` | `llm_ui_bldg_panel_open` | `void llm_ui_bldg_panel_open(void)` | conf:med |
| `0x0041c19c` | `llm_game_load_available_projects` | `int llm_game_load_available_projects(int file_handle)` | conf:high |
| `0x0041c245` | `llm_ui_bldg_panel_load_state` | `int llm_ui_bldg_panel_load_state(int file_handle)` | conf:med |
| `0x0041c2ac` | `llm_tact_move_path_build` | `void llm_tact_move_path_build(void)` | conf:med |
| `0x0041c7e7` | `llm_strat_group_all_members_on_valid_region` | `int llm_strat_group_all_members_on_valid_region(void)` | conf:high |
| `0x0041c86c` | `llm_strat_group_move_order_commit` | `void llm_strat_group_move_order_commit(int goal_x, int goal_y, int player_id, int member_count, int move_group_id, uint is_plain_move_flag, uint target_class)` | conf:high |
| `0x0041ca37` | `llm_strat_group_move_order_pathfind` | `void llm_strat_group_move_order_pathfind(int mode, byte target_mask)` | conf:med |
| `0x0041dbfe` | `llm_strat_group_plan_formation_positions` | `void llm_strat_group_plan_formation_positions(byte target_type_mask)` | conf:med |
| `0x0041e790` | `llm_strat_pathfind_dir_code_from_delta` | `int llm_strat_pathfind_dir_code_from_delta(int d_col_sign, int d_row_sign)` | conf:high |
| `0x0041e836` | `llm_strat_pathfind_build_steps` | `int llm_strat_pathfind_build_steps(int start_x, int start_y, int target_range, undefined4 unused_reserved, int path_slot_index)` | conf:med |
| `0x0041ed6d` | `llm_map_region_find_nearest_valid_tile` | `int llm_map_region_find_nearest_valid_tile(byte * col, byte * row)` | conf:high |
| `0x0041ef3e` | `llm_strat_pathfind_trace_route` | `int llm_strat_pathfind_trace_route(byte src_col, byte src_row, byte dst_col, byte dst_row, void * step_ctx)` | conf:med |
| `0x0041f7ef` | `llm_strat_group_path_step_record` | `void llm_strat_group_path_step_record(int order_idx, uint heading, int col, int row)` | conf:med todo:enum |
| `0x0041fdda` | `llm_strat_group_path_step_append` | `void llm_strat_group_path_step_append(int order_idx, uint heading)` | conf:high |
| `0x0041fea7` | `llm_strat_unit_path_queue_count` | `int llm_strat_unit_path_queue_count(int unit_index, int max_len)` | conf:med |
| `0x0041fff5` | `llm_strat_pathfind_find_closer_visible_tile` | `int llm_strat_pathfind_find_closer_visible_tile(byte src_col, byte src_row, uint * out_col, uint * out_row, int dst_col, int dst_row)` | conf:high |
| `0x004201e4` | `llm_map_region_walk_to_valid_tile` | `int llm_map_region_walk_to_valid_tile(uint src_col, uint src_row, uint dst_col, uint dst_row, uint * out_col, uint * out_row)` | conf:high |
| `0x0042039c` | `llm_strat_pathfind_plan_group_route` | `int llm_strat_pathfind_plan_group_route(uint start_col, uint start_row, uint dest_col, uint dest_row, uint * out_col, uint * out_row)` | conf:med |
| `0x00420616` | `llm_strat_pathfind_route_leg_reconcile` | `int llm_strat_pathfind_route_leg_reconcile(int step_count)` | conf:med |
| `0x00420919` | `llm_strat_pathfind_mark_group_member_regions` | `void llm_strat_pathfind_mark_group_member_regions(void)` | conf:high |
| `0x004209b2` | `llm_strat_unit_path_detour` | `int llm_strat_unit_path_detour(int player, int unit_idx, int alt_unit_idx)` | conf:med |
| `0x00421758` | `llm_strat_unit_path_queue_splice` | `int llm_strat_unit_path_queue_splice(int owner_index, int unit_index, int slot, int count)` | conf:med |
| `0x004218a1` | `llm_map_tile_distance_wrapped` | `int llm_map_tile_distance_wrapped(int col_a, int row_a, int col_b, int row_b)` | conf:high |
| `0x004219b7` | `llm_stack_capacity_guard_0x20` | `void llm_stack_capacity_guard_0x20(void)` | conf:med |
| `0x004219dd` | `llm_strat_pathfind_target_hook_stub` | `void llm_strat_pathfind_target_hook_stub(int target_col, int target_row)` | conf:med |
| `0x00421a03` | `llm_map_minimap_render` | `void llm_map_minimap_render(void)` | conf:high |
| `0x0042207b` | `llm_map_compute_obstacle_proximity_flags` | `void llm_map_compute_obstacle_proximity_flags(void)` |  |
| `0x0042239a` | `llm_map_region_free` | `void llm_map_region_free(undefined4 param_1)` |  |
| `0x00422450` | `llm_map_region_flood_fill` | `int llm_map_region_flood_fill(undefined4 x, undefined4 y, llm_map_region * block)` |  |
| `0x004228e9` | `llm_map_try_seed_region_at` | `void llm_map_try_seed_region_at(undefined4 x, undefined4 y, undefined4 max_d)` |  |
| `0x004229c3` | `llm_map_region_pick_smaller` | `llm_map_region * llm_map_region_pick_smaller(llm_map_region * block, int x, int y)` |  |
| `0x00422a42` | `llm_map_assign_remaining_tiles_to_regions` | `void llm_map_assign_remaining_tiles_to_regions(void)` |  |
| `0x00422c54` | `llm_map_seed_regions_multires` | `void llm_map_seed_regions_multires(void)` |  |
| `0x00422e09` | `llm_map_region_add_adjacency_edge` | `void llm_map_region_add_adjacency_edge(llm_map_region * region_a, llm_map_region * region_b)` |  |
| `0x00422f0c` | `llm_map_region_recompute_adjacency` | `void llm_map_region_recompute_adjacency(void)` | conf:med |
| `0x0042308b` | `llm_map_compute_region_merge_threshold` | `void llm_map_compute_region_merge_threshold(void)` |  |
| `0x004230f7` | `llm_map_merge_small_regions` | `void llm_map_merge_small_regions(void)` |  |
| `0x00423299` | `llm_map_init_region_route_step_deltas` | `void llm_map_init_region_route_step_deltas(void)` |  |
| `0x00423335` | `llm_map_build_regions` | `void llm_map_build_regions(void)` |  |
| `0x004234b8` | `llm_map_region_pool_reset` | `void llm_map_region_pool_reset(void)` |  |
| `0x0042352e` | `llm_map_region_neighbor_edge_value` | `int llm_map_region_neighbor_edge_value(int param_1, int param_2)` | conf:med |
| `0x004235a1` | `llm_map_region_first_shared_node` | `int llm_map_region_first_shared_node(int param_1, int param_2)` | conf:med |
| `0x00423638` | `llm_map_region_route_mark_shared_nodes` | `void llm_map_region_route_mark_shared_nodes(int param_1)` | conf:med |
| `0x004236c2` | `llm_map_region_route_search` | `int llm_map_region_route_search(ushort start_region, short target_region, byte * out_path)` | conf:med |
| `0x0042398d` | `llm_map_region_bfs_reachable_flood` | `bool llm_map_region_bfs_reachable_flood(ushort dest_packed_coord, ushort start_packed_coord)` | conf:med |
| `0x00423d86` | `llm_map_region_find_route` | `int llm_map_region_find_route(uint to_tile_idx, uint from_tile_idx)` | conf:med |
| `0x00423fb8` | `llm_map_region_split` | `llm_map_region * llm_map_region_split(llm_map_region * old_region, int seed_col, int seed_row)` | conf:med |
| `0x004245c3` | `llm_map_region_apply_area` | `void llm_map_region_apply_area(uint tile_col_origin, int tile_row_origin, char * area_mask)` | conf:high |
| `0x00424773` | `llm_map_save_regions` | `void llm_map_save_regions(int file_handle)` | conf:high |
| `0x00424a68` | `llm_map_load_regions` | `void llm_map_load_regions(int file_handle)` | conf:high |
| `0x00424e6e` | `llm_map_region_route_prepass` | `void llm_map_region_route_prepass(void)` | conf:med |
| `0x00424ee0` | `llm_map_region_flood_reachable` | `int llm_map_region_flood_reachable(int query_cell, int start_cell)` | conf:med |
| `0x00425098` | `llm_tact_move_flood_reachable_tile` | `void llm_tact_move_flood_reachable_tile(void)` | conf:high |
| `0x00425233` | `llm_snd_play` | `void llm_snd_play(int sound_id, int volume)` |  |
| `0x0042542a` | `llm_snd_shutdown` | `void llm_snd_shutdown(void)` | conf:high |
| `0x0042547d` | `llm_cfg_read_line` | `int llm_cfg_read_line(char * out_line)` | conf:high todo:globals |
| `0x0042552a` | `llm_snd_load_sound_cfg` | `undefined llm_snd_load_sound_cfg(void)` |  |
| `0x004258e8` | `llm_strat_play_select_voice_single` | `void llm_strat_play_select_voice_single(void)` |  |
| `0x0042594d` | `llm_strat_play_select_voice_group` | `void llm_strat_play_select_voice_group(void)` |  |
| `0x004259b2` | `llm_strat_group_order_ack_voice` | `void llm_strat_group_order_ack_voice(void)` | conf:high |
| `0x00425a6e` | `llm_strat_race_alert_sound_emit` | `void llm_strat_race_alert_sound_emit(void)` | conf:med todo:globals |
| `0x00425b2a` | `llm_strat_race_alert_text_emit` | `void llm_strat_race_alert_text_emit(void)` | conf:med todo:globals |
| `0x00425bcd` | `llm_game_reload_snapshot_resync_clocks` | `void llm_game_reload_snapshot_resync_clocks(double now)` | conf:med |
| `0x00425cc8` | `llm_snd_stop_stale_voice_channels` | `void llm_snd_stop_stale_voice_channels(void)` |  |
| `0x00425d38` | `llm_snd_release_idle_channels` | `void llm_snd_release_idle_channels(void)` | conf:med |
| `0x00425d8c` | `llm_gfx_planet_bank_needed` | `int llm_gfx_planet_bank_needed(int bank_index)` | conf:med |
| `0x00425de9` | `llm_gfx_planet_bank_free` | `void llm_gfx_planet_bank_free(void)` | conf:high |
| `0x00425e1f` | `llm_gfx_planet_bank_load` | `void llm_gfx_planet_bank_load(void)` | conf:high todo:globals todo:struct |
| `0x0042613b` | `llm_fatal_cleanup` | `void llm_fatal_cleanup(void)` |  |
| `0x004261bb` | `llm_game_init_subsystems` | `undefined llm_game_init_subsystems(void)` |  |
| `0x004262b4` | `llm_boot_stage_tick` | `undefined llm_boot_stage_tick(void)` |  |
| `0x00426383` | `llm_frame_present` | `void llm_frame_present(void)` |  |
| `0x004263d3` | `llm_gfx_surface_lock` | `void llm_gfx_surface_lock(uint surface_mask)` | conf:med |
| `0x00426418` | `llm_gfx_surface_unlock` | `void llm_gfx_surface_unlock(uint surface_mask)` | conf:med |
| `0x0042644a` | `llm_gfx_present_flip` | `void llm_gfx_present_flip(void)` |  |
| `0x0042648a` | `llm_gfx_display_init` | `void llm_gfx_display_init(int width, int height)` | conf:med |
| `0x004265a7` | `llm_gfx_cursor_overlay_set` | `void llm_gfx_cursor_overlay_set(int sprite_id, int slot)` | conf:high |
| `0x00426645` | `llm_input_mouse_delta_pump` | `void llm_input_mouse_delta_pump(void)` | conf:med todo:globals todo:struct |
| `0x00426712` | `llm_input_mouse_buttons_get` | `int llm_input_mouse_buttons_get(void)` | conf:high todo:globals |
| `0x0042673f` | `llm_gfx_draw_line_clipped` | `void llm_gfx_draw_line_clipped(int x1, int y1, int x2, int y2, byte color_r, byte color_g, byte color_b)` | conf:high todo:globals |
| `0x00426983` | `llm_gfx_font_free_polalfa` | `void llm_gfx_font_free_polalfa(void)` | conf:high |
| `0x004269b9` | `llm_gfx_init_blend_luts` | `undefined llm_gfx_init_blend_luts(void)` |  |
| `0x00426f33` | `llm_planet_tlo_thumbnail_build` | `undefined llm_planet_tlo_thumbnail_build(void)` |  |
| `0x00427658` | `llm_gfx_dashed_rect_outline_set_and_draw` | `void llm_gfx_dashed_rect_outline_set_and_draw(int x1, int y1, int x2, int y2, ushort color)` | conf:med todo:globals |
| `0x004276b0` | `llm_strat_minimap_frame` | `void llm_strat_minimap_frame(void)` |  |
| `0x00427978` | `llm_strat_minimap_cam_goto` | `void llm_strat_minimap_cam_goto(uint mode, int marker_slot)` | conf:med |
| `0x00427c6c` | `llm_tact_move_find_approach_tile` | `void llm_tact_move_find_approach_tile(int from_col, int from_row, int target_col, int target_row, int ring_radius_min, int ring_radius_max, uint * out_col, uint * out_row, int unused_param9, undefined1 * out_dir)` |  |
| `0x00428201` | `llm_gfx_draw_sprite_with_bar` | `void llm_gfx_draw_sprite_with_bar(int sprite_id, int x, int y, int fill_percent, ushort viewbox_width)` | conf:med todo:struct |
| `0x004282a3` | `llm_gfx_build_bar_sprite_rle` | `void llm_gfx_build_bar_sprite_rle(int sprite_id, int bar_row_count, int fill_percent, ushort viewbox_width)` | conf:med todo:globals todo:struct |
| `0x004285c7` | `llm_gfx_set_window_resolution` | `undefined llm_gfx_set_window_resolution(void)` |  |
| `0x00428802` | `llm_tact_gfx_init_view_tile_ptrs` | `void llm_tact_gfx_init_view_tile_ptrs(void)` | conf:med |
| `0x0042894b` | `llm_gfx_release_display_surfaces` | `undefined llm_gfx_release_display_surfaces(void)` | conf:med todo:proto todo:struct |
| `0x00428981` | `llm_gfx_display_mode_apply` | `void llm_gfx_display_mode_apply(void)` | conf:med |
| `0x004289cf` | `llm_gfx_display_teardown` | `void llm_gfx_display_teardown(void)` | conf:med |
| `0x00428a0f` | `llm_gfx_apply_window_resolution` | `void llm_gfx_apply_window_resolution(int width, int height)` | conf:high |
| `0x00428ae2` | `llm_game_main_window_destroy` | `void llm_game_main_window_destroy(void)` | conf:med |
| `0x00428b11` | `llm_map_fog_of_war_recompute` | `void llm_map_fog_of_war_recompute(void)` | conf:high |
| `0x00428d9e` | `llm_tact_gfx_load_banks_alt` | `void llm_tact_gfx_load_banks_alt(void)` | conf:high todo:struct |
| `0x004290ea` | `llm_tact_mission_start` | `void llm_tact_mission_start(void)` |  |
| `0x00429371` | `llm_gfx_apply_resolution_change` | `void llm_gfx_apply_resolution_change(void)` | conf:med todo:globals |
| `0x004294e3` | `llm_tact_gfx_view_tile_rows_init` | `void llm_tact_gfx_view_tile_rows_init(void)` | conf:high |
| `0x00429634` | `llm_tact_frame_cursor_and_reset` | `void llm_tact_frame_cursor_and_reset(void)` | conf:high |
| `0x00429698` | `llm_gfx_draw_sprite_v2` | `void llm_gfx_draw_sprite_v2(int sprite_id, int dst_x, int dst_y, int tile_layer)` |  |
| `0x004296fd` | `llm_gfx_draw_sprite_rle` | `void llm_gfx_draw_sprite_rle(int sprite_id, int dst_x, int dst_y, int tile_layer)` | conf:high todo:globals |
| `0x00429776` | `llm_tact_view_metrics_init` | `void llm_tact_view_metrics_init(void)` | conf:high |
| `0x004299d3` | `llm_tact_drag_box_clamp` | `void llm_tact_drag_box_clamp(int x1, int y1, int x2, int y2)` |  |
| `0x00429b1a` | `llm_tact_frame` | `void llm_tact_frame(void)` | conf:high |
| `0x0042ad7f` | `llm_tact_units_reset_hp_for_active` | `void llm_tact_units_reset_hp_for_active(void)` | conf:high |
| `0x0042addd` | `llm_ui_debug_stats_overlay_draw` | `void llm_ui_debug_stats_overlay_draw(void)` | conf:med todo:globals |
| `0x0042af6d` | `llm_tact_selection_clear_unless_ctrl` | `void llm_tact_selection_clear_unless_ctrl(void)` | conf:high |
| `0x0042aff2` | `llm_tact_update_units_and_fx` | `void llm_tact_update_units_and_fx(void)` |  |
| `0x0042b09f` | `llm_tact_group_issue_order` | `int llm_tact_group_issue_order(int op, uint arg0, uint arg1, uint arg2, uint arg3)` |  |
| `0x0042b39d` | `llm_tact_unit_enqueue_command` | `int llm_tact_unit_enqueue_command(int unit_id, int op, byte interrupt_flag, int arg0, ushort arg1, ushort arg2, ushort arg3)` | conf:high |
| `0x0042b948` | `llm_tact_unit_spawn` | `int llm_tact_unit_spawn(int char_type, int col, int row, byte facing_dir, byte def_stat, int hp_pct)` |  |
| `0x0042bdce` | `llm_tact_fx_spawn` | `int llm_tact_fx_spawn(int fx_type, byte owner, int x, int y, int x2, int y2, byte altitude)` |  |
| `0x0042c274` | `llm_tact_unit_render` | `void llm_tact_unit_render(int building_id, int x, int y, int col, int row)` |  |
| `0x0042c48b` | `llm_tact_unit_apply_move_offset` | `void llm_tact_unit_apply_move_offset(int building_id, int * out_x, int * out_y)` |  |
| `0x0042c547` | `llm_tact_unit_update_anim` | `void llm_tact_unit_update_anim(int building_id)` |  |
| `0x0042d0cf` | `llm_tact_quantize_facing_dir` | `int llm_tact_quantize_facing_dir(int notch_span, int facing_dir)` | conf:high |
| `0x0042d127` | `llm_tact_fx_draw_entity` | `void llm_tact_fx_draw_entity(int fx_index)` |  |
| `0x0042d237` | `llm_tact_render_view` | `void llm_tact_render_view(void)` |  |
| `0x0042ddde` | `llm_tact_unit_pose_tag_select` | `void llm_tact_unit_pose_tag_select(uint pose_id)` | conf:med todo:enum todo:globals |
| `0x0042de81` | `llm_tact_fx_build_tile_draw_lists` | `void llm_tact_fx_build_tile_draw_lists(void)` |  |
| `0x0042e0e2` | `llm_tact_calc_dir24` | `int llm_tact_calc_dir24(int x1, int y1, int x2, int y2)` |  |
| `0x0042e1c9` | `llm_tact_vision_cone_setup` | `void llm_tact_vision_cone_setup(int col, int row, int angle_base, uint angle_width, int vision_dist, void * dead_outptr0, void * dead_outptr1, void * dead_outptr2, void * dead_outptr3)` | conf:high |
| `0x0042e220` | `llm_tact_fov_probe_far_cell_and_door_state` | `void llm_tact_fov_probe_far_cell_and_door_state(int unit_idx, int * out_far_col, uint * out_far_row, int * out_cell2_col, uint * out_cell2_row, int * out_door_animating_flag)` | conf:med |
| `0x0042e385` | `llm_tact_unit_vision_add` | `void llm_tact_unit_vision_add(int unit_idx)` | conf:high |
| `0x0042e53f` | `llm_tact_unit_vision_remove` | `void llm_tact_unit_vision_remove(int unit_idx)` | conf:high conf:med |
| `0x0042e6d8` | `llm_tact_tile_fog_set_origin` | `void llm_tact_tile_fog_set_origin(int origin_a, int origin_b)` | conf:med todo:globals |
| `0x0042e713` | `llm_tact_map_reset` | `void llm_tact_map_reset(char * mission_name)` | conf:high |
| `0x0042e889` | `llm_tact_camera_center_on_tile` | `void llm_tact_camera_center_on_tile(int target_col, int target_row)` | conf:high |
| `0x0042e957` | `llm_tact_cam_follow_selection_tick` | `void llm_tact_cam_follow_selection_tick(void)` | conf:high |
| `0x0042ea8a` | `llm_tact_move_path_preview_clear` | `void llm_tact_move_path_preview_clear(void)` | conf:high |
| `0x0042eaf9` | `llm_tact_move_path_preview_walk` | `void llm_tact_move_path_preview_walk(int start_col, int start_row, int path_slot_id, int * out_col, int * out_row)` | conf:high |
| `0x0042ed56` | `llm_tact_blink_overlay_clear` | `void llm_tact_blink_overlay_clear(void)` | conf:high |
| `0x0042edb8` | `llm_tact_select_next_unit` | `void llm_tact_select_next_unit(void)` | conf:high |
| `0x0042ef19` | `llm_snd_stop_all_channels` | `void llm_snd_stop_all_channels(void)` | conf:high todo:globals |
| `0x0042ef7b` | `llm_tact_ambient_sound_tick` | `void llm_tact_ambient_sound_tick(void)` | conf:high |
| `0x0042f060` | `llm_tact_fx_play_sound` | `void llm_tact_fx_play_sound(int fx_type, int volume_pct, int pan)` |  |
| `0x0042f229` | `llm_tact_zone_sound_play` | `void llm_tact_zone_sound_play(int sound_id)` | conf:med |
| `0x0042f2b7` | `llm_snd_play_using_cfg_volume` | `void llm_snd_play_using_cfg_volume(int sound_id)` | conf:med |
| `0x0042f338` | `llm_tact_unit_weapons_tick` | `void llm_tact_unit_weapons_tick(int building_id)` |  |
| `0x0042f8e4` | `llm_tact_unit_cmd_advance_with_defstat` | `void llm_tact_unit_cmd_advance_with_defstat(int unit_idx, int cmd_or_slot_index)` |  |
| `0x0042f935` | `llm_tact_unit_mine_arm_tick` | `void llm_tact_unit_mine_arm_tick(int unit_id)` | conf:high |
| `0x0042fb0b` | `llm_tact_unit_death_tick` | `int llm_tact_unit_death_tick(int unit_idx)` | conf:med |
| `0x0042fb9b` | `llm_tact_unit_move_tick` | `void llm_tact_unit_move_tick(int unit_idx, int cmd_slot, double dt)` |  |
| `0x00430363` | `llm_tact_unit_kneel_tick` | `void llm_tact_unit_kneel_tick(int unit_id)` | conf:high |
| `0x0043047d` | `llm_tact_unit_stand_tick` | `void llm_tact_unit_stand_tick(int unit_idx)` | conf:high |
| `0x00430580` | `llm_tact_unit_cmd_stance_on` | `void llm_tact_unit_cmd_stance_on(int unit_index)` | conf:high |
| `0x0043060f` | `llm_tact_unit_cmd_stance_off` | `void llm_tact_unit_cmd_stance_off(int unit_index)` | conf:high |
| `0x0043069e` | `llm_tact_unit_cmd_queue_resubmit_run` | `void llm_tact_unit_cmd_queue_resubmit_run(int unit_idx, int queue_slot)` | conf:med |
| `0x004307c5` | `llm_tact_unit_rotate_tick` | `void llm_tact_unit_rotate_tick(int unit_idx)` | conf:high |
| `0x00430855` | `llm_tact_unit_fire_weapon` | `void llm_tact_unit_fire_weapon(int building_id, int weapon_subindex, int fire_arg)` |  |
| `0x00430c77` | `llm_tact_weapon_calc_scatter` | `void llm_tact_weapon_calc_scatter(int building_id, int x, int y, int tx, int ty, int * out_dx, int * out_dy, int weapon_slot)` |  |
| `0x00430df1` | `llm_tact_unit_rotate_step` | `void llm_tact_unit_rotate_step(int building_id, int target_dir)` |  |
| `0x00430f03` | `llm_tact_unit_set_anim_state` | `void llm_tact_unit_set_anim_state(int building_id, byte state)` |  |
| `0x00430f5a` | `llm_tact_unit_move_advance` | `void llm_tact_unit_move_advance(int unit_idx, undefined4 arg_edx_unused, undefined4 arg_ebx_unused, int delta_col, int delta_row)` | conf:high |
| `0x00431166` | `llm_tact_unit_get_facing_octant` | `int llm_tact_unit_get_facing_octant(int unit_idx)` | conf:high |
| `0x004311bf` | `llm_tact_facing_to_delta` | `void llm_tact_facing_to_delta(int facing_dir, int * out_dx, int * out_dy)` |  |
| `0x00431229` | `llm_tact_unit_cmd_advance` | `void llm_tact_unit_cmd_advance(int unit_index, int cmd_slot_index)` | conf:high |
| `0x004313cf` | `llm_tact_unit_cmd_queue_advance` | `void llm_tact_unit_cmd_queue_advance(int unit_idx, uint cmd_index)` | conf:med |
| `0x004314aa` | `llm_tact_fx_splash_damage` | `void llm_tact_fx_splash_damage(int col, int row, int radius_tiles, int damage)` |  |
| `0x00431654` | `llm_tact_fx_update_projectile` | `void llm_tact_fx_update_projectile(int fx_index)` |  |
| `0x00431d8d` | `llm_tact_unit_destroy` | `void llm_tact_unit_destroy(uint unit_idx)` | conf:high |
| `0x00432048` | `llm_tact_unit_despawn` | `void llm_tact_unit_despawn(int unit_idx)` | conf:high |
| `0x00432105` | `llm_tact_unit_get_muzzle_offset` | `void llm_tact_unit_get_muzzle_offset(int unit_id, int * out_x, int * out_y, uint weapon_slot)` | conf:high |
| `0x00432266` | `llm_tact_door_anim_start` | `void llm_tact_door_anim_start(int door_idx)` | conf:high |
| `0x004322c1` | `llm_tact_door_tick` | `void llm_tact_door_tick(void)` | conf:high |
| `0x004324c0` | `llm_tact_door_update_tile_state` | `void llm_tact_door_update_tile_state(int door_idx)` | conf:med |
| `0x00432a05` | `llm_tact_door_path_clear` | `int llm_tact_door_path_clear(int door_idx)` | conf:high |
| `0x00432ba1` | `llm_tact_teleport_zone_scan_tick` | `void llm_tact_teleport_zone_scan_tick(void)` |  |
| `0x00432df0` | `llm_tact_unit_teleport` | `void llm_tact_unit_teleport(int teleport_id, int building_id)` |  |
| `0x00433464` | `llm_tact_unit_cmd_teleport_jump_tick` | `void llm_tact_unit_cmd_teleport_jump_tick(int unit_idx, int cmd_queue_slot)` | conf:high |
| `0x004334da` | `llm_tact_teleport_cmdqueue_jump` | `int llm_tact_teleport_cmdqueue_jump(int unit_id, int dest_x, int dest_y)` |  |
| `0x0043356e` | `llm_tact_tile_rebuild_occupancy_layer_for_map` | `void llm_tact_tile_rebuild_occupancy_layer_for_map(int map_id)` | conf:med |
| `0x004335a0` | `llm_tact_unit_owner_tick` | `void llm_tact_unit_owner_tick(uint owner)` |  |
| `0x00433c0d` | `llm_tact_unit_weapon_in_range` | `int llm_tact_unit_weapon_in_range(int unit_idx, int target_x, int target_y)` | conf:high |
| `0x00433d57` | `llm_tact_calc_approach_dir24_to_tile_stamp` | `int llm_tact_calc_approach_dir24_to_tile_stamp(int unit_idx, int tile_col, int tile_row)` | conf:med |
| `0x00433dfc` | `llm_tact_tile_rebuild_occupancy_layer` | `void llm_tact_tile_rebuild_occupancy_layer(void)` | conf:high |
| `0x00433e94` | `llm_tact_ui_sel_panel_init` | `void llm_tact_ui_sel_panel_init(void)` | conf:high |
| `0x00434051` | `llm_tact_ui_sel_panel_free_gfx` | `void llm_tact_ui_sel_panel_free_gfx(void)` | conf:med todo:globals |
| `0x004340a2` | `llm_tact_unit_draw_hp_bar_slot` | `void llm_tact_unit_draw_hp_bar_slot(int unit_idx, int ui_slot_row)` | conf:high |
| `0x004341cd` | `llm_tact_ui_char_panel_ammo_draw` | `void llm_tact_ui_char_panel_ammo_draw(int unit_id, int row_index)` | conf:med todo:struct |
| `0x00434322` | `llm_tact_ui_char_panel_row_draw` | `void llm_tact_ui_char_panel_row_draw(int unit_idx, int row_slot)` | conf:med |
| `0x0043468d` | `llm_tact_ui_sidebar_row_draw_left` | `void llm_tact_ui_sidebar_row_draw_left(int unit_id, int row_index, int highlight_flag)` | conf:med todo:globals |
| `0x004348bf` | `llm_tact_ui_sidebar_row_draw_right` | `void llm_tact_ui_sidebar_row_draw_right(int unit_id, int row_index, int highlight_flag)` | conf:med |
| `0x00434af7` | `llm_tact_selection_panel_refresh` | `void llm_tact_selection_panel_refresh(void)` | conf:high |
| `0x00434b68` | `llm_tact_ui_sidebar_roster_refresh` | `void llm_tact_ui_sidebar_roster_refresh(void)` | conf:high todo:globals |
| `0x00434ceb` | `llm_tact_ui_sidebar_draw_rows` | `void llm_tact_ui_sidebar_draw_rows(void)` | conf:high todo:globals |
| `0x00434dc2` | `llm_tact_active_unit_count_hud_draw` | `void llm_tact_active_unit_count_hud_draw(void)` | conf:high |
| `0x00434f28` | `llm_tact_ui_char_panel_row_refresh` | `void llm_tact_ui_char_panel_row_refresh(int unit_id)` | conf:low todo:globals |
| `0x00434f98` | `llm_tact_unit_refresh_ui_slot` | `void llm_tact_unit_refresh_ui_slot(int building_id)` |  |
| `0x00435008` | `llm_tact_ui_sidebar_redraw_unit_slot` | `void llm_tact_ui_sidebar_redraw_unit_slot(int unit_id)` | conf:med todo:globals |
| `0x00435078` | `llm_tact_ui_sel_panel_draw` | `void llm_tact_ui_sel_panel_draw(void)` | conf:med todo:globals todo:struct |
| `0x004354d3` | `llm_tact_ui_draw_player_row_list` | `void llm_tact_ui_draw_player_row_list(int selected_row)` | conf:low todo:globals |
| `0x004356bd` | `llm_tact_squad_roster_refresh` | `void llm_tact_squad_roster_refresh(void)` | conf:high |
| `0x00435909` | `llm_tact_ui_mouse_in_rect` | `int llm_tact_ui_mouse_in_rect(int rect_x0, int rect_y0, int rect_x1, int rect_y1)` | conf:high |
| `0x00435972` | `llm_tact_sidebar_dispatch` | `void llm_tact_sidebar_dispatch(void)` | conf:high |
| `0x00435cea` | `llm_tact_ui_order_buttons_minimap_tick` | `void llm_tact_ui_order_buttons_minimap_tick(void)` | conf:high |
| `0x004361b5` | `llm_tact_ui_sel_panel_multi_mode_tick` | `void llm_tact_ui_sel_panel_multi_mode_tick(void)` | conf:med |
| `0x004369c1` | `llm_tact_ui_sel_panel_single_mode_tick` | `void llm_tact_ui_sel_panel_single_mode_tick(void)` | conf:med |
| `0x00436f30` | `llm_tact_fov_raycast_stencil` | `void llm_tact_fov_raycast_stencil(void)` | conf:med |
| `0x00437138` | `llm_tact_fov_update_nearest_target` | `void llm_tact_fov_update_nearest_target(int cell_index)` | conf:high |
| `0x0043717f` | `llm_tact_mission_load` | `void llm_tact_mission_load(char * filename)` |  |
| `0x00438f09` | `llm_tact_squad_sync_hp` | `void llm_tact_squad_sync_hp(void)` | conf:high |
| `0x00439005` | `llm_tact_scroll_target_proximity_tick` | `void llm_tact_scroll_target_proximity_tick(void)` | conf:high |
| `0x00439103` | `llm_tact_cfg_keyword_token_match` | `int llm_tact_cfg_keyword_token_match(char * line, char * keyword)` | conf:med |
| `0x004391e7` | `llm_tact_mission_parse_float` | `double llm_tact_mission_parse_float(char * line, double max_value)` | conf:high |
| `0x00439382` | `llm_tact_mission_parse_coord_pair` | `void llm_tact_mission_parse_coord_pair(char * text, uchar * out_col, uchar * out_row)` | conf:med |
| `0x00439526` | `llm_tact_mission_parse_disposition_spawn` | `int llm_tact_mission_parse_disposition_spawn(char * line, uint * out_col, uint * out_row)` | conf:high |
| `0x00439749` | `llm_tact_mission_parse_keyword_int` | `int llm_tact_mission_parse_keyword_int(char * line, char * keyword, int * out_value)` | conf:high |
| `0x004398eb` | `llm_tact_door_parse_definition` | `void llm_tact_door_parse_definition(char * door_def_line, int door_index)` | conf:high |
| `0x00439bdf` | `llm_tact_mission_parse_int_token` | `int llm_tact_mission_parse_int_token(char * line, uint * io_pos, int * out_value)` | conf:med |
| `0x00439cf5` | `llm_tact_door_apply_to_map` | `void llm_tact_door_apply_to_map(void)` | conf:med |
| `0x00439fa8` | `llm_tact_mission_parse_quoted_string` | `void llm_tact_mission_parse_quoted_string(char * line, char * out_buf)` | conf:high |
| `0x0043a06f` | `llm_tact_map_compute_bounds` | `void llm_tact_map_compute_bounds(void)` | conf:high |
| `0x0043a1d9` | `llm_tact_mission_parse_command_token` | `int llm_tact_mission_parse_command_token(char * mission_line, int * out_opcode, int * out_arg0, int * out_arg1, int * out_arg2, int * out_arg3)` | conf:med todo:enum |
| `0x0043a5b9` | `llm_tact_cfg_match_keyword_at_offset` | `int llm_tact_cfg_match_keyword_at_offset(char * text_base, int start_offset, char * keyword)` | conf:med |
| `0x0043a649` | `llm_tact_character_parse_frame_table` | `void llm_tact_character_parse_frame_table(char * frames_directive_line, int character_type_index)` | conf:med |
| `0x0043a7cd` | `llm_tlo_shade_table_build_tact` | `void llm_tlo_shade_table_build_tact(void)` | conf:high |
| `0x0043aa88` | `llm_gfx_draw_dashed_rect_outline` | `void llm_gfx_draw_dashed_rect_outline(void)` | conf:med todo:globals |
| `0x0043abfe` | `llm_tact_vis_map_fill_default` | `void llm_tact_vis_map_fill_default(void)` |  |
| `0x0043ac28` | `llm_tact_vis_map_clear_right_margin` | `void llm_tact_vis_map_clear_right_margin(void)` | conf:med |
| `0x0043ac65` | `llm_tact_tlo_tile_blit` | `void llm_tact_tlo_tile_blit(void)` | conf:high todo:globals |
| `0x0043b1ce` | `llm_gfx_draw_cursor_ingame` | `void llm_gfx_draw_cursor_ingame(void)` |  |
| `0x0043b241` | `llm_gfx_blit_sprite_rle` | `void llm_gfx_blit_sprite_rle(void)` | conf:high todo:globals todo:struct |
| `0x0043bbaf` | `llm_gfx_skip_sprite_row` | `undefined llm_gfx_skip_sprite_row(void)` | conf:high |
| `0x0043bc1a` | `llm_gfx_blit_finish` | `void llm_gfx_blit_finish(void)` | conf:high |
| `0x0043bc21` | `llm_gfx_blit_sprite_rle_v2` | `void llm_gfx_blit_sprite_rle_v2(void)` |  |
| `0x0043bfae` | `llm_gfx_hittest_check_blit` | `void llm_gfx_hittest_check_blit(void)` |  |
| `0x0043bfcf` | `llm_tact_tile_fog_compute_edges` | `void llm_tact_tile_fog_compute_edges(void)` | conf:med todo:globals todo:struct |
| `0x0043c0e1` | `llm_tact_view_shift_col_inc` | `void llm_tact_view_shift_col_inc(void)` | conf:high |
| `0x0043c155` | `llm_tact_view_shift_col_dec` | `void llm_tact_view_shift_col_dec(void)` | conf:high |
| `0x0043c1dd` | `llm_tact_view_shift_row_inc` | `void llm_tact_view_shift_row_inc(void)` | conf:med |
| `0x0043c266` | `llm_tact_view_shift_row_dec` | `void llm_tact_view_shift_row_dec(void)` | conf:high |
| `0x0043c303` | `llm_tact_mark_view_tiles_dirty` | `void llm_tact_mark_view_tiles_dirty(void)` | conf:high |
| `0x0043c323` | `llm_tact_scroll_fade_step` | `void llm_tact_scroll_fade_step(void)` | conf:high |
| `0x0043c39c` | `llm_tact_tile_clear_margin_patch` | `void llm_tact_tile_clear_margin_patch(void)` | conf:low |
| `0x0043c3dc` | `llm_tact_tile_overlay_refresh` | `void llm_tact_tile_overlay_refresh(void)` |  |
| `0x0043c4f2` | `llm_tact_ui_minimap_draw_viewport_outline` | `void llm_tact_ui_minimap_draw_viewport_outline(void)` | conf:high |
| `0x0043c56b` | `llm_gfx_pack_rgb16` | `ushort llm_gfx_pack_rgb16(undefined4 param_1, undefined4 param_2, undefined4 param_3)` |  |
| `0x0043c5f7` | `llm_gfx_init_pixel_format` | `void llm_gfx_init_pixel_format(byte * palettes, ushort * palettes_dark_out, int palettes_size)` |  |
| `0x0043c78c` | `llm_tlo_palette_convert_565_to_555` | `void llm_tlo_palette_convert_565_to_555(void)` |  |
| `0x0043c800` | `llm_gfx_convert_pixels_565_to_555` | `void llm_gfx_convert_pixels_565_to_555(short * sprite_blob)` | conf:high todo:struct |
| `0x0043c882` | `llm_gfx_downconvert_pixels_565_to_555` | `ushort * llm_gfx_downconvert_pixels_565_to_555(ushort * pixels, int count)` | conf:high |
| `0x0043c8f0` | `llm_gfx_save_screenshot_tga` | `void llm_gfx_save_screenshot_tga(char * filename_suffix)` | conf:high todo:globals |
| `0x0043cad5` | `llm_tlo_shade_table_build_planet` | `undefined llm_tlo_shade_table_build_planet(void)` |  |
| `0x0043cecc` | `llm_strat_pathfind_result_col` | `byte llm_strat_pathfind_result_col(uint packed_colrow)` | conf:med |
| `0x0043cef8` | `llm_strat_pathfind_result_row` | `byte llm_strat_pathfind_result_row(uint packed_colrow)` | conf:med |
| `0x0043cf1b` | `llm_strat_pathfind_next_step` | `int llm_strat_pathfind_next_step(int cur_col, int cur_row, int * target_col, int * target_row, byte * passable_grid, void * pf_template)` |  |
| `0x0043d01f` | `llm_strat_pathfind_route_bbox` | `void llm_strat_pathfind_route_bbox(byte * route_points, int point_count, byte * out_bbox, byte wrap_mask)` | conf:high todo:struct |
| `0x0043d18f` | `llm_strat_pathfind_sort_units_by_path_len` | `void llm_strat_pathfind_sort_units_by_path_len(ushort * unit_slot_index, uint * total_path_len, int count)` | conf:high |
| `0x0043d28e` | `llm_strat_pathfind_path_list_compact_all` | `void llm_strat_pathfind_path_list_compact_all(short * path_ctx)` | conf:low |
| `0x0043d591` | `llm_strat_pathfind_route_find_next_stop` | `int llm_strat_pathfind_route_find_next_stop(int start_pos_packed, byte * route_segments, byte wrap_mask, byte * passable_grid, ushort * walk_state, int find_blocked)` | conf:high |
| `0x0043d820` | `llm_strat_pathfind_route_node_splice` | `int llm_strat_pathfind_route_node_splice(void * route_window_a, void * route_window_b, undefined4 unused_param3, char * inout_node_list, char * insert_node_list)` | conf:med todo:struct |
| `0x0043da7e` | `llm_strat_pathfind_path_list_compact` | `void llm_strat_pathfind_path_list_compact(short * path_ctx, ushort * unit_result_colrow)` | conf:low |
| `0x0043dbe2` | `llm_strat_pathfind_route_start_adjust` | `int llm_strat_pathfind_route_start_adjust(short * route)` | conf:med todo:struct |
| `0x0043ddc6` | `llm_strat_pathfind_reserve_route_cells` | `void llm_strat_pathfind_reserve_route_cells(short * route, short * waypoints_colrow)` | conf:high todo:globals todo:struct |
| `0x0043de65` | `llm_strat_pathfind_restore_route_cells` | `void llm_strat_pathfind_restore_route_cells(short * route, short * waypoints_colrow)` | conf:high todo:globals todo:struct |
| `0x0043def7` | `llm_strat_pathfind_dispatch_route_order` | `int llm_strat_pathfind_dispatch_route_order(short * route)` | conf:low todo:globals todo:struct |
| `0x0043e5b8` | `llm_gfx_draw_blended_line` | `void llm_gfx_draw_blended_line(void)` | conf:low todo:globals |
| `0x0043ecfa` | `llm_strat_frame` | `void llm_strat_frame(void)` |  |
| `0x0043ed50` | `llm_strat_frame_tick_and_present` | `void llm_strat_frame_tick_and_present(void)` | conf:high |
| `0x0043ed98` | `llm_strat_frame_redraw_behind_dialog` | `void llm_strat_frame_redraw_behind_dialog(void)` | conf:high |
| `0x0043ede0` | `llm_strat_frame_sim_only` | `undefined llm_strat_frame_sim_only(void)` | conf:high |
| `0x0043ee0c` | `llm_pause_frame` | `undefined llm_pause_frame(void)` |  |
| `0x0043ee38` | `llm_wait_screen_frame` | `void llm_wait_screen_frame(void)` |  |
| `0x0043eea3` | `llm_strat_time_tick` | `int llm_strat_time_tick(void)` | todo:globals |
| `0x0043f3eb` | `llm_strat_sim_tick` | `void llm_strat_sim_tick(void)` |  |
| `0x0043f512` | `llm_strat_sim_step` | `void llm_strat_sim_step(void)` |  |
| `0x0043fe1e` | `llm_strat_check_population_change` | `void llm_strat_check_population_change(uint player)` |  |
| `0x0043fe9d` | `llm_strat_check_storage_overflow` | `void llm_strat_check_storage_overflow(int player)` |  |
| `0x00440138` | `llm_progress_notify_unit_available` | `void llm_progress_notify_unit_available(game_t_Player_s player, cfg_t_unit_index_s u_i)` |  |
| `0x004401a6` | `llm_game_notify_system_available` | `void llm_game_notify_system_available(game_t_Player_s player, int system_idx)` |  |
| `0x0044048c` | `llm_strat_revoke_invention` | `void llm_strat_revoke_invention(game_t_Player_s player, ushort progress_id)` |  |
| `0x004404cc` | `llm_progress_finalize_acquire` | `void llm_progress_finalize_acquire(game_t_Player_s player, cfg_t_invention_index_s pid)` |  |
| `0x00440cb5` | `llm_progress_propagate_unlocks` | `void llm_progress_propagate_unlocks(game_t_Player_s player)` |  |
| `0x00440e1c` | `llm_strat_projectile_tick` | `void llm_strat_projectile_tick(void)` |  |
| `0x004418a5` | `llm_strat_fx_anim_tick` | `void llm_strat_fx_anim_tick(void)` |  |
| `0x004419a8` | `llm_strat_projectile_draw` | `void llm_strat_projectile_draw(int pool_index)` |  |
| `0x00441a8e` | `llm_strat_fx_anim_draw` | `void llm_strat_fx_anim_draw(int anim_index, uchar layer)` |  |
| `0x00441b88` | `llm_strat_input_update` | `void llm_strat_input_update(void)` |  |
| `0x004445e5` | `llm_pause_input_wait` | `void llm_pause_input_wait(void)` |  |
| `0x004446f8` | `llm_map_cam_snap_to_ctrl_group0_if_offscreen` | `int llm_map_cam_snap_to_ctrl_group0_if_offscreen(void)` | conf:med |
| `0x00444894` | `llm_strat_group_issue_attack_order` | `void llm_strat_group_issue_attack_order(uint param_1, uint player, ushort selector)` |  |
| `0x00444c9c` | `llm_strat_ui_unload_group_from_storage` | `void llm_strat_ui_unload_group_from_storage(void)` |  |
| `0x00444e5f` | `llm_strat_group_issue_move_order_deferred` | `void llm_strat_group_issue_move_order_deferred(int op_code, int op_arg, int modifier)` |  |
| `0x00444fcb` | `llm_strat_group_issue_move_order_confirmed` | `void llm_strat_group_issue_move_order_confirmed(int dst_x, int dst_y)` |  |
| `0x004451d2` | `llm_strat_group_issue_enter_building_order` | `void llm_strat_group_issue_enter_building_order(uint param_1, uint param_2, uint param_3)` |  |
| `0x004452fe` | `llm_strat_group_issue_move_order` | `void llm_strat_group_issue_move_order(uint tile_x, uint tile_y)` |  |
| `0x004454a6` | `llm_unit_ctrl_group_recall` | `void llm_unit_ctrl_group_recall(int group)` |  |
| `0x00445531` | `llm_cursor_anim_tick` | `void llm_cursor_anim_tick(int anim_id)` |  |
| `0x004456c4` | `llm_hit_list_has_own` | `int llm_hit_list_has_own(void)` |  |
| `0x0044572d` | `llm_hit_list_find_own_of_type` | `int llm_hit_list_find_own_of_type(int * out_payload, ushort type_mask)` |  |
| `0x004457c6` | `llm_hit_list_has_enemy` | `int llm_hit_list_has_enemy(void)` |  |
| `0x0044582f` | `llm_hit_list_find_enemy_of_type` | `int llm_hit_list_find_enemy_of_type(uint * out_1, ushort * out_2, ushort type_mask)` |  |
| `0x004458e1` | `llm_map_tile_has_own` | `int llm_map_tile_has_own(int tile_col, int tile_row)` |  |
| `0x004459cd` | `llm_map_tile_find_own_of_type` | `bool llm_map_tile_find_own_of_type(int tile_col, int tile_row, uint * out_id, short type_flag)` |  |
| `0x00445b62` | `llm_map_tile_has_enemy` | `int llm_map_tile_has_enemy(int tile_col, int tile_row)` |  |
| `0x00445c4e` | `llm_map_tile_find_enemy_of_type` | `undefined4 llm_map_tile_find_enemy_of_type(int param_1, int param_2, uint * a2, ushort * param_4, short param_5)` |  |
| `0x00445e06` | `llm_ui_click_select_target` | `void llm_ui_click_select_target(uint target_id, uint target_flags)` | conf:med |
| `0x00445f27` | `llm_strat_ctrl_group_contains_unit` | `int llm_strat_ctrl_group_contains_unit(uint unit_id, int count, int group_index)` | conf:high |
| `0x00445f96` | `llm_strat_selection_drag_finish` | `int llm_strat_selection_drag_finish(void)` |  |
| `0x004468a0` | `llm_strat_selection_click_resolve` | `void llm_strat_selection_click_resolve(void)` |  |
| `0x00446e90` | `llm_strat_tile_collect_own_units_in_elev_range` | `void llm_strat_tile_collect_own_units_in_elev_range(int tile_x, int tile_y, undefined4 * collection, int elev_min, int elev_max)` |  |
| `0x00447017` | `llm_strat_tile_has_enemy_unit_in_elev_range` | `int llm_strat_tile_has_enemy_unit_in_elev_range(int tile_x, int tile_y, undefined4 unused, int elev_min, int elev_max)` |  |
| `0x004475de` | `llm_game_load` | `int llm_game_load(char * save_name)` |  |
| `0x00448661` | `llm_lzw_compress_and_write_block` | `int llm_lzw_compress_and_write_block(void * src, undefined4 file_h, uint size)` | rev:verified |
| `0x004487a7` | `llm_cfg_save_setup_dat` | `int llm_cfg_save_setup_dat(void)` | todo:globals |
| `0x004488ad` | `llm_game_check_setup_dat_exists` | `undefined llm_game_check_setup_dat_exists(void)` | todo:globals |
| `0x004489a7` | `llm_cfg_save_final_snapshot` | `void llm_cfg_save_final_snapshot(char * save_name)` | conf:high |
| `0x00448ad9` | `llm_cfg_load_final_snapshot` | `void llm_cfg_load_final_snapshot(char * save_name)` | conf:high |
| `0x00448c0b` | `llm_strat_bldg_try_begin_placement` | `int llm_strat_bldg_try_begin_placement(ushort player_idx, cfg_t_building_index building_idx)` | conf:high |
| `0x00448cc3` | `llm_strat_weapon_pixel_distance_ratio` | `float10 llm_strat_weapon_pixel_distance_ratio(int weapon_id, int x1, int y1, int x2, int y2)` | conf:med |
| `0x00448d55` | `llm_strat_unit_predict_coords_after_delay` | `void llm_strat_unit_predict_coords_after_delay(uint player, int unit_idx, undefined4 unused_ebx, undefined4 unused_ecx, double time_delta, uint * out_x, uint * out_y)` |  |
| `0x00448ee1` | `llm_strat_unit_update_target_tracking` | `int llm_strat_unit_update_target_tracking(uint player, int unit_idx)` |  |
| `0x00449062` | `llm_strat_unit_update_target2_tracking` | `int llm_strat_unit_update_target2_tracking(uint player, int unit_idx)` |  |
| `0x0044928f` | `llm_strat_unit_ctrlgroup_leave` | `void llm_strat_unit_ctrlgroup_leave(uint unit_index)` |  |
| `0x00449401` | `llm_strat_unit_ctrlgroup_add_member` | `void llm_strat_unit_ctrlgroup_add_member(int param_1, int * param_2, int a2)` |  |
| `0x0044947e` | `llm_strat_unit_ctrlgroup_remove_member` | `void llm_strat_unit_ctrlgroup_remove_member(uint unit_idx, int * count_ptr, int group_idx)` |  |
| `0x0044955f` | `llm_strat_unit_path_encode_directions` | `void llm_strat_unit_path_encode_directions(ushort player, int unit_idx)` | conf:med |
| `0x004495f9` | `llm_strat_target_class` | `int llm_strat_target_class(uint owner_and_kind_flag, int roster_slot)` |  |
| `0x0044965d` | `llm_strat_unit_in_weapon_range` | `undefined4 llm_strat_unit_in_weapon_range(int param_1, int param_2, int a2, int param_4, int param_5)` |  |
| `0x0044988f` | `llm_strat_unit_goal_in_weapon_range` | `int llm_strat_unit_goal_in_weapon_range(int player, int unit_idx, int target_x, int target_y, int target_kind)` |  |
| `0x00449ac1` | `llm_strat_dist_out_of_range` | `undefined4 llm_strat_dist_out_of_range(int param_1, int param_2, int a2, int param_4, int param_5, int param_6)` |  |
| `0x00449b28` | `llm_strat_dir_step_factor` | `double llm_strat_dir_step_factor(int dir)` |  |
| `0x00449b8a` | `llm_strat_bldg_get_coords` | `void llm_strat_bldg_get_coords(ushort player, int building_index, fine_coord * out_x, fine_coord * out_y)` |  |
| `0x00449c61` | `llm_strat_bldg_footprint_random_offset` | `void llm_strat_bldg_footprint_random_offset(undefined4 param_1, undefined4 param_2, int a2, int param_4, uint * param_5, uint * param_6)` |  |
| `0x00449d41` | `llm_strat_bldg_footprint_random_point` | `void llm_strat_bldg_footprint_random_point(undefined4 param_1, undefined4 param_2, int a2, int param_4, uint * param_5, uint * param_6)` |  |
| `0x00449e21` | `llm_strat_time_resync_and_tick` | `void llm_strat_time_resync_and_tick(void)` | conf:high |
| `0x00449e58` | `llm_strat_bldg_prod_get_queued_count` | `int llm_strat_bldg_prod_get_queued_count(ushort player_idx, int bldg_idx, int unit_type)` |  |
| `0x00449f1c` | `llm_strat_bldg_prod_get_active_unit_type` | `uchar llm_strat_bldg_prod_get_active_unit_type(uint player, int bldg_idx)` |  |
| `0x00449ffe` | `llm_strat_bldg_get_progress_percent` | `int llm_strat_bldg_get_progress_percent(ushort player, int b_index, int * out_progress_pct)` |  |
| `0x0044a362` | `llm_strat_bldg_get_production_substatus` | `int llm_strat_bldg_get_production_substatus(ushort player_idx, uint bldg_idx)` | conf:med |
| `0x0044a3e7` | `llm_strat_bldg_get_active_research_project` | `int llm_strat_bldg_get_active_research_project(uint player, int building_idx)` |  |
| `0x0044a469` | `llm_strat_unit_housing_get_vehicles` | `void llm_strat_unit_housing_get_vehicles(ushort player_idx, int * out_used_vehicles, int * out_cap_prev_vehicles)` | conf:high |
| `0x0044a4b4` | `llm_strat_unit_housing_get_soldiers` | `void llm_strat_unit_housing_get_soldiers(ushort player_idx, int * out_used_soldiers, int * out_cap_prev_soldiers)` | conf:high |
| `0x0044a4ff` | `llm_strat_unit_housing_get_helis` | `void llm_strat_unit_housing_get_helis(ushort player_idx, int * out_used_helis, int * out_cap_prev_helis)` | conf:high |
| `0x0044a54a` | `llm_strat_unit_housing_get_planes` | `void llm_strat_unit_housing_get_planes(ushort player_idx, int * out_used_planes, int * out_cap_prev_planes)` | conf:high |
| `0x0044a595` | `llm_strat_unit_count_alive_cached` | `int llm_strat_unit_count_alive_cached(int unit_index)` | todo:dead |
| `0x0044a5d2` | `llm_ctrl_group_get_count` | `int llm_ctrl_group_get_count(int group_idx)` | conf:high |
| `0x0044a60a` | `llm_strat_unit_count_ungrouped` | `int llm_strat_unit_count_ungrouped(uint player)` |  |
| `0x0044a685` | `llm_strat_unit_count_alive_excluding_ctrl_group` | `int llm_strat_unit_count_alive_excluding_ctrl_group(int player, uint ctrl_group_id)` |  |
| `0x0044a73d` | `llm_unit_ctrl_group_activate` | `void llm_unit_ctrl_group_activate(int group_id)` |  |
| `0x0044a7c5` | `llm_unit_ctrl_group_flash_observer` | `void llm_unit_ctrl_group_flash_observer(int group_id)` |  |
| `0x0044aa18` | `llm_strat_unit_ctrl_group_assign` | `void llm_strat_unit_ctrl_group_assign(int unit_id, int new_group_id)` |  |
| `0x0044aac5` | `llm_strat_unit_ctrl_group_assign_by_ref` | `void llm_strat_unit_ctrl_group_assign_by_ref(undefined4 unit_ref, int new_group_id)` | conf:med |
| `0x0044ab7d` | `llm_strat_ctrlgrp_remove_member_by_id` | `void llm_strat_ctrlgrp_remove_member_by_id(uint unit_id, int group_idx)` | conf:med |
| `0x0044abd8` | `llm_strat_ctrlgrp_remove_member_by_index` | `void llm_strat_ctrlgrp_remove_member_by_index(int member_index, int group_idx)` | conf:high |
| `0x0044ac4b` | `llm_strat_ai_ctrl_group_member_scan_dead` | `void llm_strat_ai_ctrl_group_member_scan_dead(int ctrl_group_index)` | conf:med todo:dead |
| `0x0044ac95` | `llm_strat_unit_find_next_alive` | `int llm_strat_unit_find_next_alive(int unit_idx)` |  |
| `0x0044ad08` | `llm_strat_unit_find_nth_alive` | `int llm_strat_unit_find_nth_alive(int nth_alive)` |  |
| `0x0044ad87` | `llm_strat_ctrlgrp_get_unit_id_at_count` | `ushort llm_strat_ctrlgrp_get_unit_id_at_count(int group_idx, int flag)` | todo:dead |
| `0x0044adef` | `llm_strat_unit_find_nth_alive_excluding_ctrl_group` | `int llm_strat_unit_find_nth_alive_excluding_ctrl_group(int nth, uint ctrl_group_id)` |  |
| `0x0044ae93` | `llm_strat_unit_find_nth_alive_unassigned` | `int llm_strat_unit_find_nth_alive_unassigned(int nth)` |  |
| `0x0044af33` | `llm_map_cam_set_col` | `void llm_map_cam_set_col(int col)` |  |
| `0x0044af6d` | `llm_map_cam_set_row` | `void llm_map_cam_set_row(int row)` |  |
| `0x0044afa7` | `llm_strat_unit_tile_chain_fix_self_loop` | `void llm_strat_unit_tile_chain_fix_self_loop(int unused_reserved)` | conf:med |
| `0x0044b09e` | `llm_unit_status_bit_set` | `void llm_unit_status_bit_set(int unit_player, int unit_index, byte bit_index)` |  |
| `0x0044b0ec` | `llm_unit_status_bit_clear` | `void llm_unit_status_bit_clear(int unit_player, int unit_index, byte bit_index)` |  |
| `0x0044b141` | `llm_strat_unit_get_coords` | `void llm_strat_unit_get_coords(ushort player, int unit_index, fine_coord * out_x, fine_coord * out_y)` |  |
| `0x0044b35e` | `llm_ui_print_game_speed` | `void llm_ui_print_game_speed(void)` |  |
| `0x0044b3b3` | `llm_math_scale_pct` | `double llm_math_scale_pct(double value, int pct)` |  |
| `0x0044b3eb` | `llm_game_cheats_table_init` | `undefined llm_game_cheats_table_init(void)` |  |
| `0x0044b67f` | `llm_debug_console_dispatch` | `void llm_debug_console_dispatch(char * command_line)` |  |
| `0x0044c35a` | `llm_cfg_snapshot_project_inventions` | `undefined llm_cfg_snapshot_project_inventions(void)` |  |
| `0x0044c3a5` | `llm_cfg_verify_project_inventions_unchanged` | `void llm_cfg_verify_project_inventions_unchanged(undefined4 param_1)` |  |
| `0x0044c3fb` | `llm_strat_apply_area_damage` | `void llm_strat_apply_area_damage(int x, int y, int target_kind, double damage, int ring_count, uint owner_filter_zeroed, uint killer_info, int killer_unit_index)` |  |
| `0x0044c67f` | `llm_strat_bldg_kill_credit` | `void llm_strat_bldg_kill_credit(uint victim_player, int victim_building_index, double damage, uint killer_info, int killer_unit_index)` |  |
| `0x0044cac9` | `llm_strat_unit_kill_credit` | `void llm_strat_unit_kill_credit(uint victim_player, int victim_unit_index, double damage, uint killer_info, int killer_unit_index)` |  |
| `0x0044d0ed` | `llm_strat_advance_sim_clock` | `void llm_strat_advance_sim_clock(void)` |  |
| `0x0044d160` | `llm_strat_planet_transition_finalize` | `void llm_strat_planet_transition_finalize(void)` |  |
| `0x0044d238` | `llm_strat_sim_clock_advance` | `int llm_strat_sim_clock_advance(void)` | conf:high todo:globals |
| `0x0044d34f` | `llm_strat_try_enter_tactical_mission` | `void llm_strat_try_enter_tactical_mission(uint player, uint bldg_idx, uint param_3)` |  |
| `0x0044d3a8` | `llm_tact_mission_end_return_to_strategic` | `void llm_tact_mission_end_return_to_strategic(void)` | conf:high |
| `0x0044d468` | `llm_strat_bldg_gather_nearby_squad_status` | `int llm_strat_bldg_gather_nearby_squad_status(int scan_player, int bldg_owner, int bldg_idx)` | conf:med |
| `0x0044d81d` | `llm_strat_squad_assault_resolve` | `void llm_strat_squad_assault_resolve(void)` | conf:med |
| `0x0044da59` | `llm_tact_mission_end_finalize_stub` | `void llm_tact_mission_end_finalize_stub(void)` |  |
| `0x0044daa4` | `llm_snd_ambient_tick` | `void llm_snd_ambient_tick(void)` |  |
| `0x0044dbd2` | `llm_strat_spawn_debris_burst` | `void llm_strat_spawn_debris_burst(int intensity)` |  |
| `0x0044dd11` | `llm_cam_jump_queue_clear` | `void llm_cam_jump_queue_clear(void)` | conf:high |
| `0x0044dd43` | `llm_cam_jump_queue_pop` | `void llm_cam_jump_queue_pop(void)` |  |
| `0x0044dddc` | `llm_strat_offscreen_fx_scale` | `int llm_strat_offscreen_fx_scale(tile_coord tile_x, tile_coord tile_y)` |  |
| `0x0044deb3` | `llm_net_chat_input_process` | `void llm_net_chat_input_process(void)` | conf:high |
| `0x0044e046` | `llm_ui_floating_tip_tick` | `void llm_ui_floating_tip_tick(void)` | conf:med todo:globals |
| `0x0044e150` | `llm_gfx_view_metrics_init` | `void llm_gfx_view_metrics_init(void)` | todo:globals |
| `0x0044e28a` | `llm_tact_gfx_init_view_surfaces` | `void llm_tact_gfx_init_view_surfaces(void)` | conf:med todo:globals |
| `0x0044e421` | `llm_gfx_free_draw_buffers` | `void llm_gfx_free_draw_buffers(void)` | conf:med todo:globals |
| `0x0044e47d` | `llm_view_set_size_mode` | `int llm_view_set_size_mode(int size_mode)` |  |
| `0x0044e578` | `llm_strat_render_present` | `void llm_strat_render_present(void)` |  |
| `0x0044e5a4` | `llm_strat_render_view` | `void llm_strat_render_view(void)` |  |
| `0x0044eb60` | `llm_strat_draw_floating_messages` | `void llm_strat_draw_floating_messages(void)` |  |
| `0x0044ed14` | `llm_gfx_hittest_add_candidate` | `void llm_gfx_hittest_add_candidate(uint owner_and_kind, int roster_slot)` |  |
| `0x0044edc7` | `llm_strat_render_selection_markers` | `void llm_strat_render_selection_markers(int render_layer)` |  |
| `0x0044faae` | `llm_ui_map_drag_select_rect_draw` | `void llm_ui_map_drag_select_rect_draw(void)` | conf:high |
| `0x0044fcbe` | `llm_strat_render_unit_sprite` | `int llm_strat_render_unit_sprite(uint player, uint unit_idx, char alt_frame_flag)` |  |
| `0x0045046d` | `llm_strat_render_unit_shadow` | `void llm_strat_render_unit_shadow(uint player, int unit_idx)` |  |
| `0x00450a60` | `llm_strat_bldg_sprite_anchor_offset` | `void llm_strat_bldg_sprite_anchor_offset(ushort building_id, int * out_x, int * out_y)` |  |
| `0x00450b40` | `llm_gfx_bldg_frame_center_offset` | `void llm_gfx_bldg_frame_center_offset(ushort building_id, int * out_dx, int * out_dy)` | conf:med |
| `0x00450c12` | `llm_strat_bldg_footprint_visible` | `int llm_strat_bldg_footprint_visible(uint player, int building_index)` |  |
| `0x00450d1f` | `llm_strat_render_docked_unit_air` | `int llm_strat_render_docked_unit_air(uint player, uint unit_index, char flag)` |  |
| `0x00451251` | `llm_strat_render_docked_unit_ground` | `void llm_strat_render_docked_unit_ground(uint player, uint unit_index, char flag)` |  |
| `0x004515dd` | `llm_strat_render_bldg_docked_unit` | `void llm_strat_render_bldg_docked_unit(uint player, int bldg_idx)` |  |
| `0x00451d6a` | `llm_tact_view_draw_tile_border_edges` | `void llm_tact_view_draw_tile_border_edges(int cam_tile_col, int cam_tile_row)` | conf:high |
| `0x0045229a` | `llm_strat_render_tile_object` | `void llm_strat_render_tile_object(int tile_col_offset, int tile_row_offset)` |  |
| `0x00452edc` | `llm_strat_render_units_wide_pass` | `void llm_strat_render_units_wide_pass(int col, int row)` |  |
| `0x00452fbf` | `llm_strat_render_units_hittest_pass` | `void llm_strat_render_units_hittest_pass(int col, int row)` |  |
| `0x0045310f` | `llm_strat_draw_mine_resource_overlay` | `void llm_strat_draw_mine_resource_overlay(void)` |  |
| `0x00453482` | `llm_strat_tile_is_storage_door_exit` | `undefined4 llm_strat_tile_is_storage_door_exit(int x, int y)` |  |
| `0x004536e6` | `llm_strat_check_bldg_placement_valid` | `void llm_strat_check_bldg_placement_valid(int col, int row, int building_type)` |  |
| `0x004539cc` | `llm_fx_anim_seq_cancel` | `void llm_fx_anim_seq_cancel(int anim_seq_start_frame)` | conf:high |
| `0x00453a6d` | `llm_bldg_placement_check_and_preview` | `int llm_bldg_placement_check_and_preview(int origin_x, int origin_y, int building_index)` | conf:med |
| `0x00453c9f` | `llm_strat_tile_in_view_and_visible` | `int llm_strat_tile_in_view_and_visible(int tile_col, int tile_row)` |  |
| `0x00453e5e` | `llm_game_init_first_run_defaults` | `undefined llm_game_init_first_run_defaults(void)` | todo:globals |
| `0x00453ee4` | `llm_strat_session_state_reset` | `void llm_strat_session_state_reset(undefined4 param_1)` |  |
| `0x004541c3` | `llm_strat_planet_session_begin` | `void llm_strat_planet_session_begin(int race, int reset_flag)` |  |
| `0x0045435f` | `llm_strat_session_begin_multi` | `int llm_strat_session_begin_multi(void * cfg_blob)` |  |
| `0x00454695` | `llm_str_input_sanitize` | `void llm_str_input_sanitize(char * str)` | conf:med |
| `0x004546e8` | `llm_str_char_subst` | `void llm_str_char_subst(char * str, byte mode, char c1, char c2)` | conf:high |
| `0x004547f2` | `llm_net_mp_enter_gameplay_sync` | `void llm_net_mp_enter_gameplay_sync(void)` | conf:med |
| `0x00454846` | `llm_progress_recheck_planet_system_all_players` | `void llm_progress_recheck_planet_system_all_players(void)` |  |
| `0x00454966` | `llm_strat_session_begin_empty_stub` | `void llm_strat_session_begin_empty_stub(void)` | conf:low |
| `0x00454985` | `llm_strat_player_profile_init` | `void llm_strat_player_profile_init(game_t_Player player, uint controller_flags, uint race, double game_clock, uint color_index, char * name_str, int side_id)` |  |
| `0x00454c04` | `llm_strat_player_apply_all_colors` | `void llm_strat_player_apply_all_colors(void)` | conf:high |
| `0x00454c51` | `llm_strat_player_set_color` | `void llm_strat_player_set_color(int player_idx, uint color_index)` |  |
| `0x00454d8d` | `llm_strat_count_landing_spots` | `int llm_strat_count_landing_spots(void)` |  |
| `0x00454de5` | `llm_strat_landing_spots_reroll_out_of_bounds` | `void llm_strat_landing_spots_reroll_out_of_bounds(void)` | conf:high |
| `0x00454eb2` | `llm_strat_claim_landing_spot` | `int llm_strat_claim_landing_spot(game_t_Player player, game_t_PlanetIndex planet)` |  |
| `0x00454fe3` | `llm_strat_set_landing_site` | `undefined1 llm_strat_set_landing_site(game_t_Player player, game_t_PlanetIndex planet, undefined4 x, undefined4 param_4, undefined4 param_5)` |  |
| `0x00455055` | `llm_planet_tlo_load` | `int llm_planet_tlo_load(uint planet_index)` |  |
| `0x00455163` | `llm_planet_tlo_thumbnail_init` | `void llm_planet_tlo_thumbnail_init(void)` | conf:med todo:globals |
| `0x00455224` | `llm_planet_tlo_free` | `undefined llm_planet_tlo_free(void)` |  |
| `0x0045534e` | `llm_game_land_players_on_planet` | `void llm_game_land_players_on_planet(game_t_PlanetIndex planet_index)` | conf:high |
| `0x004557eb` | `llm_fx_anim_chain_walk_noop` | `void llm_fx_anim_chain_walk_noop(int anim_id)` | conf:med |
| `0x0045583c` | `llm_fx_anim_chain_find_tail` | `int llm_fx_anim_chain_find_tail(int start_frame)` |  |
| `0x004558a2` | `llm_gfx_cursor_anim_init` | `undefined llm_gfx_cursor_anim_init(void)` |  |
| `0x00455a84` | `llm_strat_player_param_defaults_init` | `void llm_strat_player_param_defaults_init(void)` |  |
| `0x00455b0a` | `llm_ui_message_queue_clear_all` | `void llm_ui_message_queue_clear_all(void)` | conf:high todo:globals |
| `0x00455b5a` | `llm_strat_tech_tables_reset` | `void llm_strat_tech_tables_reset(void)` |  |
| `0x00455df0` | `llm_strat_new_game_init` | `void llm_strat_new_game_init(void)` |  |
| `0x0045a017` | `llm_strat_bldg_init_all` | `void llm_strat_bldg_init_all(void)` |  |
| `0x0045a068` | `llm_strat_bldg_init_defaults` | `void llm_strat_bldg_init_defaults(uint building_id)` |  |
| `0x0045ba25` | `llm_strat_scenario_planet_clone` | `void llm_strat_scenario_planet_clone(void * cfg_blob)` | conf:med |
| `0x0045bb62` | `llm_snd_ambient_add` | `int llm_snd_ambient_add(uint index, undefined4 param_2, undefined4 param_3, double param_4, double param_5, undefined4 param_6)` |  |
| `0x0045bc70` | `llm_snd_ambient_planet_clone` | `void llm_snd_ambient_planet_clone(uint planet_id)` | conf:low todo:globals |
| `0x0045bcae` | `llm_snd_ambient_init_tables` | `undefined llm_snd_ambient_init_tables(void)` |  |
| `0x0045e277` | `llm_snd_ambient_reseed_planet_event_times` | `void llm_snd_ambient_reseed_planet_event_times(int planet_index, double current_time)` | conf:high |
| `0x0045e340` | `llm_strat_tech_count_unlocked_planets_in_system` | `int llm_strat_tech_count_unlocked_planets_in_system(ushort player_index, ushort system_index)` | conf:med |
| `0x0045e3cb` | `llm_system_get_available_planet` | `int llm_system_get_available_planet(int nth_available)` | conf:high |
| `0x0045ec1d` | `llm_map_resources_apply_value_radius` | `void llm_map_resources_apply_value_radius(int center_x, int center_y, int value_scale, short value_delta, uint radius)` | conf:med |
| `0x0045ed72` | `llm_strat_bldg_find_race_mothership_index` | `int llm_strat_bldg_find_race_mothership_index(game_e_race race)` | conf:high todo:enum |
| `0x0045ee68` | `llm_strat_unit_find_race_infantry_index` | `int llm_strat_unit_find_race_infantry_index(game_e_race race)` | conf:high todo:enum |
| `0x0045f027` | `llm_strat_mode_init` | `undefined llm_strat_mode_init(void)` |  |
| `0x0045f09b` | `llm_cfg_text_ptrs_reset` | `undefined llm_cfg_text_ptrs_reset(void)` |  |
| `0x0045f122` | `llm_strat_set_unit_state_handler` | `void llm_strat_set_unit_state_handler(llm_strat_unit_state id, void * handler)` |  |
| `0x0045f168` | `llm_strat_set_bldg_state_handler` | `void llm_strat_set_bldg_state_handler(llm_strat_bldg_state id, void * handler)` |  |
| `0x0045f1ae` | `llm_bldg_register_done_callback` | `void llm_bldg_register_done_callback(undefined4 param_1, voidCallback * f)` |  |
| `0x0045f20c` | `llm_bldg_register_tick2_callback` | `void llm_bldg_register_tick2_callback(undefined4 building_type, voidCallback * f)` |  |
| `0x0045f26a` | `llm_strat_register_state_handlers` | `void llm_strat_register_state_handlers(void)` |  |
| `0x0045f76a` | `llm_strat_register_bldg_type_callbacks` | `void llm_strat_register_bldg_type_callbacks(void)` |  |
| `0x0045fac0` | `llm_strat_unit_facing_offset_init` | `undefined llm_strat_unit_facing_offset_init(void)` | conf:high |
| `0x0045fefb` | `llm_strat_squad_placement_offset_table_init` | `undefined llm_strat_squad_placement_offset_table_init(void)` |  |
| `0x00460091` | `llm_strat_dir8_offsets_init` | `undefined llm_strat_dir8_offsets_init(void)` |  |
| `0x00460150` | `llm_strat_microstep_table_init` | `undefined llm_strat_microstep_table_init(void)` |  |
| `0x00460644` | `llm_strat_dir_neighbor_table_init` | `undefined llm_strat_dir_neighbor_table_init(void)` |  |
| `0x00460a23` | `llm_strat_walker_frame_blend_table_init` | `undefined llm_strat_walker_frame_blend_table_init(void)` |  |
| `0x00460a93` | `llm_strat_walker_frame_blend_index` | `undefined llm_strat_walker_frame_blend_index(void)` |  |
| `0x00460be3` | `llm_strat_group_step_heading_table_init` | `undefined llm_strat_group_step_heading_table_init(void)` |  |
| `0x0046122c` | `llm_strat_squad_placement_offset_set` | `void llm_strat_squad_placement_offset_set(int row, int col, uint row_class, uint col_class)` |  |
| `0x00461340` | `llm_strat_microstep_build_row` | `undefined llm_strat_microstep_build_row(int param_1, int param_2, int param_3, int param_4, int param_5, int param_6, int param_7)` |  |
| `0x004614f9` | `llm_strat_pathfinder_init` | `void llm_strat_pathfinder_init(void)` |  |
| `0x004615de` | `llm_strat_pathfinder_ctx_link` | `void llm_strat_pathfinder_ctx_link(void)` | conf:med |
| `0x00461634` | `llm_strat_pathfinder_shutdown` | `void llm_strat_pathfinder_shutdown(void)` | conf:high |
| `0x0046177b` | `llm_fx_anim_dir_frame_stride` | `int llm_fx_anim_dir_frame_stride(cfg_t_frame_index start_frame)` | conf:med |
| `0x004617dd` | `llm_fx_anim_chain_total_time` | `double llm_fx_anim_chain_total_time(int anim_id)` |  |
| `0x0046185b` | `llm_gfx_anim_get_frame_sprite` | `cfg_t_sprite_index llm_gfx_anim_get_frame_sprite(cfg_t_frame_index start_frame, double time_scale)` | conf:med |
| `0x0046190d` | `llm_cfg_anim_frame_at_progress` | `cfg_t_frame_index llm_cfg_anim_frame_at_progress(cfg_t_frame_index start_frame, double progress_fraction)` | conf:med |
| `0x004619b0` | `llm_strat_unit_init_record` | `void llm_strat_unit_init_record(int unit_idx, uint unit_proto_id, uint player)` |  |
| `0x00462284` | `llm_ui_cursor_probe_anim_frame_offsets` | `void llm_ui_cursor_probe_anim_frame_offsets(void)` | conf:low |
| `0x00462d89` | `llm_bldg_queue_construction` | `int llm_bldg_queue_construction(int player, int building_type, int x, int y)` |  |
| `0x00462e66` | `llm_bldg_construct_finalize` | `int llm_bldg_construct_finalize(undefined4 param_1, int y_b, ushort player, char param_4, undefined4 x_b, undefined4 building_id)` |  |
| `0x0046318d` | `llm_strat_prod_shuttle_slot_release` | `void llm_strat_prod_shuttle_slot_release(int player, int slot)` |  |
| `0x00463328` | `llm_strat_storage_find_home_for_unit` | `int llm_strat_storage_find_home_for_unit(uint player, uint unit_type, int probe_slot)` |  |
| `0x004636bc` | `llm_strat_storage_dock_unit_at_building` | `void llm_strat_storage_dock_unit_at_building(ushort player, int unit_idx, int storage_slot)` |  |
| `0x00463860` | `llm_strat_unit_create` | `map_t_unit_id llm_strat_unit_create(uint x, uint y, cfg_t_unit_index_s unit, game_t_Player_s player, bool is_ship)` |  |
| `0x00463ac6` | `llm_unit_create_soldier` | `map_t_unit_id llm_unit_create_soldier(uint param_1, uint param_2, ushort a2, ushort param_4, char param_5)` |  |
| `0x00463ec8` | `llm_unit_recruit` | `int llm_unit_recruit(uint player, uint unit_type_id)` |  |
| `0x00464119` | `llm_strat_unit_spawn_on_tile` | `int llm_strat_unit_spawn_on_tile(uint x, uint y, ushort unit_proto_id, ushort player)` |  |
| `0x0046434c` | `llm_strat_unit_spawn_docked` | `int llm_strat_unit_spawn_docked(ushort unit_proto_id, ushort player, undefined4 probe_slot)` |  |
| `0x0046443a` | `llm_strat_unit_add_docked` | `int llm_strat_unit_add_docked(uint unit_proto_id, ushort player, undefined4 probe_slot)` |  |
| `0x00464855` | `llm_strat_fx_anim_spawn` | `undefined4 llm_strat_fx_anim_spawn(undefined4 x, undefined4 y, undefined4 param_3, double param_4, undefined4 param_5)` |  |
| `0x00464932` | `llm_strat_projectile_spawn` | `int llm_strat_projectile_spawn(int src_x, int src_y_ground, int src_y_vis_offset, uint dst_x, uint dst_y_ground, int dst_y_vis_offset, int weapon_id, byte owner_player, ushort homing_player_and_flags, int homing_target_unit, uint shooter_ref, int shooter_unit_index)` | conf:med |
| `0x00465093` | `llm_gfx_load_all_sprite_banks` | `void llm_gfx_load_all_sprite_banks(void)` | conf:high |
| `0x004650bc` | `llm_gfx_load_banks` | `undefined llm_gfx_load_banks(void)` |  |
| `0x00465502` | `llm_gfx_load_sprite_bank_files` | `void llm_gfx_load_sprite_bank_files(void)` | conf:high todo:globals todo:struct |
| `0x004657ac` | `llm_gfx_load_planet_extra_sprite_banks` | `void llm_gfx_load_planet_extra_sprite_banks(void)` | conf:high todo:globals todo:struct |
| `0x00465a83` | `llm_cfg_cache_placement_anims` | `undefined llm_cfg_cache_placement_anims(void)` |  |
| `0x00465ac0` | `llm_diplomacy_init_multiplayer` | `void llm_diplomacy_init_multiplayer(void)` | conf:med todo:enum |
| `0x00465baf` | `llm_diplomacy_init_skirmish` | `void llm_diplomacy_init_skirmish(void)` | conf:med todo:enum |
| `0x00465c6e` | `llm_diplomacy_restore_relations` | `void llm_diplomacy_restore_relations(void)` | conf:med todo:enum |
| `0x00465d16` | `llm_strat_deploy_starting_squad` | `int llm_strat_deploy_starting_squad(void)` |  |
| `0x00465fdf` | `llm_strat_order_dispatch` | `int llm_strat_order_dispatch(ushort unit_id, uint player, ushort op_code, ushort arg)` |  |
| `0x00466094` | `llm_strat_order_enqueue` | `int llm_strat_order_enqueue(ushort unit_index, ushort owner_and_kind, short param0, ushort order_code)` |  |
| `0x0046616e` | `llm_strat_order_integrity_check` | `void llm_strat_order_integrity_check(llm_strat_order order, char * tag)` | conf:high |
| `0x00466211` | `llm_strat_order_stage_scheduled` | `int llm_strat_order_stage_scheduled(uint unit_index, uint owner_and_kind, int param0, uint order_code, double exec_time)` | conf:med |
| `0x00466348` | `llm_strat_order_schedule` | `int llm_strat_order_schedule(void)` |  |
| `0x0046652e` | `llm_strat_order_release_due` | `int llm_strat_order_release_due(double now)` |  |
| `0x00466790` | `llm_strat_order_pending_enqueue` | `int llm_strat_order_pending_enqueue(llm_strat_order order)` | conf:med todo:proto |
| `0x00466812` | `llm_strat_order_scratch_reset` | `void llm_strat_order_scratch_reset(void)` |  |
| `0x0046685d` | `llm_strat_order_scratch_set_field` | `void llm_strat_order_scratch_set_field(int index, int value)` |  |
| `0x00466892` | `llm_strat_order_queue_dispatch` | `void llm_strat_order_queue_dispatch(void)` |  |
| `0x00469996` | `llm_strat_order_queue_find_index` | `int llm_strat_order_queue_find_index(int player, int unit_idx, int kind_tag)` | conf:high todo:proto |
| `0x00469a37` | `llm_strat_order_queue_apply_and_dequeue` | `void llm_strat_order_queue_apply_and_dequeue(uint player, int unit_idx, int queue_idx)` |  |
| `0x00469de6` | `llm_strat_unit_order_dispatch_default` | `void llm_strat_unit_order_dispatch_default(uint player, int unit_index)` | conf:med todo:dead |
| `0x00469e72` | `llm_strat_unit_issue_default_order` | `void llm_strat_unit_issue_default_order(uint player, int unit_index)` | conf:med |
| `0x00469efe` | `llm_strat_unit_reset_order_and_target` | `void llm_strat_unit_reset_order_and_target(uint player, int unit_idx)` | todo:dead |
| `0x00469fe6` | `llm_strat_unit_order_move` | `void llm_strat_unit_order_move(uint param_1, int param_2, undefined4 a2, undefined4 param_4, undefined4 param_5)` |  |
| `0x0046a0d5` | `llm_strat_unit_order_move_enqueue` | `void llm_strat_unit_order_move_enqueue(ushort player, int unit_idx, undefined4 x, undefined4 y, undefined4 move_flag)` |  |
| `0x0046a19d` | `llm_strat_order_issue_0xf_adjacent_by_offset` | `void llm_strat_order_issue_0xf_adjacent_by_offset(uint param_1, int param_2, int a2, int param_4)` |  |
| `0x0046a221` | `llm_strat_order_issue_0xf_adjacent` | `void llm_strat_order_issue_0xf_adjacent(uint param_1, int param_2, int a2, int param_4)` | todo:dead |
| `0x0046a325` | `llm_strat_order_issue_0xf_adjacent_enqueue` | `void llm_strat_order_issue_0xf_adjacent_enqueue(ushort player, int unit_idx, int target_x, int target_y)` |  |
| `0x0046a402` | `llm_strat_unit_order_move_default` | `void llm_strat_unit_order_move_default(uint param_1, int param_2, undefined4 a2, undefined4 param_4, undefined4 param_5)` |  |
| `0x0046a4cb` | `llm_strat_unit_order_move_default_enqueue` | `void llm_strat_unit_order_move_default_enqueue(ushort param_1, int param_2, undefined4 a2, undefined4 param_4, undefined4 param_5)` | todo:dead |
| `0x0046a56d` | `llm_strat_unit_order_move_auto` | `void llm_strat_unit_order_move_auto(ushort player, int unit_idx, undefined4 x, undefined4 y)` |  |
| `0x0046a626` | `llm_strat_unit_order_scatter_from_spawn` | `void llm_strat_unit_order_scatter_from_spawn(ushort param_1, int param_2, undefined4 a2, undefined4 param_4)` |  |
| `0x0046a6b9` | `llm_strat_unit_order_exit_storage` | `void llm_strat_unit_order_exit_storage(uint param_1, uint param_2, int a2, undefined4 param_4, undefined4 param_5)` |  |
| `0x0046a918` | `llm_strat_unit_order_exit_storage_enqueue` | `void llm_strat_unit_order_exit_storage_enqueue(uint player, uint unit_idx, int storage_idx, undefined4 param_4, undefined4 param_5)` |  |
| `0x0046ab08` | `llm_strat_unit_order_exit_storage_auto` | `void llm_strat_unit_order_exit_storage_auto(uint param_1, uint param_2, int a2, undefined4 param_4, undefined4 param_5)` |  |
| `0x0046accf` | `llm_strat_unit_order_move_confirmed_with_bump` | `void llm_strat_unit_order_move_confirmed_with_bump(uint param_1, int param_2, undefined4 a2, undefined4 param_4)` |  |
| `0x0046ae0a` | `llm_strat_ai_unit_order_move_with_bump` | `void llm_strat_ai_unit_order_move_with_bump(uint player, int unit_idx, undefined4 order_arg0, undefined4 order_arg1)` |  |
| `0x0046af1e` | `llm_strat_unit_order_attack_target` | `void llm_strat_unit_order_attack_target(uint param_1, int param_2, uint a2, int param_4, uint param_5)` |  |
| `0x0046b271` | `llm_strat_unit_order_attack_target_enqueue` | `void llm_strat_unit_order_attack_target_enqueue(uint param_1, int param_2, uint a2, int param_4, uint param_5)` |  |
| `0x0046b599` | `llm_strat_unit_order_attack_target_alt` | `void llm_strat_unit_order_attack_target_alt(uint player, int unit_idx, uint target_side, int target_unit_idx, uint weapon_idx)` |  |
| `0x0046b8ec` | `llm_strat_unit_order_attack_target_alt_enqueue` | `void llm_strat_unit_order_attack_target_alt_enqueue(uint param_1, int param_2, uint a2, int param_4, uint param_5)` |  |
| `0x0046bc14` | `llm_strat_unit_order_attack_unit` | `void llm_strat_unit_order_attack_unit(uint param_1, int param_2, uint a2, int param_4, uint param_5)` |  |
| `0x0046bcfd` | `llm_strat_unit_order_attack_unit_enqueue` | `void llm_strat_unit_order_attack_unit_enqueue(uint player, int unit_idx, uint target_player, int target_unit_idx, uint weapon_id)` |  |
| `0x0046bde6` | `llm_strat_unit_order_attack_target_preset_weapon_enqueue` | `void llm_strat_unit_order_attack_target_preset_weapon_enqueue(uint param_1, int param_2, uint a2, int param_4)` | todo:dead |
| `0x0046bf9c` | `llm_strat_unit_order_attack_building_reposition` | `void llm_strat_unit_order_attack_building_reposition(uint param_1, int param_2, uint a2, int param_4, uint param_5)` |  |
| `0x0046c21c` | `llm_strat_unit_order_attack_building_reposition_alt_enqueue` | `void llm_strat_unit_order_attack_building_reposition_alt_enqueue(uint player, int unit_idx, uint target_player, int target_bldg_idx, uint weapon_idx)` |  |
| `0x0046c471` | `llm_strat_unit_order_attack_building_reposition_enqueue` | `void llm_strat_unit_order_attack_building_reposition_enqueue(uint param_1, int param_2, uint a2, int param_4, uint param_5)` | todo:dead |
| `0x0046c6c6` | `llm_strat_unit_order_attack_building` | `void llm_strat_unit_order_attack_building(uint param_1, int param_2, uint a2, int param_4, uint param_5)` | todo:dead |
| `0x0046c7a0` | `llm_strat_unit_order_attack_building_enqueue` | `void llm_strat_unit_order_attack_building_enqueue(uint param_1, int param_2, uint a2, int param_4, uint param_5)` |  |
| `0x0046c87a` | `llm_strat_unit_order_move_relative` | `void llm_strat_unit_order_move_relative(uint param_1, int param_2, int a2, int param_4)` |  |
| `0x0046c99d` | `llm_strat_unit_order_move_relative_enqueue` | `void llm_strat_unit_order_move_relative_enqueue(uint param_1, int param_2, int a2, int param_4)` | todo:dead |
| `0x0046ca99` | `llm_storage_cancel_pending_docked` | `void llm_storage_cancel_pending_docked(uint player, int building_index)` |  |
| `0x0046cb5a` | `llm_strat_storage_scrap_docked_units_of_type` | `void llm_strat_storage_scrap_docked_units_of_type(uint player, int building_index)` |  |
| `0x0046cd8b` | `llm_strat_unit_order_auto_launch_from_storage` | `void llm_strat_unit_order_auto_launch_from_storage(uint player, int unit_idx, uint x, uint y)` |  |
| `0x0046cf2c` | `llm_strat_unit_order_auto_launch_from_storage_enqueue` | `void llm_strat_unit_order_auto_launch_from_storage_enqueue(uint player, int unit_idx, uint x, uint y)` |  |
| `0x0046d0cd` | `llm_unit_force_disembark` | `void llm_unit_force_disembark(uint unit_player, int unit_index)` |  |
| `0x0046d229` | `llm_strat_bldg_instant_construct_find_slot` | `int llm_strat_bldg_instant_construct_find_slot(tile_coord tile_col, tile_coord tile_row, cfg_t_building_index building_type_id, int initial_workers, game_t_Player player)` | conf:high |
| `0x0046d514` | `llm_strat_bldg_instant_construct_find_slot_enqueue` | `int llm_strat_bldg_instant_construct_find_slot_enqueue(undefined4 param_1, undefined4 param_2, int building_type_id, undefined4 param_4, ushort player)` |  |
| `0x0046d800` | `llm_strat_order_create_unit_debug` | `undefined4 llm_strat_order_create_unit_debug(undefined4 param_1, undefined4 param_2, undefined4 a2, uint param_4)` |  |
| `0x0046d873` | `llm_strat_dmp_enqueue_scripted_order` | `uint llm_strat_dmp_enqueue_scripted_order(uint param_1, uint param_2, uint param_3, ushort param_4)` |  |
| `0x0046d8e6` | `llm_strat_ai_create_reinforcement_unit` | `void llm_strat_ai_create_reinforcement_unit(uint x, uint y, uint unit_proto_id, game_t_Player_s param_4)` |  |
| `0x0046d952` | `llm_strat_order_recruit_unit` | `int llm_strat_order_recruit_unit(int unit_type, uint player)` | todo:dead |
| `0x0046d9a9` | `llm_strat_order_recruit_unit_enqueue` | `int llm_strat_order_recruit_unit_enqueue(uint unit_id, uint player_id)` |  |
| `0x0046da00` | `llm_strat_order_queue_construction_debug` | `undefined4 llm_strat_order_queue_construction_debug(undefined4 param_1, undefined4 param_2, undefined4 a2, uint param_4)` |  |
| `0x0046da70` | `llm_strat_order_queue_construction_enqueue` | `undefined4 llm_strat_order_queue_construction_enqueue(undefined4 param_1, undefined4 param_2, undefined4 a2, ushort param_4)` |  |
| `0x0046dae0` | `llm_strat_bldg_order_production_add` | `void llm_strat_bldg_order_production_add(uint player, int bldg_idx, int unit_type)` |  |
| `0x0046db58` | `llm_strat_bldg_order_production_add_enqueue` | `void llm_strat_bldg_order_production_add_enqueue(ushort player, int bldg_idx, int unit_type)` |  |
| `0x0046dbd0` | `llm_strat_bldg_order_production_add_count` | `void llm_strat_bldg_order_production_add_count(uint player, int bldg_idx, int unit_type, int count)` | todo:dead |
| `0x0046dc47` | `llm_strat_bldg_order_production_add_count_enqueue` | `void llm_strat_bldg_order_production_add_count_enqueue(ushort player, int bldg_idx, int unit_type, int count)` | todo:dead |
| `0x0046dcbe` | `llm_strat_bldg_order_production_remove` | `void llm_strat_bldg_order_production_remove(uint player, int bldg_idx, int unit_type)` |  |
| `0x0046dd27` | `llm_strat_bldg_order_production_remove_enqueue` | `void llm_strat_bldg_order_production_remove_enqueue(ushort player, int bldg_idx, int unit_type)` | todo:dead |
| `0x0046dd90` | `llm_strat_turret_order_target_unit` | `void llm_strat_turret_order_target_unit(ushort player, ushort building_or_id, uint target_or_side, uint target_unit_idx)` |  |
| `0x0046ddf0` | `llm_strat_turret_order_target_unit_enqueue` | `void llm_strat_turret_order_target_unit_enqueue(ushort param_1, ushort param_2, uint a2, undefined4 param_4)` | todo:dead |
| `0x0046de50` | `llm_strat_turret_order_target_building` | `void llm_strat_turret_order_target_building(ushort param_1, ushort param_2, uint a2, undefined4 param_4)` |  |
| `0x0046deb0` | `llm_strat_turret_order_target_building_enqueue` | `void llm_strat_turret_order_target_building_enqueue(ushort player, ushort turret_idx, uint target_bldg_idx, undefined4 order_arg1)` | todo:dead |
| `0x0046df10` | `llm_strat_bldg_order_set_worker_count` | `void llm_strat_bldg_order_set_worker_count(ushort param_1, ushort param_2, uint a2)` | todo:dead |
| `0x0046df63` | `llm_strat_bldg_order_set_worker_count_enqueue` | `void llm_strat_bldg_order_set_worker_count_enqueue(ushort param_1, ushort param_2, int a2)` | todo:dead |
| `0x0046dfb6` | `llm_strat_bldg_order_assign_workers` | `void llm_strat_bldg_order_assign_workers(ushort player, ushort building_index, uint count)` |  |
| `0x0046e005` | `llm_strat_bldg_order_assign_workers_enqueue` | `void llm_strat_bldg_order_assign_workers_enqueue(ushort player, ushort bldg_idx, undefined4 worker_count)` |  |
| `0x0046e054` | `llm_strat_bldg_order_unassign_workers` | `void llm_strat_bldg_order_unassign_workers(ushort player, ushort building_index, uint count)` |  |
| `0x0046e0a3` | `llm_strat_bldg_order_unassign_workers_enqueue` | `void llm_strat_bldg_order_unassign_workers_enqueue(ushort param_1, ushort param_2, undefined4 a2)` |  |
| `0x0046e0f2` | `llm_strat_bldg_order_activate` | `void llm_strat_bldg_order_activate(uint player, ushort bldg_idx)` |  |
| `0x0046e133` | `llm_strat_bldg_order_activate_enqueue` | `void llm_strat_bldg_order_activate_enqueue(uint player_id, int building_index)` |  |
| `0x0046e174` | `llm_strat_bldg_order_deactivate` | `void llm_strat_bldg_order_deactivate(uint player, ushort building_idx)` |  |
| `0x0046e1b5` | `llm_strat_bldg_order_deactivate_enqueue` | `void llm_strat_bldg_order_deactivate_enqueue(ushort owner_kind, ushort bldg_index)` |  |
| `0x0046e1f6` | `llm_strat_bldg_order_toggle_active` | `void llm_strat_bldg_order_toggle_active(ushort player, int building_index)` |  |
| `0x0046e252` | `llm_strat_bldg_order_toggle_active_enqueue` | `void llm_strat_bldg_order_toggle_active_enqueue(uint player, int building_index)` | todo:dead |
| `0x0046e2ae` | `llm_strat_bldg_order_upgrade` | `void llm_strat_bldg_order_upgrade(uint player, uint bldg_unit_id)` |  |
| `0x0046e2ef` | `llm_strat_bldg_order_upgrade_enqueue` | `void llm_strat_bldg_order_upgrade_enqueue(uint player_id, int building_index)` |  |
| `0x0046e330` | `llm_strat_bldg_order_repair_cycle_start` | `void llm_strat_bldg_order_repair_cycle_start(uint player, int bldg_idx)` |  |
| `0x0046e409` | `llm_strat_bldg_order_repair_cycle_start_enqueue` | `void llm_strat_bldg_order_repair_cycle_start_enqueue(uint player, int bldg_idx)` |  |
| `0x0046e4e2` | `llm_strat_bldg_order_hangar_recharge` | `void llm_strat_bldg_order_hangar_recharge(uint player, int bldg_idx)` |  |
| `0x0046e523` | `llm_strat_bldg_order_hangar_recharge_enqueue` | `void llm_strat_bldg_order_hangar_recharge_enqueue(ushort target_id, ushort player)` | todo:dead |
| `0x0046e564` | `llm_strat_bldg_order_cancel_reset` | `void llm_strat_bldg_order_cancel_reset(uint target_id, ushort player)` | todo:dead |
| `0x0046e5a5` | `llm_strat_bldg_order_cancel_reset_enqueue` | `void llm_strat_bldg_order_cancel_reset_enqueue(ushort target_id, ushort player)` | todo:dead |
| `0x0046e5e6` | `llm_strat_bldg_order_restart_construction` | `void llm_strat_bldg_order_restart_construction(uint target_id, ushort player)` |  |
| `0x0046e627` | `llm_strat_bldg_order_restart_construction_enqueue` | `void llm_strat_bldg_order_restart_construction_enqueue(uint player_id, int building_index)` |  |
| `0x0046e668` | `llm_strat_bldg_order_mine_rescan` | `void llm_strat_bldg_order_mine_rescan(uint target_id, ushort player)` | todo:dead |
| `0x0046e6a9` | `llm_strat_bldg_order_mine_rescan_enqueue` | `void llm_strat_bldg_order_mine_rescan_enqueue(ushort target_id, ushort player)` | todo:dead |
| `0x0046e6ea` | `llm_strat_bldg_order_purge_dead_docked` | `void llm_strat_bldg_order_purge_dead_docked(uint player, ushort bldg_idx)` |  |
| `0x0046e72b` | `llm_strat_bldg_order_purge_dead_docked_enqueue` | `void llm_strat_bldg_order_purge_dead_docked_enqueue(ushort target_id, ushort player)` | todo:dead |
| `0x0046e76c` | `llm_unit_order_disembark_soldiers` | `void llm_unit_order_disembark_soldiers(uint player, int unit_idx)` |  |
| `0x0046e7d6` | `llm_unit_order_disembark_soldiers_enqueue` | `void llm_unit_order_disembark_soldiers_enqueue(uint player_idx, int unit_idx)` |  |
| `0x0046e840` | `llm_strat_bldg_order_shuttle_depart` | `void llm_strat_bldg_order_shuttle_depart(uint param_1, ushort param_2, undefined4 a2)` |  |
| `0x0046e88f` | `llm_strat_bldg_order_shuttle_depart_enqueue` | `void llm_strat_bldg_order_shuttle_depart_enqueue(ushort player, ushort bldg_idx, undefined4 planet_idx)` |  |
| `0x0046e8de` | `llm_strat_order_issue_0xd5` | `void llm_strat_order_issue_0xd5(uint param_1, ushort param_2, undefined4 a2, undefined4 param_4)` | todo:dead |
| `0x0046e93b` | `llm_strat_order_issue_0xd5_enqueue` | `void llm_strat_order_issue_0xd5_enqueue(ushort param_1, ushort param_2, undefined4 a2, undefined4 param_4)` | todo:dead |
| `0x0046e998` | `llm_strat_bldg_order_cargo_bind_slot` | `void llm_strat_bldg_order_cargo_bind_slot(uint target_id, ushort player)` | todo:dead |
| `0x0046e9d9` | `llm_strat_bldg_order_cargo_bind_slot_enqueue` | `void llm_strat_bldg_order_cargo_bind_slot_enqueue(ushort target_id, ushort player)` | todo:dead |
| `0x0046ea1a` | `llm_strat_bldg_order_flush_cargo_hold` | `void llm_strat_bldg_order_flush_cargo_hold(uint player, uint bldg_unit_id)` |  |
| `0x0046ea5b` | `llm_strat_bldg_order_flush_cargo_hold_enqueue` | `void llm_strat_bldg_order_flush_cargo_hold_enqueue(ushort target_id, ushort player)` | todo:dead |
| `0x0046ea9c` | `llm_strat_bldg_order_load_passengers` | `void llm_strat_bldg_order_load_passengers(ushort player, ushort bldg_idx, undefined4 planet_idx)` |  |
| `0x0046eaeb` | `llm_strat_bldg_order_load_passengers_enqueue` | `void llm_strat_bldg_order_load_passengers_enqueue(ushort param_1, ushort param_2, undefined4 a2)` | todo:dead |
| `0x0046eb3a` | `llm_strat_bldg_order_unload_passengers` | `void llm_strat_bldg_order_unload_passengers(uint player, ushort bldg_idx, undefined4 passenger_count)` |  |
| `0x0046eb89` | `llm_strat_bldg_order_unload_passengers_enqueue` | `void llm_strat_bldg_order_unload_passengers_enqueue(ushort player, ushort bldg_idx, undefined4 unload_count)` | todo:dead |
| `0x0046ebd8` | `llm_strat_bldg_order_load_resource` | `void llm_strat_bldg_order_load_resource(uint param_1, ushort param_2, undefined4 a2, undefined4 param_4)` |  |
| `0x0046ec35` | `llm_strat_bldg_order_load_resource_enqueue` | `void llm_strat_bldg_order_load_resource_enqueue(ushort param_1, ushort param_2, undefined4 a2, undefined4 param_4)` | todo:dead |
| `0x0046ec92` | `llm_strat_bldg_order_unload_resource` | `void llm_strat_bldg_order_unload_resource(uint player, ushort bldg_idx, undefined4 resource_slot, undefined4 count)` |  |
| `0x0046ecef` | `llm_strat_bldg_order_unload_resource_enqueue` | `void llm_strat_bldg_order_unload_resource_enqueue(ushort param_1, ushort param_2, undefined4 a2, undefined4 param_4)` | todo:dead |
| `0x0046ed4c` | `llm_strat_bldg_order_port_depart` | `void llm_strat_bldg_order_port_depart(ushort param_1, ushort param_2, undefined4 a2)` |  |
| `0x0046ed9b` | `llm_strat_bldg_order_port_depart_enqueue` | `void llm_strat_bldg_order_port_depart_enqueue(ushort player, ushort bldg_idx, undefined4 planet_idx)` |  |
| `0x0046edea` | `llm_strat_bldg_order_mother_depart` | `void llm_strat_bldg_order_mother_depart(uint param_1, ushort param_2, undefined4 a2)` |  |
| `0x0046ee39` | `llm_strat_bldg_order_mother_depart_enqueue` | `void llm_strat_bldg_order_mother_depart_enqueue(ushort param_1, ushort param_2, undefined4 a2)` |  |
| `0x0046ee88` | `llm_strat_bldg_order_depart_dispatch_by_type` | `void llm_strat_bldg_order_depart_dispatch_by_type(uint player, int bldg_idx, game_t_PlanetIndex dest_planet_idx)` |  |
| `0x0046efe4` | `llm_strat_bldg_order_depart_dispatch_by_type_enqueue` | `void llm_strat_bldg_order_depart_dispatch_by_type_enqueue(ushort param_1, int param_2, game_t_PlanetIndex a2)` | todo:dead |
| `0x0046f140` | `llm_strat_bldg_order_start_research_project` | `void llm_strat_bldg_order_start_research_project(uint param_1, ushort param_2, undefined4 a2)` |  |
| `0x0046f18f` | `llm_strat_bldg_order_start_research_project_enqueue` | `void llm_strat_bldg_order_start_research_project_enqueue(ushort player, ushort bldg_idx, undefined4 project_id)` | todo:dead |
| `0x0046f1de` | `llm_strat_order_ctrlgrp_select_member` | `void llm_strat_order_ctrlgrp_select_member(uint side, ushort unit_id, int group_index)` |  |
| `0x0046f22b` | `llm_strat_order_ctrlgrp_select_member_raw` | `void llm_strat_order_ctrlgrp_select_member_raw(ushort side, ushort unit_id, int group_index)` | todo:dead |
| `0x0046f278` | `llm_strat_order_ctrlgrp_flash_member` | `void llm_strat_order_ctrlgrp_flash_member(ushort side, ushort unit_id, int group_index)` |  |
| `0x0046f2c5` | `llm_strat_order_ctrlgrp_flash_member_raw` | `void llm_strat_order_ctrlgrp_flash_member_raw(ushort side, ushort unit_id, int group_index)` | todo:dead |
| `0x0046f312` | `llm_strat_order_debug_roll_random` | `void llm_strat_order_debug_roll_random(void)` | todo:dead |
| `0x0046f34c` | `llm_strat_order_debug_roll_random_enqueue` | `void llm_strat_order_debug_roll_random_enqueue(void)` | todo:dead |
| `0x0046f381` | `llm_strat_order_game_speed_increase` | `void llm_strat_order_game_speed_increase(uint arg)` | todo:dead |
| `0x0046f3bb` | `llm_strat_order_game_speed_increase_enqueue` | `void llm_strat_order_game_speed_increase_enqueue(ushort arg)` | todo:dead |
| `0x0046f3f5` | `llm_strat_order_game_speed_decrease` | `void llm_strat_order_game_speed_decrease(uint arg)` | todo:dead |
| `0x0046f42f` | `llm_strat_order_game_speed_decrease_enqueue` | `void llm_strat_order_game_speed_decrease_enqueue(ushort arg)` | todo:dead |
| `0x0046f469` | `llm_strat_dispatch_matching_table_entry` | `undefined llm_strat_dispatch_matching_table_entry(void)` |  |
| `0x0046f4d0` | `llm_strat_dispatch_matching_table_entry_dup` | `undefined llm_strat_dispatch_matching_table_entry_dup(void)` |  |
| `0x0046f537` | `llm_strat_order_grant_resource` | `void llm_strat_order_grant_resource(uint param_1, undefined4 param_2, undefined4 a2)` |  |
| `0x0046f592` | `llm_strat_order_grant_resource_raw` | `void llm_strat_order_grant_resource_raw(ushort player, uint res_type, uint amount)` |  |
| `0x0046f5ed` | `llm_strat_order_population_delta_debug` | `void llm_strat_order_population_delta_debug(uint player_id, int population_delta)` |  |
| `0x0046f63a` | `llm_strat_order_population_delta_enqueue` | `void llm_strat_order_population_delta_enqueue(uint player_id, int population_delta)` |  |
| `0x0046f687` | `llm_strat_order_collect_available_projects` | `void llm_strat_order_collect_available_projects(uint side)` |  |
| `0x0046f6c1` | `llm_strat_order_collect_available_projects_enqueue` | `void llm_strat_order_collect_available_projects_enqueue(ushort player)` |  |
| `0x0046f6fb` | `llm_strat_order_recheck_projects` | `void llm_strat_order_recheck_projects(uint side)` |  |
| `0x0046f735` | `llm_strat_order_recheck_projects_enqueue` | `void llm_strat_order_recheck_projects_enqueue(ushort player)` | todo:dead |
| `0x0046f76f` | `llm_strat_order_recheck_buildings` | `void llm_strat_order_recheck_buildings(uint side)` |  |
| `0x0046f7a9` | `llm_strat_order_recheck_buildings_enqueue` | `void llm_strat_order_recheck_buildings_enqueue(ushort player)` | todo:dead |
| `0x0046f7e3` | `llm_strat_order_recheck_planet_system_all_players` | `void llm_strat_order_recheck_planet_system_all_players(void)` |  |
| `0x0046f81d` | `llm_strat_order_recheck_planet_system_all_players_enqueue` | `void llm_strat_order_recheck_planet_system_all_players_enqueue(void)` | todo:dead |
| `0x0046f852` | `llm_strat_order_credit_conquest_kills` | `void llm_strat_order_credit_conquest_kills(uint side)` |  |
| `0x0046f88c` | `llm_strat_order_credit_conquest_kills_enqueue` | `void llm_strat_order_credit_conquest_kills_enqueue(ushort player)` | todo:dead |
| `0x0046f8c6` | `llm_strat_order_set_player_relation` | `void llm_strat_order_set_player_relation(uint player, undefined4 opponent_idx, undefined1 relation_value)` |  |
| `0x0046f922` | `llm_strat_order_set_player_relation_raw` | `void llm_strat_order_set_player_relation_raw(ushort param_1, undefined4 param_2, undefined1 a2)` | todo:dead |
| `0x0046f97e` | `llm_strat_order_set_player_control_mode` | `void llm_strat_order_set_player_control_mode(uint param_1, undefined4 param_2, char a2)` |  |
| `0x0046fa3e` | `llm_strat_order_set_player_control_mode_enqueue` | `void llm_strat_order_set_player_control_mode_enqueue(ushort player, undefined4 param_2, undefined1 a2)` | todo:dead |
| `0x0046fa9a` | `llm_strat_order_debug_energy_refill_full` | `void llm_strat_order_debug_energy_refill_full(uint player_id, int target_id)` |  |
| `0x0046fafb` | `llm_strat_order_debug_energy_refill_full_enqueue` | `void llm_strat_order_debug_energy_refill_full_enqueue(int arg0, int arg1)` | todo:dead |
| `0x0046fb5c` | `llm_strat_order_debug_damage_scaled` | `void llm_strat_order_debug_damage_scaled(uint player_id, int damage_amount)` |  |
| `0x0046fbbd` | `llm_strat_order_debug_charge_add_scaled_enqueue` | `void llm_strat_order_debug_charge_add_scaled_enqueue(int arg0, int arg1)` | todo:dead |
| `0x0046fc1e` | `llm_strat_order_debug_kill_group` | `void llm_strat_order_debug_kill_group(uint player_id, int damage_amount)` |  |
| `0x0046fc7f` | `llm_strat_order_debug_charge_add_full_group_enqueue` | `void llm_strat_order_debug_charge_add_full_group_enqueue(int arg0, int arg1)` | todo:dead |
| `0x0046fce0` | `llm_strat_order_fow_reveal_full` | `void llm_strat_order_fow_reveal_full(uint player_id)` |  |
| `0x0046fd33` | `llm_strat_order_fow_reveal_full_enqueue` | `void llm_strat_order_fow_reveal_full_enqueue(int player)` | todo:dead |
| `0x0046fd86` | `llm_strat_building_tick` | `void llm_strat_building_tick(void)` | conf:med |
| `0x0046ff2c` | `llm_strat_done_default` | `void llm_strat_done_default(void)` |  |
| `0x0046ff4e` | `llm_strat_done_mother` | `void llm_strat_done_mother(void)` |  |
| `0x0046ff7f` | `llm_strat_done_plant` | `void llm_strat_done_plant(void)` |  |
| `0x0046ffa6` | `llm_strat_done_colony` | `void llm_strat_done_colony(void)` |  |
| `0x0046ffd2` | `llm_strat_done_quarters_vehicles` | `void llm_strat_done_quarters_vehicles(void)` |  |
| `0x0046fffe` | `llm_strat_done_quarters_soldiers` | `void llm_strat_done_quarters_soldiers(void)` |  |
| `0x0047002a` | `llm_strat_done_airfield` | `void llm_strat_done_airfield(void)` |  |
| `0x00470056` | `llm_strat_done_helipad` | `void llm_strat_done_helipad(void)` |  |
| `0x00470082` | `llm_strat_done_mine` | `void llm_strat_done_mine(void)` |  |
| `0x004700ae` | `llm_strat_done_production` | `void llm_strat_done_production(void)` |  |
| `0x004700d5` | `llm_strat_done_lab` | `void llm_strat_done_lab(void)` |  |
| `0x004700fc` | `llm_strat_done_turret` | `void llm_strat_done_turret(void)` |  |
| `0x00470123` | `llm_strat_done_silos` | `void llm_strat_done_silos(void)` |  |
| `0x0047014f` | `llm_strat_done_shuttle` | `void llm_strat_done_shuttle(void)` |  |
| `0x00470171` | `llm_strat_done_relay` | `void llm_strat_done_relay(void)` |  |
| `0x00470198` | `llm_strat_done_port` | `void llm_strat_done_port(void)` |  |
| `0x004701bf` | `llm_strat_add_storage_capacity` | `void llm_strat_add_storage_capacity(void)` |  |
| `0x004702af` | `llm_strat_power_generate` | `void llm_strat_power_generate(void)` |  |
| `0x00470323` | `llm_strat_add_population_housing` | `void llm_strat_add_population_housing(void)` |  |
| `0x00470446` | `llm_strat_power_consume` | `void llm_strat_power_consume(void)` |  |
| `0x0047048e` | `llm_strat_add_unit_capacity_vehicles` | `void llm_strat_add_unit_capacity_vehicles(void)` |  |
| `0x004704e2` | `llm_strat_add_unit_capacity_soldiers` | `void llm_strat_add_unit_capacity_soldiers(void)` |  |
| `0x00470536` | `llm_strat_add_unit_capacity_planes` | `void llm_strat_add_unit_capacity_planes(void)` |  |
| `0x0047058a` | `llm_strat_add_unit_capacity_helis` | `void llm_strat_add_unit_capacity_helis(void)` |  |
| `0x004705de` | `llm_strat_refresh_building` | `void llm_strat_refresh_building(game_t_Player_s p_id, map_t_building_id b_id)` |  |
| `0x00470b56` | `llm_strat_refresh_all_buildings` | `void llm_strat_refresh_all_buildings(uint player)` |  |
| `0x00470bdd` | `llm_strat_bldg_notify_ui` | `void llm_strat_bldg_notify_ui(undefined2 player, uint b_Index)` |  |
| `0x00470c5c` | `llm_strat_bldg_notify_state_change` | `void llm_strat_bldg_notify_state_change(ushort player, uint building_id)` |  |
| `0x00470cab` | `llm_bldg_finish_current_order` | `void llm_bldg_finish_current_order(uint player, uint building_index)` |  |
| `0x004710ba` | `llm_strat_bldg_apply_damage` | `void llm_strat_bldg_apply_damage(void)` |  |
| `0x004711c3` | `llm_strat_bldg_state_default_reset` | `void llm_strat_bldg_state_default_reset(void)` |  |
| `0x00471203` | `llm_strat_bldg_state_idle_noop` | `void llm_strat_bldg_state_idle_noop(void)` |  |
| `0x00471239` | `llm_strat_bldg_state_deploy_anim_wait` | `void llm_strat_bldg_state_deploy_anim_wait(void)` |  |
| `0x00471288` | `llm_strat_bldg_state_land_activate` | `void llm_strat_bldg_state_land_activate(uint param_1, uint param_2, uint param_3, uint param_4)` |  |
| `0x00471377` | `llm_strat_bldg_state_deploy_start` | `void llm_strat_bldg_state_deploy_start(uint param_1, uint param_2, uint param_3, uint param_4)` |  |
| `0x00471443` | `llm_strat_bldg_state_to_unit` | `void llm_strat_bldg_state_to_unit(void)` |  |
| `0x00471ab1` | `llm_strat_bldg_state_turret_scan` | `void llm_strat_bldg_state_turret_scan(void)` |  |
| `0x00471e53` | `llm_strat_bldg_state_turret_attack` | `void llm_strat_bldg_state_turret_attack(void)` |  |
| `0x00472415` | `llm_strat_bldg_state_idle_activate` | `void llm_strat_bldg_state_idle_activate(void)` |  |
| `0x004724af` | `llm_strat_bldg_state_construction` | `void llm_strat_bldg_state_construction(uint param_1, uint param_2, uint param_3, uint param_4)` |  |
| `0x00472674` | `llm_strat_bldg_state_charge_gate` | `void llm_strat_bldg_state_charge_gate(void)` |  |
| `0x00472979` | `llm_strat_bldg_state_charge_step` | `void llm_strat_bldg_state_charge_step(void)` |  |
| `0x00472b26` | `llm_strat_bldg_state_hangar_recharge_check` | `void llm_strat_bldg_state_hangar_recharge_check(void)` |  |
| `0x00472b9d` | `llm_strat_bldg_state_hangar_recharge_units` | `void llm_strat_bldg_state_hangar_recharge_units(void)` |  |
| `0x00472c42` | `llm_strat_bldg_state_upgrading` | `void llm_strat_bldg_state_upgrading(void)` |  |
| `0x00472eb7` | `llm_strat_bldg_state_dismantling` | `void llm_strat_bldg_state_dismantling(void)` |  |
| `0x00472f3c` | `llm_strat_bldg_state_researching` | `void llm_strat_bldg_state_researching(void)` |  |
| `0x00473194` | `llm_strat_bldg_state_destroyed` | `void llm_strat_bldg_state_destroyed(void)` |  |
| `0x004736a2` | `llm_strat_bldg_state_dismantle_finish` | `void llm_strat_bldg_state_dismantle_finish(void)` |  |
| `0x004738a3` | `llm_strat_bldg_state_rubble_sight_decay` | `void llm_strat_bldg_state_rubble_sight_decay(void)` |  |
| `0x00473a20` | `llm_strat_bldg_state_prod_pick_next` | `void llm_strat_bldg_state_prod_pick_next(void)` |  |
| `0x00473ca8` | `llm_strat_bldg_state_prod_working` | `void llm_strat_bldg_state_prod_working(uint param_1, uint param_2, uint param_3, uint param_4)` |  |
| `0x0047401c` | `llm_strat_reason_to_housing_bldg` | `int llm_strat_reason_to_housing_bldg(uint param_1, int param_2)` |  |
| `0x00474139` | `llm_strat_bldg_state_prod_blocked_notify` | `void llm_strat_bldg_state_prod_blocked_notify(void)` |  |
| `0x0047430b` | `llm_strat_bldg_state_prod_retry_wait` | `void llm_strat_bldg_state_prod_retry_wait(void)` |  |
| `0x00474384` | `llm_strat_bldg_state_mine_scan_deposits` | `void llm_strat_bldg_state_mine_scan_deposits(void)` |  |
| `0x004743e1` | `llm_strat_bldg_state_mine_check_deposits` | `void llm_strat_bldg_state_mine_check_deposits(void)` |  |
| `0x0047457a` | `llm_strat_bldg_state_mine_extracting` | `void llm_strat_bldg_state_mine_extracting(uint param_1, uint param_2, uint param_3, uint param_4)` |  |
| `0x0047464e` | `llm_strat_bldg_state_mine_depleted` | `void llm_strat_bldg_state_mine_depleted(void)` |  |
| `0x0047468e` | `llm_strat_bldg_state_mine_rescan_wait` | `void llm_strat_bldg_state_mine_rescan_wait(void)` |  |
| `0x00474707` | `llm_strat_bldg_state_power_primary_check` | `void llm_strat_bldg_state_power_primary_check(void)` |  |
| `0x0047477a` | `llm_strat_bldg_state_power_generate` | `void llm_strat_bldg_state_power_generate(void)` |  |
| `0x004747d6` | `llm_strat_bldg_register_online` | `void llm_strat_bldg_register_online(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x004749ed` | `llm_strat_bldg_online_default` | `void llm_strat_bldg_online_default(int player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00474ae0` | `llm_bldg_reset_construction_anim` | `void llm_bldg_reset_construction_anim(uint player, int building_index)` |  |
| `0x00474bfc` | `llm_strat_bldg_online_barracks_garage_a` | `void llm_strat_bldg_online_barracks_garage_a(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00474d4a` | `llm_strat_bldg_online_vehicles_h` | `void llm_strat_bldg_online_vehicles_h(int player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00474f33` | `llm_strat_bldg_online_soldiers_h` | `void llm_strat_bldg_online_soldiers_h(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x004750e5` | `llm_strat_bldg_online_helipad_a` | `void llm_strat_bldg_online_helipad_a(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00475233` | `llm_strat_bldg_online_helipad_h_or_misc` | `void llm_strat_bldg_online_helipad_h_or_misc(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x004753e5` | `llm_strat_bldg_online_airfield_a` | `void llm_strat_bldg_online_airfield_a(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00475597` | `llm_strat_bldg_online_airfield_h` | `void llm_strat_bldg_online_airfield_h(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00475811` | `llm_strat_bldg_online_shuttle_a` | `void llm_strat_bldg_online_shuttle_a(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x004759c3` | `llm_strat_bldg_online_shuttle_h` | `void llm_strat_bldg_online_shuttle_h(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00475b75` | `llm_strat_bldg_start_special_anim` | `void llm_strat_bldg_start_special_anim(ushort player, int b_index, undefined4 unused_ebx, undefined4 unused_ecx, double timestamp)` | conf:med |
| `0x00475ce3` | `llm_strat_bldg_start_liftoff_anim_shuttle` | `void llm_strat_bldg_start_liftoff_anim_shuttle(uint param_1, int param_2, uint param_3, uint param_4, uint param_5, uint param_6)` |  |
| `0x00475e51` | `llm_strat_bldg_anim_state_trigger` | `void llm_strat_bldg_anim_state_trigger(uint param_1, int param_2, uint param_3, uint param_4, uint param_5, uint param_6)` | conf:med |
| `0x00475ef9` | `llm_strat_bldg_start_liftoff_anim_mother` | `void llm_strat_bldg_start_liftoff_anim_mother(uint param_1, int param_2, uint param_3, uint param_4, uint param_5, uint param_6)` |  |
| `0x00475fa1` | `llm_strat_bldg_online_port_a` | `void llm_strat_bldg_online_port_a(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00476153` | `llm_strat_bldg_online_port_h` | `void llm_strat_bldg_online_port_h(short player, int building_index, uint param_3, uint param_4, double anim_dur)` |  |
| `0x00476305` | `llm_strat_bldg_tick_animation_state` | `void llm_strat_bldg_tick_animation_state(void)` |  |
| `0x00476447` | `llm_strat_bldg_anim_tick` | `void llm_strat_bldg_anim_tick(void)` | conf:high |
| `0x00476605` | `llm_strat_bldg_anim_state_turret` | `void llm_strat_bldg_anim_state_turret(void)` |  |
| `0x00476627` | `llm_strat_bldg_anim_state_barracks_garage_a` | `void llm_strat_bldg_anim_state_barracks_garage_a(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x004769a9` | `llm_strat_bldg_anim_state_vehicles_h` | `void llm_strat_bldg_anim_state_vehicles_h(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x00476e9d` | `llm_strat_bldg_anim_state_soldiers_h` | `void llm_strat_bldg_anim_state_soldiers_h(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x00477231` | `llm_strat_bldg_anim_state_helipad_a` | `void llm_strat_bldg_anim_state_helipad_a(void)` | conf:high |
| `0x004773f4` | `llm_strat_bldg_anim_state_helipad` | `void llm_strat_bldg_anim_state_helipad(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x00477881` | `llm_strat_bldg_anim_state_airfield_a` | `void llm_strat_bldg_anim_state_airfield_a(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x00477bed` | `llm_strat_bldg_anim_state_airfield_h` | `void llm_strat_bldg_anim_state_airfield_h(void)` | conf:high |
| `0x00477d9e` | `llm_strat_bldg_anim_state_shuttle_a` | `void llm_strat_bldg_anim_state_shuttle_a(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x0047815f` | `llm_strat_bldg_anim_state_shuttle_h` | `void llm_strat_bldg_anim_state_shuttle_h(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` |  |
| `0x00478520` | `llm_strat_bldg_anim_state_online_toggle` | `void llm_strat_bldg_anim_state_online_toggle(undefined4 unused_eax, undefined4 unused_edx, uint unused_ebx, uint unused_ecx)` | conf:high todo:enum |
| `0x004786f5` | `llm_strat_bldg_anim_state_port` | `void llm_strat_bldg_anim_state_port(void)` |  |
| `0x00478bfe` | `llm_strat_bldg_anim_state_port_h` | `void llm_strat_bldg_anim_state_port_h(void)` | conf:high |
| `0x00478d70` | `llm_strat_bldg_construction_complete` | `void llm_strat_bldg_construction_complete(uint player, uint building_index, uint param_3, uint param_4)` |  |
| `0x00478eb4` | `llm_strat_bldg_update_charge_pips` | `void llm_strat_bldg_update_charge_pips(ushort player, uint building_id)` |  |
| `0x00479244` | `llm_strat_bldg_tick_pip_anim` | `void llm_strat_bldg_tick_pip_anim(ushort player, int b_index)` | conf:med |
| `0x00479493` | `llm_strat_bldg_refund_resources_scaled_by_energy` | `void llm_strat_bldg_refund_resources_scaled_by_energy(int param_1, int param_2)` | conf:med |
| `0x00479562` | `llm_strat_bldg_apply_cfg_resources` | `void llm_strat_bldg_apply_cfg_resources(game_t_Player player, cfg_t_building_index building_type)` | conf:med |
| `0x004795dd` | `llm_strat_bldg_completion_dispatch` | `void llm_strat_bldg_completion_dispatch(uint param_1, uint param_2, uint param_3, uint param_4, double param_5)` |  |
| `0x0047ab20` | `llm_strat_mine_scan_deposit_slot` | `uint llm_strat_mine_scan_deposit_slot(uchar slot_index, uint player, int building_index)` |  |
| `0x0047b0db` | `llm_strat_hangar_any_unit_needs_energy` | `int llm_strat_hangar_any_unit_needs_energy(int param_1, int param_2)` |  |
| `0x0047b1be` | `llm_strat_hangar_recharge_pulse` | `void llm_strat_hangar_recharge_pulse(uint param_1, int param_2)` |  |
| `0x0047b36b` | `llm_strat_bldg_unmap_footprint` | `void llm_strat_bldg_unmap_footprint(ushort player, int building_index)` | conf:med |
| `0x0047ba6c` | `llm_strat_bldg_free_record` | `void llm_strat_bldg_free_record(uint param_1, int param_2)` |  |
| `0x0047baf9` | `llm_strat_turret_acquire_target` | `uint llm_strat_turret_acquire_target(uint player, int building_index, uint * out_1, uint * out_2)` |  |
| `0x0047bf8e` | `llm_strat_turret_fire` | `void llm_strat_turret_fire(uint player, uint bldg_index, int param_3, int param_4, int param_5, uint param_6, uint param_7, byte param_8)` |  |
| `0x0047c60b` | `llm_strat_bldg_turret_reload_tick` | `void llm_strat_bldg_turret_reload_tick(ushort player, uint building_id, double dt)` | conf:med |
| `0x0047c73a` | `llm_strat_unit_tick` | `void llm_strat_unit_tick(void)` |  |
| `0x0047c903` | `llm_strat_unit_state_move_walker` | `void llm_strat_unit_state_move_walker(void)` |  |
| `0x0047d741` | `llm_strat_unit_update_rotation` | `void llm_strat_unit_update_rotation(void)` |  |
| `0x0047dd56` | `llm_strat_unit_update_anim` | `void llm_strat_unit_update_anim(void)` |  |
| `0x0047de7d` | `llm_strat_unit_weapon_reload_tick` | `void llm_strat_unit_weapon_reload_tick(byte weapon_slot, double delta_time, double game_clock_unread)` |  |
| `0x0047e065` | `llm_strat_unit_target_tick` | `void llm_strat_unit_target_tick(void)` |  |
| `0x0047e1e1` | `llm_strat_unit_state_stop_to_default` | `void llm_strat_unit_state_stop_to_default(void)` |  |
| `0x0047e276` | `llm_strat_unit_state_parked_noop` | `void llm_strat_unit_state_parked_noop(void)` |  |
| `0x0047e2ac` | `llm_strat_unit_state_default_noop` | `void llm_strat_unit_state_default_noop(void)` |  |
| `0x0047e2e2` | `llm_strat_unit_state_patrol_swap` | `void llm_strat_unit_state_patrol_swap(void)` |  |
| `0x0047e380` | `llm_strat_unit_apply_damage` | `void llm_strat_unit_apply_damage(void)` |  |
| `0x0047e612` | `llm_strat_unit_update_soldiers` | `void llm_strat_unit_update_soldiers(void)` |  |
| `0x0047ea62` | `llm_strat_unit_state_squad_merge` | `void llm_strat_unit_state_squad_merge(void)` |  |
| `0x0047efc6` | `llm_strat_unit_state_exit_storage_begin` | `void llm_strat_unit_state_exit_storage_begin(void)` |  |
| `0x0047f270` | `llm_strat_unit_state_exit_wait` | `void llm_strat_unit_state_exit_wait(void)` |  |
| `0x0047f351` | `llm_strat_unit_state_exit_cancel` | `void llm_strat_unit_state_exit_cancel(void)` |  |
| `0x0047f3a1` | `llm_strat_unit_state_exit_walk_out` | `void llm_strat_unit_state_exit_walk_out(void)` |  |
| `0x0047f69f` | `llm_strat_unit_state_takeoff_taxi` | `void llm_strat_unit_state_takeoff_taxi(void)` |  |
| `0x0047f981` | `llm_strat_unit_state_dock_taxi_in` | `void llm_strat_unit_state_dock_taxi_in(void)` |  |
| `0x0047fe68` | `llm_strat_unit_state_enter_arrival_check` | `void llm_strat_unit_state_enter_arrival_check(void)` |  |
| `0x0047ff50` | `llm_strat_unit_state_enter_storage_begin` | `void llm_strat_unit_state_enter_storage_begin(void)` |  |
| `0x004800a8` | `llm_strat_unit_state_landing_request` | `void llm_strat_unit_state_landing_request(void)` |  |
| `0x00480218` | `llm_strat_unit_state_enter_wait` | `void llm_strat_unit_state_enter_wait(void)` |  |
| `0x004803bf` | `llm_strat_unit_state_enter_walk_in` | `void llm_strat_unit_state_enter_walk_in(void)` |  |
| `0x004807d9` | `llm_strat_unit_state_hover_engage` | `void llm_strat_unit_state_hover_engage(void)` |  |
| `0x00480b53` | `llm_strat_unit_state_move_path` | `void llm_strat_unit_state_move_path(void)` |  |
| `0x0048117c` | `llm_strat_unit_state_deploy_approach` | `void llm_strat_unit_state_deploy_approach(void)` |  |
| `0x004813f1` | `llm_strat_unit_state_takeoff_landing` | `void llm_strat_unit_state_takeoff_landing(void)` |  |
| `0x00481960` | `llm_strat_unit_state_climb_vertical` | `void llm_strat_unit_state_climb_vertical(void)` |  |
| `0x00481a6b` | `llm_strat_unit_state_deploy_to_building` | `void llm_strat_unit_state_deploy_to_building(void)` |  |
| `0x00481f7c` | `llm_strat_unit_state_descend_cruise` | `void llm_strat_unit_state_descend_cruise(void)` |  |
| `0x0048214d` | `llm_strat_unit_state_ascend_to_orbit` | `void llm_strat_unit_state_ascend_to_orbit(void)` |  |
| `0x004822dc` | `llm_strat_unit_state_corpse_fow_decay` | `void llm_strat_unit_state_corpse_fow_decay(void)` |  |
| `0x00482451` | `llm_strat_unit_state_group_marshal` | `void llm_strat_unit_state_group_marshal(void)` | conf:high |
| `0x00482f2e` | `llm_strat_unit_state_enter_class_move` | `void llm_strat_unit_state_enter_class_move(void)` |  |
| `0x00482f7f` | `llm_strat_unit_state_group_step` | `void llm_strat_unit_state_group_step(void)` |  |
| `0x00483011` | `llm_strat_unit_group_step_ground` | `void llm_strat_unit_group_step_ground(void)` |  |
| `0x00484a14` | `llm_strat_unit_get_ready_home_building` | `int llm_strat_unit_get_ready_home_building(void)` |  |
| `0x00484b4a` | `llm_strat_unit_group_step_plane` | `void llm_strat_unit_group_step_plane(void)` |  |
| `0x00484e10` | `llm_strat_unit_state_attack_building` | `void llm_strat_unit_state_attack_building(void)` |  |
| `0x00485008` | `llm_strat_unit_state_attack_unit` | `void llm_strat_unit_state_attack_unit(void)` |  |
| `0x00485466` | `llm_strat_storage_release_door_held_by_unit` | `int llm_strat_storage_release_door_held_by_unit(int player, int unit_idx)` |  |
| `0x004854f3` | `llm_strat_unit_state_die_explode` | `void llm_strat_unit_state_die_explode(void)` |  |
| `0x0048592d` | `llm_strat_unit_state_remove_silent` | `void llm_strat_unit_state_remove_silent(void)` |  |
| `0x00485bab` | `llm_strat_unit_state_step_adjacent` | `void llm_strat_unit_state_step_adjacent(void)` |  |
| `0x00485d43` | `llm_strat_unit_state_plot_turn_path` | `void llm_strat_unit_state_plot_turn_path(void)` |  |
| `0x00485ebb` | `llm_strat_unit_state_idle_scatter` | `void llm_strat_unit_state_idle_scatter(void)` |  |
| `0x00486106` | `llm_strat_unit_state_production_ready` | `void llm_strat_unit_state_production_ready(void)` |  |
| `0x004862c6` | `llm_strat_unit_fire_at_target_if_aimed` | `void llm_strat_unit_fire_at_target_if_aimed(void)` | conf:high todo:enum |
| `0x004863e6` | `llm_strat_unit_fire_at_target2_if_aimed` | `void llm_strat_unit_fire_at_target2_if_aimed(void)` | conf:high todo:enum |
| `0x00486506` | `llm_strat_unit_fire_at_target` | `void llm_strat_unit_fire_at_target(void)` |  |
| `0x00486657` | `llm_strat_unit_set_state_order` | `void llm_strat_unit_set_state_order(llm_strat_unit_state param_1, llm_strat_unit_state param_2)` |  |
| `0x00486697` | `llm_strat_unit_set_order` | `void llm_strat_unit_set_order(llm_strat_unit_state order)` | conf:high |
| `0x004866c9` | `llm_strat_unit_set_state` | `void llm_strat_unit_set_state(llm_strat_unit_state new_state)` |  |
| `0x004866fb` | `llm_strat_unit_chase_check` | `int llm_strat_unit_chase_check(void)` |  |
| `0x00486913` | `llm_strat_unit_set_state_order_of` | `void llm_strat_unit_set_state_order_of(int player, int unit_index, short state, short param)` |  |
| `0x0048696f` | `llm_unit_set_order_param` | `void llm_unit_set_order_param(int player, int unit_index, short param)` |  |
| `0x004869b0` | `llm_strat_unit_set_state_of` | `void llm_strat_unit_set_state_of(int player, int unit_index, short state)` |  |
| `0x00486a8a` | `llm_strat_facing_step_apply` | `void llm_strat_facing_step_apply(char * out_x, char * out_y, int cursor_state_index)` | conf:med |
| `0x00486b17` | `llm_strat_squad_placement_offset_lookup` | `void llm_strat_squad_placement_offset_lookup(int table_col, int table_row, char * out_a, char * out_b)` | conf:med |
| `0x00486b6d` | `llm_strat_tile_assert_unit_stack_coords` | `void llm_strat_tile_assert_unit_stack_coords(uint tile_x, uint tile_y)` |  |
| `0x00486ef7` | `llm_strat_unit_move_to_tile` | `void llm_strat_unit_move_to_tile(int owner, int unit_id, int tile_x, int tile_y)` | conf:high |
| `0x00486f41` | `llm_strat_unit_unlink_tile` | `void llm_strat_unit_unlink_tile(uint unit_player, ushort unit_index)` |  |
| `0x004870d8` | `llm_strat_unit_update_damage_smoke` | `void llm_strat_unit_update_damage_smoke(uint player, int unit_idx)` |  |
| `0x00487252` | `llm_strat_unit_remove_from_map` | `void llm_strat_unit_remove_from_map(ushort player, uint unit_idx)` |  |
| `0x0048753e` | `llm_strat_unit_teardown_mapped` | `void llm_strat_unit_teardown_mapped(uint param_1, uint param_2)` |  |
| `0x004877bc` | `llm_strat_unit_on_destroyed` | `void llm_strat_unit_on_destroyed(ushort player, uint unit_idx)` |  |
| `0x00487b25` | `llm_strat_unit_free_slot` | `void llm_strat_unit_free_slot(uint param_1, int param_2)` |  |
| `0x00487ba5` | `llm_strat_unit_teardown` | `void llm_strat_unit_teardown(uint player, ushort unit_index)` |  |
| `0x0048802e` | `llm_strat_unit_approach_weapon_range` | `int llm_strat_unit_approach_weapon_range(int * target_col, int * target_row)` |  |
| `0x00488291` | `llm_strat_unit_calc_range_approach_point` | `int llm_strat_unit_calc_range_approach_point(uint * io_target_x, uint * io_target_y)` | conf:med |
| `0x00488620` | `llm_map_passable_mark_annulus_set` | `void llm_map_passable_mark_annulus_set(int center_x, int center_y, int inner_radius, int outer_radius)` | conf:high |
| `0x0048874b` | `llm_map_passable_mark_annulus_clear` | `void llm_map_passable_mark_annulus_clear(int center_x, int center_y, int inner_radius, int outer_radius)` | conf:high |
| `0x00488876` | `llm_tact_move_find_reachable_dest` | `int llm_tact_move_find_reachable_dest(int start_col, int start_row, uint * inout_dest_col, uint * inout_dest_row)` | conf:high todo:globals |
| `0x004888f4` | `llm_strat_unit_release_path_and_targets` | `void llm_strat_unit_release_path_and_targets(ushort player, int unit_idx, undefined4 * out_cleared_pair)` |  |
| `0x00488a22` | `llm_strat_unit_notify_ui` | `void llm_strat_unit_notify_ui(uint side, uint unit_index)` | conf:high |
| `0x00488a8b` | `llm_unit_transport_unload_field` | `void llm_unit_transport_unload_field(uint transport_player, uint transport_unit_index)` |  |
| `0x0048905e` | `llm_strat_unit_squad_pick_lead_soldier_in_direction` | `uint llm_strat_unit_squad_pick_lead_soldier_in_direction(int param_1, uint param_2, undefined4 a2)` | conf:med |
| `0x00489164` | `llm_math_manhattan_dist` | `int llm_math_manhattan_dist(int x0, int y0, int x1, int y1)` |  |
| `0x004891d8` | `llm_unit_transport_unload_docked` | `void llm_unit_transport_unload_docked(uint player, uint unit_index)` |  |
| `0x00489521` | `llm_strat_unit_change_proto_and_energy` | `void llm_strat_unit_change_proto_and_energy(ushort player, int unit_idx, short proto_delta, int unused, double energy_delta)` | conf:med |
| `0x00489595` | `llm_strat_unit_soldier_remove_last` | `void llm_strat_unit_soldier_remove_last(uint player, int unit_index)` |  |
| `0x0048967b` | `llm_strat_unit_soldier_unlink` | `void llm_strat_unit_soldier_unlink(ushort player, int unit_idx, uint soldier_idx)` | conf:med |
| `0x004897c7` | `llm_strat_unit_soldiers_start_walk_anim` | `void llm_strat_unit_soldiers_start_walk_anim(uint player, int unit_index)` |  |
| `0x004899ed` | `llm_strat_squad_pick_free_formation_anchor` | `void llm_strat_squad_pick_free_formation_anchor(int param_1, char * param_2, char * param_3)` | conf:med |
| `0x00489ab6` | `llm_strat_unit_soldiers_set_heading` | `void llm_strat_unit_soldiers_set_heading(ushort player, int unit_index, byte sprite_frame)` |  |
| `0x00489b40` | `llm_strat_storage_resolve_exit_blockage` | `void llm_strat_storage_resolve_exit_blockage(uint player, int storage_slot)` |  |
| `0x00489cff` | `llm_strat_storage_exit_tile_is_clear` | `undefined4 llm_strat_storage_exit_tile_is_clear(ushort player, int storage_slot)` |  |
| `0x00489dc4` | `llm_strat_storage_remove_docked_unit` | `void llm_strat_storage_remove_docked_unit(ushort player, int unit_index, int storage_slot)` |  |
| `0x00489f54` | `llm_strat_storage_place_exit_ground` | `void llm_strat_storage_place_exit_ground(ushort player, int unit_index, int storage_slot)` |  |
| `0x0048a548` | `llm_strat_storage_can_exit` | `int llm_strat_storage_can_exit(int player, uint unit_index, int storage_slot)` |  |
| `0x0048a808` | `llm_strat_storage_can_enter` | `int llm_strat_storage_can_enter(ushort player, uint unit_index, uint storage_slot)` |  |
| `0x0048ae90` | `llm_strat_storage_can_land` | `int llm_strat_storage_can_land(int player, uint unit_index, int storage_slot)` |  |
| `0x0048b01d` | `llm_strat_storage_exit_air` | `void llm_strat_storage_exit_air(ushort player, int unit_index, int storage_slot, uint param_4)` |  |
| `0x0048b162` | `llm_strat_unit_takeoff_finalize` | `void llm_strat_unit_takeoff_finalize(game_t_Player param_1, uint param_2)` |  |
| `0x0048b294` | `llm_strat_tile_neighbor_in_dir` | `void llm_strat_tile_neighbor_in_dir(tile_coord x, tile_coord y, int dir, tile_coord * out_col, tile_coord * out_row)` |  |
| `0x0048b308` | `llm_strat_tile_neighbor_reverse_dir` | `void llm_strat_tile_neighbor_reverse_dir(int tile_x, int tile_y, int dir_index, uint * out_x, uint * out_y)` | conf:med todo:enum |
| `0x0048b37c` | `llm_strat_storage_get_approach_tile` | `void llm_strat_storage_get_approach_tile(ushort param_1, ushort param_2, uint * a2, uint * param_4, uint param_5)` |  |
| `0x0048b5a7` | `llm_strat_storage_dock_list_append` | `void llm_strat_storage_dock_list_append(ushort player, ushort unit_idx, ushort storage_slot)` |  |
| `0x0048b6ba` | `llm_strat_storage_board_unit` | `void llm_strat_storage_board_unit(game_t_Player_s player, undefined4 unit_idx, undefined4 storage_idx)` |  |
| `0x0048ba1a` | `llm_strat_storage_accept_landing` | `void llm_strat_storage_accept_landing(int player, int unit_index, int storage_slot, int path_slot)` |  |
| `0x0048ba9e` | `llm_strat_unit_select_weapon` | `byte llm_strat_unit_select_weapon(ushort player, int unit_index, uint target_mask)` |  |
| `0x0048bb6c` | `llm_strat_unit_fire_weapon` | `void llm_strat_unit_fire_weapon(uint param_1, uint param_2, byte a2, uint param_4, ushort param_5, int param_6, int param_7)` |  |
| `0x0048c6b2` | `llm_strat_weapon_scatter_offset` | `void llm_strat_weapon_scatter_offset(int param_1, int param_2, undefined4 a2, undefined4 param_4, undefined4 param_5, undefined4 param_6, int * param_7, int * param_8)` |  |
| `0x0048c79a` | `llm_strat_unit_walk_step_allowed` | `int llm_strat_unit_walk_step_allowed(uint param_1, int param_2)` | conf:med |
| `0x0048c83f` | `llm_strat_unit_calc_fine_axis_pos` | `int llm_strat_unit_calc_fine_axis_pos(ushort player, int unit_idx, char axis_is_x)` |  |
| `0x0048ca68` | `llm_strat_unit_calc_render_axis_pos` | `uint llm_strat_unit_calc_render_axis_pos(ushort player, int unit_idx, char axis_is_x)` |  |
| `0x0048ccb4` | `llm_strat_unit_calc_render_fine_y` | `uint llm_strat_unit_calc_render_fine_y(ushort player, int unit_idx)` |  |
| `0x0048ce21` | `llm_strat_unit_calc_interp_pixel_pos` | `int llm_strat_unit_calc_interp_pixel_pos(ushort player, int unit_idx, char axis_is_x)` |  |
| `0x0048cf3e` | `llm_strat_unit_hover_tile_crowded` | `int llm_strat_unit_hover_tile_crowded(int param_1, uint param_2)` |  |
| `0x0048d054` | `llm_bldg_calc_placement_corner_from_center` | `void llm_bldg_calc_placement_corner_from_center(ushort unit_index, int center_x, int center_y, uint * out_col, uint * out_row)` | conf:med |
| `0x0048d0ea` | `llm_bldg_calc_placement_corner_from_center_by_type` | `void llm_bldg_calc_placement_corner_from_center_by_type(ushort building_type, int center_x, int center_y, uint * out_x, uint * out_y)` | conf:med |
| `0x0048d16b` | `llm_strat_group_move_register_member` | `void llm_strat_group_move_register_member(int player, int unit_idx, int * scratch_count)` | conf:high |
| `0x0048d283` | `llm_strat_group_scratch_add_unit_and_normalize_heading` | `void llm_strat_group_scratch_add_unit_and_normalize_heading(int param_1, int param_2, int * a2)` | conf:med |
| `0x0048d2f4` | `llm_strat_heading_candidate_find_slot` | `int llm_strat_heading_candidate_find_slot(int heading, int turn_delta)` | conf:high todo:enum |
| `0x0048d36f` | `llm_strat_group_scratch_compute_centroid` | `void llm_strat_group_scratch_compute_centroid(int player, int member_count, int * out_x, int * out_y)` | conf:high |
| `0x0048d53d` | `llm_strat_group_member_find_nearest_to_point` | `int llm_strat_group_member_find_nearest_to_point(int player, int member_count, uint target_x, uint target_y)` |  |
| `0x0048d636` | `llm_strat_group_formation_trim_far_members` | `void llm_strat_group_formation_trim_far_members(int player, int * count_ptr, int goal_x, int goal_y)` | conf:med todo:dead |
| `0x0048d706` | `llm_strat_unit_refund_build_cost_by_health` | `void llm_strat_unit_refund_build_cost_by_health(int player, int unit_index)` | conf:med |
| `0x0048d7d5` | `llm_bldg_scrap_stored_units` | `void llm_bldg_scrap_stored_units(uint player, int building_index)` |  |
| `0x0048d886` | `llm_strat_production_complete` | `void llm_strat_production_complete(uint player, uint slot, double elapsed_time)` |  |
| `0x0048dc65` | `llm_strat_prod_deliver_arrivals` | `void llm_strat_prod_deliver_arrivals(void)` |  |
| `0x0048dea7` | `llm_prod_shuttle_slot_bind_default` | `int llm_prod_shuttle_slot_bind_default(uint player, int building_index)` |  |
| `0x0048e046` | `llm_strat_bldg_flush_cargo_hold` | `void llm_strat_bldg_flush_cargo_hold(uint player, int building_index)` |  |
| `0x0048e16a` | `llm_prod_shuttle_depart` | `int llm_prod_shuttle_depart(ushort player, int building_index, int dest_planet)` |  |
| `0x0048e2c0` | `llm_strat_prod_unload_cargo_manifest` | `int llm_strat_prod_unload_cargo_manifest(uint param_1, int param_2)` |  |
| `0x0048e3bb` | `llm_prod_shuttle_load_resource` | `int llm_prod_shuttle_load_resource(uint player, int building_index, uint resource_id, uint cap)` |  |
| `0x0048e4fb` | `llm_prod_shuttle_unload_resource` | `bool llm_prod_shuttle_unload_resource(ushort player, int building_index, ushort resource_id, int cap)` |  |
| `0x0048e5e5` | `llm_prod_shuttle_load_passengers` | `uint llm_prod_shuttle_load_passengers(ushort player, int building_index, uint cap)` |  |
| `0x0048e6f2` | `llm_prod_shuttle_unload_passengers` | `int llm_prod_shuttle_unload_passengers(ushort player, int building_index, int cap)` |  |
| `0x0048e7c5` | `llm_strat_unit_load_into_shuttle_cargo` | `int llm_strat_unit_load_into_shuttle_cargo(ushort player, int building_idx, ushort unit_idx)` |  |
| `0x0048ea27` | `llm_strat_prod_unload_cargo_unit` | `undefined4 llm_strat_prod_unload_cargo_unit(ushort param_1, int param_2, uint a2)` |  |
| `0x0048ec51` | `llm_prod_bldg_depart_finalize` | `int llm_prod_bldg_depart_finalize(ushort player, int building_index, int dest_planet)` |  |
| `0x0048efcc` | `llm_strat_prod_set_transfer_destination` | `int llm_strat_prod_set_transfer_destination(uint player_idx, int prod_slot, int dest_planet)` |  |
| `0x0048f2f8` | `llm_bldg_transfer_notify_noop` | `void llm_bldg_transfer_notify_noop(void)` |  |
| `0x0048f31e` | `llm_strat_prod_spawn_arrived_unit` | `map_t_unit_id llm_strat_prod_spawn_arrived_unit(ushort param_1, uint param_2, uint a2, uint param_4, int param_5)` |  |
| `0x0048f7a6` | `llm_prod_shuttle_bay_unload_all` | `void llm_prod_shuttle_bay_unload_all(ushort player, int building_index)` | conf:med |
| `0x0048f89e` | `llm_strat_storage_scrap_home_docked_units` | `void llm_strat_storage_scrap_home_docked_units(ushort player, int unit_index)` |  |
| `0x0048f952` | `llm_strat_storage_launch_parked_to_orbit` | `int llm_strat_storage_launch_parked_to_orbit(ushort player, int building_index)` |  |
| `0x0048fa45` | `llm_strat_bldg_ui_shuttle_slot_free` | `int llm_strat_bldg_ui_shuttle_slot_free(void)` | conf:high |
| `0x0048fa97` | `llm_strat_bldg_shuttle_slot_is_free` | `int llm_strat_bldg_shuttle_slot_is_free(int player, int building_id)` |  |
| `0x0048fbfa` | `llm_strat_locate_active_port` | `undefined4 llm_strat_locate_active_port(uint player, int * out_col, int * out_row, uint * out_port_slot)` |  |
| `0x0048fdd0` | `llm_strat_bldg_find_mothership_position` | `undefined4 llm_strat_bldg_find_mothership_position(int player, uint * out_x, uint * out_y)` |  |
| `0x0048feef` | `llm_strat_prod_bind_planet` | `int llm_strat_prod_bind_planet(int player, int queue_slot, int shuttle_slot)` | conf:med |
| `0x0048ff73` | `llm_strat_prod_unbind_planet` | `void llm_strat_prod_unbind_planet(int player, int planet_slot)` |  |
| `0x0048ffe9` | `llm_strat_prod_reset_system` | `void llm_strat_prod_reset_system(void)` |  |
| `0x0049007d` | `llm_strat_bldg_get_shuttle_reserved_resource` | `int llm_strat_bldg_get_shuttle_reserved_resource(int param_1, int param_2, int a2)` |  |
| `0x00490100` | `llm_strat_bldg_get_shuttle_passengers_reserved` | `int llm_strat_bldg_get_shuttle_passengers_reserved(int player, int bldg_idx)` |  |
| `0x0049017a` | `llm_strat_bldg_get_resource_capacity` | `int llm_strat_bldg_get_resource_capacity(int player, int bldg_idx, int resource_id)` |  |
| `0x004901d2` | `llm_strat_bldg_get_human_transport_flag` | `int llm_strat_bldg_get_human_transport_flag(int player, int bldg_idx)` |  |
| `0x00490221` | `llm_prod_planet_distance_factor` | `double llm_prod_planet_distance_factor(int src_planet, int dest_planet)` | conf:med |
| `0x00490264` | `llm_strat_prod_shuttle_count_active` | `int llm_strat_prod_shuttle_count_active(void)` | conf:high |
| `0x004902d9` | `llm_strat_prod_shuttle_slot_by_ordinal` | `int llm_strat_prod_shuttle_slot_by_ordinal(int ordinal)` | conf:high |
| `0x00490368` | `llm_strat_prod_transfer_progress` | `double llm_strat_prod_transfer_progress(int slot)` |  |
| `0x00490462` | `llm_map_system_planet_bbox` | `void llm_map_system_planet_bbox(uint * out_min_x, uint * out_min_y, uint * out_max_x, uint * out_max_y)` | conf:med todo:globals |
| `0x004905a4` | `llm_strat_shuttle_capacity_remaining` | `int llm_strat_shuttle_capacity_remaining(int dock_slot, game_t_PlanetIndex planet)` | conf:med |
| `0x004906c1` | `llm_strat_prod_shuttle_slot_spawn_arrival` | `void llm_strat_prod_shuttle_slot_spawn_arrival(uint slot_index)` | conf:high |
| `0x0049076b` | `llm_strat_bldg_get_unit_capacity_clamped` | `int llm_strat_bldg_get_unit_capacity_clamped(int player, int bldg_idx)` |  |
| `0x004907f3` | `llm_fx_anim_chain_last_sprite_id` | `int llm_fx_anim_chain_last_sprite_id(int anim_id)` |  |
| `0x00490841` | `llm_strat_unit_calc_mount_fine_pos` | `uint llm_strat_unit_calc_mount_fine_pos(ushort player, int unit_idx, uint mount_idx, char axis_is_x)` |  |
| `0x00490b82` | `llm_strat_unit_calc_mount_render_pos` | `int llm_strat_unit_calc_mount_render_pos(ushort player, int unit_idx, uint mount_idx, char axis_is_x)` | conf:high |
| `0x00490eb1` | `llm_strat_bldg_get_sprite_anchor_coord` | `uint llm_strat_bldg_get_sprite_anchor_coord(uint player, int b_index, int anchor_kind, byte axis)` | conf:med |
| `0x00491086` | `llm_strat_unit_soldier_get_sprite_screen_pos` | `void llm_strat_unit_soldier_get_sprite_screen_pos(ushort param_1, int param_2, int a2, uint * param_4, uint * param_5)` |  |
| `0x00491260` | `llm_strat_power_recompute` | `void llm_strat_power_recompute(game_t_Player_s player)` |  |
| `0x00491328` | `llm_strat_population_add` | `void llm_strat_population_add(game_t_Player_s player, int count)` |  |
| `0x00491486` | `llm_strat_population_remove` | `void llm_strat_population_remove(uint player, int count)` |  |
| `0x00491604` | `llm_strat_decay_excess_resources` | `bool llm_strat_decay_excess_resources(int player, int res)` |  |
| `0x00491689` | `llm_strat_population_layoff_workers` | `void llm_strat_population_layoff_workers(int player, int worker_count)` |  |
| `0x004917c5` | `llm_strat_bldg_remove_workers` | `uint llm_strat_bldg_remove_workers(ushort player, uint building_id, uint count)` |  |
| `0x00491916` | `llm_strat_bldg_add_workers` | `int llm_strat_bldg_add_workers(ushort player, uint building_id, int count)` |  |
| `0x00491b78` | `llm_strat_bldg_assign_workers` | `int llm_strat_bldg_assign_workers(undefined4 player, uint building_id, int count)` |  |
| `0x00491c08` | `llm_strat_bldg_unassign_workers` | `int llm_strat_bldg_unassign_workers(ushort player, uint building_index, uint count)` |  |
| `0x00491c76` | `llm_strat_bldg_power_network_recompute` | `void llm_strat_bldg_power_network_recompute(ushort player)` |  |
| `0x0049209b` | `llm_strat_bldg_propagate_network_connectivity` | `void llm_strat_bldg_propagate_network_connectivity(ushort player, int b_index)` |  |
| `0x0049232e` | `llm_strat_bldg_link_to_network_if_adjacent` | `void llm_strat_bldg_link_to_network_if_adjacent(undefined2 player, undefined4 index)` |  |
| `0x00492843` | `llm_strat_prod_try_start_unit` | `int llm_strat_prod_try_start_unit(uint param_1, int param_2)` |  |
| `0x00492b60` | `llm_strat_unit_type_group_index` | `int llm_strat_unit_type_group_index(int unit_id)` | conf:med todo:enum |
| `0x00492bf1` | `llm_unit_apply_production_completion` | `void llm_unit_apply_production_completion(uint player, int unit_proto_id)` |  |
| `0x00492e35` | `llm_cfg_apply_project_resources` | `void llm_cfg_apply_project_resources(uint player_id, cfg_t_project_id project_index)` | conf:high |
| `0x00492eb1` | `llm_bldg_pay_build_cost` | `int llm_bldg_pay_build_cost(uint player, int building_type_id)` |  |
| `0x00492ff9` | `llm_strat_bldg_pay_cycle_inputs` | `int llm_strat_bldg_pay_cycle_inputs(undefined4 player, undefined4 b_index)` |  |
| `0x0049312c` | `llm_strat_bldg_calc_remaining_resource_need` | `int llm_strat_bldg_calc_remaining_resource_need(ushort param_1, int param_2, cfg_enum_E_RESOURCE a2)` |  |
| `0x00493254` | `llm_strat_unit_try_pay_action_cost` | `int llm_strat_unit_try_pay_action_cost(uint param_1, int param_2)` | conf:med |
| `0x00493387` | `llm_strat_bldg_grant_resource_bonus` | `int llm_strat_bldg_grant_resource_bonus(uint player, int bldg_idx)` |  |
| `0x0049342a` | `llm_strat_bldg_sum_stored_resource` | `int llm_strat_bldg_sum_stored_resource(int building_id, int param2_unused, int resource_type)` | conf:med |
| `0x004934b6` | `llm_prod_shuttle_fuel_check` | `int llm_prod_shuttle_fuel_check(ushort player, int building_index, int dest_planet)` |  |
| `0x00493705` | `llm_prod_shuttle_fuel_apply` | `int llm_prod_shuttle_fuel_apply(ushort player, int building_index, int dest_planet)` | conf:high |
| `0x004937bf` | `llm_strat_bldg_grant_type_resources` | `void llm_strat_bldg_grant_type_resources(game_t_Player player_idx, cfg_t_building_index building_type_idx)` | conf:high |
| `0x0049383b` | `llm_map_bldg_footprint_clear_passable` | `void llm_map_bldg_footprint_clear_passable(int origin_x, int origin_y, int building_idx)` | conf:high |
| `0x004938d3` | `llm_map_bldg_footprint_set_passable` | `void llm_map_bldg_footprint_set_passable(int origin_x, int origin_y, int building_idx)` | conf:high |
| `0x0049396b` | `llm_bldg_footprint_is_clear` | `int llm_bldg_footprint_is_clear(int x, int y, int building_type, uint viewer)` |  |
| `0x00493a96` | `llm_strat_bldg_footprint_destroy_occupants` | `int llm_strat_bldg_footprint_destroy_occupants(int param_1, int param_2, int a2)` |  |
| `0x00493c28` | `llm_tact_tile_calc_fog_edge_shape` | `void llm_tact_tile_calc_fog_edge_shape(int tile_col, int tile_row)` | conf:high |
| `0x00493fb2` | `llm_strat_planet_distance` | `double llm_strat_planet_distance(int x1, int y1, int x2, int y2)` |  |
| `0x0049404e` | `llm_strat_tile_dist_wrapped` | `int llm_strat_tile_dist_wrapped(int x1, int y1, int x2, int y2)` |  |
| `0x004940a9` | `llm_map_wrap_delta_x` | `int llm_map_wrap_delta_x(int pos_a, undefined4 unused_param, int pos_b)` | conf:med |
| `0x00494131` | `llm_map_wrap_delta_y` | `int llm_map_wrap_delta_y(int x1, int y1, int x2, int y2)` | conf:med |
| `0x004941b9` | `llm_strat_tile_delta_wrapped` | `void llm_strat_tile_delta_wrapped(int x1, int y1, int x2, int y2, int * out_dx, int * out_dy)` |  |
| `0x004942a1` | `llm_map_wrap_delta_x_quarterres` | `int llm_map_wrap_delta_x_quarterres(int x1, int y1, int x2, int y2)` | conf:med |
| `0x00494349` | `llm_map_wrap_delta_y_quarterres` | `int llm_map_wrap_delta_y_quarterres(int x1, int y1, int x2, int y2)` | conf:med |
| `0x004943e6` | `llm_map_wrap_delta_col` | `int llm_map_wrap_delta_col(int x1, int y1, int x2, int y2)` |  |
| `0x0049446e` | `llm_map_wrap_delta_row` | `int llm_map_wrap_delta_row(int x1, int y1, int x2, int y2)` | conf:med |
| `0x004944f6` | `llm_strat_pixel_delta_wrapped` | `void llm_strat_pixel_delta_wrapped(int param_1, int param_2, int a2, int param_4, int * param_5, int * param_6)` |  |
| `0x004945de` | `llm_strat_map_wrapped_delta` | `void llm_strat_map_wrapped_delta(int param_1, int param_2, int a2, int param_4, double * param_5, double * param_6)` |  |
| `0x004946d2` | `llm_strat_facing24_from_points` | `byte llm_strat_facing24_from_points(char from_x, char from_y, char to_x, char to_y)` |  |
| `0x004947c2` | `llm_strat_dir_from_to_is_octant_sector` | `int llm_strat_dir_from_to_is_octant_sector(tile_coord x1, tile_coord y1, tile_coord x2, tile_coord y2)` |  |
| `0x0049482b` | `llm_strat_dir_from_to` | `int llm_strat_dir_from_to(tile_coord x1, tile_coord y1, tile_coord x2, tile_coord y2)` | conf:med |
| `0x004948ff` | `llm_strat_dir_sector_to` | `int llm_strat_dir_sector_to(int x0, int y0, int x1, int y1)` | conf:med todo:enum |
| `0x00494aaf` | `llm_map_fow_reveal_here` | `void llm_map_fow_reveal_here(uint col, uint row)` | conf:high |
| `0x00494aec` | `llm_strat_fow_remove_sight_r12` | `void llm_strat_fow_remove_sight_r12(int col, int row)` | conf:low |
| `0x00494b29` | `llm_tact_move_step_attempt` | `int llm_tact_move_step_attempt(uint src_col, uint src_row, uint dst_col, uint dst_row)` | conf:med |
| `0x00494eb8` | `llm_tact_move_commit_pending_path` | `int llm_tact_move_commit_pending_path(uint player, int unit_idx)` | conf:high |
| `0x00494f0c` | `llm_strat_group_move_request_build` | `void llm_strat_group_move_request_build(ushort goal_x, ushort goal_y, uint player, int member_count, uint flags)` | conf:med todo:dead |
| `0x0049508a` | `llm_strat_unit_assign_path_from_job_result` | `undefined4 llm_strat_unit_assign_path_from_job_result(ushort param_1, int param_2, uint a2)` |  |
| `0x00495214` | `llm_strat_group_move_member_step_blocked` | `int llm_strat_group_move_member_step_blocked(uint player, int unit_idx, uint scratch_idx)` |  |
| `0x00495406` | `llm_strat_unit_path_step_blocked` | `int llm_strat_unit_path_step_blocked(uint player, int unit_idx)` |  |
| `0x0049561f` | `llm_strat_path_make_single_step` | `int llm_strat_path_make_single_step(uint param_1, int param_2)` |  |
| `0x0049581d` | `llm_strat_path_write_from_solver` | `void llm_strat_path_write_from_solver(uint param_1, int param_2, uint param_3, uint param_4, int param_5)` |  |
| `0x00495aa0` | `llm_strat_unit_path_store_result` | `void llm_strat_unit_path_store_result(uint player, int unit_idx, undefined4 unused1, undefined4 unused2, int path_slot)` | conf:med |
| `0x00495b6f` | `llm_strat_storage_setup_approach_path` | `void llm_strat_storage_setup_approach_path(uint player, int unit_index, int path_slot, int storage_slot)` |  |
| `0x00495cd3` | `llm_strat_storage_setup_exit_path` | `void llm_strat_storage_setup_exit_path(uint player, int unit_index, byte exit_x, byte exit_y, int path_slot, int storage_slot)` |  |
| `0x00495f1b` | `llm_strat_path_find_free_slot` | `int llm_strat_path_find_free_slot(int player)` |  |
| `0x00495f87` | `llm_strat_unit_assign_shared_path` | `int llm_strat_unit_assign_shared_path(uint player, int unit_index, uchar start_col, uchar start_row)` | conf:med |
| `0x0049610f` | `llm_strat_facing24_to_delta` | `void llm_strat_facing24_to_delta(uint facing24, int * out_dx, int * out_dy)` | conf:high |
| `0x0049646f` | `llm_ui_print_floating_msg_cyan` | `void llm_ui_print_floating_msg_cyan(LPWSTR message)` | conf:low |
| `0x004964bd` | `llm_ui_print_floating_msg_red` | `int llm_ui_print_floating_msg_red(LPWSTR message)` | conf:low |
| `0x0049653d` | `llm_ui_print_queue_text_id` | `void llm_ui_print_queue_text_id(int text_id)` | conf:med |
| `0x004965d6` | `llm_bldg_set_connected_flag` | `void llm_bldg_set_connected_flag(ushort player, int b_index)` |  |
| `0x0049663d` | `llm_strat_bldg_clear_flag_bit0_notify` | `void llm_strat_bldg_clear_flag_bit0_notify(ushort player, int building_index)` | conf:med |
| `0x004966a4` | `llm_strat_bldg_set_staffed_flag` | `void llm_strat_bldg_set_staffed_flag(ushort player, int building_index)` |  |
| `0x0049670b` | `llm_strat_bldg_clear_staffed_flag` | `void llm_strat_bldg_clear_staffed_flag(ushort player, uint building_id)` |  |
| `0x00496772` | `llm_strat_bldg_staffed_flag_toggle` | `void llm_strat_bldg_staffed_flag_toggle(uint player_idx, int bldg_idx)` |  |
| `0x004967ce` | `llm_unit_state_is_boarding` | `int llm_unit_state_is_boarding(int state)` |  |
| `0x00496868` | `llm_strat_fow_remove_sight` | `void llm_strat_fow_remove_sight(game_t_Player player, int x, int y, byte radius)` |  |
| `0x004968b6` | `llm_strat_sight_add_circle` | `game_t_Player llm_strat_sight_add_circle(game_t_Player param_1, int param_2, int a2, int param_4, byte param_5)` |  |
| `0x0049694f` | `llm_strat_sight_remove_circle` | `int llm_strat_sight_remove_circle(int param_1, int param_2, int a2, int param_4, undefined1 param_5)` |  |
| `0x004969e8` | `llm_strat_path_free_slot` | `void llm_strat_path_free_slot(ushort player, int unit_index)` |  |
| `0x00496a7b` | `llm_strat_path_attach_slot` | `void llm_strat_path_attach_slot(int param_1, int param_2, int param_3)` |  |
| `0x00496ad5` | `llm_strat_offscreen_snd_volume` | `int llm_strat_offscreen_snd_volume(int tile_col, int tile_row)` |  |
| `0x00496bac` | `llm_strat_bldg_upgrade_available` | `int llm_strat_bldg_upgrade_available(int player_index, int building_index)` |  |
| `0x00496c75` | `llm_strat_bldg_mine_extraction_rate` | `double llm_strat_bldg_mine_extraction_rate(int player, int bldg_idx, int resource_type)` | rev:verified |
| `0x00496d89` | `llm_strat_bldg_player_mine_extraction_total` | `double llm_strat_bldg_player_mine_extraction_total(int player, int resource_type)` | rev:verified |
| `0x00496e42` | `llm_strat_bldg_mine_scan_nearby_resource` | `int llm_strat_bldg_mine_scan_nearby_resource(int player, int bldg_idx, uint resource_type)` | conf:med |
| `0x00497066` | `llm_strat_bldg_sum_player_mines_resource` | `int llm_strat_bldg_sum_player_mines_resource(int player_index, int resource_id)` | conf:med |
| `0x00497322` | `llm_strat_bldg_count_same_type_scan` | `int llm_strat_bldg_count_same_type_scan(int player, int building_idx)` | conf:med |
| `0x00497405` | `llm_strat_bldg_is_network_critical` | `int llm_strat_bldg_is_network_critical(int player, int b_index)` |  |
| `0x00497623` | `llm_game_speed_recompute` | `void llm_game_speed_recompute(void)` | conf:med |
| `0x004976cd` | `llm_game_speed_increase` | `void llm_game_speed_increase(uint player)` |  |
| `0x00497734` | `llm_game_speed_decrease` | `void llm_game_speed_decrease(uint player)` |  |
| `0x0049779b` | `llm_strat_unit_housing_count_add` | `void llm_strat_unit_housing_count_add(int housing_id, int unit_index)` |  |
| `0x00497842` | `llm_strat_unit_housing_count_remove` | `void llm_strat_unit_housing_count_remove(int player, int unit_proto_id)` |  |
| `0x004978e9` | `llm_save_prepare_directories` | `void llm_save_prepare_directories(void)` | conf:med todo:globals |
| `0x00497975` | `llm_game_clear_save_temp_files` | `void llm_game_clear_save_temp_files(void)` | conf:high todo:globals |
| `0x00497ac4` | `llm_bldg_storage_accepts_unit_type` | `int llm_bldg_storage_accepts_unit_type(short building_type, ushort unit_type)` |  |
| `0x00497b91` | `llm_strat_storage_type_accepts_unit` | `int llm_strat_storage_type_accepts_unit(uint building_index, ushort unit_index)` | conf:med |
| `0x00497cfb` | `llm_strat_get_vehicle_building_sub_id` | `uint llm_strat_get_vehicle_building_sub_id(uint player, int building_index)` |  |
| `0x00497f4a` | `llm_resource_add` | `void llm_resource_add(int player, int resource_index, int amount)` |  |
| `0x0049800c` | `llm_player_teardown_hook_stub` | `int llm_player_teardown_hook_stub(int player_index)` | conf:med |
| `0x0049803b` | `llm_game_sp_outcome_announce` | `void llm_game_sp_outcome_announce(void)` | conf:med |
| `0x00498089` | `llm_strat_player_presence_lost` | `uint llm_strat_player_presence_lost(uint player, uint mode)` |  |
| `0x004987ae` | `llm_game_session_clear_system_presence_flag` | `void llm_game_session_clear_system_presence_flag(void)` | conf:med |
| `0x004988b0` | `llm_strat_bldg_uses_workers` | `int llm_strat_bldg_uses_workers(uint player, int building_index)` | conf:high |
| `0x00498900` | `llm_ui_healthbars_enabled` | `int llm_ui_healthbars_enabled(uint player_select_flags, int entity_id)` |  |
| `0x00498934` | `llm_strat_tiles_adjacent` | `int llm_strat_tiles_adjacent(int x1, int y1, int x2, int y2)` |  |
| `0x004989c2` | `llm_map_setup_dimensions` | `void llm_map_setup_dimensions(void)` |  |
| `0x00498a3f` | `llm_map_set_minimap_zoom_for_size` | `void llm_map_set_minimap_zoom_for_size(void)` |  |
| `0x00498aad` | `llm_strat_mother_reelect_primary` | `int llm_strat_mother_reelect_primary(int player, int x, int y)` |  |
| `0x00498dc0` | `llm_strat_game_check_players_mothership_alive` | `int llm_strat_game_check_players_mothership_alive(void)` | conf:med |
| `0x00498eb8` | `llm_cam_pan_to_target` | `void llm_cam_pan_to_target(void)` |  |
| `0x00498fb4` | `llm_cam_pan_to_building` | `void llm_cam_pan_to_building(game_t_Player player_idx, int building_id)` | conf:med |
| `0x0049902b` | `llm_cam_center_on_ctrl_group` | `void llm_cam_center_on_ctrl_group(int ctrl_group_index)` |  |
| `0x004990bf` | `llm_progress_collect_available_projects` | `void llm_progress_collect_available_projects(game_t_Player player)` |  |
| `0x00499174` | `llm_progress_recheck_projects` | `void llm_progress_recheck_projects(game_t_Player player)` |  |
| `0x004991f2` | `llm_progress_recheck_buildings` | `void llm_progress_recheck_buildings(game_t_Player player)` |  |
| `0x0049928a` | `llm_combat_credit_planet_conquest_kills` | `void llm_combat_credit_planet_conquest_kills(uint player)` |  |
| `0x00499420` | `llm_strat_ctrlgrp_is_unit_selected` | `int llm_strat_ctrlgrp_is_unit_selected(game_t_Player player_idx, map_t_unit_id unit_id)` | conf:high |
| `0x0049949d` | `llm_strat_invasion_due_check` | `int llm_strat_invasion_due_check(void)` |  |
| `0x0049953a` | `llm_strat_invasion_chance_roll` | `int llm_strat_invasion_chance_roll(int building_completed)` |  |
| `0x004998ae` | `llm_strat_spawn_enemy_landing` | `int llm_strat_spawn_enemy_landing(void)` |  |
| `0x00499a3a` | `llm_strat_clock_resync_units_and_buildings` | `void llm_strat_clock_resync_units_and_buildings(double new_time)` |  |
| `0x00499bd0` | `llm_strat_unit_purge_unregistered` | `void llm_strat_unit_purge_unregistered(uint player)` |  |
| `0x00499dc8` | `llm_strat_rng_seed_wallclock_seconds` | `int llm_strat_rng_seed_wallclock_seconds(void)` | conf:med |
| `0x00499e06` | `llm_strat_player_has_planet_presence` | `int llm_strat_player_has_planet_presence(int player, uint planet)` |  |
| `0x00499ef6` | `llm_tact_save_screenshot` | `void llm_tact_save_screenshot(void)` | conf:high |
| `0x00499f49` | `llm_rand_below` | `int llm_rand_below(int upper_bound)` |  |
| `0x00499f84` | `llm_rand_below_fx` | `int llm_rand_below_fx(uint upper_bound)` |  |
| `0x00499fbf` | `llm_strat_rng_seed_ch0` | `void llm_strat_rng_seed_ch0(uint seed_value)` |  |
| `0x00499ff2` | `llm_strat_rng_seed_ch1` | `void llm_strat_rng_seed_ch1(uint seed_value)` |  |
| `0x0049a025` | `llm_bldg_first_occupied_unit_slot_has_soldiers` | `int llm_bldg_first_occupied_unit_slot_has_soldiers(int building_index)` | conf:med |
| `0x0049a0b3` | `llm_diplomacy_set_relation` | `void llm_diplomacy_set_relation(int player_a, int player_b, byte relation)` |  |
| `0x0049a1ce` | `llm_strat_path_alloc_slot` | `int llm_strat_path_alloc_slot(int path_group_idx, int entity_id)` | conf:med |
| `0x0049a23d` | `llm_strat_path_step_check_and_request_detour` | `int llm_strat_path_step_check_and_request_detour(uint src_x, uint src_y, int dst_x, int dst_y)` |  |
| `0x0049a48c` | `llm_strat_play_building_select_sound` | `void llm_strat_play_building_select_sound(uint player_idx, int bldg_idx)` |  |
| `0x0049a61c` | `llm_strat_bldg_footprint_mark_passable` | `void llm_strat_bldg_footprint_mark_passable(int tile_x, int tile_y)` | conf:med |
| `0x0049a652` | `llm_strat_bldg_footprint_mark_impassable` | `void llm_strat_bldg_footprint_mark_impassable(int tile_x, int tile_y)` | conf:med |
| `0x0049a688` | `llm_strat_bldg_set_footprint_passable` | `uint llm_strat_bldg_set_footprint_passable(int param_1, int param_2, int a2)` |  |
| `0x0049a849` | `llm_map_view_zoom_step` | `void llm_map_view_zoom_step(int zoom_direction)` |  |
| `0x0049a8c2` | `llm_unit_bldg_energy_refill_full` | `void llm_unit_bldg_energy_refill_full(uint player_and_flags, uint target_index)` |  |
| `0x0049a9aa` | `llm_unit_bldg_apply_scaled_damage` | `void llm_unit_bldg_apply_scaled_damage(uint target_selector, int target_index)` |  |
| `0x0049aa6e` | `llm_unit_bldg_apply_lethal_damage` | `void llm_unit_bldg_apply_lethal_damage(uint target_ref, int target_index)` |  |
| `0x0049ab26` | `llm_map_fow_reveal_full` | `void llm_map_fow_reveal_full(game_t_Player player)` |  |
| `0x0049ab96` | `llm_str_to_upper_inplace` | `void llm_str_to_upper_inplace(char * str)` |  |
| `0x0049ac8d` | `llm_gfx_sprite_pix_offsets_init` | `void llm_gfx_sprite_pix_offsets_init(void)` | conf:low |
| `0x0049ad06` | `llm_strat_minimap_init` | `undefined llm_strat_minimap_init(void)` |  |
| `0x0049b352` | `llm_ui_get_menu_resource_ptr` | `void * llm_ui_get_menu_resource_ptr(char * name)` | conf:med |
| `0x0049b3a7` | `llm_str_truncate_with_ellipsis` | `void llm_str_truncate_with_ellipsis(char * src, char * dst, int max_len)` | conf:high |
| `0x0049b447` | `llm_strat_invasion_alert_reset_all` | `void llm_strat_invasion_alert_reset_all(void)` |  |
| `0x0049b49e` | `llm_strat_invasion_alert_arm` | `void llm_strat_invasion_alert_arm(int planet, double timestamp)` |  |
| `0x0049b4dd` | `llm_strat_invasion_alert_clear` | `void llm_strat_invasion_alert_clear(int planet)` |  |
| `0x0049b51c` | `llm_strat_invasion_alert_poll` | `int llm_strat_invasion_alert_poll(double now)` |  |
| `0x0049b5b0` | `llm_strat_advisor_tick` | `void llm_strat_advisor_tick(double now)` |  |
| `0x0049b8f5` | `llm_strat_storage_purge_dead_docked` | `void llm_strat_storage_purge_dead_docked(int player, map_t_building_id storage_sub_id)` |  |
| `0x0049b9fc` | `llm_net_init_stub` | `undefined llm_net_init_stub(void)` | conf:med |
| `0x0049ba27` | `llm_net_shutdown_stub` | `void llm_net_shutdown_stub(void)` | conf:high |
| `0x0049ba6b` | `llm_net_disconnect_stub` | `void llm_net_disconnect_stub(void)` |  |
| `0x0049ba8d` | `llm_net_transport_send` | `void llm_net_transport_send(void * buf, int len)` | conf:med |
| `0x0049bab3` | `llm_net_transport_recv` | `int llm_net_transport_recv(int * sender_id_out, void * buf, int * len)` | conf:med |
| `0x0049bb98` | `llm_net_discovery_refresh_stub` | `void llm_net_discovery_refresh_stub(int handle)` | conf:med |
| `0x0049bbbd` | `llm_net_session_init_stub` | `undefined llm_net_session_init_stub(void)` |  |
| `0x0049bbeb` | `llm_net_join_connect_stub` | `undefined llm_net_join_connect_stub(void)` |  |
| `0x0049bc19` | `llm_lobby_map_send_step_stub` | `undefined llm_lobby_map_send_step_stub(void)` | conf:low |
| `0x0049bc44` | `llm_teardown_hook_stub` | `void llm_teardown_hook_stub(void)` | conf:low |
| `0x0049bc66` | `llm_teardown_hook_stub_b` | `void llm_teardown_hook_stub_b(void)` | conf:high |
| `0x0049bc88` | `llm_net_lockstep_hook_stub` | `void llm_net_lockstep_hook_stub(void)` | conf:low |
| `0x0049bcaa` | `llm_net_flush_stub` | `void llm_net_flush_stub(void)` | conf:med |
| `0x0049bcee` | `llm_net_lockstep_no_players` | `int llm_net_lockstep_no_players(void)` | conf:med |
| `0x0049bd5d` | `llm_net_discovery_poll_stub` | `undefined llm_net_discovery_poll_stub(void)` | conf:low |
| `0x0049bd88` | `llm_net_lobby_scan_state_reset` | `void llm_net_lobby_scan_state_reset(void)` | conf:med |
| `0x0049bde0` | `llm_net_session_list_merge_refresh` | `void llm_net_session_list_merge_refresh(void)` |  |
| `0x0049bfa5` | `llm_lobby_map_recv_step_stub` | `void llm_lobby_map_recv_step_stub(void)` | conf:low |
| `0x0049bfd0` | `llm_net_resolve_ip_stub` | `undefined llm_net_resolve_ip_stub(void)` | conf:med |
| `0x0049bffe` | `llm_net_connect_prep_stub` | `undefined llm_net_connect_prep_stub(void)` |  |
| `0x0049c02c` | `llm_net_lockstep_sync_delay_stub` | `int llm_net_lockstep_sync_delay_stub(void)` | conf:high |
| `0x0049c088` | `llm_net_lockstep_pump` | `void llm_net_lockstep_pump(void)` | conf:high |
| `0x0049c112` | `llm_net_lockstep_extend_if_near_horizon` | `void llm_net_lockstep_extend_if_near_horizon(double lookahead_scale)` | conf:high |
| `0x0049c189` | `llm_net_lockstep_commit_horizon` | `void llm_net_lockstep_commit_horizon(void)` | conf:high |
| `0x0049c22c` | `llm_net_lockstep_find_horizon_match_side` | `int llm_net_lockstep_find_horizon_match_side(void)` | conf:med |
| `0x0049c2cd` | `llm_net_lockstep_dispatch` | `void llm_net_lockstep_dispatch(void)` | conf:med |
| `0x0049d33b` | `llm_net_send_lockstep_extend` | `void llm_net_send_lockstep_extend(double horizon)` | todo:globals |
| `0x0049d3c8` | `llm_net_send_order` | `int llm_net_send_order(llm_strat_order order)` | todo:globals |
| `0x0049d450` | `llm_net_chat_send_team` | `void llm_net_chat_send_team(void * text, uint len)` | todo:globals |
| `0x0049d50b` | `llm_net_chat_send_all` | `void llm_net_chat_send_all(void * text, uint len)` | todo:globals |
| `0x0049d5c1` | `llm_ui_chat_target_add` | `void llm_ui_chat_target_add(byte player_id)` | conf:high |
| `0x0049d5f8` | `llm_ui_chat_target_remove` | `void llm_ui_chat_target_remove(byte player_id)` | conf:high |
| `0x0049d631` | `llm_ui_chat_recalc_target_mode` | `void llm_ui_chat_recalc_target_mode(void)` | conf:med |
| `0x0049d750` | `llm_net_chat_ally_mask_rebuild` | `void llm_net_chat_ally_mask_rebuild(void)` | conf:high |
| `0x0049d7ff` | `llm_net_send_buf_flush` | `int llm_net_send_buf_flush(void)` | conf:high |
| `0x0049d84c` | `llm_net_send_lockstep_keepalive` | `void llm_net_send_lockstep_keepalive(int side_id)` | conf:med |
| `0x0049d8ef` | `llm_net_lockstep_broadcast_resync_state` | `void llm_net_lockstep_broadcast_resync_state(double exec_time)` | conf:low |
| `0x0049d9d6` | `llm_net_lockstep_force_resync` | `void llm_net_lockstep_force_resync(void)` | conf:med |
| `0x0049da72` | `llm_net_send_lockstep_step_size` | `void llm_net_send_lockstep_step_size(double step_size)` | conf:high todo:enum |
| `0x0049dafe` | `llm_net_send_lockstep_rate_scale` | `void llm_net_send_lockstep_rate_scale(double scale, int side_id)` | conf:med todo:enum todo:globals |
| `0x0049dbb4` | `llm_net_send_lockstep_resync_resume` | `void llm_net_send_lockstep_resync_resume(double unused_arg)` | conf:high todo:enum |
| `0x0049dc16` | `llm_net_send_lockstep_kick` | `void llm_net_send_lockstep_kick(int side_id)` | conf:high todo:enum |
| `0x0049dca3` | `llm_net_player_remove` | `void llm_net_player_remove(int side_id)` | todo:globals |
| `0x0049ddc1` | `llm_net_player_remove_timeout` | `void llm_net_player_remove_timeout(int side_id)` | todo:globals |
| `0x0049dedf` | `llm_net_lockstep_broadcast_player_leave` | `void llm_net_lockstep_broadcast_player_leave(int side_id)` | conf:med |
| `0x0049e01f` | `llm_net_lockstep_send_horizon_ack` | `void llm_net_lockstep_send_horizon_ack(int side_id)` | conf:med todo:enum todo:globals |
| `0x0049e0d4` | `llm_net_lockstep_send_horizon_desync` | `void llm_net_lockstep_send_horizon_desync(int side_id)` | conf:med todo:enum todo:globals |
| `0x0049e189` | `llm_net_lockstep_send_slot_reset` | `void llm_net_lockstep_send_slot_reset(int side_id)` | conf:med todo:globals |
| `0x0049e230` | `llm_net_lockstep_reset_player_horizon` | `void llm_net_lockstep_reset_player_horizon(int player_idx)` | conf:low todo:globals todo:struct |
| `0x0049e2c8` | `llm_net_send_lockstep_ctrl_sub2` | `void llm_net_send_lockstep_ctrl_sub2(void)` | conf:med |
| `0x0049e328` | `llm_net_lockstep_send_presence_lost` | `void llm_net_lockstep_send_presence_lost(void)` | conf:high todo:enum |
| `0x0049e388` | `llm_strat_player_by_side_id` | `int llm_strat_player_by_side_id(int side_id)` |  |
| `0x0049e3ea` | `llm_net_lockstep_count_active_players` | `int llm_net_lockstep_count_active_players(void)` | conf:high |
| `0x0049e462` | `llm_net_player_slot_from_id` | `int llm_net_player_slot_from_id(int player_id)` | conf:med todo:globals |
| `0x0049e4c3` | `llm_net_session_globals_reset` | `void llm_net_session_globals_reset(void)` | conf:high |
| `0x0049e5fd` | `llm_net_lockstep_status_debug_stub` | `void llm_net_lockstep_status_debug_stub(int status_arg)` |  |
| `0x0049e653` | `llm_net_lockstep_is_local_leader_peer` | `int llm_net_lockstep_is_local_leader_peer(int exclude_side_id)` | conf:med |
| `0x0049e6f5` | `llm_net_lockstep_sync_busywait` | `int llm_net_lockstep_sync_busywait(void)` | conf:high |
| `0x0049e759` | `llm_game_player_set_human` | `void llm_game_player_set_human(byte player)` |  |
| `0x0049e79c` | `llm_game_player_set_ai` | `void llm_game_player_set_ai(byte player)` |  |
| `0x0049e7e1` | `llm_strat_debug_make_all_players_human` | `void llm_strat_debug_make_all_players_human(void)` |  |
| `0x0049e834` | `llm_net_send_lockstep_peer_horizon` | `void llm_net_send_lockstep_peer_horizon(double horizon, int marker)` | conf:low todo:enum |
| `0x0049ea0e` | `llm_net_bandwidth_stats_draw` | `void llm_net_bandwidth_stats_draw(void)` | conf:med todo:globals todo:struct |
| `0x0049f110` | `llm_strat_unit_show_debug_status` | `void llm_strat_unit_show_debug_status(int player, int unit_idx)` | conf:low |
| `0x0049f146` | `llm_strat_unit_format_debug_status` | `void llm_strat_unit_format_debug_status(int player, int unit_idx)` |  |
| `0x0049f73c` | `llm_strat_bldg_refresh_resource_needs` | `int llm_strat_bldg_refresh_resource_needs(int sys_idx, int building_idx)` |  |
| `0x0049f84a` | `llm_bldg_load_resource_tail_noop` | `void llm_bldg_load_resource_tail_noop(void)` | conf:med |
| `0x0049f8a0` | `llm_strat_cfg_scan_planet_type_marker` | `int llm_strat_cfg_scan_planet_type_marker(int fallback_value)` | conf:low todo:enum todo:globals |
| `0x0049f95d` | `llm_map_scan_invention_planets_noop` | `void llm_map_scan_invention_planets_noop(void)` | conf:med |
| `0x0049fa3d` | `llm_map_scan_current_system_progress_noop` | `void llm_map_scan_current_system_progress_noop(void)` | conf:med |
| `0x0049fab3` | `llm_snd_debug_cfg_browser_dispatch` | `void llm_snd_debug_cfg_browser_dispatch(int cmd)` | conf:high todo:enum todo:globals |
| `0x0049fc6f` | `llm_debug_roll_random` | `void llm_debug_roll_random(void)` |  |
| `0x0049fcc2` | `llm_debug_print_team_planet_status` | `void llm_debug_print_team_planet_status(void)` | conf:med |
| `0x0049fef7` | `llm_net_lockstep_peer_timing_reset` | `void llm_net_lockstep_peer_timing_reset(void)` | conf:med |
| `0x0049ff58` | `llm_net_lockstep_record_peer_horizon` | `int llm_net_lockstep_record_peer_horizon(int player_index, double horizon, int order_marker)` | conf:high todo:globals |
| `0x004a00e5` | `llm_strat_bldg_compute_state_checksum` | `int llm_strat_bldg_compute_state_checksum(void)` | conf:med |
| `0x004a01d6` | `llm_unit_alive_count_resync` | `void llm_unit_alive_count_resync(int player_idx)` | conf:med |
| `0x004a026b` | `llm_wnd_on_create` | `void llm_wnd_on_create(void * hWnd)` | todo:globals |
| `0x004a02d6` | `llm_wnd_on_destroy` | `undefined llm_wnd_on_destroy(void)` | todo:globals |
| `0x004a035e` | `llm_frame_dispatch` | `undefined llm_frame_dispatch(void)` |  |
| `0x004a0452` | `llm_wnd_on_activate` | `void llm_wnd_on_activate(void * hWnd, uint uMsg, uint activate_state, int lParam)` | todo:globals |
| `0x004a04fc` | `llm_wnd_on_activateapp` | `undefined llm_wnd_on_activateapp(void)` |  |
| `0x004a0524` | `llm_wnd_proc` | `LRESULT llm_wnd_proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)` |  |
| `0x004a06d8` | `llm_game_create_main_window` | `undefined llm_game_create_main_window(void)` |  |
| `0x004a07e0` | `llm_game_parse_cmdline` | `undefined llm_game_parse_cmdline(void)` |  |
| `0x004a17b3` | `llm_strat_minimap_render` | `void llm_strat_minimap_render(void)` |  |
| `0x004a2302` | `llm_strat_minimap_mark_tiles_drawn` | `void llm_strat_minimap_mark_tiles_drawn(void)` | conf:med |
| `0x004a2344` | `llm_strat_minimap_draw_viewport_box` | `void llm_strat_minimap_draw_viewport_box(void)` |  |
| `0x004a23a8` | `llm_lookup_indirect_id_byte` | `uint llm_lookup_indirect_id_byte(byte id)` | conf:low todo:globals |
| `0x004a2400` | `llm_gfx_font_layout_text` | `void llm_gfx_font_layout_text(ushort * text)` | conf:med todo:globals |
| `0x004a2493` | `llm_gfx_font_measure_text_width` | `int llm_gfx_font_measure_text_width(ushort * text, int max_chars)` | conf:high todo:globals |
| `0x004a251a` | `llm_gfx_font_select` | `void llm_gfx_font_select(int font_slot)` |  |
| `0x004a2580` | `llm_gfx_font_load` | `undefined llm_gfx_font_load(void)` |  |
| `0x004a26e4` | `llm_gfx_font_system_init` | `undefined llm_gfx_font_system_init(void)` |  |
| `0x004a2802` | `llm_gfx_font_unload` | `void llm_gfx_font_unload(void)` | conf:high todo:globals |
| `0x004a284c` | `llm_gfx_text_mark_tiles_dirty` | `void llm_gfx_text_mark_tiles_dirty(int x, int y)` | conf:high todo:globals |
| `0x004a291a` | `llm_ui_text_draw_wrapped_shadowed` | `void llm_ui_text_draw_wrapped_shadowed(int x, int y, wchar_t * text, uint color)` | conf:med todo:globals |
| `0x004a2a64` | `llm_ui_wrapped_text_line_draw` | `void llm_ui_wrapped_text_line_draw(int x, int y, LPWSTR text, uint color, uint msg_kind)` | conf:med |
| `0x004a2be7` | `llm_gfx_font_measure_text` | `int llm_gfx_font_measure_text(ushort * text)` | conf:high todo:globals |
| `0x004a2c1f` | `llm_gfx_font_get_line_height` | `int llm_gfx_font_get_line_height(void)` | conf:high |
| `0x004a2c4c` | `llm_gfx_draw_text_blend_clipped` | `void llm_gfx_draw_text_blend_clipped(int x, int y, ushort * text, short color)` | conf:high |
| `0x004a2c9a` | `llm_gfx_draw_text_rgb` | `void llm_gfx_draw_text_rgb(int x, int y, wchar_t * text, byte color_r, byte color_g, byte color_b)` | conf:med todo:globals |
| `0x004a2d16` | `llm_gfx_sprite_clone_frame_pixels` | `int llm_gfx_sprite_clone_frame_pixels(int sprite_index_a, int sprite_index_b)` | conf:low todo:struct |
| `0x004a2df5` | `llm_gfx_font_draw_glyph_solid` | `void llm_gfx_font_draw_glyph_solid(int glyph_or_sprite_index, int x, int y, undefined4 glyph_stream_selector, undefined4 fill_color_pair)` | conf:med todo:globals todo:struct |
| `0x004a2e6b` | `llm_gfx_draw_glyph_to_surface` | `uint llm_gfx_draw_glyph_to_surface(ushort * surface, int x, int y, uint char_code, short color)` | conf:med todo:globals |
| `0x004a2f1b` | `llm_gfx_draw_glyph_to_framebuffer` | `uint llm_gfx_draw_glyph_to_framebuffer(byte * glyph_data, int x, int y, short color)` | conf:med todo:globals |
| `0x004a2f9d` | `llm_gfx_font_set_line_height` | `int llm_gfx_font_set_line_height(int new_line_height)` | conf:high |
| `0x004a2fdc` | `llm_gfx_font_draw_glyph_blend_clipped` | `void llm_gfx_font_draw_glyph_blend_clipped(void)` | conf:med todo:globals todo:struct |
| `0x004a310f` | `llm_gfx_font_blit_glyph_solid` | `void llm_gfx_font_blit_glyph_solid(void)` | conf:med todo:globals todo:struct |
| `0x004a33d1` | `llm_map_load_main_plane` | `void llm_map_load_main_plane(void)` |  |
| `0x004a34f8` | `llm_map_load_passable_plane` | `void llm_map_load_passable_plane(void)` |  |
| `0x004a3594` | `llm_map_load_halfres_plane` | `void llm_map_load_halfres_plane(undefined4 param_1)` |  |
| `0x004a36c5` | `llm_map_load_objects` | `void llm_map_load_objects(undefined4 param_1)` |  |
| `0x004a384e` | `llm_map_load_resources_plane` | `void llm_map_load_resources_plane(void)` |  |
| `0x004a3b1c` | `llm_map_load_landing_spots` | `void llm_map_load_landing_spots(undefined4 param_1)` |  |
| `0x004a3e61` | `llm_map_load_loose_fallback` | `undefined4 llm_map_load_loose_fallback(undefined4 param_1, undefined4 param_2, undefined4 param_3, undefined4 param_4)` |  |
| `0x004a4177` | `llm_map_readfile_loose_fallback` | `undefined4 llm_map_readfile_loose_fallback(cfg_struct_map_header * param_1)` |  |
| `0x004a44e0` | `llm_map_read_planet_header` | `int llm_map_read_planet_header(int planet_index, int * out_field1, int * out_field2, int * out_field3)` | conf:high |
| `0x004a4542` | `llm_cfg_map_read_header` | `int llm_cfg_map_read_header(char * dir_path, char * file_name, int * out_field1, int * out_field2, int * out_field3)` | conf:med todo:struct |
| `0x004a46d8` | `llm_map_load_main_plane_loose` | `void llm_map_load_main_plane_loose(int entry_count)` |  |
| `0x004a47e5` | `llm_map_load_main_plane_skip` | `void llm_map_load_main_plane_skip(void)` |  |
| `0x004a4827` | `llm_map_load_passable_plane_loose` | `void llm_map_load_passable_plane_loose(void)` |  |
| `0x004a48b7` | `llm_map_load_passable_plane_skip` | `void llm_map_load_passable_plane_skip(void)` |  |
| `0x004a48f9` | `llm_map_load_halfres_plane_loose` | `void llm_map_load_halfres_plane_loose(int entry_count)` |  |
| `0x004a4a00` | `llm_map_load_halfres_plane_skip` | `void llm_map_load_halfres_plane_skip(int entry_count)` |  |
| `0x004a4a77` | `llm_map_load_objects_loose` | `undefined llm_map_load_objects_loose(void)` |  |
| `0x004a4be6` | `llm_map_seek_object_records_loose` | `void llm_map_seek_object_records_loose(int record_count)` | conf:high |
| `0x004a4c1e` | `llm_map_load_resources_plane_loose` | `void llm_map_load_resources_plane_loose(void)` |  |
| `0x004a4edf` | `llm_map_seek_region_table_loose` | `void llm_map_seek_region_table_loose(void)` | conf:med |
| `0x004a4f48` | `llm_map_load_landing_spots_loose` | `undefined llm_map_load_landing_spots_loose(void)` |  |
| `0x004a4ff4` | `llm_strat_render_view_metrics_refresh` | `void llm_strat_render_view_metrics_refresh(void)` |  |
| `0x004a5167` | `llm_strat_render_ground_tile` | `void llm_strat_render_ground_tile(void)` | todo:globals |
| `0x004a5778` | `llm_gfx_draw_dashed_rect_outline_colored` | `void llm_gfx_draw_dashed_rect_outline_colored(void)` | conf:med todo:globals todo:struct |
| `0x004a5a6b` | `llm_map_cam_mark_viewport_dirty` | `void llm_map_cam_mark_viewport_dirty(void)` |  |
| `0x004a5ae8` | `llm_map_view_tilevis_fill_bottom_row` | `void llm_map_view_tilevis_fill_bottom_row(void)` | conf:med |
| `0x004a5b12` | `llm_map_view_tilevis_fill_right_margin` | `void llm_map_view_tilevis_fill_right_margin(void)` | conf:high |
| `0x004a5b4d` | `llm_gfx_draw_cursor_menu` | `void llm_gfx_draw_cursor_menu(void)` |  |
| `0x004a5d3b` | `llm_gfx_blit_sprite_ui` | `void llm_gfx_blit_sprite_ui(void)` |  |
| `0x004a665a` | `llm_gfx_sprite_rle_row_skip` | `undefined llm_gfx_sprite_rle_row_skip(void)` | conf:med todo:proto |
| `0x004a66cc` | `llm_gfx_blit_rect_rows` | `void llm_gfx_blit_rect_rows(void)` | conf:high todo:globals |
| `0x004a6737` | `llm_gfx_hittest_begin` | `void llm_gfx_hittest_begin(void)` |  |
| `0x004a6771` | `llm_gfx_hittest_check` | `void llm_gfx_hittest_check(void)` |  |
| `0x004a6946` | `llm_strat_fow_remove_sight_apply` | `void llm_strat_fow_remove_sight_apply(void)` | conf:high |
| `0x004a6a68` | `llm_gfx_tile_fog_edge_lookup` | `void llm_gfx_tile_fog_edge_lookup(void)` | conf:med |
| `0x004a6bb2` | `llm_gfx_terrain_screen_blend_convert` | `void llm_gfx_terrain_screen_blend_convert(void)` | conf:med todo:globals |
| `0x004a6bf2` | `llm_gfx_blit_rgb565_hblend_alt_rows` | `void llm_gfx_blit_rgb565_hblend_alt_rows(void)` | conf:low |
| `0x004a6c49` | `llm_gfx_blit_avi_frame_rgb565_2x` | `void llm_gfx_blit_avi_frame_rgb565_2x(void)` | conf:high todo:globals |
| `0x004a763f` | `llm_gfx_sprite_rle_row_skip_hittest` | `undefined llm_gfx_sprite_rle_row_skip_hittest(void)` |  |
| `0x004a7e12` | `llm_map_set_zoom_scale` | `void llm_map_set_zoom_scale(double zoom_x, double zoom_y)` | conf:high todo:globals |
| `0x004a7ebe` | `llm_map_zoom_scale_x_inc` | `void llm_map_zoom_scale_x_inc(double delta_x)` |  |
| `0x004a7f16` | `llm_map_zoom_scale_x_dec` | `void llm_map_zoom_scale_x_dec(double delta_x)` |  |
| `0x004a7f6a` | `llm_map_zoom_scale_y_inc` | `void llm_map_zoom_scale_y_inc(double delta_y)` |  |
| `0x004a7fc2` | `llm_map_zoom_scale_y_dec` | `void llm_map_zoom_scale_y_dec(double delta_y)` |  |
| `0x004a8016` | `llm_map_zoom_scale_x_get` | `double llm_map_zoom_scale_x_get(void)` |  |
| `0x004a8084` | `llm_map_zoom_apply` | `void llm_map_zoom_apply(void)` | conf:med |
| `0x004a8145` | `llm_map_zoom_translate_cursor` | `void llm_map_zoom_translate_cursor(void)` | conf:high |
| `0x004a82b0` | `llm_gfx_bitmap_resample_center_out` | `void llm_gfx_bitmap_resample_center_out(undefined4 dead1, undefined4 dead2, undefined4 dead3, undefined4 dead4, undefined2 * pixel_buf, int old_width, int old_height, uint new_width, uint new_height, uint row_pitch_bytes, uint pixel_halve_mask)` | conf:high todo:globals todo:struct |
| `0x004a881b` | `llm_gfx_draw_sprite` | `void llm_gfx_draw_sprite(int sprite_id, int dst_x, int dst_y)` |  |
| `0x004a889f` | `llm_gfx_draw_sprite_dark` | `void llm_gfx_draw_sprite_dark(int sprite_id, int dst_x, int dst_y)` |  |
| `0x004a8923` | `llm_gfx_draw_sprite_unclipped` | `void llm_gfx_draw_sprite_unclipped(int sprite_id, int dst_x, int dst_y)` |  |
| `0x004a89db` | `llm_gfx_draw_sprite_clipped_width` | `void llm_gfx_draw_sprite_clipped_width(int param_1, int param_2, int a2, int param_4, int param_5)` |  |
| `0x004a8ab3` | `llm_gfx_draw_sprite_to_buffer` | `undefined llm_gfx_draw_sprite_to_buffer(ushort * header, int sprite_id, int dst_x, int dst_y)` |  |
| `0x004a8b73` | `llm_ui_set_draw_surface` | `void llm_ui_set_draw_surface(void * surface, int pitch, int x, int y, int clip_x, int clip_y, int clip_w, int clip_h, void * src_bitmap)` | conf:med |
| `0x004a933d` | `llm_cfg_skip_spaces` | `WCHAR * llm_cfg_skip_spaces(WCHAR * strPtr)` |  |
| `0x004b3fe1` | `llm_ui_progress_bar_draw` | `void llm_ui_progress_bar_draw(int x, int y, int width, uint percent)` | conf:high |
| `0x004b4249` | `llm_gfx_draw_bitmask_icon_6x6` | `int llm_gfx_draw_bitmask_icon_6x6(int dst_x_offset, byte icon_index)` | conf:low todo:globals todo:struct |
| `0x004b42df` | `llm_gfx_draw_gauge_bar_horizontal` | `void llm_gfx_draw_gauge_bar_horizontal(int x, int y, int width, int height, int percent, byte fill_color_r5, byte fill_color_g6, byte fill_color_b5, byte empty_color_r5, byte empty_color_g6, byte empty_color_b5)` | conf:med |
| `0x004b44cc` | `llm_gfx_draw_gauge_bar_vertical` | `void llm_gfx_draw_gauge_bar_vertical(int x, int y, int width, int height, int percent, byte fill_color_r5, byte fill_color_g6, byte fill_color_b5, byte empty_color_r5, byte empty_color_g6, byte empty_color_b5)` | conf:med |
| `0x004b46d0` | `llm_ui_text_blit_char_glyph` | `undefined llm_ui_text_blit_char_glyph(void)` | conf:med todo:proto |
| `0x004b47bb` | `llm_util_memset16` | `undefined llm_util_memset16(void)` | conf:med |
| `0x004b47e3` | `llm_mem_copy_words` | `undefined llm_mem_copy_words(void)` |  |
| `0x004b49da` | `llm_strat_ai_grid_fill_below_threshold` | `void llm_strat_ai_grid_fill_below_threshold(byte * grid_base, int width, int height, int threshold, int fill_value)` | conf:high |
| `0x004b4a10` | `llm_strat_ai_grid_flood_step` | `void llm_strat_ai_grid_flood_step(byte * grid_base, int width, int height, int source_level, int fill_value)` | conf:high |
| `0x004b4ac2` | `llm_strat_ai_grid_stamp_seeds` | `void llm_strat_ai_grid_stamp_seeds(byte * grid, int map_width, int map_height, byte * stencil, int span_x, int span_y, int origin_x, int origin_y, int seed_value)` |  |
| `0x004b4b1c` | `llm_strat_ai_grid_match_stencil` | `int llm_strat_ai_grid_match_stencil(byte * grid, int grid_width, int grid_height, byte * footprint_mask, int span_x, int span_y, int start_x, int start_y, int target_byte)` | conf:high |
| `0x004b4b7a` | `llm_strat_ai_grid_stencil_all_near_unthreatened` | `int llm_strat_ai_grid_stencil_all_near_unthreatened(byte * grid, int grid_width, int grid_height, byte * footprint_mask, int span_x, int span_y, int start_x, int start_y)` | conf:high |
| `0x004b4bdd` | `llm_scan_masked_table_for_empty_cell` | `int llm_scan_masked_table_for_empty_cell(byte * grid, int grid_width, int grid_height, byte * footprint_mask, int span_x, int span_y, int start_x, int start_y)` | conf:med |
| `0x004b4c3f` | `llm_strat_ai_grid_stamp_threat_ring` | `void llm_strat_ai_grid_stamp_threat_ring(byte * grid_base, int grid_height, int grid_width, int center_y, int center_x, int radius)` |  |
| `0x004b4c94` | `llm_strat_ai_grid_clear_threat_bit` | `void llm_strat_ai_grid_clear_threat_bit(byte * grid_base, int row_count, int col_count)` |  |
| `0x004b4cc0` | `llm_rand_prng_tick_slot` | `int llm_rand_prng_tick_slot(int slot)` |  |
| `0x004b4ce6` | `llm_strat_rng_seed_channel` | `void llm_strat_rng_seed_channel(int ch, uint value)` |  |
| `0x004b4d01` | `llm_strat_rng_next` | `int llm_strat_rng_next(int channel, int lo, int hi)` |  |
| `0x004b4d3b` | `llm_rand_state_advance` | `double llm_rand_state_advance(int rng_index)` |  |
| `0x004b4d70` | `llm_ui_avi_ic_send_msg_0x403e_dup` | `undefined llm_ui_avi_ic_send_msg_0x403e_dup(void)` | conf:high todo:proto todo:struct |
| `0x004b4df8` | `llm_ui_avi_ic_send_msg_0x403c_dup` | `undefined llm_ui_avi_ic_send_msg_0x403c_dup(void)` | conf:high todo:proto todo:struct |
| `0x004b4e80` | `llm_ui_avi_ic_send_msg_0x403d_dup` | `undefined llm_ui_avi_ic_send_msg_0x403d_dup(void)` | conf:high todo:proto todo:struct |
| `0x004b4f08` | `llm_ui_avi_ic_send_msg_0x4032_dup` | `undefined llm_ui_avi_ic_send_msg_0x4032_dup(void)` | conf:high todo:proto todo:struct |
| `0x004b4f6c` | `llm_ui_avi_ic_send_msg_0x4048_dup` | `undefined llm_ui_avi_ic_send_msg_0x4048_dup(void)` | conf:high todo:proto todo:struct |
| `0x004b4fb8` | `llm_ui_error_dialog_proc` | `int llm_ui_error_dialog_proc(HWND hwnd, uint msg, WPARAM wparam, LPARAM lparam)` | todo:struct |
| `0x004b51bd` | `llm_ui_show_formatted_error_dialog` | `void llm_ui_show_formatted_error_dialog(wchar_t * format, ...)` | todo:globals |
| `0x004b528f` | `llm_ui_modal_key_pump` | `int llm_ui_modal_key_pump(void)` | conf:med todo:globals |
| `0x004b53a5` | `llm_ui_mouse_poll_next_event` | `undefined llm_ui_mouse_poll_next_event(void)` |  |
| `0x004b54da` | `llm_ui_menu_transition_settle` | `undefined llm_ui_menu_transition_settle(void)` | conf:med |
| `0x004b55be` | `llm_gfx_draw_sprite_vclipped` | `void llm_gfx_draw_sprite_vclipped(ushort * sprite_header, int y, int x, int clip_top, int clip_bottom)` | conf:med |
| `0x004b56b2` | `llm_ui_text_measure_width` | `int llm_ui_text_measure_width(undefined4 font_ctx, ushort * text)` |  |
| `0x004b5726` | `llm_ui_tab_stop_advance` | `int llm_ui_tab_stop_advance(int * pos_record, int target)` | conf:med |
| `0x004b579b` | `llm_ui_text_layout_line` | `int llm_ui_text_layout_line(int font_or_style_id, ushort * text, int * rect, int * tab_stops, uint style_flags)` | conf:high todo:globals |
| `0x004b5a6e` | `llm_ui_rich_text_layout_walk` | `int llm_ui_rich_text_layout_walk(void * * layout_ctx, ushort * text, int * line_state, uint format_flags)` | conf:med todo:globals todo:struct |
| `0x004b5c8a` | `llm_gfx_draw_formatted_text` | `void llm_gfx_draw_formatted_text(llm_gfx_font_desc * font, int color_index, ushort * text, int * pos_rect, uint flags)` | conf:med todo:globals todo:struct |
| `0x004b6027` | `llm_ui_text_wrap_find_break` | `int llm_ui_text_wrap_find_break(llm_gfx_font_desc * font_ctx, LPWSTR line_start, WCHAR * out_remainder, int max_width_px, LPWSTR line_end, int * out_fitted_len)` | conf:high |
| `0x004b60f6` | `llm_ui_text_wrap_find_break_ansi` | `int llm_ui_text_wrap_find_break_ansi(llm_gfx_font_desc * font, char * text_ansi, WCHAR * arg3, int max_width, int text_end_ofs, int * out_count)` | conf:high |
| `0x004b6155` | `llm_gfx_font_desc_for_flags` | `void * llm_gfx_font_desc_for_flags(uint style_flags)` | conf:med |
| `0x004b6190` | `llm_gfx_rgb16_table_entry_update` | `void llm_gfx_rgb16_table_entry_update(undefined4 unused, int index)` | conf:med todo:globals |
| `0x004b61e7` | `llm_gfx_font_desc_init` | `void llm_gfx_font_desc_init(uint * glyph_desc_out, int glyph_index)` | conf:med |
| `0x004b637d` | `llm_snd_dsound_lazy_init` | `void llm_snd_dsound_lazy_init(void)` | conf:high todo:globals todo:struct |
| `0x004b6418` | `llm_ui_main_menu_activate` | `undefined llm_ui_main_menu_activate(void)` |  |
| `0x004b6472` | `llm_ui_text_extract_mnemonic_char` | `int llm_ui_text_extract_mnemonic_char(wchar_t * label)` | conf:high conf:low |
| `0x004b64c8` | `llm_ui_main_menu_screen_load` | `undefined llm_ui_main_menu_screen_load(void)` | todo:globals |
| `0x004b6ca2` | `llm_game_shutdown_cleanup` | `undefined llm_game_shutdown_cleanup(void)` | todo:globals |
| `0x004b6e9c` | `llm_snd_dsound_buffer_upload_and_play` | `void llm_snd_dsound_buffer_upload_and_play(void * pcm_desc, int param_2)` | conf:med todo:globals todo:struct |
| `0x004b6fb4` | `llm_ui_widget_list_center` | `void llm_ui_widget_list_center(astruct_3 * param_1)` |  |
| `0x004b70c6` | `llm_menu_widget_selection_reset` | `int llm_menu_widget_selection_reset(void)` | conf:med |
| `0x004b7149` | `llm_ui_widget_layout_resolve_position` | `void llm_ui_widget_layout_resolve_position(llm_ui_widget * widget, void * parent_rect)` | conf:med todo:globals todo:struct |
| `0x004b72d8` | `llm_ui_widget_list_draw_frame_layer` | `undefined llm_ui_widget_list_draw_frame_layer(llm_ui_widget_list * list)` |  |
| `0x004b7384` | `llm_ui_widget_list_draw` | `void llm_ui_widget_list_draw(llm_ui_widget_list * list)` |  |
| `0x004b73f9` | `llm_ui_widget_input_tick` | `int llm_ui_widget_input_tick(void)` | conf:med todo:globals |
| `0x004b7a11` | `llm_ui_menu_screen_save_config` | `undefined llm_ui_menu_screen_save_config(void)` |  |
| `0x004b7a87` | `llm_ui_menu_screen_restore_config` | `undefined llm_ui_menu_screen_restore_config(void)` |  |
| `0x004b7af7` | `llm_ui_menu_push_screen` | `undefined llm_ui_menu_push_screen(llm_ui_widget_list * list)` |  |
| `0x004b7b62` | `llm_ui_menu_pop_screen` | `undefined llm_ui_menu_pop_screen(void)` |  |
| `0x004b7b97` | `llm_ui_menu_widget_list_activate` | `void llm_ui_menu_widget_list_activate(llm_ui_widget_list * widget_list)` | conf:med todo:globals |
| `0x004b7bdf` | `llm_ui_menu_async_callback_pump` | `void llm_ui_menu_async_callback_pump(undefined4 arg1)` | conf:med todo:globals |
| `0x004b7c3b` | `llm_ui_menu_state_tick` | `void llm_ui_menu_state_tick(void)` | todo:globals |
| `0x004b7e98` | `llm_ui_menu_frame` | `undefined llm_ui_menu_frame(void)` |  |
| `0x004b7f5a` | `llm_ui_dlg_confirm_quit_to_desktop` | `undefined llm_ui_dlg_confirm_quit_to_desktop(void)` |  |
| `0x004b7f90` | `llm_menu_screen_close_finalize` | `int llm_menu_screen_close_finalize(void)` | todo:globals |
| `0x004b7ff2` | `llm_ui_menu_bg_redraw_cb` | `int llm_ui_menu_bg_redraw_cb(void)` |  |
| `0x004b8038` | `llm_map_view_size_mode_apply` | `void llm_map_view_size_mode_apply(void)` | conf:med |
| `0x004b8116` | `llm_menu_enter_gameplay` | `undefined llm_menu_enter_gameplay(void)` |  |
| `0x004b8245` | `llm_menu_finish_enter_gameplay` | `int llm_menu_finish_enter_gameplay(void)` |  |
| `0x004b82c1` | `llm_snd_apply_option_changes` | `void llm_snd_apply_option_changes(void)` | conf:high todo:globals |
| `0x004b8383` | `llm_ui_options_menu_open` | `int llm_ui_options_menu_open(void)` | conf:high |
| `0x004b845e` | `llm_menu_newgame_fade_done_cb` | `undefined llm_menu_newgame_fade_done_cb(void)` | conf:high |
| `0x004b849e` | `llm_menu_newgame_open` | `undefined llm_menu_newgame_open(void)` | conf:high |
| `0x004b84ff` | `llm_menu_campaign_start_and_enter` | `undefined llm_menu_campaign_start_and_enter(void)` |  |
| `0x004b857c` | `llm_menu_newgame_arm_start` | `undefined llm_menu_newgame_arm_start(void)` |  |
| `0x004b85cb` | `llm_ui_screen_fade_transition_swap` | `int llm_ui_screen_fade_transition_swap(void)` | conf:med todo:globals |
| `0x004b8618` | `llm_ui_options_menu_cancel` | `undefined llm_ui_options_menu_cancel(void)` | todo:proto |
| `0x004b8656` | `llm_ui_options_menu_confirm` | `int llm_ui_options_menu_confirm(void)` | conf:high |
| `0x004b86a4` | `llm_ui_options_menu_ok_action_cb` | `undefined llm_ui_options_menu_ok_action_cb(void)` | conf:high |
| `0x004b8709` | `llm_ui_widget_slide_tick` | `undefined llm_ui_widget_slide_tick(void)` | conf:high |
| `0x004b892e` | `llm_ui_dlg_quit_cancel_cb` | `undefined llm_ui_dlg_quit_cancel_cb(void)` |  |
| `0x004b8b26` | `llm_str_skip_eol` | `wchar_t * llm_str_skip_eol(wchar_t * * cursor)` | conf:med |
| `0x004b8be2` | `llm_ui_text_decode_escapes` | `undefined llm_ui_text_decode_escapes(void)` |  |
| `0x004b8cb1` | `llm_ui_text_reflow_line` | `wchar_t * llm_ui_text_reflow_line(wchar_t * src, wchar_t * dst)` | conf:med |
| `0x004b8de3` | `llm_ui_stepper_update_arrow_state` | `void llm_ui_stepper_update_arrow_state(int * stepper_state)` | conf:med todo:globals todo:struct |
| `0x004b8e52` | `llm_fs_scan_dir` | `undefined llm_fs_scan_dir(void)` |  |
| `0x004b9142` | `llm_ui_list_remove_entry` | `void llm_ui_list_remove_entry(void * list, int index)` | conf:med todo:struct |
| `0x004b922d` | `llm_ui_dialog_bg_redraw_cb` | `void llm_ui_dialog_bg_redraw_cb(void)` | conf:med |
| `0x004b9278` | `llm_menu_race_select_cb` | `undefined llm_menu_race_select_cb(void)` |  |
| `0x004b9313` | `llm_menu_racebck_exit_cb` | `undefined llm_menu_racebck_exit_cb(void)` | conf:high |
| `0x004b935c` | `llm_menu_race_select_draw_cb` | `undefined llm_menu_race_select_draw_cb(void)` | conf:high |
| `0x004b941e` | `llm_game_return_to_main_menu_cb` | `int llm_game_return_to_main_menu_cb(void)` |  |
| `0x004b9538` | `llm_ui_dlg_confirm_quit_to_menu` | `undefined llm_ui_dlg_confirm_quit_to_menu(void)` | todo:globals |
| `0x004b95f6` | `llm_ui_dlg_confirm_quit_to_menu_alt` | `undefined llm_ui_dlg_confirm_quit_to_menu_alt(void)` | todo:globals |
| `0x004b9640` | `llm_ui_hover_title_refresh` | `void llm_ui_hover_title_refresh(char * default_text)` | conf:med todo:struct |
| `0x004b96a0` | `llm_ui_text_viewer_draw` | `void llm_ui_text_viewer_draw(void * widget)` | conf:high todo:struct |
| `0x004b975f` | `llm_ui_text_viewer_fade_tick_cb` | `undefined llm_ui_text_viewer_fade_tick_cb(void)` | conf:high |
| `0x004b97f3` | `llm_ui_text_viewer_tick` | `int llm_ui_text_viewer_tick(void)` | conf:high todo:globals |
| `0x004b99a8` | `llm_ui_text_viewer_open` | `undefined llm_ui_text_viewer_open(void)` | conf:high |
| `0x004b9aff` | `llm_menu_show_credits` | `undefined llm_menu_show_credits(void)` | conf:high |
| `0x004b9b68` | `llm_ui_tutorial_hint_draw_cb` | `undefined llm_ui_tutorial_hint_draw_cb(void)` | conf:high |
| `0x004b9cc2` | `llm_tutorial_load_script` | `int llm_tutorial_load_script(void)` |  |
| `0x004ba683` | `llm_menu_tutorial_uistate_restore` | `void llm_menu_tutorial_uistate_restore(void)` | conf:high todo:globals |
| `0x004ba6fc` | `llm_strat_ai_unit_commit_attack_on_enemy_hq` | `void llm_strat_ai_unit_commit_attack_on_enemy_hq(void)` | conf:med |
| `0x004ba7e8` | `llm_strat_ai_start_hq_attack_scenario` | `void llm_strat_ai_start_hq_attack_scenario(void)` | conf:high |
| `0x004ba8af` | `llm_tutorial_step_driver` | `int llm_tutorial_step_driver(void)` |  |
| `0x004baf11` | `llm_tutorial_menu_redraw_cb` | `undefined llm_tutorial_menu_redraw_cb(void)` |  |
| `0x004baf42` | `llm_ui_tutorial_step_enter_cb` | `undefined llm_ui_tutorial_step_enter_cb(void)` | conf:high |
| `0x004bafb1` | `llm_game_start_tutorial` | `int llm_game_start_tutorial(void)` |  |
| `0x004bb2fa` | `llm_menu_ui_state_reset` | `int llm_menu_ui_state_reset(void)` | conf:med todo:globals |
| `0x004bb35f` | `llm_ui_menu_close_to_hud` | `int llm_ui_menu_close_to_hud(void)` | conf:low todo:globals |
| `0x004bb422` | `llm_menu_begin_enter_gameplay` | `int llm_menu_begin_enter_gameplay(void)` | conf:high todo:globals |
| `0x004bb4cd` | `llm_menu_enter_gameplay_screen_open` | `int llm_menu_enter_gameplay_screen_open(void)` | conf:med todo:globals |
| `0x004bb517` | `llm_ui_ingame_menu_open` | `int llm_ui_ingame_menu_open(void)` |  |
| `0x004bb60e` | `llm_menu_savegame_confirm` | `undefined llm_menu_savegame_confirm(void)` |  |
| `0x004bb6d3` | `llm_menu_savelist_select_name` | `undefined llm_menu_savelist_select_name(void)` |  |
| `0x004bb72f` | `llm_menu_loadgame_confirm` | `undefined llm_menu_loadgame_confirm(void)` |  |
| `0x004bb78d` | `llm_menu_loadgame_ok_action_cb` | `undefined llm_menu_loadgame_ok_action_cb(void)` | conf:high |
| `0x004bb83c` | `llm_menu_savegame_list_build` | `undefined llm_menu_savegame_list_build(void)` | conf:high |
| `0x004bb9a6` | `llm_ui_menu_dialog_open_handler` | `int llm_ui_menu_dialog_open_handler(void)` | conf:med todo:globals |
| `0x004bba65` | `llm_menu_load_game_open` | `undefined llm_menu_load_game_open(void)` | conf:high |
| `0x004bbafd` | `llm_menu_savename_screen_build` | `int llm_menu_savename_screen_build(void)` |  |
| `0x004bbb7b` | `llm_ui_dlg_confirm_delete_savegame` | `undefined llm_ui_dlg_confirm_delete_savegame(void)` |  |
| `0x004bbbba` | `llm_savegame_delete_confirm_cb` | `undefined llm_savegame_delete_confirm_cb(void)` |  |
| `0x004bbc8e` | `llm_ui_widget_set_text` | `void llm_ui_widget_set_text(llm_ui_widget * widget, LPWSTR text)` | todo:struct |
| `0x004bbd30` | `llm_ui_edit_field_modal_tick` | `int llm_ui_edit_field_modal_tick(void)` | conf:med todo:globals todo:struct |
| `0x004bc08a` | `llm_ui_list_widget_mouse_cb` | `undefined llm_ui_list_widget_mouse_cb(void)` | conf:high |
| `0x004bc1b6` | `llm_gfx_sprite_width` | `uint llm_gfx_sprite_width(int sprite_id)` | conf:high |
| `0x004bc203` | `llm_gfx_ui_sprite_get_header_field2` | `uint llm_gfx_ui_sprite_get_header_field2(int sprite_index)` | conf:low todo:struct |
| `0x004bc251` | `llm_menu_widget_slide_pump` | `int llm_menu_widget_slide_pump(int slide_param)` | conf:med todo:globals |
| `0x004bc30e` | `llm_ui_menu_screen_slide_transition` | `undefined llm_ui_menu_screen_slide_transition(void)` |  |
| `0x004bc3c1` | `llm_lobby_show_intro_wait` | `undefined llm_lobby_show_intro_wait(void)` |  |
| `0x004bc4af` | `llm_ui_screen_fade_transition_tick` | `int llm_ui_screen_fade_transition_tick(void)` |  |
| `0x004bc6aa` | `llm_ui_dlg_msgbox_ok` | `undefined llm_ui_dlg_msgbox_ok(void)` | todo:globals |
| `0x004bc6f0` | `llm_cfg_mru_list_write` | `undefined llm_cfg_mru_list_write(void)` |  |
| `0x004bc75f` | `llm_cfg_mru_list_read` | `undefined llm_cfg_mru_list_read(void)` |  |
| `0x004bc7ea` | `llm_cfg_setup_dat_write_mru_lists` | `undefined llm_cfg_setup_dat_write_mru_lists(void)` |  |
| `0x004bc869` | `llm_cfg_setup_dat_read_mru_lists` | `undefined llm_cfg_setup_dat_read_mru_lists(void)` |  |
| `0x004bc8e3` | `llm_ui_scrollbar_bind_range` | `void llm_ui_scrollbar_bind_range(void * scrollbar)` |  |
| `0x004bc972` | `llm_net_crc32` | `uint llm_net_crc32(byte * buf, int len)` |  |
| `0x004bc9f7` | `llm_net_send_packet` | `void llm_net_send_packet(int mode, int dest_player_id, byte * buf, int len)` |  |
| `0x004bca36` | `llm_net_poll_recv` | `undefined llm_net_poll_recv(void)` |  |
| `0x004bcacb` | `llm_net_send_broadcast` | `void llm_net_send_broadcast(undefined4 param_1, byte * param_2, int a2)` |  |
| `0x004bcb08` | `llm_lobby_network_setup_screen` | `undefined llm_lobby_network_setup_screen(void)` | todo:globals |
| `0x004bcbbe` | `llm_mp_discovery_browser_refresh` | `undefined llm_mp_discovery_browser_refresh(void)` |  |
| `0x004bcf9c` | `llm_mp_local_browser_setup` | `undefined llm_mp_local_browser_setup(void)` | conf:high todo:globals |
| `0x004bd03f` | `llm_mp_netsetup_name_confirm` | `undefined llm_mp_netsetup_name_confirm(void)` | conf:med |
| `0x004bd0a6` | `llm_lobby_network_setup_cancel_cb` | `undefined llm_lobby_network_setup_cancel_cb(void)` | conf:high |
| `0x004bd117` | `llm_mp_map_picker_open_action` | `undefined llm_mp_map_picker_open_action(void)` | conf:med |
| `0x004bd183` | `llm_mp_netsetup_game_field_cancel` | `undefined llm_mp_netsetup_game_field_cancel(void)` |  |
| `0x004bd1ec` | `llm_mp_netsetup_enter_game_name_screen` | `undefined llm_mp_netsetup_enter_game_name_screen(void)` |  |
| `0x004bd2a5` | `llm_mp_local_browser_cancel_to_main` | `undefined llm_mp_local_browser_cancel_to_main(void)` | conf:high |
| `0x004bd325` | `llm_mp_netsetup_field_get_mru_entry_wide` | `undefined llm_mp_netsetup_field_get_mru_entry_wide(void)` |  |
| `0x004bd3ee` | `llm_mp_netsetup_field_build_mru` | `undefined llm_mp_netsetup_field_build_mru(void)` |  |
| `0x004bd58f` | `llm_ui_mru_list_add` | `undefined llm_ui_mru_list_add(void)` |  |
| `0x004bd77a` | `llm_mp_connect_by_ip_action` | `undefined llm_mp_connect_by_ip_action(void)` | conf:med |
| `0x004bd805` | `llm_mp_netsetup_ip_field_cancel` | `undefined llm_mp_netsetup_ip_field_cancel(void)` |  |
| `0x004bd86f` | `llm_mp_netsetup_enter_connect_ip_screen` | `undefined llm_mp_netsetup_enter_connect_ip_screen(void)` |  |
| `0x004bd8e8` | `llm_mp_session_browser_enter` | `undefined llm_mp_session_browser_enter(void)` | conf:med |
| `0x004bd957` | `llm_mp_session_browser_back_to_local_list` | `undefined llm_mp_session_browser_back_to_local_list(void)` | conf:med |
| `0x004bd99c` | `llm_mp_create_game_action` | `undefined llm_mp_create_game_action(void)` |  |
| `0x004bda06` | `llm_mp_local_browser_close_and_refresh_cb` | `undefined llm_mp_local_browser_close_and_refresh_cb(void)` | conf:high todo:globals |
| `0x004bda50` | `llm_lobby_map_file_close` | `undefined llm_lobby_map_file_close(void)` | todo:globals |
| `0x004bda92` | `llm_ui_dlg_savegame_io_error` | `undefined llm_ui_dlg_savegame_io_error(void)` | todo:globals |
| `0x004bdb11` | `llm_mp_session_browser_rescan` | `undefined llm_mp_session_browser_rescan(void)` | conf:med |
| `0x004bdd7d` | `llm_lobby_map_send_tick` | `undefined llm_lobby_map_send_tick(void)` | todo:globals |
| `0x004bdf41` | `llm_lobby_map_transfer_write_chunk` | `undefined4 llm_lobby_map_transfer_write_chunk(undefined4 param_1)` | todo:globals |
| `0x004be081` | `llm_lobby_client_request_map` | `undefined llm_lobby_client_request_map(void)` |  |
| `0x004be0bc` | `llm_cfg_map_verify_version` | `int llm_cfg_map_verify_version(void)` | conf:high todo:globals todo:struct |
| `0x004be21d` | `llm_lobby_join_handler` | `undefined4 llm_lobby_join_handler(undefined4 param_1)` | conf:med |
| `0x004be303` | `llm_lobby_join_head` | `undefined llm_lobby_join_head(void)` | conf:med |
| `0x004be468` | `llm_lobby_kick_player_confirm_cb` | `undefined llm_lobby_kick_player_confirm_cb(void)` | todo:globals |
| `0x004be552` | `llm_ui_dlg_lobby_kick_confirm_trigger` | `undefined llm_ui_dlg_lobby_kick_confirm_trigger(void)` | todo:globals |
| `0x004be5c0` | `llm_lobby_host_new_game_start` | `undefined llm_lobby_host_new_game_start(void)` | conf:high |
| `0x004be683` | `llm_lobby_announce_line` | `void llm_lobby_announce_line(int arg1, void * fmt, void * src)` |  |
| `0x004be7b1` | `llm_lobby_chat_send_cb` | `undefined llm_lobby_chat_send_cb(void)` | conf:high |
| `0x004be8a7` | `llm_lobby_screen_open` | `undefined llm_lobby_screen_open(void)` | todo:globals |
| `0x004be9b2` | `llm_lobby_count_ai_slots` | `int llm_lobby_count_ai_slots(void)` | conf:med |
| `0x004bea15` | `llm_lobby_build_players_from_slots` | `void llm_lobby_build_players_from_slots(void)` |  |
| `0x004bea62` | `llm_lobby_build_players_step` | `void llm_lobby_build_players_step(void)` | conf:med |
| `0x004beab8` | `llm_lobby_build_players_tail_copy` | `undefined llm_lobby_build_players_tail_copy(void)` | conf:med todo:proto |
| `0x004beb33` | `llm_lobby_build_players_finish` | `undefined llm_lobby_build_players_finish(void)` | conf:med |
| `0x004bec08` | `llm_lobby_finalize_custom_map_and_sync` | `void llm_lobby_finalize_custom_map_and_sync(void)` | conf:med todo:globals |
| `0x004bece0` | `llm_menu_loading_countdown_tick` | `int llm_menu_loading_countdown_tick(void)` | conf:high todo:globals |
| `0x004bede3` | `llm_lobby_wait_dialog_open` | `undefined llm_lobby_wait_dialog_open(void)` | conf:med |
| `0x004bee94` | `llm_lobby_map_load_async_step` | `undefined llm_lobby_map_load_async_step(void)` |  |
| `0x004bef6e` | `llm_lobby_begin_map_load` | `undefined llm_lobby_begin_map_load(void)` | todo:globals |
| `0x004beff6` | `llm_ui_lobby_screen_widget_reset` | `undefined llm_ui_lobby_screen_widget_reset(void)` | conf:low todo:globals todo:proto |
| `0x004bf058` | `llm_lobby_host_start_game` | `undefined llm_lobby_host_start_game(void)` | todo:globals |
| `0x004bf0f1` | `llm_lobby_finalize_transfer_or_enter` | `undefined llm_lobby_finalize_transfer_or_enter(void)` | todo:globals |
| `0x004bf15c` | `llm_lobby_push_local_slot_state` | `void llm_lobby_push_local_slot_state(void)` |  |
| `0x004bf1d9` | `llm_lobby_slot_race_cycle_cb` | `int llm_lobby_slot_race_cycle_cb(void)` |  |
| `0x004bf292` | `llm_lobby_slot_color_cycle_cb` | `undefined llm_lobby_slot_color_cycle_cb(void)` | conf:high |
| `0x004bf348` | `llm_lobby_slot_for_player_fixed8` | `int llm_lobby_slot_for_player_fixed8(int player_id)` |  |
| `0x004bf3a7` | `llm_lobby_slot_compact` | `int llm_lobby_slot_compact(int slot_idx)` |  |
| `0x004bf3e5` | `llm_lobby_remove_player_slot` | `int llm_lobby_remove_player_slot(int player_id)` |  |
| `0x004bf43c` | `llm_lobby_slot_mark_occupied` | `void llm_lobby_slot_mark_occupied(int player_id, int slot_idx, byte status)` |  |
| `0x004bf47d` | `llm_lobby_slot_find_or_alloc` | `int llm_lobby_slot_find_or_alloc(int player_id, byte status)` |  |
| `0x004bf513` | `llm_lobby_peer_table_add_ai` | `llm_net_lobby_peer * llm_lobby_peer_table_add_ai(void)` |  |
| `0x004bf59f` | `llm_lobby_peer_slot_remove` | `void llm_lobby_peer_slot_remove(int player_id)` |  |
| `0x004bf621` | `llm_lobby_peer_table_clear` | `undefined llm_lobby_peer_table_clear(void)` |  |
| `0x004bf65d` | `llm_lobby_slot_cycle_state_cb` | `int llm_lobby_slot_cycle_state_cb(void)` | todo:globals |
| `0x004bf730` | `llm_lobby_slot_assign_color` | `uint llm_lobby_slot_assign_color(int slot_idx, int requested_color, int step)` |  |
| `0x004bf824` | `llm_lobby_slot_for_player` | `int llm_lobby_slot_for_player(int player_id)` |  |
| `0x004bf888` | `llm_lobby_slot_cursor_init` | `void llm_lobby_slot_cursor_init(llm_lobby_slot_cursor * ptr)` |  |
| `0x004bf906` | `llm_lobby_slot_cursor_advance` | `void llm_lobby_slot_cursor_advance(llm_lobby_slot_cursor * param_1)` |  |
| `0x004bf969` | `llm_lobby_slot_widget_list_clear` | `undefined llm_lobby_slot_widget_list_clear(void)` |  |
| `0x004bf99b` | `llm_lobby_build_slot_widgets` | `void llm_lobby_build_slot_widgets(void)` |  |
| `0x004bfcca` | `llm_lobby_idle_keepalive` | `undefined llm_lobby_idle_keepalive(void)` |  |
| `0x004bfd35` | `llm_lobby_host_net_dispatch` | `int llm_lobby_host_net_dispatch(void)` | todo:globals |
| `0x004c06a2` | `llm_lobby_clear_map_status_text` | `undefined llm_lobby_clear_map_status_text(void)` |  |
| `0x004c06e3` | `llm_lobby_map_transfer_finalize` | `void llm_lobby_map_transfer_finalize(void)` |  |
| `0x004c08bb` | `llm_lobby_apply_selected_session_map` | `undefined llm_lobby_apply_selected_session_map(void)` |  |
| `0x004c0a03` | `llm_mp_mappicker_add_mapfile` | `undefined llm_mp_mappicker_add_mapfile(void)` |  |
| `0x004c0a5c` | `llm_mp_mappicker_add_folder` | `undefined llm_mp_mappicker_add_folder(void)` |  |
| `0x004c0aca` | `llm_mp_mappicker_populate_list` | `undefined llm_mp_mappicker_populate_list(void)` |  |
| `0x004c0b76` | `llm_lobby_mappicker_selection_changed` | `void llm_lobby_mappicker_selection_changed(void)` | conf:high |
| `0x004c0ce0` | `llm_mp_map_picker_setup` | `undefined llm_mp_map_picker_setup(void)` | conf:med |
| `0x004c0d60` | `llm_mp_mappicker_proceed_cb` | `undefined llm_mp_mappicker_proceed_cb(void)` | conf:med |
| `0x004c0dd5` | `llm_mp_mappicker_activate_cb` | `undefined llm_mp_mappicker_activate_cb(void)` |  |
| `0x004c0eaa` | `llm_gfx_draw_bevel_rect` | `void llm_gfx_draw_bevel_rect(int * rect, ushort * bevel_colors, int border_grow)` | conf:high todo:globals todo:struct |
| `0x004c1084` | `llm_ui_widget_get_visual_state` | `undefined llm_ui_widget_get_visual_state(void)` | conf:high todo:proto |
| `0x004c1100` | `llm_ui_widget_draw` | `void llm_ui_widget_draw(llm_ui_widget * widget)` | todo:globals |
| `0x004c1585` | `llm_ui_list_widget_append_entry` | `void * llm_ui_list_widget_append_entry(void * list_widget, LPWSTR text, void * aux_filetime)` |  |
| `0x004c17a9` | `llm_ui_list_widget_clear` | `void llm_ui_list_widget_clear(void * list_widget)` |  |
| `0x004c18b0` | `llm_ui_list_widget_draw` | `void llm_ui_list_widget_draw(llm_ui_widget * widget)` | conf:high todo:struct |
| `0x004c1b15` | `llm_ui_widget_draw_content` | `void llm_ui_widget_draw_content(void * widget)` | conf:med todo:struct |
| `0x004c1d37` | `llm_ui_menu_pending_widget_option_advance` | `undefined llm_ui_menu_pending_widget_option_advance(void)` | conf:med todo:proto todo:struct |
| `0x004c1daa` | `llm_mp_browser_scrollbar_draw` | `undefined llm_mp_browser_scrollbar_draw(void)` |  |
| `0x004c1f67` | `llm_ui_widget_drag_modal_cb` | `undefined llm_ui_widget_drag_modal_cb(void)` | conf:high |
| `0x004c20ab` | `llm_gfx_clear_and_flip_both_buffers` | `void llm_gfx_clear_and_flip_both_buffers(void)` | conf:high |
| `0x004c2111` | `llm_ui_avi_audio_playback_loop` | `uint llm_ui_avi_audio_playback_loop(LPVOID lpParameter)` | conf:med todo:globals |
| `0x004c240c` | `llm_ui_menu_async_tick` | `undefined llm_ui_menu_async_tick(void)` |  |
| `0x004c29d8` | `llm_game_boot_init` | `int llm_game_boot_init(char * pack_name, char * file_name, int menu_bar_height, int mode_flag)` | todo:globals todo:struct |
| `0x004c312e` | `llm_menu_fade_to_main_menu_cb` | `undefined llm_menu_fade_to_main_menu_cb(void)` | conf:high |
| `0x004c316d` | `llm_menu_intro_avi_done_cb` | `undefined llm_menu_intro_avi_done_cb(void)` | conf:high |
| `0x004c31aa` | `llm_menu_play_intro_avi` | `undefined llm_menu_play_intro_avi(void)` | conf:high |
| `0x004c3216` | `llm_ui_avi_close_return_to_menu` | `int llm_ui_avi_close_return_to_menu(void)` | conf:med todo:globals |
| `0x004c33d1` | `llm_gfx_draw_boot_loadbar_frame` | `void llm_gfx_draw_boot_loadbar_frame(int stage, int total)` | conf:med |
| `0x004c34ee` | `llm_ui_dialog_message_show` | `int llm_ui_dialog_message_show(void)` | conf:med |
| `0x004c3680` | `llm_intro_frame` | `undefined llm_intro_frame(void)` |  |
| `0x004c371b` | `llm_cd_locate_and_open_audio` | `int llm_cd_locate_and_open_audio(void)` |  |
| `0x004c385c` | `llm_game_find_mh_cd_drive` | `int llm_game_find_mh_cd_drive(void)` | conf:high |
| `0x004c3921` | `llm_cd_ensure_present` | `int llm_cd_ensure_present(void)` |  |
| `0x004c3992` | `llm_wnd_on_devicechange` | `uint llm_wnd_on_devicechange(uint event_code, void * broadcast_hdr)` |  |
| `0x004c3aa0` | `llm_game_verify_cd_inserted` | `int llm_game_verify_cd_inserted(void)` | conf:high |
| `0x004c3afe` | `llm_cd_audio_advance_or_restart_track` | `undefined llm_cd_audio_advance_or_restart_track(void)` | todo:globals |
| `0x004c3bae` | `llm_cd_audio_on_mci_notify` | `int llm_cd_audio_on_mci_notify(uint notify_code, uint mci_id)` |  |
| `0x004c3c53` | `llm_boot_progress_draw` | `undefined llm_boot_progress_draw(void)` | todo:globals |
| `0x004c3e9e` | `llm_boot_apply_lang_variant_bytes` | `undefined llm_boot_apply_lang_variant_bytes(void)` | conf:low todo:globals todo:proto |
| `0x004c3fd2` | `llm_game_boot_async_thread_pump` | `int llm_game_boot_async_thread_pump(void * async_task)` | conf:high todo:struct |
| `0x004c40ba` | `llm_strat_planet_transition_perform_switch` | `undefined llm_strat_planet_transition_perform_switch(void)` |  |
| `0x004c4119` | `llm_strat_planet_transition_error_dismiss` | `undefined llm_strat_planet_transition_error_dismiss(void)` |  |
| `0x004c4165` | `llm_strat_planet_transition_tick` | `undefined llm_strat_planet_transition_tick(void)` | todo:globals |
| `0x004c4311` | `llm_ui_planet_select_cancel_cb` | `undefined llm_ui_planet_select_cancel_cb(void)` | conf:med |
| `0x004c43c1` | `llm_strat_ui_shipment_contents_dialog` | `void llm_strat_ui_shipment_contents_dialog(undefined4 ctx)` |  |
| `0x004c4699` | `llm_strat_ui_shuttle_colonist_status_list_build` | `undefined llm_strat_ui_shuttle_colonist_status_list_build(void)` | conf:med todo:globals todo:proto todo:struct |
| `0x004c48da` | `llm_strat_planet_sel_widget_list_reset` | `undefined llm_strat_planet_sel_widget_list_reset(void)` | conf:low todo:proto todo:struct |
| `0x004c4978` | `llm_ui_planet_select_frame_enter` | `undefined llm_ui_planet_select_frame_enter(void)` | conf:med |
| `0x004c49bf` | `llm_ui_widget_hover_anim_update` | `void llm_ui_widget_hover_anim_update(llm_ui_widget * widget)` | conf:med todo:globals |
| `0x004c4a9b` | `llm_ui_tooltip_draw_text_line` | `void llm_ui_tooltip_draw_text_line(ushort * text)` | conf:low todo:globals |
| `0x004c4b9c` | `llm_ui_tooltip_layout_init` | `void llm_ui_tooltip_layout_init(void)` | conf:high |
| `0x004c4c09` | `llm_strat_ui_shipment_list_item_draw` | `void llm_strat_ui_shipment_list_item_draw(llm_ui_widget * widget)` |  |
| `0x004c4f29` | `llm_gfx_build_rgb16_palette_lut` | `void llm_gfx_build_rgb16_palette_lut(ushort * out_rgb16_palette, byte * src_vga_palette)` | conf:high |
| `0x004c5054` | `llm_str_append_duration_mm_ss` | `void llm_str_append_duration_mm_ss(wchar_t * dst, int seconds)` |  |
| `0x004c50df` | `llm_str_append_duration_hh_mm` | `void llm_str_append_duration_hh_mm(wchar_t * dst, int seconds)` |  |
| `0x004c5178` | `llm_ui_render_planet_selector_tooltip` | `void llm_ui_render_planet_selector_tooltip(llm_ui_widget * widget)` | conf:med |
| `0x004c578e` | `llm_ui_planet_sel_widget_find` | `llm_ui_widget * llm_ui_planet_sel_widget_find(uint user_data)` | conf:med |
| `0x004c57e1` | `llm_ui_planet_sel_stack_icon_pos` | `void llm_ui_planet_sel_stack_icon_pos(int obj, int entry, int * out_xy)` | conf:med todo:struct |
| `0x004c5995` | `llm_ui_planet_sel_route_icon_place` | `undefined llm_ui_planet_sel_route_icon_place(void)` | conf:med todo:proto todo:struct |
| `0x004c5a78` | `llm_strat_prod_shuttle_calc_position` | `void llm_strat_prod_shuttle_calc_position(void * shuttle_obj)` | conf:med todo:struct |
| `0x004c5c11` | `llm_ui_dlg_shuttle_colonist_status_popup` | `undefined llm_ui_dlg_shuttle_colonist_status_popup(void)` | todo:globals |
| `0x004c5de5` | `llm_ui_planet_select_screen_build` | `undefined llm_ui_planet_select_screen_build(void)` | conf:high |
| `0x004c61a4` | `llm_ui_dlg_confirm_planet_travel` | `undefined llm_ui_dlg_confirm_planet_travel(void)` | todo:globals |
| `0x004c633c` | `llm_dlg_close_and_resume` | `int llm_dlg_close_and_resume(void)` | todo:globals |
| `0x004c6414` | `llm_menu_reset_widget_list_to_hud` | `int llm_menu_reset_widget_list_to_hud(void)` | conf:med |
| `0x004c6481` | `llm_menu_reset_and_enter_gameplay` | `int llm_menu_reset_and_enter_gameplay(void)` | conf:low |
| `0x004c64b7` | `llm_menu_campaign_start_confirm_cb` | `undefined llm_menu_campaign_start_confirm_cb(void)` | conf:low |
| `0x004c6506` | `llm_ui_outcome_report_close_to_hud` | `void llm_ui_outcome_report_close_to_hud(void)` | conf:med |
| `0x004c6649` | `llm_ui_outcome_dlg_tick` | `int llm_ui_outcome_dlg_tick(void)` | conf:high |
| `0x004c6866` | `llm_ui_outcome_stat_cell_add` | `void llm_ui_outcome_stat_cell_add(int * cell, int add)` |  |
| `0x004c68ab` | `llm_ui_dialog_redraw_cb` | `void llm_ui_dialog_redraw_cb(void)` | conf:low |
| `0x004c68dc` | `llm_ui_outcome_dlg_open` | `int llm_ui_outcome_dlg_open(void)` | conf:high |
| `0x004c6bc5` | `llm_ui_outcome_dlg_note_icon_action_cb` | `undefined llm_ui_outcome_dlg_note_icon_action_cb(void)` | conf:high |
| `0x004c6c4f` | `llm_ui_outcome_dialog` | `int llm_ui_outcome_dialog(byte outcome)` |  |
| `0x004c6fef` | `llm_ui_dlg_building_info_close_cb` | `undefined llm_ui_dlg_building_info_close_cb(void)` | todo:globals |
| `0x004c703b` | `llm_ui_building_finish_order_cb` | `undefined llm_ui_building_finish_order_cb(void)` | todo:globals |
| `0x004c707a` | `llm_ui_building_restart_construction_cb` | `undefined llm_ui_building_restart_construction_cb(void)` | todo:globals |
| `0x004c70b9` | `llm_ui_dlg_building_construction_status` | `int llm_ui_dlg_building_construction_status(void)` |  |
| `0x004c75d9` | `llm_ui_dlg_building_construction_status_open` | `void llm_ui_dlg_building_construction_status_open(int building_index)` | conf:high todo:globals |
| `0x004c767c` | `llm_cd_audio_open_and_init_volume` | `int llm_cd_audio_open_and_init_volume(void)` |  |
| `0x004c76fb` | `llm_cd_audio_shutdown` | `void llm_cd_audio_shutdown(void)` | conf:high todo:globals |
| `0x004c7736` | `llm_cd_audio_start_play` | `void llm_cd_audio_start_play(void)` |  |
| `0x004c777c` | `llm_cd_audio_stop` | `void llm_cd_audio_stop(void)` |  |
| `0x004c77ad` | `llm_snd_cd_stop` | `void llm_snd_cd_stop(void)` |  |
| `0x004c77de` | `llm_cd_audio_start_if_enabled` | `void llm_cd_audio_start_if_enabled(void)` | conf:high |
| `0x004c7822` | `llm_cd_audio_tick` | `void llm_cd_audio_tick(void)` | conf:high todo:globals |
| `0x004c78ce` | `llm_game_confirm_quit_to_desktop_cb` | `undefined llm_game_confirm_quit_to_desktop_cb(void)` |  |
| `0x004c7904` | `llm_ui_dlg_close_cb` | `int llm_ui_dlg_close_cb(void)` |  |
| `0x004c7941` | `llm_ui_dlg_build_from_table` | `void llm_ui_dlg_build_from_table(llm_ui_dlg_table * table)` |  |
| `0x004c7ad6` | `llm_ui_menu_widget_list_contains` | `int llm_ui_menu_widget_list_contains(llm_ui_widget * widget)` | conf:high |
| `0x004c7b33` | `llm_ui_widget_menu_request_set` | `int llm_ui_widget_menu_request_set(llm_ui_widget * widget)` | conf:med |
| `0x004c7b8a` | `llm_net_lockstep_sync_retry_rearm_cb` | `undefined llm_net_lockstep_sync_retry_rearm_cb(void)` | conf:high |
| `0x004c7c0a` | `llm_ui_menu_overlay_frame` | `undefined llm_ui_menu_overlay_frame(void)` | conf:high |
| `0x004c7d1c` | `llm_net_lockstep_overlay_dismiss` | `int llm_net_lockstep_overlay_dismiss(void)` | conf:high |
| `0x004c7dc0` | `llm_net_lockstep_wait_player_overlay_show` | `int llm_net_lockstep_wait_player_overlay_show(int player_idx)` | conf:high |
| `0x004c7eea` | `llm_net_lockstep_sync_overlay_show` | `int llm_net_lockstep_sync_overlay_show(void)` | conf:high |
| `0x004c7fc6` | `llm_ui_diplomacy_apply_and_resume` | `int llm_ui_diplomacy_apply_and_resume(void)` | conf:med |
| `0x004c811c` | `llm_ui_diplomacy_screen_build` | `int llm_ui_diplomacy_screen_build(void)` | conf:med |
| `0x004c85bd` | `llm_net_lockstep_extend_ui_enter` | `void llm_net_lockstep_extend_ui_enter(void)` |  |
| `0x004c85fe` | `llm_net_mp_leave_reset_game_mode` | `void llm_net_mp_leave_reset_game_mode(void)` | conf:med todo:globals |
| `0x004c862f` | `llm_menu_force_return_to_main` | `void llm_menu_force_return_to_main(void)` |  |
| `0x004c888f` | `llm_gfx_draw_markup_text` | `ushort * llm_gfx_draw_markup_text(ushort * text)` | conf:med todo:globals todo:struct |
| `0x004c8ae2` | `llm_gfx_draw_wrapped_text_line` | `ushort * llm_gfx_draw_wrapped_text_line(ushort * text, int wrap_width)` | conf:med todo:globals |
| `0x004c8e1a` | `llm_gfx_build_info_desc_pages` | `void llm_gfx_build_info_desc_pages(void * background_template, ushort * markup_text)` | conf:med todo:globals |
| `0x004c8f61` | `llm_ui_info_text_append` | `void llm_ui_info_text_append(LPCWSTR text)` |  |
| `0x004c8fa7` | `llm_ui_info_text_emit_stat` | `void llm_ui_info_text_emit_stat(int x_pos, int value_x_pos, int y_pos, LPCWSTR label_text, int count)` |  |
| `0x004c90c0` | `llm_ui_mainmenu_stats_summary_build` | `void llm_ui_mainmenu_stats_summary_build(void * stats)` | conf:med todo:struct |
| `0x004c927b` | `llm_ui_info_text_emit_resource_stats` | `void llm_ui_info_text_emit_resource_stats(void * resource)` | conf:high todo:globals todo:struct |
| `0x004c936e` | `llm_ui_info_text_emit_labeled_stat` | `void llm_ui_info_text_emit_labeled_stat(LPCWSTR label_text, int value, byte desc_marker)` |  |
| `0x004c942a` | `llm_ui_info_screen_list_draw_columns` | `void llm_ui_info_screen_list_draw_columns(void)` | conf:med todo:struct |
| `0x004c94f7` | `llm_ui_info_screen_draw_stats` | `void llm_ui_info_screen_draw_stats(void)` | conf:med todo:struct |
| `0x004c95d3` | `llm_ui_info_stat_line_draw` | `void llm_ui_info_stat_line_draw(uint color)` | conf:high |
| `0x004c9637` | `llm_gfx_font_center_text_x` | `int llm_gfx_font_center_text_x(ushort * text, int container_width)` | conf:high |
| `0x004c9679` | `llm_ui_info_screen_stat_lines_draw` | `void llm_ui_info_screen_stat_lines_draw(void)` | conf:med |
| `0x004c9754` | `llm_ui_info_stat_row_emit` | `void llm_ui_info_stat_row_emit(int stat_label, int stat_value, int show_extra)` | conf:low todo:globals |
| `0x004c97e4` | `llm_ui_bldg_info_panel_populate_stat_line` | `void llm_ui_bldg_info_panel_populate_stat_line(void)` | conf:high |
| `0x004c9b90` | `llm_ui_info_panel_weapon_stats_draw` | `void llm_ui_info_panel_weapon_stats_draw(void)` | conf:high |
| `0x004c9d2a` | `llm_ui_info_screen_stats_draw` | `void llm_ui_info_screen_stats_draw(void)` | conf:high todo:globals |
| `0x004c9f0a` | `llm_ui_info_text_build` | `LPWSTR llm_ui_info_text_build(undefined4 entity_id, undefined4 kind, LPWSTR extra_desc_text)` |  |
| `0x004ca853` | `llm_ui_entity_info_screen_close` | `undefined llm_ui_entity_info_screen_close(void)` |  |
| `0x004caa46` | `llm_ui_info_media_frame_tick` | `undefined llm_ui_info_media_frame_tick(void)` | todo:proto |
| `0x004cabba` | `llm_ui_paged_list_frame` | `void llm_ui_paged_list_frame(void)` |  |
| `0x004cac59` | `llm_game_enter_menu_mode` | `void llm_game_enter_menu_mode(void)` | conf:med |
| `0x004cac85` | `llm_ui_info_media_draw_p1` | `void llm_ui_info_media_draw_p1(void)` | conf:med todo:struct |
| `0x004cacc5` | `llm_ui_info_media_draw_p2` | `void llm_ui_info_media_draw_p2(void)` | conf:med todo:struct |
| `0x004cad05` | `llm_menu_build_placement_pending_clear` | `void llm_menu_build_placement_pending_clear(void)` | conf:high |
| `0x004cad31` | `llm_ui_entity_info_screen_open` | `void llm_ui_entity_info_screen_open(int entity_id, llm_ui_info_kind kind)` |  |
| `0x004cb3f2` | `llm_ui_planet_select_screen_open` | `int llm_ui_planet_select_screen_open(int open_arg)` | conf:high conf:med |
| `0x004cb45a` | `llm_ui_demo_slideshow_tick` | `int llm_ui_demo_slideshow_tick(void)` | conf:med todo:globals todo:struct |
| `0x004cb5ad` | `llm_ui_info_text_track_cursor` | `undefined llm_ui_info_text_track_cursor(void)` |  |
| `0x004cb7fa` | `llm_lobby_ensure_unique_slot_name` | `void llm_lobby_ensure_unique_slot_name(int slot_idx)` | conf:high todo:globals todo:struct |
| `0x004cb91e` | `llm_input_key_dequeue_translate_ascii` | `void llm_input_key_dequeue_translate_ascii(uint * key_event)` | conf:high todo:globals todo:struct |
| `0x004cb9e9` | `llm_input_key_queue_flush` | `void llm_input_key_queue_flush(void)` | conf:high |
| `0x004cba1e` | `llm_strat_pathfind_route_leg_group_and_sort` | `int llm_strat_pathfind_route_leg_group_and_sort(uint leg_count, int ref_x, int ref_y)` | conf:med |
| `0x004cbc28` | `llm_strat_claim_free_slots_within_dist` | `void llm_strat_claim_free_slots_within_dist(int record_count, int max_dist, int claim_value, int origin_index)` | conf:med |
| `0x004cbc9b` | `llm_strat_dir_step_toroidal_dist` | `int llm_strat_dir_step_toroidal_dist(int a_index, int b_index)` | conf:high |
| `0x004cbd63` | `llm_strat_slot_dist_to_ref` | `int llm_strat_slot_dist_to_ref(int slot_index)` | conf:med |
| `0x004cbe7a` | `llm_snd_mixer_check_error` | `int llm_snd_mixer_check_error(uint mm_error_code)` | conf:high |
| `0x004cbf8f` | `llm_snd_mixer_find_volume_control` | `uint llm_snd_mixer_find_volume_control(void)` | conf:high todo:globals todo:struct |
| `0x004cc10f` | `llm_snd_mixer_close` | `void llm_snd_mixer_close(void)` | conf:high todo:globals |
| `0x004cc147` | `llm_cd_audio_set_volume` | `void llm_cd_audio_set_volume(int volume_raw)` |  |
| `0x004cc198` | `llm_snd_mixer_get_control_value` | `int llm_snd_mixer_get_control_value(void)` | conf:med todo:globals todo:struct |
| `0x004cc1e9` | `llm_mci_check_result` | `void llm_mci_check_result(int mci_result)` |  |
| `0x004cc20e` | `llm_cd_audio_select_and_open_device` | `int llm_cd_audio_select_and_open_device(int device_match_id, HWND notify_hwnd)` | conf:med todo:globals |
| `0x004cc3a5` | `llm_cd_audio_close_device` | `void llm_cd_audio_close_device(void)` | conf:high todo:globals |
| `0x004cc407` | `llm_cd_scan_audio_track_range` | `void llm_cd_scan_audio_track_range(void)` | conf:high todo:globals |
| `0x004cc4f1` | `llm_cd_send_pause_command` | `void llm_cd_send_pause_command(void)` | conf:high |
| `0x004cc536` | `llm_cd_audio_send_play_command` | `void llm_cd_audio_send_play_command(int from_track, int to_track)` |  |
| `0x004cc5c4` | `llm_cd_send_stop_command` | `void llm_cd_send_stop_command(void)` | conf:high |
| `0x004cc609` | `llm_cd_audio_query_mode_and_position` | `int llm_cd_audio_query_mode_and_position(void)` |  |
| `0x004cc72a` | `llm_cd_query_track_length` | `int llm_cd_query_track_length(int track_index)` | conf:low todo:globals |
| `0x004cc7b1` | `llm_ui_avi_ic_send_msg_0x403e` | `undefined llm_ui_avi_ic_send_msg_0x403e(void)` | conf:high todo:enum todo:proto todo:struct |
| `0x004cc839` | `llm_ui_avi_ic_send_msg_0x403c` | `undefined llm_ui_avi_ic_send_msg_0x403c(void)` | conf:high todo:proto todo:struct |
| `0x004cc8c1` | `llm_ui_avi_ic_send_msg_0x403d` | `undefined llm_ui_avi_ic_send_msg_0x403d(void)` | conf:high todo:proto todo:struct |
| `0x004cc949` | `llm_ui_avi_ic_send_msg_0x4032` | `undefined llm_ui_avi_ic_send_msg_0x4032(void)` | conf:high todo:proto todo:struct |
| `0x004cc9ad` | `llm_ui_avi_ic_send_msg_0x4048` | `undefined llm_ui_avi_ic_send_msg_0x4048(void)` | conf:high todo:proto todo:struct |
| `0x004cc9f9` | `llm_ui_acm_result_dispatch` | `void llm_ui_acm_result_dispatch(void)` | conf:med todo:struct |
| `0x004ccc03` | `llm_ui_avi_icm_result_to_string` | `void llm_ui_avi_icm_result_to_string(int result)` | conf:high |
| `0x004ccddd` | `llm_ui_avi_check_result` | `undefined llm_ui_avi_check_result(void)` |  |
| `0x004cd027` | `llm_ui_avi_shutdown` | `void llm_ui_avi_shutdown(void)` | conf:high |
| `0x004cd04e` | `llm_ui_avi_open` | `int llm_ui_avi_open(void * ctx, char * filename)` |  |
| `0x004cd0a9` | `llm_ui_avi_stream_end` | `undefined llm_ui_avi_stream_end(void)` |  |
| `0x004cd132` | `llm_ui_avi_close` | `undefined llm_ui_avi_close(void)` |  |
| `0x004cd19a` | `llm_ui_avi_stream_begin` | `undefined llm_ui_avi_stream_begin(void)` |  |
| `0x004cd2dc` | `llm_ui_avi_open_stream` | `int llm_ui_avi_open_stream(void * ctx, uint fourcc_type, int stream_index)` |  |
| `0x004cd388` | `llm_ui_avi_init_codecs` | `int llm_ui_avi_init_codecs(void * ctx)` |  |
| `0x004cd686` | `llm_ui_avi_close_codecs` | `undefined llm_ui_avi_close_codecs(void)` |  |
| `0x004cd722` | `llm_ui_avi_skip_samples` | `void llm_ui_avi_skip_samples(void * ctx, int count)` |  |
| `0x004cd792` | `llm_ui_avi_decode_audio_chunk` | `int llm_ui_avi_decode_audio_chunk(void * avi_ctx)` | conf:high todo:struct |
| `0x004cdae6` | `llm_ui_avi_decode_frame` | `int llm_ui_avi_decode_frame(void * ctx, int time_arg)` |  |
| `0x004cdd57` | `llm_snd_wav_open_and_read_fmt` | `MMRESULT llm_snd_wav_open_and_read_fmt(char * path_or_membuf, int membuf_size, HMMIO * out_hmmio, void * out_format, LPMMCKINFO out_riff_chunk)` | conf:high |
| `0x004cdf2e` | `llm_snd_wav_descend_data_chunk` | `MMRESULT llm_snd_wav_descend_data_chunk(HMMIO * mmio_handle_ptr, LPMMCKINFO out_data_chunk, MMCKINFO * parent_chunk)` | conf:med |
| `0x004cdfb1` | `llm_snd_wav_read_chunk_bytes` | `MMRESULT llm_snd_wav_read_chunk_bytes(HMMIO hmmio, uint requested_bytes, void * out_buffer, LPMMCKINFO chunk_info, uint * out_bytes_read)` | conf:high |
| `0x004ce0ce` | `llm_snd_wav_load` | `MMRESULT llm_snd_wav_load(char * path_or_membuf, int membuf_size, uint * out_data_size, void * out_format, void * * out_data_ptr)` | conf:high |
| `0x004ce1cf` | `llm_net_get_local_ip_string` | `undefined llm_net_get_local_ip_string(void)` |  |
| `0x004ce317` | `llm_cd_read_track_toc` | `int llm_cd_read_track_toc(MCIDEVICEID mci_device_id, byte * out_track_toc)` | conf:high todo:struct |
| `0x004ce501` | `llm_game_log_protect_message` | `void llm_game_log_protect_message(void)` | conf:med todo:globals |
| `0x004ce633` | `llm_media_diag_read_gfx_resource_records` | `void llm_media_diag_read_gfx_resource_records(char * resource_name, void * out_records)` | conf:med todo:globals |
| `0x004ce728` | `llm_build_media_diag_report` | `void * llm_build_media_diag_report(void)` | conf:low todo:globals |
| `0x004ce80a` | `llm_cd_write_cdcheck_log` | `void llm_cd_write_cdcheck_log(int cdcheck_resource_id, char * detected_value)` | conf:med todo:globals |
| `0x004cea04` | `llm_file_find_byte_pattern` | `uint llm_file_find_byte_pattern(HANDLE file_handle, byte * needle, uint needle_len)` | conf:med |
| `0x004ceb28` | `llm_file_crc32_range_checksum` | `uint llm_file_crc32_range_checksum(HANDLE file_handle, int start_percent, int end_percent, int skip_abs_offset, int * out_skip_rel_offset)` | conf:med |
| `0x004cecd6` | `llm_lzw_compress_block_with_header` | `void llm_lzw_compress_block_with_header(undefined4 src, void * dst, undefined4 size_)` | rev:verified |
| `0x004cef4a` | `llm_rsr_close_all_files` | `void llm_rsr_close_all_files(void)` | conf:high |
| `0x004cf379` | `llm_str_ansi_to_wide` | `wchar_t * llm_str_ansi_to_wide(wchar_t * dst, char * src)` | conf:med |
| `0x004cf3e0` | `llm_str_ansi_to_wide_scratch` | `wchar_t * llm_str_ansi_to_wide_scratch(char * src)` | conf:med |
| `0x004cf4e1` | `llm_crt_vsprintf` | `void llm_crt_vsprintf(char * dst, char * format, pointer args_ptr)` |  |
| `0x004d0350` | `llm_game_clock_pause` | `undefined llm_game_clock_pause(void)` | todo:globals |
| `0x004d036c` | `llm_game_clock_resume` | `undefined llm_game_clock_resume(void)` | todo:globals |
| `0x004d03c4` | `llm_game_clock_tick_update` | `void llm_game_clock_tick_update(void)` | conf:high |
| `0x004d0480` | `llm_game_clock_reset` | `void llm_game_clock_reset(uint ticks_per_second)` | conf:high |
| `0x004d056c` | `llm_time_get_ticks_ms` | `uint llm_time_get_ticks_ms(void)` | conf:high |
| `0x004d0810` | `llm_wnd_is_fullscreen` | `int llm_wnd_is_fullscreen(HWND hwnd)` | conf:high |
| `0x004d0894` | `llm_input_dinput_acquire` | `void llm_input_dinput_acquire(void)` | conf:high |
| `0x004d0904` | `llm_input_dinput_release` | `void llm_input_dinput_release(void)` | conf:high |
| `0x004d0948` | `llm_input_dinput_keyboard_init` | `void llm_input_dinput_keyboard_init(void)` | conf:high |
| `0x004d0a48` | `llm_input_di_keyboard_poll` | `undefined llm_input_di_keyboard_poll(void)` |  |
| `0x004d0b88` | `llm_input_di_mouse_create` | `void llm_input_di_mouse_create(undefined4 param1_unused, HWND hwnd)` | conf:high |
| `0x004d0cc4` | `llm_input_di_mouse_poll` | `undefined llm_input_di_mouse_poll(void)` |  |
| `0x004d0fb8` | `llm_input_keystate_reset` | `int llm_input_keystate_reset(void)` | conf:high |
| `0x004d1008` | `llm_input_di_keyboard_destroy` | `void llm_input_di_keyboard_destroy(void)` | conf:high |
| `0x004d1018` | `llm_input_key_queue_empty` | `int llm_input_key_queue_empty(void)` |  |
| `0x004d1030` | `llm_input_key_dequeue` | `void llm_input_key_dequeue(undefined4 param_1)` |  |
| `0x004d1070` | `llm_input_mouse_init` | `int llm_input_mouse_init(uint screen_width, uint screen_height)` | conf:high todo:globals |
| `0x004d10d0` | `llm_input_dimouse_shutdown` | `void llm_input_dimouse_shutdown(void)` | conf:high |
| `0x004d10e0` | `llm_input_mouse_queue_is_empty` | `int llm_input_mouse_queue_is_empty(void)` | conf:high |
| `0x004d10f8` | `llm_input_mouse_pop_event` | `void llm_input_mouse_pop_event(llm_input_mouse_event * out_event)` | conf:high |
| `0x004d1194` | `llm_input_wndproc_tap` | `void llm_input_wndproc_tap(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)` | todo:globals |
| `0x004d19f0` | `llm_snd_create_notify_window` | `int llm_snd_create_notify_window(void)` | conf:high todo:globals |
| `0x004d1a88` | `llm_snd_destroy_sound_window` | `void llm_snd_destroy_sound_window(void)` | conf:high todo:globals |
| `0x004d1ab8` | `llm_snd_waveout_channel_and_buffer_init` | `void llm_snd_waveout_channel_and_buffer_init(void)` | conf:low todo:globals todo:struct |
| `0x004d1bb4` | `llm_snd_dsound_backend_init` | `int llm_snd_dsound_backend_init(void)` | conf:high todo:globals todo:struct |
| `0x004d1f60` | `llm_snd_dsound_backend_release` | `void llm_snd_dsound_backend_release(void)` | conf:high todo:struct |
| `0x004d2008` | `llm_snd_waveout_backend_init` | `void llm_snd_waveout_backend_init(void)` | conf:high todo:globals todo:struct |
| `0x004d2268` | `llm_snd_waveout_backend_teardown` | `void llm_snd_waveout_backend_teardown(void)` | conf:high todo:globals |
| `0x004d2388` | `llm_snd_backend_startup_dispatch` | `void llm_snd_backend_startup_dispatch(int cfg_entry_count, undefined4 param2, HWND notify_hwnd, undefined4 param4)` | conf:med todo:globals |
| `0x004d2404` | `llm_snd_backend_teardown_dispatch` | `void llm_snd_backend_teardown_dispatch(void)` | conf:high todo:globals |
| `0x004d2478` | `llm_snd_voice_slot_register` | `int llm_snd_voice_slot_register(void * sample_key, char * pcm_data, int data_len, undefined4 field_20_val, undefined4 field_24_val, undefined4 field_28_val)` | conf:med todo:globals todo:struct |
| `0x004d25cc` | `llm_snd_release_voice_slot` | `void llm_snd_release_voice_slot(int slot_index)` | conf:high todo:globals todo:struct |
| `0x004d2658` | `llm_snd_stop_voice_playing_cfg` | `void llm_snd_stop_voice_playing_cfg(void * cfg)` | conf:high todo:struct |
| `0x004d26b8` | `llm_snd_release_channel_voice` | `void llm_snd_release_channel_voice(uint channel_index)` | conf:med todo:globals todo:struct |
| `0x004d276c` | `llm_snd_channel_configure_voice_and_play` | `undefined llm_snd_channel_configure_voice_and_play(void)` | conf:low todo:globals todo:proto todo:struct |
| `0x004d2b44` | `llm_snd_play_matching_sample` | `void llm_snd_play_matching_sample(uint channel_index, llm_snd_cfg_entry * cfg, int volume, int pan, int note_or_rate)` |  |
| `0x004d2d14` | `llm_snd_channel_get_play_state` | `undefined llm_snd_channel_get_play_state(void)` | conf:med todo:proto todo:struct |
| `0x004d2eac` | `llm_snd_dsound_negotiate_mixer_format` | `void llm_snd_dsound_negotiate_mixer_format(void)` | conf:med todo:globals |
| `0x004d32aa` | `llm_strat_unit_estimate_weapon_damage` | `uint llm_strat_unit_estimate_weapon_damage(int param_1, int param_2, uint a2, int param_4)` |  |
| `0x004d3437` | `llm_strat_ai_unit_squad_firepower_value` | `uint llm_strat_ai_unit_squad_firepower_value(int player, int unit_idx)` | conf:med |
| `0x004d352c` | `llm_strat_ai_score_reinforcement_unit` | `uint llm_strat_ai_score_reinforcement_unit(int player, int unit_proto_id)` |  |
| `0x004d3608` | `llm_strat_bldg_find_by_type_for_player` | `int llm_strat_bldg_find_by_type_for_player(int player_idx, uint building_type)` | conf:med |
| `0x004d3666` | `llm_strat_bldg_find_by_ai_build_for_player` | `uint llm_strat_bldg_find_by_ai_build_for_player(int player, uint ai_build_id)` | conf:med |
| `0x004d36c4` | `llm_strat_bldg_find_by_ai_build_and_type` | `uint llm_strat_bldg_find_by_ai_build_and_type(int unused, uint ai_build_id, uint building_type)` | conf:high |
| `0x004d370a` | `llm_strat_ai_bldg_count_by_category` | `int llm_strat_ai_bldg_count_by_category(int player, uint category)` |  |
| `0x004d37f2` | `llm_strat_bldg_count_by_type` | `int llm_strat_bldg_count_by_type(int player, int bldg_type)` |  |
| `0x004d388e` | `llm_strat_bldg_count_by_id` | `int llm_strat_bldg_count_by_id(int player, int building_id)` |  |
| `0x004d3917` | `llm_bldg_count_owned_matching_ids` | `int llm_bldg_count_owned_matching_ids(int player_idx, uint building_id_a, uint building_id_b, uint building_id_c)` | conf:med |
| `0x004d39bb` | `llm_strat_ai_bldg_queue_has_type` | `int llm_strat_ai_bldg_queue_has_type(int player_idx, uint ai_build_type)` | conf:med todo:dead |
| `0x004d3a27` | `llm_strat_ai_bldg_type_queue_has_pending` | `int llm_strat_ai_bldg_type_queue_has_pending(int player_idx, uint bldg_type)` |  |
| `0x004d3a93` | `llm_strat_ai_bldg_type_already_queued` | `int llm_strat_ai_bldg_type_already_queued(int player, uint building_type)` | conf:high |
| `0x004d3af2` | `llm_strat_ai_group_has_split_group_link` | `int llm_strat_ai_group_has_split_group_link(int player_id, uint target_group_id)` | conf:med |
| `0x004d3b55` | `llm_strat_dock_slot_is_busy` | `int llm_strat_dock_slot_is_busy(int player, int slot)` | conf:med |
| `0x004d3cf0` | `llm_strat_ai_target_ref_has_engageable_weapon` | `int llm_strat_ai_target_ref_has_engageable_weapon(int target_ref_kind, int target_ref_index, uint attacker_weapon_flags)` | conf:med |
| `0x004d3d45` | `llm_strat_bldg_is_alive` | `int llm_strat_bldg_is_alive(int player, int building_index)` |  |
| `0x004d3dab` | `llm_bldg_get_name_text` | `char * llm_bldg_get_name_text(uint bldg_idx)` | conf:high |
| `0x004d3e16` | `llm_bldg_get_unit_name_text` | `char * llm_bldg_get_unit_name_text(uint bldg_idx)` | conf:med |
| `0x004d3e61` | `llm_strat_bldg_max_defense_radius_sq` | `uint llm_strat_bldg_max_defense_radius_sq(int player, int x, int y)` | conf:med |
| `0x004d4063` | `llm_rand_below_ai` | `int llm_rand_below_ai(uint range)` |  |
| `0x004d4082` | `llm_strat_ai_target_ref_is_alive` | `int llm_strat_ai_target_ref_is_alive(uint target_ref_packed, int target_index)` |  |
| `0x004d4132` | `llm_unit_state_is_busy` | `int llm_unit_state_is_busy(int player_index, int unit_index)` | conf:med |
| `0x004d4158` | `llm_strat_unit_order_state_is_settled` | `int llm_strat_unit_order_state_is_settled(int player, int unit_idx)` | conf:med |
| `0x004d41ff` | `llm_strat_ai_unit_is_order_pending` | `uint llm_strat_ai_unit_is_order_pending(game_t_Player player, map_t_unit_id unit_id)` |  |
| `0x004d4226` | `llm_strat_unit_attack_target_is_dead` | `int llm_strat_unit_attack_target_is_dead(int player, int unit_index)` |  |
| `0x004d431e` | `llm_strat_unit_state_is_in_transit` | `int llm_strat_unit_state_is_in_transit(game_t_Player player, map_t_unit_id unit_id)` | conf:high |
| `0x004d43d4` | `llm_strat_unit_state_is_in_storage_transit` | `int llm_strat_unit_state_is_in_storage_transit(game_t_Player player, map_t_unit_id unit_id)` | conf:high |
| `0x004d4452` | `llm_strat_unit_is_idle_or_parked` | `int llm_strat_unit_is_idle_or_parked(int player, int unit_index)` | conf:med |
| `0x004d44d0` | `llm_strat_bldg_find_mother_position_indexed` | `undefined4 llm_strat_bldg_find_mother_position_indexed(int param_1, uint * param_2, uint * a2)` |  |
| `0x004d45c0` | `llm_strat_unit_is_aircraft` | `int llm_strat_unit_is_aircraft(uint unit_ref, int unit_index)` |  |
| `0x004d46f0` | `llm_tact_unit_is_tile_heli_transport` | `int llm_tact_unit_is_tile_heli_transport(uint packed_floor_and_flags, int col_index)` | conf:high |
| `0x004d480f` | `llm_strat_unit_max_weapon_range` | `uint llm_strat_unit_max_weapon_range(uint unit_ref, int unit_index)` |  |
| `0x004d4888` | `llm_strat_unit_get_sight` | `uint llm_strat_unit_get_sight(uint unit_ref, int unit_index)` |  |
| `0x004d48c8` | `llm_strat_ai_unit_in_group` | `bool llm_strat_ai_unit_in_group(int player_idx, uint group_id, int unit_idx)` | conf:high todo:dead |
| `0x004d48fa` | `llm_strat_unit_get_ai_group_index` | `uint llm_strat_unit_get_ai_group_index(int param_1, int param_2)` |  |
| `0x004d4919` | `llm_strat_ai_group_member_unlink` | `void llm_strat_ai_group_member_unlink(game_t_Player player, int ai_group_index, map_t_unit_id unit_id)` |  |
| `0x004d4a2e` | `llm_strat_ai_group_member_link` | `void llm_strat_ai_group_member_link(game_t_Player player, int ai_group_index, map_t_unit_id unit_id)` |  |
| `0x004d4af5` | `llm_strat_ai_group_member_move` | `void llm_strat_ai_group_member_move(game_t_Player player, int src_group, int dst_group, int unit_id)` | conf:high |
| `0x004d4b25` | `llm_strat_ai_group_task_preempt` | `void llm_strat_ai_group_task_preempt(int param_1, int param_2, short a2, int param_4, int param_5, int param_6, int param_7, int param_8, short param_9)` |  |
| `0x004d4c20` | `llm_strat_ai_group_task_enqueue` | `void llm_strat_ai_group_task_enqueue(int param_1, int param_2, undefined2 a2, undefined4 param_4, undefined4 param_5, undefined4 param_6, undefined4 param_7, undefined4 param_8, undefined2 param_9)` |  |
| `0x004d4cd3` | `llm_strat_ai_group_task_dequeue` | `void llm_strat_ai_group_task_dequeue(int player_id, int group_index)` |  |
| `0x004d4d85` | `llm_strat_ai_group_create` | `int llm_strat_ai_group_create(int player_id)` |  |
| `0x004d4e6b` | `llm_strat_ai_group_split_half` | `void llm_strat_ai_group_split_half(game_t_Player player, int src_group_index)` | conf:med todo:dead |
| `0x004d4f16` | `llm_strat_ai_group_remove` | `void llm_strat_ai_group_remove(int player, uint group_index)` | conf:high |
| `0x004d50b3` | `llm_strat_ai_engage_candidate_add` | `void llm_strat_ai_engage_candidate_add(uint target_ref, int target_index)` |  |
| `0x004d5245` | `llm_strat_ai_engage_rank_candidates_by_range` | `void llm_strat_ai_engage_rank_candidates_by_range(uint source_ref, int source_index)` | todo:dead |
| `0x004d53ae` | `llm_strat_ai_engage_sort_candidates_by_dist` | `void llm_strat_ai_engage_sort_candidates_by_dist(uint source_ref, int source_index, int inherited_sorted_flag)` |  |
| `0x004d552b` | `llm_strat_ai_engage_partition_turret_candidates` | `void llm_strat_ai_engage_partition_turret_candidates(void)` |  |
| `0x004d55ef` | `llm_strat_ai_commit_attack_order` | `void llm_strat_ai_commit_attack_order(uint param_1, int param_2, uint a2, int param_4)` |  |
| `0x004d5708` | `llm_strat_ai_commit_attack_order_alt` | `void llm_strat_ai_commit_attack_order_alt(uint param_1, int param_2, uint a2, int param_4)` |  |
| `0x004d5801` | `llm_strat_unit_order_attack_dispatch` | `void llm_strat_unit_order_attack_dispatch(uint player_raw, int target_id, uint target_flags, int order_arg)` | conf:high todo:dead |
| `0x004d585e` | `llm_strat_ai_unit_group_assign_by_type` | `void llm_strat_ai_unit_group_assign_by_type(game_t_Player player, int group_index, int unit_id, int exit_param)` | conf:med todo:enum |
| `0x004d58e7` | `llm_strat_ai_unit_launch_from_storage_enqueue` | `void llm_strat_ai_unit_launch_from_storage_enqueue(byte player, int unit_id, uint target_x, uint target_y)` | conf:high |
| `0x004d590e` | `llm_strat_ai_route_unit_to_home_storage` | `void llm_strat_ai_route_unit_to_home_storage(uint player, int unit_id)` | conf:med |
| `0x004d5c17` | `llm_strat_ai_group_compute_centroid` | `void llm_strat_ai_group_compute_centroid(int player, int group_index, uint * out_x, uint * out_y)` | conf:high |
| `0x004d5dfc` | `llm_strat_ai_group_rally_formup_worker` | `void llm_strat_ai_group_rally_formup_worker(int player_id, int group_index)` |  |
| `0x004d5eaf` | `llm_strat_ai_random_point_near` | `void llm_strat_ai_random_point_near(int x, int y, int radius, int * out_x, int * out_y)` |  |
| `0x004d5f4c` | `llm_strat_ai_group_scatter_random_worker` | `void llm_strat_ai_group_scatter_random_worker(int param_1, int param_2, undefined4 a2)` |  |
| `0x004d5fe7` | `llm_strat_ai_unit_move_near_random_point` | `void llm_strat_ai_unit_move_near_random_point(int player_or_context, int unit_id, int anchor_x, int anchor_y, int radius)` | conf:low todo:dead |
| `0x004d601f` | `llm_watcom_epilogue_004d601f` | `void llm_watcom_epilogue_004d601f(void)` | conf:low |
| `0x004d6028` | `llm_strat_ai_group_scatter_near_point` | `void llm_strat_ai_group_scatter_near_point(int param_1, int param_2, int a2, int param_4, int param_5)` | todo:dead |
| `0x004d60f5` | `llm_strat_ai_group_scatter_idle_members` | `void llm_strat_ai_group_scatter_idle_members(int player, int group_index, int point_x, int point_y, int radius)` | conf:med todo:dead |
| `0x004d61d3` | `llm_strat_ai_group_reposition_members` | `void llm_strat_ai_group_reposition_members(uint player, int group_idx)` | conf:med |
| `0x004d65f0` | `llm_strat_ai_group_issue_default_order_all` | `void llm_strat_ai_group_issue_default_order_all(int player_idx, int group_id, undefined4 order_type, undefined4 order_arg)` | conf:med todo:dead |
| `0x004d6646` | `llm_strat_ai_group_is_within_radius` | `int llm_strat_ai_group_is_within_radius(game_t_Player player, int group_index)` | conf:med todo:dead |
| `0x004d66d1` | `llm_watcom_return_true_004d66d1` | `int llm_watcom_return_true_004d66d1(void)` |  |
| `0x004d66d6` | `llm_watcom_epilogue_004d66d6` | `undefined llm_watcom_epilogue_004d66d6(void)` | conf:low |
| `0x004d66df` | `llm_strat_ai_group_no_member_near_centroid` | `int llm_strat_ai_group_no_member_near_centroid(int player, int group_index)` |  |
| `0x004d6772` | `llm_strat_ai_group_is_gathered` | `bool llm_strat_ai_group_is_gathered(int player_idx, int group_id)` | conf:high todo:dead |
| `0x004d6816` | `llm_strat_ai_group_tick_arrivals` | `bool llm_strat_ai_group_tick_arrivals(int player_idx, int group_id)` | conf:high todo:dead |
| `0x004d68e6` | `llm_strat_ai_group_rally_stragglers` | `int llm_strat_ai_group_rally_stragglers(int player, int ai_group_id)` | todo:dead |
| `0x004d69ce` | `llm_strat_ai_group_area_scan_hostile` | `int llm_strat_ai_group_area_scan_hostile(game_t_Player player, int group_index, byte owner_mask)` | conf:high |
| `0x004d6b84` | `llm_strat_ai_group_all_units_settled` | `int llm_strat_ai_group_all_units_settled(int player_id, int group_index)` |  |
| `0x004d6be3` | `llm_strat_ai_group_find_slowest_unit` | `uint llm_strat_ai_group_find_slowest_unit(int player_id, int group_index)` |  |
| `0x004d6c85` | `llm_strat_ai_group_pick_best_weapon_unit` | `uint llm_strat_ai_group_pick_best_weapon_unit(int player_id, int group_index)` | conf:med |
| `0x004d6d67` | `llm_strat_ai_target_list_add` | `void llm_strat_ai_target_list_add(game_t_Player player, int victim_ref, int victim_index, uint aggressor_ref, int aggressor_index)` | conf:high |
| `0x004d6e6f` | `llm_strat_ai_target_list_remove` | `void llm_strat_ai_target_list_remove(int player, uint aggressor_ref, int aggressor_index)` |  |
| `0x004d71c7` | `llm_gfx_save_screenshot_rle` | `void llm_gfx_save_screenshot_rle(ushort * framebuffer, undefined4 unused_param2, char * filepath)` | conf:med |
| `0x004d729d` | `llm_strat_ai_unit_weapon_power` | `int llm_strat_ai_unit_weapon_power(game_t_Player player, map_t_unit_id unit_id)` |  |
| `0x004d7311` | `llm_strat_unit_has_aa_weapon` | `int llm_strat_unit_has_aa_weapon(uint unit_ref, int unit_index)` |  |
| `0x004d7377` | `llm_strat_unit_has_ground_weapon` | `int llm_strat_unit_has_ground_weapon(uint unit_ref, int unit_index)` |  |
| `0x004d73dd` | `llm_strat_ai_target_ref_has_ground_weapon` | `int llm_strat_ai_target_ref_has_ground_weapon(uint target_ref_packed, int target_index)` |  |
| `0x004d7457` | `llm_strat_ai_target_ref_has_aa_weapon` | `int llm_strat_ai_target_ref_has_aa_weapon(uint target_ref_packed, int target_index)` |  |
| `0x004d746c` | `llm_strat_bldg_has_aa_weapon` | `int llm_strat_bldg_has_aa_weapon(uint player, int building_index)` |  |
| `0x004d74de` | `llm_strat_ai_update_opponent_relations` | `void llm_strat_ai_update_opponent_relations(game_t_Player assessed_player, llm_strat_ai_opponent_assessment * out)` |  |
| `0x004d77cc` | `llm_strat_bldg_connectivity_flood_fill` | `void llm_strat_bldg_connectivity_flood_fill(uint player, int exclude_bldg_idx, byte * flag_array)` | conf:med |
| `0x004d7a2e` | `llm_strat_ai_recompute_map_influence` | `void llm_strat_ai_recompute_map_influence(int player)` | conf:high |
| `0x004d7b95` | `llm_gfx_render_ai_vision_debug_map` | `void llm_gfx_render_ai_vision_debug_map(int player_index, ushort * dest_surface, int dest_row_stride)` | conf:med |
| `0x004d7da9` | `llm_strat_ai_group_move_formation_rotating` | `void llm_strat_ai_group_move_formation_rotating(game_t_Player player, int unused_param2, int unused_param3, int target_x, int target_y)` |  |
| `0x004d7e22` | `llm_strat_ai_unit_flag_and_move` | `void llm_strat_ai_unit_flag_and_move(uint param_1, int param_2, undefined4 a2, undefined4 param_4)` |  |
| `0x004d7e6b` | `llm_strat_ai_group_scatter_to_passable_tile` | `void llm_strat_ai_group_scatter_to_passable_tile(uint param_1, int param_2, int a2)` |  |
| `0x004d7f3a` | `llm_strat_unit_queue_advance_search` | `int llm_strat_unit_queue_advance_search(uint param_1, uint param_2)` |  |
| `0x004d81a6` | `llm_strat_ai_target_dist_sq` | `uint llm_strat_ai_target_dist_sq(uint ref_a, int idx_a, uint ref_b, int idx_b)` |  |
| `0x004d82ca` | `llm_strat_ai_notify_map_changed` | `void llm_strat_ai_notify_map_changed(int builder_player, int building_type, int tile_x, int tile_y)` |  |
| `0x004d83c7` | `llm_strat_ai_notify_map_changed_2` | `void llm_strat_ai_notify_map_changed_2(int builder_player, int building_type, int tile_x, int tile_y)` |  |
| `0x004d84ba` | `llm_strat_ai_building_defense_weapon_range` | `int llm_strat_ai_building_defense_weapon_range(int player, int building_index)` |  |
| `0x004d8526` | `llm_strat_ai_turret_threat_rescan` | `void llm_strat_ai_turret_threat_rescan(uint player)` |  |
| `0x004d86c8` | `llm_strat_serialize_player_objects` | `void llm_strat_serialize_player_objects(int player)` | conf:med |
| `0x004d8839` | `llm_strat_load_base_layout_dmp` | `void llm_strat_load_base_layout_dmp(int param_1, char * dmp_path)` |  |
| `0x004d8a9d` | `llm_strat_ai_bldg_has_heli_unit` | `int llm_strat_ai_bldg_has_heli_unit(int player)` | conf:med |
| `0x004d8b89` | `llm_strat_bldg_side_has_aircraft_producer` | `int llm_strat_bldg_side_has_aircraft_producer(int player)` | conf:med |
| `0x004d8c7d` | `llm_strat_bldg_check_placement_encloses_neighbors` | `int llm_strat_bldg_check_placement_encloses_neighbors(int player, int x, int y, uint width, uint height)` | conf:med |
| `0x004d900c` | `llm_ui_message_box_confirm` | `int llm_ui_message_box_confirm(LPCSTR caption, LPCSTR message, uint mb_flags)` | conf:med |
| `0x004d9054` | `llm_gfx_ddraw_surface_by_id` | `void * llm_gfx_ddraw_surface_by_id(llm_gfx_ddraw_device * ctx, uint surface_id)` |  |
| `0x004d90b0` | `llm_gfx_ddraw_create_video_surface` | `int llm_gfx_ddraw_create_video_surface(llm_gfx_ddraw_device * ctx)` |  |
| `0x004d92ec` | `llm_gfx_ddraw_create_surf40` | `int llm_gfx_ddraw_create_surf40(llm_gfx_ddraw_device * ctx)` | conf:low |
| `0x004d9480` | `llm_gfx_ddraw_create_surfaces` | `int llm_gfx_ddraw_create_surfaces(llm_gfx_ddraw_device * ctx)` | conf:med todo:struct |
| `0x004d97cc` | `llm_gfx_ddraw_set_display_mode` | `int llm_gfx_ddraw_set_display_mode(llm_gfx_ddraw_device * ctx)` | conf:high |
| `0x004d99e8` | `llm_gfx_ddraw_teardown` | `int llm_gfx_ddraw_teardown(llm_gfx_ddraw_device * ctx)` | conf:high todo:struct |
| `0x004d9bb4` | `llm_gfx_ddraw_lock_surface` | `int llm_gfx_ddraw_lock_surface(llm_gfx_ddraw_device * ctx, uint surface_id, void * out_lock_info)` | conf:med todo:enum todo:struct |
| `0x004d9d0c` | `llm_gfx_ddraw_release_surface` | `int llm_gfx_ddraw_release_surface(llm_gfx_ddraw_device * ctx, uint surface_id)` |  |
| `0x004d9e40` | `llm_gfx_ddraw_surface_restore_if_lost` | `int llm_gfx_ddraw_surface_restore_if_lost(IDirectDrawSurface * surf)` |  |
| `0x004d9ef0` | `llm_gfx_ddraw_restore_surfaces` | `int llm_gfx_ddraw_restore_surfaces(llm_gfx_ddraw_device * ctx)` | conf:high todo:struct |
| `0x004d9fdc` | `llm_gfx_ddraw_restore_surface` | `int llm_gfx_ddraw_restore_surface(llm_gfx_ddraw_device * ctx, uint surface_id)` |  |
| `0x004da014` | `llm_gfx_ddraw_present` | `int llm_gfx_ddraw_present(llm_gfx_ddraw_device * ctx)` |  |
| `0x004da1f4` | `llm_gfx_ddraw_read_current_mode` | `int llm_gfx_ddraw_read_current_mode(llm_gfx_ddraw_device * ctx)` |  |
| `0x004da3f0` | `llm_gfx_ddraw_dll_acquire` | `void llm_gfx_ddraw_dll_acquire(void)` | conf:high |
| `0x004da434` | `llm_gfx_ddraw_dll_release` | `void llm_gfx_ddraw_dll_release(void)` | conf:high |
| `0x004da98b` | `llm_rand` | `int llm_rand(void)` |  |
| `0x004da9c0` | `llm_sqrt` | `double llm_sqrt(double x)` |  |
| `0x004daa52` | `llm_math_atan` | `double llm_math_atan(double x)` | conf:med |
| `0x004dabbc` | `llm_math_cos_impl` | `double llm_math_cos_impl(void)` | conf:high |
| `0x004dabc6` | `llm_math_fsin_reduce_loop` | `double llm_math_fsin_reduce_loop(void)` | conf:med |
| `0x004dac44` | `llm_strat_target_release_ref` | `void llm_strat_target_release_ref(uint player_idx, int unit_idx, uint mode)` |  |
| `0x004dae0e` | `llm_strat_unit_notify_status` | `void llm_strat_unit_notify_status(uint player, int unit_index, uint status_code)` |  |
| `0x004daec2` | `llm_strat_ai_notify_bldg_constructed` | `void llm_strat_ai_notify_bldg_constructed(uint player, undefined4 x_b, undefined4 param_3, undefined4 building_id, undefined4 y_b, undefined4 param_6)` |  |
| `0x004db22f` | `llm_strat_ai_bldg_register_visible_building` | `void llm_strat_ai_bldg_register_visible_building(int victim_index, uint victim_ref, uint aggressor_unit_index, uint aggressor_ref, int victim_destroyed)` | conf:high |
| `0x004db499` | `llm_strat_ai_group_member_count_adjust` | `void llm_strat_ai_group_member_count_adjust(uint player, uint unit_index, uint group_or_type, uint mode)` |  |
| `0x004db54a` | `llm_util_empty_return_stub` | `void llm_util_empty_return_stub(void)` | conf:med |
| `0x004db551` | `llm_strat_ai_notify_object_removed` | `void llm_strat_ai_notify_object_removed(uint flags, uint object_index, int hard_remove)` |  |
| `0x004db905` | `llm_strat_ai_players_tick` | `void llm_strat_ai_players_tick(double dt)` |  |
| `0x004dbb38` | `llm_strat_ai_notify_unit_lifecycle` | `void llm_strat_ai_notify_unit_lifecycle(game_t_Player_s player_, cfg_t_unit_index_s unit_type, map_t_unit_id unit_id, undefined4 param_4)` | conf:med |
| `0x004dbdf6` | `llm_gfx_plot_pixel_mark_tile_visible` | `void llm_gfx_plot_pixel_mark_tile_visible(int x, int y, ushort color_rgb565)` | conf:med |
| `0x004dbe50` | `llm_gfx_draw_number_5x7` | `void llm_gfx_draw_number_5x7(int x, int y, int value)` | conf:low |
| `0x004dbf27` | `llm_strat_unit_queue_advance` | `void llm_strat_unit_queue_advance(uint param_1, uint param_2)` |  |
| `0x004dbf36` | `llm_strat_ai_queue_release_order` | `void llm_strat_ai_queue_release_order(int player, int building_index, int mode)` | conf:high |
| `0x004dc0dd` | `llm_diplomacy_ai_relation_swap` | `int llm_diplomacy_ai_relation_swap(int player, int toward_player, int new_relation)` |  |
| `0x004dc117` | `llm_strat_sort_sites_by_dist` | `void llm_strat_sort_sites_by_dist(int player_id)` | conf:med |
| `0x004dc25f` | `llm_strat_bldg_recompute_cell_grid` | `void llm_strat_bldg_recompute_cell_grid(void)` | conf:med |
| `0x004dc597` | `llm_strat_ai_spiral_table_init` | `void llm_strat_ai_spiral_table_init(void)` | conf:med |
| `0x004dc65a` | `llm_strat_planet_map_session_init` | `void llm_strat_planet_map_session_init(void)` | conf:med |
| `0x004dc74d` | `llm_bldg_find_index_by_type` | `int llm_bldg_find_index_by_type(uint building_type)` | conf:high |
| `0x004dc781` | `llm_strat_ai_build_plan_push` | `void llm_strat_ai_build_plan_push(int player, int ai_build_id)` | conf:high |
| `0x004dc7be` | `llm_strat_ai_init_build_candidate_priorities` | `void llm_strat_ai_init_build_candidate_priorities(int player_index)` | conf:med |
| `0x004dcd0e` | `llm_strat_spawn_ai_base` | `void llm_strat_spawn_ai_base(int player, int is_alien, int x, int y)` |  |
| `0x004dd446` | `llm_strat_spawn_invasion_force` | `int llm_strat_spawn_invasion_force(game_t_Player player, int is_alien_race, int home_tile_x, int home_tile_y, int invasion_points)` |  |
| `0x004dd91d` | `llm_strat_init_human_player_data` | `void llm_strat_init_human_player_data(uint player_idx, int is_alien_race)` |  |
| `0x004dda5b` | `llm_wnd_destroy_prehook` | `undefined llm_wnd_destroy_prehook(void)` |  |
| `0x004dda66` | `llm_game_save_player_data` | `void llm_game_save_player_data(void * save_file)` | conf:high |
| `0x004ddadf` | `llm_strat_ai_player_get_phase_flags` | `uint llm_strat_ai_player_get_phase_flags(int player_index)` | conf:med todo:dead |
| `0x004ddb07` | `llm_strat_ai_player_set_phase_flags` | `undefined llm_strat_ai_player_set_phase_flags(void)` | conf:med todo:dead |
| `0x004ddb31` | `llm_strat_ai_scr_parse` | `void llm_strat_ai_scr_parse(char * filename)` |  |
| `0x004ddc70` | `llm_lzss_decompress_block` | `void llm_lzss_decompress_block(void * block, void * dst)` | conf:med |
| `0x004ddcd7` | `llm_lzss_next_flag_bit` | `undefined1[9] llm_lzss_next_flag_bit(undefined2 param_1, undefined4 param_2, undefined2 param_3, undefined1 param_4)` | conf:med todo:proto |
| `0x004ddff0` | `llm_str_find_substr` | `byte * llm_str_find_substr(byte * haystack, byte * needle)` | conf:high |
| `0x004de1fd` | `llm_str_copy_fixed_w` | `undefined llm_str_copy_fixed_w(void)` | conf:high todo:proto |
| `0x004de261` | `llm_util_memmove` | `void llm_util_memmove(void * dest, void * src, uint size)` | conf:high |
| `0x004de369` | `llm_str_rchr_last` | `char * llm_str_rchr_last(char * str, char target_char)` | conf:high |
| `0x004de3f0` | `llm_gfx_rle_blit_rows_8bpp` | `void llm_gfx_rle_blit_rows_8bpp(void)` | conf:med todo:globals |
| `0x004de459` | `llm_gfx_rle_blit_rows_packbits` | `void llm_gfx_rle_blit_rows_packbits(void)` | conf:med todo:globals |
| `0x004de4ab` | `llm_gfx_rle_blit_rows_16bpp_clipped` | `void llm_gfx_rle_blit_rows_16bpp_clipped(void)` | conf:med todo:globals |
| `0x004de613` | `llm_gfx_rle_widget_draw_dispatch` | `void llm_gfx_rle_widget_draw_dispatch(int * anim_state, int dest_pixels, int advance_flag)` | conf:med |
| `0x004ded64` | `llm_crt_get_thread_data` | `void * llm_crt_get_thread_data(void)` | conf:high todo:globals |
| `0x004dfae3` | `llm_crt_set_errno` | `pointer llm_crt_set_errno(int errno_code)` | todo:globals |
| `0x004dfede` | `llm_crt_file_buffer_flush` | `undefined llm_crt_file_buffer_flush(void)` | conf:high todo:proto |
| `0x004dffc8` | `llm_crt_lseek` | `DWORD llm_crt_lseek(int fd, LONG offset, DWORD whence)` | conf:high todo:globals |
| `0x004e00cd` | `llm_crt_map_last_error_to_errno` | `void llm_crt_map_last_error_to_errno(void)` | conf:high |
| `0x004e0459` | `llm_crt_write_fd` | `undefined llm_crt_write_fd(void)` | conf:high todo:proto |
| `0x004e0e7e` | `llm_crt_wfmt_convert_spec` | `undefined llm_crt_wfmt_convert_spec(void)` | conf:low todo:proto todo:struct |
| `0x004e1764` | `llm_crt_scanf_parse_conv_flags` | `char * llm_crt_scanf_parse_conv_flags(char * fmt, void * conv_spec)` | conf:med todo:struct |
| `0x004e1af1` | `llm_crt_scanf_read_scanset` | `int llm_crt_scanf_read_scanset(void * scan_ctx, int * arg_cursor, char * * pfmt)` | conf:med todo:struct |
| `0x004e1beb` | `llm_crt_scanf_read_float` | `int llm_crt_scanf_read_float(void * scan_ctx, int * arg_cursor)` | conf:med todo:struct |
| `0x004e1f02` | `llm_crt_scanf_read_int` | `int llm_crt_scanf_read_int(void * scan_ctx, int * arg_cursor, int lookahead_state, int allow_sign)` | conf:med todo:struct |
| `0x004e215f` | `llm_strat_econ_track_unit_resource_spend` | `void llm_strat_econ_track_unit_resource_spend(int player_idx, int unit_idx)` | conf:med |
| `0x004e21e4` | `llm_strat_ai_count_unit_build_sources` | `void llm_strat_ai_count_unit_build_sources(int player, int * out_counts)` |  |
| `0x004e23ae` | `llm_unit_can_afford_resources` | `int llm_unit_can_afford_resources(int player_id, int unit_type)` | conf:high |
| `0x004e2416` | `llm_bldg_can_afford_resources` | `int llm_bldg_can_afford_resources(int player_id, int building_type)` | conf:high |
| `0x004e2478` | `llm_strat_bldg_find_idle_producer_for_unit` | `int llm_strat_bldg_find_idle_producer_for_unit(int player_id, int unit_id)` |  |
| `0x004e2550` | `llm_strat_bldg_queue_construction` | `int llm_strat_bldg_queue_construction(int player, int building_type, short x, ushort y)` |  |
| `0x004e268c` | `llm_strat_ai_queue_train_unit` | `int llm_strat_ai_queue_train_unit(int player, uint unit_id)` |  |
| `0x004e284d` | `llm_strat_ai_queue_bldg_repair` | `int llm_strat_ai_queue_bldg_repair(int player, int building_index)` |  |
| `0x004e2a2e` | `llm_strat_ai_queue_bldg_upgrade` | `int llm_strat_ai_queue_bldg_upgrade(int player, uint building_index)` |  |
| `0x004e2c8e` | `llm_strat_ai_queue_flush_unit_train_entries` | `void llm_strat_ai_queue_flush_unit_train_entries(int player)` |  |
| `0x004e2c98` | `llm_strat_ai_queue_flush_unit_train_entries_2` | `void llm_strat_ai_queue_flush_unit_train_entries_2(int player)` |  |
| `0x004e2cac` | `llm_strat_ai_queue_rotate_newest_to_front` | `void llm_strat_ai_queue_rotate_newest_to_front(int player)` |  |
| `0x004e2d52` | `llm_strat_bldg_total_resource_cost` | `int llm_strat_bldg_total_resource_cost(int building_type)` |  |
| `0x004e2d91` | `llm_strat_ai_queue_reconcile_bldg_change` | `void llm_strat_ai_queue_reconcile_bldg_change(uint player, uint building_index)` |  |
| `0x004e2eb9` | `llm_strat_ai_calc_resource_sum_5x5` | `void llm_strat_ai_calc_resource_sum_5x5(int building_type, int x, int y, int * out_sums)` | todo:dead |
| `0x004e3006` | `llm_strat_ai_calc_building_extract_value` | `int llm_strat_ai_calc_building_extract_value(int building_type, int tile_x, int tile_y)` | conf:med todo:dead |
| `0x004e30ab` | `llm_strat_ai_calc_site_resource_score` | `int llm_strat_ai_calc_site_resource_score(int param_1, int param_2, int a2)` | todo:dead |
| `0x004e30e9` | `llm_strat_ai_calc_mine_yield_estimate` | `void llm_strat_ai_calc_mine_yield_estimate(int building_id, uint tile_x, uint tile_y, int * out_yield, int * out_quality)` |  |
| `0x004e32ef` | `llm_strat_ai_sum_player_mine_resources` | `void llm_strat_ai_sum_player_mine_resources(int player, int * out_sums)` | todo:dead |
| `0x004e340f` | `llm_watcom_epilogue_004e340f` | `void llm_watcom_epilogue_004e340f(void)` | conf:low |
| `0x004e3418` | `llm_strat_ai_mine_portfolio_rebalance` | `void llm_strat_ai_mine_portfolio_rebalance(uint player, int * out_yield)` |  |
| `0x004e3872` | `llm_strat_ai_storage_capacity_short_and_cap_check` | `int llm_strat_ai_storage_capacity_short_and_cap_check(int player)` |  |
| `0x004e3926` | `llm_strat_ai_calc_power_supply_ratio` | `double llm_strat_ai_calc_power_supply_ratio(int player)` |  |
| `0x004e3a77` | `llm_strat_ai_is_worker_priority_candidate` | `int llm_strat_ai_is_worker_priority_candidate(int player, int site_index)` |  |
| `0x004e3bc2` | `llm_strat_ai_rebalance_building_workers` | `int llm_strat_ai_rebalance_building_workers(uint player)` |  |
| `0x004e40ae` | `llm_strat_ai_count_nearby_turrets` | `int llm_strat_ai_count_nearby_turrets(int player, int x, int y)` | todo:dead |
| `0x004e419e` | `llm_watcom_epilogue_004e419e` | `undefined llm_watcom_epilogue_004e419e(void)` | conf:low todo:proto |
| `0x004e41a6` | `llm_strat_ai_apply_building_proximity_bonus` | `int llm_strat_ai_apply_building_proximity_bonus(int player, int x, int y, int delta)` | todo:dead |
| `0x004e426b` | `llm_strat_ai_scan_construction_sites` | `int llm_strat_ai_scan_construction_sites(int player, int category)` | conf:med |
| `0x004e4a95` | `llm_strat_ai_expand_adjacent_mine_relay` | `void llm_strat_ai_expand_adjacent_mine_relay(uint player_idx)` | todo:dead |
| `0x004e4e36` | `llm_strat_bldg_queue_construction_thunk` | `void llm_strat_bldg_queue_construction_thunk(int player, int building_type, short x, ushort y)` |  |
| `0x004e4e54` | `llm_strat_ai_find_nearest_flagged_building` | `int llm_strat_ai_find_nearest_flagged_building(int player, int x, int y)` |  |
| `0x004e4f1f` | `llm_strat_ai_plan_turret_upgrade` | `void llm_strat_ai_plan_turret_upgrade(uint player)` |  |
| `0x004e52c4` | `llm_strat_ai_plan_mine_construction` | `void llm_strat_ai_plan_mine_construction(int player)` |  |
| `0x004e54cc` | `llm_strat_ai_score_build_categories` | `void llm_strat_ai_score_build_categories(int player)` |  |
| `0x004e5686` | `llm_strat_ai_player_score_tier` | `int llm_strat_ai_player_score_tier(int player_idx)` | conf:med |
| `0x004e571d` | `llm_strat_ai_react_resource_shortage` | `void llm_strat_ai_react_resource_shortage(int player)` |  |
| `0x004e58d1` | `llm_strat_ai_maintain_unit_housing` | `void llm_strat_ai_maintain_unit_housing(int player)` |  |
| `0x004e5ae0` | `llm_strat_ai_plan_construction` | `void llm_strat_ai_plan_construction(uint player_idx)` |  |
| `0x004e5da2` | `llm_strat_ai_queue_remove_at` | `void llm_strat_ai_queue_remove_at(int player, uint slot_index)` |  |
| `0x004e5e04` | `llm_strat_ai_scan_bldg_repair_upgrade` | `void llm_strat_ai_scan_bldg_repair_upgrade(int player)` |  |
| `0x004e611f` | `llm_strat_order_collect_available_projects_thunk` | `void llm_strat_order_collect_available_projects_thunk(int player)` |  |
| `0x004e612e` | `llm_strat_ai_mine_exists_nearby` | `int llm_strat_ai_mine_exists_nearby(uint player_id, int tile_x, int tile_y)` | conf:med todo:dead |
| `0x004e6228` | `llm_strat_ai_sort_site_candidates_by_dist` | `void llm_strat_ai_sort_site_candidates_by_dist(void)` | conf:high |
| `0x004e62ad` | `llm_strat_ai_resource_site_meets_threshold` | `int llm_strat_ai_resource_site_meets_threshold(int player_idx, int building_type, int fine_x, int fine_y)` | conf:med |
| `0x004e6396` | `llm_strat_ai_bldg_scan_resource_site_candidates` | `void llm_strat_ai_bldg_scan_resource_site_candidates(game_t_Player player, int building_type)` | conf:med |
| `0x004e6591` | `llm_strat_ai_bldg_scan_grid_candidates` | `void llm_strat_ai_bldg_scan_grid_candidates(game_t_Player player, int building_idx)` | conf:med |
| `0x004e66a4` | `llm_strat_ai_scan_build_site_candidates` | `void llm_strat_ai_scan_build_site_candidates(int player_idx, int building_idx)` | conf:med |
| `0x004e685b` | `llm_strat_bldg_record_resource_expenditure_stats` | `void llm_strat_bldg_record_resource_expenditure_stats(game_t_Player player_id, cfg_t_building_index building_id)` | conf:med |
| `0x004e68dd` | `llm_strat_ai_group_split_off_create` | `void llm_strat_ai_group_split_off_create(game_t_Player player, int centroid_x, int centroid_y, int source_group_idx, uint member_count)` | conf:high |
| `0x004e69ed` | `llm_strat_ai_group_split_excess_members` | `void llm_strat_ai_group_split_excess_members(game_t_Player player, int group_idx)` | conf:med |
| `0x004e6a89` | `llm_strat_ai_group_redistribute_units` | `void llm_strat_ai_group_redistribute_units(uint player, int group_index)` | conf:med |
| `0x004e6d03` | `llm_strat_ai_plan_unit_training` | `void llm_strat_ai_plan_unit_training(int player)` |  |
| `0x004e6f39` | `llm_strat_ai_group_home_guard_replenish` | `void llm_strat_ai_group_home_guard_replenish(int player_id)` |  |
| `0x004e7138` | `llm_strat_ai_pick_owned_tile_or_home` | `void llm_strat_ai_pick_owned_tile_or_home(int param_1, uint * param_2, uint * a2)` |  |
| `0x004e722a` | `llm_strat_ai_group_form_standby_from_pool3` | `void llm_strat_ai_group_form_standby_from_pool3(int player_id)` |  |
| `0x004e72ef` | `llm_strat_ai_group_form_surplus_from_pool4` | `void llm_strat_ai_group_form_surplus_from_pool4(int player_id)` |  |
| `0x004e73b3` | `llm_strat_ai_army_milestone_advance_or_attack` | `void llm_strat_ai_army_milestone_advance_or_attack(game_t_Player player)` |  |
| `0x004e769e` | `llm_strat_ai_group_expansion_form_or_repurpose` | `void llm_strat_ai_group_expansion_form_or_repurpose(uint player_id)` |  |
| `0x004e7938` | `llm_strat_ai_group_form_perimeter_patrol` | `void llm_strat_ai_group_form_perimeter_patrol(int player_idx)` | todo:dead |
| `0x004e7ad2` | `llm_strat_ai_group_form_patrol` | `void llm_strat_ai_group_form_patrol(int player_id)` |  |
| `0x004e7bb5` | `llm_strat_ai_resource_spend_rate_update` | `void llm_strat_ai_resource_spend_rate_update(int player)` |  |
| `0x004e7cec` | `llm_strat_ai_bldg_production_type_dispatch` | `void llm_strat_ai_bldg_production_type_dispatch(game_t_Player player_id, cfg_t_building_index building_id)` | conf:med |
| `0x004e7dd7` | `llm_strat_ai_bldg_queue_process_entry` | `void llm_strat_ai_bldg_queue_process_entry(game_t_Player player, int queue_index)` | conf:high |
| `0x004e80ef` | `llm_strat_ai_bldg_queue_handle_recruit_state` | `void llm_strat_ai_bldg_queue_handle_recruit_state(uint player_id, int queue_index)` | conf:med |
| `0x004e81df` | `llm_strat_ai_bldg_queue_handle_state2_empty` | `void llm_strat_ai_bldg_queue_handle_state2_empty(void)` |  |
| `0x004e81ea` | `llm_strat_ai_bldg_queue_handle_upgrade_or_cancel` | `void llm_strat_ai_bldg_queue_handle_upgrade_or_cancel(uint player_id, int queue_index)` | conf:med |
| `0x004e82ab` | `llm_strat_ai_bldg_queue_process` | `void llm_strat_ai_bldg_queue_process(uint player_id)` | conf:high todo:enum |
| `0x004e853f` | `llm_strat_ai_recompute_shortage_state` | `void llm_strat_ai_recompute_shortage_state(uint player_idx)` |  |
| `0x004e8773` | `llm_strat_ai_invasion_spawn_reinforcements` | `int llm_strat_ai_invasion_spawn_reinforcements(int player_idx)` |  |
| `0x004e8a1f` | `llm_strat_ai_invasion_launch_attack_group` | `void llm_strat_ai_invasion_launch_attack_group(game_t_Player player)` |  |
| `0x004e8b9d` | `llm_strat_ai_player_tick` | `void llm_strat_ai_player_tick(int player)` |  |
| `0x004e93a5` | `llm_strat_ai_group_members_ready` | `int llm_strat_ai_group_members_ready(uint player, uint ai_group_index)` | conf:med todo:dead |
| `0x004e943d` | `llm_strat_ai_group_check_arrival_status` | `int llm_strat_ai_group_check_arrival_status(uint player, int ai_group_id)` |  |
| `0x004e95c1` | `llm_strat_ai_group_readiness_state` | `int llm_strat_ai_group_readiness_state(uint player, uint ai_group_index)` | conf:med todo:dead |
| `0x004e96b1` | `llm_strat_ai_group_task_step` | `uint llm_strat_ai_group_task_step(uint player_id, int group_index)` |  |
| `0x004e98a7` | `llm_strat_ai_group_find_nearest_building_of_types` | `int llm_strat_ai_group_find_nearest_building_of_types(int player_id, int query_x, int query_y, uint building_type_1, uint building_type_2, uint building_type_3, uint building_type_4)` |  |
| `0x004e99be` | `llm_strat_ai_group_collect_buildings_of_types` | `void llm_strat_ai_group_collect_buildings_of_types(int player_id, undefined4 unused_edx_slot, undefined4 unused_ebx_slot, uint building_type_1, uint building_type_2, uint building_type_3, uint building_type_4)` |  |
| `0x004e9a73` | `llm_strat_ai_group_task_attack_random_target` | `void llm_strat_ai_group_task_attack_random_target(int player_id, int group_index)` | conf:med |
| `0x004e9c98` | `llm_strat_ai_group_task_drain_reserve_attack` | `void llm_strat_ai_group_task_drain_reserve_attack(game_t_Player player_id, int group_index)` |  |
| `0x004ea072` | `llm_strat_ai_group_task_engage_target` | `void llm_strat_ai_group_task_engage_target(int player_id, int group_index)` |  |
| `0x004ea0d2` | `llm_strat_ai_group_task_attack_nearest_defended` | `void llm_strat_ai_group_task_attack_nearest_defended(int player_id, int group_index)` |  |
| `0x004ea469` | `llm_strat_ai_group_task_muster_from_pool` | `void llm_strat_ai_group_task_muster_from_pool(uint player_id, int group_index)` |  |
| `0x004ea5d9` | `llm_strat_ai_group_task_recruit_from_pool3` | `void llm_strat_ai_group_task_recruit_from_pool3(game_t_Player player_id, int group_index)` |  |
| `0x004ea69c` | `llm_strat_ai_group_task_recruit_from_pool4` | `void llm_strat_ai_group_task_recruit_from_pool4(game_t_Player player_id, int group_index)` |  |
| `0x004ea729` | `llm_strat_ai_group_task_recruit_from_storage` | `void llm_strat_ai_group_task_recruit_from_storage(game_t_Player player_id, int group_index)` |  |
| `0x004eaaa4` | `llm_strat_ai_group_task_disband` | `void llm_strat_ai_group_task_disband(game_t_Player player_id, int group_index)` |  |
| `0x004eab33` | `llm_strat_ai_group_task_hold` | `void llm_strat_ai_group_task_hold(void)` |  |
| `0x004eab3e` | `llm_strat_ai_group_task_recall_home` | `void llm_strat_ai_group_task_recall_home(int player_id, int group_index)` |  |
| `0x004eabd3` | `llm_strat_ai_group_task_patrol_shuttle` | `void llm_strat_ai_group_task_patrol_shuttle(int player_id, int group_index)` |  |
| `0x004ead22` | `llm_strat_ai_group_task_loiter_wander` | `void llm_strat_ai_group_task_loiter_wander(int player_id, int group_index)` |  |
| `0x004eaea5` | `llm_strat_ai_group_task_rally_formup` | `void llm_strat_ai_group_task_rally_formup(int player_id, int group_index)` |  |
| `0x004eaeb4` | `llm_strat_ai_group_task_scatter_random` | `void llm_strat_ai_group_task_scatter_random(int player_id, int group_index)` |  |
| `0x004eaecb` | `llm_strat_ai_group_task_nudge_stragglers` | `void llm_strat_ai_group_task_nudge_stragglers(uint player_id, int group_index)` |  |
| `0x004eaf67` | `llm_strat_ai_group_task_wait` | `void llm_strat_ai_group_task_wait(void)` |  |
| `0x004eaf72` | `llm_strat_ai_group_task_advance_to_anchor` | `void llm_strat_ai_group_task_advance_to_anchor(int player_id, int group_index)` |  |
| `0x004eb02e` | `llm_strat_ai_group_task_disperse_passable` | `void llm_strat_ai_group_task_disperse_passable(uint player_id, int group_index)` |  |
| `0x004eb131` | `llm_strat_ai_group_task_activate` | `void llm_strat_ai_group_task_activate(int player_id, int group_index)` |  |
| `0x004eb245` | `llm_strat_ai_group_enter_hold` | `void llm_strat_ai_group_enter_hold(int player_id, int group_index)` |  |
| `0x004eb2a0` | `llm_strat_ai_group1_drain_to_group0` | `void llm_strat_ai_group1_drain_to_group0(game_t_Player player_id)` |  |
| `0x004eb2ec` | `llm_strat_ai_unit_group_tick` | `void llm_strat_ai_unit_group_tick(int player)` |  |
| `0x004eb430` | `llm_strat_ai_log_scan_targets` | `void llm_strat_ai_log_scan_targets(void * stream)` | conf:low todo:dead |
| `0x004eb48e` | `llm_strat_ai_group_classify_target_object` | `void llm_strat_ai_group_classify_target_object(uint player_id, llm_strat_ai_scan_target_entry * target_record)` |  |
| `0x004eb9ab` | `llm_strat_ai_build_target_list` | `void llm_strat_ai_build_target_list(uint player)` | conf:med todo:dead |
| `0x004ebe84` | `llm_strat_ai_collect_unclaimed_scan_targets` | `void llm_strat_ai_collect_unclaimed_scan_targets(int unused_param1, llm_strat_ai_scan_target_entry * out_targets)` | conf:med todo:dead |
| `0x004ebf09` | `llm_strat_ai_group_scan_building_targets` | `uint llm_strat_ai_group_scan_building_targets(int player, void * target_list, uint group_index)` | conf:med todo:dead |
| `0x004ec1ee` | `llm_watcom_epilogue_004ec1ee` | `void llm_watcom_epilogue_004ec1ee(void)` | conf:low |
| `0x004ec1f6` | `llm_strat_ai_collect_group_scan_targets` | `int llm_strat_ai_collect_group_scan_targets(int player_idx, llm_strat_ai_scan_target_entry * out_targets, uint ai_group_idx)` | conf:high todo:dead |
| `0x004ec2d0` | `llm_strat_ai_scan_target_list_add` | `void llm_strat_ai_scan_target_list_add(int player, uint target_ref, int target_index)` |  |
| `0x004ec35f` | `llm_strat_ai_holding_pen_scan_targets` | `int llm_strat_ai_holding_pen_scan_targets(int player, uint pen_index)` |  |
| `0x004ec51d` | `llm_strat_ai_target_list_invalidate_by_id` | `int llm_strat_ai_target_list_invalidate_by_id(int player, int target_id)` |  |
| `0x004ec58a` | `llm_strat_ai_target_list_refresh_mothers` | `int llm_strat_ai_target_list_refresh_mothers(int player)` |  |
| `0x004ec5ea` | `llm_strat_ai_target_list_scan_visible_enemies` | `int llm_strat_ai_target_list_scan_visible_enemies(uint player)` |  |
| `0x004ec73c` | `llm_strat_ai_group_seed_resolved_target` | `bool llm_strat_ai_group_seed_resolved_target(int player_id, int group_index)` |  |
| `0x004ec792` | `llm_strat_ai_scan_target_sort_cmp` | `int llm_strat_ai_scan_target_sort_cmp(void * a, void * b)` |  |
| `0x004ec7bb` | `llm_strat_ai_scan_target_list_sort` | `void llm_strat_ai_scan_target_list_sort(void * base, uint count)` |  |
| `0x004ec7d9` | `llm_strat_ai_attack_candidate_add` | `void llm_strat_ai_attack_candidate_add(int player, int unit_index)` |  |
| `0x004ec84d` | `llm_strat_ai_unit_should_abandon_target` | `int llm_strat_ai_unit_should_abandon_target(uint player, int unit_index)` |  |
| `0x004ed130` | `llm_strat_ai_scan_targets_for_engage` | `int llm_strat_ai_scan_targets_for_engage(int player_idx, int target_unit_id)` |  |
| `0x004ed1ae` | `llm_strat_ai_scan_targets_by_owner_and_id_for_engage` | `int llm_strat_ai_scan_targets_by_owner_and_id_for_engage(int player_idx, int owner_flags_key, int target_unit_id)` | conf:med todo:dead |
| `0x004ed231` | `llm_strat_ai_scan_targets_by_type_for_engage` | `int llm_strat_ai_scan_targets_by_type_for_engage(int player_idx, int target_type_or_group)` | conf:med todo:dead |
| `0x004ed2ad` | `llm_strat_ai_scan_target_list_for_engage_candidates` | `int llm_strat_ai_scan_target_list_for_engage_candidates(int player, ushort ai_group_mask)` |  |
| `0x004ed34a` | `llm_strat_ai_scan_enemy_turrets_in_weapon_range` | `int llm_strat_ai_scan_enemy_turrets_in_weapon_range(uint player_idx, int unit_idx)` | todo:dead |
| `0x004ed495` | `llm_strat_ai_scan_spiral_ring_for_engage_candidates` | `int llm_strat_ai_scan_spiral_ring_for_engage_candidates(int player, int x, int y, int ring_index, uint target_mask)` |  |
| `0x004ed5f0` | `llm_strat_ai_scan_area_for_engage_candidates` | `int llm_strat_ai_scan_area_for_engage_candidates(int ai_player, uint col_start, uint row_start, uint col_end, uint row_end, byte owner_class_mask)` | todo:dead |
| `0x004ed74b` | `llm_strat_ai_unit_scan_engage_candidates_at_max_weapon_range` | `int llm_strat_ai_unit_scan_engage_candidates_at_max_weapon_range(int player, int unit_id, uint target_mask)` | conf:med todo:dead |
| `0x004ed78d` | `llm_strat_ai_unit_scan_engage_candidates_in_range` | `int llm_strat_ai_unit_scan_engage_candidates_in_range(int player, int unit_id, uint target_mask)` | conf:med |
| `0x004ed7ec` | `llm_strat_ai_scan_targets_by_capability_mask_for_engage` | `int llm_strat_ai_scan_targets_by_capability_mask_for_engage(int player_idx, uint capability_mask)` | conf:med todo:dead |
| `0x004eda5a` | `llm_strat_ai_scan_for_engage_candidates` | `int llm_strat_ai_scan_for_engage_candidates(int ai_player, uint col_start, uint row_start, uint col_end, uint row_end, int ring_radius, uint owner_class_mask)` | conf:low todo:dead |
| `0x004eda8b` | `llm_strat_ai_group_dispatch_action_flags` | `undefined4 llm_strat_ai_group_dispatch_action_flags(int player, int ai_group_idx, int unit_idx, uint action_flags)` | todo:dead |
| `0x004edcca` | `llm_strat_ai_engage_select_and_commit` | `int llm_strat_ai_engage_select_and_commit(int player, int unit_index, int use_alt_commit)` |  |
| `0x004ede8b` | `llm_strat_ai_engage_filter_and_commit_target` | `int llm_strat_ai_engage_filter_and_commit_target(uint attacker_ref, undefined4 context)` | conf:med |
| `0x004ee05c` | `llm_strat_ai_unit_select_attack_target` | `void llm_strat_ai_unit_select_attack_target(uint player, int unit_id)` | todo:dead |
| `0x004ee30b` | `llm_strat_ai_active_unit_tick` | `void llm_strat_ai_active_unit_tick(int player)` |  |
| `0x004ee4c2` | `llm_strat_unit_is_idle_or_patrolling` | `int llm_strat_unit_is_idle_or_patrolling(int player, int unit_index)` | conf:med |
| `0x004ee50e` | `llm_strat_unit_passive_engage_tick` | `void llm_strat_unit_passive_engage_tick(int player)` |  |
| `0x004ee7f5` | `llm_map_debug_build_player_terrain_ascii_row` | `void llm_map_debug_build_player_terrain_ascii_row(int player_idx)` | conf:med |
| `0x004ee8df` | `llm_debug_log_msg_stub` | `void llm_debug_log_msg_stub(char * msg)` | conf:med |
| `0x004ee8ea` | `llm_strat_ai_group_report_unit_states` | `void llm_strat_ai_group_report_unit_states(int player_id)` | conf:low todo:dead |
| `0x004eec6c` | `llm_strat_ai_tick_all_groups` | `void llm_strat_ai_tick_all_groups(void)` | conf:med todo:dead |
| `0x004ef24c` | `llm_res_bank_get_file_size` | `int llm_res_bank_get_file_size(char * filename, uint passthrough_tag)` | conf:med todo:globals todo:struct |
| `0x004f11f4` | `llm_crt_console_ctrl_handler_install` | `int llm_crt_console_ctrl_handler_install(void)` | conf:med |
| `0x004f23fb` | `llm_crt_printf_g_trim_trailing_zeros` | `void llm_crt_printf_g_trim_trailing_zeros(char * numbuf, int conv_state)` | conf:med todo:struct |
| `0x004f2ad8` | `llm_debug_trace_printf` | `void llm_debug_trace_printf(char * fmt, ...)` |  |
| `0x004f2b78` | `llm_debug_trace_flush` | `void llm_debug_trace_flush(void)` |  |
| `0x004f2bb8` | `llm_crt_console_set_cursor_ioctl` | `undefined llm_crt_console_set_cursor_ioctl(void)` | conf:low todo:proto |
| `0x004f2c18` | `llm_debug_trace_printf_stub` | `void llm_debug_trace_printf_stub(char * fmt, ...)` |  |
| `0x004f46a7` | `llm_crt_thread_entry_trampoline` | `void llm_crt_thread_entry_trampoline(void * thread_start_info)` | conf:high todo:proto |
| `0x004f49cc` | `llm_lzw_emit_code` | `void llm_lzw_emit_code(undefined4 param_1, undefined4 param_2)` | rev:verified todo:proto |
| `0x004f49fd` | `llm_lzw_encode_core` | `void llm_lzw_encode_core(void)` | conf:low rev:verified todo:globals todo:proto |
| `0x004f4b31` | `llm_lzw_find_match` | `int llm_lzw_find_match(undefined4 param_1, int param_2)` | rev:verified todo:proto |
| `0x0055b665` | `llm_pf_expand_step` | `undefined llm_pf_expand_step(void)` |  |
| `0x0055bd1d` | `llm_pf_expand_wavefront` | `undefined llm_pf_expand_wavefront(void)` |  |
| `0x0055bd7f` | `llm_pf_select_resolution` | `void llm_pf_select_resolution(void * ctx)` |  |
| `0x0055e1cc` | `llm_pf_run` | `void llm_pf_run(void * ctx)` |  |
| `0x0055e233` | `llm_pf_seed_goals` | `undefined llm_pf_seed_goals(void)` |  |
| `0x00580223` | `llm_strat_pathfind_flood_expand_ring` | `void llm_strat_pathfind_flood_expand_ring(void)` | conf:med todo:globals |
| `0x005808a9` | `llm_strat_pathfind_flood_search_loop` | `void llm_strat_pathfind_flood_search_loop(void)` | conf:med |
| `0x0058090b` | `llm_strat_pathfind_route_extract` | `short llm_strat_pathfind_route_extract(void)` | conf:high |
| `0x00580aae` | `llm_strat_pathfind_route_backtrace_recurse` | `undefined llm_strat_pathfind_route_backtrace_recurse(void)` | conf:med todo:proto |
| `0x00580e29` | `llm_strat_pathfind_flood_select_grid_level` | `void llm_strat_pathfind_flood_select_grid_level(void * ctx)` | conf:high |
| `0x00583416` | `llm_strat_pathfind_flood_search_driver` | `void llm_strat_pathfind_flood_search_driver(void * ctx)` | conf:med todo:struct |
| `0x005834bf` | `llm_strat_pathfind_flood_seed_from_route` | `undefined llm_strat_pathfind_flood_seed_from_route(void)` | conf:high conf:low todo:globals todo:proto todo:struct |
| `0x00583716` | `llm_strat_pathfind_flood_frontier_step` | `void llm_strat_pathfind_flood_frontier_step(void)` | conf:high |
| `0x00583820` | `llm_strat_pathfind_backfill_flood_step` | `void llm_strat_pathfind_backfill_flood_step(void)` | conf:med |
| `0x0058411b` | `llm_strat_pathfind_backfill_resolve_target` | `void llm_strat_pathfind_backfill_resolve_target(void)` | conf:high |
| `0x005841f3` | `llm_strat_pathfind_clearance_check` | `ushort llm_strat_pathfind_clearance_check(ushort coord_packed_row_col)` | conf:high |
| `0x00669f90` | `llm_strat_toroidal_dist_sq` | `uint llm_strat_toroidal_dist_sq(int x1, int y1, int x2, int y2)` |  |
| `0x00669fe8` | `llm_strat_tile_midpoint_wrapped` | `void llm_strat_tile_midpoint_wrapped(tile_coord x0, tile_coord y0, tile_coord x1, tile_coord y1, tile_coord * out_x, tile_coord * out_y)` | conf:med |
| `0x0066a9ac` | `llm_strat_trace_greedy_path` | `byte * llm_strat_trace_greedy_path(int start_col, int start_row, int mode, int goal_col, int goal_row, int heading)` |  |
| `0x0066b376` | `llm_strat_pathtrace_remove_loops` | `int llm_strat_pathtrace_remove_loops(void)` |  |
| `0x0066b415` | `llm_strat_pathtrace_dirs_get` | `byte * llm_strat_pathtrace_dirs_get(void)` | conf:high |
| `0x0066b4a1` | `llm_strat_pathtrace_normalize_repeat_dir_table` | `void llm_strat_pathtrace_normalize_repeat_dir_table(void)` | conf:med todo:enum |

### Named globals
| Address | Name | Type |
| --- | --- | --- |
| `0x00453e2d` | `_G_LLM_STRAT_DEPLOY_FORMATION_STENCIL` | `byte[49]` |
| `0x004672af` | `_G_LLM_STRAT_ORDER_BLDG_SWITCH_SCAN` | `byte[25]` |
| `0x004672c8` | `_G_LLM_STRAT_ORDER_BLDG_SWITCH_TARGETS` | `pointer32[26]` |
| `0x0046961f` | `_G_LLM_STRAT_ORDER_ADMIN_SWITCH_SCAN` | `byte[22]` |
| `0x00469635` | `_G_LLM_STRAT_ORDER_ADMIN_SWITCH_TARGETS` | `pointer32[23]` |
| `0x005000e0` | `_G_LLM_STR_DASH_DASH` | `wchar_t[3]` |
| `0x005000e6` | `_G_LLM_STR_SLASH` | `wchar_t[2]` |
| `0x005000ee` | `_G_LLM_UI_RESOURCE_STOCK_FILL_WARN_FRACTION` | `double` |
| `0x005000fc` | `_G_LLM_STR_UNDERSCORE` | `wchar_t[1]` |
| `0x005000fe` | `_G_LLM_CONST_SECONDS_PER_HOUR` | `double` |
| `0x00500106` | `_G_LLM_STR_COLON` | `wchar_t[2]` |
| `0x0050010a` | `_G_LLM_STR_PERIOD` | `wchar_t[2]` |
| `0x0050010e` | `_G_LLM_STR_PERCENT` | `wchar_t[2]` |
| `0x00500112` | `_G_LLM_STR_PAREN_OPEN` | `wchar_t[2]` |
| `0x00500116` | `_G_LLM_STR_PAREN_CLOSE_SLASH` | `wchar_t[3]` |
| `0x0050011c` | `_G_LLM_UI_IDLE_POP_HOUSING_WARN_FRACTION` | `double` |
| `0x00500124` | `_G_LLM_CHAT_EMPTY_STR` | `undefined1` |
| `0x00500125` | `_G_LLM_UI_PROGRESS_PERCENT_SCALE` | `double` |
| `0x0050012d` | `_G_LLM_UI_CAPACITY_WARN_RATIO` | `double` |
| `0x00500135` | `_G_LLM_UI_TEXT_DASHES` | `TerminatedUnicode` |
| `0x0050013d` | `_G_LLM_STR_PAREN_CLOSE` | `wchar_t[2]` |
| `0x00500141` | `_G_LLM_CONST_PERCENT_SCALE_500141` | `double` |
| `0x0050019e` | `_G_LLM_STRAT_ORDER_ACK_VOICE_COOLDOWN_SEC` | `double` |
| `0x005001a6` | `_G_LLM_STRAT_RACE_ALERT_SOUND_COOLDOWN_SEC` | `double` |
| `0x005001ae` | `_G_LLM_STRAT_RACE_ALERT_TEXT_COOLDOWN_SEC` | `double` |
| `0x005001b6` | `_G_LLM_STRAT_RELOAD_TICK_BACKDATE` | `double` |
| `0x005001be` | `_G_LLM_STRAT_RELOAD_CYCLE_BACKDATE` | `double` |
| `0x00500204` | `_G_LLM_GAME_CLOCK_TICKS_PER_SECOND` | `double` |
| `0x005002f8` | `_G_LLM_TACT_UNIT_ANIM_FRAME_INTERVAL_BASE` | `double` |
| `0x00500308` | `_G_LLM_TACT_UNIT_ANIM_STEP_SEC_1` | `double` |
| `0x00500310` | `_G_LLM_TACT_UNIT_ANIM_JITTER_SCALE_1` | `double` |
| `0x00500318` | `_G_LLM_TACT_UNIT_ANIM_STEP_SEC_2` | `double` |
| `0x00500320` | `_G_LLM_TACT_UNIT_ANIM_JITTER_SCALE_2` | `double` |
| `0x00500328` | `_G_LLM_CONST_DBL_1DIV32_500328` | `double` |
| `0x00500330` | `_G_LLM_CONST_DBL_24_500330` | `double` |
| `0x0050045a` | `_G_LLM_MATH_HALF_TURN_DEG` | `double` |
| `0x0050046a` | `_G_LLM_TACT_AMBIENT_SND_MIN_INTERVAL_SEC` | `double` |
| `0x00500484` | `_G_LLM_CONST_DBL_1DIV32_500484` | `double` |
| `0x0050048c` | `_G_LLM_CONST_DBL_24_50048C` | `double` |
| `0x00500494` | `_G_LLM_CONST_DBL_1DIV32_500494` | `double` |
| `0x0050049c` | `_G_LLM_CONST_DBL_24_50049C` | `double` |
| `0x005004a4` | `_G_LLM_TACT_DOOR_OPEN_HOLD_TIME_SEC` | `double` |
| `0x005004ac` | `_G_LLM_TACT_UNIT_WANDER_RETRY_INTERVAL` | `double` |
| `0x005004d8` | `_G_LLM_TACT_SIDEBAR_FMT_UNIT_ID` | `unicode` |
| `0x005004e0` | `_G_LLM_TACT_SIDEBAR_FMT_UNIT_NAME` | `unicode` |
| `0x005004f0` | `_G_LLM_TACT_KW_STOP` | `char[5]` |
| `0x005004f5` | `_G_LLM_TACT_KW_TIME` | `string` |
| `0x005004fb` | `_G_LLM_TACT_KW_SEE_ENEMY` | `string` |
| `0x00500505` | `_G_LLM_TACT_KW_MAP` | `char[4]` |
| `0x00500509` | `_G_LLM_TACT_KW_BANK` | `char[5]` |
| `0x0050050e` | `_G_LLM_TACT_KW_GROUND` | `string` |
| `0x00500515` | `_G_LLM_TACT_KW_CHARACTER` | `string` |
| `0x0050051f` | `_G_LLM_TACT_KW_GUN` | `char[5]` |
| `0x00500524` | `_G_LLM_TACT_KW_EXPLOSION` | `string` |
| `0x0050052e` | `_G_LLM_TACT_KW_DISPOSITION` | `string` |
| `0x0050053a` | `_G_LLM_TACT_KW_DOOR` | `char[5]` |
| `0x0050053f` | `_G_LLM_TACT_KW_TELEPORT` | `string` |
| `0x00500549` | `_G_LLM_TACT_KW_DETONATION` | `string` |
| `0x00500554` | `_G_LLM_TACT_KW_QUIT` | `string` |
| `0x0050055a` | `_G_LLM_TACT_KW_TARGET` | `string` |
| `0x00500562` | `_G_LLM_TACT_KW_NAME` | `char[5]` |
| `0x00500567` | `_G_LLM_TACT_KW_PANEL` | `string` |
| `0x00500593` | `_G_LLM_TACT_KW_WHO` | `char[4]` |
| `0x00500597` | `_G_LLM_TACT_KW_ANGLE_SEE` | `string` |
| `0x005005a1` | `_G_LLM_TACT_KW_DISTANCE_SEE` | `string` |
| `0x005005ae` | `_G_LLM_TACT_KW_FIRST_FRAME` | `string` |
| `0x005005ba` | `_G_LLM_TACT_KW_NUMBER_GUN1` | `string` |
| `0x005005c6` | `_G_LLM_TACT_KW_NUMBER_GUN2` | `string` |
| `0x005005d2` | `_G_LLM_TACT_KW_HEIGHT_GUN1` | `string` |
| `0x005005de` | `_G_LLM_TACT_KW_HEIGHT_GUN2` | `string` |
| `0x005005ea` | `_G_LLM_TACT_KW_KNEEL_GUN1` | `string` |
| `0x005005f5` | `_G_LLM_TACT_KW_KNEEL_GUN2` | `string` |
| `0x00500600` | `_G_LLM_TACT_KW_ENERGY` | `string` |
| `0x00500607` | `_G_LLM_TACT_KW_SPEED` | `string` |
| `0x0050060d` | `_G_LLM_TACT_KW_ROTATE` | `string` |
| `0x00500614` | `_G_LLM_TACT_KW_KNEEL` | `string` |
| `0x0050061c` | `_G_LLM_TACT_KW_DEATH` | `string` |
| `0x00500622` | `_G_LLM_TACT_KW_MINE` | `char[5]` |
| `0x00500627` | `_G_LLM_TACT_KW_RUN` | `string` |
| `0x0050062b` | `_G_LLM_TACT_KW_FRAMES` | `string` |
| `0x00500632` | `_G_LLM_TACT_KW_SPEED_GUN` | `string` |
| `0x0050063a` | `_G_LLM_TACT_KW_SPEED_FIRE` | `string` |
| `0x00500645` | `_G_LLM_TACT_KW_BULLETS` | `string` |
| `0x0050064d` | `_G_LLM_TACT_KW_REPEAT` | `string` |
| `0x00500654` | `_G_LLM_TACT_KW_MAGAZINES` | `string` |
| `0x0050065e` | `_G_LLM_TACT_KW_PRECISE` | `string` |
| `0x00500668` | `_G_LLM_TACT_KW_PRECISE_KNEEL` | `string` |
| `0x00500676` | `_G_LLM_TACT_KW_DIRECT` | `string` |
| `0x0050067d` | `_G_LLM_TACT_KW_POWER` | `string` |
| `0x00500683` | `_G_LLM_TACT_KW_COLISION1` | `string` |
| `0x0050068d` | `_G_LLM_TACT_COLISION1_BIAS` | `double` |
| `0x00500695` | `_G_LLM_TACT_KW_COLISION2` | `char[10]` |
| `0x0050069f` | `_G_LLM_TACT_COLISION2_BIAS` | `double` |
| `0x005006a7` | `_G_LLM_TACT_KW_RANGE_MAX` | `char[10]` |
| `0x005006b1` | `_G_LLM_TACT_KW_RANGE_MIN` | `string` |
| `0x005006bb` | `_G_LLM_TACT_KW_SOUND` | `string` |
| `0x005006c1` | `_G_LLM_TACT_KW_RANGE_KILL` | `string` |
| `0x005006cc` | `_G_LLM_TACT_KW_DIRECT_COLON` | `string` |
| `0x005006d4` | `_G_LLM_TACT_KW_DEFENSE_NONE` | `string` |
| `0x005006e1` | `_G_LLM_TACT_KW_DEFENSE_GUARD1` | `string` |
| `0x005006f0` | `_G_LLM_TACT_KW_DEFENSE_GUARD2` | `string` |
| `0x005006ff` | `_G_LLM_TACT_KW_DEFENSE_ATTACK` | `string` |
| `0x0050070e` | `_G_LLM_TACT_KW_DIRECT_LEFTRIGHT` | `string` |
| `0x0050071f` | `_G_LLM_TACT_KW_DIRECT_RIGHTLEFT` | `string` |
| `0x00500730` | `_G_LLM_TACT_KW_DIRECT_NORMAL` | `string` |
| `0x0050073e` | `_G_LLM_TACT_KW_LEFT` | `char[5]` |
| `0x00500743` | `_G_LLM_TACT_KW_RIGHT` | `string` |
| `0x00500749` | `_G_LLM_TACT_KW_XY` | `char[4]` |
| `0x0050074d` | `_G_LLM_TACT_KW_RANDOM` | `string` |
| `0x00500754` | `_G_LLM_TACT_KW_SEQUENCE` | `string` |
| `0x0050075d` | `_G_LLM_TACT_KW_NO_ENEMY` | `string` |
| `0x00500766` | `_G_LLM_TACT_KW_START` | `string` |
| `0x0050076c` | `_G_LLM_TACT_KW_WHERE` | `string` |
| `0x00500772` | `_G_LLM_TACT_KW_ASSOCIATION` | `string` |
| `0x0050077e` | `_G_LLM_TACT_TELEPORT_DEATH_BIAS` | `double` |
| `0x00500786` | `_G_LLM_TACT_KW_TIMER` | `char[6]` |
| `0x0050078c` | `_G_LLM_DECIMAL_DIGIT_MUL` | `double` |
| `0x00500794` | `_G_LLM_DECIMAL_DIGIT_DIV` | `double` |
| `0x0050079c` | `_G_LLM_TACT_KW_MOVE` | `undefined` |
| `0x005007a1` | `_G_LLM_TACT_KW_WALK` | `undefined` |
| `0x005007a6` | `_G_LLM_TACT_KW_WAIT` | `undefined` |
| `0x005007ab` | `_G_LLM_TACT_KW_TELE` | `undefined` |
| `0x005007b0` | `_G_LLM_TACT_KW_DEFENSE_SNIPER` | `string` |
| `0x005007bf` | `_G_LLM_TACT_CFG_FRAME_TOKEN_DELIM` | `undefined` |
| `0x005007c1` | `_G_LLM_TACT_CFG_FRAME_TABLE_OPEN` | `undefined` |
| `0x005007c3` | `_G_LLM_TACT_CFG_FRAME_LIST_DELIM` | `undefined` |
| `0x005007c6` | `_G_LLM_TACT_CFG_FRAME_TABLE_CLOSE` | `undefined` |
| `0x005009e0` | `_G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_SECS` | `double` |
| `0x005009f8` | `_G_LLM_STRAT_LOCKSTEP_STEP_MAX` | `double` |
| `0x00500a00` | `_G_LLM_STRAT_LOCKSTEP_STEP_GROW_MUL` | `double` |
| `0x00500a08` | `_G_LLM_STRAT_LOCKSTEP_STEP_SHRINK_DIV` | `double` |
| `0x00500a10` | `_G_LLM_STRAT_LOCKSTEP_STEP_MIN_FPS_NUM_CMP` | `double` |
| `0x00500a18` | `_G_LLM_STRAT_LOCKSTEP_STEP_MIN_FPS_NUM_SET` | `double` |
| `0x00500a20` | `_G_LLM_STRAT_LOCKSTEP_ADAPT_INTERVAL_SECS` | `double` |
| `0x00500a28` | `_G_LLM_STRAT_FPS_WINDOW_NUM` | `double` |
| `0x00500a30` | `_G_LLM_STRAT_SUBTICK_A_PERIOD` | `double` |
| `0x00500a38` | `_G_LLM_STRAT_SUBTICK_A_PERIOD_POS` | `double` |
| `0x00500a40` | `_G_LLM_STRAT_SUBTICK_A_PERIOD_NEG` | `double` |
| `0x00500a48` | `_G_LLM_STRAT_SUBTICK_B_PERIOD` | `double` |
| `0x00500a50` | `_G_LLM_STRAT_SUBTICK_B_PERIOD_POS` | `double` |
| `0x00500a58` | `_G_LLM_STRAT_SUBTICK_B_PERIOD_NEG` | `double` |
| `0x00500a60` | `_G_LLM_STRAT_PROD_CHECK_PERIOD_NEG` | `double` |
| `0x00500a68` | `_G_LLM_GAME_PLANET_AVAILABLE_NOTIFY_DELAY` | `double` |
| `0x00500a70` | `_G_LLM_STRAT_UPGRADE_SPEED_PERCENT_DIVISOR` | `double` |
| `0x00500a78` | `_G_LLM_STRAT_UPGRADE_MSG_SEP_BEFORE_CATEGORY` | `wchar16` |
| `0x00500a7e` | `_G_LLM_STRAT_UPGRADE_MSG_SEP_BEFORE_NAME` | `wchar16` |
| `0x00500a84` | `_G_LLM_STRAT_UPGRADE_MSG_CLAUSE_SEP_FIRST` | `wchar16` |
| `0x00500a8c` | `_G_LLM_STRAT_UPGRADE_MSG_CLAUSE_SEP_NEXT` | `wchar16` |
| `0x00500a92` | `_G_LLM_STRAT_UPGRADE_MSG_TRAILER` | `wchar16` |
| `0x00500a9e` | `_G_LLM_CHAT_HORIZON_EXTEND_MIN_CLOCK` | `double` |
| `0x00500aa6` | `_G_LLM_CHAT_HORIZON_EXTEND_DELTA` | `double` |
| `0x00500aae` | `_G_LLM_EMPTY_STR` | `undefined` |
| `0x00500aaf` | `_G_LLM_DEBUG_CONSOLE_ACTIVATE_MIN_CLOCK` | `double` |
| `0x00500ad7` | `_G_LLM_CAM_ZOOM_PRIME_AD7` | `double` |
| `0x00500adf` | `_G_LLM_CAM_ZOOM_PRIME_ADF` | `double` |
| `0x00500ae7` | `_G_LLM_CAM_SCROLL_LEFT_PRIME_AE7` | `double` |
| `0x00500aef` | `_G_LLM_CAM_SCROLL_RIGHT_PRIME_AEF` | `double` |
| `0x00500af7` | `_G_LLM_CAM_SCROLL_UP_PRIME_AF7` | `double` |
| `0x00500aff` | `_G_LLM_CAM_SCROLL_DOWN_PRIME_AFF` | `double` |
| `0x00500b07` | `_G_LLM_CAM_SCROLL_UP_PRIME_B07` | `double` |
| `0x00500b0f` | `_G_LLM_CAM_SCROLL_LEFT_PRIME_B0F` | `double` |
| `0x00500b17` | `_G_LLM_CAM_SCROLL_RIGHT_PRIME_B17` | `double` |
| `0x00500b1f` | `_G_LLM_CAM_SCROLL_DOWN_PRIME_B1F` | `double` |
| `0x00500b27` | `_G_LLM_CAM_SCROLL_RIGHT_PRIME_B27` | `double` |
| `0x00500b2f` | `_G_LLM_CAM_SCROLL_LEFT_PRIME_B2F` | `double` |
| `0x00500b37` | `_G_LLM_CAM_SCROLL_UP_PRIME_B37` | `double` |
| `0x00500b3f` | `_G_LLM_CAM_SCROLL_DOWN_PRIME_B3F` | `double` |
| `0x00500b47` | `_G_LLM_CAM_SCROLL_DOWN_PRIME_B47` | `double` |
| `0x00500b4f` | `_G_LLM_CAM_SCROLL_LEFT_PRIME_B4F` | `double` |
| `0x00500b57` | `_G_LLM_CAM_SCROLL_RIGHT_PRIME_B57` | `double` |
| `0x00500b5f` | `_G_LLM_CAM_SCROLL_UP_PRIME_B5F` | `double` |
| `0x00500b67` | `_G_LLM_MSG_DISPLAY_DURATION` | `double` |
| `0x00500b6f` | `_G_LLM_MSG_AGE_CLEAR_OFFSET` | `double` |
| `0x00500b77` | `_G_LLM_MSG_AGE_STEP_OFFSET` | `double` |
| `0x00500b7f` | `_G_LLM_CAM_SCROLL_LEFT_HOLD_INTERVAL` | `double` |
| `0x00500b87` | `_G_LLM_CAM_SCROLL_LEFT_HOLD_STEP` | `double` |
| `0x00500b8f` | `_G_LLM_CAM_SCROLL_UP_HOLD_INTERVAL` | `double` |
| `0x00500b97` | `_G_LLM_CAM_SCROLL_UP_HOLD_STEP` | `double` |
| `0x00500b9f` | `_G_LLM_CAM_SCROLL_RIGHT_HOLD_INTERVAL` | `double` |
| `0x00500ba7` | `_G_LLM_CAM_SCROLL_RIGHT_HOLD_STEP` | `double` |
| `0x00500baf` | `_G_LLM_CAM_SCROLL_DOWN_HOLD_INTERVAL` | `double` |
| `0x00500bb7` | `_G_LLM_CAM_SCROLL_DOWN_HOLD_STEP` | `double` |
| `0x00500bbf` | `_G_LLM_CAM_SCROLL_ZOOM_HOLD_INTERVAL` | `double` |
| `0x00500bc7` | `_G_LLM_CAM_SCROLL_ZOOM_HOLD_STEP` | `double` |
| `0x00500bcf` | `_G_LLM_CAM_EDGE_LEFT_PRIME_BCF` | `double` |
| `0x00500bd7` | `_G_LLM_CAM_EDGE_UP_PRIME_BD7` | `double` |
| `0x00500bdf` | `_G_LLM_CAM_EDGE_DOWN_PRIME_BDF` | `double` |
| `0x00500be7` | `_G_LLM_CAM_EDGE_UP_PRIME_BE7` | `double` |
| `0x00500bef` | `_G_LLM_CAM_EDGE_LEFT_PRIME_BEF` | `double` |
| `0x00500bf7` | `_G_LLM_CAM_EDGE_RIGHT_PRIME_BF7` | `double` |
| `0x00500bff` | `_G_LLM_CAM_EDGE_RIGHT_PRIME_BFF` | `double` |
| `0x00500c07` | `_G_LLM_CAM_EDGE_UP_PRIME_C07` | `double` |
| `0x00500c0f` | `_G_LLM_CAM_EDGE_DOWN_PRIME_C0F` | `double` |
| `0x00500c17` | `_G_LLM_CAM_EDGE_DOWN_PRIME_C17` | `double` |
| `0x00500c1f` | `_G_LLM_CAM_EDGE_LEFT_PRIME_C1F` | `double` |
| `0x00500c27` | `_G_LLM_CAM_EDGE_RIGHT_PRIME_C27` | `double` |
| `0x00500c2f` | `_G_LLM_RMB_DBLCLICK_WINDOW_C2F` | `double` |
| `0x00500c37` | `_G_LLM_RMB_DBLCLICK_WINDOW_C37` | `double` |
| `0x00500c3f` | `_G_LLM_RMB_DBLCLICK_WINDOW_C3F` | `double` |
| `0x00500c47` | `_G_LLM_RMB_DBLCLICK_WINDOW_C47` | `double` |
| `0x00500c4f` | `_G_LLM_RMB_DBLCLICK_WINDOW_C4F` | `double` |
| `0x00500c57` | `_G_LLM_RMB_DBLCLICK_WINDOW_C57` | `double` |
| `0x00500c5f` | `_G_LLM_RMB_DBLCLICK_WINDOW_C5F` | `double` |
| `0x00500c67` | `_G_LLM_CAM_EDGE_LEFT_INTERVAL` | `double` |
| `0x00500c6f` | `_G_LLM_CAM_EDGE_LEFT_STEP` | `double` |
| `0x00500c77` | `_G_LLM_CAM_EDGE_UP_INTERVAL` | `double` |
| `0x00500c7f` | `_G_LLM_CAM_EDGE_UP_STEP` | `double` |
| `0x00500c87` | `_G_LLM_CAM_EDGE_RIGHT_INTERVAL` | `double` |
| `0x00500c8f` | `_G_LLM_CAM_EDGE_RIGHT_STEP` | `double` |
| `0x00500c97` | `_G_LLM_CAM_EDGE_DOWN_INTERVAL` | `double` |
| `0x00500c9f` | `_G_LLM_CAM_EDGE_DOWN_STEP` | `double` |
| `0x00500ca7` | `_G_LLM_FMT_HOVER_NAME` | `unicode` |
| `0x00500cb7` | `_G_LLM_CHAT_PROMPT_PREFIX` | `undefined` |
| `0x00500cbd` | `_G_LLM_CHAT_TARGET_SUFFIX` | `undefined` |
| `0x00500cfd` | `_G_LLM_STRAT_UNIT_PREDICT_COORDS_ROUND_BIAS` | `double` |
| `0x00500d05` | `_G_LLM_STRAT_BLDG_BUILD_PROGRESS_PERCENT_SCALE_STATE_0X64` | `double` |
| `0x00500d0d` | `_G_LLM_STRAT_BLDG_BUILD_PROGRESS_PERCENT_SCALE_STATE_0X6B` | `double` |
| `0x00500d15` | `_G_LLM_STRAT_BLDG_ENERGY_PERCENT_SCALE` | `double` |
| `0x00500d1d` | `_G_LLM_STRAT_BLDG_UPGRADE_PROGRESS_PERCENT_SCALE` | `double` |
| `0x00500d25` | `_G_LLM_STRAT_BLDG_PRODUCTION_PROGRESS_PERCENT_SCALE` | `double` |
| `0x00500d2d` | `_G_LLM_STRAT_BLDG_PROJECT_PROGRESS_PERCENT_SCALE` | `double` |
| `0x00500d4b` | `_G_LLM_MATH_PERCENT_DIVISOR` | `double` |
| `0x00500d53` | `_G_LLM_CHEAT_DEFAULT_JOKE_STR` | `WCHAR[26]` |
| `0x00500d87` | `_G_LLM_CONST_DBL_60_500D87` | `double` |
| `0x00500d8f` | `_G_LLM_CONST_DBL_1_15_500D8F` | `double` |
| `0x00500d97` | `_G_LLM_CONST_DBL_1_1_500D97` | `double` |
| `0x00500ddd` | `_G_LLM_STRAT_LOCKSTEP_ADAPT_INTERVAL_DBG` | `double` |
| `0x00500f0c` | `_G_LLM_STRAT_BLDG_MAIN_BASE_DAMAGE_MULT` | `double` |
| `0x00500f14` | `_G_LLM_STRAT_BLDG_LOST_FEEDBACK_COOLDOWN_MOTHER` | `double` |
| `0x00500f1c` | `_G_LLM_STRAT_BLDG_LOST_FEEDBACK_COOLDOWN_OTHER` | `double` |
| `0x00500f24` | `_G_LLM_STRAT_UNIT_LOST_FEEDBACK_COOLDOWN` | `double` |
| `0x00500f40` | `_G_LLM_STRAT_UNIT_ENERGY_STATUS_PERCENT_SCALE` | `double` |
| `0x00500f48` | `_G_LLM_STRAT_BLDG_ENERGY_STATUS_PERCENT_SCALE` | `double` |
| `0x00500f50` | `_G_LLM_STRAT_SQUAD_ASSAULT_POWER_PERCENT_SCALE` | `double` |
| `0x00500f58` | `_G_LLM_STRAT_BLDG_ENERGY_TO_PERCENT_SCALE` | `double` |
| `0x00500f60` | `_G_LLM_STRAT_BLDG_ENERGY_PERCENT_TO_ABS_SCALE` | `double` |
| `0x00500f68` | `_G_LLM_FX_DEBRIS_SCALE_DIVISOR` | `float` |
| `0x00500fca` | `_G_LLM_STRAT_UNIT_ENERGY_BAR_PERCENT_SCALE_SELECTED` | `double` |
| `0x00500fd2` | `_G_LLM_STRAT_BLDG_ENERGY_BAR_PERCENT_SCALE_SELECTED` | `double` |
| `0x00500fda` | `_G_LLM_STRAT_BLDG_ENERGY_BAR_PERCENT_SCALE_CLICK_TARGET` | `double` |
| `0x00500fe2` | `_G_LLM_STRAT_UNIT_ENERGY_BAR_PERCENT_SCALE_CLICK_TARGET` | `double` |
| `0x00500fea` | `_G_LLM_STRAT_UNIT_SHADOW_ELEVATION_SCALE` | `double` |
| `0x00500ff2` | `_G_LLM_STRAT_DOCKED_UNIT_AIR_ANIM_OFFSET_SCALE_ADD` | `double` |
| `0x00500ffa` | `_G_LLM_STRAT_DOCKED_UNIT_AIR_ANIM_OFFSET_SCALE_SUB` | `double` |
| `0x00501002` | `_G_LLM_STRAT_UNIT_ELEVATION_SPRITE_OFFSET_SCALE` | `double` |
| `0x00501031` | `_G_LLM_STRAT_LOCKSTEP_SESSION_ADAPT_DELAY` | `double` |
| `0x00501039` | `_G_LLM_STRAT_NAME_ELLIPSIS_STR` | `TerminatedCString` |
| `0x0050108f` | `_G_LLM_SHARED_EMPTY_WSTRING` | `wchar_t` |
| `0x00501091` | `_G_LLM_STRAT_INVENTION_NAME_PLACEHOLDER` | `string` |
| `0x0050109d` | `_G_LLM_STRAT_EMPTY_NAME_STR` | `string` |
| `0x0050109e` | `_G_LLM_STRAT_STORAGE_STATS_INIT_DELAY` | `double` |
| `0x005010a6` | `_G_LLM_CFG_UNIT_COST2_DIVIDER_0` | `double` |
| `0x005010ae` | `_G_LLM_CFG_UNIT_COST2_DIVIDER_1` | `double` |
| `0x005010b6` | `_G_LLM_CFG_UNIT_COST2_DIVIDER_2` | `double` |
| `0x005010be` | `_G_LLM_CFG_UNIT_COST2_DIVIDER_3` | `double` |
| `0x005010c6` | `_G_LLM_CFG_UNIT_ENERGY2_FACTOR` | `double` |
| `0x005010ce` | `_G_LLM_CFG_BLDG_FRAME_SPRITE_PREFIX` | `char[8]` |
| `0x005010d6` | `_G_LLM_CFG_BLDG_FRAME2_SPRITE_SUFFIX` | `char[2]` |
| `0x005010e0` | `_G_LLM_CFG_BLDG_RESOURCE_DIVIDER_1` | `double` |
| `0x005010e8` | `_G_LLM_CFG_BLDG_RESOURCE_DIVIDER_2` | `double` |
| `0x005010f0` | `_G_LLM_CFG_BLDG_RESOURCE_DIVIDER_3` | `double` |
| `0x005010f8` | `_G_LLM_CFG_BLDG_RESOURCE_DIVIDER_4` | `double` |
| `0x00501100` | `_G_LLM_CFG_BLDG_RESOURCE_DIVIDER_5` | `double` |
| `0x00501108` | `_G_LLM_CFG_BLDG_RESOURCE_DIVIDER_6` | `double` |
| `0x00501118` | `_G_LLM_DEG2RAD_MUL1` | `double` |
| `0x00501120` | `_G_LLM_DEG2RAD_MUL2` | `double` |
| `0x00501128` | `_G_LLM_DEG2RAD_DIV` | `double` |
| `0x0050114c` | `_G_LLM_STRAT_BLDG_INSTANT_CONSTRUCT_CYCLE_PROGRESS_OFFSET` | `double` |
| `0x00501154` | `_G_LLM_STRAT_PROJECTILE_DIST_SCALE` | `double` |
| `0x0050115c` | `_G_LLM_STRAT_PROJECTILE_DIR_Y_SIGN` | `double` |
| `0x005011b6` | `_G_LLM_STRAT_ACTIVITY_PHASE_DIVISOR` | `double` |
| `0x005012d4` | `_G_LLM_STRAT_BLDG_DEATH_HQ_ENERGY_CREDIT` | `double` |
| `0x005012dc` | `_G_LLM_STRAT_BLDG_TO_UNIT_HEADER_ROW_ENERGY_DELTA` | `double` |
| `0x005012e4` | `_G_LLM_STRAT_TURRET_ATTACK_INTERVAL_SCALE` | `double` |
| `0x005012fa` | `_G_LLM_STRAT_HANGAR_RECHARGE_PERIOD` | `double` |
| `0x00501302` | `_G_LLM_STRAT_HANGAR_RECHARGE_PERIOD_NEG` | `double` |
| `0x0050130a` | `_G_LLM_STRAT_DISMANTLE_PROGRESS_DIVISOR` | `double` |
| `0x00501312` | `_G_LLM_STRAT_DEBRIS_BURST_ENERGY_DIVISOR` | `double` |
| `0x0050131a` | `_G_LLM_STRAT_BLDG_DISMANTLE_HQ_ENERGY_CREDIT` | `double` |
| `0x00501322` | `_G_LLM_STRAT_RUBBLE_SIGHT_DECAY_PERIOD` | `double` |
| `0x0050132a` | `_G_LLM_STRAT_RUBBLE_SIGHT_DECAY_PERIOD_NEG` | `double` |
| `0x00501332` | `_G_LLM_STRAT_RUBBLE_CLEANUP_PERIOD` | `double` |
| `0x00501352` | `_G_LLM_STRAT_PROD_RETRY_PERIOD` | `double` |
| `0x0050135a` | `_G_LLM_STRAT_PROD_RETRY_PERIOD_NEG` | `double` |
| `0x00501362` | `_G_LLM_STRAT_MINE_EXTRACT_PERIOD` | `double` |
| `0x0050136a` | `_G_LLM_STRAT_MINE_EXTRACT_PERIOD_NEG` | `double` |
| `0x00501372` | `_G_LLM_STRAT_MINE_RESCAN_PERIOD` | `double` |
| `0x0050137a` | `_G_LLM_STRAT_MINE_RESCAN_PERIOD_NEG` | `double` |
| `0x00501382` | `_G_LLM_STRAT_CHARGE_PIP_LEVEL_SCALE` | `double` |
| `0x0050138a` | `_G_LLM_STRAT_REFUND_ENERGY_FACTOR` | `double` |
| `0x00501392` | `_G_LLM_STRAT_MOTHER_LOST_ESCALATION_INTERVAL` | `double` |
| `0x005013c0` | `_G_LLM_STRAT_MOVE_STEP_SPEED_SCALE_BLEND` | `double` |
| `0x005013c8` | `_G_LLM_STRAT_MOVE_PATH_BLOCKED_RETRY_PERIOD` | `double` |
| `0x005013d0` | `_G_LLM_STRAT_UNIT_DEATH_HQ_ENERGY_CREDIT` | `double` |
| `0x005013d8` | `_G_LLM_STRAT_STORAGE_EXIT_WAIT_OPERATIONAL_BUMP` | `double` |
| `0x005013e0` | `_G_LLM_STRAT_STORAGE_EXIT_WAIT_DEFAULT_BUMP` | `double` |
| `0x005013e8` | `_G_LLM_STRAT_UNIT_EXIT_WALK_SOLDIER_STEP_SCALE` | `double` |
| `0x005013f0` | `_G_LLM_STRAT_UNIT_TAKEOFF_TAXI_STEP_SPEED_MULT` | `double` |
| `0x005013f8` | `_G_LLM_STRAT_UNIT_DOCK_TAXI_STEP_SPEED_MULT` | `double` |
| `0x00501400` | `_G_LLM_STRAT_UNIT_ENTER_WAIT_ACTIVITY_BACKOFF` | `double` |
| `0x00501408` | `_G_LLM_STRAT_UNIT_ENTER_WALK_SOLDIER_STEP_SCALE` | `double` |
| `0x00501410` | `_G_LLM_STRAT_HOVER_STEP_PERIOD_SCALE` | `double` |
| `0x00501418` | `_G_LLM_STRAT_MOVE_PATH_HEADING_SCALE_FAR` | `double` |
| `0x00501420` | `_G_LLM_STRAT_MOVE_PATH_HEADING_SCALE_MID` | `double` |
| `0x00501428` | `_G_LLM_STRAT_UNIT_TAKEOFF_LANDING_STEP_COST_SCALE` | `double` |
| `0x00501430` | `_G_LLM_STRAT_UNIT_TAKEOFF_LANDING_HELIPAD_ACTIVITY_BUMP` | `double` |
| `0x00501442` | `_G_LLM_STRAT_CORPSE_FOW_DECAY_STEP_PERIOD` | `double` |
| `0x0050144a` | `_G_LLM_STRAT_CORPSE_FOW_DECAY_STEP_PERIOD_NEG` | `double` |
| `0x00501452` | `_G_LLM_STRAT_CORPSE_FREE_SLOT_PERIOD` | `double` |
| `0x0050145a` | `_G_LLM_STRAT_GROUP_MARSHAL_NO_PATH_RETRY_PERIOD` | `double` |
| `0x00501462` | `_G_LLM_STRAT_GROUP_MARSHAL_PATH_BLOCKED_RETRY_PERIOD` | `double` |
| `0x0050146a` | `_G_LLM_STRAT_UNIT_REMOVE_SILENT_HQ_ENERGY_CREDIT` | `double` |
| `0x00501472` | `_G_LLM_STRAT_UNIT_STEP_ADJACENT_BUDGET_GATE` | `double` |
| `0x0050147a` | `_G_LLM_STRAT_DMG_SMOKE_LEVEL_SCALE` | `double` |
| `0x00501482` | `_G_LLM_STRAT_UNIT_TEARDOWN_MAPPED_HQ_ENERGY_CREDIT` | `double` |
| `0x0050148a` | `_G_LLM_STRAT_UNIT_TEARDOWN_HQ_ENERGY_CREDIT` | `double` |
| `0x00501492` | `_G_LLM_STRAT_SOLDIER_WALK_DURATION_SCALE` | `double` |
| `0x005014be` | `_G_LLM_STRAT_UNIT_REFUND_HEALTH_FRACTION_SCALE` | `double` |
| `0x005014d2` | `_G_LLM_PROD_SHUTTLE_BOARD_ACTIVITY_BUMP` | `double` |
| `0x005014da` | `_G_LLM_PROD_SHUTTLE_DURATION_MULT_DEAD_BRANCH` | `double` |
| `0x005014e4` | `_G_LLM_STRAT_POP_GROWTH_FACTOR` | `double` |
| `0x005014ec` | `_G_LLM_STRAT_POP_DECAY_FACTOR` | `double` |
| `0x005014f4` | `_G_LLM_STRAT_RESOURCE_DECAY_RATE` | `double` |
| `0x005014fc` | `_G_LLM_STRAT_FACING24_RAD2DEG_NUM` | `double` |
| `0x00501504` | `_G_LLM_STRAT_FACING24_RAD2DEG_DEN` | `double` |
| `0x0050150c` | `_G_LLM_STRAT_FACING24_HALF_TURN_DEG` | `double` |
| `0x00501514` | `_G_LLM_STRAT_FACING24_BIAS_DEG` | `double` |
| `0x0050151c` | `_G_LLM_STRAT_FACING24_WRAP_ADD_DEG` | `double` |
| `0x00501524` | `_G_LLM_STRAT_FACING24_WRAP_LIMIT_DEG` | `double` |
| `0x0050152c` | `_G_LLM_STRAT_FACING24_WRAP_SUB_DEG` | `double` |
| `0x00501534` | `_G_LLM_STRAT_FACING24_SECTOR_DEG` | `double` |
| `0x0050153c` | `_G_LLM_STRAT_DIR24_RAD2DEG_NUM` | `double` |
| `0x00501544` | `_G_LLM_STRAT_DIR24_RAD2DEG_DEN` | `double` |
| `0x0050154c` | `_G_LLM_STRAT_DIR24_HALF_TURN_DEG` | `double` |
| `0x00501554` | `_G_LLM_STRAT_DIR24_BIAS_DEG` | `double` |
| `0x0050155c` | `_G_LLM_STRAT_DIR24_WRAP_ADD_DEG` | `double` |
| `0x00501564` | `_G_LLM_STRAT_DIR24_WRAP_LIMIT_DEG` | `double` |
| `0x0050156c` | `_G_LLM_STRAT_DIR24_WRAP_SUB_DEG` | `double` |
| `0x00501574` | `_G_LLM_STRAT_DIR24_SECTOR_DEG` | `double` |
| `0x0050157c` | `_G_LLM_STRAT_DIR128_RAD2DEG_NUM` | `double` |
| `0x00501584` | `_G_LLM_STRAT_DIR128_RAD2DEG_DEN` | `double` |
| `0x0050158c` | `_G_LLM_STRAT_DIR128_HALF_TURN_DEG` | `double` |
| `0x00501594` | `_G_LLM_STRAT_DIR128_BIAS_DEG` | `double` |
| `0x0050159c` | `_G_LLM_STRAT_DIR128_WRAP_ADD_DEG` | `double` |
| `0x005015a4` | `_G_LLM_STRAT_DIR128_WRAP_LIMIT_DEG` | `double` |
| `0x005015ac` | `_G_LLM_STRAT_DIR128_WRAP_SUB_DEG` | `double` |
| `0x005015b4` | `_G_LLM_STRAT_DIR128_SECTOR_DEG` | `double` |
| `0x005015fe` | `_G_LLM_STRAT_MINE_EXTRACT_RATE_TENTHS_DIVISOR` | `double` |
| `0x00501606` | `_G_LLM_STRAT_MINE_EXTRACT_ROUND_UP_BIAS` | `double` |
| `0x0050160e` | `_G_LLM_STRAT_BLDG_COUNT_OFFLINE_DECREMENT` | `double` |
| `0x00501616` | `_G_LLM_GAME_SPEED_FACTOR_MAX` | `double` |
| `0x0050161e` | `_G_LLM_GAME_SPEED_FACTOR_STEP_UP` | `double` |
| `0x00501626` | `_G_LLM_GAME_SPEED_FACTOR_MIN` | `double` |
| `0x0050162e` | `_G_LLM_GAME_SPEED_FACTOR_STEP_DOWN` | `double` |
| `0x005016b3` | `_G_LLM_STRAT_INVASION_ROLL_BASE_TIME` | `double` |
| `0x00501738` | `_G_LLM_STRAT_BLDG_ENERGY_DEFICIT_PERCENT_SCALE` | `double` |
| `0x00501740` | `_G_LLM_STRAT_UNIT_SCALED_DAMAGE_FRACTION` | `double` |
| `0x00501748` | `_G_LLM_STRAT_BLDG_SCALED_DAMAGE_DIVISOR` | `double` |
| `0x0050176d` | `_G_LLM_STRAT_INVASION_ALERT_INTERVAL` | `double` |
| `0x00501775` | `_G_LLM_STRAT_ADVISOR_DUE_DELAY` | `double` |
| `0x0050177d` | `_G_LLM_STRAT_ADVISOR_STAFF_THRESHOLD` | `double` |
| `0x00501785` | `_G_LLM_STRAT_ADVISOR_INTERVAL` | `double` |
| `0x00501790` | `_G_LLM_NET_LOCKSTEP_PUMP_REFILL_FRAC` | `double` |
| `0x00501798` | `_G_LLM_NET_LOCKSTEP_KEEPALIVE_MARGIN_MUL` | `double` |
| `0x005017a0` | `_G_LLM_NET_LOCKSTEP_KEEPALIVE_STEP_MUL` | `double` |
| `0x005017b4` | `_G_LLM_STRAT_LOCKSTEP_EMERGENCY_STEP_MUL` | `double` |
| `0x005017f4` | `_G_LLM_NET_LOCKSTEP_RESYNC_TIMEOUT_MS` | `double` |
| `0x005017fc` | `_G_LLM_NET_LOCKSTEP_RESYNC_DEADLINE_PAD_MS` | `double` |
| `0x00501c7d` | `_G_LLM_SINGLE_INSTANCE_MUTEX_NAME` | `string` |
| `0x00501e68` | `_G_LLM_CFG_KW_BANK` | `unicode` |
| `0x00501e72` | `_G_LLM_CFG_KW_WEAPON_POWER_ADD` | `unicode` |
| `0x00501e94` | `_G_LLM_CFG_KW_WEAPON_POWER_MUL` | `unicode` |
| `0x00501eb6` | `_G_LLM_CFG_KW_UNIT_SPEED_ADD` | `unicode` |
| `0x00501ed4` | `_G_LLM_CFG_KW_UNIT_SPEED_MUL` | `unicode` |
| `0x00501ef2` | `_G_LLM_CFG_KW_UNIT_ENERGY_ADD` | `unicode` |
| `0x00501f12` | `_G_LLM_CFG_KW_UNIT_ENERGY_MUL` | `unicode` |
| `0x00501f32` | `_G_LLM_CFG_KW_UNIT_SIGHT_ADD` | `unicode` |
| `0x00501f50` | `_G_LLM_CFG_KW_UNIT_SIGHT_MUL` | `unicode` |
| `0x00501f6e` | `_G_LLM_CFG_KW_UNIT_BUILD_TIME_ADD` | `unicode` |
| `0x00501f96` | `_G_LLM_CFG_KW_UNIT_BUILD_TIME_MUL` | `unicode` |
| `0x00501fbe` | `_G_LLM_CFG_KW_UNIT_RESOURCE_ADD` | `unicode` |
| `0x00501fe2` | `_G_LLM_CFG_KW_UNIT_RESOURCE_MUL` | `unicode` |
| `0x00502006` | `_G_LLM_CFG_KW_BUILDING_ENERGY_ADD` | `unicode` |
| `0x0050202e` | `_G_LLM_CFG_KW_BUILDING_ENERGY_MUL` | `unicode` |
| `0x00502056` | `_G_LLM_CFG_KW_BUILDING_POWER_ADD` | `unicode` |
| `0x0050207c` | `_G_LLM_CFG_KW_BUILDING_POWER_MUL` | `unicode` |
| `0x005020a2` | `_G_LLM_CFG_KW_BUILDING_SIGHT_ADD` | `unicode` |
| `0x005020c8` | `_G_LLM_CFG_KW_BUILDING_SIGHT_MUL` | `unicode` |
| `0x005020ee` | `_G_LLM_CFG_KW_BUILDING_BUILD_TIME_ADD` | `unicode` |
| `0x0050211e` | `_G_LLM_CFG_KW_BUILDING_BUILD_TIME_MUL` | `unicode` |
| `0x0050214e` | `_G_LLM_CFG_KW_BUILDING_HUMAN_ADD` | `unicode` |
| `0x00502174` | `_G_LLM_CFG_KW_BUILDING_HUMAN_MUL` | `unicode` |
| `0x0050219a` | `_G_LLM_CFG_KW_BUILDING_BUILDER_ADD` | `unicode` |
| `0x005021c4` | `_G_LLM_CFG_KW_BUILDING_BUILDER_MUL` | `unicode` |
| `0x005021ee` | `_G_LLM_CFG_KW_TIME` | `unicode` |
| `0x005021f8` | `_G_LLM_CFG_KW_STEP` | `unicode` |
| `0x00502202` | `_G_LLM_CFG_KW_REPEAT` | `unicode` |
| `0x00502210` | `_G_LLM_CFG_KW_CYCLIC` | `unicode` |
| `0x0050221e` | `_G_LLM_CFG_KW_TYPE` | `unicode` |
| `0x00502228` | `_G_LLM_CFG_KW_AMMO` | `unicode` |
| `0x00502232` | `_G_LLM_CFG_KW_POCKET` | `unicode` |
| `0x00502240` | `_G_LLM_CFG_KW_SHORT_TIME` | `unicode` |
| `0x00502256` | `_G_LLM_CFG_KW_LONG_TIME` | `unicode` |
| `0x0050226a` | `_G_LLM_CFG_KW_RANGE_MIN` | `unicode` |
| `0x0050227e` | `_G_LLM_CFG_KW_RANGE_MAX` | `unicode` |
| `0x00502292` | `_G_LLM_CFG_KW_FIRE_EXPLO` | `unicode` |
| `0x005022a8` | `_G_LLM_CFG_KW_TARGET_EXPLO` | `unicode` |
| `0x005022c2` | `_G_LLM_CFG_KW_POWER` | `unicode` |
| `0x005022ce` | `_G_LLM_CFG_KW_SPEED` | `unicode` |
| `0x005022da` | `_G_LLM_CFG_KW_MISSING` | `unicode` |
| `0x005022ea` | `_G_LLM_CFG_KW_BULLET_ANIM` | `unicode` |
| `0x00502302` | `_G_LLM_CFG_KW_PROBABILYTY` | `unicode` |
| `0x0050231a` | `_G_LLM_CFG_KW_FIRE_RANGE` | `unicode` |
| `0x00502330` | `_G_LLM_CFG_KW_EXPLO_TIME` | `unicode` |
| `0x00502346` | `_G_LLM_CFG_KW_SMOKE_TIME` | `unicode` |
| `0x0050235c` | `_G_LLM_CFG_KW_SMOKE_SPRITE` | `unicode` |
| `0x00502376` | `_G_LLM_CFG_KW_HOMING` | `unicode` |
| `0x00502384` | `_G_LLM_CFG_KW_TARGET` | `unicode` |
| `0x00502392` | `_G_LLM_CFG_KW_SOUND_TARGET` | `unicode` |
| `0x005023ac` | `_G_LLM_CFG_KW_SOUND_FIRE` | `unicode` |
| `0x005023c2` | `_G_LLM_CFG_KW_INVENTION` | `unicode` |
| `0x005023d6` | `_G_LLM_CFG_KW_SPRITE_SHADOW` | `unicode` |
| `0x005023f2` | `_G_LLM_CFG_KW_SPRITE_TYPE` | `unicode` |
| `0x0050240a` | `_G_LLM_CFG_KW_FRAME` | `unicode` |
| `0x00502416` | `_G_LLM_CFG_KW_FRAME_2` | `unicode` |
| `0x00502426` | `_G_LLM_CFG_KW_EQUIVALENT` | `unicode` |
| `0x0050243c` | `_G_LLM_CFG_KW_ANIM_EXPLO` | `unicode` |
| `0x00502452` | `_G_LLM_CFG_KW_INFO_TXT` | `unicode` |
| `0x00502464` | `_G_LLM_CFG_KW_INFO_FLC` | `unicode` |
| `0x00502476` | `_G_LLM_CFG_KW_STEP_SPEED` | `unicode` |
| `0x0050248c` | `_G_LLM_CFG_KW_TURN_SPEED` | `unicode` |
| `0x005024a2` | `_G_LLM_CFG_KW_ARMOR` | `unicode` |
| `0x005024ae` | `_G_LLM_CFG_KW_ENERGY` | `unicode` |
| `0x005024bc` | `_G_LLM_CFG_KW_P_1` | `TerminatedUnicode` |
| `0x005024c4` | `_G_LLM_CFG_KW_P_2` | `TerminatedUnicode` |
| `0x005024cc` | `_G_LLM_CFG_KW_P_3` | `TerminatedUnicode` |
| `0x005024d4` | `_G_LLM_CFG_KW_P_4` | `TerminatedUnicode` |
| `0x005024dc` | `_G_LLM_CFG_KW_P_5` | `TerminatedUnicode` |
| `0x005024e4` | `_G_LLM_CFG_KW_BUILD_TIME` | `unicode` |
| `0x005024fa` | `_G_LLM_CFG_KW_RESOURCE` | `unicode` |
| `0x0050250c` | `_G_LLM_CFG_KW_INDEPENDENT` | `unicode` |
| `0x00502524` | `_G_LLM_CFG_KW_SIGHT` | `unicode` |
| `0x00502530` | `_G_LLM_CFG_KW_HUMAN` | `unicode` |
| `0x0050253c` | `_G_LLM_CFG_KW_SOUND_EXPLO` | `unicode` |
| `0x00502554` | `_G_LLM_CFG_KW_SOUND_MOVE` | `unicode` |
| `0x0050256a` | `_G_LLM_CFG_KW_TRACE` | `unicode` |
| `0x00502576` | `_G_LLM_CFG_KW_AIUNIT` | `unicode` |
| `0x00502584` | `_G_LLM_CFG_KW_AILEVEL` | `unicode` |
| `0x00502594` | `_G_LLM_CFG_KW_SOLDIER_TYPE` | `unicode` |
| `0x005025ae` | `_G_LLM_CFG_KW_COMPONENT` | `unicode` |
| `0x005025c2` | `_G_LLM_CFG_KW_COMPONENT_QUANT` | `unicode` |
| `0x005025e2` | `_G_LLM_CFG_KW_ANIM_P` | `unicode` |
| `0x005025f0` | `_G_LLM_CFG_KW_SPRITE_QUANTITY` | `unicode` |
| `0x00502610` | `_G_LLM_CFG_KW_AREA` | `unicode` |
| `0x0050261a` | `_G_LLM_CFG_KW_HEIGHT` | `unicode` |
| `0x00502628` | `_G_LLM_CFG_KW_AIBLD` | `unicode` |
| `0x00502634` | `_G_LLM_CFG_KW_PARAMETR` | `unicode` |
| `0x00502646` | `_G_LLM_CFG_KW_ELECTRIC_POWER` | `unicode` |
| `0x00502664` | `_G_LLM_CFG_KW_BUILDER` | `unicode` |
| `0x00502674` | `_G_LLM_CFG_KW_PRODUCTION` | `unicode` |
| `0x0050268a` | `_G_LLM_CFG_KW_TRANSPORT` | `unicode` |
| `0x0050269e` | `_G_LLM_CFG_KW_PRODUCTION_TIME` | `unicode` |
| `0x005026be` | `_G_LLM_CFG_KW_TRANSPORT_QUANT` | `unicode` |
| `0x005026de` | `_G_LLM_CFG_KW_EXTRACT` | `unicode` |
| `0x005026ee` | `_G_LLM_CFG_KW_FUEL` | `unicode` |
| `0x005026f8` | `_G_LLM_CFG_KW_CAPACITY` | `unicode` |
| `0x0050270a` | `_G_LLM_CFG_KW_SOUND_CLICK` | `unicode` |
| `0x00502722` | `_G_LLM_CFG_KW_VELOCITY` | `unicode` |
| `0x00502734` | `_G_LLM_CFG_KW_ABILITY` | `unicode` |
| `0x00502744` | `_G_LLM_CFG_KW_DEPEND` | `unicode` |
| `0x00502752` | `_G_LLM_CFG_KW_MAP` | `TerminatedUnicode` |
| `0x0050275a` | `_G_LLM_CFG_KW_X1` | `TerminatedUnicode` |
| `0x00502760` | `_G_LLM_CFG_KW_X2` | `TerminatedUnicode` |
| `0x00502766` | `_G_LLM_CFG_KW_Y1` | `TerminatedUnicode` |
| `0x0050276c` | `_G_LLM_CFG_KW_Y2` | `TerminatedUnicode` |
| `0x00502772` | `_G_LLM_CFG_KW_COORDINATE_X` | `unicode` |
| `0x0050278c` | `_G_LLM_CFG_KW_COORDINATE_Y` | `unicode` |
| `0x005027a6` | `_G_LLM_CFG_KW_ASTEROIDS` | `unicode` |
| `0x005027ba` | `_G_LLM_CFG_KW_ENEMY` | `unicode` |
| `0x005027c6` | `_G_LLM_CFG_KW_SOURCE_MUL` | `unicode` |
| `0x005027dc` | `_G_LLM_CFG_KW_SOURCE_ADD` | `unicode` |
| `0x005027f2` | `_G_LLM_CFG_KW_OBJECT` | `unicode` |
| `0x0050281c` | `_G_LLM_CFG_ANIMNM_FIRE_1` | `string` |
| `0x00502825` | `_G_LLM_CFG_ANIMNM_FIRE_2` | `string` |
| `0x0050282e` | `_G_LLM_CFG_ANIMNM_FIRE_3` | `string` |
| `0x00502837` | `_G_LLM_CFG_ANIMNM_FIRE_4` | `string` |
| `0x00502840` | `_G_LLM_CFG_ANIMNM_VEHSMOKE_1` | `string` |
| `0x0050284e` | `_G_LLM_CFG_ANIMNM_VEHSMOKE_2` | `string` |
| `0x0050285c` | `_G_LLM_CFG_ANIMNM_VEHSMOKE_3` | `string` |
| `0x0050286a` | `_G_LLM_CFG_ANIMNM_VEHSMOKE_4` | `string` |
| `0x00502878` | `_G_LLM_CFG_ANIMNM_TRACK_11A` | `string` |
| `0x00502883` | `_G_LLM_CFG_ANIMNM_TRACK_11B` | `string` |
| `0x0050288e` | `_G_LLM_CFG_ANIMNM_TRACK_11C` | `string` |
| `0x00502899` | `_G_LLM_CFG_ANIMNM_TRACK_11D` | `string` |
| `0x005028a4` | `_G_LLM_CFG_ANIMNM_TRACK_22A` | `string` |
| `0x005028af` | `_G_LLM_CFG_ANIMNM_TRACK_22B` | `string` |
| `0x005028ba` | `_G_LLM_CFG_ANIMNM_TRACK_22C` | `string` |
| `0x005028c5` | `_G_LLM_CFG_ANIMNM_TRACK_22D` | `string` |
| `0x005028d0` | `_G_LLM_CFG_ANIMNM_TRACK_33A` | `string` |
| `0x005028db` | `_G_LLM_CFG_ANIMNM_TRACK_33B` | `string` |
| `0x005028e6` | `_G_LLM_CFG_ANIMNM_TRACK_33C` | `string` |
| `0x005028f1` | `_G_LLM_CFG_ANIMNM_TRACK_33D` | `string` |
| `0x005028fc` | `_G_LLM_CFG_ANIMNM_TRACK_43A` | `string` |
| `0x00502907` | `_G_LLM_CFG_ANIMNM_TRACK_43B` | `string` |
| `0x00502912` | `_G_LLM_CFG_ANIMNM_TRACK_43C` | `string` |
| `0x0050291d` | `_G_LLM_CFG_ANIMNM_TRACK_43D` | `string` |
| `0x00502928` | `_G_LLM_CFG_ANIMNM_TRACK_44A` | `string` |
| `0x00502933` | `_G_LLM_CFG_ANIMNM_TRACK_44B` | `string` |
| `0x0050293e` | `_G_LLM_CFG_ANIMNM_TRACK_44C` | `string` |
| `0x00502949` | `_G_LLM_CFG_ANIMNM_TRACK_44D` | `string` |
| `0x00502c2d` | `_G_LLM_STR_EMPTY` | `wchar_t[1]` |
| `0x00502d92` | `_G_LLM_STR_SPACE` | `wchar_t[2]` |
| `0x00502df2` | `_G_LLM_FMT_D` | `wchar_t[3]` |
| `0x00502df8` | `_G_LLM_TUT_KW_WELCOME` | `unicode` |
| `0x00502e0a` | `_G_LLM_TUT_KW_COLORS` | `unicode` |
| `0x00502e1a` | `_G_LLM_TUT_FMT_RGB` | `unicode` |
| `0x00502e2c` | `_G_LLM_TUT_KW_TEKST` | `unicode` |
| `0x00502e3a` | `_G_LLM_TUT_KW_PANEL` | `unicode` |
| `0x00502e48` | `_G_LLM_TUT_KW_SEL_LIMIT` | `unicode` |
| `0x00502e5e` | `_G_LLM_TUT_KW_CARD_LIMIT` | `unicode` |
| `0x00502e76` | `_G_LLM_TUT_KW_RMB_LIMIT` | `unicode` |
| `0x00502e8c` | `_G_LLM_TUT_KW_BLD_LIMIT` | `unicode` |
| `0x00502ea2` | `_G_LLM_TUT_KW_BACK_SEL` | `unicode` |
| `0x00502eb6` | `_G_LLM_TUT_KW_MATKA` | `unicode` |
| `0x00502ec2` | `_G_LLM_TUT_KW_KOPALNIA` | `unicode` |
| `0x00502ed4` | `_G_LLM_TUT_KW_ELEKTROWNIA` | `unicode` |
| `0x00502eec` | `_G_LLM_TUT_KW_OSIEDLE` | `unicode` |
| `0x00502efc` | `_G_LLM_TUT_KW_KOSZARY` | `unicode` |
| `0x00502f0e` | `_G_LLM_TUT_KW_ZALOGA1` | `unicode` |
| `0x00502f1e` | `_G_LLM_TUT_KW_AKADEMIA` | `unicode` |
| `0x00502f30` | `_G_LLM_TUT_KW_BUDOWLE` | `unicode` |
| `0x00502f40` | `_G_LLM_TUT_KW_NONE` | `unicode` |
| `0x00502f4a` | `_G_LLM_TUT_KW_REAKCJE` | `unicode` |
| `0x00502f5c` | `_G_LLM_TUT_KW_EXIST` | `unicode` |
| `0x00502f6a` | `_G_LLM_TUT_KW_PLACING` | `unicode` |
| `0x00502f7c` | `_G_LLM_TUT_KW_BUILD` | `unicode` |
| `0x00502f8a` | `_G_LLM_TUT_KW_SELECT` | `unicode` |
| `0x00502f9a` | `_G_LLM_TUT_KW_PRODUCED` | `unicode` |
| `0x00502fae` | `_G_LLM_TUT_KW_ISOUT` | `unicode` |
| `0x00502fbc` | `_G_LLM_TUT_KW_KILLED` | `unicode` |
| `0x00502fcc` | `_G_LLM_TUT_KW_DESTROYED` | `unicode` |
| `0x00502fe2` | `_G_LLM_TUT_KW_AMATKA` | `unicode` |
| `0x00502ff0` | `_G_LLM_TUT_KW_AZALOGA1` | `unicode` |
| `0x00503002` | `_G_LLM_TUT_KW_KOMENDA` | `unicode` |
| `0x00503014` | `_G_LLM_TUT_KW_AI_ATTACK` | `unicode` |
| `0x0050304e` | `_G_LLM_MP_BROWSER_ROW_FMT_HIGHLIGHT` | `TerminatedUnicode` |
| `0x00503087` | `_G_LLM_LOBBY_SESSION_TITLE_FMT_WCS` | `TerminatedUnicode` |
| `0x005030bf` | `_G_LLM_LOBBY_AI_DEFAULT_NAME` | `char[6]` |
| `0x00503158` | `_G_LLM_RES_DIR_PREFIX` | `char[5]` |
| `0x0050315d` | `_G_LLM_FMT_LOCAL_PATH` | `char[5]` |
| `0x00503169` | `_G_LLM_FMT_ERR_COLON` | `wchar_t[7]` |
| `0x00503177` | `_G_LLM_FMODE_RB` | `char[3]` |
| `0x0050317a` | `_G_LLM_UI_ERROR_DIALOG_FMT` | `wchar_t[8]` |
| `0x005032a3` | `_G_LLM_STR_COLON` | `wchar_t[2]` |
| `0x005032a7` | `_G_LLM_CONST_ROUND_HALF_5032A7` | `double` |
| `0x005032af` | `_G_LLM_STR_TWO_SPACES` | `wchar_t[3]` |
| `0x005032b5` | `_G_LLM_STR_COLON_SPACE` | `wchar_t[3]` |
| `0x005032bb` | `_G_LLM_STR_TEXT_COLOR_2` | `wchar_t[2]` |
| `0x005032bf` | `_G_LLM_STR_TEXT_COLOR_3` | `wchar_t[2]` |
| `0x005032c3` | `_G_LLM_STR_TEXT_COLOR_RESET` | `wchar_t[2]` |
| `0x005032c7` | `_G_LLM_STR_TEXT_COLOR_RESET_SPACE` | `wchar_t[3]` |
| `0x00503364` | `_G_LLM_CONST_ROUND_HALF_503364` | `double` |
| `0x0050336c` | `_G_LLM_STR_SPACE_PERCENT_NEWLINE` | `wchar_t[4]` |
| `0x00503374` | `_G_LLM_UI_FMT_BLDG_RESOURCE_ROW_4COL` | `wchar_t[27]` |
| `0x005033aa` | `_G_LLM_CONST_HALF_5033AA` | `double` |
| `0x00503462` | `_G_LLM_CONST_ROUND_HALF_F32_503462` | `float` |
| `0x00503466` | `_G_LLM_CONST_ROUND_HALF_503466` | `double` |
| `0x0050346e` | `_G_LLM_CONST_DBL_1000_50346E` | `double` |
| `0x00503476` | `_G_LLM_CONST_ROUND_HALF_503476` | `double` |
| `0x0050347e` | `_G_LLM_STR_INFO_TEXT_CARET_TOKEN` | `wchar_t[4]` |
| `0x005034e0` | `_G_LLM_STRAT_GROUP_MOVE_WAVE_DIST_SCALE` | `double` |
| `0x005034e8` | `_G_LLM_STRAT_GROUP_MOVE_WAVE_DIST_BIAS` | `double` |
| `0x00503aac` | `_G_LLM_MS_PER_SECOND` | `double` |
| `0x00506d7c` | `_G_LLM_STRAT_AI_BASE_SPAWN_TIMER_STAGGER_SCALE` | `double` |
| `0x00506da0` | `_G_LLM_STRAT_AI_INVASION_CLOCK_STAGGER` | `double` |
| `0x00506da8` | `_G_LLM_STRAT_AI_CLOCK_STAGGER_FRACTION` | `double` |
| `0x00506dd0` | `_G_LLM_STRAT_AI_LOITER_ANGLE_SCALE` | `double` |
| `0x0050a65c` | `_G_LLM_UI_SCROLL_AUTOREPEAT_STATE` | `int *` |
| `0x0050a660` | `_G_LLM_PANEL_REFRESH_RETRY` | `int` |
| `0x0050a664` | `_G_LLM_TUTORIAL_BUILD_TYPE_FILTER` | `int` |
| `0x0050a668` | `_G_LLM_STRAT_UI_PANEL_FALLBACK_TABLE` | `int[4]` |
| `0x0050a678` | `_G_LLM_TUTORIAL_RESET_SLOT_0050A678` | `undefined4` |
| `0x0050a67c` | `_G_LLM_TUTORIAL_PENDING_BUILD_PLACEMENT_ID` | `int` |
| `0x0050a680` | `_G_LLM_TUTORIAL_RMB_LIMIT_FLAG` | `int` |
| `0x0050a684` | `_G_LLM_STRAT_UI_DEPART_PENDING_BLDG_A` | `map_t_building_id` |
| `0x0050a688` | `_G_LLM_STRAT_UI_DEPART_PENDING_BLDG_B` | `map_t_building_id` |
| `0x0050a68c` | `_G_LLM_STRAT_UI_PLANET_SEL_ACTION_TARGET` | `uint` |
| `0x0050a690` | `_G_LLM_UI_SCROLL_ACTIVE` | `int` |
| `0x0050a694` | `_G_LLM_UI_UNASSIGNED_ROW_STATE` | `int[40]` |
| `0x0050a734` | `_G_LLM_UI_CTRLGROUP_MEMBER_ROW_STATE` | `int[40]` |
| `0x0050a7d4` | `_G_LLM_CHAT_INPUT_WRITE_POS` | `int` |
| `0x0050a7d8` | `_G_LLM_CHAT_INPUT_LEN` | `undefined4` |
| `0x0050a7dc` | `_G_LLM_CHAT_INPUT_ACTIVE` | `int` |
| `0x0050a7e0` | `_G_LLM_CHAT_INPUT_CURSOR` | `int` |
| `0x0050a7e4` | `_G_LLM_CHAT_HISTORY_CURSOR` | `undefined4` |
| `0x0050a7f0` | `_G_LLM_UI_STORAGE_PANEL_SELECTED_BLDG_INDEX` | `int` |
| `0x0050a7f4` | `_G_LLM_UI_BLDG_UNITS_PANEL_BUILDING_TYPE` | `uint` |
| `0x0050a7f8` | `_G_LLM_UI_BLDG_PANEL_LAST_SHOWN_INDEX` | `uint` |
| `0x0050a7fc` | `_G_LLM_UI_BUILD_MENU_ITEM_PENDING` | `int` |
| `0x0050a800` | `_G_LLM_UI_BLDG_OWNED_TAB_TOGGLE_ACTIVE_PENDING_ROW` | `int` |
| `0x0050a804` | `_G_LLM_UI_BLDG_CONSTRUCTION_STATUS_PENDING_ROW` | `int` |
| `0x0050a808` | `_G_LLM_UI_BLDG_UPGRADE_REPAIR_PENDING_ROW` | `int` |
| `0x0050a80c` | `_G_LLM_UI_BLDG_PANEL_CLICKED_SLOT` | `int` |
| `0x0050a810` | `_G_LLM_UI_BLDG_PROJECT_PANEL_PENDING_START_ROW` | `int` |
| `0x0050a814` | `_G_LLM_UI_BLDG_UNITS_PANEL_CLICK_MODE` | `int` |
| `0x0050a818` | `_G_LLM_UI_INFO_SCREEN_PENDING_ROW` | `int` |
| `0x0050a81c` | `_G_LLM_UI_BLDG_PROJECT_PANEL_PENDING_ROW` | `int` |
| `0x0050a820` | `_G_LLM_UI_BLDG_UNITS_PANEL_PENDING_ROW` | `undefined4` |
| `0x0050a824` | `_G_LLM_UI_STORAGE_PANEL_SHOW_INFO_SLOT_REQUEST` | `int` |
| `0x0050a828` | `_G_LLM_UI_BLDG_OWNED_PANEL_SELECT_ROW_PENDING` | `int` |
| `0x0050a82c` | `_G_LLM_UI_STORAGE_PANEL_AUTOLAUNCH_PENDING` | `undefined4` |
| `0x0050a830` | `_G_LLM_UI_STORAGE_PANEL_RECHARGE_PENDING` | `undefined4` |
| `0x0050a834` | `_G_LLM_UI_BLDG_LOAD_CARGO_PENDING` | `int` |
| `0x0050a838` | `_G_LLM_UI_MINIMAP_EDGE_DRAG_ACTIVE` | `int` |
| `0x0050a83c` | `_G_LLM_UI_STORAGE_PANEL_DEPART_TYPE_PENDING` | `uint` |
| `0x0050a848` | `_G_LLM_UI_ICON_DRAG_HOVER_INDEX` | `int` |
| `0x0050a84c` | `_G_LLM_STRAT_CHAT_INPUT_LINE` | `char[81]` |
| `0x0050a89d` | `_G_LLM_CHAT_HISTORY_LINES` | `char[410]` |
| `0x0050bcf7` | `_G_LLM_UI_ICON_GROUP_FIRST_ENTRY` | `int[56]` |
| `0x0050bddb` | `_G_LLM_UI_BLDG_WORKER_ADJ_BTNS` | `llm_ui_bldg_worker_adjust_btn[2]` |
| `0x0050be03` | `_G_LLM_STRAT_UI_BASE_MARKER_COORDS` | `llm_strat_ui_base_marker_coord[8]` |
| `0x0050be53` | `_G_LLM_UI_BLDG_WORKER_POOL_COUNTER` | `uint` |
| `0x0050be5b` | `_G_LLM_UI_BLDG_ADJUST_ROW_HOLD_MS` | `uint` |
| `0x0050be63` | `_G_LLM_UI_HUD_WIDGETS` | `llm_ui_hud_widget[332]` |
| `0x0050e797` | `_G_LLM_UI_HUD_COLOR_TABLE` | `llm_ui_hud_color_slot[16]` |
| `0x0050e817` | `_G_LLM_UI_ICON_ENTRIES` | `llm_ui_icon_entry[562]` |
| `0x0050fb9f` | `_G_LLM_UI_HUD_TOPBAR_TICK_COUNTER` | `uint` |
| `0x0050fba3` | `_G_LLM_UI_HUD_TOPBAR_VIEW_SIZE_MODE_APPLIED` | `int` |
| `0x0050fba7` | `_G_LLM_STRAT_UI_EVENT_DEFER_ACTIVE` | `int` |
| `0x0050fbab` | `_G_LLM_UI_ICON_CLICK_CONSUMED_FLAG` | `int` |
| `0x0050fbaf` | `_G_LLM_STRAT_UI_EVENT_QUEUE_POS` | `int` |
| `0x0050fbb3` | `_G_LLM_STRAT_UI_EVENT_QUEUE_BUF` | `game_e_event[256]` |
| `0x0050ffb3` | `_G_LLM_UI_VIEW_SIZE_MODE_BIT` | `uint` |
| `0x0050ffb7` | `_G_LLM_TACT_UI_MINIMAP_ORIGIN_X` | `undefined4` |
| `0x0050ffbb` | `_G_LLM_TACT_UI_MINIMAP_ORIGIN_Y` | `undefined4` |
| `0x0050ffbf` | `_G_LLM_STRAT_UI_PANEL_MODE` | `undefined4` |
| `0x0050ffc3` | `_G_LLM_STRAT_UI_PANEL_PAGE` | `undefined4` |
| `0x0050ffc7` | `_G_LLM_STRAT_UI_UNIT_TAB_TOGGLE` | `int` |
| `0x0050ffcb` | `_G_LLM_STRAT_UI_MAINPANEL_TAB_INDEX` | `int` |
| `0x0050ffd3` | `_G_LLM_STRAT_UI_BLDG_PANEL_REFRESH_PENDING` | `int` |
| `0x0050ffd7` | `_G_LLM_STRAT_UI_PANEL_SWITCH_PENDING` | `int` |
| `0x0050ffdb` | `_G_LLM_STRAT_UI_UNIT_PANEL_REFRESH_PENDING` | `int` |
| `0x0050ffdf` | `_G_LLM_STRAT_UI_MAINPANEL_REFRESH_PENDING` | `int` |
| `0x0050ffe3` | `_G_LLM_STRAT_UI_VIEW_RESIZE_PENDING` | `int` |
| `0x0050ffe7` | `_G_LLM_UI_SELECTED_CTRL_GROUP_SLOT` | `int` |
| `0x0050ffeb` | `_G_LLM_UI_UNASSIGNED_UNIT_SCAN_IDX` | `int` |
| `0x0050ffef` | `_G_LLM_UI_BLDG_UNITS_SCROLL_LIST` | `llm_ui_scroll_list_descriptor` |
| `0x00510020` | `_G_LLM_UI_SCROLL_DESC_STORAGE` | `llm_ui_scroll_list_descriptor` |
| `0x00510051` | `_G_LLM_UI_SCROLL_DESC_PROJECT` | `llm_ui_scroll_list_descriptor` |
| `0x00510082` | `_G_LLM_UI_SCROLL_DESC_UNIT_UNASSIGNED` | `llm_ui_scroll_list_descriptor` |
| `0x005100b3` | `_G_LLM_UI_SCROLL_DESC_SELECTION` | `llm_ui_scroll_list_descriptor` |
| `0x005100e4` | `_G_LLM_UI_SCROLL_DESC_SELECTION_CTRLGRP` | `llm_ui_scroll_list_descriptor` |
| `0x00510115` | `_G_LLM_UI_SCROLL_DESC_BLDG_AVAILABLE` | `llm_ui_scroll_list_descriptor` |
| `0x00510146` | `_G_LLM_UI_SCROLL_DESC_BLDG_OWNED` | `llm_ui_scroll_list_descriptor` |
| `0x00510177` | `_G_LLM_UI_SCROLL_ARROW_DESCS` | `llm_ui_scroll_arrow_desc[16]` |
| `0x00510277` | `_G_LLM_UI_TOPBAR_POWER_PCT_NA_TEXT` | `wchar_t *` |
| `0x005102bf` | `_G_LLM_UI_SELECTED_BLDG_PROJECT_TYPE` | `int` |
| `0x005102c7` | `_G_LLM_UI_LIST_ROW_STYLE_HILITE` | `llm_ui_list_row_icon_style[2]` |
| `0x005102d7` | `_G_LLM_UI_LIST_ROW_STYLE_NORMAL` | `llm_ui_list_row_icon_style[2]` |
| `0x005102e8` | `_G_LLM_UI_ICON_GROUP_BLOB` | `undefined1` |
| `0x005182f5` | `_G_LLM_UI_HUD_ICON_WIDGET_DESCS` | `llm_ui_icon_widget_desc[12]` |
| `0x00518475` | `_G_LLM_PANEL_ICON_FILES` | `llm_panel_icon_file_entry[313]` |
| `0x00519cec` | `_G_LLM_ITOA_SCRATCH_BUF_LOBOUND` | `undefined2` |
| `0x00519d2c` | `_G_LLM_UI_ICON_SPRITE_BASE` | `int` |
| `0x00519d30` | `_G_LLM_UI_ICON_HOVER_ACTIVE` | `int` |
| `0x00519d34` | `_G_LLM_UI_BLDG_PANEL_REDRAWN_FLAG` | `bool` |
| `0x00519d3c` | `_G_LLM_UI_MOUSE_LEFT_RELEASE_EDGE` | `int` |
| `0x00519d40` | `_G_LLM_UI_MOUSE_LEFT_PRESS_EDGE` | `int` |
| `0x00519d48` | `_G_LLM_UI_MOUSE_RIGHT_RELEASE_EDGE` | `int` |
| `0x00519d4c` | `_G_LLM_UI_MOUSE_RIGHT_PRESS_EDGE` | `int` |
| `0x00519d50` | `_G_LLM_CURSOR_ICON_OVERRIDE` | `undefined4` |
| `0x00519d58` | `_G_LLM_UI_STAGED_CURSOR_X` | `int` |
| `0x00519d5c` | `_G_LLM_UI_STAGED_CURSOR_Y` | `int` |
| `0x00519e05` | `_G_LLM_TACT_MOVE_PATH_TRACE_TILE` | `int` |
| `0x00519e09` | `_G_LLM_TACT_MOVE_PATH_TRACE_BASE` | `int` |
| `0x00519e0d` | `_G_LLM_TACT_MOVE_PATH_TRACE_DIAG_CAND` | `int` |
| `0x00519e11` | `_G_LLM_TACT_MOVE_PATH_GOAL_PACKED` | `int` |
| `0x00519e15` | `_G_LLM_TACT_MOVE_PATH_START_PACKED` | `int` |
| `0x00519e19` | `_G_LLM_TACT_MOVE_PATH_QUEUE_B` | `ushort[4096]` |
| `0x0051be19` | `_G_LLM_TACT_MOVE_PATH_QUEUE_A` | `ushort[4096]` |
| `0x0051de19` | `_G_LLM_TACT_MOVE_PATH_QUEUE_CUR` | `ushort *` |
| `0x0051de1d` | `_G_LLM_TACT_MOVE_PATH_COORD_MASK` | `int` |
| `0x0051de21` | `_G_LLM_TACT_MOVE_PATH_TILE_COST` | `int` |
| `0x0051de25` | `_G_LLM_TACT_MOVE_PATH_TRACE_BEST_DIR` | `byte` |
| `0x0051de26` | `_G_LLM_TACT_MOVE_PATH_RLE_COUNT` | `int` |
| `0x0051de2a` | `_G_LLM_TACT_MOVE_PATH_TRACE_BEST_TILE` | `int` |
| `0x0051de30` | `_G_LLM_STRAT_GROUP_CENTROID_X` | `undefined4` |
| `0x0051de34` | `_G_LLM_STRAT_GROUP_CENTROID_Y` | `undefined4` |
| `0x0051de38` | `_G_LLM_STRAT_GROUP_MEMBER_COUNT` | `int` |
| `0x0051de3c` | `_G_LLM_STRAT_GROUP_PATH_BUILD_IDX` | `undefined4` |
| `0x0051de40` | `_G_LLM_MAP_DIR_STEP_DELTAS` | `char[28][2]` |
| `0x0051de78` | `_G_LLM_STRAT_PLANET_SWITCH_MUTEX` | `HANDLE` |
| `0x0051de7c` | `_G_LLM_MAP_REGION_LIST_HEAD` | `llm_map_region *` |
| `0x0051de80` | `_G_LLM_MAP_REGION_POOL_FREE_HEAD` | `llm_map_region *` |
| `0x0051de88` | `_G_LLM_MAP_REGION_ROUTE_DIR_CODES` | `int[8]` |
| `0x0051deac` | `_G_LLM_MAP_PROXIMITY_STENCIL` | `llm_map_proximity_stencil_entry[169]` |
| `0x0051e201` | `_G_LLM_TACT_MOVE_FLOOD_TILE_COST` | `int` |
| `0x0051e205` | `_G_LLM_TACT_MOVE_FLOOD_COORD_MASK` | `int` |
| `0x0051e20d` | `_G_LLM_TACT_MOVE_FLOOD_START_PACKED` | `int` |
| `0x0051e211` | `_G_LLM_TACT_MOVE_FLOOD_QUEUE_B` | `ushort[2048]` |
| `0x0051f211` | `_G_LLM_TACT_MOVE_FLOOD_QUEUE_A` | `ushort[2048]` |
| `0x00520211` | `_G_LLM_TACT_MOVE_FLOOD_QUEUE_CUR` | `ushort *` |
| `0x00520224` | `_G_LLM_SND_CHANNEL_COUNT` | `undefined4` |
| `0x00520264` | `_G_LLM_STRAT_ORDER_ACK_VOICE_SND_ID_BY_RACE` | `int[6]` |
| `0x00520280` | `_G_LLM_STRAT_RACE_ALERT_SOUND_SND_ID_BY_RACE` | `int[6]` |
| `0x0052029c` | `_G_LLM_STRAT_ORDER_ACK_VOICE_LAST_PLAY_TIME` | `double` |
| `0x005202a4` | `_G_LLM_STRAT_ORDER_ACK_VOICE_SUPPRESSED_COUNT` | `int` |
| `0x005202a8` | `_G_LLM_STRAT_RACE_ALERT_SOUND_LAST_PLAY_TIME` | `double` |
| `0x005202b0` | `_G_LLM_STRAT_RACE_ALERT_SOUND_SUPPRESSED_COUNT` | `int` |
| `0x005202b4` | `_G_LLM_STRAT_RACE_ALERT_TEXT_LAST_PLAY_TIME` | `double` |
| `0x005202bc` | `_G_LLM_STRAT_RACE_ALERT_TEXT_SUPPRESSED_COUNT` | `int` |
| `0x005202d0` | `_G_LLM_GAME_MODE` | `byte` |
| `0x005202d1` | `_G_LLM_GAME_LAND_NO_START_UNIT_FLAG` | `int` |
| `0x005202d5` | `_G_LLM_TACT_FOG_EDGE_SHAPE_OVERRIDE` | `int` |
| `0x005202d9` | `_G_LLM_BANK_SPRITE_BASE` | `int[47]` |
| `0x00520395` | `_G_LLM_UI_MENU_SPRITE_ID_BASE` | `undefined4` |
| `0x0052046d` | `_G_LLM_PLANET_TLO_THUMBNAIL_PTR` | `undefined4` |
| `0x00520471` | `_G_LLM_PLANET_TLO_THUMBNAIL_2X2_PTR` | `ushort *` |
| `0x00520475` | `_G_LLM_PLANET_TLO_THUMBNAIL_4X4_PTR` | `ushort *` |
| `0x00520479` | `_G_LLM_CURSOR_OVL_PIXELS` | `undefined4` |
| `0x00520489` | `_G_LLM_CURSOR_OVL_PALETTES` | `undefined4` |
| `0x005204a1` | `_G_LLM_CURSOR_X` | `undefined4` |
| `0x005204a5` | `_G_LLM_CURSOR_Y` | `undefined4` |
| `0x005204b1` | `_G_LLM_CURSOR_OVL_HOTX` | `undefined` |
| `0x005204c1` | `_G_LLM_CURSOR_OVL_HOTY` | `undefined` |
| `0x005204d1` | `_G_LLM_CURSOR_VISIBLE` | `undefined4` |
| `0x005204d5` | `_G_LLM_INPUT_MOUSE_DELTA_X` | `int` |
| `0x005204d9` | `_G_LLM_INPUT_MOUSE_DELTA_Y` | `int` |
| `0x005204e1` | `_G_LLM_HITTEST_HIT` | `undefined1` |
| `0x005204e2` | `_G_LLM_GFX_VIEW_TILE_HEIGHT_PX` | `undefined4` |
| `0x005204e6` | `_G_LLM_GFX_BLEND_LUT_A` | `undefined` |
| `0x005244e6` | `_G_LLM_GFX_BLEND_LUT_B` | `undefined` |
| `0x005284e6` | `_G_LLM_GFX_ALPHA_BLEND_LUT_GHI` | `undefined` |
| `0x005484e6` | `_G_LLM_GFX_ALPHA_BLEND_LUT_R` | `undefined` |
| `0x005584e6` | `_G_LLM_GFX_ALPHA_BLEND_LUT_B` | `undefined` |
| `0x005586e6` | `_G_LLM_STRAT_MINIMAP_ORIGIN_COL` | `int` |
| `0x005586ea` | `_G_LLM_STRAT_MINIMAP_ORIGIN_ROW` | `int` |
| `0x005586ee` | `_G_LLM_STRAT_MINIMAP_ZOOM_MODE` | `undefined4` |
| `0x005586f2` | `_G_LLM_STRAT_MINIMAP_FULLMAP_FLAG` | `undefined4` |
| `0x005586f6` | `_G_LLM_MINIMAP_TERRAIN_COLOR_MODE` | `undefined1` |
| `0x005586fa` | `_G_LLM_STRAT_MINIMAP_TERRAIN_FLAT_COLOR` | `ushort` |
| `0x005586fe` | `_G_LLM_STRAT_MINIMAP_RESOURCE_OVERLAY_SLOT` | `short` |
| `0x00558702` | `_G_LLM_MAP_VIEWPORT_DIRTY_FLAG` | `bool` |
| `0x00558706` | `_G_LLM_TACT_MOVE_FLOOD_START_COL` | `byte` |
| `0x00558707` | `_G_LLM_TACT_MOVE_FLOOD_START_ROW` | `byte` |
| `0x00558708` | `_G_LLM_TACT_MOVE_FLOOD_GOAL_COL` | `byte` |
| `0x00558709` | `_G_LLM_TACT_MOVE_FLOOD_GOAL_ROW` | `byte` |
| `0x0055870a` | `_G_LLM_TACT_MOVE_FLOOD_RESULT_COL` | `byte` |
| `0x0055870b` | `_G_LLM_TACT_MOVE_FLOOD_RESULT_ROW` | `byte` |
| `0x0055870c` | `_G_LLM_TACT_MOVE_FLOOD_FOUND` | `int` |
| `0x00558710` | `_G_LLM_TACT_MOVE_PATH_SLOT_ID` | `undefined4` |
| `0x00558714` | `_G_LLM_TACT_MOVE_PATH_TRACE_COST_SUM` | `int` |
| `0x0055871c` | `_G_LLM_BOOT_STAGE` | `undefined4` |
| `0x00558ba0` | `_G_LLM_TACT_SCROLL_CMD` | `undefined4` |
| `0x00558ba4` | `_G_LLM_TACT_FX_SPLASH_SPARE_NONZERO_OWNER` | `int` |
| `0x00558ba8` | `_G_LLM_TACT_MINE_BLAST_TIME_END` | `double` |
| `0x00558bb8` | `_G_LLM_TACT_WHO_XOR_KEY` | `int` |
| `0x00558bbc` | `_G_LLM_TACT_MOVE_PATH_CACHE_VALID` | `undefined4` |
| `0x00558bc0` | `_G_LLM_TACT_UNIT_ACTIVE_COUNT` | `int` |
| `0x00558bc4` | `_G_LLM_TACT_FX_LIVE_COUNT` | `int` |
| `0x00558bc8` | `_G_LLM_TACT_HOVERED_UNIT_ID` | `undefined4` |
| `0x00558bcc` | `_G_LLM_GFX_DIM_FLAG` | `undefined4` |
| `0x00558bd8` | `_G_LLM_TACT_CLICK_SCAN_SCRATCH` | `int` |
| `0x00558bdc` | `_G_LLM_TACT_DIR8_DELTA_TABLE` | `llm_vec2i[8]` |
| `0x00558c1c` | `_G_LLM_TACT_SIDEBAR_HIGHLIGHTED_UNIT_ID` | `undefined4` |
| `0x00558c20` | `_G_LLM_TACT_FOV_STENCIL_PTR` | `undefined1 *` |
| `0x00558c24` | `_G_LLM_TACT_CAM_FOLLOW_SELECTION` | `undefined4` |
| `0x00558c28` | `_G_LLM_TACT_CAM_FOLLOW_TARGET_COL` | `int` |
| `0x00558c2c` | `_G_LLM_TACT_CAM_FOLLOW_TARGET_ROW` | `int` |
| `0x00558c30` | `_G_LLM_TACT_CAM_COL_F` | `double` |
| `0x00558c38` | `_G_LLM_TACT_CAM_ROW_F` | `double` |
| `0x00558c40` | `_G_LLM_TACT_DEBUG_UNIT_STATS_OVERLAY_ENABLED` | `int` |
| `0x00558c44` | `_G_LLM_TACT_SEE_ENEMY_FLAG` | `undefined4` |
| `0x00558c48` | `_G_LLM_TACT_DRAG_ANCHOR_X` | `undefined4` |
| `0x00558c4c` | `_G_LLM_TACT_DRAG_ANCHOR_Y` | `undefined4` |
| `0x00558c50` | `_G_LLM_TACT_DRAG_SELECT_ACTIVE` | `undefined4` |
| `0x00558c68` | `_G_LLM_TACT_CAM_SCROLL_UP_HELD` | `undefined4` |
| `0x00558c6c` | `_G_LLM_TACT_CAM_SCROLL_DOWN_HELD` | `undefined4` |
| `0x00558c70` | `_G_LLM_TACT_CAM_SCROLL_LEFT_HELD` | `undefined4` |
| `0x00558c74` | `_G_LLM_TACT_CAM_SCROLL_RIGHT_HELD` | `undefined4` |
| `0x00558c78` | `_G_LLM_TACT_EXIT_CONFIRM_OPEN` | `undefined4` |
| `0x00558c7c` | `_G_LLM_SND_CHANNEL_NEXT_RETRIGGER_TIME` | `double[6]` |
| `0x00558cac` | `_G_LLM_TACT_AMBIENT_SND_ZONE_COUNT` | `int` |
| `0x00558cb0` | `_G_LLM_TACT_AMBIENT_SND_ZONE_TABLE` | `int[10]` |
| `0x00558cdc` | `_G_LLM_TACT_UNIT_RECORD_STRIDE` | `int` |
| `0x00558ce4` | `_G_LLM_TACT_FX_POOL_RECORD_STRIDE` | `int` |
| `0x00558ce8` | `_G_LLM_TACT_MINES_ENABLED` | `int` |
| `0x00558cec` | `_G_LLM_TACT_DIR24_APPROACH_DELTA` | `llm_dir24_delta[24]` |
| `0x00558e6c` | `_G_LLM_TACT_OCCUPANCY_REBUILD_MAP_ID` | `int` |
| `0x00558f00` | `_G_LLM_TACT_UI_SEL_PANEL_MULTI_MODE` | `undefined4` |
| `0x00558f04` | `_G_LLM_TACT_SEL_PANEL_ICON_GFX` | `void *[41]` |
| `0x00559004` | `_G_LLM_TACT_CHAR_PANEL_GFX` | `void *[16]` |
| `0x00559084` | `_G_LLM_TACT_SEL_PANEL_ICON_SLOT_STATE` | `int[4]` |
| `0x00559094` | `_G_LLM_TACT_SIDEBAR_SLOT_SCROLL` | `undefined4` |
| `0x00559098` | `_G_LLM_TACT_SIDEBAR_SLOT_VISIBLE_COUNT` | `int` |
| `0x0055909c` | `_G_LLM_TACT_SIDEBAR_SCROLLBTN_STATE` | `int[8]` |
| `0x005590bc` | `_G_LLM_TACT_SIDEBAR_UNASSIGNED_SCROLL_ROW` | `int` |
| `0x005590c0` | `_G_LLM_TACT_SIDEBAR_GROUP_SCROLL_ROW` | `int` |
| `0x005590c4` | `_G_LLM_TACT_SIDEBAR_MULTI_PANEL_VISIBLE_ROWS` | `int` |
| `0x005590e8` | `_G_LLM_TACT_SIDEBAR_ACTIVE_GROUP_ID` | `int` |
| `0x005590ec` | `_G_LLM_TACT_ACTIVE_UNIT_COUNT` | `undefined4` |
| `0x005590f0` | `_G_LLM_TACT_ACTIVE_UNIT_COUNT_CACHED` | `undefined4` |
| `0x005590f4` | `_G_LLM_TACT_SIDEBAR_MOUSE_X` | `int` |
| `0x005590f8` | `_G_LLM_TACT_SIDEBAR_MOUSE_Y` | `int` |
| `0x005590fc` | `_G_LLM_TACT_SIDEBAR_UI_HIT_CODE` | `int` |
| `0x00559100` | `_G_LLM_TACT_SEL_PANEL_ICON_COUNT` | `int` |
| `0x00559104` | `_G_LLM_TACT_SEL_PANEL_ICON_NAMES` | `llm_tact_panel_icon_name[41]` |
| `0x00559400` | `_G_LLM_TACT_FOV_ANGLE_STEP` | `int` |
| `0x00559404` | `_G_LLM_TACT_FOV_RAY_COUNT` | `int` |
| `0x00559408` | `_G_LLM_TACT_FOV_DIR_TABLE_PTR` | `short *` |
| `0x0055940c` | `_G_LLM_TACT_FOV_RAY_INDEX` | `int` |
| `0x00559410` | `_G_LLM_TACT_FOV_CANDIDATE_DIST` | `int` |
| `0x00559414` | `_G_LLM_TACT_FOV_STEP_BUDGET` | `int` |
| `0x00559418` | `_G_LLM_TACT_FOV_ANGLE_INDEX` | `int` |
| `0x0055941c` | `_G_LLM_TACT_FOV_RAY_DELTA` | `undefined4` |
| `0x00559420` | `_G_LLM_TACT_FOV_RAY_DELTA_NEG` | `undefined4` |
| `0x00559428` | `_G_LLM_TACT_FOV_DELTA_TABLE_72` | `short[72]` |
| `0x005594b8` | `_G_LLM_TACT_FOV_DELTA_TABLE_120` | `short[120]` |
| `0x005595a8` | `_G_LLM_TACT_FOV_DELTA_TABLE_360` | `short[360]` |
| `0x00559878` | `_G_LLM_TACT_FOV_NEAREST_HIBIT_DIST` | `int` |
| `0x0055987c` | `_G_LLM_TACT_FOV_NEAREST_LOW_DIST` | `int` |
| `0x00559888` | `_G_LLM_TACT_SQUAD_SIZE` | `undefined4` |
| `0x0055988c` | `_G_LLM_TACT_ENEMY_COUNT` | `int` |
| `0x00559890` | `_G_LLM_TACT_MINE_BLAST_FIRST_FRAME` | `undefined4` |
| `0x00559894` | `_G_LLM_TACT_MINE_BLAST_FRAME_COUNT` | `undefined4` |
| `0x00559898` | `_G_LLM_TACT_MINE_BLAST_DURATION` | `double` |
| `0x005598a0` | `_G_LLM_TACT_MINE_BLAST_SOUND_ID` | `undefined4` |
| `0x005598a4` | `_G_LLM_TACT_BLAST_MARKER_COL` | `undefined4` |
| `0x005598a8` | `_G_LLM_TACT_BLAST_MARKER_ROW` | `undefined4` |
| `0x005598b0` | `_G_LLM_TACT_QUIT_TILE_COL` | `undefined4` |
| `0x005598b4` | `_G_LLM_TACT_QUIT_TILE_ROW` | `undefined4` |
| `0x005598b8` | `_G_LLM_TACT_TARGET_TILE_COL` | `undefined4` |
| `0x005598bc` | `_G_LLM_TACT_TARGET_TILE_ROW` | `undefined4` |
| `0x00559900` | `_G_LLM_BLIT_CUR_X` | `undefined4` |
| `0x00559904` | `_G_LLM_BLIT_CUR_Y` | `undefined4` |
| `0x00559908` | `_G_LLM_TILE_PX_H` | `undefined4` |
| `0x0055990c` | `_G_LLM_BLIT_ROW_X` | `undefined4` |
| `0x00559918` | `_G_LLM_BLIT_CURSOR_FLAG` | `undefined` |
| `0x00559935` | `_G_LLM_TACT_TILE_BLIT_DST_ROW_PTR` | `void *` |
| `0x00559946` | `_G_LLM_FOG_STENCIL_IDX_LUT` | `undefined` |
| `0x00559a00` | `_G_LLM_TACT_UI_MINIMAP_OVERLAY_PTR` | `ushort *` |
| `0x00559a0d` | `_G_LLM_PF_RESOLUTION_MASK_BYTE` | `char` |
| `0x00559a28` | `_G_LLM_PF_WRAP_MASK` | `undefined` |
| `0x0055b397` | `_G_LLM_PF_GOAL_REACHED` | `undefined1` |
| `0x0055b3a2` | `_G_LLM_PF_GENERATION` | `undefined1` |
| `0x0055b3c1` | `_G_LLM_PF_START_CHECK_ENABLED` | `byte` |
| `0x0055b5a7` | `_G_LLM_PF_EXPAND_PROGRESS` | `byte` |
| `0x005d00a4` | `_G_LLM_STATUS_LINE_TEXT_BUF` | `wchar_t[120]` |
| `0x005d0194` | `_G_LLM_STRAT_ORDER_QUEUE_COUNT` | `undefined4` |
| `0x005d0198` | `_G_LLM_STRAT_GAME_CLOCK` | `double` |
| `0x005d01a0` | `_G_LLM_STRAT_AI_ENABLED` | `undefined4` |
| `0x005d01a4` | `_G_LLM_STRAT_ORDER_STAGING_COUNT` | `undefined4` |
| `0x005d01a8` | `_G_LLM_STRAT_ORDER_PENDING_COUNT` | `undefined4` |
| `0x005d01ac` | `_G_LLM_CFG_TEXT_PTR_TABLE` | `undefined` |
| `0x005d01d4` | `_G_LLM_STRAT_SIM_ACTIVE` | `undefined4` |
| `0x005d01d8` | `_G_LLM_CHAT_MODE` | `undefined4` |
| `0x005d01dc` | `_G_LLM_BUILD_PLACEMENT_ID` | `undefined4` |
| `0x005d01e0` | `_G_LLM_STRAT_BUILD_PREVIEW_SUPPRESS_FLAG` | `undefined4` |
| `0x005d01e4` | `_G_LLM_MINIMAP_CLICK_VALID` | `undefined4` |
| `0x005d01e8` | `_G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER` | `byte[8]` |
| `0x005d09e4` | `_G_LLM_GAME_MISSION_VERSION_INDEX` | `undefined4` |
| `0x005d09e8` | `_G_LLM_CAM_JUMP_QUEUE_COUNT` | `undefined4` |
| `0x005d09ec` | `_G_LLM_CHEAT_STR_PTRS` | `WCHAR *[45]` |
| `0x005d0ac0` | `_G_LLM_STRAT_PATH_SLOT_FLAGS_PTR` | `byte *` |
| `0x005d0ac4` | `_G_LLM_STRAT_PATH_BUFFERS_PTR` | `llm_strat_path_waypoint *` |
| `0x005d0ac8` | `_G_LLM_STRAT_MINIMAP_RESOURCE_GRID_PTR` | `map_resources *` |
| `0x005d0acc` | `_G_LLM_GFX_TILE_FOG_AUTOTILE_LUT` | `byte *` |
| `0x005d0ad4` | `_G_LLM_STRAT_MINIMAP_VIGNETTE_MASK_PTR` | `ushort *` |
| `0x005d0adc` | `_G_LLM_INPUT_RSHIFT_SNAPSHOT` | `uint` |
| `0x005d0ae8` | `_G_LLM_CHEAT_SEL_UNIT_TYPE` | `undefined4` |
| `0x005d0aec` | `_G_LLM_CHEAT_SEL_BLDG_TYPE` | `undefined4` |
| `0x005d0af0` | `_G_LLM_CHEAT_MSG_CURSOR` | `undefined4` |
| `0x005d0af4` | `_G_LLM_STRAT_FRAME_TIME_RING` | `double[20]` |
| `0x005d0b94` | `_G_LLM_STRAT_FRAME_TIME_RING_IDX` | `int` |
| `0x005d0b9c` | `_G_LLM_LMB_DOWN_T_CUR` | `double` |
| `0x005d0ba4` | `_G_LLM_LMB_DOWN_T_PREV` | `double` |
| `0x005d0bac` | `_G_LLM_RMB_DBLCLICK_T_CUR` | `double` |
| `0x005d0bb4` | `_G_LLM_RMB_DBLCLICK_T_PREV` | `double` |
| `0x005d0bbc` | `_G_LLM_CAM_SCROLL_LEFT_HELD` | `undefined4` |
| `0x005d0bc0` | `_G_LLM_CAM_SCROLL_RIGHT_HELD` | `undefined4` |
| `0x005d0bc4` | `_G_LLM_CAM_SCROLL_UP_HELD` | `undefined4` |
| `0x005d0bc8` | `_G_LLM_CAM_SCROLL_DOWN_HELD` | `undefined4` |
| `0x005d0bcc` | `_G_LLM_CAM_SCROLL_LEFT_QUEUED` | `undefined4` |
| `0x005d0bd0` | `_G_LLM_CAM_SCROLL_RIGHT_QUEUED` | `undefined4` |
| `0x005d0bd4` | `_G_LLM_CAM_SCROLL_UP_QUEUED` | `undefined4` |
| `0x005d0bd8` | `_G_LLM_CAM_SCROLL_DOWN_QUEUED` | `undefined4` |
| `0x005d0bdc` | `_G_LLM_CAM_SCROLL_LEFT_NEXT_TIME` | `double` |
| `0x005d0be4` | `_G_LLM_CAM_SCROLL_RIGHT_NEXT_TIME` | `double` |
| `0x005d0bec` | `_G_LLM_CAM_SCROLL_UP_NEXT_TIME` | `double` |
| `0x005d0bf4` | `_G_LLM_CAM_SCROLL_DOWN_NEXT_TIME` | `double` |
| `0x005d0bfc` | `_G_LLM_CAM_EDGE_LEFT_ACTIVE` | `undefined4` |
| `0x005d0c00` | `_G_LLM_CAM_EDGE_RIGHT_ACTIVE` | `undefined4` |
| `0x005d0c04` | `_G_LLM_CAM_EDGE_UP_ACTIVE` | `undefined4` |
| `0x005d0c08` | `_G_LLM_CAM_EDGE_DOWN_ACTIVE` | `undefined4` |
| `0x005d0c0c` | `_G_LLM_CAM_EDGE_LEFT_NEXT_TIME` | `double` |
| `0x005d0c14` | `_G_LLM_CAM_EDGE_RIGHT_NEXT_TIME` | `double` |
| `0x005d0c1c` | `_G_LLM_CAM_EDGE_UP_NEXT_TIME` | `double` |
| `0x005d0c24` | `_G_LLM_CAM_EDGE_DOWN_NEXT_TIME` | `double` |
| `0x005d0c2c` | `_G_LLM_CAM_ZOOM_HELD_TAG` | `undefined4` |
| `0x005d0c30` | `_G_LLM_CAM_ZOOM_QUEUED` | `undefined4` |
| `0x005d0c34` | `_G_LLM_CAM_ZOOM_NEXT_TIME` | `double` |
| `0x005d0c3c` | `_G_LLM_CAM_ZOOM_DIRECTION` | `undefined4` |
| `0x005d0c40` | `_G_LLM_CURSOR_ICON_HOLD_TIMER` | `undefined4` |
| `0x005d0c44` | `_G_LLM_CAM_FOLLOW_SELECTION` | `undefined4` |
| `0x005d0c48` | `_G_LLM_CURSOR_ANIM_ID` | `undefined4` |
| `0x005d0c4c` | `_G_LLM_CHEATS_ENABLED` | `undefined4` |
| `0x005d0c54` | `_G_LLM_STRAT_ANIM_FRAME_PARITY` | `byte` |
| `0x005d0c55` | `_G_LLM_STRAT_ANIM_FRAME_PHASE` | `byte` |
| `0x005d0c66` | `_G_LLM_VIEW_SIZE_MODE_CACHE` | `undefined4` |
| `0x005d0c70` | `_G_LLM_STRAT_UNIT_CHASE_RESULT` | `int` |
| `0x005d0c74` | `_G_LLM_STRAT_DEBUG_CAMPAIGN_CHEAT` | `undefined4` |
| `0x005d0c78` | `_G_LLM_TACT_SCREENSHOT_COUNTER` | `int` |
| `0x005d0c7c` | `_G_LLM_BLDG_FOOTPRINT_PASSABLE_SAVE_SLOT` | `uint` |
| `0x005d0c80` | `_G_LLM_STRAT_ADVISOR_PHASE` | `undefined4` |
| `0x005d0d88` | `_G_LLM_MP_PLAYER_NAME` | `string` |
| `0x005d0da8` | `_G_LLM_MP_GAME_NAME` | `undefined1` |
| `0x005d0f81` | `_G_LLM_LOBBY_PEER_COUNT_U8` | `undefined` |
| `0x005d1358` | `_G_LLM_MP_PROBE_SERVER_COUNT` | `uint` |
| `0x005d135c` | `_G_LLM_NET_SESSION_LIST` | `llm_net_session_desc *` |
| `0x005d1364` | `_G_LLM_NET_SESSION_COUNT` | `undefined4` |
| `0x005d1368` | `_G_LLM_NET_ROSTER_SCAN` | `llm_net_roster_entry[8]` |
| `0x005d3368` | `_G_LLM_NET_ROSTER_LIVE` | `llm_net_roster_entry[8]` |
| `0x005d336c` | `_G_LLM_NET_HOST_PLAYER_ID` | `?` |
| `0x005d5368` | `_G_LLM_NET_LOBBY_SESSION_LIST` | `llm_net_session_entry[8]` |
| `0x005d54b0` | `_G_LLM_NET_LOBBY_SESSION_LIST_COUNT` | `uint` |
| `0x005d54b8` | `_G_LLM_NET_LOBBY_SCAN_SESSION_COUNT` | `uint` |
| `0x005d54bc` | `_G_LLM_NET_LOBBY_SCAN_HOST_COUNT` | `uint` |
| `0x005d54c0` | `_G_LLM_NET_LOCKSTEP_PLAYER_COUNT` | `undefined4` |
| `0x005d54c4` | `_G_LLM_NET_ACTIVE_PLAYER_COUNT` | `uint` |
| `0x005d54c8` | `_G_LLM_NET_LOBBY_MAP_RECV_DONE` | `int` |
| `0x005d54cc` | `_G_LLM_NET_PEER_HORIZON` | `double[8]` |
| `0x005d550c` | `_G_LLM_NET_PEER_HORIZON_PENDING` | `double[8]` |
| `0x005d554c` | `_G_LLM_GAME_SPEED_PLAYER_FACTOR` | `double[8]` |
| `0x005d558c` | `_G_LLM_STRAT_LOCKSTEP_COMMITTED_HORIZON` | `double` |
| `0x005d5594` | `_G_LLM_STRAT_LOCKSTEP_HORIZON` | `double` |
| `0x005d55ac` | `_G_LLM_NET_LOCAL_PLAYER_INDEX` | `undefined4` |
| `0x005d55b0` | `_G_LLM_NET_IS_HOST` | `undefined4` |
| `0x005d55b4` | `_G_LLM_NET_LOCKSTEP_STATUS_FLAGS` | `byte` |
| `0x005d55b8` | `_G_LLM_NET_LOCAL_PLAYER_SLOT` | `int` |
| `0x005d55bc` | `_G_LLM_STRAT_LOCKSTEP_STEP_SIZE` | `double` |
| `0x005d55c4` | `_G_LLM_STRAT_LOCKSTEP_ADAPT_NEXT_TIME` | `double` |
| `0x005d55cc` | `_G_LLM_NET_SEND_BUF` | `undefined1[1016]` |
| `0x005d59c4` | `_G_LLM_NET_SEND_BUF_CURSOR` | `undefined4` |
| `0x005d5ddc` | `_G_LLM_GAME_ARGV` | `int` |
| `0x005d5fdc` | `_G_LLM_GAME_ARGC` | `undefined4` |
| `0x005d5fe8` | `_G_LLM_GAME_RUNNING` | `undefined4` |
| `0x005d5fec` | `_G_LLM_CURSOR_DISPLAY_COUNT` | `undefined4` |
| `0x005d6100` | `_G_LLM_STRAT_MINIMAP_COORD_WRAP_MASK` | `uint` |
| `0x005d6104` | `_G_LLM_STRAT_MINIMAP_CURSOR_TILE` | `ushort` |
| `0x005d6106` | `_G_LLM_STRAT_MINIMAP_RENDER_PHASE` | `uint` |
| `0x005d6184` | `_G_LLM_GFX_FONT_LINE_HEIGHT` | `int` |
| `0x005d7190` | `_G_LLM_UI_SURFACE_PTR` | `undefined4` |
| `0x005d7194` | `_G_LLM_UI_SURFACE_PITCH` | `undefined4` |
| `0x005d8198` | `_G_LLM_GFX_FONT_DATA_PTRS` | `undefined` |
| `0x005d81b4` | `_G_LLM_GFX_CUR_FONT_GLYPH_PTR_TABLE` | `byte * *` |
| `0x005d81b8` | `_G_LLM_GFX_FONT_GLYPH_PTR_TABLE` | `undefined4` |
| `0x005d85f8` | `_G_LLM_GFX_TEXT_LAYOUT_WIDTH` | `int` |
| `0x005d8800` | `_G_LLM_UI_BLIT_FLAG` | `undefined` |
| `0x005d8830` | `_G_LLM_GFX_TILE_WRAP_MASK` | `uint` |
| `0x005d8834` | `_G_LLM_GFX_VIEW_TILES_W` | `uint` |
| `0x005d8838` | `_G_LLM_GFX_VIEW_TILES_H` | `uint` |
| `0x005d883c` | `_G_LLM_GFX_MARGIN_TILE_CURSOR` | `uint` |
| `0x005d8840` | `_G_LLM_GFX_MARGIN_TILES_W` | `uint` |
| `0x005d8844` | `_G_LLM_GFX_MARGIN_TILE_COUNT` | `uint` |
| `0x005d8848` | `_G_LLM_GFX_MARGIN_COL_FIRST` | `uint` |
| `0x005d884c` | `_G_LLM_HITTEST_PIXEL_PTR` | `undefined4` |
| `0x005d8850` | `_G_LLM_HITTEST_SAVED_PIXEL` | `undefined2` |
| `0x005d8b04` | `_G_LLM_MAP_ZOOM_SCALE_X` | `double` |
| `0x005d8b0c` | `_G_LLM_MAP_ZOOM_SCALE_Y` | `double` |
| `0x005d8b50` | `_G_LLM_GFX_UI_CLIP_TOP` | `undefined4` |
| `0x005d8b54` | `_G_LLM_GFX_UI_CLIP_BOTTOM` | `undefined4` |
| `0x005d8b58` | `_G_LLM_CUR_DST_X` | `undefined4` |
| `0x005d8b5c` | `_G_LLM_CUR_DST_Y` | `undefined4` |
| `0x005d8b60` | `_G_LLM_SPRITE_PIX_OFFSETS` | `uint32_t[22000]` |
| `0x005ee320` | `_G_LLM_SPRITE_PAL_OFFSETS` | `undefined` |
| `0x00603ae0` | `_G_LLM_GFX_BANK_PIXELS` | `gfx_u_pixel *` |
| `0x00603ae4` | `_G_LLM_GFX_BANK_PALETTES` | `undefined4` |
| `0x00603ae8` | `_G_LLM_GFX_BANK_PALETTES_DARK` | `undefined4` |
| `0x00603aec` | `_G_LLM_CUR_SPRITE_HDR` | `undefined4` |
| `0x00603af0` | `_G_LLM_CUR_SPRITE_PALETTE` | `undefined4` |
| `0x00603af8` | `_G_LLM_FRAMEBUFFER` | `undefined4` |
| `0x00603afc` | `_G_LLM_FB_PITCH` | `undefined4` |
| `0x00603b00` | `_G_LLM_GFX_DRAW_SURFACE` | `undefined4` |
| `0x00603b04` | `_G_LLM_GFX_PANEL_ROW_SKIP` | `undefined4` |
| `0x00603b0c` | `_G_LLM_GFX_VIEWPORT_ROW_BYTES` | `undefined4` |
| `0x00603ec8` | `_G_LLM_STRAT_AI_GRID_WRAP_MASK` | `uint` |
| `0x00603ecc` | `_G_LLM_STRAT_RNG_STATE` | `uint[4]` |
| `0x00603f0c` | `_G_LLM_STRAT_RNG_NORM_DIVISOR` | `double` |
| `0x00603f14` | `_G_LLM_GAME_QUIT_REQUESTED` | `undefined4` |
| `0x00603f18` | `_G_LLM_UI_DLG_TITLE_STR` | `pointer` |
| `0x00603f50` | `_G_LLM_UI_DLG_OK_STR_SECONDARY` | `wchar_t *` |
| `0x00603f54` | `_G_LLM_CD_AUDIO_VOLUME_RAW` | `undefined4` |
| `0x00603f5c` | `_G_LLM_CD_AUDIO_PLAY_FROM` | `undefined4` |
| `0x00603f60` | `_G_LLM_CD_AUDIO_PLAY_TO` | `undefined4` |
| `0x00603f6c` | `_G_LLM_CD_AUDIO_ACTIVE` | `undefined4` |
| `0x00603f70` | `_G_LLM_UI_IN_MAIN_MENU` | `int` |
| `0x00604284` | `_G_LLM_CD_DRIVE_BITMASK` | `undefined` |
| `0x00604348` | `_G_LLM_UI_FONT_LAYOUT_FILE_END` | `undefined4` |
| `0x006444be` | `_G_LLM_GFX_UI_SPRITE_BASE_INDEX` | `int` |
| `0x006444c6` | `_G_LLM_LOBBY_DIRTY_FLAG` | `undefined1` |
| `0x006444c7` | `_G_LLM_LOBBY_READY_COUNT` | `undefined4` |
| `0x006444cb` | `_G_LLM_GAME_MODE_SAVED` | `undefined1` |
| `0x006444d3` | `_G_LLM_UI_MAINMENU_LIST_ITEMS` | `llm_ui_menu_list_item[4]` |
| `0x006445e3` | `_G_LLM_UI_BEVEL_COLOR_TABLE` | `ushort[8]` |
| `0x006445f3` | `_G_LLM_MENU_PENDING_BUILD_PLACEMENT_ID` | `int` |
| `0x006445ff` | `_G_LLM_BOOT_FILE_BUF_PUB` | `void *` |
| `0x00644607` | `_G_LLM_UI_VIDEO_ORIGIN_Y` | `int` |
| `0x0064460f` | `_G_LLM_UI_MENU_BAR_HEIGHT` | `int` |
| `0x00644613` | `_G_LLM_BOOT_TXT_CURSOR` | `int` |
| `0x0064461b` | `_G_LLM_BOOT_ASYNC_LOCK` | `undefined1[24]` |
| `0x00644633` | `_G_LLM_INTRO_FRAME_INIT_DONE` | `int` |
| `0x00644637` | `_G_LLM_BOOT_MODE_FLAG` | `int` |
| `0x0064463b` | `_G_LLM_AVI_PLAYER_CTX` | `llm_avi_player_ctx` |
| `0x0064484f` | `_G_LLM_BOOT_PUMP_ACC` | `int` |
| `0x00644853` | `_G_LLM_BOOT_PUMP_TICK_BASE` | `uint` |
| `0x00644857` | `_G_LLM_BOOT_PUMP_FLAG` | `int` |
| `0x0064485b` | `_G_LLM_BOOT_ASYNC_PUMP_FN` | `void *` |
| `0x00644873` | `_G_LLM_LOBBY_MAP_SUMMARY_LINE` | `wchar_t[256]` |
| `0x00644a73` | `_G_LLM_LOBBY_MAP_STATUS_LINE` | `wchar_t[256]` |
| `0x00644f8f` | `_G_LLM_UI_OUTCOME_DLG_PLAYER_SIDE_SNAPSHOT` | `int` |
| `0x00644fdb` | `_G_LLM_UI_MAPPICKER_LIST_ITEMS` | `void * *` |
| `0x0064504b` | `_G_LLM_UI_OPT_SLIDER_MASTER_VOL_RANGE` | `int` |
| `0x0064504f` | `_G_LLM_UI_OPT_SLIDER_MASTER_VOL_VALUE_PTR` | `void *` |
| `0x0064505b` | `_G_LLM_UI_OPT_SLIDER_MUSIC_VOL_RANGE` | `int` |
| `0x0064505f` | `_G_LLM_UI_OPT_SLIDER_MUSIC_VOL_VALUE_PTR` | `void *` |
| `0x00645093` | `_G_LLM_UI_OPT_VIEW_SIZE_OPTION_TABLE` | `void *[3]` |
| `0x006450a3` | `_G_LLM_LOBBY_SPIN_OPEN_OPTIONS` | `pointer` |
| `0x006450af` | `_G_LLM_LOBBY_SPIN_RACE_OPTIONS` | `undefined *` |
| `0x006450c3` | `_G_LLM_UI_OPT_SPIN_VIEW_SIZE` | `llm_ui_spinner` |
| `0x006450cb` | `_G_LLM_UI_OPT_SPIN_VIEW_SIZE_VALUE_PTR` | `?` |
| `0x006450d7` | `_G_LLM_UI_OPT_SPIN_MUSIC_ENABLED` | `llm_ui_spinner` |
| `0x006450df` | `_G_LLM_UI_OPT_SPIN_MUSIC_ENABLED_VALUE_PTR` | `?` |
| `0x006450eb` | `_G_LLM_UI_OPT_SPIN_SND_ENABLED` | `llm_ui_spinner` |
| `0x006450f3` | `_G_LLM_UI_OPT_SPIN_SND_ENABLED_VALUE_PTR` | `?` |
| `0x0064510b` | `_G_LLM_UI_OPT_SAVED_SHOW_UNIT_FLAGS` | `int` |
| `0x00645113` | `_G_LLM_LOBBY_WIDGETS_SPIN_RACE` | `llm_ui_widget *` |
| `0x006451b3` | `_G_LLM_LOBBY_WIDGETS_SPIN_COLOR` | `llm_ui_widget *` |
| `0x00645253` | `_G_LLM_LOBBY_WIDGETS_SPIN_OPEN` | `llm_ui_widget *` |
| `0x006452f3` | `_G_LLM_UI_DIPLO_RELATION_SPINNERS` | `llm_ui_spinner[8]` |
| `0x00645393` | `_G_LLM_UI_DIPLO_CHAT_SPINNERS` | `llm_ui_spinner[8]` |
| `0x00645433` | `_G_LLM_UI_DIPLO_CONTROL_SPINNERS` | `llm_ui_spinner[8]` |
| `0x006454d3` | `_G_LLM_PLANET_SEL_FLC` | `llm_ui_flc_anim_state[32]` |
| `0x0064fd97` | `_G_LLM_UI_WGT_OPTIONS_MP_ONLY_ROW` | `llm_ui_widget` |
| `0x0064fddb` | `_G_LLM_UI_WGT_MAINMENU_NEWGAME_BTN` | `llm_ui_widget` |
| `0x0064fe1f` | `_G_LLM_UI_WGT_MAINMENU_TUTORIAL_BTN` | `llm_ui_widget` |
| `0x00650083` | `_G_LLM_UI_WGT_LOBBY_LOG_LIST` | `llm_ui_widget` |
| `0x006500c7` | `_G_LLM_UI_WGT_NETSETUP_FIELD_EDIT` | `llm_ui_widget` |
| `0x0065014f` | `_G_LLM_UI_MP_BROWSER_LIST_WIDGET` | `llm_ui_widget` |
| `0x00650193` | `_G_LLM_UI_WGT_NETSETUP_MRU_LIST` | `llm_ui_widget` |
| `0x0065025f` | `_G_LLM_UI_MAPPICKER_LIST_WIDGET` | `llm_ui_widget` |
| `0x0065058f` | `_G_LLM_UI_WGT_LOCKSTEP_WAIT_PLAYER_LIST` | `llm_ui_widget` |
| `0x006505d3` | `_G_LLM_UI_WGT_LOCKSTEP_KICK_COUNTDOWN_TEXT` | `llm_ui_widget` |
| `0x00650617` | `_G_LLM_UI_WGT_LOCKSTEP_KICK_RESET_BTN` | `llm_ui_widget` |
| `0x0065065b` | `_G_LLM_UI_WGT_LOCKSTEP_KICK_DISCONNECT_BTN` | `llm_ui_widget` |
| `0x0065069f` | `_G_LLM_UI_WGT_DESYNC_ICON` | `llm_ui_widget` |
| `0x00650903` | `_G_LLM_UI_WGT_SHIPMENT_CONTENTS_SCROLL` | `llm_ui_widget` |
| `0x0065098b` | `_G_LLM_PLANET_SEL_WIDGET_CANCEL` | `llm_ui_widget` |
| `0x00650a57` | `_G_LLM_UI_WGT_MENU_OK` | `llm_ui_widget` |
| `0x00650adf` | `_G_LLM_UI_OUTCOME_DLG_MESSAGE_WIDGET` | `llm_ui_widget` |
| `0x00650bef` | `_G_LLM_UI_OUTCOME_DLG_MAIN_BTN_ACTION` | `llm_ui_widget` |
| `0x00650c33` | `_G_LLM_UI_OUTCOME_DLG_SKIP_BTN` | `llm_ui_widget` |
| `0x00650c77` | `_G_LLM_UI_OUTCOME_DLG_NOTE_ICON_WIDGET` | `llm_ui_widget` |
| `0x00650cbb` | `_G_LLM_UI_OUTCOME_DLG_NOTE_TEXT_WIDGET` | `llm_ui_widget` |
| `0x00650cff` | `_G_LLM_UI_DLG_TABLE_WIDGET_ARRAY` | `llm_ui_widget[4]` |
| `0x00650e97` | `_G_LLM_UI_WGT_MENU_SCREEN_TITLE` | `llm_ui_widget` |
| `0x00650edb` | `_G_LLM_UI_OUTCOME_DLG_TITLE_WIDGET` | `llm_ui_widget` |
| `0x00650f63` | `_G_LLM_UI_TUTORIAL_HINT_WIDGET` | `llm_ui_widget` |
| `0x00650feb` | `_G_LLM_UI_WGT_TUTORIAL_WELCOME` | `llm_ui_widget` |
| `0x00651073` | `_G_LLM_UI_SAVEGAME_LIST_WIDGET` | `llm_ui_widget` |
| `0x006510b7` | `_G_LLM_UI_WGT_SAVENAME_FIELD` | `llm_ui_widget` |
| `0x0065113f` | `_G_LLM_UI_WGT_INGAME_DIPLOMACY_BTN` | `llm_ui_widget` |
| `0x006514b3` | `_G_LLM_UI_PAGED_LIST_PREV_BTN_WIDGET` | `llm_ui_widget` |
| `0x006514f7` | `_G_LLM_UI_PAGED_LIST_NEXT_BTN_WIDGET` | `llm_ui_widget` |
| `0x0065168f` | `_G_LLM_UI_TEXT_VIEWER_BODY_WIDGET` | `llm_ui_widget` |
| `0x006516d3` | `_G_LLM_UI_WGT_FRAME_MENU_PANEL` | `llm_ui_widget` |
| `0x006517e3` | `_G_LLM_PLANET_SEL_WIDGETS` | `llm_ui_widget[42]` |
| `0x0065230b` | `_G_LLM_LOBBY_WIDGETS_COL0` | `llm_ui_widget[8]` |
| `0x0065252b` | `_G_LLM_LOBBY_WIDGETS_COL1` | `llm_ui_widget[8]` |
| `0x0065274b` | `_G_LLM_LOBBY_WIDGETS_COL2` | `llm_ui_widget[8]` |
| `0x0065296b` | `_G_LLM_LOBBY_WIDGETS_COL3` | `llm_ui_widget[8]` |
| `0x00652b8b` | `_G_LLM_UI_DIPLO_RELATION_WIDGETS` | `llm_ui_widget[8]` |
| `0x00652dab` | `_G_LLM_UI_DIPLO_CONTROL_WIDGETS` | `llm_ui_widget[8]` |
| `0x00652fcb` | `_G_LLM_UI_DIPLO_CHAT_WIDGETS` | `llm_ui_widget[8]` |
| `0x0065345f` | `_G_LLM_UI_INGAME_MENU_WIDGETS_MP_LOCKSTEP` | `llm_ui_widget *` |
| `0x0065347b` | `_G_LLM_UI_INGAME_MENU_WIDGETS_MP_OTHER` | `llm_ui_widget *` |
| `0x00653493` | `_G_LLM_UI_INGAME_MENU_WIDGETS_SP` | `llm_ui_widget *` |
| `0x006534af` | `_G_LLM_UI_INGAME_MENU_WIDGETS_TUTORIAL` | `llm_ui_widget *` |
| `0x00653563` | `_G_LLM_PLANET_SEL_WIDGET_PTRS` | `llm_ui_widget *[33]` |
| `0x00653663` | `_G_LLM_LOBBY_SLOT_WIDGET_PTRS` | `undefined4` |
| `0x006536ef` | `_G_LLM_UI_DIPLO_WIDGET_LIST` | `llm_ui_widget *[24]` |
| `0x0065377b` | `_G_LLM_UI_DLG_MSGBOX_TITLE` | `wchar_t[128]` |
| `0x006539af` | `_G_LLM_UI_DLG_TABLE_CONFIRM_QUIT_TO_MENU` | `llm_ui_dlg_table` |
| `0x00653a4b` | `_G_LLM_UI_DLG_TABLE_MSGBOX_OK` | `llm_ui_dlg_table` |
| `0x00653a7f` | `_G_LLM_UI_WGT_LIST_GAMEPLAY_HUD` | `llm_ui_widget_list` |
| `0x00653acb` | `_G_LLM_UI_WGT_LIST_TUTORIAL_INTRO` | `llm_ui_widget_list` |
| `0x00653b17` | `_G_LLM_UI_WGT_LIST_TUTORIAL_STEP` | `llm_ui_widget_list` |
| `0x00653b63` | `_G_LLM_UI_WGT_LIST_TUTORIAL_WELCOME` | `llm_ui_widget_list` |
| `0x00653baf` | `_G_LLM_UI_WGT_LIST_TUTORIAL_DONE` | `llm_ui_widget_list` |
| `0x00653bfb` | `_G_LLM_UI_WGT_LIST_SAVEGAME_LIST` | `llm_ui_widget_list` |
| `0x00653c47` | `_G_LLM_UI_WGT_LIST_RACE_SELECT` | `llm_ui_widget_list` |
| `0x00653c93` | `_G_LLM_UI_WGT_LIST_SHIPMENT_CONTENTS` | `llm_ui_widget_list` |
| `0x00653cdf` | `_G_LLM_UI_WGT_LIST_PLANET_SELECT` | `llm_ui_widget_list` |
| `0x00653d2b` | `_G_LLM_UI_WGT_LIST_BUILD_MENU_LIST` | `llm_ui_widget_list` |
| `0x00653d77` | `_G_LLM_UI_OUTCOME_REPORT_WIDGET_LIST` | `llm_ui_widget_list` |
| `0x00653dc3` | `_G_LLM_UI_OUTCOME_DLG_WIDGET_LIST` | `llm_ui_widget_list` |
| `0x00653e0f` | `_G_LLM_UI_TEXT_VIEWER_WIDGET_LIST` | `llm_ui_widget_list` |
| `0x00653e5b` | `_G_LLM_UI_WGT_LIST_MP_LOCAL_BROWSER` | `llm_ui_widget_list` |
| `0x00653ea7` | `_G_LLM_UI_WGT_LIST_MP_SESSION_BROWSER` | `llm_ui_widget_list` |
| `0x00653ef3` | `_G_LLM_UI_WGT_LIST_NET_SETUP` | `llm_ui_widget_list` |
| `0x00653f3f` | `_G_LLM_UI_SCREEN_LOBBY_WIDGET_LIST` | `llm_ui_widget_list` |
| `0x00653f8b` | `_G_LLM_UI_WGT_LIST_MP_MAP_PICKER` | `llm_ui_widget_list` |
| `0x00653fd7` | `_G_LLM_UI_WGT_LIST_DIPLOMACY` | `llm_ui_widget_list` |
| `0x00654023` | `_G_LLM_UI_WGT_LIST_LOCKSTEP_SYNC` | `llm_ui_widget_list` |
| `0x0065406f` | `_G_LLM_UI_WGT_LIST_MENU_OVERLAY_DEFAULT` | `llm_ui_widget_list` |
| `0x006540bb` | `_G_LLM_UI_WGT_LIST_GENERIC_DLG` | `llm_ui_widget_list` |
| `0x00654107` | `_G_LLM_UI_INGAME_MENU_ACTIVE_LIST` | `llm_ui_widget_list` |
| `0x00654157` | `_G_LLM_UI_LIST_SELECTED_INDEX` | `int` |
| `0x0065416b` | `_G_LLM_UI_OPTIONS_MENU_OPEN_ACCUM` | `int` |
| `0x0065419f` | `_G_LLM_UI_SCREEN_MAIN_MENU_ID` | `int` |
| `0x0065421f` | `_G_LLM_UI_SCREEN_RACEBCK_ID` | `undefined4` |
| `0x00654227` | `_G_LLM_UI_SCRATCH_CANVAS_PTR` | `ushort *` |
| `0x0065422b` | `_G_LLM_UI_MENU_MASK_BITMAPS` | `void *[12]` |
| `0x00654273` | `_G_LLM_UI_TOOLTIP_FONT` | `llm_gfx_font_desc` |
| `0x0065428b` | `_G_LLM_UI_MENU_STATE` | `byte` |
| `0x0065428c` | `_G_LLM_UI_MENU_SAVED_STATE` | `undefined1` |
| `0x0065428d` | `_G_LLM_UI_MENU_WIDGET_LIST` | `llm_ui_widget_list *` |
| `0x00654295` | `_G_LLM_UI_ACTIVE_DIALOG` | `llm_ui_widget_list *` |
| `0x00654299` | `_G_LLM_UI_MENU_PENDING_WIDGET` | `llm_ui_widget *` |
| `0x0065429d` | `_G_LLM_UI_WIDGET_SELECTED` | `llm_ui_widget *` |
| `0x006542a1` | `_G_LLM_UI_WIDGET_HOVERED` | `llm_ui_widget *` |
| `0x006542a5` | `_G_LLM_UI_WIDGET_KEY_SELECTED` | `llm_ui_widget *` |
| `0x006542a9` | `_G_LLM_UI_MENU_ASYNC_CALLBACK` | `intCallback *` |
| `0x006542ad` | `_G_LLM_UI_MENU_ASYNC_CALLBACK_A` | `eax_eax_func *` |
| `0x006542b1` | `_G_LLM_UI_MENU_ASYNC_CALLBACK_B` | `intCallback *` |
| `0x006542b5` | `_G_LLM_UI_MOUSE_EVENT_KIND` | `undefined1` |
| `0x006542c2` | `_G_LLM_UI_HITTEST_CURSOR_X` | `undefined4` |
| `0x006542c6` | `_G_LLM_UI_HITTEST_CURSOR_Y` | `undefined4` |
| `0x006542ea` | `_G_LLM_UI_MODAL_STEP_DEADLINE_MS` | `undefined4` |
| `0x00654302` | `_G_LLM_UI_MODAL_KEY_SCANCODE` | `undefined4` |
| `0x00654306` | `_G_LLM_UI_MODAL_KEY_ASCII` | `short` |
| `0x0065430a` | `_G_LLM_UI_MODAL_STEP_NEXT_MS` | `undefined4` |
| `0x0065430e` | `_G_LLM_UI_ANIM_FRAME_DELTA` | `undefined4` |
| `0x00654312` | `_G_LLM_UI_MENU_NOW_MS` | `undefined4` |
| `0x00654316` | `_G_LLM_UI_MENU_INPUT_LOCK_TIMER` | `undefined4` |
| `0x0065431a` | `_G_LLM_UI_SCREEN_TAB_STOPS` | `int *` |
| `0x0065431e` | `_G_LLM_UI_WIDGET_DRAW_X` | `undefined4` |
| `0x00654322` | `_G_LLM_UI_WIDGET_DRAW_Y` | `undefined4` |
| `0x00654326` | `_G_LLM_UI_WIDGET_DRAW_W` | `undefined4` |
| `0x0065432a` | `_G_LLM_UI_WIDGET_DRAW_H` | `undefined4` |
| `0x0065434e` | `_G_LLM_DLG_STATE_FLAGS` | `llm_dlg_state_flags32` |
| `0x00654356` | `_G_LLM_UI_VIEW_SIZE_MODE_CACHE` | `undefined4` |
| `0x0065435a` | `_G_LLM_VIEW_SIZE_MODE_SAVE` | `undefined4` |
| `0x0065435e` | `_G_LLM_UI_DOUBLE_CLICK_TIME_MS` | `undefined4` |
| `0x00654362` | `_G_LLM_UI_OPT_SETTINGS_BACKUP_COPY` | `undefined4[22]` |
| `0x006543ba` | `_G_LLM_UI_OPT_SETTINGS_RESTORE_COPY` | `undefined4[22]` |
| `0x0065441a` | `_G_LLM_PLANET_SEL_WIDGET_PTRS_END` | `undefined4` |
| `0x0065441e` | `_G_LLM_PLANET_SEL_WIDGET_PTRS_BEGIN` | `undefined4` |
| `0x00654422` | `_G_LLM_UI_DLG_TRIGGER_WIDGET` | `undefined4` |
| `0x00654426` | `_G_LLM_PLANET_SEL_OPEN_ARG` | `undefined4` |
| `0x0065442a` | `_G_LLM_UI_DLG_TRIGGER_ACTIVE` | `undefined4` |
| `0x0065442e` | `_G_LLM_STRAT_PLANET_TRANSITION_TARGET` | `undefined4` |
| `0x00654436` | `_G_LLM_PLANET_TRANSITION_TICK` | `undefined4` |
| `0x0065443a` | `_G_LLM_UI_TOOLTIP_CURSOR_X` | `undefined4` |
| `0x0065443e` | `_G_LLM_UI_TOOLTIP_CURSOR_Y` | `undefined4` |
| `0x00654446` | `_G_LLM_UI_RACE_SEL_PENDING_GFX_IDX` | `undefined4` |
| `0x0065444a` | `_G_LLM_UI_TOOLTIP_TEXT_RECT` | `int[3]` |
| `0x00654462` | `_G_LLM_MENU_SAVE_NAME_PTR` | `char *` |
| `0x00654472` | `_G_LLM_UI_INFO_TXT_INDEX_BEGIN` | `void * *` |
| `0x00654476` | `_G_LLM_UI_INFO_TXT_INDEX_END` | `void * *` |
| `0x0065447a` | `_G_LLM_UI_INFO_TXT_RESOURCE_PTR` | `byte *` |
| `0x0065447e` | `_G_LLM_UI_INFO_SCREEN_FRAME_DELTA_MS` | `int` |
| `0x00654482` | `_G_LLM_UI_INFO_DESC_INDEX` | `int` |
| `0x0065448a` | `_G_LLM_UI_INFO_TEXT_CURSOR_X` | `int` |
| `0x0065448e` | `_G_LLM_UI_INFO_PANEL_STAT_LINE_Y` | `int` |
| `0x00654492` | `_G_LLM_UI_INFO_UPGRADE_STAT_MODE` | `int` |
| `0x0065449a` | `_G_LLM_UI_INFO_DESC_PTRS` | `void *[32]` |
| `0x0065451a` | `_G_LLM_UI_INFO_DESC_LIST_PTR` | `void *` |
| `0x0065451e` | `_G_LLM_UI_INFO_DESC_COUNT` | `int` |
| `0x00654522` | `_G_LLM_UI_INFO_SCREEN_WORKSPACE` | `llm_ui_info_screen_workspace` |
| `0x00654d42` | `_G_LLM_UI_INFO_SCREEN_KIND` | `llm_ui_info_kind` |
| `0x00654d46` | `_G_LLM_UI_OUTCOME_DLG_REVEAL_FLAG` | `int` |
| `0x00654d4a` | `_G_LLM_UI_OUTCOME_DLG_REVEAL_COUNTER` | `undefined4` |
| `0x00654d4e` | `_G_LLM_UI_OUTCOME_DLG_SHOW_DETAILS` | `undefined4` |
| `0x00654d52` | `_G_LLM_UI_OUTCOME_DLG_STAT_ROWS` | `llm_ui_outcome_stat_row[8]` |
| `0x00654e12` | `_G_LLM_NET_MAX_PROTOCOL_VERSION` | `int` |
| `0x00654e16` | `_G_LLM_LOBBY_STATE_SNAPSHOT` | `undefined4` |
| `0x00654e1b` | `_G_LLM_LOBBY_SLOTS` | `llm_lobby_player_slot[8]` |
| `0x00654fe3` | `_G_LLM_LOBBY_MAP_FILE` | `undefined4` |
| `0x00655217` | `_G_LLM_LOBBY_SELECTED_SESSION_STATE` | `char` |
| `0x00655238` | `_G_LLM_UI_OUTCOME_REPORT_TAB_STOPS` | `int[8]` |
| `0x00655258` | `_G_LLM_UI_DIPLO_TAB_STOPS` | `int[8]` |
| `0x00655278` | `_G_LLM_UI_SHIPMENT_CONTENTS_TAB_STOPS` | `undefined` |
| `0x00655298` | `_G_LLM_UI_MP_BROWSER_TAB_STOPS` | `int[24]` |
| `0x006552f8` | `_G_LLM_RSR_SOURCE_MODE` | `char` |
| `0x006552f9` | `_G_LLM_UI_INFO_SCREEN_CD_FLAG` | `byte` |
| `0x006552fa` | `_G_LLM_LOBBY_WIDGETS_BUILT` | `undefined4` |
| `0x0065532a` | `_G_LLM_LOBBY_MAP_XFER_TABLE` | `undefined` |
| `0x006566fa` | `_G_LLM_LOBBY_MAP_XFER_ACTIVE_COUNT` | `int` |
| `0x006566fe` | `_G_LLM_UI_FADE_TRANSITION` | `llm_ui_fade_transition_state` |
| `0x00656722` | `_G_LLM_UI_TEXT_COLOR_OVERRIDE_INDEX` | `int` |
| `0x00656726` | `_G_LLM_UI_DIGIT_SPRITE_BASE` | `int` |
| `0x0065672a` | `_G_LLM_GAME_QUIT_TEARDOWN_FORCED_FLAG` | `int` |
| `0x0065673e` | `_G_LLM_TLO_REGISTRY` | `llm_tlo_registry_entry[8]` |
| `0x00656786` | `_G_LLM_LOBBY_MAP_CHUNK_RECEIVED` | `bool` |
| `0x0065678a` | `_G_LLM_MP_NET_MODE` | `int` |
| `0x0065678e` | `_G_LLM_UI_LOCKSTEP_OVERLAY_COUNTDOWN_TEXT` | `wchar_t[4]` |
| `0x00656796` | `_G_LLM_NET_LOCKSTEP_WAIT_PLAYER_IDX` | `int` |
| `0x0065679a` | `_G_LLM_NET_LOCKSTEP_OVERLAY_RESULT` | `int` |
| `0x0065679e` | `_G_LLM_LOBBY_LOCAL_SLOT_INDEX` | `undefined4` |
| `0x006568aa` | `_G_LLM_UI_MENU_CLICK_SOUNDS` | `llm_ui_click_sound_slot[4]` |
| `0x00656942` | `_G_LLM_GAME_BOOT_START_TICKS_MS` | `uint` |
| `0x00656946` | `_G_LLM_GFX_UI_COLORS_RGB` | `int[4][3]` |
| `0x0065697e` | `_G_LLM_UI_DLG_MSG_TEXT_BUF` | `undefined` |
| `0x006569fe` | `_G_LLM_UI_DLG_SHUTTLE_OTHER_PLANET_TABLE` | `llm_ui_dlg_table` |
| `0x00656b36` | `_G_LLM_UI_BLDG_STATUS_MSG_BUF` | `wchar_t[256]` |
| `0x00656d9e` | `_G_LLM_UI_NETSETUP_FLD_IP` | `pointer` |
| `0x00656db6` | `_G_LLM_UI_NETSETUP_FLD_NAME` | `pointer` |
| `0x00656dce` | `_G_LLM_UI_NETSETUP_FLD_GAME` | `pointer` |
| `0x00656de6` | `_G_LLM_UI_TEXT_BIND_TABLE` | `llm_ui_text_bind_entry[100]` |
| `0x00657302` | `_G_LLM_MENU_NEWGAME_INTRO_VIDEO_PATH` | `char *` |
| `0x00657306` | `_G_LLM_UI_TEXT_VIEWER_FILE_BUF` | `undefined4` |
| `0x0065730a` | `_G_LLM_UI_TEXT_VIEWER_TEXT_PTR` | `undefined4` |
| `0x0065730e` | `_G_LLM_UI_TEXT_VIEWER_PREV_TICK_MS` | `undefined4` |
| `0x00657312` | `_G_LLM_UI_TEXT_VIEWER_TICK_MS` | `undefined4` |
| `0x0065731e` | `_G_LLM_UI_TEXT_VIEWER_BG_IMAGE` | `undefined4` |
| `0x00657322` | `_G_LLM_TUTORIAL_HQ_ATTACK_SCENARIO_DONE` | `bool` |
| `0x00657326` | `_G_LLM_TUTORIAL_FORCED_BLDG_SELECTION` | `uint` |
| `0x0065732a` | `_G_LLM_TUTORIAL_STEP_COUNT` | `undefined4` |
| `0x0065732e` | `_G_LLM_TUTORIAL_STEPS` | `llm_tutorial_step[16]` |
| `0x0065d3ee` | `_G_LLM_TUTORIAL_WELCOME_TEXT_BUF` | `wchar_t[256]` |
| `0x0065d5ee` | `_G_LLM_TUTORIAL_COLORS_RGB` | `int[5][3]` |
| `0x0065d62a` | `_G_LLM_TUTORIAL_UISTATE_BACKUP` | `undefined1[48]` |
| `0x0065d65a` | `_G_LLM_STRAT_AI_ATTACK_HQ_UNIT_ID` | `uint` |
| `0x0065d65e` | `_G_LLM_LOBBY_RX_PACKET_LEN` | `uint` |
| `0x0065d662` | `_G_LLM_LOBBY_RX_CRC32_COMPUTED` | `uint` |
| `0x0065d666` | `_G_LLM_LOBBY_RX_SENDER_SLOT` | `int` |
| `0x0065d66a` | `_G_LLM_LOBBY_RX_SENDER_ID` | `int` |
| `0x0065d66e` | `_G_LLM_LOBBY_RX_TYPE` | `llm_lobby_packet_type` |
| `0x0065d66f` | `_G_LLM_LOBBY_RX_CRC32_RECV` | `uint` |
| `0x0065d673` | `_G_LLM_LOBBY_RX_PAYLOAD` | `undefined` |
| `0x0065da6a` | `_G_LLM_MP_NETSETUP_MRU_ACTIVE_FIELD` | `void *` |
| `0x0065da6e` | `_G_LLM_LOBBY_KICK_PENDING_SLOT` | `undefined4` |
| `0x0065da72` | `_G_LLM_LOBBY_INTRO_WAIT_SHOWN` | `undefined4` |
| `0x0065da76` | `_G_LLM_LOBBY_PEER_TABLE` | `llm_net_lobby_peer[7]` |
| `0x0065f692` | `_G_LLM_LOBBY_PEER_TABLE_COUNT` | `int` |
| `0x0065f69f` | `_G_LLM_UI_INFO_DDRAW_SURFACE_DESC` | `llm_ddraw_surface_desc` |
| `0x0065f713` | `_G_LLM_GFX_VIDEO_SURFACE` | `void *` |
| `0x0065f717` | `_G_LLM_GFX_VIDEO_SURFACE2` | `void *` |
| `0x0065f71b` | `_G_LLM_CD_RECHECK_IN_PROGRESS` | `undefined4` |
| `0x0065f79f` | `_G_LLM_PLANET_SEL_SCREEN_OPEN` | `undefined4` |
| `0x0065f7a3` | `_G_LLM_UI_OUTCOME_DLG_REVEAL_TOTAL` | `int` |
| `0x0065f7a7` | `_G_LLM_UI_OUTCOME_DLG_STAT_ROW_COUNT` | `int` |
| `0x0065f7ab` | `_G_LLM_UI_SELECTED_BUILDING_INDEX` | `uint` |
| `0x0065f82f` | `_G_LLM_UI_INFO_MEDIA_CTX` | `llm_ui_info_media_ctx` |
| `0x0065faeb` | `_G_LLM_UI_INFO_TXT_SEARCH_KEY` | `wchar_t[7]` |
| `0x0065fb01` | `_G_LLM_UI_INFO_TEXT_CURSOR_LEN` | `undefined` |
| `0x0065fd30` | `_G_LLM_CD_AUDIO_TRACK_BASE` | `undefined4` |
| `0x0065fd48` | `_G_LLM_CD_AUDIO_MCI_CALLBACK_HWND` | `uint` |
| `0x0065fd4c` | `_G_LLM_UI_AVI_LAST_RESULT` | `undefined4` |
| `0x0065fd50` | `_G_LLM_UI_ACM_LAST_RESULT` | `undefined4` |
| `0x0065fd54` | `_G_LLM_NET_LOCAL_IP_COUNT` | `undefined4` |
| `0x0065fd58` | `_G_LLM_NET_LOCAL_HOSTNAME` | `undefined4` |
| `0x00661548` | `_G_LLM_GAME_CLOCK_PAUSE_TICK` | `undefined4` |
| `0x0066155c` | `_G_LLM_DI_KEYBOARD_DEVICE` | `undefined4` |
| `0x00661560` | `_G_LLM_DI_MOUSE_DEVICE` | `undefined4` |
| `0x00661564` | `_G_LLM_INPUT_DI_MOUSE_ACCEL_THRESHOLD` | `int` |
| `0x00661568` | `_G_LLM_INPUT_DI_MOUSE_DIVISOR` | `int` |
| `0x006616a8` | `_G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT` | `undefined4` |
| `0x006616ac` | `_G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS` | `int[4]` |
| `0x006616bc` | `_G_LLM_STRAT_AI_MINE_YIELD_KERNEL` | `llm_strat_ai_mine_kernel_cell[25]` |
| `0x0066931c` | `_G_LLM_STRAT_AI_STRATEGY_PERIOD` | `float` |
| `0x00669320` | `_G_LLM_STRAT_AI_TACTIC_PERIOD` | `float` |
| `0x00669324` | `_G_LLM_STRAT_AI_MOVE_PERIOD` | `float` |
| `0x00669330` | `_G_LLM_STRAT_AI_HOME_GUARD_TARGET_PCT` | `int` |
| `0x00669344` | `_G_LLM_STRAT_AI_PATROL_GROUP_SIZE` | `int` |
| `0x00669348` | `_G_LLM_STRAT_AI_PATROL_GROUP_MAX` | `int` |
| `0x0066934c` | `_G_LLM_STRAT_AI_PATROL_WANDER_BOUNCES` | `int` |
| `0x00669350` | `_G_LLM_STRAT_AI_LOITER_JITTER_RADIUS` | `int` |
| `0x00669360` | `_G_LLM_STRAT_AI_EXPAND_MIN_BUILDING_COUNT` | `int` |
| `0x00669364` | `_G_LLM_STRAT_AI_EXPAND_MIN_TICKS` | `int` |
| `0x00669368` | `_G_LLM_STRAT_AI_RELAY_TO_MINE_RATIO_PCT` | `int` |
| `0x0066936c` | `_G_LLM_STRAT_AI_CFG_MINE_WORTH` | `int` |
| `0x00669370` | `_G_LLM_STRAT_AI_UNEMPLOYED_MIN` | `int` |
| `0x00669374` | `_G_LLM_STRAT_AI_UNEMPLOYED_RATIO` | `float` |
| `0x00669378` | `_G_LLM_STRAT_AI_MAX_FUCK_RATIO` | `float` |
| `0x0066937c` | `_G_LLM_STRAT_AI_EXTRA_SPACE` | `float` |
| `0x00669380` | `_G_LLM_STRAT_AI_ENERGY_RATIO` | `float` |
| `0x00669384` | `_G_LLM_STRAT_AI_SILO_RATIO` | `float` |
| `0x00669388` | `_G_LLM_STRAT_AI_REPAIR_RATIO` | `float` |
| `0x0066938c` | `_G_LLM_STRAT_AI_TRAIN_QUEUE_PER_UNIT_CAP` | `uint` |
| `0x00669390` | `_G_LLM_STRAT_AI_CFG_PROMO_ADD` | `undefined4` |
| `0x00669394` | `_G_LLM_STRAT_AI_CFG_PROMO_SUB` | `undefined4` |
| `0x00669398` | `_G_LLM_STRAT_AI_CFG_START_UNITS` | `int` |
| `0x0066939c` | `_G_LLM_STRAT_AI_REPOSITION_SMALL_GROUP_MAX` | `int` |
| `0x006693a0` | `_G_LLM_STRAT_AI_SURPLUS_GROUP_MIN_POOL4` | `int` |
| `0x006693a4` | `_G_LLM_STRAT_AI_ATTACK_OVERKILL_PCT` | `int` |
| `0x006693a8` | `_G_LLM_STRAT_AI_MIN_ATTACK_STRENGTH` | `int` |
| `0x006693ac` | `_G_LLM_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD` | `uint` |
| `0x006693b0` | `_G_LLM_STRAT_AI_ATTACK_MILESTONE_COUNT` | `int` |
| `0x006693b4` | `_G_LLM_STRAT_AI_SCR_KEYWORD_TABLE` | `undefined4` |
| `0x0066991c` | `_G_LLM_SND_USE_DSOUND_BACKEND` | `bool` |
| `0x00669928` | `_G_LLM_SND_VOICE_FORMAT_SCRATCH` | `undefined4` |
| `0x00669990` | `_G_LLM_UNITQ_NEIGHBOR_DX` | `int[8]` |
| `0x006699b0` | `_G_LLM_UNITQ_NEIGHBOR_DY` | `int[8]` |
| `0x006699d0` | `_G_LLM_GFX_DDRAW_DLL_MODULE` | `HMODULE` |
| `0x006699d4` | `_G_LLM_GFX_DDRAW_DLL_REFCOUNT` | `int` |
| `0x006699d8` | `_G_LLM_GFX_DDRAW_CREATE_PROC` | `DirectDrawCreate *` |
| `0x0066a0a4` | `_G_LLM_STRAT_DIR_BITMASK_TABLE` | `uint[8]` |
| `0x0066a124` | `_G_LLM_STRAT_COORD_SIGN_LUT` | `byte[256]` |
| `0x0066a224` | `_G_LLM_STRAT_MOVE_DIR_TABLE` | `llm_strat_move_dir_step[24]` |
| `0x0066a2e4` | `_G_LLM_STRAT_DIR8_STEP_OFFSETS` | `char[16]` |
| `0x0066a2f4` | `_G_LLM_STRAT_PATHTRACE_DIR_MERGE_LUT` | `byte[64]` |
| `0x0066a334` | `_G_LLM_STRAT_PATHTRACE_COORD_MASK` | `uint` |
| `0x0066a338` | `_G_LLM_STRAT_PATHTRACE_COL_MASK` | `uint` |
| `0x0066a33c` | `_G_LLM_STRAT_PATHTRACE_ROW_MASK` | `uint` |
| `0x0066a340` | `_G_LLM_STRAT_PATHTRACE_MAP_W` | `uint` |
| `0x0066a344` | `_G_LLM_STRAT_PATHTRACE_MAP_H` | `uint` |
| `0x0066a348` | `_G_LLM_STRAT_PATHTRACE_HALF_W` | `uint` |
| `0x0066a34c` | `_G_LLM_STRAT_PATHTRACE_HALF_H` | `uint` |
| `0x0066a350` | `_G_LLM_STRAT_PATHTRACE_HALF_W_M1` | `uint` |
| `0x0066a354` | `_G_LLM_STRAT_PATHTRACE_HALF_H_M1` | `uint` |
| `0x0066a358` | `_G_LLM_STRAT_PATHTRACE_NEG_HALF_W` | `int` |
| `0x0066a35c` | `_G_LLM_STRAT_PATHTRACE_NEG_HALF_H` | `int` |
| `0x0066a360` | `_G_LLM_STRAT_PATHTRACE_START_COL` | `uint` |
| `0x0066a364` | `_G_LLM_STRAT_PATHTRACE_START_ROW` | `uint` |
| `0x0066a368` | `_G_LLM_STRAT_PATHTRACE_WALK_DIR` | `uint` |
| `0x0066a36c` | `_G_LLM_STRAT_PATHTRACE_GOAL_COL` | `uint` |
| `0x0066a370` | `_G_LLM_STRAT_PATHTRACE_GOAL_ROW` | `uint` |
| `0x0066a374` | `_G_LLM_STRAT_PATHTRACE_GOAL_PACKED` | `uint` |
| `0x0066a378` | `_G_LLM_STRAT_PATHTRACE_APPROACH_DIR` | `uint` |
| `0x0066a37c` | `_G_LLM_STRAT_PATHTRACE_LEN` | `uint` |
| `0x0066a380` | `_G_LLM_STRAT_PATHTRACE_DIRS` | `byte[512]` |
| `0x0066a580` | `_G_LLM_STRAT_PATHTRACE_POS` | `ushort[512]` |
| `0x0066a980` | `_G_LLM_STRAT_PATHTRACE_BEST_DIR` | `uint` |
| `0x0066a984` | `_G_LLM_STRAT_PATHTRACE_BEST_DIST` | `int` |
| `0x0066a988` | `_G_LLM_STRAT_PATHTRACE_FORBID_CELLS` | `uint[7]` |
| `0x0066a9a4` | `_G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG` | `undefined4` |
| `0x0066b3e1` | `_G_LLM_STRAT_PATHTRACE_DIR_SPLIT_BASE_M8` | `?` |
| `0x0066b421` | `_G_LLM_STRAT_PATHTRACE_DIR_SPLIT_TABLE` | `llm_strat_pathtrace_dir_split[16]` |
| `0x0066d155` | `_G_LLM_CRT_DEFAULT_FILE_TRANSLATE_MODE` | `int` |
| `0x0066d160` | `_G_LLM_CRT_CUSTOM_WRITE_LOOKUP_HOOK` | `void *` |
| `0x0066d18c` | `_G_LLM_CRT_CUSTOM_WRITE_HOOK` | `void *` |
| `0x0066f3f8` | `_G_LLM_STRAT_AI_QUADRANT_DX2` | `int[4]` |
| `0x0066f408` | `_G_LLM_STRAT_AI_QUADRANT_DY2` | `int[4]` |
| `0x0066f418` | `_G_LLM_STRAT_AI_QUADRANT_DX1` | `int[4]` |
| `0x0066f428` | `_G_LLM_STRAT_AI_QUADRANT_DY1` | `int[4]` |
| `0x0066f438` | `_G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS` | `int[4]` |
| `0x0066f6ac` | `_G_LLM_LZW_ENC_NEXT_CHAR` | `undefined4` |
| `0x0066f6b0` | `_G_LLM_LZW_ENC_CODE_WIDTH` | `undefined` |
| `0x0066f6b3` | `_G_LLM_LZW_ENC_DICT_SIZE` | `undefined4` |
| `0x0067b6b7` | `_G_LLM_LZW_ENC_CHAIN_TAILS` | `undefined` |
| `0x00680078` | `_G_LLM_UI_BLDG_RESOURCE_ROW_TEXT_X` | `undefined4` |
| `0x0068007c` | `_G_LLM_UI_BLDG_RESOURCE_ROW_HIGHLIGHT_FLAG` | `undefined4` |
| `0x00680080` | `_G_LLM_UI_BLDG_RESOURCE_ROW_STEP_Y` | `undefined4` |
| `0x00680084` | `_G_LLM_UI_BLDG_RESOURCE_ROW_STEP_X` | `undefined4` |
| `0x00680088` | `_G_LLM_UI_BLDG_RESOURCE_ROW_ADVANCE_X` | `int` |
| `0x0068008c` | `_G_LLM_UI_BLDG_RESOURCE_ROW_TEXT_Y_OFFSET` | `undefined` |
| `0x00680090` | `_G_LLM_UI_BLDG_RESOURCE_ROW_LAYOUT_MODE` | `int` |
| `0x00680094` | `_G_LLM_UI_BLDG_RESOURCE_ROW_X` | `int` |
| `0x00680098` | `_G_LLM_UI_BLDG_RESOURCE_ROW_Y` | `int` |
| `0x006800ac` | `_G_LLM_STRAT_GROUP_ROUTE_STEPS` | `llm_strat_route_step[256]` |
| `0x006802ac` | `_G_LLM_STRAT_GROUP_MEMBER_TILE` | `byte[256][2]` |
| `0x006804ac` | `_G_LLM_STRAT_GROUP_MEMBERS` | `llm_strat_group_member[256]` |
| `0x00680aac` | `_G_LLM_STRAT_PATH_WRAP_MASK` | `uint` |
| `0x00680ab0` | `_G_LLM_STRAT_GROUP_ORDER_OWNER` | `undefined4` |
| `0x00680ab4` | `_G_LLM_STRAT_GROUP_ORDER_GOAL_Y` | `undefined4` |
| `0x00680ab8` | `_G_LLM_STRAT_GROUP_ORDER_GOAL_X` | `int` |
| `0x00680abc` | `_G_LLM_STRAT_GROUP_ANCHOR_Y` | `int` |
| `0x00680ac0` | `_G_LLM_STRAT_GROUP_ANCHOR_X` | `int` |
| `0x00680ac4` | `_G_LLM_MAP_REGION_FLOOD_TILE_QUEUE` | `uint[4096]` |
| `0x00684ac4` | `_G_LLM_MAP_REGION_BY_INDEX` | `llm_map_region *[4096]` |
| `0x00688ac4` | `_G_LLM_MAP_REGION_GRID` | `llm_map_region_cell[256][256]` |
| `0x00708acc` | `_G_LLM_MAP_REGION_MERGE_THRESHOLD` | `uint` |
| `0x00708ad0` | `_G_LLM_MAP_REGION_ROUTE_CAND_SCRATCH` | `ushort[16]` |
| `0x00708af0` | `_G_LLM_MAP_REGION_ROUTE_BEST_CAND` | `undefined` |
| `0x00708af4` | `_G_LLM_MAP_REGION_ROUTE_STEP_DELTAS` | `byte[32]` |
| `0x00708b14` | `_G_LLM_MAP_REGION_ROUTE_STEP_DELTA_WRAP` | `byte[4]` |
| `0x00708b1c` | `_G_LLM_MAP_REGION_COORD_WRAP_MASK` | `uint` |
| `0x00708b20` | `_G_LLM_MAP_BFS_QUEUE` | `llm_map_bfs_entry[2048]` |
| `0x0070ab20` | `_G_LLM_MAP_REGION_ROUTE_BFS_QUEUE` | `llm_map_region *[512]` |
| `0x0070b320` | `_G_LLM_MAP_BFS_QUEUE` | `llm_map_bfs_entry[2048]` |
| `0x0070b320` | `_G_LLM_MAP_BFS_QUEUE_REGIONSPLIT` | `llm_map_bfs_entry[2048]` |
| `0x0070d5c0` | `_G_LLM_SND_CFG_TABLE` | `llm_snd_cfg_entry[200]` |
| `0x0070f82c` | `_G_LLM_SND_CFG_TABLE_COUNT` | `undefined4` |
| `0x0070fc50` | `_G_LLM_TACT_VIEW_TILE_LOS_CACHE` | `undefined` |
| `0x00710050` | `_G_LLM_GFX_DISPLAY_MODE_DESC` | `llm_gfx_ddraw_device` |
| `0x00710094` | `_G_LLM_GFX_DDRAW_DEVICE` | `?` |
| `0x0071009c` | `_G_LLM_GFX_DDRAW_AUX` | `?` |
| `0x00712120` | `_G_LLM_TACT_VIEW_TILE_SURFACE_ROW_PTRS` | `undefined` |
| `0x00712920` | `_G_LLM_TACT_VIEW_TILE_FB_ROW_PTRS` | `undefined` |
| `0x00713920` | `_G_LLM_TILE_DRAWN_MAP` | `byte[300]` |
| `0x00713d1a` | `_G_LLM_TILE_VIS_MAP_M6` | `byte` |
| `0x00713d1b` | `_G_LLM_TILE_VIS_MAP_M5` | `byte` |
| `0x00713d1c` | `_G_LLM_TILE_VIS_MAP_M4` | `byte` |
| `0x00713d1d` | `_G_LLM_TILE_VIS_MAP_M3` | `byte` |
| `0x00713d1e` | `_G_LLM_TILE_VIS_MAP_M2` | `byte` |
| `0x00713d1f` | `_G_LLM_TILE_VIS_MAP_M1` | `byte` |
| `0x00713d20` | `_G_LLM_TILE_VIS_MAP` | `byte[300]` |
| `0x00714120` | `_G_LLM_SPRITE_META` | `gfx_sprite_meta[22000]` |
| `0x00824fa0` | `_G_LLM_STRAT_MINIMAP_VIEWPORT_BOX_W` | `int` |
| `0x00824fa4` | `_G_LLM_GFX_BAR_SPRITE_FILL_COLOR` | `ushort` |
| `0x00824fa8` | `_G_LLM_STRAT_MINIMAP_VIEWPORT_BOX_H` | `int` |
| `0x00824fac` | `_G_LLM_STRAT_MINIMAP_VIEWPORT_BOX_OFS` | `int` |
| `0x00824fb8` | `_G_LLM_GFX_SPRITE_HITTEST_CURSOR` | `undefined4` |
| `0x00824fc4` | `_G_LLM_TACT_LOS_CACHE_PTR` | `undefined4` |
| `0x00824fc8` | `_G_LLM_GFX_FOG_LOOKUP_ROW` | `byte` |
| `0x00824fcc` | `_G_LLM_GFX_FOG_LOOKUP_COL` | `byte` |
| `0x00824fd4` | `_G_LLM_PLANET_TLO_TILE_COUNT` | `undefined4` |
| `0x00824fdc` | `_G_LLM_GFX_POLALFA2_GLYPH_PTR` | `undefined4` |
| `0x00824fe0` | `_G_LLM_GFX_POLALFA1_GLYPH_PTR` | `undefined4` |
| `0x00824fe4` | `_G_LLM_FOG_STENCILS_PTR` | `undefined4` |
| `0x00824ff4` | `_G_LLM_TILE_VIS_MAP_PTR` | `undefined4` |
| `0x00824ff8` | `_G_LLM_TACT_VIEW_TILE_SURFACE_ROW_PTRS_PTR` | `pointer` |
| `0x00824ffc` | `_G_LLM_TILE_DRAWN_MAP_PTR` | `undefined4` |
| `0x00825000` | `_G_LLM_STRAT_SAVE_MISC_DWORD` | `int` |
| `0x00825004` | `_G_LLM_TACT_VIEW_TILE_FB_ROW_PTRS_PTR` | `pointer` |
| `0x00825028` | `_G_LLM_GFX_DASHED_RECT_BOTTOM` | `int` |
| `0x0082502c` | `_G_LLM_GFX_DASHED_RECT_TOP` | `int` |
| `0x00825030` | `_G_LLM_GFX_DASHED_RECT_RIGHT` | `int` |
| `0x00825034` | `_G_LLM_GFX_DASHED_RECT_LEFT` | `int` |
| `0x00825038` | `_G_LLM_GFX_BANK_PLANET_PIXEL_RESERVE` | `uint` |
| `0x0082503c` | `_G_LLM_CUR_TILE_LAYER` | `undefined1` |
| `0x0082504c` | `_G_LLM_MOUSE_BUTTONS_CUR` | `undefined1` |
| `0x0082505c` | `_G_LLM_CURSOR_HOTSPOT_Y` | `int` |
| `0x00825060` | `_G_LLM_CURSOR_HOTSPOT_X` | `int` |
| `0x00825068` | `_G_LLM_VIEW_TILES_H` | `undefined4` |
| `0x0082506c` | `_G_LLM_GFX_TILE_DRAW_ROW` | `undefined4` |
| `0x00825070` | `_G_LLM_GFX_TILE_DRAW_COL` | `int` |
| `0x00825074` | `_G_LLM_CURSOR_PALETTE` | `undefined4` |
| `0x00825078` | `_G_LLM_TACT_MAP_TILE_HEIGHT_SPRITES` | `undefined4` |
| `0x0082507c` | `_G_LLM_VIEW_RADIUS_H` | `int` |
| `0x00825088` | `_G_LLM_GFX_TILE_DRAW_TERRAIN_ID` | `undefined4` |
| `0x0082508c` | `_G_LLM_VIEW_RADIUS_W` | `undefined4` |
| `0x00825090` | `_G_LLM_VIEW_TILES_W` | `undefined4` |
| `0x00825098` | `_G_LLM_GFX_TILE_DRAW_FOG_HI` | `undefined1` |
| `0x00825099` | `_G_LLM_GFX_TILE_DRAW_FOG_LO` | `undefined1` |
| `0x0082509c` | `_G_LLM_TACT_FOV_STENCIL` | `undefined1[4096]` |
| `0x0082609c` | `_G_LLM_TACT_FOV_DIST` | `int` |
| `0x008260a0` | `_G_LLM_TACT_FOV_ROW` | `int` |
| `0x008260a4` | `_G_LLM_TACT_SAVED_WINDOW_WIDTH` | `int` |
| `0x008260a8` | `_G_LLM_TACT_CLICK_ACTION_TAKEN` | `int` |
| `0x008260ac` | `_G_LLM_TACT_FOV_ANGLE_BASE` | `undefined4` |
| `0x008260b0` | `_G_LLM_TACT_FOV_COL` | `int` |
| `0x008260b4` | `_G_LLM_TACT_UNIT_POSE_TAG` | `char[8]` |
| `0x008260c8` | `_G_LLM_TACT_FOV_ANGLE_WIDTH` | `uint` |
| `0x008260cc` | `_G_LLM_TACT_FOV_NEAREST_LOW_CELL` | `undefined2` |
| `0x008260ce` | `_G_LLM_TACT_FOV_NEAREST_HIBIT_CELL` | `undefined2` |
| `0x008260d0` | `_G_LLM_TACT_UNITS` | `tact_unit_record[129]` |
| `0x008566b8` | `_G_LLM_TACT_FX_TILE_DRAW_LISTS` | `undefined` |
| `0x0085b4d8` | `_G_LLM_TACT_MOVE_CUR_COL` | `undefined4` |
| `0x0085b4dc` | `_G_LLM_TACT_MOVE_CUR_ROW` | `undefined4` |
| `0x0085b4e0` | `_G_LLM_TACT_FX_POOL` | `llm_tact_fx[1024]` |
| `0x0086fd34` | `_G_LLM_TACT_SIDEBAR_ICON_SLOT_UNIT_LUT` | `byte[40]` |
| `0x0086fd84` | `_G_LLM_TACT_UNASSIGNED_UNIT_ROSTER` | `llm_tact_unit_roster_slot` |
| `0x0086fe84` | `_G_LLM_TACT_GROUP_UNIT_ROSTER` | `llm_tact_unit_roster_slot[8]` |
| `0x00870884` | `_G_LLM_TACT_SIDEBAR_SLOT_UNIT_IDS` | `int[64]` |
| `0x00870984` | `_G_LLM_TACT_FOG_GLYPH_BLEND_TABLE` | `byte[8192]` |
| `0x00872984` | `_G_LLM_TACT_FX_TYPE_TABLE` | `llm_tact_fx_type[64]` |
| `0x00873c04` | `_G_LLM_TACT_CHARACTER_TYPES` | `llm_tact_character_type[16]` |
| `0x008742c4` | `_G_LLM_TACT_DOOR_TABLE` | `llm_tact_door[16]` |
| `0x00874948` | `_G_LLM_TACT_MAP_WIDTH_CACHE` | `undefined4` |
| `0x0087494c` | `_G_LLM_TACT_MAP_HEIGHT_CACHE` | `undefined4` |
| `0x00874950` | `_G_LLM_TACT_TELEPORT_TABLE` | `llm_tact_teleport[66]` |
| `0x00875658` | `_G_LLM_TLO_SHADE_TABLE` | `undefined` |
| `0x00879668` | `_G_LLM_GFX_RGB565_HALVE_MASK2` | `undefined4` |
| `0x00879670` | `_G_LLM_RGB565_HALVE_MASK` | `undefined4` |
| `0x0095c424` | `_G_LLM_STRAT_PATHFIND_OCCUPANCY_SNAPSHOT` | `undefined1[65536]` |
| `0x0096c424` | `_G_LLM_STRAT_PATHFIND_UNIT_COLROW` | `ushort[100]` |
| `0x00a49868` | `_G_LLM_CAM_JUMP_QUEUE` | `llm_vec2i[60]` |
| `0x00a49a48` | `_G_LLM_CFG_PROJECTS_INIT_SNAPSHOT` | `undefined` |
| `0x00a4eca8` | `_G_LLM_STRAT_HEADING_CANDIDATE_TABLE` | `llm_strat_heading_slot[72]` |
| `0x00ae18e8` | `_G_LLM_STRAT_GROUP_STEP_HEADING_REMAP` | `int[24]` |
| `0x00ae1948` | `_G_LLM_STRAT_PLAYER_COLOR_LUT` | `undefined` |
| `0x00ae1958` | `_G_LLM_STRAT_PATH_JOB_RESULT_TABLE` | `llm_strat_job_result_entry[100]` |
| `0x00ae1c78` | `_G_LLM_STRAT_DIR_REMAP_TABLE` | `llm_strat_dir_remap_row[24]` |
| `0x00ae1df8` | `_G_LLM_STRAT_WALKER_FRAME_TABLE` | `undefined` |
| `0x00ae2a90` | `_G_LLM_NET_BW_STAT` | `int[2]` |
| `0x00ae2aa0` | `_G_LLM_STRAT_FPS_ESTIMATE` | `double` |
| `0x00ae2aa8` | `_G_LLM_VIEW_SIZE_MODE` | `undefined4` |
| `0x00ae2aac` | `_G_LLM_GAME_MUSIC_ENABLED` | `undefined4` |
| `0x00ae2ab0` | `_G_LLM_SND_ENABLED` | `undefined4` |
| `0x00ae2ab4` | `_G_LLM_SND_MASTER_VOLUME` | `undefined4` |
| `0x00ae2ab8` | `_G_LLM_SND_MUSIC_VOLUME_PCT` | `int` |
| `0x00ae2abc` | `_G_LLM_UI_SHOW_HEALTH_BARS` | `int` |
| `0x00ae2b00` | `_G_LLM_ANIM_PLACE_DENIED` | `int` |
| `0x00ae2b04` | `_G_LLM_ANIM_PLACE_ALLOWED` | `int` |
| `0x00ae2b08` | `_G_LLM_TACT_CHAR_SHIFT_LUT` | `undefined4` |
| `0x00ae2d08` | `_G_LLM_STRAT_DIR_STEP_OFFSET_TABLE` | `llm_vec2i[40]` |
| `0x00ae2e48` | `_G_LLM_STRAT_GROUP_MOVE_SCRATCH` | `llm_strat_group_scratch_member[100]` |
| `0x00ae3618` | `_G_LLM_STRAT_SQUAD_PLACEMENT_OFFSET_TABLE` | `llm_squad_placement_offset[6][6]` |
| `0x00ae3738` | `_G_LLM_STRAT_TICK_BUDGET` | `double` |
| `0x00ae3740` | `_G_LLM_STRAT_MOVE_MICROSTEPS` | `llm_strat_move_microstep[1][32]` |
| `0x00ae4640` | `_G_LLM_STRAT_SQUAD_FORMATION_ANCHOR_SCRATCH` | `llm_strat_squad_formation_anchor_scratch[5]` |
| `0x00ae4668` | `_G_LLM_CURSOR_ANIM_STATES` | `gfx_struct_cursor_anim_state[16]` |
| `0x00ae4768` | `_G_LLM_STRAT_FACING_STEP_SIGN` | `llm_vec2i[25]` |
| `0x00ae4830` | `_G_LLM_STRAT_BLDG_DONE_FUNCS` | `pointer[100]` |
| `0x00ae49c0` | `_G_LLM_STRAT_BLDG_TICK2_FUNCS` | `pointer[100]` |
| `0x00ae4b50` | `_G_LLM_STRAT_DIR8_OFFSET_TABLE` | `llm_strat_dir8_offset[8]` |
| `0x00aee8c0` | `_G_LLM_STRAT_PATH_BUFFERS` | `llm_strat_path_waypoint[240000]` |
| `0x00b63bc0` | `_G_LLM_STRAT_PATH_FREE_SLOT_COUNT` | `int[8]` |
| `0x00b63be0` | `_G_LLM_STRAT_CTRL_GROUPS` | `llm_strat_ctrl_group[10]` |
| `0x00b64ba8` | `_G_LLM_STRAT_ADVISOR_NEXT_TIME` | `double` |
| `0x00b74bb0` | `_G_LLM_STRAT_PATH_SLOT_FLAGS` | `byte[800]` |
| `0x00b74ed0` | `_G_LLM_TACT_MOVE_COST_MAP` | `int[65536]` |
| `0x00bb4ed0` | `_G_LLM_STRAT_ORDER_QUEUE` | `llm_strat_order[300]` |
| `0x00bb9e80` | `_G_LLM_STRAT_ORDER_PENDING` | `llm_strat_order[1000]` |
| `0x00bca820` | `_G_LLM_STRAT_INVASION_ALERT_TIME` | `double[32]` |
| `0x00bca920` | `_G_LLM_STRAT_ORDER_STAGING` | `llm_strat_order[300]` |
| `0x00bd215c` | `_G_LLM_PROD_SHUTTLE_SLOTS` | `llm_prod_shuttle_slot[80]` |
| `0x00bef280` | `_G_LLM_SND_AMBIENT_BY_PLANET` | `llm_snd_ambient_planet_list[32]` |
| `0x00bf4d00` | `_G_LLM_STRAT_STORAGE_STATS` | `llm_strat_storage_stats[8]` |
| `0x00bf4fc0` | `_G_LLM_STRAT_POWER_STATS` | `llm_strat_power_stats[8]` |
| `0x00bf5080` | `_G_LLM_STRAT_DEATH_ANIM_TABLE` | `cfg_t_frame_index[6][4]` |
| `0x00bf50e0` | `_G_LLM_STRAT_FX_ANIMS` | `llm_strat_fx_anim[10000]` |
| `0x00c38530` | `_G_LLM_STRAT_UNIT_HOUSING_STATS` | `llm_strat_unit_housing_stats[8]` |
| `0x00c38730` | `_G_LLM_BLDG_PIP_ANIM_BASE_FRAME` | `int[4]` |
| `0x00c3a370` | `_G_LLM_STRAT_POP_STATS` | `llm_strat_pop_stats[8]` |
| `0x00c7e660` | `_G_LLM_STRAT_MINIMAP_SKY_IMG` | `undefined` |
| `0x00c86660` | `_G_LLM_MAP_OBJECTS` | `llm_map_object[10000]` |
| `0x00cc46e0` | `_G_LLM_MAP_OBJECT_TABLE` | `undefined1[240000]` |
| `0x00cff060` | `_G_LLM_STRAT_PLAYERS` | `llm_strat_player_profile[8]` |
| `0x00d033c0` | `_G_LLM_CHEAT_CMD_TABLE` | `WCHAR *[48]` |
| `0x00d06c80` | `_G_LLM_MAP_HALFRES_GRID` | `undefined1[65536]` |
| `0x00d16c80` | `_G_LLM_STRAT_MINIMAP_GLOBE_IMG` | `undefined` |
| `0x00d1eb88` | `_G_LLM_TACT_TILE_FOG` | `undefined` |
| `0x00d1eb8f` | `_G_LLM_TACT_TILE_VIS_REFCOUNT` | `undefined` |
| `0x00dd8b88` | `_G_LLM_STRAT_LANDING_SPOTS` | `llm_strat_landing_spot[16]` |
| `0x00e06468` | `_G_LLM_STRAT_MINIMAP_VIGNETTE_MASK` | `undefined` |
| `0x00e0e5a8` | `_G_LLM_STRAT_SOLDIERS` | `llm_strat_crew_soldier[8][100]` |
| `0x00e15388` | `_G_LLM_STRAT_CUR_FX_ANIM` | `llm_strat_fx_anim *` |
| `0x00e1538c` | `_G_LLM_STRAT_CUR_PROJECTILE` | `llm_strat_projectile *` |
| `0x00e153c0` | `_G_LLM_HIT_LIST` | `byte[60]` |
| `0x00e153fc` | `_G_LLM_HIT_LIST_COUNT` | `int` |
| `0x00e15404` | `_G_LLM_HIT_LIST_PREV_COUNT` | `int` |
| `0x00e1540c` | `_G_LLM_HIT_LIST_PREV` | `byte[60]` |
| `0x00e15448` | `_G_LLM_STRAT_BLDG_STATE_FUNCS` | `pointer[255]` |
| `0x00e15848` | `_G_LLM_STRAT_FACING_TRIG_TABLE` | `llm_facing_trig[24]` |
| `0x00e15a38` | `_G_LLM_STRAT_STATE_LOOP_GUARD` | `int` |
| `0x00e15a3c` | `_G_LLM_STRAT_UNIT_STATE_FUNCS` | `void *[255]` |
| `0x00e15e38` | `_G_LLM_STRAT_CAM_SCROLL_OFFSET_COL` | `int` |
| `0x00e15e3c` | `_G_LLM_STRAT_CAM_SCROLL_OFFSET_ROW` | `int` |
| `0x00e15e40` | `_G_LLM_MINIMAP_CLICK_COL` | `int` |
| `0x00e15e44` | `_G_LLM_MINIMAP_CLICK_ROW` | `undefined` |
| `0x00e15e48` | `_G_LLM_SQUAD_BB_SCAN_PLAYER` | `int` |
| `0x00e15e4c` | `_G_LLM_SQUAD_BB_TARGET_OWNER` | `int` |
| `0x00e15e50` | `_G_LLM_SQUAD_BB_TARGET_BUILDING_ID` | `int` |
| `0x00e15e54` | `_G_LLM_SQUAD_BB_TARGET_ENERGY_PCT` | `int` |
| `0x00e15e58` | `_G_LLM_SQUAD_BB_TARGET_BUILDING_IDX` | `int` |
| `0x00e15e60` | `_G_LLM_SQUAD_STATUS` | `llm_squad_status_slot[64]` |
| `0x00e15e64` | `_G_LLM_SQUAD_STATUS` | `?` |
| `0x00e16260` | `_G_LLM_SQUAD_STATUS_COUNT` | `int` |
| `0x00e16264` | `_G_LLM_STRAT_FLOATING_MSG_QUEUE_ACTIVE` | `int` |
| `0x00e16268` | `_G_LLM_STRAT_ORDER_SCRATCH_ARGS` | `int[13]` |
| `0x00e1629c` | `_G_LLM_CLICK_SELECT_TARGET_ID` | `undefined2` |
| `0x00e1629e` | `_G_LLM_CLICK_SELECT_TARGET_FLAGS` | `undefined2` |
| `0x00e162a0` | `_G_LLM_STRAT_FLOATING_MSG_COLOR` | `ushort` |
| `0x00e162dc` | `_G_LLM_STRAT_CUR_BUILDING` | `map_object_building *` |
| `0x00e162e0` | `_G_LLM_STRAT_CUR_UNIT` | `map_object_unit *` |
| `0x00e1db98` | `_G_LLM_STRAT_PROJECTILE_POOL` | `llm_strat_projectile[1500]` |
| `0x00e4a094` | `_G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING` | `int` |
| `0x00e58134` | `_G_LLM_MAP_CAM_ROW` | `int` |
| `0x00e58138` | `_G_LLM_MAP_CAM_COL` | `int` |
| `0x00e5813c` | `_G_LLM_STRAT_UI_SELECTED_BLDG_INDEX` | `ushort` |
| `0x00e58142` | `_G_LLM_STRAT_CUR_INDEX` | `ushort` |
| `0x00e58144` | `_G_LLM_STRAT_CUR_PLAYER` | `game_t_Player_s` |
| `0x00e58146` | `_G_LLM_STRAT_LAND_DMP_PATH_SCRATCH` | `char[32]` |
| `0x00e58245` | `_G_LLM_STRAT_DMP_PATH_SCRATCH` | `char[32]` |
| `0x00e58344` | `_G_LLM_GAME_SESSION_MODE` | `E_SESSION_MODE` |
| `0x00e58348` | `_G_LLM_DEBUG_TAP_FLAG` | `int` |
| `0x00e5834c` | `_G_LLM_GAME_TUTORIAL_STEP` | `undefined4` |
| `0x00e58350` | `_G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG` | `int` |
| `0x00e58356` | `_G_LLM_STRAT_DEBUG_RESOURCE_YIELD_CUT` | `double` |
| `0x00e5835e` | `_G_LLM_STRAT_LOCAL_PLAYER_SLOT` | `undefined2` |
| `0x00e58360` | `_G_LLM_STRAT_PROD_COMPLETE_THROTTLE` | `byte` |
| `0x00e58361` | `_G_LLM_STRAT_PLAYER_RACE` | `int` |
| `0x00e58365` | `_G_LLM_STRAT_UI_BLDG_TAB_SELECT_BLOCKED` | `byte` |
| `0x00e5836a` | `_G_LLM_STRAT_PLANET_TRANSITION_STATE` | `byte` |
| `0x00e583eb` | `_G_LLM_STRAT_FLOATING_MSG_SUPPRESS_FLAG` | `byte` |
| `0x00e584ef` | `_G_LLM_STRAT_LOCKSTEP_STEP_MULT` | `byte` |
| `0x00e584f0` | `_G_LLM_STRAT_OUTER_PLANET_LAND_STATE` | `int` |
| `0x00e584f4` | `_G_LLM_STRAT_PLANET_MOTHER_LOST_TIME` | `double[32]` |
| `0x00e585f4` | `_G_LLM_CHEAT_PENALTY_SCORE` | `int` |
| `0x00e585f8` | `_G_LLM_STRAT_INVASION_TIME` | `double[32]` |
| `0x00e586f8` | `_G_LLM_STRAT_OUTER_PLANET_LANDED_FLAG` | `int` |
| `0x00e586fc` | `_G_LLM_STRAT_PLANET_INT_TABLE` | `int[32]` |
| `0x00e5877c` | `_G_LLM_STRAT_BLDG_COMPLETION_SLOT_COUNT` | `byte` |
| `0x00e5877d` | `_G_LLM_STRAT_BLDG_COMPLETION_ACCUM` | `int` |
| `0x00e58789` | `_G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN` | `int` |
| `0x00e5878d` | `_G_LLM_NET_LOCKSTEP_STALL_NAG_COUNT` | `int` |
| `0x00e58791` | `_G_LLM_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT` | `int` |
| `0x00e58795` | `_G_LLM_STRAT_LOCKSTEP_STALL_COUNT` | `int` |
| `0x00e58799` | `_G_LLM_NET_LOCKSTEP_SYNC_WAIT_ELAPSED` | `double` |
| `0x00e587a1` | `_G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED` | `double` |
| `0x00e587a9` | `_G_LLM_NET_SYNC_WAIT_ACTIVE` | `int` |
| `0x00e587ad` | `_G_LLM_NET_RESYNC_IN_PROGRESS` | `int` |
| `0x00e587c9` | `_G_LLM_STRAT_SIM_STEP_INTERVAL` | `double` |
| `0x00e587e1` | `_G_LLM_STRAT_CAM_PAN_TARGET_COL` | `undefined4` |
| `0x00e587e5` | `_G_LLM_STRAT_CAM_PAN_TARGET_ROW` | `undefined4` |
| `0x00e58989` | `_G_LLM_STRAT_INJECTED_MAP_PLANET_SLOT` | `byte` |
| `0x00e5898a` | `_G_LLM_STRAT_RNG_SEED_BYTE` | `byte` |
| `0x00e5898b` | `_G_LLM_CHAT_TARGET_MASK` | `undefined1` |
| `0x00e5898c` | `_G_LLM_GAME_HUMAN_PLAYER_MASK` | `undefined1` |
| `0x00e5898d` | `_G_LLM_PLAYER_CONTROL_MASK` | `byte` |
| `0x00e5898e` | `_G_LLM_CHAT_TARGET_MODE` | `undefined` |
| `0x00e5898f` | `_G_LLM_STRAT_SHOW_UNIT_FLAGS` | `int` |
| `0x00e58993` | `_G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG` | `int` |
| `0x00e58997` | `_G_LLM_STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG` | `int` |
| `0x00e5899b` | `_G_LLM_MOUSE_BUTTONS_PREV` | `byte` |
| `0x00e5899c` | `_G_LLM_DRAG_SELECT_ANCHOR_COL` | `int` |
| `0x00e589a0` | `_G_LLM_DRAG_SELECT_ANCHOR_ROW` | `int` |
| `0x00e589a4` | `_G_LLM_CURSOR_WORLD_COL` | `int` |
| `0x00e589a8` | `_G_LLM_CURSOR_WORLD_ROW` | `int` |
| `0x00e589ac` | `_G_LLM_LMB_GESTURE_STATE` | `undefined` |
| `0x00e589ad` | `_G_LLM_RMB_GESTURE_STATE` | `undefined` |
| `0x00e589c0` | `_G_LLM_STRAT_SCENARIO_PLANET_NAME_W` | `wchar_t[16]` |
| `0x00e58bc8` | `_G_LLM_NET_LOCKSTEP_EXTEND_MARGIN_1` | `double` |
| `0x00e58bd0` | `_G_LLM_NET_LOCKSTEP_EXTEND_MARGIN_2` | `double` |
| `0x00e58bd8` | `_G_LLM_NET_LOCKSTEP_PEER_STATE` | `llm_net_lockstep_peer_state[8][8]` |
| `0x00e58c34` | `_G_LLM_NET_LOCKSTEP_PING_MS` | `uint` |
| `0x00e58c38` | `_G_LLM_NET_LOCKSTEP_RESYNC_DEADLINE_MS` | `uint` |
| `0x00e58c3c` | `_G_LLM_NET_LOCKSTEP_MAX_PING_MS` | `uint` |
| `0x00e58c40` | `_G_LLM_NET_LOCKSTEP_PEER_TIMING` | `llm_net_lockstep_peer_timing[8]` |
| `0x00e58cc0` | `_G_LLM_MAP_LOAD_FILE_HANDLE` | `undefined4` |
| `0x00e58cc8` | `_G_LLM_CURSOR_MENU_Y` | `int` |
| `0x00e58ccc` | `_G_LLM_CURSOR_MENU_X` | `int` |
| `0x00e5b8d0` | `_G_LLM_CFG_WEAPON_TABLE_STATIC_PTR` | `pointer` |
| `0x00e5b8d4` | `_G_LLM_CFG_WEAPON_TABLE_DYNAMIC_PTR` | `pointer` |
| `0x00e5b8d8` | `_G_LLM_CFG_WEAPON_POWER_ADD` | `int` |
| `0x00e5b8dc` | `_G_LLM_CFG_WEAPON_POWER_MUL` | `double` |
| `0x00e5b8e4` | `_G_LLM_CFG_UNIT_SPEED_ADD` | `double` |
| `0x00e5b8ec` | `_G_LLM_CFG_UNIT_SPEED_MUL` | `double` |
| `0x00e5b8f4` | `_G_LLM_CFG_UNIT_ENERGY_ADD` | `int` |
| `0x00e5b8f8` | `_G_LLM_CFG_UNIT_ENERGY_MUL` | `double` |
| `0x00e5b900` | `_G_LLM_CFG_UNIT_SIGHT_ADD` | `int` |
| `0x00e5b904` | `_G_LLM_CFG_UNIT_SIGHT_MUL` | `double` |
| `0x00e5b90c` | `_G_LLM_CFG_UNIT_BUILD_TIME_ADD` | `int` |
| `0x00e5b910` | `_G_LLM_CFG_UNIT_BUILD_TIME_MUL` | `double` |
| `0x00e5b918` | `_G_LLM_CFG_UNIT_RESOURCE_ADD` | `int` |
| `0x00e5b91c` | `_G_LLM_CFG_UNIT_RESOURCE_MUL` | `double` |
| `0x00e5b924` | `_G_LLM_CFG_BUILDING_ENERGY_ADD` | `int` |
| `0x00e5b928` | `_G_LLM_CFG_BUILDING_ENERGY_MUL` | `double` |
| `0x00e5b930` | `_G_LLM_CFG_BUILDING_POWER_ADD` | `int` |
| `0x00e5b934` | `_G_LLM_CFG_BUILDING_POWER_MUL` | `double` |
| `0x00e5b93c` | `_G_LLM_CFG_BUILDING_SIGHT_ADD` | `int` |
| `0x00e5b940` | `_G_LLM_CFG_BUILDING_SIGHT_MUL` | `double` |
| `0x00e5b948` | `_G_LLM_CFG_BUILDING_BUILD_TIME_ADD` | `int` |
| `0x00e5b94c` | `_G_LLM_CFG_BUILDING_BUILD_TIME_MUL` | `double` |
| `0x00e5b954` | `_G_LLM_CFG_BUILDING_HUMAN_ADD` | `int` |
| `0x00e5b958` | `_G_LLM_CFG_BUILDING_HUMAN_MUL` | `double` |
| `0x00e5b960` | `_G_LLM_CFG_BUILDING_BUILDER_ADD` | `int` |
| `0x00e5b964` | `_G_LLM_CFG_BUILDING_BUILDER_MUL` | `double` |
| `0x00e60530` | `_G_LLM_GFX_VIEWPORT_RECT` | `int[4]` |
| `0x00e60540` | `_G_LLM_PLANET_SEL_FOCUS_X` | `undefined4` |
| `0x00e60544` | `_G_LLM_PLANET_SEL_FOCUS_Y` | `undefined4` |
| `0x00e605a0` | `_G_LLM_LOBBY_SESSION_TITLE_WBUF` | `wchar_t[176]` |
| `0x00e60760` | `_G_LLM_MP_CONNECT_IP_BUF` | `undefined` |
| `0x00e61ca0` | `_G_LLM_UI_ERROR_DIALOG_ARG_TEXT_A` | `undefined` |
| `0x00e620a0` | `_G_LLM_UI_ERROR_DIALOG_ARG_TEXT_B` | `undefined` |
| `0x00e642a0` | `_G_LLM_AVI_OPEN_STATUS` | `int` |
| `0x00e642c4` | `_G_LLM_UI_OUTCOME_DLG_CODE` | `undefined1` |
| `0x00e642c5` | `_G_LLM_UI_DIPLO_TITLE_STR` | `undefined` |
| `0x00e644c3` | `_G_LLM_PLAYER_NAME_TABLE` | `undefined` |
| `0x00e654b4` | `_G_LLM_STRAT_GROUP_MOVE_DIST_REF_X` | `int` |
| `0x00e654b8` | `_G_LLM_STRAT_GROUP_MOVE_DIST_REF_Y` | `int` |
| `0x00e654bc` | `_G_LLM_STRAT_GROUP_MOVE_DIST_HALF_HEIGHT` | `int` |
| `0x00e654c0` | `_G_LLM_STRAT_GROUP_MOVE_DIST_HALF_WIDTH` | `int` |
| `0x00e654c8` | `_G_LLM_HEAP_FREE_ROVER` | `void *` |
| `0x00e654d0` | `_G_LLM_GAME_CLOCK_TICKS_PER_MS` | `double` |
| `0x00e654d8` | `_G_LLM_GAME_CLOCK_MS_PER_TICK` | `double` |
| `0x00e654e0` | `_G_LLM_GAME_CLOCK_PREV_RAW_TICK` | `uint` |
| `0x00e654e4` | `_G_LLM_GAME_CLOCK_RAW_TICKS` | `int` |
| `0x00e654e8` | `_G_LLM_TIME_TICK_QUERY_COUNT` | `int` |
| `0x00e654ec` | `_G_LLM_INPUT_TIME_EPOCH` | `uint` |
| `0x00e654f0` | `_G_LLM_GAME_CLOCK_ACCUM_TICKS` | `uint` |
| `0x00e654f4` | `_G_LLM_GAME_CLOCK_COUNTDOWN_TICKS` | `uint` |
| `0x00e654f8` | `_G_LLM_TIME_TICKS_MS` | `uint` |
| `0x00e654fc` | `_G_LLM_TIME_TICKS_MS_LAST` | `uint` |
| `0x00e65504` | `_G_LLM_GAME_CLOCK_RAW_TICK_NOW` | `uint` |
| `0x00e66510` | `_G_LLM_INPUT_MOUSE_EVENTS` | `llm_input_mouse_event[128]` |
| `0x00e68110` | `_G_LLM_INPUT_KEY_EVENTS` | `llm_input_key_event[128]` |
| `0x00e69d10` | `_G_LLM_INPUT_KEYSTATE` | `undefined` |
| `0x00e69d2d` | `_G_LLM_KEY_LCTRL_HELD` | `byte` |
| `0x00e69d3a` | `_G_LLM_KEY_LSHIFT_HELD` | `byte` |
| `0x00e69d46` | `_G_LLM_KEY_RSHIFT_HELD` | `byte` |
| `0x00e69d48` | `_G_LLM_KEY_LALT_HELD` | `byte` |
| `0x00e69d4a` | `_G_LLM_KEY_CAPSLOCK_HELD` | `undefined` |
| `0x00e69d90` | `_G_LLM_INPUT_MOUSE_WRITE_IDX` | `int` |
| `0x00e69d94` | `_G_LLM_INPUT_MOUSE_BUTTON_STATE` | `undefined` |
| `0x00e69d98` | `_G_LLM_INPUT_MOUSE_LAST_Y` | `int` |
| `0x00e69d9c` | `_G_LLM_INPUT_MOUSE_LAST_X` | `int` |
| `0x00e69da0` | `_G_LLM_INPUT_MOUSE_WHEEL_TOTAL` | `undefined` |
| `0x00e69dac` | `_G_LLM_INPUT_KEY_READ_IDX` | `int` |
| `0x00e69db0` | `_G_LLM_INPUT_MOUSE_READ_IDX` | `int` |
| `0x00e69db4` | `_G_LLM_INPUT_KEY_WRITE_IDX` | `int` |
| `0x00e69db8` | `_G_LLM_INPUT_KEY_QUEUE_READY` | `undefined` |
| `0x00e69dc0` | `_G_LLM_STRAT_AI_ATTACK_STRENGTH_TABLE` | `int[32]` |
| `0x00e69e40` | `_G_LLM_STRAT_AI_ATTACK_TIME_TABLE` | `float[32]` |
| `0x00e69ec0` | `_G_LLM_STRAT_PLANET_MAP_PAL4_WHITE` | `ushort` |
| `0x00e69ec2` | `_G_LLM_STRAT_PLANET_MAP_PAL4_BLACK` | `ushort` |
| `0x00e69ec4` | `_G_LLM_STRAT_PLANET_MAP_PAL4_MAGENTA` | `ushort` |
| `0x00e69ec6` | `_G_LLM_STRAT_PLANET_MAP_PAL4_YELLOW` | `ushort` |
| `0x00e69ed0` | `_G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST` | `int[100]` |
| `0x00fb26b0` | `_G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SOURCE_LIST` | `llm_strat_ai_engage_candidate[256]` |
| `0x00fb32b0` | `_G_LLM_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS` | `uint[128]` |
| `0x00fb32c4` | `_G_LLM_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS_R5` | `?` |
| `0x00fb32ec` | `_G_LLM_STRAT_AI_SCAN_SPIRAL_COUNT` | `?` |
| `0x00fb34b0` | `_G_LLM_STRAT_BLDG_CELL_GRID` | `byte[100][8][8]` |
| `0x00fb4db0` | `_G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH` | `llm_strat_ai_engage_candidate[256]` |
| `0x00fb59b0` | `_G_LLM_STRAT_AI_SITE_CANDIDATES` | `llm_strat_ai_site_candidate[4096]` |
| `0x00fc59b0` | `_G_LLM_STRAT_AI_TILE_SPIRAL_OFFSETS` | `llm_strat_ai_spiral_offset[65536]` |
| `0x00fe59b0` | `_G_LLM_STRAT_BLDG_CELL_GRID_ROW_SHIFT` | `int[100]` |
| `0x00fe5b48` | `_G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_COUNT` | `int` |
| `0x00fe5b4c` | `_G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SOURCE_COUNT` | `int` |
| `0x00fe5b50` | `_G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT` | `undefined4` |
| `0x00fe5b54` | `_G_LLM_STRAT_AI_SITE_CANDIDATE_COUNT` | `int` |
| `0x00fe5b58` | `_G_LLM_STRAT_AI_TILE_SPIRAL_CELL_COUNT` | `int` |
| `0x00fe5b60` | `_G_LLM_STRAT_PLANET_MAP_PAL5_BLACK` | `ushort` |
| `0x00fe5b62` | `_G_LLM_STRAT_PLANET_MAP_PAL5_MAGENTA` | `ushort` |
| `0x00fe5b64` | `_G_LLM_STRAT_PLANET_MAP_PAL5_GREEN` | `ushort` |
| `0x00fe5b66` | `_G_LLM_STRAT_PLANET_MAP_PAL5_RED` | `ushort` |
| `0x00fe5b68` | `_G_LLM_STRAT_PLANET_MAP_PAL5_BLUE` | `ushort` |
| `0x00fe93fc` | `_G_LLM_SND_SUBSYSTEM_READY` | `undefined` |
| `0x00fe9404` | `_G_LLM_STRAT_BLDG_ENCLOSURE_SCRATCH` | `byte[65536]` |
| `0x00ff9404` | `_G_LLM_STRAT_AI_SPREAD_TILES` | `llm_strat_ai_spread_tile[1024]` |
| `0x00ffac04` | `_G_LLM_STRAT_AI_SPREAD_TILE_COUNT` | `int` |
| `0x01000c08` | `_G_LLM_UNITQ_CUR_TILE` | `uint` |
| `0x01000c10` | `_G_LLM_UNITQ_CLOSED_COUNT` | `int` |
| `0x01000c14` | `_G_LLM_UNITQ_ITER` | `int` |
| `0x01000c18` | `_G_LLM_UNITQ_FRONTIER_COUNT` | `int` |
| `0x01000c1c` | `_G_LLM_UNITQ_CLOSED` | `llm_strat_unitq_search_node[1024]` |
| `0x01002c1c` | `_G_LLM_UNITQ_FRONTIER` | `llm_strat_unitq_search_node[128]` |
| `0x01013040` | `_G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT` | `int` |
| `0x01013044` | `_G_LLM_STRAT_AI_BLDG_CONNECTIVITY_TRIAL` | `byte[100]` |
| `0x010130a8` | `_G_LLM_STRAT_AI_BLDG_CONNECTIVITY_BASE` | `byte[100]` |
| `0x0101310c` | `_G_LLM_STRAT_AI_MINE_ROSTER_INDEX` | `int[32]` |
| `0x0101318c` | `_G_LLM_STRAT_AI_MINE_QUALITY_HIST` | `llm_strat_ai_mine_quality[32]` |
| `0x0101330c` | `_G_LLM_STRAT_AI_MINE_YIELD_ESTIMATE` | `llm_strat_ai_mine_yield[32]` |
| `0x0101370c` | `_G_LLM_STRAT_AI_EXPAND_SITE_COUNT` | `uint` |
| `0x01013710` | `_G_LLM_STRAT_AI_EXPAND_SITE_Y` | `byte[1024]` |
| `0x01013b10` | `_G_LLM_STRAT_AI_EXPAND_SITE_X` | `byte[1024]` |
| `0x01013f10` | `_G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST` | `int[256]` |
| `0x01014310` | `_G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_COUNT` | `int` |
| `0x01017314` | `_G_LLM_STRAT_AI_SCAN_TARGETS` | `llm_strat_ai_scan_target_entry[2048]` |
| `0x01022314` | `_G_LLM_STRAT_AI_ATTACK_CANDIDATES` | `llm_strat_ai_attack_candidate[128]` |
| `0x01022b18` | `_G_LLM_STRAT_AI_TARGET_SCRATCH_COUNT` | `uint` |
| `0x01022b1c` | `_G_LLM_STRAT_AI_ATTACK_CANDIDATE_COUNT` | `int` |
| `0x01022b20` | `_G_LLM_STRAT_AI_SCAN_TARGET_COUNT` | `int` |
| `0x01063b24` | `_G_LLM_CRT_WINMAIN_PTR` | `void *` |
| `0x01063b2c` | `_G_LLM_CRT_FD_HANDLE_TABLE` | `HANDLE *` |
<!-- END generated-symbols -->
