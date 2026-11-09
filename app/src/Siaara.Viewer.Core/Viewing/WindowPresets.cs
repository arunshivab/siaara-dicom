// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Siaara.Viewer.Core.Settings;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>Window presets per modality (keys 1-9), defaults for CT, editable in settings.json.</summary>
public static class WindowPresets
{
    /// <summary>The CT defaults: brain, subdural, stroke, lung, mediastinum, abdomen, liver, bone.</summary>
    public static IReadOnlyList<WindowPresetSetting> CtDefaults { get; } =
    [
        new() { Name = "Brain", Center = 40, Width = 80 },
        new() { Name = "Subdural", Center = 75, Width = 215 },
        new() { Name = "Stroke", Center = 40, Width = 40 },
        new() { Name = "Lung", Center = -600, Width = 1500 },
        new() { Name = "Mediastinum", Center = 50, Width = 350 },
        new() { Name = "Abdomen", Center = 60, Width = 400 },
        new() { Name = "Liver", Center = 60, Width = 160 },
        new() { Name = "Bone", Center = 300, Width = 1500 },
    ];

    /// <summary>Adds the CT defaults when the settings have none.</summary>
    /// <param name="prefs">Settings.</param>
    public static void FillDefaults(UserPrefs prefs)
    {
        ArgumentNullException.ThrowIfNull(prefs);
        if (!prefs.Presets.ContainsKey("CT"))
        {
            prefs.Presets["CT"] = [.. CtDefaults.Select(p => new WindowPresetSetting { Name = p.Name, Center = p.Center, Width = p.Width })];
        }
    }

    /// <summary>The presets of a modality.</summary>
    /// <param name="prefs">Settings.</param>
    /// <param name="modality">Modality.</param>
    /// <returns>Presets (may be empty).</returns>
    public static IReadOnlyList<WindowPresetSetting> For(UserPrefs prefs, string modality)
    {
        ArgumentNullException.ThrowIfNull(prefs);
        return prefs.Presets.TryGetValue(modality, out IList<WindowPresetSetting>? list) ? [.. list] : [];
    }
}
