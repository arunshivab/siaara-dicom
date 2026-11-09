// SIAARA DICOM Toolkit - Metal backend for macOS (see gpu.hpp).
//
// UNTESTED: written from Apple's Metal framework and Metal Shading Language documentation and the
// Objective-C runtime reference; it has not been compiled with the Apple SDK or run on a Mac in
// this tree's development environment. Its C++ is compile-checked on Linux against stub
// declarations of the Objective-C runtime functions it uses (SIAARA_METAL_COMPILE_CHECK); the
// Metal Shading Language source is compiled only at run time by the Metal framework.
//
// Plain C++ (no Objective-C): Metal objects are driven through the Objective-C runtime's C API
// (objc_msgSend cast to the exact method signature, sel_registerName, objc_getClass), with manual
// reference counting (new* / alloc / copy results are released; autoreleased results live in an
// NSAutoreleasePool around each operation). Links Metal.framework and Foundation.framework (OS
// frameworks) on Apple only.
//
// Kernels: Metal Shading Language source (kMsl below, with the parameter layout of gpu_params.hpp
// prepended as #defines), compiled once per device with newLibraryWithSource:options:error: and
// fast math disabled (IEEE semantics, as the Vulkan kernels). They mirror spirv_kernels.cpp line
// by line. Volumes live in private 3D textures (R16Unorm for int16 with +32768, R32Float for
// float32) filled through a blit from a shared staging buffer; R32Float is sampled through 8
// texel reads unless the device reports supports32BitFloatFiltering. Output: a shared buffer of
// one packed RGBA8 uint per pixel.
#if defined(__APPLE__) || defined(SIAARA_METAL_COMPILE_CHECK)

// objc_msgSend is declared as void(void) and must be cast to each method's exact type.
#ifndef OBJC_OLD_DISPATCH_PROTOTYPES
#define OBJC_OLD_DISPATCH_PROTOTYPES 0
#endif
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "gpu/gpu_backends.hpp"
#include "volume/volume.hpp"

extern "C" {
// Metal.framework C entry points (MTLDevice.h).
id MTLCreateSystemDefaultDevice(void);
id MTLCopyAllDevices(void);  // macOS: NSArray<id<MTLDevice>>*, retained (+1)
}

