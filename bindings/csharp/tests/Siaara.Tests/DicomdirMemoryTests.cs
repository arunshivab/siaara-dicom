using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using static Siaara.Tests.T;

namespace Siaara.Tests;

/// <summary>Dicomdir.Read from bytes (siaara_dicomdir_read_memory).</summary>
internal static class DicomdirMemoryTests
{
    internal static void Register()
    {
        Case("dicomdir memory: Dicomdir.Read from bytes equals the path reader, File IDs as text; limits", () =>
        {
            string src = NewTempDir("ddsrc");
            var files = new List<string>();
            for (int i = 0; i < 3; i++)
            {
                using DicomFile f = Builders.Ct(2, 2, new short[] { 1, 2, 3, 4 }, "2.25.50" + i, "2.25.60", "2.25.70");
                string p = Path.Combine(src, "i" + i + ".dcm");
                f.Save(p, DicomTransferSyntaxes.ExplicitVRLittleEndian);
                files.Add(p);
            }
            string root = NewTempDir("ddroot");
            Dicomdir.Write(root, files);
            string dd = Path.Combine(root, "DICOMDIR");
            byte[] bytes = File.ReadAllBytes(dd);
            var byPath = Dicomdir.ReadAll(dd);
            var byMem = Dicomdir.ReadAll(bytes);
            Require(byPath.Count == byMem.Count);
            CheckEq(byMem.Count, 6);
            for (int i = 0; i < byPath.Count; i++)
            {
                CheckEq(byMem[i].Index, byPath[i].Index);
                CheckEq(byMem[i].Depth, byPath[i].Depth);
                CheckEq(byMem[i].Keys.SopInstanceUid, byPath[i].Keys.SopInstanceUid);
                CheckEq(byMem[i].Keys.PatientId, byPath[i].Keys.PatientId);
                CheckEq(byPath[i].FileId, null);
                if (byPath[i].Keys.GetString(IndexField.RecordType) == "IMAGE")
                {
                    Require(byMem[i].FileId is not null);
                    CheckEq(byMem[i].FileStatus, SiaaraErrorCode.Ok);
                    Check(byMem[i].FileId!.StartsWith("DICOM/", StringComparison.Ordinal));
                    Check(File.Exists(Path.Combine(root, byMem[i].FileId!)));
                }
            }
            DicomdirSummary s = Dicomdir.Read(bytes, _ => { }, new DicomdirReadOptions { MaxRecords = 6 });
            CheckEq(s.Files, 3);
            var e = Throws<SiaaraException>(() => Dicomdir.Read(bytes, _ => { }, new DicomdirReadOptions { MaxRecords = 5 }));
            CheckEq(e?.ErrorCode, SiaaraErrorCode.LimitExceeded);
            e = Throws<SiaaraException>(() => Dicomdir.Read(bytes, _ => { }, new DicomdirReadOptions { MaxBytes = bytes.Length - 1 }));
            CheckEq(e?.ErrorCode, SiaaraErrorCode.LimitExceeded);
            Throws<ArgumentOutOfRangeException>(() => Dicomdir.ReadAll(bytes, new DicomdirReadOptions { MaxRecords = -1 }));
            e = Throws<SiaaraException>(() => Dicomdir.ReadAll(new byte[] { 1, 2, 3 }));
            CheckEq(e?.ErrorCode, SiaaraErrorCode.NotDicom);
            // The path reader keeps no limit by default and takes the new ones when asked.
            CheckEq(Dicomdir.ReadAll(dd, new DicomdirReadOptions()).Count, 6);
            Throws<SiaaraException>(() => Dicomdir.ReadAll(dd, new DicomdirReadOptions { MaxRecords = 5 }));
            CheckEq(DicomdirReadOptions.MemoryDefaultMaxRecords, 100_000L);
            CheckEq(DicomdirReadOptions.MemoryDefaultMaxBytes, 64L << 20);
            CheckEq(DicomdirReadOptions.MemoryDefaultMaxValueBytes, 65_536L);
            Throws<ArgumentOutOfRangeException>(() => Dicomdir.ReadAll(bytes, new DicomdirReadOptions { MaxValueBytes = -1 }));
            // a cap below the Referenced File IDs (about 30 bytes): the File IDs are rejected, not cut
            var capped = Dicomdir.ReadAll(bytes, new DicomdirReadOptions { MaxValueBytes = 8 });
            Check(capped.Where(x => x.Keys.GetString(IndexField.RecordType) == "IMAGE").All(x => x.FileStatus == SiaaraErrorCode.Rejected));
            // a cut DICOMDIR: the last record is dropped, the rest is read
            var cut = Dicomdir.ReadAll(bytes.AsSpan(0, (int)byMem.Max(x => x.Offset) + 40));
            Check(cut.Count < byMem.Count);
        });

    }
}
