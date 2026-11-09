// Fuzz target: clinical object readers on a parsed Part 10 file: Structured Report content trees,
// Segmentation (description and every frame's mask), RT Structure Set and softcopy presentation
// states. All readers run on every input, lenient and strict.
#include "clinical/gsps.hpp"
#include "clinical/rtstruct.hpp"
#include "clinical/seg.hpp"
#include "clinical/sr.hpp"
#include "core/reader.hpp"
#include "fuzz.hpp"
#include "fuzz_util.hpp"

using namespace siaara;
using namespace siaara::clinical;

SIAARA_FUZZ_TARGET(clinical) {
    ReadOptions ro;
    ro.max_elements = 200'000;
    ro.max_fragments = 50'000;
    ro.max_inflated_size = 32u << 20;
    auto f = read_part10(siaara_fuzz::to_bytes(data, size), ro);
    if (!f) return 0;
    const Dataset& ds = f->dataset;
    for (bool lenient : {true, false}) {
        SrReadOptions so;
        so.lenient = lenient;
        so.max_nodes = 100'000;
        (void)read_sr(ds, so);
        RtReadOptions rto;
        rto.lenient = lenient;
        rto.max_points = 1u << 20;
        (void)read_rtstruct(ds, rto);
        (void)read_presentation_state(ds, lenient);
        auto seg = read_segmentation(ds, lenient);
        if (seg && lenient) {
            const std::uint64_t px = std::uint64_t{seg->rows} * seg->columns;
            if (px > 0 && px <= (1u << 20)) {
                const std::uint32_t n = std::min<std::uint32_t>(seg->number_of_frames, 4);
                for (std::uint32_t i = 0; i < n; ++i) (void)segment_mask(ds, f->transfer_syntax, *seg, i);
                if (seg->number_of_frames > 0) (void)segment_mask(ds, f->transfer_syntax, *seg, seg->number_of_frames);
            }
        }
    }
    return 0;
}
