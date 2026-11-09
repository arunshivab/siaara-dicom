// SIAARA DICOM Toolkit - clinical-object layer shared helpers (see common.hpp).
#include "clinical/common.hpp"

#include "charset/charset.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <system_error>

namespace siaara::clinical {

namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\0')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return s;
}

// Parses exactly `n` digits at s[pos..]; false if not all digits.
bool digits(std::string_view s, std::size_t pos, std::size_t n, int& out) noexcept {
    if (pos + n > s.size()) return false;
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_digit(s[pos + i])) return false;
        v = v * 10 + (s[pos + i] - '0');
    }
    out = v;
    return true;
}

// Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar. Eras of 400 years
// (146097 days) starting on 1 March, so the leap day is the last day of the shifted year.
std::int64_t days_from_civil(std::int64_t y, int m, int d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;                                 // [0, 399]
    const std::int64_t mp = (m + 9) % 12;                                   // March = 0
    const std::int64_t doy = (153 * mp + 2) / 5 + d - 1;                    // [0, 365]
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;         // [0, 146096]
    return era * 146097 + doe - 719468;                                     // 719468 = days 0000-03-01 .. 1970-01-01
}

bool valid_date(int y, int m, int d) noexcept {
    if (m < 1 || m > 12 || d < 1) return false;
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dim = kDays[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) dim = 29;
    return d <= dim;
}

}  // namespace

// ---------------------------------------------------------------- values
std::optional<double> parse_decimal(std::string_view s) noexcept {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') neg = s[i++] == '-';
    std::uint64_t mant = 0;
    int sig = 0, exp10 = 0;
    bool any = false;
    for (; i < s.size() && is_digit(s[i]); ++i) {
        any = true;
        if (sig < 19) {
            mant = mant * 10 + static_cast<unsigned>(s[i] - '0');
            if (mant) ++sig;
        } else {
            ++exp10;
        }
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        for (; i < s.size() && is_digit(s[i]); ++i) {
            any = true;
            if (sig < 19) {
                mant = mant * 10 + static_cast<unsigned>(s[i] - '0');
                if (mant) ++sig;
                --exp10;
            }
        }
    }
    if (!any) return std::nullopt;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        if (i >= s.size() || !is_digit(s[i])) return std::nullopt;
        int e = 0;
        for (; i < s.size() && is_digit(s[i]); ++i)
            if (e < 100000) e = e * 10 + (s[i] - '0');
        exp10 += eneg ? -e : e;
    }
    if (i != s.size()) return std::nullopt;
    static constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    double v = static_cast<double>(mant);
    if (mant != 0) {
        int e = exp10;
        while (e > 22) {
            v *= 1e22;
            e -= 22;
            if (std::isinf(v)) return std::nullopt;
        }
        while (e < -22) {
            v /= 1e22;
            e += 22;
            if (v == 0.0) {
                e = 0;  // underflowed to zero: nothing left to scale (kPow10 has only 23 entries)
                break;
            }
        }
        // Dividing by an exact power of ten keeps "0.1"-style values correctly rounded.
        v = e >= 0 ? v * kPow10[e] : v / kPow10[-e];
    }
    if (!std::isfinite(v)) return std::nullopt;
    return neg ? -v : v;
}

