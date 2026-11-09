#include "repair/validate.hpp"

#include <algorithm>
#include <optional>
#include <map>
#include <set>
#include <string>

#include "charset/charset.hpp"
#include "core/transfer_syntax.hpp"
#include "dict/dictionary.hpp"
#include "repair/iod.hpp"
#include "repair/pixels.hpp"
#include "repair/values.hpp"

namespace siaara::repair {

namespace {

class Sink {
public:
    Sink(std::vector<Finding>& out, std::size_t max) : out_(out), max_(max) {}
    void add(Severity s, std::string code, std::uint32_t cat, Path path, std::string msg, std::string module = {}) {
        if (out_.size() >= max_) return;
        std::string key = code + "|" + path_string(path);
        if (!seen_.insert(key).second) return;
        Finding f;
        f.severity = s;
        f.code = std::move(code);
        f.category = cat;
        f.path = std::move(path);
        f.message = std::move(msg);
        f.module = std::move(module);
        out_.push_back(std::move(f));
    }

private:
    std::vector<Finding>& out_;
    std::size_t max_;
    std::set<std::string> seen_;
};

Path child(const Path& p, Tag t) {
    Path q = p;
    q.push_back({t, -1});
    return q;
}

bool element_empty(const Element& e) {
    if (e.is_sequence()) return e.items.empty();
    if (e.is_encapsulated()) return false;
    if (vr_is_string(e.vr)) {
        const std::string_view s = trim_value(e.value.as_chars(), VR::LO);
        return s.empty();
    }
    return e.value.empty();
}

std::string_view trimmed_part(std::string_view part, VR vr) {
    // PS3.5 6.2: trailing spaces are padding for all string VRs; leading spaces are insignificant
    // for these VRs as well (dataset.cpp trim_value has the list).
    return trim_value(part, vr);
}

bool multi_valued(VR vr) {
    return vr != VR::LT && vr != VR::ST && vr != VR::UT && vr != VR::UR;
}

// ---------------------------------------------------------------- dictionary checks

struct DictChecker {
    Sink& sink;
    std::size_t non_ascii_default = 0;  // values with 8-bit bytes under the default repertoire

    void dataset(const Dataset& ds, const Path& path, const charset::CharacterSet* inherited, bool is_meta) {
        const charset::CharacterSet cs = charset::character_set_of(ds, inherited);
        if (const Element* scs = ds.find(tags::SpecificCharacterSet)) {
            const charset::CharacterSet parsed = charset::CharacterSet::parse(scs->value.as_chars());
            if (parsed.unknown)
                sink.add(Severity::Error, "charset-unknown", kCharset, child(path, tags::SpecificCharacterSet),
                         "Specific Character Set has " + std::to_string(parsed.unknown) + " value(s) that are not defined terms");
        }
        for (const Element& e : ds.elements()) element(ds, e, path, cs, is_meta);
    }

