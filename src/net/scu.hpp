// SIAARA DICOM Toolkit - high-level service class user (C-ECHO, C-STORE, C-FIND, C-MOVE, C-GET,
// generic N-services) over one association. Services as defined in DICOM PS3.4, messages PS3.7.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codec/transcode.hpp"
#include "core/dataset.hpp"
#include "core/status.hpp"
#include "net/association.hpp"
#include "net/dimse.hpp"
#include "net/tls.hpp"
#include "net/spool.hpp"

namespace siaara::net {

// An instance received through C-STORE (by an SCP, or by an SCU during C-GET).
struct StoreRequest {
    std::string calling_ae;
    std::string called_ae;
    std::string peer_address;  // the requestor's network address (SCP side; empty for C-GET)
    std::string sop_class_uid;
    std::string sop_instance_uid;
    std::string transfer_syntax;
    std::string move_originator_ae;
    std::optional<std::uint16_t> move_originator_message_id;
    Bytes raw;        // the data set exactly as received, encoded in `transfer_syntax`
    Dataset dataset;  // parsed from `raw`
    // Set when the data set was received to disk (net/spool.hpp): a complete Part 10 file whose
    // mapping `raw` and `dataset` view. Move it into place with spool->commit(path).
    std::shared_ptr<SpoolFile> spool;
    // The command's Affected SOP Class / Instance UIDs differed from (0008,0016) / (0008,0018) of
    // the data set (R-073) and the request was accepted leniently: sop_class_uid and
    // sop_instance_uid are the data set's (used for file names, file meta and indexes), these are
    // the command's. A spooled file (whose meta was written from the command) is not offered then.
    bool uid_mismatch = false;
    std::string command_sop_class_uid, command_sop_instance_uid;
    // Set when the receiver converted the instance lossily and that gave it a new SOP Instance
    // UID (now in sop_instance_uid and the data set): the UID it arrived with (R-093).
    std::string original_sop_instance_uid;
};

// What Scu::store sent (R-093): a lossy transcode on the way gives the instance a new SOP
// Instance UID, which the C-STORE command carries; the caller learns the mapping here.
struct StoreSent {
    std::string sop_instance_uid;           // as sent (command and data set)
    std::string original_sop_instance_uid;  // the file's own; differs only when uid_changed
    std::string transfer_syntax;            // of the presentation context used
    bool uid_changed = false;
    bool lossy = false;                     // this transfer applied lossy compression
    bool incomplete = false;                // refused: the file was not read completely (allow_incomplete)
    // What the lossy transcode did with the Ratio / Method history lists (codec/transcode.hpp).
    codec::LossyHistory lossy_history = codec::LossyHistory::NotApplicable;
};

// Compares the command's Affected SOP Class / Instance UIDs (already in rq.sop_class_uid /
// rq.sop_instance_uid) with the decoded data set (R-073). Equal: Success. Otherwise strict mode
// answers DataSetDoesNotMatch (0xA900); lenient mode switches rq to the data set UIDs (where the
// data set has them), keeps the command's in command_sop_*, sets uid_mismatch and returns Success.
// `note` receives a description of the difference.
std::uint16_t check_store_uids(StoreRequest& rq, bool strict, std::string& note);

// Builds a Part 10 file (file meta + data set) from a received instance.
DicomFile to_part10(const StoreRequest& rq);
// Encodes a received instance as a Part 10 stream whose data set is the received bytes, unchanged.
Status encode_received_part10(const StoreRequest& rq, std::vector<std::uint8_t>& out);

struct RetrieveProgress {
    std::uint16_t status = 0;
    std::uint16_t remaining = 0;
    std::uint16_t completed = 0;
    std::uint16_t failed = 0;
    std::uint16_t warning = 0;
};
using RetrieveResult = RetrieveProgress;

struct FindResult {
    std::uint16_t status = 0;
    std::size_t matches = 0;
    bool cancel_sent = false;
};

struct ScuConfig {
    std::string calling_ae = "SIAARA_SCU";
    std::string called_ae = "ANY-SCP";
    std::string host = "127.0.0.1";
    std::uint16_t port = 104;
    Timeouts timeouts;
    AssociationLimits limits;
    std::optional<UserIdentityRq> identity;
    // TLS (PS3.15 B.12/B.13) when tls.enabled: connect() then never falls back to plain TCP.
    TlsSettings tls;
    // ---- transcoding, streaming and negotiation extras
    // C-STORE: convert an instance when the SCP accepted only other transfer syntaxes
    // (net/store_support.hpp). Lossy conversion only when transcode_params.allow_lossy.
    bool transcode = true;
    codec::TranscodeParams transcode_params = [] {
        codec::TranscodeParams p;
        p.allow_lossy = false;
        return p;
    }();
    // C-STORE of a DicomFile whose read was not complete (truncated, bytes skipped, header-only):
    // refused with Errc::Rejected ("INCOMPLETE_INPUT", net/store_support.hpp check_sendable) unless set.
    bool allow_incomplete = false;
    std::optional<SpoolConfig> spool;                       // C-GET: receive large instances to disk
    // C-GET sub-operations whose command UIDs differ from the data set's: refuse (0xA900) instead of
    // accepting them under the data set's UIDs with StoreRequest::uid_mismatch (R-073).
    bool strict_store_uids = false;
    std::optional<AsyncWindow> async_window;                // propose an Asynchronous Operations Window
    std::vector<ExtendedNegotiation> extended_negotiation;  // SOP Class Extended Negotiation items
};

class Scu {
public:
    explicit Scu(ScuConfig cfg);
    ~Scu();
    Scu(const Scu&) = delete;
    Scu& operator=(const Scu&) = delete;

