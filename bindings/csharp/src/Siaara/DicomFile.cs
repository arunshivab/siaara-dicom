using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>Options for parsing a Part 10 file (siaara_read_options).</summary>
public sealed record DicomReadOptions
{
    /// <summary>Stop parsing before Pixel Data (headers only).</summary>
    public bool StopBeforePixelData { get; init; }

    /// <summary>Accept a raw data set without preamble and file meta (default true).</summary>
    public bool AllowRawDataset { get; init; } = true;

    /// <summary>Strict parsing (default lenient).</summary>
    public bool Strict { get; init; }

    /// <summary>Maximum sequence nesting depth; 0 = library default (64).</summary>
    public int MaxDepth { get; init; }

    internal siaara_read_options ToNative() => new()
    {
        struct_size = StructSize.Of<siaara_read_options>(),
        stop_before_pixel_data = StopBeforePixelData ? 1 : 0,
        allow_raw_dataset = AllowRawDataset ? 1 : 0,
        strict = Strict ? 1 : 0,
        max_depth = (uint)Math.Max(0, MaxDepth),
    };
}

/// <summary>What a read left out (siaara_read_report.loss bits).</summary>
[Flags]
public enum DicomReadLoss
{
    /// <summary>The data set holds everything the input held.</summary>
    None = 0,
    /// <summary>Lenient parsing skipped input bytes that are in no element (warning code PARSE_INCOMPLETE).</summary>
    BytesSkipped = 1,
    /// <summary>The input ended inside an element, item or sequence (warning code INPUT_TRUNCATED).</summary>
    InputTruncated = 2,
    /// <summary>Header-only read (StopBeforePixelData): Pixel Data and what follows were not read.</summary>
    HeaderOnly = 4,
}

/// <summary>Code of a file parse warning (siaara_file_warning_code).</summary>
public enum DicomFileWarningCode
{
    /// <summary>A lenient-mode finding.</summary>
    General = 0,
    /// <summary>Input bytes were skipped (PARSE_INCOMPLETE).</summary>
    ParseIncomplete = 1,
    /// <summary>The input ended early (INPUT_TRUNCATED).</summary>
    InputTruncated = 2,
}

/// <summary>Whether a read was complete (siaara_read_report).</summary>
public sealed record DicomReadReport
{
    /// <summary>What was left out; None when the read was complete.</summary>
    public DicomReadLoss Loss { get; init; }
    /// <summary>True when nothing was left out: only then does <see cref="DicomFile.Save(string, string?)"/> succeed by default.</summary>
    public bool IsComplete => Loss == DicomReadLoss.None;
    /// <summary>Offset of the first loss.</summary>
    public long FirstLossOffset { get; init; }
    /// <summary>Input bytes that are in no element.</summary>
    public long BytesNotRead { get; init; }
    /// <summary>Header-only read: offset of the Pixel Data element, else 0.</summary>
    public long StoppedAtOffset { get; init; }
    /// <summary>Number of parse warnings.</summary>
    public int WarningCount { get; init; }
}

/// <summary>
/// A parsed DICOM Part 10 file (siaara_file). Owns its data sets. Thread safety follows the C API:
/// many threads may read one file at once; changes need exclusive access.
/// </summary>
public sealed partial class DicomFile : IDisposable
{
    private readonly FileHandle _handle;
    private DicomDataset? _dataset;
    private DicomDataset? _meta;

    private DicomFile(FileHandle handle) => _handle = handle;

    internal FileHandle Handle => _handle;

    /// <summary>True after Dispose.</summary>
    public bool IsDisposed => _handle.IsClosed;

    /// <summary>Takes ownership of a pointer produced by the C API.</summary>
    internal static DicomFile FromOwnedPointer(nint pointer)
    {
        var h = new FileHandle();
        h.Attach(pointer);
        return new DicomFile(h);
    }

