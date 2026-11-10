// SIAARA DICOM Toolkit - named data layer: values at a point, waveforms, RT control points, NM
// frame index vectors (see data.hpp).
#include "modules/data.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

#include "charset/charset.hpp"
#include "image/samples.hpp"
#include "volume/geometry.hpp"
#include "volume/sampler.hpp"

namespace siaara::modules {

namespace {

constexpr Tag kModality = 0x00080060;
constexpr Tag kPetUnits = 0x00541001;
constexpr Tag kWaveformSequence = 0x54000100;
constexpr Tag kChannelDefinitionSequence = 0x003A0200;
constexpr Tag kNumberOfChannels = 0x003A0005;
constexpr Tag kNumberOfSamples = 0x003A0010;
constexpr Tag kSamplingFrequency = 0x003A001A;
constexpr Tag kMultiplexGroupLabel = 0x003A0020;
constexpr Tag kBitsAllocated = 0x54001004;
constexpr Tag kSampleInterpretation = 0x54001006;
constexpr Tag kPaddingValue = 0x5400100A;
constexpr Tag kWaveformData = 0x54001010;
constexpr Tag kGroupTimeOffset = 0x00181068;
constexpr Tag kTriggerTimeOffset = 0x00181069;
constexpr Tag kChannelNumber = 0x003A0202;
constexpr Tag kChannelLabel = 0x003A0203;
constexpr Tag kChannelStatus = 0x003A0205;
constexpr Tag kChannelSource = 0x003A0208;
constexpr Tag kSensitivity = 0x003A0210;
constexpr Tag kSensitivityUnits = 0x003A0211;
constexpr Tag kCorrection = 0x003A0212;
constexpr Tag kBaseline = 0x003A0213;
constexpr Tag kTimeSkew = 0x003A0214;
constexpr Tag kSampleSkew = 0x003A0215;
constexpr Tag kChannelOffset = 0x003A0218;
constexpr Tag kBitsStored = 0x003A021A;
constexpr Tag kFilterLow = 0x003A0220;
constexpr Tag kFilterHigh = 0x003A0221;
constexpr Tag kNotch = 0x003A0222;
constexpr Tag kCodeValue = 0x00080100;
constexpr Tag kCodingScheme = 0x00080102;
constexpr Tag kCodeMeaning = 0x00080104;

std::optional<double> opt_double(const Dataset& d, Tag t) {
    if (!d.contains(t)) return std::nullopt;
    return d.get_double(t, 0);
}

const Dataset* item(const Dataset& d, Tag seq, std::uint32_t i) {
    const Element* e = d.find(seq);
    if (!e || !e->is_sequence() || i >= e->items.size()) return nullptr;
    return &e->items[i];
}

Result<const Dataset*> group_item(const Dataset& ds, std::uint32_t group) {
    const Element* e = ds.find(kWaveformSequence);
    if (!e) return Status(Errc::NotFound, "no Waveform Sequence (5400,0100)");
    if (!e->is_sequence()) return Status(Errc::TypeMismatch, "Waveform Sequence is not a sequence");
    if (group >= e->items.size()) return Status(Errc::NotFound, "multiplex group index out of range");
    return &e->items[group];
}

Result<const Dataset*> channel_item(const Dataset& g, std::uint32_t channel) {
    const Element* e = g.find(kChannelDefinitionSequence);
    if (!e || !e->is_sequence()) return Status(Errc::NotFound, "no Channel Definition Sequence (003A,0200)");
    if (channel >= e->items.size()) return Status(Errc::NotFound, "channel index out of range");
    return &e->items[channel];
}

bool interpretation_signed(std::string_view s) noexcept { return s == "SB" || s == "SS" || s == "SL" || s == "SV"; }

// Bytes per sample for a Waveform Bits Allocated / Waveform Sample Interpretation pair of PS3.3
// Table C.10-10 (8: SB UB MB AB; 16: SS US; 32: SL UL; 64: SV UV); 0 for any other pair (R-137).
unsigned waveform_sample_bytes(std::uint32_t bits, std::string_view interp) noexcept {
    switch (bits) {
        case 8: return interp == "SB" || interp == "UB" || interp == "MB" || interp == "AB" ? 1u : 0u;
        case 16: return interp == "SS" || interp == "US" ? 2u : 0u;
        case 32: return interp == "SL" || interp == "UL" ? 4u : 0u;
        case 64: return interp == "SV" || interp == "UV" ? 8u : 0u;
        default: return 0;
    }
}

std::int64_t load_sample(const std::uint8_t* p, unsigned bytes, bool is_signed) noexcept {
    std::uint64_t v = 0;
    for (unsigned k = bytes; k-- > 0;) v = (v << 8) | p[k];
    if (is_signed && bytes < 8) {
        const std::uint64_t sign = std::uint64_t{1} << (bytes * 8 - 1);
        if (v & sign) v |= ~((sign << 1) - 1);
    }
    return static_cast<std::int64_t>(v);
}

}  // namespace

// ---------------------------------------------------------------- pixel values

UnitsSource modality_units(const Dataset& ds, const image::ModalityTransform& m, std::string& units) {
    units.clear();
    if (m.dose_grid_scaling) {
        units = m.rescale_type;
        return units.empty() ? UnitsSource::None : UnitsSource::DoseUnits;
    }
    if (!m.rescale_type.empty()) {
        units = m.rescale_type;
        return m.has_lut ? UnitsSource::ModalityLutType : UnitsSource::RescaleType;
    }
    const std::string modality = ds.get_string(kModality).value_or(std::string());
    if (modality == "CT" && !m.has_lut) {
        units = "HU";
        return UnitsSource::CtHu;
    }
    if (modality == "PT") {
        if (auto u = ds.get_string(kPetUnits)) {
            units = *u;
            return UnitsSource::PetUnits;
        }
    }
    return UnitsSource::None;
}

Result<PixelValue> pixel_value(const Dataset& ds, const image::PixelInfo& pi, std::span<const std::uint8_t> frame,
                               const image::ModalityTransform* m, std::uint32_t column, std::uint32_t row,
                               std::uint32_t sample) {
    if (column >= pi.columns || row >= pi.rows) return Status(Errc::InvalidArgument, "column or row outside the image");
    if (sample >= pi.samples_per_pixel) return Status(Errc::InvalidArgument, "sample index out of range");
    const std::uint64_t pixel = std::uint64_t{row} * pi.columns + column;
    std::uint64_t index = 0;
    if (pi.samples_per_pixel == 1) {
        index = pixel;
    } else if (pi.native_subsampled() && frame.size() < pi.pixels() * pi.samples_per_pixel * ((pi.bits_allocated + 7u) / 8u)) {
        // Native YBR 4:2:2: Y0 Y1 Cb Cr per pair of pixels in a row (PS3.3 C.7.6.3.1.2).
        const std::uint64_t pair = (std::uint64_t{row} * ((pi.columns + 1u) / 2u)) + column / 2u;
        index = pair * 4u + (sample == 0 ? (column & 1u) : 1u + sample);
    } else if (pi.planar) {
        index = std::uint64_t{sample} * pi.pixels() + pixel;
    } else {
        index = pixel * pi.samples_per_pixel + sample;
    }
    PixelValue v;
    v.kind = pi.kind;
    if (pi.kind == image::SampleKind::Float32) {
        if ((index + 1) * 4 > frame.size()) return Status(Errc::Malformed, "frame shorter than its attributes");
        v.stored_real = static_cast<double>(image::detail::load_f32(frame.data() + static_cast<std::size_t>(index) * 4u));
        return v;
    }
    if (pi.kind == image::SampleKind::Float64) {
        if ((index + 1) * 8 > frame.size()) return Status(Errc::Malformed, "frame shorter than its attributes");
        v.stored_real = image::detail::load_f64(frame.data() + static_cast<std::size_t>(index) * 8u);
        return v;
    }
    const image::detail::SampleReader rd(pi);
    const std::uint64_t need = rd.one_bit ? index / 8 + 1 : (index + 1) * rd.bytes;
    if (need > frame.size()) return Status(Errc::Malformed, "frame shorter than its attributes");
    v.stored = rd.value(frame.data(), index);
    v.stored_real = static_cast<double>(v.stored);
    if (m && pi.samples_per_pixel == 1) {
        v.has_modality = true;
        v.modality_in_data = m->in_data;
        v.modality_lut = m->has_lut;
        v.dose_grid_scaling = m->dose_grid_scaling;
        v.slope = m->has_lut ? 1.0 : m->slope;
        v.intercept = m->has_lut ? 0.0 : m->intercept;
        v.modality_value = m->apply(v.stored);
        v.units_source = modality_units(ds, *m, v.units);
    }
    return v;
}

// ---------------------------------------------------------------- volumes

Result<VolumeSample> volume_value_at(const volume::Volume& v, const double patient[3], bool linear) {
    if (!std::isfinite(patient[0]) || !std::isfinite(patient[1]) || !std::isfinite(patient[2]))
        return Status(Errc::InvalidArgument, "patient coordinate is not finite");
    auto t = volume::GridTransform::make(v.grid());
    if (!t) return Status(Errc::Malformed, "volume grid is not invertible");
    VolumeSample r;
    const volume::Vec3 idx = t->index(volume::Vec3{patient[0], patient[1], patient[2]});
    r.index[0] = idx.x;
    r.index[1] = idx.y;
    r.index[2] = idx.z;
    for (int a = 0; a < 3; ++a) {
        const double c = std::floor(r.index[a] + 0.5);
        r.voxel[a] = std::fabs(c) < 9.0e18 ? static_cast<std::int64_t>(c)
                                            : (c < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max());
    }
    auto sample = [&](const auto& acc) {
        r.inside = acc.inside(idx.x, idx.y, idx.z);
        if (r.inside)
            r.value = linear ? volume::detail::sample_linear(acc, idx.x, idx.y, idx.z)
                             : volume::detail::sample_nearest(acc, idx.x, idx.y, idx.z);
    };
    if (v.type() == volume::VoxelType::Int16) sample(volume::detail::VoxelAccess<std::int16_t>(v.i16(), v.grid()));
    else sample(volume::detail::VoxelAccess<float>(v.f32(), v.grid()));
    return r;
}

// ---------------------------------------------------------------- waveforms

std::int32_t g711_mulaw(std::uint8_t code) noexcept {
    const unsigned u = static_cast<unsigned>(~code) & 0xFFu;
    int t = static_cast<int>(((u & 0x0Fu) << 3) + 0x84u);
    t <<= (u & 0x70u) >> 4;
    const int linear16 = (u & 0x80u) ? (0x84 - t) : (t - 0x84);
    return linear16 / 4;  // 14-bit decoder output (G.711 Table 2a)
}

std::int32_t g711_alaw(std::uint8_t code) noexcept {
    const unsigned a = static_cast<unsigned>(code ^ 0x55u);
    int t = static_cast<int>((a & 0x0Fu) << 4);
    const unsigned seg = (a & 0x70u) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else {
        t += 0x108;
        t <<= seg - 1;
    }
    const int linear16 = (a & 0x80u) ? t : -t;
    return linear16 / 8;  // 13-bit decoder output (G.711 Table 1a)
}

Result<std::uint32_t> waveform_group_count(const Dataset& ds) {
    const Element* e = ds.find(kWaveformSequence);
    if (!e) return Status(Errc::NotFound, "no Waveform Sequence (5400,0100)");
    if (!e->is_sequence()) return Status(Errc::TypeMismatch, "Waveform Sequence is not a sequence");
    return static_cast<std::uint32_t>(e->items.size());
}

Result<WaveformGroup> waveform_group(const Dataset& ds, std::uint32_t group) {
    auto gi = group_item(ds, group);
    if (!gi) return gi.error();
    const Dataset& g = *gi.value();
    WaveformGroup w;
    w.channels = static_cast<std::uint32_t>(g.get_int(kNumberOfChannels).value_or(0));
    w.samples = static_cast<std::uint32_t>(g.get_int(kNumberOfSamples).value_or(0));
    w.bits_allocated = static_cast<std::uint32_t>(g.get_int(kBitsAllocated).value_or(0));
    w.sampling_frequency = g.get_double(kSamplingFrequency).value_or(0);
    w.interpretation = g.get_string(kSampleInterpretation).value_or(std::string());
    w.time_offset = opt_double(g, kGroupTimeOffset);
    w.trigger_time_offset = opt_double(g, kTriggerTimeOffset);
    // Only the Bits Allocated / Sample Interpretation pairs of PS3.3 Table C.10-10 are read (R-137).
    const unsigned bytes = waveform_sample_bytes(w.bits_allocated, w.interpretation);
    if (const Element* p = g.find(kPaddingValue); p && bytes && !p->is_sequence() && p->value.size() >= bytes)
        w.padding = load_sample(p->value.data(), bytes, interpretation_signed(w.interpretation));
    if (const Element* d = g.find(kWaveformData); d && !d->is_sequence() && !d->is_encapsulated() && w.channels && bytes) {
        const std::uint64_t total = d->value.size() / bytes;
        const std::uint64_t per = total / w.channels;
        w.data_samples = static_cast<std::uint32_t>(std::min<std::uint64_t>(per, w.samples));
    }
    return w;
}

Result<WaveformChannel> waveform_channel(const Dataset& ds, std::uint32_t group, std::uint32_t channel) {
    auto gi = group_item(ds, group);
    if (!gi) return gi.error();
    auto ci = channel_item(*gi.value(), channel);
    if (!ci) return ci.error();
    const Dataset& c = *ci.value();
    WaveformChannel w;
    w.bits_stored = static_cast<std::uint32_t>(c.get_int(kBitsStored).value_or(0));
    if (c.contains(kChannelNumber)) w.number = c.get_int(kChannelNumber);
    w.sensitivity = opt_double(c, kSensitivity);
    w.correction = opt_double(c, kCorrection);
    w.baseline = opt_double(c, kBaseline);
    w.time_skew = opt_double(c, kTimeSkew);
    w.sample_skew = opt_double(c, kSampleSkew);
    w.offset = opt_double(c, kChannelOffset);
    w.filter_low = opt_double(c, kFilterLow);
    w.filter_high = opt_double(c, kFilterHigh);
    w.notch = opt_double(c, kNotch);
    return w;
}

Result<std::string> waveform_text(const Dataset& ds, std::uint32_t group, std::uint32_t channel, WaveformText which) {
    auto gi = group_item(ds, group);
    if (!gi) return gi.error();
    const Dataset& g = *gi.value();
    const charset::CharacterSet cs0 = charset::character_set_of(ds);
    const charset::CharacterSet csg = charset::character_set_of(g, &cs0);
    // Decoded once and split once whatever the value count (R-111).
    auto text_of = [](const Dataset& d, Tag t, const charset::CharacterSet& cs) -> Result<std::string> {
        const Element* e = d.find(t);
        if (!e) return Status(Errc::NotFound, "not present");
        if (e->value.empty()) return std::string();
        if (auto all = charset::get_utf8_all(d, t, &cs)) return std::move(*all);
        return d.get_string(t).value_or(std::string());  // not a string VR: its first value as text
    };
    if (which == WaveformText::GroupLabel) return text_of(g, kMultiplexGroupLabel, csg);
    auto ci = channel_item(g, channel);
    if (!ci) return ci.error();
    const Dataset& c = *ci.value();
    const charset::CharacterSet csc = charset::character_set_of(c, &csg);
    switch (which) {
        case WaveformText::Label: return text_of(c, kChannelLabel, csc);
        case WaveformText::Status: return text_of(c, kChannelStatus, csc);
        case WaveformText::UnitsCode:
        case WaveformText::UnitsMeaning: {
            const Dataset* u = item(c, kSensitivityUnits, 0);
            if (!u) return Status(Errc::NotFound, "no Channel Sensitivity Units Sequence item");
            return text_of(*u, which == WaveformText::UnitsCode ? kCodeValue : kCodeMeaning, charset::character_set_of(*u, &csc));
        }
        case WaveformText::SourceCode:
        case WaveformText::SourceScheme:
        case WaveformText::SourceMeaning: {
            const Dataset* s = item(c, kChannelSource, 0);
            if (!s) return Status(Errc::NotFound, "no Channel Source Sequence item");
            const Tag t = which == WaveformText::SourceCode ? kCodeValue : which == WaveformText::SourceScheme ? kCodingScheme : kCodeMeaning;
            return text_of(*s, t, charset::character_set_of(*s, &csc));
        }
        default: return Status(Errc::InvalidArgument, "unknown waveform text");
    }
}

namespace {

// The samples of one channel as Waveform Data holds them (interleaved by channel).
struct RawChannel {
    WaveformGroup group;
    const std::uint8_t* data = nullptr;
    unsigned bytes = 0;  // per sample
    bool is_signed = false;
    std::uint32_t channel = 0;

