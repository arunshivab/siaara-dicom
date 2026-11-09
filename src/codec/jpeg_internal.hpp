// SIAARA DICOM Toolkit - JPEG internals shared by the decoder, encoder and tests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace siaara::codec::jpeg_detail {

// Zig-zag sequence of T.81 figure A.6: kZigzag[k] is the natural (row-major) index of the
// k-th coefficient in zig-zag order. Entries 64..79 repeat 63 so a run past the end stays in range.
inline constexpr std::array<std::uint8_t, 80> make_zigzag() {
    std::array<std::uint8_t, 80> z{};
    int k = 0;
    for (int d = 0; d < 15; ++d) {           // anti-diagonal d = row + col
        if (d % 2 == 0) {                    // even diagonals run bottom-left to top-right
            for (int row = (d < 8 ? d : 7); row >= 0 && d - row < 8; --row)
                z[static_cast<std::size_t>(k++)] = static_cast<std::uint8_t>(row * 8 + (d - row));
        } else {                             // odd diagonals run top-right to bottom-left
            for (int col = (d < 8 ? d : 7); col >= 0 && d - col < 8; --col)
                z[static_cast<std::size_t>(k++)] = static_cast<std::uint8_t>((d - col) * 8 + col);
        }
    }
    for (; k < 80; ++k) z[static_cast<std::size_t>(k)] = 63;
    return z;
}
inline constexpr std::array<std::uint8_t, 80> kZigzag = make_zigzag();

// T.81 Annex K example tables.
extern const std::uint8_t kLumaQuant[64];    // Table K.1, natural order
extern const std::uint8_t kChromaQuant[64];  // Table K.2, natural order
struct HuffSpec {
    std::uint8_t counts[17] = {};  // counts[l] = number of codes of length l (1..16), [0] unused
    std::uint8_t values[256] = {};
    int nvalues = 0;
};
extern const HuffSpec kStdDcLuma, kStdDcChroma, kStdAcLuma, kStdAcChroma;  // Tables K.3 - K.6

// Builds a length-limited (16 bit) Huffman code for the symbols with non-zero frequency, keeping
// the all-ones code point unused (T.81 Annex K.2). Symbols 0..255.
void build_huffman_spec(const std::uint32_t freq[256], HuffSpec& spec);

// Code assignment of T.81 Annex C: code[] / len[] indexed by symbol. Returns false when the
// counts over-subscribe the code space.
bool make_codes(const HuffSpec& spec, std::uint16_t code[256], std::uint8_t len[256]);

// 8x8 inverse DCT (T.81 A.3.3). coef: dequantized coefficients in natural order.
// Writes level-shifted samples clamped to 0..maxval.
void idct8x8(const std::int32_t* coef, std::uint16_t* out, std::size_t stride, int level_shift,
             int maxval);
// Same result as idct8x8 for a block whose only non-zero coefficient is coef[0].
void idct8x8_dc(std::int32_t dc, std::uint16_t* out, std::size_t stride, int level_shift, int maxval);

// 8x8 forward DCT (T.81 A.3.3) of level-shifted samples, natural order in and out.
void fdct8x8(const double* in, double* out);

}  // namespace siaara::codec::jpeg_detail
