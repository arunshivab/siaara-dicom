// SIAARA DICOM Toolkit - RT Structure Set reading (see rtstruct.hpp for the references).
#include "clinical/rtstruct.hpp"

#include <algorithm>
#include <cmath>

#include "charset/charset.hpp"

namespace siaara::clinical {

namespace {

std::vector<SourceReference> image_items(const Dataset& item) {
    std::vector<SourceReference> out;
    for (const Dataset& r : items_of(item, tags::ContourImageSequence)) {
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

Result<RtStructureSet> read_rtstruct(const Dataset& ds, const RtReadOptions& opt) {
    RtStructureSet ss;
    if (!ds.contains(tags::StructureSetRoiSequence) && !ds.contains(tags::RoiContourSequence))
        return Status(Errc::NotFound, "not an RT Structure Set");
    const charset::CharacterSet cs = charset::character_set_of(ds);
    if (auto v = charset::get_utf8(ds, tags::StructureSetLabel, 0, &cs)) ss.label = *v;
    for (const Dataset& f : items_of(ds, tags::ReferencedFrameOfReferenceSequence))
        for (const Dataset& st : items_of(f, tags::RtReferencedStudySequence))
            for (const Dataset& se : items_of(st, tags::RtReferencedSeriesSequence)) {
                auto r = image_items(se);
                ss.referenced_images.insert(ss.referenced_images.end(), r.begin(), r.end());
            }
    for (const Dataset& it : items_of(ds, tags::StructureSetRoiSequence)) {
        RtRoi roi;
        const auto num = it.get_int(tags::RoiNumber);
        if (!num) {
            if (!opt.lenient) return Status(Errc::Malformed, "Structure Set ROI item without ROI Number");
            ss.warnings.push_back("ROI without ROI Number skipped");
            continue;
        }
        roi.number = *num;
        if (auto v = charset::get_utf8(it, tags::RoiName, 0, &cs)) roi.name = *v;
        if (auto v = charset::get_utf8(it, tags::RoiDescription, 0, &cs)) roi.description = *v;
        roi.generation_algorithm = get_text(it, tags::RoiGenerationAlgorithm).value_or("");
        roi.frame_of_reference_uid = get_text(it, tags::ReferencedFrameOfReferenceUid).value_or("");
        ss.rois.push_back(std::move(roi));
    }
    auto find_roi = [&](std::int64_t number) -> RtRoi* {
        for (auto& r : ss.rois)
            if (r.number == number) return &r;
        return nullptr;
    };
    std::uint64_t total = 0;
    for (const Dataset& it : items_of(ds, tags::RoiContourSequence)) {
        const auto ref = it.get_int(tags::ReferencedRoiNumber);
        RtRoi* roi = ref ? find_roi(*ref) : nullptr;
        if (!roi) {
            if (!opt.lenient) return Status(Errc::Malformed, "ROI Contour item references an undefined ROI");
            ss.warnings.push_back("ROI Contour for an undefined ROI skipped");
            continue;
        }
        if (auto c = get_numbers_n(it, tags::RoiDisplayColor, 3)) {
            std::array<std::uint8_t, 3> rgb{};
            for (std::size_t k = 0; k < 3; ++k) rgb[k] = static_cast<std::uint8_t>(std::clamp((*c)[k], 0.0, 255.0));
            roi->color = rgb;
        }
        for (const Dataset& c : items_of(it, tags::ContourSequence)) {
            RtContour ct;
            ct.number = c.get_int(tags::ContourNumber).value_or(0);
            ct.geometric_type = get_text(c, tags::ContourGeometricType).value_or("");
            ct.images = image_items(c);
            auto data = get_numbers(c, tags::ContourData, static_cast<std::size_t>(std::min<std::uint64_t>(opt.max_points * 3, SIZE_MAX / 2)));
            if (!data || data->empty() || data->size() % 3) {
                if (!opt.lenient) return Status(Errc::Malformed, "Contour Data missing or not triplets");
                ss.warnings.push_back("contour with bad Contour Data skipped");
                continue;
            }
            const auto npts = c.get_int(tags::NumberOfContourPoints);
            if (npts && static_cast<std::uint64_t>(*npts) * 3 != data->size()) {
                if (!opt.lenient) return Status(Errc::Malformed, "Number of Contour Points does not match Contour Data");
                ss.warnings.push_back("Number of Contour Points does not match Contour Data; data used");
            }
            total += data->size() / 3;
            if (total > opt.max_points) return Status(Errc::LimitExceeded, "too many contour points");
            ct.points.reserve(data->size() / 3);
            bool finite = true;
            for (std::size_t k = 0; k + 2 < data->size(); k += 3) {
                const Vec3 p{(*data)[k], (*data)[k + 1], (*data)[k + 2]};
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) finite = false;
                ct.points.push_back(p);
            }
            if (!finite) {
                if (!opt.lenient) return Status(Errc::Malformed, "non-finite contour point");
                ss.warnings.push_back("contour with non-finite points skipped");
                continue;
            }
            roi->contours.push_back(std::move(ct));
        }
    }
    for (const Dataset& it : items_of(ds, tags::RtRoiObservationsSequence)) {
        const auto ref = it.get_int(tags::ReferencedRoiNumber);
        if (RtRoi* roi = ref ? find_roi(*ref) : nullptr)
            roi->interpreted_type = get_text(it, tags::RtRoiInterpretedType).value_or("");
    }
    return ss;
}

std::vector<ContourOnImage> contours_on_image(const RtStructureSet& ss, const PlaneGeometry& image,
                                              std::string_view sop, double tol) {
    std::vector<ContourOnImage> out;
    if (!image.complete()) return out;
    if (tol < 0.0) tol = image.slice_thickness && *image.slice_thickness > 0.0 ? *image.slice_thickness / 2.0 : 0.5;
    for (std::uint32_t r = 0; r < ss.rois.size(); ++r) {
        const RtRoi& roi = ss.rois[r];
        for (std::uint32_t c = 0; c < roi.contours.size(); ++c) {
            const RtContour& ct = roi.contours[c];
            bool by_ref = false, match = false;
            if (!ct.images.empty()) {
                for (const auto& i : ct.images)
                    if (i.sop_instance_uid == sop) by_ref = match = true;
            } else if (roi.frame_of_reference_uid.empty() || image.frame_of_reference_uid.empty() ||
                       roi.frame_of_reference_uid == image.frame_of_reference_uid) {
                match = true;
                for (const auto& p : ct.points)
                    if (std::fabs((p - image.position).dot(image.normal)) > tol) {
                        match = false;
                        break;
                    }
            }
            if (!match) continue;
            ContourOnImage o;
            o.roi = r;
            o.contour = c;
            o.by_reference = by_ref;
            o.closed = ct.geometric_type == "CLOSED_PLANAR";
            o.points.reserve(ct.points.size());
            for (const auto& p : ct.points) {
                double col, row;
                if (!image.patient_to_pixel(p, col, row)) break;
                o.points.push_back({col + 0.5, row + 0.5});
            }
            out.push_back(std::move(o));
        }
    }
    return out;
}

}  // namespace siaara::clinical
