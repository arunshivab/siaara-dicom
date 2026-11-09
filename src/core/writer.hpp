// SIAARA DICOM Toolkit - data set and Part 10 file writer (PS3.5 7, PS3.10 7)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/status.hpp"

namespace siaara {

constexpr std::string_view kImplementationClassUid = "2.25.208936390756097257212108686309372694101";
constexpr std::string_view kImplementationVersionName = "SIAARA_010";

// How sequence (SQ) and item lengths are encoded.
enum class SequenceLengths : std::uint8_t {
    KeepAsRead,  // default: each SQ and item keeps the length mode it was read with (see below)
    Explicit,    // every SQ and item gets an explicit length
    Undefined,   // every SQ and item gets undefined length + delimitation item
};

// What happens to group length elements (gggg,0000) outside group 0002.
enum class GroupLengths : std::uint8_t {
    Keep,       // default: written where present; an unchanged group keeps its value as read, an
                // edited group (or a transfer syntax change) gets its value recomputed, never stale
    Drop,       // removed (they are retired, PS3.5 7.2)
    Recompute,  // a correct group length is written for every group, added where missing
};

// What happens to values with an odd length (PS3.5 7.1.1 asks for even lengths).
enum class OddLengths : std::uint8_t {
    Keep,  // default: values and fragments read with an odd length are written unchanged;
           // values built or edited in memory are padded
    Pad,   // every odd value and fragment is padded to even length
};

// The defaults reproduce an unedited file byte for byte in the same transfer syntax (F1):
//  - "Edited" means: built in memory (Dataset::put, the set_* helpers, the C API setters), or a
//    value whose size differs from the size it was read with. Unedited elements carry
//    Element::read from the reader.
//  - KeepAsRead for an SQ built in memory: explicit length, except in implicit VR syntaxes where
//    the tag is private or not a sequence in the dictionary; there undefined length is used, so
//    that readers without the (private) dictionary still see a sequence. New items follow their
//    sequence. The same rule overrides an explicit length read from an explicit VR file when the
//    target syntax is implicit VR and the dictionary does not know the tag as a sequence.
//  - A sequence read as UN with undefined length (PS3.5 6.2.2) is written back as UN with
//    implicit VR little endian items under KeepAsRead in explicit VR syntaxes; Explicit and
//    Undefined write it as a normal SQ.
//  - Lengths of explicit-length SQs and items are always recomputed from what is written, so
//    they are never stale.
struct WriteOptions {
    SequenceLengths sequence_lengths = SequenceLengths::KeepAsRead;
    GroupLengths group_lengths = GroupLengths::Keep;
    OddLengths odd_lengths = OddLengths::Keep;
    int deflate_level = 6;  // for the Deflated transfer syntax
};

// Encodes a data set body in the given transfer syntax (native syntaxes only, plus deflate).
// Encapsulated pixel data is written as-is; transcoding pixel data is the codec layer's job.
Status write_dataset(const Dataset& ds, std::string_view transfer_syntax_uid,
                     std::vector<std::uint8_t>& out, const WriteOptions& opt = {});

// Writes a full Part 10 stream: 128-byte preamble, "DICM", file meta (built from `file.meta`
// plus required elements filled in from the data set), then the body.
Status write_part10(const DicomFile& file, std::vector<std::uint8_t>& out,
                    const WriteOptions& opt = {});

// The value bytes of a non-sequence, non-encapsulated element in the byte order the given
// transfer syntax stores them (data sets hold values little endian in memory; big-endian syntaxes
// swap them back exactly as the writer does). `bits_allocated` is the data set's Bits Allocated,
// used for OW pixel data wider than 16 bits. `top_level`: the element is in the main data set (not
// in a sequence item), which matters for syntaxes with big-endian Pixel Data only.
Status stored_value_bytes(const Element& e, std::string_view transfer_syntax_uid,
                          std::uint16_t bits_allocated, std::vector<std::uint8_t>& out,
                          bool top_level = true);

Status write_file(const DicomFile& file, const std::string& utf8_path, const WriteOptions& opt = {});

}  // namespace siaara
