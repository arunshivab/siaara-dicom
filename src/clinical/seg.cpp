// SIAARA DICOM Toolkit - DICOM SEG reading (see seg.hpp for the references).
#include "clinical/seg.hpp"

#include <algorithm>
#include <cmath>

#include "charset/charset.hpp"
#include "image/frames.hpp"
#include "image/samples.hpp"

namespace siaara::clinical {

namespace {

constexpr Tag kNumberOfFrames = 0x00280008, kRows = 0x00280010, kColumns = 0x00280011;
constexpr Tag kPerFrame = 0x52009230;

std::vector<SourceReference> source_refs(const Dataset& item, Tag seq) {
    std::vector<SourceReference> out;
    for (const Dataset& r : items_of(item, seq)) {
        SourceReference s;
        s.sop_class_uid = get_text(r, tags::ReferencedSopClassUid).value_or("");
        s.sop_instance_uid = get_text(r, tags::ReferencedSopInstanceUid).value_or("");
        if (auto f = get_numbers(r, tags::ReferencedFrameNumber, 1u << 20))
            for (double x : *f)
                if (x >= 1.0 && x <= 2147483647.0) s.frames.push_back(static_cast<std::int32_t>(x));
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

const Segment* Segmentation::segment(std::uint32_t number) const noexcept {
    for (const auto& s : segments)
        if (s.number == number) return &s;
    return nullptr;
}

std::vector<std::uint32_t> Segmentation::frames_for_source(std::uint32_t number, std::string_view sop,
                                                           std::uint32_t f1) const {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < frames.size(); ++i) {
        const SegFrame& f = frames[i];
        if (type != SegmentationType::Labelmap && f.segment != number) continue;
        for (const auto& s : f.sources) {
            if (s.sop_instance_uid != sop) continue;
            const bool frame_ok = f1 == 0 || (s.frames.empty() ? f1 == 1 : std::find(s.frames.begin(), s.frames.end(), static_cast<std::int32_t>(f1)) != s.frames.end());
            if (frame_ok) {
                out.push_back(i);
                break;
            }
        }
    }
    return out;
}

std::vector<std::uint32_t> Segmentation::frames_on_plane(std::uint32_t number, const PlaneGeometry& plane, double tol) const {
    std::vector<std::uint32_t> out;
    if (!plane.has_position || !plane.has_orientation) return out;
    for (std::uint32_t i = 0; i < frames.size(); ++i) {
        const SegFrame& f = frames[i];
        if (type != SegmentationType::Labelmap && f.segment != number) continue;
        const PlaneGeometry& g = f.geometry;
        if (!g.has_position || !g.has_orientation) continue;
        if (!plane.frame_of_reference_uid.empty() && !g.frame_of_reference_uid.empty() &&
            plane.frame_of_reference_uid != g.frame_of_reference_uid)
            continue;
        if (1.0 - std::fabs(g.normal.dot(plane.normal)) > 1e-4) continue;
        if (std::fabs((g.position - plane.position).dot(plane.normal)) <= tol) out.push_back(i);
    }
    return out;
}

Result<Segmentation> read_segmentation(const Dataset& ds, bool lenient) {
    Segmentation seg;
    const std::string type = get_text(ds, tags::SegmentationType).value_or("");
    if (type == "BINARY") seg.type = SegmentationType::Binary;
    else if (type == "FRACTIONAL") seg.type = SegmentationType::Fractional;
    else if (type == "LABELMAP") seg.type = SegmentationType::Labelmap;
    else return Status(Errc::Unsupported, "Segmentation Type is not BINARY, FRACTIONAL or LABELMAP");
    seg.fractional_type = get_text(ds, tags::SegmentationFractionalType).value_or("");
    if (seg.type == SegmentationType::Fractional) {
        const auto m = ds.get_int(tags::MaximumFractionalValue);
        if (!m || *m < 1 || *m > 255) {
            if (!lenient) return Status(Errc::Malformed, "Maximum Fractional Value missing or out of range");
            seg.warnings.push_back("Maximum Fractional Value missing or out of range: 255 assumed");
            seg.max_fractional = 255;
        } else {
            seg.max_fractional = static_cast<std::uint32_t>(*m);
        }
    }
    seg.rows = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ds.get_int(kRows).value_or(0), 0, 65535));
    seg.columns = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ds.get_int(kColumns).value_or(0), 0, 65535));
    const auto nf = ds.get_int(kNumberOfFrames).value_or(1);
    if (nf < 1 || nf > (1 << 24)) return Status(Errc::Malformed, "Number of Frames out of range");
    seg.number_of_frames = static_cast<std::uint32_t>(nf);
    seg.frame_of_reference_uid = get_text(ds, tags::FrameOfReferenceUid).value_or("");
    const charset::CharacterSet cs = charset::character_set_of(ds);
    for (const Dataset& it : items_of(ds, tags::SegmentSequence)) {
        Segment s;
        const auto num = it.get_int(tags::SegmentNumber);
        if (!num || *num < 1 || *num > 65535) {
            if (!lenient) return Status(Errc::Malformed, "Segment Number missing or out of range");
            seg.warnings.push_back("segment without a valid Segment Number skipped");
            continue;
        }
        s.number = static_cast<std::uint32_t>(*num);
        if (auto v = charset::get_utf8(it, tags::SegmentLabel, 0, &cs)) s.label = *v;
        if (auto v = charset::get_utf8(it, tags::SegmentDescription, 0, &cs)) s.description = *v;
        s.algorithm_type = get_text(it, tags::SegmentAlgorithmType).value_or("");
        if (auto v = charset::get_utf8(it, tags::SegmentAlgorithmName, 0, &cs)) s.algorithm_name = *v;
        if (auto v = charset::get_utf8(it, tags::TrackingId, 0, &cs)) s.tracking_id = *v;
        s.tracking_uid = get_text(it, tags::TrackingUid).value_or("");
        s.category = read_code(it, tags::SegmentedPropertyCategoryCodeSequence);
        s.type = read_code(it, tags::SegmentedPropertyTypeCodeSequence);
        if (auto lab = get_numbers_n(it, tags::RecommendedDisplayCielabValue, 3)) {
            std::array<std::uint16_t, 3> l{};
            for (std::size_t k = 0; k < 3; ++k) l[k] = static_cast<std::uint16_t>(std::clamp((*lab)[k], 0.0, 65535.0));
            s.cielab = l;
            s.rgb = cielab_to_rgb8(l[0], l[1], l[2]);
        }
        if (seg.segment(s.number)) {
            if (!lenient) return Status(Errc::Malformed, "duplicate Segment Number");
            seg.warnings.push_back("duplicate Segment Number ignored");
            continue;
        }
        seg.segments.push_back(std::move(s));
    }
    // Common instance reference: Referenced Series Sequence > Referenced Instance Sequence (0008,114A).
    for (const Dataset& s : items_of(ds, tags::ReferencedSeriesSequence)) {
        auto r = source_refs(s, 0x0008114A);
        seg.referenced_images.insert(seg.referenced_images.end(), r.begin(), r.end());
    }
    const bool enhanced = ds.contains(kPerFrame);
    seg.frames.resize(seg.number_of_frames);
    for (std::uint32_t f = 0; f < seg.number_of_frames; ++f) {
        SegFrame& fr = seg.frames[f];
        if (seg.type != SegmentationType::Labelmap) {
            const Dataset* si = functional_group(ds, f, tags::SegmentIdentificationSequence);
            const auto n = si ? si->get_int(tags::ReferencedSegmentNumber) : std::nullopt;
            if (!n || *n < 1 || *n > 65535) {
                if (!lenient) return Status(Errc::Malformed, "frame without Referenced Segment Number");
                if (seg.warnings.size() < 100) seg.warnings.push_back("frame without Referenced Segment Number");
            } else {
                fr.segment = static_cast<std::uint32_t>(*n);
                auto it = std::find_if(seg.segments.begin(), seg.segments.end(),
                                       [&](const Segment& x) { return x.number == fr.segment; });
                if (it != seg.segments.end()) it->frames.push_back(f);
                else if (seg.warnings.size() < 100) seg.warnings.push_back("frame references an undefined segment");
            }
        }
        if (enhanced) {
            const auto& per = items_of(ds, kPerFrame);
            if (f < per.size())
                for (const Dataset& d : items_of(per[f], tags::DerivationImageSequence)) {
                    auto r = source_refs(d, tags::SourceImageSequence);
                    fr.sources.insert(fr.sources.end(), r.begin(), r.end());
                }
        }
        if (auto g = frame_geometry(ds, f)) fr.geometry = std::move(g).value();
    }
    if (seg.type == SegmentationType::Labelmap)
        for (auto& s : seg.segments)
            for (std::uint32_t f = 0; f < seg.number_of_frames; ++f) s.frames.push_back(f);
    return seg;
}

