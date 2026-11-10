#include "repair/report.hpp"

#include <charconv>
#include <cstring>
#include <system_error>

#include "deid/deid.hpp"
#include "dict/dictionary.hpp"

namespace siaara::repair {

std::string_view severity_name(Severity s) noexcept {
    switch (s) {
        case Severity::Info: return "Info";
        case Severity::Warning: return "Warning";
        case Severity::Error: return "Error";
    }
    return "Error";
}

namespace {
struct CategoryName {
    std::uint32_t bit;
    std::string_view name;
};
constexpr CategoryName kCategoryNames[] = {
    {kStructural, "structural"}, {kValues, "values"},     {kUids, "uids"},
    {kCharset, "charset"},       {kAttributes, "attributes"}, {kPixels, "pixels"},
    {kRescue, "rescue"},         {kDecompress, "decompress"}, {kStudy, "study"},
};

char hex_digit(unsigned v) { return "0123456789ABCDEF"[v & 15u]; }
}  // namespace

std::string_view category_name(std::uint32_t bit) noexcept {
    for (const auto& c : kCategoryNames)
        if (c.bit == bit) return c.name;
    return "";
}

bool parse_categories(std::string_view list, std::uint32_t& out) {
    out = 0;
    while (!list.empty()) {
        const auto comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item.empty()) continue;
        if (item == "all") {
            out |= kAllCategories & ~static_cast<std::uint32_t>(kDecompress);
            continue;
        }
        bool found = false;
        for (const auto& c : kCategoryNames)
            if (c.name == item) {
                out |= c.bit;
                found = true;
            }
        if (!found) return false;
    }
    return true;
}

std::string tag_string(Tag t) {
    std::string s = "(gggg,eeee)";
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned shift = 12u - 4u * static_cast<unsigned>(i);
        s[1 + i] = hex_digit(static_cast<unsigned>(tag_group(t)) >> shift);
        s[6 + i] = hex_digit(static_cast<unsigned>(tag_element(t)) >> shift);
    }
    return s;
}

std::string path_string(const Path& p) {
    std::string s;
    for (const PathStep& st : p) {
        s += tag_string(st.tag);
        if (st.item >= 0) s += "[" + std::to_string(st.item) + "]";
    }
    return s;
}

std::size_t count(const std::vector<Finding>& f, Severity s) noexcept {
    std::size_t n = 0;
    for (const auto& x : f) n += x.severity == s ? 1u : 0u;
    return n;
}

bool value_is_loggable(const Path& path, Tag tag, VR vr) noexcept {
    auto listed = [](Tag t) {
        if (tag_is_private(t)) return true;
        return deid::find_row(t) != nullptr;
    };
    for (const PathStep& st : path)
        if (st.tag != tag && listed(st.tag)) return false;
    if (listed(tag)) return false;
    switch (vr) {
        case VR::AE: case VR::AS: case VR::AT: case VR::CS: case VR::DS: case VR::FD: case VR::FL:
        case VR::IS: case VR::LO: case VR::SH: case VR::SL: case VR::SS: case VR::SV: case VR::UI:
        case VR::UL: case VR::US: case VR::UV:
            return true;
        default:
            return false;
    }
}

std::string printable_text(std::string_view s) {
    std::string out;
    std::size_t shown = 0;
    for (char c : s) {
        if (shown >= 64) {
            out += "...";
            break;
        }
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F && c != '"') {
            out.push_back(c);
        } else {
            out += "\\x";
            out.push_back(hex_digit(u >> 4));
            out.push_back(hex_digit(u));
        }
        ++shown;
    }
    return out;
}