namespace siaara::gpu::metal {

namespace {

using NSUInteger = unsigned long;  // LP64 (every macOS target)
using NSInteger = long;
struct MTLSize {
    NSUInteger width, height, depth;
};
struct MTLOrigin {
    NSUInteger x, y, z;
};

// Enumerants (Metal framework headers, documented values).
constexpr NSUInteger kPixelFormatR16Unorm = 20;
constexpr NSUInteger kPixelFormatR32Float = 55;
constexpr NSUInteger kTextureType3D = 7;
constexpr NSUInteger kTextureUsageShaderRead = 0x1;
constexpr NSUInteger kStorageModePrivate = 2;
constexpr NSUInteger kResourceStorageModeShared = 0;  // MTLResourceStorageModeShared | default CPU cache mode
constexpr NSUInteger kCommandBufferStatusCompleted = 4;
constexpr NSInteger kMathModeSafe = 0;

// ---------------------------------------------------------------- Objective-C runtime helpers
template <class R, class... A>
R call(id self, const char* sel, A... args) {
    using Fn = R (*)(id, SEL, A...);
    return reinterpret_cast<Fn>(&objc_msgSend)(self, sel_registerName(sel), args...);
}
id cls(const char* name) { return reinterpret_cast<id>(objc_getClass(name)); }
void drop(id o) {
    if (o) call<void>(o, "release");
}
bool responds(id o, const char* sel) { return o && call<BOOL>(o, "respondsToSelector:", sel_registerName(sel)); }

struct Pool {
    id pool = nullptr;
    Pool() { pool = call<id>(call<id>(cls("NSAutoreleasePool"), "alloc"), "init"); }
    ~Pool() {
        if (pool) call<void>(pool, "drain");
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
};

// A retained NSString (caller releases).
id make_string(const char* s) { return call<id>(call<id>(cls("NSString"), "alloc"), "initWithUTF8String:", s); }
std::string to_string(id ns) {
    if (!ns) return {};
    const char* s = call<const char*>(ns, "UTF8String");
    return s ? std::string(s) : std::string();
}
std::string error_text(id err) {
    if (!err) return "unknown error";
    return to_string(call<id>(err, "localizedDescription"));
}

// ---------------------------------------------------------------- MSL kernels
// The standard library comes first: the parameter #defines (msl_source) must not reach it.
const char* const kMslHead = R"MSL(
#include <metal_stdlib>
using namespace metal;
)MSL";

const char* const kMsl = R"MSL(
struct Ctx {
    device const uint* P;
    device const uint* D;
};
static inline uint pu(Ctx c, uint w) { return c.P[w]; }
static inline float pf(Ctx c, uint w) { return as_type<float>(c.P[w]); }
static inline int pi(Ctx c, uint w) { return as_type<int>(c.P[w]); }
static inline float3 pv3(Ctx c, uint w) { return float3(pf(c, w), pf(c, w + 1), pf(c, w + 2)); }
static inline uint du(Ctx c, uint i) { return c.D[i]; }
static inline float df(Ctx c, uint i) { return as_type<float>(c.D[i]); }
static inline bool flag(Ctx c, uint w, uint bit) { return (c.P[w] & bit) != 0u; }
static inline float clamp01(float v) { return fmin(fmax(v, 0.0f), 1.0f); }

constexpr sampler lin_s(coord::normalized, address::clamp_to_edge, filter::linear);

static inline float decode(Ctx c, uint L, float texel, bool fetched) {
    float s = texel * pf(c, L + LScale);
    if (fetched && flag(c, L + LFlags, kLayerInteger)) s = rint(s);
    return s + pf(c, L + LBias);
}
static inline float fetch(Ctx c, texture3d<float> t, uint L, int3 i) { return decode(c, L, t.read(uint3(i)).x, true); }
static inline int3 dims_i(Ctx c, uint L) { return int3(pi(c, L + LDimsU), pi(c, L + LDimsU + 1), pi(c, L + LDimsU + 2)); }

static inline float nearest_s(Ctx c, texture3d<float> t, uint L, float3 p) {
    const int3 n = dims_i(c, L);
    const int3 i = clamp(int3(floor(p + 0.5f)), int3(0), n - 1);
    return fetch(c, t, L, i);
}
static inline float manual_linear(Ctx c, texture3d<float> t, uint L, float3 p) {
    const int3 n = dims_i(c, L);
    const float3 f = floor(p);
    const int3 i = int3(f);
    const float3 tt = p - f;
    const int3 a = clamp(i, int3(0), n - 1), b = clamp(i + 1, int3(0), n - 1);
    const float c000 = fetch(c, t, L, int3(a.x, a.y, a.z)), c100 = fetch(c, t, L, int3(b.x, a.y, a.z));
    const float c010 = fetch(c, t, L, int3(a.x, b.y, a.z)), c110 = fetch(c, t, L, int3(b.x, b.y, a.z));
    const float c001 = fetch(c, t, L, int3(a.x, a.y, b.z)), c101 = fetch(c, t, L, int3(b.x, a.y, b.z));
    const float c011 = fetch(c, t, L, int3(a.x, b.y, b.z)), c111 = fetch(c, t, L, int3(b.x, b.y, b.z));
    const float sx = 1.0f - tt.x, sy = 1.0f - tt.y, sz = 1.0f - tt.z;
    const float c00 = c000 * sx + c100 * tt.x, c10 = c010 * sx + c110 * tt.x;
    const float c01 = c001 * sx + c101 * tt.x, c11 = c011 * sx + c111 * tt.x;
    const float c0 = c00 * sy + c10 * tt.y, c1 = c01 * sy + c11 * tt.y;
    return c0 * sz + c1 * tt.z;
}
static inline float hw_linear(Ctx c, texture3d<float> t, uint L, float3 p) {
    const float3 d = pv3(c, L + LDimsF);
    return decode(c, L, t.sample(lin_s, (p + 0.5f) / d, level(0.0f)).x, false);
}
static inline float linear_s(Ctx c, texture3d<float> t, uint L, float3 p) {
    return flag(c, L + LFlags, kLayerManual) ? manual_linear(c, t, L, p) : hw_linear(c, t, L, p);
}
static inline bool inside(Ctx c, uint L, float3 p) {
    const float3 e = pv3(c, L + LEdge);
    return p.x >= -0.5f && p.x <= e.x && p.y >= -0.5f && p.y <= e.y && p.z >= -0.5f && p.z <= e.z;
}
static inline bool sample_s(Ctx c, texture3d<float> t, uint L, float3 p, bool lin, thread float& v) {
    v = 0.0f;
    if (!inside(c, L, p)) return false;
    v = lin ? linear_s(c, t, L, p) : nearest_s(c, t, L, p);
    return true;
}

struct Color {
    float3 rgb;
    bool below;
};
static inline float3 unpack(uint w) {
    return float3(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u));
}
static inline Color color_of(Ctx c, uint L, float v) {
    Color r;
    const uint lut = pu(c, L + LLut);
    const bool integral = v == floor(v) && v >= -32768.0f && v <= 32767.0f;
    if (lut != kNone && integral) {
        const uint w = du(c, lut + uint(int(v) + 32768));
        r.rgb = unpack(w);
        r.below = ((w >> 24) & 1u) != 0u;
    } else {
        const float t = v * pf(c, L + LWinA) + pf(c, L + LWinB);
        const float tc = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
        const uint gray = min(uint(tc * 255.0f + 0.5f), 255u);
        r.rgb = unpack(du(c, pu(c, L + LCmap) + gray));
        r.below = !(t > 0.0f);
    }
    return r;
}

static inline bool layer_value(Ctx c, texture3d<float> t, uint L, float fx, float fy, bool lin, thread float& out) {
    const float3 p = pv3(c, L + LS0) + pv3(c, L + LDU) * fx + pv3(c, L + LDV) * fy;
    const uint count = pu(c, SlabCount);
    if (count == 0u) return sample_s(c, t, L, p, lin, out);
    const uint mode = pu(c, SlabMode);
    const float3 dn = pv3(c, L + LDN);
    const uint off0 = pu(c, SlabOffsets);
    bool have = false;
    float acc = 0.0f, cnt = 0.0f;
    for (uint i = 0; i < count; ++i) {
        const float off = df(c, off0 + i);
        float v;
        if (sample_s(c, t, L, p + dn * off, lin, v)) {
            if (have) acc = mode == 1u ? fmax(acc, v) : (mode == 2u ? fmin(acc, v) : acc + v);
            else { acc = v; have = true; }
            cnt += 1.0f;
        }
    }
    if (mode == 3u && have) acc = acc / cnt;
    out = acc;
    return have;
}

static inline float3 background(Ctx c) { return float3(pf(c, BgR), pf(c, BgG), pf(c, BgB)); }

static inline float3 mpr(Ctx c, texture3d<float> t0, texture3d<float> t1, float fx, float fy) {
    const bool lin = flag(c, Flags, kFlagLinear);
    float3 rgb = background(c);
    float bv;
    if (layer_value(c, t0, L0, fx, fy, lin, bv)) rgb = color_of(c, L0, bv).rgb;
    if (flag(c, Flags, kFlagOverlay)) {
        float ov;
        if (layer_value(c, t1, L1, fx, fy, lin, ov)) {
            const Color col = color_of(c, L1, ov);
            const bool transparent = pu(c, L1 + LTransparent) != 0u;
            if (!(transparent && col.below)) {
                const float a = pf(c, L1 + LOpacity);
                rgb = rgb * (1.0f - a) + col.rgb * a;
            }
        }
    }
    return rgb;
}

struct Ray {
    float3 s, dir;
    bool hit;
    int k0, k1;
};
static inline Ray make_ray(Ctx c, float fx, float fy) {
    Ray R;
    R.s = pv3(c, L0 + LS0) + pv3(c, L0 + LDU) * fx + pv3(c, L0 + LDV) * fy;
    R.dir = pv3(c, L0 + LDN);
    const float3 e = pv3(c, L0 + LEdge);
    float t0 = -as_type<float>(0x7f800000u), t1 = as_type<float>(0x7f800000u);
    bool hit = true;
    for (int a = 0; a < 3; ++a) {
        const float s = R.s[a], d = R.dir[a], hi = e[a];
        const bool par = fabs(d) < 1e-30f;
        if (par) {
            if (s < -0.5f || s > hi) hit = false;
            continue;
        }
        const float ta = (-0.5f - s) / d, tb = (hi - s) / d;
        t0 = fmax(t0, fmin(ta, tb));
        t1 = fmin(t1, fmax(ta, tb));
    }
    hit = hit && t0 <= t1 && !isinf(t0) && !isinf(t1);
    const float step = pf(c, Step);
    const float kf0 = ceil(t0 / step), kf1 = floor(t1 / step);
    hit = hit && kf1 >= kf0 && kf1 - kf0 <= 1e7f;
    R.k0 = int(clamp(kf0, -1e9f, 1e9f));
    R.k1 = int(clamp(kf1, -1e9f, 1e9f));
    R.hit = hit;
    return R;
}

struct Brick {
    uint idx[3];
    uint b;
};
static inline Brick brick_of(Ctx c, float3 p) {
    Brick br;
    const float inv = pf(c, BrickInv);
    for (int a = 0; a < 3; ++a) {
        const float fa = p[a] * inv;
        const uint last = pu(c, BrickNb + uint(a)) - 1u;
        br.idx[a] = fa <= 0.0f ? 0u : (fa >= float(last) ? last : min(uint(fa), last));
    }
    br.b = (br.idx[2] * pu(c, BrickNb + 1u) + br.idx[1]) * pu(c, BrickNb) + br.idx[0];
    return br;
}
static inline int next_k(Ctx c, Brick br, float3 p, float t, float3 dir, int k, int k1) {
    const float B = pf(c, BrickSize);
    float te = as_type<float>(0x7f800000u);
    for (int a = 0; a < 3; ++a) {
        const float da = dir[a];
        if (fabs(da) < 1e-30f) continue;
        const uint nb = pu(c, BrickNb + uint(a));
        const float lo = br.idx[a] == 0u ? -1e30f : float(br.idx[a]) * B;
        const float hi = br.idx[a] + 1u == nb ? 1e30f : float(br.idx[a] + 1u) * B;
        te = fmin(te, t + ((da > 0.0f ? hi : lo) - p[a]) / da);
    }
    const float nk = ceil(te / pf(c, Step));
    const int target = nk > float(k1) ? k1 + 1 : int(clamp(nk, -1e9f, 1e9f));
    return max(k + 1, target);
}

static inline float piecewise_gop(Ctx c, float x) {
    const uint cnt = pu(c, GopCount), off = pu(c, GopOffset);
    const uint last = (cnt - 1u) * 2u;
    if (!(x > df(c, off))) return df(c, off + 1u);
    if (!(x < df(c, off + last))) return df(c, off + last + 1u);
    for (uint i = 1; i < cnt; ++i) {
        const uint at = off + i * 2u;
        const float vi = df(c, at);
        if (x <= vi) {
            const float vp = df(c, at - 2u), yp = df(c, at - 1u), yi = df(c, at + 1u);
            const float span = vi - vp;
            const float fr = span > 0.0f ? (x - vp) / span : 1.0f;
            return yp + (yi - yp) * fr;
        }
    }
    return df(c, off + last + 1u);
}

static inline float3 composite(Ctx c, texture3d<float> t0, float fx, float fy) {
    const Ray R = make_ray(c, fx, fy);
    float3 C = float3(0.0f);
    float A = 0.0f;
    if (R.hit) {
        const float step = pf(c, Step), ert = pf(c, Ert);
        const bool skip = flag(c, Flags, kFlagSkip), grad = flag(c, Flags, kFlagGradient);
        const bool shading = flag(c, Flags, kFlagShading), gop = flag(c, Flags, kFlagGradOpacity);
        const float lo = pf(c, TfLo), scale = pf(c, TfScale);
        const float emax = float(pu(c, TfEntries) - 1u);
        int k = R.k0;
        while (k <= R.k1 && A < ert) {
            const float t = float(k) * step;
            const float3 p = R.s + R.dir * t;
            if (skip) {
                const Brick br = brick_of(c, p);
                const uint word = du(c, pu(c, BrickEmpty) + (br.b >> 5));
                if (((word >> (br.b & 31u)) & 1u) != 0u) {
                    k = next_k(c, br, p, t, R.dir, k, R.k1);
                    continue;
                }
            }
            k = k + 1;
            const float x = linear_s(c, t0, L0, p);
            const uint e = uint(fmin(fmax((x - lo) * scale + 0.5f, 0.0f), emax));
            float a = df(c, pu(c, TfA) + e);
            if (!(a > 0.0f)) continue;
            float3 col = float3(df(c, pu(c, TfR) + e), df(c, pu(c, TfG) + e), df(c, pu(c, TfB) + e));
            if (grad) {
                const float3 ex = float3(1.0f, 0.0f, 0.0f), ey = float3(0.0f, 1.0f, 0.0f), ez = float3(0.0f, 0.0f, 1.0f);
                const float3 gx = float3(linear_s(c, t0, L0, p + ex) - linear_s(c, t0, L0, p - ex),
                                         linear_s(c, t0, L0, p + ey) - linear_s(c, t0, L0, p - ey),
                                         linear_s(c, t0, L0, p + ez) - linear_s(c, t0, L0, p - ez)) * 0.5f;
                const float3 G = float3(dot(pv3(c, Gi), gx), dot(pv3(c, Gi + 3u), gx), dot(pv3(c, Gi + 6u), gx));
                const float gm = sqrt(dot(G, G));
                if (gop) a *= clamp01(piecewise_gop(c, gm));
                if (shading && gm > 1e-12f && isfinite(gm)) {
                    const float ndl = fabs(dot(G, pv3(c, Normal))) / gm;
                    const float shin = pf(c, Shininess);
                    const float pw = shin == 0.0f ? 1.0f : (ndl > 0.0f ? pow(fmax(ndl, 1e-30f), shin) : 0.0f);
                    const float spec = pf(c, Specular) * pw;
                    const float lit = pf(c, Ambient) + pf(c, Diffuse) * ndl;
                    col = float3(clamp01(col.x * lit + spec), clamp01(col.y * lit + spec), clamp01(col.z * lit + spec));
                }
            }
            const float w = (1.0f - A) * a;
            C += w * col;
            A += w;
        }
    }
    const float rest = 1.0f - A;
    const float3 bg = background(c);
    return float3(clamp01(C.x + rest * (bg.x * (1.0f / 255.0f))), clamp01(C.y + rest * (bg.y * (1.0f / 255.0f))),
                  clamp01(C.z + rest * (bg.z * (1.0f / 255.0f)))) * 255.0f;
}

static inline float3 mip(Ctx c, texture3d<float> t0, float fx, float fy) {
    const Ray R = make_ray(c, fx, fy);
    const bool minip = flag(c, Flags, kFlagMinIp);
    float best = minip ? as_type<float>(0x7f800000u) : -as_type<float>(0x7f800000u);
    bool any = false;
    if (R.hit) {
        const float step = pf(c, Step);
        const bool skip = flag(c, Flags, kFlagSkip);
        int k = R.k0;
        while (k <= R.k1) {
            const float t = float(k) * step;
            const float3 p = R.s + R.dir * t;
            if (skip && any) {
                const Brick br = brick_of(c, p);
                const float mn = df(c, pu(c, BrickMin) + br.b), mx = df(c, pu(c, BrickMax) + br.b);
                const bool useless = !(mn <= mx) || (minip ? mn >= best : mx <= best);
                if (useless) {
                    k = next_k(c, br, p, t, R.dir, k, R.k1);
                    continue;
                }
            }
            const float x = linear_s(c, t0, L0, p);
            if (minip ? x < best : x > best) {
                best = x;
                any = true;
            }
            k = k + 1;
        }
    }
    return any ? color_of(c, L0, best).rgb : background(c);
}

static inline uint pack(float3 rgb) {
    const uint r = uint(rgb.x + 0.5f), g = uint(rgb.y + 0.5f), b = uint(rgb.z + 0.5f);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

kernel void k_mpr(device const uint* P [[buffer(0)]], device const uint* D [[buffer(1)]], device uint* out [[buffer(2)]],
                  texture3d<float> t0 [[texture(0)]], texture3d<float> t1 [[texture(1)]],
                  uint2 gid [[thread_position_in_grid]]) {
    const Ctx c = {P, D};
    if (gid.x >= P[Width] || gid.y >= P[Height]) return;
    out[gid.y * P[Width] + gid.x] = pack(mpr(c, t0, t1, float(gid.x), float(gid.y)));
}
kernel void k_composite(device const uint* P [[buffer(0)]], device const uint* D [[buffer(1)]], device uint* out [[buffer(2)]],
                        texture3d<float> t0 [[texture(0)]], texture3d<float> t1 [[texture(1)]],
                        uint2 gid [[thread_position_in_grid]]) {
    const Ctx c = {P, D};
    if (gid.x >= P[Width] || gid.y >= P[Height]) return;
    out[gid.y * P[Width] + gid.x] = pack(composite(c, t0, float(gid.x), float(gid.y)));
}
kernel void k_mip(device const uint* P [[buffer(0)]], device const uint* D [[buffer(1)]], device uint* out [[buffer(2)]],
                  texture3d<float> t0 [[texture(0)]], texture3d<float> t1 [[texture(1)]],
                  uint2 gid [[thread_position_in_grid]]) {
    const Ctx c = {P, D};
    if (gid.x >= P[Width] || gid.y >= P[Height]) return;
    out[gid.y * P[Width] + gid.x] = pack(mip(c, t0, float(gid.x), float(gid.y)));
}
)MSL";

// The parameter layout as MSL #defines (from gpu_params.hpp, so both sides cannot drift),
// placed after the standard library include.
std::string msl_source() {
    namespace P = param;
    std::string s = kMslHead;
    auto def = [&](const char* name, std::uint32_t v) { s += std::string("#define ") + name + " " + std::to_string(v) + "u\n"; };
    def("kNone", P::kNone);
    def("kFlagLinear", P::kFlagLinear);
    def("kFlagOverlay", P::kFlagOverlay);
    def("kFlagMinIp", P::kFlagMinIp);
    def("kFlagSkip", P::kFlagSkip);
    def("kFlagGradient", P::kFlagGradient);
    def("kFlagShading", P::kFlagShading);
    def("kFlagGradOpacity", P::kFlagGradOpacity);
    def("kLayerManual", P::kLayerManual);
    def("kLayerInteger", P::kLayerInteger);
    def("LS0", P::LS0);
    def("LDU", P::LDU);
    def("LDV", P::LDV);
    def("LDN", P::LDN);
    def("LDimsF", P::LDimsF);
    def("LEdge", P::LEdge);
    def("LDimsU", P::LDimsU);
    def("LScale", P::LScale);
    def("LBias", P::LBias);
    def("LWinA", P::LWinA);
    def("LWinB", P::LWinB);
    def("LLut", P::LLut);
    def("LCmap", P::LCmap);
    def("LOpacity", P::LOpacity);
    def("LTransparent", P::LTransparent);
    def("LFlags", P::LFlags);
    def("Width", P::Width);
    def("Height", P::Height);
    def("Flags", P::Flags);
    def("BgR", P::BgR);
    def("BgG", P::BgG);
    def("BgB", P::BgB);
    def("SlabCount", P::SlabCount);
    def("SlabMode", P::SlabMode);
    def("SlabOffsets", P::SlabOffsets);
    def("L0", P::L0);
    def("L1", P::L1);
    def("Step", P::Step);
    def("Ert", P::Ert);
    def("TfLo", P::TfLo);
    def("TfScale", P::TfScale);
    def("TfEntries", P::TfEntries);
    def("TfR", P::TfR);
    def("TfG", P::TfG);
    def("TfB", P::TfB);
    def("TfA", P::TfA);
    def("Ambient", P::Ambient);
    def("Diffuse", P::Diffuse);
    def("Specular", P::Specular);
    def("Shininess", P::Shininess);
    def("Gi", P::Gi);
    def("Normal", P::Normal);
    def("GopOffset", P::GopOffset);
    def("GopCount", P::GopCount);
    def("BrickSize", P::BrickSize);
    def("BrickInv", P::BrickInv);
    def("BrickNb", P::BrickNb);
    def("BrickEmpty", P::BrickEmpty);
    def("BrickMin", P::BrickMin);
    def("BrickMax", P::BrickMax);
    return s + kMsl;
}

// ---------------------------------------------------------------- devices
struct Dev {
    id device = nullptr;  // retained
    DeviceInfo info;
    bool float_filter = false;
};

struct Probed {
    std::vector<Dev> devs;
    ~Probed() {
        for (auto& d : devs) drop(d.device);
    }
};

std::mutex& probe_mutex() {
    static std::mutex m;
    return m;
}
std::shared_ptr<Probed>& probed() {
    static std::shared_ptr<Probed>* p = new std::shared_ptr<Probed>();  // never destroyed
    return *p;
}

DeviceInfo describe(id dev, bool& float_filter) {
    DeviceInfo d;
    d.name = to_string(call<id>(dev, "name"));
    d.api = Api::Metal;
    const bool unified = responds(dev, "hasUnifiedMemory") && call<BOOL>(dev, "hasUnifiedMemory");
    const bool low_power = responds(dev, "isLowPower") && call<BOOL>(dev, "isLowPower");
    d.type = (unified || low_power) ? DeviceType::Integrated : DeviceType::Discrete;
    if (responds(dev, "recommendedMaxWorkingSetSize")) d.memory_bytes = call<std::uint64_t>(dev, "recommendedMaxWorkingSetSize");
    if (responds(dev, "registryID")) d.device_id = static_cast<std::uint32_t>(call<std::uint64_t>(dev, "registryID"));
    d.max_texture_3d = 2048;  // every Metal GPU family (Metal feature set tables)
    float_filter = responds(dev, "supports32BitFloatFiltering") && call<BOOL>(dev, "supports32BitFloatFiltering");
    d.float32_filtering = float_filter;
    d.int16_filtering = true;
    d.driver = "Metal";
    return d;
}

// ---------------------------------------------------------------- backend
class MtlContext;

class MtlTexture final : public Texture {
public:
    explicit MtlTexture(std::shared_ptr<MtlContext> c) : ctx(std::move(c)) {}
    ~MtlTexture() override {
        if (tex) {
            drop(tex);
            detail::count_object(-1);
        }
    }
    MtlTexture(const MtlTexture&) = delete;
    MtlTexture& operator=(const MtlTexture&) = delete;
    std::shared_ptr<MtlContext> ctx;
    id tex = nullptr;
};

class MtlContext final : public Backend, public std::enable_shared_from_this<MtlContext> {
public:
    MtlContext() = default;
    MtlContext(const MtlContext&) = delete;
    MtlContext& operator=(const MtlContext&) = delete;
    ~MtlContext() override {
        for (auto& p : pipelines)
            if (p) {
                drop(p);
                detail::count_object(-1);
            }
        if (queue) {
            drop(queue);
            detail::count_object(-1);
        }
        drop(device);
    }
    Status init(const Dev& d);
    const DeviceInfo& info() const override { return info_; }
    Result<std::shared_ptr<const Texture>> texture(const std::shared_ptr<const volume::Volume>& v) override;
    Result<std::unique_ptr<Session>> session() override;
    std::uint64_t max_buffer_bytes() const override { return max_buffer_; }
    bool lost() const override { return lost_; }
    // Commits a command buffer and waits for it; false (and lost) on a GPU error.
    Status commit_wait(id cb);

