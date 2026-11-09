// SIAARA DICOM Toolkit - Upper Layer PDU encoding and parsing (PS3.8 9.3, PS3.7 D.3.3).
#include "net/pdu.hpp"

#include <algorithm>
#include <vector>
#include <string>
#include <cstdint>
#include <type_traits>
#include <cstring>
#include <memory>

#include "net/socket.hpp"

namespace siaara::net {

namespace {

// Item types (PS3.8 9.3.2 and Annex D; PS3.7 D.3.3).
constexpr std::uint8_t kItemAppContext = 0x10;
constexpr std::uint8_t kItemPcRq = 0x20;
constexpr std::uint8_t kItemPcAc = 0x21;
constexpr std::uint8_t kItemAbstractSyntax = 0x30;
constexpr std::uint8_t kItemTransferSyntax = 0x40;
constexpr std::uint8_t kItemUserInfo = 0x50;
constexpr std::uint8_t kSubMaxLength = 0x51;
constexpr std::uint8_t kSubImplClassUid = 0x52;
constexpr std::uint8_t kSubAsyncWindow = 0x53;
constexpr std::uint8_t kSubRoleSelection = 0x54;
constexpr std::uint8_t kSubImplVersion = 0x55;
constexpr std::uint8_t kSubExtNeg = 0x56;
constexpr std::uint8_t kSubCommonExtNeg = 0x57;
constexpr std::uint8_t kSubIdentityRq = 0x58;
constexpr std::uint8_t kSubIdentityAc = 0x59;

constexpr std::size_t kMaxUid = 64;
constexpr std::size_t kAeLen = 16;

// ------------------------------------------------------------------ encoding

class Out {
public:
    explicit Out(std::vector<std::uint8_t>& v) : v_(v) {}
    void u8(std::uint8_t x) { v_.push_back(x); }
    void u16(std::uint16_t x) {
        std::uint8_t b[2];
        store_be16(b, x);
        v_.insert(v_.end(), b, b + 2);
    }
    void u32(std::uint32_t x) {
        std::uint8_t b[4];
        store_be32(b, x);
        v_.insert(v_.end(), b, b + 4);
    }
    void zeros(std::size_t n) { v_.insert(v_.end(), n, 0); }
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        v_.insert(v_.end(), b, b + n);
    }
    void str(std::string_view s) { raw(s.data(), s.size()); }
    std::size_t pos() const { return v_.size(); }
    // Patches a 16-bit length covering [body, end).
    Status patch16(std::size_t at, std::size_t body) {
        const std::size_t len = v_.size() - body;
        if (len > 0xFFFF) return Status(Errc::LimitExceeded, "item longer than 65535 bytes");
        store_be16(v_.data() + at, static_cast<std::uint16_t>(len));
        return Status::ok();
    }
    Status patch32(std::size_t at, std::size_t body) {
        const std::uint64_t len = v_.size() - body;
        if (len > 0xFFFFFFFFull) return Status(Errc::LimitExceeded, "PDU longer than 4 GiB");
        store_be32(v_.data() + at, static_cast<std::uint32_t>(len));
        return Status::ok();
    }

private:
    std::vector<std::uint8_t>& v_;
};

Status check_uid(std::string_view uid, const char* what) {
    if (!is_valid_uid(uid)) return Status(Errc::InvalidArgument, std::string("invalid UID for ") + what);
    return Status::ok();
}

// Item with a 1-byte type, 1 reserved byte and a 16-bit length, holding a UID.
Status uid_item(Out& o, std::uint8_t type, std::string_view uid, const char* what) {
    if (Status s = check_uid(uid, what); !s) return s;
    o.u8(type);
    o.u8(0);
    o.u16(static_cast<std::uint16_t>(uid.size()));
    o.str(uid);
    return Status::ok();
}

Status ae_field(Out& o, std::string_view ae, const char* what) {
    if (!is_valid_ae_title(ae)) return Status(Errc::InvalidArgument, std::string("invalid AE title: ") + what);
    o.str(ae);
    for (std::size_t i = ae.size(); i < kAeLen; ++i) o.u8(' ');
    return Status::ok();
}

Status bytes16(Out& o, const std::vector<std::uint8_t>& b, const char* what) {
    if (b.size() > 0xFFFF) return Status(Errc::InvalidArgument, std::string(what) + " longer than 65535 bytes");
    o.u16(static_cast<std::uint16_t>(b.size()));
    o.raw(b.data(), b.size());
    return Status::ok();
}

Status uid16(Out& o, std::string_view uid, const char* what) {
    if (Status s = check_uid(uid, what); !s) return s;
    o.u16(static_cast<std::uint16_t>(uid.size()));
    o.str(uid);
    return Status::ok();
}

Status encode_user_info(Out& o, const UserInformation& u, bool is_rq) {
    o.u8(kItemUserInfo);
    o.u8(0);
    const std::size_t len_at = o.pos();
    o.u16(0);
    const std::size_t body = o.pos();

    o.u8(kSubMaxLength);
    o.u8(0);
    o.u16(4);
    o.u32(u.max_length);

    if (Status s = uid_item(o, kSubImplClassUid, u.implementation_class_uid, "implementation class"); !s) return s;

    if (u.async_window) {
        o.u8(kSubAsyncWindow);
        o.u8(0);
        o.u16(4);
        o.u16(u.async_window->max_invoked);
        o.u16(u.async_window->max_performed);
    }
    for (const RoleSelection& r : u.roles) {
        o.u8(kSubRoleSelection);
        o.u8(0);
        const std::size_t at = o.pos();
        o.u16(0);
        const std::size_t b = o.pos();
        if (Status s = uid16(o, r.sop_class_uid, "role selection"); !s) return s;
        o.u8(r.scu_role ? 1 : 0);
        o.u8(r.scp_role ? 1 : 0);
        if (Status s = o.patch16(at, b); !s) return s;
    }
    if (!u.implementation_version_name.empty()) {
        const std::string& v = u.implementation_version_name;
        if (v.size() > kAeLen) return Status(Errc::InvalidArgument, "implementation version name over 16 characters");
        o.u8(kSubImplVersion);
        o.u8(0);
        o.u16(static_cast<std::uint16_t>(v.size()));
        o.str(v);
    }
    for (const ExtendedNegotiation& e : u.extended) {
        o.u8(kSubExtNeg);
        o.u8(0);
        const std::size_t at = o.pos();
        o.u16(0);
        const std::size_t b = o.pos();
        if (Status s = uid16(o, e.sop_class_uid, "extended negotiation"); !s) return s;
        o.raw(e.info.data(), e.info.size());
        if (Status s = o.patch16(at, b); !s) return s;
    }
    if (is_rq) {
        for (const CommonExtendedNegotiation& c : u.common_extended) {
            o.u8(kSubCommonExtNeg);
            o.u8(0);  // sub-item version
            const std::size_t at = o.pos();
            o.u16(0);
            const std::size_t b = o.pos();
            if (Status s = uid16(o, c.sop_class_uid, "common extended negotiation"); !s) return s;
            if (Status s = uid16(o, c.service_class_uid, "common extended negotiation"); !s) return s;
            const std::size_t rel_at = o.pos();
            o.u16(0);
            const std::size_t rel = o.pos();
            for (const std::string& g : c.related_general_sop_classes)
                if (Status s = uid16(o, g, "related general SOP class"); !s) return s;
            if (Status s = o.patch16(rel_at, rel); !s) return s;
            if (Status s = o.patch16(at, b); !s) return s;
        }
        if (u.identity_rq) {
            const UserIdentityRq& id = *u.identity_rq;
            o.u8(kSubIdentityRq);
            o.u8(0);
            const std::size_t at = o.pos();
            o.u16(0);
            const std::size_t b = o.pos();
            o.u8(id.type);
            o.u8(id.positive_response_requested ? 1 : 0);
            if (Status s = bytes16(o, id.primary, "user identity primary field"); !s) return s;
            if (Status s = bytes16(o, id.secondary, "user identity secondary field"); !s) return s;
            if (Status s = o.patch16(at, b); !s) return s;
        }
    } else if (u.identity_ac) {
        o.u8(kSubIdentityAc);
        o.u8(0);
        const std::size_t at = o.pos();
        o.u16(0);
        const std::size_t b = o.pos();
        if (Status s = bytes16(o, u.identity_ac->server_response, "user identity response"); !s) return s;
        if (Status s = o.patch16(at, b); !s) return s;
    }
    return o.patch16(len_at, body);
}

template <class Pc>
Status encode_associate(Out& o, std::uint8_t type, std::uint16_t version, const std::string& called,
                        const std::string& calling, const std::string& app_ctx, const std::vector<Pc>& pcs,
                        const UserInformation& user) {
    constexpr bool is_rq = std::is_same_v<Pc, PresentationContextRq>;
    o.u8(type);
    o.u8(0);
    const std::size_t len_at = o.pos();
    o.u32(0);
    const std::size_t body = o.pos();
    o.u16(version);
    o.u16(0);
    if (Status s = ae_field(o, called, "called"); !s) return s;
    if (Status s = ae_field(o, calling, "calling"); !s) return s;
    o.zeros(32);
    if (Status s = uid_item(o, kItemAppContext, app_ctx, "application context"); !s) return s;
    if (pcs.size() > 128) return Status(Errc::LimitExceeded, "more than 128 presentation contexts");
    for (const Pc& pc : pcs) {
        if ((pc.id & 1u) == 0) return Status(Errc::InvalidArgument, "presentation context ID must be odd");
        o.u8(is_rq ? kItemPcRq : kItemPcAc);
        o.u8(0);
        const std::size_t at = o.pos();
        o.u16(0);
        const std::size_t b = o.pos();
        o.u8(pc.id);
        o.u8(0);
        if constexpr (is_rq) {
            o.u8(0);
            o.u8(0);
            if (pc.transfer_syntaxes.empty())
                return Status(Errc::InvalidArgument, "presentation context without transfer syntax");
            if (Status s = uid_item(o, kItemAbstractSyntax, pc.abstract_syntax, "abstract syntax"); !s) return s;
            for (const std::string& ts : pc.transfer_syntaxes)
                if (Status s = uid_item(o, kItemTransferSyntax, ts, "transfer syntax"); !s) return s;
        } else {
            o.u8(pc.result);
            o.u8(0);
            // PS3.8 9.3.3.2: the transfer syntax sub-item is present; it is not significant
            // when the context was rejected, so an empty UID is not allowed but any valid one is.
            const std::string_view ts =
                pc.result == pc_result::Acceptance || !pc.transfer_syntax.empty() ? std::string_view(pc.transfer_syntax)
                                                                                 : std::string_view("1.2.840.10008.1.2");
            if (Status s = uid_item(o, kItemTransferSyntax, ts, "transfer syntax"); !s) return s;
        }
        if (Status s = o.patch16(at, b); !s) return s;
    }
    if (Status s = encode_user_info(o, user, is_rq); !s) return s;
    return o.patch32(len_at, body);
}

void fixed4(Out& o, std::uint8_t type, std::uint8_t b2, std::uint8_t b3, std::uint8_t b4) {
    o.u8(type);
    o.u8(0);
    o.u32(4);
    o.u8(0);
    o.u8(b2);
    o.u8(b3);
    o.u8(b4);
}

// ------------------------------------------------------------------ parsing

class Cursor {
public:
    Cursor(const std::uint8_t* p, std::size_t n, std::uint64_t base) : p_(p), n_(n), base_(base) {}
    std::size_t left() const { return n_ - pos_; }
    bool done() const { return pos_ == n_; }
    std::uint64_t offset() const { return base_ + pos_; }
    bool u8(std::uint8_t& v) {
        if (left() < 1) return false;
        v = p_[pos_++];
        return true;
    }
    bool u16(std::uint16_t& v) {
        if (left() < 2) return false;
        v = load_be16(p_ + pos_);
        pos_ += 2;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (left() < 4) return false;
        v = load_be32(p_ + pos_);
        pos_ += 4;
        return true;
    }
    bool skip(std::size_t n) {
        if (left() < n) return false;
        pos_ += n;
        return true;
    }
    bool take(std::size_t n, const std::uint8_t*& out) {
        if (left() < n) return false;
        out = p_ + pos_;
        pos_ += n;
        return true;
    }
    // Sub-cursor over the next n bytes.
    bool sub(std::size_t n, Cursor& out) {
        if (left() < n) return false;
        out = Cursor(p_ + pos_, n, base_ + pos_);
        pos_ += n;
        return true;
    }
    std::size_t pos() const { return pos_; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t pos_ = 0;
    std::uint64_t base_;
};

Status truncated(const Cursor& c, const char* what) {
    return Status(Errc::Truncated, std::string("truncated ") + what, c.offset());
}
Status malformed(const Cursor& c, const std::string& what) { return Status(Errc::Malformed, what, c.offset()); }

// UIDs in UL items are not padded, but some senders add a trailing NUL or space; tolerate that.
Status read_uid(Cursor& c, std::size_t len, std::string& out, const char* what) {
    const std::uint8_t* p = nullptr;
    if (!c.take(len, p)) return truncated(c, what);
    std::size_t n = len;
    while (n > 0 && (p[n - 1] == 0 || p[n - 1] == ' ')) --n;
    out.assign(reinterpret_cast<const char*>(p), n);
    if (!is_valid_uid(out)) return malformed(c, std::string("invalid UID in ") + what);
    return Status::ok();
}

Status read_ae(Cursor& c, std::string& out) {
    const std::uint8_t* p = nullptr;
    if (!c.take(kAeLen, p)) return truncated(c, "AE title");
    std::size_t b = 0, e = kAeLen;
    while (b < e && (p[b] == ' ' || p[b] == 0)) ++b;
    while (e > b && (p[e - 1] == ' ' || p[e - 1] == 0)) --e;
    out.assign(reinterpret_cast<const char*>(p + b), e - b);
    for (char ch : out)
        if (static_cast<unsigned char>(ch) < 0x20 || ch == '\\' || static_cast<unsigned char>(ch) == 0x7F)
            return malformed(c, "invalid character in AE title");
    return Status::ok();
}

// Reads a sub-item header (type, reserved, 16-bit length) and returns a cursor over its body.
Status item_header(Cursor& c, std::uint8_t& type, Cursor& body, const char* what) {
    std::uint8_t reserved = 0;
    std::uint16_t len = 0;
    if (!c.u8(type) || !c.u8(reserved) || !c.u16(len)) return truncated(c, what);
    if (!c.sub(len, body)) return truncated(c, what);
    return Status::ok();
}

Status read_uid16(Cursor& c, std::string& out, const char* what) {
    std::uint16_t n = 0;
    if (!c.u16(n)) return truncated(c, what);
    if (n > kMaxUid) return malformed(c, std::string("UID too long in ") + what);
    return read_uid(c, n, out, what);
}

Status read_bytes16(Cursor& c, std::vector<std::uint8_t>& out, const char* what) {
    std::uint16_t n = 0;
    if (!c.u16(n)) return truncated(c, what);
    const std::uint8_t* p = nullptr;
    if (!c.take(n, p)) return truncated(c, what);
    out.assign(p, p + n);
    return Status::ok();
}

Status parse_user_info(Cursor& c, UserInformation& u, bool is_rq, const PduLimits& lim) {
    bool have_max = false;
    std::size_t count = 0;
    while (!c.done()) {
        if (++count > lim.max_user_items) return Status(Errc::LimitExceeded, "too many user information items", c.offset());
        std::uint8_t type = 0;
        Cursor b(nullptr, 0, 0);
        if (Status s = item_header(c, type, b, "user information sub-item"); !s) return s;
        switch (type) {
            case kSubMaxLength:
                if (b.left() != 4) return malformed(b, "maximum length sub-item length is not 4");
                (void)b.u32(u.max_length);
                have_max = true;
                break;
            case kSubImplClassUid:
                if (b.left() > kMaxUid) return malformed(b, "implementation class UID too long");
                if (Status s = read_uid(b, b.left(), u.implementation_class_uid, "implementation class UID"); !s) return s;
                break;
            case kSubAsyncWindow: {
                if (b.left() != 4) return malformed(b, "asynchronous operations window length is not 4");
                AsyncWindow w;
                (void)b.u16(w.max_invoked);
                (void)b.u16(w.max_performed);
                u.async_window = w;
                break;
            }
            case kSubRoleSelection: {
                RoleSelection r;
                if (Status s = read_uid16(b, r.sop_class_uid, "role selection"); !s) return s;
                std::uint8_t scu = 0, scp = 0;
                if (!b.u8(scu) || !b.u8(scp)) return truncated(b, "role selection");
                if (!b.done()) return malformed(b, "trailing bytes in role selection");
                if (scu > 1 || scp > 1) return malformed(b, "invalid role value");
                r.scu_role = scu == 1;
                r.scp_role = scp == 1;
                u.roles.push_back(std::move(r));
                break;
            }
            case kSubImplVersion: {
                if (b.left() > kAeLen) return malformed(b, "implementation version name too long");
                const std::uint8_t* p = nullptr;
                const std::size_t n = b.left();
                (void)b.take(n, p);
                std::size_t e = n;
                while (e > 0 && (p[e - 1] == ' ' || p[e - 1] == 0)) --e;
                u.implementation_version_name.assign(reinterpret_cast<const char*>(p), e);
                break;
            }
            case kSubExtNeg: {
                ExtendedNegotiation x;
                if (Status s = read_uid16(b, x.sop_class_uid, "extended negotiation"); !s) return s;
                const std::uint8_t* p = nullptr;
                const std::size_t n = b.left();
                (void)b.take(n, p);
                x.info.assign(p, p + n);
                u.extended.push_back(std::move(x));
                break;
            }
            case kSubCommonExtNeg: {
                if (!is_rq) return malformed(b, "common extended negotiation in A-ASSOCIATE-AC");
                CommonExtendedNegotiation x;
                if (Status s = read_uid16(b, x.sop_class_uid, "common extended negotiation"); !s) return s;
                if (Status s = read_uid16(b, x.service_class_uid, "common extended negotiation"); !s) return s;
                std::uint16_t rel_len = 0;
                if (!b.u16(rel_len)) return truncated(b, "common extended negotiation");
                Cursor rel(nullptr, 0, 0);
                if (!b.sub(rel_len, rel)) return truncated(b, "related general SOP classes");
                while (!rel.done()) {
                    if (x.related_general_sop_classes.size() >= lim.max_related_sop_classes)
                        return Status(Errc::LimitExceeded, "too many related general SOP classes", rel.offset());
                    std::string g;
                    if (Status s = read_uid16(rel, g, "related general SOP class"); !s) return s;
                    x.related_general_sop_classes.push_back(std::move(g));
                }
                if (!b.done()) return malformed(b, "trailing bytes in common extended negotiation");
                u.common_extended.push_back(std::move(x));
                break;
            }
            case kSubIdentityRq: {
                if (!is_rq) return malformed(b, "user identity request in A-ASSOCIATE-AC");
                if (u.identity_rq) return malformed(b, "duplicate user identity item");
                UserIdentityRq id;
                std::uint8_t pos = 0;
                if (!b.u8(id.type) || !b.u8(pos)) return truncated(b, "user identity");
                if (pos > 1) return malformed(b, "invalid positive-response-requested value");
                id.positive_response_requested = pos == 1;
                if (Status s = read_bytes16(b, id.primary, "user identity primary field"); !s) return s;
                if (Status s = read_bytes16(b, id.secondary, "user identity secondary field"); !s) return s;
                if (!b.done()) return malformed(b, "trailing bytes in user identity");
                u.identity_rq = std::move(id);
                break;
            }
            case kSubIdentityAc: {
                if (is_rq) return malformed(b, "user identity response in A-ASSOCIATE-RQ");
                UserIdentityAc id;
                if (Status s = read_bytes16(b, id.server_response, "user identity response"); !s) return s;
                if (!b.done()) return malformed(b, "trailing bytes in user identity response");
                u.identity_ac = std::move(id);
                break;
            }
            default:
                break;  // unknown sub-items are ignored (PS3.7 D.3.3)
        }
    }
    if (!have_max) return malformed(c, "user information without maximum length sub-item");
    if (u.implementation_class_uid.empty()) return malformed(c, "user information without implementation class UID");
    return Status::ok();
}

template <class Assoc>
Status parse_associate(Cursor& c, Assoc& a, const PduLimits& lim) {
    constexpr bool is_rq = std::is_same_v<Assoc, AssociateRq>;
    std::uint16_t reserved = 0;
    if (!c.u16(a.protocol_version) || !c.u16(reserved)) return truncated(c, "A-ASSOCIATE header");
    if (Status s = read_ae(c, a.called_ae); !s) return s;
    if (Status s = read_ae(c, a.calling_ae); !s) return s;
    if (!c.skip(32)) return truncated(c, "A-ASSOCIATE header");
    bool have_app = false, have_user = false;
    while (!c.done()) {
        std::uint8_t type = 0;
        Cursor b(nullptr, 0, 0);
        if (Status s = item_header(c, type, b, "variable item"); !s) return s;
        if (type == kItemAppContext) {
            if (have_app) return malformed(b, "duplicate application context item");
            if (b.left() > kMaxUid) return malformed(b, "application context name too long");
            if (Status s = read_uid(b, b.left(), a.application_context, "application context"); !s) return s;
            have_app = true;
        } else if (type == (is_rq ? kItemPcRq : kItemPcAc)) {
            if (a.contexts.size() >= lim.max_presentation_contexts)
                return Status(Errc::LimitExceeded, "too many presentation contexts", b.offset());
            std::uint8_t id = 0, r1 = 0, r2 = 0, r3 = 0;
            if (!b.u8(id) || !b.u8(r1) || !b.u8(r2) || !b.u8(r3)) return truncated(b, "presentation context");
            if ((id & 1u) == 0) return malformed(b, "even presentation context ID");
            for (const auto& prev : a.contexts)
                if (prev.id == id) return malformed(b, "duplicate presentation context ID");
            if constexpr (is_rq) {
                PresentationContextRq pc;
                pc.id = id;
                bool have_abs = false;
                while (!b.done()) {
                    std::uint8_t st = 0;
                    Cursor sb(nullptr, 0, 0);
                    if (Status s = item_header(b, st, sb, "presentation context sub-item"); !s) return s;
                    if (sb.left() > kMaxUid) return malformed(sb, "UID too long in presentation context");
                    if (st == kItemAbstractSyntax) {
                        if (have_abs) return malformed(sb, "duplicate abstract syntax");
                        if (Status s = read_uid(sb, sb.left(), pc.abstract_syntax, "abstract syntax"); !s) return s;
                        have_abs = true;
                    } else if (st == kItemTransferSyntax) {
                        if (pc.transfer_syntaxes.size() >= lim.max_transfer_syntaxes)
                            return Status(Errc::LimitExceeded, "too many transfer syntaxes", sb.offset());
                        std::string ts;
                        if (Status s = read_uid(sb, sb.left(), ts, "transfer syntax"); !s) return s;
                        pc.transfer_syntaxes.push_back(std::move(ts));
                    } else {
                        return malformed(sb, "unexpected presentation context sub-item");
                    }
                }
                if (!have_abs || pc.transfer_syntaxes.empty())
                    return malformed(b, "incomplete presentation context");
                a.contexts.push_back(std::move(pc));
            } else {
                PresentationContextAc pc;
                pc.id = id;
                pc.result = r2;
                if (pc.result > pc_result::TransferSyntaxesNotSupported)
                    return malformed(b, "invalid presentation context result");
                bool have_ts = false;
                while (!b.done()) {
                    std::uint8_t st = 0;
                    Cursor sb(nullptr, 0, 0);
                    if (Status s = item_header(b, st, sb, "presentation context sub-item"); !s) return s;
                    if (st != kItemTransferSyntax || have_ts)
                        return malformed(sb, "unexpected presentation context sub-item");
                    if (sb.left() > kMaxUid) return malformed(sb, "UID too long in presentation context");
                    if (pc.result == pc_result::Acceptance) {
                        if (Status s = read_uid(sb, sb.left(), pc.transfer_syntax, "transfer syntax"); !s) return s;
                    } else {
                        (void)sb.skip(sb.left());  // not significant for rejected contexts
                    }
                    have_ts = true;
                }
                if (pc.result == pc_result::Acceptance && !have_ts)
                    return malformed(b, "accepted presentation context without transfer syntax");
                a.contexts.push_back(std::move(pc));
            }
        } else if (type == kItemUserInfo) {
            if (have_user) return malformed(b, "duplicate user information item");
            if (Status s = parse_user_info(b, a.user, is_rq, lim); !s) return s;
            have_user = true;
        } else {
            return malformed(b, "unexpected variable item");
        }
    }
    if (!have_app) return malformed(c, "missing application context item");
    if (!have_user) return malformed(c, "missing user information item");
    if constexpr (is_rq) {
        if (a.contexts.empty()) return malformed(c, "A-ASSOCIATE-RQ without presentation contexts");
    }
    return Status::ok();
}

}  // namespace

bool is_valid_uid(std::string_view uid) noexcept {
    if (uid.empty() || uid.size() > kMaxUid) return false;
    if (uid.front() == '.' || uid.back() == '.') return false;
    char prev = '.';
    for (char c : uid) {
        if (c == '.') {
            if (prev == '.') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
        prev = c;
    }
    return true;
}

bool is_valid_ae_title(std::string_view ae) noexcept {
    if (ae.empty() || ae.size() > kAeLen) return false;
    bool any = false;
    for (char c : ae) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == '\\') return false;
        if (c != ' ') any = true;
    }
    return any;
}

