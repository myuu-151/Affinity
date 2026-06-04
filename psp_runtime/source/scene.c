// Affinity PSP runtime — Mode 4 scene (camera, input, mesh + rig draw).
#include "scene.h"
#include "affinity_psp.h"
#include "meshcull.h"
#include "rig.h"
#include "collision.h"
#include "sky.h"
#include "billboard.h"
#include "input.h"

#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <math.h>

#define DEG2RAD (3.14159265f / 180.0f)
#define RAD2DEG (180.0f / 3.14159265f)

// ---- State ----------------------------------------------------------------
// When a player rig exists we run a follow-cam: the camera orbits the player,
// the analog stick moves the player in camera-relative space, and the rig faces
// its movement. Without a rig we fall back to a free-fly debug camera.
static int   s_follow;                 // 1 = follow player rig, 0 = free fly
static float camX, camY, camZ;         // camera eye (world)
static float camAngle;                 // camera yaw (radians); forward=(sin,0,cos)
static float playerX, playerY, playerZ;
static float playerYaw;                // degrees, facing of the rig
static float playerVY;                 // vertical velocity (gravity/jump)
static int   grounded;
static float s_orbit;                  // orbit distance
static float s_floorN[3] = {0.0f, 1.0f, 0.0f};  // smoothed floor normal (slope tilt)

#define GRAVITY      0.8f
#define JUMP_VEL     13.0f
#define TERMINAL_VY  30.0f

void scene_init(void) {
    meshcull_build();
    collide_build();
    rig_init();
    // GE reads physical RAM; flush the CPU dcache so baked const data (textures,
    // bucket indices) is visible — otherwise textures render black.
    sceKernelDcacheWritebackAll();

    camAngle = afn_cam_start_angle;
    s_orbit  = afn_orbit_dist > 1.0f ? afn_orbit_dist : 200.0f;
    s_follow = rig_present();
    if (s_follow) {
        float st[3]; rig_player_start(st);
        playerX = st[0]; playerY = st[1]; playerZ = st[2];
        playerYaw = afn_cam_start_angle * RAD2DEG;
        // Drop onto the floor at spawn so we don't start mid-air.
        float fy;
        if (collide_floor(playerX, playerZ, playerY + 200.0f, &fy, s_floorN)) playerY = fy;
        playerVY = 0.0f; grounded = 1;
    } else {
        camX = afn_cam_start_x; camY = afn_cam_start_h; camZ = afn_cam_start_z;
    }
}

void scene_update(void) {
    input_update();

    float ax = afn_input_right / 128.0f;
    float ay = -afn_input_fwd  / 128.0f;
    if (ax < 0.15f && ax > -0.15f) ax = 0.0f;
    if (ay < 0.15f && ay > -0.15f) ay = 0.0f;

    float fwdX = sinf(camAngle), fwdZ = cosf(camAngle);
    float rgtX = cosf(camAngle), rgtZ = -sinf(camAngle);

    if (s_follow) {
        // L/R triggers orbit the camera around the player.
        if (key_is_down(KEY_L)) camAngle -= 0.04f;
        if (key_is_down(KEY_R)) camAngle += 0.04f;
        // Analog moves the player in camera-relative space; face the movement.
        float mvX = -ay * fwdX + ax * rgtX;
        float mvZ = -ay * fwdZ + ax * rgtZ;
        float mag = mvX*mvX + mvZ*mvZ;
        int moving = (mag > 0.0001f);
        rig_set_moving(moving);
        if (moving) {
            float speed = afn_walk_speed > 0.0f ? afn_walk_speed * 0.25f : 6.0f;
            playerX += mvX * speed;
            playerZ += mvZ * speed;
            playerYaw = atan2f(mvX, mvZ) * RAD2DEG;
        }
        // Jump (Cross = KEY_A).
        if (grounded && key_is_down(KEY_A)) { playerVY = JUMP_VEL; grounded = 0; }
        // Wall pushback, then gravity + floor snap.
        collide_walls(&playerX, &playerZ, playerY);
        playerVY -= GRAVITY;
        if (playerVY < -TERMINAL_VY) playerVY = -TERMINAL_VY;
        playerY += playerVY;
        float fy, fn[3];
        if (collide_floor(playerX, playerZ, playerY, &fy, fn) && playerY <= fy) {
            playerY = fy; playerVY = 0.0f; grounded = 1;
            // Smooth the floor normal so the slope tilt eases (no popping).
            s_floorN[0] += (fn[0]-s_floorN[0])*0.2f;
            s_floorN[1] += (fn[1]-s_floorN[1])*0.2f;
            s_floorN[2] += (fn[2]-s_floorN[2])*0.2f;
        } else {
            grounded = 0;
            // Airborne: ease back upright.
            s_floorN[0] += (0.0f-s_floorN[0])*0.1f;
            s_floorN[1] += (1.0f-s_floorN[1])*0.1f;
            s_floorN[2] += (0.0f-s_floorN[2])*0.1f;
        }
    } else {
        // Free-fly debug camera.
        if (key_is_down(KEY_L)) camAngle -= 0.04f;
        if (key_is_down(KEY_R)) camAngle += 0.04f;
        if (key_is_down(KEY_A)) camY += 4.0f;
        if (key_is_down(KEY_B)) camY -= 4.0f;
        float speed = afn_walk_speed > 0.0f ? afn_walk_speed * 0.25f : 6.0f;
        camX += (-ay * fwdX + ax * rgtX) * speed;
        camZ += (-ay * fwdZ + ax * rgtZ) * speed;
    }
}

