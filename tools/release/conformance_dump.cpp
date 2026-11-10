// SIAARA DICOM Toolkit - prints, as JSON, the facts of the built library that the generated
// tables of docs/CONFORMANCE.md are made of (tools/release/gen_conformance.py reads it).
// Not installed; built with the tests. Arguments: transfer syntax UIDs to describe.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "codec/transcode.hpp"
#include "core/incomplete_policy.hpp"
#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "dict/dictionary.hpp"
#include "net/association.hpp"
#include "net/dimse.hpp"
#include "net/pdu.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "net/store_support.hpp"
#include "web/http.hpp"
#include "web/server.hpp"

namespace {
using namespace siaara;

std::string q(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o + "\"";
}
std::string name_of(std::string_view uid) {
    const dict::UidEntry* e = dict::lookup_uid(uid);
    return e ? std::string(e->name) : std::string();
}
void uid_list(const char* key, const std::vector<std::string>& v, bool last = false) {
    std::printf("  %s: [", q(key).c_str());
    for (std::size_t i = 0; i < v.size(); ++i)
        std::printf("%s{\"uid\": %s, \"name\": %s}", i ? ", " : "", q(v[i]).c_str(), q(name_of(v[i])).c_str());
    std::printf("]%s\n", last ? "" : ",");
}
std::vector<std::string> sv(std::initializer_list<std::string_view> l) {
    std::vector<std::string> v;
    for (std::string_view s : l) v.emplace_back(s);
    return v;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace siaara::net;
    std::printf("{\n");
    std::printf("  \"implementation_class_uid\": %s,\n", q(kImplementationClassUid).c_str());
    std::printf("  \"implementation_version_name\": %s,\n", q(kImplementationVersionName).c_str());
    std::printf("  \"version\": %s,\n", q(SIAARA_VERSION_STRING).c_str());

    // Transfer syntaxes named on the command line.
    codec::TranscodeParams lossless;  // defaults: .81 / .91 / .203 coded losslessly
    std::printf("  \"transfer_syntaxes\": [\n");
    for (int i = 1; i < argc; ++i) {
        const std::string_view uid = argv[i];
        const TransferSyntaxInfo* t = find_registered_transfer_syntax(uid);
        if (!t) {
            std::fprintf(stderr, "unknown transfer syntax %s\n", argv[i]);
            return 1;
        }
        const bool readable = find_transfer_syntax(uid) != nullptr;
        std::printf("    {\"uid\": %s, \"name\": %s, \"dataset\": %s, \"encapsulated\": %s, \"lossy\": %s, \"retired\": %s, "
                    "\"decode\": %s, \"encode\": %s, \"encode_always_lossy\": %s}%s\n",
                    q(t->uid).c_str(), q(t->name).c_str(), readable ? "true" : "false",
                    t->encapsulated ? "true" : "false", t->lossy ? "true" : "false", t->retired ? "true" : "false",
                    readable && codec::can_decode(uid) ? "true" : "false", codec::can_encode(uid) ? "true" : "false",
                    codec::can_encode(uid) && codec::encoding_is_lossy(uid, lossless) ? "true" : "false",
                    i + 1 < argc ? "," : "");
    }
    std::printf("  ],\n");

    uid_list("native_transfer_syntaxes", native_transfer_syntaxes());
    codec::TranscodeParams p;
    p.allow_lossy = true;
    uid_list("transcode_targets_lossless", transcode_targets(p, false));
    uid_list("transcode_targets_lossy", transcode_targets(p, true));
    uid_list("storage_sop_classes", storage_sop_classes());
    uid_list("verification", sv({sop::Verification}));
    uid_list("qr_find", sv({sop::PatientRootFind, sop::StudyRootFind}));
    uid_list("qr_move", sv({sop::PatientRootMove, sop::StudyRootMove}));
    uid_list("qr_get", sv({sop::PatientRootGet, sop::StudyRootGet}));
    uid_list("composite_retrieve_move", sv({sop::CompositeInstanceRootMove}));
    uid_list("composite_retrieve_get",
             sv({sop::CompositeInstanceRootGet, sop::CompositeInstanceRetrieveWithoutBulkDataGet}));
    uid_list("worklist", sv({sop::ModalityWorklistFind}));
    uid_list("mpps", sv({sop::ModalityPerformedProcedureStep}));
    uid_list("storage_commitment", sv({sop::StorageCommitmentPushModel}));
    uid_list("ups", sv({sop::UpsPush, sop::UpsWatch, sop::UpsPull, sop::UpsEvent, sop::UpsQuery}));
    uid_list("instance_availability", sv({sop::InstanceAvailabilityNotification}));
    uid_list("print", sv({sop::BasicGrayscalePrintManagementMeta, sop::BasicColorPrintManagementMeta,
                          sop::BasicFilmSession, sop::BasicFilmBox, sop::BasicGrayscaleImageBox,
                          sop::BasicColorImageBox, sop::Printer}));

    const Timeouts t;
    const AssociationLimits l;
    const ScpConfig scp;
    const ScuConfig scu;
    const web::DicomwebServerConfig web;
    std::printf("  \"defaults\": {\n");
    std::printf("    \"application_context\": %s,\n", q(kDicomApplicationContext).c_str());
    std::printf("    \"scp_ae_title\": %s, \"scu_ae_title\": %s,\n", q(scp.ae_title).c_str(), q(scu.calling_ae).c_str());
    std::printf("    \"max_receive_pdu\": %u, \"max_pdu_cap\": %llu,\n", l.max_receive_pdu,
                static_cast<unsigned long long>(l.pdu.max_pdu_length));
    std::printf("    \"max_presentation_contexts\": %zu,\n", l.pdu.max_presentation_contexts);
    std::printf("    \"max_associations\": %zu, \"max_associations_per_ip\": %zu,\n", scp.max_associations,
                scp.max_associations_per_ip);
    std::printf("    \"max_association_lifetime_ms\": %d, \"max_operations_per_association\": %llu,\n",
                scp.max_association_lifetime_ms, static_cast<unsigned long long>(scp.max_operations_per_association));
    std::printf("    \"max_async_operations\": %u,\n", static_cast<unsigned>(scp.max_async_operations));
    std::printf("    \"connect_ms\": %d, \"acse_ms\": %d, \"dimse_ms\": %d, \"artim_ms\": %d, \"idle_ms\": %d, \"write_ms\": %d,\n",
                t.connect_ms, t.acse_ms, t.dimse_ms, t.artim_ms, t.idle_ms, t.write_ms);
    std::printf("    \"incomplete_policy_quarantine\": %s, \"incomplete_warning_status\": %u, \"incomplete_failure_status\": %u,\n",
                kDefaultIncompletePolicy == IncompletePolicy::Quarantine ? "true" : "false",
                static_cast<unsigned>(kQuarantineWarningStatus), static_cast<unsigned>(kIncompleteFailureStatus));
    std::printf("    \"network_overlong_recovery\": %s,\n", kRecoverOverlongLengthsNetwork ? "true" : "false");
    std::printf("    \"web_max_qido_results\": %zu, \"web_max_connections\": %zu\n", web.max_qido_results,
                web.http.max_connections);
    std::printf("  }\n}\n");
    return 0;
}
