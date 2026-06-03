// DSM / DSA emitters — see dsma_emit.h. Faithful C++ port of dsma_common.py so
// the editor's NDS export produces the same bytes as tools/gltf_to_dsma.py.
// Math is done in double to match the Python reference.

#include "dsma_emit.h"

#include <cmath>

namespace {

// ---- Fixed-point conversions (display_list.py) -----------------------------

uint32_t f32(double v)   { long long r = (long long)(v * 4096.0);   return (uint32_t)(r & 0xFFFFFFFFLL); }
uint32_t v16(double v)   { int r = (int)(v * 4096.0); if (r < 0) r += 0x10000; return (uint32_t)(r & 0xFFFF); }
uint32_t v10(double v)   { int r = (int)(v * 64.0);   if (r < 0) r += 0x400;   return (uint32_t)(r & 0x3FF); }
uint32_t diff10(double v){ int r = (int)(v * 512.0);  if (r < 0) r += 0x400;   return (uint32_t)(r & 0x3FF); }
uint32_t t16(double v)   { int r = (int)(v * 16.0);   if (r < 0) r += 0x10000; return (uint32_t)(r & 0xFFFF); }
uint32_t n10(double v)   { int r = (int)(v * 512.0); if (r < -0x200) r = -0x200; if (r > 0x1FF) r = 0x1FF; if (r < 0) r += 0x400; return (uint32_t)(r & 0x3FF); }
double v16f(double v)    { return v / 4096.0; }
double v10f(double v)    { return v / 64.0; }

// ---- Small vector / quaternion math (dsma_common.py) -----------------------

struct V3 { double x, y, z; };
struct Q  { double w, x, y, z; };

V3 sub(V3 a, V3 b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
V3 add(V3 a, V3 b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
V3 cross(V3 a, V3 b) { return { a.y*b.z-b.y*a.z, a.z*b.x-b.z*a.x, a.x*b.y-b.x*a.y }; }
double len(V3 v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
V3 norm(V3 v) { double m = len(v); return m > 0 ? V3{ v.x/m, v.y/m, v.z/m } : v; }

Q qcomplement(Q q) { return { q.w, -q.x, -q.y, -q.z }; }
Q qmul(Q a, Q b) {
    return {
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
        a.x*b.w + a.w*b.x + a.y*b.z - a.z*b.y,
        a.y*b.w + a.w*b.y + a.z*b.x - a.x*b.z,
        a.z*b.w + a.w*b.z + a.x*b.y - a.y*b.x
    };
}

// 4x3 (rotation+translation) matrix from quaternion + translation, row-major.
struct M4x3 { double m[3][4]; };
M4x3 jointInfoToM4x3(Q q, V3 t) {
    double wx=2*q.w*q.x, wy=2*q.w*q.y, wz=2*q.w*q.z;
    double x2=2*q.x*q.x, xy=2*q.x*q.y, xz=2*q.x*q.z;
    double y2=2*q.y*q.y, yz=2*q.y*q.z, z2=2*q.z*q.z;
    return {{
        { 1-y2-z2,  xy-wz,    xz+wy,   t.x },
        { xy+wz,    1-x2-z2,  yz-wx,   t.y },
        { xz-wy,    yz+wx,    1-x2-y2, t.z },
    }};
}
V3 mulM4x3(V3 v, const M4x3& m) {
    return {
        v.x*m.m[0][0] + v.y*m.m[0][1] + v.z*m.m[0][2] + m.m[0][3],
        v.x*m.m[1][0] + v.y*m.m[1][1] + v.z*m.m[1][2] + m.m[1][3],
        v.x*m.m[2][0] + v.y*m.m[2][1] + v.z*m.m[2][2] + m.m[2][3]
    };
}

// ---- Display list builder (display_list.py DisplayList) ---------------------

struct DisplayList {
    std::vector<uint32_t> commands;
    std::vector<uint32_t> parameters;
    std::vector<uint32_t> out;
    bool hasLastVtx = false, hasLastNormal = false, hasLastTexcoord = false;
    double lvx=0, lvy=0, lvz=0, lnx=0, lny=0, lnz=0, ltu=0, ltv=0;

    void addCommand(uint32_t cmd) { addCommand(cmd, nullptr, 0); }
    void addCommand(uint32_t cmd, const uint32_t* args, int nargs) {
        commands.push_back(cmd);
        for (int i = 0; i < nargs; i++) parameters.push_back(args[i]);
        if (commands.size() == 4) {
            uint32_t header = commands[0] | (commands[1]<<8) | (commands[2]<<16) | (commands[3]<<24);
            out.push_back(header);
            for (uint32_t p : parameters) out.push_back(p);
            commands.clear();
            parameters.clear();
        }
    }
    void nop()          { addCommand(0x00); }
    void mtxRestore(int i){ uint32_t a=(uint32_t)i; addCommand(0x14, &a, 1); }
    void beginVtxs(int t){ uint32_t a=(uint32_t)t; addCommand(0x40, &a, 1); }
    void endVtxs()      { addCommand(0x41); }

    void normal(double x, double y, double z) {
        if (hasLastNormal && lnx==x && lny==y && lnz==z) return;
        uint32_t a = n10(x) | (n10(y)<<10) | (n10(z)<<20);
        addCommand(0x21, &a, 1);
        lnx=x; lny=y; lnz=z; hasLastNormal=true;
    }
    void texcoord(double u, double v) {
        if (hasLastTexcoord && ltu==u && ltv==v) return;
        uint32_t a = t16(u) | (t16(v)<<16);
        addCommand(0x22, &a, 1);
        ltu=u; ltv=v; hasLastTexcoord=true;
    }
    void vtx16(double x, double y, double z) {
        uint32_t a[2] = { v16(x) | (v16(y)<<16), v16(z) };
        addCommand(0x23, a, 2);
        lvx=x; lvy=y; lvz=z; hasLastVtx=true;
    }
    void vtx10(double x, double y, double z) {
        uint32_t a = v10(x) | (v10(y)<<10) | (v10(z)<<20);
        addCommand(0x24, &a, 1);
        lvx=x; lvy=y; lvz=z; hasLastVtx=true;
    }
    void vtxXY(double x, double y) { uint32_t a=v16(x)|(v16(y)<<16); addCommand(0x25,&a,1); lvx=x; lvy=y; hasLastVtx=true; }
    void vtxXZ(double x, double z) { uint32_t a=v16(x)|(v16(z)<<16); addCommand(0x26,&a,1); lvx=x; lvz=z; hasLastVtx=true; }
    void vtxYZ(double y, double z) { uint32_t a=v16(y)|(v16(z)<<16); addCommand(0x27,&a,1); lvy=y; lvz=z; hasLastVtx=true; }

    static double err(double x1,double x2,double y1,double y2,double z1,double z2) {
        return std::fabs(x1-x2)*std::fabs(x1-x2) + std::fabs(y1-y2)*std::fabs(y1-y2) + std::fabs(z1-z2)*std::fabs(z1-z2);
    }
    void vtx(double x, double y, double z) {
        if (hasLastVtx) {
            if (v16(lvx) == v16(x)) { vtxYZ(y, z); return; }
            if (v16(lvy) == v16(y)) { vtxXZ(x, z); return; }
            if (v16(lvz) == v16(z)) { vtxXY(x, y); return; }
        }
        double e16 = err(v16f((double)(int16_t)v16(x)), x, v16f((double)(int16_t)v16(y)), y, v16f((double)(int16_t)v16(z)), z);
        double e10 = err(v10f(((int)v10(x)<<22>>22)), x, v10f(((int)v10(y)<<22>>22)), y, v10f(((int)v10(z)<<22>>22)), z);
        if (e10 <= e16) vtx10(x, y, z); else vtx16(x, y, z);
    }
    void switchVtxs(int t) { beginVtxs(t); }  // first call only

    void finalize() {
        while (!commands.empty()) nop();           // pad to a full header group
        out.insert(out.begin(), (uint32_t)out.size());
    }
};

} // namespace

namespace DsmaEmit {

std::vector<uint32_t> BuildDSM(const RiggedMeshAsset& rm, int texW, int texH, bool smooth, int matSlot)
{
    DisplayList dl;
    dl.switchVtxs(0); // triangles

    int nb = rm.boneCount;
    int baseMatrix = 30 - nb + 1;
    int lastJoint = -1;

    int nTri = (int)rm.indices.size() / 3;

    // Per-triangle face normals in world (bind) space.
    std::vector<V3> triNormal(nTri);
    for (int t = 0; t < nTri; t++) {
        V3 world[3];
        for (int k = 0; k < 3; k++) {
            uint32_t vi = rm.indices[t*3 + k];
            int b = rm.vertBone[vi];
            const BonePose& bp = rm.bindPose[b];
            Q q{ bp.qw, bp.qx, bp.qy, bp.qz };
            V3 pos{ bp.px, bp.py, bp.pz };
            M4x3 m = jointInfoToM4x3(q, pos);
            const MeshVertex& mv = rm.baseVerts[vi];
            world[k] = mulM4x3(V3{ mv.px, mv.py, mv.pz }, m);
        }
        V3 a = sub(world[0], world[1]);
        V3 b = sub(world[1], world[2]);
        V3 n = cross(a, b);
        triNormal[t] = (len(n) > 0) ? norm(n) : V3{0,0,0};
    }

    for (int t = 0; t < nTri; t++) {
        // Multi-material: only emit triangles tagged with the requested slot.
        if (matSlot >= 0 && t < (int)rm.triMaterial.size() && rm.triMaterial[t] != matSlot) continue;
        V3 fn = triNormal[t];
        for (int k = 0; k < 3; k++) {
            uint32_t vi = rm.indices[t*3 + k];
            int b = rm.vertBone[vi];
            const MeshVertex& mv = rm.baseVerts[vi];

            dl.texcoord(mv.u * texW, mv.v * texH);

            if (b != lastJoint) { dl.mtxRestore(baseMatrix + b); lastJoint = b; }

            V3 n;
            if (smooth) {
                // The vertex normal is already in this bone's local space.
                n = V3{ mv.nx, mv.ny, mv.nz };
                if (len(n) > 0) n = norm(n);
            } else {
                // Rotate the world face normal into the bone's local space.
                const BonePose& bp = rm.bindPose[b];
                Q q{ bp.qw, bp.qx, bp.qy, bp.qz };
                Q qt = qcomplement(q);
                Q nq{ 0, fn.x, fn.y, fn.z };
                Q r = qmul(qmul(qt, nq), q);
                n = V3{ r.x, r.y, r.z };
                if (len(n) > 0) n = norm(n);
            }
            dl.normal(n.x, n.y, n.z);

            dl.vtx(mv.px, mv.py, mv.pz);
        }
    }

    dl.endVtxs();
    dl.finalize();
    return dl.out;
}

std::vector<uint32_t> BuildDSA(const RiggedMeshAsset& rm, int clipIdx)
{
    std::vector<uint32_t> out;
    if (clipIdx < 0 || clipIdx >= (int)rm.clips.size()) return out;
    const RigAnimClip& clip = rm.clips[clipIdx];
    int nb = rm.boneCount;

    out.push_back(1);                       // version
    out.push_back((uint32_t)clip.frameCount);
    out.push_back((uint32_t)nb);
    for (int f = 0; f < clip.frameCount; f++) {
        for (int b = 0; b < nb; b++) {
            const BonePose& p = clip.frames[f*nb + b];
            out.push_back(f32(p.px)); out.push_back(f32(p.py)); out.push_back(f32(p.pz));
            out.push_back(f32(p.qw)); out.push_back(f32(p.qx)); out.push_back(f32(p.qy)); out.push_back(f32(p.qz));
        }
    }
    return out;
}

} // namespace DsmaEmit
