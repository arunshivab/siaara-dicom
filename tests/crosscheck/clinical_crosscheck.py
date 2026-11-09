#!/usr/bin/env python3
"""Cross-checks the clinical-object layer (src/clinical) against independent tools.

The third-party tools are used only as test oracles; no code from them is in the library.

    python3 tests/crosscheck/clinical_crosscheck.py --tool build/tests/clinical_tool

Oracles:
  * pydicom test files (rtstruct.dcm, test-SR.dcm, reportsi.dcm, CT_small.dcm, MR_small.dcm):
    our reading compared with a plain pydicom traversal.
  * highdicom (pip install highdicom): SEG (BINARY and FRACTIONAL), GSPS and Comprehensive 3D SR
    written by highdicom and read by us; our KOS read back by highdicom; CIELab -> RGB compared
    with highdicom.color.CIELabColor.to_rgb.
  * dciodvfy (dicom3tools): IOD validation of the KOS and GSPS we write (skipped, and reported,
    when it is not installed).
  * SUV: an independent Python evaluation of the QIBA body-weight SUV rule on a pydicom-made PET
    image.
Exit status 0 when every check passes.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
import pydicom
from pydicom.dataset import Dataset, FileMetaDataset
from pydicom.uid import ExplicitVRLittleEndian, generate_uid

FAILS = []
PASSES = 0


def check(cond, what):
    global PASSES
    if cond:
        PASSES += 1
    else:
        FAILS.append(what)
        print("  FAIL:", what)


def run(tool, *args):
    out = subprocess.run([tool, *args], capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError("tool failed: %s %s: %s" % (args[0], args[1:], out.stderr))
    return json.loads(out.stdout)


def testdata(name):
    from pydicom.data import get_testdata_file
    return get_testdata_file(name)


# ---------------------------------------------------------------- pydicom files
def check_rtstruct(tool):
    path = testdata("rtstruct.dcm")
    ours = run(tool, "rtstruct", path)
    ds = pydicom.dcmread(path, force=True)
    names = {r.ROINumber: r.ROIName for r in ds.StructureSetROISequence}
    check(len(ours["rois"]) == len(names), "rtstruct ROI count")
    by_num = {r["number"]: r for r in ours["rois"]}
    for rc in ds.ROIContourSequence:
        r = by_num.get(int(rc.ReferencedROINumber))
        check(r is not None, "rtstruct ROI %s present" % rc.ReferencedROINumber)
        if r is None:
            continue
        check(r["name"] == names[rc.ReferencedROINumber], "rtstruct ROI name")
        check(r["color"] == [int(v) for v in rc.ROIDisplayColor], "rtstruct colour")
        cs = list(rc.ContourSequence)
        check(len(cs) == len(r["contours"]), "rtstruct contour count")
        for c, o in zip(cs, r["contours"]):
            data = [float(v) for v in c.ContourData]
            check(o["points"] * 3 == len(data), "rtstruct point count")
            check(all(abs(a - b) < 1e-9 for a, b in zip(o["first"], data[:3])), "rtstruct first point")
            check(o["type"] == c.ContourGeometricType, "rtstruct geometric type")
    print("rtstruct.dcm: %d ROIs compared" % len(ours["rois"]))


def walk(ds, pos, out):
    """Pre-order pydicom traversal of an SR content tree."""
    vt = ds.get("ValueType", "")
    if vt == "" and "ReferencedContentItemIdentifier" in ds:
        vt = "REFERENCE"
    concept = None
    if "ConceptNameCodeSequence" in ds and len(ds.ConceptNameCodeSequence):
        c = ds.ConceptNameCodeSequence[0]
        concept = [c.get("CodeValue", c.get("LongCodeValue", "")), c.get("CodingSchemeDesignator", ""), c.get("CodeMeaning", "")]
    num = None
    if vt == "NUM" and "MeasuredValueSequence" in ds and len(ds.MeasuredValueSequence):
        num = float(ds.MeasuredValueSequence[0].NumericValue)
    text = ""
    for kw in ("TextValue", "DateTime", "Date", "Time", "UID", "PersonName"):
        if kw in ds and vt != "CONTAINER":
            text = str(ds.get(kw))
    out.append({"pos": ".".join(map(str, pos)), "vt": vt, "concept": concept, "num": num, "text": text,
                "rel": ds.get("RelationshipType", "")})
    for i, child in enumerate(ds.get("ContentSequence", []), 1):
        walk(child, pos + [i], out)


def check_sr_file(tool, path, label):
    ours = run(tool, "sr", path)
    ref = []
    walk(pydicom.dcmread(path, force=True), [1], ref)
    check(len(ours["nodes"]) == len(ref), "%s node count %d vs %d" % (label, len(ours["nodes"]), len(ref)))
    for a, b in zip(ours["nodes"], ref):
        check(a["pos"] == b["pos"], "%s position %s" % (label, b["pos"]))
        check(a["vt"] == b["vt"], "%s value type at %s" % (label, b["pos"]))
        check(a["rel"] == b["rel"], "%s relationship at %s" % (label, b["pos"]))
        check(a["concept"] == b["concept"], "%s concept at %s" % (label, b["pos"]))
        if b["num"] is not None:
            check(a["num"] is not None and abs(a["num"] - b["num"]) < 1e-9, "%s NUM at %s" % (label, b["pos"]))
        if b["text"] and b["vt"] in ("TEXT", "UIDREF", "DATE", "TIME", "DATETIME"):
            check(a["text"] == b["text"], "%s text at %s" % (label, b["pos"]))
    check(ours["warnings"] == 0, "%s read without warnings" % label)
    print("%s: %d SR nodes compared" % (label, len(ref)))
    return ours


def check_geometry(tool):
    for name in ("CT_small.dcm", "MR_small.dcm"):
        path = testdata(name)
        ds = pydicom.dcmread(path, force=True)
        ours = run(tool, "geometry", path)
        ipp = [float(v) for v in ds.ImagePositionPatient]
        iop = [float(v) for v in ds.ImageOrientationPatient]
        ps = [float(v) for v in ds.PixelSpacing]
        # Independent evaluation of PS3.3 equation C.7.6.2.1-1 for the last pixel.
        i, j = ds.Columns - 1, ds.Rows - 1
        last = [ipp[k] + iop[k] * ps[1] * i + iop[3 + k] * ps[0] * j for k in range(3)]
        check(all(abs(a - b) < 1e-9 for a, b in zip(ours["position"], ipp)), "%s position" % name)
        check(all(abs(a - b) < 1e-6 for a, b in zip(ours["last"], last)), "%s last pixel in patient space" % name)
        check(ours["spacing"] == ps, "%s pixel spacing" % name)
    print("geometry: CT_small / MR_small compared")


def lab_d50_to_srgb8(lab):
    """Independent CIELab (DICOM encoding, ICC PCS D50) -> 8-bit sRGB: CIE 15 inverse, Bradford
    D50 -> D65, IEC 61966-2-1. Written separately from the C++ code as an oracle."""
    L = lab[0] * 100.0 / 65535.0
    a = lab[1] * 255.0 / 65535.0 - 128.0
    b = lab[2] * 255.0 / 65535.0 - 128.0
    fy = (L + 16) / 116
    fx, fz = fy + a / 500, fy - b / 200
    e = 6 / 29
    finv = lambda t: t ** 3 if t > e else 3 * e * e * (t - 4 / 29)
    w50, w65 = np.array([0.96422, 1.0, 0.82521]), np.array([0.95047, 1.0, 1.08883])
    xyz = w50 * np.array([finv(fx), finv(fy), finv(fz)])
    m = np.array([[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]])
    adapt = np.linalg.inv(m) @ np.diag((m @ w65) / (m @ w50)) @ m
    rgb = np.array([[3.2406, -1.5372, -0.4986], [-0.9689, 1.8758, 0.0415], [0.0557, -0.2040, 1.0570]]) @ (adapt @ xyz)
    rgb = np.clip(rgb, 0, 1)
    rgb = np.where(rgb <= 0.0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - 0.055)
    return [int(round(v * 255)) for v in rgb]


# ---------------------------------------------------------------- synthetic source images
def make_ct_series(folder, n=4, rows=16, cols=12):
    study, series, forf = generate_uid(), generate_uid(), generate_uid()
    paths = []
    for k in range(n):
        ds = Dataset()
        ds.file_meta = FileMetaDataset()
        ds.file_meta.TransferSyntaxUID = ExplicitVRLittleEndian
        ds.file_meta.MediaStorageSOPClassUID = "1.2.840.10008.5.1.4.1.1.2"
        ds.SOPClassUID = "1.2.840.10008.5.1.4.1.1.2"
        ds.SOPInstanceUID = generate_uid()
        ds.file_meta.MediaStorageSOPInstanceUID = ds.SOPInstanceUID
        ds.StudyInstanceUID, ds.SeriesInstanceUID, ds.FrameOfReferenceUID = study, series, forf
        ds.PatientName, ds.PatientID, ds.PatientBirthDate, ds.PatientSex = "Test^Patient", "XCHK", "19700101", "O"
        ds.StudyDate, ds.StudyTime, ds.StudyID, ds.AccessionNumber, ds.ReferringPhysicianName = "20260301", "101500", "1", "A1", ""
        ds.Modality, ds.SeriesNumber, ds.InstanceNumber = "CT", 1, k + 1
        ds.Manufacturer = "X"
        ds.ImagePositionPatient = [-10.0, -20.0, 2.5 * k]
        ds.ImageOrientationPatient = [1, 0, 0, 0, 1, 0]
        ds.PixelSpacing = [0.75, 0.5]
        ds.SliceThickness = 2.5
        ds.Rows, ds.Columns = rows, cols
        ds.SamplesPerPixel, ds.PhotometricInterpretation = 1, "MONOCHROME2"
        ds.BitsAllocated, ds.BitsStored, ds.HighBit, ds.PixelRepresentation = 16, 16, 15, 1
        ds.RescaleIntercept, ds.RescaleSlope = -1024, 1
        ds.ImageType = ["ORIGINAL", "PRIMARY", "AXIAL"]
        ds.FrameOfReferenceUID = forf
        ds.PositionReferenceIndicator = ""
        ds.KVP = 120
        ds.AcquisitionNumber = 1
        ds.PixelData = (np.arange(rows * cols, dtype=np.int16) + 100 * k).tobytes()
        p = os.path.join(folder, "ct%d.dcm" % k)
        ds.save_as(p, enforce_file_format=True)
        paths.append(p)
    return paths


def check_sort(tool, paths):
    shuffled = [paths[2], paths[0], paths[3], paths[1]]
    ours = run(tool, "sort", *shuffled)
    check(ours["order"] == [1, 3, 0, 2], "series sort order")
    check(ours["warnings"] == 0 and ours["groups"] == 1, "series sort warnings")


def check_seg(tool, paths, tmp):
    import highdicom as hd
    from pydicom.sr.codedict import codes
    srcs = [pydicom.dcmread(p) for p in paths]
    rows, cols = srcs[0].Rows, srcs[0].Columns
    rng = np.random.default_rng(7)
    for seg_type in ("BINARY", "FRACTIONAL"):
        n_seg = 2
        if seg_type == "BINARY":
            arr = (rng.random((len(srcs), rows, cols, n_seg)) > 0.6).astype(np.uint8)
        else:
            arr = rng.random((len(srcs), rows, cols, 1)).astype(np.float32)
            n_seg = 1
        labs = [hd.color.CIELabColor.from_rgb(220, 40, 40), hd.color.CIELabColor.from_rgb(30, 200, 90)]
        descs = [hd.seg.SegmentDescription(segment_number=i + 1, segment_label="S%d" % (i + 1),
                                           segmented_property_category=codes.SCT.Tissue,
                                           segmented_property_type=codes.SCT.Liver,
                                           algorithm_type="MANUAL", display_color=labs[i])
                 for i in range(n_seg)]
        seg = hd.seg.Segmentation(source_images=srcs, pixel_array=arr, segmentation_type=seg_type,
                                  segment_descriptions=descs, series_instance_uid=generate_uid(), series_number=50,
                                  sop_instance_uid=generate_uid(), instance_number=1, manufacturer="highdicom",
                                  manufacturer_model_name="oracle", software_versions="1", device_serial_number="1",
                                  omit_empty_frames=False)
        p = os.path.join(tmp, "seg_%s.dcm" % seg_type.lower())
        seg.save_as(p)
        ours = run(tool, "seg", p)
        ds = pydicom.dcmread(p)
        check(ours["type"] == (0 if seg_type == "BINARY" else 1), "SEG %s type" % seg_type)
        check(len(ours["segments"]) == n_seg, "SEG %s segment count" % seg_type)
        for s, d in zip(ours["segments"], ds.SegmentSequence):
            check(s["label"] == d.SegmentLabel and s["number"] == d.SegmentNumber, "SEG %s segment label" % seg_type)
            lab = [int(v) for v in d.RecommendedDisplayCIELabValue]
            check(s["lab"] == lab, "SEG %s CIELab value" % seg_type)
            # PS3.3 says the value is in ICC PCS-Values (D50); we adapt to sRGB's D65 white with
            # Bradford. The oracle for that is an independent evaluation below; highdicom's
            # to_rgb() treats the value as D65-relative, so it is reported, not required.
            mine = lab_d50_to_srgb8(lab)
            check(all(abs(a - b) <= 1 for a, b in zip(s["rgb"], mine)),
                  "SEG %s CIELab->RGB %s vs independent D50/Bradford %s" % (seg_type, s["rgb"], mine))
            hd_rgb = hd.color.CIELabColor.from_dicom_value(lab).to_rgb()
            print("   segment %d: ours %s, independent D50 %s, highdicom (no D50->D65 adaptation) %s"
                  % (s["number"], s["rgb"], mine, list(hd_rgb)))
        px = ds.pixel_array.reshape((int(ds.NumberOfFrames), rows, cols))
        check(len(ours["frames"]) == px.shape[0], "SEG %s frame count" % seg_type)
        src_uids = [s.SOPInstanceUID for s in srcs]
        shared = ds.SharedFunctionalGroupsSequence[0]
        for k, (f, item) in enumerate(zip(ours["frames"], ds.PerFrameFunctionalGroupsSequence)):
            sid = item if "SegmentIdentificationSequence" in item else shared
            check(f["segment"] == sid.SegmentIdentificationSequence[0].ReferencedSegmentNumber, "SEG %s frame segment" % seg_type)
            want = item.DerivationImageSequence[0].SourceImageSequence[0].ReferencedSOPInstanceUID
            check(f["sources"] == [want] and want in src_uids, "SEG %s frame source" % seg_type)
            check(abs(f["z"] - float(item.PlanePositionSequence[0].ImagePositionPatient[2])) < 1e-9, "SEG %s frame z" % seg_type)
            check(f["mask"] == [int(v) for v in px[k].ravel()], "SEG %s frame %d mask" % (seg_type, k))
        print("SEG %s (highdicom): %d frames, %d segments compared" % (seg_type, px.shape[0], n_seg))


def check_hd_gsps(tool, paths, tmp):
    import highdicom as hd
    src = pydicom.dcmread(paths[0])
    layer = hd.pr.GraphicLayer(layer_name="LAYER1", order=1, display_color=hd.color.CIELabColor.from_rgb(255, 0, 0))
    gobs = [hd.pr.GraphicObject(graphic_type="POLYLINE", graphic_data=np.array([[1.0, 2.0], [5.5, 6.25], [9.0, 2.0]]), units="PIXEL"),
            hd.pr.GraphicObject(graphic_type="CIRCLE", graphic_data=np.array([[6.0, 6.0], [8.0, 6.0]]), units="PIXEL", is_filled=True),
            hd.pr.GraphicObject(graphic_type="ELLIPSE", graphic_data=np.array([[0.1, 0.5], [0.9, 0.5], [0.5, 0.3], [0.5, 0.7]]), units="DISPLAY"),
            hd.pr.GraphicObject(graphic_type="POINT", graphic_data=np.array([[3.0, 4.0]]), units="PIXEL")]
    texts = [hd.pr.TextObject(text_value="hello", units="PIXEL", anchor_point=(4.0, 4.0))]
    ann = hd.pr.GraphicAnnotation(referenced_images=[src], graphic_layer=layer, graphic_objects=gobs, text_objects=texts)
    pr = hd.pr.GrayscaleSoftcopyPresentationState(
        referenced_images=[src], series_instance_uid=generate_uid(), series_number=60, sop_instance_uid=generate_uid(),
        instance_number=1, manufacturer="highdicom", manufacturer_model_name="oracle", software_versions="1",
        device_serial_number="1", content_label="HDTEST", graphic_annotations=[ann], graphic_layers=[layer],
        voi_lut_transformations=[hd.pr.SoftcopyVOILUTTransformation(window_center=40.0, window_width=400.0)])
    p = os.path.join(tmp, "gsps_hd.dcm")
    pr.save_as(p)
    ours = run(tool, "gsps", p)
    check(ours["label"] == "HDTEST", "highdicom GSPS label")
    check(ours["refs"] == [src.SOPInstanceUID], "highdicom GSPS references")
    check(ours["voi"] == [[40.0, 400.0]], "highdicom GSPS VOI")
    check(ours["layers"] == ["LAYER1"], "highdicom GSPS layers")
    check(len(ours["annotations"]) == 1, "highdicom GSPS annotation count")
    g = ours["annotations"][0]["graphics"]
    check([x["type"] for x in g] == ["POLYLINE", "CIRCLE", "ELLIPSE", "POINT"], "highdicom GSPS graphic types")
    check(g[0]["points"] == [[1.0, 2.0], [5.5, 6.25], [9.0, 2.0]], "highdicom GSPS polyline points")
    check(g[1]["filled"] and g[2]["units"] == "DISPLAY", "highdicom GSPS filled / units")
    check(ours["annotations"][0]["texts"] == ["hello"], "highdicom GSPS text")
    check(ours["warnings"] == 0, "highdicom GSPS read without warnings")
    print("GSPS written by highdicom: read and compared")


def check_hd_sr(tool, paths, tmp):
    import highdicom as hd
    from pydicom.sr.codedict import codes
    src = pydicom.dcmread(paths[0])
    observer = hd.sr.ObserverContext(observer_type=codes.DCM.Person,
                                     observer_identifying_attributes=hd.sr.PersonObserverIdentifyingAttributes(name="Doe^John"))
    ctx = hd.sr.ObservationContext(observer_person_context=observer)
    region = hd.sr.ImageRegion(graphic_type=hd.sr.GraphicTypeValues.CIRCLE,
                               graphic_data=np.array([[6.0, 6.0], [8.0, 8.0]]),
                               source_image=hd.sr.SourceImageForRegion.from_source_image(src))
    tracking = hd.sr.TrackingIdentifier(identifier="lesion1", uid=generate_uid())
    meas = hd.sr.Measurement(name=codes.SCT.Area, value=12.25, unit=hd.sr.CodedConcept("mm2", "UCUM", "square millimeter"))
    group = hd.sr.PlanarROIMeasurementsAndQualitativeEvaluations(tracking_identifier=tracking, referenced_region=region,
                                                                 measurements=[meas])
    report = hd.sr.MeasurementReport(observation_context=ctx, procedure_reported=codes.LN.CTUnspecifiedBodyRegion,
                                     imaging_measurements=[group])
    sr = hd.sr.Comprehensive3DSR(evidence=[src], content=report, series_instance_uid=generate_uid(), series_number=70,
                                 sop_instance_uid=generate_uid(), instance_number=1, manufacturer="highdicom")
    p = os.path.join(tmp, "sr_hd.dcm")
    sr.save_as(p)
    ours = check_sr_file(tool, p, "highdicom Comprehensive 3D SR")
    nums = [n for n in ours["nodes"] if n["vt"] == "NUM"]
    check(len(nums) == 1 and nums[0]["num"] == 12.25 and nums[0]["units"][0] == "mm2", "highdicom SR measurement")
    scoords = [n for n in ours["nodes"] if n["vt"] == "SCOORD"]
    check(len(scoords) == 1 and scoords[0]["graphic"] == 4, "highdicom SR SCOORD")


def dciodvfy(path, label):
    exe = shutil.which("dciodvfy")
    if not exe:
        print("dciodvfy not installed: IOD validation of %s skipped" % label)
        return None
    out = subprocess.run([exe, "-new", path], capture_output=True, text=True)
    text = out.stdout + out.stderr
    errors = [l for l in text.splitlines() if l.startswith("Error")]
    warnings = [l for l in text.splitlines() if l.startswith("Warning")]
    for e in errors:
        print("   ", e)
    check(not errors, "dciodvfy %s: %d errors" % (label, len(errors)))
    print("dciodvfy %s: %d errors, %d warnings" % (label, len(errors), len(warnings)))
    for w in warnings:
        print("   ", w)
    return errors


def check_our_kos(tool, paths, tmp):
    import highdicom as hd
    p = os.path.join(tmp, "kos_ours.dcm")
    subprocess.run([tool, "write-kos", p, *paths[:3]], check=True)
    ds = pydicom.dcmread(p)
    doc = hd.ko.KeyObjectSelectionDocument.from_dataset(ds)
    kos = doc.content
    title = ds.ConceptNameCodeSequence[0]
    check((title.CodeValue, title.CodingSchemeDesignator, title.CodeMeaning) == ("113000", "DCM", "Of Interest"), "KOS title")
    refs = kos.get_references()
    uids = sorted(r.ReferencedSOPSequence[0].ReferencedSOPInstanceUID for r in refs)
    want = sorted(pydicom.dcmread(x).SOPInstanceUID for x in paths[:3])
    check(uids == want, "highdicom reads our KOS references")
    ev = ds.CurrentRequestedProcedureEvidenceSequence
    check(len(ev) == 1 and len(ev[0].ReferencedSeriesSequence) == 1 and
          len(ev[0].ReferencedSeriesSequence[0].ReferencedSOPSequence) == 3, "our KOS evidence (pydicom)")
    check(isinstance(kos, hd.ko.KeyObjectSelection), "highdicom parses our KOS content as TID 2010")
    ours = run(tool, "sr", p)
    check(len(ours["nodes"]) == 5, "our KOS tree")
    print("KOS written by us: read by highdicom")
    dciodvfy(p, "our KOS")


def check_our_gsps(tool, paths, tmp):
    p = os.path.join(tmp, "gsps_ours.dcm")
    subprocess.run([tool, "write-gsps", p, paths[0]], check=True)
    ds = pydicom.dcmread(p)
    src = pydicom.dcmread(paths[0])
    check(ds.SOPClassUID == "1.2.840.10008.5.1.4.1.1.11.1" and ds.Modality == "PR", "our GSPS SOP class / modality")
    check(ds.ReferencedSeriesSequence[0].ReferencedImageSequence[0].ReferencedSOPInstanceUID == src.SOPInstanceUID, "our GSPS reference")
    check(ds.StudyInstanceUID == src.StudyInstanceUID and ds.PatientID == src.PatientID, "our GSPS patient / study copied")
    ga = ds.GraphicAnnotationSequence[0]
    types = [g.GraphicType for g in ga.GraphicObjectSequence]
    check(types == ["POLYLINE", "POLYLINE", "CIRCLE", "ELLIPSE", "POINT"], "our GSPS graphic types (pydicom)")
    check(list(ga.GraphicObjectSequence[0].GraphicData) == [10.0, 10.0, 40.0, 30.0], "our GSPS graphic data (pydicom)")
    check(ga.TextObjectSequence[0].UnformattedTextValue == "36.1 mm", "our GSPS text (pydicom)")
    check(float(ds.SoftcopyVOILUTSequence[0].WindowWidth) == 400.0, "our GSPS window (pydicom)")
    check(list(ds.DisplayedAreaSelectionSequence[0].DisplayedAreaBottomRightHandCorner) == [src.Columns, src.Rows], "our GSPS displayed area")
    print("GSPS written by us: read by pydicom")
    dciodvfy(p, "our GSPS")


def check_suv(tool, tmp):
    ds = Dataset()
    ds.file_meta = FileMetaDataset()
    ds.file_meta.TransferSyntaxUID = ExplicitVRLittleEndian
    ds.SOPClassUID = "1.2.840.10008.5.1.4.1.1.128"
    ds.SOPInstanceUID = generate_uid()
    ds.file_meta.MediaStorageSOPClassUID = ds.SOPClassUID
    ds.file_meta.MediaStorageSOPInstanceUID = ds.SOPInstanceUID
    ds.Units, ds.DecayCorrection, ds.CorrectedImage = "BQML", "START", ["ATTN", "DECY"]
    ds.PatientWeight, ds.PatientSize, ds.PatientSex = 82.3, 1.71, "F"
    ds.SeriesDate, ds.SeriesTime = "20260515", "141203.5"
    ds.AcquisitionDate, ds.AcquisitionTime = "20260515", "141500"
    rp = Dataset()
    rp.RadionuclideTotalDose, rp.RadionuclideHalfLife = 296000000, 6586.2
    rp.RadiopharmaceuticalStartDateTime = "20260515125510"
    ds.RadiopharmaceuticalInformationSequence = [rp]
    p = os.path.join(tmp, "pet.dcm")
    ds.save_as(p, enforce_file_format=True)
    ours = run(tool, "suv", p)
    # Independent QIBA body-weight SUV: decay from injection to Series Date/Time.
    t = (14 * 3600 + 12 * 60 + 3.5) - (12 * 3600 + 55 * 60 + 10)
    want = 82.3 * 1000 / (296000000 * 2 ** (-t / 6586.2))
    check(ours["decay_seconds"] == t, "SUV decay time")
    check(abs(ours["bw"] - want) <= 1e-12 * want, "SUV body weight factor %r vs %r" % (ours["bw"], want))
    w, h = 82.3, 171.0
    james = 1.07 * w - 148 * (w / h) ** 2
    check(abs(ours["lbm_james"] - james * 1000 / (296000000 * 2 ** (-t / 6586.2))) <= 1e-12 * want, "SUV LBM James")
    print("SUV: body weight / LBM factors compared with an independent evaluation")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    args = ap.parse_args()
    tool = os.path.abspath(args.tool)
    check_rtstruct(tool)
    check_sr_file(tool, testdata("test-SR.dcm"), "pydicom test-SR.dcm")
    check_sr_file(tool, testdata("reportsi.dcm"), "pydicom reportsi.dcm")
    check_geometry(tool)
    tmp = tempfile.mkdtemp(prefix="siaara_clinical_")
    try:
        paths = make_ct_series(tmp)
        check_sort(tool, paths)
        check_suv(tool, tmp)
        try:
            import highdicom  # noqa: F401
            have_hd = True
        except ImportError:
            have_hd = False
            print("highdicom not installed: SEG / GSPS / SR / KOS oracle checks skipped")
        if have_hd:
            check_seg(tool, paths, tmp)
            check_hd_gsps(tool, paths, tmp)
            check_hd_sr(tool, paths, tmp)
            check_our_kos(tool, paths, tmp)
        check_our_gsps(tool, paths, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d checks passed, %d failed" % (PASSES, len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
