// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Buffers;
using System.Net.WebSockets;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.DependencyInjection;
using Siaara.Viewer.Core.Logging;
using Siaara.Viewer.Core.Viewing;
using Siaara.Viewer.Web.Security;

namespace Siaara.Viewer.Web.Streaming;

/// <summary>
/// /ws/view?s=SESSION: one binary WebSocket per viewer session (separate from the Blazor circuit).
/// The upgrade needs the session cookie and the app's exact Origin. Inbound messages over 64 KB
/// close the socket (the browser only sends acks, input events and small commands). Sending is
/// serialised through one bounded channel; frames are rate-limited by the per-viewport flow control
/// (one in flight), so the queue stays short. A browser that stops reading until the queue is full
/// is disconnected; when it reconnects the session sends the current state of every viewport again.
/// A sign-out closes the socket.
/// </summary>
public static class ViewSocketEndpoint
{
    /// <summary>Maps the endpoint.</summary>
    /// <param name="endpoints">Routes.</param>
    /// <returns>The builder.</returns>
    public static IEndpointConventionBuilder MapViewSocket(this IEndpointRouteBuilder endpoints)
    {
        ArgumentNullException.ThrowIfNull(endpoints);
        return endpoints.Map("/ws/view", HandleAsync);
    }

    private static async Task HandleAsync(HttpContext ctx)
    {
        LaunchSecurity sec = ctx.RequestServices.GetRequiredService<LaunchSecurity>();
        if (!ctx.WebSockets.IsWebSocketRequest)
        {
            ctx.Response.StatusCode = StatusCodes.Status400BadRequest;
            return;
        }
        if (!sec.WebSocketAllowed(ctx))
        {
            ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
            return;
        }
        SessionRegistry reg = ctx.RequestServices.GetRequiredService<SessionRegistry>();
        ViewerSession? session = reg.Get(ctx.Request.Query["s"]);
        if (session is null)
        {
            ctx.Response.StatusCode = StatusCodes.Status404NotFound;
            return;
        }
        // Ends with the request, with a sign-out, or when the browser stops reading (the sink aborts).
        using var life = CancellationTokenSource.CreateLinkedTokenSource(ctx.RequestAborted, sec.SessionToken);
        using WebSocket ws = await ctx.WebSockets.AcceptWebSocketAsync().ConfigureAwait(false);
        using var sink = new SocketSink(ws, life);
        Task sending = sink.RunAsync(life.Token);
        session.Attach(sink);
        try
        {
            await ReceiveAsync(ws, session, life.Token).ConfigureAwait(false);
        }
        catch (WebSocketException)
        {
            // Browser went away.
        }
        catch (OperationCanceledException)
        {
            // Shutdown or abort.
        }
        finally
        {
            session.Detach(sink);
            sink.Complete();
            try
            {
                await sending.ConfigureAwait(false);
            }
            catch (WebSocketException)
            {
                // Closed under us.
            }
            catch (OperationCanceledException)
            {
                // Aborted.
            }
        }
    }

    private static async Task ReceiveAsync(WebSocket ws, ViewerSession session, CancellationToken ct)
    {
        byte[] buf = ArrayPool<byte>.Shared.Rent(16 * 1024);
        try
        {
            var message = new ArrayBufferWriter<byte>(1024);
            while (ws.State == WebSocketState.Open)
            {
                ValueWebSocketReceiveResult r = await ws.ReceiveAsync(buf.AsMemory(), ct).ConfigureAwait(false);
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    await ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "bye", ct).ConfigureAwait(false);
                    return;
                }
                if (message.WrittenCount + r.Count > FrameProtocol.MaxInboundMessageBytes)
                {
                    await ws.CloseOutputAsync(WebSocketCloseStatus.MessageTooBig, "message too big", ct).ConfigureAwait(false);
                    return;
                }
                message.Write(buf.AsSpan(0, r.Count));
                if (!r.EndOfMessage)
                {
                    continue;
                }
                if (r.MessageType == WebSocketMessageType.Binary && FrameProtocol.Decode(message.WrittenMemory.ToArray()) is BrowserMessage m)
                {
                    try
                    {
                        InputRouter.Dispatch(session, m);
                    }
                    catch (Exception e) when (e is InvalidOperationException or FormatException or System.Text.Json.JsonException or ArgumentException)
                    {
                        // A malformed message must not end the receive loop with an unhandled failure.
                        AppLog.Warn("socket", "a malformed message from the browser was ignored (" + e.GetType().Name + ")");
                    }
                }
                message.ResetWrittenCount();
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buf);
        }
    }
}