PduType pdu_type(const Pdu& p) noexcept {
    switch (p.index()) {
        case 0: return PduType::AssociateRq;
        case 1: return PduType::AssociateAc;
        case 2: return PduType::AssociateRj;
        case 3: return PduType::PData;
        case 4: return PduType::ReleaseRq;
        case 5: return PduType::ReleaseRp;
        default: return PduType::Abort;
    }
}

void encode_single_pdv_header(std::uint8_t context_id, bool command, bool last, std::uint32_t data_len,
                              std::uint8_t out[12]) noexcept {
    out[0] = static_cast<std::uint8_t>(PduType::PData);
    out[1] = 0;
    store_be32(out + 2, data_len + 6u);
    store_be32(out + 6, data_len + 2u);
    out[10] = context_id;
    out[11] = static_cast<std::uint8_t>((command ? 1u : 0u) | (last ? 2u : 0u));
}

Status encode_pdu(const Pdu& pdu, std::vector<std::uint8_t>& out) {
    Out o(out);
    switch (pdu.index()) {
        case 0: {
            const auto& a = std::get<AssociateRq>(pdu);
            return encode_associate(o, 0x01, a.protocol_version, a.called_ae, a.calling_ae, a.application_context,
                                    a.contexts, a.user);
        }
        case 1: {
            const auto& a = std::get<AssociateAc>(pdu);
            return encode_associate(o, 0x02, a.protocol_version, a.called_ae, a.calling_ae, a.application_context,
                                    a.contexts, a.user);
        }
        case 2: {
            const auto& r = std::get<AssociateRj>(pdu);
            fixed4(o, 0x03, r.result, r.source, r.reason);
            return Status::ok();
        }
        case 3: {
            const auto& d = std::get<PDataTf>(pdu);
            if (d.pdvs.empty()) return Status(Errc::InvalidArgument, "P-DATA-TF without PDV items");
            o.u8(0x04);
            o.u8(0);
            const std::size_t len_at = o.pos();
            o.u32(0);
            const std::size_t body = o.pos();
            for (const Pdv& v : d.pdvs) {
                if ((v.context_id & 1u) == 0) return Status(Errc::InvalidArgument, "even presentation context ID");
                const std::uint64_t item = static_cast<std::uint64_t>(v.data.size()) + 2;
                if (item > 0xFFFFFFFFull) return Status(Errc::LimitExceeded, "PDV too large");
                o.u32(static_cast<std::uint32_t>(item));
                o.u8(v.context_id);
                o.u8(static_cast<std::uint8_t>((v.command ? 1u : 0u) | (v.last ? 2u : 0u)));
                o.raw(v.data.data(), v.data.size());
            }
            return o.patch32(len_at, body);
        }
        case 4: fixed4(o, 0x05, 0, 0, 0); return Status::ok();
        case 5: fixed4(o, 0x06, 0, 0, 0); return Status::ok();
        default: {
            const auto& a = std::get<Abort>(pdu);
            fixed4(o, 0x07, 0, a.source, a.reason);
            return Status::ok();
        }
    }
}

