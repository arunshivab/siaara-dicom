// SIAARA DICOM Toolkit - Vulkan compute kernels as SPIR-V (see spirv_kernels.hpp).
//
// The arithmetic follows render3d.cpp operation by operation (in float instead of double):
//   - MPR: pixel (x, y) is at S0 + x * DU + y * DV in voxel index space; inside means every
//     coordinate in [-0.5, n - 0.5]; nearest = voxel floor(c + 0.5) (clamped); trilinear either
//     from the hardware filter or from 8 fetches with the CPU's lerp order.
//   - slabs: samples at the host's offsets along the normal, combined by MIP / MinIP / average
//     over the samples that are inside.
//   - colour: int16 layers with integral values use the host's 65536-entry LUT (computed in double
//     exactly like the CPU); other values use the window formula in float and the 256-entry map.
//   - ray casting: the CPU's box clip, sample positions k * step, brick skipping, TF lookup, opacity,
//     gradient, Phong headlight and early termination.
#include "gpu/spirv_kernels.hpp"

#include <limits>

#include "gpu/spirv_builder.hpp"

namespace siaara::gpu::spirv {

namespace {

namespace P = param;

struct V3 {
    V x, y, z;
    V operator[](int a) const { return a == 0 ? x : (a == 1 ? y : z); }
};

class Gen {
public:
    explicit Gen(P::Kernel k) : kind_(k) {
        tf = m.t_f32();
        tu = m.t_u32();
        ti = m.t_i32();
        v3f = m.t_vec(tf, 3);
        v3i = m.t_vec(ti, 3);
        v2i = m.t_vec(ti, 2);
        v4u = m.t_vec(tu, 4);
        params = m.buffer_u32(0, 0, "params");
        data = m.buffer_u32(0, 1, "data");
        tex0 = m.sampled_image_3d(0, 2, "base");
        tex1 = m.sampled_image_3d(0, 3, "overlay");
        out = m.storage_image_2d_r32ui(0, 4, "out");
        gid = m.global_invocation_id();
        m.begin_main(P::kGroupX, P::kGroupY);
    }

    std::vector<std::uint32_t> build() {
        const V g = m.load_input_uvec3(gid);
        const V x = m.extract(tu, g, 0), y = m.extract(tu, g, 1);
        const V inb = m.land(m.lt(x, Pu(P::Width)), m.lt(y, Pu(P::Height)));
        m.if_(inb, [&] {
            const V fx = m.u2f(x), fy = m.u2f(y);
            V3 rgb;
            switch (kind_) {
                case P::KernelMpr: rgb = mpr(fx, fy); break;
                case P::KernelComposite: rgb = composite(fx, fy); break;
                default: rgb = mip(fx, fy); break;
            }
            const V half = f(0.5f);
            const V r = m.f2u(m.add(rgb.x, half)), gg = m.f2u(m.add(rgb.y, half)), b = m.f2u(m.add(rgb.z, half));
            const V packed = m.bor(m.bor(r, m.shl(gg, u(8))), m.bor(m.shl(b, u(16)), u(0xFF000000u)));
            const V coord = m.construct(v2i, {m.bitcast(ti, x), m.bitcast(ti, y)});
            m.image_write(out, coord, m.construct(v4u, {packed, packed, packed, packed}));
        });
        return m.finish();
    }

private:
    // ---- small helpers
    V f(float v) { return m.c_f32(v); }
    V u(std::uint32_t v) { return m.c_u32(v); }
    V i(std::int32_t v) { return m.c_i32(v); }
    V Pu(std::uint32_t w) { return m.buf_load(params, u(w)); }
    V Pf(std::uint32_t w) { return m.bitcast(tf, Pu(w)); }
    V Pi(std::uint32_t w) { return m.bitcast(ti, Pu(w)); }
    V3 Pv3(std::uint32_t w) { return {Pf(w), Pf(w + 1), Pf(w + 2)}; }
    V Du(V idx) { return m.buf_load(data, idx); }
    V Df(V idx) { return m.bitcast(tf, Du(idx)); }
    V flag(std::uint32_t word, std::uint32_t bit) { return m.ne(m.band(Pu(word), u(bit)), u(0)); }
    V3 add3(V3 a, V3 b) { return {m.add(a.x, b.x), m.add(a.y, b.y), m.add(a.z, b.z)}; }
    V3 sub3(V3 a, V3 b) { return {m.sub(a.x, b.x), m.sub(a.y, b.y), m.sub(a.z, b.z)}; }
    V3 scale3(V3 a, V s) { return {m.mul(a.x, s), m.mul(a.y, s), m.mul(a.z, s)}; }
    V dot3(V3 a, V3 b) { return m.add(m.add(m.mul(a.x, b.x), m.mul(a.y, b.y)), m.mul(a.z, b.z)); }
    V clamp01(V v) { return m.fmin(m.fmax(v, f(0.0f)), f(1.0f)); }

