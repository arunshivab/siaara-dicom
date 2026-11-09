// SIAARA DICOM Toolkit - C API: cancellation tokens, logging, DIMSE SCU calls and the Storage
// SCP (include/siaara/siaara.h, "viewer / PACS additions").
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "capi/capi_internal.hpp"
#include "core/io.hpp"
#include "core/reader.hpp"
#include "core/writer.hpp"
#include "net/dimse.hpp"
#include "net/scp.hpp"
#include "net/scu.hpp"
#include "net/services.hpp"
#include "net/store_support.hpp"

namespace {

using siaara::Errc;
using siaara::Status;
namespace net = siaara::net;
namespace st = siaara::store;
namespace capi = siaara::capi;

siaara_status fail(Errc c, std::string_view m) noexcept { return capi::set_error(c, m); }
siaara_status fail(const Status& s) noexcept { return capi::set_error(s); }
siaara_status ok() noexcept { return capi::clear_error(); }

template <class F>
siaara_status guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return fail(Errc::OutOfMemory, "out of memory");
    } catch (...) {
        return fail(Errc::Internal, "internal error");
    }
}

const siaara::Dataset* ds_cast(const siaara_dataset* p) { return reinterpret_cast<const siaara::Dataset*>(p); }

// ---------------------------------------------------------------- logging
struct LogState {
    std::shared_mutex mu;
    siaara_log_callback cb = nullptr;
    void* user = nullptr;
    std::atomic<int> min_level{SIAARA_LOG_ERROR + 1};
};
LogState& log_state() {
    static LogState s;
    return s;
}

// ---------------------------------------------------------------- DIMSE helpers
std::string hex4(unsigned v) {
    static const char* d = "0123456789ABCDEF";
    std::string s = "0x0000";
    for (int i = 0; i < 4; ++i) s[static_cast<std::size_t>(5 - i)] = d[(v >> (4 * i)) & 0xFu];
    return s;
}

void result_init(siaara_dimse_result* r) {
    if (!r) return;
    const uint32_t size = r->struct_size;
    std::memset(r, 0, sizeof(siaara_dimse_result));
    r->struct_size = size;
}

bool result_ok(const siaara_dimse_result* r) { return !r || r->struct_size >= sizeof(siaara_dimse_result); }

const std::vector<std::string>& compressed_syntaxes() {
    static const std::vector<std::string> v = {
        "1.2.840.10008.1.2.1.99", "1.2.840.10008.1.2.5",    "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51",
        "1.2.840.10008.1.2.4.57", "1.2.840.10008.1.2.4.70", "1.2.840.10008.1.2.4.80", "1.2.840.10008.1.2.4.81",
        "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.91", "1.2.840.10008.1.2.4.201", "1.2.840.10008.1.2.4.202",
        "1.2.840.10008.1.2.4.203"};
    return v;
}

bool ae_ok(const std::string& ae) {
    if (ae.empty() || ae.size() > 16) return false;
    if (ae.find_first_not_of(' ') == std::string::npos) return false;
    for (char c : ae)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E || c == '\\') return false;
    return true;
}

siaara_status peer_config(const siaara_dimse_peer* p, net::ScuConfig& c) {
    if (!p || p->struct_size < sizeof(siaara_dimse_peer))
        return fail(Errc::InvalidArgument, "peer must be set up with struct_size");
    if (!p->host || !p->host[0]) return fail(Errc::InvalidArgument, "peer->host must be set");
    if (p->port == 0 || p->port > 65535) return fail(Errc::InvalidArgument, "peer->port must be 1..65535");
    if (p->connect_timeout_ms < 0 || p->acse_timeout_ms < 0 || p->dimse_timeout_ms < 0)
        return fail(Errc::InvalidArgument, "timeouts must not be negative");
    c.host = p->host;
    c.port = static_cast<std::uint16_t>(p->port);
    if (p->calling_ae) c.calling_ae = p->calling_ae;
    if (p->called_ae) c.called_ae = p->called_ae;
    if (!ae_ok(c.calling_ae) || !ae_ok(c.called_ae))
        return fail(Errc::InvalidArgument, "AE titles: 1 to 16 printable ASCII characters, not all spaces, no backslash");
    if (p->connect_timeout_ms) c.timeouts.connect_ms = p->connect_timeout_ms;
    if (p->acse_timeout_ms) {
        c.timeouts.acse_ms = p->acse_timeout_ms;
        c.timeouts.artim_ms = p->acse_timeout_ms;
    }
    if (p->dimse_timeout_ms) {
        c.timeouts.dimse_ms = p->dimse_timeout_ms;
        c.timeouts.write_ms = p->dimse_timeout_ms;
    }
    if (p->max_pdu) {
        if (p->max_pdu < 4096) return fail(Errc::InvalidArgument, "max_pdu must be at least 4096");
        c.limits.max_receive_pdu = p->max_pdu;
        c.limits.pdu.max_pdu_length = std::max<std::uint64_t>(c.limits.pdu.max_pdu_length, p->max_pdu);
    }
    c.transcode = true;
    c.transcode_params.allow_lossy = p->allow_lossy_transcode != 0;
    if (p->tls) {
        if (p->tls->struct_size < sizeof(siaara_tls_options))
            return fail(Errc::InvalidArgument, "tls->struct_size is too small");
        c.tls = capi::tls_settings(p->tls);
        c.tls.handshake_timeout_ms = c.timeouts.connect_ms;
        if (capi::log_enabled(SIAARA_LOG_INFO)) c.tls.log = capi::log_sink(SIAARA_LOG_INFO);
    }
    return SIAARA_OK;
}

