// SIAARA DICOM Toolkit - volume building (see volume.hpp).
#include "volume/volume.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/reader.hpp"
#include "image/frames.hpp"
#include "image/lut.hpp"
#include "image/pixel_info.hpp"
#include "image/samples.hpp"
#include "volume/parallel.hpp"

namespace siaara::volume {

// ---------------------------------------------------------------- Volume
Result<Volume> Volume::allocate(const Grid& grid, VoxelType type, std::uint64_t memory_budget) {
    if (Status s = grid.validate(); !s) return s;
    std::uint64_t count = 0, bytes = 0;
    if (!grid.voxel_count(count) || !checked_mul(count, type == VoxelType::Int16 ? 2u : 4u, bytes))
        return Status(Errc::LimitExceeded, "volume: size overflows");
    if (bytes > memory_budget) return Status(Errc::LimitExceeded, "volume: voxel buffer exceeds the memory budget");
    if (bytes > static_cast<std::uint64_t>(SIZE_MAX / 2) || count > static_cast<std::uint64_t>(SIZE_MAX / 4))
        return Status(Errc::LimitExceeded, "volume: voxel buffer does not fit in memory on this platform");
    Volume v;
    v.grid_ = grid;
    v.type_ = type;
    v.count_ = count;
    const auto n = static_cast<std::size_t>(count);
    if (type == VoxelType::Int16) {
        v.i16_.reset(new (std::nothrow) std::int16_t[n]);
        if (!v.i16_) return Status(Errc::OutOfMemory, "volume: out of memory");
    } else {
        v.f32_.reset(new (std::nothrow) float[n]);
        if (!v.f32_) return Status(Errc::OutOfMemory, "volume: out of memory");
    }
    return v;
}

Result<Volume> Volume::clone(std::uint64_t memory_budget) const {
    auto r = allocate(grid_, type_, memory_budget);
    if (!r) return r;
    Volume& v = *r;
    if (count_) std::memcpy(type_ == VoxelType::Int16 ? static_cast<void*>(v.i16_.get()) : static_cast<void*>(v.f32_.get()),
                            data(), static_cast<std::size_t>(byte_size()));
    v.value_min = value_min;
    v.value_max = value_max;
    v.histogram = histogram;
    v.warnings = warnings;
    v.flags = flags;
    v.phase_count = phase_count;
    v.phase_index = phase_index;
    v.phase_key = phase_key;
    v.tilt_degrees = tilt_degrees;
    v.source_frames = source_frames;
    return r;
}

const void* Volume::data() const noexcept {
    return type_ == VoxelType::Int16 ? static_cast<const void*>(i16_.get()) : static_cast<const void*>(f32_.get());
}

void Volume::set(std::uint32_t i, std::uint32_t j, std::uint32_t k, float v) noexcept {
    const std::size_t n = index(i, j, k);
    if (type_ == VoxelType::Int16) {
        float c = std::isfinite(v) ? std::clamp(v, -32768.0f, 32767.0f) : 0.0f;
        i16_[n] = static_cast<std::int16_t>(std::lrint(c));
    } else {
        f32_[n] = v;
    }
}

namespace {

std::size_t hist_bin(double v, double lo, double hi, std::uint32_t bins) noexcept {
    if (!(hi > lo)) return 0;
    double t = (v - lo) / (hi - lo) * bins;
    if (!(t > 0)) return 0;
    if (t >= bins) return bins - 1u;
    return static_cast<std::size_t>(t);
}

}  // namespace

namespace detail {

// Exact counts of int16 values. Four interleaved tables break the store-to-load dependency of
// long runs of one value (air in CT), which otherwise serialises the increments.
struct Int16Counts {
    std::vector<std::uint64_t> c;  // 4 x 65536
    bool init() noexcept {
        try {
            c.assign(std::size_t{4} * 65536u, 0);
            return true;
        } catch (...) {
            return false;
        }
    }
    void add(const std::int16_t* p, std::size_t n) noexcept {
        std::uint64_t* t0 = c.data() + 32768;
        std::uint64_t* t1 = t0 + 65536;
        std::uint64_t* t2 = t1 + 65536;
        std::uint64_t* t3 = t2 + 65536;
        std::size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            ++t0[p[i]];
            ++t1[p[i + 1]];
            ++t2[p[i + 2]];
            ++t3[p[i + 3]];
        }
        for (; i < n; ++i) ++t0[p[i]];
    }
};

}  // namespace detail

void Volume::set_int16_statistics(const std::vector<const std::vector<std::uint64_t>*>& parts, std::uint32_t bins) {
    std::vector<std::uint64_t> total(65536, 0);  // may throw std::bad_alloc: callers catch it
    for (const auto* part : parts)
        for (std::size_t q = 0; q < 4; ++q)
            for (std::size_t v = 0; v < 65536; ++v) total[v] += (*part)[q * 65536 + v];
    std::size_t lo = 0, hi = 65535;
    while (lo < 65535 && total[lo] == 0) ++lo;
    while (hi > 0 && total[hi] == 0) --hi;
    if (lo > hi) lo = hi = 32768;
    value_min = static_cast<double>(static_cast<std::int64_t>(lo) - 32768);
    value_max = static_cast<double>(static_cast<std::int64_t>(hi) - 32768);
    histogram.min = value_min;
    histogram.max = value_max;
    for (std::size_t v = lo; v <= hi; ++v)
        if (total[v])
            histogram.bins[hist_bin(static_cast<double>(static_cast<std::int64_t>(v) - 32768), value_min, value_max, bins)] += total[v];
}

