// SIAARA DICOM Toolkit - 2D rendering to RGBA8.
// Pipeline (PS3.4 N.2, PS3.3 C.11): stored values -> Modality LUT / rescale -> VOI (window or
// VOI LUT) -> Presentation LUT (shape or sequence; MONOCHROME1 inversion) -> 8-bit P-Values,
// or palette / colour conversion for colour images; then display shutters, then overlays.
// The output is 8-bit sRGB-ish RGBA (alpha 255); P-Values are emitted as gray levels as-is, i.e.
// a GSDF-calibrated display is assumed (PS3.14), no display-specific correction is applied.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/color.hpp"
#include "image/frames.hpp"
#include "image/lut.hpp"
#include "image/overlay.hpp"
#include "image/pixel_info.hpp"

namespace siaara::image {

enum class VoiMode {
    File,       // the file's VOI number voi_index (windows first, then VOI LUT Sequence items);
                // falls back to FullRange when the file has none
    Explicit,   // window_center / window_width / function below
    FullRange,  // window spanning the minimum..maximum modality output present in the frame
    Identity,   // window spanning the whole possible modality output range
};

struct RenderParams {
    std::uint32_t frame = 0;
    VoiMode voi = VoiMode::File;
    std::uint32_t voi_index = 0;
    double window_center = 0.0;
    double window_width = 0.0;
    VoiFunction function = VoiFunction::Linear;
    bool invert = false;                 // in addition to MONOCHROME1 / Presentation LUT Shape INVERSE
    bool apply_presentation_lut = true;  // Presentation LUT Sequence / Shape found in the data set
    bool overlays = true;
    std::uint32_t overlay_groups = 0xFFFFu;  // bit k enables group 0x6000 + 2k
    std::array<std::uint8_t, 4> overlay_rgba{255, 255, 255, 255};
    bool shutters = true;
    std::optional<StandardPalette> pseudo_color;  // grayscale images only: map gray levels through a palette
    ColorOptions color;                           // native_layout / codec_applied_mct are set by the renderer
};

// An image ready for repeated rendering: the frame index plus everything parsed once (overlays,
// shutters, palette, presentation LUT). The Dataset must outlive the Image (per-frame functional
// groups are looked up at render time).
class Image {
public:
    static Result<Image> open(const Dataset& ds, std::string_view transfer_syntax_uid, const FrameOptions& opt = {});

    const PixelInfo& info() const noexcept { return frames_.info(); }
    const FrameReader& frames() const noexcept { return frames_; }
    const Dataset& dataset() const noexcept { return *ds_; }
    const std::vector<Overlay>& overlays() const noexcept { return overlays_; }
    const Shutters& shutters() const noexcept { return shutters_; }
    const std::optional<PaletteLut>& palette() const noexcept { return palette_; }
    bool supplemental_palette() const noexcept { return supplemental_; }
    bool inverted() const noexcept { return inverted_; }
    const std::optional<LutTable>& presentation_lut() const noexcept { return plut_; }

    // Modality and VOI for a frame (functional groups first).
    Result<ModalityTransform> modality(std::uint32_t frame) const;
    Result<VoiSet> voi(std::uint32_t frame, const ModalityTransform& m) const;

private:
    const Dataset* ds_ = nullptr;
    FrameReader frames_;
    std::vector<Overlay> overlays_;
    Shutters shutters_;
    std::optional<PaletteLut> palette_;
    bool supplemental_ = false;
    std::optional<LutTable> plut_;
    bool inverted_ = false;
};

// Renders one frame to rows * columns * 4 bytes of RGBA8.
Status render_2d(const Image& img, const RenderParams& p, std::span<std::uint8_t> rgba);
Result<std::vector<std::uint8_t>> render_2d(const Image& img, const RenderParams& p);

// Decodes a frame and applies the modality transformation, giving rows * columns * samples
// floats, colour-by-pixel (4:2:2 native data expanded by replication, planar data interleaved).
// Colour samples and floating point pixel data are passed through unchanged.
Status decode_modality_f32(const Image& img, std::uint32_t frame, std::span<float> out);

// Window spanning the minimum x1 .. maximum x2 of the frame's modality output: the "full range of
// input values present" of PS3.3 C.11.2.1.2 (LINEAR, c = (x1 + x2 + 1) / 2, w = x2 - x1 + 1) for
// integer pixel data; LINEAR_EXACT with c = (x1 + x2) / 2, w = x2 - x1 for floating point data.
Result<VoiWindow> full_range_window(const Image& img, std::uint32_t frame, std::span<const std::uint8_t> decoded,
                                    const ModalityTransform& m);

}  // namespace siaara::image
