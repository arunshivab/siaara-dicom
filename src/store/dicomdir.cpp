#include "store/dicomdir.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "charset/charset.hpp"
#include "core/dataset.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "core/untrusted.hpp"
#include "core/writer.hpp"
#include "store/common.hpp"
#include "store/path.hpp"
#include "store/store.hpp"

namespace siaara::store {

namespace fs = std::filesystem;

namespace {

namespace dt {
constexpr Tag FileSetId = 0x00041130;
constexpr Tag OffsetFirstRoot = 0x00041200;
constexpr Tag OffsetLastRoot = 0x00041202;
constexpr Tag ConsistencyFlag = 0x00041212;
constexpr Tag RecordSequence = 0x00041220;
constexpr Tag OffsetNext = 0x00041400;
constexpr Tag InUse = 0x00041410;
constexpr Tag OffsetLower = 0x00041420;
constexpr Tag RecordType = 0x00041430;
constexpr Tag ReferencedFileId = 0x00041500;
constexpr Tag RefSopClass = 0x00041510;
constexpr Tag RefSopInstance = 0x00041511;
constexpr Tag RefTransferSyntax = 0x00041512;
}  // namespace dt

bool in_buffer(const Bytes& buf, const std::uint8_t* p) noexcept {
    return p && buf.data() && p >= buf.data() && p <= buf.data() + buf.size();
}

int rank_of(std::string_view type) noexcept {
    if (type == "PATIENT") return 0;
    if (type == "STUDY") return 1;
    if (type == "SERIES") return 2;
    return 3;
}

}  // namespace

std::string_view record_type_for(std::string_view uid) noexcept {
    constexpr std::string_view rt = "1.2.840.10008.5.1.4.1.1.481.";
    if (uid.starts_with(rt)) {
        const auto tail = uid.substr(rt.size());
        if (tail == "2") return "RT DOSE";
        if (tail == "3") return "RT STRUCTURE SET";
        if (tail == "5" || tail == "8") return "RT PLAN";
        if (tail == "4" || tail == "6" || tail == "7" || tail == "9") return "RT TREAT RECORD";
        return "IMAGE";
    }
    if (uid == "1.2.840.10008.5.1.4.1.1.88.59") return "KEY OBJECT DOC";
    if (uid.starts_with("1.2.840.10008.5.1.4.1.1.88.")) return "SR DOCUMENT";
    if (uid.starts_with("1.2.840.10008.5.1.4.1.1.11.")) return "PRESENTATION";
    if (uid.starts_with("1.2.840.10008.5.1.4.1.1.104.")) return "ENCAP DOC";
    if (uid.starts_with("1.2.840.10008.5.1.4.1.1.9.")) return "WAVEFORM";
    if (uid == "1.2.840.10008.5.1.4.1.1.66") return "RAW DATA";
    if (uid == "1.2.840.10008.5.1.4.1.1.66.1" || uid == "1.2.840.10008.5.1.4.1.1.66.3") return "REGISTRATION";
    if (uid == "1.2.840.10008.5.1.4.1.1.66.2") return "FIDUCIAL";
    if (uid == "1.2.840.10008.5.1.4.1.1.66.5") return "SURFACE";
    if (uid == "1.2.840.10008.5.1.4.1.1.67") return "VALUE MAP";
    if (uid == "1.2.840.10008.5.1.4.1.1.4.2") return "SPECTROSCOPY";
    if (uid == "1.2.840.10008.5.1.4.1.1.77.1.5.3") return "STEREOMETRIC";
    return "IMAGE";
}

// ================================================================ reading
bool file_id_well_formed(const std::vector<std::string>& components) noexcept {
    if (components.empty() || components.size() > kFileIdMaxComponents) return false;
    for (const auto& c : components) {
        if (c.empty() || c.size() > kFileIdMaxComponentChars) return false;
        for (char ch : c)
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}

const std::string& DicomdirKeys::operator[](Str k) const noexcept {
    static const std::string kEmpty;
    for (const DicomdirKeys* n = this; n; n = n->parent_.get())
        if (n->owns(k)) return n->own[static_cast<std::size_t>(k)];
    return kEmpty;
}

IndexRecord DicomdirKeys::flatten() const {
    IndexRecord r;
    for (std::size_t k = 0; k < kStrCount; ++k) r.s[k] = (*this)[static_cast<Str>(k)];
    r.flags = flags;
    r.series_number = series_number;
    r.instance_number = instance_number;
    return r;
}

const std::string& DicomdirKeysRef::operator[](Str k) const noexcept {
    static const std::string kEmpty;
    return node ? (*node)[k] : kEmpty;
}

namespace {

// What a reader does with a record's Referenced File ID: resolve it on disk (read_dicomdir) or
// only check it (read_dicomdir_memory). `raw` is the value exactly as stored; `file_path` is the
// record's FilePath key to fill.
using FileIdHandler = std::function<void(DicomdirRecord&, std::string_view raw, std::string& file_path, DicomdirSummary&)>;

constexpr std::uint64_t bit(Str k) noexcept { return std::uint64_t{1} << static_cast<unsigned>(k); }

// The record walk shared by both readers. `bytes` is the input `file` was parsed from (record
// offsets are positions in it). max_records and max_value_bytes: 0 = no limit. untrusted
// (read_dicomdir_memory): a record cut by the end of the input is dropped and reported.
Result<DicomdirSummary> walk_dicomdir(DicomdirSummary sum, const Bytes& bytes, const DicomFile& file,
                                      const DicomdirCallback& cb, const DicomdirReadOptions& opt,
                                      std::uint64_t max_records, std::uint64_t max_value_bytes, bool untrusted,
                                      const FileIdHandler& on_file_id) {
    const Dataset& ds = file.dataset;
    const charset::CharacterSet top_cs = charset::character_set_of(ds);
    sum.file_set_id = element_text(ds, dt::FileSetId, top_cs);
    if (auto c = file.meta.get_string(tags::MediaStorageSOPClassUID); !c || *c != kMediaStorageDirectoryUid)
        sum.warnings.push_back("file meta does not name the Media Storage Directory SOP Class");

    const Element* seq = ds.find(dt::RecordSequence);
    if (!seq) return sum;
    if (!seq->is_sequence()) return Status(Errc::Malformed, "Directory Record Sequence is not a sequence");
    const auto& items = seq->items;
    sum.records = items.size();
    if (max_records && items.size() > max_records)
        return Status(Errc::LimitExceeded, "the DICOMDIR has more records than the limit of " + std::to_string(max_records));

    // A record cut by the end of the input: when the input ended inside the Directory Record
    // Sequence, its last item is not whole. It is dropped (never passed on with a cut value).
    std::vector<bool> dropped(items.size(), false);
    if (untrusted && (file.read_loss & DicomFile::kReadInputTruncated) && !items.empty() &&
        !ds.elements().empty() && &ds.elements().back() == seq) {
        dropped.back() = true;
        sum.warnings.push_back("the last directory record runs past the end of the input; it was dropped");
    }

    const TransferSyntaxInfo* ts = find_transfer_syntax(file.transfer_syntax);
    const bool explicit_vr = !ts || ts->explicit_vr;
    std::vector<std::uint64_t> off(items.size(), 0);
    std::unordered_map<std::uint64_t, std::size_t> at;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].empty() || dropped[i]) continue;
        const Element& e0 = items[i].elements().front();
        if (e0.is_sequence() || e0.is_encapsulated() || !in_buffer(bytes, e0.value.data())) continue;
        const std::uint64_t hdr = explicit_vr ? (vr_has_long_length(e0.vr) ? 12u : 8u) : 8u;
        const auto pos = static_cast<std::uint64_t>(e0.value.data() - bytes.data());
        if (pos < hdr + 8) continue;
        off[i] = pos - hdr - 8;
        at.emplace(off[i], i);
    }