    struct Var {
        Id ptr = 0;
        Id type = 0;
    };
    Var var(V init) {
        Var v{m.var(init.type), init.type};
        m.store(v.ptr, init);
        return v;
    }
    V ld(const Var& v) { return m.load(v.ptr, v.type); }
    void st(const Var& v, V x) { m.store(v.ptr, x); }

    // ---- texel access
    // value = texel * scale + bias; integer layers round the scaled texel first (exact int16).
    V decode(std::uint32_t L, V texel, bool fetched) {
        const V s = m.mul(texel, Pf(L + P::LScale));
        if (!fetched) return m.add(s, Pf(L + P::LBias));
        const V integer = flag(L + P::LFlags, P::kLayerInteger);
        return m.add(m.select(integer, m.round(s), s), Pf(L + P::LBias));
    }
    V fetch(Id tex, std::uint32_t L, V ix, V iy, V iz) {
        const V t = m.fetch(tex, m.construct(v3i, {ix, iy, iz}));
        return decode(L, m.extract(tf, t, 0), true);
    }
    V clampi(V v, V n) { return m.sclamp(v, i(0), m.sub(n, i(1))); }
    V3 dims_i(std::uint32_t L) { return {Pi(L + P::LDimsU), Pi(L + P::LDimsU + 1), Pi(L + P::LDimsU + 2)}; }

    V nearest(Id tex, std::uint32_t L, V3 p) {
        const V3 n = dims_i(L);
        const V half = f(0.5f);
        const V ix = clampi(m.f2i(m.floor(m.add(p.x, half))), n.x);
        const V iy = clampi(m.f2i(m.floor(m.add(p.y, half))), n.y);
        const V iz = clampi(m.f2i(m.floor(m.add(p.z, half))), n.z);
        return fetch(tex, L, ix, iy, iz);
    }

    // Trilinear with edge clamping, in the CPU's order (sampler.hpp sample_linear).
    V manual_linear(Id tex, std::uint32_t L, V3 p) {
        const V3 n = dims_i(L);
        const V fx = m.floor(p.x), fy = m.floor(p.y), fz = m.floor(p.z);
        const V ix = m.f2i(fx), iy = m.f2i(fy), iz = m.f2i(fz);
        const V tx = m.sub(p.x, fx), ty = m.sub(p.y, fy), tz = m.sub(p.z, fz);
        const V x0 = clampi(ix, n.x), x1 = clampi(m.add(ix, i(1)), n.x);
        const V y0 = clampi(iy, n.y), y1 = clampi(m.add(iy, i(1)), n.y);
        const V z0 = clampi(iz, n.z), z1 = clampi(m.add(iz, i(1)), n.z);
        const V c000 = fetch(tex, L, x0, y0, z0), c100 = fetch(tex, L, x1, y0, z0);
        const V c010 = fetch(tex, L, x0, y1, z0), c110 = fetch(tex, L, x1, y1, z0);
        const V c001 = fetch(tex, L, x0, y0, z1), c101 = fetch(tex, L, x1, y0, z1);
        const V c011 = fetch(tex, L, x0, y1, z1), c111 = fetch(tex, L, x1, y1, z1);
        const V one = f(1.0f);
        const V sx = m.sub(one, tx), sy = m.sub(one, ty), sz = m.sub(one, tz);
        const V c00 = m.add(m.mul(c000, sx), m.mul(c100, tx)), c10 = m.add(m.mul(c010, sx), m.mul(c110, tx));
        const V c01 = m.add(m.mul(c001, sx), m.mul(c101, tx)), c11 = m.add(m.mul(c011, sx), m.mul(c111, tx));
        const V c0 = m.add(m.mul(c00, sy), m.mul(c10, ty)), c1 = m.add(m.mul(c01, sy), m.mul(c11, ty));
        return m.add(m.mul(c0, sz), m.mul(c1, tz));
    }