void scene_render(void) {
    // Sky panorama first (behind everything), scrolled by camera yaw.
    sky_render(camAngle);

    // Follow-cam: place the eye behind/above the player, looking at them.
    if (s_follow) {
        camX = playerX - sinf(camAngle) * s_orbit;
        camZ = playerZ - cosf(camAngle) * s_orbit;
        camY = playerY + s_orbit * 0.45f;
    }

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(75.0f, 480.0f / 272.0f, 1.0f, 10000.0f);

    ScePspFVector3 eye = { camX, camY, camZ };
    ScePspFVector3 center = s_follow
        ? (ScePspFVector3){ playerX, playerY + s_orbit * 0.12f, playerZ }
        : (ScePspFVector3){ camX + sinf(camAngle), camY, camZ + cosf(camAngle) };
    ScePspFVector3 up = { 0.0f, 1.0f, 0.0f };
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumLookAt(&eye, &center, &up);

    float camSin = sinf(camAngle), camCos = cosf(camAngle);
    float drawDist = afn_draw_distance;

    sceGumMatrixMode(GU_MODEL);

    for (int si = 0; si < afn_sprite_count; si++) {
        int mi = afn_sprites[si].meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        const AfnMesh* m = &afn_meshes[mi];
        if (!m->visible || m->vertCount <= 0) continue;

        const AfnSpriteInst* sp = &afn_sprites[si];

        sceGumLoadIdentity();
        ScePspFVector3 t = { sp->x, sp->y, sp->z };
        sceGumTranslate(&t);
        if (sp->rotZ != 0.0f) sceGumRotateZ(sp->rotZ * DEG2RAD);
        if (sp->rotX != 0.0f) sceGumRotateX(sp->rotX * DEG2RAD);
        if (sp->rotY != 0.0f) sceGumRotateY(sp->rotY * DEG2RAD);
        ScePspFVector3 s = { sp->scale, sp->scale, sp->scale };
        sceGumScale(&s);

        if (m->cullMode == 2) {
            sceGuDisable(GU_CULL_FACE);
        } else {
            sceGuEnable(GU_CULL_FACE);
            sceGuFrontFace(m->cullMode == 1 ? GU_CW : GU_CCW);
        }

        if (m->textured && m->texPixels) {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, m->texW, m->texH, m->texW, m->texPixels);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);   // atlas: no bilinear (seams)
            sceGuTexLevelMode(GU_TEXTURE_CONST, 0.0f);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
        if (m->texHasAlpha) {
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        } else {
            sceGuDisable(GU_BLEND);
        }

        meshcull_draw(mi, sp->x, sp->y, sp->z, sp->scale,
                      sp->rotY, sp->rotX, sp->rotZ,
                      camX, camY, camZ, camSin, camCos, drawDist);
    }

    // Player rig (skinned) — draws at the player's world transform, tilted to
    // the floor slope.
    if (s_follow)
        rig_render(playerX, playerY, playerZ, playerYaw, s_floorN, 0);

    // Sprite billboards (enemies/pickups), camera-facing + animated.
    billboards_render(camX, camY, camZ, camAngle);

    sceGuEnable(GU_CULL_FACE);
    sceGuFrontFace(GU_CW);
    sceGuEnable(GU_TEXTURE_2D);
}
