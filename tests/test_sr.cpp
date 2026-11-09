// Clinical layer: Structured Report content trees (src/clinical/sr.*): every value type, by-
// reference relationships, text rendering, limits and malformed input, the Key Object Selection
// writer round trip (CID 7010 from the generated PS3.16 tables) and the C API tree walk.
#include <cstring>
#include <string>
#include <vector>

#include "clinical/codes.hpp"
#include "clinical/common.hpp"
#include "clinical/sr.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;
using namespace siaara::clinical;
namespace ct = siaara::clinical::tags;

namespace {

void put(Dataset& d, Tag t, VR vr, std::string_view v) { (void)d.set_string(t, vr, v); }

void concept_of(Dataset& d, std::string_view value, std::string_view scheme, std::string_view meaning) {
    d.put(make_sequence(ct::ConceptNameCodeSequence, {code_item(Code{std::string(value), std::string(scheme), std::string(meaning), {}})}));
}

Dataset item(std::string_view rel, std::string_view vt, std::string_view cv = "", std::string_view meaning = "") {
    Dataset d;
    put(d, ct::RelationshipType, VR::CS, rel);
    put(d, ct::ValueType, VR::CS, vt);
    if (!cv.empty()) concept_of(d, cv, "DCM", meaning);
    return d;
}

Dataset sop_ref(std::string_view cls, std::string_view inst, std::string_view frames = "") {
    Dataset r;
    put(r, ct::ReferencedSopClassUid, VR::UI, cls);
    put(r, ct::ReferencedSopInstanceUid, VR::UI, inst);
    if (!frames.empty()) put(r, ct::ReferencedFrameNumber, VR::IS, frames);
    return r;
}

// A Comprehensive 3D SR using every value type.
Dataset comprehensive() {
    Dataset d;
    put(d, siaara::tags::SOPClassUID, VR::UI, "1.2.840.10008.5.1.4.1.1.88.34");
    put(d, siaara::tags::SOPInstanceUID, VR::UI, "1.2.3.500");
    put(d, ct::ValueType, VR::CS, "CONTAINER");
    concept_of(d, "126000", "DCM", "Imaging Measurement Report");
    put(d, ct::ContinuityOfContent, VR::CS, "SEPARATE");
    put(d, ct::CompletionFlag, VR::CS, "COMPLETE");
    put(d, ct::VerificationFlag, VR::CS, "UNVERIFIED");
    Dataset tmpl;
    put(tmpl, ct::MappingResource, VR::CS, "DCMR");
    put(tmpl, ct::TemplateIdentifier, VR::CS, "1500");
    d.put(make_sequence(ct::ContentTemplateSequence, {tmpl}));
    std::vector<Dataset> c;
    {  // 1.1 TEXT
        Dataset t = item("HAS OBS CONTEXT", "TEXT", "121008", "Person Observer Name");
        put(t, ct::TextValue, VR::UT, "observer note");
        c.push_back(t);
    }
    {  // 1.2 CODE
        Dataset t = item("HAS CONCEPT MOD", "CODE", "121058", "Procedure reported");
        t.put(make_sequence(ct::ConceptCodeSequence, {code_item(Code{"77477000", "SCT", "CT", {}})}));
        c.push_back(t);
    }
    {  // 1.3 CONTAINER with NUM (1.3.1), IMAGE (1.3.2), SCOORD (1.3.3 > 1.3.3.1 by reference)
        Dataset grp = item("CONTAINS", "CONTAINER", "125007", "Measurement Group");
        put(grp, ct::ContinuityOfContent, VR::CS, "SEPARATE");
        std::vector<Dataset> g;
        Dataset num = item("CONTAINS", "NUM", "410668003", "Length");
        Dataset mv;
        put(mv, ct::NumericValue, VR::DS, "12.5");
        mv.put(make_sequence(ct::MeasurementUnitsCodeSequence, {code_item(Code{"mm", "UCUM", "millimeter", {}})}));
        num.put(make_sequence(ct::MeasuredValueSequence, {mv}));
        g.push_back(num);
        Dataset img = item("CONTAINS", "IMAGE");
        img.put(make_sequence(ct::ReferencedSopSequence, {sop_ref("1.2.840.10008.5.1.4.1.1.2", "1.2.3.77", "2\\3")}));
        g.push_back(img);
        Dataset sc = item("INFERRED FROM", "SCOORD", "111030", "Image Region");
        put(sc, ct::GraphicType, VR::CS, "POLYLINE");
        const float pts[4] = {1, 2, 3, 4};
        (void)sc.set_bytes(ct::GraphicData, VR::FL, encode_fl(pts, 4));
        Dataset byref;
        put(byref, ct::RelationshipType, VR::CS, "SELECTED FROM");
        const std::uint8_t path[12] = {1, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0};  // 1.3.2 (the IMAGE)
        (void)byref.set_bytes(ct::ReferencedContentItemIdentifier, VR::UL, Bytes::copy_of(path, 12));
        sc.put(make_sequence(ct::ContentSequence, {byref}));
        g.push_back(sc);
        grp.put(make_sequence(ct::ContentSequence, g));
        c.push_back(grp);
    }
    {  // 1.4 .. 1.8 DATE, TIME, DATETIME, UIDREF, PNAME
        Dataset a = item("HAS OBS CONTEXT", "DATE", "111060", "Study Date");
        put(a, ct::DateValue, VR::DA, "20260301");
        c.push_back(a);
        Dataset b = item("HAS OBS CONTEXT", "TIME", "111061", "Study Time");
        put(b, ct::TimeValue, VR::TM, "101500");
        c.push_back(b);
        Dataset e = item("HAS OBS CONTEXT", "DATETIME", "111526", "DateTime Started");
        put(e, ct::DateTimeValue, VR::DT, "20260301101500");
        c.push_back(e);
        Dataset u = item("HAS OBS CONTEXT", "UIDREF", "121018", "Procedure Study Instance UID");
        put(u, ct::UidValue, VR::UI, "1.2.3.1");
        c.push_back(u);
        Dataset p = item("HAS OBS CONTEXT", "PNAME", "121008", "Person Observer Name");
        put(p, ct::PersonNameValue, VR::PN, "Smith^John");
        c.push_back(p);
    }
    {  // 1.9 COMPOSITE, 1.10 WAVEFORM, 1.11 SCOORD3D, 1.12 TCOORD, 1.13 TABLE
        Dataset co = item("CONTAINS", "COMPOSITE", "121112", "Source of Measurement");
        co.put(make_sequence(ct::ReferencedSopSequence, {sop_ref("1.2.840.10008.5.1.4.1.1.88.33", "1.2.3.88")}));
        c.push_back(co);
        Dataset wf = item("CONTAINS", "WAVEFORM");
        Dataset wr = sop_ref("1.2.840.10008.5.1.4.1.1.9.1.1", "1.2.3.99");
        const std::uint8_t ch[4] = {1, 0, 2, 0};
        (void)wr.set_bytes(ct::ReferencedWaveformChannels, VR::US, Bytes::copy_of(ch, 4));
        wf.put(make_sequence(ct::ReferencedSopSequence, {wr}));
        c.push_back(wf);
        Dataset s3 = item("CONTAINS", "SCOORD3D", "111030", "Image Region");
        put(s3, ct::GraphicType, VR::CS, "POINT");
        const float p3[3] = {10, 20, 30};
        (void)s3.set_bytes(ct::GraphicData, VR::FL, encode_fl(p3, 3));
        put(s3, 0x30060024, VR::UI, "1.2.3.4");
        c.push_back(s3);
        Dataset tc = item("CONTAINS", "TCOORD");
        put(tc, ct::TemporalRangeType, VR::CS, "SEGMENT");
        put(tc, ct::ReferencedTimeOffsets, VR::DS, "0.5\\1.5");
        c.push_back(tc);
        Dataset tb = item("CONTAINS", "TABLE", "999", "Table");
        Dataset tv;
        (void)tv.set_uint32(ct::NumberOfTableRows, 2);
        (void)tv.set_uint32(ct::NumberOfTableColumns, 2);
        Dataset cell;
        (void)cell.set_uint32(ct::TableRowNumber, 1);
        put(cell, ct::SelectorAttributeVr, VR::CS, "DS");
        put(cell, 0x00720072, VR::DS, "1.5\\2.5");
        tv.put(make_sequence(ct::CellValuesSequence, {cell}));
        tb.put(make_sequence(ct::TabulatedValuesSequence, {tv}));
        c.push_back(tb);
    }
    d.put(make_sequence(ct::ContentSequence, c));
    return d;
}

const SrNode* by_position(const SrDocument& doc, std::vector<std::uint32_t> pos) {
    for (const auto& n : doc.nodes)
        if (n.position == pos) return &n;
    return nullptr;
}

}  // namespace