/// <summary>
/// The sending side of one /ws/view socket: a bounded queue (<see cref="FrameProtocol.MaxQueuedOutbound"/>) drained
/// by one sender. When the queue is full the browser has stopped reading: nothing more is queued, pooled
/// buffers go back to the pool and the socket's lifetime is cancelled.
/// </summary>
internal sealed class SocketSink : IViewSink, IDisposable
{
    private readonly WebSocket _ws;
    private readonly CancellationTokenSource _life;
    private readonly Channel<(byte[] Buffer, int Length, bool Pooled)> _out =
        Channel.CreateBounded<(byte[], int, bool)>(new BoundedChannelOptions(FrameProtocol.MaxQueuedOutbound) { SingleReader = true, FullMode = BoundedChannelFullMode.Wait });
    private int _overflowed;

    public SocketSink(WebSocket ws, CancellationTokenSource life)
    {
        _ws = ws;
        _life = life;
    }

    // The queue is full: the browser is not reading. Nothing more is queued and the socket is closed; the
    // browser's reconnect makes the session send every viewport afresh, so no state is lost.
    private void Overflow()
    {
        if (Interlocked.Exchange(ref _overflowed, 1) == 0)
        {
            AppLog.Warn("socket", "the browser stopped reading; the view socket was closed");
            _out.Writer.TryComplete();
            try
            {
                _life.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // The request ended at the same moment.
            }
        }
    }

    /// <summary>True once the queue overflowed and the socket was given up.</summary>
    public bool Overflowed => Volatile.Read(ref _overflowed) != 0;

    /// <summary>Messages waiting to be sent.</summary>
    public int Queued => _out.Reader.Count;

    public int FrameHeaderBytes => FrameProtocol.FrameHeaderBytes;

    public void SendFrame(int viewport, uint seq, FramePixelFormat format, int width, int height, byte[] rented, int length)
    {
        FrameProtocol.WriteFrameHeader(rented, format, viewport, seq, width, height);
        if (!_out.Writer.TryWrite((rented, length, true)))
        {
            ArrayPool<byte>.Shared.Return(rented);
            Overflow();
        }
    }

    public void SendState(int viewport, uint seq, ViewportStateMessage state)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(state, ViewJsonContext.Default.ViewportStateMessage);
        byte[] msg = FrameProtocol.State(viewport, seq, json);
        if (!_out.Writer.TryWrite((msg, msg.Length, false)))
        {
            Overflow();
        }
    }

    public void Complete() => _out.Writer.TryComplete();

    public async Task RunAsync(CancellationToken ct)
    {
        await foreach ((byte[] buffer, int length, bool pooled) in _out.Reader.ReadAllAsync(ct).ConfigureAwait(false))
        {
            try
            {
                if (_ws.State == WebSocketState.Open)
                {
                    await _ws.SendAsync(buffer.AsMemory(0, length), WebSocketMessageType.Binary, true, ct).ConfigureAwait(false);
                }
            }
            catch (WebSocketException e)
            {
                AppLog.Warn("socket", "send failed: " + e.WebSocketErrorCode);
            }
            finally
            {
                if (pooled)
                {
                    ArrayPool<byte>.Shared.Return(buffer);
                }
            }
        }
    }

    public void Dispose()
    {
        _out.Writer.TryComplete();
        while (_out.Reader.TryRead(out var item))
        {
            if (item.Pooled)
            {
                ArrayPool<byte>.Shared.Return(item.Buffer);
            }
        }
    }
}
