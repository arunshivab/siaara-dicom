// SIAARA DICOM Toolkit - grayscale transformations.
// Written from DICOM PS3.3 C.11.1 (Modality LUT), C.11.2 (VOI LUT: window centre/width with the
// LINEAR, LINEAR_EXACT and SIGMOID functions of C.11.2.1.2 / C.11.2.1.3, VOI LUT Sequence),
// C.11.6 (Presentation LUT), C.7.6.16.2.9 / C.7.6.16.2.10 (Pixel Value Transformation and Frame
// VOI LUT functional groups) and PS3.4 N.2 (grayscale pipeline order).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/pixel_info.hpp"

namespace siaara::image {

namespace tags {
constexpr Tag WindowCenter                    = 0x00281050;
constexpr Tag WindowWidth                     = 0x00281051;
constexpr Tag WindowCenterWidthExplanation    = 0x00281055;
constexpr Tag VoiLutFunction                  = 0x00281056;
constexpr Tag RescaleType                     = 0x00281054;
constexpr Tag ModalityLutSequence             = 0x00283000;
constexpr Tag LutDescriptor                   = 0x00283002;
constexpr Tag LutExplanation                  = 0x00283003;
constexpr Tag ModalityLutType                 = 0x00283004;
constexpr Tag LutData                         = 0x00283006;
constexpr Tag VoiLutSequence                  = 0x00283010;
constexpr Tag PresentationLutSequence         = 0x20500010;
constexpr Tag PresentationLutShape            = 0x20500020;
constexpr Tag SharedFunctionalGroupsSequence  = 0x52009229;
constexpr Tag PerFrameFunctionalGroupsSequence = 0x52009230;
constexpr Tag PixelValueTransformationSequence = 0x00289145;
constexpr Tag FrameVoiLutSequence             = 0x00289132;
}  // namespace tags

// A lookup table as described by a LUT Descriptor (C.11.1.1.1 / C.11.2.1.1 / C.11.6.1.1).
struct LutTable {
    std::uint32_t entries = 0;        // 1..65536 (descriptor value 0 means 65536)
    std::int64_t first_mapped = 0;    // first input value mapped
    std::uint16_t bits = 16;          // bits per entry: output range 0 .. 2^bits - 1
    std::vector<std::uint16_t> data;  // `entries` values
    std::string explanation;

    // Input below first_mapped -> first entry; at or past first_mapped + entries -> last entry.
    std::uint16_t lookup(std::int64_t x) const noexcept {
        const std::int64_t i = x - first_mapped;
        if (i <= 0) return data[0];
        if (i >= static_cast<std::int64_t>(entries)) return data[entries - 1];
        return data[static_cast<std::size_t>(i)];
    }
    double max_output() const noexcept { return static_cast<double>((1u << bits) - 1u); }
};

// Reads LUT Descriptor (0028,3002) + LUT Data (0028,3006) from a sequence item. `first_signed`
// is how the second descriptor value is read when the element's VR does not say (implicit VR).
Result<LutTable> read_lut(const Dataset& item, bool first_signed, bool lenient = true);

// ---------------------------------------------------------------- modality
struct ModalityTransform {
    bool has_lut = false;   // Modality LUT Sequence used (else rescale)
    LutTable lut;
    double slope = 1.0;
    double intercept = 0.0;
    std::string rescale_type;
    bool from_functional_groups = false;
    // The data holds the transform: a Modality LUT Sequence, Rescale Slope or Rescale Intercept,
    // or Dose Grid Scaling. False: none of them is present and the identity is assumed, which
    // callers can tell apart from an explicit slope 1 / intercept 0 (testing 2026-11-11, 6.9).
    bool in_data = false;
    // RT Dose (PS3.3 C.8.8.3): slope is Dose Grid Scaling (3004,000E), intercept 0, and
    // rescale_type the Dose Units (3004,0002, e.g. "GY"). Testing-chat P0-2a.
    bool dose_grid_scaling = false;
    // An RT Dose object that also carries a non-identity Rescale Slope / Intercept: Dose Grid
    // Scaling is used and the rescale is ignored (the RT Dose IOD has no Modality LUT module).
    bool rescale_ignored = false;