Status Volume::update_statistics(unsigned threads, std::uint32_t bins) {
    if (bins == 0) bins = 1;
    if (bins > (1u << 20)) return Status(Errc::LimitExceeded, "volume: too many histogram bins");
    histogram = Histogram{};
    value_min = value_max = 0.0;
    const unsigned workers = detail::resolve_threads(threads);
    try {
        histogram.bins.assign(bins, 0);
        if (count_ == 0) return Status::ok();
        const std::size_t slice = static_cast<std::size_t>(grid_.dims[0]) * grid_.dims[1];
        const std::uint32_t nz = grid_.dims[2];
        if (type_ == VoxelType::Int16) {
            std::vector<detail::Int16Counts> counts(workers);
            for (auto& c : counts)
                if (!c.init()) return Status(Errc::OutOfMemory, "volume: out of memory");
            detail::parallel_for(nz, workers, [&](std::uint64_t k, unsigned w) noexcept {
                counts[w].add(i16_.get() + static_cast<std::size_t>(k) * slice, slice);
            });
            std::vector<const std::vector<std::uint64_t>*> parts;
            for (auto& c : counts) parts.push_back(&c.c);
            set_int16_statistics(parts, bins);
            return Status::ok();
        }
        std::vector<float> lo(workers, std::numeric_limits<float>::infinity()), hi(workers, -std::numeric_limits<float>::infinity());
        detail::parallel_for(nz, workers, [&](std::uint64_t k, unsigned w) noexcept {
            const float* p = f32_.get() + static_cast<std::size_t>(k) * slice;
            float a = lo[w], b = hi[w];
            for (std::size_t i = 0; i < slice; ++i) {
                const float v = p[i];
                // NaN fails the comparison; infinities are excluded too.
                if (std::fabs(v) <= std::numeric_limits<float>::max()) {
                    a = v < a ? v : a;
                    b = v > b ? v : b;
                }
            }
            lo[w] = a;
            hi[w] = b;
        });
        double mn = std::numeric_limits<double>::infinity(), mx = -mn;
        for (unsigned w = 0; w < workers; ++w) {
            mn = std::min(mn, static_cast<double>(lo[w]));
            mx = std::max(mx, static_cast<double>(hi[w]));
        }
        if (!std::isfinite(mn)) mn = mx = 0.0;  // no finite voxel
        value_min = mn;
        value_max = mx;
        histogram.min = mn;
        histogram.max = mx;
        const double scale = mx > mn ? bins / (mx - mn) : 0.0;
        std::vector<std::vector<std::uint64_t>> parts(workers, std::vector<std::uint64_t>(bins, 0));
        detail::parallel_for(nz, workers, [&](std::uint64_t k, unsigned w) noexcept {
            auto& h = parts[w];
            const float* p = f32_.get() + static_cast<std::size_t>(k) * slice;
            const double top = static_cast<double>(bins - 1u);
            for (std::size_t i = 0; i < slice; ++i) {
                const double v = p[i];
                if (!(std::fabs(v) <= std::numeric_limits<double>::max())) continue;
                const double t = (v - mn) * scale;
                ++h[static_cast<std::size_t>(t > 0.0 ? (t < top ? t : top) : 0.0)];
            }
        });
        for (auto& p : parts)
            for (std::uint32_t b = 0; b < bins; ++b) histogram.bins[b] += p[b];
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "volume: out of memory");
    }
    return Status::ok();
}

