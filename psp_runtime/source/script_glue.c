// Affinity PSP runtime — script glue. Defines the runtime variables the
// generated node code (psp_script.h) reads/writes, and runs the OnStart /
// OnUpdate / OnKey hooks each frame. The controller (scene.c) consumes the
// node-set variables (afn_input_fwd/right, afn_move_speed, orbit_angle,
// afn_rig_clip) so all behaviour is node-driven, per the engine convention.
#include "script.h"
#include "input.h"
#include "psp_script.h"

#ifdef AFN_HAS_SCRIPT

// Node variables the emitted code reads/writes. Core ones are consumed by
// the controller / rig; the rest are present-but-inert until their consumers
// are ported (sound, HUD, grind, sprite manipulation, ...).
int afn_player_frozen = 0;
int afn_move_speed = 0;
int afn_speed_prio = 0;
int afn_rig_clip = 0;
int orbit_angle = 0;
int afn_current_mode = 4;     // Mode 4 (3D)
int afn_current_scene = 0;
int afn_collided_sprite = -1;
int afn_collided_tm_obj = -1;
int afn_bp_cur_spr_idx = -1;
int afn_bp_cur_tm_obj = -1;

void afn_play_sound(int smp) { (void)smp; }   // TODO: audio

void script_start(void) {
    afn_emitted_script_start();
    afn_bp_dispatch_start();
}

void script_tick(void) {
    // Node-driven movement inputs are recomputed from scratch each frame.
    afn_input_fwd = 0; afn_input_right = 0;
    afn_speed_prio = 0; afn_move_speed = 0;

    afn_emitted_script_update();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_emitted_script_key_released();
    afn_bp_dispatch_update();
    afn_bp_dispatch_key_held();
    afn_bp_dispatch_key_pressed();
    afn_bp_dispatch_key_released();
}

int script_present(void) { return 1; }

#else  // no script in this build

int afn_rig_clip = 0;   // still consumed by the rig
void script_start(void) {}
void script_tick(void)  {}
int  script_present(void) { return 0; }

#endif
