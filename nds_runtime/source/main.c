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
    glClearColor(8, 12, 20, 31);
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glViewport(0, 0, 255, 191);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(70, 256.0 / 192.0, 0.1, 100);

    m7_bg = bgInit(2, BgType_Rotation, BgSize_R_256x256, 4, 0);
    bgSetPriority(m7_bg, 3);
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
