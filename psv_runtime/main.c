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
#include <stdlib.h>

#include "psv_mapdata.h"   // defines afn_meshes / afn_sprites / camera start
#include "psv_rig.h"       // player rig data (skinned glTF), if AFN_HAS_PLAYER_RIG
#include "psv_player.h"    // AFN_PLAYER_COL_* (custom collision box, if authored)
#include "psv_sky.h"       // sky panorama texture (AFN_HAS_SKY)
#include "psv_sprites.h"   // billboard sprites (AFN_HAS_SPRITES)

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
// Script-glue globals rigs_render reads (defined later in the script-glue block).
extern int afn_skel_anim_obj, afn_skel_anim_clip;   // SetSkelAnim: NPC sprite idx + clip
extern unsigned char afn_sprite_visible[];          // SetVisible/DestroyObject per sprite
static AfnRigVertex s_skinned[AFN_RIG_MAX_VERTS];
static float  s_bonemat[AFN_RIG_MAX_BONES][12];   // 3x4 row-major per bone
static GLuint s_rigTex[AFN_RIG_COUNT][AFN_RIG_MAX_MATS];
static float  s_pframe = 0.0f;                     // player anim frame
static int    s_pclip  = AFN_PLAYER_DEFAULT_CLIP;
static int    s_npcClip[AFN_NPC_COUNT + 1];        // per-NPC clip override (-1 = use default)
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
    for (int i = 0; i < AFN_NPC_COUNT; i++) s_npcClip[i] = -1;   // no SetSkelAnim override yet
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
                     float px, float py, float pz, float yawDeg, float instScale,
                     const float* upN) {
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

    // Orient with a basis matrix: up = floor normal (slope tilt), forward = the
    // yaw heading projected onto the slope plane, right = up x fwd. Ported from
    // psp_runtime/rig.c. NPCs pass world-up so they stay vertical.
    float ux = upN ? upN[0] : 0.0f, uy = upN ? upN[1] : 1.0f, uz = upN ? upN[2] : 0.0f;
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul > 1e-6f) { ux/=ul; uy/=ul; uz/=ul; }
    float yr = yawDeg * DEG2RAD + AFN_RIG_YAW_OFFSET;
    float ydx = sinf(yr), ydz = cosf(yr);
    float d = ydx*ux + ydz*uz;                              // yawDir . up (ydy = 0)
    float fx = ydx - ux*d, fy = -uy*d, fz = ydz - uz*d;     // project onto slope plane
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl > 1e-6f) { fx/=fl; fy/=fl; fz/=fl; } else { fx=0; fy=0; fz=1; }
    float rgx = uy*fz - uz*fy, rgy = uz*fx - ux*fz, rgz = ux*fy - uy*fx;  // right = up x fwd
    float S = R->scale * instScale;
    float model[16] = {     // column-major: col0=right, col1=up, col2=forward
        rgx*S, rgy*S, rgz*S, 0,
        ux*S,  uy*S,  uz*S,  0,
        fx*S,  fy*S,  fz*S,  0,
        px,    py,    pz,    1
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
static void rigs_render(const float* view, float playerX, float playerY, float playerZ,
                        float playerYaw, const float* floorN) {
    const AfnRig* PR = &afn_rigs[AFN_PLAYER_RIG_SLOT];
    if (afn_rig_clip >= 0 && afn_rig_clip < PR->clips && afn_rig_clip != s_pclip) {
        s_pclip = afn_rig_clip; s_pframe = 0.0f;
    }
    s_pframe = rig_advance(PR, s_pclip, s_pframe);
    build_bone_mats(PR, s_pclip, s_pframe); skin(PR);
    rig_draw(PR, s_rigTex[AFN_PLAYER_RIG_SLOT], view, playerX, playerY, playerZ, playerYaw, AFN_PLAYER_SCALE, floorN);

    // SetSkelAnim: set the matching NPC's clip override (by editor sprite index).
    // Needs the 8-wide afn_npc_inst (editor index in col 7) from the new export.
#ifdef AFN_HAS_SPRITE_IDX
    if (afn_skel_anim_obj >= 0) {
        for (int i = 0; i < AFN_NPC_COUNT; i++)
            if ((int)afn_npc_inst[i][7] == afn_skel_anim_obj) s_npcClip[i] = afn_skel_anim_clip;
        afn_skel_anim_obj = -1;
    }
#endif
    for (int i = 0; i < AFN_NPC_COUNT; i++) {
        const float* N = afn_npc_inst[i];
        int slot = (int)N[6]; if (slot < 0 || slot >= AFN_RIG_COUNT) continue;
        const AfnRig* R = &afn_rigs[slot];
#ifdef AFN_HAS_SPRITE_IDX
        int eidx = (int)N[7];
        if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;   // hidden / destroyed
#endif
        int clip = s_npcClip[i] >= 0 ? s_npcClip[i] : (int)N[5];   // SetSkelAnim override
        if (clip < 0 || clip >= R->clips) clip = 0;
        s_npcFrame[i] = rig_advance(R, clip, s_npcFrame[i]);
        build_bone_mats(R, clip, s_npcFrame[i]); skin(R);
        rig_draw(R, s_rigTex[slot], view, N[0], N[1], N[2], N[3], N[4], 0);  // NPCs: world up
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

// ---------------------------------------------------------------------------
// Sky panorama + sprite billboards (ported from psp_runtime/sky.c + billboard.c)
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_SKY
static GLuint s_skyTex = 0;
static void sky_init(void) {
    glGenTextures(1, &s_skyTex);
    glBindTexture(GL_TEXTURE_2D, s_skyTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, AFN_SKY_W, AFN_SKY_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, afn_sky_tex);
}
// Far textured quad in eye space (identity modelview) so it tracks the camera;
// U scrolls with yaw for the 360 wrap. No depth writes -> scene draws on top.
static void sky_render(float camAngle) {
    float u = camAngle / (2.0f * 3.14159265f);
    const float D = 5000.0f, X = 7200.0f, Y = 4200.0f;
    AfnVertex sky[4] = {
        { u,      0.0f, 0xFFFFFFFFu, -X,  Y, -D },
        { u+1.0f, 0.0f, 0xFFFFFFFFu,  X,  Y, -D },
        { u+1.0f, 1.0f, 0xFFFFFFFFu,  X, -Y, -D },
        { u,      1.0f, 0xFFFFFFFFu, -X, -Y, -D },
    };
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);  glDisable(GL_BLEND); glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);  glBindTexture(GL_TEXTURE_2D, s_skyTex);
    AfnVertex* v = sky;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
}
#endif // AFN_HAS_SKY

#ifdef AFN_HAS_SPRITES
static float  s_sprFrame[AFN_SPR_INST_COUNT];
static GLuint s_sprTex[sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0])];
static void billboards_init(void) {
    int nf = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
    for (int f = 0; f < nf; f++) {
        glGenTextures(1, &s_sprTex[f]);
        glBindTexture(GL_TEXTURE_2D, s_sprTex[f]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, afn_spr_frame_w[f], afn_spr_frame_h[f], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, afn_spr_frame_ptrs[f]);
    }
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) s_sprFrame[i] = (float)afn_spr_fstart[i];
}
// Camera-facing (Y-axis) textured quads in world space, drawn through the view.
static void billboards_render(const float* view, float camAngle) {
    float rx = cosf(camAngle), rz = -sinf(camAngle);   // camera right in XZ
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) {
#ifdef AFN_HAS_SPRITE_IDX
        int eidx = afn_spr_editor_idx[i];
        if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
        int lo = afn_spr_fstart[i], hi = afn_spr_fend[i]; if (hi < lo) hi = lo;
        s_sprFrame[i] += afn_spr_fps[i] / 60.0f;
        if (s_sprFrame[i] >= (float)(hi+1)) s_sprFrame[i] = (float)lo;
        int cf = (int)s_sprFrame[i]; if (cf < lo) cf = lo; if (cf > hi) cf = hi;
        float sz = afn_spr_basesize[i] * afn_spr_scale[i] * 0.25f, hw = sz * 0.5f;
        float px = afn_spr_x[i], py = afn_spr_y[i], pz = afn_spr_z[i];
        float lx = px - rx*hw, lz = pz - rz*hw, Rx = px + rx*hw, Rz = pz + rz*hw;
        float top = py + sz, bot = py;
        AfnVertex q[4] = {
            { 0,0, 0xFFFFFFFFu, lx, top, lz },
            { 1,0, 0xFFFFFFFFu, Rx, top, Rz },
            { 1,1, 0xFFFFFFFFu, Rx, bot, Rz },
            { 0,1, 0xFFFFFFFFu, lx, bot, lz },
        };
        glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &q->u);
        glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &q->color);
        glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &q->x);
        glBindTexture(GL_TEXTURE_2D, s_sprTex[cf]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_CULL_FACE);
}
#endif // AFN_HAS_SPRITES

