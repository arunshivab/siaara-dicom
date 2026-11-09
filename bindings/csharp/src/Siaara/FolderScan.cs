using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Siaara.Interop;

namespace Siaara;

/// <summary>Kind of a scanned file.</summary>
public enum FolderScanItemKind
{
    /// <summary>A DICOM instance.</summary>
    Instance = C.SIAARA_SCAN_INSTANCE,
    /// <summary>A DICOMDIR.</summary>
    Dicomdir = C.SIAARA_SCAN_DICOMDIR,
    /// <summary>Not DICOM (reported with ReportSkipped).</summary>
    NotDicom = C.SIAARA_SCAN_NOT_DICOM,
    /// <summary>Could not be read.</summary>
    Error = C.SIAARA_SCAN_ERROR,
}

/// <summary>Folder scan options (siaara_scan_options).</summary>
public sealed record FolderScanOptions
{
    /// <summary>Worker threads; 0 = hardware concurrency (at most 16).</summary>
    public int Threads { get; init; }
    /// <summary>Recurse into sub-folders (default true).</summary>
    public bool Recursive { get; init; } = true;
    /// <summary>Follow symbolic links.</summary>
    public bool FollowSymlinks { get; init; }
    /// <summary>Accept files without preamble (default true).</summary>
    public bool AllowRaw { get; init; } = true;
    /// <summary>Also report non-DICOM files.</summary>
    public bool ReportSkipped { get; init; }
    /// <summary>Scan only the files root/DICOMDIR lists, when present.</summary>
    public bool UseDicomdir { get; init; }
    /// <summary>Stop after this many files; 0 = no limit.</summary>
    public long MaxFiles { get; init; }
}

/// <summary>One scanned file.</summary>
/// <param name="Path">The path.</param>
/// <param name="Kind">The kind.</param>
/// <param name="Status">Read status (Ok for instances).</param>
/// <param name="Record">Index record of instances and DICOMDIRs, else null.</param>
public sealed record FolderScanItem(string Path, FolderScanItemKind Kind, SiaaraErrorCode Status, DicomIndexRecord? Record);

/// <summary>Totals of a scan (siaara_scan_stats).</summary>
/// <param name="UsedDicomdir">The DICOMDIR at the root decided the file list.</param>
/// <param name="Files">Files looked at.</param>
/// <param name="Instances">Instances found.</param>
/// <param name="Dicomdirs">DICOMDIRs found.</param>
/// <param name="Skipped">Non-DICOM files.</param>
/// <param name="Errors">Unreadable files.</param>
public sealed record FolderScanStatistics(bool UsedDicomdir, long Files, long Instances, long Dicomdirs, long Skipped, long Errors);

/// <summary>Multi-threaded folder scan for DICOM files (siaara_folder_scan).</summary>
public static class FolderScanner
{
    /// <summary>
    /// Scans a folder, calling onItem once per file (never concurrently, possibly from different
    /// threads). Blocking; see <see cref="ScanAsync"/>.
    /// </summary>
    /// <param name="root">Folder.</param>
    /// <param name="onItem">Called per file.</param>
    /// <param name="options">Options, or null.</param>
    /// <param name="cancellationToken">Stops the scan (OperationCanceledException).</param>
    /// <returns>Totals.</returns>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    public static FolderScanStatistics Scan(string root, Action<FolderScanItem> onItem, FolderScanOptions? options = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(root);
        ArgumentNullException.ThrowIfNull(onItem);
        var sink = new Sink(onItem, cancellationToken);
        GCHandle gc = GCHandle.Alloc(sink);
        int st;
        var stats = new siaara_scan_stats { struct_size = StructSize.Of<siaara_scan_stats>() };
        try
        {
            byte[] r = Status.Utf8(root);
            if (options is null)
            {
                st = Native.siaara_folder_scan(r, in Unsafe.NullRef<siaara_scan_options>(), Callbacks.ScanPointer,
                    GCHandle.ToIntPtr(gc), ref stats);
            }
            else
            {
                var o = new siaara_scan_options
                {
                    struct_size = StructSize.Of<siaara_scan_options>(),
                    threads = (uint)Math.Max(0, options.Threads),
                    recursive = options.Recursive ? 1 : 0,
                    follow_symlinks = options.FollowSymlinks ? 1 : 0,
                    allow_raw = options.AllowRaw ? 1 : 0,
                    report_skipped = options.ReportSkipped ? 1 : 0,
                    use_dicomdir = options.UseDicomdir ? 1 : 0,
                    max_files = (ulong)Math.Max(0, options.MaxFiles),
                };
                st = Native.siaara_folder_scan(r, in o, Callbacks.ScanPointer, GCHandle.ToIntPtr(gc), ref stats);
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
        return new FolderScanStatistics(stats.used_dicomdir != 0, (long)stats.files, (long)stats.instances,
            (long)stats.dicomdirs, (long)stats.skipped, (long)stats.errors);
    }

    /// <summary>Scan on a thread-pool thread (blocking file I/O over a whole tree).</summary>
    /// <param name="root">Folder.</param>
    /// <param name="onItem">Called per file (on a worker thread).</param>
    /// <param name="options">Options, or null.</param>
    /// <param name="cancellationToken">Stops the scan.</param>
    /// <returns>Totals.</returns>
    public static Task<FolderScanStatistics> ScanAsync(string root, Action<FolderScanItem> onItem, FolderScanOptions? options = null,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => Scan(root, onItem, options, cancellationToken), cancellationToken);

    /// <summary>Convenience: every instance record under root.</summary>
    /// <param name="root">Folder.</param>
    /// <param name="options">Options, or null.</param>
    /// <returns>The records.</returns>
    public static IReadOnlyList<DicomIndexRecord> FindInstances(string root, FolderScanOptions? options = null)
    {
        var list = new List<DicomIndexRecord>();
        Scan(root, item =>
        {
            if (item.Kind == FolderScanItemKind.Instance && item.Record is not null)
            {
                list.Add(item.Record);
            }
        }, options);
        return list;
    }

    private sealed class Sink : IScanSink
    {
        private readonly Action<FolderScanItem> _onItem;
        private readonly CancellationToken _token;

        internal Sink(Action<FolderScanItem> onItem, CancellationToken token)
        {
            _onItem = onItem;
            _token = token;
        }

        internal Exception? Error { get; private set; }

        public int OnScanItem(string path, int kind, int status, bool hasRecord, in siaara_index_record record, ReadOnlySpan<byte> strings)
        {
            if (_token.IsCancellationRequested || Error is not null)
            {
                return 1;
            }
            DicomIndexRecord? rec = hasRecord ? new DicomIndexRecord(in record, strings) : null;
            _onItem(new FolderScanItem(path, (FolderScanItemKind)kind, (SiaaraErrorCode)status, rec));
            return 0;
        }

        public void OnCallbackException(Exception exception) => Error ??= exception;
    }
}
