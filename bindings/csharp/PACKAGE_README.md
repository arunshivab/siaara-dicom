# SIAARA DICOM Toolkit for .NET

Managed bindings (`Siaara.dll`, `Siaara.Interop.dll`, .NET 10) over the SIAARA native library.
The native library for each supported platform is inside this package under
`runtimes/<rid>/native/` and is copied next to your application by `dotnet build` / `publish`.
The package has **no package dependencies**; the toolkit has no third-party dependencies at all.

| Runtime identifier | Native library |
|---|---|
| `win-x64`, `win-x86`, `win-arm64` | `siaara.dll` |
| `linux-x64`, `linux-arm64` (glibc) | `libsiaara.so` |
| `osx-x64`, `osx-arm64` | `libsiaara.dylib` (universal binary) |

32-bit x86 Linux and musl (Alpine) are not .NET targets of this package.

## Use

Put the `.nupkg` in a folder and name that folder as a package source (`nuget.config`):

```xml
<configuration>
  <packageSources>
    <clear />
    <add key="siaara-local" value="./packages" />
  </packageSources>
</configuration>
```

```xml
<ItemGroup>
  <PackageReference Include="Siaara" Version="x.y.z" />
</ItemGroup>
```

```csharp
using Siaara;

using DicomFile file = DicomFile.Open("image.dcm");
Console.WriteLine(file.Dataset.GetString(DicomTags.PatientName));
Console.WriteLine(SiaaraLibrary.Version);
```

Namespaces: `Siaara` (files, data sets, pixels, transcoding, store, de-identification),
`Siaara.Network` (DIMSE SCU calls in `Dimse`, the SCP in `StorageScp`), `Siaara.Web`
(`DicomWebClient`, `DicomWebServer`), `Siaara.Clinical`, `Siaara.Volumes`.

## Notes

- The assemblies are trimmable and NativeAOT compatible. A framework-dependent application needs
  nothing but the .NET 10 runtime.
- The native library is loaded by absolute path from `runtimes/<rid>/native/` below the
  application folder, or from the application folder itself (RID-specific publish). The current
  directory and `PATH` are never searched.
- Debug symbols are embedded in the assemblies. XML documentation is included.
- TLS uses the operating system's TLS stack (SChannel, Network.framework, the system OpenSSL 3
  on Linux, loaded on demand).
- A product built on the toolkit issues its own DICOM Conformance Statement; the toolkit's
  statement (`docs/CONFORMANCE.md` in the release archive) has the tables to copy from.

## Licence

Proprietary. Copyright (c) 2026 Sthraas Intelligence Pvt Ltd. All rights reserved. See the
`LICENSE` file in this package.