TEST_CASE("sr: every value type and by-reference relationships") {
    auto doc = read_sr(comprehensive(), {64, 1u << 20, false});
    REQUIRE(doc.is_ok());
    CHECK(doc->kind == SrDocumentKind::Comprehensive3D);
    CHECK(doc->completion_flag == "COMPLETE" && doc->verification_flag == "UNVERIFIED");
    CHECK(doc->warnings.empty());
    CHECK_EQ(doc->nodes.size(), 18u);
    const SrNode& root = doc->nodes[0];
    CHECK(root.value_type == SrValueType::Container && root.parent == -1 && root.relationship == SrRelationship::None);
    CHECK(root.concept_name && root.concept_name->value == "126000");
    CHECK(root.template_id == "1500" && root.mapping_resource == "DCMR" && root.continuity == "SEPARATE");
    CHECK_EQ(root.children.size(), 13u);
    const SrNode* t = by_position(*doc, {1, 1});
    REQUIRE(t);
    CHECK(t->value_type == SrValueType::Text && t->text == "observer note" && t->relationship == SrRelationship::HasObsContext);
    const SrNode* code = by_position(*doc, {1, 2});
    REQUIRE(code && code->code);
    CHECK(code->code->value == "77477000" && code->code->scheme == "SCT" && code->relationship == SrRelationship::HasConceptMod);
    const SrNode* num = by_position(*doc, {1, 3, 1});
    REQUIRE(num);
    CHECK(num->value_type == SrValueType::Num && num->numeric && *num->numeric == 12.5 && num->numeric_text == "12.5");
    CHECK(num->units && num->units->value == "mm" && num->units->scheme == "UCUM");
    CHECK(num->depth == 2);
    const SrNode* img = by_position(*doc, {1, 3, 2});
    REQUIRE(img && img->references.size() == 1);
    CHECK(img->references[0].sop_instance_uid == "1.2.3.77");
    CHECK((img->references[0].frames == std::vector<std::int64_t>{2, 3}));
    const SrNode* sc = by_position(*doc, {1, 3, 3});
    REQUIRE(sc);
    CHECK(sc->value_type == SrValueType::Scoord && sc->graphic_type == "POLYLINE" && sc->graphic_data.size() == 4);
    CHECK(sc->relationship == SrRelationship::InferredFrom);
    const SrNode* br = by_position(*doc, {1, 3, 3, 1});
    REQUIRE(br);
    CHECK(br->value_type == SrValueType::Reference && br->relationship == SrRelationship::SelectedFrom);
    REQUIRE(br->referenced_node >= 0);
    CHECK(&doc->nodes[static_cast<std::size_t>(br->referenced_node)] == img);
    CHECK(by_position(*doc, {1, 4})->text == "20260301");
    CHECK(by_position(*doc, {1, 5})->text == "101500");
    CHECK(by_position(*doc, {1, 6})->value_type == SrValueType::DateTime);
    CHECK(by_position(*doc, {1, 7})->text == "1.2.3.1");
    CHECK(by_position(*doc, {1, 8})->text == "Smith^John");
    CHECK(by_position(*doc, {1, 9})->value_type == SrValueType::Composite);
    const SrNode* wf = by_position(*doc, {1, 10});
    CHECK(wf && (wf->references[0].channels == std::vector<std::int64_t>{1, 2}));
    const SrNode* s3 = by_position(*doc, {1, 11});
    CHECK(s3 && s3->graphic_data.size() == 3 && s3->frame_of_reference_uid == "1.2.3.4");
    const SrNode* tc = by_position(*doc, {1, 12});
    CHECK(tc && tc->temporal_range_type == "SEGMENT" && tc->time_offsets.size() == 2);
    const SrNode* tb = by_position(*doc, {1, 13});
    REQUIRE(tb && tb->value_type == SrValueType::Table);
    CHECK(tb->table_rows == 2 && tb->table_columns == 2 && tb->cells.size() == 1);
    CHECK((tb->cells[0].values == std::vector<std::string>{"1.5", "2.5"}) && tb->cells[0].row == 1);
    // Text rendering.
    const std::string text = sr_to_text(*doc);
    CHECK(text.find("1 CONTAINER (126000, DCM, \"Imaging Measurement Report\") [SEPARATE] TID 1500") != std::string::npos);
    CHECK(text.find("    1.3.1 CONTAINS NUM (410668003, DCM, \"Length\") = 12.5 mm") != std::string::npos);
    CHECK(text.find("1.3.3.1 SELECTED FROM -> 1.3.2") != std::string::npos);
    CHECK(text.find("1.13 CONTAINS TABLE") != std::string::npos);
    CHECK(sr_to_text(*doc, 40).size() <= 44);
}