    std::size_t too_long = 0;
    auto fits = [&](const Dataset& it, Tag tag) {
        const Element* e = it.find(tag);
        return !max_value_bytes || !e || e->value.size() <= max_value_bytes;
    };
    // Builds the report for record i. Its keys reference the parent's (never copied).
    using Node = std::shared_ptr<const DicomdirKeys>;
    auto make = [&](std::size_t i, std::int64_t parent, std::uint32_t depth, const Node& inherited) {
        DicomdirRecord r;
        r.index = i;
        r.parent = parent;
        r.depth = depth;
        r.offset = off[i];
        const Dataset& it = items[i];
        if (fits(it, dt::InUse))
            if (auto u = it.get_int(dt::InUse)) r.in_use = *u != 0;
        const charset::CharacterSet cs = charset::character_set_of(it, &top_cs);
        auto node = std::make_shared<DicomdirKeys>();
        node->parent_ = inherited;
        IndexRecord tmp;
        if (inherited) {
            tmp.flags = inherited->flags & ~kHasInstanceNumber;
            tmp.series_number = inherited->series_number;
        }
        const std::size_t before = too_long;
        std::uint64_t mask = read_present_keys(it, cs, tmp, max_value_bytes, &too_long);
        // Never inherited: always the record's own (possibly empty).
        for (Str k : {Str::FilePath, Str::RecordType, Str::SopClassUid, Str::SopInstanceUid, Str::TransferSyntaxUid}) {
            if (!(mask & bit(k))) tmp[k].clear();
            mask |= bit(k);
        }
        auto own_text = [&](Str k, Tag tag) {
            if (!it.contains(tag)) return;
            if (fits(it, tag)) {
                tmp[k] = element_text(it, tag, cs);
            } else {
                tmp[k].clear();
                ++too_long;
            }
        };
        tmp[Str::RecordType].clear();
        own_text(Str::RecordType, dt::RecordType);
        own_text(Str::SopClassUid, dt::RefSopClass);
        own_text(Str::SopInstanceUid, dt::RefSopInstance);
        own_text(Str::TransferSyntaxUid, dt::RefTransferSyntax);
        if (auto v = it.get_raw_string(dt::ReferencedFileId)) {
            if (fits(it, dt::ReferencedFileId)) {
                r.file_id = split_file_id(*v);
                on_file_id(r, *v, tmp[Str::FilePath], sum);
            } else {
                // Too long to keep: the File ID is not passed on (a component list cannot be cut).
                r.file_status = Status(Errc::Rejected, "the Referenced File ID is longer than max_value_bytes");
                ++sum.rejected_paths;
                ++too_long;
            }
        }
        if (too_long != before)
            sum.warnings.push_back("record " + std::to_string(i) + ": " + std::to_string(too_long - before) +
                                   " values longer than max_value_bytes left out");
        node->flags = tmp.flags;
        node->series_number = tmp.series_number;
        node->instance_number = tmp.instance_number;
        node->own_mask = mask;
        for (std::size_t k = 0; k < kStrCount; ++k)
            if (mask & (std::uint64_t{1} << k)) node->own[k] = std::move(tmp.s[k]);
        r.keys.node = std::move(node);
        return r;
    };

