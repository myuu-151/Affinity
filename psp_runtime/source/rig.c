// Affinity PSP runtime — player rig skinning + render.
// DSMA is rigid (1 bone/vertex). baseVerts are in their bone's local space and
// clip frames are ABSOLUTE bone poses, so per frame:
//   bone matrix  = mat(lerp(poseF0, poseF1))
//   skinned vert = boneMat[vertBone] * baseVert
// The skinned verts go in a 16-byte-aligned buffer, dcache-flushed for the GE.
#include "rig.h"
#include "affinity_psp.h"
#include "psp_rig.h"
#include "tex.h"

#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#ifdef AFN_HAS_PLAYER_RIG

// Yaw offset so the rig's authored forward matches the runtime heading
// (tune if the model faces the wrong way; 0/90/180/270 are the usual fixes).
#ifndef AFN_RIG_YAW_OFFSET
#define AFN_RIG_YAW_OFFSET 0.0f
#endif

static AfnRigVertex* s_skinned = 0;              // AFN_RIG_VERTS, recomputed/frame
static float      s_bonemat[AFN_RIG_BONES][12];  // 3x4 row-major per bone
static float      s_frame = (float)AFN_PLAYER_DEFAULT_CLIP * 0.0f;
static int        s_clip  = AFN_PLAYER_DEFAULT_CLIP;

extern int afn_rig_clip;   // node-driven (script) or controller-set clip selector

// Per-material swizzled 16-bit rig textures (built once in rig_init).
static unsigned short* s_rigTexSw[AFN_RIG_MATS];

// pose {px,py,pz, qw,qx,qy,qz} -> 3x4 row-major (rot | translation)
static void pose_to_mat(const float* p, float* m) {
    float px=p[0],py=p[1],pz=p[2], w=p[3],x=p[4],y=p[5],z=p[6];
    float n = w*w+x*x+y*y+z*z;
    if (n > 1e-8f) { n = 1.0f/sqrtf(n); w*=n; x*=n; y*=n; z*=n; }
    float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    m[0]=1-2*(yy+zz); m[1]=2*(xy-wz);   m[2]=2*(xz+wy);   m[3]=px;
    m[4]=2*(xy+wz);   m[5]=1-2*(xx+zz); m[6]=2*(yz-wx);   m[7]=py;
    m[8]=2*(xz-wy);   m[9]=2*(yz+wx);   m[10]=1-2*(xx+yy);m[11]=pz;
}

static void build_bone_mats(int clip, float frame) {
    const float* cd = afn_rig_clip_ptrs[clip];
    int nf = afn_rig_clip_frames[clip];
    if (nf < 1) nf = 1;
    int f0 = (int)frame; if (f0 < 0) f0 = 0; if (f0 >= nf) f0 = nf - 1;
    int f1 = f0 + 1;
    if (f1 >= nf) f1 = afn_rig_clip_loop[clip] ? 0 : nf - 1;
    float t = frame - (float)((int)frame);

    for (int b = 0; b < AFN_RIG_BONES; b++) {
        const float* p0 = &cd[(f0 * AFN_RIG_BONES + b) * 7];
        const float* p1 = &cd[(f1 * AFN_RIG_BONES + b) * 7];
        float p[7];
        p[0] = p0[0] + (p1[0]-p0[0])*t;
        p[1] = p0[1] + (p1[1]-p0[1])*t;
        p[2] = p0[2] + (p1[2]-p0[2])*t;
        // nlerp quaternion along the shortest arc
        float d = p0[3]*p1[3] + p0[4]*p1[4] + p0[5]*p1[5] + p0[6]*p1[6];
        float s = (d < 0.0f) ? -1.0f : 1.0f;
        p[3] = p0[3] + (p1[3]*s - p0[3])*t;
        p[4] = p0[4] + (p1[4]*s - p0[4])*t;
        p[5] = p0[5] + (p1[5]*s - p0[5])*t;
        p[6] = p0[6] + (p1[6]*s - p0[6])*t;
        pose_to_mat(p, s_bonemat[b]);
    }
}

