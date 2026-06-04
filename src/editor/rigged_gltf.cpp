// glTF / GLB skinned-mesh loader. See rigged_gltf.h.
//
// Port of dsma-library/tools/gltf_to_dsma.py into C++ using cgltf. DSMA does
// RIGID skinning (one bone per vertex, bone matrix in the DS matrix stack), so
// multi-weight vertices are collapsed to their dominant bone. Vertex positions
// are stored in their bone's LOCAL space (IBM * pos); bind pose and per-frame
// bone transforms are ABSOLUTE (hierarchy composed) — matching the converter so
// the editor preview and the on-device DSMA render agree.

#define CGLTF_IMPLEMENTATION
#include "../../thirdparty/cgltf/cgltf.h"

#include "rigged_gltf.h"

#include "stb_image.h"   // declarations only; impl lives in frame_loop.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace {

// Bleed opaque colors outward into transparent (alpha<8) texels so UV-island
// edges don't sample empty/white padding. Fixes the white seams that show up
// under the DS's nearest-neighbor texture sampling. RGBA in 0xAABBGGRR order.
void dilateTransparentEdges(std::vector<uint32_t>& px, int w, int h, int passes)
{
    for (int p = 0; p < passes; p++) {
        std::vector<uint32_t> src = px;
        bool changed = false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                if ((src[y*w + x] >> 24) >= 8) continue;   // already opaque
                for (int dy = -1; dy <= 1 && (px[y*w+x] >> 24) < 8; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        uint32_t n = src[ny*w + nx];
                        if ((n >> 24) >= 8) {
                            px[y*w + x] = (n & 0x00FFFFFF) | 0xFF000000;  // copy color, mark opaque
                            changed = true;
                            break;
                        }
                    }
            }
        if (!changed) break;
    }
}

// ---- Column-major 4x4 matrix helpers (glTF / cgltf layout: m[col*4 + row]) --

struct Mat4 { float m[16]; };

Mat4 mat4Mul(const Mat4& a, const Mat4& b)
{
    Mat4 o;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o.m[c * 4 + r] = a.m[0 * 4 + r] * b.m[c * 4 + 0] +
                             a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                             a.m[2 * 4 + r] * b.m[c * 4 + 2] +
                             a.m[3 * 4 + r] * b.m[c * 4 + 3];
    return o;
}

void mat4MulPoint(const Mat4& m, float x, float y, float z,
                  float& ox, float& oy, float& oz)
{
    ox = m.m[0] * x + m.m[4] * y + m.m[8]  * z + m.m[12];
    oy = m.m[1] * x + m.m[5] * y + m.m[9]  * z + m.m[13];
    oz = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
}

// Rotate a direction by the matrix's 3x3 part (no translation).
void mat4MulDir(const Mat4& m, float x, float y, float z,
                float& ox, float& oy, float& oz)
{
    ox = m.m[0] * x + m.m[4] * y + m.m[8]  * z;
    oy = m.m[1] * x + m.m[5] * y + m.m[9]  * z;
    oz = m.m[2] * x + m.m[6] * y + m.m[10] * z;
}

// MESA gluInvertMatrix, column-major.
bool mat4Invert(const float m[16], float invOut[16])
{
    float inv[16], det;
    inv[0]  =  m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) invOut[i] = inv[i] * det;
    return true;
}