    // Every record visit counts against max_records, a refused one too.
    std::uint64_t visits = 0;
    auto count_visit = [&]() -> Status {
        if (max_records && ++visits > max_records)
            return Status(Errc::LimitExceeded, "the DICOMDIR's record links make more visits than the limit of " +
                                                   std::to_string(max_records) + " records");
        return Status::ok();
    };

    std::vector<bool> visited(items.size(), false);
    const auto root = ds.get_int(dt::OffsetFirstRoot);
    const bool linked = root && *root > 0 && at.count(static_cast<std::uint64_t>(*root));
    if (!items.empty() && !linked) {
        sum.sequential_fallback = true;
        sum.warnings.push_back("root record offset does not point at a record; using sequence order");
        struct Up {
            int rank;
            std::size_t index;
            Node keys;
        };
        std::vector<Up> stack;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (dropped[i]) continue;
            if (Status s = count_visit(); !s) return s;
            const charset::CharacterSet cs = charset::character_set_of(items[i], &top_cs);
            const int rank = rank_of(fits(items[i], dt::RecordType) ? element_text(items[i], dt::RecordType, cs) : std::string());
            while (!stack.empty() && stack.back().rank >= rank) stack.pop_back();
            DicomdirRecord r = make(i, stack.empty() ? -1 : static_cast<std::int64_t>(stack.back().index),
                                    static_cast<std::uint32_t>(stack.size()), stack.empty() ? Node{} : stack.back().keys);
            visited[i] = true;
            if (!r.in_use && !opt.include_inactive) continue;
            ++sum.reported;
            if (cb && !cb(r)) return Status(Errc::Cancelled, "cancelled by the callback");
            if (rank < 3) stack.push_back({rank, i, r.keys.node});
        }
        return sum;
    }

    struct Frame {
        std::uint64_t offset;
        std::uint32_t depth;
        std::int64_t parent;
        Node inherited;
    };
    std::vector<Frame> stack;
    if (linked) stack.push_back({static_cast<std::uint64_t>(*root), 0, -1, Node{}});
    while (!stack.empty()) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (f.offset == 0) continue;
        if (Status s = count_visit(); !s) return s;
        auto hit = at.find(f.offset);
        if (hit == at.end()) {
            sum.warnings.push_back("record link at offset " + std::to_string(f.offset) + " does not point at a record");
            continue;
        }
        const std::size_t i = hit->second;
        if (visited[i]) {
            sum.warnings.push_back("record links form a cycle at offset " + std::to_string(f.offset));
            continue;
        }
        visited[i] = true;
        DicomdirRecord r = make(i, f.parent, f.depth, f.inherited);
        // A link value longer than max_value_bytes is not read (0: no link).
        auto link = [&](Tag tag) -> std::int64_t {
            if (!fits(items[i], tag)) return 0;
            const auto v = items[i].get_int(tag);
            return v ? *v : 0;
        };
        const std::int64_t next = link(dt::OffsetNext);
        const std::int64_t lower = link(dt::OffsetLower);
        if (next > 0) stack.push_back({static_cast<std::uint64_t>(next), f.depth, f.parent, f.inherited});
        if (!r.in_use && !opt.include_inactive) continue;
        ++sum.reported;
        if (cb && !cb(r)) return Status(Errc::Cancelled, "cancelled by the callback");
        if (lower > 0) {
            if (f.depth + 1 >= opt.max_depth) {
                sum.warnings.push_back("record hierarchy deeper than the limit; lower levels skipped");
                continue;
            }
            stack.push_back({static_cast<std::uint64_t>(lower), f.depth + 1, static_cast<std::int64_t>(i), r.keys.node});
        }
    }
    for (std::size_t i = 0; i < visited.size(); ++i)
        if (!visited[i] && !dropped[i]) ++sum.unreachable;
    if (sum.unreachable) sum.warnings.push_back(std::to_string(sum.unreachable) + " records are not reachable from the root");
    return sum;
}

}  // namespace

