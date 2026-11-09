// SIAARA DICOM Toolkit - de-identification engine (DICOM PS3.15 Annex E).
//
// Rules applied where PS3.15 leaves a choice (also the Conformance Statement text):
//  * D (dummy): AE/CS/LO/LT/PN/SH/ST/UC/UT "DEIDENTIFIED", AS "000Y", DA "19000101",
//    DT "19000101000000", TM "000000", DS/IS "0", UR "urn:oid:2.25.0", binary VRs one zero
//    value, UI the keyed replacement UID, SQ one empty item (the original content is dropped).
//    Patient ID gets a keyed pseudonym "ID" + 16 hex digits of HMAC(key, "PatientID"), so
//    different patients stay distinct and one patient stays consistent across runs.
//  * Z: zero-length value; SQ with no items.
//  * C (clean): DA/DT shifted by the per-patient offset (Modified Dates); TM kept (a whole-day
//    shift keeps time of day, so DA/TM pairs and intervals spanning midnight stay consistent);
//    UI replaced as U; SQ walked again with every free-text attribute inside cleaned too;
//    text values reduced to the caller's safe words (placeholder "CLEANED" if none survive);
//    DS/IS/AS kept only if they are well-formed numbers/ages; OB/OW/OF/OD/OL/OV/UN zero-filled
//    with the same length; other binary VRs kept.
//  * K on a sequence and U on X/Z/U* sequences: every item is walked with the same rules.
//  * Combined actions: see CombinedPolicy (deid.hpp).
//  * Private attributes: removed, except with Retain Safe Private where the private creator and
//    element are in Table E.3.10-1; private creator elements are kept only for blocks that keep an
//    element. Elements of unknown creators and private groups below 0009 are always removed.
//  * Group 0004 and all group length elements are removed; group 0002 found inside the data
//    set is removed. Removing Overlay Data (60xx,3000) removes the whole 60xx overlay group.
//    Overlays embedded in unused Pixel Data bits (retired) are cleared from the pixel data.
#include "deid/deid.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/reader.hpp"
#include "core/transfer_syntax.hpp"
#include "deid/sha256.hpp"
#include "dict/dictionary.hpp"

