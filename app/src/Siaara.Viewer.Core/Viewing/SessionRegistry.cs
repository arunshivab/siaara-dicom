// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>Open viewer sessions by id (random, 128 bits, in the socket URL next to the cookie check).</summary>
public sealed class SessionRegistry : IDisposable
{
    private readonly ConcurrentDictionary<string, ViewerSession> _sessions = new(StringComparer.Ordinal);
    private readonly ViewerServices _services;
    private readonly Timer _sweep;

    /// <summary>Creates the registry.</summary>
    /// <param name="services">Services for new sessions.</param>
    public SessionRegistry(ViewerServices services)
    {
        _services = services;
        _sweep = new Timer(_ => Sweep(), null, TimeSpan.FromMinutes(1), TimeSpan.FromMinutes(1));
    }

    /// <summary>Services.</summary>
    public ViewerServices Services => _services;

    /// <summary>Open sessions.</summary>
    public int Count => _sessions.Count;

    /// <summary>Creates a session and starts loading it.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <returns>The session.</returns>
    public ViewerSession Create(string studyInstanceUid)
    {
        string id = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16));
        var s = new ViewerSession(id, studyInstanceUid, _services);
        _sessions[id] = s;
        s.Observe(s.LoadAsync(), "loading the study");
        return s;
    }

    /// <summary>A session by id.</summary>
    /// <param name="id">Id.</param>
    /// <returns>The session or null.</returns>
    public ViewerSession? Get(string? id) => id is not null && _sessions.TryGetValue(id, out ViewerSession? s) ? s : null;

    /// <summary>Closes every session showing one of these studies (before they are deleted): their header handles are released.</summary>
    /// <param name="studyInstanceUids">Studies.</param>
    /// <returns>Sessions closed.</returns>
    public int CloseStudies(IReadOnlyCollection<string> studyInstanceUids)
    {
        ArgumentNullException.ThrowIfNull(studyInstanceUids);
        int n = 0;
        foreach (KeyValuePair<string, ViewerSession> kv in _sessions)
        {
            if (studyInstanceUids.Contains(kv.Value.StudyInstanceUid))
            {
                Close(kv.Key);
                n++;
            }
        }
        return n;
    }

    /// <summary>Closes a session.</summary>
    /// <param name="id">Session id.</param>
    public void Close(string id)
    {
        if (_sessions.TryRemove(id, out ViewerSession? s))
        {
            s.Dispose();
        }
    }

    private void Sweep()
    {
        DateTime limit = DateTime.UtcNow.AddMinutes(-30);
        foreach (KeyValuePair<string, ViewerSession> kv in _sessions)
        {
            if (!kv.Value.Connected && kv.Value.LastActivityUtc < limit)
            {
                Close(kv.Key);
            }
        }
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        _sweep.Dispose();
        foreach (string id in _sessions.Keys)
        {
            Close(id);
        }
    }
}
