// Affinity NDS runtime — boot + main loop.
// Per-module work lives in audio.c, fps3d.c, mode0.c, hud.c, script_glue.c.

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/videoGL.h>
#include <stdio.h>

static void init_video(void)
{
    powerOn(POWER_ALL_2D);

    // Top screen: Mode 0 with 3D on BG0
    videoSetMode(MODE_0_3D);

    // Bottom screen: console for debug output (Phase 5 will replace)
    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleDemoInit();

    vramSetBankA(VRAM_A_TEXTURE);
    vramSetBankB(VRAM_B_MAIN_BG);
    vramSetBankE(VRAM_E_TEX_PALETTE);

    oamInit(&oamMain, SpriteMapping_1D_32, false);
    vramSetBankF(VRAM_F_MAIN_SPRITE);

    glInit();
    glEnable(GL_ANTIALIAS | GL_TEXTURE_2D);
    glClearColor(0, 0, 0, 31);  // black backdrop
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glViewport(0, 0, 255, 191);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Far plane bumped from 100 → 1024. Our coords are 16.8 fixed scaled
    // through fx8_to_f32 (shift-left-4), so a 256-unit-wide scene maps to
    // ~256 in f32 world units after the matrix — easy to put the far plane
    // inside the scene with the default value of 100.
    gluPerspective(70, 256.0 / 192.0, 0.1, 1024);

    // Warm up the geometry engine: it needs the projection matrix to settle
    // before the first user draw, otherwise the first frame renders with a
    // stale (often identity) projection — the cube appears as a single dot
    // until the next D-pad input forces a fresh frame.
    glFlush(0);
    swiWaitForVBlank();
}

int main(void)
{
    init_video();

    afn_audio_init();
    afn_fps3d_init();
    afn_hud_init();
    afn_script_init();
    // afn_mode0_init() called by scene loader once Phase 4 lands.

    irqSet(IRQ_HBLANK, m7_hbl);
    irqEnable(IRQ_HBLANK);

    iprintf("Affinity NDS\n");

    // Pre-roll the GPU pipeline: submit-display latency means the first user
    // frame appears with stale state (cube collapses to a 1-pixel dot until
    // input forces a re-render). A few warmup frames push valid geometry +
    // matrices through before the player sees anything.
    for (int w = 0; w < 4; w++) {
        afn_fps3d_update();
        swiWaitForVBlank();
    }

    while (pmMainLoop())
    {
        afn_script_tick();
        afn_scene_tick();      // fade state machine
        afn_fps3d_update();
        afn_hud_draw();
        afn_audio_tick();
        swiWaitForVBlank();
    }

    return 0;
}
