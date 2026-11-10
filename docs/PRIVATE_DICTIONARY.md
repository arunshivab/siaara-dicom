# Private-tag dictionary

SIAARA ships a dictionary of vendor private data elements: names, keywords, VRs and VMs for the
private tags that Siemens, GE, Philips, Canon/Toshiba, Varian and Elekta document. It is used to
interpret implicit VR files, to show private names in DICOM XML/JSON and `siaara-dump`, and is
available to applications through C++, C and C#.

## Sources

The dictionary is built **only** from vendor **DICOM Conformance Statements** (PDF) supplied by the
project owner: 2,914 documents. No other toolkit's private dictionary (GDCM, DCMTK, pydicom,
dcm4che, fo-dicom or any other) was used, as a source or as a check. The extraction, with full
provenance for every row (vendor, product, software version, document, page), is committed in
`data/private-tags/` (see its `README.md` and `summary.md`); it is the generator's only input.

## Counts (generated table)

| | |
|---|---|
| Source rows | 153,281 (one per document per creator + group + element) |
| Rows with no creator stated (not in the lookup) | 11,902 |
| Private creators | 349 |
| Entries (creator + group + element) | 8,064 |
| of which creator stated / assumed | 7,043 / 1,021 |
| VR decided | 7,481 |
| VR conflicts (undecided, candidates listed) | 318 (290 stated, 28 assumed) |
| VR not stated by any document (undecided) | 265 |
| VM decided / VM conflicts | 6,879 / 305 |
| Retired (marked so by a document) | 14 |
| Creator spelling-variant groups (fuzzy aliases) | 9 groups, 18 spellings |
| Candidate rows (VR conflicts: one per VR and document) | 7,035 |
| Entries by vendor | GE 4,030, Siemens 2,243, Philips 949, Canon/Toshiba 461, Elekta 262, Varian 119 |

The table adds about 630 KB to the library (Linux x86-64 Release): one string pool plus 28-byte
rows of integer offsets and indices, with no relocations.

## Rules

A key is **creator + group + element**, where the element is the low byte of the element within
the creator's block: (gggg,xxEE) is found through its Private Creator (gggg,00xx) and looked up as
EE. The owner decided:

1. **VR conflicts stay undecided.** When the documents that define a key disagree on its VR, the
   entry has no VR. Implicit VR reads keep such values as UN (raw bytes). Every candidate VR is
   listed with its documents and pages, and the entry is flagged *conflict*. A document that
   prints two VRs ("OW/OB") gives two candidates. A key whose documents state no VR is also
   undecided, but is not a conflict and has no candidates.
2. **Rows with no creator stated are not in the lookup.** The documents really do not say which
   creator they belong to (for example Philips annexes listing `2001,115F Private SQ`), so they
   are kept only in the source data.
3. **Assumed creators are lower confidence.** Rows whose creator was "assumed from preceding
   group" are included, marked *assumed*, and used for a key only when no row of that key states
   its creator. Where both exist, the assumed rows are ignored for that key (1,247 keys).
4. **Creator spellings: exact first, then fuzzy.** The creator is looked up exactly as stored
   (trailing spaces and NULs are value padding and ignored). If that creator does not have the
   element, spelling variants that are equal ignoring ASCII case and whitespace are tried (e.g.
   "GEMS_ADWSoft_3D1" / "GEMS_ADWSOFT_3D1", "GEMS_ PETD_01" / "GEMS_PETD_01"); such results are
   flagged *fuzzy* and report the dictionary's spelling. Among several variants, a stated entry
   is preferred to an assumed one, then the one defined by more documents.

Also:

- **VM** comes from the rows that agree with the decided VR (all rows when the VR is undecided).
  It is decided when those rows agree ignoring case and spaces, and given in the most common
  printed spelling; otherwise it is empty and flagged *VM conflict*.
- **Name**: the most common description (the vendor's label as printed); **keyword**: the most
  common `keyword_suggestion` of the rows with that description.
- **Retired** only where a document marks the element retired.
- **Primary source**: the first document (by path, then page) that gives the chosen name and VR;
  **source count**: the number of documents that define the key.

## Reading and writing

- An **implicit VR little endian** private data element whose creator and element resolve to a
  decided VR gets that VR for interpretation (getters, JSON/XML, the C API). The stored bytes are
  never changed and `Element::ReadInfo::private_vr` records the dictionary VR.
- Saving keeps the element **as read**: an implicit VR save is byte-identical; an explicit VR save
  writes the element as UN (a private sequence as a defined-length UN with implicit VR items, PS3.5
  6.2.2). If the application changes the element's VR, its VR is written.
- A private element the dictionary calls SQ that does not parse as a sequence stays UN bytes, also
  in strict mode (the dictionary is a guess about this file).
- An **explicit VR file's own VR always wins**: the private dictionary is not consulted.
- Big endian syntaxes are left alone (the bytes would need swapping).
- Creators are scoped to their data set (PS3.5 7.8.1): an item does not inherit its parent's.
- `ReadOptions::private_dictionary = false` turns it off.

## Output

- DICOM XML: `keyword="..."` on private attributes the dictionary knows (with `xml_keywords`, the
  default), next to `privateCreator`.
- DICOM JSON: `"keyword"` and `"name"` members when asked for (`ModelWriteOptions::json_private_names`,
  C `SIAARA_JSON_PRIVATE_NAMES`, C# `DicomJsonOptions.PrivateNames`). They are off by default
  because they are not members of the PS3.18 F.2 attribute object; readers ignore them.
- `siaara-dump` prints the private keyword (or name), noting *creator assumed*, the dictionary
  spelling of a *fuzzy* creator, and *VR conflict*.

## API

- C++ (`src/dict/dictionary.hpp`): `dict::lookup_private(creator, group, element_low_byte)`,
  `dict::lookup_private_element(dataset, tag)`, `dict::private_candidate(entry, i, out)`,
  `dict::implicit_private_vr(...)`. The entry has name, keyword, VR (`VR::None` when undecided),
  VM, retired, confidence (stated / assumed), match (exact / fuzzy), conflict and VM-conflict
  flags, vendor, primary source (document + page), source count and candidate count.
- C (`siaara.h`): `siaara_dict_lookup_private`, `siaara_dict_lookup_private_element`,
  `siaara_dict_private_candidate` with `siaara_private_dict_entry` / `siaara_private_dict_candidate`
  (set `struct_size`). Strings are static and NUL-terminated.
- C#: `DicomPrivateDictionary.Lookup/TryLookup/GetCandidates`, `DicomDataset.TryGetPrivateDictionaryEntry`.

## Regenerating

```
python3 tools/gen_private_dict.py            # writes src/dict/private_dictionary_data.inc
python3 tools/gen_private_dict.py --check    # exit 1 if the committed table differs (ctest private_dict_up_to_date)
python3 tools/gen_private_dict.py --stats    # the counts above
```

The generator uses the Python standard library only and is deterministic. It decodes the string
literals it writes and stops if they do not reproduce the pool exactly. Change the dictionary by
changing `data/private-tags/` (with provenance), never by editing the generated file.

## Reporting a missing or wrong private tag

Report it to the SIAARA maintainers (support channel or issue tracker) with:

- the Private Creator string exactly as in the file (gggg,00xx), the group and the element;
- the vendor, product and software version;
- ideally the vendor's DICOM Conformance Statement (document name and page) that defines it.

Entries are added only from a vendor conformance statement, with its provenance recorded in
`data/private-tags/`. Please do not send files with patient data; a description of the tag, or a
de-identified file, is enough.
