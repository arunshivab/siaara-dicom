using System;
using System.Buffers;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>The library's display-safe copy of text (siaara_text_display_safe): C0 and C1 controls, DEL and the
/// line / paragraph separators become a space, Unicode format characters (category Cf: bidirectional overrides and
/// isolates, zero-width characters, the byte order mark, tag characters) are removed, and what is not well-formed
/// UTF-8 becomes U+FFFD. One implementation for C, C++ and C#.</summary>
internal static class DisplayText
{
    /// <summary>The display-safe copy of a string. A lone surrogate becomes U+FFFD (UTF-8 encoding does that
    /// before the library sees the text).</summary>
    internal static string Safe(string text)
    {
        if (text.Length == 0)
        {
            return text;
        }
        byte[] utf8 = Encoding.UTF8.GetBytes(text);
        return Safe(utf8);
    }

    /// <summary>The display-safe copy of UTF-8 bytes (which need not be well formed).</summary>
    internal static string Safe(ReadOnlySpan<byte> utf8)
    {
        if (utf8.IsEmpty)
        {
            return string.Empty;
        }
        // Never longer than 3 x the input (one U+FFFD per byte at worst).
        int cap = checked(utf8.Length * 3 + 1);
        byte[] rented = ArrayPool<byte>.Shared.Rent(cap);
        try
        {
            Status.Check(Native.siaara_text_display_safe(utf8, rented.AsSpan(0, cap), out nuint needed));
            return Encoding.UTF8.GetString(rented, 0, (int)needed);
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(rented, clearArray: true);
        }
    }
}
