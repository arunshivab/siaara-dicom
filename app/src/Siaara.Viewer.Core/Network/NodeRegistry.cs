// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Text;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Platform;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Network;

/// <summary>A node's settings are not valid; <see cref="Problems"/> lists what to correct, in plain words.</summary>
public sealed class NodeValidationException : Exception
{
    /// <summary>Creates the exception.</summary>
    public NodeValidationException() : this([])
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Message.</param>
    public NodeValidationException(string message) : base(message) => Problems = [message];

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Message.</param>
    /// <param name="innerException">Cause.</param>
    public NodeValidationException(string message, Exception innerException) : base(message, innerException) => Problems = [message];

    /// <summary>Creates the exception.</summary>
    /// <param name="problems">What to correct.</param>
    public NodeValidationException(IReadOnlyList<string> problems) : base(string.Join(" ", problems)) => Problems = problems;

    /// <summary>What to correct.</summary>
    public IReadOnlyList<string> Problems { get; }
}

/// <summary>Result of the Test button.</summary>
/// <param name="Ok">The node answered.</param>
/// <param name="Message">Plain words.</param>
public sealed record NodeTestResult(bool Ok, string Message);

/// <summary>
/// The network nodes and our receiver's settings. They live in settings.json (written atomically by
/// <see cref="SettingsStore"/>); passwords and tokens live only in the <see cref="ISecretStore"/> under
/// "&lt;node id&gt;.password" (Basic password or Bearer token) and "&lt;node id&gt;.tls" (PKCS#12 password; "scp.tls" for
/// our receiver). Every change is validated first and audited as node.change / scp.settings without secrets.
/// </summary>
public sealed class NodeRegistry
{
    /// <summary>Secret id of our receiver's PKCS#12 password.</summary>
    public const string ScpTlsSecret = "scp.tls";

    private readonly SettingsStore _settings;
    private readonly ISecretStore _secrets;
    private readonly AuditLog? _audit;

    /// <summary>Creates the registry.</summary>
    /// <param name="settings">Settings.</param>
    /// <param name="secrets">Secret store.</param>
    /// <param name="audit">Audit log, or null.</param>
    public NodeRegistry(SettingsStore settings, ISecretStore secrets, AuditLog? audit)
    {
        ArgumentNullException.ThrowIfNull(settings);
        ArgumentNullException.ThrowIfNull(secrets);
        _settings = settings;
        _secrets = secrets;
        _audit = audit;
    }

    /// <summary>Raised after nodes or receiver settings changed.</summary>
    public event EventHandler? Changed;

    /// <summary>A snapshot of the network settings.</summary>
    public NetworkSetting Current => _settings.Current.Network;

    /// <summary>How secrets are protected on this computer.</summary>
    public string SecretProtection => _secrets.Protection;

    /// <summary>Every node, DIMSE first, by name.</summary>
    /// <returns>The nodes.</returns>
    public IReadOnlyList<NodeRef> List()
    {
        NetworkSetting n = Current;
        return [.. n.DimseNodes.OrderBy(x => x.Name, StringComparer.OrdinalIgnoreCase).Select(x => new NodeRef(x.Id, x.Name, false)),
                .. n.WebNodes.OrderBy(x => x.Name, StringComparer.OrdinalIgnoreCase).Select(x => new NodeRef(x.Id, x.Name, true))];
    }

    /// <summary>A DIMSE node by id.</summary>
    /// <param name="id">Id.</param>
    /// <returns>The node or null.</returns>
    public DimseNodeSetting? FindDimse(string? id) => Current.DimseNodes.FirstOrDefault(x => x.Id == id);

    /// <summary>A DICOMweb node by id.</summary>
    /// <param name="id">Id.</param>
    /// <returns>The node or null.</returns>
    public WebNodeSetting? FindWeb(string? id) => Current.WebNodes.FirstOrDefault(x => x.Id == id);

