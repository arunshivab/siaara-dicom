#include "repair/repair.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <system_error>

#include "charset/charset.hpp"
#include "codec/transcode.hpp"
#include "core/io.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "deid/sha256.hpp"
#include "dict/dictionary.hpp"
#include "image/frames.hpp"
#include "repair/iod.hpp"
#include "repair/pixels.hpp"
#include "repair/values.hpp"

namespace siaara::repair {

namespace fs = std::filesystem;

namespace {

constexpr Tag kPerFrameFunctionalGroups = 0x52009230;

fs::path fs_path(const std::string& utf8) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
std::string utf8_of(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string random_seed() {
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 4; ++i) s += std::to_string(rd()) + ".";
    return s;
}

Path child(const Path& p, Tag t) {
    Path q = p;
    q.push_back({t, -1});
    return q;
}

// The data set holding the element a path ends at, or nullptr.
Dataset* container_of(Dataset& top, const Path& p) {
    Dataset* ds = &top;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        Element* e = ds->find(p[i].tag);
        if (!e || !e->is_sequence() || p[i].item < 0 || static_cast<std::size_t>(p[i].item) >= e->items.size())
            return nullptr;
        ds = &e->items[static_cast<std::size_t>(p[i].item)];
    }
    return ds;
}

bool is_class_uid_tag(Tag t) {
    if (t == tags::TransferSyntaxUID || t == tags::SOPClassUID || t == tags::MediaStorageSOPClassUID) return true;
    if (tag_is_private(t)) return false;
    if (const dict::Entry* e = dict::lookup(t)) {
        const std::string_view k = e->keyword;
        if (k.size() >= 8 && k.substr(k.size() - 8) == "ClassUID") return true;
        if (k.find("TransferSyntax") != std::string_view::npos) return true;
        if (k == "CodingSchemeUID" || k == "ContextGroupExtensionCreatorUID" || k == "MappingResourceUID") return true;
    }
    return false;
}

bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        if (n == 2 && c < 0xC2) return false;
        i += n;
    }
    return true;
}


// ---------------------------------------------------------------- per-file work

struct Work {
    std::size_t index = 0;
    std::string in_path, out_path;
    Bytes data;
    bool from_file = false;
    std::vector<std::uint8_t>* mem_out = nullptr;
    ScanResult scan;
    bool scanned = false;
    DicomFile file;  // working copy
    FileReport rep;
    std::vector<PlannedSet> study;
    WriteOptions wopt;
    std::string sop_before, class_before;
};

class Engine {
public:
    Engine(const RepairOptions& opt) : opt_(opt), seed_(opt.uid_seed.empty() ? random_seed() : opt.uid_seed) {}

    std::string new_uid(std::string_view what) const { return uid_from_seed(seed_ + "|" + std::string(what)); }

    void log(Work& w, std::uint32_t cat, std::string action, Path path, std::string detail, const Element* old_e,
             const Element* new_e) {
        Change c;
        c.category = cat;
        c.action = std::move(action);
        c.detail = std::move(detail);
        const Tag t = path.empty() ? 0 : path.back().tag;
        const VR vr = new_e ? new_e->vr : old_e ? old_e->vr : VR::None;
        Path parent = path;
        if (!parent.empty()) parent.pop_back();
        const bool loggable = opt_.detailed_report || (t != 0 && value_is_loggable(parent, t, vr));
        if (old_e || new_e) {
            if (loggable) {
                c.old_value = old_e ? printable_value(*old_e) : std::string("(absent)");
                c.new_value = new_e ? printable_value(*new_e) : std::string("(removed)");
            } else {
                c.values_withheld = true;
            }
        }
        c.path = std::move(path);
        w.rep.changes.push_back(std::move(c));
    }

    // ---- forced: things the writer always does because the output is a valid encoding
    void forced(Work& w) {
        static const std::set<std::string> kForced = {
            "duplicate-element", "element-order", "unterminated-item", "unterminated-sequence", "stray-delimiter",
            "stray-item", "item-length", "encoding-mismatch", "preamble-missing", "not-part10", "meta-missing",
            "trailing-bytes", "undefined-length", "meta-implicit-vr", "expected-item", "pixel-sequence-unterminated",
            "fragment-truncated", "value-truncated", "truncated"};
        for (const Finding& f : w.scan.findings) {
            if (!kForced.count(f.code)) continue;
            if (f.code == "invalid-vr" && f.category != kStructural) continue;
            const bool pixel = f.code == "fragment-truncated" || f.code == "value-truncated" ||
                               f.code == "pixel-sequence-unterminated";
            Change c;
            c.category = pixel ? kRescue : kStructural;
            c.action = "rewrite";
            c.path = f.path;
            c.detail = "[" + f.code + "] " + f.message +
                       " (always applied when writing: the output is a valid encoding" +
                       (pixel ? "; incomplete bytes are not written)" : ")");
            w.rep.changes.push_back(std::move(c));
        }
        for (const Finding& f : w.scan.findings)
            if (f.code == "invalid-vr" && f.category == kStructural) {
                Change c;
                c.category = kStructural;
                c.action = "re-encode";
                c.path = f.path;
                c.detail = "element without explicit VR written with its dictionary VR (always applied when writing)";
                w.rep.changes.push_back(std::move(c));
            }
    }

