// Affinity PS Vita runtime — Mode 4 scene bring-up.
// Renders the exported level meshes with vitaGL. Ported from psp_runtime's
// scene.c, but simplified for the Vita's headroom: no per-frame frustum
// culling / bucketing and no texture swizzling (just upload GL textures and
// draw each mesh's full index buffer). Free-orbit camera so the level can be
// inspected on-device. Rig / sprites / sky / audio / collision come next.
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <math.h>
#include <string.h>

#include "psv_mapdata.h"   // defines afn_meshes / afn_sprites / camera start
#include "psv_rig.h"       // player rig data (skinned glTF), if AFN_HAS_PLAYER_RIG

#define SCR_W 960.0f
#define SCR_H 544.0f
#define DEG2RAD (3.14159265f / 180.0f)

static GLuint s_meshTex[256];   // one GL texture per mesh (0 = none)

// ---------------------------------------------------------------------------
// Rigs: CPU rigid skinning (ported from psp_runtime/rig.c) + vitaGL draw.
// Multi-rig: the scene can carry several distinct models (player + each NPC /
// enemy type). afn_rigs[] holds one AfnRig descriptor per used model; each
// instance (the player, and afn_npc_inst[]) names a rig slot, position, facing,
// scale and clip, and is CPU-skinned into a shared buffer then drawn.
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_PLAYER_RIG
#ifndef AFN_RIG_YAW_OFFSET
#define AFN_RIG_YAW_OFFSET 0.0f
#endif
int afn_rig_clip = AFN_PLAYER_DEFAULT_CLIP;   // player clip selector (script-set; local for bring-up)
static AfnRigVertex s_skinned[AFN_RIG_MAX_VERTS];
static float  s_bonemat[AFN_RIG_MAX_BONES][12];   // 3x4 row-major per bone
static GLuint s_rigTex[AFN_RIG_COUNT][AFN_RIG_MAX_MATS];
static float  s_pframe = 0.0f;                     // player anim frame
static int    s_pclip  = AFN_PLAYER_DEFAULT_CLIP;
static float  s_npcFrame[AFN_NPC_COUNT + 1];       // per-NPC anim frame (+1 avoids zero-size)