// ---------------------------------------------------------------------------
// Mesh collision (floor/wall) — float port of psp_runtime/collision.c. Faces are
// built once from the exported mesh geometry, transformed to world space, and
// bucketed into an XZ grid for cheap per-cell floor/wall queries.
// ---------------------------------------------------------------------------
#define COL_GN     16
#define COL_NCELL  (COL_GN * COL_GN)
typedef struct {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    float nx, ny, nz;
    int   flags;   // 1 floor, 2 ceiling, 4 wall
} ColFace;
static ColFace* s_faces = 0;
static int      s_faceCount = 0;
static int*     s_cellStart = 0;
static int*     s_cellCount = 0;
static int*     s_cellFaces = 0;
static float    s_minX, s_minZ, s_cellSize;
static unsigned* s_faceStamp = 0;
static unsigned  s_queryStamp = 0;

static int cell_x(float x) { int c=(int)((x-s_minX)/s_cellSize); return c<0?0:(c>=COL_GN?COL_GN-1:c); }
static int cell_z(float z) { int c=(int)((z-s_minZ)/s_cellSize); return c<0?0:(c>=COL_GN?COL_GN-1:c); }

static void collide_build(void) {
    int total = 0;
    for (int si = 0; si < afn_sprite_count; si++) {
        int mi = afn_sprites[si].meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        total += afn_meshes[mi].indexCount / 3;
    }
    if (total <= 0) return;
    s_faces = (ColFace*)malloc(sizeof(ColFace) * total);
    if (!s_faces) return;
    float mnx=1e30f, mnz=1e30f, mxx=-1e30f, mxz=-1e30f;
    for (int si = 0; si < afn_sprite_count; si++) {
        const AfnSpriteInst* sp = &afn_sprites[si];
        int mi = sp->meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        const AfnMesh* m = &afn_meshes[mi];
        const AfnVertex* V = m->verts;
        const unsigned short* I = m->indices;
        float ry=sp->rotY*DEG2RAD, rx=sp->rotX*DEG2RAD, rz=sp->rotZ*DEG2RAD;
        float cY=cosf(ry),sY=sinf(ry),cX=cosf(rx),sX=sinf(rx),cZ=cosf(rz),sZ=sinf(rz);
        float scl=sp->scale;
        for (int t = 0; t + 3 <= m->indexCount; t += 3) {
            float wp[9];
            for (int k=0;k<3;k++){
                const AfnVertex* vv=&V[I[t+k]];
                float lx=vv->x*scl, ly=vv->y*scl, lz=vv->z*scl;
                float ax= lx*cY+lz*sY, az=-lx*sY+lz*cY, ay=ly;
                float ay2=ay*cX-az*sX, az2=ay*sX+az*cX;
                float ax2=ax*cZ-ay2*sZ, ay3=ax*sZ+ay2*cZ;
                wp[k*3+0]=sp->x+ax2; wp[k*3+1]=sp->y+ay3; wp[k*3+2]=sp->z+az2;
            }
            float e1x=wp[3]-wp[0],e1y=wp[4]-wp[1],e1z=wp[5]-wp[2];
            float e2x=wp[6]-wp[0],e2y=wp[7]-wp[1],e2z=wp[8]-wp[2];
            float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
            float len=sqrtf(nx*nx+ny*ny+nz*nz); if (len<1e-6f) continue;
            nx/=len; ny/=len; nz/=len;
            ColFace* F=&s_faces[s_faceCount++];
            F->ax=wp[0];F->ay=wp[1];F->az=wp[2]; F->bx=wp[3];F->by=wp[4];F->bz=wp[5]; F->cx=wp[6];F->cy=wp[7];F->cz=wp[8];
            F->flags=(ny>0.3f)?1:(ny<-0.7f)?2:4;
            F->nx=nx; F->ny=ny; F->nz=nz;
            for (int k=0;k<3;k++){ float X=wp[k*3],Z=wp[k*3+2]; if(X<mnx)mnx=X; if(X>mxx)mxx=X; if(Z<mnz)mnz=Z; if(Z>mxz)mxz=Z; }
        }
    }
    if (s_faceCount<=0) return;
    s_minX=mnx; s_minZ=mnz;
    float span=(mxx-mnx)>(mxz-mnz)?(mxx-mnx):(mxz-mnz);
    s_cellSize=span/COL_GN; if (s_cellSize<1.0f) s_cellSize=1.0f;
    s_cellStart=(int*)calloc(COL_NCELL,sizeof(int));
    s_cellCount=(int*)calloc(COL_NCELL,sizeof(int));
    s_faceStamp=(unsigned*)calloc(s_faceCount,sizeof(unsigned));
    if (!s_cellStart||!s_cellCount||!s_faceStamp) return;
    for (int i=0;i<s_faceCount;i++){
        const ColFace* F=&s_faces[i];
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        for (int gz=cell_z(mnZ);gz<=cell_z(mxZ);gz++) for (int gx=cell_x(mnX);gx<=cell_x(mxX);gx++) s_cellCount[gz*COL_GN+gx]++;
    }
    int te=0; for (int c=0;c<COL_NCELL;c++){ s_cellStart[c]=te; te+=s_cellCount[c]; s_cellCount[c]=0; }
    s_cellFaces=(int*)malloc(sizeof(int)*(te>0?te:1)); if (!s_cellFaces) return;
    for (int i=0;i<s_faceCount;i++){
        const ColFace* F=&s_faces[i];
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        for (int gz=cell_z(mnZ);gz<=cell_z(mxZ);gz++) for (int gx=cell_x(mnX);gx<=cell_x(mxX);gx++){
            int c=gz*COL_GN+gx; s_cellFaces[s_cellStart[c]+s_cellCount[c]++]=i; }
    }
}