// ---------------------------------------------------------------- building
namespace {

struct Source {
    const DicomFile* file = nullptr;
    std::optional<image::FrameReader> fr;
    bool per_frame_groups = false;
};

struct ModSummary {
    bool lut = false;
    bool identity = true;
    double slope = 1.0;
    double intercept = 0.0;
    double out_lo = 0.0, out_hi = 0.0;  // possible output range
};

struct FrameRec {
    std::uint32_t src = 0;
    std::uint32_t frame = 0;
    std::uint32_t order = 0;  // input order (tie break)
    FramePlane plane;
    FrameKeys keys;
    ModSummary mod;
    double d = 0.0;           // position along the reference normal
};

bool same_position(const Vec3& a, const Vec3& b, double eps) noexcept {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

constexpr double kPositionEps = 1e-3;  // mm: positions closer than this are the same position

// True when some frames share a position.
bool has_duplicate_positions(const std::vector<FrameRec>& frames, const std::vector<std::size_t>& idx) {
    std::vector<std::size_t> s = idx;
    std::sort(s.begin(), s.end(), [&](std::size_t a, std::size_t b) {
        const Vec3& p = frames[a].plane.position;
        const Vec3& q = frames[b].plane.position;
        if (p.x != q.x) return p.x < q.x;
        if (p.y != q.y) return p.y < q.y;
        return p.z < q.z;
    });
    for (std::size_t i = 1; i < s.size(); ++i)
        if (same_position(frames[s[i]].plane.position, frames[s[i - 1]].plane.position, kPositionEps)) return true;
    return false;
}

struct PhaseSplit {
    std::string key;
    std::vector<std::vector<std::size_t>> groups;
};

// Groups frames by a key; the key must be present on every frame.
template <class Get>
std::optional<PhaseSplit> group_by(const std::vector<FrameRec>& frames, const char* name, Get get) {
    using K = std::decay_t<decltype(*get(frames[0]))>;
    std::map<K, std::vector<std::size_t>> m;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        auto k = get(frames[i]);
        if (!k) return std::nullopt;
        m[*k].push_back(i);
    }
    PhaseSplit p;
    p.key = name;
    for (auto& [k, v] : m) p.groups.push_back(std::move(v));
    return p;
}

PhaseSplit split_phases(const std::vector<FrameRec>& frames, bool positions_known) {
    std::vector<std::size_t> all(frames.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    const bool dups = positions_known && has_duplicate_positions(frames, all);
    auto unique_positions = [&](const PhaseSplit& p) {
        if (!positions_known) return true;
        for (const auto& g : p.groups)
            if (has_duplicate_positions(frames, g)) return false;
        return true;
    };
    // Explicit enhanced keys: Stack ID (several stacks), Temporal Position Index (several times).
    auto by_stack = group_by(frames, "StackID", [](const FrameRec& f) { return f.keys.stack_id; });
    auto by_tpi = group_by(frames, "TemporalPositionIndex", [](const FrameRec& f) { return f.keys.temporal_position_index; });
    if (by_stack && by_stack->groups.size() > 1 && (!dups || unique_positions(*by_stack))) return *by_stack;
    if (by_tpi && by_tpi->groups.size() > 1 && (!dups || unique_positions(*by_tpi))) return *by_tpi;
    if (dups) {
        auto by_tpid = group_by(frames, "TemporalPositionIdentifier",
                                [](const FrameRec& f) { return f.keys.temporal_position_identifier; });
        auto by_trig = group_by(frames, "TriggerTime", [](const FrameRec& f) { return f.keys.trigger_time; });
        for (auto* p : {&by_tpid, &by_trig})
            if (*p && (*p)->groups.size() > 1 && unique_positions(**p)) return **p;
    }
    PhaseSplit one;
    one.groups.push_back(std::move(all));
    return one;
}

std::optional<double> median(std::vector<double> v) {
    if (v.empty()) return std::nullopt;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// ---- stored value conversion
struct SliceStats {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    bool clamped = false;
};

// Converts one decoded frame (FrameReader layout) to modality values in `out` (float).
void frame_to_float(const image::PixelInfo& pi, const std::uint8_t* src, const image::ModalityTransform& m, float* out,
                    std::size_t n) noexcept {
    if (pi.kind == image::SampleKind::Float32) {
        for (std::size_t i = 0; i < n; ++i) out[i] = image::detail::load_f32(src + 4 * i);
        return;
    }
    if (pi.kind == image::SampleKind::Float64) {
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(image::detail::load_f64(src + 8 * i));
        return;
    }
    const image::detail::SampleReader rd(pi);
    if (m.has_lut) {
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(m.lut.lookup(rd.value(src, i)));
        return;
    }
    const double slope = m.slope, icpt = m.intercept;
    if (pi.bits_allocated == 16) {
        const unsigned shift = pi.low_bit();
        const auto mask = static_cast<std::uint32_t>(rd.mask);
        const auto sign = static_cast<std::uint32_t>(rd.sign);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t raw = static_cast<std::uint32_t>(src[2 * i]) | (static_cast<std::uint32_t>(src[2 * i + 1]) << 8);
            const std::uint32_t v = (raw >> shift) & mask;
            const std::int32_t sv = static_cast<std::int32_t>(v ^ sign) - static_cast<std::int32_t>(sign);
            out[i] = static_cast<float>(static_cast<double>(sv) * slope + icpt);
        }
        return;
    }
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(static_cast<double>(rd.value(src, i)) * slope + icpt);
}

// int16 output. Returns false when a value had to be clamped or rounded.
bool frame_to_int16(const image::PixelInfo& pi, const std::uint8_t* src, const image::ModalityTransform& m, std::int16_t* out,
                    std::size_t n, float* scratch) noexcept {
    const bool integral_rescale = !m.has_lut && pi.kind == image::SampleKind::Integer && std::trunc(m.slope) == m.slope &&
                                  std::trunc(m.intercept) == m.intercept && std::fabs(m.slope) <= 65536.0 &&
                                  std::fabs(m.intercept) <= 1e9;
    if (integral_rescale && pi.bits_allocated == 16) {
        const image::detail::SampleReader rd(pi);
        const unsigned shift = pi.low_bit();
        const auto mask = static_cast<std::uint32_t>(rd.mask);
        const auto sign = static_cast<std::uint32_t>(rd.sign);
        const auto slope = static_cast<std::int64_t>(m.slope);
        const auto icpt = static_cast<std::int64_t>(m.intercept);
        std::int64_t bad = 0;
        if (slope == 1 && icpt >= INT32_MIN / 2 && icpt <= INT32_MAX / 2) {
            const auto ic = static_cast<std::int32_t>(icpt);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t raw = static_cast<std::uint32_t>(src[2 * i]) | (static_cast<std::uint32_t>(src[2 * i + 1]) << 8);
                const std::uint32_t v = (raw >> shift) & mask;
                const std::int32_t r = static_cast<std::int32_t>(v ^ sign) - static_cast<std::int32_t>(sign) + ic;
                const std::int32_t c = std::clamp(r, -32768, 32767);
                bad |= (r ^ c);
                out[i] = static_cast<std::int16_t>(c);
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t raw = static_cast<std::uint32_t>(src[2 * i]) | (static_cast<std::uint32_t>(src[2 * i + 1]) << 8);
                const std::uint32_t v = (raw >> shift) & mask;
                const std::int64_t r =
                    (static_cast<std::int64_t>(static_cast<std::int32_t>(v ^ sign) - static_cast<std::int32_t>(sign))) * slope + icpt;
                const std::int64_t c = std::clamp<std::int64_t>(r, -32768, 32767);
                bad |= (r ^ c);
                out[i] = static_cast<std::int16_t>(c);
            }
        }
        return bad == 0;
    }
    frame_to_float(pi, src, m, scratch, n);
    bool ok = true;
    for (std::size_t i = 0; i < n; ++i) {
        const float f = scratch[i];
        float r = std::isfinite(f) ? std::nearbyint(f) : 0.0f;
        const float c = std::clamp(r, -32768.0f, 32767.0f);
        if (c != f) ok = false;
        out[i] = static_cast<std::int16_t>(c);
    }
    return ok;
}

// Bilinear in-plane shift of a float slice: out(i, j) = in(i + si, j + sj); outside -> fill.
void shift_slice(const float* in, float* out, std::uint32_t nx, std::uint32_t ny, double si, double sj, float fill) noexcept {
    for (std::uint32_t j = 0; j < ny; ++j) {
        const double y = j + sj;
        for (std::uint32_t i = 0; i < nx; ++i) {
            const double x = i + si;
            float v = fill;
            if (x >= -0.5 && x <= nx - 0.5 && y >= -0.5 && y <= ny - 0.5) {
                const double fx = std::floor(x), fy = std::floor(y);
                const double ax = x - fx, ay = y - fy;
                auto cl = [](double c, std::uint32_t n) {
                    return static_cast<std::size_t>(std::clamp(c, 0.0, static_cast<double>(n - 1)));
                };
                const std::size_t x0 = cl(fx, nx), x1 = cl(fx + 1, nx), y0 = cl(fy, ny), y1 = cl(fy + 1, ny);
                const double a = in[y0 * nx + x0] * (1 - ax) + in[y0 * nx + x1] * ax;
                const double b = in[y1 * nx + x0] * (1 - ax) + in[y1 * nx + x1] * ax;
                v = static_cast<float>(a * (1 - ay) + b * ay);
            }
            out[static_cast<std::size_t>(j) * nx + i] = v;
        }
    }
}

void add_warning(std::vector<std::string>& w, std::string s) {
    if (std::find(w.begin(), w.end(), s) == w.end() && w.size() < 64) w.push_back(std::move(s));
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

// RT Dose Grid Frame Offset Vector (3004,000C), PS3.3 C.8.8.3.2 (testing-chat P0-2b). Returns the
// offset of each frame along the plane normal from Image Position (Patient), or nullopt when the
// object has no usable vector (the caller then stacks with the header spacing and says so).
//  - relative form: the first value is 0 and the values are offsets from the first frame;
//  - absolute form: the first value is the z of Image Position (Patient) and the values are the
//    z coordinates of the planes; allowed only for transverse planes (normal along z).
// A vector of the wrong length, with values that are not finite or not strictly monotonic, or an
// absolute form that does not start at the IPP z or is not transverse, fails in strict mode and
// is ignored with a warning in lenient mode. Irregular (but monotonic) spacing is kept: the gap
// check of build_volume reports it as non-uniform spacing.
Result<std::optional<std::vector<double>>> grid_frame_offsets(const Dataset& ds, std::uint32_t nf, const FramePlane& plane,
                                                              bool strict, std::vector<std::string>& warnings) {
    constexpr Tag kGridFrameOffsetVector = 0x3004000C;
    using Out = std::optional<std::vector<double>>;
    if (!image::is_rt_dose(ds) || !ds.contains(kGridFrameOffsetVector)) return Out{};
    auto bad = [&](const std::string& why) -> Result<Out> {
        if (strict) return Status(Errc::Malformed, "volume: Grid Frame Offset Vector " + why, 0, kGridFrameOffsetVector);
        add_warning(warnings, "RT Dose Grid Frame Offset Vector " + why + "; ignored, frames stacked with the header slice spacing");
        return Out{};
    };
    const std::size_t vm = ds.value_multiplicity(kGridFrameOffsetVector);
    if (vm != nf) return bad("has " + std::to_string(vm) + " values for " + std::to_string(nf) + " frames");
    std::vector<double> v(nf);
    // One pass over the DS text (security review R-111), not one scan per frame.
    if (!ds.get_doubles(kGridFrameOffsetVector, 0, v)) return bad("has a value that is not a number");
    for (std::uint32_t k = 0; k < nf; ++k)
        if (!std::isfinite(v[k])) return bad("value " + std::to_string(k + 1) + " is not a number");
    for (std::uint32_t k = 1; k < nf; ++k) {
        const double g0 = v[1] - v[0], g = v[k] - v[k - 1];
        if (g == 0.0 || (g > 0.0) != (g0 > 0.0)) return bad("is not strictly increasing or decreasing");
    }
    if (v[0] == 0.0) return Out{std::move(v)};  // relative form
    const Vec3 n = plane.normal();
    if (std::fabs(std::fabs(n.z) - 1.0) > 1e-3)
        return bad("starts with " + fmt(v[0]) + " (absolute z form) but the planes are not transverse");
    if (std::fabs(v[0] - plane.position.z) > 1e-2)
        return bad("starts with " + fmt(v[0]) + ", neither 0 (relative form) nor the Image Position (Patient) z " +
                   fmt(plane.position.z) + " (absolute form)");
    for (double& x : v) x = (x - plane.position.z) / n.z;  // z coordinates -> offsets along the normal
    return Out{std::move(v)};
}

}  // namespace

Result<Volume> build_volume(std::span<const DicomFile* const> files, const VolumeOptions& opt) {
    if (files.empty()) return Status(Errc::InvalidArgument, "volume: no input files");
    for (const DicomFile* f : files)
        if (!f) return Status(Errc::InvalidArgument, "volume: null file");
    const unsigned threads = detail::resolve_threads(opt.threads);
    std::vector<std::string> warnings;

    // ---- 1. open the frame readers and read every frame's plane, keys and modality transform
    std::vector<Source> sources;
    std::vector<std::vector<FrameRec>> per_file;
    std::vector<Status> errors;
    try {
        sources.resize(files.size());
        per_file.resize(files.size());
        errors.resize(files.size());
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "volume: out of memory");
    }
    std::atomic<std::uint64_t> frame_total{0};
    image::FrameOptions fo;
    fo.lenient = opt.lenient;
    fo.threads = 1;
    fo.memory_budget = opt.memory_budget;
    detail::parallel_for(files.size(), threads, [&](std::uint64_t fi64, unsigned) noexcept {
        const auto fi = static_cast<std::size_t>(fi64);  // < files.size()
        try {
            Source& s = sources[fi];
            s.file = files[fi];
            const Dataset& ds = s.file->dataset;
            auto fr = image::FrameReader::open(ds, s.file->transfer_syntax, fo);
            if (!fr) {
                errors[fi] = fr.take_error();
                return;
            }
            s.fr.emplace(std::move(fr).value());
            const image::PixelInfo& pi = s.fr->info();
            if (pi.samples_per_pixel != 1 || !(pi.is_monochrome() || pi.kind != image::SampleKind::Integer)) {
                errors[fi] = Status(Errc::Unsupported, "volume: only single-sample grayscale images can form a volume");
                return;
            }
            if (pi.kind == image::SampleKind::Integer && pi.bits_stored > 32) {
                errors[fi] = Status(Errc::Unsupported, "volume: more than 32 bits stored");
                return;
            }
            const std::uint32_t nf = pi.number_of_frames;
            if (frame_total.fetch_add(nf) + nf > opt.max_frames) {
                errors[fi] = Status(Errc::LimitExceeded, "volume: too many frames");
                return;
            }
            const Element* pf = ds.find(tags::PerFrameFunctionalGroupsSequence);
            s.per_frame_groups = pf && pf->is_sequence() && !pf->items.empty();
            auto& out = per_file[fi];
            out.reserve(nf);
            for (std::uint32_t f = 0; f < nf; ++f) {
                FrameRec r;
                r.src = static_cast<std::uint32_t>(fi);
                r.frame = f;
                auto pl = read_frame_plane(ds, f);
                if (!pl) {
                    errors[fi] = pl.take_error();
                    return;
                }
                r.plane = std::move(pl).value();
                r.keys = read_frame_keys(ds, f);
                auto m = image::modality_for_frame(ds, pi, f, opt.lenient);
                if (!m) {
                    errors[fi] = m.take_error();
                    return;
                }
                r.mod.lut = m->has_lut;
                r.mod.identity = m->is_identity();
                r.mod.slope = m->slope;
                r.mod.intercept = m->intercept;
                if (pi.kind == image::SampleKind::Integer) {
                    m->output_range(image::detail::min_stored(pi), image::detail::max_stored(pi), r.mod.out_lo, r.mod.out_hi);
                } else {
                    r.mod.out_lo = -std::numeric_limits<double>::infinity();
                    r.mod.out_hi = std::numeric_limits<double>::infinity();
                }
                out.push_back(std::move(r));
            }
        } catch (const std::bad_alloc&) {
            errors[fi] = Status(Errc::OutOfMemory, "volume: out of memory");
        } catch (...) {
            errors[fi] = Status(Errc::Internal, "volume: internal error");
        }
    });
    for (auto& e : errors)
        if (!e) return e;