    // ---- structural
    void structural(Work& w, PixelAnalysis* pa) {
        DicomFile& f = w.file;
        // Misplaced group 0000 / 0002 elements in the data set.
        std::vector<Tag> drop;
        for (const Element& e : f.dataset.elements())
            if (tag_group(e.tag) == 0x0000 || tag_group(e.tag) == 0x0002) drop.push_back(e.tag);
        for (Tag t : drop) {
            log(w, kStructural, "remove", child({}, t), "command / file meta group element removed from the data set",
                f.dataset.find(t), nullptr);
            f.dataset.remove(t);
        }
        // Group lengths.
        if (opt_.recompute_group_lengths) {
            w.wopt.group_lengths = GroupLengths::Recompute;
            for (const GroupLengthInfo& g : w.scan.group_lengths)
                if (g.group != 0x0002 && g.actual != g.declared)
                    log_text(w, kStructural, "recompute", child(g.path, make_tag(g.group, 0)), "group length recomputed",
                             std::to_string(g.declared), std::to_string(g.actual));
        } else {
            w.wopt.group_lengths = GroupLengths::Drop;
            drop_group_lengths(w, f.dataset, {});
        }
        // Odd lengths.
        w.wopt.odd_lengths = OddLengths::Pad;
        for (const Finding& x : w.scan.findings)
            if (x.code == "odd-length") {
                Change c;
                c.category = kStructural;
                c.action = "pad";
                c.path = x.path;
                c.detail = "odd-length value padded to even length (PS3.5 7.1.1)";
                w.rep.changes.push_back(std::move(c));
            }
        // Offset tables.
        if (pa && (pa->bot_invalid || pa->eot_invalid)) rebuild_offset_tables(w, *pa);
        // File Meta Information.
        rebuild_meta(w);
    }

    void log_text(Work& w, std::uint32_t cat, const char* action, Path p, std::string detail, std::string old_v,
                  std::string new_v) {
        Change c;
        c.category = cat;
        c.action = action;
        c.path = std::move(p);
        c.detail = std::move(detail);
        c.old_value = std::move(old_v);
        c.new_value = std::move(new_v);
        w.rep.changes.push_back(std::move(c));
    }

    void drop_group_lengths(Work& w, Dataset& ds, const Path& path) {
        std::vector<Tag> gl;
        for (Element& e : ds.elements()) {
            if (tag_element(e.tag) == 0 && tag_group(e.tag) != 0x0002) gl.push_back(e.tag);
            if (e.is_sequence())
                for (std::size_t i = 0; i < e.items.size(); ++i) {
                    Path ip = child(path, e.tag);
                    ip.back().item = static_cast<std::int32_t>(i);
                    drop_group_lengths(w, e.items[i], ip);
                }
        }
        for (Tag t : gl) {
            log(w, kStructural, "remove", child(path, t), "group length removed (retired, PS3.5 7.2)", ds.find(t), nullptr);
            ds.remove(t);
        }
    }

    void rebuild_offset_tables(Work& w, const PixelAnalysis& pa) {
        Element* px = w.file.dataset.find(tags::PixelData);
        if (!px || px->fragments.empty() || pa.frames.empty()) return;
        std::vector<Bytes> data(px->fragments.begin() + 1, px->fragments.end());
        const bool has_eot = w.file.dataset.contains(image::tags::ExtendedOffsetTable);
        bool overflow = false;
        std::vector<std::uint8_t> bot = build_basic_offset_table(data, pa.frames, &overflow);
        if (pa.bot_invalid || (has_eot && pa.eot_invalid)) {
            const Element old = *px;
            if (has_eot || overflow) bot.clear();  // an Extended Offset Table requires an empty Basic one
            px->fragments[0] = Bytes::copy_of(bot.data(), bot.size());
            px->read = {};
            log_text(w, kStructural, "rebuild", child({}, tags::PixelData),
                     "Basic Offset Table rebuilt for " + std::to_string(pa.frames.size()) + " frame(s) (" + pa.layout_source + ")",
                     std::to_string(old.fragments[0].size() / 4) + " entries", std::to_string(bot.size() / 4) + " entries");
        }
        if (has_eot && (pa.eot_invalid || overflow)) {
            std::vector<std::uint8_t> off, len;
            std::uint64_t at = 0;
            std::vector<std::uint64_t> item_off;
            for (const Bytes& b : data) {
                item_off.push_back(at);
                at += 8 + b.size() + (b.size() & 1u);
            }
            for (const FrameSpan& fr : pa.frames) {
                std::uint64_t n = 0;
                for (std::uint32_t i = fr.first; i < fr.first + fr.count; ++i) n += data[i].size() + (data[i].size() & 1u);
                std::uint8_t b[8];
                const std::uint64_t o = item_off[fr.first];
                for (int k = 0; k < 8; ++k) b[k] = static_cast<std::uint8_t>(o >> (8 * k));
                off.insert(off.end(), b, b + 8);
                for (int k = 0; k < 8; ++k) b[k] = static_cast<std::uint8_t>(n >> (8 * k));
                len.insert(len.end(), b, b + 8);
            }
            (void)w.file.dataset.set_bytes(image::tags::ExtendedOffsetTable, VR::OV, Bytes::copy_of(off.data(), off.size()));
            (void)w.file.dataset.set_bytes(image::tags::ExtendedOffsetTableLengths, VR::OV,
                                           Bytes::copy_of(len.data(), len.size()));
            log_text(w, kStructural, "rebuild", child({}, image::tags::ExtendedOffsetTable),
                     "Extended Offset Table and Lengths recomputed for " + std::to_string(pa.frames.size()) + " frame(s)", "",
                     "");
        }
    }

