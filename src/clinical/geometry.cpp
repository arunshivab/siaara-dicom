// SIAARA DICOM Toolkit - image plane geometry (see geometry.hpp for the standard references).
#include "clinical/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <string_view>
#include <tuple>

#include "clinical/common.hpp"

namespace siaara::clinical {

namespace {

constexpr Tag kNumberOfFrames = 0x00280008;
constexpr Tag kPerFrame = 0x52009230;
constexpr Tag kRows = 0x00280010, kColumns = 0x00280011;

bool positive_pair(const std::optional<std::vector<double>>& v) noexcept {
    return v && v->size() == 2 && (*v)[0] > 0.0 && (*v)[1] > 0.0 && std::isfinite((*v)[0]) && std::isfinite((*v)[1]);
}

bool nearly_equal(double a, double b) noexcept { return std::fabs(a - b) <= 1e-6 * std::max(std::fabs(a), std::fabs(b)); }

}  // namespace

// ---------------------------------------------------------------- plane maths
Vec3 PlaneGeometry::pixel_to_patient(double col, double row) const noexcept {
    // Equation C.7.6.2.1-1: P = S + X * delta_i * i + Y * delta_j * j, i = column, j = row,
    // delta_i = column spacing (Pixel Spacing value 2), delta_j = row spacing (value 1).
    return position + row_cosine * (spacing.column * col) + column_cosine * (spacing.row * row);
}

bool PlaneGeometry::patient_to_pixel(const Vec3& p, double& col, double& row, double* distance) const noexcept {
    if (!complete()) return false;
    const Vec3 d = p - position;
    // Solve [X.X X.Y; X.Y Y.Y] [a b]^T = [d.X d.Y]^T (exact for non-orthogonal cosines too).
    const double xx = row_cosine.dot(row_cosine), yy = column_cosine.dot(column_cosine), xy = row_cosine.dot(column_cosine);
    const double det = xx * yy - xy * xy;
    if (std::fabs(det) < 1e-12) return false;
    const double dx = d.dot(row_cosine), dy = d.dot(column_cosine);
    const double a = (dx * yy - dy * xy) / det;
    const double b = (dy * xx - dx * xy) / det;
    col = a / spacing.column;
    row = b / spacing.row;
    if (distance) *distance = d.dot(normal);
    return true;
}

// ---------------------------------------------------------------- spacing
PixelSpacingInfo resolve_pixel_spacing(const Dataset& ds, std::uint32_t frame, const double* us_point) {
    PixelSpacingInfo info;
    bool per = false;
    if (const Dataset* pm = functional_group(ds, frame, tags::PixelMeasuresSequence, &per)) {
        auto ps = get_numbers(*pm, tags::PixelSpacing, 4);
        if (positive_pair(ps)) {
            info.row = (*ps)[0];
            info.column = (*ps)[1];
            info.source = SpacingSource::PixelSpacing;
            info.from = per ? GeometrySource::PerFrame : GeometrySource::Shared;
            return info;
        }
    }
    const auto ps = get_numbers(ds, tags::PixelSpacing, 4);
    const auto ips = get_numbers(ds, tags::ImagerPixelSpacing, 4);
    const auto ermf = ds.get_double(tags::EstimatedRadiographicMagnificationFactor);
    const bool have_ermf = ermf && std::isfinite(*ermf) && *ermf > 0.0;
    if (positive_pair(ps)) {
        info.row = (*ps)[0];
        info.column = (*ps)[1];
        info.source = SpacingSource::PixelSpacing;
        info.from = GeometrySource::Classic;
        if (auto ct = get_text(ds, tags::PixelSpacingCalibrationType)) {
            info.calibrated = true;
            info.calibration_type = *ct;
            return info;
        }
        // PS3.3 10.7.1.3: Pixel Spacing equal to Imager Pixel Spacing in a projection image is
        // the detector-plane spacing, not a calibrated patient spacing.
        if (positive_pair(ips) && nearly_equal((*ps)[0], (*ips)[0]) && nearly_equal((*ps)[1], (*ips)[1])) {
            info.at_detector = true;
            if (have_ermf) {
                info.row /= *ermf;
                info.column /= *ermf;
                info.magnification = *ermf;
                info.at_detector = false;
                info.source = SpacingSource::ImagerPixelSpacingMagnified;
            }
        }
        return info;
    }
    if (positive_pair(ips)) {
        info.row = (*ips)[0];
        info.column = (*ips)[1];
        info.from = GeometrySource::Classic;
        if (have_ermf) {
            info.row /= *ermf;
            info.column /= *ermf;
            info.magnification = *ermf;
            info.source = SpacingSource::ImagerPixelSpacingMagnified;
        } else {
            info.at_detector = true;
            info.source = SpacingSource::ImagerPixelSpacing;
        }
        return info;
    }
    const auto nsps = get_numbers(ds, tags::NominalScannedPixelSpacing, 4);
    if (positive_pair(nsps)) {
        info.row = (*nsps)[0];
        info.column = (*nsps)[1];
        info.source = SpacingSource::NominalScannedPixelSpacing;
        info.from = GeometrySource::Classic;
        return info;
    }
    // Sequence of Ultrasound Regions (C.8.5.5): 2D regions (Region Spatial Format 1) with both
    // physical units in cm (code 3); Physical Delta X/Y are cm per pixel.
    const auto& regions = items_of(ds, tags::SequenceOfUltrasoundRegions);
    int chosen = -1;
    bool ambiguous = false;
    double dx = 0, dy = 0;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const Dataset& r = regions[i];
        const auto fmt = r.get_int(tags::RegionSpatialFormat);
        const auto ux = r.get_int(tags::PhysicalUnitsXDirection), uy = r.get_int(tags::PhysicalUnitsYDirection);
        const auto px = r.get_double(tags::PhysicalDeltaX), py = r.get_double(tags::PhysicalDeltaY);
        if (!fmt || *fmt != 1 || !ux || !uy || *ux != 3 || *uy != 3 || !px || !py) continue;
        const double ax = std::fabs(*px), ay = std::fabs(*py);
        if (!(ax > 0.0) || !(ay > 0.0) || !std::isfinite(ax) || !std::isfinite(ay)) continue;
        if (us_point) {
            const auto x0 = r.get_int(tags::RegionLocationMinX0), y0 = r.get_int(tags::RegionLocationMinY0);
            const auto x1 = r.get_int(tags::RegionLocationMaxX1), y1 = r.get_int(tags::RegionLocationMaxY1);
            if (!x0 || !y0 || !x1 || !y1) continue;
            if (us_point[0] < static_cast<double>(*x0) - 0.5 || us_point[0] > static_cast<double>(*x1) + 0.5 ||
                us_point[1] < static_cast<double>(*y0) - 0.5 || us_point[1] > static_cast<double>(*y1) + 0.5)
                continue;
            chosen = static_cast<int>(i);
            dx = ax;
            dy = ay;
            ambiguous = false;
            break;
        }
        if (chosen < 0) {
            chosen = static_cast<int>(i);
            dx = ax;
            dy = ay;
        } else if (!nearly_equal(ax, dx) || !nearly_equal(ay, dy)) {
            ambiguous = true;
        }
    }
    if (chosen >= 0 && !ambiguous) {
        info.row = dy * 10.0;
        info.column = dx * 10.0;
        info.source = SpacingSource::UltrasoundRegion;
        info.from = GeometrySource::Classic;
        info.ultrasound_region = chosen;
    }
    return info;
}