// Connects; maps an association rejection to a readable message.
siaara_status connect(net::Scu& scu, siaara_cancel* cancel, const net::ScuConfig& c) {
    if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
    net::AssociateRj rj;
    Status s = scu.connect(&rj);
    if (cancel && cancel->is_set()) return fail(Errc::Cancelled, "cancelled");
    if (!s) {
        if (s.code == Errc::Rejected) {
            const std::string m = "association rejected by " + c.called_ae + " (result " + std::to_string(rj.result) +
                                  ", source " + std::to_string(rj.source) + ", reason " + std::to_string(rj.reason) + ")";
            capi::log(SIAARA_LOG_WARNING, false, m);
            return fail(Errc::Rejected, m);
        }
        capi::log(SIAARA_LOG_WARNING, false, "association to " + c.called_ae + " at " + c.host + ": " + s.message);
        return fail(s);
    }
    capi::log(SIAARA_LOG_DEBUG, false, "association established: " + c.calling_ae + " -> " + c.called_ae + " at " + c.host);
    return SIAARA_OK;
}

void finish(net::Scu& scu, bool aborted) {
    if (!scu.connected()) return;
    if (aborted) {
        scu.abort();
        return;
    }
    if (Status s = scu.release(); !s) scu.abort();
}

// Interrupts the association of `scu` (only registered while connected; see CancelScope use).
std::function<void()> interrupter(net::Scu& scu) {
    return [&scu]() {
        if (net::Association* a = scu.association()) a->interrupt();
    };
}

// Error of a failed service call: cancellation wins.
siaara_status op_error(const Status& s, const capi::CancelScope& scope) {
    if (scope.cancelled()) return fail(Errc::Cancelled, "cancelled");
    return fail(s);
}

siaara_status dimse_outcome(std::uint16_t status, const char* what) {
    switch (net::classify_status(status)) {
        case net::StatusClass::Success:
        case net::StatusClass::Warning: return ok();
        case net::StatusClass::Cancel: return fail(Errc::Cancelled, std::string(what) + " cancelled (status 0xFE00)");
        default: break;
    }
    return fail(Errc::Rejected, std::string(what) + " failed with status " + hex4(status));
}

std::string_view model_find(int32_t m) {
    switch (m) {
        case SIAARA_QR_PATIENT_ROOT: return net::sop::PatientRootFind;
        case SIAARA_QR_STUDY_ROOT: return net::sop::StudyRootFind;
        case SIAARA_QR_WORKLIST: return net::sop::ModalityWorklistFind;
        default: return {};
    }
}
std::string_view model_move(int32_t m) {
    return m == SIAARA_QR_PATIENT_ROOT ? net::sop::PatientRootMove
           : m == SIAARA_QR_STUDY_ROOT ? net::sop::StudyRootMove
                                       : std::string_view();
}
std::string_view model_get(int32_t m) {
    return m == SIAARA_QR_PATIENT_ROOT ? net::sop::PatientRootGet
           : m == SIAARA_QR_STUDY_ROOT ? net::sop::StudyRootGet
                                       : std::string_view();
}

siaara::Bytes bytes_of(std::vector<std::uint8_t>&& v) {
    auto owner = std::make_shared<std::vector<std::uint8_t>>(std::move(v));
    const std::uint8_t* d = owner->data();
    const std::size_t n = owner->size();
    return siaara::Bytes(std::move(owner), d, n);
}