Result<std::vector<std::uint8_t>> segment_mask(const Dataset& ds, std::string_view ts, const Segmentation& seg,
                                               std::uint32_t frame, std::uint32_t segment_number) {
    auto fr = image::FrameReader::open(ds, ts);
    if (!fr) return fr.take_error();
    const image::PixelInfo& pi = fr->info();
    if (frame >= fr->frame_count()) return Status(Errc::InvalidArgument, "SEG frame index out of range");
    if (pi.samples_per_pixel != 1 || pi.kind != image::SampleKind::Integer)
        return Status(Errc::Malformed, "SEG pixel data must be one integer sample per pixel");
    if (seg.type == SegmentationType::Binary && pi.bits_allocated != 1)
        return Status(Errc::Malformed, "BINARY segmentation must have Bits Allocated 1");
    if (seg.type == SegmentationType::Fractional && pi.bits_allocated != 8)
        return Status(Errc::Malformed, "FRACTIONAL segmentation must have Bits Allocated 8");
    auto bytes = fr->decode(frame);
    if (!bytes) return bytes.take_error();
    const std::uint64_t n = pi.pixels();
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    const image::detail::SampleReader rd(pi);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::int64_t v = rd.value(bytes->data(), i);
        std::uint8_t m;
        switch (seg.type) {
            case SegmentationType::Binary: m = v ? 1 : 0; break;
            case SegmentationType::Fractional: m = static_cast<std::uint8_t>(std::clamp<std::int64_t>(v, 0, 255)); break;
            default:
                if (segment_number) m = v == static_cast<std::int64_t>(segment_number) ? 1 : 0;
                else m = static_cast<std::uint8_t>(std::clamp<std::int64_t>(v, 0, 255));
                break;
        }
        out[static_cast<std::size_t>(i)] = m;
    }
    return out;
}

}  // namespace siaara::clinical