    void element(const Dataset& ds, const Element& e, const Path& path, const charset::CharacterSet& cs, bool is_meta) {
        const Path here = child(path, e.tag);
        const std::uint16_t g = tag_group(e.tag);
        if (!is_meta && g == 0x0002)
            sink.add(Severity::Error, "meta-in-dataset", kStructural, here, "File Meta Information element inside the data set");
        if (g == 0x0000)
            sink.add(Severity::Error, "command-in-dataset", kStructural, here, "command group element inside a stored data set");
        if (g == 0x0001 || g == 0x0003 || g == 0x0005 || g == 0x0007 || g == 0xFFFF)
            sink.add(Severity::Error, "illegal-group", kStructural, here, "odd group that may not be used (PS3.5 7.8.1)");
        if (tag_element(e.tag) == 0x0000) return;  // group lengths are checked by the scanner

        VR want = VR::None, want_alt = VR::None;
        std::string_view vm;
        if (tag_is_private(e.tag)) {
            const std::uint16_t el = tag_element(e.tag);
            if (el >= 0x0010 && el <= 0x00FF) {
                want = VR::LO;
                vm = "1";
            } else if (el >= 0x1000) {
                const Tag creator = make_tag(g, static_cast<std::uint16_t>(el >> 8));
                if (!ds.contains(creator))
                    sink.add(Severity::Warning, "private-no-creator", 0, here,
                             "private element without its Private Creator (PS3.5 7.8.1)");
            } else {
                sink.add(Severity::Error, "private-reserved", 0, here,
                         "private element in the reserved range (gggg,0001-000F) or (gggg,0100-0FFF)");
            }
        } else {
            const dict::Entry* d = dict::lookup(e.tag);
            if (!d) {
                sink.add(Severity::Warning, "unknown-tag", 0, here, "public tag is not in the data dictionary (PS3.6)");
            } else {
                want = d->vr;
                want_alt = d->vr_alt;
                vm = d->vm;
                if (d->retired) sink.add(Severity::Warning, "retired", 0, here, "retired attribute");
            }
        }
        // VR
        VR vr = e.vr;
        if (want != VR::None && vr != want && vr != want_alt) {
            const bool ox = (want == VR::OB || want == VR::OW) && (vr == VR::OB || vr == VR::OW);
            if (vr == VR::UN) {
                sink.add(Severity::Warning, "vr-un", kValues, here,
                         "VR is UN; the dictionary VR is " + std::string(vr_name(want)));
            } else if (!ox) {
                sink.add(Severity::Error, "vr-mismatch", kValues, here,
                         "VR is " + std::string(vr_name(vr)) + "; the dictionary VR is " + std::string(vr_name(want)) +
                             (want_alt != VR::None ? " or " + std::string(vr_name(want_alt)) : std::string()));
            }
        }
        if (e.is_sequence()) {
            for (std::size_t i = 0; i < e.items.size(); ++i) {
                Path ip = here;
                ip.back().item = static_cast<std::int32_t>(i);
                dataset(e.items[i], ip, &cs, false);
            }
            if (!vm.empty() && vm != "1" && !vm_allows(vm, e.items.size()) && !e.items.empty())
                sink.add(Severity::Error, "vm", 0, here, "sequence item count not allowed by the dictionary VM " + std::string(vm));
            return;
        }
        if (e.is_encapsulated()) return;
        if (vr == VR::UN || vr == VR::OB || vr == VR::OW || vr == VR::OD || vr == VR::OF || vr == VR::OL ||
            vr == VR::OV || vr == VR::None)
            return;
        const std::size_t w = vr_value_size(vr);
        if (w) {
            if (e.value.size() % w != 0) {
                sink.add(Severity::Error, "length", kValues, here,
                         "value length " + std::to_string(e.value.size()) + " is not a multiple of " + std::to_string(w) +
                             " for VR " + std::string(vr_name(vr)));
                return;
            }
            if (vr == VR::AT && e.value.size() % 4 != 0) return;
            const std::size_t n = vr == VR::AT ? e.value.size() / 4 : e.value.size() / w;
            if (n && !vm_allows(vm, n))
                sink.add(Severity::Error, "vm", 0, here,
                         "value multiplicity " + std::to_string(n) + " not allowed (dictionary VM " + std::string(vm) + ")");
            return;
        }
        if (!vr_is_string(vr)) return;
        std::string_view raw = e.value.as_chars();
        if (vr != VR::UI && !raw.empty() && raw.back() == '\0')
            sink.add(Severity::Error, "nul-padding", kValues, here,
                     "string value padded with NUL instead of a space (PS3.5 6.2: only UI uses NUL)");
        if (vr == VR::UI && !raw.empty() && raw.back() == ' ')
            sink.add(Severity::Error, "ui-padding", kValues, here, "UID value padded with a space instead of NUL (PS3.5 9.1)");
        std::string text;
        std::size_t repl = 0;
        const bool decoded = charset::vr_uses_character_set(vr);
        if (decoded) {
            const bool has8 = std::any_of(raw.begin(), raw.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80 || c == 0x1B; });
            if (has8 && cs.scheme == charset::Scheme::Iso2022 && cs.terms.size() == 1 && cs.terms[0] == charset::Term::IR6) {
                ++non_ascii_default;
                sink.add(Severity::Error, "charset-undeclared", kCharset, here,
                         "text has 8-bit or escape bytes but no Specific Character Set declares them");
            }
            text = charset::decode(raw, cs, charset::text_kind(vr), &repl);
            if (repl && !(cs.terms.size() == 1 && cs.terms[0] == charset::Term::IR6))
                sink.add(Severity::Error, "charset-undecodable", kCharset, here,
                         std::to_string(repl) + " byte sequence(s) cannot be decoded in the declared Specific Character Set");
        } else {
            text.assign(raw.data(), raw.size());
        }
        // Values
        std::string_view all = text;
        while (!all.empty() && (all.back() == ' ' || all.back() == '\0')) all.remove_suffix(1);
        if (all.empty()) return;
        const bool mv = multi_valued(vr);
        const std::size_t n = mv ? split_count(all) : 1;
        if (mv && !vm.empty() && !vm_allows(vm, n))
            sink.add(Severity::Error, "vm", 0, here,
                     "value multiplicity " + std::to_string(n) + " not allowed (dictionary VM " + std::string(vm) + ")");
        for (std::size_t i = 0; i < n; ++i) {
            std::string_view part = mv ? *split_value(all, i) : all;
            if (vr == VR::UI) {
                while (!part.empty() && part.back() == '\0') part.remove_suffix(1);
            }
            const std::string_view t = (vr == VR::LT || vr == VR::ST || vr == VR::UT) ? part : trimmed_part(part, vr);
            ValueCheck c = check_value(vr, t);
            if (decoded && repl == 0 && c == ValueCheck::BadCharacter) {
                // Decoded text may hold any character set character.
                c = ValueCheck::Ok;
                for (char ch : t) {
                    const auto u = static_cast<unsigned char>(ch);
                    if (u < 0x20 && !(vr == VR::ST || vr == VR::LT || vr == VR::UT) && u != 0x1B) c = ValueCheck::BadCharacter;
                }
            }
            if (c == ValueCheck::Ok && vr == VR::UI && !t.empty() &&
                std::all_of(t.begin(), t.end(), [](char ch) { return ch == '0' || ch == '.'; }))
                sink.add(Severity::Warning, "uid-zero", kUids, here,
                         "value " + std::to_string(i + 1) + ": UID has only zero components (no registered root)");
            if (c == ValueCheck::Ok) continue;
            const bool can_fix = normalise_value(vr, std::string(part)).has_value();
            std::uint32_t cat = can_fix ? std::uint32_t{kValues} : 0u;
            if (vr == VR::UI && !can_fix) cat = kUids;
            std::string msg = "value " + std::to_string(i + 1) + ": " + std::string(value_check_name(c)) + " (VR " +
                              std::string(vr_name(vr)) + ")";
            if (can_fix) msg += "; repair can normalise it unambiguously";
            else if (vr == VR::UI) msg += "; repair can replace it with a new UID (uids)";
            else msg += "; the intended value is not certain, so repair leaves it";
            sink.add(Severity::Error, c == ValueCheck::TooLong ? "value-too-long" : "value-invalid", cat, here, msg);
        }
    }
};

// ---------------------------------------------------------------- file meta

void check_meta(const ScanResult& sr, Sink& sink) {
    const DicomFile& f = sr.file;
    if (!sr.meta_present) return;
    const Dataset& m = f.meta;
    const Path root;
    auto need = [&](Tag t, const char* what) {
        if (!m.contains(t))
            sink.add(Severity::Error, "meta-missing-element", kStructural, child(root, t),
                     std::string(what) + " is missing from the File Meta Information (PS3.10 7.1)");
    };
    need(tags::FileMetaInformationVersion, "File Meta Information Version");
    need(tags::MediaStorageSOPClassUID, "Media Storage SOP Class UID");
    need(tags::MediaStorageSOPInstanceUID, "Media Storage SOP Instance UID");
    need(tags::TransferSyntaxUID, "Transfer Syntax UID");
    need(tags::ImplementationClassUID, "Implementation Class UID");
    if (const Element* v = m.find(tags::FileMetaInformationVersion)) {
        if (v->value.size() != 2 || v->value.data()[0] != 0x00 || v->value.data()[1] != 0x01)
            sink.add(Severity::Error, "meta-version", kStructural, child(root, tags::FileMetaInformationVersion),
                     "File Meta Information Version is not 00H 01H");
    }
    auto cmp = [&](Tag mt, Tag dt, const char* what) {
        auto a = m.get_string(mt);
        auto b = f.dataset.get_string(dt);
        if (a && b && *a != *b)
            sink.add(Severity::Error, "meta-mismatch", kStructural, child(root, mt),
                     std::string(what) + " does not match the data set");
    };
    cmp(tags::MediaStorageSOPClassUID, tags::SOPClassUID, "Media Storage SOP Class UID");
    cmp(tags::MediaStorageSOPInstanceUID, tags::SOPInstanceUID, "Media Storage SOP Instance UID");
    for (const Element& e : m.elements())
        if (tag_group(e.tag) != 0x0002)
            sink.add(Severity::Error, "meta-group", kStructural, child(root, e.tag), "non-0002 element in the File Meta Information");
}

// ---------------------------------------------------------------- IOD requirements

struct IodChecker {
    Sink& sink;
    const Dataset& top;
    std::string module_name;
    std::size_t not_evaluated = 0;
    const std::map<Tag, AttrType>* overrides = nullptr;  // top-level Type overrides from other modules

