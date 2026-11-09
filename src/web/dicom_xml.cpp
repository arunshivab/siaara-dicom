// SIAARA DICOM Toolkit - Native DICOM Model XML (PS3.19 Annex A), written from the standard.
#include "web/dicom_xml.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dict/dictionary.hpp"
#include "web/base64.hpp"
#include "web/xml.hpp"

namespace siaara::web {

using detail::Charset;

namespace {

constexpr std::string_view kNamespace = "http://dicom.nema.org/PS3.19/models/NativeDICOM";
constexpr const char* kPnGroups[3] = {"Alphabetic", "Ideographic", "Phonetic"};
constexpr const char* kPnComponents[5] = {"FamilyName", "GivenName", "MiddleName", "NamePrefix", "NameSuffix"};

bool vr_is_bulk_binary(VR vr) noexcept {
    return vr == VR::OB || vr == VR::OD || vr == VR::OF || vr == VR::OL || vr == VR::OV || vr == VR::OW ||
           vr == VR::UN;
}
bool vr_is_number(VR vr) noexcept {
    return vr == VR::US || vr == VR::SS || vr == VR::UL || vr == VR::SL || vr == VR::SV || vr == VR::UV ||
           vr == VR::FL || vr == VR::FD;
}
bool vr_single_valued(VR vr) noexcept { return vr == VR::LT || vr == VR::ST || vr == VR::UT || vr == VR::UR; }
bool is_private_data(Tag t) noexcept { return tag_is_private(t) && tag_element(t) >= 0x1000; }

std::string_view local_name(std::string_view n) {
    const auto c = n.find(':');
    return c == std::string_view::npos ? n : n.substr(c + 1);
}

// ---------------------------------------------------------------- encoder
class Encoder {
public:
    Encoder(std::string& out, const ModelWriteOptions& opt) : out_(out), opt_(opt) {}

    Status dataset(const Dataset& ds, Charset inherited, std::uint32_t depth) {
        std::string csname;
        const Charset cs = detail::charset_of(ds, inherited, csname);
        for (const Element& e : ds.elements()) {
            if (tag_element(e.tag) == 0 || tag_group(e.tag) == 0xFFFE) continue;
            path_.push_back(e.tag);
            Status st = element(ds, e, cs, depth);
            path_.pop_back();
            if (!st) return st;
        }
        return Status::ok();
    }

private:
    std::string& out_;
    const ModelWriteOptions& opt_;
    ElementPath path_;

    Status text(std::string_view v, Charset cs, Tag tag) {
        std::string u;
        if (!detail::to_utf8(v, cs, opt_.strict_charset, u))
            return Status(Errc::Unsupported, "text cannot be converted to UTF-8 from its character set", 0, tag);
        xml::append_escaped(out_, u, false);
        return Status::ok();
    }

    void value_open(std::size_t i) {
        out_.append("<Value number=\"");
        out_.append(std::to_string(i + 1));
        out_.append("\">");
    }

    Status element(const Dataset& ds, const Element& e, Charset cs, std::uint32_t depth) {
        const VR vr = e.vr == VR::None ? VR::UN : e.vr;
        out_.append("<DicomAttribute tag=\"");
        out_.append(detail::tag_hex(e.tag));
        out_.append("\" vr=\"");
        out_.append(vr_name(vr));
        out_.push_back('"');
        if (opt_.xml_keywords && !tag_is_private(e.tag)) {
            if (const auto* d = dict::lookup(e.tag); d && !d->keyword.empty()) {
                out_.append(" keyword=\"");
                xml::append_escaped(out_, d->keyword, true);
                out_.push_back('"');
            }
        }
        if (is_private_data(e.tag)) {
            const Tag creator = make_tag(tag_group(e.tag), static_cast<std::uint16_t>(tag_element(e.tag) >> 8));
            if (auto c = ds.get_raw_string(creator)) {
                std::string u;
                (void)detail::to_utf8(trim_value(*c, VR::LO), cs, false, u);
                out_.append(" privateCreator=\"");
                xml::append_escaped(out_, u, true);
                out_.push_back('"');
            }
        }
        out_.push_back('>');
        Status st = body(e, vr, cs, depth);
        if (!st) return st;
        out_.append("</DicomAttribute>");
        return Status::ok();
    }

