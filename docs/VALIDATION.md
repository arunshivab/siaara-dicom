# Validation: Type 1C / 2C conditions

The validator (`siaara-repair validate`, `siaara_validate*`, `repair::validate`) checks every
module of the IOD against the attribute tables of PS3.3, which `tools/gen_iod.py` turns into
`src/repair/iod_tables.inc`. Type 1 and 2 attributes are simple. Type 1C and 2C attributes are
required only *if* something holds, and PS3.3 writes that condition in English. This page lists
which conditions the validator evaluates and how.

Regenerate the tables after a new edition of the standard:

```
python3 -I tools/gen_iod.py part03.xml part04.xml part16.xml src/repair/iod_tables.inc
```

## What is evaluated

From PS3.3 2026d: 1829 Type 1C / 2C rows. **1057 are evaluated** (565 distinct conditions, 666
terms); **772 are not evaluated** (the condition is prose, see below).

A condition is evaluated when its "Required if ..." sentence (also "Shall be present if ...",
"Required only if ...", "Required for images where ...") is made of terms joined by **only "and"
or only "or"**. Mixed "and" / "or" wording is not evaluated (English does not say how to group
it). Each term names an attribute, with its tag or by its PS3.6 name (`Rescale Intercept is
present` is looked up in the dictionary), and has one of these forms:

| Term | Example wording | Meaning |
|---|---|---|
| Present | `X (gggg,eeee) is present`, `A and B are present` | the element exists (an empty value counts) |
| HasValue | `X has a value`, `X is present and has a value` | present and not empty (a sequence: at least one item) |
| Absent | `X is not present`, `X is absent`, `A and B are not present` | the element does not exist |
| Equals | `X has a value of A`, `X equals A or B`, `X Value 1 is ORIGINAL or MIXED`, `X is "1.2.840..." (name), ...` | a value (or value N) is one of the listed ones |
| NotEquals | `X has a value other than A`, `X is not A`, `X equals other than NONE` | present with a value, and none of the listed ones |
| Greater | `X has a Value greater than 1`, `X is non-zero` | the value is an integer greater than N |
| AbsentOrEquals | `X is absent, or is present and has the Value NO` | absent, or Equals |
| IncludesTag | `Frame Increment Pointer (0028,0009) includes the Tag for Y (gggg,eeee)`, `... points to Y` | the AT value lists Y |

`A or B is greater than 1` applies the predicate to both attributes. Binary values compare
numerically (`1`, `0001H`). `at the image level` restricts the lookup to the top-level data set.

Fixed wordings PS3.3 defines in terms of an attribute are rewritten first:

| Wording | Evaluated as |
|---|---|
| `Pixel Data contains more than one Frame`, `multi-frame pixel data are present`, `the image is a multi-frame image` | Number of Frames (0028,0008) greater than 1 (PS3.3 C.7.6.6) |
| `Lossy Compression has been performed on the Image` | Lossy Image Compression (0028,2110) equals "01" (C.7.6.1.1.5) |
| `the Target Content Item is included by-value` | Referenced Content Item Identifier (0040,DB73) absent (C.17.3) |

And two prose conditions get a structured rule (`STRUCTURED` in the generator):

* **Patient Orientation (0020,0020), General Image, 2C** - "Required if image does not require
  Image Orientation (Patient) and Image Position (Patient) or ... Image Orientation (Slide)":
  required when none of (0020,0037), (0020,0032), (0048,0102) is present anywhere in the data set.
* **Laterality (0020,0060), General Series, 2C** - "Required if the body part examined is a paired
  structure and Image Laterality, Frame Laterality or Measurement Laterality are not present":
  required when Body Part Examined (0018,0015) is one of the 147 Defined Terms whose anatomic
  concept PS3.16 Table L-5 marks as a paired structure (via Table L-1), and none of (0020,0062),
  (0020,9072), (0024,0113) is present anywhere. Without Body Part Examined it is not judged.

### Where the attribute of a term is looked for

In the sequence item being checked, then each enclosing item outwards, then the top-level data
set. Inside the Shared / Per-Frame Functional Groups Sequences also anywhere in the frame's
functional group item and in the shared functional group item (a frame's attributes sit in the
macros' own sequences).

## Findings

| Code | Severity | When |
|---|---|---|
| `missing-type1` / `missing-type2` "(condition met)" | Error | the condition holds and the attribute is missing |
| `empty-type1` | Error | a present Type 1C attribute has no value |
| `present-condition-unmet` | Warning, or Error | a top-level 1C / 2C attribute is present, its condition is evaluated and does not hold, its description has no permissive wording ("may be present otherwise", "may also be present if ...", "may be present for ..."; any "may be present" counts), and no other module of the IOD allows it. **Error** only when the description itself forbids presence ("shall not be present otherwise", "required only if"); otherwise a **Warning**: PS3.5 7.4.4 / 7.4.6 says it shall not be present, but the condition was read from prose by the generator. Not raised for the structured rules, where "does not hold" may mean "cannot be decided". |
| `missing-functional-group` | Error | an IOD with Functional Group Macros (PS3.3 "<IOD> Functional Group Macros" table): a mandatory macro is neither in the Shared Functional Groups Sequence item nor in every Per-Frame Functional Groups Sequence item. The path names the first frame without it; the message counts them. |
| `condition-not-evaluated` | Info | once per module: the number of absent 1C / 2C attributes whose condition was not evaluated. Never an error. |
| `values-not-all-checked` | Warning | an attribute that a condition or an Enumerated Value list looks at has more than 4,096 values: only the first 4,096 are compared (a condition that would need the others counts as not evaluated). Once per attribute. |

A term that cannot be decided (a value that is not a number for Greater, a condition attribute
that is a sequence for Equals) makes the whole condition "not evaluated" unless the other terms
decide it (a false term in an "and", a true term in an "or").

## What is not evaluated

Conditions that depend on things outside the data set or on prose judgement: "a Performed
Procedure Step SOP Class was involved", "present and consistent in the contributing SOP Instances"
(legacy converted), "the right eye is measured", "the Patient is a non-human organism", "image is
part of a Series in which the images are temporally related", conditions about a referenced
instance or another item ("the Instance referenced in ..."), about code sequence items
("contains an Item with the value (...)"), and "and" / "or" mixed in one sentence.

Measured on the 232 files of gdcmData: of the 75 files for which `dciodvfy` reports 1C / 2C
errors other than Laterality, the validator reports 1C / 2C errors for 65 (before: 0).

## Functional groups, several conditions, module selection

- **Functional Group Macros.** For IODs that have them, every macro present in the Shared item or
  in a Per-Frame item is checked there like a module (Type 1 / 2, and 1C / 2C with the condition's
  attribute looked up in the frame's own item first); findings carry the item's path, e.g.
  `(5200,9230)[3](0020,9111)[0](0020,9057)`. Most frame-level conditions of PS3.3 are prose ("of
  this frame", "if the image is ...") and are not evaluated; those that name attributes are.
- **Several "Required if" sentences** in one description are alternatives: the attribute is
  required when any holds. They are evaluated only when every sentence is machine-checkable;
  otherwise the condition as a whole is not evaluated (SR Concept Name Code Sequence).
- **Optional modules** are checked when one of their attributes is present. Attributes that a
  mandatory module also lists do not count, nor do attributes of another optional module that is
  present by an attribute only it has: an object with Float Pixel Data is checked against the
  Floating Point Image Pixel Module, not also against the Image Pixel Module.