TEST_CASE("sr: limits and malformed trees") {
    {  // unknown value type: warning (lenient) or error (strict)
        Dataset d = comprehensive();
        auto& c = d.find(ct::ContentSequence)->items;
        put(c[0], ct::ValueType, VR::CS, "BOGUS");
        put(c[1], ct::RelationshipType, VR::CS, "IS FRIENDS WITH");
        auto lenient = read_sr(d);
        REQUIRE(lenient.is_ok());
        CHECK(lenient->warnings.size() >= 2);
        CHECK(lenient->nodes[1].value_type == SrValueType::Unknown);
        CHECK(lenient->nodes[2].relationship == SrRelationship::Unknown);
        SrReadOptions strict;
        strict.lenient = false;
        CHECK(!read_sr(d, strict).is_ok());
    }
    {  // unresolved by-reference
        Dataset d;
        put(d, ct::ValueType, VR::CS, "CONTAINER");
        Dataset r;
        put(r, ct::RelationshipType, VR::CS, "INFERRED FROM");
        const std::uint8_t path[8] = {1, 0, 0, 0, 9, 0, 0, 0};
        (void)r.set_bytes(ct::ReferencedContentItemIdentifier, VR::UL, Bytes::copy_of(path, 8));
        d.put(make_sequence(ct::ContentSequence, {r}));
        auto doc = read_sr(d);
        REQUIRE(doc.is_ok());
        CHECK(doc->nodes[1].referenced_node == -1);
        CHECK(!doc->warnings.empty());
        CHECK(sr_to_text(*doc).find("(unresolved)") != std::string::npos);
    }
    {  // deep nesting beyond the limit
        Dataset leaf = item("CONTAINS", "CONTAINER");
        for (int i = 0; i < 10; ++i) {
            Dataset p = item("CONTAINS", "CONTAINER");
            p.put(make_sequence(ct::ContentSequence, {leaf}));
            leaf = p;
        }
        put(leaf, ct::ValueType, VR::CS, "CONTAINER");
        SrReadOptions o;
        o.max_depth = 5;
        CHECK(read_sr(leaf, o).error().code == Errc::LimitExceeded);
        o.max_depth = 64;
        o.max_nodes = 4;
        CHECK(read_sr(leaf, o).error().code == Errc::LimitExceeded);
        CHECK(read_sr(leaf).is_ok());
    }
    {  // NUM without value, IMAGE without reference, SCOORD with odd data
        Dataset d;
        put(d, ct::ValueType, VR::CS, "CONTAINER");
        Dataset sc = item("CONTAINS", "SCOORD");
        const float pts[3] = {1, 2, 3};
        (void)sc.set_bytes(ct::GraphicData, VR::FL, encode_fl(pts, 3));
        d.put(make_sequence(ct::ContentSequence, {item("CONTAINS", "NUM"), item("CONTAINS", "IMAGE"), sc}));
        auto doc = read_sr(d);
        REQUIRE(doc.is_ok());
        CHECK_EQ(doc->warnings.size(), 3u);
        SrReadOptions strict;
        strict.lenient = false;
        CHECK(!read_sr(d, strict).is_ok());
    }
    CHECK(read_sr(Dataset{}).error().code == Errc::NotFound);
}

