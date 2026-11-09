// SIAARA DICOM Toolkit - DICOM JSON Model (PS3.18 Annex F), written from the standard.
#include "web/dicom_json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dict/dictionary.hpp"
#include "web/base64.hpp"

namespace siaara::web {

using detail::Charset;

namespace {

bool vr_is_bulk_binary(VR vr) noexcept {
    return vr == VR::OB || vr == VR::OD || vr == VR::OF || vr == VR::OL || vr == VR::OV || vr == VR::OW ||
           vr == VR::UN;
}
bool vr_is_number(VR vr) noexcept {
    return vr == VR::US || vr == VR::SS || vr == VR::UL || vr == VR::SL || vr == VR::SV || vr == VR::UV ||
           vr == VR::FL || vr == VR::FD;
}
bool vr_single_valued(VR vr) noexcept { return vr == VR::LT || vr == VR::ST || vr == VR::UT || vr == VR::UR; }

// ---------------------------------------------------------------- encoder
class Encoder {
public:
    Encoder(std::string& out, const ModelWriteOptions& opt) : out_(out), opt_(opt) {}

    Status dataset(const Dataset& ds, Charset inherited, std::uint32_t depth) {
        std::string csname;
        const Charset cs = detail::charset_of(ds, inherited, csname);
        out_.push_back('{');
        bool first = true;
        for (const Element& e : ds.elements()) {
            if (tag_element(e.tag) == 0 || tag_group(e.tag) == 0xFFFE) continue;  // group lengths, delimiters
            if (!first) out_.push_back(',');
            first = false;
            out_.push_back('"');
            out_.append(detail::tag_hex(e.tag));
            out_.append("\":");
            path_.push_back(e.tag);
            Status st = element(e, cs, depth);
            path_.pop_back();
            if (!st) return st;
        }
        out_.push_back('}');
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
        json::write_string(out_, u);
        return Status::ok();
    }

    Status element(const Element& e, Charset cs, std::uint32_t depth) {
        VR vr = e.vr == VR::None ? VR::UN : e.vr;
        out_.append("{\"vr\":\"");
        out_.append(vr_name(vr));
        out_.push_back('"');
        if (vr == VR::SQ) {
            if (!e.items.empty()) {
                if (depth + 1 > opt_.max_depth) return Status(Errc::LimitExceeded, "sequence nesting too deep", 0, e.tag);
                out_.append(",\"Value\":[");
                for (std::size_t i = 0; i < e.items.size(); ++i) {
                    if (i) out_.push_back(',');
                    path_.push_back(static_cast<std::uint32_t>(i));
                    Status st = dataset(e.items[i], cs, depth + 1);
                    path_.pop_back();
                    if (!st) return st;
                }
                out_.push_back(']');
            }
            out_.push_back('}');
            return Status::ok();
        }
        if (opt_.bulk_data_uri) {
            if (auto uri = opt_.bulk_data_uri(e, path_)) {
                out_.append(",\"BulkDataURI\":");
                json::write_string(out_, *uri);
                out_.push_back('}');
                return Status::ok();
            }
        }
        if (e.is_encapsulated()) {
            if (!opt_.inline_encapsulated)
                return Status(Errc::Unsupported, "encapsulated data needs a bulk data URI", 0, e.tag);
            std::vector<std::uint8_t> stream;
            if (Status st = detail::encapsulated_stream(e, stream); !st) return st;
            out_.append(",\"InlineBinary\":\"");
            if (Status st = base64_encode(stream, out_); !st) return st;
            out_.append("\"}");
            return Status::ok();
        }
        const std::uint8_t* p = e.value.data();
        const std::size_t n = e.value.size();
        if (vr_is_bulk_binary(vr) || !vr_is_known(vr)) {
            if (n) {
                out_.append(",\"InlineBinary\":\"");
                if (Status st = base64_encode(e.value.span(), out_); !st) return st;
                out_.push_back('"');
            }
            out_.push_back('}');
            return Status::ok();
        }
        if (vr_is_number(vr) || vr == VR::AT) {
            const std::size_t w = vr == VR::AT ? 4 : vr_value_size(vr);
            if (n % w) return Status(Errc::Malformed, "value length is not a multiple of the VR size", 0, e.tag);
            if (n) {
                out_.append(",\"Value\":[");
                for (std::size_t i = 0; i < n / w; ++i) {
                    if (i) out_.push_back(',');
                    const std::uint8_t* q = p + i * w;
                    if (vr == VR::AT) {
                        json::write_string(out_, detail::tag_hex(make_tag(load_le16(q), load_le16(q + 2))));
                    } else if (detail::number_needs_string(vr, q)) {
                        json::write_string(out_, detail::number_text(vr, q));
                    } else {
                        out_.append(detail::number_text(vr, q));
                    }
                }
                out_.push_back(']');
            }
            out_.push_back('}');
            return Status::ok();
        }
        // Character string VRs.
        const auto values = detail::split_values(e.value.as_chars(), vr);
        if (!values.empty()) {
            out_.append(",\"Value\":[");
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) out_.push_back(',');
                const auto v = values[i];
                if (v.empty()) {
                    out_.append("null");
                } else if (vr == VR::PN) {
                    if (Status st = person_name(v, cs, e.tag); !st) return st;
                } else if (vr == VR::DS || vr == VR::IS) {
                    if (!opt_.numbers_as_strings && detail::number_is_safe(v, vr)) out_.append(v);
                    else if (Status st = text(v, cs, e.tag); !st) return st;
                } else {
                    if (Status st = text(v, cs, e.tag); !st) return st;
                }
            }
            out_.push_back(']');
        }
        out_.push_back('}');
        return Status::ok();
    }

