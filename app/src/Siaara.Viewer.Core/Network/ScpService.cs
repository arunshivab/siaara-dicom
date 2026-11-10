// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Network;

/// <summary>
/// Our Storage SCP ("receiver"): off unless the user switches it on. It is the only listener of the app that other
/// computers can reach (the web pages stay on 127.0.0.1). Calling AE titles must be on the allow-list; "accept every
/// AE title" is a separate setting that is audited before it takes effect. Arriving images are grouped into activity
/// rows per sender; a row closes after five quiet seconds and is audited as scp.receive.
/// </summary>
public sealed class ScpService : IDisposable
{
    private static readonly TimeSpan s_rowIdle = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan s_refusalAuditGap = TimeSpan.FromSeconds(10);

    private readonly NodeRegistry _nodes;
    private readonly StoreService _store;
    private readonly NetworkJobs _jobs;
    private readonly AuditLog? _audit;
    private readonly Lock _gate = new();
    private readonly Dictionary<string, Row> _rows = new(StringComparer.Ordinal);
    private readonly Timer _timer;
    private TkScp? _scp;
    private TkScpStats _stats;
    private long _auditedRejected, _auditedRefused;
    private DateTime _lastRefusalAudit = DateTime.MinValue;
    private IReadOnlyList<string> _allowed = [];
    private bool _acceptAny;
    private bool _disposed;

    private sealed class Row(NetworkJob job, string ae, string address)
    {
        public NetworkJob Job { get; } = job;
        public string Ae { get; } = ae;
        public string Address { get; } = address;
        public int Stored { get; set; }
        public int Failed { get; set; }
        public DateTime LastUtc { get; set; } = DateTime.UtcNow;
    }

    /// <summary>Creates the service (the receiver is not started).</summary>
    /// <param name="nodes">Settings.</param>
    /// <param name="store">The store images are received into.</param>
    /// <param name="jobs">Activity list.</param>
    /// <param name="audit">Audit log, or null.</param>
    public ScpService(NodeRegistry nodes, StoreService store, NetworkJobs jobs, AuditLog? audit)
    {
        _nodes = nodes;
        _store = store;
        _jobs = jobs;
        _audit = audit;
        _timer = new Timer(_ => Tick(), null, 1000, 1000);
    }

    /// <summary>Raised when the receiver started, stopped, or its counters changed (about once a second while running).</summary>
    public event EventHandler? Changed;

    /// <summary>The receiver is listening.</summary>
    public bool IsRunning
    {
        get
        {
            lock (_gate)
            {
                return _scp is not null;
            }
        }
    }

    /// <summary>The port it listens on (0 when stopped).</summary>
    public int Port { get; private set; }

    /// <summary>The AE title it runs under ("" when stopped).</summary>
    public string AeTitle { get; private set; } = "";

    /// <summary>It runs with "accept every AE title".</summary>
    public bool AcceptsAny => IsRunning && _acceptAny;

    /// <summary>Why the last start failed, in plain words; null when it worked.</summary>
    public string? LastError { get; private set; }

    /// <summary>Live counters (zero when stopped).</summary>
    public TkScpStats Stats
    {
        get
        {
            lock (_gate)
            {
                return _stats;
            }
        }
    }

    internal TkScp? Running
    {
        get
        {
            lock (_gate)
            {
                return _scp;
            }
        }
    }

    /// <summary>True when the running receiver accepts connections from this AE title.</summary>
    /// <param name="callingAe">AE title of the sender.</param>
    /// <returns>True when accepted.</returns>
    public bool Accepts(string callingAe)
    {
        lock (_gate)
        {
            return _scp is not null && (_acceptAny || _allowed.Contains(callingAe, StringComparer.Ordinal));
        }
    }

    /// <summary>Saves the settings and makes the receiver match them: (re)started when enabled, stopped otherwise.</summary>
    /// <param name="settings">Settings.</param>
    /// <param name="tlsPassword">PKCS#12 password: null keeps the stored one, "" removes it.</param>
    /// <exception cref="NodeValidationException">The settings are not valid; nothing changed.</exception>
    /// <exception cref="NetworkException">Saved, but the receiver could not start.</exception>
    public void Apply(ScpSetting settings, string? tlsPassword = null)
    {
        ArgumentNullException.ThrowIfNull(settings);
        _nodes.SaveScp(settings, tlsPassword);
        Stop();
        if (settings.Enabled)
        {
            Start();
        }
    }