static void pose_to_mat(const float* p, float* m) {
    float px=p[0],py=p[1],pz=p[2], w=p[3],x=p[4],y=p[5],z=p[6];
    float n = w*w+x*x+y*y+z*z;
    if (n > 1e-8f) { n = 1.0f/sqrtf(n); w*=n; x*=n; y*=n; z*=n; }
    float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    m[0]=1-2*(yy+zz); m[1]=2*(xy-wz);   m[2]=2*(xz+wy);   m[3]=px;
    m[4]=2*(xy+wz);   m[5]=1-2*(xx+zz); m[6]=2*(yz-wx);   m[7]=py;
    m[8]=2*(xz-wy);   m[9]=2*(yz+wx);   m[10]=1-2*(xx+yy);m[11]=pz;
}
static void build_bone_mats(const AfnRig* R, int clip, float frame) {
    const float* cd = R->clip[clip];
    int nf = R->clipframes[clip]; if (nf < 1) nf = 1;
    int f0 = (int)frame; if (f0 < 0) f0 = 0; if (f0 >= nf) f0 = nf - 1;
    int f1 = f0 + 1; if (f1 >= nf) f1 = R->cliploop[clip] ? 0 : nf - 1;
    float t = frame - (float)((int)frame);
    for (int b = 0; b < R->bones; b++) {
        const float* p0 = &cd[(f0 * R->bones + b) * 7];
        const float* p1 = &cd[(f1 * R->bones + b) * 7];
        float p[7];
        p[0]=p0[0]+(p1[0]-p0[0])*t; p[1]=p0[1]+(p1[1]-p0[1])*t; p[2]=p0[2]+(p1[2]-p0[2])*t;
        float d = p0[3]*p1[3]+p0[4]*p1[4]+p0[5]*p1[5]+p0[6]*p1[6];
        float s = (d < 0.0f) ? -1.0f : 1.0f;
        p[3]=p0[3]+(p1[3]*s-p0[3])*t; p[4]=p0[4]+(p1[4]*s-p0[4])*t;
        p[5]=p0[5]+(p1[5]*s-p0[5])*t; p[6]=p0[6]+(p1[6]*s-p0[6])*t;
        pose_to_mat(p, s_bonemat[b]);
    }
}
// Skin rig R into s_skinned (positions + normals + uv + white vertex color).
static void skin(const AfnRig* R) {
    for (int v = 0; v < R->verts; v++) {
        const float* m = s_bonemat[R->vbone[v]];
        float x=R->vpos[v*3+0], y=R->vpos[v*3+1], z=R->vpos[v*3+2];
        s_skinned[v].x = m[0]*x+m[1]*y+m[2]*z+m[3];
        s_skinned[v].y = m[4]*x+m[5]*y+m[6]*z+m[7];
        s_skinned[v].z = m[8]*x+m[9]*y+m[10]*z+m[11];
        float nx=R->vnorm[v*3+0], ny=R->vnorm[v*3+1], nz=R->vnorm[v*3+2];
        float wx=m[0]*nx+m[1]*ny+m[2]*nz, wy=m[4]*nx+m[5]*ny+m[6]*nz, wz=m[8]*nx+m[9]*ny+m[10]*nz;
        float nl=wx*wx+wy*wy+wz*wz; if (nl>1e-12f){nl=1.0f/sqrtf(nl);wx*=nl;wy*=nl;wz*=nl;}
        s_skinned[v].nx=wx; s_skinned[v].ny=wy; s_skinned[v].nz=wz;
        s_skinned[v].u = R->vuv[v*2+0]; s_skinned[v].v = R->vuv[v*2+1];
        s_skinned[v].color = 0xFFFFFFFF;
    }
}
static void rig_init(void) {
    for (int r = 0; r < AFN_RIG_COUNT; r++) {
        const AfnRig* R = &afn_rigs[r];
        for (int g = 0; g < AFN_RIG_MAX_MATS; g++) {
            s_rigTex[r][g] = 0;
            if (g < R->mats && R->tex[g] && R->texw[g] > 0 && R->texh[g] > 0) {
                glGenTextures(1, &s_rigTex[r][g]);
                glBindTexture(GL_TEXTURE_2D, s_rigTex[r][g]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, R->texw[g], R->texh[g], 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, R->tex[g]);
            }
        }
    }
}
// Advance an animation frame for rig R's clip.
static float rig_advance(const AfnRig* R, int clip, float frame) {
    frame += 0.4f;
    int nf = R->clipframes[clip];
    if (nf > 1) {
        if (R->cliploop[clip]) { while (frame >= (float)nf) frame -= (float)nf; }
        else if (frame > (float)(nf-1)) frame = (float)(nf-1);
    } else frame = 0.0f;
    return frame;
}
// Draw one rig instance. s_skinned must already hold R's skinned pose. The
// headlamp is the same NDS-matched setup as before, per rig's baked direction.
static void rig_draw(const AfnRig* R, GLuint* texArr, const float* view,
                     float px, float py, float pz, float yawDeg, float instScale) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    if (R->camlight) {
        GLfloat ldir[4]   = { -R->ldx, -R->ldy, -R->ldz, 0.0f };  // GL_POSITION = toward light (negated DS dir)
        GLfloat white[4]  = { 1, 1, 1, 1 };
        GLfloat matAmb[4] = { 8.0f/31.0f,  8.0f/31.0f,  8.0f/31.0f,  1 };
        GLfloat matDif[4] = { 28.0f/31.0f, 28.0f/31.0f, 28.0f/31.0f, 1 };
        GLfloat noAmb[4]  = { 0, 0, 0, 1 };
        glLightfv(GL_LIGHT0, GL_POSITION, ldir);
        glLightfv(GL_LIGHT0, GL_DIFFUSE,  white);
        glLightfv(GL_LIGHT0, GL_AMBIENT,  white);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, noAmb);
        glDisable(GL_COLOR_MATERIAL);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, matAmb);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, matDif);
        glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_NORMALIZE);
    } else {
        glDisable(GL_LIGHTING);
    }

    float yr = yawDeg * DEG2RAD + AFN_RIG_YAW_OFFSET;
    float fx = sinf(yr), fz = cosf(yr);
    float S = R->scale * instScale;
    float model[16] = {
        fz*S, 0, -fx*S, 0,
        0,    S, 0,    0,
        fx*S, 0, fz*S, 0,
        px,   py, pz,  1
    };
    glLoadMatrixf(view);
    glMultMatrixf(model);

    glDisable(GL_BLEND);
    if (R->cull == 2) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(R->cull == 1 ? GL_CW : GL_CCW); }

    AfnRigVertex* v = s_skinned;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnRigVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnRigVertex), &v->color);
    glNormalPointer (   GL_FLOAT,         sizeof(AfnRigVertex), &v->nx);
    glVertexPointer (3, GL_FLOAT,         sizeof(AfnRigVertex), &v->x);
    for (int g = 0; g < R->mats; g++) {
        int ic = R->idxcount[g];
        if (ic <= 0) continue;
        if (texArr[g]) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texArr[g]); }
        else glDisable(GL_TEXTURE_2D);
        glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, R->idx[g]);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    if (R->camlight) { glDisable(GL_LIGHTING); glDisable(GL_LIGHT0); glDisable(GL_NORMALIZE); }
}