    id device = nullptr;
    id queue = nullptr;
    id pipelines[param::KernelCount] = {nullptr, nullptr, nullptr};
    std::mutex queue_mu;

private:
    Result<std::shared_ptr<MtlTexture>> upload(const volume::Volume& v);
    DeviceInfo info_;
    bool float_filter_ = false;
    std::uint64_t max_buffer_ = 256ull << 20;
    bool lost_ = false;
    std::mutex cache_mu_;
    std::vector<std::pair<std::weak_ptr<const volume::Volume>, std::weak_ptr<const Texture>>> cache_;
};

Status MtlContext::init(const Dev& d) {
    Pool pool;
    device = d.device;
    call<id>(device, "retain");
    info_ = d.info;
    float_filter_ = d.float_filter;
    if (responds(device, "maxBufferLength")) max_buffer_ = call<NSUInteger>(device, "maxBufferLength");
    queue = call<id>(device, "newCommandQueue");
    if (!queue) return Status(Errc::Unsupported, "metal: newCommandQueue failed");
    detail::count_object(1);
    id opts = call<id>(call<id>(cls("MTLCompileOptions"), "alloc"), "init");
    if (!opts) return Status(Errc::OutOfMemory, "metal: out of memory");
    // IEEE semantics (no fast math), as in the Vulkan kernels.
    if (responds(opts, "setMathMode:")) call<void>(opts, "setMathMode:", kMathModeSafe);
    else call<void>(opts, "setFastMathEnabled:", static_cast<BOOL>(0));
    std::string src;
    try {
        src = msl_source();
    } catch (const std::bad_alloc&) {
        drop(opts);
        return Status(Errc::OutOfMemory, "metal: out of memory");
    }
    id source = make_string(src.c_str());
    id err = nullptr;
    id lib = call<id>(device, "newLibraryWithSource:options:error:", source, opts, &err);
    drop(source);
    drop(opts);
    if (!lib) return Status(Errc::Internal, "metal: kernel compilation failed: " + error_text(err));
    const char* names[param::KernelCount] = {"k_mpr", "k_composite", "k_mip"};
    Status st = Status::ok();
    for (std::uint32_t k = 0; k < param::KernelCount && st; ++k) {
        id name = make_string(names[k]);
        id fn = call<id>(lib, "newFunctionWithName:", name);
        drop(name);
        if (!fn) {
            st = Status(Errc::Internal, std::string("metal: kernel ") + names[k] + " missing");
            break;
        }
        id perr = nullptr;
        pipelines[k] = call<id>(device, "newComputePipelineStateWithFunction:error:", fn, &perr);
        drop(fn);
        if (!pipelines[k]) {
            st = Status(Errc::Internal, "metal: pipeline creation failed: " + error_text(perr));
            break;
        }
        detail::count_object(1);
        if (call<NSUInteger>(pipelines[k], "maxTotalThreadsPerThreadgroup") < param::kGroupX * param::kGroupY)
            st = Status(Errc::Unsupported, "metal: the kernels need 64 threads per threadgroup");
    }
    drop(lib);
    return st;
}

Status MtlContext::commit_wait(id cb) {
    call<void>(cb, "commit");
    call<void>(cb, "waitUntilCompleted");
    if (call<NSUInteger>(cb, "status") != kCommandBufferStatusCompleted) {
        lost_ = true;
        return Status(Errc::Internal, "metal: command buffer failed: " + error_text(call<id>(cb, "error")));
    }
    return Status::ok();
}

Result<std::shared_ptr<MtlTexture>> MtlContext::upload(const volume::Volume& vol) {
    Pool pool;
    const auto& g = vol.grid();
    const std::uint32_t nx = g.dims[0], ny = g.dims[1], nz = g.dims[2];
    if (nx > info_.max_texture_3d || ny > info_.max_texture_3d || nz > info_.max_texture_3d)
        return Status(Errc::Unsupported, "metal: the volume exceeds the 3D texture limit");
    const bool i16 = vol.type() == volume::VoxelType::Int16;
    const std::uint64_t texel = i16 ? 2u : 4u;
    const std::uint64_t row = std::uint64_t{nx} * texel, slice = row * ny;
    auto tex = std::make_shared<MtlTexture>(shared_from_this());
    tex->dims[0] = nx;
    tex->dims[1] = ny;
    tex->dims[2] = nz;
    tex->encoding = i16 ? TextureEncoding{65535.0f, -32768.0f, true, true} : TextureEncoding{1.0f, 0.0f, false, float_filter_};
    id desc = call<id>(call<id>(cls("MTLTextureDescriptor"), "alloc"), "init");
    if (!desc) return Status(Errc::OutOfMemory, "metal: out of memory");
    call<void>(desc, "setTextureType:", kTextureType3D);
    call<void>(desc, "setPixelFormat:", i16 ? kPixelFormatR16Unorm : kPixelFormatR32Float);
    call<void>(desc, "setWidth:", static_cast<NSUInteger>(nx));
    call<void>(desc, "setHeight:", static_cast<NSUInteger>(ny));
    call<void>(desc, "setDepth:", static_cast<NSUInteger>(nz));
    call<void>(desc, "setUsage:", kTextureUsageShaderRead);
    call<void>(desc, "setStorageMode:", kStorageModePrivate);
    tex->tex = call<id>(device, "newTextureWithDescriptor:", desc);
    drop(desc);
    if (!tex->tex) return Status(Errc::OutOfMemory, "metal: newTextureWithDescriptor failed");
    detail::count_object(1);
    const std::uint64_t per = std::max<std::uint64_t>(1, detail::upload_chunk_bytes() / slice);
    const std::uint64_t chunk = std::min<std::uint64_t>(per, nz);
    std::uint64_t staging_bytes = 0;
    if (!checked_mul(chunk, slice, staging_bytes) || staging_bytes > max_buffer_)
        return Status(Errc::LimitExceeded, "metal: volume slice too large");
    id staging = call<id>(device, "newBufferWithLength:options:", static_cast<NSUInteger>(staging_bytes), kResourceStorageModeShared);
    if (!staging) return Status(Errc::OutOfMemory, "metal: staging buffer allocation failed");
    Status st = Status::ok();
    const std::size_t plane = static_cast<std::size_t>(nx) * ny;
    for (std::uint32_t z0 = 0; z0 < nz && st;) {
        const std::uint32_t z1 = static_cast<std::uint32_t>(std::min<std::uint64_t>(nz, z0 + chunk));
        void* dst = call<void*>(staging, "contents");
        const std::size_t count = plane * (z1 - z0), first = plane * z0;
        if (i16) {
            auto* d16 = static_cast<std::uint16_t*>(dst);
            const std::int16_t* src = vol.i16() + first;
            for (std::size_t i = 0; i < count; ++i) d16[i] = static_cast<std::uint16_t>(static_cast<std::int32_t>(src[i]) + 32768);
        } else {
            std::memcpy(dst, vol.f32() + first, count * sizeof(float));
        }
        Pool inner;
        id cb = call<id>(queue, "commandBuffer");
        id blit = cb ? call<id>(cb, "blitCommandEncoder") : nullptr;
        if (!blit) {
            st = Status(Errc::Internal, "metal: no blit encoder");
            break;
        }
        const MTLSize size{nx, ny, z1 - z0};
        const MTLOrigin origin{0, 0, z0};
        call<void>(blit, "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:"
                         "destinationLevel:destinationOrigin:",
                   staging, static_cast<NSUInteger>(0), static_cast<NSUInteger>(row), static_cast<NSUInteger>(slice), size, tex->tex,
                   static_cast<NSUInteger>(0), static_cast<NSUInteger>(0), origin);
        call<void>(blit, "endEncoding");
        {
            std::lock_guard<std::mutex> lock(queue_mu);
            st = commit_wait(cb);
        }
        z0 = z1;
    }
    drop(staging);
    if (!st) return st;
    return tex;
}

Result<std::shared_ptr<const Texture>> MtlContext::texture(const std::shared_ptr<const volume::Volume>& v) {
    if (!v) return Status(Errc::InvalidArgument, "metal: no volume");
    if (lost_) return Status(Errc::Unsupported, "metal: the device failed");
    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            auto vol = it->first.lock();
            auto tex = it->second.lock();
            if (!vol || !tex) {
                it = cache_.erase(it);
                continue;
            }
            if (vol == v) return std::shared_ptr<const Texture>(tex);
            ++it;
        }
    }
    auto t = upload(*v);
    if (!t) return t.take_error();
    std::shared_ptr<const Texture> tex = std::move(t).value();
    std::lock_guard<std::mutex> lock(cache_mu_);
    cache_.emplace_back(v, tex);
    return tex;
}

