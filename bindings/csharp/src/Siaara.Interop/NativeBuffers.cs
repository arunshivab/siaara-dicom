using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Siaara.Interop;

/// <summary>
/// Native memory for the pointer fields of option structs (NUL-terminated UTF-8 strings and
/// arrays). Everything is zeroed before it is freed (passwords, keys). Not thread safe.
/// </summary>
internal sealed unsafe class NativeAllocations : IDisposable
{
    private readonly List<(nint Pointer, nuint Size)> _blocks = new();

    /// <summary>Copies s as NUL-terminated UTF-8; null gives 0 (NULL).</summary>
    internal nint Utf8(string? s)
    {
        if (s is null)
        {
            return 0;
        }
        int n = Encoding.UTF8.GetByteCount(s);
        nint p = Allocate((nuint)n + 1);
        var span = new Span<byte>((void*)p, n + 1);
        Encoding.UTF8.GetBytes(s, span);
        span[n] = 0;
        return p;
    }

    /// <summary>Copies an array of unmanaged values; an empty span gives 0 (NULL).</summary>
    internal nint Array<T>(ReadOnlySpan<T> values) where T : unmanaged
    {
        if (values.IsEmpty)
        {
            return 0;
        }
        nuint size = (nuint)values.Length * (nuint)sizeof(T);
        nint p = Allocate(size);
        values.CopyTo(new Span<T>((void*)p, values.Length));
        return p;
    }

    private nint Allocate(nuint size)
    {
        nint p = (nint)NativeMemory.Alloc(size == 0 ? 1 : size);
        _blocks.Add((p, size));
        return p;
    }

    public void Dispose()
    {
        foreach ((nint p, nuint size) in _blocks)
        {
            NativeMemory.Clear((void*)p, size);
            NativeMemory.Free((void*)p);
        }
        _blocks.Clear();
    }
}

/// <summary>Spans over native memory the C API lends out.</summary>
internal static unsafe class NativeSpan
{
    /// <summary>A read-only view of length bytes at pointer (empty for NULL or length 0).</summary>
    internal static ReadOnlySpan<byte> Bytes(nint pointer, nuint length)
    {
        if (pointer == 0 || length == 0)
        {
            return default;
        }
        if (length > int.MaxValue)
        {
            throw new OverflowException("The native buffer is larger than a span can hold.");
        }
        return new ReadOnlySpan<byte>((void*)pointer, (int)length);
    }
}

/// <summary>
/// Memory over a native buffer borrowed from a handle. The handle is kept alive (DangerousAddRef)
/// until this manager is disposed, so the memory cannot be freed while it is in use.
/// </summary>
internal sealed unsafe class BorrowedMemoryManager<T> : MemoryManager<T> where T : unmanaged
{
    private readonly SafeHandle _owner;
    private nint _pointer;
    private readonly int _length;
    private bool _added;

    internal BorrowedMemoryManager(SafeHandle owner, nint pointer, int length)
    {
        _owner = owner;
        owner.DangerousAddRef(ref _added);
        _pointer = pointer;
        _length = length;
    }

    public override Span<T> GetSpan()
    {
        ObjectDisposedException.ThrowIf(_pointer == 0 && _length != 0, this);
        return _length == 0 ? default : new Span<T>((void*)_pointer, _length);
    }

    public override MemoryHandle Pin(int elementIndex = 0)
    {
        ObjectDisposedException.ThrowIf(_pointer == 0 && _length != 0, this);
        ArgumentOutOfRangeException.ThrowIfNegative(elementIndex);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(elementIndex, _length);
        return new MemoryHandle((T*)_pointer + elementIndex);
    }

    public override void Unpin()
    {
    }

    protected override void Dispose(bool disposing)
    {
        _pointer = 0;
        if (_added)
        {
            _added = false;
            _owner.DangerousRelease();
        }
    }
}
