// SIAARA DICOM Toolkit - named data layer: values at a point, waveforms, RT control points and NM
// frame index vectors, read exactly as the standard defines them (no derived metrics).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"
#include "image/lut.hpp"
#include "image/pixel_info.hpp"
#include "modules/modules.hpp"
#include "volume/volume.hpp"

namespace siaara::modules {

// ---------------------------------------------------------------- pixel values
enum class UnitsSource : std::int32_t { None = 0, RescaleType = 1, ModalityLutType = 2, DoseUnits = 3, CtHu = 4, PetUnits = 5 };

struct PixelValue {
    image::SampleKind kind = image::SampleKind::Integer;
    std::int64_t stored = 0;
    double stored_real = 0;
    bool has_modality = false;
    bool modality_lut = false;
    bool dose_grid_scaling = false;
    double modality_value = 0;
    double slope = 1, intercept = 0;
    UnitsSource units_source = UnitsSource::None;
    std::string units;
};

// Units of the modality values of `ds` under the transform m (see siaara_pixel_value).
UnitsSource modality_units(const Dataset& ds, const image::ModalityTransform& m, std::string& units);

// The sample at (column, row, sample) of a decoded frame in the DICOM native layout (FrameReader
// output), with the frame's modality transform applied when `m` is given and the image is
// single-sample integer data.
Result<PixelValue> pixel_value(const Dataset& ds, const image::PixelInfo& pi, std::span<const std::uint8_t> frame,
                               const image::ModalityTransform* m, std::uint32_t column, std::uint32_t row,
                               std::uint32_t sample);

// ---------------------------------------------------------------- volumes
struct VolumeSample {
    bool inside = false;
    double index[3] = {0, 0, 0};       // continuous voxel index
    std::int64_t voxel[3] = {0, 0, 0}; // nearest voxel index (may be outside the grid)
    double value = 0;                  // inside only
};
// Value at a patient coordinate (mm): nearest voxel, or trilinear when `linear`.
Result<VolumeSample> volume_value_at(const volume::Volume& v, const double patient[3], bool linear);

// ---------------------------------------------------------------- waveforms (PS3.3 C.10.9)
struct WaveformGroup {
    std::uint32_t channels = 0, samples = 0, bits_allocated = 0;
    double sampling_frequency = 0;
    std::string interpretation;
    std::optional<std::int64_t> padding;
    std::optional<double> time_offset, trigger_time_offset;
    std::uint32_t data_samples = 0;  // per channel, as Waveform Data holds them
};
struct WaveformChannel {
    std::uint32_t bits_stored = 0;
    std::optional<std::int64_t> number;
    std::optional<double> sensitivity, correction, baseline, time_skew, sample_skew, offset, filter_low, filter_high, notch;
};
Result<std::uint32_t> waveform_group_count(const Dataset& ds);
Result<WaveformGroup> waveform_group(const Dataset& ds, std::uint32_t group);
Result<WaveformChannel> waveform_channel(const Dataset& ds, std::uint32_t group, std::uint32_t channel);
enum class WaveformText : std::int32_t {
    Label = 0, Status = 1, UnitsCode = 2, UnitsMeaning = 3, SourceCode = 4, SourceScheme = 5, SourceMeaning = 6, GroupLabel = 7
};
Result<std::string> waveform_text(const Dataset& ds, std::uint32_t group, std::uint32_t channel, WaveformText which);
// Samples are read only for the Waveform Bits Allocated / Waveform Sample Interpretation pairs of
// PS3.3 Table C.10-10 (8: SB UB MB AB; 16: SS US; 32: SL UL; 64: SV UV); any other pair is
// Malformed (R-137). The vector forms never throw: LimitExceeded past kNamedDataMemoryBudget,
// OutOfMemory when the allocation fails (R-123).
Result<std::vector<std::int64_t>> waveform_raw(const Dataset& ds, std::uint32_t group, std::uint32_t channel);
// sample x sensitivity x correction + baseline (G.711 codes expanded first; padding -> NaN).
Result<std::vector<double>> waveform_scaled(const Dataset& ds, std::uint32_t group, std::uint32_t channel);
// Samples per channel (WaveformGroup::data_samples) after the same checks as waveform_raw
// (scaled: as waveform_scaled, so a channel without sensitivity is NotFound). Nothing is decoded.
Result<std::uint32_t> waveform_sample_count(const Dataset& ds, std::uint32_t group, std::uint32_t channel, bool scaled);
// The samples decoded straight into `out`, whose size must be waveform_sample_count(). No allocation.
Status waveform_raw_into(const Dataset& ds, std::uint32_t group, std::uint32_t channel, std::span<std::int64_t> out);
Status waveform_samples_into(const Dataset& ds, std::uint32_t group, std::uint32_t channel, bool scaled, std::span<double> out);
// ITU-T G.711 decoder outputs: mu-law 14-bit (-8031..8031), A-law 13-bit (-4032..4032).
std::int32_t g711_mulaw(std::uint8_t code) noexcept;
std::int32_t g711_alaw(std::uint8_t code) noexcept;

// ---------------------------------------------------------------- RT control points
// The control point sequence of a beam item (RT / Ion plan, treatment record), or nullptr.
const Element* control_point_sequence(const Dataset& beam) noexcept;
// Item index at or before cp holding `tag` (PS3.3: unchanged values are not repeated).
Result<std::uint32_t> control_point_source(const Dataset& beam, std::uint32_t cp, Tag tag);
// Leaf / Jaw Positions of a device type at cp (effective: inherited from earlier control points).
Result<std::vector<double>> device_positions(const Dataset& beam, std::uint32_t cp, std::string_view device_type,
                                             bool effective, std::uint32_t* from_cp);
// The number of positions (from the value count alone; nothing decoded), and the values written
// into `out`, whose size must be that count.
Result<std::size_t> device_positions_count(const Dataset& beam, std::uint32_t cp, std::string_view device_type,
                                           bool effective, std::uint32_t* from_cp);
Status device_positions_into(const Dataset& beam, std::uint32_t cp, std::string_view device_type, bool effective,
                             std::span<double> out);

// ---------------------------------------------------------------- NM index vectors
struct NmFrameIndex {
    std::uint32_t present = 0, in_pointer = 0;
    std::uint32_t values[9] = {};  // energy window, detector, phase, rotation, R-R, time slot, slice, angular view, time slice
};
Result<NmFrameIndex> nm_frame_index(const Dataset& ds, std::uint32_t frame);

}  // namespace siaara::modules