    /// <summary>True when a secret is stored (the value itself is never shown again).</summary>
    /// <param name="secretId">Secret id.</param>
    /// <returns>True when present.</returns>
    public bool HasSecret(string secretId)
    {
        try
        {
            return _secrets.Read(secretId) is not null;
        }
        catch (System.Security.Cryptography.CryptographicException)
        {
            return false;
        }
    }

    /// <summary>Secret id of a node's password or token.</summary>
    /// <param name="nodeId">Node id.</param>
    /// <returns>The id.</returns>
    public static string PasswordSecret(string nodeId) => nodeId + ".password";

    /// <summary>Secret id of a node's PKCS#12 password.</summary>
    /// <param name="nodeId">Node id.</param>
    /// <returns>The id.</returns>
    public static string TlsSecret(string nodeId) => nodeId + ".tls";

    // ------------------------------------------------------------------ validation

    /// <summary>Checks an AE title: 1 to 16 characters of the DICOM default character set, no backslash, no control
    /// characters, not only spaces, no space at either end.</summary>
    /// <param name="ae">AE title.</param>
    /// <returns>True when valid.</returns>
    public static bool IsValidAe(string? ae) =>
        ae is { Length: >= 1 and <= 16 } && ae.All(c => c is >= ' ' and <= '~' and not '\\') && ae[0] != ' ' && ae[^1] != ' ';

    private static void CheckTls(TlsSetting tls, List<string> problems, bool identityRequired)
    {
        if (!tls.Enabled)
        {
            return;
        }
        if (identityRequired && tls.Pkcs12File.Trim().Length == 0)
        {
            problems.Add("TLS for the receiver needs an identity file (PKCS#12).");
        }
        if (tls.Pkcs12File.Trim().Length > 0 && !File.Exists(tls.Pkcs12File.Trim()))
        {
            problems.Add("The TLS identity file does not exist.");
        }
        if (tls.CaFile.Trim().Length > 0 && !File.Exists(tls.CaFile.Trim()))
        {
            problems.Add("The file of trusted certificate authorities does not exist.");
        }
    }

    private static void CheckName(string name, IEnumerable<string> others, List<string> problems)
    {
        if (name.Trim().Length is 0 or > 64)
        {
            problems.Add("Give the node a name of 1 to 64 characters.");
        }
        else if (name.Any(char.IsControl))
        {
            problems.Add("The name must not hold control characters.");
        }
        else if (others.Any(o => string.Equals(o.Trim(), name.Trim(), StringComparison.OrdinalIgnoreCase)))
        {
            problems.Add("Another node already has this name.");
        }
    }

    /// <summary>What is wrong with a DIMSE node, in plain words (empty when valid).</summary>
    /// <param name="node">The node.</param>
    /// <param name="otherNames">Names of the other nodes.</param>
    /// <returns>Problems.</returns>
    public static IReadOnlyList<string> Validate(DimseNodeSetting node, IEnumerable<string> otherNames)
    {
        ArgumentNullException.ThrowIfNull(node);
        var p = new List<string>();
        CheckName(node.Name, otherNames, p);
        string host = node.Host.Trim();
        if (host.Length is 0 or > 255 || host.Any(c => char.IsWhiteSpace(c) || char.IsControl(c) || c is '/' or '\\' or '@'))
        {
            p.Add("Host must be a host name or an IP address.");
        }
        if (node.Port is < 1 or > 65535)
        {
            p.Add("Port must be between 1 and 65535.");
        }
        if (!IsValidAe(node.CalledAe))
        {
            p.Add("The node's AE title must be 1 to 16 characters (no backslash, no space at the ends).");
        }
        if (!IsValidAe(node.CallingAe))
        {
            p.Add("Our AE title must be 1 to 16 characters (no backslash, no space at the ends).");
        }
        if (node.MoveDestinationAe.Length > 0 && !IsValidAe(node.MoveDestinationAe))
        {
            p.Add("The move destination AE title must be 1 to 16 characters.");
        }
        if (node.ConnectTimeoutSeconds is < 1 or > 600 || node.AssociationTimeoutSeconds is < 1 or > 600 || node.DimseTimeoutSeconds is < 1 or > 3600
            || node.CommitmentWaitSeconds is < 1 or > 600)
        {
            p.Add("Timeouts must be between 1 and 600 seconds (answers: up to 3600).");
        }
        if (node.MaxPdu != 0 && node.MaxPdu is < 4096 or > 1048576)
        {
            p.Add("Maximum PDU must be 0 (default) or between 4096 and 1048576.");
        }
        CheckTls(node.Tls, p, identityRequired: false);
        return p;
    }

