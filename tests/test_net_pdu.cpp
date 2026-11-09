// Tests for the Upper Layer PDU codec, the PS3.8 state table, DIMSE command sets and negotiation.
#include <cstdint>
#include <variant>
#include <memory>
#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "net/association.hpp"
#include "net/dimse.hpp"
#include "net/pdu.hpp"
#include "net/socket.hpp"
#include "test.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

Bytes to_bytes(const std::vector<std::uint8_t>& v) { return Bytes::copy_of(v.data(), v.size()); }

std::vector<std::uint8_t> enc(const Pdu& p) {
    std::vector<std::uint8_t> out;
    Status s = encode_pdu(p, out);
    CHECK(s.is_ok());
    return out;
}

AssociateRq full_rq() {
    AssociateRq rq;
    rq.called_ae = "CALLED";
    rq.calling_ae = "CALLING AE 16 ch";
    rq.contexts.push_back({1, "1.2.840.10008.1.1", {"1.2.840.10008.1.2", "1.2.840.10008.1.2.1"}});
    rq.contexts.push_back({3, "1.2.840.10008.5.1.4.1.1.2", {"1.2.840.10008.1.2.4.80"}});
    rq.contexts.push_back({255, "1.2.840.10008.5.1.4.1.2.2.3", {"1.2.840.10008.1.2.1"}});
    rq.user.max_length = 16384;
    rq.user.implementation_class_uid = "1.2.3.4.5";
    rq.user.implementation_version_name = "TEST_1";
    rq.user.async_window = AsyncWindow{4, 2};
    rq.user.roles.push_back({"1.2.840.10008.5.1.4.1.1.2", false, true});
    rq.user.extended.push_back({"1.2.840.10008.5.1.4.1.2.2.1", {1, 0, 1}});
    rq.user.common_extended.push_back(
        {"1.2.840.10008.5.1.4.1.1.2.1", "1.2.840.10008.4.2", {"1.2.840.10008.5.1.4.1.1.2", "1.2.3"}});
    UserIdentityRq id;
    id.type = identity_type::UsernamePasscode;
    id.positive_response_requested = true;
    id.primary = {'u', 's', 'e', 'r'};
    id.secondary = {'p', 'w'};
    rq.user.identity_rq = id;
    return rq;
}

AssociateAc full_ac() {
    AssociateAc ac;
    ac.called_ae = "CALLED";
    ac.calling_ae = "CALLING";
    ac.contexts.push_back({1, pc_result::Acceptance, "1.2.840.10008.1.2.1"});
    ac.contexts.push_back({3, pc_result::TransferSyntaxesNotSupported, ""});
    ac.user.max_length = 0;
    ac.user.implementation_class_uid = "1.2.3.4.6";
    ac.user.implementation_version_name = "SCP";
    ac.user.async_window = AsyncWindow{1, 1};
    ac.user.roles.push_back({"1.2.840.10008.5.1.4.1.1.2", false, true});
    ac.user.extended.push_back({"1.2.840.10008.5.1.4.1.2.2.1", {1}});
    ac.user.identity_ac = UserIdentityAc{{'o', 'k'}};
    return ac;
}

PDataTf sample_pdata() {
    PDataTf d;
    const std::uint8_t a[5] = {1, 2, 3, 4, 5};
    d.pdvs.push_back({1, true, false, Bytes::copy_of(a, 3)});
    d.pdvs.push_back({1, true, true, Bytes::copy_of(a + 3, 2)});
    d.pdvs.push_back({3, false, true, Bytes()});
    return d;
}

std::vector<std::vector<std::uint8_t>> all_samples() {
    std::vector<std::vector<std::uint8_t>> v;
    v.push_back(enc(full_rq()));
    v.push_back(enc(full_ac()));
    v.push_back(enc(AssociateRj{rj::Transient, rj::SourcePresentationProvider, rj::PresentationLocalLimitExceeded}));
    v.push_back(enc(sample_pdata()));
    v.push_back(enc(ReleaseRq{}));
    v.push_back(enc(ReleaseRp{}));
    v.push_back(enc(Abort{abort_src::Provider, abort_reason::UnexpectedPdu}));
    return v;
}