std::string printable_value(const Element& e) {
    if (e.is_sequence()) return std::to_string(e.items.size()) + " item(s)";
    if (e.is_encapsulated()) return std::to_string(e.fragments.size()) + " fragment(s)";
    if (vr_is_string(e.vr) || e.vr == VR::None) return printable_text(e.value.as_chars());
    const std::size_t w = vr_value_size(e.vr);
    if (w == 0 || e.value.size() % w != 0 || e.value.size() > 16 * w) {
        return std::to_string(e.value.size()) + " bytes";
    }
    std::string out;
    const std::uint8_t* p = e.value.data();
    for (std::size_t i = 0; i + w <= e.value.size(); i += w) {
        if (!out.empty()) out.push_back('\\');
        switch (e.vr) {
            case VR::US: out += std::to_string(load_le16(p + i)); break;
            case VR::SS: out += std::to_string(static_cast<std::int16_t>(load_le16(p + i))); break;
            case VR::UL: out += std::to_string(load_le32(p + i)); break;
            case VR::SL: out += std::to_string(static_cast<std::int32_t>(load_le32(p + i))); break;
            case VR::UV: out += std::to_string(load_le64(p + i)); break;
            case VR::SV: out += std::to_string(static_cast<std::int64_t>(load_le64(p + i))); break;
            case VR::AT:
                out += tag_string(make_tag(load_le16(p + i), load_le16(p + i + 2)));
                break;
            case VR::FL: {
                float f;
                std::uint32_t b = load_le32(p + i);
                std::memcpy(&f, &b, 4);
                char buf[48];
                auto r = std::to_chars(buf, buf + sizeof buf, f);
                out.append(buf, r.ec == std::errc() ? r.ptr : buf);
                break;
            }
            case VR::FD: {
                double d;
                std::uint64_t b = load_le64(p + i);
                std::memcpy(&d, &b, 8);
                char buf[48];
                auto r = std::to_chars(buf, buf + sizeof buf, d);
                out.append(buf, r.ec == std::errc() ? r.ptr : buf);
                break;
            }
            default: return std::to_string(e.value.size()) + " bytes";
        }
    }
    return out;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20 || u >= 0x7F) {
                    out += "\\u00";
                    out.push_back(hex_digit(u >> 4));
                    out.push_back(hex_digit(u));
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

namespace {

std::string keyword_of(const Path& p) {
    if (p.empty()) return {};
    const Tag t = p.back().tag;
    if (tag_is_private(t)) return "private";
    if (const dict::Entry* e = dict::lookup(t)) return std::string(e->keyword);
    return {};
}

void finding_json(std::string& o, const Finding& f) {
    o += "{\"severity\":\"";
    o += severity_name(f.severity);
    o += "\",\"code\":\"" + json_escape(f.code) + "\"";
    if (f.category) o += ",\"category\":\"" + std::string(category_name(f.category)) + "\"";
    if (!f.path.empty()) {
        o += ",\"path\":\"" + path_string(f.path) + "\"";
        const std::string kw = keyword_of(f.path);
        if (!kw.empty()) o += ",\"keyword\":\"" + json_escape(kw) + "\"";
    }
    if (!f.module.empty()) o += ",\"module\":\"" + json_escape(f.module) + "\"";
    if (f.has_offset) o += ",\"offset\":" + std::to_string(f.offset);
    o += ",\"message\":\"" + json_escape(f.message) + "\"}";
}

void findings_array(std::string& o, const std::vector<Finding>& f) {
    o += "[";
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (i) o += ",";
        finding_json(o, f[i]);
    }
    o += "]";
}

void counts_json(std::string& o, const std::vector<Finding>& f) {
    o += "{\"errors\":" + std::to_string(count(f, Severity::Error)) +
         ",\"warnings\":" + std::to_string(count(f, Severity::Warning)) +
         ",\"info\":" + std::to_string(count(f, Severity::Info)) + "}";
}

void u32_array(std::string& o, const std::vector<std::uint32_t>& v) {
    o += "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += ",";
        o += std::to_string(v[i]);
    }
    o += "]";
}

std::string finding_line(const Finding& f) {
    std::string s(severity_name(f.severity));
    s += " [" + f.code + "]";
    if (!f.path.empty()) {
        s += " " + path_string(f.path);
        const std::string kw = keyword_of(f.path);
        if (!kw.empty()) s += " " + kw;
    }
    if (!f.module.empty()) s += " (" + f.module + ")";
    s += ": " + f.message;
    if (f.has_offset) s += " [offset " + std::to_string(f.offset) + "]";
    return s;
}

}  // namespace

std::string findings_json(const std::vector<Finding>& f) {
    std::string o;
    findings_array(o, f);
    return o;
}