    /// <summary>Wraps a *_open / *_create call that fills an out siaara_file*.</summary>
    internal static DicomFile CreateWith<TState>(TState state, Func<TState, (int Status, nint Pointer)> open)
    {
        (int st, nint p) = open(state);
        if (st != Status.Ok)
        {
            SiaaraException e = Status.Error(st);
            if (p != 0)
            {
                Raw.siaara_file_free(p);
            }
            throw e;
        }
        return FromOwnedPointer(p);
    }

    /// <summary>Opens and parses a file (memory-mapped).</summary>
    /// <param name="path">File path.</param>
    /// <param name="options">Parse options, or null for the defaults.</param>
    /// <returns>The file.</returns>
    /// <exception cref="SiaaraException">The file cannot be read or parsed.</exception>
    public static DicomFile Open(string path, DicomReadOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        byte[] p = Status.Utf8(path);
        return CreateWith((p, options), static s =>
        {
            int st;
            nint f;
            if (s.options is null)
            {
                st = Native.siaara_file_open(s.p, in Unsafe.NullRef<siaara_read_options>(), out f);
            }
            else
            {
                siaara_read_options o = s.options.ToNative();
                st = Native.siaara_file_open(s.p, in o, out f);
            }
            return (st, f);
        });
    }

    /// <summary>Parses a Part 10 stream (or raw data set) from memory. The bytes are copied.</summary>
    /// <param name="data">The encoded file.</param>
    /// <param name="options">Parse options, or null for the defaults.</param>
    /// <returns>The file.</returns>
    /// <exception cref="SiaaraException">The data cannot be parsed.</exception>
    public static DicomFile Open(ReadOnlySpan<byte> data, DicomReadOptions? options = null)
    {
        int st;
        nint f;
        if (options is null)
        {
            st = Native.siaara_file_open_memory(data, in Unsafe.NullRef<siaara_read_options>(), out f);
        }
        else
        {
            siaara_read_options o = options.ToNative();
            st = Native.siaara_file_open_memory(data, in o, out f);
        }
        Status.Check(st);
        return FromOwnedPointer(f);
    }

    /// <summary>Creates an empty file (Explicit VR Little Endian).</summary>
    /// <returns>The file.</returns>
    public static DicomFile Create()
    {
        Status.Check(Native.siaara_file_create(out nint f));
        return FromOwnedPointer(f);
    }

    /// <summary>Parses one DICOM JSON object (PS3.18 Annex F) into a new file. BulkDataURI values stay empty.</summary>
    /// <param name="json">The JSON text.</param>
    /// <returns>The file.</returns>
    /// <exception cref="SiaaraException">The JSON is not a valid DICOM JSON object.</exception>
    public static DicomFile FromJson(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        return FromJson(Encoding.UTF8.GetBytes(json));
    }

    /// <summary>Parses one DICOM JSON object given as UTF-8 bytes.</summary>
    /// <param name="utf8Json">The UTF-8 JSON.</param>
    /// <returns>The file.</returns>
    public static DicomFile FromJson(ReadOnlySpan<byte> utf8Json)
    {
        Status.Check(Native.siaara_dataset_from_json(utf8Json, out nint f));
        return FromOwnedPointer(f);
    }

    /// <summary>The main data set (borrowed from this file).</summary>
    public DicomDataset Dataset => _dataset ??= new DicomDataset(this, DicomDataset.RootKind.Main);

    /// <summary>The File Meta Information group (0002,xxxx).</summary>
    public DicomDataset FileMetaInformation => _meta ??= new DicomDataset(this, DicomDataset.RootKind.Meta);

    /// <summary>The Transfer Syntax UID of the file.</summary>
    public string TransferSyntaxUid
    {
        get
        {
            using var l = new HandleLease(_handle);
            return Status.ReadString(l.Pointer, static (nint f, Span<byte> b, out nuint n) =>
                Native.siaara_file_transfer_syntax(f, b, out n));
        }
    }

