# Private-tag source data

This folder is the provenance source of SIAARA's private-tag dictionary. It is the only input of
`tools/gen_private_dict.py`, which turns it into `src/dict/private_dictionary_data.inc`.

## Where it came from

Every row was extracted from vendor **DICOM Conformance Statements** (PDF) supplied by the
project owner: 2,914 documents from Siemens, GE, Philips, Canon/Toshiba, Varian and Elekta.
No other toolkit's private dictionary (GDCM, DCMTK, pydicom, dcm4che, fo-dicom or any other) was
used to build it or to check it. `summary.md` gives the totals, the column meanings, how each
creator was found and the odd findings; `method/` holds the extraction scripts and the per-vendor
creator vocabularies that were reviewed by hand.

## Files

| File | What |
|---|---|
| `siemens.tsv`, `ge.tsv`, `philips.tsv`, `canon_toshiba.tsv`, `varian.tsv`, `elekta.tsv` | One row per document per creator+group+element, with provenance (vendor, product, software version, document, page) and notes. **Generator input.** |
| `creators.tsv` | Every private creator string, the vendors, groups and documents it appears in. |
| `conflicts.tsv` | The rows of every creator+group+element whose documents disagree on VR, VM or name. |
| `inventory.tsv` | Every PDF read: size, vendor, product, version, date, pages, text layer, tags found. |
| `unreadable.tsv` | PDFs with no text layer (not OCR'd, nothing guessed). |
| `summary.md` | Totals, column meanings, notes vocabulary, findings, verification. |
| `method/` | Extraction scripts and creator vocabularies (for the record; not run by the build). |

The data is kept exactly as extracted: creator spellings, names and VM text are as printed in
the documents. Decisions about conflicts, assumed creators and spelling variants are made by the
generator, not by editing these files; see `docs/PRIVATE_DICTIONARY.md`.

## Regenerating the dictionary

```
python3 tools/gen_private_dict.py           # writes src/dict/private_dictionary_data.inc
python3 tools/gen_private_dict.py --check   # exits 1 if the committed file differs (ctest: private_dict_up_to_date)
```

Files are UTF-8 with LF line endings and tab separators (no quoting); `.gitattributes` keeps them
that way on every platform.
