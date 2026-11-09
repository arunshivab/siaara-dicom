using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Web;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>A tiny HTTP/1.1 server on 127.0.0.1 for the DICOMweb client tests (keep-alive, Content-Length).</summary>
internal sealed class LoopbackHttpServer : IDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly Func<string, string, byte[], (int Status, string ContentType, byte[] Body)> _handler;
    private readonly Thread _thread;
    private volatile bool _stop;

    internal ConcurrentQueue<string> Requests { get; } = new();

    internal LoopbackHttpServer(Func<string, string, byte[], (int, string, byte[])> handler)
    {
        _handler = handler;
        _listener.Start();
        _thread = new Thread(Loop) { IsBackground = true };
        _thread.Start();
    }

    internal int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

    private void Loop()
    {
        while (!_stop)
        {
            TcpClient c;
            try
            {
                c = _listener.AcceptTcpClient();
            }
            catch (SocketException)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            var t = new Thread(() => Serve(c)) { IsBackground = true };
            t.Start();
        }
    }

    private void Serve(TcpClient client)
    {
        using (client)
        {
            NetworkStream s = client.GetStream();
            var buf = new MemoryStream();
            byte[] tmp = new byte[8192];
            try
            {
                while (true)
                {
                    int headerEnd;
                    while ((headerEnd = IndexOf(buf, "\r\n\r\n"u8)) < 0)
                    {
                        int n = s.Read(tmp, 0, tmp.Length);
                        if (n <= 0)
                        {
                            return;
                        }
                        buf.Write(tmp, 0, n);
                    }
                    string head = Encoding.ASCII.GetString(buf.GetBuffer(), 0, headerEnd);
                    string[] lines = head.Split("\r\n");
                    string[] first = lines[0].Split(' ');
                    int length = 0;
                    foreach (string l in lines)
                    {
                        if (l.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                        {
                            length = int.Parse(l.AsSpan(15).Trim(), System.Globalization.CultureInfo.InvariantCulture);
                        }
                    }
                    while (buf.Length < headerEnd + 4 + length)
                    {
                        int n = s.Read(tmp, 0, tmp.Length);
                        if (n <= 0)
                        {
                            return;
                        }
                        buf.Write(tmp, 0, n);
                    }
                    byte[] body = buf.GetBuffer().AsSpan(headerEnd + 4, length).ToArray();
                    byte[] rest = buf.GetBuffer().AsSpan(headerEnd + 4 + length, (int)buf.Length - headerEnd - 4 - length).ToArray();
                    buf = new MemoryStream();
                    buf.Write(rest);
                    Requests.Enqueue(head);
                    (int status, string type, byte[] resp) = _handler(first[0], first[1], body);
                    string h = $"HTTP/1.1 {status} X\r\nContent-Type: {type}\r\nContent-Length: {resp.Length}\r\n\r\n";
                    s.Write(Encoding.ASCII.GetBytes(h));
                    s.Write(resp);
                }
            }
            catch (IOException)
            {
            }
        }
    }

    private static int IndexOf(MemoryStream m, ReadOnlySpan<byte> pattern) =>
        m.GetBuffer().AsSpan(0, (int)m.Length).IndexOf(pattern);

    public void Dispose()
    {
        _stop = true;
        _listener.Dispose();
    }
}

