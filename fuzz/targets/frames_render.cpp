// Fuzz target: frame reader (native and encapsulated, every codec) and the 2D render pipeline
// (modality/VOI/presentation LUTs, palettes, colour, overlays, shutters) on a parsed Part 10 file.
// The last input byte selects render parameters.
#include "core/reader.hpp"
#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "image/render.hpp"

using namespace siaara;

SIAARA_FUZZ_TARGET(frames_render) {
    if (size < 2) return 0;
    const std::uint8_t sel = data[size - 1];
    ReadOptions ro;
    ro.max_elements = 200'000;
    ro.max_fragments = 50'000;
    ro.max_inflated_size = 32u << 20;
    auto f = read_part10(siaara_fuzz::to_bytes(data, size - 1), ro);
    if (!f) return 0;
    image::FrameOptions fo;
    fo.memory_budget = 32u << 20;
    fo.threads = 1;
    fo.lenient = (sel & 0x80u) == 0;
    auto fr = image::FrameReader::open(f->dataset, f->transfer_syntax, fo);
    if (fr) {
        const std::uint32_t n = fr->frame_count();
        // Large (valid) frames are left to the render call below (one decode per input keeps each
        // run well under the 2 s hang limit).
        if (n > 0 && fr->frame_bytes() <= (1u << 18)) {
            (void)fr->decode(0);
            if (n > 1) (void)fr->decode(n - 1);
            if (n >= 2 && fr->frame_bytes() * 2 <= (16u << 20)) (void)fr->decode_range(0, 2);
            if (fr->encapsulated()) {
                std::vector<std::uint8_t> enc;
                (void)fr->encoded_frame(n - 1, enc);
            }
            (void)fr->native_view(0);
        }
        (void)fr->decode(n);  // one past the end must fail cleanly
    }
    auto img = image::Image::open(f->dataset, f->transfer_syntax, fo);
    if (!img) return 0;
    const image::PixelInfo& pi = img->info();
    if (pi.pixels() == 0 || pi.pixels() > (1u << 20) || img->frames().frame_bytes() > (16u << 20)) return 0;
    image::RenderParams rp;
    rp.frame = (sel & 1u) ? pi.number_of_frames - 1 : 0;
    static constexpr image::VoiMode kModes[] = {image::VoiMode::File, image::VoiMode::Explicit,
                                                image::VoiMode::FullRange, image::VoiMode::Identity};
    rp.voi = kModes[(sel >> 1) & 3u];
    rp.window_center = 40;
    rp.window_width = (sel & 8u) ? 0.5 : 400;
    rp.function = (sel & 16u) ? image::VoiFunction::Sigmoid : image::VoiFunction::Linear;
    rp.invert = (sel & 32u) != 0;
    if (sel & 64u) rp.pseudo_color = image::StandardPalette::HotIron;
    (void)image::render_2d(*img, rp);
    if (pi.samples_per_pixel <= 4 && img->frames().frame_bytes() <= (1u << 18)) {
        std::vector<float> fl(static_cast<std::size_t>(pi.pixels() * pi.samples_per_pixel));
        (void)image::decode_modality_f32(*img, rp.frame, fl);
    }
    return 0;
}