    Status body(const Element& e, VR vr, Charset cs, std::uint32_t depth) {
        if (vr == VR::SQ) {
            if (!e.items.empty() && depth + 1 > opt_.max_depth)
                return Status(Errc::LimitExceeded, "sequence nesting too deep", 0, e.tag);
            for (std::size_t i = 0; i < e.items.size(); ++i) {
                out_.append("<Item number=\"");
                out_.append(std::to_string(i + 1));
                out_.append("\">");
                path_.push_back(static_cast<std::uint32_t>(i));
                Status st = dataset(e.items[i], cs, depth + 1);
                path_.pop_back();
                if (!st) return st;
                out_.append("</Item>");
            }
            return Status::ok();
        }
        if (opt_.bulk_data_uri) {
            if (auto uri = opt_.bulk_data_uri(e, path_)) {
                out_.append("<BulkData uri=\"");
                xml::append_escaped(out_, *uri, true);
                out_.append("\"/>");
                return Status::ok();
            }
        }
        if (e.is_encapsulated()) {
            if (!opt_.inline_encapsulated)
                return Status(Errc::Unsupported, "encapsulated data needs a bulk data URI", 0, e.tag);
            std::vector<std::uint8_t> stream;
            if (Status st = detail::encapsulated_stream(e, stream); !st) return st;
            out_.append("<InlineBinary>");
            if (Status st = base64_encode(stream, out_); !st) return st;
            out_.append("</InlineBinary>");
            return Status::ok();
        }
        const std::uint8_t* p = e.value.data();
        const std::size_t n = e.value.size();
        if (vr_is_bulk_binary(vr) || !vr_is_known(vr)) {
            if (n) {
                out_.append("<InlineBinary>");
                if (Status st = base64_encode(e.value.span(), out_); !st) return st;
                out_.append("</InlineBinary>");
            }
            return Status::ok();
        }
        if (vr_is_number(vr) || vr == VR::AT) {
            const std::size_t w = vr == VR::AT ? 4 : vr_value_size(vr);
            if (n % w) return Status(Errc::Malformed, "value length is not a multiple of the VR size", 0, e.tag);
            for (std::size_t i = 0; i < n / w; ++i) {
                const std::uint8_t* q = p + i * w;
                value_open(i);
                if (vr == VR::AT) out_.append(detail::tag_hex(make_tag(load_le16(q), load_le16(q + 2))));
                else out_.append(detail::number_text(vr, q));
                out_.append("</Value>");
            }
            return Status::ok();
        }
        const auto values = detail::split_values(e.value.as_chars(), vr);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto v = values[i];
            if (v.empty()) continue;  // empty values are omitted; numbering keeps their position
            if (vr == VR::PN) {
                if (Status st = person_name(v, i, cs, e.tag); !st) return st;
                continue;
            }
            value_open(i);
            if (Status st = text(v, cs, e.tag); !st) return st;
            out_.append("</Value>");
        }
        return Status::ok();
    }

    Status person_name(std::string_view v, std::size_t index, Charset cs, Tag tag) {
        std::string_view groups[3];
        std::size_t start = 0;
        for (int g = 0; g < 3; ++g) {
            const auto pos = g < 2 ? v.find('=', start) : std::string_view::npos;
            groups[g] = v.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
            if (pos == std::string_view::npos) break;
            start = pos + 1;
        }
        out_.append("<PersonName number=\"");
        out_.append(std::to_string(index + 1));
        out_.append("\">");
        for (int g = 0; g < 3; ++g) {
            auto gv = groups[g];
            while (!gv.empty() && gv.back() == ' ') gv.remove_suffix(1);
            if (gv.empty()) continue;
            out_.push_back('<');
            out_.append(kPnGroups[g]);
            out_.push_back('>');
            std::size_t s = 0;
            for (int c = 0; c < 5; ++c) {
                const auto pos = c < 4 ? gv.find('^', s) : std::string_view::npos;
                const auto comp = gv.substr(s, pos == std::string_view::npos ? std::string_view::npos : pos - s);
                if (!comp.empty()) {
                    out_.push_back('<');
                    out_.append(kPnComponents[c]);
                    out_.push_back('>');
                    if (Status st = text(comp, cs, tag); !st) return st;
                    out_.append("</");
                    out_.append(kPnComponents[c]);
                    out_.push_back('>');
                }
                if (pos == std::string_view::npos) break;
                s = pos + 1;
            }
            out_.append("</");
            out_.append(kPnGroups[g]);
            out_.push_back('>');
        }
        out_.append("</PersonName>");
        return Status::ok();
    }
};

// ---------------------------------------------------------------- decoder
class Decoder {
public:
    explicit Decoder(const ModelReadOptions& opt) : opt_(opt) {}
    bool non_ascii = false;