    Status person_name(std::string_view v, Charset cs, Tag tag) {
        static constexpr const char* kGroups[3] = {"Alphabetic", "Ideographic", "Phonetic"};
        std::string_view groups[3];
        std::size_t start = 0;
        for (int g = 0; g < 3; ++g) {
            const auto pos = g < 2 ? v.find('=', start) : std::string_view::npos;
            groups[g] = v.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
            if (pos == std::string_view::npos) break;
            start = pos + 1;
        }
        bool any = false;
        for (auto& gv : groups) {
            while (!gv.empty() && gv.back() == ' ') gv.remove_suffix(1);
            if (!gv.empty()) any = true;
        }
        if (!any) {
            out_.append("null");
            return Status::ok();
        }
        out_.push_back('{');
        bool first = true;
        for (int g = 0; g < 3; ++g) {
            if (groups[g].empty()) continue;
            if (!first) out_.push_back(',');
            first = false;
            out_.push_back('"');
            out_.append(kGroups[g]);
            out_.append("\":");
            if (Status st = text(groups[g], cs, tag); !st) return st;
        }
        out_.push_back('}');
        return Status::ok();
    }
};

// ---------------------------------------------------------------- decoder
class Decoder {
public:
    explicit Decoder(const ModelReadOptions& opt) : opt_(opt) {}
    bool non_ascii = false;

    Status dataset(const json::Value& obj, Dataset& ds, std::uint32_t depth) {
        if (!obj.is_object()) return Status(Errc::Malformed, "DICOM JSON: data set is not an object");
        for (const auto& [key, attr] : obj.object) {
            const auto tag = detail::parse_tag_hex(key);
            if (!tag) return Status(Errc::Malformed, "DICOM JSON: attribute name is not 8 hex digits");
            if (++count_ > opt_.max_elements) return Status(Errc::LimitExceeded, "DICOM JSON: too many elements");
            if (tag_group(*tag) == 0xFFFE) return Status(Errc::Malformed, "DICOM JSON: item tags are not attributes", 0, *tag);
            if (ds.contains(*tag)) return Status(Errc::Malformed, "DICOM JSON: duplicate attribute", 0, *tag);
            Element e;
            e.tag = *tag;
            path_.push_back(*tag);
            Status st = element(attr, e, depth);
            path_.pop_back();
            if (!st) return st;
            ds.append_sorted(std::move(e));
        }
        return Status::ok();
    }

private:
    const ModelReadOptions& opt_;
    ElementPath path_;
    std::uint64_t count_ = 0;

    static Status bad(const char* m, Tag t) { return Status(Errc::Malformed, std::string("DICOM JSON: ") + m, 0, t); }

    Status check_size(std::uint64_t n, Tag t) const {
        if (n > opt_.max_value_bytes) return Status(Errc::LimitExceeded, "DICOM JSON: value too large", 0, t);
        return Status::ok();
    }