#ifdef AFN_HAS_PLAYER_COL
#define COL_RADIUS AFN_PLAYER_COL_RADIUS
#define COL_BOTTOM AFN_PLAYER_COL_BOTTOM
#define COL_TOP    AFN_PLAYER_COL_TOP
#else
#define COL_RADIUS 6.0f
#define COL_BOTTOM 0.0f
#define COL_TOP    24.0f
#endif
#define WALL_TOP_TOL 5.0f

static int collide_floor(float x, float z, float py, float* outY, float* outN) {
    if (!s_cellFaces) return 0;
    int c=cell_z(z)*COL_GN+cell_x(x);
    int start=s_cellStart[c], count=s_cellCount[c];
    float bestY=0; int found=0; const ColFace* bestF=0;
    for (int i=0;i<count;i++){
        const ColFace* F=&s_faces[s_cellFaces[start+i]];
        if (!(F->flags&1)) continue;
        float c0=(F->bx-F->ax)*(z-F->az)-(F->bz-F->az)*(x-F->ax);
        float c1=(F->cx-F->bx)*(z-F->bz)-(F->cz-F->bz)*(x-F->bx);
        float c2=(F->ax-F->cx)*(z-F->cz)-(F->az-F->cz)*(x-F->cx);
        if (!((c0>=0&&c1>=0&&c2>=0)||(c0<=0&&c1<=0&&c2<=0))) continue;
        float cs=c0+c1+c2;
        float fy=(cs==0)?(F->ay+F->by+F->cy)/3.0f:(c1*F->ay+c2*F->by+c0*F->cy)/cs;
        if (fy>py+COL_TOP) continue;
        if (!found||fy>bestY){ bestY=fy; found=1; bestF=F; }
    }
    *outY=bestY;
    if (outN){ if(bestF){outN[0]=bestF->nx;outN[1]=bestF->ny;outN[2]=bestF->nz;} else {outN[0]=0;outN[1]=1;outN[2]=0;} }
    return found;
}

