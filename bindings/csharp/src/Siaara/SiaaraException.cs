using System;
using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>Status codes of the SIAARA C API (siaara_status).</summary>
public enum SiaaraErrorCode
{
    /// <summary>Success.</summary>
    Ok = 0,
    /// <summary>An argument was invalid.</summary>
    InvalidArgument = 1,
    /// <summary>An I/O error occurred.</summary>
    IO = 2,
    /// <summary>The input is not DICOM.</summary>
    NotDicom = 3,
    /// <summary>The input ended early.</summary>
    Truncated = 4,
    /// <summary>The input is malformed.</summary>
    Malformed = 5,
    /// <summary>The operation or encoding is not supported.</summary>
    Unsupported = 6,
    /// <summary>The element, item or object was not found.</summary>
    NotFound = 7,
    /// <summary>The value cannot be read as the requested type.</summary>
    TypeMismatch = 8,
    /// <summary>A configured limit was exceeded.</summary>
    LimitExceeded = 9,
    /// <summary>Out of memory.</summary>
    OutOfMemory = 10,
    /// <summary>A network error occurred.</summary>
    Network = 11,
    /// <summary>A network wait timed out.</summary>
    Timeout = 12,
    /// <summary>The peer or the policy rejected the request.</summary>
    Rejected = 13,
    /// <summary>The operation was cancelled (for example by a callback).</summary>
    Cancelled = 14,
    /// <summary>The output buffer is too small.</summary>
    BufferTooSmall = 15,
    /// <summary>Internal error.</summary>
    Internal = 99,
}

/// <summary>A SIAARA C API call failed.</summary>
public sealed class SiaaraException : Exception
{
    /// <summary>Creates an exception with a generic message.</summary>
    public SiaaraException() : this(SiaaraErrorCode.Internal, string.Empty)
    {
    }

    /// <summary>Creates an exception with a message.</summary>
    /// <param name="message">The message.</param>
    public SiaaraException(string message) : this(SiaaraErrorCode.Internal, message)
    {
    }

    /// <summary>Creates an exception with a message and an inner exception.</summary>
    /// <param name="message">The message.</param>
    /// <param name="innerException">The cause.</param>
    public SiaaraException(string message, Exception innerException) : base(message, innerException)
    {
        ErrorCode = SiaaraErrorCode.Internal;
        NativeMessage = message;
    }

    /// <summary>Creates an exception for a status code and the library's message.</summary>
    /// <param name="errorCode">The status code.</param>
    /// <param name="nativeMessage">siaara_last_error_message() of the failed call.</param>
    public SiaaraException(SiaaraErrorCode errorCode, string nativeMessage)
        : base(Format(errorCode, nativeMessage))
    {
        ErrorCode = errorCode;
        NativeMessage = nativeMessage;
    }

    /// <summary>The status code returned by the C API.</summary>
    public SiaaraErrorCode ErrorCode { get; }

    /// <summary>The message of the failed call (siaara_last_error_message), possibly empty.</summary>
    public string NativeMessage { get; }

    private static string Format(SiaaraErrorCode code, string message) =>
        string.IsNullOrEmpty(message) ? code.ToString() : code + ": " + message;
}

/// <summary>Status checks and string helpers shared by the wrappers.</summary>
internal static class Status
{
    internal const int Ok = C.SIAARA_OK;
    internal const int BufferTooSmall = C.SIAARA_ERR_BUFFER_TOO_SMALL;

    /// <summary>The last error message on this thread.</summary>
    internal static string LastMessage() => Marshal.PtrToStringUTF8(Native.siaara_last_error_message()) ?? string.Empty;

    /// <summary>Builds the exception for a failed status; reads the message at once.</summary>
    internal static SiaaraException Error(int status) => new((SiaaraErrorCode)status, LastMessage());

    /// <summary>Throws SiaaraException unless status is SIAARA_OK.</summary>
    internal static void Check(int status)
    {
        if (status != Ok)
        {
            throw Error(status);
        }
    }

    /// <summary>A UTF-8 string argument with a NUL terminator (for NUL-terminated C parameters).</summary>
    internal static byte[]? Utf8Z(string? s)
    {
        if (s is null)
        {
            return null;
        }
        byte[] b = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, b);
        return b;
    }

    /// <summary>UTF-8 bytes without a terminator (for pointer + length C parameters).</summary>
    internal static byte[] Utf8(string s) => Encoding.UTF8.GetBytes(s);

    /// <summary>
    /// Reads a string with the C buffer convention (buf/cap/needed): first into a stack buffer,
    /// then, if that is too small, into a pooled buffer of the reported size.
    /// </summary>
    internal static int TryReadString<TState>(TState state, StringGetter<TState> getter, out string value)
        where TState : allows ref struct
    {
        Span<byte> stack = stackalloc byte[256];
        int st = getter(state, stack, out nuint needed);
        if (st == Ok)
        {
            value = Encoding.UTF8.GetString(stack[..(int)needed]);
            return st;
        }
        if (st != BufferTooSmall)
        {
            value = string.Empty;
            return st;
        }
        if (needed >= int.MaxValue)
        {
            throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "string too large");
        }
        byte[] rented = ArrayPool<byte>.Shared.Rent((int)needed + 1);
        try
        {
            st = getter(state, rented.AsSpan(0, (int)needed + 1), out needed);
            value = st == Ok ? Encoding.UTF8.GetString(rented, 0, (int)needed) : string.Empty;
            return st;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }

    /// <summary>As TryReadString, throwing SiaaraException on failure.</summary>
    internal static string ReadString<TState>(TState state, StringGetter<TState> getter)
        where TState : allows ref struct
    {
        int st = TryReadString(state, getter, out string value);
        Check(st);
        return value;
    }

    /// <summary>Reads an array with the C buffer convention (size query, then fill).</summary>
    internal static T[] ReadArray<T, TState>(TState state, ArrayGetter<T, TState> getter)
        where T : unmanaged
        where TState : allows ref struct
    {
        int st = getter(state, default, out nuint needed);
        if (st != Ok && st != BufferTooSmall)
        {
            throw Error(st);
        }
        if (needed == 0)
        {
            return Array.Empty<T>();
        }
        if (needed > int.MaxValue)
        {
            throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "array too large");
        }
        var result = new T[(int)needed];
        Check(getter(state, result, out _));
        return result;
    }
}

/// <summary>A C string getter: fills buffer, reports the full length in needed.</summary>
internal delegate int StringGetter<TState>(TState state, Span<byte> buffer, out nuint needed)
    where TState : allows ref struct;

/// <summary>A C array getter: fills buffer, reports the element count in needed.</summary>
internal delegate int ArrayGetter<T, TState>(TState state, Span<T> buffer, out nuint needed)
    where TState : allows ref struct;
