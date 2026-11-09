// SIAARA DICOM Toolkit - core types
// Written from DICOM PS3.5 (Data Structures and Encoding).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace siaara {

// ---------------------------------------------------------------- Tag
// A tag is (group << 16) | element, as in PS3.5 section 7.1.
using Tag = std::uint32_t;

constexpr Tag make_tag(std::uint16_t group, std::uint16_t element) noexcept {
    return (static_cast<Tag>(group) << 16) | element;
}
constexpr std::uint16_t tag_group(Tag t) noexcept { return static_cast<std::uint16_t>(t >> 16); }
constexpr std::uint16_t tag_element(Tag t) noexcept { return static_cast<std::uint16_t>(t & 0xFFFFu); }
constexpr bool tag_is_private(Tag t) noexcept { return (tag_group(t) & 1u) != 0 && tag_group(t) > 8; }

namespace tags {
constexpr Tag FileMetaInformationGroupLength = 0x00020000;
constexpr Tag FileMetaInformationVersion     = 0x00020001;
constexpr Tag MediaStorageSOPClassUID        = 0x00020002;
constexpr Tag MediaStorageSOPInstanceUID     = 0x00020003;
constexpr Tag TransferSyntaxUID              = 0x00020010;
constexpr Tag ImplementationClassUID         = 0x00020012;
constexpr Tag ImplementationVersionName      = 0x00020013;
constexpr Tag SpecificCharacterSet           = 0x00080005;
constexpr Tag SOPClassUID                    = 0x00080016;
constexpr Tag SOPInstanceUID                 = 0x00080018;
constexpr Tag Modality                       = 0x00080060;
constexpr Tag PatientName                    = 0x00100010;
constexpr Tag PatientID                      = 0x00100020;
constexpr Tag StudyInstanceUID               = 0x0020000D;
constexpr Tag SeriesInstanceUID              = 0x0020000E;
constexpr Tag SamplesPerPixel                = 0x00280002;
constexpr Tag PhotometricInterpretation      = 0x00280004;
constexpr Tag PlanarConfiguration            = 0x00280006;
constexpr Tag NumberOfFrames                 = 0x00280008;
constexpr Tag Rows                           = 0x00280010;
constexpr Tag Columns                        = 0x00280011;
constexpr Tag BitsAllocated                  = 0x00280100;
constexpr Tag BitsStored                     = 0x00280101;
constexpr Tag HighBit                        = 0x00280102;
constexpr Tag PixelRepresentation            = 0x00280103;
constexpr Tag RescaleIntercept               = 0x00281052;
constexpr Tag RescaleSlope                   = 0x00281053;
constexpr Tag PixelData                      = 0x7FE00010;
constexpr Tag FloatPixelData                 = 0x7FE00008;
constexpr Tag DoubleFloatPixelData           = 0x7FE00009;
constexpr Tag Item                           = 0xFFFEE000;
constexpr Tag ItemDelimitationItem           = 0xFFFEE00D;
constexpr Tag SequenceDelimitationItem       = 0xFFFEE0DD;
}  // namespace tags

constexpr std::uint32_t kUndefinedLength = 0xFFFFFFFFu;

// ---------------------------------------------------------------- VR
// Value representations from PS3.5 table 6.2-1. Encoded as the two ASCII chars.
enum class VR : std::uint16_t {
    None = 0,
    AE = 0x4145, AS = 0x4153, AT = 0x4154, CS = 0x4353, DA = 0x4441, DS = 0x4453,
    DT = 0x4454, FL = 0x464C, FD = 0x4644, IS = 0x4953, LO = 0x4C4F, LT = 0x4C54,
    OB = 0x4F42, OD = 0x4F44, OF = 0x4F46, OL = 0x4F4C, OV = 0x4F56, OW = 0x4F57,
    PN = 0x504E, SH = 0x5348, SL = 0x534C, SQ = 0x5351, SS = 0x5353, ST = 0x5354,
    SV = 0x5356, TM = 0x544D, UC = 0x5543, UI = 0x5549, UL = 0x554C, UN = 0x554E,
    UR = 0x5552, US = 0x5553, UT = 0x5554, UV = 0x5556,
};

constexpr VR vr_from_chars(char a, char b) noexcept {
    return static_cast<VR>((static_cast<std::uint16_t>(static_cast<unsigned char>(a)) << 8) |
                           static_cast<unsigned char>(b));
}

bool vr_is_known(VR vr) noexcept;
// True when explicit VR encoding uses 2 reserved bytes + a 32-bit length (PS3.5 7.1.2).
bool vr_has_long_length(VR vr) noexcept;
// True for VRs whose value is character data.
bool vr_is_string(VR vr) noexcept;
// Byte size of one binary value (US=2, UL=4, FD=8 ...), 0 for string/other VRs.
std::size_t vr_value_size(VR vr) noexcept;
// Padding byte for even length: space for strings, NUL for UI and binary.
char vr_pad_char(VR vr) noexcept;
std::string_view vr_name(VR vr) noexcept;

// ---------------------------------------------------------------- Bytes
// A read-only view of value bytes that keeps its backing storage alive.
// Values read from a file point straight into the (memory-mapped) file buffer: no copy.
class Bytes {
public:
    Bytes() = default;
    Bytes(std::shared_ptr<const void> owner, const std::uint8_t* data, std::size_t size) noexcept
        : owner_(std::move(owner)), data_(data), size_(size) {}

    static Bytes copy_of(const void* data, std::size_t size);
    static Bytes from_string(std::string_view s) { return copy_of(s.data(), s.size()); }

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::span<const std::uint8_t> span() const noexcept { return {data_, size_}; }
    std::string_view as_chars() const noexcept {
        return {reinterpret_cast<const char*>(data_), size_};
    }
    Bytes sub(std::size_t offset, std::size_t len) const noexcept {
        return Bytes(owner_, data_ + offset, len);
    }

private:
    std::shared_ptr<const void> owner_;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------- byte order helpers
inline std::uint16_t load_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t load_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t load_le64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(load_le32(p)) |
           (static_cast<std::uint64_t>(load_le32(p + 4)) << 32);
}
inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
inline void store_le16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void store_le32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v);
}
inline void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

}  // namespace siaara