static void collide_walls(float* x, float* z, float py) {
    if (!s_cellFaces||!s_faceStamp) return;
    float ppx=*x, ppz=*z;
    int gx0=cell_x(ppx-COL_RADIUS), gx1=cell_x(ppx+COL_RADIUS);
    int gz0=cell_z(ppz-COL_RADIUS), gz1=cell_z(ppz+COL_RADIUS);
    unsigned stamp=++s_queryStamp;
    for (int gz=gz0;gz<=gz1;gz++) for (int gx=gx0;gx<=gx1;gx++){
        int c=gz*COL_GN+gx; int start=s_cellStart[c], count=s_cellCount[c];
        for (int i=0;i<count;i++){
            int fi=s_cellFaces[start+i];
            if (s_faceStamp[fi]==stamp) continue; s_faceStamp[fi]=stamp;
            const ColFace* F=&s_faces[fi];
            if (!(F->flags&4)) continue;
            float fMinY=fminf(F->ay,fminf(F->by,F->cy)), fMaxY=fmaxf(F->ay,fmaxf(F->by,F->cy));
            if (py+COL_TOP<fMinY || py+COL_BOTTOM>=fMaxY-WALL_TOP_TOL) continue;
            if (F->nx*F->nx+F->nz*F->nz<1e-8f) continue;
            float vx[3]={F->ax,F->bx,F->cx}, vz[3]={F->az,F->bz,F->cz};
            float bestPx=ppx,bestPz=ppz,bestD2=1e30f;
            for (int e=0;e<3;e++){
                float x0=vx[e],z0=vz[e], sx=vx[(e+1)%3]-x0, sz=vz[(e+1)%3]-z0;
                float L2=sx*sx+sz*sz;
                float t=(L2>1e-8f)?((ppx-x0)*sx+(ppz-z0)*sz)/L2:0.0f;
                if (t<0)t=0; else if (t>1)t=1;
                float Px=x0+sx*t, Pz=z0+sz*t;
                float dx=ppx-Px, dz=ppz-Pz, d2=dx*dx+dz*dz;
                if (d2<bestD2){ bestD2=d2; bestPx=Px; bestPz=Pz; }
            }
            if (bestD2>=COL_RADIUS*COL_RADIUS) continue;
            float d=sqrtf(bestD2), push=COL_RADIUS-d;
            if (d>1e-4f){ ppx+=(ppx-bestPx)/d*push; ppz+=(ppz-bestPz)/d*push; }
            else { float xl=sqrtf(F->nx*F->nx+F->nz*F->nz); ppx+=F->nx/xl*push; ppz+=F->nz/xl*push; }
        }
    }
    *x=ppx; *z=ppz;
}

// ---------------------------------------------------------------------------
// Input + node-script variable layer. Behaviour is node-driven, per the engine
// convention: the emitted script (psv_script.h) sets these each frame and the
// movement/camera/rig below only READ them. input_update() supplies the raw
// analog/button defaults so the game is playable before/without a script
// (mirrors psp_runtime input.c + script_glue.c).
// ---------------------------------------------------------------------------
int afn_input_fwd = 0, afn_input_right = 0;   // camera-space move intent (256 = full)
int afn_move_speed = 0, afn_speed_prio = 0;   // node-set speed (0 = use walk default)
int afn_player_frozen = 0;
int orbit_angle = 0;                          // camera yaw, brad (65536 = full circle)
int orbit_pitch = 0;                          // camera pitch, brad (node OrbitCamera Up/Down + right stick)

enum { KEY_A=1,KEY_B=2,KEY_X=4,KEY_Y=8,KEY_L=16,KEY_R=32,KEY_START=64,KEY_SELECT=128,
       KEY_UP=256,KEY_DOWN=512,KEY_LEFT=1024,KEY_RIGHT=2048 };
unsigned afn_keys_held=0, afn_keys_pressed=0, afn_keys_released=0;
static int key_is_down(unsigned k){ return (afn_keys_held & k)!=0; }
static int key_hit(unsigned k){ return (afn_keys_pressed & k)!=0; }
static int key_released(unsigned k){ return (afn_keys_released & k)!=0; }

