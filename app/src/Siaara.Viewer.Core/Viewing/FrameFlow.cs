// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Viewing;

/// <summary>
/// Back-pressure of one viewport: at most one frame in flight. Until frame n is acknowledged,
/// further requests only mark the viewport dirty (input is coalesced); after the ack, one render
/// of the newest state starts. Stale or unknown acks are ignored. Thread-safe.
/// </summary>
public sealed class FrameFlow
{
    private readonly Lock _gate = new();
    private uint _seq;
    private uint _inFlight;
    private bool _rendering;
    private bool _dirty;

    /// <summary>Sequence number of the last frame sent.</summary>
    public uint LastSent
    {
        get
        {
            lock (_gate)
            {
                return _seq;
            }
        }
    }

    /// <summary>Frames sent and not yet acknowledged (0 or 1).</summary>
    public int InFlight
    {
        get
        {
            lock (_gate)
            {
                return _inFlight != 0 ? 1 : 0;
            }
        }
    }

    /// <summary>A render is running or a frame waits for its ack.</summary>
    public bool Busy
    {
        get
        {
            lock (_gate)
            {
                return _rendering || _inFlight != 0;
            }
        }
    }

    /// <summary>The state changed. Returns true when the caller must start a render now.</summary>
    /// <returns>Start a render.</returns>
    public bool Request()
    {
        lock (_gate)
        {
            if (_rendering || _inFlight != 0)
            {
                _dirty = true;
                return false;
            }
            _rendering = true;
            _dirty = false;
            return true;
        }
    }

    /// <summary>The render finished and its frame is about to be sent: returns its sequence number.</summary>
    /// <returns>Sequence number.</returns>
    public uint BeginSend()
    {
        lock (_gate)
        {
            _seq = _seq == uint.MaxValue ? 1 : _seq + 1;
            _inFlight = _seq;
            _rendering = false;
            return _seq;
        }
    }

    /// <summary>The render produced nothing (no image, error). Returns true when a newer request is waiting.</summary>
    /// <returns>Render again.</returns>
    public bool Abandon()
    {
        lock (_gate)
        {
            _rendering = false;
            if (_dirty)
            {
                _rendering = true;
                _dirty = false;
                return true;
            }
            return false;
        }
    }

    /// <summary>The browser acknowledged a frame. Returns true when the caller must render the newest state now.</summary>
    /// <param name="seq">Acknowledged sequence number.</param>
    /// <returns>Start a render.</returns>
    public bool Ack(uint seq)
    {
        lock (_gate)
        {
            if (seq == 0 || seq != _inFlight)
            {
                return false;
            }
            _inFlight = 0;
            if (_dirty && !_rendering)
            {
                _rendering = true;
                _dirty = false;
                return true;
            }
            return false;
        }
    }

    /// <summary>Forgets an unacknowledged frame (socket reconnect). Returns true when a render must start.</summary>
    /// <returns>Start a render.</returns>
    public bool Reset()
    {
        lock (_gate)
        {
            _inFlight = 0;
            if (!_rendering)
            {
                _rendering = true;
                _dirty = false;
                return true;
            }
            _dirty = true;
            return false;
        }
    }
}