    V hw_linear(Id tex, std::uint32_t L, V3 p) {
        const V half = f(0.5f);
        const V3 d = Pv3(L + P::LDimsF);
        const V c = m.construct(v3f, {m.div(m.add(p.x, half), d.x), m.div(m.add(p.y, half), d.y), m.div(m.add(p.z, half), d.z)});
        const V t = m.sample_lod0(tex, c);
        return decode(L, m.extract(tf, t, 0), false);
    }

    V linear(Id tex, std::uint32_t L, V3 p) {
        Var r = var(f(0.0f));
        m.if_(flag(L + P::LFlags, P::kLayerManual), [&] { st(r, manual_linear(tex, L, p)); },
              [&] { st(r, hw_linear(tex, L, p)); });
        return ld(r);
    }

    V inside(std::uint32_t L, V3 p) {
        const V lo = f(-0.5f);
        const V3 e = Pv3(L + P::LEdge);
        V r = m.land(m.ge(p.x, lo), m.le(p.x, e.x));
        r = m.land(r, m.land(m.ge(p.y, lo), m.le(p.y, e.y)));
        return m.land(r, m.land(m.ge(p.z, lo), m.le(p.z, e.z)));
    }

    // (value, valid) of one MPR sample.
    std::pair<V, V> sample(Id tex, std::uint32_t L, V3 p, V lin) {
        const V ok = inside(L, p);
        Var v = var(f(0.0f));
        m.if_(ok, [&] {
            m.if_(lin, [&] { st(v, linear(tex, L, p)); }, [&] { st(v, nearest(tex, L, p)); });
        });
        return {ld(v), ok};
    }

    // ---- colour of a layer value: (r, g, b) in 0..255 and "below the window bottom".
    struct Color {
        V r, g, b, below;
    };
    Color color_of(std::uint32_t L, V v) {
        Var r = var(f(0.0f)), g = var(f(0.0f)), b = var(f(0.0f)), below = var(m.c_bool(false));
        const V lut = Pu(L + P::LLut);
        const V integral = m.land(m.eq(v, m.floor(v)), m.land(m.ge(v, f(-32768.0f)), m.le(v, f(32767.0f))));
        const V use_lut = m.land(m.ne(lut, u(P::kNone)), integral);
        auto unpack = [&](V w) {
            st(r, m.u2f(m.band(w, u(255))));
            st(g, m.u2f(m.band(m.shr(w, u(8)), u(255))));
            st(b, m.u2f(m.band(m.shr(w, u(16)), u(255))));
        };
        m.if_(use_lut,
              [&] {
                  const V e = m.bitcast(tu, m.add(m.f2i(v), i(32768)));
                  const V w = Du(m.add(lut, e));
                  unpack(w);
                  st(below, m.ne(m.band(m.shr(w, u(24)), u(1)), u(0)));
              },
              [&] {
                  const V t = m.add(m.mul(v, Pf(L + P::LWinA)), Pf(L + P::LWinB));
                  // CPU: v > 0 ? (v < 1 ? v : 1) : 0, then gray = (uint8)(t * 255 + 0.5).
                  const V tc = m.select(m.gt(t, f(0.0f)), m.select(m.lt(t, f(1.0f)), t, f(1.0f)), f(0.0f));
                  const V gray = m.f2u(m.add(m.mul(tc, f(255.0f)), f(0.5f)));
                  unpack(Du(m.add(Pu(L + P::LCmap), m.umin(gray, u(255)))));
                  st(below, m.lnot(m.gt(t, f(0.0f))));
              });
        return {ld(r), ld(g), ld(b), ld(below)};
    }