static void skin(void) {
    for (int v = 0; v < AFN_RIG_VERTS; v++) {
        const float* m = s_bonemat[afn_rig_vbone[v]];
        float x = afn_rig_vpos[v*3+0], y = afn_rig_vpos[v*3+1], z = afn_rig_vpos[v*3+2];
        s_skinned[v].x = m[0]*x + m[1]*y + m[2]*z + m[3];
        s_skinned[v].y = m[4]*x + m[5]*y + m[6]*z + m[7];
        s_skinned[v].z = m[8]*x + m[9]*y + m[10]*z + m[11];
        // Skin the normal by the bone's rotation (no translation), renormalize.
        // The GE then transforms it by the rig's model matrix for lighting.
        float nx = afn_rig_vnorm[v*3+0], ny = afn_rig_vnorm[v*3+1], nz = afn_rig_vnorm[v*3+2];
        float wx = m[0]*nx + m[1]*ny + m[2]*nz;
        float wy = m[4]*nx + m[5]*ny + m[6]*nz;
        float wz = m[8]*nx + m[9]*ny + m[10]*nz;
        float nl = wx*wx + wy*wy + wz*wz;
        if (nl > 1e-12f) { nl = 1.0f/sqrtf(nl); wx*=nl; wy*=nl; wz*=nl; }
        s_skinned[v].nx = wx; s_skinned[v].ny = wy; s_skinned[v].nz = wz;
    }
}

void rig_init(void) {
    s_skinned = (AfnRigVertex*)memalign(16, sizeof(AfnRigVertex) * AFN_RIG_VERTS);
    if (!s_skinned) return;
    for (int v = 0; v < AFN_RIG_VERTS; v++) {
        s_skinned[v].u = afn_rig_vuv[v*2+0];
        s_skinned[v].v = afn_rig_vuv[v*2+1];
        s_skinned[v].color = 0xFFFFFFFF;   // modulate texture unchanged
    }
    if (s_clip < 0 || s_clip >= AFN_RIG_CLIPS) s_clip = 0;

    // Convert each material's texture to swizzled 16-bit (same fill-rate win
    // as the world meshes).
    for (int g = 0; g < AFN_RIG_MATS; g++) {
        s_rigTexSw[g] = 0;
        if (afn_rig_tex_ptrs[g] && afn_rig_tex_w[g] > 0 && afn_rig_tex_h[g] > 0)
            s_rigTexSw[g] = psp_make_tex16(afn_rig_tex_ptrs[g], afn_rig_tex_w[g], afn_rig_tex_h[g]);
    }
}

int  rig_present(void) { return 1; }
void rig_player_start(float out[3]) {
    out[0] = AFN_PLAYER_START_X; out[1] = AFN_PLAYER_START_Y; out[2] = AFN_PLAYER_START_Z;
}
void rig_set_moving(int moving) {
    // No-script fallback: drive the clip selector from movement (default = idle,
    // the other clip = walk). When a script is present it sets afn_rig_clip.
    afn_rig_clip = (moving && AFN_RIG_CLIPS > 1) ? ((AFN_PLAYER_DEFAULT_CLIP == 0) ? 1 : 0)
                                                 : AFN_PLAYER_DEFAULT_CLIP;
}

void rig_render(float px, float py, float pz, float yawDeg, const float* upN,
                const float* camR, const float* camU, const float* camF, int frozen) {
    if (!s_skinned) return;

    // Clip selector is node-driven (afn_rig_clip): switch + restart on change.
    if (afn_rig_clip >= 0 && afn_rig_clip < AFN_RIG_CLIPS && afn_rig_clip != s_clip) {
        s_clip = afn_rig_clip; s_frame = 0.0f;
    }

    if (!frozen) {
        s_frame += 0.4f;   // ~24 fps animation at 60 Hz
        int nf = afn_rig_clip_frames[s_clip];
        if (nf > 1) {
            if (afn_rig_clip_loop[s_clip]) { while (s_frame >= (float)nf) s_frame -= (float)nf; }
            else if (s_frame > (float)(nf-1)) s_frame = (float)(nf-1);
        } else s_frame = 0.0f;
    }
    build_bone_mats(s_clip, s_frame);
    skin();
    sceKernelDcacheWritebackRange(s_skinned, sizeof(AfnRigVertex) * AFN_RIG_VERTS);

    // Orient the rig with a basis matrix: up = floor normal (slope tilt),
    // forward = the yaw heading projected onto the slope plane, right = up x fwd.
    // Baking it into the model matrix avoids gum rotate-order/gimbal issues.
    float ux = upN ? upN[0] : 0.0f, uy = upN ? upN[1] : 1.0f, uz = upN ? upN[2] : 0.0f;
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul > 1e-6f) { ux/=ul; uy/=ul; uz/=ul; }
    float yr = yawDeg * (3.14159265f/180.0f) + AFN_RIG_YAW_OFFSET;
    float ydx = sinf(yr), ydz = cosf(yr);
    float d = ydx*ux + ydz*uz;                          // yawDir . up (ydy = 0)
    float fx = ydx - ux*d, fy = -uy*d, fz = ydz - uz*d; // project onto slope plane
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl > 1e-6f) { fx/=fl; fy/=fl; fz/=fl; } else { fx=0; fy=0; fz=1; }
    float rx = uy*fz - uz*fy, ry = uz*fx - ux*fz, rz = ux*fy - uy*fx;  // right = up x fwd
    float S = AFN_PLAYER_RIG_SCALE;
    ScePspFMatrix4 m;
    m.x.x = rx*S; m.x.y = ry*S; m.x.z = rz*S; m.x.w = 0.0f;   // local +X -> right
    m.y.x = ux*S; m.y.y = uy*S; m.y.z = uz*S; m.y.w = 0.0f;   // local +Y -> up
    m.z.x = fx*S; m.z.y = fy*S; m.z.z = fz*S; m.z.w = 0.0f;   // local +Z -> forward
    m.w.x = px;   m.w.y = py;   m.w.z = pz;   m.w.w = 1.0f;
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadMatrix(&m);

    if (AFN_RIG_CULL == 2) sceGuDisable(GU_CULL_FACE);
    else { sceGuEnable(GU_CULL_FACE); sceGuFrontFace(AFN_RIG_CULL == 1 ? GU_CW : GU_CCW); }
