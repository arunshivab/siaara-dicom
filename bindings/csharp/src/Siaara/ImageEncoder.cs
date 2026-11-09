using System;
using Siaara.Interop;

namespace Siaara;

/// <summary>
/// Encodes rendered images (RGBA8 or GRAY8, e.g. from <see cref="DicomFrame.Render(ViewportOptions?)"/>)
/// as JPEG or PNG for browsers and exports (siaara_encode_jpeg / siaara_encode_png). Thread safe.
/// </summary>
public static class ImageEncoder
{
    /// <summary>Baseline JPEG (JFIF; colour as YCbCr 4:4:4; alpha ignored).</summary>
    /// <param name="pixels">width x height x (4 or 1) bytes, rows top to bottom.</param>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    /// <param name="format">Rgba8 or Gray8.</param>
    /// <param name="quality">1..100.</param>
    /// <returns>The JPEG file bytes.</returns>
    public static byte[] EncodeJpeg(ReadOnlySpan<byte> pixels, int width, int height, PixelOutputFormat format = PixelOutputFormat.Rgba8,
        int quality = 90)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(quality, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(quality, 100);
        Check(width, height);
        int st = Native.siaara_encode_jpeg(pixels, (uint)width, (uint)height, (int)format, quality, out nint b);
        return Take(st, b);
    }

    /// <summary>PNG (8-bit RGBA or gray, lossless).</summary>
    /// <param name="pixels">width x height x (4 or 1) bytes, rows top to bottom.</param>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    /// <param name="format">Rgba8 or Gray8.</param>
    /// <param name="compressionLevel">0..9.</param>
    /// <returns>The PNG file bytes.</returns>
    public static byte[] EncodePng(ReadOnlySpan<byte> pixels, int width, int height, PixelOutputFormat format = PixelOutputFormat.Rgba8,
        int compressionLevel = 6)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(compressionLevel);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(compressionLevel, 9);
        Check(width, height);
        int st = Native.siaara_encode_png(pixels, (uint)width, (uint)height, (int)format, compressionLevel, out nint b);
        return Take(st, b);
    }

    private static void Check(int width, int height)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
    }

    private static byte[] Take(int status, nint buffer)
    {
        using var h = new BufferHandle();
        if (buffer != 0)
        {
            h.Attach(buffer);
        }
        Status.Check(status);
        Status.Check(Native.siaara_buffer_data(buffer, out nint data, out ulong size));
        return NativeSpan.Bytes(data, (nuint)size).ToArray();
    }
}
