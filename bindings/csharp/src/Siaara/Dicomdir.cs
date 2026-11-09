using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;

namespace Siaara;

/// <summary>DICOMDIR read options (siaara_dicomdir_read_options).</summary>
public sealed record DicomdirReadOptions
{
    /// <summary>Also report inactive records.</summary>
    public bool IncludeInactive { get; init; }
    /// <summary>Resolve referenced files (default true).</summary>
    public bool ResolveFiles { get; init; } = true;
    /// <summary>Case-insensitive file resolution (default true).</summary>
    public bool CaseInsensitive { get; init; } = true;
    /// <summary>Maximum record depth; 0 = default (16).</summary>
    public int MaxDepth { get; init; }
}

/// <summary>One directory record with its own and inherited keys.</summary>
/// <param name="Index">Position in the Directory Record Sequence.</param>
/// <param name="Parent">Parent record index, -1 at the root.</param>
/// <param name="Depth">Depth.</param>
/// <param name="InUse">Record In-use flag.</param>
/// <param name="FileStatus">Ok, Rejected (unsafe File ID) or NotFound.</param>
/// <param name="Offset">File offset of the record.</param>
/// <param name="Keys">Keys (RecordType, FilePath = resolved absolute path).</param>
public sealed record DicomdirRecord(int Index, int Parent, int Depth, bool InUse, SiaaraErrorCode FileStatus, long Offset, DicomIndexRecord Keys);

/// <summary>Totals of a DICOMDIR read (siaara_dicomdir_summary).</summary>
/// <param name="Records">Records in the DICOMDIR.</param>
/// <param name="Reported">Records reported.</param>
/// <param name="Files">Referenced files.</param>
/// <param name="MissingFiles">Referenced files not found.</param>
/// <param name="RejectedPaths">Unsafe File IDs.</param>
/// <param name="Unreachable">Records not reachable from the root.</param>
/// <param name="SequentialFallback">Offsets were broken; records were read in sequence order.</param>
/// <param name="Warnings">Warnings.</param>
public sealed record DicomdirSummary(int Records, int Reported, int Files, int MissingFiles, int RejectedPaths, int Unreachable,
    bool SequentialFallback, int Warnings);

/// <summary>Media application profile for DICOMDIR writing.</summary>
public enum DicomdirProfile
{
    /// <summary>STD-GEN-CD.</summary>
    GeneralCD = C.SIAARA_PROFILE_GENERAL_CD,
    /// <summary>STD-GEN-USB.</summary>
    GeneralUsb = C.SIAARA_PROFILE_GENERAL_USB,
}

/// <summary>DICOMDIR write options (siaara_dicomdir_write_options).</summary>
public sealed record DicomdirWriteOptions
{
    /// <summary>Profile.</summary>
    public DicomdirProfile Profile { get; init; }
    /// <summary>Overwrite an existing DICOMDIR.</summary>
    public bool Overwrite { get; init; }
    /// <summary>Allow any transfer syntax.</summary>
    public bool AllowAnyTransferSyntax { get; init; }
    /// <summary>File-set ID (up to 16 of A-Z 0-9 _ space), or null.</summary>
    public string? FileSetId { get; init; }
}

/// <summary>DICOMDIR (PS3.10) reading and writing.</summary>
public static class Dicomdir
{
    /// <summary>Reads a DICOMDIR, calling onRecord per record.</summary>
    /// <param name="path">Path of the DICOMDIR.</param>
    /// <param name="onRecord">Called per record.</param>
    /// <param name="options">Options, or null.</param>
    /// <param name="cancellationToken">Stops reading (OperationCanceledException).</param>
    /// <returns>Totals.</returns>
    public static DicomdirSummary Read(string path, Action<DicomdirRecord> onRecord, DicomdirReadOptions? options = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        ArgumentNullException.ThrowIfNull(onRecord);
        var sink = new Sink(onRecord, cancellationToken);
        GCHandle gc = GCHandle.Alloc(sink);
        var summary = new siaara_dicomdir_summary { struct_size = StructSize.Of<siaara_dicomdir_summary>() };
        int st;
        try
        {
            byte[] p = Status.Utf8(path);
            if (options is null)
            {
                st = Native.siaara_dicomdir_read(p, in Unsafe.NullRef<siaara_dicomdir_read_options>(), Callbacks.DicomdirPointer,
                    GCHandle.ToIntPtr(gc), ref summary);
            }
            else
            {
                var o = new siaara_dicomdir_read_options
                {
                    struct_size = StructSize.Of<siaara_dicomdir_read_options>(),
                    include_inactive = options.IncludeInactive ? 1 : 0,
                    resolve_files = options.ResolveFiles ? 1 : 0,
                    case_insensitive = options.CaseInsensitive ? 1 : 0,
                    max_depth = (uint)Math.Max(0, options.MaxDepth),
                };
                st = Native.siaara_dicomdir_read(p, in o, Callbacks.DicomdirPointer, GCHandle.ToIntPtr(gc), ref summary);
            }
        }
        finally
        {
            gc.Free();
        }
        if (sink.Error is not null)
        {
            ExceptionDispatchInfo.Capture(sink.Error).Throw();
        }
        if (st == C.SIAARA_ERR_CANCELLED)
        {
            cancellationToken.ThrowIfCancellationRequested();
        }
        Status.Check(st);
        return new DicomdirSummary((int)summary.records, (int)summary.reported, (int)summary.files, (int)summary.missing_files,
            (int)summary.rejected_paths, (int)summary.unreachable, summary.sequential_fallback != 0, (int)summary.warnings);
    }

