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
        DataFolderWarning = paths.AccessWarning();
    }

    /// <summary>Per-run ids that stand for Study and Series Instance UIDs in URLs.</summary>
    public OpaqueIds Ids { get; } = new();

    /// <summary>Set when the data folder can be reached by other users (shown as a banner; null when it is private).</summary>
    public string? DataFolderWarning { get; }

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
        try
        {
            return Sessions.Create(studyInstanceUid);
        }
        catch (SessionLimitException)
        {
            Audit.Append("study.open", AuditOutcome.Failure, new Dictionary<string, string> { ["patientKey"] = study?.PatientKey ?? "", ["study"] = studyInstanceUid },
                "too many viewer sessions are open");
            throw;
        }
    }

    /// <summary>Sign out / lock: closes every viewer session (and so its socket) and invalidates the session cookie;
    /// every open page then leaves. Audited as session.signout.</summary>
    /// <param name="security">The app's launch security.</param>
    public void SignOut(Security.LaunchSecurity security)
    {
        ArgumentNullException.ThrowIfNull(security);
        Sessions.CloseAll();
        security.SignOut();
    }

    /// <summary>Changes the settings and audits settings.change with the names of the settings that changed (a changed
    /// store folder as a path reference, never the path).</summary>
    /// <param name="change">Mutation of a copy of the settings.</param>
    public void UpdateSettings(Action<UserPrefs> change)
    {
        ArgumentNullException.ThrowIfNull(change);
        UserPrefs before = Settings.Current;
        var fields = new Dictionary<string, string>();
        try
        {
            Settings.Update(change);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Audit.Append("settings.change", AuditOutcome.Failure, fields, Reason(e));
            throw;
        }
        UserPrefs after = Settings.Current;
        IReadOnlyList<string> changed = SettingsStore.ChangedNames(before, after);
        if (changed.Count == 0)
        {
            return;
        }
        fields["changed"] = string.Join(",", changed);
        if (changed.Contains("storeRoot"))
        {
            fields["storeRoot"] = Audit.PathReference(after.StoreRoot);
        }
        Audit.Append("settings.change", AuditOutcome.Success, fields);
    }

    /// <summary>Deletes a study. The intent is written to the audit log first (study.delete, outcome "intent"): when that
    /// cannot be written, <see cref="AuditUnavailableException"/> is thrown and nothing is deleted. The outcome
    /// (success with the number of instances, or failure with the reason) is written after it.
    /// Viewer sessions of the study are closed and cached handles released first, so no file stays open.</summary>
    /// <param name="study">Study row.</param>
    /// <returns>Instances removed.</returns>
    public async Task<int> DeleteStudyAsync(StudyRow study)
    {
        ArgumentNullException.ThrowIfNull(study);
        var fields = new Dictionary<string, string> { ["patientKey"] = study.PatientKey, ["study"] = study.StudyInstanceUid };
        Audit.AppendRequired("study.delete", AuditOutcome.Intent, fields);
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

    /// <summary>Deletes every study of the row's patient (by patient key). As for a study: the intent is audited first
    /// (patient.delete, outcome "intent") and nothing is deleted when it cannot be written; the outcome follows.</summary>
    /// <param name="study">A study row of the patient.</param>
    /// <returns>Instances removed.</returns>
    public async Task<int> DeletePatientAsync(StudyRow study)
    {
        ArgumentNullException.ThrowIfNull(study);
        var fields = new Dictionary<string, string> { ["patientKey"] = study.PatientKey };
        Audit.AppendRequired("patient.delete", AuditOutcome.Intent, fields);
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