    void rebuild_meta(Work& w) {
        DicomFile& f = w.file;
        Dataset meta;
        for (const Element& e : f.meta.elements()) {
            const Tag t = e.tag;
            if (tag_group(t) != 0x0002) continue;
            if (t == tags::FileMetaInformationGroupLength || t == tags::FileMetaInformationVersion ||
                t == tags::MediaStorageSOPClassUID || t == tags::MediaStorageSOPInstanceUID ||
                t == tags::TransferSyntaxUID || t == tags::ImplementationClassUID || t == tags::ImplementationVersionName)
                continue;
            meta.put(e);
        }
        static constexpr std::uint8_t kVersion[2] = {0x00, 0x01};
        (void)meta.set_bytes(tags::FileMetaInformationVersion, VR::OB, Bytes::copy_of(kVersion, 2));
        if (auto v = f.dataset.get_string(tags::SOPClassUID)) (void)meta.set_string(tags::MediaStorageSOPClassUID, VR::UI, *v);
        else if (const Element* e = f.meta.find(tags::MediaStorageSOPClassUID)) meta.put(*e);
        if (auto v = f.dataset.get_string(tags::SOPInstanceUID))
            (void)meta.set_string(tags::MediaStorageSOPInstanceUID, VR::UI, *v);
        else if (const Element* e = f.meta.find(tags::MediaStorageSOPInstanceUID)) meta.put(*e);
        (void)meta.set_string(tags::TransferSyntaxUID, VR::UI, f.transfer_syntax);
        // Keep the original implementation's identity if it had one: the file still comes from it.
        if (const Element* e = f.meta.find(tags::ImplementationClassUID);
            e && uid_is_valid(trim_value(e->value.as_chars(), VR::UI)) && !trim_value(e->value.as_chars(), VR::UI).empty()) {
            meta.put(*e);
            if (const Element* n = f.meta.find(tags::ImplementationVersionName)) meta.put(*n);
        } else {
            (void)meta.set_string(tags::ImplementationClassUID, VR::UI, kImplementationClassUid);
            (void)meta.set_string(tags::ImplementationVersionName, VR::SH, kImplementationVersionName);
        }
        // Log differences.
        std::set<Tag> all;
        for (const Element& e : f.meta.elements())
            if (e.tag != tags::FileMetaInformationGroupLength) all.insert(e.tag);
        for (const Element& e : meta.elements()) all.insert(e.tag);
        for (Tag t : all) {
            const Element* a = f.meta.find(t);
            const Element* b = meta.find(t);
            if (a && b && a->value.size() == b->value.size() &&
                std::equal(a->value.data(), a->value.data() + a->value.size(), b->value.data()) && a->vr == b->vr)
                continue;
            log(w, kStructural, !a ? "add" : !b ? "remove" : "set", child({}, t), "File Meta Information rebuilt (PS3.10 7.1)",
                a, b);
        }
        f.meta = std::move(meta);
        f.meta_as_read = false;
    }

    // ---- values
    static bool all_parts_valid(VR vr, std::string_view s) {
        std::string_view v = s;
        while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.remove_suffix(1);
        if (v.empty()) return true;
        const bool mv = vr != VR::LT && vr != VR::ST && vr != VR::UT && vr != VR::UR;
        if (charset::vr_uses_character_set(vr)) return true;
        if (!mv) return check_value(vr, trim_value(v, vr)) == ValueCheck::Ok;
        bool good = true;
        for_each_value(v, [&](std::size_t, std::string_view part) {  // one pass (R-112)
            good = check_value(vr, trim_value(part, vr)) == ValueCheck::Ok;
            return good;
        });
        return good;
    }

    bool reencode(Element& e, VR want, VR alt) {
        auto take = [&](VR v) {
            e.vr = v;
            return true;
        };
        const VR target = (alt != VR::None && e.vr == VR::SS && alt == VR::SS) ? alt : want;
        if (e.vr == VR::UN) {
            if (want == VR::SQ) {
                std::vector<Dataset> items;
                if (!parse_implicit_items(e.value, items, opt_.scan)) return false;
                e.items = ItemList(std::move(items));
                e.value = {};
                e.read = {};
                return take(VR::SQ);
            }
            if (vr_is_string(want)) return all_parts_valid(want, e.value.as_chars()) ? take(want) : false;
            const std::size_t sz = vr_value_size(want);
            if (sz) return e.value.size() % sz == 0 ? take(want) : false;
            if (want == VR::OW || want == VR::OF || want == VR::OL || want == VR::OD || want == VR::OV) {
                const std::size_t u = want == VR::OW ? 2 : want == VR::OF || want == VR::OL ? 4 : 8;
                return e.value.size() % u == 0 ? take(want) : false;
            }
            if (want == VR::OB) return take(want);
            return false;
        }
        if (vr_is_string(e.vr) && vr_is_string(target))
            return all_parts_valid(target, e.value.as_chars()) ? take(target) : false;
        auto ints_fit = [&](std::size_t w, bool to_signed) {
            if (e.value.size() % w != 0) return false;
            for (std::size_t i = 0; i < e.value.size(); i += w) {
                const std::uint32_t v = w == 2 ? load_le16(e.value.data() + i) : load_le32(e.value.data() + i);
                const std::uint32_t top = w == 2 ? 0x8000u : 0x80000000u;
                if (v >= top) return false;  // the sign bit would change the meaning either way
                (void)to_signed;
            }
            return true;
        };
        if ((e.vr == VR::US && target == VR::SS) || (e.vr == VR::SS && target == VR::US))
            return ints_fit(2, target == VR::SS) ? take(target) : false;
        if ((e.vr == VR::UL && target == VR::SL) || (e.vr == VR::SL && target == VR::UL))
            return ints_fit(4, target == VR::SL) ? take(target) : false;
        if (e.tag != tags::PixelData && ((e.vr == VR::OB && target == VR::OW && e.value.size() % 2 == 0) ||
                                         (e.vr == VR::OW && target == VR::OB)))
            return take(target);
        return false;
    }