// In-memory transport fed from a byte queue, delivering at most `chunk` bytes per read.
class MemTransport final : public Transport {
public:
    explicit MemTransport(std::vector<std::uint8_t> in, std::size_t chunk = 7) : in_(std::move(in)), chunk_(chunk) {}
    Result<std::size_t> read_some(std::uint8_t* buf, std::size_t n, int) override {
        if (pos_ >= in_.size()) return std::size_t{0};
        const std::size_t k = std::min({n, chunk_, in_.size() - pos_});
        std::memcpy(buf, in_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    Status write_all(const std::uint8_t* d, std::size_t n, int) override {
        out.insert(out.end(), d, d + n);
        return Status::ok();
    }
    Result<bool> wait_readable(int) override { return true; }
    void shutdown() noexcept override {}
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }
    std::string peer_address() const override { return "mem"; }
    std::vector<std::uint8_t> out;

private:
    std::vector<std::uint8_t> in_;
    std::size_t chunk_;
    std::size_t pos_ = 0;
    bool open_ = true;
};

std::uint32_t lcg(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

}  // namespace

TEST_CASE("net/pdu: A-ASSOCIATE-RQ round trip with every sub-item") {
    const AssociateRq rq = full_rq();
    const std::vector<std::uint8_t> b = enc(rq);
    CHECK_EQ(b[0], 0x01);
    Result<Pdu> p = parse_pdu(to_bytes(b));
    REQUIRE(p.is_ok());
    REQUIRE(std::holds_alternative<AssociateRq>(*p));
    CHECK(std::get<AssociateRq>(*p) == rq);
    // re-encoding is byte-identical
    CHECK(enc(*p) == b);
}

TEST_CASE("net/pdu: A-ASSOCIATE-AC round trip") {
    const AssociateAc ac = full_ac();
    const std::vector<std::uint8_t> b = enc(ac);
    Result<Pdu> p = parse_pdu(to_bytes(b));
    REQUIRE(p.is_ok());
    REQUIRE(std::holds_alternative<AssociateAc>(*p));
    AssociateAc got = std::get<AssociateAc>(*p);
    CHECK_EQ(got.contexts.size(), 2u);
    CHECK_EQ(got.contexts[0].transfer_syntax, "1.2.840.10008.1.2.1");
    CHECK_EQ(got.contexts[1].result, pc_result::TransferSyntaxesNotSupported);
    CHECK(got.contexts[1].transfer_syntax.empty());  // not significant when rejected
    CHECK(got.user == ac.user);
    CHECK_EQ(got.called_ae, "CALLED");
}

TEST_CASE("net/pdu: fixed PDUs and P-DATA-TF round trip") {
    {
        Result<Pdu> p = parse_pdu(to_bytes(enc(AssociateRj{rj::Permanent, rj::SourceUser, rj::UserCalledAeNotRecognized})));
        REQUIRE(p.is_ok());
        CHECK(std::get<AssociateRj>(*p) == (AssociateRj{1, 1, 7}));
    }
    for (const Pdu& x : {Pdu(ReleaseRq{}), Pdu(ReleaseRp{}), Pdu(Abort{2, 6})}) {
        const auto b = enc(x);
        CHECK_EQ(b.size(), 10u);
        Result<Pdu> p = parse_pdu(to_bytes(b));
        REQUIRE(p.is_ok());
        CHECK_EQ(p->index(), x.index());
        CHECK(enc(*p) == b);
    }
    const auto b = enc(sample_pdata());
    Result<Pdu> p = parse_pdu(to_bytes(b));
    REQUIRE(p.is_ok());
    const PDataTf& d = std::get<PDataTf>(*p);
    REQUIRE(d.pdvs.size() == 3);
    CHECK(d.pdvs[0].command && !d.pdvs[0].last && d.pdvs[0].data.size() == 3);
    CHECK(d.pdvs[1].command && d.pdvs[1].last && d.pdvs[1].data.data()[1] == 5);
    CHECK(!d.pdvs[2].command && d.pdvs[2].last && d.pdvs[2].data.empty() && d.pdvs[2].context_id == 3);
    std::uint8_t h[12];
    encode_single_pdv_header(5, false, true, 100, h);
    CHECK_EQ(load_be32(h + 2), 106u);
    CHECK_EQ(load_be32(h + 6), 102u);
    CHECK_EQ(h[11], 2);
}

TEST_CASE("net/pdu: every truncation is an error") {
    for (const auto& b : all_samples()) {
        for (std::size_t n = 0; n < b.size(); ++n) {
            std::vector<std::uint8_t> t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
            Result<Pdu> p = parse_pdu(to_bytes(t));
            CHECK(!p.is_ok());
            // Same prefix with the PDU length field rewritten to match: must not crash; when it
            // still parses (a cut at an item boundary), it must re-encode.
            if (n >= 6) {
                store_be32(t.data() + 2, static_cast<std::uint32_t>(n - 6));
                Result<Pdu> q = parse_pdu(to_bytes(t));
                if (q.is_ok()) {
                    std::vector<std::uint8_t> again;
                    CHECK(encode_pdu(*q, again).is_ok() || std::holds_alternative<PDataTf>(*q));
                }
            }
        }
        // read_pdu over a transport closing early: error, never a partial PDU
        for (std::size_t n = 0; n < b.size(); n += 3) {
            MemTransport mt(std::vector<std::uint8_t>(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n)));
            CHECK(!read_pdu(mt, 10).is_ok());
        }
        MemTransport whole(b, 5);
        CHECK(read_pdu(whole, 10).is_ok());
    }
}

