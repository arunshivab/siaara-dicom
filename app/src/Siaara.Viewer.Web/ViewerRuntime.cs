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

    /// <summary>Opens a study in a new viewer session and audits study.open with the store's patient key.</summary>
    /// <param name="studyInstanceUid">Study.</param>
    /// <returns>The session (loading in the background).</returns>
    public async Task<ViewerSession> OpenStudyAsync(string studyInstanceUid)
    {
        StudyRow? study = await Store.GetStudyAsync(studyInstanceUid).ConfigureAwait(false);
        Audit.Append("study.open", new Dictionary<string, string>
        {
            ["patientKey"] = study?.PatientKey ?? "",
            ["study"] = studyInstanceUid,
        });
        return Sessions.Create(studyInstanceUid);
    }

    /// <summary>Deletes a study and audits study.delete with the patient key.</summary>
    /// <param name="study">Study row.</param>
    /// <returns>Instances removed.</returns>
    public async Task<int> DeleteStudyAsync(StudyRow study)
    {
        ArgumentNullException.ThrowIfNull(study);
        int removed = await Store.DeleteStudyAsync(study.StudyInstanceUid).ConfigureAwait(false);
        Audit.Append("study.delete", new Dictionary<string, string>
        {
            ["patientKey"] = study.PatientKey,
            ["study"] = study.StudyInstanceUid,
            ["instances"] = removed.ToString(CultureInfo.InvariantCulture),
        });
        return removed;
    }

    /// <summary>Deletes every study of the row's patient (by patient key) and audits patient.delete.</summary>
    /// <param name="study">A study row of the patient.</param>
    /// <returns>Instances removed.</returns>
    public async Task<int> DeletePatientAsync(StudyRow study)
    {
        ArgumentNullException.ThrowIfNull(study);
        int removed = await Store.DeletePatientAsync(study.PatientKey).ConfigureAwait(false);
        Audit.Append("patient.delete", new Dictionary<string, string>
        {
            ["patientKey"] = study.PatientKey,
            ["instances"] = removed.ToString(CultureInfo.InvariantCulture),
        });
        return removed;
    }

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