class MtlSession final : public Session {
public:
    explicit MtlSession(std::shared_ptr<MtlContext> c) : ctx_(std::move(c)) {}
    MtlSession(const MtlSession&) = delete;
    MtlSession& operator=(const MtlSession&) = delete;
    ~MtlSession() override {
        for (id* b : {&params_, &data_, &out_})
            if (*b) {
                drop(*b);
                detail::count_object(-1);
            }
    }
    Status run(param::Kernel k, std::uint32_t width, std::uint32_t height, std::span<const std::uint32_t> params,
               std::span<const std::uint32_t> data, const Texture& base, const Texture* overlay,
               std::span<std::uint8_t> rgba) override;

private:
    Status ensure(id& b, NSUInteger& cap, std::uint64_t need) {
        if (b && cap >= need) return Status::ok();
        if (b) {
            drop(b);
            detail::count_object(-1);
            b = nullptr;
        }
        const std::uint64_t size = need + need / 4 + 256;
        if (size > ctx_->max_buffer_bytes()) return Status(Errc::Unsupported, "metal: buffer exceeds the device limit");
        b = call<id>(ctx_->device, "newBufferWithLength:options:", static_cast<NSUInteger>(size), kResourceStorageModeShared);
        if (!b) return Status(Errc::OutOfMemory, "metal: buffer allocation failed");
        detail::count_object(1);
        cap = static_cast<NSUInteger>(size);
        return Status::ok();
    }
    std::shared_ptr<MtlContext> ctx_;
    id params_ = nullptr, data_ = nullptr, out_ = nullptr;
    NSUInteger pcap_ = 0, dcap_ = 0, ocap_ = 0;
};