std::optional<std::vector<double>> get_numbers(const Dataset& ds, Tag tag, std::size_t max_values) {
    const Element* e = ds.find(tag);
    if (!e || e->is_sequence() || e->is_encapsulated()) return std::nullopt;
    std::vector<double> out;
    const std::uint8_t* p = e->value.data();
    const std::size_t n = e->value.size();
    auto binary = [&](std::size_t w, auto&& load) -> bool {
        if (n % w) return false;
        if (n / w > max_values) return false;
        out.reserve(n / w);
        for (std::size_t i = 0; i < n; i += w) out.push_back(load(p + i));
        return true;
    };
    bool ok = true;
    switch (e->vr) {
        case VR::FL: case VR::OF:
            ok = binary(4, [](const std::uint8_t* q) {
                const std::uint32_t b = load_le32(q);
                float f;
                std::memcpy(&f, &b, 4);
                return static_cast<double>(f);
            });
            break;
        case VR::FD: case VR::OD:
            ok = binary(8, [](const std::uint8_t* q) {
                const std::uint64_t b = load_le64(q);
                double d;
                std::memcpy(&d, &b, 8);
                return d;
            });
            break;
        case VR::US: ok = binary(2, [](const std::uint8_t* q) { return static_cast<double>(load_le16(q)); }); break;
        case VR::SS:
            ok = binary(2, [](const std::uint8_t* q) { return static_cast<double>(static_cast<std::int16_t>(load_le16(q))); });
            break;
        case VR::UL: ok = binary(4, [](const std::uint8_t* q) { return static_cast<double>(load_le32(q)); }); break;
        case VR::SL:
            ok = binary(4, [](const std::uint8_t* q) { return static_cast<double>(static_cast<std::int32_t>(load_le32(q))); });
            break;
        case VR::DS: case VR::IS: case VR::UN: case VR::None: {
            const std::string_view s = e->value.as_chars();
            if (trim(s).empty()) return out;
            std::size_t start = 0;
            while (true) {
                const std::size_t bs = s.find('\\', start);
                const std::string_view part = s.substr(start, bs == std::string_view::npos ? std::string_view::npos : bs - start);
                auto v = parse_decimal(part);
                if (!v) return std::nullopt;
                if (out.size() >= max_values) return std::nullopt;
                out.push_back(*v);
                if (bs == std::string_view::npos) break;
                start = bs + 1;
            }
            break;
        }
        default: return std::nullopt;
    }
    if (!ok) return std::nullopt;
    return out;
}

std::optional<std::vector<double>> get_numbers_n(const Dataset& ds, Tag tag, std::size_t n) {
    auto v = get_numbers(ds, tag, n + 1);
    if (!v || v->size() != n) return std::nullopt;
    return v;
}

std::optional<std::string> get_text(const Dataset& ds, Tag tag, std::size_t vm) {
    auto s = ds.get_string(tag, vm);
    if (!s) return std::nullopt;
    const std::string_view t = trim(*s);
    if (t.empty()) return std::nullopt;
    return std::string(t);
}

const std::vector<Dataset>& items_of(const Dataset& ds, Tag tag) noexcept {
    static const std::vector<Dataset> kEmpty;
    const Element* e = ds.find(tag);
    if (!e || !e->is_sequence()) return kEmpty;
    return e->items;
}

const Dataset* first_item(const Dataset& ds, Tag tag) noexcept {
    const auto& it = items_of(ds, tag);
    return it.empty() ? nullptr : &it.front();
}

