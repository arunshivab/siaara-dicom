#include "store/index.hpp"

#include <cstddef>
#include <optional>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "core/incomplete_policy.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "store/common.hpp"

namespace siaara::store {

namespace {

namespace t {
constexpr Tag ImageType = 0x00080008, StudyDate = 0x00080020, SeriesDate = 0x00080021,
              ContentDate = 0x00080023, StudyTime = 0x00080030, SeriesTime = 0x00080031,
              ContentTime = 0x00080033, AccessionNumber = 0x00080050,
              ReferringPhysician = 0x00080090, StudyDescription = 0x00081030,
              SeriesDescription = 0x0008103E, IssuerOfPatientId = 0x00100021,
              BirthDate = 0x00100030, Sex = 0x00100040, SliceThickness = 0x00180050,
              BodyPart = 0x00180015, StudyId = 0x00200010, SeriesNumber = 0x00200011,
              InstanceNumber = 0x00200013, Position = 0x00200032, Orientation = 0x00200037,
              FrameOfReference = 0x00200052, PixelSpacing = 0x00280030;
}  // namespace t

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}

// The whole value (all multiplicities) as UTF-8, padding removed.
std::string text(const Dataset& ds, Tag tag, const charset::CharacterSet& cs) {
    const Element* e = ds.find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return {};
    static const charset::CharacterSet kDefault{};
    const bool uses = charset::vr_uses_character_set(e->vr);
    const VR vr = e->vr == VR::UN ? VR::LO : e->vr;
    std::string s = charset::decode(e->value.as_chars(), uses ? cs : kDefault, charset::text_kind(vr));
    return std::string(trim(s));
}

template <class T>
bool get_num(const Dataset& ds, Tag tag, T& out) {
    auto v = ds.get_int(tag);
    if (!v || *v < 0) return false;
    out = static_cast<T>(*v);
    return true;
}

bool get_doubles(const Dataset& ds, Tag tag, double* out, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        auto v = ds.get_double(tag, i);
        if (!v) return false;
        out[i] = *v;
    }
    return true;
}

bool in_buffer(const Bytes& buf, const std::uint8_t* p) noexcept {
    return p && buf.data() && p >= buf.data() && p <= buf.data() + buf.size();
}

constexpr struct {
    Tag tag;
    Str key;
} kKeys[] = {
    {tags::PatientID, Str::PatientId},
    {t::IssuerOfPatientId, Str::IssuerOfPatientId},
    {tags::PatientName, Str::PatientName},
    {t::BirthDate, Str::PatientBirthDate},
    {t::Sex, Str::PatientSex},
    {tags::StudyInstanceUID, Str::StudyInstanceUid},
    {t::StudyDate, Str::StudyDate},
    {t::StudyTime, Str::StudyTime},
    {t::AccessionNumber, Str::AccessionNumber},
    {t::StudyId, Str::StudyId},
    {t::StudyDescription, Str::StudyDescription},
    {t::ReferringPhysician, Str::ReferringPhysician},
    {tags::SeriesInstanceUID, Str::SeriesInstanceUid},
    {tags::Modality, Str::Modality},
    {t::SeriesDescription, Str::SeriesDescription},
    {t::BodyPart, Str::BodyPartExamined},
    {t::SeriesDate, Str::SeriesDate},
    {t::SeriesTime, Str::SeriesTime},
    {t::FrameOfReference, Str::FrameOfReferenceUid},
    {tags::SOPClassUID, Str::SopClassUid},
    {tags::SOPInstanceUID, Str::SopInstanceUid},
    {tags::PhotometricInterpretation, Str::PhotometricInterpretation},
    {t::ContentDate, Str::ContentDate},
    {t::ContentTime, Str::ContentTime},
    {t::ImageType, Str::ImageType},
};

}  // namespace

void read_keys(const Dataset& ds, const charset::CharacterSet& cs, IndexRecord& out, bool overlay) {
    for (const auto& k : kKeys)
        if (!overlay || ds.contains(k.tag)) out[k.key] = text(ds, k.tag, cs);
    if (auto v = ds.get_int(t::SeriesNumber); v && *v >= INT32_MIN && *v <= INT32_MAX) {
        out.series_number = static_cast<std::int32_t>(*v);
        out.flags |= kHasSeriesNumber;
    }
    if (auto v = ds.get_int(t::InstanceNumber); v && *v >= INT32_MIN && *v <= INT32_MAX) {
        out.instance_number = static_cast<std::int32_t>(*v);
        out.flags |= kHasInstanceNumber;
    }
}

std::string element_text(const Dataset& ds, Tag tag, const charset::CharacterSet& cs) { return text(ds, tag, cs); }

std::uint64_t read_present_keys(const Dataset& ds, const charset::CharacterSet& cs, IndexRecord& out,
                                std::uint64_t max_value_bytes, std::size_t* too_long) {
    std::uint64_t present = 0;
    auto fits = [&](Tag tag) {
        const Element* e = ds.find(tag);
        return !max_value_bytes || !e || e->value.size() <= max_value_bytes;
    };
    for (const auto& k : kKeys) {
        if (!ds.contains(k.tag)) continue;
        present |= std::uint64_t{1} << static_cast<unsigned>(k.key);
        if (fits(k.tag)) {
            out[k.key] = text(ds, k.tag, cs);
        } else {
            out[k.key].clear();  // left out, never cut
            if (too_long) ++*too_long;
        }
    }
    if (fits(t::SeriesNumber))
        if (auto v = ds.get_int(t::SeriesNumber); v && *v >= INT32_MIN && *v <= INT32_MAX) {
            out.series_number = static_cast<std::int32_t>(*v);
            out.flags |= kHasSeriesNumber;
        }
    if (fits(t::InstanceNumber))
        if (auto v = ds.get_int(t::InstanceNumber); v && *v >= INT32_MIN && *v <= INT32_MAX) {
            out.instance_number = static_cast<std::int32_t>(*v);
            out.flags |= kHasInstanceNumber;
        }
    for (Tag tag : {t::SeriesNumber, t::InstanceNumber})
        if (ds.contains(tag) && !fits(tag) && too_long) ++*too_long;
    return present;
}