// Decompose an absolute transform into translation + unit quaternion (ignores
// scale, which DSMA does not store).
BonePose mat4Decompose(const Mat4& m)
{
    BonePose bp;
    bp.px = m.m[12]; bp.py = m.m[13]; bp.pz = m.m[14];

    float sx = std::sqrt(m.m[0]*m.m[0] + m.m[1]*m.m[1] + m.m[2]*m.m[2]);
    float sy = std::sqrt(m.m[4]*m.m[4] + m.m[5]*m.m[5] + m.m[6]*m.m[6]);
    float sz = std::sqrt(m.m[8]*m.m[8] + m.m[9]*m.m[9] + m.m[10]*m.m[10]);
    if (sx == 0) sx = 1; if (sy == 0) sy = 1; if (sz == 0) sz = 1;

    // Normalized rotation matrix r[row][col].
    float r[3][3] = {
        { m.m[0]/sx, m.m[4]/sy, m.m[8]/sz },
        { m.m[1]/sx, m.m[5]/sy, m.m[9]/sz },
        { m.m[2]/sx, m.m[6]/sy, m.m[10]/sz },
    };

    float trace = r[0][0] + r[1][1] + r[2][2];
    float qw, qx, qy, qz;
    if (trace > 0) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        qw = 0.25f * s;
        qx = (r[2][1] - r[1][2]) / s;
        qy = (r[0][2] - r[2][0]) / s;
        qz = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        qw = (r[2][1] - r[1][2]) / s;
        qx = 0.25f * s;
        qy = (r[0][1] + r[1][0]) / s;
        qz = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        qw = (r[0][2] - r[2][0]) / s;
        qx = (r[0][1] + r[1][0]) / s;
        qy = 0.25f * s;
        qz = (r[1][2] + r[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        qw = (r[1][0] - r[0][1]) / s;
        qx = (r[0][2] + r[2][0]) / s;
        qy = (r[1][2] + r[2][1]) / s;
        qz = 0.25f * s;
    }
    float mag = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
    if (mag == 0) mag = 1;
    bp.qw = qw/mag; bp.qx = qx/mag; bp.qy = qy/mag; bp.qz = qz/mag;
    return bp;
}

Mat4 readAccessorMat4(const cgltf_accessor* acc, cgltf_size index)
{
    Mat4 m;
    cgltf_accessor_read_float(acc, index, m.m, 16);  // glTF stores column-major
    return m;
}

// ---- Animation sampling -----------------------------------------------------

// Interpolate a sampler at time t into out[ncomp]. Handles STEP/LINEAR and
// approximates CUBICSPLINE as linear over its keyframe values. isQuat enables
// hemisphere-corrected normalized lerp.
void sampleSampler(const cgltf_animation_sampler* s, float t, int ncomp,
                   float* out, bool isQuat)
{
    cgltf_size n = s->input->count;
    bool cubic = (s->interpolation == cgltf_interpolation_type_cubic_spline);

    auto readVal = [&](cgltf_size key, float* dst) {
        cgltf_size idx = cubic ? (key * 3 + 1) : key;  // skip in/out tangents
        cgltf_accessor_read_float(s->output, idx, dst, ncomp);
    };

    float t0; cgltf_accessor_read_float(s->input, 0, &t0, 1);
    if (n == 1 || t <= t0) { readVal(0, out); return; }
    float tn; cgltf_accessor_read_float(s->input, n - 1, &tn, 1);
    if (t >= tn) { readVal(n - 1, out); return; }

    cgltf_size k = 0;
    float tk = t0, tk1 = tn;
    for (cgltf_size i = 0; i + 1 < n; i++) {
        float a, b;
        cgltf_accessor_read_float(s->input, i, &a, 1);
        cgltf_accessor_read_float(s->input, i + 1, &b, 1);
        if (t >= a && t <= b) { k = i; tk = a; tk1 = b; break; }
    }

    float v0[4], v1[4];
    readVal(k, v0);
    if (s->interpolation == cgltf_interpolation_type_step) {
        for (int c = 0; c < ncomp; c++) out[c] = v0[c];
        return;
    }
    readVal(k + 1, v1);
    float u = (tk1 > tk) ? (t - tk) / (tk1 - tk) : 0.0f;

    if (isQuat) {
        float dot = v0[0]*v1[0] + v0[1]*v1[1] + v0[2]*v1[2] + v0[3]*v1[3];
        float sign = (dot < 0) ? -1.0f : 1.0f;
        float q[4], mag = 0;
        for (int c = 0; c < 4; c++) { q[c] = v0[c]*(1-u) + sign*v1[c]*u; mag += q[c]*q[c]; }
        mag = std::sqrt(mag); if (mag == 0) mag = 1;
        for (int c = 0; c < 4; c++) out[c] = q[c]/mag;
    } else {
        for (int c = 0; c < ncomp; c++) out[c] = v0[c]*(1-u) + v1[c]*u;
    }
}

int findAttr(const cgltf_primitive* prim, cgltf_attribute_type type, int setIndex)
{
    for (cgltf_size i = 0; i < prim->attributes_count; i++)
        if (prim->attributes[i].type == type && prim->attributes[i].index == setIndex)
            return (int)i;
    return -1;
}

} // namespace