Status MtlSession::run(param::Kernel k, std::uint32_t width, std::uint32_t height, std::span<const std::uint32_t> params,
                       std::span<const std::uint32_t> data, const Texture& base, const Texture* overlay,
                       std::span<std::uint8_t> rgba) {
    MtlContext& c = *ctx_;
    if (c.lost()) return Status(Errc::Unsupported, "metal: the device failed");
    if (k >= param::KernelCount || width == 0 || height == 0) return Status(Errc::InvalidArgument, "metal: bad dispatch");
    const std::uint64_t pixels = std::uint64_t{width} * height;
    if (rgba.size() < pixels * 4u) return Status(Errc::BufferTooSmall, "metal: output too small");
    Pool pool;
    if (Status s = ensure(params_, pcap_, params.size() * 4u); !s) return s;
    if (Status s = ensure(data_, dcap_, std::max<std::size_t>(data.size(), 4) * 4u); !s) return s;
    if (Status s = ensure(out_, ocap_, pixels * 4u); !s) return s;
    std::memcpy(call<void*>(params_, "contents"), params.data(), params.size() * 4u);
    if (!data.empty()) std::memcpy(call<void*>(data_, "contents"), data.data(), data.size() * 4u);
    const auto& tb = static_cast<const MtlTexture&>(base);
    const auto& to = overlay ? static_cast<const MtlTexture&>(*overlay) : tb;
    id cb = call<id>(c.queue, "commandBuffer");
    id enc = cb ? call<id>(cb, "computeCommandEncoder") : nullptr;
    if (!enc) return Status(Errc::Internal, "metal: no compute encoder");
    call<void>(enc, "setComputePipelineState:", c.pipelines[k]);
    call<void>(enc, "setBuffer:offset:atIndex:", params_, static_cast<NSUInteger>(0), static_cast<NSUInteger>(0));
    call<void>(enc, "setBuffer:offset:atIndex:", data_, static_cast<NSUInteger>(0), static_cast<NSUInteger>(1));
    call<void>(enc, "setBuffer:offset:atIndex:", out_, static_cast<NSUInteger>(0), static_cast<NSUInteger>(2));
    call<void>(enc, "setTexture:atIndex:", tb.tex, static_cast<NSUInteger>(0));
    call<void>(enc, "setTexture:atIndex:", to.tex, static_cast<NSUInteger>(1));
    const MTLSize groups{(width + param::kGroupX - 1) / param::kGroupX, (height + param::kGroupY - 1) / param::kGroupY, 1};
    const MTLSize threads{param::kGroupX, param::kGroupY, 1};
    call<void>(enc, "dispatchThreadgroups:threadsPerThreadgroup:", groups, threads);
    call<void>(enc, "endEncoding");
    {
        std::lock_guard<std::mutex> lock(c.queue_mu);
        if (Status s = c.commit_wait(cb); !s) return s;
    }
    const auto* px = static_cast<const std::uint32_t*>(call<void*>(out_, "contents"));
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t v = px[i];
        rgba[4 * i] = static_cast<std::uint8_t>(v);
        rgba[4 * i + 1] = static_cast<std::uint8_t>(v >> 8);
        rgba[4 * i + 2] = static_cast<std::uint8_t>(v >> 16);
        rgba[4 * i + 3] = static_cast<std::uint8_t>(v >> 24);
    }
    return Status::ok();
}

