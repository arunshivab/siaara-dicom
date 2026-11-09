// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Text;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Imaging;

/// <summary>
/// A sorted, displayable (sub-)series. When the toolkit's sort finds several stacks, each stack
/// becomes its own SeriesSource with the same Series Instance UID and a stack number.
/// </summary>
public sealed class SeriesSource
{
    internal SeriesSource(string seriesUid, int? seriesNumber, string description, string modality, int stack, int stackCount,
                          IReadOnlyList<SeriesImage> images, IReadOnlyList<string> warnings)
    {
        SeriesInstanceUid = seriesUid;
        SeriesNumber = seriesNumber;
        Description = description;
        Modality = modality;
        Stack = stack;
        StackCount = stackCount;
        Images = images;
        Warnings = warnings;
    }

    /// <summary>Series Instance UID.</summary>
    public string SeriesInstanceUid { get; }

    /// <summary>Series Number.</summary>
    public int? SeriesNumber { get; }

    /// <summary>Series Description (stored value).</summary>
    public string Description { get; }

    /// <summary>Modality.</summary>
    public string Modality { get; }

    /// <summary>Stack number (0-based) when the series was split.</summary>
    public int Stack { get; }

    /// <summary>Stacks the series was split into.</summary>
    public int StackCount { get; }

    /// <summary>Images in display order.</summary>
    public IReadOnlyList<SeriesImage> Images { get; }

    /// <summary>Sort warnings in plain words (shown in the viewport banner, never hidden).</summary>
    public IReadOnlyList<string> Warnings { get; }

    /// <summary>A single multi-frame instance (cine at the file's own rate).</summary>
    public bool IsMultiFrame => Images.Count > 1 && Images.All(i => ReferenceEquals(i.Instance, Images[0].Instance));

    /// <summary>Any image is lossy.</summary>
    public bool AnyLossy => Images.Any(i => i.Instance.IsLossy);

    /// <summary>Label for the series strip.</summary>
    public string Label
    {
        get
        {
            string n = SeriesNumber?.ToString(CultureInfo.InvariantCulture) ?? "-";
            string s = StackCount > 1 ? " (stack " + (Stack + 1).ToString(CultureInfo.InvariantCulture) + ")" : "";
            return n + " " + Modality + " " + Description + s;
        }
    }

    /// <summary>Default cine rate: Frame Time, then Cine Rate, then Recommended Display Frame Rate, else the given defaults.</summary>
    /// <param name="stackFps">Stacks.</param>
    /// <param name="multiFrameFps">Multi-frame images without rate attributes.</param>
    /// <returns>Frames per second.</returns>
    public double DefaultFps(double stackFps, double multiFrameFps)
    {
        if (!IsMultiFrame)
        {
            return stackFps;
        }
        TkFile h = Images[0].Instance.Header;
        if (h.GetDouble(Tags.FrameTime) is double ms && ms > 0)
        {
            return 1000.0 / ms;
        }
        if (h.GetDouble(Tags.CineRate) is double cr && cr > 0)
        {
            return cr;
        }
        if (h.GetDouble(Tags.RecommendedDisplayFrameRate) is double rd && rd > 0)
        {
            return rd;
        }
        return multiFrameFps;
    }

    /// <summary>Plain words for the toolkit's sort warnings.</summary>
    /// <param name="w">Warnings.</param>
    /// <returns>Sentences.</returns>
    public static IReadOnlyList<string> DescribeWarnings(TkSortWarnings w)
    {
        var list = new List<string>();
        void Add(TkSortWarnings flag, string text)
        {
            if (w.HasFlag(flag))
            {
                list.Add(text);
            }
        }
        Add(TkSortWarnings.UnevenSpacing, "Uneven slice spacing: the distance between slices varies in this series.");
        Add(TkSortWarnings.MissingGeometry, "Some images have no position or orientation: they are ordered by instance number.");
        Add(TkSortWarnings.MultipleStacks, "Several stacks in one series: each stack is shown as its own series.");
        Add(TkSortWarnings.MixedOrientation, "Mixed image orientations in one series.");
        Add(TkSortWarnings.DuplicatePositions, "Several images share the same slice position.");
        Add(TkSortWarnings.GantryTilt, "Gantry tilt: slices are not perpendicular to the stack axis.");
        Add(TkSortWarnings.MultipleFramesOfReference, "Several frames of reference in one series.");
        Add(TkSortWarnings.TemporalSplit, "Several time points: split by temporal position.");
        Add(TkSortWarnings.SizeMismatch, "Image sizes differ within the series.");
        return list;
    }

    /// <summary>Reads the headers of a series (stop before pixel data) and sorts it (multi-frame expanded).</summary>
    /// <param name="series">Series row (UID, number, description, modality).</param>
    /// <param name="instancePaths">Instance file paths.</param>
    /// <param name="cancellationToken">Token.</param>
    /// <returns>One SeriesSource per stack.</returns>
    public static IReadOnlyList<SeriesSource> Load(TkRecord series, IReadOnlyList<string> instancePaths, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(series);
        ArgumentNullException.ThrowIfNull(instancePaths);
        var instances = new List<ImageInstance>(instancePaths.Count);
        var unreadable = 0;
        foreach (string p in instancePaths)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                instances.Add(ImageInstance.OpenHeader(p));
            }
            catch (TkException)
            {
                unreadable++;
            }
        }
        var files = instances.Select(i => (TkFile?)i.Header).ToArray();
        TkSortResult sorted = instances.Count == 0 ? new TkSortResult([], TkSortWarnings.None) : TkFile.SortSeries(files, expandFrames: true);
        var warnings = DescribeWarnings(sorted.Warnings).ToList();
        if (unreadable > 0)
        {
            warnings.Add(unreadable.ToString(CultureInfo.InvariantCulture) + " file(s) of this series could not be read and are not shown.");
        }
        // The toolkit expands enhanced multi-frame objects (per-frame functional groups); a classic
        // multi-frame object (US, XA, NM cine) comes back as one entry: show each of its frames.
        var slices = new List<(int Group, SeriesImage Image)>();
        if (sorted.Slices.Count == 0)
        {
            slices.AddRange(instances.SelectMany(i => Enumerable.Range(0, i.FrameCount).Select(f => (0, new SeriesImage(i, f)))));
        }
        else
        {
            var expanded = sorted.Slices.GroupBy(x => x.FileIndex).ToDictionary(g => g.Key, g => g.Count());
            foreach (TkSlice x in sorted.Slices)
            {
                ImageInstance inst = instances[x.FileIndex];
                if (expanded[x.FileIndex] == 1 && inst.FrameCount > 1)
                {
                    slices.AddRange(Enumerable.Range(0, inst.FrameCount).Select(f => (x.Group, new SeriesImage(inst, f))));
                }
                else
                {
                    slices.Add((x.Group, new SeriesImage(inst, x.Frame)));
                }
            }
        }
        SeriesImage[][] groups = [.. slices.Select((t, k) => (t.Group, t.Image, k)).GroupBy(t => t.Group).OrderBy(g => g.Key)
            .Select(g => g.OrderBy(t => t.k).Select(t => t.Image).ToArray())];
        string modality = series.Modality.Length > 0 ? series.Modality : instances.FirstOrDefault()?.Modality ?? "";
        var result = new List<SeriesSource>(groups.Length);
        for (int g = 0; g < groups.Length; g++)
        {
            if (groups[g].Length > 0)
            {
                result.Add(new SeriesSource(series.SeriesInstanceUid, series.SeriesNumber, series.SeriesDescription, modality, g, groups.Length,
                    groups[g], warnings));
            }
        }
        return result;
    }
}
