using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Siaara.Interop;

/// <summary>Receives exceptions thrown while handling a native callback.</summary>
internal interface ICallbackSink
{
    /// <summary>Called on the callback thread; must not throw.</summary>
    void OnCallbackException(Exception exception);
}

/// <summary>Target of siaara_store_callback.</summary>
internal interface IStoreEventSink : ICallbackSink
{
    void OnStoreEvent(int kind, string sopInstanceUid, string seriesUid, string studyUid);
}

/// <summary>Target of siaara_scan_callback. Returns 0 to continue, non-zero to stop.</summary>
internal interface IScanSink : ICallbackSink
{
    int OnScanItem(string path, int kind, int status, bool hasRecord, in siaara_index_record record, ReadOnlySpan<byte> strings);
}

/// <summary>Target of siaara_dicomdir_callback. Returns 0 to continue, non-zero to stop.</summary>
internal interface IDicomdirSink : ICallbackSink
{
    int OnRecord(in siaara_dicomdir_entry entry, in siaara_index_record keys, ReadOnlySpan<byte> strings);
}

/// <summary>Target of siaara_find_callback. Returns 0 to continue.</summary>
internal interface IFindSink : ICallbackSink
{
    int OnMatch(nint dataset);
}

/// <summary>Target of siaara_dimse_progress_callback. Returns 0 to continue.</summary>
internal interface IDimseProgressSink : ICallbackSink
{
    int OnProgress(in siaara_dimse_result progress);
}

/// <summary>Target of siaara_progress_callback. Returns 0 to continue.</summary>
internal interface IProgressSink : ICallbackSink
{
    int OnProgress(ulong done, ulong total);
}

/// <summary>A siaara_scp_event copied out of native memory (Dataset is borrowed: valid during the call).</summary>
internal readonly record struct ScpEventData(int Kind, int DimseStatus, int Outcome, string CallingAe, string SopClassUid,
    string SopInstanceUid, string PeerAddress, bool UidMismatch, string CommandSopClassUid, string CommandSopInstanceUid,
    string OriginalSopInstanceUid, string TransactionUid, string DestinationAe, nint Dataset, int Completed, int Failed,
    int Warning, int CommandField, ulong AssociationId = 0, uint ReadLoss = 0);

/// <summary>Target of siaara_scp_callback.</summary>
internal interface IScpSink : ICallbackSink
{
    void OnEvent(in ScpEventData e);
}

/// <summary>The emit() of a siaara_worklist_callback, valid during that callback only.</summary>
internal readonly unsafe struct WorklistEmitter
{
    private readonly delegate* unmanaged[Cdecl]<void*, nint, int> _emit;
    private readonly void* _context;

    internal WorklistEmitter(delegate* unmanaged[Cdecl]<void*, nint, int> emit, void* context)
    {
        _emit = emit;
        _context = context;
    }

    /// <summary>Offers one item (a native data set); false once the query should stop.</summary>
    internal bool Emit(nint dataset) => _emit != null && _emit(_context, dataset) == 0;
}

/// <summary>Target of siaara_worklist_callback: returns the final DIMSE status.</summary>
internal interface IWorklistSink : ICallbackSink
{
    uint OnQuery(string callingAe, nint identifier, WorklistEmitter emitter);
}

/// <summary>Target of siaara_uid_change_callback.</summary>
internal interface IUidChangeSink : ICallbackSink
{
    void OnUidChange(int index, string originalUid, string newUid);
}

/// <summary>Target of siaara_web_authorize_callback: true to allow.</summary>
internal interface IWebAuthorizeSink : ICallbackSink
{
    bool Authorize(string method, string target, string authorization);
}

/// <summary>Target of siaara_web_progress_callback. Returns 0 to continue.</summary>
internal interface IWebProgressSink : ICallbackSink
{
    int OnProgress(uint parts, ulong bytes, ulong totalBytes);
}

/// <summary>Target of siaara_log_callback.</summary>
internal interface ILogSink : ICallbackSink
{
    void OnLog(int level, bool containsPhi, string message);
}

/// <summary>
/// [UnmanagedCallersOnly] trampolines for the C callbacks. user_data is a GCHandle to the sink.
/// Every exception is caught here and handed to the sink: nothing ever unwinds into native code.
/// A scan or DICOMDIR callback that failed returns 1 (stop).
/// </summary>
internal static unsafe class Callbacks
{
    internal static nint StoreEventPointer =>
        (nint)(delegate* unmanaged[Cdecl]<void*, int, byte*, byte*, byte*, void>)&StoreEvent;