Result<DicomdirSummary> read_dicomdir(const std::string& utf8_path, const DicomdirCallback& cb,
                                      const DicomdirReadOptions& opt) {
    DicomdirSummary sum;
    auto bytes = io::open_file(utf8_path);
    if (!bytes) return bytes.take_error();
    if (opt.max_bytes && bytes->size() > opt.max_bytes)
        return Status(Errc::LimitExceeded, "the DICOMDIR is larger than the limit of " + std::to_string(opt.max_bytes) + " bytes");
    auto file = read_part10(*bytes);
    if (!file) return file.take_error();
    std::error_code ec;
    fs::path dir = fs::absolute(u8path(utf8_path), ec).parent_path();
    if (ec) dir = u8path(utf8_path).parent_path();
    return walk_dicomdir(std::move(sum), *bytes, *file, cb, opt, opt.max_records, opt.max_value_bytes, false,
                         [&](DicomdirRecord& r, std::string_view, std::string& file_path, DicomdirSummary& s) {
                             if (!opt.resolve_files) return;
                             auto p = resolve_file_id(dir, r.file_id, opt.case_insensitive);
                             if (p) {
                                 file_path = to_utf8(*p);
                                 ++s.files;
                             } else {
                                 r.file_status = p.take_error();
                                 if (r.file_status.code == Errc::Rejected) ++s.rejected_paths;
                                 else ++s.missing_files;
                             }
                         });
}

Result<DicomdirSummary> read_dicomdir_memory(const std::uint8_t* data, std::size_t size, const DicomdirCallback& cb,
                                             const DicomdirReadOptions& opt) {
    if (!data && size) return Status(Errc::InvalidArgument, "data must not be NULL");
    const std::uint64_t max_bytes = opt.max_bytes ? opt.max_bytes : kDicomdirMemoryDefaultMaxBytes;
    const std::uint64_t max_records = opt.max_records ? opt.max_records : kDicomdirMemoryDefaultMaxRecords;
    const std::uint64_t max_value_bytes = opt.max_value_bytes ? opt.max_value_bytes : kDicomdirMemoryDefaultMaxValueBytes;
    if (size > max_bytes)
        return Status(Errc::LimitExceeded, "the DICOMDIR is larger than the limit of " + std::to_string(max_bytes) + " bytes");
    // A view of the caller's bytes (no copy): the parsed file and everything taken from it live
    // only during this call. The element limit allows max_records records (R-223).
    const Bytes bytes(std::shared_ptr<const void>{}, data, size);
    std::uint64_t max_elements = kUntrustedMaxElements;
    if (max_records <= UINT64_MAX / kDicomdirElementsPerRecord)
        max_elements = std::max(max_elements, max_records * kDicomdirElementsPerRecord);
    else
        max_elements = UINT64_MAX;
    auto file = read_part10(bytes, untrusted_read_options(0, max_elements, 0, false));
    if (!file) return file.take_error();
    // Refused unless the file meta names the Media Storage Directory SOP Class.
    if (auto c = file->meta.get_string(tags::MediaStorageSOPClassUID); !c || *c != kMediaStorageDirectoryUid)
        return Status(Errc::Rejected, "not a DICOMDIR: the Media Storage SOP Class is not 1.2.840.10008.1.3.10");
    DicomdirSummary first;
    first.incomplete = (file->read_loss & (DicomFile::kReadBytesSkipped | DicomFile::kReadInputTruncated | DicomFile::kReadRecovered)) != 0;
    if (first.incomplete)
        first.warnings.push_back("the DICOMDIR was not read completely (" + std::string(file->warnings.empty() ? "read loss" : file->warnings.front()) + ")");
    return walk_dicomdir(std::move(first), bytes, *file, cb, opt, max_records, max_value_bytes, true,
                         [](DicomdirRecord& r, std::string_view raw, std::string& file_path, DicomdirSummary& s) {
        if (file_id_well_formed(r.file_id)) {
            for (const auto& c : r.file_id) {
                if (!file_path.empty()) file_path.push_back('/');
                file_path += c;
            }
            ++s.files;
        } else {
            // Exactly as stored: an unsafe entry in the sender's DICOMDIR, not a missing file.
            file_path.assign(raw.data(), raw.size());
            r.file_status = Status(Errc::Rejected, "the Referenced File ID is not well formed (PS3.10 8.5)");
            ++s.rejected_paths;
        }
    });
}