TEST_CASE("sr: generated CID 7010 table") {
    const auto g = context_group(7010);
    REQUIRE(!g.empty());
    CHECK(g[0].value == "113000" && g[0].scheme == "DCM" && g[0].meaning == "Of Interest");
    auto rej = cid_code(7010, "113001");
    CHECK(rej && rej->meaning == "Rejected for Quality Reasons");
    CHECK(!cid_code(7010, "999999").has_value());
    CHECK(context_group_uid(7010) == "1.2.840.10008.6.1.490");
    CHECK(!context_group(7011).empty());
    CHECK(context_group(1234).empty());
    CHECK(codes::kKeyObjectDescription.meaning == "Key Object Description");
}

TEST_CASE("sr: Key Object Selection writer round trip") {
    Dataset src;
    put(src, siaara::tags::StudyInstanceUID, VR::UI, "1.2.3.1");
    put(src, siaara::tags::PatientName, VR::PN, "Doe^Jane");
    put(src, siaara::tags::PatientID, VR::LO, "ID1");
    KosWriteRequest req;
    req.source = &src;
    req.title = cid_code(7010, "113004");  // For Teaching
    req.description = "teaching file";
    req.references.push_back({"1.2.3.1", "1.2.3.2", "1.2.840.10008.5.1.4.1.1.2", "1.2.3.100", {}, false});
    req.references.push_back({"1.2.3.1", "1.2.3.2", "1.2.840.10008.5.1.4.1.1.2", "1.2.3.101", {2}, false});
    req.references.push_back({"1.2.3.1", "1.2.3.3", "1.2.840.10008.5.1.4.1.1.11.1", "1.2.3.200", {}, true});
    auto f = write_kos(req);
    REQUIRE(f.is_ok());
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(*f, bytes).is_ok());
    auto back = read_part10(Bytes::copy_of(bytes.data(), bytes.size()));
    REQUIRE(back.is_ok());
    const Dataset& d = back->dataset;
    CHECK(d.get_string(siaara::tags::Modality) == std::string("KO"));
    CHECK(d.get_string(siaara::tags::SOPClassUID) == std::string("1.2.840.10008.5.1.4.1.1.88.59"));
    CHECK(d.get_string(siaara::tags::StudyInstanceUID) == std::string("1.2.3.1"));
    CHECK(!d.contains(siaara::tags::SpecificCharacterSet));
    const auto& ev = items_of(d, ct::CurrentRequestedProcedureEvidenceSequence);
    REQUIRE(ev.size() == 1);
    CHECK_EQ(items_of(ev[0], ct::ReferencedSeriesSequence).size(), 2u);
    auto doc = read_sr(d, {64, 1000, false});
    REQUIRE(doc.is_ok());
    CHECK(doc->kind == SrDocumentKind::KeyObjectSelection);
    REQUIRE(doc->nodes.size() == 5);
    CHECK(doc->nodes[0].concept_name && doc->nodes[0].concept_name->value == "113004" &&
          doc->nodes[0].concept_name->meaning == "For Teaching");
    CHECK(doc->nodes[0].template_id == "2010");
    CHECK(doc->nodes[1].value_type == SrValueType::Text && doc->nodes[1].text == "teaching file");
    CHECK(doc->nodes[1].concept_name->value == "113012");
    CHECK(doc->nodes[2].value_type == SrValueType::Image && doc->nodes[2].references[0].sop_instance_uid == "1.2.3.100");
    CHECK((doc->nodes[3].references[0].frames == std::vector<std::int64_t>{2}));
    CHECK(doc->nodes[4].value_type == SrValueType::Composite);
    // Invalid requests.
    KosWriteRequest bad = req;
    bad.title = Code{"121000", "DCM", "Not a KOS title", {}};
    CHECK(!write_kos(bad).is_ok());
    bad = req;
    bad.references.clear();
    CHECK(!write_kos(bad).is_ok());
    bad = req;
    bad.references[0].sop_instance_uid = "x";
    CHECK(!write_kos(bad).is_ok());
    bad = req;
    bad.title.reset();  // default "Of Interest"
    auto def = write_kos(bad);
    REQUIRE(def.is_ok());
    CHECK(read_code(def->dataset, ct::ConceptNameCodeSequence)->meaning == "Of Interest");
}