const Dataset* functional_group(const Dataset& ds, std::uint32_t frame, Tag macro, bool* from_per_frame) noexcept {
    const auto& per = items_of(ds, 0x52009230);
    if (frame < per.size()) {
        if (const Dataset* m = first_item(per[frame], macro)) {
            if (from_per_frame) *from_per_frame = true;
            return m;
        }
    }
    if (const Dataset* shared = first_item(ds, 0x52009229)) {
        if (const Dataset* m = first_item(*shared, macro)) {
            if (from_per_frame) *from_per_frame = false;
            return m;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- dates and times
std::optional<double> parse_tm(std::string_view s) noexcept {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    // Retired ACR-NEMA form "HH:MM:SS.frac": drop the colons.
    char buf[32];
    std::size_t n = 0;
    for (char c : s) {
        if (c == ':') continue;
        if (n >= sizeof buf) return std::nullopt;
        buf[n++] = c;
    }
    const std::string_view t(buf, n);
    int hh = 0, mm = 0, ss = 0;
    if (!digits(t, 0, 2, hh) || hh > 23) return std::nullopt;
    std::size_t pos = 2;
    if (pos < t.size()) {
        if (!digits(t, pos, 2, mm) || mm > 59) return std::nullopt;
        pos += 2;
    }
    if (pos < t.size() && t[pos] != '.') {
        if (!digits(t, pos, 2, ss) || ss > 60) return std::nullopt;  // 60: leap second
        pos += 2;
    }
    double frac = 0.0;
    if (pos < t.size()) {
        if (t[pos] != '.' || pos + 1 >= t.size() || t.size() - pos - 1 > 6) return std::nullopt;
        double scale = 0.1;
        for (std::size_t i = pos + 1; i < t.size(); ++i) {
            if (!is_digit(t[i])) return std::nullopt;
            frac += (t[i] - '0') * scale;
            scale /= 10.0;
        }
    }
    return hh * 3600.0 + mm * 60.0 + ss + frac;
}

std::optional<std::int64_t> parse_da(std::string_view s) noexcept {
    s = trim(s);
    int y = 0, m = 0, d = 0;
    if (s.size() == 8) {
        if (!digits(s, 0, 4, y) || !digits(s, 4, 2, m) || !digits(s, 6, 2, d)) return std::nullopt;
    } else if (s.size() == 10 && s[4] == '.' && s[7] == '.') {
        if (!digits(s, 0, 4, y) || !digits(s, 5, 2, m) || !digits(s, 8, 2, d)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (!valid_date(y, m, d)) return std::nullopt;
    return days_from_civil(y, m, d);
}

std::optional<double> parse_dt(std::string_view s, std::optional<int>* utc_offset_seconds) noexcept {
    s = trim(s);
    if (utc_offset_seconds) utc_offset_seconds->reset();
    // Offset suffix &ZZXX.
    const std::size_t sign = s.find_first_of("+-", 4);
    if (sign != std::string_view::npos) {
        const std::string_view off = s.substr(sign);
        int hh = 0, mm = 0;
        if (off.size() != 5 || !digits(off, 1, 2, hh) || !digits(off, 3, 2, mm) || hh > 14 || mm > 59)
            return std::nullopt;
        if (utc_offset_seconds) *utc_offset_seconds = (off[0] == '-' ? -1 : 1) * (hh * 3600 + mm * 60);
        s = s.substr(0, sign);
    }
    int y = 0, m = 1, d = 1;
    if (!digits(s, 0, 4, y)) return std::nullopt;
    std::size_t pos = 4;
    if (pos < s.size()) {
        if (!digits(s, pos, 2, m)) return std::nullopt;
        pos += 2;
    }
    if (pos < s.size()) {
        if (!digits(s, pos, 2, d)) return std::nullopt;
        pos += 2;
    }
    if (!valid_date(y, m, d)) return std::nullopt;
    double secs = 0.0;
    if (pos < s.size()) {
        auto t = parse_tm(s.substr(pos));
        if (!t) return std::nullopt;
        secs = *t;
    }
    return static_cast<double>(days_from_civil(y, m, d)) * 86400.0 + secs;
}

std::optional<double> combine_da_tm(std::string_view da, std::string_view tm) noexcept {
    auto d = parse_da(da);
    auto t = parse_tm(tm);
    if (!d || !t) return std::nullopt;
    return static_cast<double>(*d) * 86400.0 + *t;
}

// ---------------------------------------------------------------- writing
std::string format_ds(double v) {
    if (!std::isfinite(v)) return "0";
    if (v == 0.0) return "0";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    if (r.ec == std::errc() && r.ptr - buf <= 16) return std::string(buf, r.ptr);
    for (int prec = 15; prec >= 1; --prec) {
        r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::general, prec);
        // Rounding near the largest double can round up past it ("1.797693135e+308"), giving
        // text that no longer parses as a finite number: try fewer digits then.
        if (r.ec == std::errc() && r.ptr - buf <= 16 && parse_decimal(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf))))
            return std::string(buf, r.ptr);
    }
    return "0";
}

std::string format_ds_list(const double* v, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out.push_back('\\');
        out += format_ds(v[i]);
    }
    return out;
}

std::string format_is(std::int64_t v) { return std::to_string(v); }

Bytes encode_fl(const float* v, std::size_t n) {
    std::vector<std::uint8_t> b(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t u;
        std::memcpy(&u, &v[i], 4);
        store_le32(b.data() + 4 * i, u);
    }
    return Bytes::copy_of(b.data(), b.size());
}

Bytes encode_fd(const double* v, std::size_t n) {
    std::vector<std::uint8_t> b(n * 8);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t u;
        std::memcpy(&u, &v[i], 8);
        store_le32(b.data() + 8 * i, static_cast<std::uint32_t>(u));
        store_le32(b.data() + 8 * i + 4, static_cast<std::uint32_t>(u >> 32));
    }
    return Bytes::copy_of(b.data(), b.size());
}

