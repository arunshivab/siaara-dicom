using System;
using System.Globalization;
using System.Text;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>CaptionText on the library's display-safe cleaner (siaara_text_display_safe).</summary>
internal static class DisplayTextTests
{
    // CaptionText as it was before it called the library (C# only): the reference the library must match.
    private static string OldCaptionText(string value, int maxLength, bool strip)
    {
        int max = Math.Max(0, maxLength);
        var sb = new StringBuilder(Math.Min(value.Length, max));
        for (int i = 0; i < value.Length && sb.Length < max;)
        {
            if (!Rune.TryGetRuneAt(value, i, out Rune rune))
            {
                sb.Append(strip ? '\uFFFD' : value[i]);
                i++;
                continue;
            }
            i += rune.Utf16SequenceLength;
            if (strip)
            {
                switch (Rune.GetUnicodeCategory(rune))
                {
                    case UnicodeCategory.Control:
                    case UnicodeCategory.LineSeparator:
                    case UnicodeCategory.ParagraphSeparator:
                        sb.Append(' ');
                        continue;
                    case UnicodeCategory.Format:
                        continue;
                }
            }
            if (sb.Length + rune.Utf16SequenceLength > max)
            {
                break;
            }
            sb.Append(rune.ToString());
        }
        return sb.ToString();
    }

    internal static void Register()
    {
        Case("display text: CaptionText calls the library and gives what it gave before, for every code point", () =>
        {
            var sb = new StringBuilder();
            int differences = 0;
            for (int c = 0; c <= 0x10FFFF; c++)
            {
                if (c >= 0xD800 && c <= 0xDFFF)
                {
                    continue;
                }
                string s = "a" + char.ConvertFromUtf32(c) + "b";
                if (DicomAttachmentPreview.CaptionText(s, 100) != OldCaptionText(s, 100, true))
                {
                    if (differences++ < 5)
                    {
                        Console.Error.WriteLine($"  U+{c:X4}: library and .NET categories differ");
                    }
                }
                if (c % 97 == 0)
                {
                    sb.Append(char.ConvertFromUtf32(c));
                }
            }
            CheckEq(differences, 0);
            // Lone surrogates, the cut, strip off, empty.
            foreach (string s in new[] { "a\uD83Db", "\uDC00x\uD800", "ab\U0001F600cd", "a\u202Eb\u200D\u0000\u2028", sb.ToString() })
            {
                foreach (int max in new[] { 0, 1, 2, 3, 4, 5, 100, 100_000 })
                {
                    CheckEq(DicomAttachmentPreview.CaptionText(s, max), OldCaptionText(s, max, true));
                    CheckEq(DicomAttachmentPreview.CaptionText(s, max, strip: false), OldCaptionText(s, max, false));
                }
            }
            CheckEq(DicomAttachmentPreview.CaptionText("", 10), "");
        });

    }
}