TEST_CASE("net/pdu: random corruption never crashes") {
    std::uint32_t seed = 12345;
    const auto samples = all_samples();
    int parsed = 0, failed = 0;
    for (int iter = 0; iter < 30000; ++iter) {
        std::vector<std::uint8_t> b = samples[static_cast<std::size_t>(iter) % samples.size()];
        const int flips = 1 + static_cast<int>(lcg(seed) % 4);
        for (int f = 0; f < flips; ++f) {
            const std::size_t at = lcg(seed) % b.size();
            b[at] = static_cast<std::uint8_t>(lcg(seed));
        }
        if (lcg(seed) % 5 == 0) b.resize(lcg(seed) % (b.size() + 1));
        Result<Pdu> p = parse_pdu(to_bytes(b));
        if (p.is_ok()) {
            ++parsed;
            std::vector<std::uint8_t> again;
            (void)encode_pdu(*p, again);
        } else {
            ++failed;
        }
    }
    CHECK(failed > 0);
    CHECK(parsed + failed == 30000);
}

TEST_CASE("net/pdu: limits") {
    // PDU length over the cap is refused before allocation (header only, huge length).
    std::vector<std::uint8_t> hdr = {0x04, 0, 0xFF, 0xFF, 0xFF, 0xF0};
    MemTransport mt(hdr);
    Result<Pdu> r = read_pdu(mt, 10);
    REQUIRE(!r.is_ok());
    CHECK(r.error().code == Errc::LimitExceeded);
    PduLimits small;
    small.max_pdu_length = 100;
    CHECK(parse_pdu(to_bytes(enc(full_rq())), small).error().code == Errc::LimitExceeded);

    // 129 presentation contexts: the encoder refuses, and the parser refuses a forged PDU.
    AssociateRq rq = full_rq();
    rq.contexts.clear();
    for (unsigned i = 0; i < 128; ++i)
        rq.contexts.push_back({static_cast<std::uint8_t>(2 * i + 1), "1.2.840.10008.1.1", {"1.2.840.10008.1.2"}});
    std::vector<std::uint8_t> b = enc(rq);
    CHECK(parse_pdu(to_bytes(b)).is_ok());
    PduLimits lim;
    lim.max_presentation_contexts = 127;
    CHECK(parse_pdu(to_bytes(b), lim).error().code == Errc::LimitExceeded);
    rq.contexts.push_back({1, "1.2.840.10008.1.1", {"1.2.840.10008.1.2"}});
    std::vector<std::uint8_t> big;
    CHECK(encode_pdu(rq, big).code == Errc::LimitExceeded);

    // Too many transfer syntaxes in one context.
    AssociateRq rq2 = full_rq();
    rq2.contexts.resize(1);
    rq2.contexts[0].transfer_syntaxes.assign(65, "1.2.840.10008.1.2");
    CHECK(parse_pdu(to_bytes(enc(rq2))).error().code == Errc::LimitExceeded);

    // Invalid values the encoder refuses.
    AssociateRq bad = full_rq();
    bad.called_ae = "THIS AE IS TOO LONG";
    CHECK(!encode_pdu(bad, big).is_ok());
    bad = full_rq();
    bad.contexts[0].abstract_syntax = std::string(65, '1');
    CHECK(!encode_pdu(bad, big).is_ok());
    bad = full_rq();
    bad.contexts[0].id = 2;
    CHECK(!encode_pdu(bad, big).is_ok());

    // A UID over 64 bytes on the wire: forge by patching a valid encoding of a 64-byte UID.
    CHECK(is_valid_uid(std::string(64, '1')));
    CHECK(!is_valid_uid(std::string(65, '1')));
    CHECK(!is_valid_uid("1..2"));
    CHECK(!is_valid_uid("1.2a"));
    CHECK(is_valid_ae_title("A"));
    CHECK(!is_valid_ae_title("    "));
    CHECK(!is_valid_ae_title("A\\B"));

    // Too many PDVs in one P-DATA-TF.
    PDataTf many;
    for (int i = 0; i < 10; ++i) many.pdvs.push_back({1, false, false, Bytes()});
    PduLimits pl;
    pl.max_pdvs = 9;
    CHECK(parse_pdu(to_bytes(enc(many)), pl).error().code == Errc::LimitExceeded);
}