    const Element* find(const Dataset& ds, const ReqNode& n) const {
        if (n.mask == 0xFFFFFFFFu) return ds.find(n.tag);
        for (const Element& e : ds.elements())
            if (n.matches(e.tag)) return &e;
        return nullptr;
    }

    // Values of the condition's attribute: looked up in the item first, then the top level.
    const Element* cond_element(const Dataset& ds, const Condition& c) const {
        auto look = [&](const Dataset& d) -> const Element* {
            if (c.mask == 0xFFFFFFFFu) return d.find(c.tag);
            for (const Element& e : d.elements())
                if ((e.tag & c.mask) == c.tag) return &e;
            return nullptr;
        };
        if (const Element* e = look(ds)) return e;
        return &ds == &top ? nullptr : look(top);
    }

    static std::vector<std::string> values_of(const Element& e) {
        std::vector<std::string> out;
        if (e.is_sequence() || e.is_encapsulated()) return out;
        if (vr_is_string(e.vr)) {
            std::string_view s = trim_value(e.value.as_chars(), e.vr);
            if (s.empty()) return out;
            for (std::size_t i = 0, n = split_count(s); i < n; ++i) out.emplace_back(trim_value(*split_value(s, i), e.vr));
            return out;
        }
        const std::size_t w = vr_value_size(e.vr);
        if (!w) return out;
        for (std::size_t i = 0; i + w <= e.value.size(); i += w) {
            std::int64_t v = 0;
            switch (e.vr) {
                case VR::US: v = load_le16(e.value.data() + i); break;
                case VR::SS: v = static_cast<std::int16_t>(load_le16(e.value.data() + i)); break;
                case VR::UL: v = load_le32(e.value.data() + i); break;
                case VR::SL: v = static_cast<std::int32_t>(load_le32(e.value.data() + i)); break;
                default: return out;
            }
            out.push_back(std::to_string(v));
        }
        return out;
    }