    std::int64_t sample(std::uint64_t s) const noexcept {
        const auto idx = static_cast<std::size_t>(s * group.channels + channel);  // < value size (data_samples bound)
        return load_sample(data + idx * bytes, bytes, is_signed);
    }
};

Result<RawChannel> raw_channel(const Dataset& ds, std::uint32_t group, std::uint32_t channel) {
    auto wg = waveform_group(ds, group);
    if (!wg) return wg.error();
    RawChannel r;
    r.group = wg.value();
    const WaveformGroup& g = r.group;
    if (channel >= g.channels) return Status(Errc::NotFound, "channel index out of range");
    r.bytes = waveform_sample_bytes(g.bits_allocated, g.interpretation);
    if (!r.bytes)
        return Status(Errc::Malformed,
                      "Waveform Bits Allocated and Waveform Sample Interpretation are not a pair of PS3.3 Table C.10-10 "
                      "(8: SB UB MB AB; 16: SS US; 32: SL UL; 64: SV UV)");
    const Dataset& gi = *group_item(ds, group).value();
    const Element* d = gi.find(kWaveformData);
    if (!d || d->is_sequence()) return Status(Errc::NotFound, "no Waveform Data (5400,1010)");
    if (d->is_encapsulated()) return Status(Errc::Unsupported, "encapsulated Waveform Data");
    r.data = d->value.data();
    r.is_signed = interpretation_signed(g.interpretation);
    r.channel = channel;
    return r;
}

// sample x sensitivity x correction + baseline for one channel.
struct Scaling {
    double k = 1, b = 0;
};
Result<Scaling> channel_scaling(const Dataset& ds, std::uint32_t group, std::uint32_t channel) {
    auto wc = waveform_channel(ds, group, channel);
    if (!wc) return wc.error();
    const WaveformChannel& c = wc.value();
    if (!c.sensitivity) return Status(Errc::NotFound, "channel has no Channel Sensitivity (003A,0210): arbitrary units");
    return Scaling{*c.sensitivity * c.correction.value_or(1.0), c.baseline.value_or(0.0)};
}

double scaled_sample(const RawChannel& rc, const Scaling& sc, std::int64_t r) noexcept {
    const WaveformGroup& g = rc.group;
    if (g.padding && r == *g.padding) return std::numeric_limits<double>::quiet_NaN();
    double x = static_cast<double>(r);
    if (g.interpretation == "MB") x = g711_mulaw(static_cast<std::uint8_t>(r));
    else if (g.interpretation == "AB") x = g711_alaw(static_cast<std::uint8_t>(r));
    return x * sc.k + sc.b;
}

}  // namespace

Result<std::uint32_t> waveform_sample_count(const Dataset& ds, std::uint32_t group, std::uint32_t channel, bool scaled) {
    if (scaled)
        if (auto sc = channel_scaling(ds, group, channel); !sc) return sc.error();
    auto rc = raw_channel(ds, group, channel);
    if (!rc) return rc.error();
    return rc.value().group.data_samples;
}

Status waveform_raw_into(const Dataset& ds, std::uint32_t group, std::uint32_t channel, std::span<std::int64_t> out) {
    auto rc = raw_channel(ds, group, channel);
    if (!rc) return rc.error();
    if (out.size() != rc.value().group.data_samples) return Status(Errc::BufferTooSmall, "buffer does not hold the sample count");
    for (std::size_t s = 0; s < out.size(); ++s) out[s] = rc.value().sample(s);
    return Status::ok();
}

Status waveform_samples_into(const Dataset& ds, std::uint32_t group, std::uint32_t channel, bool scaled, std::span<double> out) {
    Scaling sc;
    if (scaled) {
        auto r = channel_scaling(ds, group, channel);
        if (!r) return r.error();
        sc = r.value();
    }
    auto rc = raw_channel(ds, group, channel);
    if (!rc) return rc.error();
    if (out.size() != rc.value().group.data_samples) return Status(Errc::BufferTooSmall, "buffer does not hold the sample count");
    for (std::size_t s = 0; s < out.size(); ++s) {
        const std::int64_t r = rc.value().sample(s);
        out[s] = scaled ? scaled_sample(rc.value(), sc, r) : static_cast<double>(r);
    }
    return Status::ok();
}

Result<std::vector<std::int64_t>> waveform_raw(const Dataset& ds, std::uint32_t group, std::uint32_t channel) {
    try {
        auto n = waveform_sample_count(ds, group, channel, false);
        if (!n) return n.error();
        if (!fits_named_data_budget(n.value(), sizeof(std::int64_t)))
            return Status(Errc::LimitExceeded, "waveform samples exceed the memory budget");
        std::vector<std::int64_t> out(n.value());
        if (Status s = waveform_raw_into(ds, group, channel, out); !s) return s;
        return out;
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "out of memory");
    } catch (const std::length_error&) {  // more elements than a vector can hold (32-bit builds)
        return Status(Errc::LimitExceeded, "result exceeds the memory budget");
    }
}