TEST_CASE("sr: C API tree walk") {
    DicomFile f;
    f.dataset = comprehensive();
    f.transfer_syntax = "1.2.840.10008.1.2.1";
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    siaara_file* file = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &file) == SIAARA_OK);
    siaara_sr* sr = nullptr;
    REQUIRE(siaara_sr_open(file, &sr) == SIAARA_OK);
    std::uint32_t n = 0;
    CHECK_EQ(siaara_sr_node_count(sr, &n), SIAARA_OK);
    CHECK_EQ(n, 18u);
    siaara_sr_node_info info{};
    info.struct_size = sizeof info;
    CHECK_EQ(siaara_sr_node(sr, 0, &info), SIAARA_OK);
    CHECK(info.parent == -1 && info.child_count == 13 && info.value_type == SIAARA_SR_CONTAINER);
    std::uint32_t kids[13];
    size_t need = 0;
    CHECK_EQ(siaara_sr_node_children(sr, 0, kids, 13, &need), SIAARA_OK);
    CHECK_EQ(need, 13u);
    // Node of 1.3 -> its first child is the NUM.
    const std::uint32_t grp = kids[2];
    std::uint32_t gk[3];
    CHECK_EQ(siaara_sr_node_children(sr, grp, gk, 3, &need), SIAARA_OK);
    CHECK_EQ(siaara_sr_node(sr, gk[0], &info), SIAARA_OK);
    CHECK(info.value_type == SIAARA_SR_NUM && info.has_numeric == 1 && info.numeric == 12.5);
    CHECK(info.relationship == SIAARA_REL_CONTAINS);
    char buf[64];
    CHECK_EQ(siaara_sr_node_string(sr, gk[0], SIAARA_SRF_UNITS_VALUE, buf, sizeof buf, &need), SIAARA_OK);
    CHECK(std::strcmp(buf, "mm") == 0);
    CHECK_EQ(siaara_sr_node_string(sr, gk[0], SIAARA_SRF_CONCEPT_MEANING, buf, sizeof buf, &need), SIAARA_OK);
    CHECK(std::strcmp(buf, "Length") == 0);
    CHECK_EQ(siaara_sr_node_string(sr, gk[0], SIAARA_SRF_POSITION, buf, sizeof buf, &need), SIAARA_OK);
    CHECK(std::strcmp(buf, "1.3.1") == 0);
    CHECK_EQ(siaara_sr_node_string(sr, gk[1], SIAARA_SRF_REFERENCED_SOP_INSTANCE, buf, sizeof buf, &need), SIAARA_OK);
    CHECK(std::strcmp(buf, "1.2.3.77") == 0);
    double gd[4];
    CHECK_EQ(siaara_sr_node_graphic_data(sr, gk[2], gd, 4, &need), SIAARA_OK);
    CHECK(need == 4 && gd[3] == 4.0);
    CHECK_EQ(siaara_sr_node_string(sr, 0, 999, buf, sizeof buf, &need), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_sr_node(sr, 9999, &info), SIAARA_ERR_INVALID_ARGUMENT);
    CHECK_EQ(siaara_sr_to_text(sr, nullptr, 0, &need), SIAARA_OK);
    CHECK(need > 100);
    siaara_sr_free(sr);
    siaara_sr_free(nullptr);
    siaara_file_free(file);
}