    /// <summary>Number of warnings recorded while parsing (lenient mode).</summary>
    public int WarningCount
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_file_warning_count(l.Pointer, out uint n));
            return (int)n;
        }
    }

    /// <summary>Parse warnings as text. When the read was not complete the first one starts with
    /// PARSE_INCOMPLETE or INPUT_TRUNCATED.</summary>
    public IReadOnlyList<string> Warnings
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_file_warning_count(l.Pointer, out uint n));
            var list = new string[n];
            for (int i = 0; i < list.Length; i++)
            {
                list[i] = Status.ReadString((F: l.Pointer, I: (uint)i), static (s, b, out nn) => Native.siaara_file_warning(s.F, s.I, b, out nn));
            }
            return list;
        }
    }

    /// <summary>Code of each parse warning, in the order of <see cref="Warnings"/>.</summary>
    public IReadOnlyList<DicomFileWarningCode> WarningCodes
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_file_warning_count(l.Pointer, out uint n));
            var list = new DicomFileWarningCode[n];
            for (int i = 0; i < list.Length; i++)
            {
                Status.Check(Native.siaara_file_warning_code(l.Pointer, (uint)i, out int c));
                list[i] = (DicomFileWarningCode)c;
            }
            return list;
        }
    }

    /// <summary>Whether the read was complete (siaara_file_read_report).</summary>
    public DicomReadReport ReadReport
    {
        get
        {
            using var l = new HandleLease(_handle);
            var r = new siaara_read_report { struct_size = StructSize.Of<siaara_read_report>() };
            Status.Check(Native.siaara_file_read_report(l.Pointer, ref r));
            return new DicomReadReport
            {
                Loss = (DicomReadLoss)r.loss,
                FirstLossOffset = (long)r.first_loss_offset,
                BytesNotRead = (long)r.bytes_not_read,
                StoppedAtOffset = (long)r.stopped_at_offset,
                WarningCount = (int)r.warning_count,
            };
        }
    }

    /// <summary>Writes the file. Changing the transfer syntax here never applies lossy compression.
    /// A file whose read was not complete (<see cref="ReadReport"/>) is refused, so a save never
    /// silently writes less than was read; use <see cref="Save(string, string?, bool)"/> to write it anyway.</summary>
    /// <param name="path">Destination path.</param>
    /// <param name="transferSyntaxUid">Transfer syntax to write, or null to keep the current one.</param>
    /// <exception cref="SiaaraException">The file cannot be written, or the read was not complete (Rejected).</exception>
    public void Save(string path, string? transferSyntaxUid = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_file_save(l.Pointer, Status.Utf8(path), Status.Utf8Z(transferSyntaxUid)));
    }

    /// <summary>Writes the file; <paramref name="allowIncomplete"/> also writes a file whose read was not complete.</summary>
    /// <param name="path">Destination path.</param>
    /// <param name="transferSyntaxUid">Transfer syntax to write, or null to keep the current one.</param>
    /// <param name="allowIncomplete">Write even when <see cref="ReadReport"/> is not complete (the result then holds less than the input).</param>
    /// <exception cref="SiaaraException">The file cannot be written.</exception>
    public void Save(string path, string? transferSyntaxUid, bool allowIncomplete)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        using var l = new HandleLease(_handle);
        var o = new siaara_save_options
        {
            struct_size = StructSize.Of<siaara_save_options>(),
            allow_incomplete = allowIncomplete ? 1 : 0,
        };
        Status.Check(Native.siaara_file_save_ex(l.Pointer, Status.Utf8(path), Status.Utf8Z(transferSyntaxUid), in o));
    }

    // ------------------------------------------------------------------ pixel data

    /// <summary>The validated Image Pixel module of the main data set.</summary>
    /// <returns>The pixel description.</returns>
    /// <exception cref="SiaaraException">No or invalid pixel data.</exception>
    public DicomPixelInfo GetPixelInfo()
    {
        int st = TryGetPixelInfoCore(out DicomPixelInfo? info);
        Status.Check(st);
        return info!;
    }

    /// <summary>The pixel description, without throwing.</summary>
    /// <param name="info">The pixel description, when the method returns true.</param>
    /// <returns>True when the file has valid pixel data.</returns>
    public bool TryGetPixelInfo([System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out DicomPixelInfo? info) =>
        TryGetPixelInfoCore(out info) == Status.Ok;

    private int TryGetPixelInfoCore(out DicomPixelInfo? info)
    {
        using var l = new HandleLease(_handle);
        var pi = new siaara_pixel_info { struct_size = StructSize.Of<siaara_pixel_info>() };
        int st = Native.siaara_file_pixel_info(l.Pointer, ref pi);
        info = st == Status.Ok ? DicomPixelInfo.FromNative(pi) : null;
        return st;
    }

    /// <summary>Number of frames of the pixel data.</summary>
    public int FrameCount
    {
        get
        {
            using var l = new HandleLease(_handle);
            Status.Check(Native.siaara_frame_count(l.Pointer, out uint n));
            return (int)n;
        }
    }

    /// <summary>Bytes needed for one decoded frame in a format.</summary>
    /// <param name="frame">0-based frame.</param>
    /// <param name="format">Output format.</param>
    /// <returns>The size in bytes.</returns>
    public int GetFrameSize(int frame, FrameFormat format = FrameFormat.Native)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_frame_decode(l.Pointer, (uint)frame, (int)format, default, out nuint needed));
        return ToInt(needed);
    }

    /// <summary>Decodes a frame into destination (which must be at least GetFrameSize bytes).</summary>
    /// <param name="frame">0-based frame.</param>
    /// <param name="destination">Output buffer.</param>
    /// <param name="format">Output format.</param>
    /// <returns>Bytes written.</returns>
    /// <exception cref="SiaaraException">Decoding failed or the buffer is too small.</exception>
    public int DecodeFrame(int frame, Span<byte> destination, FrameFormat format = FrameFormat.Native)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        using var l = new HandleLease(_handle);
        if (destination.IsEmpty)
        {
            // An empty span would be a size query; ask for the size so the error is BufferTooSmall.
            Status.Check(Native.siaara_frame_decode(l.Pointer, (uint)frame, (int)format, default, out nuint n));
            if (n != 0)
            {
                throw new SiaaraException(SiaaraErrorCode.BufferTooSmall, "buffer too small");
            }
            return 0;
        }
        Status.Check(Native.siaara_frame_decode(l.Pointer, (uint)frame, (int)format, destination, out nuint needed));
        return ToInt(needed);
    }

    /// <summary>Decodes a frame without throwing (hot path).</summary>
    /// <param name="frame">0-based frame.</param>
    /// <param name="destination">Output buffer.</param>
    /// <param name="format">Output format.</param>
    /// <param name="bytesWritten">Bytes written, or the bytes needed when the buffer is too small.</param>
    /// <returns>The status: Ok, BufferTooSmall or the failure.</returns>
    public SiaaraErrorCode TryDecodeFrame(int frame, Span<byte> destination, FrameFormat format, out int bytesWritten)
    {
        bytesWritten = 0;
        if (frame < 0 || destination.IsEmpty)
        {
            return frame < 0 ? SiaaraErrorCode.InvalidArgument : SiaaraErrorCode.BufferTooSmall;
        }
        using var l = new HandleLease(_handle);
        int st = Native.siaara_frame_decode(l.Pointer, (uint)frame, (int)format, destination, out nuint needed);
        bytesWritten = needed > int.MaxValue ? int.MaxValue : (int)needed;
        return (SiaaraErrorCode)st;
    }

    /// <summary>Decodes a frame into a new array.</summary>
    /// <param name="frame">0-based frame.</param>
    /// <param name="format">Output format.</param>
    /// <returns>The frame bytes.</returns>
    public byte[] DecodeFrame(int frame, FrameFormat format = FrameFormat.Native)
    {
        byte[] buf = new byte[GetFrameSize(frame, format)];
        if (buf.Length != 0)
        {
            DecodeFrame(frame, buf, format);
        }
        return buf;
    }

    /// <summary>Decodes a frame into a buffer rented from ArrayPool; dispose it to return the buffer.</summary>
    /// <param name="frame">0-based frame.</param>
    /// <param name="format">Output format.</param>
    /// <returns>The pooled frame.</returns>
    public PooledBuffer DecodeFramePooled(int frame, FrameFormat format = FrameFormat.Native)
    {
        var buf = new PooledBuffer(GetFrameSize(frame, format));
        try
        {
            if (buf.Length != 0)
            {
                DecodeFrame(frame, buf.Span, format);
            }
            return buf;
        }
        catch
        {
            buf.Dispose();
            throw;
        }
    }

    /// <summary>Modality values (after Modality LUT / rescale) of a frame as float32, colour-by-pixel.</summary>
    /// <param name="frame">0-based frame.</param>
    /// <returns>rows x columns x samples values.</returns>
    public float[] DecodeFrameModality(int frame)
    {
        int bytes = GetFrameSize(frame, FrameFormat.Float32Modality);
        float[] values = new float[bytes / sizeof(float)];
        if (values.Length != 0)
        {
            DecodeFrame(frame, MemoryMarshal.AsBytes(values.AsSpan()), FrameFormat.Float32Modality);
        }
        return values;
    }

    /// <summary>Decodes a frame once for repeated rendering (see <see cref="DicomFrame"/>).</summary>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The decoded frame; dispose it.</returns>
    public DicomFrame OpenFrame(int frame = 0) => DicomFrame.Open(this, frame);

    /// <summary>
    /// Zero-copy view of a frame of native (uncompressed, little-endian, not 1-bit) pixel data,
    /// straight from the file's Pixel Data value (siaara_file_frame_view). The file stays alive
    /// until the returned owner is disposed; do not change the Pixel Data meanwhile.
    /// </summary>
    /// <param name="frame">0-based frame.</param>
    /// <returns>The frame bytes.</returns>
    /// <exception cref="SiaaraException">Unsupported when the frame must be decoded.</exception>
    public System.Buffers.IMemoryOwner<byte> LeaseNativeFrame(int frame = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        nint data;
        ulong bytes;
        using (var l = new HandleLease(_handle))
        {
            Status.Check(Native.siaara_file_frame_view(l.Pointer, (uint)frame, out data, out bytes));
        }
        if (bytes > int.MaxValue)
        {
            throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "frame larger than a span can address");
        }
        return new BorrowedMemoryManager<byte>(_handle, data, (int)bytes);
    }

    /// <summary>Bytes of an RGBA8 rendering (rows x columns x 4).</summary>
    /// <param name="options">Render options, or null (frame 0, the file's VOI).</param>
    /// <returns>The size in bytes.</returns>
    public int GetRenderSize(RenderOptions? options = null)
    {
        using var l = new HandleLease(_handle);
        return ToInt(RenderCore(l.Pointer, options, default, throwOnError: true, out _));
    }

    /// <summary>Renders a frame to RGBA8 (presentation pipeline: modality, VOI, presentation LUT, overlays, shutters).</summary>
    /// <param name="destination">At least GetRenderSize bytes.</param>
    /// <param name="options">Render options, or null.</param>
    /// <returns>Bytes written.</returns>
    public int RenderRgba8(Span<byte> destination, RenderOptions? options)
    {
        if (destination.IsEmpty)
        {
            throw new ArgumentException("destination must not be empty", nameof(destination));
        }
        using var l = new HandleLease(_handle);
        return ToInt(RenderCore(l.Pointer, options, destination, throwOnError: true, out _));
    }

    /// <summary>Renders without throwing (hot path).</summary>
    /// <param name="destination">Output buffer.</param>
    /// <param name="options">Render options, or null.</param>
    /// <param name="bytesWritten">Bytes written, or the bytes needed when the buffer is too small.</param>
    /// <returns>The status.</returns>
    public SiaaraErrorCode TryRenderRgba8(Span<byte> destination, RenderOptions? options, out int bytesWritten)
    {
        bytesWritten = 0;
        if (destination.IsEmpty)
        {
            return SiaaraErrorCode.BufferTooSmall;
        }
        using var l = new HandleLease(_handle);
        nuint n = RenderCore(l.Pointer, options, destination, throwOnError: false, out int st);
        bytesWritten = n > int.MaxValue ? int.MaxValue : (int)n;
        return (SiaaraErrorCode)st;
    }

    /// <summary>Renders a frame to a new RGBA8 array.</summary>
    /// <param name="options">Render options, or null.</param>
    /// <returns>rows x columns x 4 bytes.</returns>
    public byte[] RenderRgba8(RenderOptions? options = null)
    {
        byte[] buf = new byte[GetRenderSize(options)];
        RenderRgba8(buf, options);
        return buf;
    }

    /// <summary>Renders a frame into a pooled buffer; dispose it to return the buffer.</summary>
    /// <param name="options">Render options, or null.</param>
    /// <returns>The pooled RGBA8 image.</returns>
    public PooledBuffer RenderRgba8Pooled(RenderOptions? options = null)
    {
        var buf = new PooledBuffer(GetRenderSize(options));
        try
        {
            RenderRgba8(buf.Span, options);
            return buf;
        }
        catch
        {
            buf.Dispose();
            throw;
        }
    }

    private static nuint RenderCore(nint file, RenderOptions? options, Span<byte> destination, bool throwOnError, out int status)
    {
        nuint needed;
        if (options is null)
        {
            status = Native.siaara_render_rgba8(file, in Unsafe.NullRef<siaara_render_options>(), destination, out needed);
        }
        else
        {
            siaara_render_options o = options.ToNative();
            status = Native.siaara_render_rgba8(file, in o, destination, out needed);
        }
        if (throwOnError)
        {
            Status.Check(status);
        }
        return needed;
    }

    // ------------------------------------------------------------------ transcoding

    /// <summary>Converts the pixel data to another transfer syntax in place (see <see cref="Transcoder"/>).</summary>
    /// <param name="transferSyntaxUid">Target transfer syntax.</param>
    /// <param name="parameters">Codec parameters, or null for the library defaults.</param>
    public void Transcode(string transferSyntaxUid, TranscodeParameters? parameters = null) =>
        Transcoder.Transcode(this, transferSyntaxUid, parameters);

    internal static int ToInt(nuint n) =>
        n > int.MaxValue ? throw new SiaaraException(SiaaraErrorCode.LimitExceeded, "result larger than 2 GiB") : (int)n;

    /// <summary>Frees the native file. Data sets borrowed from it become unusable.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}

/// <summary>A byte buffer rented from <see cref="ArrayPool{T}.Shared"/>; Dispose returns it.</summary>
public sealed class PooledBuffer : IMemoryOwner<byte>
{
    private byte[]? _array;

    internal PooledBuffer(int length)
    {
        Length = length;
        _array = ArrayPool<byte>.Shared.Rent(Math.Max(1, length));
    }

    /// <summary>Number of valid bytes.</summary>
    public int Length { get; }

    /// <summary>The valid bytes.</summary>
    public Memory<byte> Memory => Array().AsMemory(0, Length);

    /// <summary>The valid bytes as a span.</summary>
    public Span<byte> Span => Array().AsSpan(0, Length);

    private byte[] Array()
    {
        ObjectDisposedException.ThrowIf(_array is null, this);
        return _array;
    }

    /// <summary>Returns the buffer to the pool.</summary>
    public void Dispose()
    {
        byte[]? a = _array;
        _array = null;
        if (a is not null)
        {
            ArrayPool<byte>.Shared.Return(a);
        }
        GC.SuppressFinalize(this);
    }
}