// ================================================================ writing
namespace {

namespace kt {
constexpr Tag SpecificCharacterSet = 0x00080005, StudyDate = 0x00080020,
              ContentDate = 0x00080023, StudyTime = 0x00080030, ContentTime = 0x00080033,
              Accession = 0x00080050, RefSeriesSeq = 0x00081115, StudyDescription = 0x00081030,
              PatientName = 0x00100010, PatientId = 0x00100020, BirthDate = 0x00100030,
              Sex = 0x00100040, StudyUid = 0x0020000D, SeriesUid = 0x0020000E, StudyId = 0x00200010,
              SeriesNumber = 0x00200011, InstanceNumber = 0x00200013, Modality = 0x00080060,
              DoseSummation = 0x3004000A, SsLabel = 0x30060002, SsDate = 0x30060008, SsTime = 0x30060009,
              PlanLabel = 0x300A0002, PlanDate = 0x300A0006, PlanTime = 0x300A0007,
              TreatDate = 0x30080250, TreatTime = 0x30080251, VerifDateTime = 0x0040A030,
              ConceptName = 0x0040A043, Completion = 0x0040A491, Verification = 0x0040A493,
              ContentLabel = 0x00700080, ContentDesc = 0x00700081, PresDate = 0x00700082,
              PresTime = 0x00700083, Creator = 0x00700084, DocTitle = 0x00420010, Mime = 0x00420012;
}  // namespace kt

struct Key {
    Tag tag;
    VR vr;
    int type;  // 1, 2 or 3
};

constexpr Key kPatientKeys[] = {{kt::PatientName, VR::PN, 2}, {kt::PatientId, VR::LO, 1},
                                {kt::BirthDate, VR::DA, 3}, {kt::Sex, VR::CS, 3}};
constexpr Key kStudyKeys[] = {{kt::StudyDate, VR::DA, 1}, {kt::StudyTime, VR::TM, 1},
                              {kt::Accession, VR::SH, 2}, {kt::StudyDescription, VR::LO, 2},
                              {kt::StudyUid, VR::UI, 1}, {kt::StudyId, VR::SH, 1}};
constexpr Key kSeriesKeys[] = {{kt::Modality, VR::CS, 1}, {kt::SeriesUid, VR::UI, 1},
                               {kt::SeriesNumber, VR::IS, 1}};

std::vector<Key> leaf_keys(std::string_view type) {
    if (type == "RT DOSE") return {{kt::InstanceNumber, VR::IS, 1}, {kt::DoseSummation, VR::CS, 1}};
    if (type == "RT STRUCTURE SET")
        return {{kt::InstanceNumber, VR::IS, 1}, {kt::SsLabel, VR::SH, 1}, {kt::SsDate, VR::DA, 2}, {kt::SsTime, VR::TM, 2}};
    if (type == "RT PLAN")
        return {{kt::InstanceNumber, VR::IS, 1}, {kt::PlanLabel, VR::SH, 1}, {kt::PlanDate, VR::DA, 2}, {kt::PlanTime, VR::TM, 2}};
    if (type == "RT TREAT RECORD")
        return {{kt::InstanceNumber, VR::IS, 1}, {kt::TreatDate, VR::DA, 2}, {kt::TreatTime, VR::TM, 2}};
    if (type == "SR DOCUMENT")
        return {{kt::ContentDate, VR::DA, 1}, {kt::ContentTime, VR::TM, 1}, {kt::InstanceNumber, VR::IS, 1},
                {kt::VerifDateTime, VR::DT, 3}, {kt::ConceptName, VR::SQ, 1}, {kt::Completion, VR::CS, 1},
                {kt::Verification, VR::CS, 1}};
    if (type == "KEY OBJECT DOC")
        return {{kt::ContentDate, VR::DA, 1}, {kt::ContentTime, VR::TM, 1}, {kt::InstanceNumber, VR::IS, 1},
                {kt::ConceptName, VR::SQ, 1}};
    if (type == "PRESENTATION")
        return {{kt::RefSeriesSeq, VR::SQ, 3}, {kt::InstanceNumber, VR::IS, 1}, {kt::ContentLabel, VR::CS, 1},
                {kt::ContentDesc, VR::LO, 2}, {kt::PresDate, VR::DA, 1}, {kt::PresTime, VR::TM, 1},
                {kt::Creator, VR::PN, 2}};
    if (type == "ENCAP DOC")
        return {{kt::ContentDate, VR::DA, 2}, {kt::ContentTime, VR::TM, 2}, {kt::InstanceNumber, VR::IS, 1},
                {kt::DocTitle, VR::ST, 2}, {kt::Mime, VR::LO, 1}};
    if (type == "IMAGE") return {{kt::InstanceNumber, VR::IS, 1}};
    return {{kt::ContentDate, VR::DA, 3}, {kt::ContentTime, VR::TM, 3}, {kt::InstanceNumber, VR::IS, 1}};
}

// Copies the keys present in `from` into `to`; Type 1 and 2 keys that are absent are written
// empty, and a missing Type 1 key is warned about.
void copy_keys(const Dataset& from, Dataset& to, const Key* keys, std::size_t n, const char* what,
               std::vector<std::string>& warnings) {
    for (std::size_t i = 0; i < n; ++i) {
        const Key& k = keys[i];
        if (const Element* e = from.find(k.tag); e && (!e->value.empty() || !e->items.empty())) {
            Element c = *e;
            if (c.vr == VR::UN || c.vr == VR::None) c.vr = k.vr;
            to.put(std::move(c));
            continue;
        }
        if (k.type == 3) continue;
        if (k.type == 1) {
            char tag[16];
            std::snprintf(tag, sizeof tag, "(%04X,%04X)", tag_group(k.tag), tag_element(k.tag));
            warnings.push_back(std::string(what) + " record: Type 1 key " + tag + " is missing; written empty");
        }
        Element c;
        c.tag = k.tag;
        c.vr = k.vr;
        to.put(std::move(c));
    }
}

struct Node {
    Dataset ds;
    std::vector<std::size_t> children;
};

std::string counter_name(char prefix, std::size_t n) {
    char b[16];
    std::snprintf(b, sizeof b, "%c%07u", prefix, static_cast<unsigned>(n % 10000000u));
    return b;
}

bool file_set_id_ok(std::string_view s) noexcept {
    if (s.size() > 16) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ' ')) return false;
    return true;
}