    // true / false, or nullopt when not evaluable.
    std::optional<bool> evaluate(const Dataset& ds, const ReqNode& n) const {
        if (!n.condition || n.condition->op == CondOp::None) return std::nullopt;
        const Condition& c = *n.condition;
        const Element* e = cond_element(ds, c);
        switch (c.op) {
            case CondOp::Present: return e != nullptr;
            case CondOp::Absent: return e == nullptr;
            case CondOp::Equals:
            case CondOp::NotEquals: {
                if (!e) return false;
                const auto vals = values_of(*e);
                if (vals.empty()) return std::nullopt;
                bool any = false;
                for (const auto& v : vals)
                    for (std::string_view want : c.values)
                        if (v == want) any = true;
                return c.op == CondOp::Equals ? any : !any;
            }
            case CondOp::Greater: {
                if (!e || c.values.empty()) return std::nullopt;
                const auto vals = values_of(*e);
                if (vals.empty()) return std::nullopt;
                std::int64_t lim = 0;
                for (char ch : c.values[0]) lim = lim * 10 + (ch - '0');
                std::int64_t v = 0;
                bool neg = false;
                std::size_t k = 0;
                const std::string& s = vals[0];
                if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
                    neg = s[0] == '-';
                    k = 1;
                }
                for (; k < s.size(); ++k) {
                    if (s[k] < '0' || s[k] > '9') return std::nullopt;
                    v = v * 10 + (s[k] - '0');
                }
                return (neg ? -v : v) > lim;
            }
            case CondOp::None: break;
        }
        return std::nullopt;
    }