    /// <summary>Starts the receiver from the saved settings (whatever "Enabled" says). The intent is written to the audit
    /// log first; when that cannot be written the receiver does not start.</summary>
    /// <exception cref="NetworkException">It could not start; the message says why in plain words.</exception>
    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ScpSetting s = _nodes.Current.Scp;
        try
        {
            StartCore(s);
            LastError = null;
        }
        catch (NetworkException e)
        {
            LastError = e.Message;
            throw;
        }
        finally
        {
            Changed?.Invoke(this, EventArgs.Empty);
        }
    }

    private void StartCore(ScpSetting s)
    {
        lock (_gate)
        {
            if (_scp is not null)
            {
                return;
            }
        }
        IReadOnlyList<string> problems = NodeRegistry.Validate(s);
        if (problems.Count > 0)
        {
            throw new NetworkException("The receiver's settings are not valid: " + string.Join(" ", problems));
        }
        string[] allowed = [.. s.AllowedCallingAes.Distinct(StringComparer.Ordinal)];
        if (allowed.Length == 0 && !s.AcceptAnyCallingAe)
        {
            throw new NetworkException("The receiver was not started: list at least one sender (AE title) that may connect, or tick \"accept every AE title\".");
        }
        var fields = new Dictionary<string, string>
        {
            ["ae"] = s.AeTitle,
            ["port"] = s.Port.ToString(CultureInfo.InvariantCulture),
            ["bind"] = s.BindAddress.Length == 0 ? "all" : s.BindAddress,
            ["allowed"] = allowed.Length.ToString(CultureInfo.InvariantCulture),
            ["acceptAny"] = s.AcceptAnyCallingAe ? "yes" : "no",
            ["tls"] = s.Tls.Enabled ? "yes" : "no",
            ["queryRetrieve"] = s.ServeQueryRetrieve ? "yes" : "no",
        };
        try
        {
            // Opening a port to the network is on record before it happens.
            _audit?.AppendRequired("scp.start", AuditOutcome.Intent, fields);
        }
        catch (AuditUnavailableException e)
        {
            throw new NetworkException("The receiver was not started: the audit log could not be written.", e);
        }
        TkScp scp;
        try
        {
            scp = TkScp.Start(new TkScpOptions
            {
                AeTitle = s.AeTitle,
                Port = s.Port,
                BindAddress = s.BindAddress,
                // With "accept every AE title" the list is not checked at all.
                AllowedCallingAes = s.AcceptAnyCallingAe ? [] : allowed,
                AcceptAnyCallingAe = s.AcceptAnyCallingAe,
                Tls = _nodes.TlsFor(s.Tls, NodeRegistry.ScpTlsSecret),
                ServeQueryRetrieve = s.ServeQueryRetrieve,
                AeTable = [.. _nodes.Current.DimseNodes.Select(n => new TkAeEntry(n.CalledAe, n.Host, n.Port))],
            }, _store.Store);
        }
        catch (Exception e) when (e is TkException or System.Security.Cryptography.CryptographicException)
        {
            string why = e is TkException { Error: TkError.Network or TkError.IO or TkError.Rejected }
                ? "the port is in use, or the listen address is not an address of this computer"
                : e is System.Security.Cryptography.CryptographicException ? "the password of the TLS identity could not be read from the secret store"
                : NetworkText.Describe(e);
            _audit?.Append("scp.start", AuditOutcome.Failure, fields, why);
            throw new NetworkException("The receiver could not start: " + why + ".", e);
        }
        scp.Received += OnEvent;
        lock (_gate)
        {
            _scp = scp;
            _stats = default;
            _auditedRejected = _auditedRefused = 0;
            _allowed = allowed;
            _acceptAny = s.AcceptAnyCallingAe;
            Port = scp.Port;
            AeTitle = s.AeTitle;
        }
        fields["port"] = scp.Port.ToString(CultureInfo.InvariantCulture);
        _audit?.Append("scp.start", AuditOutcome.Success, fields);
        AppLog.Info("network", string.Create(CultureInfo.InvariantCulture, $"receiver started on port {scp.Port}"));
        if (s.AcceptAnyCallingAe)
        {
            AppLog.Warn("network", "the receiver accepts every AE title");
        }
    }

    /// <summary>Stops the receiver (running associations are aborted). Does nothing when it is not running.</summary>
    public void Stop()
    {
        TkScp? scp;
        TkScpStats last;
        lock (_gate)
        {
            scp = _scp;
            _scp = null;
            last = _stats;
        }
        if (scp is null)
        {
            return;
        }
        try
        {
            last = scp.GetStatistics();
        }
        catch (TkException)
        {
            // Keep the last polled counters.
        }
        scp.Received -= OnEvent;
        scp.Dispose();
        CloseRows(all: true);
        lock (_gate)
        {
            _stats = default;
            Port = 0;
            AeTitle = "";
        }
        _audit?.Append("scp.stop", AuditOutcome.Success, new Dictionary<string, string>
        {
            ["associations"] = last.Associations.ToString(CultureInfo.InvariantCulture),
            ["rejected"] = last.Rejected.ToString(CultureInfo.InvariantCulture),
            ["stored"] = last.Stored.ToString(CultureInfo.InvariantCulture),
            ["failed"] = last.StoreFailures.ToString(CultureInfo.InvariantCulture),
        });
        AppLog.Info("network", "receiver stopped");
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // On the SCP's dispatcher thread: count and leave.
    private void OnEvent(object? sender, TkScpEventArgs e)
    {
        TkScpEvent ev = e.Event;
        if (ev.Kind is not (TkScpEventKind.Stored or TkScpEventKind.StoreFailed))
        {
            return;
        }
        lock (_gate)
        {
            string key = ev.CallingAe + "\n" + ev.PeerAddress;
            if (!_rows.TryGetValue(key, out Row? row))
            {
                row = new Row(_jobs.AddReceive(ev.CallingAe, "Receiving from " + ev.CallingAe + (ev.PeerAddress.Length > 0 ? " (" + ev.PeerAddress + ")" : "")),
                    ev.CallingAe, ev.PeerAddress);
                _rows[key] = row;
            }
            if (ev.Kind == TkScpEventKind.Stored)
            {
                row.Stored++;
            }
            else
            {
                row.Failed++;
            }
            row.LastUtc = DateTime.UtcNow;
            row.Job.Report(row.Stored, row.Failed, 0);
        }
    }

    private void CloseRows(bool all)
    {
        List<Row> closed = [];
        lock (_gate)
        {
            DateTime now = DateTime.UtcNow;
            foreach ((string key, Row row) in _rows.ToArray())
            {
                if (all || now - row.LastUtc >= s_rowIdle)
                {
                    _rows.Remove(key);
                    closed.Add(row);
                }
            }
        }
        foreach (Row row in closed)
        {
            if (row.Failed > 0)
            {
                row.Job.Note(row.Failed.ToString(CultureInfo.InvariantCulture) + " image(s) were refused or could not be stored (incomplete, damaged or not allowed).");
            }
            row.Job.Finish(row.Failed > 0 ? NetworkJobState.CompletedWithErrors : NetworkJobState.Completed, null);
            _audit?.Append("scp.receive", row.Failed > 0 && row.Stored == 0 ? AuditOutcome.Failure : AuditOutcome.Success, new Dictionary<string, string>
            {
                ["ae"] = row.Ae,
                ["address"] = row.Address,
                ["stored"] = row.Stored.ToString(CultureInfo.InvariantCulture),
                ["failed"] = row.Failed.ToString(CultureInfo.InvariantCulture),
            });
            AppLog.Info("network", string.Create(CultureInfo.InvariantCulture, $"received {row.Stored} image(s), {row.Failed} failed"));
        }
    }

    private void Tick()
    {
        TkScp? scp = Running;
        if (scp is null || _disposed)
        {
            return;
        }
        try
        {
            TkScpStats s = scp.GetStatistics();
            long newRejected, newRefused;
            lock (_gate)
            {
                _stats = s;
                newRejected = s.Rejected - _auditedRejected;
                newRefused = s.Refused - _auditedRefused;
                if ((newRejected > 0 || newRefused > 0) && DateTime.UtcNow - _lastRefusalAudit >= s_refusalAuditGap)
                {
                    _auditedRejected = s.Rejected;
                    _auditedRefused = s.Refused;
                    _lastRefusalAudit = DateTime.UtcNow;
                }
                else
                {
                    newRejected = newRefused = 0;
                }
            }
            if (newRejected > 0 || newRefused > 0)
            {
                // Connections from AE titles that are not allowed, and requests that were refused: counted, at most one entry in ten seconds.
                _audit?.Append("scp.rejected", AuditOutcome.Failure, new Dictionary<string, string>
                {
                    ["associations"] = newRejected.ToString(CultureInfo.InvariantCulture),
                    ["requests"] = newRefused.ToString(CultureInfo.InvariantCulture),
                });
            }
            CloseRows(all: false);
            Changed?.Invoke(this, EventArgs.Empty);
        }
        catch (ObjectDisposedException)
        {
            // Stopped between the check and the call.
        }
#pragma warning disable CA1031 // A timer callback must not take the process down; the next tick tries again.
        catch (Exception e)
#pragma warning restore CA1031
        {
            AppLog.Error("network", "receiver status update failed", e);
        }
    }

    /// <summary>Stops the receiver. Call before the store closes.</summary>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        _timer.Dispose();
        Stop();
    }
}