    // ---- MPR / slab / fusion
    std::pair<V, V> layer_value(Id tex, std::uint32_t L, V fx, V fy, V lin) {
        const V3 p = add3(add3(Pv3(L + P::LS0), scale3(Pv3(L + P::LDU), fx)), scale3(Pv3(L + P::LDV), fy));
        const V count = Pu(P::SlabCount);
        Var val = var(f(0.0f)), ok = var(m.c_bool(false));
        m.if_(m.eq(count, u(0)),
              [&] {
                  auto s = sample(tex, L, p, lin);
                  st(val, s.first);
                  st(ok, s.second);
              },
              [&] {
                  const V mode = Pu(P::SlabMode);
                  const V3 dn = Pv3(L + P::LDN);
                  const V off0 = Pu(P::SlabOffsets);
                  Var idx = var(u(0)), have = var(m.c_bool(false)), acc = var(f(0.0f)), cnt = var(f(0.0f));
                  m.while_([&] { return m.lt(ld(idx), count); },
                           [&] {
                               const V off = Df(m.add(off0, ld(idx)));
                               auto s = sample(tex, L, add3(p, scale3(dn, off)), lin);
                               m.if_(s.second, [&] {
                                   m.if_(ld(have),
                                         [&] {
                                             const V a = ld(acc);
                                             const V mx = m.fmax(a, s.first), mn = m.fmin(a, s.first), sum = m.add(a, s.first);
                                             st(acc, m.select(m.eq(mode, u(1)), mx, m.select(m.eq(mode, u(2)), mn, sum)));
                                         },
                                         [&] {
                                             st(acc, s.first);
                                             st(have, m.c_bool(true));
                                         });
                                   st(cnt, m.add(ld(cnt), f(1.0f)));
                               });
                               st(idx, m.add(ld(idx), u(1)));
                           });
                  m.if_(m.land(m.eq(mode, u(3)), ld(have)), [&] { st(acc, m.div(ld(acc), ld(cnt))); });
                  st(val, ld(acc));
                  st(ok, ld(have));
              });
        return {ld(val), ld(ok)};
    }

    V3 background() { return {Pf(P::BgR), Pf(P::BgG), Pf(P::BgB)}; }

    V3 mpr(V fx, V fy) {
        const V lin = flag(P::Flags, P::kFlagLinear);
        const V3 bg = background();
        Var r = var(bg.x), g = var(bg.y), b = var(bg.z);
        auto base = layer_value(tex0, P::L0, fx, fy, lin);
        m.if_(base.second, [&] {
            const Color c = color_of(P::L0, base.first);
            st(r, c.r);
            st(g, c.g);
            st(b, c.b);
        });
        m.if_(flag(P::Flags, P::kFlagOverlay), [&] {
            auto ov = layer_value(tex1, P::L1, fx, fy, lin);
            m.if_(ov.second, [&] {
                const Color c = color_of(P::L1, ov.first);
                const V transparent = m.ne(Pu(P::L1 + P::LTransparent), u(0));
                m.if_(m.lnot(m.land(transparent, c.below)), [&] {
                    const V a = Pf(P::L1 + P::LOpacity);
                    const V ia = m.sub(f(1.0f), a);
                    st(r, m.add(m.mul(ld(r), ia), m.mul(c.r, a)));
                    st(g, m.add(m.mul(ld(g), ia), m.mul(c.g, a)));
                    st(b, m.add(m.mul(ld(b), ia), m.mul(c.b, a)));
                });
            });
        });
        return {ld(r), ld(g), ld(b)};
    }

    // ---- ray casting
    struct Ray {
        V3 s, dir;
        V hit;
        V k0, k1;  // int
    };
    // Ray / box [-0.5, n - 0.5]^3 in index space and the sample range k0..k1 (render3d.cpp clip_ray).
    Ray ray(V fx, V fy) {
        Ray R;
        R.s = add3(add3(Pv3(P::L0 + P::LS0), scale3(Pv3(P::L0 + P::LDU), fx)), scale3(Pv3(P::L0 + P::LDV), fy));
        R.dir = Pv3(P::L0 + P::LDN);
        const V3 e = Pv3(P::L0 + P::LEdge);
        const V inf = f(std::numeric_limits<float>::infinity());
        V t0 = m.neg(inf), t1 = inf;
        V hit = m.c_bool(true);
        const V lo = f(-0.5f);
        for (int a = 0; a < 3; ++a) {
            const V s = R.s[a], d = R.dir[a], hi = e[a];
            const V par = m.lt(m.fabs(d), f(1e-30f));
            const V outside = m.lor(m.lt(s, lo), m.gt(s, hi));
            hit = m.land(hit, m.lnot(m.land(par, outside)));
            const V ta = m.div(m.sub(lo, s), d), tb = m.div(m.sub(hi, s), d);
            t0 = m.select(par, t0, m.fmax(t0, m.fmin(ta, tb)));
            t1 = m.select(par, t1, m.fmin(t1, m.fmax(ta, tb)));
        }
        hit = m.land(hit, m.land(m.le(t0, t1), m.lnot(m.lor(m.is_inf(t0), m.is_inf(t1)))));
        const V step = Pf(P::Step);
        const V kf0 = m.ceil(m.div(t0, step)), kf1 = m.floor(m.div(t1, step));
        hit = m.land(hit, m.land(m.ge(kf1, kf0), m.le(m.sub(kf1, kf0), f(1e7f))));
        // Conversions are clamped so that rays rejected above never convert out of range.
        const V lim = f(1e9f);
        R.k0 = m.f2i(m.fmin(m.fmax(kf0, m.neg(lim)), lim));
        R.k1 = m.f2i(m.fmin(m.fmax(kf1, m.neg(lim)), lim));
        R.hit = hit;
        return R;
    }