static void input_update(const SceCtrlData* pad) {
    unsigned b = pad->buttons, k = 0;
    if (b & SCE_CTRL_CROSS)    k|=KEY_A;
    if (b & SCE_CTRL_CIRCLE)   k|=KEY_B;
    if (b & SCE_CTRL_SQUARE)   k|=KEY_X;
    if (b & SCE_CTRL_TRIANGLE) k|=KEY_Y;
    if (b & SCE_CTRL_LTRIGGER) k|=KEY_L;
    if (b & SCE_CTRL_RTRIGGER) k|=KEY_R;
    if (b & SCE_CTRL_START)    k|=KEY_START;
    if (b & SCE_CTRL_SELECT)   k|=KEY_SELECT;
    if (b & SCE_CTRL_UP)       k|=KEY_UP;
    if (b & SCE_CTRL_DOWN)     k|=KEY_DOWN;
    if (b & SCE_CTRL_LEFT)     k|=KEY_LEFT;
    if (b & SCE_CTRL_RIGHT)    k|=KEY_RIGHT;
    int ax = (int)pad->lx - 128, ay = (int)pad->ly - 128;
    if (ay<-48) k|=KEY_UP;   if (ay>48) k|=KEY_DOWN;
    if (ax<-48) k|=KEY_LEFT; if (ax>48) k|=KEY_RIGHT;
    afn_input_right = ax;       // raw analog default (±128); nodes use ±256
    afn_input_fwd   = -ay;
    afn_keys_pressed  = k & ~afn_keys_held;
    afn_keys_released = ~k & afn_keys_held;
    afn_keys_held     = k;
}

// ---- Script-glue variables ------------------------------------------------
// The emitted node graph writes a LOT of variables that belong to subsystems
// the PSV runtime hasn't ported yet (HUD, grind rails, fades, sprite manip,
// Mode 0). They're defined here as inert globals — exactly like PSP's
// script_glue.c — so the generated code links; only the movement/camera/anim
// ones above are actually consumed. As each subsystem lands, its var stops
// being inert.
#ifndef NUM_SPRITES
#define NUM_SPRITES 64
#endif
int afn_play_anim=0, afn_anim_prio=0, afn_anim_speed=0, afn_auto_orbit_speed=0;
int afn_skel_anim_obj=-1, afn_skel_anim_clip=0, afn_sprite_anim_spr=-1, afn_sprite_anim_val=0;
int afn_collided_sprite=-1, afn_collided_tm_obj=-1, afn_bp_cur_spr_idx=-1, afn_bp_cur_tm_obj=-1;
int afn_current_mode=0, afn_current_scene=0, afn_scripts_stopped=0;
int afn_start_x=0, afn_start_y=0, afn_start_z=0, afn_text_color=0, afn_timer_visible=0;
int afn_wall_collided_sprite=-1, afn_fade_target=0, afn_fade_frames=0, afn_fade_counter=0, afn_fade_level=0;
int player_vy=0, player_ground_y=0, afn_player_vx_world=0, afn_player_vz_world=0;
int afn_velocity_falloff=0, afn_pending_boost_fwd=0;
int afn_grinding=0, afn_grinding_active=0, afn_grind_rail=-1, afn_grind_power=0;
int afn_grind_boost=0, afn_grind_bleed=0, afn_grind_catch_y=0, afn_grind_catch_x=0;
// Gravity/terminal in 8.8 fixed (256 = 1 world unit/frame), so a SetGravity /
// SetMaxFall node (which writes value*256) drives them. Seeded to the PSV
// world defaults (0.8 / 30) rather than the weak editor-pixel defaults.
int afn_gravity=205, afn_terminal_vel=7680, afn_friction=0, afn_force_x=0, afn_force_z=0;
int afn_cam_locked=0, afn_cam_speed=0, afn_tank_camera=0, afn_player_heading=0;
int afn_player_height=0, afn_player_width=0, afn_bg_color=0, afn_anim_speed_dummy=0;
int afn_active_element=0, afn_elem_idx=0, afn_cursor_stop=0, afn_stop_count=0, afn_hud_value=0;
int afn_checkpoint_set=0, afn_checkpoint_x=0, afn_checkpoint_y=0, afn_checkpoint_z=0;
int afn_score=0, afn_shake_frames=0, afn_shake_intensity=0, afn_last_key=0;
int afn_frame_count=0, afn_dt_tick=0;
int afn_scene_start_transition=0;
// Player physics vars the emitted code reads/writes (NDS defines these in
// fps3d.c). Kept inert for now — the movement loop uses its own playerX/Y/Z;
// wiring teleport/IsMoving/Jump nodes to these is a follow-up.
int player_x=0, player_y=0, player_z=0, player_vy_unused=0;
int player_on_ground=1, player_moving=0;
unsigned int afn_flags=0, afn_rng=1;
unsigned char afn_hud_visible[NUM_SPRITES]={0}, afn_sprite_visible[NUM_SPRITES]={0};
unsigned char afn_sprite_flip[NUM_SPRITES]={0}, afn_collision_enabled[NUM_SPRITES]={0};
int afn_hp[NUM_SPRITES]={0}, afn_state_timer[NUM_SPRITES]={0};
int afn_stop_links[16]={0};
int afn_hud_layer_frame[8]={0}, afn_hud_layer_tick[8]={0};
unsigned char afn_hud_layer_active[8]={0}, afn_hud_layer_speed_override[8]={0};
int tm_fol_active=0, tm_fol_obj=-1, tm_fol_dist=0, tm_fol_facing=0, tm_fol_moving=0, tm_fol_speed=0;
int tm_player_tx=0, tm_player_ty=0;
// Audio entry points — defined in audio.c (sceAudio software mixer + sequencer).
void afn_play_sound(int id);
void afn_play_sfx(int smpIdx, int gain, int fifo);
void afn_stop_sound(void);
void afn_stop_sfx_sample(int smpIdx);
void afn_audio_init(void);
void afn_audio_tick(void);