    // ---- proposals (before connect)
    void add_context(std::string abstract_syntax, std::vector<std::string> transfer_syntaxes,
                     std::optional<RoleSelection> role = std::nullopt);
    // Verification with the native transfer syntaxes.
    void add_verification();
    // Proposes a storage SOP class with the SCP role for this SCU, as C-GET needs (PS3.4 C.4.3.1).
    void add_get_storage(std::string sop_class, std::vector<std::string> transfer_syntaxes = native_transfer_syntaxes());
    // Proposes one context per (SOP class, transfer syntax of the files) found in the files' headers.
    Status add_contexts_for_files(const std::vector<std::string>& paths);
    const std::vector<ProposedContext>& proposals() const noexcept { return proposals_; }

    // ---- association
    Status connect(AssociateRj* rj = nullptr);
    // Uses an already connected transport (TLS later); otherwise as connect().
    Status connect_over(std::unique_ptr<Transport> t, AssociateRj* rj = nullptr);
    bool connected() const noexcept { return assoc_ && assoc_->established(); }
    Association* association() noexcept { return assoc_.get(); }
    Status release();
    void abort();

    // ---- services. The returned value is the final DIMSE status of the operation.
    Result<std::uint16_t> echo();
    // `sent` (may be null) receives the UID and transfer syntax used (R-093).
    Result<std::uint16_t> store(const DicomFile& file, std::string_view move_originator_ae = {},
                                std::optional<std::uint16_t> move_originator_message_id = std::nullopt,
                                StoreSent* sent = nullptr);
    Result<std::uint16_t> store_file(const std::string& path);
    // `on_result` is called for every pending response; returning false sends C-CANCEL.
    Result<FindResult> find(std::string_view sop_class, const Dataset& identifier,
                            const std::function<bool(const Dataset&)>& on_result);
    // `on_progress` (optional) sees every pending response; returning false sends C-CANCEL.
    Result<RetrieveResult> move(std::string_view sop_class, std::string_view destination_ae, const Dataset& identifier,
                                const std::function<bool(const RetrieveProgress&)>& on_progress = {});
    // `on_instance` receives each C-STORE sub-operation and returns its status.
    Result<RetrieveResult> get(std::string_view sop_class, const Dataset& identifier,
                               const std::function<std::uint16_t(const StoreRequest&)>& on_instance,
                               const std::function<bool(const RetrieveProgress&)>& on_progress = {});

    // Generic N-service request (N-EVENT-REPORT/N-GET/N-SET/N-ACTION/N-CREATE/N-DELETE).
    // The Message ID is assigned here; `rq.has_dataset` decides whether `ds` is sent.
    struct NResponse {
        Command command;
        Dataset dataset;
    };
    Result<NResponse> n_request(Command rq, const Dataset* ds = nullptr);

    const ScuConfig& config() const noexcept { return cfg_; }

private:
    Result<Message> wait_response(std::uint16_t message_id, std::uint16_t response_field);
    Result<RetrieveResult> retrieve(bool is_get, std::string_view sop_class, std::string_view destination,
                                    const Dataset& identifier,
                                    const std::function<std::uint16_t(const StoreRequest&)>& on_instance,
                                    const std::function<bool(const RetrieveProgress&)>& on_progress);
    Status handle_get_store(const Message& m, const std::function<std::uint16_t(const StoreRequest&)>& on_instance);

    ScuConfig cfg_;
    std::vector<ProposedContext> proposals_;
    std::unique_ptr<Association> assoc_;
};

}  // namespace siaara::net