Result<Pdu> parse_pdu(const Bytes& buf, const PduLimits& limits) {
    if (buf.size() < 6) return Status(Errc::Truncated, "PDU header truncated");
    const std::uint8_t* p = buf.data();
    const std::uint8_t type = p[0];
    const std::uint64_t len = load_be32(p + 2);
    if (len > limits.max_pdu_length) return Status(Errc::LimitExceeded, "PDU length exceeds limit", 2);
    if (len + 6 > buf.size()) return Status(Errc::Truncated, "PDU body truncated", buf.size());
    if (len + 6 < buf.size()) return Status(Errc::Malformed, "bytes after PDU", len + 6);
    Cursor c(p + 6, static_cast<std::size_t>(len), 6);

    auto fixed = [&](std::uint8_t& b2, std::uint8_t& b3, std::uint8_t& b4) -> Status {
        if (len != 4) return Status(Errc::Malformed, "fixed-size PDU length is not 4", 2);
        std::uint8_t b1 = 0;
        (void)c.u8(b1);
        (void)c.u8(b2);
        (void)c.u8(b3);
        (void)c.u8(b4);
        return Status::ok();
    };

    switch (type) {
        case 0x01: {
            AssociateRq a;
            if (Status s = parse_associate(c, a, limits); !s) return s;
            return Pdu(std::move(a));
        }
        case 0x02: {
            AssociateAc a;
            if (Status s = parse_associate(c, a, limits); !s) return s;
            return Pdu(std::move(a));
        }
        case 0x03: {
            AssociateRj r;
            if (Status s = fixed(r.result, r.source, r.reason); !s) return s;
            if (r.result < 1 || r.result > 2 || r.source < 1 || r.source > 3)
                return Status(Errc::Malformed, "invalid A-ASSOCIATE-RJ values", 7);
            return Pdu(r);
        }
        case 0x04: {
            PDataTf d;
            if (c.done()) return Status(Errc::Malformed, "P-DATA-TF without PDV items", 6);
            while (!c.done()) {
                if (d.pdvs.size() >= limits.max_pdvs) return Status(Errc::LimitExceeded, "too many PDV items", c.offset());
                std::uint32_t item_len = 0;
                if (!c.u32(item_len)) return truncated(c, "PDV item");
                if (item_len < 2) return malformed(c, "PDV item length below 2");
                if (item_len > c.left()) return truncated(c, "PDV item");
                Pdv v;
                std::uint8_t hdr = 0;
                (void)c.u8(v.context_id);
                (void)c.u8(hdr);
                if ((v.context_id & 1u) == 0) return malformed(c, "even presentation context ID in PDV");
                if ((hdr & 0xFCu) != 0) return malformed(c, "reserved bits set in PDV header");
                v.command = (hdr & 1u) != 0;
                v.last = (hdr & 2u) != 0;
                const std::size_t off = 6 + c.pos();
                const std::size_t n = item_len - 2u;
                (void)c.skip(n);
                v.data = buf.sub(off, n);
                d.pdvs.push_back(std::move(v));
            }
            return Pdu(std::move(d));
        }
        case 0x05: {
            std::uint8_t a = 0, b = 0, d = 0;
            if (Status s = fixed(a, b, d); !s) return s;
            return Pdu(ReleaseRq{});
        }
        case 0x06: {
            std::uint8_t a = 0, b = 0, d = 0;
            if (Status s = fixed(a, b, d); !s) return s;
            return Pdu(ReleaseRp{});
        }
        case 0x07: {
            Abort a;
            std::uint8_t reserved = 0;
            if (Status s = fixed(reserved, a.source, a.reason); !s) return s;
            if (a.source != abort_src::User && a.source != abort_src::Provider && a.source != 1)
                return Status(Errc::Malformed, "invalid A-ABORT source", 8);
            return Pdu(a);
        }
        default:
            return Status(Errc::Unsupported, "unrecognized PDU type", 0);
    }
}