namespace siaara::deid {

namespace {

namespace t {
constexpr Tag CodeValue = 0x00080100;
constexpr Tag CodingSchemeDesignator = 0x00080102;
constexpr Tag CodingSchemeVersion = 0x00080103;
constexpr Tag CodeMeaning = 0x00080104;
constexpr Tag LongCodeValue = 0x00080119;
constexpr Tag URNCodeValue = 0x00080120;
constexpr Tag PatientIdentityRemoved = 0x00120062;
constexpr Tag DeidentificationMethod = 0x00120063;
constexpr Tag DeidentificationMethodCodeSequence = 0x00120064;
constexpr Tag BurnedInAnnotation = 0x00280301;
constexpr Tag RecognizableVisualFeatures = 0x00280302;
constexpr Tag LongitudinalTemporalInformationModified = 0x00280303;
constexpr Tag OverlayBitsAllocated = 0x60000100;
constexpr Tag OverlayBitPosition = 0x60000102;
constexpr Tag OverlayData = 0x60003000;
}  // namespace t

constexpr std::string_view kDicomdirSopClass = "1.2.840.10008.1.3.10";
constexpr std::string_view kDummy = "DEIDENTIFIED";
constexpr std::string_view kCleaned = "CLEANED";
constexpr std::uint32_t kMaxDepth = 128;

// CID 7050 code values for each option bit, in OptionBits order.
constexpr std::string_view kOptionCode[kOptionCount] = {
    "113111", "113110", "113109", "113112", "113108", "113106", "113107", "113105", "113104", "113103",
};

// Coding schemes whose Code Value / Code Meaning are published vocabularies, kept by the clean
// action inside code sequence items (local and private schemes are cleaned like free text).
constexpr std::string_view kStandardSchemes[] = {
    "DCM", "SCT", "SRT", "LN", "UCUM", "FMA", "RADLEX", "NCIt", "UMLS", "MDC", "ICD10", "I10",
    "ICD10PCS", "BI", "UBERON", "NCBITaxon", "MSH", "HPC", "ACR", "IBSI", "RXNORM",
};

bool is_text_vr(VR vr) noexcept {
    switch (vr) {
        case VR::SH: case VR::LO: case VR::ST: case VR::LT: case VR::UT: case VR::UC: case VR::PN:
            return true;
        default:
            return false;
    }
}
bool is_date_vr(VR vr) noexcept { return vr == VR::DA || vr == VR::DT || vr == VR::TM; }
bool is_bulk_vr(VR vr) noexcept {
    switch (vr) {
        case VR::OB: case VR::OW: case VR::OF: case VR::OD: case VR::OL: case VR::OV: case VR::UN:
            return true;
        default:
            return false;
    }
}
bool single_valued(VR vr) noexcept {
    return vr == VR::LT || vr == VR::ST || vr == VR::UT || vr == VR::UR;
}

std::string_view trim_all(std::string_view s) noexcept {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\0')) s.remove_prefix(1);
    return s;
}

std::vector<std::string_view> split_values(std::string_view s, VR vr) {
    std::vector<std::string_view> out;
    if (single_valued(vr)) {
        out.push_back(s);
        return out;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = s.find('\\', start);
        if (p == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

Bytes string_value(std::string v, VR vr) {
    if (v.size() & 1u) v.push_back(vr_pad_char(vr));
    return Bytes::from_string(v);
}

bool well_known_uid(std::string_view uid) noexcept {
    if (uid.substr(0, 14) == "1.2.840.10008.") return true;
    return dict::lookup_uid(uid) != nullptr;
}

std::size_t max_length(VR vr) noexcept {
    switch (vr) {
        case VR::AE: case VR::CS: case VR::SH: return 16;
        case VR::LO: case VR::PN: return 64;
        case VR::ST: return 1024;
        case VR::LT: return 10240;
        default: return 65534;
    }
}

// ---------------------------------------------------------------- dates (proleptic Gregorian)
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    // Count days from 0000-03-01 by shifting the year to start in March, then subtract the
    // offset of 1970-01-01. Each 400-year era has 146097 days.
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t mp = (static_cast<std::int64_t>(m) + 9) % 12;  // March = 0
    const std::int64_t doy = (153 * mp + 2) / 5 + static_cast<std::int64_t>(d) - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}
bool leap(std::int64_t y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
unsigned month_days(std::int64_t y, unsigned m) noexcept {
    static constexpr unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29u : kDays[m - 1];
}
bool digits(std::string_view s) noexcept {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
unsigned to_uint(std::string_view s) noexcept {
    unsigned v = 0;
    for (char c : s) v = v * 10u + static_cast<unsigned>(c - '0');
    return v;
}
void put_digits(std::string& out, std::int64_t v, int width) {
    char buf[8];
    for (int i = width - 1; i >= 0; --i) {
        buf[i] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    out.append(buf, static_cast<std::size_t>(width));
}

// Shifts YYYY[MM[DD]] keeping its precision. Returns false for invalid dates.
bool shift_ymd(std::string_view ymd, std::int32_t days, std::string& out) {
    if (!digits(ymd) || (ymd.size() != 4 && ymd.size() != 6 && ymd.size() != 8)) return false;
    const std::int64_t y = to_uint(ymd.substr(0, 4));
    const unsigned m = ymd.size() >= 6 ? to_uint(ymd.substr(4, 2)) : 1u;
    if (m < 1 || m > 12) return false;
    const unsigned d = ymd.size() == 8 ? to_uint(ymd.substr(6, 2)) : 1u;
    if (d < 1 || d > month_days(y, m)) return false;
    std::int64_t ny = 0;
    unsigned nm = 0, nd = 0;
    civil_from_days(days_from_civil(y, m, d) + days, ny, nm, nd);
    if (ny < 1 || ny > 9999) return false;
    put_digits(out, ny, 4);
    if (ymd.size() >= 6) put_digits(out, nm, 2);
    if (ymd.size() == 8) put_digits(out, nd, 2);
    return true;
}

bool valid_time(std::string_view s) noexcept {
    // HH[MM[SS[.F{1,6}]]], also the ACR-NEMA HH:MM:SS form.
    std::string compact;
    for (char c : s)
        if (c != ':') compact.push_back(c);
    const std::size_t dot = compact.find('.');
    const std::string_view main = std::string_view(compact).substr(0, dot);
    if (!digits(main) || (main.size() != 2 && main.size() != 4 && main.size() != 6)) return false;
    if (to_uint(main.substr(0, 2)) > 23) return false;
    if (main.size() >= 4 && to_uint(main.substr(2, 2)) > 59) return false;
    if (main.size() == 6 && to_uint(main.substr(4, 2)) > 60) return false;
    if (dot != std::string::npos) {
        const std::string_view frac = std::string_view(compact).substr(dot + 1);
        if (main.size() != 6 || frac.size() > 6 || (!frac.empty() && !digits(frac))) return false;
    }
    return true;
}

bool valid_number(std::string_view s, bool integer) noexcept {
    s = trim_all(s);
    if (s.empty()) return true;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                        (!integer && (c == '.' || c == 'e' || c == 'E'));
        if (!ok) return false;
    }
    return true;
}

bool valid_age(std::string_view s) noexcept {
    s = trim_all(s);
    return s.empty() || (s.size() == 4 && digits(s.substr(0, 3)) &&
                         (s[3] == 'D' || s[3] == 'W' || s[3] == 'M' || s[3] == 'Y'));
}

// 128-bit unsigned (16 big-endian bytes) to decimal, without leading zeros.
std::string to_decimal(std::array<std::uint8_t, 16> n) {
    std::string digits_rev;
    for (;;) {
        bool zero = true;
        unsigned rem = 0;
        for (std::uint8_t& b : n) {
            const unsigned cur = (rem << 8) | b;
            b = static_cast<std::uint8_t>(cur / 10u);
            rem = cur % 10u;
            if (b) zero = false;
        }
        digits_rev.push_back(static_cast<char>('0' + rem));
        if (zero) break;
    }
    std::reverse(digits_rev.begin(), digits_rev.end());
    return digits_rev;
}

std::string uid_from_digest(Digest d) {
    std::array<std::uint8_t, 16> n{};
    std::memcpy(n.data(), d.data(), 16);
    n[6] = static_cast<std::uint8_t>((n[6] & 0x0Fu) | 0x80u);  // UUID version 8 (RFC 9562 5.8)
    n[8] = static_cast<std::uint8_t>((n[8] & 0x3Fu) | 0x80u);  // variant 10
    secure_wipe(d.data(), d.size());
    return "2.25." + to_decimal(n);
}

std::int32_t offset_from_digest(Digest d, std::uint32_t max_days) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | d[static_cast<std::size_t>(i)];
    secure_wipe(d.data(), d.size());
    return -static_cast<std::int32_t>(1u + static_cast<std::uint32_t>(v % max_days));
}

constexpr std::string_view kDatePrefix = std::string_view("SIAARA-DEID-DATE-OFFSET\0", 24);
constexpr std::string_view kPatientIdPrefix = std::string_view("SIAARA-DEID-PATIENT-ID\0", 23);
constexpr std::string_view kDummyUidPrefix = std::string_view("SIAARA-DEID-DUMMY-UID\0", 22);

// ---------------------------------------------------------------- engine
class Engine {
public:
    Engine(const Options& opt, Report& rep) : opt_(opt), rep_(rep) {
        hmac_.set_key(opt.key);
        for (const std::string& w : opt.safe_words) {
            std::string lw;
            for (char c : w) lw.push_back(lower(c));
            if (!lw.empty()) safe_.insert(std::move(lw));
        }
    }

    void set_context(std::string sop_class, std::string_view patient_id) {
        sop_class_ = std::move(sop_class);
        offset_ = offset_from_digest(hmac_.mac(kDatePrefix, patient_id), opt_.max_shift_days);
        patient_pseudonym_ = "ID";
        const Digest d = hmac_.mac(kPatientIdPrefix, patient_id);
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (int i = 0; i < 8; ++i) {
            patient_pseudonym_.push_back(kHex[d[static_cast<std::size_t>(i)] >> 4]);
            patient_pseudonym_.push_back(kHex[d[static_cast<std::size_t>(i)] & 15]);
        }
    }

    std::string map_uid(std::string_view uid) const { return uid_from_digest(hmac_.mac(uid)); }

    Status process(Dataset& ds, bool clean_mode, std::uint32_t depth);

private:
    static char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
    bool on(std::uint32_t bit) const noexcept { return (opt_.options & bit) != 0; }

    Action resolve(Action a, Tag tag) const;
    Action unlisted_action(const Dataset& ds, const Element& e, VR vr, bool clean_mode) const;
    // Returns false when the element is to be dropped.
    bool apply(Element& e, VR vr, Action a, bool clean_mode, bool only_unknown_uids, std::uint32_t depth,
               Status& err);
    Bytes dummy(Tag tag, VR vr, const Element& e);
    Bytes map_uids(const Element& e, bool only_unknown);
    bool clean(Element& e, VR vr, std::uint32_t depth, Status& err);
    std::string clean_text(std::string_view raw, VR vr) const;
    bool shift_dates(Element& e, VR vr);
    bool materialize_sequence(Element& e);
    Status walk_items(Element& e, bool clean_mode, std::uint32_t depth);
    bool process_private(const Dataset& ds, Element& e, std::uint32_t depth, std::set<std::uint32_t>& blocks,
                         Status& err);

    const Options& opt_;
    Report& rep_;
    HmacSha256 hmac_;
    std::set<std::string> safe_;
    std::string sop_class_;
    std::string patient_pseudonym_;
    std::int32_t offset_ = -1;
    std::vector<Tag> path_;
};

Action Engine::resolve(Action a, Tag tag) const {
    if (a != Action::XZ && a != Action::XD && a != Action::XZD && a != Action::ZD && a != Action::XZU) return a;
    int type = 0;
    if (opt_.attribute_type) type = opt_.attribute_type(sop_class_, path_, tag);
    if (type == 1) return a == Action::XZU ? Action::U : Action::D;
    if (type == 2) return Action::Z;
    if (type == 3) return a == Action::ZD ? Action::Z : Action::X;
    if (opt_.combined == CombinedPolicy::Remove) return a == Action::ZD ? Action::Z : Action::X;
    switch (a) {
        case Action::XZ: return Action::Z;
        case Action::XZU: return Action::U;
        default: return Action::D;
    }
}

Action Engine::unlisted_action(const Dataset& ds, const Element& e, VR vr, bool clean_mode) const {
    const std::uint16_t g = tag_group(e.tag);
    if (vr == VR::SQ) return Action::K;
    if (vr == VR::UI) {
        if (on(RetainUids) || !opt_.remap_unlisted_uids) return Action::K;
        return Action::U;  // well-known values are skipped value by value
    }
    if (opt_.dates_by_vr && is_date_vr(vr)) {
        if (on(RetainLongitudinalFullDates)) return Action::K;
        if (on(RetainLongitudinalModifiedDates)) return Action::C;
        return Action::XZ;
    }
    const bool overlay_text = (g & 0xFF00u) == 0x6000u && on(CleanGraphics);
    if (is_text_vr(vr) && (clean_mode || overlay_text)) {
        // Code Value / Meaning of a published coding scheme are not free text.
        const Tag tg = e.tag;
        if (tg == t::CodeValue || tg == t::CodeMeaning || tg == t::CodingSchemeVersion || tg == t::LongCodeValue ||
            tg == t::URNCodeValue || tg == t::CodingSchemeDesignator) {
            if (const Element* csd = ds.find(t::CodingSchemeDesignator)) {
                const std::string_view scheme = trim_all(csd->value.as_chars());
                for (std::string_view s : kStandardSchemes)
                    if (s == scheme) return Action::K;
            }
        }
        return Action::C;
    }
    if (opt_.dates_by_vr && vr == VR::PN) return Action::XZ;
    return Action::K;
}

Bytes Engine::map_uids(const Element& e, bool only_unknown) {
    std::string out;
    bool first = true;
    for (std::string_view v : split_values(e.value.as_chars(), VR::UI)) {
        if (!first) out.push_back('\\');
        first = false;
        const std::string_view u = trim_all(v);
        if (u.empty()) continue;
        if (only_unknown && well_known_uid(u)) {
            out.append(u);
            continue;
        }
        out += map_uid(u);
        ++rep_.uids_replaced;
    }
    return string_value(std::move(out), VR::UI);
}

Bytes Engine::dummy(Tag tag, VR vr, const Element& e) {
    switch (vr) {
        case VR::AE: case VR::CS: case VR::LO: case VR::LT: case VR::PN: case VR::SH: case VR::ST:
        case VR::UC: case VR::UT:
            if (tag == siaara::tags::PatientID) return string_value(patient_pseudonym_, vr);
            return string_value(std::string(kDummy), vr);
        case VR::AS: return string_value("000Y", vr);
        case VR::DA: return string_value("19000101", vr);
        case VR::DT: return string_value("19000101000000", vr);
        case VR::TM: return string_value("000000", vr);
        case VR::DS: case VR::IS: return string_value("0", vr);
        case VR::UR: return string_value("urn:oid:2.25.0", vr);
        case VR::UI: {
            const std::string_view u = trim_all(e.value.as_chars());
            if (!u.empty()) return map_uids(e, false);
            char tb[8];
            for (int i = 0; i < 8; ++i) tb[i] = "0123456789ABCDEF"[(tag >> (28 - 4 * i)) & 15u];
            ++rep_.uids_replaced;
            return string_value(uid_from_digest(hmac_.mac(kDummyUidPrefix, std::string_view(tb, 8))), vr);
        }
        default: {
            std::size_t n = vr_value_size(vr);
            if (vr == VR::AT) n = 4;
            if (vr == VR::OF || vr == VR::OL) n = 4;
            if (vr == VR::OD || vr == VR::OV) n = 8;
            if (n < 2) n = 2;
            const std::vector<std::uint8_t> zeros(n, 0);
            return Bytes::copy_of(zeros.data(), zeros.size());
        }
    }
}

std::string Engine::clean_text(std::string_view raw, VR vr) const {
    std::string out;
    bool first_value = true;
    for (std::string_view v : split_values(raw, vr)) {
        if (!first_value) out.push_back('\\');
        first_value = false;
        if (trim_all(v).empty()) continue;
        std::string kept;
        std::size_t i = 0;
        while (i < v.size()) {
            const auto alnum = [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            };
            if (!alnum(v[i])) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < v.size() && alnum(v[j])) ++j;
            // Tokens touching a non-ASCII byte are part of a longer word; never keep them.
            const bool bounded = (i == 0 || static_cast<unsigned char>(v[i - 1]) < 0x80) &&
                                 (j == v.size() || static_cast<unsigned char>(v[j]) < 0x80);
            std::string tok(v.substr(i, j - i));
            std::string low;
            for (char c : tok) low.push_back(lower(c));
            if (bounded && safe_.count(low)) {
                if (!kept.empty()) kept.push_back(' ');
                kept += vr == VR::CS ? [&] {
                    std::string up;
                    for (char c : tok) up.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
                    return up;
                }() : tok;
            }
            i = j;
        }
        if (kept.empty()) kept = kCleaned;
        const std::size_t cap = max_length(vr);
        if (kept.size() > cap) kept.resize(cap);
        while (!kept.empty() && kept.back() == ' ') kept.pop_back();
        out += kept;
    }
    return out;
}

bool Engine::shift_dates(Element& e, VR vr) {
    std::string out;
    bool first = true;
    for (std::string_view v0 : split_values(e.value.as_chars(), vr)) {
        if (!first) out.push_back('\\');
        first = false;
        const std::string_view v = trim_all(v0);
        if (v.empty()) continue;
        std::string r;
        if (!shift_date(v, vr, offset_, r)) return false;
        out += r;
        ++rep_.dates_shifted;
    }
    e.value = string_value(std::move(out), vr);
    return true;
}

bool Engine::materialize_sequence(Element& e) {
    // A sequence stored as UN with a defined length holds implicit VR little endian items
    // (PS3.5 6.2.2). Re-read it as a one-element implicit VR data set.
    if (e.vr == VR::SQ) return true;
    if (e.value.empty()) {
        e.vr = VR::SQ;
        e.value = {};
        return true;
    }
    if (e.value.size() > 0xFFFFFFF0u) return false;
    std::vector<std::uint8_t> buf(8 + e.value.size());
    store_le16(buf.data(), tag_group(e.tag));
    store_le16(buf.data() + 2, tag_element(e.tag));
    store_le32(buf.data() + 4, static_cast<std::uint32_t>(e.value.size()));
    std::memcpy(buf.data() + 8, e.value.data(), e.value.size());
    ReadOptions ro;
    ro.lenient = false;
    auto r = read_dataset(Bytes::copy_of(buf.data(), buf.size()), ts::ImplicitVRLittleEndian, ro);
    if (!r) return false;
    Element* parsed = r->find(e.tag);
    if (!parsed || parsed->vr != VR::SQ) return false;
    e.vr = VR::SQ;
    e.value = {};
    e.items = std::move(parsed->items);
    return true;
}

Status Engine::walk_items(Element& e, bool clean_mode, std::uint32_t depth) {
    path_.push_back(e.tag);
    for (Dataset& item : e.items) {
        ++rep_.items_visited;
        if (Status s = process(item, clean_mode, depth + 1); !s) {
            path_.pop_back();
            return s;
        }
    }
    path_.pop_back();
    return Status::ok();
}

bool Engine::clean(Element& e, VR vr, std::uint32_t depth, Status& err) {
    ++rep_.cleaned;
    if (vr == VR::SQ) {
        if (!materialize_sequence(e)) return false;
        err = walk_items(e, true, depth);
        return true;
    }
    if (e.is_encapsulated()) return false;
    if (vr == VR::DA || vr == VR::DT) {
        if (!shift_dates(e, vr)) {
            e.value = {};
            rep_.flags |= UnparseableValuesRemoved;
        }
        e.vr = vr;
        return true;
    }
    if (vr == VR::TM) {
        for (std::string_view v : split_values(e.value.as_chars(), vr))
            if (!trim_all(v).empty() && !valid_time(trim_all(v))) {
                e.value = {};
                rep_.flags |= UnparseableValuesRemoved;
                break;
            }
        e.vr = vr;
        return true;
    }
    if (vr == VR::UI) {
        e.value = map_uids(e, false);
        e.vr = vr;
        return true;
    }
    if (vr == VR::DS || vr == VR::IS || vr == VR::AS) {
        bool ok = true;
        for (std::string_view v : split_values(e.value.as_chars(), vr))
            ok = ok && (vr == VR::AS ? valid_age(v) : valid_number(v, vr == VR::IS));
        if (!ok) {
            e.value = {};
            rep_.flags |= UnparseableValuesRemoved;
        }
        e.vr = vr;
        return true;
    }
    if (vr_is_string(vr)) {
        e.value = string_value(clean_text(e.value.as_chars(), vr), vr);
        e.vr = vr;
        return true;
    }
    if (is_bulk_vr(vr)) {
        const std::vector<std::uint8_t> zeros(e.value.size(), 0);
        e.value = Bytes::copy_of(zeros.data(), zeros.size());
        return true;
    }
    return true;  // numeric binary values carry no text
}

bool Engine::apply(Element& e, VR vr, Action a, bool clean_mode, bool only_unknown_uids, std::uint32_t depth,
                   Status& err) {
    switch (a) {
        case Action::X:
        case Action::None:
            ++rep_.removed;
            return false;
        case Action::Z:
            ++rep_.emptied;
            e.items.clear();
            e.fragments.clear();
            e.value = {};
            e.undefined_length = false;
            if (vr != VR::UN) e.vr = vr;
            return true;
        case Action::D:
            ++rep_.dummied;
            if (vr == VR::SQ) {
                e.vr = VR::SQ;
                e.value = {};
                e.items.assign(1, Dataset{});
                return true;
            }
            e.value = dummy(e.tag, vr, e);
            e.fragments.clear();
            e.undefined_length = false;
            if (vr != VR::UN) e.vr = vr;
            return true;
        case Action::K:
            ++rep_.kept;
            if (vr == VR::SQ) {
                if (!materialize_sequence(e)) {
                    ++rep_.removed;
                    return false;
                }
                err = walk_items(e, clean_mode, depth);
            }
            return true;
        case Action::C:
            if (!clean(e, vr, depth, err)) {
                ++rep_.removed;
                return false;
            }
            return true;
        case Action::U:
            if (vr == VR::SQ) {
                ++rep_.kept;
                if (!materialize_sequence(e)) {
                    ++rep_.removed;
                    return false;
                }
                err = walk_items(e, clean_mode, depth);
                return true;
            }
            if (vr != VR::UI || e.is_encapsulated()) return apply(e, vr, Action::D, clean_mode, false, depth, err);
            e.value = map_uids(e, only_unknown_uids);
            e.vr = VR::UI;
            return true;
        default:
            return apply(e, vr, resolve(a, e.tag), clean_mode, only_unknown_uids, depth, err);
    }
}

bool Engine::process_private(const Dataset& ds, Element& e, std::uint32_t depth, std::set<std::uint32_t>& blocks,
                             Status& err) {
    const std::uint16_t g = tag_group(e.tag);
    const std::uint16_t el = tag_element(e.tag);
    const Action a = effective_action(private_attributes_row(), opt_.options);
    if (g < 0x0009 || el < 0x0010 || a != Action::C) return false;
    if (el <= 0x00FF) return true;  // creator: kept only if its block keeps an element (see process)
    const Element* creator = ds.find(make_tag(g, static_cast<std::uint16_t>(el >> 8)));
    if (!creator || creator->vr == VR::SQ) return false;
    const SafePrivateRow* row =
        find_safe_private(g, trim_all(creator->value.as_chars()), static_cast<std::uint8_t>(el & 0xFFu));
    if (!row) return false;
    if (row->is_sequence) {
        if (e.vr != VR::SQ) return false;  // cannot be reviewed
        // E.3.10: the content of a safe private sequence is still handled on its own merits.
        err = walk_items(e, false, depth);
    } else if (e.vr == VR::SQ) {
        return false;
    }
    blocks.insert((static_cast<std::uint32_t>(g) << 8) | (el >> 8));
    return true;
}

Status Engine::process(Dataset& ds, bool clean_mode, std::uint32_t depth) {
    if (depth > kMaxDepth) return Status(Errc::LimitExceeded, "sequence nesting too deep for de-identification");
    std::vector<Element> out;
    out.reserve(ds.size());
    std::set<std::uint32_t> private_blocks;
    std::set<std::uint16_t> drop_groups;
    for (Element& e : ds.elements()) {
        const std::uint16_t g = tag_group(e.tag);
        const std::uint16_t el = tag_element(e.tag);
        if (el == 0 || g == 0x0002 || g == 0x0004) {  // group lengths, misplaced meta, DICOMDIR
            ++rep_.removed;
            continue;
        }
        Status err;
        if (g & 1u) {
            if (process_private(ds, e, depth, private_blocks, err)) {
                out.push_back(std::move(e));
            } else {
                ++rep_.private_removed;
            }
            if (!err) return err;
            continue;
        }
        VR vr = e.vr;
        if (vr == VR::UN || vr == VR::None) {
            if (const dict::Entry* de = dict::lookup(e.tag)) vr = de->vr == VR::None ? VR::UN : de->vr;
        }
        const ProfileRow* row = find_row(e.tag);
        Action a;
        bool only_unknown = false;
        if (row) {
            a = resolve(effective_action(*row, opt_.options), e.tag);
        } else {
            a = resolve(unlisted_action(ds, e, vr, clean_mode), e.tag);
            only_unknown = a == Action::U;
        }
        const bool keep = apply(e, vr, a, clean_mode, only_unknown, depth, err);
        if (!err) return err;
        if (keep) {
            out.push_back(std::move(e));
        } else if (row && row->tag == t::OverlayData && row->mask != 0xFFFFFFFFu) {
            drop_groups.insert(g);  // the rest of the overlay group is meaningless without its data
        }
    }
    // Private creators: keep those whose block kept an element.
    std::erase_if(out, [&](const Element& e) {
        const std::uint16_t g = tag_group(e.tag);
        const std::uint16_t el = tag_element(e.tag);
        if ((g & 1u) && el >= 0x0010 && el <= 0x00FF &&
            !private_blocks.count((static_cast<std::uint32_t>(g) << 8) | el)) {
            ++rep_.private_removed;
            return true;
        }
        if (drop_groups.count(g)) {
            ++rep_.removed;
            return true;
        }
        return false;
    });
    for (const Element& e : out)
        if ((tag_group(e.tag) & 1u) && tag_element(e.tag) > 0x00FF) ++rep_.private_kept;
    ds.elements() = std::move(out);
    return Status::ok();
}

// Overlays embedded in unused bits of Pixel Data (retired, PS3.3 C.9.2): clears those bits and
// returns the overlay groups concerned.
std::vector<std::uint16_t> clear_embedded_overlays(Dataset& ds) {
    std::vector<std::uint16_t> groups;
    for (std::uint16_t g = 0x6000; g <= 0x601E; g = static_cast<std::uint16_t>(g + 2)) {
        const Tag ba = make_tag(g, tag_element(t::OverlayBitsAllocated));
        const Tag bp = make_tag(g, tag_element(t::OverlayBitPosition));
        if (!ds.contains(bp) || ds.contains(make_tag(g, tag_element(t::OverlayData)))) continue;
        auto bits_alloc = ds.get_int(ba);
        auto pos = ds.get_int(bp);
        if (!pos || (bits_alloc && *bits_alloc == 1)) continue;
        groups.push_back(g);
        Element* px = ds.find(siaara::tags::PixelData);
        auto alloc = ds.get_int(siaara::tags::BitsAllocated);
        if (!px || px->is_encapsulated() || !alloc || (*alloc != 16 && *alloc != 32 && *alloc != 8)) continue;
        if (*pos < 0 || *pos >= *alloc) continue;
        std::vector<std::uint8_t> v(px->value.data(), px->value.data() + px->value.size());
        const std::size_t bytes = static_cast<std::size_t>(*alloc / 8);
        const std::size_t byte_in = static_cast<std::size_t>(*pos / 8);
        const auto mask = static_cast<std::uint8_t>(~(1u << (*pos % 8)));
        for (std::size_t i = byte_in; i < v.size(); i += bytes) v[i] &= mask;
        px->value = Bytes::copy_of(v.data(), v.size());
    }
    return groups;
}

}  // namespace

// ---------------------------------------------------------------- public helpers

std::string replacement_uid(std::span<const std::uint8_t> key, std::string_view uid) {
    HmacSha256 h(key);
    return uid_from_digest(h.mac(trim_all(uid)));
}

std::int32_t date_offset_days(std::span<const std::uint8_t> key, std::string_view patient_id,
                              std::uint32_t max_shift_days) {
    HmacSha256 h(key);
    return offset_from_digest(h.mac(kDatePrefix, patient_id), max_shift_days ? max_shift_days : 1u);
}

bool shift_date(std::string_view value, VR vr, std::int32_t days, std::string& out) {
    out.clear();
    if (vr == VR::TM) {
        if (!valid_time(value)) return false;
        out.assign(value);
        return true;
    }
    if (vr == VR::DA) {
        // Query ranges "A-B", "-B", "A-" (PS3.4 C.2.2.2.5) and the ACR-NEMA "YYYY.MM.DD" form.
        const std::size_t dash = value.find('-');
        if (dash != std::string_view::npos) {
            std::string a, b;
            const std::string_view l = value.substr(0, dash), r = value.substr(dash + 1);
            if ((l.empty() && r.empty()) || (!l.empty() && !shift_date(l, vr, days, a)) ||
                (!r.empty() && !shift_date(r, vr, days, b)))
                return false;
            out = a + "-" + b;
            return true;
        }
        std::string compact;
        if (value.size() == 10 && value[4] == '.' && value[7] == '.') {
            compact.append(value.substr(0, 4)).append(value.substr(5, 2)).append(value.substr(8, 2));
            value = compact;
        }
        if (value.size() != 8) return false;
        return shift_ymd(value, days, out);
    }
    if (vr == VR::DT) {
        // YYYY[MM[DD[HH[MM[SS[.F]]]]]][&ZZXX]: shift the date, keep the rest as written.
        std::size_t n = 0;
        while (n < value.size() && value[n] >= '0' && value[n] <= '9') ++n;
        const std::size_t date_len = n >= 8 ? 8 : (n >= 6 ? 6 : (n >= 4 ? 4 : 0));
        if (date_len == 0 || (n > 8 && n != 10 && n != 12 && n != 14) || (n < 8 && n != date_len)) return false;
        const std::string_view rest = value.substr(date_len);
        for (char c : rest)
            if (!((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-')) return false;
        if (n > 8 && !valid_time(value.substr(8, n - 8))) return false;
        if (!shift_ymd(value.substr(0, date_len), days, out)) return false;
        out.append(rest);
        return true;
    }
    return false;
}

Status deidentify(DicomFile& file, const Options& opt, Report* report) {
    if (opt.key.size() < 16) return Status(Errc::InvalidArgument, "de-identification key must be at least 16 bytes");
    if (opt.options & ~kAllOptionBits) return Status(Errc::InvalidArgument, "unknown de-identification option");
    if ((opt.options & RetainLongitudinalFullDates) && (opt.options & RetainLongitudinalModifiedDates))
        return Status(Errc::InvalidArgument, "the Full Dates and Modified Dates options are mutually exclusive");
    if (opt.max_shift_days == 0 || opt.max_shift_days > 3'650'000)
        return Status(Errc::InvalidArgument, "max_shift_days must be 1..3650000");

    std::string sop_class(trim_all(file.dataset.get_raw_string(siaara::tags::SOPClassUID).value_or("")));
    if (sop_class.empty()) sop_class = trim_all(file.meta.get_raw_string(siaara::tags::MediaStorageSOPClassUID).value_or(""));
    if (sop_class == kDicomdirSopClass)
        return Status(Errc::Unsupported, "DICOMDIR files must be rebuilt from de-identified files (PS3.15 E.1.1)");

    Report rep;
    DicomFile copy = file;
    Dataset& ds = copy.dataset;

    // Burned-in annotation and recognizable features (E.3.1, E.3.2) are judged on the original.
    const bool has_pixels = ds.contains(siaara::tags::PixelData) || ds.contains(siaara::tags::FloatPixelData) ||
                            ds.contains(siaara::tags::DoubleFloatPixelData);
    const std::string_view burned = trim_all(ds.get_raw_string(t::BurnedInAnnotation).value_or(""));
    const std::string_view visual = trim_all(ds.get_raw_string(t::RecognizableVisualFeatures).value_or(""));

    if (!opt.pixel_regions.empty()) {
        if (Status s = blank_pixel_regions(ds, opt.pixel_regions); !s) return s;
        rep.flags |= PixelDataCleaned;
    }
    for (std::uint16_t g : clear_embedded_overlays(ds)) {
        std::erase_if(ds.elements(), [g](const Element& e) { return tag_group(e.tag) == g; });
        ++rep.removed;
    }

    std::string_view original_sop_instance = trim_all(ds.get_raw_string(siaara::tags::SOPInstanceUID).value_or(""));
    if (original_sop_instance.empty())
        original_sop_instance = trim_all(file.meta.get_raw_string(siaara::tags::MediaStorageSOPInstanceUID).value_or(""));
    const std::string original_instance(original_sop_instance);

    {
        Engine engine(opt, rep);
        engine.set_context(sop_class, trim_all(file.dataset.get_raw_string(siaara::tags::PatientID).value_or("")));
        if (Status s = engine.process(ds, false, 0); !s) return s;

        // E.1.1: Patient Identity Removed, De-identification Method and its code sequence.
        (void)ds.set_string(t::PatientIdentityRemoved, VR::CS, "YES");
        std::vector<const MethodCode*> codes;
        auto add_code = [&](std::string_view value) {
            for (const MethodCode& c : method_codes())
                if (c.value == value) codes.push_back(&c);
        };
        add_code("113100");
        const bool pixels_clean = opt.pixel_data_clean;
        if (pixels_clean && has_pixels) add_code("113101");
        for (std::size_t i = 0; i < kOptionCount; ++i)
            if (opt.options & (1u << i)) add_code(kOptionCode[i]);
        std::sort(codes.begin(), codes.end(), [](const MethodCode* a, const MethodCode* b) { return a->value < b->value; });
        std::string method;
        Element seq;
        seq.tag = t::DeidentificationMethodCodeSequence;
        seq.vr = VR::SQ;
        for (const MethodCode* c : codes) {
            if (!method.empty()) method.push_back('\\');
            method.append(c->meaning);
            Dataset item;
            (void)item.set_string(t::CodeValue, VR::SH, c->value);
            (void)item.set_string(t::CodingSchemeDesignator, VR::SH, c->scheme);
            (void)item.set_string(t::CodeMeaning, VR::LO, c->meaning);
            seq.items.push_back(std::move(item));
        }
        (void)ds.set_string(t::DeidentificationMethod, VR::LO, method);
        ds.put(std::move(seq));

        if (opt.options & RetainLongitudinalFullDates)
            (void)ds.set_string(t::LongitudinalTemporalInformationModified, VR::CS, "UNMODIFIED");
        if (opt.options & RetainLongitudinalModifiedDates)
            (void)ds.set_string(t::LongitudinalTemporalInformationModified, VR::CS, "MODIFIED");

        if (has_pixels) {
            if (pixels_clean) {
                (void)ds.set_string(t::BurnedInAnnotation, VR::CS, "NO");
            } else if (burned != "NO") {
                rep.flags |= BurnedInAnnotationRemains | NotFullyDeidentified;
            }
        }
        if (visual == "YES") rep.flags |= RecognizableFeaturesRemain | NotFullyDeidentified;

        // File meta: replaced with a description of this application (E.1.1); the writer fills
        // in the Implementation Class UID and Version Name. The preamble is written as zeros.
        Dataset meta;
        if (const Element* v = copy.meta.find(siaara::tags::FileMetaInformationVersion)) meta.put(*v);
        if (!sop_class.empty()) (void)meta.set_string(siaara::tags::MediaStorageSOPClassUID, VR::UI, sop_class);
        std::string instance(trim_all(ds.get_raw_string(siaara::tags::SOPInstanceUID).value_or("")));
        if (instance.empty() && !original_instance.empty())
            instance = (opt.options & RetainUids) ? original_instance : engine.map_uid(original_instance);
        if (!instance.empty()) (void)meta.set_string(siaara::tags::MediaStorageSOPInstanceUID, VR::UI, instance);
        if (!copy.transfer_syntax.empty())
            (void)meta.set_string(siaara::tags::TransferSyntaxUID, VR::UI, copy.transfer_syntax);
        copy.meta = std::move(meta);
        copy.had_preamble = false;
        copy.warnings.clear();
    }

    file = std::move(copy);
    if (report) *report = rep;
    return Status::ok();
}

}  // namespace siaara::deid
