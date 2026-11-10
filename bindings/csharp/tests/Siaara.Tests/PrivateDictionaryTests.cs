using System;
using System.IO;
using System.Linq;
using System.Text;
using static Siaara.Tests.T;

namespace Siaara.Tests;

internal static class PrivateDictionaryTests
{
    internal static void Register()
    {
        Case("private dictionary: exact, fuzzy, conflict, assumed and blank creators", () =>
        {
            DicomPrivateDictionaryEntry e = DicomPrivateDictionary.Lookup("SIEMENS CSA HEADER", 0x0029, 0x10);
            CheckEq(e.VR, "OB");
            CheckEq(e.Keyword, "CSAImageHeaderInfo");
            CheckEq(e.Match, PrivateCreatorMatch.Exact);
            CheckEq(e.Confidence, PrivateConfidence.Stated);
            Check(e.SourceCount > 0 && e.SourcePage > 0 && e.SourceDocument.Length > 0);
            CheckEq(DicomPrivateDictionary.GetCandidates(e).Count, 1);

            Require(DicomPrivateDictionary.TryLookup("siemens csa  header", 0x0029, 0x10, out DicomPrivateDictionaryEntry? f));
            CheckEq(f!.Match, PrivateCreatorMatch.Fuzzy);
            CheckEq(f.Creator, "SIEMENS CSA HEADER");

            DicomPrivateDictionaryEntry c = DicomPrivateDictionary.Lookup("Applicare/RadWorks/Version 6.0", 0x4105, 0x10);
            Check(c.IsVRConflict);
            Check(c.VR is null);
            var cands = DicomPrivateDictionary.GetCandidates(c);
            CheckEq(cands.Count, c.CandidateCount);
            Check(cands.Any(x => x.VR == "CS") && cands.Any(x => x.VR == "IS") && cands.Any(x => x.VR == "US"));
            Check(cands.All(x => x.Document.Length > 0 && x.Page > 0));

            CheckEq(DicomPrivateDictionary.Lookup("GEMS_ACQU_01", 0x0019, 0x01).Confidence, PrivateConfidence.Stated);
            CheckEq(DicomPrivateDictionary.Lookup("CANON MDW NON-IMAGE", 0x7FE1, 0x10).Confidence, PrivateConfidence.Assumed);
            Check(!DicomPrivateDictionary.TryLookup("", 0x0019, 0x3A, out _));
            Check(!DicomPrivateDictionary.TryLookup("NOT A REAL CREATOR", 0x0029, 0x10, out _));
            var ex = Throws<SiaaraException>(() => DicomPrivateDictionary.Lookup("NOT A REAL CREATOR", 0x0029, 0x10));
            CheckEq(ex?.ErrorCode, SiaaraErrorCode.NotFound);
        });

        Case("private dictionary: implicit VR read gets the decided VR, explicit keeps its own", () =>
        {
            string dir = NewTempDir("privdict");
            string imp = Path.Combine(dir, "implicit.dcm");
            string exp = Path.Combine(dir, "explicit.dcm");
            foreach ((string path, string ts) in new[] { (imp, DicomTransferSyntaxes.ImplicitVRLittleEndian), (exp, DicomTransferSyntaxes.ExplicitVRLittleEndian) })
            {
                using DicomFile w = DicomFile.Create();
                w.Dataset.SetRawString(DicomTags.SOPClassUID, "UI", Builders.SecondaryCapture);
                w.Dataset.SetRawString(DicomTags.SOPInstanceUID, "UI", "2.25.77");
                w.Dataset.SetRawString(new DicomTag(0x00290010), "LO", "SIEMENS CSA HEADER");
                w.Dataset.SetBytes(new DicomTag(0x00291009), "UN", Encoding.ASCII.GetBytes("20100114"));
                w.Dataset.SetRawString(new DicomTag(0x41050010), "LO", "Applicare/RadWorks/Version 6.0");
                w.Dataset.SetBytes(new DicomTag(0x41051010), "UN", Encoding.ASCII.GetBytes("12"));
                w.Save(path, ts);
            }
            using (DicomFile f = DicomFile.Open(imp))
            {
                CheckEq(f.Dataset.GetVR(new DicomTag(0x00291009)), "LO");
                CheckEq(f.Dataset.GetString(new DicomTag(0x00291009)), "20100114");
                CheckEq(f.Dataset.GetVR(new DicomTag(0x41051010)), "UN");  // conflict: undecided
                Require(f.Dataset.TryGetPrivateDictionaryEntry(new DicomTag(0x00291009), out DicomPrivateDictionaryEntry? e));
                CheckEq(e!.Keyword, "CSAImageHeaderVersion");
                Check(!f.Dataset.TryGetPrivateDictionaryEntry(DicomTags.SOPClassUID, out _));
                Check(f.Dataset.ToJson(DicomJsonOptions.PrivateNames).Contains("\"keyword\":\"CSAImageHeaderVersion\"", StringComparison.Ordinal));
                Check(!f.Dataset.ToJson().Contains("CSAImageHeaderVersion", StringComparison.Ordinal));
                string again = Path.Combine(dir, "again.dcm");
                f.Save(again);
                Check(File.ReadAllBytes(again).AsSpan().SequenceEqual(File.ReadAllBytes(imp)));
            }
            using (DicomFile f = DicomFile.Open(exp))
            {
                CheckEq(f.Dataset.GetVR(new DicomTag(0x00291009)), "UN");  // the file's own VR wins
                Require(f.Dataset.TryGetPrivateDictionaryEntry(new DicomTag(0x00291009), out DicomPrivateDictionaryEntry? e));
                CheckEq(e!.VR, "LO");
            }
        });
    }
}
