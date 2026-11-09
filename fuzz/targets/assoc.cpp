// Fuzz target: the association state machine (PS3.8 9.2) fed bytes as if from a peer.
// First byte bit 0 selects the role:
//   acceptor  - wait for A-ASSOCIATE-RQ, negotiate against a storage/verification policy, accept,
//               then serve DIMSE messages (echo, store, find answered with a response) until the
//               peer releases, aborts or the bytes run out;
//   requestor - send an A-ASSOCIATE-RQ and interpret the peer's A-ASSOCIATE-AC/RJ, send a C-ECHO
//               and read responses, then release.
// Other bits pick the read chunk size. Timeouts are 0: the in-memory peer never blocks.
#include "fuzz.hpp"
#include "fuzz_util.hpp"
#include "net/association.hpp"
#include "net/dimse.hpp"

using namespace siaara;
using namespace siaara::net;

namespace {

Timeouts no_wait() {
    Timeouts t;
    t.connect_ms = t.acse_ms = t.dimse_ms = t.artim_ms = t.idle_ms = t.write_ms = 0;
    return t;
}

AssociationLimits small_limits() {
    AssociationLimits l;
    l.pdu.max_pdu_length = 1u << 20;
    l.max_receive_pdu = 16384;
    l.max_command_size = 1u << 16;
    l.max_dataset_size = 4u << 20;
    return l;
}

void serve(Association& a) {
    for (int i = 0; i < 32; ++i) {
        auto r = receive_dimse(a, 0);
        if (!r) return;
        if (r->kind == DimseKind::PeerReleaseRequest) {
            (void)a.respond_release();
            return;
        }
        Message& m = r->message;
        if (m.has_data) (void)decode_message_dataset(m);
        if (m.command.is_response()) continue;
        const Command rsp = make_response(m.command, 0x0000);
        if (!send_dimse(a, m.context_id, rsp)) return;
    }
    a.abort();
}

void acceptor(const std::uint8_t* d, std::size_t n, std::size_t chunk) {
    auto t = std::make_unique<siaara_fuzz::MemTransport>(d, n, chunk);
    auto a = Association::receive_request(std::move(t), no_wait(), small_limits());
    if (!a) return;
    std::vector<AcceptedSyntax> policy;
    policy.push_back({std::string(sop::Verification), native_transfer_syntaxes(), true, false});
    policy.push_back({"1.2.840.10008.5.1.4.1.1.2", native_transfer_syntaxes(), true, true});
    policy.push_back({std::string(sop::StudyRootFind), native_transfer_syntaxes(), true, false});
    AcceptedSyntax any{"", {"1.2.840.10008.1.2.1", "1.2.840.10008.1.2"}, true, false};
    AssociateAc ac;
    std::vector<NegotiatedContext> ctx = negotiate_contexts((*a)->request_pdu(), policy, (n & 2u) ? &any : nullptr, ac);
    if (n % 7 == 0) {
        AssociateRj rj;
        (*a)->reject(rj);
        return;
    }
    if (!(*a)->accept(ac, std::move(ctx))) return;
    serve(**a);
}

void requestor(const std::uint8_t* d, std::size_t n, std::size_t chunk) {
    AssociateRq rq;
    rq.calling_ae = "FUZZSCU";
    rq.called_ae = "FUZZSCP";
    std::vector<ProposedContext> pc;
    pc.push_back({std::string(sop::Verification), native_transfer_syntaxes(), std::nullopt});
    pc.push_back({"1.2.840.10008.5.1.4.1.1.2", native_transfer_syntaxes(), RoleSelection{"1.2.840.10008.5.1.4.1.1.2", true, true}});
    if (!build_proposals(pc, rq)) return;
    auto t = std::make_unique<siaara_fuzz::MemTransport>(d, n, chunk);
    AssociateRj rj;
    auto a = Association::request_over(std::move(t), rq, no_wait(), small_limits(), &rj);
    if (!a) return;
    if (const NegotiatedContext* c = (*a)->find_context(sop::Verification)) {
        if (send_dimse(**a, c->id, make_c_echo_rq((*a)->next_message_id()))) {
            for (int i = 0; i < 8; ++i) {
                auto r = receive_dimse(**a, 0);
                if (!r || r->kind == DimseKind::PeerReleaseRequest) break;
            }
        }
    }
    (void)(*a)->release();
}

}  // namespace

SIAARA_FUZZ_TARGET(assoc) {
    if (size < 1) return 0;
    const std::size_t chunk = 1 + ((data[0] >> 1) % 64u) * 37u;
    if (data[0] & 1u)
        requestor(data + 1, size - 1, chunk);
    else
        acceptor(data + 1, size - 1, chunk);
    return 0;
}
