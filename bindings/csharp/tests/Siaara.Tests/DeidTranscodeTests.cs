using System;
using System.IO;
using System.Linq;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class DeidTranscodeTests
{
    private static readonly byte[] s_key = Enumerable.Range(1, 32).Select(i => (byte)i).ToArray();

    internal static void Register()
    {
        Case("deid: basic profile removes identity, replaces UIDs repeatably, reports flags", () =>
        {
            using var deid = new Deidentifier(s_key);
            string expectedSop = deid.GetReplacementUid("2.25.100");
            Check(expectedSop.Length > 0 && expectedSop != "2.25.100");
            CheckEq(deid.GetReplacementUid("2.25.100"), expectedSop);
            using DicomFile f = Builders.Smoke();
            f.Dataset.SetString(DicomTags.PatientBirthDate, "19700101");
            f.Dataset.SetString(DicomTags.InstitutionName, "Apulki Hospital");
            f.Dataset.SetString(DicomTags.BurnedInAnnotation, "NO");
            DeidentificationResults r = deid.Deidentify(f);
            DicomDataset ds = f.Dataset;
            Check(ds.GetString(DicomTags.PatientName) != "Doe^Jane");
            Check(!ds.Contains(DicomTags.InstitutionName) || ds.GetString(DicomTags.InstitutionName) != "Apulki Hospital");
            CheckEq(ds.GetRawString(DicomTags.SOPInstanceUID), expectedSop);
            Check(!r.HasFlag(DeidentificationResults.NotFullyDeidentified));
            Check(ds.GetString(DicomTags.PatientIdentityRemoved) == "YES");
        });

        Case("deid: retain UIDs option, burned-in flag, region cleaning, key validation", () =>
        {
            using var deid = new Deidentifier(s_key) { Options = DeidentificationOptions.RetainUids };
            using DicomFile f = Builders.Smoke();
            DeidentificationResults r = deid.Deidentify(f);
            CheckEq(f.Dataset.GetRawString(DicomTags.SOPInstanceUID), "2.25.100");
            Check(r.HasFlag(DeidentificationResults.BurnedInAnnotationRemains));
            Check(r.HasFlag(DeidentificationResults.NotFullyDeidentified));

            using var cleaner = new Deidentifier(s_key) { PixelDataClean = true };
            cleaner.CleanRegions.Add(new PixelRegion(0, 0, 1, 1));
            using DicomFile g = Builders.Smoke();
            DeidentificationResults r2 = cleaner.Deidentify(g);
            Check(r2.HasFlag(DeidentificationResults.PixelDataCleaned));
            byte[] px = g.DecodeFrame(0);
            Check(px[0] != 0x00 || px[1] != 0x04); // first pixel blanked
            CheckEq(px[2], (byte)0x32);            // second pixel untouched
            Throws<ArgumentException>(() => new Deidentifier(new byte[8]).Dispose());
            cleaner.Dispose();
            Throws<ObjectDisposedException>(() => cleaner.Deidentify(g));
        });

        Case("transcode: RLE, JPEG-LS, JPEG lossless and JPEG 2000 lossless round trips", () =>
        {
            var px = new short[16 * 16];
            for (int i = 0; i < px.Length; i++)
            {
                px[i] = (short)((i * 37) % 3000 - 500);
            }
            string[] syntaxes =
            {
                DicomTransferSyntaxes.RLELossless,
                DicomTransferSyntaxes.JPEGLSLossless,
                DicomTransferSyntaxes.JPEGLosslessSV1,
                DicomTransferSyntaxes.JPEG2000Lossless,
                DicomTransferSyntaxes.DeflatedExplicitVRLittleEndian,
            };
            string dir = NewTempDir("transcode");
            foreach (string ts in syntaxes)
            {
                // Signed samples go through every lossless codec, JPEG-LS included (two's
                // complement bit patterns, sign-extended again by the decoder).
                using DicomFile f = Builders.Ct(16, 16, px);
                byte[] original = f.DecodeFrame(0);
                f.Transcode(ts);
                CheckEq(f.TransferSyntaxUid, ts);
                DicomPixelInfo pi = f.GetPixelInfo();
                CheckEq(pi.IsEncapsulated, ts != DicomTransferSyntaxes.DeflatedExplicitVRLittleEndian);
                Check(!pi.IsLossy);
                Check(f.DecodeFrame(0).AsSpan().SequenceEqual(original));
                string path = Path.Combine(dir, ts + ".dcm");
                f.Save(path);
                using DicomFile back = DicomFile.Open(path);
                CheckEq(back.TransferSyntaxUid, ts);
                Check(back.DecodeFrame(0).AsSpan().SequenceEqual(original));
                CheckEq(back.RenderRgba8().Length, 16 * 16 * 4);
                // back to native
                back.Transcode(DicomTransferSyntaxes.ExplicitVRLittleEndian);
                Check(!back.GetPixelInfo().IsEncapsulated);
                Check(back.DecodeFrame(0).AsSpan().SequenceEqual(original));
            }
        });

        Case("transcode: lossy needs permission; parameters; failures leave the file unchanged", () =>
        {
            using DicomFile f = Builders.Rgb(16, 16);
            var e = Throws<SiaaraException>(() => f.Transcode(DicomTransferSyntaxes.JPEGBaseline8Bit, new TranscodeParameters()));
            CheckEq(e?.ErrorCode, SiaaraErrorCode.Rejected);
            CheckEq(f.TransferSyntaxUid, DicomTransferSyntaxes.ExplicitVRLittleEndian);
            f.Transcode(DicomTransferSyntaxes.JPEGBaseline8Bit, new TranscodeParameters { AllowLossy = true, Quality = 95 });
            DicomPixelInfo pi = f.GetPixelInfo();
            Check(pi.IsLossy && pi.IsEncapsulated);
            CheckEq(f.DecodeFrame(0).Length, 16 * 16 * 3);
            CheckEq(Transcoder.TryTranscode(f, "1.2.3.4.5.6"), SiaaraErrorCode.Unsupported);
            using DicomFile g = Builders.Smoke();
            g.Dataset.SetUInt16(DicomTags.PixelRepresentation, 0);
            g.Transcode(DicomTransferSyntaxes.JPEGLSNearLossless, new TranscodeParameters { AllowLossy = true, Lossy = true, NearLossless = 2 });
            Check(g.GetPixelInfo().IsLossy);
            short[] back = new short[4];
            System.Runtime.InteropServices.MemoryMarshal.Cast<byte, short>(g.DecodeFrame(0)).CopyTo(back);
            Check(Math.Abs(back[1] - 1074) <= 2);
        });

        Case("transcode: lossy makes a new SOP Instance UID unless KeepUidOnLossy (owner decision 2026-11-10)", () =>
        {
            using DicomFile f = Builders.Rgb(16, 16);
            f.Dataset.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.4242");
            f.Transcode(DicomTransferSyntaxes.JPEGBaseline8Bit, new TranscodeParameters { AllowLossy = true });
            Check(f.Dataset.GetString(DicomTags.SOPInstanceUID) != "2.25.4242");
            CheckEq(f.FileMetaInformation.GetString(new DicomTag(0x00020003)), f.Dataset.GetString(DicomTags.SOPInstanceUID));
            using DicomFile g = Builders.Rgb(16, 16);
            g.Dataset.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.4243");
            g.Transcode(DicomTransferSyntaxes.JPEGBaseline8Bit, new TranscodeParameters { AllowLossy = true, KeepUidOnLossy = true });
            CheckEq(g.Dataset.GetString(DicomTags.SOPInstanceUID), "2.25.4243");
        });

        Case("transcode: lossy history lists stay aligned or the mismatch is reported (owner decision 2026-11-10)", () =>
        {
            var ratio = new DicomTag(0x00282112);
            var method = new DicomTag(0x00282114);
            var lossy = new TranscodeParameters { AllowLossy = true, Lossy = true };
            // Aligned (neither present): one value each.
            using DicomFile a = Builders.Rgb(16, 16);
            TranscodeReport ra = Transcoder.TranscodeWithReport(a, DicomTransferSyntaxes.JPEGBaseline8Bit, lossy);
            Check(ra.LossyApplied && ra.PixelDataRecoded && !ra.SourceLossy);
            CheckEq(ra.LossyHistory, LossyHistory.Aligned);
            CheckEq(ra.LossyHistoryNote, string.Empty);
            CheckEq(ra.NewSopInstanceUid, a.Dataset.GetString(DicomTags.SOPInstanceUID));
            CheckEq(a.Dataset.GetString(method), "ISO_10918_1");
            // One Ratio, no Method, JPEG source: the syntax proves ISO_10918_1.
            a.Dataset.Remove(method);
            a.Dataset.SetRawString(ratio, "DS", "7");
            TranscodeReport rf = Transcoder.TranscodeWithReport(a, DicomTransferSyntaxes.JPEG2000, lossy);
            CheckEq(rf.LossyHistory, LossyHistory.MethodFilled);
            CheckEq(rf.LossyMethodFilled, "ISO_10918_1");
            Check(rf.LossyHistoryNote.StartsWith("LOSSY_METHOD_FILLED: ", StringComparison.Ordinal));
            Check(rf.SourceLossy);
            CheckEq(a.Dataset.GetString(method, 0), "ISO_10918_1");
            CheckEq(a.Dataset.GetString(method, 1), "ISO_15444_1");
            CheckEq(a.Dataset.GetString(ratio, 0), "7");
            // One Ratio, no Method, uncompressed source: nothing proves the method; Method stays absent.
            using DicomFile m = Builders.Rgb(16, 16);
            m.Dataset.SetRawString(ratio, "DS", "7");
            TranscodeReport rm = Transcoder.TranscodeWithReport(m, DicomTransferSyntaxes.JPEGBaseline8Bit,
                new TranscodeParameters { AllowLossy = true, KeepUidOnLossy = true });
            CheckEq(rm.LossyHistory, LossyHistory.Misaligned);
            CheckEq(rm.LossyMethodFilled, string.Empty);
            Check(rm.LossyHistoryNote.StartsWith("LOSSY_HISTORY_MISALIGNED: ", StringComparison.Ordinal));
            Check(!m.Dataset.Contains(method));
            CheckEq(m.Dataset.GetString(ratio, 0), "7");
            Check(m.Dataset.GetString(ratio, 1).Length > 0);
            CheckEq(rm.NewSopInstanceUid, string.Empty);
            // No lossy step: not applicable.
            using DicomFile n = Builders.Rgb(16, 16);
            TranscodeReport rn = Transcoder.TranscodeWithReport(n, DicomTransferSyntaxes.RLELossless);
            CheckEq(rn.LossyHistory, LossyHistory.NotApplicable);
            Check(!rn.LossyApplied);
        });
    }
}