bool uid_filename_ok(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return s.front() != '.' && s.back() != '.';
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

// Adds a received instance to a store and / or folder. Returns the DIMSE status to answer.
std::uint16_t keep_instance(const net::StoreRequest& rq, siaara_store* store, st::DuplicatePolicy policy,
                            const std::string& folder, int32_t* outcome, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (Status s = net::encode_received_part10(rq, bytes); !s) {
        error = s.message;
        return 0xC000;  // cannot understand
    }
    if (!folder.empty()) {
        if (!uid_filename_ok(rq.sop_instance_uid)) {
            error = "SOP Instance UID is not usable as a file name";
            return 0xC000;
        }
        const std::string path = join_path(folder, rq.sop_instance_uid + ".dcm");
        if (Status s = siaara::io::write_file_atomic(path, bytes.data(), bytes.size()); !s) {
            error = s.message;
            return 0xA700;  // out of resources
        }
    }
    if (store) {
        auto r = store->store->add_buffer(bytes_of(std::move(bytes)), policy);
        if (!r) {
            error = r.error().message;
            return 0xA700;
        }
        if (outcome) *outcome = static_cast<int32_t>(*r);
    }
    return net::dimse_status::Success;
}

void fill_progress(siaara_dimse_result& p, const net::RetrieveProgress& r) {
    p.status = r.status;
    p.remaining = r.remaining;
    p.completed = r.completed;
    p.failed = r.failed;
    p.warning = r.warning;
}

}  // namespace

// ---------------------------------------------------------------- internal logging API
namespace siaara::capi {

void log(int level, bool phi, std::string_view message) noexcept {
    LogState& s = log_state();
    if (level < s.min_level.load(std::memory_order_relaxed)) return;
    try {
        siaara_log_callback cb = nullptr;
        void* user = nullptr;
        {
            std::shared_lock<std::shared_mutex> l(s.mu);
            cb = s.cb;
            user = s.user;
        }
        if (!cb) return;
        const std::string text(message);
        cb(user, level, phi ? 1 : 0, text.c_str());
    } catch (...) {
        swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
    }
}

bool log_enabled(int level) noexcept { return level >= log_state().min_level.load(std::memory_order_relaxed); }

std::function<void(const std::string&)> log_sink(int level) {
    return [level](const std::string& m) { log(level, false, m); };
}

}  // namespace siaara::capi

// ---------------------------------------------------------------- the SCP handle
struct siaara_scp {
    std::unique_ptr<net::ScpServer> server;
    siaara_store* store = nullptr;
    st::DuplicatePolicy policy = st::DuplicatePolicy::Keep;
    siaara_scp_callback cb = nullptr;
    void* user = nullptr;
    std::mutex cb_mu;
    std::atomic<std::uint64_t> stored{0}, failures{0}, echoes{0}, bytes{0};
    siaara::capi::LiveHandle live_{};

    // Stops the server (joins its threads) before the members its handlers use go away.
    ~siaara_scp() {
        if (server) server->stop();
    }

    void event(int32_t kind, std::uint16_t status, int32_t outcome, const std::string& calling, const std::string& cls,
               const std::string& inst, const std::string& addr) {
        if (!cb) return;
        siaara_scp_event e{};
        e.struct_size = sizeof e;
        e.kind = kind;
        e.dimse_status = status;
        e.outcome = outcome;
        e.calling_ae = calling.c_str();
        e.sop_class_uid = cls.c_str();
        e.sop_instance_uid = inst.c_str();
        e.peer_address = addr.c_str();
        std::lock_guard<std::mutex> l(cb_mu);
        try {
            cb(user, &e);
        } catch (...) {
            siaara::capi::swallowed_exceptions().fetch_add(1, std::memory_order_relaxed);
        }
    }
};