// The emitted node graph. Included AFTER the variables/keys above so its static
// functions can reference them (single translation unit). Defines AFN_HAS_SCRIPT
// when the scene actually has nodes; otherwise this is an inert stub and the raw
// stick input above drives movement.
#include "psv_script.h"

#ifdef AFN_HAS_SCRIPT
static void script_start(void) { afn_emitted_script_start(); afn_bp_dispatch_start(); }
static void script_tick(void) {
    // Node-driven inputs are recomputed from scratch each frame by the graph.
    afn_input_fwd = 0; afn_input_right = 0; afn_speed_prio = 0; afn_move_speed = 0;
    afn_emitted_script_update();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_emitted_script_key_released();
    afn_bp_dispatch_update();
    afn_bp_dispatch_key_held();
    afn_bp_dispatch_key_pressed();
    afn_bp_dispatch_key_released();
}
static int script_present(void) { return 1; }
#else
static void script_start(void) {}
static void script_tick(void)  {}
static int  script_present(void){ return 0; }
#endif

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
    afn_audio_init();   // software mixer thread (no-op if the scene has no sound)
#ifdef AFN_HAS_SKY
    sky_init();
#endif
#ifdef AFN_HAS_SPRITES
    billboards_init();
#endif
#ifdef AFN_HAS_PLAYER_RIG
    rig_init();
#endif

    // Player state. The follow camera and NPCs read playerX/Y/Z; the movement
    // step below drives it (camera-relative left stick + gravity + floor/wall
    // collision against the level mesh).
#ifdef AFN_HAS_PLAYER_RIG
    float playerX = AFN_PLAYER_START_X, playerY = AFN_PLAYER_START_Y, playerZ = AFN_PLAYER_START_Z;
#else
    float playerX = afn_cam_start_x, playerY = afn_cam_start_h, playerZ = afn_cam_start_z;
#endif
    float playerYaw = 0.0f;   // rig facing (degrees)
    float playerVY  = 0.0f;   // vertical velocity (gravity / jump)
    int   grounded  = 1;
    float s_floorN[3] = {0.0f, 1.0f, 0.0f};   // smoothed floor normal (slope tilt)
    collide_build();
    {   // Drop onto the floor at spawn so we don't start mid-air.
        float fy, fn[3];
        if (collide_floor(playerX, playerZ, playerY + 200.0f, &fy, fn))
            playerY = fy - COL_BOTTOM;
    }

    // Orbit-follow camera, anchored to the active camera slot
    // (afn_cam_slots[afn_active_camera] = { yaw, dist, height, horizon }). The
    // player orbits freely with the right stick / L-R; triggers zoom. The slot's
    // height sets the default downward tilt and the look-at target height. PSV
    // has no scripts yet so afn_active_camera stays 0 (scene default), but the
    // runtime re-reads it each frame so a future SetCamera node just works.
    const float* slot0 = afn_cam_slots[afn_active_camera];
    orbit_angle       = (int)(slot0[0] * (65536.0f / 6.2831853f));  // GLOBAL brad (node + manual)
    afn_player_heading = orbit_angle;   // tank heading starts facing the camera-forward
    float camDist     = slot0[1] > 1.0f ? slot0[1] : 60.0f;  // world px
    float camHeight   = slot0[2];                            // world px
    // Pitch is also brad in the global orbit_pitch so the OrbitCamera Up/Down
    // node and the right stick drive it together. Seed it from the slot's tilt.
    orbit_pitch       = (int)(atan2f(camHeight > 0.0f ? camHeight : 8.0f, camDist) * (65536.0f / 6.2831853f));

    SceCtrlData pad;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
#ifdef AFN_HAS_SPRITE_IDX
    // All sprites start visible + collidable (the inert arrays default to 0 =
    // hidden). DestroyObject/SetVisible nodes flip these at runtime.
    for (int i = 0; i < NUM_SPRITES; i++) { afn_sprite_visible[i] = 1; afn_collision_enabled[i] = 1; }
