// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics.CodeAnalysis;

// Test code only: the cases build their input files with plain synchronous file calls between awaits;
// async file I/O would add noise without testing anything. Product code keeps CA1849.
[assembly: SuppressMessage("Performance", "CA1849:Call async methods when in an async method", Justification = "Synchronous fixture I/O in tests.",
    Scope = "namespaceanddescendants", Target = "~N:Siaara.Viewer.Tests")]