internal static class JsonWebTests
{
    internal static void Register()
    {
        Case("json: dataset to DICOM JSON and back", () =>
        {
            using DicomFile f = Builders.Smoke();
            f.Dataset.SetString(DicomTags.SpecificCharacterSet, "ISO_IR 100");
            f.Dataset.SetString(DicomTags.StudyDescription, "Schädel");
            string json = f.Dataset.ToJson();
            Check(json.StartsWith('{'));
            Check(json.Contains("\"00100010\"", StringComparison.Ordinal));
            Check(json.Contains("Schädel", StringComparison.Ordinal)); // UTF-8 in JSON
            Check(json.Contains("InlineBinary", StringComparison.Ordinal));
            string strict = f.Dataset.ToJson(DicomJsonOptions.NumbersAsStrings | DicomJsonOptions.StrictCharset);
            Check(strict.Contains("\"-1024\"", StringComparison.Ordinal));
            using DicomFile back = DicomFile.FromJson(json);
            CheckEq(back.Dataset.GetString(DicomTags.PatientName), "Doe^Jane");
            CheckEq(back.Dataset.GetString(DicomTags.StudyDescription), "Schädel");
            Check(back.DecodeFrame(0).AsSpan().SequenceEqual(f.DecodeFrame(0)));
            CheckEq(Throws<SiaaraException>(() => DicomFile.FromJson("{\"0010"))?.ErrorCode is SiaaraErrorCode.Malformed or SiaaraErrorCode.Truncated, true);
        });

        Case("web: QIDO, WADO, STOW and Authorization against a loopback server", () =>
        {
            byte[] instance;
            string tmp = Path.Combine(NewTempDir("web"), "i.dcm");
            using (DicomFile f = Builders.Smoke())
            {
                f.Save(tmp, DicomTransferSyntaxes.ExplicitVRLittleEndian);
            }
            instance = File.ReadAllBytes(tmp);
            string bigJson = "[" + string.Join(",", System.Linq.Enumerable.Repeat("{\"00100020\":{\"vr\":\"LO\",\"Value\":[\"" + new string('P', 100) + "\"]}}", 1500)) + "]";
            using var server = new LoopbackHttpServer((method, path, body) =>
            {
                if (method == "GET" && path.StartsWith("/dicom-web/studies?", StringComparison.Ordinal))
                {
                    string q = path.Contains("big=1", StringComparison.Ordinal) ? bigJson : "[{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"1.2.3\"]}}]";
                    return (200, "application/dicom+json", Encoding.UTF8.GetBytes(q));
                }
                if (method == "GET" && path.StartsWith("/dicom-web/studies/1.2.3/series/4.5/instances/6.7", StringComparison.Ordinal))
                {
                    string boundary = "BOUNDARY42";
                    var ms = new MemoryStream();
                    ms.Write(Encoding.ASCII.GetBytes($"--{boundary}\r\nContent-Type: application/dicom\r\n\r\n"));
                    ms.Write(instance);
                    ms.Write(Encoding.ASCII.GetBytes($"\r\n--{boundary}--\r\n"));
                    return (200, $"multipart/related; type=\"application/dicom\"; boundary={boundary}", ms.ToArray());
                }
                if (method == "POST" && path.StartsWith("/dicom-web/studies", StringComparison.Ordinal))
                {
                    return (200, "application/dicom+json", "{}"u8.ToArray());
                }
                return (404, "text/plain", "no"u8.ToArray());
            });
            using var client = new DicomWebClient("http://127.0.0.1:" + server.Port + "/dicom-web", TimeSpan.FromSeconds(10));
            client.SetAuthorization("Bearer token123");
            string json = client.Search(QidoLevel.Studies, "PatientID=X&limit=10");
            Check(json.Contains("1.2.3", StringComparison.Ordinal));
            Check(server.Requests.ToArray()[^1].Contains("Authorization: Bearer token123", StringComparison.Ordinal));
            CheckEq(client.GetLastResponse(), json);
            string big = client.Search(QidoLevel.Studies, "big=1");
            CheckEq(big.Length, bigJson.Length); // larger than the first buffer: read via last_response
            int before = server.Requests.Count;
            CheckEq(client.SearchAsync(QidoLevel.Studies, "PatientID=Y").GetAwaiter().GetResult().Contains("1.2.3", StringComparison.Ordinal), true);
            CheckEq(server.Requests.Count, before + 1);
            using (DicomFile got = client.RetrieveInstance("1.2.3", "4.5", "6.7"))
            {
                CheckEq(got.Dataset.GetString(DicomTags.PatientName), "Doe^Jane");
            }
            using (DicomFile up = Builders.Smoke())
            {
                client.Store(up);
                client.StoreAsync(up, "1.2.3").GetAwaiter().GetResult();
            }
            var nf = Throws<SiaaraException>(() => client.Search(QidoLevel.Series, null, "9.9.9"));
            Check(nf?.ErrorCode is SiaaraErrorCode.NotFound or SiaaraErrorCode.Rejected);
            client.SetAuthorization(null);
            _ = client.Search(QidoLevel.Studies, "x=1");
            Check(!server.Requests.ToArray()[^1].Contains("Authorization", StringComparison.Ordinal));
            using var cts = new CancellationTokenSource();
            cts.Cancel();
            Check(Throws<TaskCanceledException>(() => client.SearchAsync(QidoLevel.Studies, null, null, null, cts.Token).GetAwaiter().GetResult()) is not null);
        });

        Case("web: TLS availability is reported; https client creation", () =>
        {
            bool tls = SiaaraLibrary.IsTlsAvailable(out string reason);
            Console.WriteLine("  TLS available: " + tls + (tls ? "" : " (" + reason + ")"));
            if (!tls)
            {
                Check(reason.Length > 0);
                return;
            }
            // Creating a client does not connect; a request to a closed port fails with Network.
            using var c = new DicomWebClient("https://127.0.0.1:1/dicom-web", new TlsOptions { Tls13Only = true }, TimeSpan.FromSeconds(2));
            var e = Throws<SiaaraException>(() => c.Search(QidoLevel.Studies));
            Check(e?.ErrorCode is SiaaraErrorCode.Network or SiaaraErrorCode.Timeout);
        });
    }
}
