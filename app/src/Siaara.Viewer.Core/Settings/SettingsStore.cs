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

    private static UserPrefs Clone(UserPrefs p) =>
        JsonSerializer.Deserialize(Serialize(p), SettingsJsonContext.Default.UserPrefs) ?? new UserPrefs();
}