extern "C" {

// ---------------------------------------------------------------- cancellation
SIAARA_API siaara_status SIAARA_CALL siaara_cancel_create(siaara_cancel** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = new siaara_cancel;
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_cancel_free(siaara_cancel* token) SIAARA_NOEXCEPT { delete token; }

SIAARA_API siaara_status SIAARA_CALL siaara_cancel_set(siaara_cancel* token) {
    return guarded([&]() -> siaara_status {
        if (!token) return fail(Errc::InvalidArgument, "token must not be NULL");
        token->set();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_cancel_reset(siaara_cancel* token) {
    return guarded([&]() -> siaara_status {
        if (!token) return fail(Errc::InvalidArgument, "token must not be NULL");
        token->flag.store(false, std::memory_order_release);
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_cancel_is_set(const siaara_cancel* token, int32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!token || !out) return fail(Errc::InvalidArgument, "token and out must not be NULL");
        *out = token->is_set() ? 1 : 0;
        return ok();
    });
}

// ---------------------------------------------------------------- logging
SIAARA_API siaara_status SIAARA_CALL siaara_set_log_callback(siaara_log_callback cb, void* user, int32_t min_level) {
    return guarded([&]() -> siaara_status {
        if (min_level < SIAARA_LOG_DEBUG || min_level > SIAARA_LOG_ERROR)
            return fail(Errc::InvalidArgument, "min_level must be SIAARA_LOG_DEBUG .. SIAARA_LOG_ERROR");
        LogState& s = log_state();
        std::unique_lock<std::shared_mutex> l(s.mu);
        s.cb = cb;
        s.user = user;
        s.min_level.store(cb ? min_level : SIAARA_LOG_ERROR + 1, std::memory_order_relaxed);
        return ok();
    });
}

// ---------------------------------------------------------------- SCU
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_echo(const siaara_dimse_peer* peer, siaara_cancel* cancel,
                                                       siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        net::Scu scu(c);
        scu.add_verification();
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        siaara_status out = SIAARA_OK;
        bool aborted = false;
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            auto r = scu.echo();
            if (!r) {
                out = op_error(r.error(), scope);
                aborted = true;
            } else {
                if (result) result->status = *r;
                out = dimse_outcome(*r, "C-ECHO");
            }
        }
        finish(scu, aborted);
        return out;
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_find(const siaara_dimse_peer* peer, int32_t model,
                                                       const siaara_dataset* identifier, siaara_cancel* cancel,
                                                       siaara_find_callback cb, void* user, siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        if (!identifier) return fail(Errc::InvalidArgument, "identifier must not be NULL");
        const std::string_view cls = model_find(model);
        if (cls.empty()) return fail(Errc::InvalidArgument, "unknown model");
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        net::Scu scu(c);
        scu.add_context(std::string(cls), net::native_transfer_syntaxes());
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        siaara_status out = SIAARA_OK;
        bool aborted = false, stopped = false;
        std::uint32_t matches = 0;
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            auto r = scu.find(cls, *ds_cast(identifier), [&](const siaara::Dataset& m) {
                ++matches;
                if (scope.cancelled()) return false;
                if (cb && cb(user, reinterpret_cast<const siaara_dataset*>(&m)) != 0) {
                    stopped = true;
                    return false;
                }
                return true;
            });
            if (result) result->matches = matches;
            if (!r) {
                out = op_error(r.error(), scope);
                aborted = true;
            } else {
                if (result) result->status = r->status;
                out = stopped || scope.cancelled() ? fail(Errc::Cancelled, "C-FIND cancelled")
                                                   : dimse_outcome(r->status, "C-FIND");
            }
        }
        finish(scu, aborted);
        return out;
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_move(const siaara_dimse_peer* peer, int32_t model,
                                                       const siaara_dataset* identifier, const char* destination_ae,
                                                       siaara_cancel* cancel, siaara_dimse_progress_callback cb,
                                                       void* user, siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        if (!identifier || !destination_ae) return fail(Errc::InvalidArgument, "identifier and destination_ae must be set");
        const std::string dest(destination_ae);
        if (!ae_ok(dest)) return fail(Errc::InvalidArgument, "destination_ae is not a valid AE title");
        const std::string_view cls = model_move(model);
        if (cls.empty()) return fail(Errc::InvalidArgument, "model must be SIAARA_QR_PATIENT_ROOT or SIAARA_QR_STUDY_ROOT");
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        net::Scu scu(c);
        scu.add_context(std::string(cls), net::native_transfer_syntaxes());
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        siaara_status out = SIAARA_OK;
        bool aborted = false, stopped = false;
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            siaara_dimse_result prog{};
            prog.struct_size = sizeof prog;
            auto r = scu.move(cls, dest, *ds_cast(identifier), [&](const net::RetrieveProgress& p) {
                if (scope.cancelled()) return false;
                fill_progress(prog, p);
                if (cb && cb(user, &prog) != 0) {
                    stopped = true;
                    return false;
                }
                return true;
            });
            if (!r) {
                out = op_error(r.error(), scope);
                aborted = true;
            } else {
                if (result) {
                    fill_progress(*result, *r);
                }
                out = stopped || scope.cancelled() ? fail(Errc::Cancelled, "C-MOVE cancelled")
                                                   : dimse_outcome(r->status, "C-MOVE");
                if (out == SIAARA_OK && r->failed) out = fail(Errc::Rejected, "C-MOVE: some sub-operations failed");
            }
        }
        finish(scu, aborted);
        return out;
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_get(const siaara_dimse_peer* peer, int32_t model,
                                                      const siaara_dataset* identifier, const siaara_get_options* options,
                                                      siaara_cancel* cancel, siaara_dimse_progress_callback cb, void* user,
                                                      siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        if (!identifier || !options || options->struct_size < sizeof(siaara_get_options))
            return fail(Errc::InvalidArgument, "identifier and options (with struct_size) must be set");
        if (!options->store && !options->folder) return fail(Errc::InvalidArgument, "options->store or options->folder is required");
        if (options->sop_class_count && !options->sop_classes) return fail(Errc::InvalidArgument, "sop_classes is NULL");
        if (options->duplicate_policy != SIAARA_DUP_KEEP && options->duplicate_policy != SIAARA_DUP_REPLACE)
            return fail(Errc::InvalidArgument, "unknown duplicate_policy");
        const std::string_view cls = model_get(model);
        if (cls.empty()) return fail(Errc::InvalidArgument, "model must be SIAARA_QR_PATIENT_ROOT or SIAARA_QR_STUDY_ROOT");
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        std::vector<std::string> classes;
        if (options->sop_classes) {
            for (uint32_t i = 0; i < options->sop_class_count; ++i) {
                if (!options->sop_classes[i]) return fail(Errc::InvalidArgument, "sop_classes entry is NULL");
                classes.emplace_back(options->sop_classes[i]);
            }
        } else {
            classes = net::storage_sop_classes();
        }
        if (classes.size() > 120) return fail(Errc::LimitExceeded, "at most 120 storage SOP classes can be proposed");
        std::vector<std::string> ts = net::native_transfer_syntaxes();
        if (!options->native_only) ts.insert(ts.end(), compressed_syntaxes().begin(), compressed_syntaxes().end());
        net::Scu scu(c);
        scu.add_context(std::string(cls), net::native_transfer_syntaxes());
        for (const std::string& k : classes) scu.add_get_storage(k, ts);
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        const st::DuplicatePolicy policy =
            options->duplicate_policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep;
        const std::string folder = options->folder ? options->folder : "";
        siaara_status out = SIAARA_OK;
        bool aborted = false, stopped = false;
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            siaara_dimse_result prog{};
            prog.struct_size = sizeof prog;
            auto r = scu.get(
                cls, *ds_cast(identifier),
                [&](const net::StoreRequest& rq) -> std::uint16_t {
                    if (scope.cancelled()) return 0xA700;
                    std::string err;
                    const std::uint16_t s = keep_instance(rq, options->store, policy, folder, nullptr, err);
                    if (s != net::dimse_status::Success)
                        capi::log(SIAARA_LOG_WARNING, true, "C-GET: instance " + rq.sop_instance_uid + " not kept: " + err);
                    return s;
                },
                [&](const net::RetrieveProgress& p) {
                    if (scope.cancelled()) return false;
                    fill_progress(prog, p);
                    if (cb && cb(user, &prog) != 0) {
                        stopped = true;
                        return false;
                    }
                    return true;
                });
            if (!r) {
                out = op_error(r.error(), scope);
                aborted = true;
            } else {
                if (result) fill_progress(*result, *r);
                out = stopped || scope.cancelled() ? fail(Errc::Cancelled, "C-GET cancelled")
                                                   : dimse_outcome(r->status, "C-GET");
                if (out == SIAARA_OK && r->failed) out = fail(Errc::Rejected, "C-GET: some sub-operations failed");
            }
        }
        finish(scu, aborted);
        return out;
    });
}

extern "C++" {  // internal helpers inside the extern "C" block keep C++ linkage
namespace {

// Contexts for C-STORE of the given headers: as found, plus uncompressed ones per SOP class so
// that store() can transcode.
siaara_status propose_store(net::Scu& scu, const std::vector<std::pair<std::string, std::string>>& class_ts) {
    std::set<std::pair<std::string, std::string>> seen;
    std::set<std::string> classes;
    for (const auto& [cls, ts] : class_ts) {
        if (cls.empty()) continue;
        if (seen.insert({cls, ts}).second) {
            std::vector<std::string> list{ts};
            scu.add_context(cls, std::move(list));
        }
        classes.insert(cls);
    }
    for (const std::string& cls : classes) {
        bool have = false;
        for (const auto& [c2, t2] : seen)
            if (c2 == cls && t2 == siaara::ts::ExplicitVRLittleEndian) have = true;
        if (!have) scu.add_context(cls, net::native_transfer_syntaxes());
    }
    if (scu.proposals().size() > 128) return fail(Errc::LimitExceeded, "more than 128 presentation contexts needed: store in smaller batches");
    if (scu.proposals().empty()) return fail(Errc::InvalidArgument, "nothing to store");
    return SIAARA_OK;
}

std::string sop_class_of(const siaara::DicomFile& f) {
    auto v = f.dataset.get_string(siaara::tags::SOPClassUID);
    if (!v) v = f.meta.get_string(siaara::tags::MediaStorageSOPClassUID);
    return v.value_or(std::string());
}

}  // namespace
}  // extern "C++"

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_files(const siaara_dimse_peer* peer, const char* const* paths,
                                                              const size_t* lens, uint32_t count, siaara_cancel* cancel,
                                                              siaara_dimse_progress_callback cb, void* user,
                                                              siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        if (!paths || count == 0) return fail(Errc::InvalidArgument, "no files");
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        std::vector<std::string> files;
        files.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!paths[i]) return fail(Errc::InvalidArgument, "path is NULL");
            files.emplace_back(paths[i], lens ? lens[i] : std::strlen(paths[i]));
        }
        // Headers first (SOP class and transfer syntax of each file).
        std::vector<std::pair<std::string, std::string>> class_ts(files.size());
        std::vector<Status> header_error(files.size());
        siaara::ReadOptions ro;
        ro.stop_before_pixel_data = true;
        for (std::size_t i = 0; i < files.size(); ++i) {
            auto h = siaara::read_file(files[i], ro);
            if (!h) {
                header_error[i] = h.error();
                continue;
            }
            class_ts[i] = {sop_class_of(*h), h->transfer_syntax};
            if (class_ts[i].first.empty()) header_error[i] = Status(Errc::Malformed, "file has no SOP Class UID");
        }
        net::Scu scu(c);
        if (siaara_status s = propose_store(scu, class_ts); s != SIAARA_OK) return s;
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        siaara_dimse_result prog{};
        prog.struct_size = sizeof prog;
        prog.remaining = count;
        bool aborted = false, stopped = false;
        Status transport = Status::ok();
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            for (std::size_t i = 0; i < files.size() && !stopped; ++i) {
                if (scope.cancelled()) break;
                std::uint16_t status = 0xC000;
                if (header_error[i]) {
                    auto f = siaara::read_file(files[i]);
                    if (!f) {
                        capi::log(SIAARA_LOG_WARNING, false, "C-STORE: input file " + std::to_string(i) + ": " + f.error().message);
                    } else {
                        auto r = scu.store(*f);
                        if (!r) {
                            if (!scu.connected() || scope.cancelled()) {
                                transport = r.error();
                                aborted = true;
                                break;
                            }
                            capi::log(SIAARA_LOG_WARNING, false, "C-STORE: input file " + std::to_string(i) + ": " + r.error().message);
                        } else {
                            status = *r;
                        }
                    }
                } else {
                    capi::log(SIAARA_LOG_WARNING, false, "C-STORE: input file " + std::to_string(i) + ": " + header_error[i].message);
                }
                prog.status = status;
                prog.current = static_cast<uint32_t>(i);
                prog.remaining = static_cast<uint32_t>(files.size() - i - 1);
                switch (net::classify_status(status)) {
                    case net::StatusClass::Success: ++prog.completed; break;
                    case net::StatusClass::Warning: ++prog.warning; break;
                    default: ++prog.failed; break;
                }
                if (cb && cb(user, &prog) != 0) stopped = true;
            }
            if (result) {
                const uint32_t size = result->struct_size;
                *result = prog;
                result->struct_size = size;
            }
            if (aborted || scope.cancelled()) {
                finish(scu, true);
                return scope.cancelled() ? fail(Errc::Cancelled, "C-STORE cancelled") : fail(transport);
            }
        }
        finish(scu, false);
        if (stopped) return fail(Errc::Cancelled, "C-STORE stopped by the callback");
        if (prog.failed) return fail(Errc::Rejected, std::to_string(prog.failed) + " of " + std::to_string(count) + " files were not stored");
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store(const siaara_dimse_peer* peer, const siaara_file* file,
                                                        siaara_cancel* cancel, siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        if (!file) return fail(Errc::InvalidArgument, "file must not be NULL");
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        const std::string cls = sop_class_of(file->file);
        if (cls.empty()) return fail(Errc::Malformed, "the file has no SOP Class UID");
        net::Scu scu(c);
        if (siaara_status s = propose_store(scu, {{cls, file->file.transfer_syntax}}); s != SIAARA_OK) return s;
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        siaara_status out = SIAARA_OK;
        bool aborted = false;
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            auto r = scu.store(file->file);
            if (!r) {
                out = op_error(r.error(), scope);
                aborted = !scu.connected() || scope.cancelled();
            } else {
                if (result) {
                    result->status = *r;
                    const auto k = net::classify_status(*r);
                    if (k == net::StatusClass::Success) result->completed = 1;
                    else if (k == net::StatusClass::Warning) result->warning = 1;
                    else result->failed = 1;
                }
                out = dimse_outcome(*r, "C-STORE");
            }
        }
        finish(scu, aborted);
        return out;
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_dimse_commit(const siaara_dimse_peer* peer, const char* const* sop_classes,
                                                         const char* const* sop_instances, uint32_t count, int32_t wait_ms,
                                                         siaara_cancel* cancel, uint16_t* failure_reasons,
                                                         siaara_dimse_result* result) {
    return guarded([&]() -> siaara_status {
        if (!result_ok(result)) return fail(Errc::InvalidArgument, "result must be set up with struct_size");
        result_init(result);
        if (count == 0 || !sop_classes || !sop_instances) return fail(Errc::InvalidArgument, "no instances");
        if (wait_ms < 0) return fail(Errc::InvalidArgument, "wait_ms must not be negative");
        std::vector<net::SopRef> items;
        items.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!sop_classes[i] || !sop_instances[i]) return fail(Errc::InvalidArgument, "UID is NULL");
            items.push_back({sop_classes[i], sop_instances[i]});
        }
        if (failure_reasons)
            for (uint32_t i = 0; i < count; ++i) failure_reasons[i] = 0xFFFF;
        net::ScuConfig c;
        if (siaara_status s = peer_config(peer, c); s != SIAARA_OK) return s;
        net::Scu scu(c);
        scu.add_context(std::string(net::sop::StorageCommitmentPushModel), net::native_transfer_syntaxes());
        if (siaara_status s = connect(scu, cancel, c); s != SIAARA_OK) return s;
        siaara_status out = SIAARA_OK;
        bool aborted = false;
        {
            capi::CancelScope scope(cancel, interrupter(scu));
            auto txn = net::request_commitment(scu, items);
            if (!txn) {
                out = op_error(txn.error(), scope);
                aborted = true;
            } else {
                auto res = net::wait_commitment_result(scu, wait_ms ? wait_ms : 60000);
                if (!res) {
                    out = scope.cancelled() ? fail(Errc::Cancelled, "cancelled")
                          : res.error().code == Errc::Timeout
                              ? fail(Errc::Timeout, "no storage commitment report arrived on the association")
                              : fail(res.error());
                    aborted = true;
                } else {
                    std::uint32_t committed = 0, failed = 0;
                    for (uint32_t i = 0; i < count; ++i) {
                        std::uint16_t reason = 0xFFFF;
                        for (const auto& x : res->committed)
                            if (x == items[i]) reason = 0;
                        for (const auto& f : res->failed)
                            if (f.ref == items[i]) reason = f.reason;
                        if (reason == 0) ++committed;
                        else ++failed;
                        if (failure_reasons) failure_reasons[i] = reason;
                    }
                    if (result) {
                        result->completed = committed;
                        result->failed = failed;
                        result->status = res->event_type();
                    }
                    out = failed ? fail(Errc::Rejected, std::to_string(failed) + " instances were not committed") : ok();
                }
            }
        }
        finish(scu, aborted);
        return out;
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_worklist_query(const char* station_ae, const char* modality, const char* date,
                                                           const char* patient_name, const char* patient_id,
                                                           siaara_file** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        auto s = [](const char* p) { return std::string_view(p ? p : ""); };
        auto f = std::make_unique<siaara_file>();
        f->file.transfer_syntax = std::string(siaara::ts::ExplicitVRLittleEndian);
        f->file.dataset = net::make_worklist_query(s(station_ae), s(modality), s(date), s(patient_name), s(patient_id));
        *out = f.release();
        return ok();
    });
}