std::string to_json(const Report& r) {
    std::string o = "{\"contains_phi\":";
    o += r.detailed ? "true" : "false";
    o += ",\"files\":[";
    for (std::size_t i = 0; i < r.files.size(); ++i) {
        const FileReport& f = r.files[i];
        if (i) o += ",";
        o += "{\"input\":" + std::to_string(f.input_index);
        if (r.detailed) {
            o += ",\"input_path\":\"" + json_escape(f.input_path) + "\"";
            if (!f.output_path.empty()) o += ",\"output_path\":\"" + json_escape(f.output_path) + "\"";
        }
        o += ",\"dry_run\":";
        o += f.dry_run ? "true" : "false";
        o += ",\"written\":";
        o += f.written ? "true" : "false";
        o += ",\"input_unchanged\":";
        o += f.input_unchanged ? "true" : "false";
        if (!f.input_sha256.empty()) o += ",\"input_sha256\":\"" + f.input_sha256 + "\"";
        if (!f.output_sha256.empty()) o += ",\"output_sha256\":\"" + f.output_sha256 + "\"";
        if (!f.error.empty()) o += ",\"error\":\"" + json_escape(f.error) + "\"";
        o += ",\"summary\":";
        counts_json(o, f.findings);
        o += ",\"findings\":";
        findings_array(o, f.findings);
        o += ",\"changes\":[";
        for (std::size_t k = 0; k < f.changes.size(); ++k) {
            const Change& c = f.changes[k];
            if (k) o += ",";
            o += "{\"category\":\"" + std::string(category_name(c.category)) + "\",\"action\":\"" +
                 json_escape(c.action) + "\"";
            if (!c.path.empty()) {
                o += ",\"path\":\"" + path_string(c.path) + "\"";
                const std::string kw = keyword_of(c.path);
                if (!kw.empty()) o += ",\"keyword\":\"" + json_escape(kw) + "\"";
            }
            o += ",\"detail\":\"" + json_escape(c.detail) + "\"";
            if (c.values_withheld) {
                o += ",\"values\":\"withheld\"";
            } else {
                o += ",\"old\":\"" + json_escape(c.old_value) + "\",\"new\":\"" + json_escape(c.new_value) + "\"";
            }
            o += "}";
        }
        o += "]";
        if (f.rescue_attempted) {
            // "recovered" / "lost": 0-based frame indices; the counts are given too.
            o += ",\"frames\":{\"declared\":" + std::to_string(f.frames_declared) +
                 ",\"recovered_count\":" + std::to_string(f.frames_recovered.size()) +
                 ",\"lost_count\":" + std::to_string(f.frames_lost.size()) + ",\"recovered\":";
            u32_array(o, f.frames_recovered);
            o += ",\"lost\":";
            u32_array(o, f.frames_lost);
            o += "}";
        }
        if (f.after_computed) {
            o += ",\"summary_after\":";
            counts_json(o, f.findings_after);
            o += ",\"findings_after\":";
            findings_array(o, f.findings_after);
        }
        o += "}";
    }
    o += "],\"study_findings\":";
    findings_array(o, r.study_findings);
    o += "}";
    return o;
}

std::string to_text(const Report& r, Severity min_severity) {
    std::string o;
    if (r.detailed)
        o += "*** THIS REPORT CONTAINS PATIENT DATA (PHI): handle and store it accordingly ***\n";
    for (const FileReport& f : r.files) {
        o += "input " + std::to_string(f.input_index);
        if (r.detailed) o += " (" + f.input_path + ")";
        o += ": " + std::to_string(count(f.findings, Severity::Error)) + " error(s), " +
             std::to_string(count(f.findings, Severity::Warning)) + " warning(s), " +
             std::to_string(count(f.findings, Severity::Info)) + " info\n";
        if (!f.error.empty()) o += "  failed: " + f.error + "\n";
        for (const Finding& x : f.findings)
            if (x.severity >= min_severity) o += "  " + finding_line(x) + "\n";
        if (!f.changes.empty()) {
            o += f.dry_run ? "  planned changes (dry run, nothing written):\n" : "  changes:\n";
            for (const Change& c : f.changes) {
                o += "    [" + std::string(category_name(c.category)) + "] " + c.action;
                if (!c.path.empty()) {
                    o += " " + path_string(c.path);
                    const std::string kw = keyword_of(c.path);
                    if (!kw.empty()) o += " " + kw;
                }
                if (!c.detail.empty()) o += ": " + c.detail;
                if (c.values_withheld) o += " (values withheld)";
                else if (!c.old_value.empty() || !c.new_value.empty())
                    o += " \"" + c.old_value + "\" -> \"" + c.new_value + "\"";
                o += "\n";
            }
        }
        if (f.rescue_attempted) {
            // Counts first, then the frame numbers (1-based, as DICOM numbers frames) of the lost
            // ones: "lost: 0" (a 0-based index) read as "nothing lost" (testing 10b P3).
            o += "  frames: " + std::to_string(f.frames_recovered.size()) + " of " +
                 std::to_string(f.frames_declared) + " recovered, " + std::to_string(f.frames_lost.size()) + " lost";
            if (!f.frames_lost.empty()) {
                o += f.frames_lost.size() == 1 ? " (frame" : " (frames";
                std::size_t shown = 0;
                for (std::uint32_t k : f.frames_lost) {
                    if (++shown > 20) {
                        o += " ...";
                        break;
                    }
                    o += " " + std::to_string(std::uint64_t{k} + 1);
                }
                o += ")";
            }
            o += "\n";
        }
        if (f.after_computed) {
            o += f.written ? "  written" : "  not written (dry run)";
            if (r.detailed && f.written) o += " to " + f.output_path;
            o += "; input unchanged: ";
            o += f.input_unchanged ? "yes" : "NO";
            o += f.written ? "; after repair: " : "; output would have: " + std::to_string(count(f.findings_after, Severity::Error)) + " error(s), " +
                 std::to_string(count(f.findings_after, Severity::Warning)) + " warning(s)\n";
        }
    }
    if (!r.study_findings.empty()) {
        o += "across files:\n";
        for (const Finding& x : r.study_findings)
            if (x.severity >= min_severity) o += "  " + finding_line(x) + "\n";
    }
    return o;
}

}  // namespace siaara::repair
