// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Text;

/// <summary>Text of the four viewport corners (sent in the state message, drawn by the browser).</summary>
/// <param name="TopLeft">Top left lines.</param>
/// <param name="TopRight">Top right lines.</param>
/// <param name="BottomLeft">Bottom left lines.</param>
/// <param name="BottomRight">Bottom right lines.</param>
public sealed record OverlayText(IReadOnlyList<string> TopLeft, IReadOnlyList<string> TopRight, IReadOnlyList<string> BottomLeft,
                                 IReadOnlyList<string> BottomRight)
{
    /// <summary>No text (privacy mode).</summary>
    public static OverlayText Empty { get; } = new([], [], [], []);
}

/// <summary>Values that change per render.</summary>
/// <param name="Index">0-based image index in the series.</param>
/// <param name="Count">Images in the series.</param>
/// <param name="Frame">Frame of the instance.</param>
/// <param name="Center">Window centre shown, or null.</param>
/// <param name="Width">Window width shown, or null.</param>
/// <param name="WindowName">Preset or file window name, or "".</param>
/// <param name="Zoom">Effective zoom.</param>
/// <param name="Invert">Inverted.</param>
/// <param name="Fps">Cine rate while playing, or null.</param>
public sealed record OverlayDynamic(int Index, int Count, int Frame, double? Center, double? Width, string WindowName, double Zoom, bool Invert,
                                    double? Fps);

/// <summary>Per-instance header values used by the templates (read once per instance).</summary>
internal sealed class OverlayStatic
{
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);
}

/// <summary>
/// Builds the corner text from a per-modality template ("*" = default; editable in settings.json).
/// Items: PatientName, PatientId, SexAge, BirthDate, Accession, Institution, StudyDescription,
/// StudyDateTime, SeriesDescription, Series, Image, Slice, Technique, Protocol, BodyPart, Contrast,
/// Manufacturer, Station, Referring, Modality, WL, Zoom, Lossy, Cine. Names are formatted for
/// display only; stored values are never changed.
/// </summary>
public static class OverlayTextBuilder
{
    /// <summary>The built-in default template.</summary>
    public static OverlayTemplateSetting DefaultTemplate { get; } = new()
    {
        TopLeft = ["PatientName", "PatientId", "SexAge", "BirthDate", "Accession"],
        TopRight = ["Institution", "StudyDescription", "StudyDateTime", "SeriesDescription", "Series"],
        BottomLeft = ["Image", "Slice", "Technique", "Protocol"],
        BottomRight = ["Lossy", "Cine", "WL", "Zoom"],
    };

    /// <summary>The template for a modality.</summary>
    /// <param name="prefs">Settings.</param>
    /// <param name="modality">Modality.</param>
    /// <returns>The template.</returns>
    public static OverlayTemplateSetting TemplateFor(UserPrefs prefs, string modality)
    {
        ArgumentNullException.ThrowIfNull(prefs);
        return prefs.OverlayTemplates.TryGetValue(modality, out OverlayTemplateSetting? t) ? t
            : prefs.OverlayTemplates.TryGetValue("*", out t) ? t : DefaultTemplate;
    }

    /// <summary>Builds the text.</summary>
    /// <param name="instance">Instance.</param>
    /// <param name="dyn">Per-render values.</param>
    /// <param name="template">Template.</param>
    /// <returns>Corner text.</returns>
    public static OverlayText Build(ImageInstance instance, OverlayDynamic dyn, OverlayTemplateSetting template)
    {
        ArgumentNullException.ThrowIfNull(instance);
        ArgumentNullException.ThrowIfNull(dyn);
        ArgumentNullException.ThrowIfNull(template);
        OverlayStatic s = instance.OverlayStatic ??= ReadStatic(instance);
        IReadOnlyList<string> Lines(IList<string> items) =>
            [.. items.Select(i => Item(i, s, instance, dyn)).Where(l => l.Length > 0)];
        return new OverlayText(Lines(template.TopLeft), Lines(template.TopRight), Lines(template.BottomLeft), Lines(template.BottomRight));
    }

    private static string Item(string item, OverlayStatic s, ImageInstance inst, OverlayDynamic d) => item switch
    {
        "Image" => "Im: " + (d.Index + 1).ToString(CultureInfo.InvariantCulture) + "/" + d.Count.ToString(CultureInfo.InvariantCulture)
                   + (inst.FrameCount > 1 ? "  Fr: " + (d.Frame + 1).ToString(CultureInfo.InvariantCulture) + "/" + inst.FrameCount.ToString(CultureInfo.InvariantCulture) : ""),
        "Slice" => SliceText(inst, d.Frame, s),
        "WL" => d.Center is double c && d.Width is double w
            ? "W " + DisplayFormat.Number(w, 0) + " L " + DisplayFormat.Number(c, 0) + (d.WindowName.Length > 0 ? " (" + d.WindowName + ")" : "") + (d.Invert ? " INV" : "")
            : (d.Invert ? "INV" : ""),
        "Zoom" => "Zoom " + DisplayFormat.Number(d.Zoom, 2) + "x",
        "Lossy" => inst.IsLossy ? "LOSSY" : "",
        "Cine" => d.Fps is double fps ? DisplayFormat.Number(fps, 1) + " fps" : "",
        _ => s.Values.TryGetValue(item, out string? v) ? v : "",
    };

