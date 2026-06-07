// Affinity PS Vita runtime — Mode 4 scene bring-up.
// Renders the exported level meshes with vitaGL. Ported from psp_runtime's
// scene.c, but simplified for the Vita's headroom: no per-frame frustum
// culling / bucketing and no texture swizzling (just upload GL textures and
// draw each mesh's full index buffer). Free-orbit camera so the level can be
// inspected on-device. Rig / sprites / sky / audio / collision come next.
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>
#include <string.h>

#include "psv_mapdata.h"   // defines afn_meshes / afn_sprites / camera start

#define SCR_W 960.0f
#define SCR_H 544.0f
#define DEG2RAD (3.14159265f / 180.0f)

static GLuint s_meshTex[256];   // one GL texture per mesh (0 = none)

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
    vglInit(0x800000);
    vglWaitVblankStart(GL_TRUE);

    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    upload_textures();

    // Free-orbit camera around the scene's start point.
    float targetX = afn_cam_start_x, targetY = afn_cam_start_h, targetZ = afn_cam_start_z;
    float yaw = afn_cam_start_angle, pitch = 0.3f;
    float dist = afn_orbit_dist > 1.0f ? afn_orbit_dist * 2.0f : 120.0f;

    SceCtrlData pad;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        // Right stick / dpad: orbit. Triggers: zoom. Left stick: pan target.
        float lx = (pad.lx - 128) / 128.0f, ly = (pad.ly - 128) / 128.0f;
        float rx = (pad.rx - 128) / 128.0f, ry = (pad.ry - 128) / 128.0f;
        if (fabsf(rx) > 0.15f) yaw   += rx * 0.04f;
        if (fabsf(ry) > 0.15f) pitch += ry * 0.03f;
        if (pad.buttons & SCE_CTRL_LEFT)  yaw -= 0.04f;
        if (pad.buttons & SCE_CTRL_RIGHT) yaw += 0.04f;
        if (pad.buttons & SCE_CTRL_LTRIGGER) dist *= 1.03f;
        if (pad.buttons & SCE_CTRL_RTRIGGER) dist *= 0.97f;
        if (pitch >  1.4f) pitch = 1.4f; if (pitch < -1.4f) pitch = -1.4f;
        float fwdX = sinf(yaw), fwdZ = cosf(yaw);
        if (fabsf(ly) > 0.15f) { targetX -= ly*fwdX*3.0f; targetZ -= ly*fwdZ*3.0f; }
        if (fabsf(lx) > 0.15f) { targetX += lx*fwdZ*3.0f; targetZ -= lx*fwdX*3.0f; }

        float cp = cosf(pitch);
        float ex = targetX - sinf(yaw)*cp*dist;
        float ez = targetZ - cosf(yaw)*cp*dist;
        float ey = targetY + sinf(pitch)*dist;

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

        vglSwapBuffers(GL_FALSE);
    }

    sceKernelExitProcess(0);
    return 0;
}