    internal static nint ScanPointer =>
        (nint)(delegate* unmanaged[Cdecl]<void*, byte*, int, int, siaara_index_record*, byte*, int>)&ScanItem;

    internal static nint DicomdirPointer =>
        (nint)(delegate* unmanaged[Cdecl]<void*, siaara_dicomdir_entry*, siaara_index_record*, byte*, int>)&DicomdirRecord;

    internal static nint FindPointer => (nint)(delegate* unmanaged[Cdecl]<void*, nint, int>)&FindMatch;

    internal static nint DimseProgressPointer =>
        (nint)(delegate* unmanaged[Cdecl]<void*, siaara_dimse_result*, int>)&DimseProgress;

    internal static nint ProgressPointer => (nint)(delegate* unmanaged[Cdecl]<void*, ulong, ulong, int>)&Progress;

    internal static nint ScpEventPointer => (nint)(delegate* unmanaged[Cdecl]<void*, siaara_scp_event*, void>)&ScpEvent;

    internal static nint LogPointer => (nint)(delegate* unmanaged[Cdecl]<void*, int, int, byte*, void>)&Log;

    internal static nint WorklistPointer =>
        (nint)(delegate* unmanaged[Cdecl]<void*, byte*, nint, delegate* unmanaged[Cdecl]<void*, nint, int>, void*, uint>)&Worklist;

    internal static nint UidChangePointer => (nint)(delegate* unmanaged[Cdecl]<void*, uint, byte*, byte*, void>)&UidChange;

    internal static nint WebAuthorizePointer => (nint)(delegate* unmanaged[Cdecl]<void*, byte*, byte*, byte*, int>)&WebAuthorize;

    internal static nint WebProgressPointer => (nint)(delegate* unmanaged[Cdecl]<void*, uint, ulong, ulong, int>)&WebProgress;

    /// <summary>Length of the string table a record refers to (largest offset + length + NUL).</summary>
    internal static int StringTableLength(in siaara_index_record record)
    {
        long max = 0;
        for (int k = 0; k < C.SIAARA_INDEX_MAX_STRINGS; k++)
        {
            long end = (long)record.str_offset[k] + record.str_length[k];
            if (record.str_length[k] != 0 && end + 1 > max)
            {
                max = end + 1;
            }
        }
        return max > int.MaxValue ? int.MaxValue : (int)max;
    }

    private static T? Sink<T>(void* user) where T : class =>
        user == null ? null : GCHandle.FromIntPtr((nint)user).Target as T;

