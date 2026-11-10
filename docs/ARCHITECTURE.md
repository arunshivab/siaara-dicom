# SIAARA architecture

## Layers
- `src/core`: types, data set model, reader, writer, file I/O, transfer syntaxes.
- `src/dict`: data dictionary and UID registry, generated from PS3.6/PS3.7 by `tools/gen_dictionary.py`.
- `src/codec`: DEFLATE, RLE, JPEG (baseline, extended, lossless), JPEG-LS, JPEG 2000/HTJ2K, all written from the standards.
- `src/image`: pixel pipeline (modality LUT, VOI LUT, palettes, overlays, rendering).
- `src/net`: DIMSE (PS3.7/PS3.8), TLS adapter, DICOMweb.
- `src/capi`: stable C API (`include/siaara/siaara.h`).
- `bindings/csharp`: C# bindings over the C API.

## Rules
See the project's standards document. Short version: zero external dependencies, no exceptions on
library paths (`Result<T>` and `Status`), 64-bit checked size arithmetic on every input-derived
value, and every limit configurable with a safe default.

## Memory model
Files are memory-mapped. Element values are `Bytes` views into the mapping, so parsing a file
copies no value data. Big-endian and deflated input are the exceptions; they are converted once.

## Write fidelity (data integrity)
A file read and saved again in the same transfer syntax, with no edits, is reproduced byte for
byte. The reader records how each element was encoded (`Element::read`, `Dataset::item_length_mode`,
`DicomFile::preamble`) and the writer's defaults (`WriteOptions`, C API `siaara_save_options`)
keep it:
- **Sequence and item lengths** (`SequenceLengths`): `KeepAsRead` (default) keeps each SQ's and
  item's explicit/undefined mode; `Explicit` and `Undefined` force one mode. Explicit lengths are
  always recomputed from what is written. Sequences built in memory get explicit length, except in
  implicit VR syntaxes for private tags or tags the dictionary does not know as SQ, which get
  undefined length so that readers without the (private) dictionary still see a sequence; the
  same applies when transcoding an explicit-length SQ of that kind to implicit VR. New items
  follow their sequence. A sequence read as UN with undefined length (PS3.5 6.2.2) is written back
  as UN with implicit VR little endian items.
- **Group lengths** (`GroupLengths`): `Keep` (default) writes (gggg,0000) where present. The value
  is kept verbatim when the group's encoded size is unchanged (so even a value that was wrong in
  the source is not silently "fixed"); otherwise (an edit, a transfer syntax change) it is
  recomputed, so a kept group length is never stale. `Drop` removes them; `Recompute` writes a
  correct one for every group, adding missing ones. Group 0002's length is always recomputed.
- **Odd lengths** (`OddLengths`): `Keep` (default) writes values and encapsulated fragments that
  were read with an odd length unchanged; values built or edited in memory (any setter, or a value
  whose size changed) and fragment lists that were changed are padded to even length, as
  PS3.5 7.1.1 asks. `Pad` pads everything.
- **File meta and preamble**: the preamble as read is written back (de-identification clears it);
  the Transfer Syntax UID element is rewritten only when the syntax changes. Missing required meta
  elements are added, so files without a preamble/meta (raw data sets) cannot round-trip byte for
  byte, nor can deflated files (the stream is re-compressed) or files the reader repaired in
  lenient mode (out-of-order or duplicate tags, stray delimiters, truncation).
- **Byte order**: data sets hold values little endian in memory; big-endian files are swapped on
  read and swapped back on write. `siaara_dataset_get_raw` returns the in-memory (little-endian)
  bytes; `siaara_file_get_raw_stored` / `stored_value_bytes` return the bytes as stored.

### Reading damaged input (lenient mode)

Lenient reading never hides damage: everything it does is a read warning, and anything that
leaves the data set short of the input or reinterprets it sets a `DicomFile::read_loss` bit
(`siaara_read_report.loss`), so saves (`siaara_file_save`) and C-STORE refuse the file unless the
caller opts in (`allow_incomplete`). The first warning names the reason: `PARSE_INCOMPLETE`
(bytes skipped), `INPUT_TRUNCATED` (input ended early), `READ_RECOVERED` (a heuristic below).

| Damage | What the reader does | Report |
|---|---|---|
| Encapsulated Pixel Data with a defined length (items inside, no delimiter) | read as the fragments when the items fill the length exactly (optionally with a delimiter at its end); written back the same way | `PIXEL_DATA_DEFINED_LENGTH` warning; complete (the bytes round-trip) |
| A value length a few bytes too large (GE Genesis: 13 for 10) | only with `ReadOptions::recover_overlong_lengths` (default: the constant `kRecoverOverlongLengthsDefault` in `src/core/reader.hpp`), only for text values in implicit VR little endian, never in the file meta group, never for binary VRs, UN or group 7FE0: when the stated end does not begin a run of three element headers and 1..7 bytes earlier a run of three real headers starts (so the removed bytes and the bytes after the stated end are exactly the next header) - the first a standard dictionary attribute after the current tag, the others dictionary or private attributes in groups up to 7FE0, later values allowed to repeat the same error; the value is read that much shorter | `ELEMENT_LENGTH_RECOVERED` per element; `READ_RECOVERED` |
| Zero bytes after the last top-level element (files padded to a block size) | skipped; they are in no element (strict mode: Malformed) | `TRAILING_PADDING` warning; `PARSE_INCOMPLETE` (bytes skipped), so a save is refused unless incomplete saves are allowed |
| ACR-NEMA implicit VR big endian, no file meta | when no explicit VR is seen, the first little-endian length does not fit and the first six elements read big endian are plausible (even groups up to 7FE0, ascending, defined lengths that fit); values held little endian, labelled Implicit VR Little Endian (no UID exists for the input's encoding) | `ACR_NEMA_IMPLICIT_BIG_ENDIAN`; `READ_RECOVERED` (a save would write little endian) |

Strict mode (`lenient = false`) applies none of these.

`tests/crosscheck/roundtrip_crosscheck.py <folder> <libsiaara>` reports how many files of a corpus
round-trip byte for byte.