    static bool enum_match(std::string_view value, VR vr, std::string_view term) {
        if (vr_is_string(vr)) return value == term;
        // Binary: terms written as decimal ("1") or hex ("0001H").
        std::int64_t t = 0;
        if (!term.empty() && term.back() == 'H') {
            for (char c : term.substr(0, term.size() - 1)) {
                int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (d < 0) return true;  // not a numeric term: do not judge
                t = t * 16 + d;
            }
        } else {
            for (char c : term) {
                if (c < '0' || c > '9') return true;
                t = t * 10 + (c - '0');
            }
        }
        return value == std::to_string(t);
    }

    void check_list(const Dataset& ds, const std::vector<ReqNode>& nodes, const Path& path) {
        for (const ReqNode& base : nodes) {
            ReqNode overridden;
            const ReqNode* np = &base;
            if (path.empty() && overrides && !base.overrides) {
                auto it = overrides->find(base.tag);
                if (it != overrides->end()) {
                    overridden = base;
                    overridden.type = it->second;
                    np = &overridden;
                }
            }
            const ReqNode& n = *np;
            const Element* e = find(ds, n);
            const Path here = child(path, n.mask == 0xFFFFFFFFu ? n.tag : (e ? e->tag : n.tag));
            bool required = false;
            bool value_required = false;
            // Conditional includes: every guard must hold.
            bool guard_unknown = n.guard_unevaluated;
            bool guard_false = false;
            for (const Condition* g : n.guards) {
                ReqNode probe;
                probe.condition = g;
                const auto r = evaluate(ds, probe);
                if (!r.has_value()) guard_unknown = true;
                else if (!*r) guard_false = true;
            }
            if (guard_false) continue;  // the macro does not apply here
            if (guard_unknown) {
                if (!e && (n.type == AttrType::T1 || n.type == AttrType::T2 || n.type == AttrType::T1C ||
                           n.type == AttrType::T2C))
                    ++not_evaluated;
                if (e && (n.type == AttrType::T1 || n.type == AttrType::T1C) && element_empty(*e))
                    sink.add(Severity::Error, "empty-type1", 0, here,
                             "Type " + std::string(attr_type_name(n.type)) + " attribute has no value", module_name);
                if (e && e->is_sequence() && !n.children.empty())
                    for (std::size_t i = 0; i < e->items.size(); ++i) {
                        Path ip = here;
                        ip.back().item = static_cast<std::int32_t>(i);
                        check_list(e->items[i], n.children, ip);
                    }
                continue;
            }
            switch (n.type) {
                case AttrType::T1: required = value_required = true; break;
                case AttrType::T2: required = true; break;
                case AttrType::T1C:
                case AttrType::T2C: {
                    const auto r = evaluate(ds, n);
                    if (r.has_value()) {
                        required = *r;
                        value_required = *r && n.type == AttrType::T1C;
                    } else if (!e) {
                        ++not_evaluated;
                    }
                    if (e && n.type == AttrType::T1C) value_required = true;  // present 1C must have a value
                    break;
                }
                default: break;
            }
            if (n.mask != 0xFFFFFFFFu && !e) continue;  // repeating groups: only when one is present
            if (!e) {
                if (required) {
                    const bool t1 = n.type == AttrType::T1 || n.type == AttrType::T1C;
                    const bool uid = n.tag == tags::SOPInstanceUID || n.tag == tags::StudyInstanceUID ||
                                     n.tag == tags::SeriesInstanceUID ||
                                     (n.tag == tags::NumberOfFrames && top.contains(tags::PixelData));
                    sink.add(Severity::Error, t1 ? "missing-type1" : "missing-type2",
                             t1 ? (uid && path.empty() ? std::uint32_t{kAttributes} : 0u) : std::uint32_t{kAttributes}, here,
                             "Type " + std::string(attr_type_name(n.type)) + " attribute is missing" +
                                 (n.condition && (n.type == AttrType::T1C || n.type == AttrType::T2C) ? " (condition met)" : ""),
                             module_name);
                }
                continue;
            }
            if (value_required && element_empty(*e))
                sink.add(Severity::Error, "empty-type1", 0, here,
                         "Type " + std::string(attr_type_name(n.type)) + " attribute has no value", module_name);
            if (!n.enumerated.empty() && !e->is_sequence()) {
                const auto vals = values_of(*e);
                for (std::size_t i = 0; i < vals.size(); ++i) {
                    bool ok = false;
                    for (std::string_view t : n.enumerated)
                        if (enum_match(vals[i], e->vr, t)) ok = true;
                    if (!ok)
                        sink.add(Severity::Error, "enumerated-value", 0, here,
                                 "value " + std::to_string(i + 1) + " is not one of the Enumerated Values (" +
                                     std::to_string(n.enumerated.size()) + " defined)",
                                 module_name);
                }
            }
            if (e->is_sequence() && !n.children.empty()) {
                for (std::size_t i = 0; i < e->items.size(); ++i) {
                    Path ip = here;
                    ip.back().item = static_cast<std::int32_t>(i);
                    check_list(e->items[i], n.children, ip);
                }
            }
        }
    }
};