    bool is_identity() const noexcept { return !has_lut && slope == 1.0 && intercept == 0.0; }
    double apply(std::int64_t stored) const noexcept {
        return has_lut ? static_cast<double>(lut.lookup(stored))
                       : static_cast<double>(stored) * slope + intercept;
    }
    // Possible output range for stored values in [lo, hi] (C.11.1.1.2).
    void output_range(std::int64_t lo, std::int64_t hi, double& out_lo, double& out_hi) const noexcept;
};

// Modality transformation for a frame: Pixel Value Transformation Sequence in the Per-frame,
// then the Shared Functional Groups; otherwise the top-level Modality LUT Sequence or Rescale
// Slope/Intercept. Identity for colour and floating point images.
// RT Dose (SOP Class 1.2.840.10008.5.1.4.1.1.481.2 or Modality RTDOSE) with Dose Grid Scaling
// (3004,000E): slope = Dose Grid Scaling, so modality values are doses in Dose Units. A missing
// or unusable Dose Grid Scaling is an error in strict mode; lenient mode leaves the stored values.
Result<ModalityTransform> modality_for_frame(const Dataset& ds, const PixelInfo& pi, std::uint32_t frame,
                                             bool lenient = true);

// ---------------------------------------------------------------- VOI
enum class VoiFunction { Linear, LinearExact, Sigmoid };

struct VoiWindow {
    double center = 0.0;
    double width = 0.0;
    VoiFunction function = VoiFunction::Linear;
    std::string explanation;
};

struct VoiSet {
    std::vector<VoiWindow> windows;   // in file order
    std::vector<LutTable> luts;       // VOI LUT Sequence items, in file order
    bool from_functional_groups = false;
    bool empty() const noexcept { return windows.empty() && luts.empty(); }
};

// VOI transformations for a frame: Frame VOI LUT Sequence (per-frame, then shared functional
// groups), else the top-level Window Center/Width/VOI LUT Function and VOI LUT Sequence.
// Windows with an invalid width are dropped (lenient) or rejected (strict).
Result<VoiSet> voi_for_frame(const Dataset& ds, const PixelInfo& pi, const ModalityTransform& m,
                             std::uint32_t frame, bool lenient = true);

// True for an RT Dose object (SOP Class UID or Modality RTDOSE).
bool is_rt_dose(const Dataset& ds);

// RT Dose module values that say what the modality values mean (PS3.3 C.8.8.3, P0-2a).
struct RtDoseInfo {
    bool is_rt_dose = false;
    std::optional<double> dose_grid_scaling;  // (3004,000E), when present and a positive number
    std::string dose_units;                   // (3004,0002) GY or RELATIVE
    std::string dose_type;                    // (3004,0004) PHYSICAL, EFFECTIVE, ERROR
    std::string dose_summation_type;          // (3004,000A) PLAN, MULTI_PLAN, FRACTION, BEAM, ...
    enum class OffsetForm { None, Relative, Absolute } offset_form = OffsetForm::None;  // (3004,000C) first value 0 / IPP z
    std::size_t offset_count = 0;             // values in the Grid Frame Offset Vector
};
RtDoseInfo read_rt_dose_info(const Dataset& ds);

// True if (c, w) is valid for the function: LINEAR needs w >= 1, the others w > 0 (C.11.2.1.2/3).
bool window_valid(double center, double width, VoiFunction fn) noexcept;

// The VOI window functions exactly as PS3.3 C.11.2.1.2 (LINEAR) and C.11.2.1.3 (LINEAR_EXACT,
// SIGMOID) define them, in double precision. The window must be valid.
double apply_window(double x, double center, double width, VoiFunction fn, double ymin, double ymax) noexcept;

// VOI LUT applied to a modality output value (rounded to the nearest integer input).
double apply_voi_lut(const LutTable& lut, double x) noexcept;

// ---------------------------------------------------------------- presentation
enum class PresentationShape { Identity, Inverse };
std::optional<PresentationShape> presentation_shape(const Dataset& ds) noexcept;

// Whether the grayscale output must be inverted to obtain P-Values (MONOCHROME1, or Presentation
// LUT Shape INVERSE, which in image IODs accompanies MONOCHROME1: never applied twice).
bool output_inverted(const PixelInfo& pi, const Dataset& ds) noexcept;

// Presentation LUT Sequence (2050,0010) item 0, if any.
Result<std::optional<LutTable>> presentation_lut(const Dataset& ds, bool lenient = true);

}  // namespace siaara::image