    std::vector<FrameRec> frames;
    try {
        frames.reserve(static_cast<std::size_t>(frame_total.load()));
        std::uint32_t order = 0;
        for (std::size_t fi = 0; fi < per_file.size(); ++fi) {
            const Source& s = sources[fi];
            const std::uint32_t nf = s.fr->info().number_of_frames;
            // Classic (non-enhanced) multi-frame object: one position for all frames; the frames are
            // stacked along the normal with the header's slice spacing.
            const bool synth_stack = nf > 1 && !s.per_frame_groups;
            // RT Dose: the Grid Frame Offset Vector gives each frame's offset (P0-2b).
            std::optional<std::vector<double>> gfov;
            if (synth_stack && !per_file[fi].empty() && per_file[fi].front().plane.has_position) {
                auto g = grid_frame_offsets(s.file->dataset, nf, per_file[fi].front().plane, opt.strict, warnings);
                if (!g) return g.take_error();
                gfov = std::move(g).value();
            }
            for (auto& r : per_file[fi]) {
                r.order = order++;
                if (synth_stack && r.plane.has_position) {
                    const double sp = r.plane.spacing_between_slices.value_or(r.plane.slice_thickness.value_or(1.0));
                    const double off = gfov ? (*gfov)[r.frame] : sp * r.frame;
                    r.plane.position = r.plane.position + r.plane.normal() * off;
                }
                frames.push_back(std::move(r));
            }
            if (synth_stack && !gfov)
                add_warning(warnings, "classic multi-frame object: frames stacked with the header slice spacing");
        }
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "volume: out of memory");
    }
    per_file.clear();
    if (frames.empty()) return Status(Errc::InvalidArgument, "volume: no frames");

    std::uint32_t flags = 0;
    bool synthetic = false;
    for (const auto& f : frames)
        if (!f.plane.has_position || !f.plane.has_orientation) synthetic = true;
    if (synthetic) {
        flags |= kSyntheticGeometry;
        add_warning(warnings, "Image Position / Orientation (Patient) missing: geometry is synthetic (input order, header spacing)");
    }

    // ---- 2. phases
    PhaseSplit split = split_phases(frames, !synthetic);
    const auto phase_count = static_cast<std::uint32_t>(split.groups.size());
    if (opt.phase >= phase_count) return Status(Errc::InvalidArgument, "volume: phase index out of range");
    if (phase_count > 1) flags |= kMultiPhase;
    std::vector<std::size_t> sel = std::move(split.groups[opt.phase]);

    // ---- 3. consistency inside the phase
    const FrameRec& ref = frames[sel[0]];
    const image::PixelInfo& ref_pi = sources[ref.src].fr->info();
    const std::uint32_t nx = ref_pi.columns, ny = ref_pi.rows;
    for (std::size_t i : sel) {
        const FrameRec& f = frames[i];
        const image::PixelInfo& pi = sources[f.src].fr->info();
        if (pi.rows != ny || pi.columns != nx) return Status(Errc::Unsupported, "volume: frames have different Rows / Columns");
        if (!synthetic) {
            const double tol = opt.orientation_tolerance;
            const Vec3 dr = f.plane.row_dir - ref.plane.row_dir, dc = f.plane.col_dir - ref.plane.col_dir;
            if (std::fabs(dr.x) > tol || std::fabs(dr.y) > tol || std::fabs(dr.z) > tol || std::fabs(dc.x) > tol ||
                std::fabs(dc.y) > tol || std::fabs(dc.z) > tol)
                return Status(Errc::Unsupported, "volume: frames have different orientations (mixed orientation)");
        }
        if (f.plane.has_spacing != ref.plane.has_spacing ||
            std::fabs(f.plane.row_spacing - ref.plane.row_spacing) > 1e-3 * ref.plane.row_spacing ||
            std::fabs(f.plane.col_spacing - ref.plane.col_spacing) > 1e-3 * ref.plane.col_spacing)
            return Status(Errc::Unsupported, "volume: frames have different pixel spacing");
    }
    if (!ref.plane.has_spacing) add_warning(warnings, "Pixel Spacing missing: 1 mm assumed");

    const Vec3 r_dir = ref.plane.row_dir, c_dir = ref.plane.col_dir;
    const Vec3 normal = ref.plane.normal();
    for (std::size_t i : sel) frames[i].d = dot(frames[i].plane.position, normal);

    // ---- 4. sort along the normal (synthetic: in-stack position, instance number, input order)
    auto tie = [&](const FrameRec& a, const FrameRec& b) {
        if (a.keys.in_stack_position && b.keys.in_stack_position && *a.keys.in_stack_position != *b.keys.in_stack_position)
            return *a.keys.in_stack_position < *b.keys.in_stack_position;
        if (a.keys.instance_number && b.keys.instance_number && *a.keys.instance_number != *b.keys.instance_number)
            return *a.keys.instance_number < *b.keys.instance_number;
        return a.order < b.order;
    };
    std::stable_sort(sel.begin(), sel.end(), [&](std::size_t ia, std::size_t ib) {
        const FrameRec& a = frames[ia];
        const FrameRec& b = frames[ib];
        if (!synthetic && std::fabs(a.d - b.d) > kPositionEps) return a.d < b.d;
        return tie(a, b);
    });

    // ---- 5. duplicates
    if (!synthetic) {
        std::vector<std::size_t> kept;
        kept.push_back(sel[0]);
        std::size_t dropped = 0;
        for (std::size_t i = 1; i < sel.size(); ++i) {
            if (std::fabs(frames[sel[i]].d - frames[kept.back()].d) <= kPositionEps) {
                ++dropped;
                continue;
            }
            kept.push_back(sel[i]);
        }
        if (dropped) {
            if (opt.strict) return Status(Errc::Unsupported, "volume: duplicate slice positions");
            flags |= kDuplicatePositions;
            add_warning(warnings, std::to_string(dropped) + " frame(s) at duplicate positions dropped");
            sel = std::move(kept);
        }
    }
    const auto nz = static_cast<std::uint32_t>(sel.size());

    // ---- 6. slice spacing, gaps, tilt
    Grid grid;
    grid.dims = {nx, ny, nz};
    grid.spacing[0] = ref.plane.col_spacing;
    grid.spacing[1] = ref.plane.row_spacing;
    grid.axes[0] = r_dir;
    grid.axes[1] = c_dir;
    grid.axes[2] = normal;
    grid.origin = synthetic ? (ref.plane.has_position ? ref.plane.position : Vec3{}) : frames[sel[0]].plane.position;
    const double header_spacing = ref.plane.spacing_between_slices.value_or(ref.plane.slice_thickness.value_or(1.0));
    double tilt_deg = 0.0;
    std::vector<std::pair<double, double>> shifts;  // per slice (i, j) shift for de-shearing
    if (synthetic || nz == 1) {
        grid.spacing[2] = header_spacing;
        if (nz == 1) {
            flags |= kSingleSlice;
            add_warning(warnings, "single slice: slice spacing taken from the header (" + fmt(header_spacing) + " mm)");
        }
    } else {
        std::vector<double> gaps;
        gaps.reserve(nz - 1u);
        for (std::uint32_t k = 1; k < nz; ++k) gaps.push_back(frames[sel[k]].d - frames[sel[k - 1]].d);
        const double nominal = *median(gaps);
        bool nonuniform = false, missing = false;
        for (double g : gaps) {
            if (std::fabs(g - nominal) > opt.spacing_tolerance * nominal + 1e-4) nonuniform = true;
            if (g > 1.5 * nominal) missing = true;
        }
        const double span = frames[sel[nz - 1]].d - frames[sel[0]].d;
        const double mean = span / (nz - 1);
        if (nonuniform || missing) {
            if (opt.strict) return Status(Errc::Unsupported, "volume: slice spacing is not uniform");
            if (nonuniform) flags |= kNonUniformSpacing;
            if (missing) flags |= kMissingSlices;
            add_warning(warnings, std::string(missing ? "missing slices: " : "non-uniform slice spacing: ") + "gaps from " +
                                      fmt(*std::min_element(gaps.begin(), gaps.end())) + " to " +
                                      fmt(*std::max_element(gaps.begin(), gaps.end())) + " mm; the volume uses the mean spacing " +
                                      fmt(mean) + " mm, so slice positions are approximate");
        }
        const Vec3 p0 = frames[sel[0]].plane.position, pn = frames[sel[nz - 1]].plane.position;
        const Vec3 shift = (pn - p0) * (1.0 / (nz - 1));
        const double slen = length(shift);
        const double cosang = std::clamp(dot(shift, normal) / slen, -1.0, 1.0);
        tilt_deg = std::acos(cosang) * 180.0 / 3.14159265358979323846;
        if (tilt_deg > opt.tilt_tolerance_degrees) {
            flags |= kGantryTilt;
            if (opt.deshear_tilt) {
                flags |= kDesheared;
                grid.spacing[2] = mean;
                shifts.resize(nz);
                for (std::uint32_t k = 0; k < nz; ++k) {
                    const Vec3 dp = p0 - frames[sel[k]].plane.position;
                    shifts[k] = {dot(dp, r_dir) / grid.spacing[0], dot(dp, c_dir) / grid.spacing[1]};
                }
                add_warning(warnings, "gantry tilt " + fmt(tilt_deg) + " degrees: resampled onto an orthogonal grid");
            } else {
                grid.axes[2] = shift * (1.0 / slen);
                grid.spacing[2] = slen;
                add_warning(warnings, "gantry tilt " + fmt(tilt_deg) + " degrees: kept as a sheared grid");
            }
        } else {
            grid.spacing[2] = mean;
        }
    }

    // ---- 7. voxel type
    VoxelType type = VoxelType::Float32;
    bool tentative = false;  // int16 chosen on integral rescale but a value might not fit
    if (opt.type == VoxelTypeRequest::Int16) {
        type = VoxelType::Int16;
    } else if (opt.type == VoxelTypeRequest::Auto) {
        bool integral = true, fits = true;
        for (std::size_t i : sel) {
            const FrameRec& f = frames[i];
            const image::PixelInfo& pi = sources[f.src].fr->info();
            if (pi.kind != image::SampleKind::Integer) integral = false;
            if (!f.mod.lut && (std::trunc(f.mod.slope) != f.mod.slope || std::trunc(f.mod.intercept) != f.mod.intercept))
                integral = false;
            if (f.mod.out_lo < -32768.0 || f.mod.out_hi > 32767.0) fits = false;
        }
        if (integral && shifts.empty()) {
            type = VoxelType::Int16;
            tentative = !fits;
        }
    }

    // ---- 8. decode
    const std::size_t slice = static_cast<std::size_t>(nx) * ny;
    std::uint64_t max_frame_bytes = 0;
    for (std::size_t i : sel) max_frame_bytes = std::max(max_frame_bytes, sources[frames[i].src].fr->frame_bytes());
    if (max_frame_bytes > SIZE_MAX / 4) return Status(Errc::LimitExceeded, "volume: frame too large");

    for (int attempt = 0;; ++attempt) {  // at most two: int16, then float32 when values did not fit
        auto vr = Volume::allocate(grid, type, opt.memory_budget);
        if (!vr) return vr.take_error();
        Volume& vol = *vr;
        const unsigned workers = threads;
        std::vector<std::vector<std::uint8_t>> raw(workers);
        std::vector<std::vector<float>> fbuf(workers), fbuf2(workers);
        std::vector<detail::Int16Counts> counts;
        if (type == VoxelType::Int16) {
            try {
                counts.resize(workers);
            } catch (const std::bad_alloc&) {
                return Status(Errc::OutOfMemory, "volume: out of memory");
            }
            for (auto& c : counts)
                if (!c.init()) return Status(Errc::OutOfMemory, "volume: out of memory");
        }
        std::vector<Status> slice_err;
        std::vector<char> slice_clamped;
        try {
            slice_err.resize(nz);
            slice_clamped.assign(nz, 0);
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "volume: out of memory");
        }
        detail::parallel_for(nz, workers, [&](std::uint64_t k64, unsigned w) noexcept {
            const auto k = static_cast<std::uint32_t>(k64);
            const FrameRec& f = frames[sel[k]];
            const Source& s = sources[f.src];
            const image::FrameReader& fr = *s.fr;
            try {
                const std::uint8_t* src = nullptr;
                if (auto v = fr.native_view(f.frame)) {
                    src = v->data();
                } else {
                    auto& b = raw[w];
                    if (b.size() < fr.frame_bytes()) b.resize(static_cast<std::size_t>(max_frame_bytes));
                    if (Status st = fr.decode(f.frame, std::span<std::uint8_t>(b.data(), static_cast<std::size_t>(fr.frame_bytes())));
                        !st) {
                        slice_err[k] = std::move(st);
                        return;
                    }
                    src = b.data();
                }
                image::ModalityTransform m;
                m.has_lut = false;
                m.slope = f.mod.slope;
                m.intercept = f.mod.intercept;
                if (f.mod.lut) {
                    auto mm = image::modality_for_frame(s.file->dataset, fr.info(), f.frame, opt.lenient);
                    if (!mm) {
                        slice_err[k] = mm.take_error();
                        return;
                    }
                    m = std::move(mm).value();
                }
                const std::size_t base = static_cast<std::size_t>(k) * slice;
                if (fbuf[w].size() < slice) fbuf[w].resize(slice);
                if (!shifts.empty()) {
                    // De-shear: modality values, then the in-plane shift (fill: slice minimum).
                    if (fbuf2[w].size() < slice) fbuf2[w].resize(slice);
                    frame_to_float(fr.info(), src, m, fbuf[w].data(), slice);
                    float fill = std::numeric_limits<float>::infinity();
                    for (std::size_t i = 0; i < slice; ++i)
                        if (std::isfinite(fbuf[w][i])) fill = std::min(fill, fbuf[w][i]);
                    if (!std::isfinite(fill)) fill = 0.0f;
                    if (type == VoxelType::Float32) {
                        shift_slice(fbuf[w].data(), vol.f32() + base, nx, ny, shifts[k].first, shifts[k].second, fill);
                    } else {
                        shift_slice(fbuf[w].data(), fbuf2[w].data(), nx, ny, shifts[k].first, shifts[k].second, fill);
                        for (std::size_t i = 0; i < slice; ++i) {
                            const float c = std::clamp(std::nearbyint(fbuf2[w][i]), -32768.0f, 32767.0f);
                            if (c != fbuf2[w][i]) slice_clamped[k] = 1;
                            vol.i16()[base + i] = static_cast<std::int16_t>(c);
                        }
                    }
                } else if (type == VoxelType::Int16) {
                    if (!frame_to_int16(fr.info(), src, m, vol.i16() + base, slice, fbuf[w].data())) slice_clamped[k] = 1;
                } else {
                    frame_to_float(fr.info(), src, m, vol.f32() + base, slice);
                }
                if (type == VoxelType::Int16) counts[w].add(vol.i16() + base, slice);  // while the slice is in cache
            } catch (const std::bad_alloc&) {
                slice_err[k] = Status(Errc::OutOfMemory, "volume: out of memory");
            }
        });
        for (auto& e : slice_err)
            if (!e) return e;
        const bool clamped = std::any_of(slice_clamped.begin(), slice_clamped.end(), [](char c) { return c != 0; });
        if (clamped && tentative && attempt == 0) {
            type = VoxelType::Float32;  // values do not fit int16 after all: decode again as float32
            continue;
        }
        if (clamped) {
            flags |= kValuesClamped;
            add_warning(warnings, "int16 output: values were rounded or clamped");
        }
        for (const auto& s : sources)
            for (const auto& w : s.fr->warnings()) add_warning(warnings, w);
        if (type == VoxelType::Int16) {
            const std::uint32_t bins = opt.histogram_bins ? std::min(opt.histogram_bins, 1u << 20) : 1u;
            try {
                vol.histogram.bins.assign(bins, 0);
                std::vector<const std::vector<std::uint64_t>*> parts;
                for (auto& c : counts) parts.push_back(&c.c);
                vol.set_int16_statistics(parts, bins);
            } catch (const std::bad_alloc&) {
                return Status(Errc::OutOfMemory, "volume: out of memory");
            }
        } else if (Status st = vol.update_statistics(threads, opt.histogram_bins); !st) {
            return st;
        }
        vol.flags = flags;
        vol.warnings = std::move(warnings);
        vol.phase_count = phase_count;
        vol.phase_index = opt.phase;
        vol.phase_key = split.key;
        vol.tilt_degrees = tilt_deg;
        vol.source_frames = nz;
        return vr;
    }
}

