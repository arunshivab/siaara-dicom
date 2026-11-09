using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text;
using Siaara.Interop;

namespace Siaara.Volumes;

/// <summary>A GPU device (siaara_gpu_device_info).</summary>
/// <param name="Index">Index for <see cref="GpuDevices.Select(int)"/>.</param>
/// <param name="Name">Device name.</param>
/// <param name="Driver">API / driver version.</param>
/// <param name="IsVulkan">Vulkan (else Metal).</param>
/// <param name="Type">SIAARA_GPU_DEVICE_*: 0 other, 1 integrated, 2 discrete, 3 virtual, 4 CPU.</param>
/// <param name="MemoryBytes">Device-local memory.</param>
/// <param name="MaxTexture3D">Largest 3D texture dimension.</param>
/// <param name="IsSelected">The device the toolkit renders with.</param>
public sealed record GpuDevice(int Index, string Name, string Driver, bool IsVulkan, int Type, long MemoryBytes, int MaxTexture3D,
                               bool IsSelected);

/// <summary>GPU devices for 3D views (siaara_gpu_devices / siaara_gpu_select).</summary>
public static class GpuDevices
{
    /// <summary>The devices (probed once per process); empty without a GPU.</summary>
    /// <returns>The list.</returns>
    public static IReadOnlyList<GpuDevice> List()
    {
        Status.Check(Native.siaara_gpu_devices(ref Unsafe.NullRef<siaara_gpu_device_info>(), 0, out uint count));
        if (count == 0)
        {
            return Array.Empty<GpuDevice>();
        }
        var raw = new siaara_gpu_device_info[count];
        for (int i = 0; i < raw.Length; i++)
        {
            raw[i].struct_size = StructSize.Of<siaara_gpu_device_info>();
        }
        Status.Check(Native.siaara_gpu_devices(ref raw[0], (uint)raw.Length, out count));
        var list = new List<GpuDevice>();
        for (int i = 0; i < raw.Length && i < count; i++)
        {
            ReadOnlySpan<byte> name = raw[i].name, driver = raw[i].driver;
            list.Add(new GpuDevice(i, Utf8Z(name), Utf8Z(driver), raw[i].api == C.SIAARA_GPU_API_VULKAN, raw[i].type,
                (long)raw[i].memory_bytes, (int)raw[i].max_texture_3d, raw[i].selected != 0));
        }
        return list;
    }

    /// <summary>Renders later views on device index instead of the automatic choice.</summary>
    /// <param name="index">Index from <see cref="List"/>.</param>
    public static void Select(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        Status.Check(Native.siaara_gpu_select((uint)index));
    }

    private static string Utf8Z(ReadOnlySpan<byte> s)
    {
        int n = s.IndexOf((byte)0);
        return Encoding.UTF8.GetString(n < 0 ? s : s[..n]);
    }
}