Result<std::unique_ptr<Session>> MtlContext::session() {
    if (lost_) return Status(Errc::Unsupported, "metal: the device failed");
    try {
        return std::unique_ptr<Session>(std::make_unique<MtlSession>(shared_from_this()));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "metal: out of memory");
    }
}

}  // namespace

detail::Probe probe() {
    detail::Probe pr;
    std::lock_guard<std::mutex> lock(probe_mutex());
    probed().reset();
    try {
        Pool pool;
        auto p = std::make_shared<Probed>();
        id all = MTLCopyAllDevices();
        if (all) {
            const NSUInteger n = call<NSUInteger>(all, "count");
            for (NSUInteger i = 0; i < n; ++i) {
                id dev = call<id>(all, "objectAtIndex:", i);
                if (!dev) continue;
                Dev d;
                d.device = call<id>(dev, "retain");
                d.info = describe(dev, d.float_filter);
                p->devs.push_back(d);
            }
            drop(all);
        } else if (id dev = MTLCreateSystemDefaultDevice()) {
            Dev d;
            d.device = dev;  // returned retained
            d.info = describe(dev, d.float_filter);
            p->devs.push_back(d);
        }
        for (const auto& d : p->devs) pr.devices.push_back(d.info);
        if (pr.devices.empty()) pr.reason = "metal: no Metal device";
        probed() = std::move(p);
    } catch (const std::bad_alloc&) {
        pr.devices.clear();
        pr.reason = "metal: out of memory";
    }
    return pr;
}

Result<std::shared_ptr<Backend>> create(std::size_t index) {
    std::shared_ptr<Probed> p;
    {
        std::lock_guard<std::mutex> lock(probe_mutex());
        p = probed();
    }
    if (!p || index >= p->devs.size()) return Status(Errc::Unsupported, "metal: not probed");
    try {
        auto c = std::make_shared<MtlContext>();
        if (Status s = c->init(p->devs[index]); !s) return s;
        return std::shared_ptr<Backend>(std::move(c));
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "metal: out of memory");
    }
}

void release() {
    std::lock_guard<std::mutex> lock(probe_mutex());
    probed().reset();
}

}  // namespace siaara::gpu::metal

#endif  // __APPLE__ || SIAARA_METAL_COMPILE_CHECK