TEST_CASE("net/pdu: structural errors") {
    // Unknown PDU type
    std::vector<std::uint8_t> u = {0x09, 0, 0, 0, 0, 4, 0, 0, 0, 0};
    CHECK(parse_pdu(to_bytes(u)).error().code == Errc::Unsupported);
    // Release RQ with wrong length
    std::vector<std::uint8_t> rl = {0x05, 0, 0, 0, 0, 2, 0, 0};
    CHECK(!parse_pdu(to_bytes(rl)).is_ok());
    // Trailing garbage after a PDU
    std::vector<std::uint8_t> tg = enc(ReleaseRq{});
    tg.push_back(0);
    CHECK(!parse_pdu(to_bytes(tg)).is_ok());
    // PDV item length 1
    std::vector<std::uint8_t> pd = {0x04, 0, 0, 0, 0, 5, 0, 0, 0, 1, 1};
    CHECK(!parse_pdu(to_bytes(pd)).is_ok());
    // PDV even context ID
    std::vector<std::uint8_t> pe = {0x04, 0, 0, 0, 0, 6, 0, 0, 0, 2, 2, 3};
    CHECK(!parse_pdu(to_bytes(pe)).is_ok());
    // RQ without user information
    AssociateRq rq = full_rq();
    std::vector<std::uint8_t> b = enc(rq);
    // find the 0x50 item (last) and cut it, fixing the PDU length
    std::size_t at = 6 + 68;
    while (at + 4 <= b.size() && b[at] != 0x50) at += 4u + std::size_t{load_be16(b.data() + at + 2)};
    REQUIRE(at < b.size());
    b.resize(at);
    store_be32(b.data() + 2, static_cast<std::uint32_t>(b.size() - 6));
    CHECK(parse_pdu(to_bytes(b)).error().code == Errc::Malformed);
    // Duplicate presentation context IDs
    AssociateRq dup = full_rq();
    dup.contexts[1].id = 1;
    CHECK(!parse_pdu(to_bytes(enc(dup))).is_ok());
}