#if AFN_RIG_USE_ALPHA
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
#else
    sceGuDisable(GU_BLEND);
#endif

#ifdef AFN_PLAYER_RIG_CAMLIGHT
    // Camera headlamp: the baked eye-space aim (DX/DY/DZ) reconstructed into world
    // space via the camera basis; the GE's view transform carries it back to eye
    // space, so the light stays fixed relative to the viewer. The white vertex
    // colour is the material, so the lit result modulates the texture.
    {
        ScePspFVector3 ld = {
            AFN_PLAYER_RIG_LIGHT_DX*camR[0] + AFN_PLAYER_RIG_LIGHT_DY*camU[0] + AFN_PLAYER_RIG_LIGHT_DZ*camF[0],
            AFN_PLAYER_RIG_LIGHT_DX*camR[1] + AFN_PLAYER_RIG_LIGHT_DY*camU[1] + AFN_PLAYER_RIG_LIGHT_DZ*camF[1],
            AFN_PLAYER_RIG_LIGHT_DX*camR[2] + AFN_PLAYER_RIG_LIGHT_DY*camU[2] + AFN_PLAYER_RIG_LIGHT_DZ*camF[2]
        };
        sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ld);
        sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFFFF);
        sceGuAmbient(0xFF404040);                    // base fill so the shadowed side isn't black
        sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE); // vertex colour acts as the material
        sceGuEnable(GU_LIGHTING);
        sceGuEnable(GU_LIGHT0);
    }
#else
    (void)camR; (void)camU; (void)camF;
#endif

    for (int g = 0; g < AFN_RIG_MATS; g++) {
        int ic = afn_rig_idx_counts[g];
        if (ic <= 0) continue;
        if (s_rigTexSw[g] && afn_rig_tex_w[g] > 0) {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_5551, 0, 0, 1);   // swizzled 16-bit
            sceGuTexImage(0, afn_rig_tex_w[g], afn_rig_tex_h[g], afn_rig_tex_w[g], s_rigTexSw[g]);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_LINEAR, GU_LINEAR);
            sceGuTexLevelMode(GU_TEXTURE_CONST, 0.0f);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
        sceGumDrawArray(GU_TRIANGLES, AFN_RIG_VERTEX_FLAGS | GU_INDEX_16BIT,
                        ic, afn_rig_idx_ptrs[g], s_skinned);
    }
#ifdef AFN_PLAYER_RIG_CAMLIGHT
    sceGuDisable(GU_LIGHTING);   // leave lighting off for billboards/sky after the rig
    sceGuDisable(GU_LIGHT0);
#endif
}

#else  // no player rig in this build — stubs

void rig_init(void) {}
int  rig_present(void) { return 0; }
void rig_player_start(float out[3]) { out[0] = out[1] = out[2] = 0.0f; }
void rig_set_moving(int moving) { (void)moving; }
void rig_render(float px, float py, float pz, float yawDeg, const float* upN,
                const float* camR, const float* camU, const float* camF, int frozen) {
    (void)px; (void)py; (void)pz; (void)yawDeg; (void)upN;
    (void)camR; (void)camU; (void)camF; (void)frozen;
}

#endif
