// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.
//
// Writes Golden/suv-roi-golden.json with the TOOLKIT's siaara_suv_factor and
// siaara_roi_stats_compute (through the bindings) on the synthetic cases of SuvRoiCases. Run once,
// before those functions leave the toolkit:
//   dotnet run --project app/tests/Siaara.Viewer.Tests -c Release -- --write-golden PATH
// This file is deleted when the app's own implementation replaces the toolkit calls; the golden
// file stays and the C# results are compared with it.

using System.Globalization;
using System.Text;
using System.Text.Json;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Tests;

internal static class GoldenWriter
{
    private static void Num(Utf8JsonWriter w, string name, double? v)
    {
        if (v is double d)
        {
            // Round-trip text of the double.
            w.WritePropertyName(name);
            w.WriteRawValue(d.ToString("R", CultureInfo.InvariantCulture));
        }
        else
        {
            w.WriteNull(name);
        }
    }

    public static void Write(string path)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteString("about", "Toolkit siaara_suv_factor / siaara_roi_stats_compute results on the synthetic cases of SuvRoiCases.cs (toolkit " + Tk.Version + ").");
            w.WriteStartArray("suv");
            foreach (SuvCase c in SuvRoiCases.Suv())
            {
                using TkFile f = TkFile.Create();
                c.Build(f);
                Siaara.Clinical.SuvFactors s = Siaara.Clinical.SuvFactors.Compute(f.File, c.Frame);
                w.WriteStartObject();
                w.WriteString("name", c.Name);
                w.WriteNumber("frame", c.Frame);
                w.WriteNumber("warnings", (uint)s.Warnings);
                Num(w, "bw", s.BodyWeight);
                Num(w, "lbmJames", s.LeanBodyMassJames);
                Num(w, "lbmJanmahasatian", s.LeanBodyMassJanmahasatian);
                Num(w, "bsa", s.BodySurfaceArea);
                Num(w, "ibw", s.IdealBodyWeight);
                Num(w, "decaySeconds", s.DecaySeconds);
                Num(w, "decayedDoseBq", s.DecayedDoseBq);
                w.WriteNumber("scanTimeSource", (int)s.ScanTimeSource);
                w.WriteNumber("injectionTimeSource", (int)s.InjectionTimeSource);
                w.WriteNumber("decayCorrection", (int)s.DecayCorrection);
                Num(w, "weightKg", s.WeightKg);
                Num(w, "injectedDoseBq", s.InjectedDoseBq);
                Num(w, "halfLifeSeconds", s.HalfLifeSeconds);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            var images = SuvRoiCases.RoiImages().ToDictionary(i => i.Name);
            w.WriteStartArray("roi");
            foreach (RoiCase c in SuvRoiCases.Roi())
            {
                using TkFile f = TkFile.Create();
                images[c.Image].Build(f);
                w.WriteStartObject();
                w.WriteString("name", c.Name);
                w.WriteString("image", c.Image);
                w.WriteNumber("frame", c.Frame);
                w.WriteNumber("shape", c.Shape);
                w.WriteStartArray("params");
                foreach (double p in c.Parameters)
                {
                    w.WriteRawValue(p.ToString("R", CultureInfo.InvariantCulture));
                }
                w.WriteEndArray();
                w.WriteBoolean("partial", c.Partial);
                try
                {
                    Siaara.Clinical.RoiStatistics r = Siaara.Clinical.RoiStatistics.Compute(f.File, c.Frame, (Siaara.Clinical.RoiShape)c.Shape, c.Parameters, c.Partial);
                    w.WriteBoolean("error", false);
                    w.WriteNumber("pixels", r.Pixels);
                    Num(w, "count", r.Count);
                    Num(w, "sum", r.Sum);
                    Num(w, "mean", r.Mean);
                    Num(w, "sd", r.StandardDeviation);
                    Num(w, "sdSample", r.SampleStandardDeviation);
                    Num(w, "min", r.Min);
                    Num(w, "max", r.Max);
                    Num(w, "areaPx", r.AreaPixels);
                    Num(w, "areaMm2", r.AreaSquareMillimetres);
                    w.WriteNumber("spacingSource", (int)r.SpacingSource);
                }
                catch (Siaara.SiaaraException e)
                {
                    w.WriteBoolean("error", true);
                    w.WriteString("errorCode", e.ErrorCode.ToString());
                }
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path, Encoding.UTF8.GetString(ms.ToArray()) + "\n", new UTF8Encoding(false));
        Console.WriteLine("wrote " + path);
    }
}
