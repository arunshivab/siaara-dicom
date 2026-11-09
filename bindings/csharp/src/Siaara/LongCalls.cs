using System;
using System.Runtime.InteropServices;
using System.Threading;
using Siaara.Interop;

namespace Siaara;

/// <summary>Progress of a long native call: steps done and the total (0 = unknown).</summary>
/// <param name="Done">Steps done.</param>
/// <param name="Total">Total steps, 0 when not known.</param>
public readonly record struct OperationProgress(long Done, long Total);

/// <summary>Runs the cancellable *_ex calls with a native token and a progress trampoline.</summary>
internal static class LongCall
{
    internal delegate int Body(nint cancel, nint callback, nint user);

    /// <summary>Calls body; cancellation (token or progress) becomes OperationCanceledException.</summary>
    internal static void Run(IProgress<OperationProgress>? progress, Body body, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        var sink = new Sink(progress);
        GCHandle gc = GCHandle.Alloc(sink);
        try
        {
            using var cancel = new NativeCancellation(token);
            int st = body(cancel.Pointer, progress is null ? 0 : Callbacks.ProgressPointer, GCHandle.ToIntPtr(gc));
            if (sink.Error is not null)
            {
                throw new InvalidOperationException("The progress handler failed: " + sink.Error.Message, sink.Error);
            }
            if (st == C.SIAARA_ERR_CANCELLED)
            {
                throw new OperationCanceledException(Status.LastMessage(), token);
            }
            Status.Check(st);
        }
        finally
        {
            gc.Free();
        }
    }

    private sealed class Sink : IProgressSink
    {
        private readonly IProgress<OperationProgress>? _progress;

        internal Sink(IProgress<OperationProgress>? progress) => _progress = progress;

        internal Exception? Error { get; private set; }

        public void OnCallbackException(Exception exception) => Error ??= exception;

        public int OnProgress(ulong done, ulong total)
        {
            _progress?.Report(new OperationProgress((long)Math.Min(done, long.MaxValue), (long)Math.Min(total, long.MaxValue)));
            return 0;
        }
    }
}