    /// <summary>True for an address of this computer (credentials may travel without TLS only there).</summary>
    /// <param name="host">Host of a URL.</param>
    /// <returns>True for localhost names and loopback addresses.</returns>
    public static bool IsLoopbackHost(string host) =>
        host is null ? throw new ArgumentNullException(nameof(host)) : string.Equals(host, "localhost", StringComparison.OrdinalIgnoreCase) || host.EndsWith(".localhost", StringComparison.OrdinalIgnoreCase)
        || (System.Net.IPAddress.TryParse(host.Trim('[', ']'), out System.Net.IPAddress? ip) && System.Net.IPAddress.IsLoopback(ip));

    /// <summary>What is wrong with a DICOMweb node, in plain words (empty when valid).</summary>
    /// <param name="node">The node.</param>
    /// <param name="otherNames">Names of the other nodes.</param>
    /// <returns>Problems.</returns>
    public static IReadOnlyList<string> Validate(WebNodeSetting node, IEnumerable<string> otherNames)
    {
        ArgumentNullException.ThrowIfNull(node);
        var p = new List<string>();
        CheckName(node.Name, otherNames, p);
        if (!Uri.TryCreate(node.Address.Trim(), UriKind.Absolute, out Uri? u) || (u.Scheme != Uri.UriSchemeHttp && u.Scheme != Uri.UriSchemeHttps) || u.Host.Length == 0)
        {
            p.Add("The address must start with http:// or https://.");
        }
        else
        {
            if (u.UserInfo.Length > 0)
            {
                p.Add("Do not put a user name or password in the address; use the fields below.");
            }
            if (u.Query.Length > 0 || u.Fragment.Length > 0)
            {
                p.Add("The address must not hold '?' or '#'.");
            }
            if (u.Scheme == Uri.UriSchemeHttp && node.Auth != WebAuthKind.None && !IsLoopbackHost(u.Host))
            {
                p.Add("A password or token is only sent over https (an http address would show it to everyone on the network).");
            }
        }
        if (node.Auth == WebAuthKind.Basic && (node.UserName.Length == 0 || node.UserName.Contains(':', StringComparison.Ordinal) || node.UserName.Any(char.IsControl)))
        {
            p.Add("Basic sign-in needs a user name (without ':').");
        }
        if (node.TimeoutSeconds is < 1 or > 3600)
        {
            p.Add("The timeout must be between 1 and 3600 seconds.");
        }
        CheckTls(new TlsSetting { Enabled = true, CaFile = node.Tls.CaFile, Pkcs12File = node.Tls.Pkcs12File }, p, identityRequired: false);
        return p;
    }