    private static string SliceText(ImageInstance inst, int frame, OverlayStatic s)
    {
        string thk = s.Values.TryGetValue("SliceThickness", out string? t) ? "Thk " + t + " mm" : "";
        string loc = "";
        if (inst.FrameCount == 1 && s.Values.TryGetValue("SliceLocation", out string? l))
        {
            loc = "Loc " + l;
        }
        else if (inst.FrameCount > 1)
        {
            try
            {
                TkPlane g = inst.Header.GetGeometry(frame);
                if (g.HasPosition)
                {
                    loc = "Pos " + DisplayFormat.Number(g.PositionZ, 1);
                }
            }
            catch (TkException)
            {
                // Without per-frame geometry the slice line shows the thickness only.
            }
        }
        return string.Join("  ", new[] { thk, loc }.Where(x => x.Length > 0));
    }

    private static OverlayStatic ReadStatic(ImageInstance inst)
    {
        TkFile h = inst.Header;
        var s = new OverlayStatic();
        void Put(string key, string? value)
        {
            if (!string.IsNullOrWhiteSpace(value))
            {
                s.Values[key] = value.Trim();
            }
        }
        string? Num(TkTag tag, int digits)
        {
            return h.GetDouble(tag) is double v ? DisplayFormat.Number(v, digits) : null;
        }
        Put("PatientName", DisplayFormat.PersonName(h.GetString(Tags.PatientName)));
        Put("PatientId", h.GetString(Tags.PatientId));
        string age = DisplayFormat.Age(h.GetString(Tags.PatientAge), h.GetString(Tags.PatientBirthDate), h.GetString(Tags.StudyDate));
        Put("SexAge", DisplayFormat.SexAge(h.GetString(Tags.PatientSex), age));
        Put("BirthDate", DisplayFormat.Date(h.GetString(Tags.PatientBirthDate)));
        Put("Accession", h.GetString(Tags.AccessionNumber) is string acc ? "Acc: " + acc : null);
        Put("Institution", h.GetString(Tags.InstitutionName));
        Put("StudyDescription", h.GetString(Tags.StudyDescription));
        Put("StudyDateTime", (DisplayFormat.Date(h.GetString(Tags.StudyDate)) + " " + DisplayFormat.Time(h.GetString(Tags.StudyTime))).Trim());
        Put("SeriesDescription", h.GetString(Tags.SeriesDescription));
        Put("Series", h.GetString(Tags.SeriesNumber) is string sn ? "Se: " + sn : null);
        Put("Protocol", h.GetString(Tags.ProtocolName));
        Put("BodyPart", h.GetString(Tags.BodyPartExamined));
        Put("Contrast", h.GetString(Tags.ContrastBolusAgent) is string cb ? "C+ " + cb : null);
        Put("Manufacturer", (h.GetString(Tags.Manufacturer) + " " + h.GetString(Tags.ManufacturerModelName)).Trim());
        Put("Station", h.GetString(Tags.StationName));
        Put("Referring", DisplayFormat.PersonName(h.GetString(Tags.ReferringPhysicianName)));
        Put("Modality", inst.Modality);
        Put("SliceThickness", Num(Tags.SliceThickness, 2));
        Put("SliceLocation", Num(Tags.SliceLocation, 1));
        string tech = inst.Modality switch
        {
            "CT" => string.Join(" ", new[] { Num(Tags.Kvp, 0) is string kv ? kv + " kV" : null, Num(Tags.XRayTubeCurrent, 0) is string ma ? ma + " mA" : null }.Where(x => x is not null)),
            "MR" => string.Join(" ", new[]
            {
                Num(Tags.RepetitionTime, 0) is string tr ? "TR " + tr : null, Num(Tags.EchoTime, 1) is string te ? "TE " + te : null,
                Num(Tags.MagneticFieldStrength, 1) is string b0 ? b0 + "T" : null,
            }.Where(x => x is not null)),
            "CR" or "DX" or "MG" => string.Join(" ", new[] { h.GetString(Tags.ViewPosition), h.GetString(Tags.ImageLaterality) ?? h.GetString(Tags.Laterality) }.Where(x => !string.IsNullOrEmpty(x))),
            _ => "",
        };
        Put("Technique", tech);
        return s;
    }
}
