// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>
/// Names for Study and Series Instance UIDs that are safe in a URL (R-145): a random 128-bit id per
/// UID, valid for this run of the app only. URLs land in the browser's history; a UID there would
/// identify a patient's study, an id of this map means nothing after the app has stopped.
/// </summary>
public sealed class OpaqueIds
{
    private const int MaxEntries = 100_000;
    private readonly ConcurrentDictionary<string, string> _byUid = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, string> _byId = new(StringComparer.Ordinal);

    /// <summary>The id of a UID (the same one for the whole run).</summary>
    /// <param name="uid">Study or Series Instance UID.</param>
    /// <returns>32 hex digits.</returns>
    public string For(string uid)
    {
        ArgumentNullException.ThrowIfNull(uid);
        if (_byUid.TryGetValue(uid, out string? known))
        {
            return known;
        }
        if (_byUid.Count >= MaxEntries)
        {
            // Far more than one user opens in a run: start again instead of growing without limit.
            _byUid.Clear();
            _byId.Clear();
        }
        string id = _byUid.GetOrAdd(uid, _ => Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16)));
        _byId[id] = uid;
        return id;
    }

    /// <summary>The UID behind an id, or null (unknown, malformed, or from an earlier run).</summary>
    /// <param name="id">Id from a URL.</param>
    /// <returns>The UID or null.</returns>
    public string? Resolve(string? id) => id is { Length: 32 } && _byId.TryGetValue(id, out string? uid) ? uid : null;
}
