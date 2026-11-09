// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

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
        Sessions = new SessionRegistry(new ViewerServices(store, Cache, scheduler, settings, Annotations));
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