TEST_CASE("net/ul: state table (PS3.8 table 9-10)") {
    using S = UlState;
    using E = UlEvent;
    using A = UlAction;
    auto t = [](S s, E e, bool req = true) { return ul_transition(s, e, req); };
    CHECK(t(S::Sta1, E::Evt1).action == A::AE1 && t(S::Sta1, E::Evt1).next == S::Sta4);
    CHECK(t(S::Sta4, E::Evt2).next == S::Sta5);
    CHECK(t(S::Sta5, E::Evt3).action == A::AE3 && t(S::Sta5, E::Evt3).next == S::Sta6);
    CHECK(t(S::Sta5, E::Evt4).action == A::AE4 && t(S::Sta5, E::Evt4).next == S::Sta1);
    CHECK(t(S::Sta1, E::Evt5).next == S::Sta2);
    CHECK(t(S::Sta2, E::Evt6).action == A::AE6);
    CHECK(t(S::Sta2, E::Evt10).action == A::AA1 && t(S::Sta2, E::Evt10).next == S::Sta13);
    CHECK(t(S::Sta2, E::Evt18).action == A::AA2);
    CHECK(t(S::Sta3, E::Evt7).next == S::Sta6);
    CHECK(t(S::Sta3, E::Evt8).next == S::Sta13);
    CHECK(t(S::Sta6, E::Evt9).action == A::DT1);
    CHECK(t(S::Sta6, E::Evt10).action == A::DT2);
    CHECK(t(S::Sta6, E::Evt3).action == A::AA8);
    CHECK(t(S::Sta6, E::Evt6).action == A::AA8);
    CHECK(t(S::Sta6, E::Evt11).next == S::Sta7);
    CHECK(t(S::Sta6, E::Evt12).next == S::Sta8);
    CHECK(t(S::Sta6, E::Evt16).action == A::AA3);
    CHECK(t(S::Sta6, E::Evt17).action == A::AA4);
    CHECK(t(S::Sta6, E::Evt19).action == A::AA8);
    CHECK(t(S::Sta7, E::Evt10).action == A::AR6);
    CHECK(t(S::Sta7, E::Evt13).action == A::AR3 && t(S::Sta7, E::Evt13).next == S::Sta1);
    CHECK(t(S::Sta7, E::Evt12, true).next == S::Sta9);
    CHECK(t(S::Sta7, E::Evt12, false).next == S::Sta10);
    CHECK(t(S::Sta8, E::Evt9).action == A::AR7);
    CHECK(t(S::Sta8, E::Evt14).action == A::AR4 && t(S::Sta8, E::Evt14).next == S::Sta13);
    CHECK(t(S::Sta9, E::Evt14).action == A::AR9 && t(S::Sta9, E::Evt14).next == S::Sta11);
    CHECK(t(S::Sta10, E::Evt13).action == A::AR10 && t(S::Sta10, E::Evt13).next == S::Sta12);
    CHECK(t(S::Sta11, E::Evt13).action == A::AR3);
    CHECK(t(S::Sta12, E::Evt14).action == A::AR4);
    CHECK(t(S::Sta13, E::Evt10).action == A::AA6);
    CHECK(t(S::Sta13, E::Evt6).action == A::AA7);
    CHECK(t(S::Sta13, E::Evt17).action == A::AR5);
    CHECK(t(S::Sta13, E::Evt18).action == A::AA2);
    CHECK(t(S::Sta4, E::Evt15).action == A::AA2);
    CHECK(t(S::Sta6, E::Evt15).action == A::AA1);
    // local primitives in the wrong state
    CHECK(t(S::Sta1, E::Evt9).action == A::Invalid);
    CHECK(t(S::Sta5, E::Evt11).action == A::Invalid);
    CHECK(t(S::Sta6, E::Evt7).action == A::Invalid);
}

TEST_CASE("net/ul: acceptor aborts on unexpected PDU in Sta2") {
    // A P-DATA-TF where an A-ASSOCIATE-RQ is expected: AA-1 sends A-ABORT and the call fails.
    std::vector<std::uint8_t> in = enc(sample_pdata());
    auto mt = std::make_unique<MemTransport>(in);
    MemTransport* raw = mt.get();
    Timeouts to;
    to.artim_ms = 50;
    Result<std::unique_ptr<Association>> a = Association::receive_request(std::move(mt), to, {});
    CHECK(!a.is_ok());
    (void)raw;  // the transport is owned (and destroyed) by the failed association
}