// ---------------------------------------------------------------- frame geometry
Result<PlaneGeometry> frame_geometry(const Dataset& ds, std::uint32_t frame) {
    PlaneGeometry g;
    auto nf = ds.get_int(kNumberOfFrames);
    const std::int64_t frames = nf && *nf > 0 ? *nf : 1;
    if (static_cast<std::int64_t>(frame) >= frames)
        return Status(Errc::InvalidArgument, "frame index past Number of Frames");
    if (auto r = ds.get_int(kRows); r && *r > 0) g.rows = static_cast<std::uint32_t>(*r);
    if (auto c = ds.get_int(kColumns); c && *c > 0) g.columns = static_cast<std::uint32_t>(*c);
    if (auto f = get_text(ds, tags::FrameOfReferenceUid)) g.frame_of_reference_uid = *f;

    bool per = false;
    std::optional<std::vector<double>> ipp, iop;
    if (const Dataset* pp = functional_group(ds, frame, tags::PlanePositionSequence, &per)) {
        ipp = get_numbers_n(*pp, tags::ImagePositionPatient, 3);
        if (ipp) g.position_from = per ? GeometrySource::PerFrame : GeometrySource::Shared;
    }
    if (const Dataset* po = functional_group(ds, frame, tags::PlaneOrientationSequence, &per)) {
        iop = get_numbers_n(*po, tags::ImageOrientationPatient, 6);
        if (iop) g.orientation_from = per ? GeometrySource::PerFrame : GeometrySource::Shared;
    }
    if (!ipp) {
        ipp = get_numbers_n(ds, tags::ImagePositionPatient, 3);
        if (ipp) g.position_from = GeometrySource::Classic;
    }
    if (!iop) {
        iop = get_numbers_n(ds, tags::ImageOrientationPatient, 6);
        if (iop) g.orientation_from = GeometrySource::Classic;
    }
    if (ipp) {
        const auto& v = *ipp;
        if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
            g.position = {v[0], v[1], v[2]};
            g.has_position = true;
        }
    }
    if (iop) {
        const auto& v = *iop;
        const Vec3 r{v[0], v[1], v[2]}, c{v[3], v[4], v[5]};
        const double rn = r.norm(), cn = c.norm();
        if (std::isfinite(rn) && std::isfinite(cn) && rn > 1e-6 && cn > 1e-6) {
            if (std::fabs(rn - 1.0) > 1e-3 || std::fabs(cn - 1.0) > 1e-3)
                g.warnings.push_back("Image Orientation (Patient) cosines are not unit vectors; normalised");
            g.row_cosine = r.normalized();
            g.column_cosine = c.normalized();
            const double d = g.row_cosine.dot(g.column_cosine);
            if (std::fabs(d) > 1e-4) {
                g.orthogonal = false;
                g.warnings.push_back("Image Orientation (Patient) cosines are not orthogonal");
            }
            const Vec3 n = g.row_cosine.cross(g.column_cosine);
            if (n.norm() > 1e-6) {
                g.normal = n.normalized();
                g.has_orientation = true;
            } else {
                g.warnings.push_back("Image Orientation (Patient) cosines are parallel");
            }
        } else {
            g.warnings.push_back("Image Orientation (Patient) has a zero cosine");
        }
    }
    g.spacing = resolve_pixel_spacing(ds, frame);
    if (const Dataset* pm = functional_group(ds, frame, tags::PixelMeasuresSequence)) {
        if (auto st = pm->get_double(tags::SliceThickness)) g.slice_thickness = *st;
        if (auto sb = pm->get_double(tags::SpacingBetweenSlices)) g.spacing_between_slices = *sb;
    }
    if (!g.slice_thickness)
        if (auto st = ds.get_double(tags::SliceThickness)) g.slice_thickness = *st;
    if (!g.spacing_between_slices)
        if (auto sb = ds.get_double(tags::SpacingBetweenSlices)) g.spacing_between_slices = *sb;
    return g;
}