    void values(Work& w, Dataset& ds, const Path& path) {
        for (Element& e : ds.elements()) {
            const Path here = child(path, e.tag);
            if (e.is_sequence()) {
                for (std::size_t i = 0; i < e.items.size(); ++i) {
                    Path ip = here;
                    ip.back().item = static_cast<std::int32_t>(i);
                    values(w, e.items[i], ip);
                }
            }
            if (tag_element(e.tag) == 0 || e.is_encapsulated()) continue;
            VR want = VR::None, alt = VR::None;
            if (tag_is_private(e.tag)) {
                if (tag_element(e.tag) >= 0x10 && tag_element(e.tag) <= 0xFF) want = VR::LO;
            } else if (const dict::Entry* d = dict::lookup(e.tag)) {
                want = d->vr;
                alt = d->vr_alt;
            }
            if (want != VR::None && e.vr != want && e.vr != alt &&
                !((want == VR::OB || want == VR::OW) && (e.vr == VR::OB || e.vr == VR::OW))) {
                const VR old = e.vr;
                if (reencode(e, want, alt))
                    log_text(w, kValues, "re-encode", here, "VR changed to the dictionary VR (lossless)",
                             std::string(vr_name(old)), std::string(vr_name(e.vr)));
                else
                    log_text(w, 0, "flag", here,
                             "VR differs from the dictionary and cannot be re-encoded losslessly; kept as " +
                                 std::string(vr_name(old)),
                             "", "");
            }
            if (!vr_is_string(e.vr) || e.is_sequence()) continue;
            if (e.vr != VR::UI && !e.value.empty() && e.value.as_chars().back() == '\0') {
                // NUL padding of a string VR: the padding character is a space (PS3.5 6.2).
                const Element old = e;
                std::string v(e.value.as_chars());
                for (std::size_t k = v.size(); k > 0 && v[k - 1] == '\0'; --k) v[k - 1] = ' ';
                e.value = Bytes::from_string(v);
                e.read = {};
                log(w, kValues, "pad", here, "NUL padding replaced by a space", &old, &e);
            }
            const std::string_view raw = e.value.as_chars();
            const bool mv = e.vr != VR::LT && e.vr != VR::ST && e.vr != VR::UT && e.vr != VR::UR;
            if (!mv) continue;
            std::string_view body = raw;
            while (!body.empty() && (body.back() == '\0' || (body.back() == ' ' && e.vr != VR::UI))) body.remove_suffix(1);
            bool changed = false;
            std::string out;
            if (body.empty()) continue;
            for_each_value(body, [&](std::size_t i, std::string_view part) {  // one pass (R-112)
                if (i) out.push_back('\\');
                const std::string_view tp = e.vr == VR::UI ? part : trim_value(part, e.vr);
                if (check_value(e.vr, tp) != ValueCheck::Ok || (e.vr == VR::UI && tp != trim_value(part, VR::UI))) {
                    if (auto nv = normalise_value(e.vr, part)) {
                        out += *nv;
                        changed = true;
                        return true;
                    }
                }
                out.append(part.data(), part.size());
                return true;
            });
            if (!changed) continue;
            const Element old = e;
            if (out.size() & 1u) out.push_back(vr_pad_char(e.vr));
            e.value = Bytes::from_string(out);
            e.read = {};
            log(w, kValues, "normalise", here, "value(s) normalised to the VR's format (unambiguous)", &old, &e);
        }
    }

    // ---- UIDs
    void collect_invalid_uids(const Dataset& ds) {
        for (const Element& e : ds.elements()) {
            if (e.is_sequence())
                for (const Dataset& it : e.items) collect_invalid_uids(it);
            if (e.vr != VR::UI || is_class_uid_tag(e.tag)) continue;
            std::string_view v = trim_value(e.value.as_chars(), VR::UI);
            if (v.empty()) continue;
            for_each_value(v, [&](std::size_t, std::string_view piece) {  // one pass (R-112)
                std::string part(trim_value(piece, VR::UI));
                if (!part.empty() && !uid_is_valid(part) && !uid_map_.count(part)) uid_map_[part] = new_uid("uid|" + part);
                return true;
            });
        }
    }

    void replace_uids(Work& w, Dataset& ds, const Path& path) {
        for (Element& e : ds.elements()) {
            const Path here = child(path, e.tag);
            if (e.is_sequence())
                for (std::size_t i = 0; i < e.items.size(); ++i) {
                    Path ip = here;
                    ip.back().item = static_cast<std::int32_t>(i);
                    replace_uids(w, e.items[i], ip);
                }
            if (e.vr != VR::UI || is_class_uid_tag(e.tag)) continue;
            std::string_view v = trim_value(e.value.as_chars(), VR::UI);
            if (v.empty()) continue;
            std::string out;
            bool changed = false;
            for_each_value(v, [&](std::size_t i, std::string_view piece) {  // one pass (R-112)
                if (i) out.push_back('\\');
                std::string part(trim_value(piece, VR::UI));
                auto it = uid_map_.find(part);
                if (it != uid_map_.end()) {
                    out += it->second;
                    changed = true;
                } else {
                    out += part;
                }
                return true;
            });
            if (!changed) continue;
            const Element old = e;
            if (out.size() & 1u) out.push_back('\0');
            e.value = Bytes::from_string(out);
            e.read = {};
            log(w, kUids, "replace", here, "invalid UID replaced by a 2.25 UID (same replacement everywhere in this run)",
                &old, &e);
        }
    }

    // ---- character set
    static void gather_text(const Dataset& ds, std::string& out, bool top) {
        if (!top && ds.contains(tags::SpecificCharacterSet)) return;  // items with their own set: not touched
        for (const Element& e : ds.elements()) {
            if (e.is_sequence()) {
                for (const Dataset& it : e.items) gather_text(it, out, false);
                continue;
            }
            if (charset::vr_uses_character_set(e.vr)) {
                out.append(e.value.as_chars());
                out.push_back('\n');
            }
        }
    }