#endif
    script_start();   // OnStart + blueprint start hooks

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        // Expose engine state to the node graph BEFORE it ticks: player_on_ground
        // (Jump/IsJumping gates), player_moving (IsMoving), and player_x/y/z
        // (teleport/distance/checkpoint nodes read+write the world position).
        player_on_ground = grounded;
        int pteleX = player_x = (int)playerX;
        int pteleY = player_y = (int)playerY;
        int pteleZ = player_z = (int)playerZ;

        // Node-driven: input_update() sets the raw defaults, then the emitted
        // graph (script_tick) overrides afn_input_fwd/right, afn_move_speed,
        // orbit_angle and afn_rig_clip. The movement/camera below only READ them.
        input_update(&pad);
        script_tick();
        afn_audio_tick();   // 60 Hz sequencer clock (envelopes / note scheduling)

        // Screen-fade tick: ease afn_fade_level toward afn_fade_target over
        // afn_fade_counter frames (set by ChangeScene / fade nodes). Rendered as
        // a fullscreen overlay below (vitaGL has no REG_MASTER_BRIGHT).
        if (afn_fade_counter > 0) {
            afn_fade_counter--;
            int span = afn_fade_frames > 0 ? afn_fade_frames : 1;
            afn_fade_level = afn_fade_target * (span - afn_fade_counter) / span;
            if (afn_fade_counter == 0) afn_fade_level = afn_fade_target;
        }

        // A teleport/checkpoint node wrote player_x/y/z — apply it to the float
        // position (normal frames leave them unchanged, so no precision loss).
        if (player_x != pteleX) playerX = (float)player_x;
        if (player_y != pteleY) playerY = (float)player_y;
        if (player_z != pteleZ) playerZ = (float)player_z;

        // Re-read the active slot each frame (a future SetCamera node retargets it).
        const float* S = afn_cam_slots[afn_active_camera];
        camDist   = S[1] > 1.0f ? S[1] : camDist;   // keep manual zoom unless slot overrides
        camHeight = S[2];

        // Right stick / L-R also orbit the camera (added to orbit_angle/pitch,
        // which the OrbitCamera node may also drive). Triggers zoom.
        float rx = (pad.rx - 128) / 128.0f, ry = (pad.ry - 128) / 128.0f;
        if (fabsf(rx) > 0.15f) orbit_angle += (int)(rx * 600.0f);
        if (fabsf(ry) > 0.15f) orbit_pitch += (int)(ry * 450.0f);
        // NOTE: no trigger-zoom here — the triggers are KEY_L/KEY_R, which the
        // node graph binds to orbit. A hardcoded zoom would double-bind them
        // (R = orbit + zoom-in, L = orbit + zoom-out). Camera distance is slot/
        // node driven only.
        // Clamp pitch to ~±80 deg in brad (65536 = 360 deg -> ±14563).
        if (orbit_pitch >  14563) orbit_pitch =  14563;
        if (orbit_pitch < -14563) orbit_pitch = -14563;

        // Camera yaw + pitch (radians) from the node/manual orbit_angle/pitch (brad).
        float camAngle = orbit_angle * (6.2831853f / 65536.0f);
        float pitch    = orbit_pitch * (6.2831853f / 65536.0f);

        // --- Player movement: reads the node-set move intent (afn_input_fwd/right,
        //     256 = full) in camera space, scaled by afn_move_speed (or the walk
        //     default when no script). Ported from PSP scene_update. ---
        float fAmt = afn_input_fwd  / 256.0f;
        float rAmt = afn_input_right / 256.0f;
        // Tank controls: when afn_tank_camera is set (a Turn Player / tank node),
        // movement + facing follow afn_player_heading (D-pad turned), so the
        // camera orbits independently. Otherwise movement is camera-relative.
        float moveAngle = afn_tank_camera ? (afn_player_heading * (6.2831853f/65536.0f)) : camAngle;
        float fwdX = sinf(moveAngle), fwdZ = cosf(moveAngle);
        float rgtX = cosf(moveAngle), rgtZ = -sinf(moveAngle);
        float mvX = fAmt*fwdX + rAmt*rgtX;
        float mvZ = fAmt*fwdZ + rAmt*rgtZ;
        int scripted = script_present();
        if ((mvX*mvX + mvZ*mvZ > 0.0001f) && (!scripted || afn_move_speed > 0) && !afn_player_frozen) {
            float speed = scripted ? (afn_move_speed * 0.08f)
                                   : (afn_walk_speed > 0.0f ? afn_walk_speed * 0.25f : 6.0f);
            float dx = mvX*speed, dz = mvZ*speed;
            float dlen = sqrtf(dx*dx + dz*dz);
            int steps = (int)(dlen / 3.0f) + 1;   // MAX_MOVE_STEP: don't tunnel walls
            float ix = dx/steps, iz = dz/steps;
            for (int st = 0; st < steps; st++) { playerX += ix; playerZ += iz; collide_walls(&playerX, &playerZ, playerY); }
            playerYaw = atan2f(mvX, mvZ) * (180.0f/3.14159265f);
        }
        // In tank mode the rig faces the heading even when standing still.
        if (afn_tank_camera) playerYaw = afn_player_heading * (360.0f/65536.0f);

        // Node-driven world-axis push velocity (boost pads / knockback). 8.8
        // fixed. BoostForward wrote a pending magnitude -> decompose along the
        // camera forward; SetVelocityX/Z write the globals directly. Linear
        // VelocityFalloff(N) decay over N frames. (Mirrors fps3d.c:1190-1207.)
        if (afn_pending_boost_fwd) {
            afn_player_vx_world = (int)(sinf(camAngle) * afn_pending_boost_fwd);
            afn_player_vz_world = (int)(cosf(camAngle) * afn_pending_boost_fwd);
            afn_pending_boost_fwd = 0;
        }
        if (afn_player_vx_world || afn_player_vz_world) {
            playerX += afn_player_vx_world / 256.0f;
            playerZ += afn_player_vz_world / 256.0f;
            collide_walls(&playerX, &playerZ, playerY);
            if (afn_velocity_falloff > 0) {
                afn_player_vx_world -= afn_player_vx_world / afn_velocity_falloff;
                afn_player_vz_world -= afn_player_vz_world / afn_velocity_falloff;
                if (--afn_velocity_falloff == 0) { afn_player_vx_world = 0; afn_player_vz_world = 0; }
            }
        }

        player_moving = (afn_input_fwd != 0 || afn_input_right != 0);   // IsMoving gate
        if (grounded && (pad.buttons & SCE_CTRL_CROSS)) { playerVY = 13.0f; grounded = 0; }  // debug jump
        if (player_vy != 0) { playerVY = player_vy / 256.0f; player_vy = 0; grounded = 0; }  // Jump node
        collide_walls(&playerX, &playerZ, playerY);
        playerVY -= afn_gravity / 256.0f;                  // gravity (SetGravity node, 8.8)
        float term = afn_terminal_vel / 256.0f;
        if (playerVY < -term) playerVY = -term;            // terminal velocity (SetMaxFall node)
        playerY += playerVY;
        {
            float fy, fn[3];
            if (collide_floor(playerX, playerZ, playerY, &fy, fn) && playerY <= fy - COL_BOTTOM) {
                playerY = fy - COL_BOTTOM; playerVY = 0.0f; grounded = 1;
                // Ease the rig's up-axis toward the floor normal (slope tilt).
                s_floorN[0] += (fn[0]-s_floorN[0])*0.2f;
                s_floorN[1] += (fn[1]-s_floorN[1])*0.2f;
                s_floorN[2] += (fn[2]-s_floorN[2])*0.2f;
            } else {
                grounded = 0;
                s_floorN[0] += (0.0f-s_floorN[0])*0.1f;   // airborne: ease back upright
                s_floorN[1] += (1.0f-s_floorN[1])*0.1f;
                s_floorN[2] += (0.0f-s_floorN[2])*0.1f;
            }
        }

        // Sprite collision -> OnCollision chains. Player circle vs each NPC's
        // position (proximity). Sets afn_collided_sprite to the NPC's editor
        // sprite index, then fires the scene + blueprint collision dispatchers
        // (a blueprint attached to that sprite reacts). 10-frame spawn grace.
        afn_frame_count++;