bool profile_allows(MediaProfile p, std::string_view ts_uid) {
    if (ts_uid == ts::ExplicitVRLittleEndian) return true;
    if (p == MediaProfile::GeneralCd) return false;
    const TransferSyntaxInfo* t = find_transfer_syntax(ts_uid);
    return t && t->explicit_vr && !t->big_endian && !t->retired;
}

}  // namespace

Result<DicomdirWriteSummary> write_dicomdir(const std::string& utf8_root, const std::vector<std::string>& files,
                                            const DicomdirWriteOptions& opt) {
    DicomdirWriteSummary sum;
    if (files.empty()) return Status(Errc::InvalidArgument, "no files to write");
    if (!file_set_id_ok(opt.file_set_id)) return Status(Errc::InvalidArgument, "File-set ID must be up to 16 of A-Z 0-9 _ space");
    std::error_code ec;
    const fs::path root = u8path(utf8_root);
    fs::create_directories(root, ec);
    if (ec) return Status(Errc::IoError, "cannot create the output folder");
    if (!opt.overwrite && (fs::exists(root / "DICOMDIR", ec) || fs::exists(root / "DICOM", ec)))
        return Status(Errc::Rejected, "output folder already holds a DICOMDIR or DICOM folder");

    struct Leaf {
        std::size_t source;
        std::string type, ts_uid, sop_class, sop_uid;
        Dataset keys;
        std::vector<std::string> file_id;
    };
    struct SeriesG {
        Dataset keys;
        std::vector<Leaf> leaves;
    };
    struct StudyG {
        Dataset keys;
        std::vector<std::string> series_order;
        std::map<std::string, SeriesG> series;
    };
    struct PatientG {
        Dataset keys;
        std::vector<std::string> study_order;
        std::map<std::string, StudyG> studies;
    };
    std::vector<std::string> patient_order;
    std::map<std::string, PatientG> patients;
    std::unordered_map<std::string, bool> seen_uids;

    // ---- pass 1: read and group
    const std::uint64_t steps = std::uint64_t{files.size()} * 2;
    std::uint64_t step = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (opt.progress && !opt.progress(step++, steps)) return Status(Errc::Cancelled, "DICOMDIR write cancelled");
        auto bytes = io::open_file(files[i]);
        if (!bytes) return Status(bytes.error().code, "cannot read input file " + std::to_string(i));
        auto f = read_part10(*bytes);
        if (!f) return Status(f.error().code, "input file " + std::to_string(i) + " is not readable DICOM");
        const Dataset& ds = f->dataset;
        IndexRecord rec;
        read_keys(ds, charset::character_set_of(ds), rec, false);
        const std::string sop_uid = rec[Str::SopInstanceUid];
        if (sop_uid.empty() || rec[Str::StudyInstanceUid].empty() || rec[Str::SeriesInstanceUid].empty())
            return Status(Errc::Malformed, "input file " + std::to_string(i) + " lacks SOP Instance, Study or Series UID");
        if (seen_uids.count(sop_uid)) {
            sum.warnings.push_back("input file " + std::to_string(i) + " repeats a SOP Instance UID; skipped");
            continue;
        }
        seen_uids[sop_uid] = true;
        const std::string ts_uid = f->transfer_syntax;
        if (!profile_allows(opt.profile, ts_uid)) {
            if (!opt.allow_any_transfer_syntax)
                return Status(Errc::Unsupported, "input file " + std::to_string(i) +
                                                     " uses a transfer syntax the media profile does not allow");
            sum.warnings.push_back("input file " + std::to_string(i) + " uses a transfer syntax outside the profile");
        }
        const Element* cs = ds.find(kt::SpecificCharacterSet);
        auto with_cs = [&](Dataset& d) {
            if (cs) d.put(*cs);
        };
        const std::string pkey = Store::patient_key(rec);
        auto [pit, pnew] = patients.try_emplace(pkey);
        if (pnew) {
            patient_order.push_back(pkey);
            copy_keys(ds, pit->second.keys, kPatientKeys, std::size(kPatientKeys), "PATIENT", sum.warnings);
            with_cs(pit->second.keys);
        }
        auto [sit, snew] = pit->second.studies.try_emplace(rec[Str::StudyInstanceUid]);
        if (snew) {
            pit->second.study_order.push_back(rec[Str::StudyInstanceUid]);
            copy_keys(ds, sit->second.keys, kStudyKeys, std::size(kStudyKeys), "STUDY", sum.warnings);
            with_cs(sit->second.keys);
        }
        auto [eit, enew] = sit->second.series.try_emplace(rec[Str::SeriesInstanceUid]);
        if (enew) {
            sit->second.series_order.push_back(rec[Str::SeriesInstanceUid]);
            copy_keys(ds, eit->second.keys, kSeriesKeys, std::size(kSeriesKeys), "SERIES", sum.warnings);
            with_cs(eit->second.keys);
        }
        Leaf leaf;
        leaf.source = i;
        leaf.sop_class = rec[Str::SopClassUid];
        if (leaf.sop_class.empty())
            if (auto v = f->meta.get_string(tags::MediaStorageSOPClassUID)) leaf.sop_class = *v;
        leaf.type = std::string(record_type_for(leaf.sop_class));
        leaf.ts_uid = ts_uid;
        leaf.sop_uid = sop_uid;
        const auto lk = leaf_keys(leaf.type);
        copy_keys(ds, leaf.keys, lk.data(), lk.size(), "leaf", sum.warnings);
        with_cs(leaf.keys);
        eit->second.leaves.push_back(std::move(leaf));
    }

    // ---- pass 2: copy files, build records (depth-first pre-order)
    std::vector<Node> nodes;
    std::vector<std::size_t> roots;
    auto make_node = [&](Dataset keys, std::string_view type) {
        Node n;
        n.ds = std::move(keys);
        (void)n.ds.set_uint32(dt::OffsetNext, 0);
        (void)n.ds.set_uint16(dt::InUse, 0xFFFF);
        (void)n.ds.set_uint32(dt::OffsetLower, 0);
        (void)n.ds.set_string(dt::RecordType, VR::CS, type);
        nodes.push_back(std::move(n));
        return nodes.size() - 1;
    };
    std::size_t pn = 0;
    for (const auto& pkey : patient_order) {
        PatientG& pg = patients[pkey];
        const std::size_t pi = make_node(pg.keys, "PATIENT");
        roots.push_back(pi);
        const std::string pdir = counter_name('P', ++pn);
        std::size_t sn = 0;
        ++sum.patients;
        for (const auto& suid : pg.study_order) {
            StudyG& sg = pg.studies[suid];
            const std::size_t si = make_node(sg.keys, "STUDY");
            nodes[pi].children.push_back(si);
            const std::string sdir = counter_name('S', ++sn);
            std::size_t en = 0;
            ++sum.studies;
            for (const auto& euid : sg.series_order) {
                SeriesG& eg = sg.series[euid];
                const std::size_t ei = make_node(eg.keys, "SERIES");
                nodes[si].children.push_back(ei);
                const std::string edir = counter_name('E', ++en);
                std::size_t in = 0;
                ++sum.series;
                for (Leaf& leaf : eg.leaves) {
                    if (opt.progress && !opt.progress(step++, steps)) return Status(Errc::Cancelled, "DICOMDIR write cancelled");
                    leaf.file_id = {"DICOM", pdir, sdir, edir, counter_name('I', ++in)};
                    fs::path dest = root;
                    for (const auto& c : leaf.file_id) dest /= u8path(c);
                    fs::create_directories(dest.parent_path(), ec);
                    if (ec) return Status(Errc::IoError, "cannot create a media folder");
                    auto bytes = io::open_file(files[leaf.source]);
                    if (!bytes) return bytes.take_error();
                    if (has_part10_header(bytes->data(), bytes->size())) {
                        if (Status s = write_replace(dest, bytes->data(), bytes->size(), false); !s) return s;
                    } else {
                        auto f = read_part10(*bytes);
                        if (!f) return f.take_error();
                        std::vector<std::uint8_t> out;
                        if (Status s = write_part10(*f, out); !s) return s;
                        if (Status s = write_replace(dest, out.data(), out.size(), false); !s) return s;
                    }
                    ++sum.files;
                    std::string fid;
                    for (const auto& c : leaf.file_id) {
                        if (!fid.empty()) fid.push_back('\\');
                        fid += c;
                    }
                    const std::size_t li = make_node(leaf.keys, leaf.type);
                    (void)nodes[li].ds.set_string(dt::ReferencedFileId, VR::CS, fid);
                    (void)nodes[li].ds.set_string(dt::RefSopClass, VR::UI, leaf.sop_class);
                    (void)nodes[li].ds.set_string(dt::RefSopInstance, VR::UI, leaf.sop_uid);
                    (void)nodes[li].ds.set_string(dt::RefTransferSyntax, VR::UI, leaf.ts_uid);
                    nodes[ei].children.push_back(li);
                }
            }
        }
    }

    // pre-order
    std::vector<std::size_t> order;
    order.reserve(nodes.size());
    {
        std::vector<std::size_t> st(roots.rbegin(), roots.rend());
        while (!st.empty()) {
            const std::size_t n = st.back();
            st.pop_back();
            order.push_back(n);
            for (auto it = nodes[n].children.rbegin(); it != nodes[n].children.rend(); ++it) st.push_back(*it);
        }
    }

    // ---- pass 3: layout. Offsets are UL (fixed size), so one sizing pass is enough.
    DicomFile head;
    head.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)head.meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, kMediaStorageDirectoryUid);
    (void)head.meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, generate_uid());
    (void)head.dataset.set_string(dt::FileSetId, VR::CS, opt.file_set_id);
    (void)head.dataset.set_uint32(dt::OffsetFirstRoot, 0);
    (void)head.dataset.set_uint32(dt::OffsetLastRoot, 0);
    (void)head.dataset.set_uint16(dt::ConsistencyFlag, 0);
    std::vector<std::uint8_t> head_bytes;
    if (Status s = write_part10(head, head_bytes); !s) return s;
    const std::uint64_t first_item = head_bytes.size() + 12;
    std::vector<std::uint64_t> item_off(nodes.size(), 0);
    {
        std::uint64_t pos = first_item;
        for (std::size_t n : order) {
            std::vector<std::uint8_t> body;
            if (Status s = write_dataset(nodes[n].ds, ts::ExplicitVRLittleEndian, body); !s) return s;
            item_off[n] = pos;
            pos += 8 + body.size();
        }
        if (pos > UINT32_MAX) return Status(Errc::LimitExceeded, "DICOMDIR larger than 4 GiB");
    }
    auto set_links = [&](const std::vector<std::size_t>& siblings) {
        for (std::size_t k = 0; k < siblings.size(); ++k) {
            Node& nd = nodes[siblings[k]];
            const std::uint64_t next = k + 1 < siblings.size() ? item_off[siblings[k + 1]] : 0;
            (void)nd.ds.set_uint32(dt::OffsetNext, static_cast<std::uint32_t>(next));
            (void)nd.ds.set_uint32(dt::OffsetLower,
                                   nd.children.empty() ? 0u : static_cast<std::uint32_t>(item_off[nd.children.front()]));
        }
    };
    set_links(roots);
    for (const Node& n : nodes) set_links(n.children);
    (void)head.dataset.set_uint32(dt::OffsetFirstRoot, static_cast<std::uint32_t>(item_off[roots.front()]));
    (void)head.dataset.set_uint32(dt::OffsetLastRoot, static_cast<std::uint32_t>(item_off[roots.back()]));
    std::vector<std::uint8_t> out;
    if (Status s = write_part10(head, out); !s) return s;
    if (out.size() != head_bytes.size()) return Status(Errc::Internal, "DICOMDIR header size changed");
    const std::uint8_t sq[12] = {0x04, 0x00, 0x20, 0x12, 'S', 'Q', 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    out.insert(out.end(), sq, sq + 12);
    for (std::size_t n : order) {
        if (out.size() != item_off[n]) return Status(Errc::Internal, "DICOMDIR record offset mismatch");
        std::vector<std::uint8_t> body;
        if (Status s = write_dataset(nodes[n].ds, ts::ExplicitVRLittleEndian, body); !s) return s;
        std::uint8_t ih[8] = {0xFE, 0xFF, 0x00, 0xE0, 0, 0, 0, 0};
        store_le32(ih + 4, static_cast<std::uint32_t>(body.size()));
        out.insert(out.end(), ih, ih + 8);
        out.insert(out.end(), body.begin(), body.end());
    }
    const std::uint8_t delim[8] = {0xFE, 0xFF, 0xDD, 0xE0, 0, 0, 0, 0};
    out.insert(out.end(), delim, delim + 8);
    if (Status s = write_replace(root / "DICOMDIR", out.data(), out.size(), true); !s) return s;
    return sum;
}

}  // namespace siaara::store