bool LoadRiggedGLTF(const std::string& path, RiggedMeshAsset& out, std::string* err)
{
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        return fail("Failed to parse glTF/GLB");
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return fail("Failed to load glTF buffers");
    }

    // Find the first node that has both a mesh and a skin.
    cgltf_node* meshNode = nullptr;
    for (cgltf_size i = 0; i < data->nodes_count; i++) {
        if (data->nodes[i].mesh && data->nodes[i].skin) { meshNode = &data->nodes[i]; break; }
    }
    if (!meshNode) { cgltf_free(data); return fail("No skinned mesh (node with mesh + skin)"); }

    cgltf_skin* skin = meshNode->skin;
    cgltf_mesh* mesh = meshNode->mesh;
    int boneCount = (int)skin->joints_count;
    if (boneCount > 29) { cgltf_free(data); return fail("Skin exceeds 29-bone DS matrix-stack limit"); }
    if (boneCount == 0) { cgltf_free(data); return fail("Skin has no joints"); }

    out = RiggedMeshAsset();
    out.sourcePath = path;
    out.boneCount = boneCount;

    // Map joint node -> bone index, and bone parent indices.
    auto jointIndexOf = [&](const cgltf_node* n) -> int {
        for (int j = 0; j < boneCount; j++) if (skin->joints[j] == n) return j;
        return -1;
    };
    out.boneParent.resize(boneCount, -1);
    for (int j = 0; j < boneCount; j++)
        out.boneParent[j] = jointIndexOf(skin->joints[j]->parent);

    // Inverse bind matrices and bind-pose absolute transforms = inverse(IBM).
    std::vector<Mat4> ibm(boneCount);
    out.bindPose.resize(boneCount);
    for (int j = 0; j < boneCount; j++) {
        ibm[j] = readAccessorMat4(skin->inverse_bind_matrices, j);
        Mat4 bindAbs;
        if (!mat4Invert(ibm[j].m, bindAbs.m)) { cgltf_free(data); return fail("Singular inverse-bind matrix"); }
        out.bindPose[j] = mat4Decompose(bindAbs);
    }

    // ---- Geometry: collapse to single-bone, store joint-local positions ----
    bool warnedMultiWeight = false;
    for (float& v : out.boundsMin) v =  1e30f;
    for (float& v : out.boundsMax) v = -1e30f;

    // Distinct materials -> slots (cap 8). out.triMaterial records the slot per
    // triangle so export/runtime can bind each material's texture and draw its
    // group separately (the DS binds one texture per draw).
    std::vector<const cgltf_material*> matSlots;
    auto slotOf = [&](const cgltf_material* m) -> int {
        if (!m) return 0;
        for (size_t i = 0; i < matSlots.size(); i++) if (matSlots[i] == m) return (int)i;
        if ((int)matSlots.size() < 8) { matSlots.push_back(m); return (int)matSlots.size() - 1; }
        return 0; // more than 8 materials: fold the overflow into slot 0
    };

    for (cgltf_size p = 0; p < mesh->primitives_count; p++) {
        const cgltf_primitive* prim = &mesh->primitives[p];
        int aPos = findAttr(prim, cgltf_attribute_type_position, 0);
        int aNrm = findAttr(prim, cgltf_attribute_type_normal, 0);
        int aUV  = findAttr(prim, cgltf_attribute_type_texcoord, 0);
        int aJnt = findAttr(prim, cgltf_attribute_type_joints, 0);
        int aWgt = findAttr(prim, cgltf_attribute_type_weights, 0);
        if (aPos < 0 || aJnt < 0 || aWgt < 0) continue;  // not skinned geometry

        const cgltf_accessor* accPos = prim->attributes[aPos].data;
        const cgltf_accessor* accNrm = aNrm >= 0 ? prim->attributes[aNrm].data : nullptr;
        const cgltf_accessor* accUV  = aUV  >= 0 ? prim->attributes[aUV].data  : nullptr;
        const cgltf_accessor* accJnt = prim->attributes[aJnt].data;
        const cgltf_accessor* accWgt = prim->attributes[aWgt].data;

        // Base color factor -> flat vertex color (texture support is a follow-up).
        float baseR = 1, baseG = 1, baseB = 1;
        if (prim->material && prim->material->has_pbr_metallic_roughness) {
            const cgltf_float* bc = prim->material->pbr_metallic_roughness.base_color_factor;
            baseR = bc[0]; baseG = bc[1]; baseB = bc[2];
        }

        int primSlot = slotOf(prim->material);
        uint32_t vbase = (uint32_t)out.baseVerts.size();
        cgltf_size vcount = accPos->count;
        for (cgltf_size v = 0; v < vcount; v++) {
            float pos[3] = {0,0,0}, nrm[3] = {0,1,0}, uv[2] = {0,0}, w[4] = {0,0,0,0};
            cgltf_uint jnt[4] = {0,0,0,0};
            cgltf_accessor_read_float(accPos, v, pos, 3);
            if (accNrm) cgltf_accessor_read_float(accNrm, v, nrm, 3);
            if (accUV)  cgltf_accessor_read_float(accUV, v, uv, 2);
            cgltf_accessor_read_float(accWgt, v, w, 4);
            cgltf_accessor_read_uint(accJnt, v, jnt, 4);

            // Dominant bone.
            int best = 0; for (int c = 1; c < 4; c++) if (w[c] > w[best]) best = c;
            if (!warnedMultiWeight && w[best] < 0.999f) {
                std::fprintf(stderr, "[rig] multi-weight vertices collapsed to dominant bone\n");
                warnedMultiWeight = true;
            }
            int bone = (int)jnt[best];
            if (bone < 0 || bone >= boneCount) bone = 0;

            // Vertex / normal into the bone's local space: IBM * v.
            MeshVertex mv;
            mat4MulPoint(ibm[bone], pos[0], pos[1], pos[2], mv.px, mv.py, mv.pz);
            mat4MulDir(ibm[bone], nrm[0], nrm[1], nrm[2], mv.nx, mv.ny, mv.nz);
            float nl = std::sqrt(mv.nx*mv.nx + mv.ny*mv.ny + mv.nz*mv.nz);
            if (nl > 0) { mv.nx/=nl; mv.ny/=nl; mv.nz/=nl; }
            mv.u = uv[0]; mv.v = uv[1];
            mv.r = baseR; mv.g = baseG; mv.b = baseB;

            out.baseVerts.push_back(mv);
            out.vertBone.push_back(bone);

            // Bind-pose world position for the AABB.
            float wx, wy, wz;
            Mat4 bindAbs; mat4Invert(ibm[bone].m, bindAbs.m);
            mat4MulPoint(bindAbs, mv.px, mv.py, mv.pz, wx, wy, wz);
            float wp[3] = { wx, wy, wz };
            for (int k = 0; k < 3; k++) {
                if (wp[k] < out.boundsMin[k]) out.boundsMin[k] = wp[k];
                if (wp[k] > out.boundsMax[k]) out.boundsMax[k] = wp[k];
            }
        }

        // Indices (reverse winding to match the converter / DS front face).
        if (prim->indices) {
            cgltf_size ic = prim->indices->count;
            for (cgltf_size i = 0; i + 3 <= ic; i += 3) {
                uint32_t i0 = vbase + (uint32_t)cgltf_accessor_read_index(prim->indices, i + 0);
                uint32_t i1 = vbase + (uint32_t)cgltf_accessor_read_index(prim->indices, i + 1);
                uint32_t i2 = vbase + (uint32_t)cgltf_accessor_read_index(prim->indices, i + 2);
                out.indices.push_back(i2);
                out.indices.push_back(i1);
                out.indices.push_back(i0);
                out.triMaterial.push_back((uint8_t)primSlot);
            }
        } else {
            for (cgltf_size i = 0; i + 3 <= vcount; i += 3) {
                out.indices.push_back(vbase + (uint32_t)(i + 2));
                out.indices.push_back(vbase + (uint32_t)(i + 1));
                out.indices.push_back(vbase + (uint32_t)(i + 0));
                out.triMaterial.push_back((uint8_t)primSlot);
            }
        }
    }

    if (out.baseVerts.empty()) { cgltf_free(data); return fail("Skinned mesh has no vertices"); }

    // ---- Textures: one base-color image per material slot, up to 256x256 ----
    // Decode (embedded GLB chunk or external file) to RGBA, resize to a
    // power-of-two <= 256, then median-cut to a 16-colour indexed DS texture.
    // Slot 0 fills the inline fields; slots 1+ go to out.extraMaterials.
    {
        auto decodeMat = [&](const cgltf_material* mat, RigMaterial& dst) {
            if (mat && mat->name) dst.name = mat->name;
            cgltf_image* image = nullptr;
            if (mat && mat->has_pbr_metallic_roughness &&
                mat->pbr_metallic_roughness.base_color_texture.texture &&
                mat->pbr_metallic_roughness.base_color_texture.texture->image)
                image = mat->pbr_metallic_roughness.base_color_texture.texture->image;
            int iw = 0, ih = 0; unsigned char* rgba = nullptr;
            if (image) {
                if (image->buffer_view) {
                    const cgltf_buffer_view* bv = image->buffer_view;
                    const unsigned char* bytes = (const unsigned char*)bv->buffer->data + bv->offset;
                    rgba = stbi_load_from_memory(bytes, (int)bv->size, &iw, &ih, nullptr, 4);
                } else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
                    std::string dir = path; size_t s = dir.find_last_of("/\\");
                    dir = (s == std::string::npos) ? "" : dir.substr(0, s + 1);
                    rgba = stbi_load((dir + image->uri).c_str(), &iw, &ih, nullptr, 4);
                    if (rgba) dst.texturePath = image->uri;
                }
            }
            if (!(rgba && iw > 0 && ih > 0)) return;
            int tw = 1, th = 1;
            while (tw < iw && tw < 256) tw <<= 1;
            while (th < ih && th < 256) th <<= 1;
            if (tw > 256) tw = 256; if (th > 256) th = 256;
            std::vector<uint32_t> resized(tw * th);
            for (int y = 0; y < th; y++)
                for (int x = 0; x < tw; x++) {
                    int sx = x * iw / tw, sy = y * ih / th;
                    unsigned char* p = rgba + (sy * iw + sx) * 4;
                    resized[y * tw + x] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
                }
            stbi_image_free(rgba);

            // Fill transparent padding with neighbouring island colour so edge
            // texels don't sample white (UV-seam fix for low-res DS sampling).
            dilateTransparentEdges(resized, tw, th, 6);

            struct CC { uint32_t rgb; int count; };
            std::vector<CC> hist;
            for (uint32_t px : resized) {
                uint32_t c = px & 0xFFFFFF; bool f = false;
                for (auto& hc : hist) if (hc.rgb == c) { hc.count++; f = true; break; }
                if (!f) hist.push_back({ c, 1 });
            }
            std::sort(hist.begin(), hist.end(), [](const CC& a, const CC& b){ return a.count > b.count; });
            int palCount = (int)std::min((size_t)256, hist.size());
            uint32_t pal[256] = {};
            for (int i = 0; i < palCount; i++) pal[i] = hist[i].rgb | 0xFF000000;

            std::vector<uint8_t> indexed(tw * th);
            for (int i = 0; i < tw * th; i++) {
                uint32_t px = resized[i] & 0xFFFFFF; int best = 0, bd = 0x7FFFFFFF;
                for (int c = 0; c < palCount; c++) {
                    int dr = (int)(px & 0xFF) - (int)(pal[c] & 0xFF);
                    int dg = (int)((px >> 8) & 0xFF) - (int)((pal[c] >> 8) & 0xFF);
                    int db = (int)((px >> 16) & 0xFF) - (int)((pal[c] >> 16) & 0xFF);
                    int d = dr*dr + dg*dg + db*db;
                    if (d < bd) { bd = d; best = c; }
                }
                indexed[i] = (uint8_t)best;
            }
            dst.texW = tw; dst.texH = th;
            dst.texturePixels = std::move(indexed);
            std::memcpy(dst.texturePalette, pal, sizeof(pal));
            dst.textured = true;
        };

        for (size_t s = 0; s < matSlots.size(); s++) {
            RigMaterial rmat;
            decodeMat(matSlots[s], rmat);
            if (s == 0) {
                out.materialName  = rmat.name;
                out.textured      = rmat.textured;
                out.textureManual = false;
                out.texturePath   = rmat.texturePath;
                out.texturePixels = std::move(rmat.texturePixels);
                std::memcpy(out.texturePalette, rmat.texturePalette, sizeof(out.texturePalette));
                out.texW = rmat.texW; out.texH = rmat.texH;
            } else {
                out.extraMaterials.push_back(std::move(rmat));
            }
        }
        // Fall back to the file's first material name if slot 0 had none.
        if (out.materialName.empty() && data->materials_count > 0 && data->materials[0].name)
            out.materialName = data->materials[0].name;
    }

    // ---- Animations: sample at keyframe times, compose hierarchy -----------
    // cgltf_node_transform_world walks node->parent using each node's TRS, so we
    // sample channels into the joint nodes' TRS, then read the world matrix.
    // Snapshot/restore the animated nodes' base TRS around each clip.
    for (cgltf_size a = 0; a < data->animations_count; a++) {
        const cgltf_animation* anim = &data->animations[a];

        // Union of keyframe times across all channels.
        std::set<float> timeSet;
        for (cgltf_size c = 0; c < anim->channels_count; c++) {
            const cgltf_accessor* in = anim->channels[c].sampler->input;
            for (cgltf_size i = 0; i < in->count; i++) {
                float t; cgltf_accessor_read_float(in, i, &t, 1);
                timeSet.insert(t);
            }
        }
        if (timeSet.empty()) continue;
        std::vector<float> times(timeSet.begin(), timeSet.end());

        RigAnimClip clip;
        clip.name = anim->name ? anim->name : ("anim" + std::to_string((int)a));
        clip.frameCount = (int)times.size();
        clip.frames.resize(times.size() * boneCount);

        // Snapshot base TRS of every node targeted by a channel.
        struct NodeTRS { cgltf_node* node; float t[3], r[4], s[3]; };
        std::vector<NodeTRS> snap;
        for (cgltf_size c = 0; c < anim->channels_count; c++) {
            cgltf_node* nd = anim->channels[c].target_node;
            bool have = false;
            for (auto& e : snap) if (e.node == nd) { have = true; break; }
            if (have) continue;
            NodeTRS e; e.node = nd;
            e.t[0]=nd->translation[0]; e.t[1]=nd->translation[1]; e.t[2]=nd->translation[2];
            e.r[0]=nd->rotation[0]; e.r[1]=nd->rotation[1]; e.r[2]=nd->rotation[2]; e.r[3]=nd->rotation[3];
            e.s[0]=nd->scale[0]; e.s[1]=nd->scale[1]; e.s[2]=nd->scale[2];
            snap.push_back(e);
        }

        for (size_t f = 0; f < times.size(); f++) {
            float t = times[f];
            // Apply sampled channels to node TRS.
            for (cgltf_size c = 0; c < anim->channels_count; c++) {
                const cgltf_animation_channel* ch = &anim->channels[c];
                cgltf_node* nd = ch->target_node;
                if (!nd) continue;
                if (ch->target_path == cgltf_animation_path_type_translation) {
                    float v[3]; sampleSampler(ch->sampler, t, 3, v, false);
                    nd->translation[0]=v[0]; nd->translation[1]=v[1]; nd->translation[2]=v[2];
                } else if (ch->target_path == cgltf_animation_path_type_rotation) {
                    float v[4]; sampleSampler(ch->sampler, t, 4, v, true);
                    nd->rotation[0]=v[0]; nd->rotation[1]=v[1]; nd->rotation[2]=v[2]; nd->rotation[3]=v[3];
                } else if (ch->target_path == cgltf_animation_path_type_scale) {
                    float v[3]; sampleSampler(ch->sampler, t, 3, v, false);
                    nd->scale[0]=v[0]; nd->scale[1]=v[1]; nd->scale[2]=v[2];
                }
            }
            // Read each joint's world transform and decompose.
            for (int j = 0; j < boneCount; j++) {
                Mat4 world;
                cgltf_node_transform_world(skin->joints[j], world.m);
                clip.frames[f * boneCount + j] = mat4Decompose(world);
            }
        }

        // Restore base TRS.
        for (auto& e : snap) {
            e.node->translation[0]=e.t[0]; e.node->translation[1]=e.t[1]; e.node->translation[2]=e.t[2];
            e.node->rotation[0]=e.r[0]; e.node->rotation[1]=e.r[1]; e.node->rotation[2]=e.r[2]; e.node->rotation[3]=e.r[3];
            e.node->scale[0]=e.s[0]; e.node->scale[1]=e.s[1]; e.node->scale[2]=e.s[2];
        }

        out.clips.push_back(std::move(clip));
    }

    // Bake a 180° yaw (Y axis) into the absolute bone transforms so the model's
    // authored forward matches the engine's. Vertices are bone-local and stay
    // unchanged; only the bind/animation transforms (and the AABB) rotate.
    {
        auto bakeYaw180 = [](BonePose& p) {
            float qw=p.qw, qx=p.qx, qy=p.qy, qz=p.qz;
            p.qw = -qy; p.qx = qz; p.qy = qw; p.qz = -qx;   // (0,0,1,0) * q
            p.px = -p.px; p.pz = -p.pz;                      // rotateY(180): x,z negate
        };
        for (auto& bp : out.bindPose) bakeYaw180(bp);
        for (auto& cl : out.clips) for (auto& fr : cl.frames) bakeYaw180(fr);
        float nminX = -out.boundsMax[0], nmaxX = -out.boundsMin[0];
        float nminZ = -out.boundsMax[2], nmaxZ = -out.boundsMin[2];
        out.boundsMin[0] = nminX; out.boundsMax[0] = nmaxX;
        out.boundsMin[2] = nminZ; out.boundsMax[2] = nmaxZ;
    }

    // Seed the collision box to the (oriented) bind-pose AABB so a fresh import
    // already wraps the model; the user scales it from there. A saved project
    // overrides these from the rig= line after re-import.
    for (int i = 0; i < 3; i++) {
        out.colCenter[i]  = (out.boundsMin[i] + out.boundsMax[i]) * 0.5f;
        out.colExtents[i] = (out.boundsMax[i] - out.boundsMin[i]) * 0.5f;
        if (out.colExtents[i] < 0.01f) out.colExtents[i] = 0.5f;
    }

    cgltf_free(data);
    return true;
}