    /// <summary>What is wrong with the receiver's settings, in plain words (empty when valid).</summary>
    /// <param name="scp">The settings.</param>
    /// <returns>Problems.</returns>
    public static IReadOnlyList<string> Validate(ScpSetting scp)
    {
        ArgumentNullException.ThrowIfNull(scp);
        var p = new List<string>();
        if (!IsValidAe(scp.AeTitle))
        {
            p.Add("The receiver's AE title must be 1 to 16 characters (no backslash, no space at the ends).");
        }
        if (scp.Port is < 1 or > 65535)
        {
            p.Add("Port must be between 1 and 65535.");
        }
        if (scp.BindAddress.Trim().Length > 0 && !System.Net.IPAddress.TryParse(scp.BindAddress.Trim(), out _))
        {
            p.Add("The listen address must be an IP address of this computer, or empty for every interface.");
        }
        if (scp.AllowedCallingAes.Any(a => !IsValidAe(a)))
        {
            p.Add("Every allowed sender must be an AE title of 1 to 16 characters.");
        }
        if (scp.AllowedCallingAes.Count > 256)
        {
            p.Add("At most 256 allowed senders.");
        }
        CheckTls(scp.Tls, p, identityRequired: true);
        return p;
    }

    // ------------------------------------------------------------------ changes

    private static string NewId() => "n" + Guid.NewGuid().ToString("N")[..16];

    private void ApplySecret(string id, string? value, Dictionary<string, string> fields, string fieldName)
    {
        // null: keep what is stored; "": remove; anything else: store.
        if (value is null)
        {
            return;
        }
        if (value.Length == 0)
        {
            _secrets.Delete(id);
            fields[fieldName] = "removed";
        }
        else
        {
            _secrets.Write(id, value);
            fields[fieldName] = "set";
        }
    }

