// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text.Json.Serialization;

namespace Siaara.Viewer.Core.Settings;

/// <summary>A window preset (key 1-9 in the viewer).</summary>
public sealed class WindowPresetSetting
{
    /// <summary>Name shown in the menu.</summary>
    public string Name { get; set; } = "";
    /// <summary>Window centre.</summary>
    public double Center { get; set; }
    /// <summary>Window width.</summary>
    public double Width { get; set; }
}

/// <summary>Four-corner overlay template: each corner lists items such as "PatientName" or "WL".</summary>
public sealed class OverlayTemplateSetting
{
    /// <summary>Top left.</summary>
    public IList<string> TopLeft { get; init; } = [];
    /// <summary>Top right.</summary>
    public IList<string> TopRight { get; init; } = [];
    /// <summary>Bottom left.</summary>
    public IList<string> BottomLeft { get; init; } = [];
    /// <summary>Bottom right.</summary>
    public IList<string> BottomRight { get; init; } = [];
}

/// <summary>User settings (settings.json). No secrets are ever stored here.</summary>
public sealed class UserPrefs
{
    /// <summary>Settings file version.</summary>
    public int Version { get; set; } = 1;

    /// <summary>Store folder; null = the per-user default.</summary>
    public string? StoreRoot { get; set; }

    /// <summary>"dark" (default) or "light".</summary>
    public string Theme { get; set; } = "dark";

    /// <summary>User calibration of the screen: true mm per nominal mm (1 = trust the browser).</summary>
    public double DisplayCalibration { get; set; } = 1.0;

    /// <summary>Default frame rate for stacks without a cine rate.</summary>
    public double StackCineFps { get; set; } = 10;

    /// <summary>Default frame rate for multi-frame images without any rate attribute.</summary>
    public double MultiFrameCineFps { get; set; } = 15;

    /// <summary>Full (pixel data) file handles kept per viewport.</summary>
    public int MaxFullHandlesPerViewport { get; set; } = 6;

    /// <summary>Budget of decoded frames kept in memory (MB).</summary>
    public int DecodedCacheMegabytes { get; set; } = 1024;

    /// <summary>Cine ring cache per window state (MB).</summary>
    public int CineCacheMegabytes { get; set; } = 256;

    /// <summary>Bilinear interpolation when zoomed.</summary>
    public bool LinearInterpolation { get; set; } = true;

    /// <summary>Window presets per modality (keys 1-9 in order).</summary>
    public IDictionary<string, IList<WindowPresetSetting>> Presets { get; init; } = new Dictionary<string, IList<WindowPresetSetting>>();

    /// <summary>Overlay templates per modality ("*" = default).</summary>
    public IDictionary<string, OverlayTemplateSetting> OverlayTemplates { get; init; } = new Dictionary<string, OverlayTemplateSetting>();
    /// <summary>Network nodes and our receiver (no passwords or tokens: those are in the secret store).</summary>
    public Network.NetworkSetting Network { get; init; } = new();
}

/// <summary>Source-generated JSON metadata (trimming-safe, no reflection).</summary>
[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(UserPrefs))]
internal sealed partial class SettingsJsonContext : JsonSerializerContext;
