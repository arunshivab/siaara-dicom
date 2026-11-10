using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Siaara.Interop;

namespace Siaara.Modules;

/// <summary>Kind of a PS3.3 table (siaara_module_info.kind).</summary>
public enum ModuleKind
{
    /// <summary>A module.</summary>
    Module = C.SIAARA_MODULE_KIND_MODULE,
    /// <summary>A macro.</summary>
    Macro = C.SIAARA_MODULE_KIND_MACRO,
    /// <summary>The item of a sequence defined inside a table.</summary>
    SequenceItem = C.SIAARA_MODULE_KIND_ITEM,
    /// <summary>Another PS3.3 attribute table.</summary>
    Table = C.SIAARA_MODULE_KIND_TABLE,
    /// <summary>Every Functional Group Macro (the items of the functional groups sequences).</summary>
    FunctionalGroups = C.SIAARA_MODULE_KIND_FUNCTIONAL_GROUPS,
}

/// <summary>Attribute Type as PS3.3 gives it.</summary>
public enum AttributeType
{
    /// <summary>The table has no Type column (normalized IODs).</summary>
    None = C.SIAARA_ATTR_TYPE_NONE,
    /// <summary>Type 1.</summary>
    Type1 = C.SIAARA_ATTR_TYPE_1,
    /// <summary>Type 1C.</summary>
    Type1C = C.SIAARA_ATTR_TYPE_1C,
    /// <summary>Type 2.</summary>
    Type2 = C.SIAARA_ATTR_TYPE_2,
    /// <summary>Type 2C.</summary>
    Type2C = C.SIAARA_ATTR_TYPE_2C,
    /// <summary>Type 3.</summary>
    Type3 = C.SIAARA_ATTR_TYPE_3,
}

/// <summary>An attribute of a module (Include rows expanded).</summary>
/// <param name="Tag">The tag (repeating groups: xx positions zero, see Mask).</param>
/// <param name="Mask">Bits of Tag that must match.</param>
/// <param name="Type">PS3.3 Type.</param>
/// <param name="Keyword">PS3.6 keyword.</param>
/// <param name="VR">PS3.6 VR ("US/SS" when it depends on context).</param>
/// <param name="VM">PS3.6 VM.</param>
/// <param name="IsRetired">Retired in PS3.6.</param>
/// <param name="ItemModule">For a sequence: index of its item's module, else -1.</param>
public sealed record ModuleAttributeInfo(DicomTag Tag, uint Mask, AttributeType Type, string Keyword, string VR, string VM,
    bool IsRetired, int ItemModule);

/// <summary>A module, macro or sequence item of the generated tables (siaara_module_info).</summary>
/// <param name="Index">Index (siaara_module_get).</param>
/// <param name="Kind">The kind.</param>
/// <param name="Name">PS3.3 name, e.g. "RT Beams Module".</param>
/// <param name="TableId">PS3.3 table, e.g. "C.8-50" (empty for items).</param>
/// <param name="ClassName">Generated class in Siaara.Modules.</param>
/// <param name="AttributeCount">Attributes after Include rows are expanded.</param>
public sealed record ModuleInfo(int Index, ModuleKind Kind, string Name, string TableId, string ClassName, int AttributeCount)
{
    /// <summary>The attributes (Include rows expanded).</summary>
    public IReadOnlyList<ModuleAttributeInfo> Attributes
    {
        get
        {
            var r = new ModuleAttributeInfo[AttributeCount];
            for (int i = 0; i < r.Length; i++)
            {
                var a = new siaara_module_attribute { struct_size = StructSize.Of<siaara_module_attribute>() };
                Status.Check(Native.siaara_module_attribute_get((uint)Index, (uint)i, ref a));
                r[i] = new ModuleAttributeInfo(new DicomTag(a.tag), a.mask, (AttributeType)a.type,
                    Marshal.PtrToStringUTF8(a.keyword) ?? "", Marshal.PtrToStringUTF8(a.vr) ?? "",
                    Marshal.PtrToStringUTF8(a.vm) ?? "", a.retired != 0, a.item_module);
            }
            return r;
        }
    }
}

/// <summary>Entry point of the generated classes: every module and macro of a data set.</summary>
public sealed partial class DicomModules
{
    private readonly DicomDataset _ds;

    internal DicomModules(DicomDataset ds) => _ds = ds;

    /// <summary>The data set read.</summary>
    public DicomDataset Dataset => _ds;

    /// <summary>True when at least one attribute of the named module is present (siaara_dataset_module_present).</summary>
    /// <param name="name">"RT Beams", "RT Beams Module", "C.8-50" or a class name.</param>
    /// <returns>True when present.</returns>
    /// <exception cref="SiaaraException">Unknown module (NotFound).</exception>
    public bool IsPresent(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        byte[] n = Encoding.UTF8.GetBytes(name);
        using var l = new HandleLease(_ds.File.Handle);
        Status.Check(Native.siaara_dataset_module_present(_ds.Resolve(l.Pointer), n, out int present));
        return present != 0;
    }

    /// <summary>Every module, macro, table and sequence item of the generated tables.</summary>
    public static IReadOnlyList<ModuleInfo> All
    {
        get
        {
            Status.Check(Native.siaara_module_count(out uint n));
            var r = new ModuleInfo[n];
            for (uint i = 0; i < n; i++)
            {
                r[i] = Get(i);
            }
            return r;
        }
    }

    /// <summary>Finds a module by name ("RT Beams", "RT Beams Module", "C.8-50", class name); null when unknown.</summary>
    /// <param name="name">The name.</param>
    /// <returns>The module, or null.</returns>
    public static ModuleInfo? Find(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        return Native.siaara_module_find(Encoding.UTF8.GetBytes(name), out uint i) == Status.Ok ? Get(i) : null;
    }

    private static ModuleInfo Get(uint i)
    {
        var m = new siaara_module_info { struct_size = StructSize.Of<siaara_module_info>() };
        Status.Check(Native.siaara_module_get(i, ref m));
        return new ModuleInfo((int)i, (ModuleKind)m.kind, Marshal.PtrToStringUTF8(m.name) ?? "",
            Marshal.PtrToStringUTF8(m.table_id) ?? "", Marshal.PtrToStringUTF8(m.class_name) ?? "", (int)m.attribute_count);
    }
}