Result<std::vector<double>> waveform_scaled(const Dataset& ds, std::uint32_t group, std::uint32_t channel) {
    try {
        auto n = waveform_sample_count(ds, group, channel, true);
        if (!n) return n.error();
        if (!fits_named_data_budget(n.value(), sizeof(double)))
            return Status(Errc::LimitExceeded, "waveform samples exceed the memory budget");
        std::vector<double> out(n.value());
        if (Status s = waveform_samples_into(ds, group, channel, true, out); !s) return s;
        return out;
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "out of memory");
    } catch (const std::length_error&) {  // more elements than a vector can hold (32-bit builds)
        return Status(Errc::LimitExceeded, "result exceeds the memory budget");
    }
}

// ---------------------------------------------------------------- RT control points

const Element* control_point_sequence(const Dataset& beam) noexcept {
    for (Tag t : {Tag{0x300A0111}, Tag{0x300A03A8}, Tag{0x30080040}, Tag{0x30080041}}) {
        const Element* e = beam.find(t);
        if (e && e->is_sequence()) return e;
    }
    return nullptr;
}

Result<std::uint32_t> control_point_source(const Dataset& beam, std::uint32_t cp, Tag tag) {
    const Element* s = control_point_sequence(beam);
    if (!s) return Status(Errc::NotFound, "no control point sequence in this item");
    if (cp >= s->items.size()) return Status(Errc::NotFound, "control point index out of range");
    for (std::uint32_t k = cp + 1; k-- > 0;)
        if (s->items[k].contains(tag)) return k;
    return Status(Errc::NotFound, "no control point at or before this one has the attribute");
}