    // Brick of a point (index space) and the bit index.
    struct Brick {
        V idx[3];  // uint
        V b;       // uint
    };
    Brick brick_of(V3 p) {
        Brick br;
        const V inv = Pf(P::BrickInv);
        for (int a = 0; a < 3; ++a) {
            const auto ua = static_cast<std::uint32_t>(a);
            const V fa = m.mul(p[a], inv);
            const V last = m.sub(Pu(P::BrickNb + ua), u(1));
            const V t = m.f2u(m.fmax(fa, f(0.0f)));
            br.idx[a] = m.select(m.le(fa, f(0.0f)), u(0), m.select(m.ge(fa, m.u2f(last)), last, m.umin(t, last)));
        }
        br.b = m.add(m.mul(m.add(m.mul(br.idx[2], Pu(P::BrickNb + 1)), br.idx[1]), Pu(P::BrickNb)), br.idx[0]);
        return br;
    }
    // Next sample index after leaving brick `br` (render3d.cpp brick_exit + the k update).
    V next_k(const Brick& br, V3 p, V t, V3 dir, V k, V k1) {
        const V B = Pf(P::BrickSize);
        V te = f(std::numeric_limits<float>::infinity());
        for (int a = 0; a < 3; ++a) {
            const auto ua = static_cast<std::uint32_t>(a);
            const V da = dir[a];
            const V nb = Pu(P::BrickNb + ua);
            const V lo = m.select(m.eq(br.idx[a], u(0)), f(-1e30f), m.mul(m.u2f(br.idx[a]), B));
            const V i1 = m.add(br.idx[a], u(1));
            const V hi = m.select(m.eq(i1, nb), f(1e30f), m.mul(m.u2f(i1), B));
            const V ta = m.add(t, m.div(m.sub(m.select(m.gt(da, f(0.0f)), hi, lo), p[a]), da));
            te = m.select(m.lt(m.fabs(da), f(1e-30f)), te, m.fmin(te, ta));
        }
        const V nk = m.ceil(m.div(te, Pf(P::Step)));
        const V k1p = m.add(k1, i(1));
        const V nki = m.f2i(m.fmin(m.fmax(nk, f(-1e9f)), f(1e9f)));
        const V target = m.select(m.gt(nk, m.i2f(k1)), k1p, nki);
        return m.smax(m.add(k, i(1)), target);
    }

    V piecewise_gop(V x) {
        const V cnt = Pu(P::GopCount), off = Pu(P::GopOffset);
        const V last = m.mul(m.sub(cnt, u(1)), u(2));
        const V v0 = Df(off), y0 = Df(m.add(off, u(1)));
        const V vl = Df(m.add(off, last)), yl = Df(m.add(off, m.add(last, u(1))));
        Var res = var(yl);
        m.if_(m.lnot(m.gt(x, v0)), [&] { st(res, y0); },
              [&] {
                  m.if_(m.lt(x, vl), [&] {
                      Var idx = var(u(1)), done = var(m.c_bool(false));
                      m.while_([&] { return m.land(m.lt(ld(idx), cnt), m.lnot(ld(done))); },
                               [&] {
                                   const V at = m.add(off, m.mul(ld(idx), u(2)));
                                   const V vi = Df(at);
                                   m.if_(m.le(x, vi), [&] {
                                       const V vp = Df(m.sub(at, u(2))), yp = Df(m.sub(at, u(1))), yi = Df(m.add(at, u(1)));
                                       const V span = m.sub(vi, vp);
                                       const V fr = m.select(m.gt(span, f(0.0f)), m.div(m.sub(x, vp), span), f(1.0f));
                                       st(res, m.add(yp, m.mul(m.sub(yi, yp), fr)));
                                       st(done, m.c_bool(true));
                                   });
                                   st(idx, m.add(ld(idx), u(1)));
                               });
                  });
              });
        return ld(res);
    }

