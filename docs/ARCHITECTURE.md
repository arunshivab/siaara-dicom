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