#ifdef AFN_HAS_SPRITE_IDX
        if (afn_frame_count > 10) {
            for (int i = 0; i < AFN_NPC_COUNT; i++) {
                int sp = (int)afn_npc_inst[i][7];
                if (sp < 0 || sp >= NUM_SPRITES) continue;
                if (!afn_sprite_visible[sp] || !afn_collision_enabled[sp]) continue;
                float dx = playerX - afn_npc_inst[i][0];
                float dz = playerZ - afn_npc_inst[i][2];
                float dy = playerY - afn_npc_inst[i][1];
                float r = COL_RADIUS + COL_RADIUS * afn_npc_inst[i][4];   // player + NPC (scale) radius
                if (dx*dx + dz*dz < r*r && dy > -COL_TOP*2.0f && dy < COL_TOP*2.0f) {
                    afn_collided_sprite = sp;
                    afn_emitted_script_collision();
                    afn_bp_dispatch_collision();
                }
            }
        }
#endif

        // Follow the player, framed at ~half the camera height (torso, not feet).
        float targetX = playerX, targetY = playerY + camHeight * 0.5f, targetZ = playerZ;

        float cp = cosf(pitch);
        float ex = targetX - sinf(camAngle)*cp*camDist;
        float ez = targetZ - cosf(camAngle)*cp*camDist;
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

#ifdef AFN_HAS_SKY
        sky_render(camAngle);   // far panorama behind everything (no depth write)
#endif

        for (int si = 0; si < afn_sprite_count; si++) {
            int mi = afn_sprites[si].meshIdx;
            if (mi < 0 || mi >= afn_mesh_count) continue;
#ifdef AFN_HAS_SPRITE_IDX
            int eidx = afn_mesh_inst_sprite[si];
            if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
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
        rigs_render(view, playerX, playerY, playerZ, playerYaw, s_floorN);
#endif

#ifdef AFN_HAS_SPRITES
        billboards_render(view, camAngle);   // camera-facing animated sprites
#endif

        // Fade overlay: a fullscreen quad over the scene. level<0 darkens (fade
        // out to black), level>0 brightens (fade in from white). |level|/16 alpha.
        if (afn_fade_level != 0) {
            int lv = afn_fade_level; if (lv > 16) lv = 16; if (lv < -16) lv = -16;
            unsigned int A = (unsigned int)((lv < 0 ? -lv : lv) * 255 / 16);
            unsigned int col = (A << 24) | (lv > 0 ? 0x00FFFFFFu : 0u);   // 0xAABBGGRR
            AfnVertex fq[4] = { {0,0,col,0,0,0}, {0,0,col,1,0,0}, {0,0,col,1,1,0}, {0,0,col,0,1,0} };
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0,1,0,1,-1,1);
            glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
            glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            AfnVertex* v = fq;
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
            glVertexPointer(3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
            glEnable(GL_DEPTH_TEST);
        }

        vglSwapBuffers(GL_FALSE);
    }

    sceKernelExitProcess(0);
    return 0;
}