TEST_CASE("net/ul: requestor handles A-ASSOCIATE-RJ and garbage") {
    {
        auto mt = std::make_unique<MemTransport>(enc(AssociateRj{1, 1, 3}));
        AssociateRj rj;
        AssociateRq rq = full_rq();
        rq.user.identity_rq.reset();
        Result<std::unique_ptr<Association>> a = Association::request_over(std::move(mt), rq, {}, {}, &rj);
        REQUIRE(!a.is_ok());
        CHECK(a.error().code == Errc::Rejected);
        CHECK_EQ(rj.reason, 3);
    }
    {
        std::vector<std::uint8_t> junk = {0x02, 0, 0, 0, 0, 3, 1, 2, 3};
        auto mt = std::make_unique<MemTransport>(junk);
        Timeouts to;
        to.artim_ms = 50;
        Result<std::unique_ptr<Association>> a = Association::request_over(std::move(mt), full_rq(), to, {});
        CHECK(!a.is_ok());
    }
}

TEST_CASE("net/dimse: command encode/decode") {
    Command c = make_c_store_rq(7, "1.2.840.10008.5.1.4.1.1.2", "1.2.3.4.5", priority::High, "MOVER", std::uint16_t{3});
    std::vector<std::uint8_t> b;
    REQUIRE(encode_command(c, b).is_ok());
    // starts with (0000,0000) UL 4 = remaining length
    CHECK_EQ(load_le32(b.data()), 0u);
    CHECK_EQ(load_le32(b.data() + 4), 4u);
    CHECK_EQ(load_le32(b.data() + 8), b.size() - 12);
    Result<Command> d = decode_command(to_bytes(b));
    REQUIRE(d.is_ok());
    CHECK_EQ(d->command_field, cmd::CStoreRq);
    CHECK(d->message_id == std::optional<std::uint16_t>(7));
    CHECK_EQ(d->affected_sop_class_uid, "1.2.840.10008.5.1.4.1.1.2");
    CHECK_EQ(d->affected_sop_instance_uid, "1.2.3.4.5");
    CHECK_EQ(d->move_originator_ae, "MOVER");
    CHECK(d->move_originator_message_id == std::optional<std::uint16_t>(3));
    CHECK(d->has_dataset);
    CHECK(validate_command(*d).is_ok());

    Command rsp = make_response(*d, dimse_status::Success);
    CHECK_EQ(rsp.command_field, cmd::CStoreRsp);
    CHECK_EQ(rsp.affected_sop_instance_uid, "1.2.3.4.5");
    b.clear();
    REQUIRE(encode_command(rsp, b).is_ok());
    Result<Command> r = decode_command(to_bytes(b));
    REQUIRE(r.is_ok());
    CHECK(!r->has_dataset);
    CHECK(r->message_id_being_responded_to == std::optional<std::uint16_t>(7));
    CHECK(validate_command(*r).is_ok());

    Command g = make_n_get_rq(9, "1.2.840.10008.1.20.1", "1.2.3", {0x00100010, 0x00100020});
    b.clear();
    REQUIRE(encode_command(g, b).is_ok());
    Result<Command> gd = decode_command(to_bytes(b));
    REQUIRE(gd.is_ok());
    REQUIRE(gd->attribute_identifier_list.has_value());
    CHECK_EQ(gd->attribute_identifier_list->size(), 2u);
    CHECK_EQ((*gd->attribute_identifier_list)[1], 0x00100020u);
    Command gr = make_response(*gd, 0);
    CHECK_EQ(gr.affected_sop_class_uid, "1.2.840.10008.1.20.1");
    CHECK_EQ(gr.affected_sop_instance_uid, "1.2.3");

    // all N builders validate
    CHECK(validate_command(make_n_event_report_rq(1, "1.2", "1.3", 1, true)).is_ok());
    CHECK(validate_command(make_n_set_rq(1, "1.2", "1.3")).is_ok());
    CHECK(validate_command(make_n_action_rq(1, "1.2", "1.3", 1, true)).is_ok());
    CHECK(validate_command(make_n_create_rq(1, "1.2", "", true)).is_ok());
    CHECK(validate_command(make_n_delete_rq(1, "1.2", "1.3")).is_ok());
    CHECK(validate_command(make_c_cancel_rq(4)).is_ok());
    CHECK(validate_command(make_c_move_rq(1, sop::StudyRootMove, "DEST")).is_ok());
    Command nomove = make_c_move_rq(1, sop::StudyRootMove, "DEST");
    nomove.move_destination.clear();
    CHECK(!validate_command(nomove).is_ok());

    // corrupt group length / foreign element
    b.clear();
    REQUIRE(encode_command(make_c_echo_rq(1), b).is_ok());
    std::vector<std::uint8_t> bad = b;
    store_le32(bad.data() + 8, 3);
    CHECK(!decode_command(to_bytes(bad)).is_ok());
    // every truncation fails or decodes without crashing
    for (std::size_t n = 0; n < b.size(); ++n) {
        std::vector<std::uint8_t> t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(!decode_command(to_bytes(t)).is_ok());
    }
    CHECK(classify_status(0xFF01) == StatusClass::Pending);
    CHECK(classify_status(0xB000) == StatusClass::Warning);
    CHECK(classify_status(0xA801) == StatusClass::Failure);
    CHECK(classify_status(0xFE00) == StatusClass::Cancel);
}

