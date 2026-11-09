// Store / scan benchmark (test-only, not part of ctest).
//
//   store_perf [--files N] [--records M] [--worst | --worst-only] [--threads T]
//
// 1. writes N small synthetic DICOM files (default 20,000) to a temporary folder,
// 2. scans them (header-only extraction) with 1, 2 and T threads,
// 3. adds them to stores (copy mode on 1 and T threads, reference mode on T threads),
// 4. bulk-indexes M synthetic records (default 1,000,000) and times typical viewer queries,
//    compaction and reopening; --worst also times a store with one instance per study.
// Numbers are wall-clock on whatever machine runs it, with the files in the OS cache.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "store_synth.hpp"
#include "store/index.hpp"
#include "store/scan.hpp"
#include "store/store.hpp"

using namespace siaara::store;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

std::string rss() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return line.substr(6);
    return " n/a";
}

const char* kSyl[] = {"ka", "ra", "an", "sh", "mi", "to", "le", "son", "ber", "gu", "ch", "\xC3\xA9", "\xC3\xBC",
                      "ya", "ma", "da", "ri", "na", "vo", "lo", "pe", "ng", "th", "ul"};

std::string name_of(std::mt19937& rng) {
    std::uniform_int_distribution<int> syl(0, static_cast<int>(std::size(kSyl)) - 1), len(2, 4);
    auto word = [&] {
        std::string w;
        const int n = len(rng);
        for (int i = 0; i < n; ++i) w += kSyl[syl(rng)];
        w[0] = static_cast<char>(w[0] >= 'a' && w[0] <= 'z' ? w[0] - 32 : w[0]);
        return w;
    };
    return word() + "^" + word();
}

struct Timing {
    double best = 1e9;
    std::size_t rows = 0, total = 0;
};

Timing time_query(const Store& st, const QueryFilter& q, int reps = 5) {
    Timing t;
    for (int i = 0; i < reps; ++i) {
        const auto a = Clock::now();
        auto r = st.query(q);
        const auto b = Clock::now();
        if (!r) {
            std::printf("  query failed: %s\n", r.error().message.c_str());
            return t;
        }
        t.best = std::min(t.best, secs(a, b));
        t.rows = r->rows.size();
        t.total = r->total;
    }
    return t;
}

void report(const char* what, const Timing& t) {
    std::printf("  %-58s %9.2f ms  (%zu rows of %zu)\n", what, t.best * 1e3, t.rows, t.total);
}

