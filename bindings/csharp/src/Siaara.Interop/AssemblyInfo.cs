using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// Every native type is blittable; marshalling is done by hand in the generated safe overloads.
[assembly: DisableRuntimeMarshalling]
// The resolver in NativeLibraryLoader loads the library from a fixed path next to the
// application; the default search (application directory, PATH, current directory) is never used.
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
[assembly: InternalsVisibleTo("Siaara")]
[assembly: InternalsVisibleTo("Siaara.Tests")]
