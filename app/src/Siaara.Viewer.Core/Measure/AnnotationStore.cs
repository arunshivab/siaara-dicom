// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Text.Json;
using Siaara.Viewer.Core.Settings;

namespace Siaara.Viewer.Core.Measure;

/// <summary>
/// Annotations as JSON files per user, SOP Instance and frame: &lt;root&gt;/&lt;user&gt;/&lt;sop&gt;/&lt;frame&gt;.json,
/// written atomically. The files hold geometry only, never dataset values.
/// </summary>
public sealed class AnnotationStore
{
    private readonly string _root;

    /// <summary>Creates the store.</summary>
    /// <param name="root">Folder.</param>
    /// <param name="user">User name (made file-name safe).</param>
    public AnnotationStore(string root, string user)
    {
        _root = root;
        User = Safe(string.IsNullOrWhiteSpace(user) ? "user" : user);
    }

    /// <summary>User folder name.</summary>
    public string User { get; }

    private static string Safe(string s) => new([.. s.Select(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '-' or '_' ? c : '_')]);

    private string PathOf(string sop, int frame) =>
        Path.Combine(_root, User, Safe(sop), frame.ToString(CultureInfo.InvariantCulture) + ".json");

    private string CalibrationPath(string sop) => Path.Combine(_root, User, Safe(sop), "calibration.json");

    /// <summary>Loads a frame's document (empty when none).</summary>
    /// <param name="sop">SOP Instance UID.</param>
    /// <param name="frame">Frame.</param>
    /// <returns>The document.</returns>
    public AnnotationDocument Load(string sop, int frame)
    {
        AnnotationDocument doc = Read(PathOf(sop, frame)) ?? new AnnotationDocument { SopInstanceUid = sop, Frame = frame };
        doc.CalibrationMmPerPixel = Read(CalibrationPath(sop))?.CalibrationMmPerPixel;
        return doc;
    }

    private static AnnotationDocument? Read(string path)
    {
        if (!File.Exists(path))
        {
            return null;
        }
        try
        {
            return JsonSerializer.Deserialize(File.ReadAllBytes(path), AnnotationJsonContext.Default.AnnotationDocument);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>Saves a frame's annotations.</summary>
    /// <param name="doc">Document.</param>
    public void Save(AnnotationDocument doc)
    {
        ArgumentNullException.ThrowIfNull(doc);
        var copy = new AnnotationDocument { SopInstanceUid = doc.SopInstanceUid, Frame = doc.Frame, Annotations = doc.Annotations };
        AtomicFile.WriteAllBytes(PathOf(doc.SopInstanceUid, doc.Frame), JsonSerializer.SerializeToUtf8Bytes(copy, AnnotationJsonContext.Default.AnnotationDocument));
    }

    /// <summary>Saves or clears an instance's manual calibration.</summary>
    /// <param name="sop">SOP Instance UID.</param>
    /// <param name="mmPerPixel">Scale or null.</param>
    public void SaveCalibration(string sop, double? mmPerPixel)
    {
        var doc = new AnnotationDocument { SopInstanceUid = sop, Frame = -1, CalibrationMmPerPixel = mmPerPixel };
        AtomicFile.WriteAllBytes(CalibrationPath(sop), JsonSerializer.SerializeToUtf8Bytes(doc, AnnotationJsonContext.Default.AnnotationDocument));
    }
}
