using System;
using System.Collections.Generic;
using Siaara.Interop;

namespace Siaara;

/// <summary>Kind of softcopy presentation state.</summary>
public enum PresentationStateKind
{
    /// <summary>Grayscale Softcopy Presentation State.</summary>
    Grayscale = C.SIAARA_PSTATE_GRAYSCALE,
    /// <summary>Color Softcopy Presentation State.</summary>
    Color = C.SIAARA_PSTATE_COLOR,
    /// <summary>Pseudo-Color Softcopy Presentation State.</summary>
    PseudoColor = C.SIAARA_PSTATE_PSEUDO_COLOR,
    /// <summary>Blending Softcopy Presentation State.</summary>
    Blending = C.SIAARA_PSTATE_BLENDING,
    /// <summary>Another presentation state class.</summary>
    Other = C.SIAARA_PSTATE_OTHER,
}

/// <summary>A referenced image of a presentation state.</summary>
/// <param name="SopInstanceUid">Referenced SOP Instance UID.</param>
/// <param name="FrameCount">Number of Referenced Frame Numbers (0 = all frames).</param>
public sealed record PresentationStateReference(string SopInstanceUid, int FrameCount);

/// <summary>
/// A softcopy presentation state (siaara_pstate). Apply it with
/// <see cref="ViewportOptions.PresentationState"/>; immutable, usable from many threads.
/// </summary>
public sealed class PresentationState : IDisposable
{
    private readonly PresentationStateHandle _handle;

    private PresentationState(PresentationStateHandle handle) => _handle = handle;

    internal PresentationStateHandle Handle => _handle;

    /// <summary>Reads a presentation state object.</summary>
    /// <param name="file">The presentation state file (may be disposed afterwards).</param>
    /// <returns>The presentation state.</returns>
    /// <exception cref="SiaaraException">Unsupported for other SOP classes.</exception>
    public static PresentationState Open(DicomFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        var h = new PresentationStateHandle();
        using (var l = new HandleLease(file.Handle))
        {
            int st = Native.siaara_pstate_open(l.Pointer, out nint p);
            if (st != C.SIAARA_OK)
            {
                h.Dispose();
                throw Status.Error(st);
            }
            h.Attach(p);
        }
        return new PresentationState(h);
    }

    private siaara_pstate_properties Info()
    {
        var p = new siaara_pstate_properties { struct_size = StructSize.Of<siaara_pstate_properties>() };
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_pstate_info(l.Pointer, ref p));
        return p;
    }

    /// <summary>The kind.</summary>
    public PresentationStateKind Kind => (PresentationStateKind)Info().kind;

    /// <summary>Clockwise rotation of the spatial transformation (0, 90, 180, 270).</summary>
    public int Rotation => Info().rotation;

    /// <summary>Horizontal flip after the rotation.</summary>
    public bool Flip => Info().flip != 0;

    /// <summary>Number of text objects in the graphic annotations (not drawn by rendering).</summary>
    public int TextObjectCount => (int)Info().text_count;

    /// <summary>Number of graphic objects in the graphic annotations.</summary>
    public int GraphicObjectCount => (int)Info().graphic_count;

    /// <summary>Content Label.</summary>
    public string ContentLabel => Text(C.SIAARA_PSTATE_LABEL);

    /// <summary>Content Description.</summary>
    public string ContentDescription => Text(C.SIAARA_PSTATE_DESCRIPTION);

    /// <summary>Content Creator's Name.</summary>
    public string ContentCreator => Text(C.SIAARA_PSTATE_CREATOR);

    /// <summary>SOP Instance UID of the presentation state.</summary>
    public string SopInstanceUid => Text(C.SIAARA_PSTATE_SOP_INSTANCE_UID);

    private string Text(int field)
    {
        using var l = new HandleLease(_handle);
        return Status.ReadString((Ps: l.Pointer, Field: field), static (s, b, out n) => Native.siaara_pstate_string(s.Ps, s.Field, b, out n));
    }

    /// <summary>The referenced images.</summary>
    public IReadOnlyList<PresentationStateReference> References
    {
        get
        {
            int n = (int)Info().reference_count;
            var list = new PresentationStateReference[n];
            using var l = new HandleLease(_handle);
            for (int i = 0; i < n; i++)
            {
                uint frames = 0;
                string uid = Status.ReadString((Ps: l.Pointer, Index: (uint)i), static (s, b, out n) =>
                    Native.siaara_pstate_reference(s.Ps, s.Index, b, out n, out _));
                Status.Check(Native.siaara_pstate_reference(l.Pointer, (uint)i, default, out _, out frames));
                list[i] = new PresentationStateReference(uid, (int)frames);
            }
            return list;
        }
    }

    /// <summary>Whether the state applies to a frame of an instance.</summary>
    /// <param name="sopInstanceUid">SOP Instance UID.</param>
    /// <param name="frame">0-based frame.</param>
    /// <returns>True when it applies.</returns>
    public bool AppliesTo(string sopInstanceUid, int frame = 0)
    {
        ArgumentNullException.ThrowIfNull(sopInstanceUid);
        ArgumentOutOfRangeException.ThrowIfNegative(frame);
        using var l = new HandleLease(_handle);
        Status.Check(Native.siaara_pstate_applies(l.Pointer, Status.Utf8(sopInstanceUid), (uint)frame + 1, out int r));
        return r != 0;
    }

    /// <summary>Frees the presentation state.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }
}