namespace {

constexpr Tag kLeafJawPositions = 0x300A011C;

// The Beam Limiting Device Position Sequence item of `device_type` that applies at cp.
struct PositionsItem {
    const Dataset* item = nullptr;
    std::uint32_t from_cp = 0;
    std::size_t count = 0;
};
Result<PositionsItem> positions_item(const Dataset& beam, std::uint32_t cp, std::string_view device_type, bool effective) {
    constexpr Tag kPositions = 0x300A011A, kType = 0x300A00B8;
    const Element* s = control_point_sequence(beam);
    if (!s) return Status(Errc::NotFound, "no control point sequence in this item");
    if (cp >= s->items.size()) return Status(Errc::NotFound, "control point index out of range");
    for (std::uint32_t k = cp + 1; k-- > 0;) {
        const Element* p = s->items[k].find(kPositions);
        if (p && p->is_sequence()) {
            for (const Dataset& it : p->items) {
                if (it.get_string(kType).value_or(std::string()) != device_type) continue;
                const Element* v = it.find(kLeafJawPositions);
                if (!v) continue;
                PositionsItem r;
                r.item = &it;
                r.from_cp = k;
                r.count = v->is_sequence() || v->value.empty() ? 0 : it.value_multiplicity(kLeafJawPositions);
                return r;
            }
        }
        if (!effective) break;
    }
    return Status(Errc::NotFound, "no positions for this device type");
}

}  // namespace