bool looks_like_dicom(const std::uint8_t* p, std::size_t n, bool allow_raw) noexcept {
    if (has_part10_header(p, n)) return true;
    if (!allow_raw || n < 8) return false;
    if (p[0] == 'D' && p[1] == 'I' && p[2] == 'C' && p[3] == 'M') return true;
    const std::uint16_t group = load_le16(p);
    if (group != 0x0002 && group != 0x0008) return false;
    const VR vr = vr_from_chars(static_cast<char>(p[4]), static_cast<char>(p[5]));
    if (vr_is_known(vr)) return true;
    const std::uint32_t len = load_le32(p + 4);
    return len <= n - 8 && len < 0x10000u;
}

Status extract_index(const Bytes& data, IndexRecord& out, const ExtractOptions& opt) {
    out = IndexRecord{};
    out.file_size = data.size();
    if (!looks_like_dicom(data.data(), data.size(), opt.allow_raw_dataset))
        return Status(Errc::NotDicom, "not a DICOM file");
    ReadOptions ro = opt.network_input ? network_read_options() : ReadOptions{};
    ro.allow_raw_dataset = opt.allow_raw_dataset;
    auto r = read_part10(data, ro);
    if (!r) return r.take_error();
    const DicomFile& f = *r;
    const Dataset& ds = f.dataset;
    if (f.had_preamble) out.flags |= kHadPreamble;
    const charset::CharacterSet cs = charset::character_set_of(ds);

    read_keys(ds, cs, out, false);
    out[Str::TransferSyntaxUid] = std::string(trim(f.transfer_syntax));
    if (out[Str::SopClassUid].empty()) out[Str::SopClassUid] = text(f.meta, tags::MediaStorageSOPClassUID, cs);
    if (out[Str::SopInstanceUid].empty())
        out[Str::SopInstanceUid] = text(f.meta, tags::MediaStorageSOPInstanceUID, cs);
    if (auto v = ds.get_raw_string(tags::SpecificCharacterSet)) out[Str::SpecificCharacterSet] = std::string(trim(*v));

    if (get_doubles(ds, t::Position, out.position, 3)) out.flags |= kHasPosition;
    if (get_doubles(ds, t::Orientation, out.orientation, 6)) out.flags |= kHasOrientation;
    if (get_doubles(ds, t::PixelSpacing, out.pixel_spacing, 2)) out.flags |= kHasPixelSpacing;
    if (auto v = ds.get_double(t::SliceThickness)) {
        out.slice_thickness = *v;
        out.flags |= kHasSliceThickness;
    }
    (void)get_num(ds, tags::Rows, out.rows);
    (void)get_num(ds, tags::Columns, out.columns);
    (void)get_num(ds, tags::BitsAllocated, out.bits_allocated);
    (void)get_num(ds, tags::BitsStored, out.bits_stored);
    (void)get_num(ds, tags::HighBit, out.high_bit);
    (void)get_num(ds, tags::SamplesPerPixel, out.samples_per_pixel);
    (void)get_num(ds, tags::PixelRepresentation, out.pixel_representation);

    const Element* pd = ds.find(tags::PixelData);
    if (!pd) pd = ds.find(tags::FloatPixelData);
    if (!pd) pd = ds.find(tags::DoubleFloatPixelData);
    if (pd) {
        out.flags |= kHasPixelData;
        out.number_of_frames = 1;
        (void)get_num(ds, tags::NumberOfFrames, out.number_of_frames);
        if (out.number_of_frames == 0) out.number_of_frames = 1;
        if (pd->is_encapsulated()) {
            out.flags |= kEncapsulated;
            if (!pd->fragments.empty()) {
                const std::uint8_t* first = pd->fragments.front().data();
                const Bytes& last = pd->fragments.back();
                if (first && in_buffer(data, first) && first - data.data() >= 8 &&
                    in_buffer(data, last.data() + last.size())) {
                    out.pixel_data_offset = static_cast<std::uint64_t>(first - data.data()) - 8u;
                    std::uint64_t end = static_cast<std::uint64_t>(last.data() + last.size() - data.data());
                    if (end + 8 <= data.size()) end += 8;  // sequence delimiter
                    out.pixel_data_length = end - out.pixel_data_offset;
                    out.flags |= kPixelOffsetKnown;
                }
            }
        } else if (in_buffer(data, pd->value.data()) &&
                   in_buffer(data, pd->value.data() + pd->value.size())) {
            out.pixel_data_offset = static_cast<std::uint64_t>(pd->value.data() - data.data());
            out.pixel_data_length = pd->value.size();
            out.flags |= kPixelOffsetKnown;
        }
    }
    return Status::ok();
}

Status extract_index_file(const std::string& utf8_path, IndexRecord& out, const ExtractOptions& opt) {
    auto bytes = map_for_header(utf8_path);
    if (!bytes) {
        out = IndexRecord{};
        return bytes.take_error();
    }
    return extract_index(*bytes, out, opt);
}

}  // namespace siaara::store