// ---------------------------------------------------------------- labels
std::string orientation_label(const Vec3& d, double threshold, std::size_t max_letters) {
    struct Comp {
        double mag;
        char letter;
    };
    Comp c[3] = {{std::fabs(d.x), d.x > 0 ? 'L' : 'R'}, {std::fabs(d.y), d.y > 0 ? 'P' : 'A'},
                 {std::fabs(d.z), d.z > 0 ? 'H' : 'F'}};
    // Stable ordering by magnitude (ties keep x, y, z order).
    std::stable_sort(c, c + 3, [](const Comp& a, const Comp& b) { return a.mag > b.mag; });
    std::string out;
    for (const auto& k : c) {
        if (out.size() >= max_letters) break;
        if (!(k.mag > threshold)) break;
        out.push_back(k.letter);
    }
    return out;
}

std::optional<EdgeLabels> edge_labels(const PlaneGeometry& g, double threshold) {
    if (!g.has_orientation) return std::nullopt;
    EdgeLabels e;
    e.right = orientation_label(g.row_cosine, threshold);
    e.left = orientation_label(-g.row_cosine, threshold);
    e.bottom = orientation_label(g.column_cosine, threshold);
    e.top = orientation_label(-g.column_cosine, threshold);
    return e;
}

// ---------------------------------------------------------------- reference lines
namespace {
// Clips the parametric segment p(t) = a + t (b - a), t in [t0, t1], against lo <= p <= hi on one
// axis (Liang-Barsky). Returns false when nothing remains.
bool clip_axis(double a, double b, double lo, double hi, double& t0, double& t1) noexcept {
    const double d = b - a;
    if (std::fabs(d) < 1e-15) return a >= lo && a <= hi;
    double ta = (lo - a) / d, tb = (hi - a) / d;
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    return t0 <= t1;
}
}  // namespace