    private void Audited(string eventName, Dictionary<string, string> fields, Action change)
    {
        try
        {
            change();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            _audit?.Append(eventName, AuditOutcome.Failure, fields, NetworkText.Describe(e));
            throw new NetworkException("The change could not be saved: " + (e is System.Security.Cryptography.CryptographicException
                ? "the secret store of this computer could not be used" : NetworkText.Describe(e)) + ".", e);
        }
        _audit?.Append(eventName, AuditOutcome.Success, fields);
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Adds a DIMSE node, or replaces the one with the same id.</summary>
    /// <param name="node">The node (an empty id means new).</param>
    /// <param name="tlsPassword">PKCS#12 password: null keeps the stored one, "" removes it.</param>
    /// <returns>The node's id.</returns>
    /// <exception cref="NodeValidationException">The settings are not valid; nothing was saved.</exception>
    public string Save(DimseNodeSetting node, string? tlsPassword = null)
    {
        ArgumentNullException.ThrowIfNull(node);
        NetworkSetting cur = Current;
        IReadOnlyList<string> problems = Validate(node, cur.DimseNodes.Where(x => x.Id != node.Id).Select(x => x.Name).Concat(cur.WebNodes.Select(x => x.Name)));
        if (problems.Count > 0)
        {
            throw new NodeValidationException(problems);
        }
        bool isNew = node.Id.Length == 0 || FindDimse(node.Id) is null;
        if (node.Id.Length == 0)
        {
            node.Id = NewId();
        }
        node.Name = node.Name.Trim();
        node.Host = node.Host.Trim();
        var fields = new Dictionary<string, string>
        {
            ["action"] = isNew ? "add" : "update",
            ["kind"] = "dimse",
            ["node"] = node.Id,
            ["name"] = node.Name,
            ["host"] = node.Host,
            ["port"] = node.Port.ToString(CultureInfo.InvariantCulture),
            ["calledAe"] = node.CalledAe,
            ["callingAe"] = node.CallingAe,
            ["tls"] = node.Tls.Enabled ? "yes" : "no",
            ["retrieve"] = node.Retrieve.ToString(),
            ["allowLossy"] = node.AllowLossyTranscode ? "yes" : "no",
            ["commitment"] = node.RequestCommitment ? "yes" : "no",
        };
        Audited("node.change", fields, () =>
        {
            ApplySecret(TlsSecret(node.Id), tlsPassword, fields, "tlsPassword");
            _settings.Update(p =>
            {
                DimseNodeSetting? old = p.Network.DimseNodes.FirstOrDefault(x => x.Id == node.Id);
                if (old is not null)
                {
                    p.Network.DimseNodes.Remove(old);
                }
                p.Network.DimseNodes.Add(node);
            });
        });
        return node.Id;
    }

    /// <summary>Adds a DICOMweb node, or replaces the one with the same id.</summary>
    /// <param name="node">The node (an empty id means new).</param>
    /// <param name="secret">Basic password or Bearer token: null keeps the stored one, "" removes it.</param>
    /// <param name="tlsPassword">PKCS#12 password: null keeps the stored one, "" removes it.</param>
    /// <returns>The node's id.</returns>
    /// <exception cref="NodeValidationException">The settings are not valid; nothing was saved.</exception>
    public string Save(WebNodeSetting node, string? secret = null, string? tlsPassword = null)
    {
        ArgumentNullException.ThrowIfNull(node);
        NetworkSetting cur = Current;
        var problems = Validate(node, cur.WebNodes.Where(x => x.Id != node.Id).Select(x => x.Name).Concat(cur.DimseNodes.Select(x => x.Name))).ToList();
        bool isNew = node.Id.Length == 0 || FindWeb(node.Id) is null;
        if (node.Auth != WebAuthKind.None && string.IsNullOrEmpty(secret) && (isNew || secret is not null || !HasSecret(PasswordSecret(node.Id))))
        {
            problems.Add(node.Auth == WebAuthKind.Basic ? "Enter the password." : "Enter the token.");
        }
        if (secret is not null && secret.Any(char.IsControl))
        {
            problems.Add("The password or token must not hold line breaks or control characters.");
        }
        if (problems.Count > 0)
        {
            throw new NodeValidationException(problems);
        }
        if (node.Id.Length == 0)
        {
            node.Id = NewId();
        }
        node.Name = node.Name.Trim();
        node.Address = node.Address.Trim().TrimEnd('/');
        var uri = new Uri(node.Address);
        var fields = new Dictionary<string, string>
        {
            ["action"] = isNew ? "add" : "update",
            ["kind"] = "dicomweb",
            ["node"] = node.Id,
            ["name"] = node.Name,
            ["scheme"] = uri.Scheme,
            ["host"] = uri.Host,
            ["port"] = uri.Port.ToString(CultureInfo.InvariantCulture),
            ["auth"] = node.Auth.ToString(),
        };
        Audited("node.change", fields, () =>
        {
            ApplySecret(PasswordSecret(node.Id), node.Auth == WebAuthKind.None ? "" : secret, fields, "secret");
            ApplySecret(TlsSecret(node.Id), tlsPassword, fields, "tlsPassword");
            _settings.Update(p =>
            {
                WebNodeSetting? old = p.Network.WebNodes.FirstOrDefault(x => x.Id == node.Id);
                if (old is not null)
                {
                    p.Network.WebNodes.Remove(old);
                }
                p.Network.WebNodes.Add(node);
            });
        });
        return node.Id;
    }

    /// <summary>Removes a node and its secrets.</summary>
    /// <param name="id">Node id.</param>
    /// <returns>True when a node was removed.</returns>
    public bool Delete(string id)
    {
        DimseNodeSetting? d = FindDimse(id);
        WebNodeSetting? w = FindWeb(id);
        if (d is null && w is null)
        {
            return false;
        }
        var fields = new Dictionary<string, string>
        {
            ["action"] = "delete",
            ["kind"] = d is not null ? "dimse" : "dicomweb",
            ["node"] = id,
            ["name"] = d?.Name ?? w!.Name,
        };
        Audited("node.change", fields, () =>
        {
            _settings.Update(p =>
            {
                foreach (DimseNodeSetting x in p.Network.DimseNodes.Where(x => x.Id == id).ToArray())
                {
                    p.Network.DimseNodes.Remove(x);
                }
                foreach (WebNodeSetting x in p.Network.WebNodes.Where(x => x.Id == id).ToArray())
                {
                    p.Network.WebNodes.Remove(x);
                }
            });
            _secrets.Delete(PasswordSecret(id));
            _secrets.Delete(TlsSecret(id));
        });
        return true;
    }

    /// <summary>Saves the receiver's settings (it is restarted by <see cref="ScpService"/>, not here). Switching
    /// "accept every AE title" on writes its audit entry first; when that cannot be written nothing is saved.</summary>
    /// <param name="scp">Settings.</param>
    /// <param name="tlsPassword">PKCS#12 password: null keeps the stored one, "" removes it.</param>
    /// <exception cref="NodeValidationException">The settings are not valid; nothing was saved.</exception>
    public void SaveScp(ScpSetting scp, string? tlsPassword = null)
    {
        ArgumentNullException.ThrowIfNull(scp);
        IReadOnlyList<string> problems = Validate(scp);
        if (problems.Count > 0)
        {
            throw new NodeValidationException(problems);
        }
        ScpSetting before = Current.Scp;
        scp.BindAddress = scp.BindAddress.Trim();
        var fields = new Dictionary<string, string>
        {
            ["enabled"] = scp.Enabled ? "yes" : "no",
            ["ae"] = scp.AeTitle,
            ["port"] = scp.Port.ToString(CultureInfo.InvariantCulture),
            ["bind"] = scp.BindAddress.Length == 0 ? "all" : scp.BindAddress,
            ["allowed"] = scp.AllowedCallingAes.Count.ToString(CultureInfo.InvariantCulture),
            ["acceptAny"] = scp.AcceptAnyCallingAe ? "yes" : "no",
            ["tls"] = scp.Tls.Enabled ? "yes" : "no",
            ["queryRetrieve"] = scp.ServeQueryRetrieve ? "yes" : "no",
        };
        if (scp.AcceptAnyCallingAe != before.AcceptAnyCallingAe)
        {
            // The explicit, risky choice (and taking it back) is on record before it takes effect.
            _audit?.AppendRequired("scp.accept-any", AuditOutcome.Intent, new Dictionary<string, string> { ["acceptAny"] = scp.AcceptAnyCallingAe ? "on" : "off" });
        }
        Audited("scp.settings", fields, () =>
        {
            ApplySecret(ScpTlsSecret, tlsPassword, fields, "tlsPassword");
            _settings.Update(p =>
            {
                ScpSetting s = p.Network.Scp;
                s.Enabled = scp.Enabled;
                s.AeTitle = scp.AeTitle;
                s.Port = scp.Port;
                s.BindAddress = scp.BindAddress;
                s.AllowedCallingAes.Clear();
                foreach (string a in scp.AllowedCallingAes.Distinct(StringComparer.Ordinal))
                {
                    s.AllowedCallingAes.Add(a);
                }
                s.AcceptAnyCallingAe = scp.AcceptAnyCallingAe;
                s.Tls.Enabled = scp.Tls.Enabled;
                s.Tls.CaFile = scp.Tls.CaFile.Trim();
                s.Tls.Pkcs12File = scp.Tls.Pkcs12File.Trim();
                s.Tls.ExpectedName = scp.Tls.ExpectedName.Trim();
                s.Tls.Tls13Only = scp.Tls.Tls13Only;
                s.ServeQueryRetrieve = scp.ServeQueryRetrieve;
            });
        });
    }

    // ------------------------------------------------------------------ connections

    internal TkTls? TlsFor(TlsSetting tls, string secretId, bool always = false)
    {
        if (!tls.Enabled && !always)
        {
            return null;
        }
        bool identity = tls.Pkcs12File.Trim().Length > 0;
        if (always && !identity && tls.CaFile.Trim().Length == 0 && tls.ExpectedName.Trim().Length == 0 && !tls.Tls13Only)
        {
            return null;
        }
        return new TkTls
        {
            CaFile = tls.CaFile.Trim(),
            Pkcs12File = tls.Pkcs12File.Trim(),
            Password = identity ? _secrets.Read(secretId) : null,
            ExpectedIdentity = tls.ExpectedName.Trim(),
            Tls13Only = tls.Tls13Only,
        };
    }

    /// <summary>The toolkit peer of a DIMSE node.</summary>
    /// <param name="node">Node.</param>
    /// <param name="allowIncomplete">Send incompletely read files (an explicit, per-send choice).</param>
    /// <returns>The peer.</returns>
    public TkPeer PeerFor(DimseNodeSetting node, bool allowIncomplete = false)
    {
        ArgumentNullException.ThrowIfNull(node);
        return new TkPeer
        {
            Host = node.Host,
            Port = node.Port,
            CalledAe = node.CalledAe,
            CallingAe = node.CallingAe,
            Tls = TlsFor(node.Tls, TlsSecret(node.Id)),
            ConnectTimeout = TimeSpan.FromSeconds(node.ConnectTimeoutSeconds),
            AssociationTimeout = TimeSpan.FromSeconds(node.AssociationTimeoutSeconds),
            DimseTimeout = TimeSpan.FromSeconds(node.DimseTimeoutSeconds),
            MaxPdu = node.MaxPdu,
            AllowLossyTranscode = node.AllowLossyTranscode,
            AllowIncomplete = allowIncomplete,
        };
    }

    /// <summary>A DICOMweb client for a node (dispose it).</summary>
    /// <param name="node">Node.</param>
    /// <returns>The client.</returns>
    public TkWebClient ClientFor(WebNodeSetting node)
    {
        ArgumentNullException.ThrowIfNull(node);
        string? auth = null;
        if (node.Auth != WebAuthKind.None)
        {
            string secret = _secrets.Read(PasswordSecret(node.Id))
                ?? throw new NetworkException("the password or token of this node is not stored on this computer; enter it again in the node's settings");
            auth = node.Auth == WebAuthKind.Bearer ? "Bearer " + secret
                : "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(node.UserName + ":" + secret));
        }
        bool https = node.Address.StartsWith("https://", StringComparison.OrdinalIgnoreCase);
        return new TkWebClient(node.Address, https ? TlsFor(node.Tls, TlsSecret(node.Id), always: true) : null, auth, TimeSpan.FromSeconds(node.TimeoutSeconds));
    }

    /// <summary>The Test button: C-ECHO for a DIMSE node, a QIDO-RS search limited to one study for a DICOMweb node.
    /// Blocks; call it off the UI thread.</summary>
    /// <param name="id">Node id.</param>
    /// <param name="cancellationToken">Cancels.</param>
    /// <returns>The result in plain words.</returns>
    public NodeTestResult Test(string id, CancellationToken cancellationToken)
    {
        try
        {
            if (FindDimse(id) is DimseNodeSetting d)
            {
                TkNetResult r = TkNet.Echo(PeerFor(d), cancellationToken);
                return r.Ok ? new NodeTestResult(true, "The node answered (C-ECHO).")
                    : new NodeTestResult(false, "The node did not accept the test: " + (r.DimseStatus == 0
                        ? "it refused the connection (check both AE titles, and that this viewer is registered there)" : NetworkText.DescribeStatus(r.DimseStatus)) + ".");
            }
            if (FindWeb(id) is WebNodeSetting w)
            {
                using TkWebClient c = ClientFor(w);
                c.Search(TkLevel.Study, "limit=1", null, cancellationToken);
                return new NodeTestResult(true, "The server answered (study search).");
            }
            return new NodeTestResult(false, "This node no longer exists.");
        }
        catch (Exception e) when (e is TkException or NetworkException or System.Security.Cryptography.CryptographicException)
        {
            return new NodeTestResult(false, "The test failed: " + (e is System.Security.Cryptography.CryptographicException
                ? "the secret store of this computer could not be read" : NetworkText.Describe(e)) + ".");
        }
    }
}