namespace {
bool local_now(std::tm& out) noexcept {
    const std::time_t t = std::time(nullptr);
#if defined(_WIN32)
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}
std::string two(int v) {
    std::string s(2, '0');
    s[0] = static_cast<char>('0' + (v / 10) % 10);
    s[1] = static_cast<char>('0' + v % 10);
    return s;
}
}  // namespace

std::string today_da() {
    std::tm tm{};
    if (!local_now(tm)) return "19700101";
    return std::to_string(tm.tm_year + 1900) + two(tm.tm_mon + 1) + two(tm.tm_mday);
}

std::string now_tm() {
    std::tm tm{};
    if (!local_now(tm)) return "000000";
    return two(tm.tm_hour) + two(tm.tm_min) + two(tm.tm_sec);
}

Element make_sequence(Tag tag, std::vector<Dataset> items) {
    Element e;
    e.tag = tag;
    e.vr = VR::SQ;
    e.items = std::move(items);
    return e;
}

Dataset code_item(const Code& c) {
    Dataset d;
    if (c.value.size() > 16) (void)d.set_string(tags::LongCodeValue, VR::UC, c.value);
    else (void)d.set_string(tags::CodeValue, VR::SH, c.value);
    (void)d.set_string(tags::CodingSchemeDesignator, VR::SH, c.scheme);
    if (!c.scheme_version.empty()) (void)d.set_string(tags::CodingSchemeVersion, VR::SH, c.scheme_version);
    (void)d.set_string(tags::CodeMeaning, VR::LO, c.meaning);
    return d;
}

std::optional<Code> read_code_item(const Dataset& item) {
    Code c;
    if (auto v = get_text(item, tags::CodeValue)) c.value = *v;
    else if (auto lv = get_text(item, tags::LongCodeValue)) c.value = *lv;
    else if (auto uv = get_text(item, tags::UrnCodeValue)) c.value = *uv;
    if (auto s = get_text(item, tags::CodingSchemeDesignator)) c.scheme = *s;
    if (auto s = get_text(item, tags::CodingSchemeVersion)) c.scheme_version = *s;
    if (auto m = get_text(item, tags::CodeMeaning)) c.meaning = *m;
    if (c.empty()) return std::nullopt;
    return c;
}

std::optional<Code> read_code(const Dataset& ds, Tag sequence_tag) {
    const Dataset* item = first_item(ds, sequence_tag);
    if (!item) return std::nullopt;
    return read_code_item(*item);
}

void put_text(Dataset& ds, Tag tag, VR vr, std::string_view utf8, bool& non_ascii) {
    for (char c : utf8)
        if (static_cast<unsigned char>(c) >= 0x80) non_ascii = true;
    (void)ds.set_string(tag, vr, utf8);
}

void finish_charset(Dataset& ds, bool non_ascii) {
    if (non_ascii) (void)ds.set_string(siaara::tags::SpecificCharacterSet, VR::CS, "ISO_IR 192");
    else ds.remove(siaara::tags::SpecificCharacterSet);
}