ReferenceLine reference_line(const PlaneGeometry& t, const PlaneGeometry& o, bool clip_to_other,
                             bool allow_other_frame_of_reference) {
    ReferenceLine out;
    out.same_frame_of_reference = !t.frame_of_reference_uid.empty() && t.frame_of_reference_uid == o.frame_of_reference_uid;
    if (!t.complete() || !o.complete() || t.rows == 0 || t.columns == 0) {
        out.reason = "incomplete geometry";
        return out;
    }
    if (!out.same_frame_of_reference && !allow_other_frame_of_reference) {
        out.reason = "different or missing Frame of Reference UID";
        return out;
    }
    if (t.normal.cross(o.normal).norm() < 1e-6) {
        out.reason = "planes are parallel";
        return out;
    }
    // Target pixel (c, r) lies on the other plane when a c + b r + k = 0.
    const double a = t.spacing.column * t.row_cosine.dot(o.normal);
    const double b = t.spacing.row * t.column_cosine.dot(o.normal);
    const double k = (t.position - o.position).dot(o.normal);
    const double cmin = -0.5, cmax = t.columns - 0.5, rmin = -0.5, rmax = t.rows - 0.5;
    // Two far-apart points on the line, then clip to the target rectangle.
    double c0, r0, c1, r1;
    const double big = 4.0 * (static_cast<double>(t.columns) + t.rows + 2.0);
    if (std::fabs(a) >= std::fabs(b)) {  // solve for c
        r0 = rmin - big;
        r1 = rmax + big;
        c0 = -(b * r0 + k) / a;
        c1 = -(b * r1 + k) / a;
    } else {
        c0 = cmin - big;
        c1 = cmax + big;
        r0 = -(a * c0 + k) / b;
        r1 = -(a * c1 + k) / b;
    }
    double t0 = 0.0, t1 = 1.0;
    if (!clip_axis(c0, c1, cmin, cmax, t0, t1) || !clip_axis(r0, r1, rmin, rmax, t0, t1)) {
        out.reason = "intersection is outside the target image";
        return out;
    }
    if (clip_to_other) {
        if (o.rows == 0 || o.columns == 0) {
            out.reason = "other image has no size";
            return out;
        }
        // Map the two end points into the other image's pixel space (an affine map of t).
        const Vec3 pa = t.pixel_to_patient(c0, r0), pb = t.pixel_to_patient(c1, r1);
        double oca, ora, ocb, orb;
        if (!o.patient_to_pixel(pa, oca, ora) || !o.patient_to_pixel(pb, ocb, orb)) {
            out.reason = "degenerate geometry";
            return out;
        }
        if (!clip_axis(oca, ocb, -0.5, o.columns - 0.5, t0, t1) || !clip_axis(ora, orb, -0.5, o.rows - 0.5, t0, t1)) {
            out.reason = "planes intersect outside the other image";
            return out;
        }
    }
    out.col0 = c0 + t0 * (c1 - c0);
    out.row0 = r0 + t0 * (r1 - r0);
    out.col1 = c0 + t1 * (c1 - c0);
    out.row1 = r0 + t1 * (r1 - r0);
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------- series sorting
namespace {

std::optional<double> acquisition_time_of(const Dataset& ds, std::uint32_t frame) {
    if (const Dataset* fc = functional_group(ds, frame, tags::FrameContentSequence)) {
        if (auto dt = get_text(*fc, tags::FrameAcquisitionDateTime))
            if (auto v = parse_dt(*dt)) return v;
    }
    if (auto dt = get_text(ds, tags::AcquisitionDateTime))
        if (auto v = parse_dt(*dt)) return v;
    auto tm = get_text(ds, tags::AcquisitionTime);
    if (!tm) return std::nullopt;
    if (auto da = get_text(ds, tags::AcquisitionDate))
        if (auto v = combine_da_tm(*da, *tm)) return v;
    return parse_tm(*tm);
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

bool number_less(const std::optional<std::int64_t>& a, const std::optional<std::int64_t>& b) {
    if (a && b) return *a < *b;
    return a.has_value() && !b.has_value();
}
bool time_less(const std::optional<double>& a, const std::optional<double>& b) {
    if (a && b) return *a < *b;
    return a.has_value() && !b.has_value();
}

}  // namespace

SeriesSort sort_series(std::span<const Dataset* const> instances, const SeriesSortOptions& opt) {
    SeriesSort out;
    // ---- 1. items
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const Dataset* ds = instances[i];
        if (!ds) continue;
        std::uint32_t frames = 1;
        if (opt.expand_frames && ds->contains(kPerFrame)) {
            auto nf = ds->get_int(kNumberOfFrames);
            if (nf && *nf > 1) frames = static_cast<std::uint32_t>(std::min<std::int64_t>(*nf, 1 << 20));
        }
        const auto inum = ds->get_int(tags::InstanceNumber);
        for (std::uint32_t f = 0; f < frames; ++f) {
            SortItem it;
            it.instance = static_cast<std::uint32_t>(i);
            it.frame = f;
            auto g = frame_geometry(*ds, f);
            if (g) it.geometry = std::move(g).value();
            it.instance_number = inum;
            it.acquisition_time = acquisition_time_of(*ds, f);
            if (const Dataset* fc = functional_group(*ds, f, tags::FrameContentSequence)) {
                if (auto tpi = fc->get_double(tags::TemporalPositionIndex)) it.temporal_index = *tpi;
                if (auto sid = get_text(*fc, tags::StackId)) it.stack_id = *sid;
                if (auto isp = fc->get_int(tags::InStackPositionNumber)) it.in_stack_position = *isp;
            }
            if (const Dataset* ct = functional_group(*ds, f, 0x00189118 /* Cardiac Synchronization Sequence */))
                if (auto tt = ct->get_double(tags::NominalCardiacTriggerDelayTime)) it.trigger_time = *tt;
            if (!it.temporal_index)
                if (auto tp = ds->get_double(tags::TemporalPositionIdentifier)) it.temporal_index = *tp;
            if (!it.trigger_time)
                if (auto tt = ds->get_double(tags::TriggerTime)) it.trigger_time = *tt;
            out.items.push_back(std::move(it));
        }
    }
    if (out.items.empty()) return out;

    // ---- 2. global checks
    {
        std::string forf;
        bool first = true;
        for (const auto& it : out.items) {
            if (it.geometry.frame_of_reference_uid.empty()) continue;
            if (first) {
                forf = it.geometry.frame_of_reference_uid;
                first = false;
            } else if (forf != it.geometry.frame_of_reference_uid) {
                out.warnings |= kSortMultipleFramesOfReference;
            }
        }
        const auto& f0 = out.items.front().geometry;
        for (const auto& it : out.items)
            if (it.geometry.rows != f0.rows || it.geometry.columns != f0.columns) out.warnings |= kSortSizeMismatch;
    }
    bool explicit_temporal = false;
    {
        std::optional<double> first;
        for (const auto& it : out.items) {
            if (!it.temporal_index) continue;
            if (!first) first = it.temporal_index;
            else if (*first != *it.temporal_index) explicit_temporal = true;
        }
    }

    // ---- 3. partition: (has geometry, stack id, temporal key, orientation cluster)
    std::vector<Vec3> normals;  // orientation clusters
    struct Key {
        int geometry;  // 0 = with geometry, 1 = without
        std::string stack;
        double temporal;
        bool has_temporal;
        int orientation;
        bool operator<(const Key& o) const {
            return std::tie(geometry, stack, has_temporal, temporal, orientation) <
                   std::tie(o.geometry, o.stack, o.has_temporal, o.temporal, o.orientation);
        }
    };
    std::map<Key, std::vector<std::uint32_t>> parts;
    for (std::uint32_t i = 0; i < out.items.size(); ++i) {
        const auto& it = out.items[i];
        const bool geo = it.geometry.has_position && it.geometry.has_orientation;
        Key k{geo ? 0 : 1, it.stack_id, 0.0, false, -1};
        if (explicit_temporal && it.temporal_index) {
            k.temporal = *it.temporal_index;
            k.has_temporal = true;
        }
        if (geo) {
            int found = -1;
            for (std::size_t n = 0; n < normals.size(); ++n)
                if (1.0 - std::fabs(normals[n].dot(it.geometry.normal)) <= opt.orientation_tolerance) {
                    found = static_cast<int>(n);
                    break;
                }
            if (found < 0) {
                found = static_cast<int>(normals.size());
                normals.push_back(it.geometry.normal);
            }
            k.orientation = found;
        } else {
            out.warnings |= kSortMissingGeometry;
        }
        parts[k].push_back(i);
    }
    if (normals.size() > 1) out.warnings |= kSortMixedOrientation | kSortMultipleStacks;
    if (explicit_temporal) out.warnings |= kSortTemporalSplit;
    {
        std::string s0;
        bool first = true;
        for (const auto& it : out.items) {
            if (first) {
                s0 = it.stack_id;
                first = false;
            } else if (it.stack_id != s0) {
                out.warnings |= kSortMultipleStacks;
            }
        }
    }

    auto position_of = [&](const SortItem& it, const Vec3& n) { return it.geometry.position.dot(n); };

    // Orders a group and fills its statistics.
    auto finish_group = [&](SortGroup& g) {
        if (!g.geometry) {
            std::stable_sort(g.items.begin(), g.items.end(), [&](std::uint32_t a, std::uint32_t b) {
                const auto& A = out.items[a];
                const auto& B = out.items[b];
                if (number_less(A.instance_number, B.instance_number)) return true;
                if (number_less(B.instance_number, A.instance_number)) return false;
                return time_less(A.acquisition_time, B.acquisition_time);
            });
            return;
        }
        for (auto i : g.items) out.items[i].position = position_of(out.items[i], g.normal);
        std::stable_sort(g.items.begin(), g.items.end(), [&](std::uint32_t a, std::uint32_t b) {
            const auto& A = out.items[a];
            const auto& B = out.items[b];
            if (std::fabs(A.position - B.position) > opt.duplicate_tolerance) return A.position < B.position;
            if (number_less(A.instance_number, B.instance_number)) return true;
            if (number_less(B.instance_number, A.instance_number)) return false;
            if (time_less(A.acquisition_time, B.acquisition_time)) return true;
            if (time_less(B.acquisition_time, A.acquisition_time)) return false;
            return A.frame < B.frame;
        });
        std::vector<double> gaps;
        for (std::size_t k = 1; k < g.items.size(); ++k) {
            const double d = out.items[g.items[k]].position - out.items[g.items[k - 1]].position;
            if (d <= opt.duplicate_tolerance) g.duplicates = true;
            else gaps.push_back(d);
        }
        g.spacing = median(gaps);
        const double tol = std::max(opt.spacing_tolerance_abs, opt.spacing_tolerance_rel * g.spacing);
        for (double d : gaps)
            if (std::fabs(d - g.spacing) > tol) g.uneven = true;
        if (g.items.size() >= 2) {
            const Vec3 a = out.items[g.items.front()].geometry.position;
            const Vec3 b = out.items[g.items.back()].geometry.position;
            const Vec3 d = b - a;
            const double len = d.norm();
            if (len > opt.duplicate_tolerance) {
                const double c = std::min(1.0, std::fabs(d.dot(g.normal)) / len);
                g.tilt_deg = std::acos(c) * 180.0 / 3.14159265358979323846;
                if (g.tilt_deg > opt.tilt_tolerance_deg) g.tilted = true;
            }
        }
        for (auto i : g.items) {
            const auto* ds = instances[out.items[i].instance];
            if (auto gt = ds->get_double(tags::GantryDetectorTilt); gt && std::fabs(*gt) > 1e-6) g.tilted = true;
        }
    };

    for (auto& [key, members] : parts) {
        SortGroup base;
        base.stack_id = key.stack;
        if (key.has_temporal) base.temporal_key = key.temporal;
        base.geometry = key.geometry == 0;
        if (!base.geometry) {
            base.items = members;
            finish_group(base);
            out.groups.push_back(std::move(base));
            continue;
        }
        base.normal = normals[static_cast<std::size_t>(key.orientation)];
        // Parallel stacks: positions that are not on one line split by in-plane offset.
        std::vector<std::vector<std::uint32_t>> stacks;
        {
            std::vector<std::uint32_t> sorted = members;
            for (auto i : sorted) out.items[i].position = position_of(out.items[i], base.normal);
            std::stable_sort(sorted.begin(), sorted.end(),
                             [&](std::uint32_t a, std::uint32_t b) { return out.items[a].position < out.items[b].position; });
            const Vec3 a = out.items[sorted.front()].geometry.position;
            const Vec3 b = out.items[sorted.back()].geometry.position;
            const Vec3 dir = b - a;
            const double len = dir.norm();
            bool collinear = true;
            for (auto i : sorted) {
                const Vec3 p = out.items[i].geometry.position - a;
                double off;
                if (len > opt.duplicate_tolerance) off = p.cross(dir).norm() / len;
                else off = (p - base.normal * p.dot(base.normal)).norm();
                if (off > opt.stack_offset_tolerance) {
                    collinear = false;
                    break;
                }
            }
            if (collinear) {
                stacks.push_back(std::move(sorted));
            } else {
                std::vector<Vec3> offsets;
                for (auto i : members) {
                    const Vec3 p = out.items[i].geometry.position;
                    const Vec3 inplane = p - base.normal * p.dot(base.normal);
                    std::size_t s = 0;
                    for (; s < offsets.size(); ++s)
                        if ((offsets[s] - inplane).norm() <= opt.stack_offset_tolerance) break;
                    if (s == offsets.size()) {
                        offsets.push_back(inplane);
                        stacks.emplace_back();
                    }
                    stacks[s].push_back(i);
                }
                out.warnings |= kSortMultipleStacks;
            }
        }
        for (auto& st : stacks) {
            // Repeated positions told apart by trigger time: a cardiac 4D series without
            // Temporal Position Index; split it per trigger time.
            SortGroup g = base;
            g.items = st;
            finish_group(g);
            bool split = false;
            if (g.duplicates && !key.has_temporal) {
                std::map<double, std::vector<std::uint32_t>> by_trigger;
                bool all = true;
                for (auto i : g.items) {
                    if (!out.items[i].trigger_time) {
                        all = false;
                        break;
                    }
                    by_trigger[*out.items[i].trigger_time].push_back(i);
                }
                if (all && by_trigger.size() > 1) {
                    std::vector<SortGroup> sub;
                    bool still_dup = false;
                    for (auto& [tt, mem] : by_trigger) {
                        SortGroup s = base;
                        s.items = mem;
                        s.temporal_key = tt;
                        s.temporal_by_trigger = true;
                        finish_group(s);
                        if (s.duplicates) still_dup = true;
                        sub.push_back(std::move(s));
                    }
                    if (!still_dup) {
                        split = true;
                        out.warnings |= kSortTemporalSplit;
                        for (auto& s : sub) out.groups.push_back(std::move(s));
                    }
                }
            }
            if (!split) out.groups.push_back(std::move(g));
        }
    }
    // Group-derived warnings come from the final groups (a trigger-time split removes the
    // duplicates it explains).
    for (const auto& g : out.groups) {
        if (g.duplicates) out.warnings |= kSortDuplicatePositions;
        if (g.uneven) out.warnings |= kSortUnevenSpacing;
        if (g.tilted) out.warnings |= kSortGantryTilt;
    }
    // Groups with geometry first (map order), then by temporal key within a stack; finally the
    // overall order.
    std::stable_sort(out.groups.begin(), out.groups.end(), [](const SortGroup& a, const SortGroup& b) {
        if (a.geometry != b.geometry) return a.geometry;
        if (a.stack_id != b.stack_id) return a.stack_id < b.stack_id;
        const bool ha = a.temporal_key.has_value(), hb = b.temporal_key.has_value();
        if (ha != hb) return hb;
        if (ha && *a.temporal_key != *b.temporal_key) return *a.temporal_key < *b.temporal_key;
        return false;
    });
    for (const auto& g : out.groups) out.order.insert(out.order.end(), g.items.begin(), g.items.end());
    return out;
}

}  // namespace siaara::clinical
