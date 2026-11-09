using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace Siaara.Interop;

/// <summary>Base of every owned SIAARA handle: zero is invalid, release calls the matching *_free.</summary>
internal abstract class SiaaraHandle : SafeHandle
{
    protected SiaaraHandle() : base(0, ownsHandle: true)
    {
    }

    public override bool IsInvalid => handle == 0;

    /// <summary>Takes ownership of a pointer returned by a *_open / *_create call.</summary>
    internal void Attach(nint pointer)
    {
        if (handle != 0)
        {
            throw new InvalidOperationException("The handle is already set.");
        }
        SetHandle(pointer);
    }
}

/// <summary>siaara_file (siaara_file_free).</summary>
internal sealed class FileHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_file_free(handle);
        return true;
    }
}

/// <summary>siaara_web_client (siaara_web_client_free).</summary>
internal sealed class WebClientHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_web_client_free(handle);
        return true;
    }
}

/// <summary>
/// siaara_store (siaara_store_free). Also owns the GCHandle passed as callback user data: the
/// callback is removed before the store is freed and the GCHandle is freed after it, so no
/// callback can run with a dangling user pointer, also when the store is only finalized. The
/// target must not reference the store's public object (that would keep it from being collected).
/// </summary>
internal sealed class StoreHandle : SiaaraHandle
{
    private GCHandle _callbackTarget;

    /// <summary>True once the event callback is installed.</summary>
    internal bool HasCallback => _callbackTarget.IsAllocated;

    /// <summary>
    /// Installs the event callback once for the lifetime of the store. It is never removed while the
    /// store is in use (another thread may be inside a callback); ReleaseHandle removes it.
    /// Returns the C status. The caller serialises calls.
    /// </summary>
    internal int InstallCallback(object target)
    {
        if (_callbackTarget.IsAllocated)
        {
            return C.SIAARA_OK;
        }
        bool added = false;
        DangerousAddRef(ref added);
        try
        {
            GCHandle gc = GCHandle.Alloc(target);
            int status = Native.siaara_store_set_callback(handle, Callbacks.StoreEventPointer, GCHandle.ToIntPtr(gc));
            if (status != C.SIAARA_OK)
            {
                gc.Free();
                return status;
            }
            _callbackTarget = gc;
            return status;
        }
        finally
        {
            if (added)
            {
                DangerousRelease();
            }
        }
    }

    protected override bool ReleaseHandle()
    {
        if (_callbackTarget.IsAllocated)
        {
            _ = Native.siaara_store_set_callback(handle, 0, 0);
        }
        Raw.siaara_store_free(handle);
        if (_callbackTarget.IsAllocated)
        {
            _callbackTarget.Free();
        }
        return true;
    }
}

/// <summary>siaara_store_result (siaara_store_result_free).</summary>
internal sealed class StoreResultHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_store_result_free(handle);
        return true;
    }
}

/// <summary>siaara_sr (siaara_sr_free).</summary>
internal sealed class SrHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_sr_free(handle);
        return true;
    }
}

/// <summary>siaara_seg (siaara_seg_free).</summary>
internal sealed class SegHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_seg_free(handle);
        return true;
    }
}

/// <summary>siaara_rtstruct (siaara_rtstruct_free).</summary>
internal sealed class RtStructHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_rtstruct_free(handle);
        return true;
    }
}

/// <summary>siaara_volume (siaara_volume_free).</summary>
internal sealed class VolumeHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_volume_free(handle);
        return true;
    }
}

/// <summary>siaara_view (siaara_view_free).</summary>
internal sealed class ViewHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_view_free(handle);
        return true;
    }
}

/// <summary>siaara_frame (siaara_frame_free).</summary>
internal sealed class FrameHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_frame_free(handle);
        return true;
    }
}

/// <summary>siaara_pstate (siaara_pstate_free).</summary>
internal sealed class PresentationStateHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_pstate_free(handle);
        return true;
    }
}

/// <summary>siaara_buffer (siaara_buffer_free).</summary>
internal sealed class BufferHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_buffer_free(handle);
        return true;
    }
}

/// <summary>siaara_cancel (siaara_cancel_free).</summary>
internal sealed class CancelHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_cancel_free(handle);
        return true;
    }
}

/// <summary>siaara_web_parts (siaara_web_parts_free).</summary>
internal sealed class WebPartsHandle : SiaaraHandle
{
    protected override bool ReleaseHandle()
    {
        Raw.siaara_web_parts_free(handle);
        return true;
    }
}

/// <summary>
/// siaara_scp (siaara_scp_free, which stops the server and joins its threads). Owns the GCHandle
/// passed as callback user data; it is freed only after the SCP is gone, so no callback can see a
/// dangling pointer. The target must not reference the public object.
/// </summary>
internal sealed class ScpHandle : SiaaraHandle
{
    internal GCHandle CallbackTarget;

    protected override bool ReleaseHandle()
    {
        Raw.siaara_scp_free(handle);
        if (CallbackTarget.IsAllocated)
        {
            CallbackTarget.Free();
        }
        return true;
    }
}

/// <summary>
/// A native cancellation token tied to a CancellationToken for the duration of one call:
/// cancelling the .NET token calls siaara_cancel_set (from the cancelling thread).
/// Dispose unregisters first (waiting for a running set), then frees the token.
/// </summary>
internal sealed class NativeCancellation : IDisposable
{
    private readonly CancelHandle _handle = new();
    private readonly CancellationTokenRegistration _registration;

    internal NativeCancellation(CancellationToken token)
    {
        int st = Native.siaara_cancel_create(out nint p);
        if (st != C.SIAARA_OK)
        {
            _handle.Dispose();
            throw new InvalidOperationException("siaara_cancel_create failed: " + st);
        }
        _handle.Attach(p);
        if (token.CanBeCanceled)
        {
            _registration = token.Register(static s => ((NativeCancellation)s!).Set(), this);
        }
    }

    /// <summary>The raw token; valid until Dispose.</summary>
    internal nint Pointer => _handle.DangerousGetHandle();

    private void Set()
    {
        if (!_handle.IsClosed)
        {
            _ = Native.siaara_cancel_set(_handle.DangerousGetHandle());
        }
    }

    public void Dispose()
    {
        _registration.Dispose();
        _handle.Dispose();
    }
}

/// <summary>
/// Keeps a SafeHandle alive for the duration of a native call (DangerousAddRef / DangerousRelease).
/// Throws ObjectDisposedException when the handle was already disposed.
/// </summary>
internal ref struct HandleLease
{
    private readonly SafeHandle? _handle;
    private bool _added;

    internal HandleLease(SafeHandle handle)
    {
        _handle = handle;
        _added = false;
        handle.DangerousAddRef(ref _added);
        Pointer = handle.DangerousGetHandle();
    }

    /// <summary>The raw pointer, valid until Dispose.</summary>
    internal nint Pointer { get; }

    public void Dispose()
    {
        if (_added)
        {
            _added = false;
            _handle!.DangerousRelease();
        }
    }
}