// Skin + draw the player and every NPC for this frame.
static void rigs_render(const float* view, float playerX, float playerY, float playerZ) {
    const AfnRig* PR = &afn_rigs[AFN_PLAYER_RIG_SLOT];
    if (afn_rig_clip >= 0 && afn_rig_clip < PR->clips && afn_rig_clip != s_pclip) {
        s_pclip = afn_rig_clip; s_pframe = 0.0f;
    }
    s_pframe = rig_advance(PR, s_pclip, s_pframe);
    build_bone_mats(PR, s_pclip, s_pframe); skin(PR);
    rig_draw(PR, s_rigTex[AFN_PLAYER_RIG_SLOT], view, playerX, playerY, playerZ, 0.0f, AFN_PLAYER_SCALE);

    for (int i = 0; i < AFN_NPC_COUNT; i++) {
        const float* N = afn_npc_inst[i];
        int slot = (int)N[6]; if (slot < 0 || slot >= AFN_RIG_COUNT) continue;
        const AfnRig* R = &afn_rigs[slot];
        int clip = (int)N[5]; if (clip < 0 || clip >= R->clips) clip = 0;
        s_npcFrame[i] = rig_advance(R, clip, s_npcFrame[i]);
        build_bone_mats(R, clip, s_npcFrame[i]); skin(R);
        rig_draw(R, s_rigTex[slot], view, N[0], N[1], N[2], N[3], N[4]);
    }
}
#endif // AFN_HAS_PLAYER_RIG

// Build a column-major 4x4 view matrix (gluLookAt equivalent — vitaGL has no GLU).
static void look_at(float m[16],
                    float ex, float ey, float ez,
                    float cx, float cy, float cz,
                    float ux, float uy, float uz)
{
    float fx = cx-ex, fy = cy-ey, fz = cz-ez;
    float fl = sqrtf(fx*fx+fy*fy+fz*fz); if (fl < 1e-6f) fl = 1.0f;
    fx/=fl; fy/=fl; fz/=fl;
    // s = f x up
    float sx = fy*uz - fz*uy, sy = fz*ux - fx*uz, sz = fx*uy - fy*ux;
    float sl = sqrtf(sx*sx+sy*sy+sz*sz); if (sl < 1e-6f) sl = 1.0f;
    sx/=sl; sy/=sl; sz/=sl;
    // u = s x f
    float ux2 = sy*fz - sz*fy, uy2 = sz*fx - sx*fz, uz2 = sx*fy - sy*fx;
    m[0]=sx;  m[4]=sy;  m[8]=sz;   m[12] = -(sx*ex + sy*ey + sz*ez);
    m[1]=ux2; m[5]=uy2; m[9]=uz2;  m[13] = -(ux2*ex + uy2*ey + uz2*ez);
    m[2]=-fx; m[6]=-fy; m[10]=-fz; m[14] =  (fx*ex + fy*ey + fz*ez);
    m[3]=0;   m[7]=0;   m[11]=0;   m[15] = 1.0f;
}

static void upload_textures(void)
{
    for (int mi = 0; mi < afn_mesh_count && mi < 256; mi++) {
        s_meshTex[mi] = 0;
        const AfnMesh* m = &afn_meshes[mi];
        if (m->textured && m->texPixels && m->texW > 0 && m->texH > 0) {
            glGenTextures(1, &s_meshTex[mi]);
            glBindTexture(GL_TEXTURE_2D, s_meshTex[mi]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // texPixels are 0xAABBGGRR == RGBA byte order in memory.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m->texW, m->texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, m->texPixels);
        }
    }
}

static void draw_mesh(int mi)
{
    const AfnMesh* m = &afn_meshes[mi];
    if (!m->visible || m->vertCount <= 0 || !m->verts || !m->indices) return;

    if (m->cullMode == 2) { glDisable(GL_CULL_FACE); }
    else { glEnable(GL_CULL_FACE); glFrontFace(m->cullMode == 1 ? GL_CW : GL_CCW); }

    if (m->textured && s_meshTex[mi]) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, s_meshTex[mi]);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    if (m->texHasAlpha) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);

    const AfnVertex* v = m->verts;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,        sizeof(AfnVertex), &v->x);
    glDrawElements(GL_TRIANGLES, m->indexCount, GL_UNSIGNED_SHORT, m->indices);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

