// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Diagnostics;

namespace Siaara.Viewer.Core.Viewing;

/// <summary>Cine end behaviour.</summary>
public enum CineMode
{
    /// <summary>Back to the first frame.</summary>
    Loop = 0,
    /// <summary>Back and forth.</summary>
    Bounce = 1,
}

/// <summary>
/// Cine timing on the server: the frame to show is computed from elapsed wall time, so when the
/// browser is slow (frames not yet acknowledged) frames are skipped and the speed stays true.
/// </summary>
public sealed class CinePlayer
{
    private readonly Stopwatch _clock = new();
    private int _startIndex;
    private int _count = 1;

    /// <summary>Frames per second.</summary>
    public double Fps { get; set; } = 10;

    /// <summary>Loop or bounce.</summary>
    public CineMode Mode { get; set; }

    /// <summary>Playing.</summary>
    public bool IsPlaying => _clock.IsRunning;

    /// <summary>Frames shown since play (for the status line).</summary>
    public long FramesShown { get; set; }

    /// <summary>Starts playing from an index.</summary>
    /// <param name="index">Current index.</param>
    /// <param name="count">Images in the series.</param>
    public void Play(int index, int count)
    {
        _count = Math.Max(1, count);
        _startIndex = Math.Clamp(index, 0, _count - 1);
        FramesShown = 0;
        _clock.Restart();
    }

    /// <summary>Stops.</summary>
    public void Pause() => _clock.Stop();

    /// <summary>Index for an elapsed time (pure; used by the timer and the tests).</summary>
    /// <param name="start">Start index.</param>
    /// <param name="count">Images.</param>
    /// <param name="fps">Rate.</param>
    /// <param name="mode">Mode.</param>
    /// <param name="elapsedSeconds">Elapsed.</param>
    /// <returns>Index.</returns>
    public static int IndexAt(int start, int count, double fps, CineMode mode, double elapsedSeconds)
    {
        if (count <= 1)
        {
            return 0;
        }
        long steps = (long)Math.Floor(Math.Max(0, elapsedSeconds) * Math.Max(0.1, fps));
        if (mode == CineMode.Loop)
        {
            return (int)((start + steps) % count);
        }
        long period = 2L * (count - 1);
        long pos = (start + steps) % period;
        return (int)(pos < count ? pos : period - pos);
    }

    /// <summary>The index to show now.</summary>
    public int CurrentIndex => IndexAt(_startIndex, _count, Fps, Mode, _clock.Elapsed.TotalSeconds);

    /// <summary>Milliseconds until the next frame is due.</summary>
    public double MillisecondsToNext
    {
        get
        {
            double period = 1000.0 / Math.Max(0.1, Fps);
            double t = _clock.Elapsed.TotalMilliseconds;
            return period - (t % period);
        }
    }
}
