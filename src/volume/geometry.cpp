// SIAARA DICOM Toolkit - minimal patient-space geometry (see geometry.hpp).
#include "volume/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace siaara::volume {

std::optional<Vec3> normalized(Vec3 a) noexcept {
    if (!finite(a)) return std::nullopt;
    const double l = length(a);
    if (!(l > 1e-12) || !std::isfinite(l)) return std::nullopt;
    return a * (1.0 / l);
}

Mat3 Mat3::from_columns(Vec3 a, Vec3 b, Vec3 c) noexcept {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        r.m[i][0] = a[i];
        r.m[i][1] = b[i];
        r.m[i][2] = c[i];
    }
    return r;
}

Mat3 Mat3::transposed() const noexcept {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = m[j][i];
    return r;
}

double Mat3::determinant() const noexcept {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

std::optional<Mat3> Mat3::inverse() const noexcept {
    // Cofactor inverse. Singular when |det| is negligible against the product of column norms.
    double scale = 1.0;
    for (int c = 0; c < 3; ++c) scale *= length(col(c));
    const double det = determinant();
    if (!std::isfinite(det) || !(scale > 0.0) || std::fabs(det) <= 1e-9 * scale) return std::nullopt;
    Mat3 r;
    const double inv = 1.0 / det;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    return r;
}

bool Grid::voxel_count(std::uint64_t& out) const noexcept {
    std::uint64_t a = 0;
    if (!checked_mul(dims[0], dims[1], a)) return false;
    return checked_mul(a, dims[2], out);
}

Mat3 Grid::index_to_patient_matrix() const noexcept {
    return Mat3::from_columns(axes[0] * spacing[0], axes[1] * spacing[1], axes[2] * spacing[2]);
}

Status Grid::validate() const {
    for (int i = 0; i < 3; ++i) {
        if (dims[static_cast<std::size_t>(i)] == 0) return Status(Errc::InvalidArgument, "volume: zero dimension");
        const double s = spacing[static_cast<std::size_t>(i)];
        if (!std::isfinite(s) || !(s > 0.0)) return Status(Errc::InvalidArgument, "volume: spacing must be finite and positive");
        const Vec3 a = axes[static_cast<std::size_t>(i)];
        if (!finite(a) || std::fabs(length(a) - 1.0) > 1e-6) return Status(Errc::InvalidArgument, "volume: axes must be unit vectors");
    }
    if (!finite(origin)) return Status(Errc::InvalidArgument, "volume: origin must be finite");
    if (!index_to_patient_matrix().inverse()) return Status(Errc::InvalidArgument, "volume: axes are degenerate");
    std::uint64_t n = 0;
    if (!voxel_count(n)) return Status(Errc::LimitExceeded, "volume: voxel count overflows");
    return Status::ok();
}

std::optional<GridTransform> GridTransform::make(const Grid& g) noexcept {
    GridTransform t;
    t.to_patient = g.index_to_patient_matrix();
    auto inv = t.to_patient.inverse();
    if (!inv) return std::nullopt;
    t.to_index = *inv;
    t.origin = g.origin;
    return t;
}

const Dataset* functional_group(const Dataset& ds, std::uint32_t frame, Tag group_seq) noexcept {
    auto item_of = [&](const Dataset& fg) -> const Dataset* {
        const Element* e = fg.find(group_seq);
        if (!e || !e->is_sequence() || e->items.empty()) return nullptr;
        return &e->items[0];
    };
    if (const Element* pf = ds.find(tags::PerFrameFunctionalGroupsSequence);
        pf && pf->is_sequence() && frame < pf->items.size()) {
        if (const Dataset* r = item_of(pf->items[frame])) return r;
    }
    if (const Element* sh = ds.find(tags::SharedFunctionalGroupsSequence); sh && sh->is_sequence() && !sh->items.empty()) {
        if (const Dataset* r = item_of(sh->items[0])) return r;
    }
    return nullptr;
}

namespace {

// Reads `count` doubles of a multi-valued element; nullopt when absent, Malformed when the
// multiplicity is wrong or a value is not finite.
Result<std::optional<std::array<double, 6>>> read_values(const Dataset& ds, Tag tag, std::size_t count, const char* what) {
    std::optional<std::array<double, 6>> none;
    const Element* e = ds.find(tag);
    if (!e || e->value.empty()) return none;
    std::array<double, 6> v{};
    for (std::size_t i = 0; i < count; ++i) {
        auto d = ds.get_double(tag, i);
        if (!d || !std::isfinite(*d)) return Status(Errc::Malformed, std::string("volume: malformed ") + what, e->offset, tag);
        v[i] = *d;
    }
    return std::optional<std::array<double, 6>>(v);
}

std::optional<std::int64_t> int_in(const Dataset* ds, Tag tag) {
    if (!ds) return std::nullopt;
    return ds->get_int(tag);
}

}  // namespace

Result<FramePlane> read_frame_plane(const Dataset& ds, std::uint32_t frame) {
    FramePlane p;
    const Dataset* pos_src = functional_group(ds, frame, tags::PlanePositionSequence);
    const Dataset* ori_src = functional_group(ds, frame, tags::PlaneOrientationSequence);
    const Dataset* pix_src = functional_group(ds, frame, tags::PixelMeasuresSequence);
    if (!pos_src || !pos_src->contains(tags::ImagePositionPatient)) pos_src = &ds;
    if (!ori_src || !ori_src->contains(tags::ImageOrientationPatient)) ori_src = &ds;
    if (!pix_src || !pix_src->contains(tags::PixelSpacing)) pix_src = &ds;

    auto pos = read_values(*pos_src, tags::ImagePositionPatient, 3, "Image Position (Patient)");
    if (!pos) return pos.take_error();
    if (const auto& pv = *pos; pv) {
        const auto& v = *pv;
        p.has_position = true;
        p.position = {v[0], v[1], v[2]};
    }
    auto ori = read_values(*ori_src, tags::ImageOrientationPatient, 6, "Image Orientation (Patient)");
    if (!ori) return ori.take_error();
    if (const auto& ov = *ori; ov) {
        const auto& o = *ov;
        auto r = normalized({o[0], o[1], o[2]});
        auto c = normalized({o[3], o[4], o[5]});
        // PS3.3 C.7.6.2.1.1: unit, orthogonal direction cosines. Values written with few digits are
        // accepted (re-normalised) when they are close; clearly wrong ones are rejected.
        if (!r || !c || std::fabs(length({o[0], o[1], o[2]}) - 1.0) > 0.01 ||
            std::fabs(length({o[3], o[4], o[5]}) - 1.0) > 0.01 || std::fabs(dot(*r, *c)) > 0.01)
            return Status(Errc::Malformed, "volume: Image Orientation (Patient) is not a pair of orthogonal unit vectors",
                          0, tags::ImageOrientationPatient);
        // Make the column direction exactly orthogonal to the row direction.
        auto c2 = normalized(*c - *r * dot(*r, *c));
        p.has_orientation = true;
        p.row_dir = *r;
        p.col_dir = c2 ? *c2 : *c;
    }
    auto sp = read_values(*pix_src, tags::PixelSpacing, 2, "Pixel Spacing");
    if (!sp) return sp.take_error();
    if (const auto& sv = *sp; sv) {
        const auto& v = *sv;
        if (!(v[0] > 0.0) || !(v[1] > 0.0))
            return Status(Errc::Malformed, "volume: Pixel Spacing must be positive", 0, tags::PixelSpacing);
        p.has_spacing = true;
        p.row_spacing = v[0];
        p.col_spacing = v[1];
    }
    const Dataset* thick_src = functional_group(ds, frame, tags::PixelMeasuresSequence);
    auto positive = [](std::optional<double> v) -> std::optional<double> {
        if (v && std::isfinite(*v) && *v > 0.0) return v;
        return std::nullopt;
    };
    if (thick_src) {
        p.slice_thickness = positive(thick_src->get_double(tags::SliceThickness));
        p.spacing_between_slices = positive(thick_src->get_double(tags::SpacingBetweenSlices));
    }
    if (!p.slice_thickness) p.slice_thickness = positive(ds.get_double(tags::SliceThickness));
    if (!p.spacing_between_slices) p.spacing_between_slices = positive(ds.get_double(tags::SpacingBetweenSlices));
    return p;
}

FrameKeys read_frame_keys(const Dataset& ds, std::uint32_t frame) {
    FrameKeys k;
    const Dataset* fc = functional_group(ds, frame, tags::FrameContentSequence);
    k.temporal_position_index = int_in(fc, tags::TemporalPositionIndex);
    if (fc) {
        if (auto s = fc->get_string(tags::StackID)) k.stack_id = *s;
    }
    k.in_stack_position = int_in(fc, tags::InStackPositionNumber);
    k.temporal_position_identifier = ds.get_int(tags::TemporalPositionIdentifier);
    if (const Dataset* cs = functional_group(ds, frame, tags::CardiacSynchronizationSequence)) {
        if (auto t = cs->get_double(tags::NominalCardiacTriggerDelayTime); t && std::isfinite(*t)) k.trigger_time = t;
    }
    if (!k.trigger_time) {
        if (auto t = ds.get_double(tags::TriggerTime); t && std::isfinite(*t)) k.trigger_time = t;
    }
    k.instance_number = ds.get_int(tags::InstanceNumber);
    return k;
}

}  // namespace siaara::volume