    void fix_charset(Work& w) {
        std::string text;
        gather_text(w.file.dataset, text, true);
        const bool has8 = std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80 || c == 0x1B; });
        if (!has8) return;
        const charset::CharacterSet declared = charset::character_set_of(w.file.dataset);
        std::size_t repl = 0;
        (void)charset::decode(text, declared, charset::TextKind::Text, &repl);
        const bool default_only = declared.scheme == charset::Scheme::Iso2022 && declared.terms.size() == 1 &&
                                  declared.terms[0] == charset::Term::IR6 && !w.file.dataset.contains(tags::SpecificCharacterSet);
        if (repl == 0 && !default_only) return;
        std::string chosen;
        std::string why;
        if (!opt_.charset.empty()) {
            const charset::CharacterSet cs = charset::CharacterSet::parse(opt_.charset);
            std::size_t r = 0;
            (void)charset::decode(text, cs, charset::TextKind::Text, &r);
            if (cs.unknown == 0 && r == 0) {
                chosen = opt_.charset;
                why = "Specific Character Set set as requested (all text decodes in it)";
            } else {
                log_text(w, 0, "flag", child({}, tags::SpecificCharacterSet),
                         "requested character set does not decode all text; not set", "", "");
                return;
            }
        } else if (valid_utf8(text) && text.find('\x1B') == std::string::npos) {
            chosen = "ISO_IR 192";
            why = "Specific Character Set set to UTF-8: all 8-bit text is valid UTF-8";
        } else {
            log_text(w, 0, "flag", child({}, tags::SpecificCharacterSet),
                     "8-bit text is not UTF-8 and several single-byte character sets would decode it; give the "
                     "character set to set it",
                     "", "");
            return;
        }
        const Element* old = w.file.dataset.find(tags::SpecificCharacterSet);
        const Element old_copy = old ? *old : Element{};
        (void)w.file.dataset.set_string(tags::SpecificCharacterSet, VR::CS, chosen);
        log(w, kCharset, old ? "set" : "add", child({}, tags::SpecificCharacterSet), why, old ? &old_copy : nullptr,
            w.file.dataset.find(tags::SpecificCharacterSet));
    }

    // ---- study plan
    void apply_study(Work& w) {
        for (const PlannedSet& p : w.study) {
            const Element* cur = w.file.dataset.find(p.tag);
            const Element old = cur ? *cur : Element{};
            const bool had = cur != nullptr;
            (void)w.file.dataset.set_string(p.tag, p.vr, p.value);
            log(w, kStudy, had ? "set" : "add", child({}, p.tag), p.detail, had ? &old : nullptr, w.file.dataset.find(p.tag));
        }
    }

    // ---- attributes
    void attributes(Work& w) {
        const std::vector<Finding> f = check_iod(w.file, opt_.validate);
        for (const Finding& x : f) {
            if (x.path.empty()) continue;
            const Tag t = x.path.back().tag;
            if (x.code == "missing-type2") {
                Dataset* ds = container_of(w.file.dataset, x.path);
                if (!ds || ds->contains(t)) continue;
                const dict::Entry* d = dict::lookup(t);
                if (!d || d->vr == VR::None) continue;
                Element e;
                e.tag = t;
                e.vr = d->vr;
                ds->put(std::move(e));
                log(w, kAttributes, "add", x.path, "Type 2 attribute added empty (" + x.module + ")", nullptr, ds->find(t));
            } else if (x.code == "missing-type1" && x.path.size() == 1 && t == tags::NumberOfFrames) {
                // Derived from the pixel data, not invented: the frames it holds.
                if (w.file.dataset.contains(t)) continue;
                const PixelAnalysis pa = analyse_pixels(w.file.dataset, w.file.transfer_syntax, &w.scan.pixel);
                std::uint64_t n = 0;
                if (pa.encapsulated) n = pa.frames.size();
                else if (pa.frame_bits && pa.native_actual * 8 % pa.frame_bits == 0) n = pa.native_actual * 8 / pa.frame_bits;
                else if (pa.frame_bits && pa.native_actual > 0 && (pa.native_actual - 1) * 8 % pa.frame_bits == 0)
                    n = (pa.native_actual - 1) * 8 / pa.frame_bits;
                if (n == 0 || n > 0x7FFFFFFF) continue;
                (void)w.file.dataset.set_string(t, VR::IS, std::to_string(n));
                log(w, kAttributes, "add", x.path, "Number of Frames added from the frames the pixel data holds (" + x.module + ")",
                    nullptr, w.file.dataset.find(t));
            } else if (x.code == "missing-type1" && x.path.size() == 1 &&
                       (t == tags::SOPInstanceUID || t == tags::StudyInstanceUID || t == tags::SeriesInstanceUID)) {
                if (w.file.dataset.contains(t)) continue;
                std::string uid;
                if (t == tags::SOPInstanceUID) uid = new_uid("sop|" + w.rep.input_sha256 + "|" + std::to_string(w.index));
                else if (t == tags::StudyInstanceUID) uid = new_uid("study|" + study_key(w.file.dataset));
                else uid = new_uid("series|" + study_key(w.file.dataset) + "|" + key_of(w.file.dataset, tags::Modality) + "|" +
                                   key_of(w.file.dataset, 0x00200011));
                (void)w.file.dataset.set_string(t, VR::UI, uid);
                log(w, kAttributes, "add", x.path, "required UID generated (" + x.module + ")", nullptr, w.file.dataset.find(t));
            }
        }
    }

    static std::string key_of(const Dataset& ds, Tag t) { return ds.get_string(t).value_or(""); }
    static std::string study_key(const Dataset& ds) {
        auto st = ds.get_string(tags::StudyInstanceUID);
        if (st && !st->empty()) return *st;
        return key_of(ds, tags::PatientID) + "|" + key_of(ds, 0x00080020) + "|" + key_of(ds, 0x00080030) + "|" +
               key_of(ds, 0x00080050) + "|" + key_of(ds, 0x00200010);
    }

    // ---- pixels
    void pixel_fixes(Work& w, const PixelAnalysis& pa) {
        for (const AttrFix& fx : pa.fixes) {
            const Element* cur = w.file.dataset.find(fx.tag);
            const Element old = cur ? *cur : Element{};
            const bool had = cur != nullptr;
            if (fx.text) (void)w.file.dataset.set_string(fx.tag, fx.vr, fx.str);
            else (void)w.file.dataset.set_uint16(fx.tag, static_cast<std::uint16_t>(fx.number));
            log(w, kPixels, had ? "set" : "add", child({}, fx.tag), fx.reason, had ? &old : nullptr, w.file.dataset.find(fx.tag));
        }
    }

    void trim_per_frame(Work& w, const std::vector<std::uint32_t>& keep) {
        Element* pf = w.file.dataset.find(kPerFrameFunctionalGroups);
        if (!pf || !pf->is_sequence()) return;
        const std::size_t before = pf->items.size();
        ItemList kept;
        for (std::uint32_t k : keep)
            if (k < pf->items.size()) kept.push_back(pf->items[k]);
        if (kept.size() == before) return;
        pf->items = std::move(kept);
        pf->read = {};
        log_text(w, kRescue, "trim", child({}, kPerFrameFunctionalGroups),
                 "per-frame functional group items kept for the recovered frames only", std::to_string(before) + " item(s)",
                 std::to_string(pf->items.size()) + " item(s)");
    }

    void set_frames(Work& w, std::uint32_t n) {
        const Element* cur = w.file.dataset.find(tags::NumberOfFrames);
        if (!cur && n == 1) return;
        const Element old = cur ? *cur : Element{};
        (void)w.file.dataset.set_string(tags::NumberOfFrames, VR::IS, std::to_string(n));
        log(w, kRescue, cur ? "set" : "add", child({}, tags::NumberOfFrames), "Number of Frames set to the recovered frames",
            cur ? &old : nullptr, w.file.dataset.find(tags::NumberOfFrames));
    }

    void rescue(Work& w, const PixelAnalysis& pa) {
        Dataset& ds = w.file.dataset;
        Element* px = ds.find(tags::PixelData);
        if (!px) return;
        w.rep.rescue_attempted = true;
        w.rep.frames_declared = pa.declared_frames;
        if (!pa.encapsulated) {
            if (pa.frame_bits == 0) return;
            const std::uint32_t declared = pa.declared_frames;
            const std::uint32_t complete = std::min(pa.native_complete_frames, declared);
            for (std::uint32_t i = 0; i < complete; ++i) w.rep.frames_recovered.push_back(i);
            for (std::uint32_t i = complete; i < declared; ++i) w.rep.frames_lost.push_back(i);
            if (complete == declared || complete == 0) return;
            const std::uint64_t bytes = (pa.frame_bits * complete + 7) / 8;
            const std::size_t old_size = px->value.size();
            px->value = px->value.sub(0, static_cast<std::size_t>(bytes));
            px->read = {};
            log_text(w, kRescue, "truncate", child({}, tags::PixelData),
                     "incomplete frame bytes removed; " + std::to_string(complete) + " complete frame(s) kept",
                     std::to_string(old_size) + " bytes", std::to_string(bytes) + " bytes");
            set_frames(w, complete);
            trim_per_frame(w, w.rep.frames_recovered);
            return;
        }
        if (px->fragments.empty() || pa.frames.empty()) return;
        const std::vector<Bytes> data(px->fragments.begin() + 1, px->fragments.end());
        const bool decodable = codec::can_decode(w.file.transfer_syntax);
        // A data set with only the Image Pixel attributes, to decode one frame at a time.
        Dataset probe;
        for (const Element& e : ds.elements())
            if (tag_group(e.tag) == 0x0028 && !e.is_sequence()) probe.put(e);
        (void)probe.set_string(tags::NumberOfFrames, VR::IS, "1");
        std::vector<std::uint32_t> good;
        for (std::uint32_t i = 0; i < pa.frames.size(); ++i) {
            if (!decodable) {
                good.push_back(i);
                continue;
            }
            Element pe;
            pe.tag = tags::PixelData;
            pe.vr = VR::OB;
            pe.undefined_length = true;
            pe.fragments.push_back(Bytes());
            for (std::uint32_t k = pa.frames[i].first; k < pa.frames[i].first + pa.frames[i].count; ++k)
                pe.fragments.push_back(data[k]);
            probe.put(std::move(pe));
            image::FrameOptions fo;
            fo.threads = 1;
            auto fr = image::FrameReader::open(probe, w.file.transfer_syntax, fo);
            if (fr && fr->decode(0)) good.push_back(i);
        }
        const std::uint32_t declared = std::max<std::uint32_t>(pa.declared_frames, static_cast<std::uint32_t>(pa.frames.size()));
        w.rep.frames_declared = declared;
        w.rep.frames_recovered = good;
        for (std::uint32_t i = 0; i < declared; ++i)
            if (std::find(good.begin(), good.end(), i) == good.end()) w.rep.frames_lost.push_back(i);
        if (!decodable)
            log_text(w, 0, "flag", child({}, tags::PixelData),
                     "no decoder for this transfer syntax: frames kept by fragment structure only, not decode-checked", "", "");
        if (good.empty() || (good.size() == pa.frames.size() && declared == pa.frames.size())) return;
        Element np;
        np.tag = tags::PixelData;
        np.vr = px->vr == VR::None ? VR::OB : px->vr;
        np.undefined_length = true;
        np.fragments.push_back(Bytes());
        std::vector<FrameSpan> spans;
        std::vector<Bytes> kept;
        for (std::uint32_t i : good) {
            spans.push_back({static_cast<std::uint32_t>(kept.size()), pa.frames[i].count});
            for (std::uint32_t k = pa.frames[i].first; k < pa.frames[i].first + pa.frames[i].count; ++k) kept.push_back(data[k]);
        }
        bool overflow = false;
        std::vector<std::uint8_t> bot = build_basic_offset_table(kept, spans, &overflow);
        if (spans.size() > 1 && !overflow) np.fragments[0] = Bytes::copy_of(bot.data(), bot.size());
        for (Bytes& b : kept) np.fragments.push_back(std::move(b));
        const std::size_t old_frags = px->fragments.size();
        *px = std::move(np);
        ds.remove(image::tags::ExtendedOffsetTable);
        ds.remove(image::tags::ExtendedOffsetTableLengths);
        log_text(w, kRescue, "salvage", child({}, tags::PixelData),
                 std::to_string(good.size()) + " of " + std::to_string(declared) + " frame(s) kept (" +
                     (decodable ? "complete and decodable" : "complete") + "); Basic Offset Table rebuilt",
                 std::to_string(old_frags) + " item(s)", std::to_string(px->fragments.size()) + " item(s)");
        set_frames(w, static_cast<std::uint32_t>(good.size()));
        trim_per_frame(w, good);
    }

    void decompress(Work& w) {
        const TransferSyntaxInfo* ts = find_transfer_syntax(w.file.transfer_syntax);
        if (!ts || !ts->encapsulated || !w.file.dataset.contains(tags::PixelData)) return;
        if (!codec::can_decode(w.file.transfer_syntax)) {
            log_text(w, 0, "flag", child({}, tags::PixelData), "no decoder for this transfer syntax; not decompressed", "", "");
            return;
        }
        codec::TranscodeReport tr;
        auto r = codec::transcode_dataset(w.file.dataset, w.file.transfer_syntax, ts::ExplicitVRLittleEndian, {}, &tr);
        if (!r) {
            log_text(w, 0, "flag", child({}, tags::PixelData), "decompression failed: " + r.error().message, "", "");
            return;
        }
        // Log the attribute changes the transcoder made (Photometric Interpretation, Planar Configuration ...).
        for (Tag t : {tags::PhotometricInterpretation, tags::PlanarConfiguration, Tag{0x00282110}}) {
            const Element* a = w.file.dataset.find(t);
            const Element* b = r->find(t);
            if ((a == nullptr) != (b == nullptr) ||
                (a && b && (a->value.size() != b->value.size() ||
                            !std::equal(a->value.data(), a->value.data() + a->value.size(), b->value.data()))))
                log(w, kDecompress, "set", child({}, t), "changed by decompression (PS3.5 8.2)", a, b);
        }
        const std::string old_ts = w.file.transfer_syntax;
        w.file.dataset = std::move(r).value();
        w.file.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
        log_text(w, kDecompress, "decompress", child({}, tags::PixelData),
                 "pixel data decoded and written native; transfer syntax changed", old_ts,
                 std::string(ts::ExplicitVRLittleEndian));
    }

    // Keeps the kept-as-read meta in step with UIDs and the transfer syntax the repair changed.
    void sync_meta(Work& w) {
        DicomFile& f = w.file;
        auto sync = [&](Tag meta_tag, Tag ds_tag, const std::string& before) {
            auto now = f.dataset.get_string(ds_tag);
            if (!now || *now == before) return;
            const Element* cur = f.meta.find(meta_tag);
            const Element old = cur ? *cur : Element{};
            (void)f.meta.set_string(meta_tag, VR::UI, *now);
            log(w, kStructural, cur ? "set" : "add", child({}, meta_tag),
                "File Meta Information updated for the data set's changed UID", cur ? &old : nullptr, f.meta.find(meta_tag));
        };
        if (f.meta_as_read) {
            sync(tags::MediaStorageSOPInstanceUID, tags::SOPInstanceUID, w.sop_before);
            sync(tags::MediaStorageSOPClassUID, tags::SOPClassUID, w.class_before);
        }
    }

    // ---- driver
    void prepare(Work& w) {
        w.rep.input_index = w.index;
        w.rep.input_path = w.in_path;
        w.rep.output_path = w.out_path;
        w.rep.dry_run = !opt_.write;
        w.rep.input_sha256 = sha256_hex(w.data.data(), w.data.size());
        ScanOptions so = opt_.scan;
        so.duplicates = opt_.duplicates;
        auto sr = scan(w.data, so);
        if (!sr) {
            w.rep.error = sr.error().message;
            return;
        }
        w.scan = std::move(sr).value();
        w.scanned = true;
        w.rep.findings = validate(w.scan, opt_.validate);
        if (w.scan.fatal) {
            w.rep.error = "no DICOM data could be parsed";
            w.scanned = false;
        }
    }

    void apply(Work& w) {
        if (!w.scanned) return;
        w.file = w.scan.file;
        w.sop_before = w.file.dataset.get_string(tags::SOPInstanceUID).value_or("");
        w.class_before = w.file.dataset.get_string(tags::SOPClassUID).value_or("");
        const std::uint32_t fx = opt_.fix;
        forced(w);
        if (fx & kValues) values(w, w.file.dataset, {});
        if (fx & kUids) {
            replace_uids(w, w.file.dataset, {});
            replace_uids(w, w.file.meta, {});
        }
        if (fx & kCharset) fix_charset(w);
        if (fx & kStudy) apply_study(w);
        if (fx & kAttributes) attributes(w);
        if (fx & kPixels) {
            const PixelAnalysis pa = analyse_pixels(w.file.dataset, w.file.transfer_syntax, &w.scan.pixel);
            pixel_fixes(w, pa);
        }
        if (fx & kRescue) {
            const PixelAnalysis pa = analyse_pixels(w.file.dataset, w.file.transfer_syntax, &w.scan.pixel);
            const bool damaged = std::any_of(pa.findings.begin(), pa.findings.end(), [](const Finding& f) {
                return f.category == kRescue;
            }) || w.scan.pixel.value_truncated || w.scan.pixel.last_fragment_truncated || w.scan.pixel.sequence_unterminated;
            if (damaged) rescue(w, pa);
        }
        if (fx & kDecompress) decompress(w);
        if (fx & kStructural) {
            PixelAnalysis pa = analyse_pixels(w.file.dataset, w.file.transfer_syntax, nullptr);
            structural(w, &pa);
        } else {
            sync_meta(w);
        }
    }

    void finish(Work& w) {
        if (!w.scanned) return;
        std::vector<std::uint8_t> buf;
        if (Status s = write_part10(w.file, buf, w.wopt); !s) {
            w.rep.error = "the repaired file could not be encoded: " + s.message;
            return;
        }
        if (auto sr = scan(Bytes::copy_of(buf.data(), buf.size()))) {
            w.rep.findings_after = validate(*sr, opt_.validate);
            w.rep.after_computed = true;
        }
        w.rep.output_sha256 = sha256_hex(buf.data(), buf.size());
        if (!opt_.write) return;
        if (w.mem_out) {
            *w.mem_out = std::move(buf);
            w.rep.written = true;
            return;
        }
        // Without overwrite the existence check and the write are one step (R-078): an output that
        // appears meanwhile is never replaced.
        const auto mode = opt_.overwrite ? io::WriteMode::Replace : io::WriteMode::NoReplace;
        if (Status s = io::write_file_atomic(w.out_path, buf.data(), buf.size(), mode); !s) {
            w.rep.error = s.code == Errc::Rejected ? "output file exists (use overwrite to replace it)"
                                                   : "writing the output failed: " + s.message;
            return;
        }
        w.rep.written = true;
    }

    void verify_input(Work& w) {
        if (!w.from_file) return;
        auto now = io::read_file(w.in_path);
        if (!now) {
            w.rep.input_unchanged = false;
            w.rep.error = "the input could not be read back to verify it";
            return;
        }
        w.rep.input_unchanged = sha256_hex(now->data(), now->size()) == w.rep.input_sha256;
    }

    void run(std::vector<Work>& works, Report& rep) {
        rep.detailed = opt_.detailed_report;
        for (Work& w : works) prepare(w);
        // Cross-file analysis.
        std::vector<const Dataset*> dsl;
        std::vector<std::string> hashes;
        for (Work& w : works) {
            dsl.push_back(w.scanned ? &w.scan.file.dataset : nullptr);
            hashes.push_back(w.rep.input_sha256);
        }
        if (works.size() > 1) {
            StudyPlan plan = plan_study(dsl, hashes, opt_.study, [this](std::string_view s) { return new_uid(s); });
            rep.study_findings = std::move(plan.findings);
            for (std::size_t i = 0; i < works.size(); ++i) works[i].study = std::move(plan.per_file[i]);
        }
        if (opt_.fix & kUids)
            for (Work& w : works)
                if (w.scanned) {
                    collect_invalid_uids(w.scan.file.dataset);
                    collect_invalid_uids(w.scan.file.meta);
                }
        for (Work& w : works) {
            apply(w);
            finish(w);
            verify_input(w);
            rep.files.push_back(std::move(w.rep));
        }
    }

