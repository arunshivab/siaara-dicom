using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Siaara.Interop;

namespace Siaara;

/// <summary>How the private creator of a private dictionary entry was established.</summary>
public enum PrivateConfidence
{
    /// <summary>A conformance statement states the creator for this element.</summary>
    Stated = 0,

    /// <summary>The creator was assumed from a nearby declaration (lower confidence); used only when no stated entry exists.</summary>
    Assumed = 1,
}

/// <summary>How the creator given to a look-up matched the dictionary.</summary>
public enum PrivateCreatorMatch
{
    /// <summary>The creator matched as written (trailing spaces and NULs ignored).</summary>
    Exact = 0,

    /// <summary>A spelling variant matched, ignoring ASCII case and whitespace.</summary>
    Fuzzy = 1,
}

/// <summary>An entry of the private dictionary (siaara_private_dict_entry), built only from vendor DICOM Conformance Statements.</summary>
/// <param name="Creator">The dictionary's spelling of the private creator (differs from the input for fuzzy matches).</param>
/// <param name="Group">The group.</param>
/// <param name="Element">The low byte of the element in the creator's block ((gggg,xxEE): EE).</param>
/// <param name="VR">The VR, or null when undecided (documents disagree, or none states one).</param>
/// <param name="ValueMultiplicity">The VM, or empty when undecided or not stated.</param>
/// <param name="Keyword">The keyword (may be empty).</param>
/// <param name="Name">The most common name in the documents (may be empty).</param>
/// <param name="IsRetired">True when a document marks the element retired.</param>
/// <param name="IsVRConflict">True when documents disagree on the VR (see <see cref="DicomPrivateDictionary.GetCandidates"/>).</param>
/// <param name="IsVMConflict">True when documents disagree on the VM.</param>
/// <param name="Confidence">Whether the creator is stated or assumed.</param>
/// <param name="Match">Whether the creator matched exactly or as a spelling variant.</param>
/// <param name="Vendor">The vendor whose documents define it.</param>
/// <param name="SourceDocument">The primary source document.</param>
/// <param name="SourcePage">The physical PDF page in the primary source.</param>
/// <param name="SourceCount">The number of documents that define this element.</param>
/// <param name="CandidateCount">The number of VR candidates.</param>
public sealed record DicomPrivateDictionaryEntry(
    string Creator, ushort Group, byte Element, string? VR, string ValueMultiplicity, string Keyword, string Name,
    bool IsRetired, bool IsVRConflict, bool IsVMConflict, PrivateConfidence Confidence, PrivateCreatorMatch Match,
    string Vendor, string SourceDocument, int SourcePage, int SourceCount, int CandidateCount);

/// <summary>A candidate VR of a private dictionary entry with its source (siaara_private_dict_candidate).</summary>
/// <param name="VR">The VR the document gives.</param>
/// <param name="Document">The document.</param>
/// <param name="Page">The physical PDF page.</param>
public sealed record DicomPrivateVRCandidate(string VR, string Document, int Page);

/// <summary>Look-ups in the private-tag dictionary compiled into the native library (docs/PRIVATE_DICTIONARY.md).</summary>
[System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1711", Justification = "\"Data Dictionary\" is the DICOM term (PS3.6).")]
public static class DicomPrivateDictionary
{
    /// <summary>The entry for a private creator, group and element low byte.</summary>
    /// <param name="creator">The Private Creator value (gggg,00xx).</param>
    /// <param name="group">The group.</param>
    /// <param name="element">The low byte of the element.</param>
    /// <param name="entry">The entry, when the method returns true.</param>
    /// <returns>True when found.</returns>
    public static bool TryLookup(string creator, ushort group, byte element,
        [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out DicomPrivateDictionaryEntry? entry)
    {
        ArgumentNullException.ThrowIfNull(creator);
        var e = new siaara_private_dict_entry { struct_size = StructSize.Of<siaara_private_dict_entry>() };
        if (Native.siaara_dict_lookup_private(Encoding.UTF8.GetBytes(creator), group, element, ref e) != Status.Ok)
        {
            entry = null;
            return false;
        }
        entry = Convert(e);
        return true;
    }

    /// <summary>The entry for a private creator, group and element low byte.</summary>
    /// <param name="creator">The Private Creator value.</param>
    /// <param name="group">The group.</param>
    /// <param name="element">The low byte of the element.</param>
    /// <returns>The entry.</returns>
    /// <exception cref="SiaaraException">Not in the private dictionary (NotFound).</exception>
    public static DicomPrivateDictionaryEntry Lookup(string creator, ushort group, byte element)
    {
        if (TryLookup(creator, group, element, out DicomPrivateDictionaryEntry? e))
        {
            return e;
        }
        throw new SiaaraException(SiaaraErrorCode.NotFound, "private element not in the private dictionary");
    }

    /// <summary>The VR candidates of an entry with their sources: every (VR, document) pair for a VR conflict, the primary source for a decided VR, none when no document states a VR.</summary>
    /// <param name="entry">The entry.</param>
    /// <returns>The candidates.</returns>
    public static IReadOnlyList<DicomPrivateVRCandidate> GetCandidates(DicomPrivateDictionaryEntry entry)
    {
        ArgumentNullException.ThrowIfNull(entry);
        byte[] creator = Encoding.UTF8.GetBytes(entry.Creator);
        var list = new List<DicomPrivateVRCandidate>(entry.CandidateCount);
        for (uint i = 0; i < (uint)entry.CandidateCount; ++i)
        {
            var c = new siaara_private_dict_candidate { struct_size = StructSize.Of<siaara_private_dict_candidate>() };
            Status.Check(Native.siaara_dict_private_candidate(creator, entry.Group, entry.Element, i, ref c));
            list.Add(new DicomPrivateVRCandidate(DicomDictionary.Chars(c.vr),
                Marshal.PtrToStringUTF8(c.document) ?? string.Empty, (int)c.page));
        }
        return list;
    }

    internal static DicomPrivateDictionaryEntry Convert(in siaara_private_dict_entry e)
    {
        string vr = DicomDictionary.Chars(e.vr);
        return new DicomPrivateDictionaryEntry(
            Marshal.PtrToStringUTF8(e.creator) ?? string.Empty, e.group, e.element, vr.Length == 0 ? null : vr,
            Marshal.PtrToStringUTF8(e.vm) ?? string.Empty, Marshal.PtrToStringUTF8(e.keyword) ?? string.Empty,
            Marshal.PtrToStringUTF8(e.name) ?? string.Empty, e.retired != 0, e.conflict != 0, e.vm_conflict != 0,
            e.confidence == 1 ? PrivateConfidence.Assumed : PrivateConfidence.Stated,
            e.match == 1 ? PrivateCreatorMatch.Fuzzy : PrivateCreatorMatch.Exact,
            Marshal.PtrToStringUTF8(e.vendor) ?? string.Empty, Marshal.PtrToStringUTF8(e.source_document) ?? string.Empty,
            (int)e.source_page, (int)e.source_count, (int)e.candidate_count);
    }
}
