// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Siaara.Viewer.Core.Settings;

namespace Siaara.Viewer.Core.Measure;

/// <summary>
/// Annotations as JSON files per user, SOP Instance and frame: &lt;root&gt;/&lt;user&gt;/&lt;key&gt;/&lt;frame&gt;.json,
/// written atomically and owner-only. &lt;key&gt; is the SHA-256 (hex) of the SOP Instance UID, so no
/// UID (for example "..") can name a folder outside the store. Files of earlier versions, which
/// used the UID itself as the folder name, are still read (never for ".", ".." or other dot-only
/// names) and are replaced by the new location at the next save. The files hold geometry and the
/// labels the user typed, never dataset values.
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
        string u = Safe(string.IsNullOrWhiteSpace(user) ? "user" : user);
        User = u.Trim('.').Length == 0 ? "user" : u;
    }

    /// <summary>User folder name.</summary>
    public string User { get; }

    private static string Safe(string s) => new([.. s.Select(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '-' or '_' ? c : '_')]);

    /// <summary>Folder name of a SOP Instance: the SHA-256 (hex) of its UID.</summary>
    /// <param name="sop">SOP Instance UID.</param>
    /// <returns>64 hex digits.</returns>
    public static string FolderKey(string sop) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(sop ?? "")));

    internal string PathOf(string sop, int frame) =>
        Path.Combine(_root, User, FolderKey(sop), frame.ToString(CultureInfo.InvariantCulture) + ".json");

    internal string CalibrationPath(string sop) => Path.Combine(_root, User, FolderKey(sop), "calibration.json");

    // Where earlier versions kept a SOP Instance's files, or null when that name could leave the user's folder.
    internal string? LegacyFolder(string sop)
    {
        string name = Safe(sop ?? "");
        return name.Length == 0 || name.Trim('.').Length == 0 ? null : Path.Combine(_root, User, name);
    }

    private AnnotationDocument? ReadLegacy(string sop, string file) =>
        LegacyFolder(sop) is string folder ? Read(Path.Combine(folder, file)) : null;

    /// <summary>Loads a frame's document (empty when none).</summary>
    /// <param name="sop">SOP Instance UID.</param>
    /// <param name="frame">Frame.</param>
    /// <returns>The document.</returns>
    public AnnotationDocument Load(string sop, int frame)
    {
        AnnotationDocument doc = Read(PathOf(sop, frame)) ?? ReadLegacy(sop, frame.ToString(CultureInfo.InvariantCulture) + ".json")
            ?? new AnnotationDocument { SopInstanceUid = sop, Frame = frame };
        doc.CalibrationMmPerPixel = (Read(CalibrationPath(sop)) ?? ReadLegacy(sop, "calibration.json"))?.CalibrationMmPerPixel;
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