std::vector<IndexRecord> synth_records(std::size_t begin, std::size_t end, std::size_t per_series,
                                       std::size_t series_per_study, std::size_t studies_per_patient,
                                       std::uint32_t seed) {
    std::vector<IndexRecord> out;
    out.reserve(end - begin);
    const char* modalities[] = {"CT", "MR", "CR", "DX", "US", "PT", "NM", "MG"};
    const char* descs[] = {"CT CHEST", "MR BRAIN", "XR CHEST PA", "CT ABDOMEN", "US ABDOMEN", "PET CT WB", "MAMMO", "MR KNEE"};
    for (std::size_t i = begin; i < end; ++i) {
        const std::size_t series = i / per_series;
        const std::size_t study = series / series_per_study;
        const std::size_t patient = study / studies_per_patient;
        std::mt19937 prng(static_cast<std::uint32_t>(patient) * 2654435761u + seed);
        std::mt19937 srng(static_cast<std::uint32_t>(study) * 40503u + seed);
        IndexRecord r;
        r[Str::PatientId] = "PID" + std::to_string(patient);
        r[Str::PatientName] = name_of(prng);
        r[Str::PatientBirthDate] = std::to_string(19300101 + (prng() % 80) * 10000 + (prng() % 12) * 100 + prng() % 28);
        r[Str::PatientSex] = (prng() & 1) ? "M" : "F";
        r[Str::StudyInstanceUid] = "1.2.826.0.1.3680043.10." + std::to_string(study);
        const std::size_t m = srng() % 8;
        r[Str::StudyDate] = std::to_string(20000101 + (srng() % 26) * 10000 + (srng() % 12) * 100 + srng() % 28);
        r[Str::StudyTime] = "101500";
        r[Str::AccessionNumber] = "ACC" + std::to_string(study);
        r[Str::StudyDescription] = descs[m];
        r[Str::ReferringPhysician] = "Ref^Doctor" + std::to_string(srng() % 100);
        r[Str::SeriesInstanceUid] = r[Str::StudyInstanceUid] + "." + std::to_string(series % series_per_study);
        r[Str::Modality] = modalities[m];
        r[Str::SeriesDescription] = "Series " + std::to_string(series % series_per_study);
        r[Str::SopInstanceUid] = r[Str::SeriesInstanceUid] + "." + std::to_string(i % per_series);
        r[Str::SopClassUid] = "1.2.840.10008.5.1.4.1.1.2";
        r[Str::TransferSyntaxUid] = "1.2.840.10008.1.2.1";
        r[Str::PhotometricInterpretation] = "MONOCHROME2";
        r[Str::FilePath] = "/archive/" + std::to_string(patient) + "/" + std::to_string(i) + ".dcm";
        r.flags = kHasSeriesNumber | kHasInstanceNumber | kHasPosition | kHasOrientation | kHasPixelData;
        r.series_number = static_cast<std::int32_t>(series % series_per_study);
        r.instance_number = static_cast<std::int32_t>(i % per_series);
        r.rows = r.columns = 512;
        r.bits_allocated = 16;
        r.bits_stored = 12;
        r.number_of_frames = 1;
        r.position[2] = static_cast<double>(i % per_series);
        r.orientation[0] = r.orientation[4] = 1;
        r.file_size = 526000;
        out.push_back(std::move(r));
    }
    return out;
}