private:
    const RepairOptions& opt_;
    std::string seed_;
    std::map<std::string, std::string> uid_map_;
};

bool same_file(const std::string& a, const std::string& b) {
    std::error_code ec;
    const fs::path pa = fs_path(a), pb = fs_path(b);
    if (fs::exists(pa, ec) && fs::exists(pb, ec)) return fs::equivalent(pa, pb, ec);
    return fs::weakly_canonical(pa, ec) == fs::weakly_canonical(pb, ec);
}

}  // namespace

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    const deid::Digest d = deid::Sha256::hash(data, size);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : d) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 15u]);
    }
    return s;
}

Result<Report> repair_files(std::span<const std::string> inputs, const std::string& out_dir, const RepairOptions& opt) {
    if (inputs.empty()) return Status(Errc::InvalidArgument, "no input files");
    if (opt.write && out_dir.empty()) return Status(Errc::InvalidArgument, "an output folder is required to write repairs");
    std::vector<Work> works(inputs.size());
    std::set<std::string> used;
    std::error_code ec;
    if (opt.write) {
        fs::create_directories(fs_path(out_dir), ec);
        if (ec) return Status(Errc::IoError, "output folder could not be created");
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        Work& w = works[i];
        w.index = i;
        w.in_path = inputs[i];
        w.from_file = true;
        if (!out_dir.empty()) {
            const fs::path name = fs_path(inputs[i]).filename();
            std::string base = utf8_of(name);
            if (base.empty()) base = "instance";
            std::string candidate = base;
            for (int k = 1; used.count(candidate); ++k) candidate = base + "." + std::to_string(k);
            used.insert(candidate);
            w.out_path = utf8_of(fs_path(out_dir) / fs_path(candidate));
            for (const std::string& in : inputs)
                if (same_file(in, w.out_path))
                    return Status(Errc::InvalidArgument, "an output path is an input file; choose another output folder");
        }
        auto bytes = io::map_file(inputs[i], true);
        if (bytes) w.data = std::move(bytes).value();
    }
    for (Work& w : works)
        if (w.data.empty())
            if (auto again = io::read_file(w.in_path)) w.data = std::move(again).value();
    Report rep;
    Engine eng(opt);
    eng.run(works, rep);
    std::sort(rep.files.begin(), rep.files.end(), [](const FileReport& a, const FileReport& b) { return a.input_index < b.input_index; });
    return rep;
}

Result<Report> repair_file(const std::string& input, const std::string& output, const RepairOptions& opt) {
    if (opt.write && output.empty()) return Status(Errc::InvalidArgument, "an output path is required to write a repair");
    if (!output.empty() && same_file(input, output))
        return Status(Errc::InvalidArgument, "the output path is the input file; a repair always writes a new file");
    auto bytes = io::map_file(input, true);
    if (!bytes) return bytes.take_error();
    std::vector<Work> works(1);
    works[0].in_path = input;
    works[0].out_path = output;
    works[0].from_file = true;
    works[0].data = std::move(bytes).value();
    Report rep;
    Engine eng(opt);
    eng.run(works, rep);
    return rep;
}

Report repair_buffer(const Bytes& input, std::vector<std::uint8_t>* out, const RepairOptions& opt) {
    std::vector<Work> works(1);
    works[0].data = input;
    works[0].mem_out = out;
    Report rep;
    Engine eng(opt);
    eng.run(works, rep);
    return rep;
}

}  // namespace siaara::repair
