// SIAARA DICOM Toolkit - Image Pixel description. Written from DICOM PS3.3 C.7.6.3 / C.7.6.6.
#include "image/pixel_info.hpp"

#include <array>
#include <string>

namespace siaara::image {

namespace {

struct PhotoName {
    Photometric p;
    std::string_view name;
};
constexpr std::array<PhotoName, 14> kPhotoNames{{
    {Photometric::Monochrome1, "MONOCHROME1"},
    {Photometric::Monochrome2, "MONOCHROME2"},
    {Photometric::PaletteColor, "PALETTE COLOR"},
    {Photometric::Rgb, "RGB"},
    {Photometric::YbrFull, "YBR_FULL"},
    {Photometric::YbrFull422, "YBR_FULL_422"},
    {Photometric::YbrPartial422, "YBR_PARTIAL_422"},
    {Photometric::YbrPartial420, "YBR_PARTIAL_420"},
    {Photometric::YbrIct, "YBR_ICT"},
    {Photometric::YbrRct, "YBR_RCT"},
    {Photometric::Hsv, "HSV"},
    {Photometric::Argb, "ARGB"},
    {Photometric::Cmyk, "CMYK"},
    {Photometric::Xyb, "XYB"},
}};

Status bad(std::string msg, Tag t) { return Status(Errc::Malformed, "pixel module: " + std::move(msg), 0, t); }

// Reads a US (or any integer VR) attribute that must fit 16 bits unsigned.
std::optional<std::int64_t> get_us(const Dataset& ds, Tag t) {
    if (!ds.contains(t)) return std::nullopt;
    return ds.get_int(t);
}

std::uint16_t expected_spp(Photometric p) noexcept {
    switch (p) {
        case Photometric::Monochrome1: case Photometric::Monochrome2: case Photometric::PaletteColor:
            return 1;
        case Photometric::Argb: case Photometric::Cmyk:
            return 4;
        case Photometric::Unknown:
            return 0;
        default:
            return 3;
    }
}

}  // namespace

Photometric parse_photometric(std::string_view text) noexcept {
    for (const auto& n : kPhotoNames)
        if (n.name == text) return n.p;
    return Photometric::Unknown;
}

std::string_view photometric_name(Photometric p) noexcept {
    for (const auto& n : kPhotoNames)
        if (n.p == p) return n.name;
    return "";
}

std::uint64_t PixelInfo::native_frame_bits() const noexcept {
    // samples per pixel actually stored: 4:2:2 native data stores 2 samples per pixel on average.
    const std::uint64_t spp = native_subsampled() ? 2u : samples_per_pixel;
    std::uint64_t bits = 0;
    if (!checked_mul(pixels(), spp, bits) || !checked_mul(bits, bits_allocated, bits)) return 0;
    return bits;
}

codec::FrameInfo PixelInfo::frame_info() const noexcept {
    codec::FrameInfo fi;
    fi.rows = rows;
    fi.columns = columns;
    fi.samples_per_pixel = samples_per_pixel;
    fi.bits_allocated = bits_allocated;
    fi.bits_stored = bits_stored;
    fi.is_signed = is_signed;
    fi.planar = planar;
    return fi;
}

Result<std::uint32_t> frame_count(const Dataset& ds, const PixelInfoOptions& opt) {
    const Element* e = ds.find(siaara::tags::NumberOfFrames);
    if (!e || e->value.empty()) return std::uint32_t{1};
    const auto n = ds.get_int(siaara::tags::NumberOfFrames);
    if (!n || *n < 1 || *n > 0x7FFFFFFF) {
        if (opt.lenient && n && *n == 0) return std::uint32_t{1};
        return bad("Number of Frames is not a positive integer", siaara::tags::NumberOfFrames);
    }
    return static_cast<std::uint32_t>(*n);
}

Result<PixelInfo> read_pixel_info(const Dataset& ds, const PixelInfoOptions& opt) {
    using namespace siaara::tags;
    PixelInfo pi;
    auto warn = [&](std::string w) { pi.warnings.push_back(std::move(w)); };

    // ---- which pixel data element (PS3.3 C.7.6.3: exactly one of the three)
    const bool has_int = ds.contains(PixelData);
    const bool has_f = ds.contains(FloatPixelData);
    const bool has_d = ds.contains(DoubleFloatPixelData);
    const int present = int(has_int) + int(has_f) + int(has_d);
    if (present == 0) return Status(Errc::NotFound, "pixel module: no pixel data element", 0, PixelData);
    if (present > 1) {
        if (!opt.lenient) return bad("more than one pixel data element", PixelData);
        warn("more than one pixel data element; using the first of 7FE0,0010/0008/0009");
    }
    if (has_int) {
        pi.pixel_tag = PixelData;
    } else if (has_f) {
        pi.pixel_tag = FloatPixelData;
        pi.kind = SampleKind::Float32;
    } else {
        pi.pixel_tag = DoubleFloatPixelData;
        pi.kind = SampleKind::Float64;
    }

    // ---- dimensions
    const auto rows = get_us(ds, Rows);
    const auto cols = get_us(ds, Columns);
    if (!rows || !cols) return bad("Rows or Columns missing or unreadable", rows ? Columns : Rows);
    if (*rows <= 0 || *rows > 0xFFFF || *cols <= 0 || *cols > 0xFFFF)
        return bad("Rows and Columns must be 1..65535", Rows);
    pi.rows = static_cast<std::uint32_t>(*rows);
    pi.columns = static_cast<std::uint32_t>(*cols);

    // ---- photometric interpretation
    bool photometric_missing = false;
    if (auto s = ds.get_string(PhotometricInterpretation)) {
        pi.photometric_text = *s;
        pi.photometric = parse_photometric(*s);
        if (pi.photometric == Photometric::Unknown && pi.photometric_text.empty()) {
            if (!opt.lenient) return bad("Photometric Interpretation is empty", PhotometricInterpretation);
        }
    } else if (!opt.lenient) {
        return bad("Photometric Interpretation missing", PhotometricInterpretation);
    } else {
        photometric_missing = true;
    }

    // ---- samples per pixel
    const bool spp_missing = !get_us(ds, SamplesPerPixel);
    if (auto spp = get_us(ds, SamplesPerPixel)) {
        if (*spp < 1 || *spp > 4 || *spp == 2) return bad("Samples per Pixel must be 1, 3 or 4", SamplesPerPixel);
        pi.samples_per_pixel = static_cast<std::uint16_t>(*spp);
    } else {
        if (!opt.lenient) return bad("Samples per Pixel missing", SamplesPerPixel);
        const std::uint16_t e = expected_spp(pi.photometric);
        pi.samples_per_pixel = e ? e : 1;
        warn("Samples per Pixel missing; assumed " + std::to_string(pi.samples_per_pixel));
    }
    if (pi.photometric_text.empty() && opt.lenient) {
        if (pi.samples_per_pixel == 1) {
            pi.photometric = Photometric::Monochrome2;
            pi.photometric_text = "MONOCHROME2";
            warn("Photometric Interpretation missing; assumed MONOCHROME2");
        } else if (pi.samples_per_pixel == 3) {
            pi.photometric = Photometric::Rgb;
            pi.photometric_text = "RGB";
            warn("Photometric Interpretation missing; assumed RGB");
        }
    }
    if (const std::uint16_t e = expected_spp(pi.photometric); e && e != pi.samples_per_pixel)
        return bad("Samples per Pixel " + std::to_string(pi.samples_per_pixel) + " is not valid for " +
                       pi.photometric_text,
                   SamplesPerPixel);

    // ---- bits
    const auto ba = get_us(ds, BitsAllocated);
    if (!ba) return bad("Bits Allocated missing", BitsAllocated);
    pi.declared_bits_allocated = static_cast<std::uint16_t>(*ba);
    bool rgb24 = false;
    if (pi.kind == SampleKind::Integer) {
        if (*ba != 1 && *ba != 8 && *ba != 16 && *ba != 32 && *ba != 64) {
            if (opt.lenient && opt.encapsulated) {
                // The codestream knows its own precision; the frame layer settles the container.
                pi.bits_from_codestream = true;
                warn("Bits Allocated " + std::to_string(*ba) + " is not a valid container; taken from the codestream");
            } else if (opt.lenient && *ba == 24 && spp_missing && photometric_missing) {
                // ACR-NEMA 24-bit colour: three 8-bit samples per pixel, colour-by-pixel.
                rgb24 = true;
                pi.samples_per_pixel = 3;
                pi.photometric = Photometric::Rgb;
                pi.photometric_text = "RGB";
                warn("Bits Allocated 24 without Samples per Pixel / Photometric Interpretation; read as 8-bit RGB");
            } else {
                return bad("Bits Allocated must be 1, 8, 16, 32 or 64", BitsAllocated);
            }
        }
    } else if (*ba != (pi.kind == SampleKind::Float32 ? 32 : 64)) {
        return bad("Bits Allocated must be 32 for Float Pixel Data and 64 for Double Float Pixel Data",
                   BitsAllocated);
    }
    pi.bits_allocated = static_cast<std::uint16_t>(*ba);
    if (rgb24) pi.bits_allocated = 8;
    if (pi.bits_from_codestream) pi.bits_allocated = *ba <= 8 ? 8 : 16;  // provisional

    if (pi.kind != SampleKind::Integer) {
        // Float data has no Bits Stored / High Bit / Pixel Representation (C.7.6.24 Floating Point Image Pixel).
        pi.bits_stored = pi.bits_allocated;
        pi.high_bit = static_cast<std::uint16_t>(pi.bits_allocated - 1);
        pi.is_signed = true;
        if (pi.samples_per_pixel != 1) return bad("floating point pixel data must have one sample per pixel", SamplesPerPixel);
    } else if (rgb24 || pi.bits_from_codestream) {
        // Bits Stored / High Bit follow the container chosen above (provisional for the codestream
        // case: the frame layer replaces them with the codestream precision).
        pi.bits_stored = pi.bits_allocated;
        pi.high_bit = static_cast<std::uint16_t>(pi.bits_allocated - 1);
        if (auto pr = get_us(ds, PixelRepresentation); pr && *pr == 1) pi.is_signed = true;
    } else {
        const auto bs = get_us(ds, BitsStored);
        if (!bs) {
            if (!opt.lenient) return bad("Bits Stored missing", BitsStored);
            pi.bits_stored = pi.bits_allocated;
            warn("Bits Stored missing; assumed Bits Allocated");
        } else {
            if (*bs < 1 || *bs > pi.bits_allocated) return bad("Bits Stored must be 1..Bits Allocated", BitsStored);
            pi.bits_stored = static_cast<std::uint16_t>(*bs);
        }
        const auto hb = get_us(ds, HighBit);
        if (!hb) {
            if (!opt.lenient) return bad("High Bit missing", HighBit);
            pi.high_bit = static_cast<std::uint16_t>(pi.bits_stored - 1);
            warn("High Bit missing; assumed Bits Stored - 1");
        } else {
            if (*hb < pi.bits_stored - 1 || *hb >= pi.bits_allocated)
                return bad("High Bit must be at least Bits Stored - 1 and less than Bits Allocated", HighBit);
            if (*hb != pi.bits_stored - 1) {
                if (!opt.lenient) return bad("High Bit shall be Bits Stored - 1", HighBit);
                warn("High Bit is not Bits Stored - 1; samples are taken from bits " +
                     std::to_string(*hb + 1 - pi.bits_stored) + ".." + std::to_string(*hb));
            }
            pi.high_bit = static_cast<std::uint16_t>(*hb);
        }
        const auto pr = get_us(ds, PixelRepresentation);
        if (!pr) {
            if (!opt.lenient) return bad("Pixel Representation missing", PixelRepresentation);
            warn("Pixel Representation missing; assumed unsigned");
        } else {
            if (*pr != 0 && *pr != 1) return bad("Pixel Representation must be 0 or 1", PixelRepresentation);
            pi.is_signed = *pr == 1;
        }
        if (pi.bits_allocated == 1 && pi.samples_per_pixel != 1)
            return bad("1-bit pixel data must have one sample per pixel", BitsAllocated);
    }

    // ---- planar configuration (Type 1C: required when Samples per Pixel > 1)
    if (pi.samples_per_pixel > 1) {
        const auto pc = get_us(ds, PlanarConfiguration);
        if (!pc) {
            if (!opt.lenient) return bad("Planar Configuration missing", PlanarConfiguration);
            warn("Planar Configuration missing; assumed 0 (colour-by-pixel)");
        } else {
            if (*pc != 0 && *pc != 1) return bad("Planar Configuration must be 0 or 1", PlanarConfiguration);
            pi.planar = *pc == 1;
        }
        if (pi.planar && (pi.photometric == Photometric::YbrFull422 || pi.photometric == Photometric::YbrPartial422 ||
                          pi.photometric == Photometric::YbrPartial420)) {
            if (!opt.lenient) return bad(pi.photometric_text + " requires Planar Configuration 0", PlanarConfiguration);
            warn(pi.photometric_text + " with Planar Configuration 1; using 0");
            pi.planar = false;
        }
    }

    // ---- frames and lossy flag
    auto nf = frame_count(ds, opt);
    if (!nf) return nf.take_error();
    pi.number_of_frames = *nf;
    if (auto lossy = ds.get_string(tags::LossyImageCompression)) pi.lossy_attribute = *lossy == "01";

    // ---- size math: must not overflow for one frame (the frame layer checks totals).
    if (pi.native_frame_bits() == 0 || pi.decoded_frame_bytes() == 0)
        return Status(Errc::LimitExceeded, "pixel module: frame size overflows", 0, PixelData);
    return pi;
}

const Dataset* icon_image(const Dataset& ds) noexcept {
    const Element* e = ds.find(tags::IconImageSequence);
    if (!e || !e->is_sequence() || e->items.empty()) return nullptr;
    return &e->items.front();
}

}  // namespace siaara::image