void bench_index(const fs::path& dir, std::size_t total, std::size_t per_series, std::size_t series_per_study,
                 std::size_t studies_per_patient, const char* label) {
    std::printf("\n== %zu-record index (%s: %zu instances/series, %zu series/study, %zu studies/patient)\n", total, label,
                per_series, series_per_study, studies_per_patient);
    StoreOptions o;
    o.compact_min_records = UINT64_MAX;  // time compaction separately
    auto st = Store::open((dir / "index").string(), o);
    if (!st) {
        std::printf("  open failed: %s\n", st.error().message.c_str());
        return;
    }
    auto t0 = Clock::now();
    const std::size_t batch = 50'000;
    for (std::size_t b = 0; b < total; b += batch) {
        auto recs = synth_records(b, std::min(total, b + batch), per_series, series_per_study, studies_per_patient, 7);
        auto r = (*st)->add_records(recs);
        if (!r) {
            std::printf("  add_records failed: %s\n", r.error().message.c_str());
            return;
        }
    }
    auto t1 = Clock::now();
    const auto s = (*st)->stats();
    std::printf("  bulk add (journal + in-memory index): %.2f s, %.0f records/s; %llu patients, %llu studies, %llu series\n",
                secs(t0, t1), static_cast<double>(total) / secs(t0, t1), static_cast<unsigned long long>(s.patients),
                static_cast<unsigned long long>(s.studies), static_cast<unsigned long long>(s.series));
    std::printf("  journal %.1f MB, process RSS%s\n", static_cast<double>(s.journal_bytes) / 1e6, rss().c_str());

    const Store& q = **st;
    QueryFilter f;
    f.level = Level::Study;
    f.limit = 100;
    report("studies, no filter, newest 100", time_query(q, f));
    f = {};
    f.level = Level::Patient;
    f.limit = 50;
    report("patients, no filter, first 50 by name", time_query(q, f));
    f = {};
    f.level = Level::Study;
    f.patient_name = "kara*";
    report("studies, patient name 'kara*' (prefix index)", time_query(q, f));
    f.patient_name = "*son*";
    f.limit = 100;
    report("studies, patient name '*son*' (scan), first 100", time_query(q, f));
    f.want_total = false;
    report("  same, without the total (early exit)", time_query(q, f));
    f.want_total = true;
    f.patient_name = "*\xC3\xA9*";  // é, accent-folded on both sides -> "*e*"
    report("studies, patient name '*\xC3\xA9*' (scan), first 100", time_query(q, f));
    f.want_total = false;
    report("  same, without the total (early exit)", time_query(q, f));
    f = {};
    f.level = Level::Study;
    f.patient_id = "PID1234";
    report("studies, patient ID exact", time_query(q, f));
    f.patient_id = "PID42*";
    report("studies, patient ID 'PID42*' (scan)", time_query(q, f));
    f.limit = 100;
    f.want_total = false;
    report("  same, first 100 without the total (early exit)", time_query(q, f));
    f = {};
    f.level = Level::Study;
    f.accession_number = "ACC1234";
    report("studies, accession exact", time_query(q, f));
    f = {};
    f.level = Level::Study;
    f.study_date = "20200101-20200131";
    report("studies, date range one month", time_query(q, f));
    f.study_date = "20200101-20201231";
    f.modality = "CT\\MR";
    report("studies, date range one year + modality CT or MR", time_query(q, f));
    f = {};
    f.level = Level::Study;
    f.study_description = "*chest*";
    f.limit = 100;
    report("studies, description '*chest*', first 100", time_query(q, f));
    f.want_total = false;
    report("  same, without the total (early exit)", time_query(q, f));
    f = {};
    f.level = Level::Series;
    f.study_uid = "1.2.826.0.1.3680043.10.777";
    report("series of one study", time_query(q, f));
    f = {};
    f.level = Level::Instance;
    f.series_uid = "1.2.826.0.1.3680043.10.777.0";
    report("instances of one series (ordered, with paths)", time_query(q, f));
    f = {};
    f.level = Level::Instance;
    f.sop_instance_uid = "1.2.826.0.1.3680043.10.777.0.3";
    report("one instance by SOP Instance UID", time_query(q, f));
    f = {};
    f.level = Level::Instance;
    f.modality = "MR";
    f.study_date = "20240101-20241231";
    f.limit = 1000;
    report("instances, MR in 2024, first 1000", time_query(q, f, 3));

    std::printf("  RSS before compaction%s\n", rss().c_str());
    t0 = Clock::now();
    if (auto c = (*st)->compact(); !c) std::printf("  compact failed: %s\n", c.message.c_str());
    t1 = Clock::now();
    std::printf("  RSS after compaction%s\n", rss().c_str());
    std::printf("  compaction (snapshot write + fsync): %.2f s, snapshot %.1f MB\n", secs(t0, t1),
                static_cast<double>((*st)->stats().snapshot_bytes) / 1e6);
    st->reset();
    t0 = Clock::now();
    auto again = Store::open((dir / "index").string(), o);
    t1 = Clock::now();
    if (again) std::printf("  reopen (load snapshot, rebuild in-memory index): %.2f s, %llu instances, RSS%s\n", secs(t0, t1),
                           static_cast<unsigned long long>((*again)->stats().instances), rss().c_str());
    std::error_code ec;
    if (again) again->reset();
    fs::remove_all(dir / "index", ec);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t nfiles = 20'000, nrecords = 1'000'000;
    unsigned threads = std::max(2u, std::thread::hardware_concurrency());
    bool worst = false, worst_only = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--files") && i + 1 < argc) nfiles = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--records") && i + 1 < argc) nrecords = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--worst")) worst = true;
        else if (!std::strcmp(argv[i], "--worst-only")) worst = worst_only = true;
    }
    std::printf("store_perf: %u hardware threads\n", std::thread::hardware_concurrency());
    store_synth::TempDir td("siaara-perf");

    if (nfiles) {
        std::printf("\n== %zu synthetic files (64x64 16-bit, ~9 KB each)\n", nfiles);
        auto t0 = Clock::now();
        std::vector<std::string> paths;
        paths.reserve(nfiles);
        for (std::size_t i = 0; i < nfiles; ++i) {
            store_synth::Spec s;
            const std::size_t series = i / 20, study = series / 5, patient = study / 4;
            s.patient_id = "P" + std::to_string(patient);
            s.patient_name = "Perf^" + std::to_string(patient);
            s.study_uid = "1.2.826.0.1.77." + std::to_string(study);
            s.series_uid = s.study_uid + "." + std::to_string(series % 5);
            s.sop_uid = s.series_uid + "." + std::to_string(i % 20);
            s.series_number = static_cast<int>(series % 5);
            s.instance_number = static_cast<int>(i % 20);
            s.rows = s.columns = 64;
            const fs::path p = td / ("gen/d" + std::to_string(i / 500) + "/IM" + std::to_string(i));
            store_synth::write_spec(p, s);
            paths.push_back(p.string());
        }
        auto t1 = Clock::now();
        std::printf("  generated in %.2f s\n", secs(t0, t1));

        for (unsigned t : {1u, 2u, threads}) {
            ScanOptions o;
            o.threads = t;
            std::atomic<std::uint64_t> n{0};
            t0 = Clock::now();
            auto r = scan_folder((td / "gen").string(), [&](const ScanItem& it) {
                if (it.kind == ScanKind::Instance) ++n;
                return true;
            }, o);
            t1 = Clock::now();
            if (!r) std::printf("  scan failed: %s\n", r.error().message.c_str());
            std::printf("  scan, %u thread(s): %.2f s, %.0f files/s (%llu instances)\n", t, secs(t0, t1),
                        static_cast<double>(nfiles) / secs(t0, t1), static_cast<unsigned long long>(n.load()));
        }

        auto add_all = [&](const char* label, unsigned t, AddMode mode, bool durable, std::size_t limit) {
            StoreOptions so;
            so.durable = durable;
            const fs::path root = td / (std::string("store-") + label);
            auto st = Store::open(root.string(), so);
            if (!st) return;
            const std::size_t n = std::min(limit, paths.size());
            std::atomic<std::size_t> next{0}, fails{0};
            const auto a = Clock::now();
            std::vector<std::thread> pool;
            for (unsigned k = 0; k < t; ++k)
                pool.emplace_back([&] {
                    for (std::size_t i; (i = next++) < n;)
                        if (!(*st)->add_file(paths[i], DuplicatePolicy::Keep, mode)) ++fails;
                });
            for (auto& th : pool) th.join();
            const auto b = Clock::now();
            std::printf("  add %-28s %u thread(s): %.2f s, %.0f files/s%s\n", label, t, secs(a, b),
                        static_cast<double>(n) / secs(a, b), fails ? " (FAILURES)" : "");
            if (std::string(label) == "copy") {
                st->reset();
                const auto c = Clock::now();
                auto re = Store::open(root.string(), so);
                const auto d = Clock::now();
                if (re) std::printf("  reopen %zu-instance store (journal replay): %.3f s\n", n, secs(c, d));
            }
            std::error_code ec;
            st->reset();
            fs::remove_all(root, ec);
        };
        add_all("copy", 1, AddMode::Copy, false, paths.size());
        add_all("copy-mt", threads, AddMode::Copy, false, paths.size());
        add_all("reference", threads, AddMode::Reference, false, paths.size());
        add_all("copy-durable (2,000 files)", threads, AddMode::Copy, true, 2000);
        std::error_code ec;
        fs::remove_all(td / "gen", ec);
    }

    if (nrecords) {
        if (!worst_only) bench_index(td.path(), nrecords, 50, 5, 2, "typical");
        if (worst) bench_index(td.path(), nrecords, 1, 1, 1, "worst case: one instance per study and patient");
    }
    return 0;
}
