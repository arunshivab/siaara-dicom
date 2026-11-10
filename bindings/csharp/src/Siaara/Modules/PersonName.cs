using System;
using System.Collections.Generic;

namespace Siaara.Modules;

/// <summary>
/// One component group of a person name (PS3.5 6.2.1): family name, given name, middle name,
/// prefix, suffix. Components are exactly as stored (decoded to UTF-8); an empty or absent
/// component is null.
/// </summary>
public sealed class PersonNameGroup
{
    internal PersonNameGroup(string raw)
    {
        Raw = raw;
        string[] c = raw.Split('^');
        Family = At(c, 0);
        Given = At(c, 1);
        Middle = At(c, 2);
        Prefix = At(c, 3);
        Suffix = At(c, 4);
    }

    /// <summary>The group as stored, components separated by '^'.</summary>
    public string Raw { get; }

    /// <summary>Family name complex (component 1).</summary>
    public string? Family { get; }

    /// <summary>Given name complex (component 2).</summary>
    public string? Given { get; }

    /// <summary>Middle name (component 3).</summary>
    public string? Middle { get; }

    /// <summary>Name prefix (component 4).</summary>
    public string? Prefix { get; }

    /// <summary>Name suffix (component 5).</summary>
    public string? Suffix { get; }

    /// <summary>
    /// Composite: the components present, in the order prefix, given, middle, family, suffix,
    /// joined by one space; each component is used as stored. Null when the group has none.
    /// </summary>
    public string? Full
    {
        get
        {
            var parts = new List<string>(5);
            foreach (string? p in new[] { Prefix, Given, Middle, Family, Suffix })
            {
                if (p is not null)
                {
                    parts.Add(p);
                }
            }
            return parts.Count == 0 ? null : string.Join(' ', parts);
        }
    }

    /// <inheritdoc/>
    public override string ToString() => Raw;

    private static string? At(string[] c, int i) => i < c.Length && c[i].Length > 0 ? c[i] : null;
}

/// <summary>
/// A PN value (PS3.5 6.2.1): up to three component groups separated by '=' (alphabetic,
/// ideographic, phonetic). Nothing is normalised. The shortcuts (Family, Given, First, Last,
/// Full, ...) refer to the alphabetic group; use <see cref="Ideographic"/> / <see cref="Phonetic"/>
/// for the others.
/// </summary>
public sealed class PersonName
{
    /// <summary>Parses a decoded PN value.</summary>
    /// <param name="value">The value as stored (UTF-8 text).</param>
    public PersonName(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        Raw = value;
        string[] g = value.Split('=');
        Alphabetic = new PersonNameGroup(g[0]);
        Ideographic = g.Length > 1 && g[1].Length > 0 ? new PersonNameGroup(g[1]) : null;
        Phonetic = g.Length > 2 && g[2].Length > 0 ? new PersonNameGroup(g[2]) : null;
    }

    /// <summary>The whole value as stored.</summary>
    public string Raw { get; }

    /// <summary>The alphabetic (single-byte) group.</summary>
    public PersonNameGroup Alphabetic { get; }

    /// <summary>The ideographic group, or null.</summary>
    public PersonNameGroup? Ideographic { get; }

    /// <summary>The phonetic group, or null.</summary>
    public PersonNameGroup? Phonetic { get; }

    /// <summary>Family name (alphabetic group).</summary>
    public string? Family => Alphabetic.Family;

    /// <summary>Given name (alphabetic group).</summary>
    public string? Given => Alphabetic.Given;

    /// <summary>Middle name (alphabetic group).</summary>
    public string? Middle => Alphabetic.Middle;

    /// <summary>Name prefix (alphabetic group).</summary>
    public string? Prefix => Alphabetic.Prefix;

    /// <summary>Name suffix (alphabetic group).</summary>
    public string? Suffix => Alphabetic.Suffix;

    /// <summary>Composite: the given name (alphabetic group), i.e. <see cref="Given"/>.</summary>
    public string? First => Alphabetic.Given;

    /// <summary>Composite: the family name (alphabetic group), i.e. <see cref="Family"/>.</summary>
    public string? Last => Alphabetic.Family;

    /// <summary>Composite: see <see cref="PersonNameGroup.Full"/> (alphabetic group).</summary>
    public string? Full => Alphabetic.Full;

    /// <inheritdoc/>
    public override string ToString() => Raw;
}