    /// <summary>Convenience: every record of a DICOMDIR.</summary>
    /// <param name="path">Path of the DICOMDIR.</param>
    /// <param name="options">Options, or null.</param>
    /// <returns>The records.</returns>
    public static IReadOnlyList<DicomdirRecord> ReadAll(string path, DicomdirReadOptions? options = null)
    {
        var list = new List<DicomdirRecord>();
        Read(path, list.Add, options);
        return list;
    }

    /// <summary>Copies files to root/DICOM/... and writes root/DICOMDIR (blocking; see <see cref="WriteAsync"/>).</summary>
    /// <param name="root">Output folder.</param>
    /// <param name="files">Files to include.</param>
    /// <param name="options">Options, or null.</param>
    /// <returns>Number of warnings.</returns>
    public static int Write(string root, IReadOnlyList<string> files, DicomdirWriteOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(root);
        ArgumentNullException.ThrowIfNull(files);
        using var mem = new NativeAllocations();
        var ptrs = new nint[files.Count];
        var lens = new nuint[files.Count];
        for (int i = 0; i < files.Count; i++)
        {
            ArgumentException.ThrowIfNullOrEmpty(files[i], nameof(files));
            ptrs[i] = mem.Utf8(files[i]);
            lens[i] = (nuint)System.Text.Encoding.UTF8.GetByteCount(files[i]);
        }
        byte[] r = Status.Utf8(root);
        int st;
        uint warnings;
        if (options is null)
        {
            st = Native.siaara_dicomdir_write(r, ptrs, lens, (nuint)files.Count, in Unsafe.NullRef<siaara_dicomdir_write_options>(), out warnings);
        }
        else
        {
            var o = new siaara_dicomdir_write_options
            {
                struct_size = StructSize.Of<siaara_dicomdir_write_options>(),
                profile = (int)options.Profile,
                overwrite = options.Overwrite ? 1 : 0,
                allow_any_transfer_syntax = options.AllowAnyTransferSyntax ? 1 : 0,
                file_set_id = mem.Utf8(options.FileSetId),
            };
            st = Native.siaara_dicomdir_write(r, ptrs, lens, (nuint)files.Count, in o, out warnings);
        }
        Status.Check(st);
        return (int)warnings;
    }

    /// <summary>Write on a thread-pool thread (copies every file: long-running and blocking).</summary>
    /// <param name="root">Output folder.</param>
    /// <param name="files">Files to include.</param>
    /// <param name="options">Options, or null.</param>
    /// <param name="cancellationToken">Cancels before the work starts.</param>
    /// <returns>Number of warnings.</returns>
    public static Task<int> WriteAsync(string root, IReadOnlyList<string> files, DicomdirWriteOptions? options = null,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => Write(root, files, options), cancellationToken);

    private sealed class Sink : IDicomdirSink
    {
        private readonly Action<DicomdirRecord> _onRecord;
        private readonly CancellationToken _token;

        internal Sink(Action<DicomdirRecord> onRecord, CancellationToken token)
        {
            _onRecord = onRecord;
            _token = token;
        }

        internal Exception? Error { get; private set; }

        public int OnRecord(in siaara_dicomdir_entry entry, in siaara_index_record keys, ReadOnlySpan<byte> strings)
        {
            if (_token.IsCancellationRequested || Error is not null)
            {
                return 1;
            }
            _onRecord(new DicomdirRecord((int)entry.index, entry.parent, (int)entry.depth, entry.in_use != 0,
                (SiaaraErrorCode)entry.file_status, (long)entry.offset, new DicomIndexRecord(in keys, strings)));
            return 0;
        }

        public void OnCallbackException(Exception exception) => Error ??= exception;
    }
}