bool module_present(const Dataset& ds, const ModuleReq& m, const std::set<Tag>& mandatory_tags) {
    if (!m.attributes) return false;
    // A module is present when any of its own attributes (of any Type) is, apart from attributes
    // that also belong to a mandatory module.
    for (const ReqNode& n : *m.attributes) {
        if (mandatory_tags.count(n.tag)) continue;
        if (n.mask == 0xFFFFFFFFu) {
            if (ds.contains(n.tag)) return true;
        } else {
            for (const Element& e : ds.elements())
                if (n.matches(e.tag)) return true;
        }
    }
    return false;
}

void check_iod_into(const DicomFile& f, Sink& sink) {
    const Dataset& ds = f.dataset;
    auto sop = ds.get_string(tags::SOPClassUID);
    if (!sop || sop->empty()) {
        sink.add(Severity::Error, "missing-sop-class", 0, child({}, tags::SOPClassUID),
                 "SOP Class UID is missing: the IOD cannot be determined");
        return;
    }
    const IodReq* iod = iod_for_sop_class(*sop);
    if (!iod) {
        sink.add(Severity::Warning, "unknown-sop-class", 0, child({}, tags::SOPClassUID),
                 "SOP Class UID is not a standard storage SOP Class with an IOD in PS3.3; IOD checks skipped");
        return;
    }
    std::set<Tag> mandatory_tags;
    std::map<Tag, AttrType> overrides;
    for (const ModuleReq& m : iod->modules) {
        if (!m.attributes) continue;
        for (const ReqNode& n : *m.attributes) {
            if (m.usage == ModuleUsage::M) mandatory_tags.insert(n.tag);
            if (n.overrides) overrides[n.tag] = n.type;
        }
    }
    for (const ModuleReq& m : iod->modules) {
        if (!m.attributes) continue;
        if (m.usage != ModuleUsage::M && !module_present(ds, m, mandatory_tags)) continue;
        IodChecker c{sink, ds, std::string(m.name) + " Module", 0, &overrides};
        c.check_list(ds, *m.attributes, {});
        if (c.not_evaluated)
            sink.add(Severity::Info, "condition-not-evaluated", 0, {},
                     std::to_string(c.not_evaluated) +
                         " absent Type 1C/2C attribute(s) whose condition was not evaluated automatically",
                     c.module_name);
    }
}

}  // namespace

std::vector<Finding> check_iod(const DicomFile& file, const ValidateOptions& opt) {
    std::vector<Finding> out;
    Sink sink(out, opt.max_findings);
    check_iod_into(file, sink);
    return out;
}

std::vector<Finding> validate(const ScanResult& sr, const ValidateOptions& opt) {
    std::vector<Finding> out = sr.findings;
    Sink sink(out, opt.max_findings);
    if (sr.fatal) return out;
    check_meta(sr, sink);
    if (opt.check_dictionary) {
        DictChecker d{sink};
        d.dataset(sr.file.meta, {}, nullptr, true);
        d.dataset(sr.file.dataset, {}, nullptr, false);
    }
    if (opt.check_iod) check_iod_into(sr.file, sink);
    if (opt.check_pixels) {
        PixelAnalysis pa = analyse_pixels(sr.file.dataset, sr.file.transfer_syntax, &sr.pixel);
        for (Finding& f : pa.findings) sink.add(f.severity, f.code, f.category, f.path, f.message);
    }
    return out;
}

Result<std::vector<Finding>> validate_bytes(const Bytes& data, const ValidateOptions& opt) {
    auto sr = scan(data);
    if (!sr) return sr.take_error();
    return validate(*sr, opt);
}

Result<std::vector<Finding>> validate_file(const std::string& path, const ValidateOptions& opt) {
    auto sr = scan_file(path);
    if (!sr) return sr.take_error();
    return validate(*sr, opt);
}

}  // namespace siaara::repair
