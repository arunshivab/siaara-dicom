// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Text.Json;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Core.Settings;

/// <summary>Loads and saves <see cref="UserPrefs"/> as JSON with atomic writes.</summary>
public sealed class SettingsStore
{
    private readonly Lock _gate = new();
    private UserPrefs _current;

    /// <summary>Loads the file (defaults when missing; an unreadable file is kept aside as .corrupt-*).</summary>
    /// <param name="path">settings.json.</param>
    public SettingsStore(string path)
    {
        FilePath = path;
        _current = Load(path);
    }

    /// <summary>The settings file.</summary>
    public string FilePath { get; }

    /// <summary>A snapshot of the settings.</summary>
    public UserPrefs Current
    {
        get
        {
            lock (_gate)
            {
                return _current;
            }
        }
    }

    /// <summary>Raised after a save.</summary>
    public event EventHandler? Changed;

    /// <summary>Changes and saves the settings.</summary>
    /// <param name="change">Mutation of a copy.</param>
    public void Update(Action<UserPrefs> change)
    {
        ArgumentNullException.ThrowIfNull(change);
        lock (_gate)
        {
            UserPrefs copy = Clone(_current);
            change(copy);
            Save(FilePath, copy);
            _current = copy;
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Serializes settings.</summary>
    /// <param name="prefs">Settings.</param>
    /// <returns>UTF-8 JSON.</returns>
    public static byte[] Serialize(UserPrefs prefs) => JsonSerializer.SerializeToUtf8Bytes(prefs, SettingsJsonContext.Default.UserPrefs);

    /// <summary>Writes settings atomically.</summary>
    /// <param name="path">File.</param>
    /// <param name="prefs">Settings.</param>
    public static void Save(string path, UserPrefs prefs) => AtomicFile.WriteAllBytes(path, Serialize(prefs));

    /// <summary>Reads settings, filling defaults.</summary>
    /// <param name="path">File.</param>
    /// <returns>Settings.</returns>
    public static UserPrefs Load(string path)
    {
        UserPrefs p;
        if (!File.Exists(path))
        {
            p = new UserPrefs();
        }
        else
        {
            try
            {
                p = JsonSerializer.Deserialize(File.ReadAllBytes(path), SettingsJsonContext.Default.UserPrefs) ?? new UserPrefs();
            }
            catch (JsonException)
            {
                string aside = path + ".corrupt-" + DateTime.UtcNow.ToString("yyyyMMddHHmmss", CultureInfo.InvariantCulture);
                File.Move(path, aside);
                AppLog.Warn("settings", "settings file unreadable; kept aside and defaults used");
                p = new UserPrefs();
            }
        }
        WindowPresets.FillDefaults(p);
        return p;
    }

    /// <summary>Names (as in settings.json) of the top-level settings whose value differs between two snapshots.</summary>
    /// <param name="before">Earlier settings.</param>
    /// <param name="after">Later settings.</param>
    /// <returns>Changed names, sorted.</returns>
    public static IReadOnlyList<string> ChangedNames(UserPrefs before, UserPrefs after)
    {
        using var a = JsonDocument.Parse(Serialize(before));
        using var b = JsonDocument.Parse(Serialize(after));
        var names = new SortedSet<string>(StringComparer.Ordinal);
        foreach (JsonProperty p in a.RootElement.EnumerateObject())
        {
            if (!b.RootElement.TryGetProperty(p.Name, out JsonElement other) || other.GetRawText() != p.Value.GetRawText())
            {
                names.Add(p.Name);
            }
        }
        foreach (JsonProperty p in b.RootElement.EnumerateObject())
        {
            if (!a.RootElement.TryGetProperty(p.Name, out _))
            {
                names.Add(p.Name);
            }
        }
        return [.. names];
    }

    private static UserPrefs Clone(UserPrefs p) =>
        JsonSerializer.Deserialize(Serialize(p), SettingsJsonContext.Default.UserPrefs) ?? new UserPrefs();
}