    V3 composite(V fx, V fy) {
        const Ray R = ray(fx, fy);
        Var cr = var(f(0.0f)), cg = var(f(0.0f)), cb = var(f(0.0f)), A = var(f(0.0f));
        m.if_(R.hit, [&] {
            const V step = Pf(P::Step), ert = Pf(P::Ert);
            const V skip = flag(P::Flags, P::kFlagSkip);
            const V grad = flag(P::Flags, P::kFlagGradient);
            const V shading = flag(P::Flags, P::kFlagShading);
            const V gop = flag(P::Flags, P::kFlagGradOpacity);
            const V lo = Pf(P::TfLo), scale = Pf(P::TfScale);
            const V emax = m.u2f(m.sub(Pu(P::TfEntries), u(1)));
            Var k = var(R.k0);
            m.while_([&] { return m.land(m.le(ld(k), R.k1), m.lt(ld(A), ert)); },
                     [&] {
                         const V kk = ld(k);
                         const V t = m.mul(m.i2f(kk), step);
                         const V3 p = add3(R.s, scale3(R.dir, t));
                         Var skipped = var(m.c_bool(false));
                         m.if_(skip, [&] {
                             const Brick br = brick_of(p);
                             const V word = Du(m.add(Pu(P::BrickEmpty), m.shr(br.b, u(5))));
                             const V empty = m.ne(m.band(m.shr(word, m.band(br.b, u(31))), u(1)), u(0));
                             m.if_(empty, [&] {
                                 st(k, next_k(br, p, t, R.dir, kk, R.k1));
                                 st(skipped, m.c_bool(true));
                             });
                         });
                         m.if_(m.lnot(ld(skipped)), [&] {
                             st(k, m.add(kk, i(1)));
                             const V x = linear(tex0, P::L0, p);
                             const V ef = m.add(m.mul(m.sub(x, lo), scale), f(0.5f));
                             const V e = m.f2u(m.fmin(m.fmax(ef, f(0.0f)), emax));
                             Var a = var(Df(m.add(Pu(P::TfA), e)));
                             m.if_(m.gt(ld(a), f(0.0f)), [&] {
                                 Var r = var(Df(m.add(Pu(P::TfR), e))), g = var(Df(m.add(Pu(P::TfG), e))),
                                     b = var(Df(m.add(Pu(P::TfB), e)));
                                 m.if_(grad, [&] {
                                     auto lin_at = [&](float dx, float dy, float dz) {
                                         return linear(tex0, P::L0, V3{m.add(p.x, f(dx)), m.add(p.y, f(dy)), m.add(p.z, f(dz))});
                                     };
                                     const V half = f(0.5f);
                                     const V3 gx{m.mul(m.sub(lin_at(1, 0, 0), lin_at(-1, 0, 0)), half),
                                                 m.mul(m.sub(lin_at(0, 1, 0), lin_at(0, -1, 0)), half),
                                                 m.mul(m.sub(lin_at(0, 0, 1), lin_at(0, 0, -1)), half)};
                                     const V3 G{dot3(Pv3(P::Gi), gx), dot3(Pv3(P::Gi + 3), gx), dot3(Pv3(P::Gi + 6), gx)};
                                     const V gm = m.sqrt(dot3(G, G));
                                     m.if_(gop, [&] { st(a, m.mul(ld(a), clamp01(piecewise_gop(gm)))); });
                                     const V finite = m.lnot(m.lor(m.is_inf(gm), m.is_nan(gm)));
                                     m.if_(m.land(shading, m.land(m.gt(gm, f(1e-12f)), finite)), [&] {
                                         const V ndl = m.div(m.fabs(dot3(G, Pv3(P::Normal))), gm);
                                         const V shin = Pf(P::Shininess);
                                         const V pw = m.select(m.eq(shin, f(0.0f)), f(1.0f),
                                                               m.select(m.gt(ndl, f(0.0f)), m.pow(m.fmax(ndl, f(1e-30f)), shin), f(0.0f)));
                                         const V spec = m.mul(Pf(P::Specular), pw);
                                         const V lit = m.add(Pf(P::Ambient), m.mul(Pf(P::Diffuse), ndl));
                                         st(r, clamp01(m.add(m.mul(ld(r), lit), spec)));
                                         st(g, clamp01(m.add(m.mul(ld(g), lit), spec)));
                                         st(b, clamp01(m.add(m.mul(ld(b), lit), spec)));
                                     });
                                 });
                                 const V w = m.mul(m.sub(f(1.0f), ld(A)), ld(a));
                                 st(cr, m.add(ld(cr), m.mul(w, ld(r))));
                                 st(cg, m.add(ld(cg), m.mul(w, ld(g))));
                                 st(cb, m.add(ld(cb), m.mul(w, ld(b))));
                                 st(A, m.add(ld(A), w));
                             });
                         });
                     });
        });
        // to8(C + (1 - A) * bg / 255), returned in 0..255 (the caller adds 0.5 and truncates).
        const V rest = m.sub(f(1.0f), ld(A));
        const V3 bg = background();
        const V k255 = f(255.0f), inv255 = f(1.0f / 255.0f);
        auto ch = [&](V c, V bgc) { return m.mul(clamp01(m.add(c, m.mul(rest, m.mul(bgc, inv255)))), k255); };
        return {ch(ld(cr), bg.x), ch(ld(cg), bg.y), ch(ld(cb), bg.z)};
    }