void copy_patient_study(const Dataset* src, Dataset& dst, bool& non_ascii) {
    struct Attr {
        Tag tag;
        VR vr;
        bool type2;
    };
    static constexpr Attr kAttrs[] = {
        {siaara::tags::PatientName, VR::PN, true},     {siaara::tags::PatientID, VR::LO, true},
        {0x00100021, VR::LO, false} /* Issuer of Patient ID */,
        {tags::PatientBirthDate, VR::DA, true},        {tags::PatientSex, VR::CS, true},
        {siaara::tags::StudyInstanceUID, VR::UI, false}, {tags::StudyDate, VR::DA, true},
        {tags::StudyTime, VR::TM, true},               {tags::ReferringPhysicianName, VR::PN, true},
        {tags::StudyId, VR::SH, true},                 {tags::AccessionNumber, VR::SH, true},
        {tags::StudyDescription, VR::LO, false},
    };
    const charset::CharacterSet cs = src ? charset::character_set_of(*src) : charset::CharacterSet{};
    for (const auto& a : kAttrs) {
        std::optional<std::string> v;
        if (src && src->contains(a.tag)) {
            if (charset::vr_uses_character_set(a.vr)) v = charset::get_utf8(*src, a.tag, 0, &cs);
            else v = src->get_string(a.tag);
        }
        if (v) put_text(dst, a.tag, a.vr, *v, non_ascii);
        else if (a.type2) put_text(dst, a.tag, a.vr, "", non_ascii);
    }
}

bool cs_valid(std::string_view s) noexcept {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == ' ')) return false;
    return true;
}

// ---------------------------------------------------------------- UIDs
std::string new_uid() {
    static std::mutex mu;
    static std::mt19937_64 engine = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    std::uint64_t hi, lo;
    {
        std::lock_guard<std::mutex> lock(mu);
        hi = engine();
        lo = engine();
    }
    // RFC 4122 version 4 (random) and variant 10xx bits.
    hi = (hi & ~(std::uint64_t{0xF} << 12)) | (std::uint64_t{0x4} << 12);
    lo = (lo & ~(std::uint64_t{0x3} << 62)) | (std::uint64_t{0x2} << 62);
    // 128-bit unsigned -> decimal by repeated division of four 32-bit limbs by 10.
    std::uint32_t limb[4] = {static_cast<std::uint32_t>(hi >> 32), static_cast<std::uint32_t>(hi),
                             static_cast<std::uint32_t>(lo >> 32), static_cast<std::uint32_t>(lo)};
    char digs[48];
    std::size_t n = 0;
    while (limb[0] | limb[1] | limb[2] | limb[3]) {
        std::uint64_t rem = 0;
        for (auto& l : limb) {
            const std::uint64_t cur = (rem << 32) | l;
            l = static_cast<std::uint32_t>(cur / 10u);
            rem = cur % 10u;
        }
        digs[n++] = static_cast<char>('0' + rem);
    }
    if (n == 0) digs[n++] = '0';
    std::string out = "2.25.";
    for (std::size_t i = n; i-- > 0;) out.push_back(digs[i]);
    return out;
}

