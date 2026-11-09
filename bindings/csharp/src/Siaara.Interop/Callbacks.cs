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
}