    Status dataset(const xml::Node& parent, Dataset& ds, std::uint32_t depth) {
        std::vector<std::pair<Tag, std::string>> creators;
        for (const auto& n : parent.children) {
            if (local_name(n.name) != "DicomAttribute") return bad("unexpected element", 0);
            const std::string* ts = n.attr("tag");
            if (!ts) return bad("DicomAttribute without tag", 0);
            const auto tag = detail::parse_tag_hex(*ts);
            if (!tag) return bad("tag is not 8 hex digits", 0);
            if (++count_ > opt_.max_elements) return Status(Errc::LimitExceeded, "DICOM XML: too many elements");
            if (tag_group(*tag) == 0xFFFE) return bad("item tags are not attributes", *tag);
            if (ds.contains(*tag)) return bad("duplicate attribute", *tag);
            if (const std::string* pc = n.attr("privateCreator"); pc && is_private_data(*tag))
                creators.emplace_back(make_tag(tag_group(*tag), static_cast<std::uint16_t>(tag_element(*tag) >> 8)), *pc);
            Element e;
            e.tag = *tag;
            path_.push_back(*tag);
            Status st = element(n, e, depth);
            path_.pop_back();
            if (!st) return st;
            ds.append_sorted(std::move(e));
        }
        for (auto& [ct, name] : creators) {
            if (ds.contains(ct)) continue;
            note_text(name);
            (void)ds.set_string(ct, VR::LO, name);
        }
        return Status::ok();
    }

private:
    const ModelReadOptions& opt_;
    ElementPath path_;
    std::uint64_t count_ = 0;

    static Status bad(const char* m, Tag t) { return Status(Errc::Malformed, std::string("DICOM XML: ") + m, 0, t); }
    void note_text(std::string_view s) {
        if (!non_ascii && !detail::is_ascii(s)) non_ascii = true;
    }
    Status check_size(std::uint64_t n, Tag t) const {
        if (n > opt_.max_value_bytes) return Status(Errc::LimitExceeded, "DICOM XML: value too large", 0, t);
        return Status::ok();
    }

