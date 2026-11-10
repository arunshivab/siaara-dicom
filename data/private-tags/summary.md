# SIAARA private-tag dictionary: source data summary
Generated 2026-11-10 from Arun's DICOM Conformance Statements folder only. No other toolkit's private dictionary (GDCM, DCMTK, pydicom, dcm4che, fo-dicom or any other) was used, not even as a check.

## Totals
- PDFs found: **2,914** (2,906 in the folders plus 8 PDFs inside the 6 GE zip files). Read-only; nothing in the source folder was changed.
- Unreadable: **5** (no text layer, listed in `unreadable.tsv`, not OCR'd, nothing guessed).
- Tag rows written: **153,281** (one row per document per creator+element; the same tag in many documents gives many rows, each with its own provenance).
- Distinct private creators: **349** (`creators.tsv`).
- Distinct creator+group+element definitions: **8,064**.
- Conflicting creator+element keys: see per-vendor table; all rows kept, flagged in `conflict_flag`, and listed in `conflicts.tsv`.

## Per vendor
| Vendor file | Documents | Docs with private tags | Rows | Creators used | Distinct creator+element | Rows with creator not stated | Conflict keys | of which VR conflicts |
|---|---|---|---|---|---|---|---|---|
| Siemens | 720 | 465 | 39458 | 148 | 2248 | 2205 (6%) | 326 | 49 |
| GE | 949 | 544 | 92633 | 114 | 4036 | 4380 (5%) | 1155 | 208 |
| Philips | 796 | 266 | 7411 | 43 | 960 | 2550 (34%) | 198 | 87 |
| Canon/Toshiba | 286 | 179 | 10574 | 21 | 461 | 1826 (17%) | 70 | 20 |
| Varian | 144 | 70 | 2836 | 30 | 125 | 868 (31%) | 14 | 7 |
| Elekta | 19 | 9 | 369 | 8 | 264 | 73 (20%) | 1 | 0 |

## How to read the files
- **Vendor TSVs** columns: creator, group, element_xx, keyword_suggestion, description, vr, vm, retired, conflict_flag, vendor, product, software_version, document, page, notes.
- **element_xx** is the low byte. Where the document printed an explicit block (e.g. (0019,101E)), the notes say "printed with block 10".
- **description** is the vendor's attribute name exactly as printed (a short label); no vendor paragraphs were copied. **keyword_suggestion** is derived from it.
- **page** is the PDF page number (physical page), not the printed page label. "also on page(s)" in notes lists repeats within the same document.
- **creator**: exactly as printed, with no case, spacing or spelling changes. How it was found is in the notes:
  - no note: the creator is on the row itself (or written inside the tag, e.g. (0029,SIEMENS SYNGO ADVANCED PRESENTATION,9C));
  - "creator from (gggg,00BB) declaration pN": from the document's own creator element for that block;
  - "creator from section heading pN": e.g. GE's "Private Creator Identification (GEMS_ACQU_01)" headings;
  - "creator assumed from preceding group gggg declaration pN": weaker; the nearest earlier declaration for that group (used where tags are printed as xx);
  - "creator from the document's only declaration for group gggg": used when the declaration comes later in the document;
  - "creator text wraps over lines" / "creator assembled from wrapped lines; verify": the string was split across lines in the PDF.
  - blank + "private creator not stated for this row in the document": the document really doesn't say. Not guessed.
- **conflict_flag**: VR, VM and/or NAME when documents disagree for the same creator+group+element. NAME conflicts ignore case, punctuation and truncated names; most are just wording differences between versions, but VR/VM conflicts deserve a look.
- **retired**: "yes" only where the document marks the row as retired.
- The creator vocabulary for each vendor was harvested from the documents' own declarations, creator columns and headings, then reviewed by hand. Values in other columns (e.g. Manufacturer = "TOSHIBA_MEC_US", Application Header Type = "TSB_RAW") were rejected as creators.

## Things worth knowing (odd findings)
1. **Varian documents in the Siemens folder**: the 72 "Siemens/Radiation Therapy (Varian)" documents are filed under Varian (agreed with Arun). "Varian Old" is also Varian.
2. **No "others" vendor file**: every document sits in one of the six vendor folders. Documents also list other vendors' creators (e.g. TOSHIBA_MEC_CT3 in GE workstation docs, ELSCINT1 in Philips, SIEMENS MR HEADER in Varian). Those rows stay in the file of the document's vendor, because provenance is per document. 13 creators appear in more than one vendor file (see creators.tsv).
3. **Creator spellings kept as printed**, which gives separate creators for what are probably the same thing: "GEMS_ PETD_01" (with a space, in GE PET Advance docs) vs "GEMS_PETD_01"; "GEMS_ADWSOFT_3D1" vs "GEMS_ADWSoft_3D1"; "CARDIO-D.R.1.0" vs "CARDIO-D.R. 1.0"; "Philips Imaging DD001" vs "Philips Imaging DD 001" vs "PHILIPS IMAGING DD 001"; "SIEMENS MED SMS USG Antares 3D" vs "...ANTARES 3D"; "SIEMENS NUERO" (as printed); "ACUSON:1.2..840.1113680.1.0:0921" (double dot as printed). Decide at dictionary-build time whether to alias them.
4. **Siemens Oncology Care (OCS)** uses one creator per attribute ("SIEMENS MED OCS AE TITLE", "SIEMENS MED OCS CBT ROTATION DIRECTION"...). These wrap badly in the PDFs; 259 rows were assembled from wrapped lines and are marked "verify".
5. **Template creators not used**: "SIEMENS <Manufacturer Model Name>" (ACUSON Origin) and "Set to “SIEMENS Ultrasound" style text are placeholders in the document, not literal creators.
6. **Philips** annexes often list tags as "2001,115F  Private  SQ" with no creator anywhere in the annex; those rows are blank-creator (2,550 rows).
7. **Varian TrueBeam** documents list groups 3243 etc. with no creator stated; left blank.
8. **Unreadable**: the two GE scanned files ("gech-dicom-conformance-rev40-br-enpdf.pdf" and "surgerygehc...oec9800...pdf") have the same size (1,657,342 bytes), so they are probably the same document twice.
9. **Placeholder notation**: documents write the block byte as xx, XX, yy, ee or nn ((7005,yy10), (300B,ee08)); all treated as the same "any block" placeholder.
10. **Product / version / date** in inventory.tsv are taken from the file name, or from page 1 when the file name is a code/hash (Philips, Canon). They are best-effort and blank when not clearly stated; "date" is the first date printed on the first pages, which is usually the document date.
11. Lines that mention an odd-group tag but carry neither a VR nor a recognisable creator (prose, cross-references) were not turned into rows: 62,562 such mentions skipped.

## Verification
- 400 random rows were checked automatically against the page text they cite: tag, name, VR and creator found on that page (or the next) for 397. The 3 exceptions were wrapped Siemens creators; 2 had a creator fragment in the name column, now fixed (name left blank with a note).
- All TSV files parse with a constant column count.