Result<Volume> build_volume_from_paths(std::span<const std::string> paths, const VolumeOptions& opt) {
    if (paths.empty()) return Status(Errc::InvalidArgument, "volume: no input files");
    if (paths.size() > opt.max_frames) return Status(Errc::LimitExceeded, "volume: too many files");
    std::vector<DicomFile> files;
    std::vector<Status> errors;
    std::vector<const DicomFile*> ptrs;
    try {
        files.resize(paths.size());
        errors.resize(paths.size());
        ptrs.resize(paths.size());
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "volume: out of memory");
    }
    ReadOptions ro;
    ro.lenient = opt.lenient;
    std::atomic<bool> stop{false};
    std::mutex progress_mu;
    std::uint64_t done = 0;
    detail::parallel_for(paths.size(), opt.threads, [&](std::uint64_t i64, unsigned) noexcept {
        const auto i = static_cast<std::size_t>(i64);  // < paths.size()
        try {
            if (stop.load()) {
                errors[i] = Status(Errc::Cancelled, "volume: cancelled");
                return;
            }
            auto f = read_file(paths[i], ro);
            if (!f) {
                errors[i] = f.take_error();
                return;
            }
            files[i] = std::move(f).value();
            if (opt.progress) {
                std::lock_guard<std::mutex> l(progress_mu);
                if (!stop.load() && !opt.progress(++done, paths.size())) stop.store(true);
            }
        } catch (const std::bad_alloc&) {
            errors[i] = Status(Errc::OutOfMemory, "volume: out of memory");
        } catch (...) {
            errors[i] = Status(Errc::Internal, "volume: internal error");
        }
    });
    if (stop.load()) return Status(Errc::Cancelled, "volume: cancelled");
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!errors[i]) {
            Status s = errors[i];
            s.message += " (input file " + std::to_string(i) + ")";
            return s;
        }
        ptrs[i] = &files[i];
    }
    return build_volume(ptrs, opt);
}

}  // namespace siaara::volume