// ---------------------------------------------------------------- Storage SCP
SIAARA_API siaara_status SIAARA_CALL siaara_scp_start(const siaara_scp_options* o, siaara_store* store,
                                                      siaara_scp_callback cb, void* user, siaara_scp** out) {
    return guarded([&]() -> siaara_status {
        if (!out) return fail(Errc::InvalidArgument, "out must not be NULL");
        *out = nullptr;
        if (!o || o->struct_size < sizeof(siaara_scp_options))
            return fail(Errc::InvalidArgument, "options must be set up with struct_size");
        if (!store) return fail(Errc::InvalidArgument, "store must not be NULL");
        if (o->port > 65535) return fail(Errc::InvalidArgument, "port must be 0..65535");
        if (o->allowed_count && !o->allowed_calling_aes) return fail(Errc::InvalidArgument, "allowed_calling_aes is NULL");
        if (o->duplicate_policy != SIAARA_DUP_KEEP && o->duplicate_policy != SIAARA_DUP_REPLACE)
            return fail(Errc::InvalidArgument, "unknown duplicate_policy");
        if (o->idle_timeout_ms < 0 || o->dimse_timeout_ms < 0) return fail(Errc::InvalidArgument, "timeouts must not be negative");
        net::ScpConfig cfg;
        if (o->ae_title) cfg.ae_title = o->ae_title;
        if (!ae_ok(cfg.ae_title)) return fail(Errc::InvalidArgument, "ae_title is not a valid AE title");
        if (o->bind_address) cfg.bind_address = o->bind_address;
        cfg.port = static_cast<std::uint16_t>(o->port);
        for (uint32_t i = 0; i < o->allowed_count; ++i) {
            if (!o->allowed_calling_aes[i]) return fail(Errc::InvalidArgument, "allowed_calling_aes entry is NULL");
            cfg.allowed_calling_aes.emplace_back(o->allowed_calling_aes[i]);
        }
        cfg.allow_any_calling_ae = o->allow_any_calling_ae != 0;
        if (cfg.allowed_calling_aes.empty() && !cfg.allow_any_calling_ae)
            return fail(Errc::InvalidArgument, "no allowed_calling_aes: list the peers, or set allow_any_calling_ae");
        if (o->max_associations) cfg.max_associations = o->max_associations;
        if (cfg.max_associations > 256) return fail(Errc::InvalidArgument, "max_associations must be at most 256");
        cfg.require_called_ae = o->accept_any_called_ae == 0;
        cfg.accept_any_sop_class = o->accept_any_sop_class != 0;
        cfg.transfer_syntaxes = net::native_transfer_syntaxes();
        if (o->accept_compressed)
            cfg.transfer_syntaxes.insert(cfg.transfer_syntaxes.end(), compressed_syntaxes().begin(), compressed_syntaxes().end());
        if (o->idle_timeout_ms) cfg.timeouts.idle_ms = o->idle_timeout_ms;
        if (o->dimse_timeout_ms) cfg.timeouts.dimse_ms = o->dimse_timeout_ms;
        if (o->max_pdu) {
            if (o->max_pdu < 4096) return fail(Errc::InvalidArgument, "max_pdu must be at least 4096");
            cfg.limits.max_receive_pdu = o->max_pdu;
            cfg.limits.pdu.max_pdu_length = std::max<std::uint64_t>(cfg.limits.pdu.max_pdu_length, o->max_pdu);
        }
        if (o->tls) {
            if (o->tls->struct_size < sizeof(siaara_tls_options)) return fail(Errc::InvalidArgument, "tls->struct_size is too small");
            cfg.tls = capi::tls_settings(o->tls);
        }
        auto h = std::make_unique<siaara_scp>();
        h->store = store;
        h->policy = o->duplicate_policy == SIAARA_DUP_REPLACE ? st::DuplicatePolicy::Replace : st::DuplicatePolicy::Keep;
        h->cb = cb;
        h->user = user;
        siaara_scp* raw = h.get();
        net::ScpHandlers hd;
        hd.on_echo = [raw](const std::string& calling) -> std::uint16_t {
            raw->echoes.fetch_add(1, std::memory_order_relaxed);
            raw->event(SIAARA_SCP_ECHO, net::dimse_status::Success, 0, calling, "", "", "");
            return net::dimse_status::Success;
        };
        hd.on_store = [raw](const net::StoreRequest& rq) -> std::uint16_t {
            std::string err;
            int32_t outcome = SIAARA_ADD_ADDED;
            const std::uint16_t s = keep_instance(rq, raw->store, raw->policy, std::string(), &outcome, err);
            if (s == net::dimse_status::Success) {
                raw->stored.fetch_add(1, std::memory_order_relaxed);
                raw->bytes.fetch_add(rq.raw.size(), std::memory_order_relaxed);
                raw->event(SIAARA_SCP_STORED, s, outcome, rq.calling_ae, rq.sop_class_uid, rq.sop_instance_uid, "");
            } else {
                raw->failures.fetch_add(1, std::memory_order_relaxed);
                capi::log(SIAARA_LOG_WARNING, true, "Storage SCP: instance " + rq.sop_instance_uid + " not stored: " + err);
                raw->event(SIAARA_SCP_STORE_FAILED, s, 0, rq.calling_ae, rq.sop_class_uid, rq.sop_instance_uid, "");
            }
            return s;
        };
        hd.log = capi::log_sink(SIAARA_LOG_INFO);
        if (cfg.tls.enabled) cfg.tls.log = capi::log_sink(SIAARA_LOG_INFO);
        h->server = std::make_unique<net::ScpServer>(std::move(cfg), std::move(hd));
        if (Status s = h->server->start(); !s) return fail(s);
        capi::log(SIAARA_LOG_INFO, false, "Storage SCP listening on port " + std::to_string(h->server->port()));
        *out = h.release();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_port(const siaara_scp* scp, uint32_t* out) {
    return guarded([&]() -> siaara_status {
        if (!scp || !out) return fail(Errc::InvalidArgument, "scp and out must not be NULL");
        *out = scp->server->port();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_stats_get(const siaara_scp* scp, siaara_scp_stats* out) {
    return guarded([&]() -> siaara_status {
        if (!scp || !out || out->struct_size < sizeof(siaara_scp_stats))
            return fail(Errc::InvalidArgument, "scp and out (with struct_size) must be set");
        out->reserved0 = 0;
        out->active_associations = scp->server->active_associations();
        out->associations_handled = scp->server->associations_handled();
        out->associations_rejected = scp->server->associations_rejected();
        out->instances_stored = scp->stored.load();
        out->store_failures = scp->failures.load();
        out->echoes = scp->echoes.load();
        out->bytes_received = scp->bytes.load();
        return ok();
    });
}

SIAARA_API siaara_status SIAARA_CALL siaara_scp_stop(siaara_scp* scp) {
    return guarded([&]() -> siaara_status {
        if (!scp) return fail(Errc::InvalidArgument, "scp must not be NULL");
        scp->server->stop();
        return ok();
    });
}

SIAARA_API void SIAARA_CALL siaara_scp_free(siaara_scp* scp) SIAARA_NOEXCEPT { delete scp; }

}  // extern "C"
