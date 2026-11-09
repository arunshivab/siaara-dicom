// SIAARA DICOM Toolkit - JPEG 2000 discrete wavelet transforms, written from T.800 annex F.
// Reversible 5-3 (integer lifting with rounding, F.3.8.1 / F.4.8.1) and irreversible 9-7
// (floating-point lifting, F.3.8.2 / F.4.8.2) with periodic symmetric extension (F.3.7). Rows are
// transformed one at a time; columns in strips of kStrip columns so the lifting runs along rows.
//
// Each 1-D transform works on an extended copy of the signal: kPad samples on each side are filled
// by symmetric extension and the lifting steps are applied across the whole copy. Every step
// invalidates one sample at each end, so after at most four steps the central part is exact.
#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "codec/j2k_internal.hpp"

namespace siaara::codec::j2k::detail {

namespace {

constexpr std::int64_t kPad = 4;
constexpr std::size_t kStrip = 32;

// PSEO (F-4): index of the sample that periodic symmetric extension places at i.
inline std::int64_t pseo(std::int64_t i, std::int64_t i0, std::int64_t i1) {
    const std::int64_t span = 2 * (i1 - i0 - 1);
    std::int64_t m = (i - i0) % span;
    if (m < 0) m += span;
    return i0 + std::min(m, span - m);
}

// Lifting kernels. Each applies one step to the samples of one parity of an extended signal of
// `n` entries (entry k holds index base + k) with `w` interleaved lanes per entry.
struct Int53 {
    using T = std::int32_t;
    static inline T inv_even(T c, T a, T b) {
        return static_cast<T>(std::int64_t{c} - ((std::int64_t{a} + b + 2) >> 2));
    }
    static inline T inv_odd(T c, T a, T b) { return static_cast<T>(std::int64_t{c} + ((std::int64_t{a} + b) >> 1)); }
    static inline T fwd_odd(T c, T a, T b) { return static_cast<T>(std::int64_t{c} - ((std::int64_t{a} + b) >> 1)); }
    static inline T fwd_even(T c, T a, T b) {
        return static_cast<T>(std::int64_t{c} + ((std::int64_t{a} + b + 2) >> 2));
    }
};

template <class T, class F>
inline void lift(T* e, std::int64_t n, std::int64_t base, int parity, std::size_t w, F f) {
    std::int64_t k = 1;
    if (((base + k) & 1) != parity) ++k;
    for (; k + 1 < n; k += 2) {
        T* c = e + static_cast<std::size_t>(k) * w;
        const T* a = c - w;
        const T* b = c + w;
        for (std::size_t j = 0; j < w; ++j) c[j] = f(c[j], a[j], b[j]);
    }
}

template <class T, class F>
inline void scale(T* e, std::int64_t n, std::int64_t base, int parity, std::size_t w, F f) {
    std::int64_t k = 0;
    if (((base + k) & 1) != parity) ++k;
    for (; k < n; k += 2) {
        T* c = e + static_cast<std::size_t>(k) * w;
        for (std::size_t j = 0; j < w; ++j) c[j] = f(c[j]);
    }
}

template <class T>
void inverse_lift(T* e, std::int64_t n, std::int64_t base, std::size_t w) {
    if constexpr (std::is_same_v<T, std::int32_t>) {
        lift(e, n, base, 0, w, Int53::inv_even);
        lift(e, n, base, 1, w, Int53::inv_odd);
    } else {
        scale(e, n, base, 0, w, [](float v) { return v * kK; });
        scale(e, n, base, 1, w, [](float v) { return v * (1.0f / kK); });
        lift(e, n, base, 0, w, [](float c, float a, float b) { return c - kDelta * (a + b); });
        lift(e, n, base, 1, w, [](float c, float a, float b) { return c - kGamma * (a + b); });
        lift(e, n, base, 0, w, [](float c, float a, float b) { return c - kBeta * (a + b); });
        lift(e, n, base, 1, w, [](float c, float a, float b) { return c - kAlpha * (a + b); });
    }
}

template <class T>
void forward_lift(T* e, std::int64_t n, std::int64_t base, std::size_t w) {
    if constexpr (std::is_same_v<T, std::int32_t>) {
        lift(e, n, base, 1, w, Int53::fwd_odd);
        lift(e, n, base, 0, w, Int53::fwd_even);
    } else {
        lift(e, n, base, 1, w, [](float c, float a, float b) { return c + kAlpha * (a + b); });
        lift(e, n, base, 0, w, [](float c, float a, float b) { return c + kBeta * (a + b); });
        lift(e, n, base, 1, w, [](float c, float a, float b) { return c + kGamma * (a + b); });
        lift(e, n, base, 0, w, [](float c, float a, float b) { return c + kDelta * (a + b); });
        scale(e, n, base, 0, w, [](float v) { return v * (1.0f / kK); });
        scale(e, n, base, 1, w, [](float v) { return v * kK; });
    }
}

template <class T>
inline T half_of(T v) {
    if constexpr (std::is_same_v<T, std::int32_t>) return static_cast<T>(v / 2);
    else return v * 0.5f;
}

// Synthesis of `lanes` interleaved 1-D signals stored as `in` rows (lane j of entry m at
// in[m * istride + j]): low-pass entries first, then high-pass. Output to `out` the same way.
template <class T>
void synth(const T* in, std::size_t istride, T* out, std::size_t ostride, std::int64_t i0, std::int64_t i1,
           std::size_t lanes, std::vector<T>& ext) {
    const std::int64_t n = i1 - i0;
    if (n <= 0) return;
    const std::int64_t nl = ceil_shift(i1, 1) - ceil_shift(i0, 1);
    if (n == 1) {
        for (std::size_t j = 0; j < lanes; ++j) out[j] = (i0 & 1) ? half_of(in[j]) : in[j];
        return;
    }
    const std::int64_t ne = n + 2 * kPad;
    ext.resize(static_cast<std::size_t>(ne) * lanes);
    const std::int64_t base = i0 - kPad;
    const std::int64_t l0 = ceil_shift(i0, 1);
    const std::int64_t h0 = i0 >> 1;
    for (std::int64_t k = 0; k < ne; ++k) {
        const std::int64_t i = pseo(base + k, i0, i1);
        const std::int64_t src = (i & 1) ? nl + ((i >> 1) - h0) : (i >> 1) - l0;
        const T* s = in + static_cast<std::size_t>(src) * istride;
        T* d = ext.data() + static_cast<std::size_t>(k) * lanes;
        for (std::size_t j = 0; j < lanes; ++j) d[j] = s[j];
    }
    inverse_lift(ext.data(), ne, base, lanes);
    for (std::int64_t k = 0; k < n; ++k) {
        const T* s = ext.data() + static_cast<std::size_t>(k + kPad) * lanes;
        T* d = out + static_cast<std::size_t>(k) * ostride;
        for (std::size_t j = 0; j < lanes; ++j) d[j] = s[j];
    }
}

// Analysis: inverse of synth (interleaved input, low then high output).
template <class T>
void analyse(const T* in, std::size_t istride, T* out, std::size_t ostride, std::int64_t i0, std::int64_t i1,
             std::size_t lanes, std::vector<T>& ext) {
    const std::int64_t n = i1 - i0;
    if (n <= 0) return;
    const std::int64_t nl = ceil_shift(i1, 1) - ceil_shift(i0, 1);
    if (n == 1) {
        for (std::size_t j = 0; j < lanes; ++j) out[j] = (i0 & 1) ? static_cast<T>(in[j] * 2) : in[j];
        return;
    }
    const std::int64_t ne = n + 2 * kPad;
    ext.resize(static_cast<std::size_t>(ne) * lanes);
    const std::int64_t base = i0 - kPad;
    for (std::int64_t k = 0; k < ne; ++k) {
        const std::int64_t i = pseo(base + k, i0, i1);
        const T* s = in + static_cast<std::size_t>(i - i0) * istride;
        T* d = ext.data() + static_cast<std::size_t>(k) * lanes;
        for (std::size_t j = 0; j < lanes; ++j) d[j] = s[j];
    }
    forward_lift(ext.data(), ne, base, lanes);
    const std::int64_t l0 = ceil_shift(i0, 1);
    const std::int64_t h0 = i0 >> 1;
    for (std::int64_t i = i0; i < i1; ++i) {
        const std::int64_t dst = (i & 1) ? nl + ((i >> 1) - h0) : (i >> 1) - l0;
        const T* s = ext.data() + static_cast<std::size_t>(i - base) * lanes;
        T* d = out + static_cast<std::size_t>(dst) * ostride;
        for (std::size_t j = 0; j < lanes; ++j) d[j] = s[j];
    }
}

template <class T>
void idwt(T* data, std::size_t stride, std::span<const ResGeom> res) {
    std::vector<T> ext, tmp;
    for (std::size_t r = 1; r < res.size(); ++r) {
        const ResGeom& g = res[r];
        const auto w = static_cast<std::size_t>(g.x1 - g.x0);
        const auto h = static_cast<std::size_t>(g.y1 - g.y0);
        if (w == 0 || h == 0) continue;
        tmp.resize(std::max(w, h * kStrip));
        // Rows.
        for (std::size_t y = 0; y < h; ++y) {
            T* row = data + y * stride;
            std::copy(row, row + w, tmp.begin());
            synth(tmp.data(), 1, row, 1, g.x0, g.x1, 1, ext);
        }
        // Columns, in strips.
        for (std::size_t x = 0; x < w; x += kStrip) {
            const std::size_t lanes = std::min(kStrip, w - x);
            for (std::size_t y = 0; y < h; ++y)
                std::copy(data + y * stride + x, data + y * stride + x + lanes, tmp.begin() + static_cast<std::ptrdiff_t>(y * lanes));
            synth(tmp.data(), lanes, data + x, stride, g.y0, g.y1, lanes, ext);
        }
    }
}

template <class T>
void fdwt(T* data, std::size_t stride, std::span<const ResGeom> res) {
    std::vector<T> ext, tmp;
    for (std::size_t r = res.size(); r-- > 1;) {
        const ResGeom& g = res[r];
        const auto w = static_cast<std::size_t>(g.x1 - g.x0);
        const auto h = static_cast<std::size_t>(g.y1 - g.y0);
        if (w == 0 || h == 0) continue;
        tmp.resize(std::max(w, h * kStrip));
        for (std::size_t x = 0; x < w; x += kStrip) {
            const std::size_t lanes = std::min(kStrip, w - x);
            for (std::size_t y = 0; y < h; ++y)
                std::copy(data + y * stride + x, data + y * stride + x + lanes, tmp.begin() + static_cast<std::ptrdiff_t>(y * lanes));
            analyse(tmp.data(), lanes, data + x, stride, g.y0, g.y1, lanes, ext);
        }
        for (std::size_t y = 0; y < h; ++y) {
            T* row = data + y * stride;
            std::copy(row, row + w, tmp.begin());
            analyse(tmp.data(), 1, row, 1, g.x0, g.x1, 1, ext);
        }
    }
}

}  // namespace

void idwt53(std::int32_t* data, std::size_t stride, std::span<const ResGeom> res) { idwt(data, stride, res); }
void idwt97(float* data, std::size_t stride, std::span<const ResGeom> res) { idwt(data, stride, res); }
void fdwt53(std::int32_t* data, std::size_t stride, std::span<const ResGeom> res) { fdwt(data, stride, res); }
void fdwt97(float* data, std::size_t stride, std::span<const ResGeom> res) { fdwt(data, stride, res); }

}  // namespace siaara::codec::j2k::detail
