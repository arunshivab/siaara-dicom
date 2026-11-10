// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text.Json.Serialization;

namespace Siaara.Viewer.Core.Network;

/// <summary>How a DIMSE node hands over images.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<RetrieveMethod>))]
public enum RetrieveMethod
{
    /// <summary>C-GET: the images come back on the connection we opened (default; no listener needed).</summary>
    Get,
    /// <summary>C-MOVE: the node opens a connection to our Storage SCP, which must be running.</summary>
    Move,
}

/// <summary>How a DICOMweb node authenticates us.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<WebAuthKind>))]
public enum WebAuthKind
{
    /// <summary>No Authorization header.</summary>
    None,
    /// <summary>HTTP Basic: user name in the settings, password in the secret store.</summary>
    Basic,
    /// <summary>Bearer token from the secret store.</summary>
    Bearer,
}

/// <summary>TLS settings of a node or of our SCP (settings.json). The password of the PKCS#12 file is in the secret
/// store. There is no setting that switches certificate checks off.</summary>
public sealed class TlsSetting
{
    /// <summary>Use TLS.</summary>
    public bool Enabled { get; set; }
    /// <summary>PEM file of trusted authorities; empty = the operating system's trust store.</summary>
    public string CaFile { get; set; } = "";
    /// <summary>Our identity (PKCS#12 file); empty = none. Required for our SCP with TLS.</summary>
    public string Pkcs12File { get; set; } = "";
    /// <summary>Name the other side's certificate must carry; empty = the host name.</summary>
    public string ExpectedName { get; set; } = "";
    /// <summary>Refuse TLS 1.2.</summary>
    public bool Tls13Only { get; set; }
}

/// <summary>A DIMSE node (settings.json; no secrets).</summary>
public sealed class DimseNodeSetting
{
    /// <summary>Stable id (also the prefix of the node's secret ids).</summary>
    public string Id { get; set; } = "";
    /// <summary>Name shown in the app.</summary>
    public string Name { get; set; } = "";
    /// <summary>Host name or address.</summary>
    public string Host { get; set; } = "";
    /// <summary>Port.</summary>
    public int Port { get; set; } = 104;
    /// <summary>The node's AE title.</summary>
    public string CalledAe { get; set; } = "";
    /// <summary>Our AE title towards this node.</summary>
    public string CallingAe { get; set; } = "SIAARA";
    /// <summary>TLS.</summary>
    public TlsSetting Tls { get; init; } = new();
    /// <summary>Retrieve method.</summary>
    public RetrieveMethod Retrieve { get; set; } = RetrieveMethod.Get;
    /// <summary>C-MOVE destination AE; empty = our SCP's AE title.</summary>
    public string MoveDestinationAe { get; set; } = "";
    /// <summary>TCP connect timeout (s).</summary>
    public int ConnectTimeoutSeconds { get; set; } = 15;
    /// <summary>Association timeout (s).</summary>
    public int AssociationTimeoutSeconds { get; set; } = 30;
    /// <summary>Wait for each answer (s).</summary>
    public int DimseTimeoutSeconds { get; set; } = 60;
    /// <summary>Largest PDU we accept; 0 = the toolkit's default.</summary>
    public int MaxPdu { get; set; }
    /// <summary>Sending may compress lossily when the node accepts nothing else (the image then gets a new identifier).</summary>
    public bool AllowLossyTranscode { get; set; }
    /// <summary>Ask the node to confirm storage after a send (Storage Commitment).</summary>
    public bool RequestCommitment { get; set; }
    /// <summary>How long to wait for that confirmation (s).</summary>
    public int CommitmentWaitSeconds { get; set; } = 30;
}

/// <summary>A DICOMweb node (settings.json; no secrets).</summary>
public sealed class WebNodeSetting
{
    /// <summary>Stable id.</summary>
    public string Id { get; set; } = "";
    /// <summary>Name shown in the app.</summary>
    public string Name { get; set; } = "";
    /// <summary>Base URL of the DICOMweb service (http or https).</summary>
    public string Address { get; set; } = "";
    /// <summary>Authentication.</summary>
    public WebAuthKind Auth { get; set; } = WebAuthKind.None;
    /// <summary>User name for Basic.</summary>
    public string UserName { get; set; } = "";
    /// <summary>TLS trust settings for https (Enabled is implied by the URL).</summary>
    public TlsSetting Tls { get; init; } = new();
    /// <summary>Wait for the server (s).</summary>
    public int TimeoutSeconds { get; set; } = 60;
}

/// <summary>Our Storage SCP (settings.json). Off by default.</summary>
public sealed class ScpSetting
{
    /// <summary>Start the receiver with the app.</summary>
    public bool Enabled { get; set; }
    /// <summary>Our AE title.</summary>
    public string AeTitle { get; set; } = "SIAARA";
    /// <summary>Port.</summary>
    public int Port { get; set; } = 11112;
    /// <summary>Address to listen on; empty = every network interface.</summary>
    public string BindAddress { get; set; } = "";
    /// <summary>AE titles allowed to connect.</summary>
    public IList<string> AllowedCallingAes { get; init; } = [];
    /// <summary>Accept every AE title (the list is then not checked). An explicit, audited choice.</summary>
    public bool AcceptAnyCallingAe { get; set; }
    /// <summary>TLS identity.</summary>
    public TlsSetting Tls { get; init; } = new();
    /// <summary>Also answer queries and retrieves from the local store for the allowed AEs.</summary>
    public bool ServeQueryRetrieve { get; set; }
}

/// <summary>Networking part of settings.json.</summary>
public sealed class NetworkSetting
{
    /// <summary>DIMSE nodes.</summary>
    public IList<DimseNodeSetting> DimseNodes { get; init; } = [];
    /// <summary>DICOMweb nodes.</summary>
    public IList<WebNodeSetting> WebNodes { get; init; } = [];
    /// <summary>Our receiver.</summary>
    public ScpSetting Scp { get; init; } = new();
    /// <summary>Most studies a remote search returns before it asks to narrow the search.</summary>
    public int SearchCap { get; set; } = 200;
}

/// <summary>A node as lists and menus show it.</summary>
/// <param name="Id">Id.</param>
/// <param name="Name">Name.</param>
/// <param name="IsWeb">DICOMweb (else DIMSE).</param>
public sealed record NodeRef(string Id, string Name, bool IsWeb);