Result<std::size_t> device_positions_count(const Dataset& beam, std::uint32_t cp, std::string_view device_type,
                                           bool effective, std::uint32_t* from_cp) {
    auto r = positions_item(beam, cp, device_type, effective);
    if (!r) return r.error();
    if (from_cp) *from_cp = r.value().from_cp;
    return r.value().count;
}

Status device_positions_into(const Dataset& beam, std::uint32_t cp, std::string_view device_type, bool effective,
                             std::span<double> out) {
    auto r = positions_item(beam, cp, device_type, effective);
    if (!r) return r.error();
    if (out.size() != r.value().count) return Status(Errc::BufferTooSmall, "buffer does not hold the value count");
    // One pass over the DS text (R-111).
    if (!out.empty() && !r.value().item->get_doubles(kLeafJawPositions, 0, out))
        return Status(Errc::Malformed, "Leaf/Jaw Positions value is not a number");
    return Status::ok();
}

Result<std::vector<double>> device_positions(const Dataset& beam, std::uint32_t cp, std::string_view device_type,
                                             bool effective, std::uint32_t* from_cp) {
    try {
        std::uint32_t from = 0;
        auto n = device_positions_count(beam, cp, device_type, effective, &from);
        if (!n) return n.error();
        if (!fits_named_data_budget(n.value(), sizeof(double)))
            return Status(Errc::LimitExceeded, "positions exceed the memory budget");
        std::vector<double> out(n.value());
        if (Status s = device_positions_into(beam, cp, device_type, effective, out); !s) return s;
        if (from_cp) *from_cp = from;
        return out;
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "out of memory");
    } catch (const std::length_error&) {  // more elements than a vector can hold (32-bit builds)
        return Status(Errc::LimitExceeded, "result exceeds the memory budget");
    }
}