bool uid_valid(std::string_view uid) noexcept {
    if (uid.empty() || uid.size() > 64) return false;
    std::size_t comp_start = 0;
    for (std::size_t i = 0; i <= uid.size(); ++i) {
        if (i == uid.size() || uid[i] == '.') {
            const std::size_t len = i - comp_start;
            if (len == 0) return false;
            if (len > 1 && uid[comp_start] == '0') return false;
            comp_start = i + 1;
        } else if (!is_digit(uid[i])) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- CIELab
namespace {
struct Mat3 {
    double m[3][3];
};
Mat3 mul(const Mat3& a, const Mat3& b) noexcept {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
Mat3 inverse(const Mat3& a) noexcept {
    const auto& m = a.m;
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Mat3 r{};
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return r;
}
std::array<double, 3> apply(const Mat3& a, const std::array<double, 3>& v) noexcept {
    return {a.m[0][0] * v[0] + a.m[0][1] * v[1] + a.m[0][2] * v[2],
            a.m[1][0] * v[0] + a.m[1][1] * v[1] + a.m[1][2] * v[2],
            a.m[2][0] * v[0] + a.m[2][1] * v[1] + a.m[2][2] * v[2]};
}

// CIE illuminant white points (Y = 1): D50 (PS3.3 C.10.7.1.1 reference) and D65 (sRGB).
constexpr std::array<double, 3> kD50{0.96422, 1.0, 0.82521};
constexpr std::array<double, 3> kD65{0.95047, 1.0, 1.08883};
// Bradford cone response matrix.
constexpr Mat3 kBradford{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
// IEC 61966-2-1 linear sRGB from XYZ (D65).
constexpr Mat3 kXyzToSrgb{{{3.2406, -1.5372, -0.4986}, {-0.9689, 1.8758, 0.0415}, {0.0557, -0.2040, 1.0570}}};

struct Transforms {
    Mat3 d50_to_srgb;
    Mat3 srgb_to_d50;
};
const Transforms& transforms() noexcept {
    static const Transforms t = [] {
        const auto s = apply(kBradford, kD50);
        const auto d = apply(kBradford, kD65);
        Mat3 scale{{{d[0] / s[0], 0, 0}, {0, d[1] / s[1], 0}, {0, 0, d[2] / s[2]}}};
        const Mat3 adapt = mul(inverse(kBradford), mul(scale, kBradford));  // XYZ D50 -> XYZ D65
        const Mat3 fwd = mul(kXyzToSrgb, adapt);
        return Transforms{fwd, inverse(fwd)};
    }();
    return t;
}

double lab_finv(double t) noexcept {
    constexpr double delta = 6.0 / 29.0;
    return t > delta ? t * t * t : 3.0 * delta * delta * (t - 4.0 / 29.0);
}
double lab_f(double t) noexcept {
    constexpr double delta = 6.0 / 29.0;
    return t > delta * delta * delta ? std::cbrt(t) : t / (3.0 * delta * delta) + 4.0 / 29.0;
}
double gamma_encode(double v) noexcept {
    v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}
double gamma_decode(double v) noexcept { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
}  // namespace

std::array<double, 3> cielab_to_rgb(std::uint16_t l, std::uint16_t a, std::uint16_t b) noexcept {
    const double L = l * 100.0 / 65535.0;
    const double A = a * 255.0 / 65535.0 - 128.0;
    const double B = b * 255.0 / 65535.0 - 128.0;
    const double fy = (L + 16.0) / 116.0;
    const std::array<double, 3> xyz{kD50[0] * lab_finv(fy + A / 500.0), kD50[1] * lab_finv(fy),
                                    kD50[2] * lab_finv(fy - B / 200.0)};
    const auto lin = apply(transforms().d50_to_srgb, xyz);
    return {gamma_encode(lin[0]), gamma_encode(lin[1]), gamma_encode(lin[2])};
}

std::array<std::uint8_t, 3> cielab_to_rgb8(std::uint16_t l, std::uint16_t a, std::uint16_t b) noexcept {
    const auto c = cielab_to_rgb(l, a, b);
    std::array<std::uint8_t, 3> out{};
    for (int i = 0; i < 3; ++i) out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(c[static_cast<std::size_t>(i)] * 255.0));
    return out;
}

std::array<std::uint16_t, 3> rgb8_to_cielab(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    const std::array<double, 3> lin{gamma_decode(r / 255.0), gamma_decode(g / 255.0), gamma_decode(b / 255.0)};
    const auto xyz = apply(transforms().srgb_to_d50, lin);
    const double fx = lab_f(xyz[0] / kD50[0]), fy = lab_f(xyz[1] / kD50[1]), fz = lab_f(xyz[2] / kD50[2]);
    double L = 116.0 * fy - 16.0, A = 500.0 * (fx - fy), B = 200.0 * (fy - fz);
    auto enc = [](double v, double lo, double range) {
        double e = (v - lo) * 65535.0 / range;
        e = e < 0.0 ? 0.0 : (e > 65535.0 ? 65535.0 : e);
        return static_cast<std::uint16_t>(std::lround(e));
    };
    return {enc(L, 0.0, 100.0), enc(A, -128.0, 255.0), enc(B, -128.0, 255.0)};
}

}  // namespace siaara::clinical