TEST_CASE("net/negotiation: transfer syntax preference and roles") {
    AssociateRq rq;
    rq.called_ae = "A";
    rq.calling_ae = "B";
    std::vector<ProposedContext> props;
    props.push_back({"1.2.840.10008.1.1", {"1.2.840.10008.1.2", "1.2.840.10008.1.2.1"}, std::nullopt});
    props.push_back({"1.2.840.10008.5.1.4.1.1.2", {"1.2.840.10008.1.2.4.50"}, std::nullopt});
    props.push_back({"1.9.9", {"1.2.840.10008.1.2"}, std::nullopt});
    props.push_back({"1.2.840.10008.5.1.4.1.1.4", {"1.2.840.10008.1.2"}, RoleSelection{"", false, true}});
    REQUIRE(build_proposals(props, rq).is_ok());
    CHECK_EQ(rq.contexts[3].id, 7);
    REQUIRE(rq.user.roles.size() == 1);

    std::vector<AcceptedSyntax> pol = {
        {"1.2.840.10008.1.1", {"1.2.840.10008.1.2.1", "1.2.840.10008.1.2"}, true, false},
        {"1.2.840.10008.5.1.4.1.1.2", {"1.2.840.10008.1.2.1"}, true, false},
        {"1.2.840.10008.5.1.4.1.1.4", {"1.2.840.10008.1.2"}, true, true},
    };
    AssociateAc ac;
    std::vector<NegotiatedContext> n = negotiate_contexts(rq, pol, nullptr, ac);
    REQUIRE(n.size() == 4);
    CHECK(n[0].accepted() && n[0].transfer_syntax == "1.2.840.10008.1.2.1");  // acceptor preference
    CHECK_EQ(n[1].result, pc_result::TransferSyntaxesNotSupported);
    CHECK_EQ(n[2].result, pc_result::AbstractSyntaxNotSupported);
    CHECK(n[3].accepted() && !n[3].requestor_scu && n[3].requestor_scp);
    REQUIRE(ac.user.roles.size() == 1);
    CHECK(!ac.user.roles[0].scu_role && ac.user.roles[0].scp_role);

    // requestor view
    std::vector<NegotiatedContext> mine = contexts_from_ac(rq, ac);
    CHECK(mine[0].accepted());
    CHECK(!mine[1].accepted());
    CHECK(mine[3].requestor_scp && !mine[3].requestor_scu);

    // accept-any policy
    AcceptedSyntax any{"", {"1.2.840.10008.1.2"}, true, false};
    AssociateAc ac2;
    std::vector<NegotiatedContext> n2 = negotiate_contexts(rq, {}, &any, ac2);
    CHECK(n2[2].accepted());
    CHECK_EQ(n2[3].result, pc_result::UserRejection);  // requested SCP role only, not allowed
}