// ---------------------------------------------------------------- NM

Result<NmFrameIndex> nm_frame_index(const Dataset& ds, std::uint32_t frame) {
    static constexpr Tag kVectors[9] = {0x00540010, 0x00540020, 0x00540030, 0x00540050, 0x00540060,
                                        0x00540070, 0x00540080, 0x00540090, 0x00540100};
    // Frames past Number of Frames are refused like everywhere else (they used to read as zeros).
    // Without a readable Number of Frames the range is not known and the vectors decide.
    if (const auto frames = ds.get_int(0x00280008); frames && *frames >= 1 && frame >= *frames)
        return Status(Errc::InvalidArgument, "frame index out of range");
    NmFrameIndex r;
    std::vector<Tag> pointer;
    if (const Element* f = ds.find(0x00280009); f && !f->is_sequence()) {
        const std::uint8_t* p = f->value.data();
        for (std::size_t i = 0; i + 4 <= f->value.size(); i += 4)
            pointer.push_back(make_tag(static_cast<std::uint16_t>(p[i] | (p[i + 1] << 8)),
                                       static_cast<std::uint16_t>(p[i + 2] | (p[i + 3] << 8))));
    }
    for (int k = 0; k < 9; ++k) {
        for (Tag t : pointer)
            if (t == kVectors[k]) r.in_pointer |= 1u << k;
        if (!ds.contains(kVectors[k])) continue;
        r.present |= 1u << k;
        if (frame < ds.value_multiplicity(kVectors[k]))
            r.values[k] = static_cast<std::uint32_t>(ds.get_int(kVectors[k], frame).value_or(0));
    }
    return r;
}

}  // namespace siaara::modules
