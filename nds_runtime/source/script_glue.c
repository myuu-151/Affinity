// Affinity NDS — visual scripting runtime glue (Phase 3).
//
// Bridges the per-frame dispatchers main.c calls (afn_script_init / tick)
// to the actual emitted script bodies in mapdata.h (afn_emitted_script_*).
// Also owns the storage for every script-side global declared as `extern`
// by mapdata.h. mapdata.h is included by multiple .c files on NDS (main.c,
// fps3d.c, sprites.c, hud.c, ...), so each global gets a single definition
// here instead of static in mapdata.h (the GBA pattern doesn't translate).

#include "affinity.h"
#include "mapdata.h"

// ---------------------------------------------------------------------------
// Script-side globals
//
// Defined only when scripts actually exist in the build. AFN_HAS_SCRIPT is
// gated on the editor having at least one script node or blueprint, so most
// of these stay out of the link for script-less projects.
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_SCRIPT
int  afn_input_fwd;
int  afn_input_right;
int  afn_move_speed;
int  afn_auto_orbit_speed;
int  afn_play_anim;
int  afn_sprite_anim_spr = -1;
int  afn_sprite_anim_val = -1;
int  afn_anim_prio;
int  afn_collided_sprite = -1;
int  afn_collided_tm_obj = -1;
int  afn_bp_cur_tm_obj   = -1;
int  afn_bp_cur_spr_idx  = -1;
int  afn_gravity;
int  afn_terminal_vel;
int  afn_player_frozen;
int  afn_anim_speed = 1;
unsigned int afn_rng = 12345;
int  afn_shake_intensity;
int  afn_shake_frames;
int  afn_fade_level;
int  afn_score;
int  afn_frame_count;
int  afn_draw_distance;
#endif

void afn_script_init(void)
{
    afn_emitted_script_init();
}

void afn_script_tick(void)
{
    afn_emitted_script_update();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_emitted_script_key_released();
#ifdef AFN_HAS_SCRIPT
    afn_frame_count++;
#endif
}