    private static void Report(void* user, Exception exception)
    {
        try
        {
            Sink<ICallbackSink>(user)?.OnCallbackException(exception);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception)
#pragma warning restore CA1031
        {
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void StoreEvent(void* user, int kind, byte* sop, byte* series, byte* study)
    {
        try
        {
            Sink<IStoreEventSink>(user)?.OnStoreEvent(kind,
                Marshal.PtrToStringUTF8((nint)sop) ?? string.Empty,
                Marshal.PtrToStringUTF8((nint)series) ?? string.Empty,
                Marshal.PtrToStringUTF8((nint)study) ?? string.Empty);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int ScanItem(void* user, byte* path, int kind, int status, siaara_index_record* rec, byte* strings)
    {
        try
        {
            IScanSink? sink = Sink<IScanSink>(user);
            if (sink is null)
            {
                return 1;
            }
            string p = Marshal.PtrToStringUTF8((nint)path) ?? string.Empty;
            if (rec == null)
            {
                siaara_index_record empty = default;
                return sink.OnScanItem(p, kind, status, false, in empty, default);
            }
            int len = strings == null ? 0 : StringTableLength(in *rec);
            return sink.OnScanItem(p, kind, status, true, in *rec, new ReadOnlySpan<byte>(strings, len));
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int DicomdirRecord(void* user, siaara_dicomdir_entry* entry, siaara_index_record* keys, byte* strings)
    {
        try
        {
            IDicomdirSink? sink = Sink<IDicomdirSink>(user);
            if (sink is null || entry == null || keys == null)
            {
                return 1;
            }
            int len = strings == null ? 0 : StringTableLength(in *keys);
            return sink.OnRecord(in *entry, in *keys, new ReadOnlySpan<byte>(strings, len));
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int FindMatch(void* user, nint dataset)
    {
        try
        {
            IFindSink? sink = Sink<IFindSink>(user);
            return sink is null ? 1 : sink.OnMatch(dataset);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int DimseProgress(void* user, siaara_dimse_result* progress)
    {
        try
        {
            IDimseProgressSink? sink = Sink<IDimseProgressSink>(user);
            return sink is null || progress == null ? 1 : sink.OnProgress(in *progress);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int Progress(void* user, ulong done, ulong total)
    {
        try
        {
            IProgressSink? sink = Sink<IProgressSink>(user);
            return sink is null ? 1 : sink.OnProgress(done, total);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void ScpEvent(void* user, siaara_scp_event* e)
    {
        try
        {
            if (e != null)
            {
                static string S(nint p) => p == 0 ? string.Empty : Marshal.PtrToStringUTF8(p) ?? string.Empty;
                // Fields after peer_address exist only when struct_size covers them (older libraries).
                // v2 ends with command_field; association_id and read_loss came later.
                bool full = e->struct_size >= (ulong)((byte*)&e->command_field - (byte*)e) + sizeof(uint);
                bool v3 = e->struct_size >= (ulong)((byte*)&e->read_loss - (byte*)e) + sizeof(uint);
                var d = new ScpEventData(e->kind, (int)e->dimse_status, e->outcome, S(e->calling_ae), S(e->sop_class_uid),
                    S(e->sop_instance_uid), S(e->peer_address),
                    full && e->uid_mismatch != 0, full ? S(e->command_sop_class_uid) : string.Empty,
                    full ? S(e->command_sop_instance_uid) : string.Empty, full ? S(e->original_sop_instance_uid) : string.Empty,
                    full ? S(e->transaction_uid) : string.Empty, full ? S(e->destination_ae) : string.Empty,
                    full ? e->dataset : 0, full ? (int)e->completed : 0, full ? (int)e->failed : 0, full ? (int)e->warning : 0,
                    full ? (int)e->command_field : 0, v3 ? e->association_id : 0UL, v3 ? e->read_loss : 0u);
                Sink<IScpSink>(user)?.OnEvent(in d);
            }
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception ex)
#pragma warning restore CA1031
        {
            Report(user, ex);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static uint Worklist(void* user, byte* calling, nint identifier, delegate* unmanaged[Cdecl]<void*, nint, int> emit,
        void* context)
    {
        try
        {
            IWorklistSink? sink = Sink<IWorklistSink>(user);
            return sink is null ? 0xC000u
                : sink.OnQuery(Marshal.PtrToStringUTF8((nint)calling) ?? string.Empty, identifier, new WorklistEmitter(emit, context));
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 0xC000u;  // Unable to process
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void UidChange(void* user, uint index, byte* original, byte* changed)
    {
        try
        {
            Sink<IUidChangeSink>(user)?.OnUidChange((int)index, Marshal.PtrToStringUTF8((nint)original) ?? string.Empty,
                Marshal.PtrToStringUTF8((nint)changed) ?? string.Empty);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int WebAuthorize(void* user, byte* method, byte* target, byte* authorization)
    {
        try
        {
            IWebAuthorizeSink? sink = Sink<IWebAuthorizeSink>(user);
            return sink is not null && sink.Authorize(Marshal.PtrToStringUTF8((nint)method) ?? string.Empty,
                Marshal.PtrToStringUTF8((nint)target) ?? string.Empty, Marshal.PtrToStringUTF8((nint)authorization) ?? string.Empty)
                ? 1 : 0;
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 0;  // deny
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int WebProgress(void* user, uint parts, ulong bytes, ulong total)
    {
        try
        {
            IWebProgressSink? sink = Sink<IWebProgressSink>(user);
            return sink is null ? 1 : sink.OnProgress(parts, bytes, total);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception e)
#pragma warning restore CA1031
        {
            Report(user, e);
            return 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void Log(void* user, int level, int phi, byte* message)
    {
        try
        {
            Sink<ILogSink>(user)?.OnLog(level, phi != 0, Marshal.PtrToStringUTF8((nint)message) ?? string.Empty);
        }
#pragma warning disable CA1031 // a callback must never let an exception reach native code
        catch (Exception)
#pragma warning restore CA1031
        {
        }
    }
}