int main(void)
{
    // Max out the Vita clocks. 444 MHz is the hardware/API ceiling for the
    // Cortex-A9 cores (the default app clock is 333 MHz); scePower clamps
    // anything higher, so a true 500 MHz needs a kernel overclock plugin and is
    // not done here. Bus/GPU are pushed to their maxima too for headroom.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    vglInit(0x800000);
    vglWaitVblankStart(GL_TRUE);

    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    upload_textures();
#ifdef AFN_HAS_PLAYER_RIG
    rig_init();
#endif

    // Player position (static until movement is ported; the follow camera and
    // the NPCs read this).
#ifdef AFN_HAS_PLAYER_RIG
    float playerX = AFN_PLAYER_START_X, playerY = AFN_PLAYER_START_Y, playerZ = AFN_PLAYER_START_Z;
#else
    float playerX = afn_cam_start_x, playerY = afn_cam_start_h, playerZ = afn_cam_start_z;
#endif

    // Orbit-follow camera, anchored to the active camera slot
    // (afn_cam_slots[afn_active_camera] = { yaw, dist, height, horizon }). The
    // player orbits freely with the right stick / L-R; triggers zoom. The slot's
    // height sets the default downward tilt and the look-at target height. PSV
    // has no scripts yet so afn_active_camera stays 0 (scene default), but the
    // runtime re-reads it each frame so a future SetCamera node just works.
    const float* slot0 = afn_cam_slots[afn_active_camera];
    float orbit_angle = slot0[0];                            // radians
    float camDist     = slot0[1] > 1.0f ? slot0[1] : 60.0f;  // world px
    float camHeight   = slot0[2];                            // world px
    float pitch       = atan2f(camHeight > 0.0f ? camHeight : 8.0f, camDist);  // downward tilt from slot

    SceCtrlData pad;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        // Re-read the active slot each frame (a future SetCamera node retargets it).
        const float* S = afn_cam_slots[afn_active_camera];
        camDist   = S[1] > 1.0f ? S[1] : camDist;   // keep manual zoom unless slot overrides
        camHeight = S[2];

        // Right stick / L-R: orbit. Triggers: zoom.
        float rx = (pad.rx - 128) / 128.0f, ry = (pad.ry - 128) / 128.0f;
        if (fabsf(rx) > 0.15f) orbit_angle += rx * 0.04f;
        if (fabsf(ry) > 0.15f) pitch       += ry * 0.03f;
        if (pad.buttons & SCE_CTRL_LEFT)  orbit_angle -= 0.04f;
        if (pad.buttons & SCE_CTRL_RIGHT) orbit_angle += 0.04f;
        if (pad.buttons & SCE_CTRL_LTRIGGER) camDist *= 1.03f;
        if (pad.buttons & SCE_CTRL_RTRIGGER) camDist *= 0.97f;
        if (pitch >  1.4f) pitch = 1.4f; if (pitch < -1.4f) pitch = -1.4f;

        // Follow the player, framed at ~half the camera height (torso, not feet).
        float targetX = playerX, targetY = playerY + camHeight * 0.5f, targetZ = playerZ;

        float cp = cosf(pitch);
        float ex = targetX - sinf(orbit_angle)*cp*camDist;
        float ez = targetZ - cosf(orbit_angle)*cp*camDist;
        float ey = targetY + sinf(pitch)*camDist;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Projection.
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        {
            const float nearp = 1.0f, farp = 5000.0f, aspect = SCR_W / SCR_H;
            const float top = nearp * 0.767f;     // tan(37.5 deg) ~ vfov 75
            const float right = top * aspect;
            glFrustum(-right, right, -top, top, nearp, farp);
        }

        // View.
        glMatrixMode(GL_MODELVIEW);
        float view[16];
        look_at(view, ex, ey, ez, targetX, targetY, targetZ, 0.0f, 1.0f, 0.0f);

        for (int si = 0; si < afn_sprite_count; si++) {
            int mi = afn_sprites[si].meshIdx;
            if (mi < 0 || mi >= afn_mesh_count) continue;
            const AfnSpriteInst* sp = &afn_sprites[si];
            glLoadMatrixf(view);
            glTranslatef(sp->x, sp->y, sp->z);
            if (sp->rotZ != 0.0f) glRotatef(sp->rotZ, 0,0,1);
            if (sp->rotX != 0.0f) glRotatef(sp->rotX, 1,0,0);
            if (sp->rotY != 0.0f) glRotatef(sp->rotY, 0,1,0);
            glScalef(sp->scale, sp->scale, sp->scale);
            draw_mesh(mi);
        }

#ifdef AFN_HAS_PLAYER_RIG
        // Player rig + every NPC: each skinned from its own rig at its own
        // transform/clip (player follows the camera; NPCs at their world spots).
        rigs_render(view, playerX, playerY, playerZ);
#endif

        vglSwapBuffers(GL_FALSE);
    }

    sceKernelExitProcess(0);
    return 0;
}