    V3 mip(V fx, V fy) {
        const Ray R = ray(fx, fy);
        const V minip = flag(P::Flags, P::kFlagMinIp);
        const V inf = f(std::numeric_limits<float>::infinity());
        Var best = var(m.select(minip, inf, m.neg(inf))), any = var(m.c_bool(false));
        m.if_(R.hit, [&] {
            const V step = Pf(P::Step);
            const V skip = flag(P::Flags, P::kFlagSkip);
            Var k = var(R.k0);
            m.while_([&] { return m.le(ld(k), R.k1); },
                     [&] {
                         const V kk = ld(k);
                         const V t = m.mul(m.i2f(kk), step);
                         const V3 p = add3(R.s, scale3(R.dir, t));
                         Var skipped = var(m.c_bool(false));
                         m.if_(m.land(skip, ld(any)), [&] {
                             const Brick br = brick_of(p);
                             const V mn = Df(m.add(Pu(P::BrickMin), br.b)), mx = Df(m.add(Pu(P::BrickMax), br.b));
                             const V bb = ld(best);
                             const V useless = m.lor(m.lnot(m.le(mn, mx)), m.select(minip, m.ge(mn, bb), m.le(mx, bb)));
                             m.if_(useless, [&] {
                                 st(k, next_k(br, p, t, R.dir, kk, R.k1));
                                 st(skipped, m.c_bool(true));
                             });
                         });
                         m.if_(m.lnot(ld(skipped)), [&] {
                             const V x = linear(tex0, P::L0, p);
                             const V bb = ld(best);
                             m.if_(m.select(minip, m.lt(x, bb), m.gt(x, bb)), [&] {
                                 st(best, x);
                                 st(any, m.c_bool(true));
                             });
                             st(k, m.add(kk, i(1)));
                         });
                     });
        });
        const V3 bg = background();
        Var r = var(bg.x), g = var(bg.y), b = var(bg.z);
        m.if_(ld(any), [&] {
            const Color c = color_of(P::L0, ld(best));
            st(r, c.r);
            st(g, c.g);
            st(b, c.b);
        });
        return {ld(r), ld(g), ld(b)};
    }

    P::Kernel kind_;
    Module m;
    Id tf = 0, tu = 0, ti = 0, v3f = 0, v3i = 0, v2i = 0, v4u = 0;
    Id params = 0, data = 0, tex0 = 0, tex1 = 0, out = 0, gid = 0;
};

}  // namespace

std::vector<std::uint32_t> kernel(param::Kernel k) {
    Gen g(k);
    return g.build();
}

}  // namespace siaara::gpu::spirv
