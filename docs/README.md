# SIAARA documentation

## For users of the toolkit (shipped in every release package)

| Document | What it covers |
|---|---|
| [GETTING_STARTED.md](GETTING_STARTED.md) | What is in a release package, linking from C, C++ and C#, the examples |
| [CONFORMANCE.md](CONFORMANCE.md) | DICOM Conformance Statement (PS3.2): services, SOP classes, transfer syntaxes, character sets, security |
| [NETWORKING.md](NETWORKING.md) | DIMSE SCU and SCP, DICOMweb client and server: options, defaults, recipes, security checklist |
| [DATA_ACCESS.md](DATA_ACCESS.md) | Reading by path and by module, pixel and voxel values, waveforms, RT data |
| [VALIDATION.md](VALIDATION.md) | Validating a data set against its IOD; the repair function |
| [PRIVATE_DICTIONARY.md](PRIVATE_DICTIONARY.md) | The private data element dictionary: sources, rules, counts |
| [capi-notes.md](capi-notes.md) | Design notes of the C API: threading, cancellation, buffers, struct versioning |
| [../CHANGELOG.md](../CHANGELOG.md) | Releases and known limitations |
| [../SECURITY.md](../SECURITY.md) | Reporting a vulnerability |
| [../bindings/csharp/PACKAGE_README.md](../bindings/csharp/PACKAGE_README.md) | The README inside the NuGet package |

The reference of the C API is the header itself, `include/siaara/siaara.h`: every function,
structure and constant is documented there. The C# API carries the same text as XML
documentation (IntelliSense).

## For maintainers (not shipped)

| Document | What it covers |
|---|---|
| [BUILDING.md](BUILDING.md) | Building from source, options, sanitizers, release gates |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Layers, memory model, write fidelity |
| [FUZZING.md](FUZZING.md) | Fuzz targets, corpora, CI fuzzing |
| [RELEASING.md](RELEASING.md) | How a release is built; the version's single source; the NuGet package |
| [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) | The gates and steps for cutting a release, and the owner's decisions |
| [../bindings/csharp/README.md](../bindings/csharp/README.md) | Working on the C# bindings |