    // Children with the given name, ordered by their 1-based number attribute.
    Status numbered(const xml::Node& n, std::string_view name, Tag tag, std::vector<std::pair<std::uint32_t, const xml::Node*>>& out) {
        for (const auto& c : n.children) {
            if (local_name(c.name) != name) continue;
            const std::string* num = c.attr("number");
            if (!num) return bad("missing number attribute", tag);
            auto v = detail::parse_uint(*num, 1'000'000'000);
            if (!v || *v == 0 || num->empty() || (*num)[0] < '0' || (*num)[0] > '9') return bad("invalid number attribute", tag);
            out.emplace_back(static_cast<std::uint32_t>(*v), &c);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 1; i < out.size(); ++i)
            if (out[i].first == out[i - 1].first) return bad("duplicate number attribute", tag);
        if (!out.empty() && out.back().first > out.size() * 2 + 1024) return bad("number attribute too large", tag);
        return Status::ok();
    }

    Status element(const xml::Node& n, Element& e, std::uint32_t depth) {
        if (const std::string* vrs = n.attr("vr")) {
            if (vrs->size() != 2) return bad("invalid vr", e.tag);
            e.vr = vr_from_chars((*vrs)[0], (*vrs)[1]);
            if (!vr_is_known(e.vr)) return bad("unknown vr", e.tag);
        } else {
            e.vr = dict::implicit_vr(e.tag);
            if (e.vr == VR::None) e.vr = VR::UN;
        }
        const VR vr = e.vr;
        int kinds = 0;
        const xml::Node* inline_bin = nullptr;
        const xml::Node* bulk = nullptr;
        bool has_values = false, has_pn = false, has_items = false;
        for (const auto& c : n.children) {
            const auto ln = local_name(c.name);
            if (ln == "InlineBinary") { if (inline_bin) return bad("repeated InlineBinary", e.tag); inline_bin = &c; }
            else if (ln == "BulkData") { if (bulk) return bad("repeated BulkData", e.tag); bulk = &c; }
            else if (ln == "Value") has_values = true;
            else if (ln == "PersonName") has_pn = true;
            else if (ln == "Item") has_items = true;
            else return bad("unexpected element in DicomAttribute", e.tag);
        }
        kinds = (inline_bin ? 1 : 0) + (bulk ? 1 : 0) + (has_values ? 1 : 0) + (has_pn ? 1 : 0) + (has_items ? 1 : 0);
        if (kinds > 1) return bad("mixed value kinds in DicomAttribute", e.tag);
        if (vr == VR::SQ) {
            if (kinds && !has_items) return bad("sequence without Item elements", e.tag);
            std::vector<std::pair<std::uint32_t, const xml::Node*>> items;
            if (Status st = numbered(n, "Item", e.tag, items); !st) return st;
            if (!items.empty() && depth + 1 > opt_.max_depth)
                return Status(Errc::LimitExceeded, "DICOM XML: sequence nesting too deep", 0, e.tag);
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].first != i + 1) return bad("Item numbers are not consecutive", e.tag);
                e.items.emplace_back();
                path_.push_back(static_cast<std::uint32_t>(i));
                Status st = dataset(*items[i].second, e.items.back(), depth + 1);
                path_.pop_back();
                if (!st) return st;
            }
            return Status::ok();
        }
        if (has_items) return bad("Item in a non-sequence attribute", e.tag);
        if (inline_bin) {
            std::vector<std::uint8_t> bytes;
            if (Status st = base64_decode(inline_bin->text, bytes, opt_.max_value_bytes); !st)
                return Status(st.code, "DICOM XML: " + st.message, 0, e.tag);
            if (opt_.detect_encapsulated && e.tag == tags::PixelData && (vr == VR::OB || vr == VR::OW)) {
                std::vector<Bytes> frags;
                if (detail::parse_encapsulated_stream(bytes, frags)) {
                    e.fragments = std::move(frags);
                    e.undefined_length = true;
                    e.vr = VR::OB;
                    return Status::ok();
                }
            }
            e.value = detail::to_bytes(std::move(bytes));
            return Status::ok();
        }
        if (bulk) {
            std::string uri;
            if (const std::string* u = bulk->attr("uri")) uri = *u;
            else if (const std::string* id = bulk->attr("uuid")) uri = "uuid:" + *id;
            else return bad("BulkData without uri or uuid", e.tag);
            if (opt_.resolve_bulk_data && bulk->attr("uri")) {
                auto r = opt_.resolve_bulk_data(uri, e.tag, vr);
                if (r) {
                    if (Status st = check_size(r->size(), e.tag); !st) return st;
                    e.value = std::move(r).value();
                    return Status::ok();
                }
                if (r.error().code != Errc::NotFound) return r.error();
            }
            if (opt_.unresolved) opt_.unresolved->push_back(BulkReference{path_, vr, uri});
            return Status::ok();
        }
        if (has_pn && vr != VR::PN) return bad("PersonName in a non-PN attribute", e.tag);
        if (has_values && vr == VR::PN) return bad("Value in a PN attribute", e.tag);
        std::vector<std::pair<std::uint32_t, const xml::Node*>> vals;
        if (Status st = numbered(n, vr == VR::PN ? "PersonName" : "Value", e.tag, vals); !st) return st;
        if (vals.empty()) return Status::ok();
        if (vr_is_bulk_binary(vr)) return bad("binary VR with Value elements", e.tag);
        if (vr_is_number(vr) || vr == VR::AT) {
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i < vals.size(); ++i) {
                if (vals[i].first != i + 1) return bad("numeric values must be numbered consecutively", e.tag);
                const std::string& t = vals[i].second->text;
                if (vr == VR::AT) {
                    std::string_view tv = t;
                    while (!tv.empty() && (tv.front() == ' ' || tv.front() == '\n')) tv.remove_prefix(1);
                    while (!tv.empty() && (tv.back() == ' ' || tv.back() == '\n')) tv.remove_suffix(1);
                    const auto tg = detail::parse_tag_hex(tv);
                    if (!tg) return bad("AT value is not 8 hex digits", e.tag);
                    std::uint8_t b[4];
                    store_le16(b, tag_group(*tg));
                    store_le16(b + 2, tag_element(*tg));
                    bytes.insert(bytes.end(), b, b + 4);
                } else if (Status st = detail::encode_number(vr, t, true, bytes); !st) {
                    return Status(st.code, "DICOM XML: " + st.message, 0, e.tag);
                }
                if (Status st = check_size(bytes.size(), e.tag); !st) return st;
            }
            e.value = detail::to_bytes(std::move(bytes));
            return Status::ok();
        }
        if (vr_single_valued(vr) && (vals.size() > 1 || vals[0].first != 1))
            return bad("multiple values for a single-valued VR", e.tag);
        std::string joined;
        std::uint32_t next = 1;
        for (const auto& [num, node] : vals) {
            for (; next < num; ++next) {
                if (next > 1) joined.push_back('\\');
            }
            if (num > 1) joined.push_back('\\');
            next = num + 1;
            if (vr == VR::PN) {
                if (Status st = person_name(*node, joined, e.tag); !st) return st;
            } else {
                if (!node->children.empty()) return bad("unexpected markup inside Value", e.tag);
                if (!vr_single_valued(vr) && node->text.find('\\') != std::string::npos)
                    return bad("backslash inside a value", e.tag);
                note_text(node->text);
                joined.append(node->text);
            }
            if (Status st = check_size(joined.size(), e.tag); !st) return st;
        }
        e.value = detail::to_bytes(std::move(joined));
        return Status::ok();
    }

    Status person_name(const xml::Node& pn, std::string& out, Tag tag) {
        std::string groups[3];
        for (const auto& g : pn.children) {
            const auto ln = local_name(g.name);
            int gi = -1;
            for (int i = 0; i < 3; ++i)
                if (ln == kPnGroups[i]) gi = i;
            if (gi < 0) return bad("unexpected element in PersonName", tag);
            if (!groups[gi].empty()) return bad("repeated PersonName component group", tag);
            std::string comps[5];
            for (const auto& c : g.children) {
                const auto cn = local_name(c.name);
                int ci = -1;
                for (int i = 0; i < 5; ++i)
                    if (cn == kPnComponents[i]) ci = i;
                if (ci < 0) return bad("unexpected PersonName component", tag);
                if (c.text.find_first_of("^=\\") != std::string::npos) return bad("delimiter inside a PN component", tag);
                comps[ci] = c.text;
            }
            int last = -1;
            for (int i = 0; i < 5; ++i)
                if (!comps[i].empty()) last = i;
            std::string s;
            for (int i = 0; i <= last; ++i) {
                if (i) s.push_back('^');
                s.append(comps[i]);
            }
            groups[gi] = std::move(s);
        }
        int last = -1;
        for (int i = 0; i < 3; ++i)
            if (!groups[i].empty()) last = i;
        for (int i = 0; i <= last; ++i) {
            if (i) out.push_back('=');
            note_text(groups[i]);
            out.append(groups[i]);
        }
        return Status::ok();
    }
};

}  // namespace

Status dataset_to_xml(const Dataset& ds, std::string& out, const ModelWriteOptions& opt) {
    out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<NativeDicomModel xmlns=\"");
    out.append(kNamespace);
    out.append("\" xml:space=\"preserve\">");
    Encoder enc(out, opt);
    if (Status st = enc.dataset(ds, Charset::Utf8, 0); !st) return st;
    out.append("</NativeDicomModel>\n");
    return Status::ok();
}

Result<Dataset> dataset_from_xml(std::string_view text, const ModelReadOptions& opt) {
    auto root = xml::parse(text, opt.xml_limits);
    if (!root) return root.take_error();
    if (local_name(root->name) != "NativeDicomModel")
        return Status(Errc::Malformed, "DICOM XML: root element is not NativeDicomModel");
    if (const std::string* ns = root->attr("xmlns"); ns && *ns != kNamespace)
        return Status(Errc::Malformed, "DICOM XML: unexpected namespace");
    Decoder dec(opt);
    Dataset ds;
    if (Status st = dec.dataset(*root, ds, 0); !st) return st;
    if (opt.set_utf8_charset) detail::apply_utf8_charset(ds, dec.non_ascii);
    return ds;
}

}  // namespace siaara::web