    void note_text(std::string_view s) {
        if (!non_ascii && !detail::is_ascii(s)) non_ascii = true;
    }

    Status element(const json::Value& a, Element& e, std::uint32_t depth) {
        if (!a.is_object()) return bad("attribute is not an object", e.tag);
        const json::Value* vrv = a.get("vr");
        if (vrv) {
            if (!vrv->is_string() || vrv->text.size() != 2) return bad("invalid vr", e.tag);
            e.vr = vr_from_chars(vrv->text[0], vrv->text[1]);
            if (!vr_is_known(e.vr)) return bad("unknown vr", e.tag);
        } else {
            e.vr = dict::implicit_vr(e.tag);
            if (e.vr == VR::None) e.vr = VR::UN;
        }
        const json::Value* val = a.get("Value");
        const json::Value* ib = a.get("InlineBinary");
        const json::Value* bd = a.get("BulkDataURI");
        if ((val ? 1 : 0) + (ib ? 1 : 0) + (bd ? 1 : 0) > 1)
            return bad("more than one of Value, InlineBinary and BulkDataURI", e.tag);
        if (e.vr == VR::SQ) {
            if (ib || bd) return bad("sequence with binary data", e.tag);
            if (!val || val->is_null()) return Status::ok();
            if (!val->is_array()) return bad("Value is not an array", e.tag);
            if (!val->array.empty() && depth + 1 > opt_.max_depth)
                return Status(Errc::LimitExceeded, "DICOM JSON: sequence nesting too deep", 0, e.tag);
            e.items.reserve(val->array.size());
            for (std::size_t i = 0; i < val->array.size(); ++i) {
                e.items.emplace_back();
                path_.push_back(static_cast<std::uint32_t>(i));
                Status st = val->array[i].is_null() ? bad("null sequence item", e.tag)
                                                    : dataset(val->array[i], e.items.back(), depth + 1);
                path_.pop_back();
                if (!st) return st;
            }
            return Status::ok();
        }
        if (ib) {
            if (!ib->is_string()) return bad("InlineBinary is not a string", e.tag);
            std::vector<std::uint8_t> bytes;
            if (Status st = base64_decode(ib->text, bytes, opt_.max_value_bytes); !st)
                return Status(st.code, "DICOM JSON: " + st.message, 0, e.tag);
            if (opt_.detect_encapsulated && e.tag == tags::PixelData && (e.vr == VR::OB || e.vr == VR::OW)) {
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
        if (bd) {
            if (!bd->is_string()) return bad("BulkDataURI is not a string", e.tag);
            if (opt_.resolve_bulk_data) {
                auto r = opt_.resolve_bulk_data(bd->text, e.tag, e.vr);
                if (r) {
                    if (Status st = check_size(r->size(), e.tag); !st) return st;
                    e.value = std::move(r).value();
                    return Status::ok();
                }
                if (r.error().code != Errc::NotFound) return r.error();
            }
            if (opt_.unresolved) opt_.unresolved->push_back(BulkReference{path_, e.vr, bd->text});
            return Status::ok();
        }
        if (!val || val->is_null()) return Status::ok();
        if (!val->is_array()) return bad("Value is not an array", e.tag);
        if (val->array.empty()) return Status::ok();
        const VR vr = e.vr;
        if (vr_is_bulk_binary(vr)) return bad("binary VR with a Value array", e.tag);
        if (vr_is_number(vr) || vr == VR::AT) {
            std::vector<std::uint8_t> bytes;
            for (const auto& v : val->array) {
                if (vr == VR::AT) {
                    if (!v.is_string()) return bad("AT value is not a string", e.tag);
                    const auto t = detail::parse_tag_hex(v.text);
                    if (!t) return bad("AT value is not 8 hex digits", e.tag);
                    std::uint8_t b[4];
                    store_le16(b, tag_group(*t));
                    store_le16(b + 2, tag_element(*t));
                    bytes.insert(bytes.end(), b, b + 4);
                } else {
                    if (!v.is_number() && !v.is_string()) return bad("numeric value is not a number", e.tag);
                    if (Status st = detail::encode_number(vr, v.text, v.is_string(), bytes); !st)
                        return Status(st.code, "DICOM JSON: " + st.message, 0, e.tag);
                }
                if (Status st = check_size(bytes.size(), e.tag); !st) return st;
            }
            e.value = detail::to_bytes(std::move(bytes));
            return Status::ok();
        }
        if (vr_single_valued(vr) && val->array.size() > 1) return bad("multiple values for a single-valued VR", e.tag);
        std::string joined;
        for (std::size_t i = 0; i < val->array.size(); ++i) {
            const auto& v = val->array[i];
            if (i) joined.push_back('\\');
            if (v.is_null()) continue;
            if (vr == VR::PN) {
                if (Status st = person_name(v, joined, e.tag); !st) return st;
            } else if (v.is_string() || (v.is_number() && (vr == VR::DS || vr == VR::IS))) {
                if (!vr_single_valued(vr) && v.text.find('\\') != std::string::npos)
                    return bad("backslash inside a value", e.tag);
                note_text(v.text);
                joined.append(v.text);
            } else {
                return bad("value has the wrong JSON type for its VR", e.tag);
            }
            if (Status st = check_size(joined.size(), e.tag); !st) return st;
        }
        e.value = detail::to_bytes(std::move(joined));
        return Status::ok();
    }

    Status person_name(const json::Value& v, std::string& out, Tag tag) {
        if (!v.is_object()) return bad("PN value is not an object", tag);
        static constexpr const char* kGroups[3] = {"Alphabetic", "Ideographic", "Phonetic"};
        std::string_view g[3];
        for (int i = 0; i < 3; ++i) {
            const json::Value* c = v.get(kGroups[i]);
            if (!c || c->is_null()) continue;
            if (!c->is_string()) return bad("PN component group is not a string", tag);
            if (c->text.find('=') != std::string::npos || c->text.find('\\') != std::string::npos)
                return bad("PN component group contains a delimiter", tag);
            g[i] = c->text;
        }
        int last = -1;
        for (int i = 0; i < 3; ++i)
            if (!g[i].empty()) last = i;
        for (int i = 0; i <= last; ++i) {
            if (i) out.push_back('=');
            note_text(g[i]);
            out.append(g[i]);
        }
        return Status::ok();
    }
};

}  // namespace

Status dataset_to_json(const Dataset& ds, std::string& out, const ModelWriteOptions& opt) {
    Encoder enc(out, opt);
    return enc.dataset(ds, Charset::Utf8, 0);
}

Status datasets_to_json(const std::vector<const Dataset*>& sets, std::string& out, const ModelWriteOptions& opt) {
    out.push_back('[');
    for (std::size_t i = 0; i < sets.size(); ++i) {
        if (i) out.push_back(',');
        if (Status st = dataset_to_json(*sets[i], out, opt); !st) return st;
    }
    out.push_back(']');
    return Status::ok();
}

Status datasets_to_json(const std::vector<Dataset>& sets, std::string& out, const ModelWriteOptions& opt) {
    std::vector<const Dataset*> p;
    p.reserve(sets.size());
    for (const auto& d : sets) p.push_back(&d);
    return datasets_to_json(p, out, opt);
}

Result<Dataset> dataset_from_json_value(const json::Value& v, const ModelReadOptions& opt) {
    Decoder dec(opt);
    Dataset ds;
    if (Status st = dec.dataset(v, ds, 0); !st) return st;
    if (opt.set_utf8_charset) detail::apply_utf8_charset(ds, dec.non_ascii);
    return ds;
}

Result<Dataset> dataset_from_json(std::string_view text, const ModelReadOptions& opt) {
    auto v = json::parse(text, opt.json_limits);
    if (!v) return v.take_error();
    return dataset_from_json_value(*v, opt);
}

Result<std::vector<Dataset>> datasets_from_json(std::string_view text, const ModelReadOptions& opt) {
    auto v = json::parse(text, opt.json_limits);
    if (!v) return v.take_error();
    std::vector<Dataset> out;
    if (v->is_object()) {
        auto d = dataset_from_json_value(*v, opt);
        if (!d) return d.take_error();
        out.push_back(std::move(d).value());
        return out;
    }
    if (!v->is_array()) return Status(Errc::Malformed, "DICOM JSON: expected an array of data sets");
    out.reserve(v->array.size());
    for (const auto& item : v->array) {
        auto d = dataset_from_json_value(item, opt);
        if (!d) return d.take_error();
        out.push_back(std::move(d).value());
    }
    return out;
}

}  // namespace siaara::web
