# NDS Symbol Audit (Phase 0)

Distinct C identifiers referenced by `setActionFunc` bodies in
`src/editor/frame_loop.cpp`. Each one must resolve at NDS link time
before Phase 3 (visual scripting) is "done."

**Total: 160 identifiers.**

Sources of resolution:
- Most are provided by the runtime (`*.c` files in `nds_runtime/source/`):
  game state flags, animation triggers, dialogue state, HUD state, scripting
  hooks like `afn_script_start` / `afn_script_update` / `afn_script_collision`,
  audio functions like `afn_play_sound` / `afn_stop_sound`.
- Some come from `mapdata.h` (data tables): `afn_anim_desc`, `afn_asset_desc`,
  `afn_pal`, blueprint dispatch externs (`afn_bp_*`).

How to use this list during Phase 3:
1. After porting each module, grep for unresolved externs in the link error.
2. Cross-reference here — if it's listed, the script bodies expect it.
3. Either declare it in the appropriate `*.c` (state flag), define it
   (function), or extend the exporter to emit it (data table).

---

## Full list

- `afn_active_element`
- `afn_ai_mode`
- `afn_anim_desc`
- `afn_anim_speed`
- `afn_apply_stop_modifiers`
- `afn_asset_desc`
- `afn_auto_orbit_speed`
- `afn_bar_color`
- `afn_bar_max`
- `afn_bg_color`
- `afn_bp`
- `afn_bp_cur_tm_obj`
- `afn_bp_def_frozen`
- `afn_cam_locked`
- `afn_cam_speed`
- `afn_cd_`
- `afn_checkpoint_set`
- `afn_checkpoint_x`
- `afn_checkpoint_z`
- `afn_clear_text`
- `afn_clone_sprite`
- `afn_collided_tm_obj`
- `afn_collision_enabled`
- `afn_collision_ignore`
- `afn_collision_size`
- `afn_current_mode`
- `afn_current_scene`
- `afn_cursor_stop`
- `afn_delay_`
- `afn_dlg_choice_a`
- `afn_dlg_choice_b`
- `afn_dlg_choosing`
- `afn_dlg_line`
- `afn_dlg_open`
- `afn_dlg_speaker`
- `afn_dlg_text`
- `afn_draw_bar`
- `afn_draw_distance`
- `afn_draw_number`
- `afn_draw_sprite_icon`
- `afn_draw_text`
- `afn_elem_idx`
- `afn_emit_particle`
- `afn_fade_counter`
- `afn_fade_frames`
- `afn_fade_target`
- `afn_flags`
- `afn_flash_obj`
- `afn_force_x`
- `afn_force_z`
- `afn_frame_count`
- `afn_friction`
- `afn_gravity`
- `afn_hp`
- `afn_hud_layer_active`
- `afn_hud_layer_frame`
- `afn_hud_layer_speed`
- `afn_hud_layer_tick`
- `afn_hud_value`
- `afn_hud_visible`
- `afn_input_`
- `afn_input_fwd`
- `afn_input_right`
- `afn_inventory`
- `afn_last_key`
- `afn_lifetime`
- `afn_max_hp`
- `afn_move_speed`
- `afn_on_death_func`
- `afn_on_hit_func`
- `afn_pal`
- `afn_pending_scene`
- `afn_pending_scene_mode`
- `afn_play_anim`
- `afn_play_sound`
- `afn_player_frozen`
- `afn_player_height`
- `afn_prev_state`
- `afn_rng`
- `afn_score`
- `afn_script`
- `afn_script_any_key`
- `afn_script_collision`
- `afn_script_collision2d`
- `afn_script_key_held`
- `afn_script_key_pressed`
- `afn_script_key_released`
- `afn_script_start`
- `afn_script_timer`
- `afn_script_trigger_enter`
- `afn_script_trigger_exit`
- `afn_script_update`
- `afn_scripts_stopped`
- `afn_shake_frames`
- `afn_shake_intensity`
- `afn_snd_inst`
- `afn_sound_mix`
- `afn_sound_tick`
- `afn_spawn_effect`
- `afn_spawn_projectile`
- `afn_spawn_sprite`
- `afn_sprite_alpha`
- `afn_sprite_flip`
- `afn_sprite_layer`
- `afn_sprite_rot`
- `afn_sprite_shake`
- `afn_sprite_tint`
- `afn_sprite_visible`
- `afn_sram_load`
- `afn_sram_save`
- `afn_start_x`
- `afn_start_y`
- `afn_start_z`
- `afn_state`
- `afn_state_timer`
- `afn_stop_count`
- `afn_stop_links`
- `afn_stop_sound`
- `afn_terminal_vel`
- `afn_text_color`
- `afn_timer_`
- `afn_timer_visible`
- `afn_trans_`
- `afn_trigger_sample`
- `afn_vars`
- `cam_fov`
- `cam_h`
- `cam_pitch`
- `cam_x`
- `cam_y`
- `m7_horizon`
- `player_facing`
- `player_moving`
- `player_on_ground`
- `player_vy`
- `player_x`
- `player_y`
- `player_z`
- `snd_seq_active`
- `snd_seq_next`
- `snd_seq_tick`
- `tm_anim_frame`
- `tm_anim_idx`
- `tm_anim_timer`
- `tm_fol_active`
- `tm_fol_dist`
- `tm_fol_facing`
- `tm_fol_moving`
- `tm_fol_obj`
- `tm_fol_prev_ptx`
- `tm_fol_prev_pty`
- `tm_fol_speed`
- `tm_fol_trail_count`
- `tm_fol_trail_head`
- `tm_move_frames`
- `tm_move_timer`
- `tm_obj_facing`
- `tm_player_facing`
- `tm_player_tx`
- `tm_player_ty`
