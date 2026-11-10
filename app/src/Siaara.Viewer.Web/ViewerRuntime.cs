// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Audit;
using Siaara.Viewer.Core.Imaging;
using Siaara.Viewer.Core.Import;
using Siaara.Viewer.Core.Measure;
using Siaara.Viewer.Core.Settings;
using Siaara.Viewer.Core.Store;
using Siaara.Viewer.Core.Toolkit;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Web;

/// <summary>Everything the web layer needs, created once by the host and shared (registered as singletons).</summary>
public sealed class ViewerRuntime : IDisposable
{
    /// <summary>Creates the runtime around an opened store.</summary>
    /// <param name="paths">Per-user folders.</param>
    /// <param name="settings">Settings.</param>
    /// <param name="scheduler">Scheduler.</param>
    /// <param name="store">Store (opened by the host).</param>
    /// <param name="audit">Audit log.</param>
    public ViewerRuntime(AppPaths paths, SettingsStore settings, ToolkitScheduler scheduler, StoreService store, AuditLog audit)
    {
        ArgumentNullException.ThrowIfNull(paths);
        ArgumentNullException.ThrowIfNull(settings);
        Paths = paths;
        Settings = settings;
        Scheduler = scheduler;
        Store = store;
        Audit = audit;
        UserPrefs p = settings.Current;
        Cache = new RenderCache(scheduler, (long)p.DecodedCacheMegabytes * 1024 * 1024, p.MaxFullHandlesPerViewport * 4);
        Annotations = new AnnotationStore(paths.AnnotationFolder, Environment.UserName);
        Import = new ImportService(store, scheduler, audit);
        Thumbnails = new ThumbnailService(store, scheduler);
        Sessions = new SessionRegistry(new ViewerServices(store, Cache, scheduler, settings, Annotations, audit));
    }

    /// <summary>Paths.</summary>
    public AppPaths Paths { get; }
    /// <summary>Settings.</summary>
    public SettingsStore Settings { get; }
    /// <summary>Scheduler.</summary>
    public ToolkitScheduler Scheduler { get; }
    /// <summary>Store.</summary>
    public StoreService Store { get; }
    /// <summary>Audit.</summary>
    public AuditLog Audit { get; }
    /// <summary>Render cache.</summary>
    public RenderCache Cache { get; }
    /// <summary>Annotations.</summary>
    public AnnotationStore Annotations { get; }
    /// <summary>Import.</summary>
    public ImportService Import { get; }
    /// <summary>Thumbnails.</summary>
    public ThumbnailService Thumbnails { get; }
    /// <summary>Sessions.</summary>
    public SessionRegistry Sessions { get; }

    /// <summary>Opens a study in a new viewer session and audits study.open (with its outcome) with the store's patient key.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <returns>The session (loading in the background).</returns>
    public async Task<ViewerSession> OpenStudyAsync(string studyInstanceUid)
    {
        StudyRow? study;
        try
        {
            study = await Store.GetStudyAsync(studyInstanceUid).ConfigureAwait(false);
        }
        catch (Exception e) when (e is TkException or IOException)
        {
            Audit.Append("study.open", AuditOutcome.Failure, new Dictionary<string, string> { ["patientKey"] = "", ["study"] = studyInstanceUid },
                Reason(e));
            throw;
        }
        Audit.Append("study.open", study is null ? AuditOutcome.Failure : AuditOutcome.Success, new Dictionary<string, string>
        {
            ["patientKey"] = study?.PatientKey ?? "",
            ["study"] = studyInstanceUid,
        }, study is null ? "study not in the store" : null);
        return Sessions.Create(studyInstanceUid);
    }

    /// <summary>Deletes a study and audits study.delete with the patient key and the outcome (failed deletes too).
    /// Viewer sessions of the study are closed and cached handles released first, so no file stays open.</summary>
    /// <param name="study">Study row.</param>
    /// <returns>Instances removed.</returns>
    public async Task<int> DeleteStudyAsync(StudyRow study)
    {
        ArgumentNullException.ThrowIfNull(study);
        var fields = new Dictionary<string, string> { ["patientKey"] = study.PatientKey, ["study"] = study.StudyInstanceUid };
        try
        {
            await ReleaseAsync([study.StudyInstanceUid]).ConfigureAwait(false);
            int removed = await Store.DeleteStudyAsync(study.StudyInstanceUid).ConfigureAwait(false);
            fields["instances"] = removed.ToString(CultureInfo.InvariantCulture);
            Audit.Append("study.delete", AuditOutcome.Success, fields);
            return removed;
        }
        catch (Exception e)
        {
            // Every failed delete is audited, whatever the cause; the caller shows the error.
            Audit.Append("study.delete", AuditOutcome.Failure, fields, Reason(e));
            throw;
        }
    }

    /// <summary>Deletes every study of the row's patient (by patient key) and audits patient.delete with the outcome.</summary>
    /// <param name="study">A study row of the patient.</param>
    /// <returns>Instances removed.</returns>
    public async Task<int> DeletePatientAsync(StudyRow study)
    {
        ArgumentNullException.ThrowIfNull(study);
        var fields = new Dictionary<string, string> { ["patientKey"] = study.PatientKey };
        try
        {
            IReadOnlyList<string> studies = await Store.GetPatientStudyUidsAsync(study.PatientId, study.PatientKey).ConfigureAwait(false);
            await ReleaseAsync([.. studies, study.StudyInstanceUid]).ConfigureAwait(false);
            int removed = await Store.DeletePatientAsync(study.PatientKey).ConfigureAwait(false);
            fields["instances"] = removed.ToString(CultureInfo.InvariantCulture);
            Audit.Append("patient.delete", AuditOutcome.Success, fields);
            return removed;
        }
        catch (Exception e)
        {
            Audit.Append("patient.delete", AuditOutcome.Failure, fields, Reason(e));
            throw;
        }
    }

    // Closes the sessions of the studies (their header handles), then drops cached full handles,
    // decoded frames and thumbnails of their instances.
    private async Task ReleaseAsync(IReadOnlyCollection<string> studyUids)
    {
        Sessions.CloseStudies(studyUids);
        var sops = new List<string>();
        foreach (string uid in studyUids.Distinct(StringComparer.Ordinal))
        {
            sops.AddRange(await Store.GetStudySopInstanceUidsAsync(uid).ConfigureAwait(false));
        }
        Cache.Forget(sops);
        Thumbnails.Forget(sops);
    }

    // Plain words for an audit reason: the toolkit's status text or the exception type, never a message
    // that could hold a path or a dataset value.
    private static string Reason(Exception e) => e switch
    {
        TkException tk => tk.Message,
        OperationCanceledException => "cancelled",
        UnauthorizedAccessException => "access denied",
        IOException => "file system error",
        ObjectDisposedException => "the store is closed",
        _ => e.GetType().Name,
    };

    /// <summary>Rendering backend tag for the status bar (2D renders on the CPU in M1).</summary>
    public static string Backend => "CPU";

    /// <summary>Toolkit version.</summary>
    public static string ToolkitVersion => Tk.Version;

    /// <inheritdoc/>
    public void Dispose()
    {
        Sessions.Dispose();
        Cache.Dispose();
    }
}