std::uint64_t pdu_length_cap(std::uint8_t type, const PduLimits& limits) noexcept {
    std::uint64_t cap = limits.max_pdu_length;
    switch (type) {
        case 0x04:  // P-DATA-TF: the general cap (and max_pdata_length when set)
            if (limits.max_pdata_length != 0) cap = std::min(cap, limits.max_pdata_length);
            break;
        case 0x03: case 0x05: case 0x06: case 0x07:  // fixed 4-byte bodies (PS3.8 9.3.4, 9.3.6-9.3.8)
            cap = std::min<std::uint64_t>(cap, 4);
            break;
        default:  // A-ASSOCIATE-RQ/AC, and unknown types (read so the error can be reported)
            cap = std::min(cap, limits.max_associate_length);
            break;
    }
    return cap;
}

Result<Pdu> read_pdu(Transport& t, int timeout_ms, const PduLimits& limits) {
    // One absolute deadline for the whole PDU, header included (NET-03).
    const Deadline deadline = deadline_after(timeout_ms);
    std::uint8_t hdr[6];
    if (Status s = read_exact_until(t, hdr, 6, deadline); !s) return s;
    const std::uint64_t len = load_be32(hdr + 2);
    // The cap depends on the PDU type and is checked before anything is allocated (NET-01).
    if (len > pdu_length_cap(hdr[0], limits)) {
        const std::uint8_t ty = hdr[0];
        if (ty == 0x03 || ty == 0x05 || ty == 0x06 || ty == 0x07)
            return Status(Errc::Malformed, "fixed-size PDU length is not 4", 2);
        return Status(Errc::LimitExceeded, "PDU length exceeds limit", 2);
    }
    std::uint64_t total = 0;
    if (!checked_add(len, 6, total) || total > SIZE_MAX) return Status(Errc::LimitExceeded, "PDU too large");
    // The buffer grows with the bytes that actually arrive: a header claiming a large PDU costs at
    // most one step until the peer really sends that much.
    constexpr std::size_t kStep = 64u << 10;
    const auto want = static_cast<std::size_t>(total);
    auto storage = std::make_shared<std::vector<std::uint8_t>>(std::min(want, 6 + kStep));
    std::memcpy(storage->data(), hdr, 6);
    std::size_t got = 6;
    while (got < want) {
        if (got == storage->size()) storage->resize(std::min(want, storage->size() * 2));
        const std::size_t room = storage->size() - got;
        if (Status s = read_exact_until(t, storage->data() + got, room, deadline); !s) return s;
        got += room;
    }
    const std::uint8_t* data = storage->data();
    Bytes buf(std::shared_ptr<const void>(storage, storage.get()), data, storage->size());
    return parse_pdu(buf, limits);
}

}  // namespace siaara::net
