// SIAARA DICOM Toolkit - DEFLATE (RFC 1951) raw streams, written from the RFC.
//
// Decoder: table-driven Huffman decoding (one primary lookup plus at most one second-level
// lookup per symbol) fed from a 64-bit bit buffer that is refilled several bytes at a time.
// Encoder: LZ77 with hash chains (optionally lazy), then for every block the cheapest of
// stored / fixed Huffman / dynamic Huffman with length-limited codes from package-merge.
#include "codec/deflate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <new>
#include <string>

#include "core/types.hpp"

namespace siaara::codec {
namespace {

// ============================================================ alphabets (RFC 1951 section 3.2.5)

constexpr int kLitLenSymbols = 288;   // 286 and 287 take part in the fixed code but never occur
constexpr int kDistSymbols = 32;      // 30 and 31 take part in the fixed code but never occur
constexpr int kEndOfBlock = 256;
constexpr int kFirstLengthSymbol = 257;
constexpr int kLengthCodes = 29;      // symbols 257..285
constexpr int kDistCodes = 30;        // symbols 0..29
constexpr int kMaxCodeBits = 15;
constexpr int kMaxClBits = 7;         // code length code lengths are 3-bit values
constexpr int kClSymbols = 19;
constexpr std::size_t kWindowSize = 32768;
constexpr std::size_t kMinMatch = 3;
constexpr std::size_t kMaxMatch = 258;
constexpr std::size_t kMaxStored = 65535;

struct Alphabets {
    std::uint16_t len_base[kLengthCodes]{};
    std::uint8_t len_extra[kLengthCodes]{};
    std::uint16_t dist_base[kDistCodes]{};
    std::uint8_t dist_extra[kDistCodes]{};
    std::uint8_t len_code[kMaxMatch + 1]{};  // match length -> index into len_* (0..28)
};

// Derives the length and distance code ranges from the rules stated in section 3.2.5:
// length codes 257..264 carry no extra bits, every following group of four codes carries one
// more extra bit than the previous group, and code 285 stands for 258 alone; distance codes 0..3
// carry no extra bits and every following pair carries one more.
constexpr Alphabets make_alphabets() {
    Alphabets a{};
    int len = 3;
    for (int i = 0; i < kLengthCodes - 1; ++i) {
        const int extra = i < 8 ? 0 : (i - 4) / 4;
        a.len_base[i] = static_cast<std::uint16_t>(len);
        a.len_extra[i] = static_cast<std::uint8_t>(extra);
        len += 1 << extra;
    }
    a.len_base[kLengthCodes - 1] = 258;
    a.len_extra[kLengthCodes - 1] = 0;
    int dist = 1;
    for (int i = 0; i < kDistCodes; ++i) {
        const int extra = i < 4 ? 0 : i / 2 - 1;
        a.dist_base[i] = static_cast<std::uint16_t>(dist);
        a.dist_extra[i] = static_cast<std::uint8_t>(extra);
        dist += 1 << extra;
    }
    for (std::size_t l = kMinMatch; l <= kMaxMatch; ++l) {
        int code = 0;
        while (code + 1 < kLengthCodes - 1 && a.len_base[code + 1] <= l) ++code;
        if (l == kMaxMatch) code = kLengthCodes - 1;
        a.len_code[l] = static_cast<std::uint8_t>(code);
    }
    return a;
}
constexpr Alphabets kAlpha = make_alphabets();

// Order in which code length code lengths are transmitted (section 3.2.7).
constexpr std::uint8_t kClOrder[kClSymbols] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                               11, 4, 12, 3, 13, 2, 14, 1, 15};

inline int dist_code(std::uint32_t dist) noexcept {
    const std::uint32_t v = dist - 1;
    if (v < 4) return static_cast<int>(v);
    const int top = static_cast<int>(std::bit_width(v)) - 1;  // v >= 4, so top >= 2
    return 2 * top + static_cast<int>((v >> (top - 1)) & 1u);
}

// Reverses the low `len` (1..16) bits of `code`.
inline std::uint32_t reverse_bits(std::uint32_t code, int len) noexcept {
    std::uint32_t r = code & 0xFFFFu;
    r = ((r & 0x5555u) << 1) | ((r >> 1) & 0x5555u);
    r = ((r & 0x3333u) << 2) | ((r >> 2) & 0x3333u);
    r = ((r & 0x0F0Fu) << 4) | ((r >> 4) & 0x0F0Fu);
    r = ((r & 0x00FFu) << 8) | ((r >> 8) & 0x00FFu);
    return r >> (16 - len);
}

// Assigns canonical codes to code lengths (section 3.2.2) and returns them bit-reversed, ready to
// be sent least significant bit first.
void canonical_codes(const std::uint8_t* lens, int n, std::uint16_t* codes) {
    std::uint32_t count[kMaxCodeBits + 1] = {};
    for (int i = 0; i < n; ++i) ++count[lens[i]];
    count[0] = 0;
    std::uint32_t next[kMaxCodeBits + 2] = {};
    std::uint32_t code = 0;
    for (int b = 1; b <= kMaxCodeBits; ++b) {
        code = (code + count[b - 1]) << 1;
        next[b] = code;
    }
    for (int i = 0; i < n; ++i) {
        const int l = lens[i];
        codes[i] = static_cast<std::uint16_t>(l ? reverse_bits(next[l]++, l) : 0u);
    }
}

// ============================================================ decoding tables

enum Kind : std::uint8_t { kLiteral = 0, kValue = 1, kEnd = 2, kSubtable = 3, kInvalid = 4 };

// One lookup entry. `bits` is the full code length to drop. For kValue entries `value` is a
// length or distance base and `extra` the number of extra bits; for kSubtable entries `value` is
// the subtable start and `extra` its index width.
struct Entry {
    std::uint16_t value = 0;
    std::uint8_t bits = 0;
    std::uint8_t kind_extra = kInvalid << 5;
};
inline unsigned kind_of(Entry e) noexcept { return e.kind_extra >> 5; }
inline unsigned extra_of(Entry e) noexcept { return e.kind_extra & 31u; }
constexpr Entry make_entry(unsigned value, unsigned bits, unsigned kind, unsigned extra) {
    Entry e;
    e.value = static_cast<std::uint16_t>(value);
    e.bits = static_cast<std::uint8_t>(bits);
    e.kind_extra = static_cast<std::uint8_t>((kind << 5) | extra);
    return e;
}

constexpr int kLitPrimary = 11;
constexpr int kDistPrimary = 8;
constexpr int kClPrimary = 7;
// Upper bounds on table size: the primary table plus at most one subtable per symbol whose code
// is longer than the primary width, each at most 2^(15 - primary) entries.
constexpr std::size_t kLitTableSize = (1u << kLitPrimary) + kLitLenSymbols * (1u << (kMaxCodeBits - kLitPrimary));
constexpr std::size_t kDistTableSize = (1u << kDistPrimary) + kDistSymbols * (1u << (kMaxCodeBits - kDistPrimary));
constexpr std::size_t kClTableSize = 1u << kClPrimary;

enum class Alphabet { LitLen, Dist, CodeLen };

Entry symbol_entry(Alphabet a, int sym, int len) {
    switch (a) {
        case Alphabet::LitLen:
            if (sym < 256) return make_entry(static_cast<unsigned>(sym), static_cast<unsigned>(len), kLiteral, 0);
            if (sym == kEndOfBlock) return make_entry(0, static_cast<unsigned>(len), kEnd, 0);
            if (sym < kFirstLengthSymbol + kLengthCodes) {
                const int i = sym - kFirstLengthSymbol;
                return make_entry(kAlpha.len_base[i], static_cast<unsigned>(len), kValue, kAlpha.len_extra[i]);
            }
            return make_entry(0, 0, kInvalid, 0);
        case Alphabet::Dist:
            if (sym < kDistCodes)
                return make_entry(kAlpha.dist_base[sym], static_cast<unsigned>(len), kValue, kAlpha.dist_extra[sym]);
            return make_entry(0, 0, kInvalid, 0);
        case Alphabet::CodeLen:
            return make_entry(static_cast<unsigned>(sym), static_cast<unsigned>(len), kValue, 0);
    }
    return make_entry(0, 0, kInvalid, 0);
}

// Builds a lookup table for the code lengths lens[0..n). Rejects over-subscribed sets. An
// incomplete set is accepted only where RFC 1951 allows it (a single code of one bit, and an empty
// distance code); the unused table slots decode as kInvalid.
Status build_table(const std::uint8_t* lens, int n, Alphabet alpha, int primary, Entry* table,
                   std::size_t capacity) {
    int count[kMaxCodeBits + 1] = {};
    for (int i = 0; i < n; ++i) ++count[lens[i]];
    count[0] = 0;
    int used = 0;
    int left = 1;
    for (int b = 1; b <= kMaxCodeBits; ++b) {
        left = (left << 1) - count[b];
        used += count[b];
        if (left < 0) return Status(Errc::Malformed, "deflate: over-subscribed Huffman code lengths");
    }
    if (left != 0) {
        const bool single = used == 1 && count[1] == 1 && alpha != Alphabet::CodeLen;
        const bool empty = used == 0 && alpha == Alphabet::Dist;
        if (!single && !empty) return Status(Errc::Malformed, "deflate: incomplete Huffman code lengths");
    }

    std::uint16_t codes[kLitLenSymbols];
    canonical_codes(lens, n, codes);

    const std::size_t psize = std::size_t{1} << primary;
    const std::uint32_t pmask = static_cast<std::uint32_t>(psize - 1);
    if (left != 0)  // a complete code overwrites every primary slot below
        for (std::size_t i = 0; i < psize; ++i) table[i] = Entry{};

    // Pass 1: the longest code under each primary prefix decides that prefix's subtable width.
    std::uint8_t longest[1u << kLitPrimary] = {};
    for (int s = 0; s < n; ++s) {
        if (lens[s] > primary) {
            const std::uint32_t pre = codes[s] & pmask;
            longest[pre] = std::max(longest[pre], lens[s]);
        }
    }
    std::size_t next_free = psize;
    for (std::size_t pre = 0; pre < psize; ++pre) {
        if (!longest[pre]) continue;
        const unsigned width = static_cast<unsigned>(longest[pre] - primary);
        const std::size_t sub = std::size_t{1} << width;
        if (next_free + sub > capacity) return Status(Errc::Internal, "deflate: table overflow");
        table[pre] = make_entry(static_cast<unsigned>(next_free), 0, kSubtable, width);
        for (std::size_t j = 0; j < sub; ++j) table[next_free + j] = Entry{};
        next_free += sub;
    }
    // Pass 2: replicate each code over every slot whose low bits match it.
    for (int s = 0; s < n; ++s) {
        const int len = lens[s];
        if (!len) continue;
        const Entry e = symbol_entry(alpha, s, len);
        const std::uint32_t c = codes[s];
        if (len <= primary) {
            for (std::size_t i = c; i < psize; i += std::size_t{1} << len) table[i] = e;
        } else {
            const Entry sub = table[c & pmask];
            const std::size_t width = extra_of(sub);
            const std::size_t step = std::size_t{1} << (len - primary);
            for (std::size_t j = c >> primary; j < (std::size_t{1} << width); j += step) table[sub.value + j] = e;
        }
    }
    return Status::ok();
}

// ============================================================ inflate

// Bit reader. Bits are consumed from the low end of `buf`; `cnt` counts valid bits. When the
// input runs out the reader appends zero bytes and counts them in `pad`; consuming any of those
// means the stream is truncated, which the caller checks with overrun().
struct BitReader {
    const std::uint8_t* p;
    const std::uint8_t* end;
    std::uint64_t buf = 0;
    unsigned cnt = 0;
    std::size_t pad = 0;

    // Afterwards at least 56 bits are buffered.
    inline void refill() noexcept {
        if (end - p >= 8) {
            buf |= load_le64(p) << cnt;
            p += (63 - cnt) >> 3;
            cnt |= 56;
        } else {
            while (cnt <= 56) {
                std::uint64_t b = 0;
                if (p < end) b = *p++;
                else ++pad;
                buf |= b << cnt;
                cnt += 8;
            }
        }
    }
    inline std::uint32_t peek(unsigned n) const noexcept {
        return static_cast<std::uint32_t>(buf & ((std::uint64_t{1} << n) - 1));
    }
    inline void drop(unsigned n) noexcept {
        buf >>= n;
        cnt -= n;
    }
    inline std::uint32_t take(unsigned n) noexcept {
        const std::uint32_t v = peek(n);
        drop(n);
        return v;
    }
    inline bool overrun() const noexcept { return pad != 0 && pad * 8 > cnt; }
};

inline Entry lookup(const Entry* table, unsigned primary, std::uint64_t bits) noexcept {
    Entry e = table[bits & ((std::uint64_t{1} << primary) - 1)];
    if (kind_of(e) == kSubtable)
        e = table[e.value + ((bits >> primary) & ((std::uint64_t{1} << extra_of(e)) - 1))];
    return e;
}

constexpr std::size_t kSlack = 16;  // spare bytes after the logical end, for word-sized copies

inline bool fits_size(std::uint64_t v) noexcept {
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) return true;
    else return v <= static_cast<std::uint64_t>(static_cast<std::size_t>(-1));
}  // spare bytes after the logical end, for word-sized copies

class Inflater {
public:
    Inflater(const std::uint8_t* in, std::size_t n, std::size_t max_output, std::vector<std::uint8_t>& out)
        : begin_(in), max_out_(max_output), out_(out) {
        br_.p = in;
        br_.end = in + n;
    }

    Status run(std::size_t* consumed) {
        if (Status s = reserve(initial_capacity()); !s) return s;
        for (;;) {
            br_.refill();
            const std::uint32_t final_block = br_.take(1);
            const std::uint32_t type = br_.take(2);
            if (br_.overrun()) return truncated();
            Status s;
            if (type == 0) s = stored_block();
            else if (type == 1) s = fixed_block();
            else if (type == 2) s = dynamic_block();
            else return Status(Errc::Malformed, "deflate: reserved block type", offset());
            if (!s) return s;
            if (final_block) break;
        }
        // Whole bytes still in the bit buffer were read from the input but not used.
        const std::size_t unused = br_.cnt / 8 - br_.pad;
        if (consumed) *consumed = static_cast<std::size_t>(br_.p - begin_) - unused;
        out_.resize(opos_);
        return Status::ok();
    }

private:
    std::uint64_t offset() const noexcept { return static_cast<std::uint64_t>(br_.p - begin_); }
    Status truncated() const { return Status(Errc::Truncated, "deflate: stream truncated", offset()); }
    Status too_big() const {
        return Status(Errc::LimitExceeded, "deflate: output exceeds limit of " + std::to_string(max_out_) + " bytes",
                      offset());
    }

    std::size_t initial_capacity() const noexcept {
        std::uint64_t guess = 0;
        if (!checked_mul(static_cast<std::uint64_t>(br_.end - br_.p), 4, guess)) guess = UINT64_MAX;
        guess = std::max<std::uint64_t>(guess, 1u << 16);
        guess = std::min<std::uint64_t>(guess, max_out_);
        return static_cast<std::size_t>(guess);
    }

    // Makes room for `logical` output bytes (never more than max_out_) plus kSlack.
    Status reserve(std::size_t logical) {
        std::uint64_t want = 0;
        if (!checked_add(logical, kSlack, want) || !fits_size(want))
            return Status(Errc::OutOfMemory, "deflate: output buffer too large");
        if (want <= out_.size()) return Status::ok();
        std::uint64_t grown = 0;
        if (!checked_mul(out_.size(), 2, grown)) grown = UINT64_MAX;
        std::uint64_t cap_limit = 0;
        if (!checked_add(max_out_, kSlack, cap_limit)) cap_limit = UINT64_MAX;
        want = std::max(want, std::min(grown, cap_limit));
        try {
            out_.resize(static_cast<std::size_t>(want));
        } catch (const std::bad_alloc&) {
            return Status(Errc::OutOfMemory, "deflate: cannot allocate output buffer");
        } catch (const std::length_error&) {
            return Status(Errc::OutOfMemory, "deflate: cannot allocate output buffer");
        }
        return Status::ok();
    }

    Status stored_block() {
        br_.drop(br_.cnt & 7u);
        if (br_.overrun()) return truncated();
        // Give back the whole bytes still buffered and read the block straight from the input.
        br_.p -= br_.cnt / 8 - br_.pad;
        br_.buf = 0;
        br_.cnt = 0;
        br_.pad = 0;
        if (br_.end - br_.p < 4) return truncated();
        const std::uint32_t len = load_le16(br_.p);
        const std::uint32_t nlen = load_le16(br_.p + 2);
        if ((len ^ nlen) != 0xFFFFu) return Status(Errc::Malformed, "deflate: stored block LEN/NLEN mismatch", offset());
        br_.p += 4;
        if (static_cast<std::size_t>(br_.end - br_.p) < len) return truncated();
        if (len > max_out_ - opos_) return too_big();
        if (Status s = reserve(opos_ + len); !s) return s;
        if (len) std::memcpy(out_.data() + opos_, br_.p, len);
        opos_ += len;
        br_.p += len;
        return Status::ok();
    }

    Status fixed_block() {
        if (!fixed_ready_) {
            std::uint8_t lens[kLitLenSymbols + kDistSymbols];
            int i = 0;
            for (; i < 144; ++i) lens[i] = 8;
            for (; i < 256; ++i) lens[i] = 9;
            for (; i < 280; ++i) lens[i] = 7;
            for (; i < kLitLenSymbols; ++i) lens[i] = 8;
            for (; i < kLitLenSymbols + kDistSymbols; ++i) lens[i] = 5;
            if (Status s = build_table(lens, kLitLenSymbols, Alphabet::LitLen, kLitPrimary, fixed_lit_.data(),
                                       fixed_lit_.size());
                !s)
                return s;
            if (Status s = build_table(lens + kLitLenSymbols, kDistSymbols, Alphabet::Dist, kDistPrimary,
                                       fixed_dist_.data(), fixed_dist_.size());
                !s)
                return s;
            fixed_ready_ = true;
        }
        return codes(fixed_lit_.data(), fixed_dist_.data());
    }

    Status dynamic_block() {
        br_.refill();
        const unsigned hlit = br_.take(5) + 257;
        const unsigned hdist = br_.take(5) + 1;
        const unsigned hclen = br_.take(4) + 4;
        if (hlit > 286) return Status(Errc::Malformed, "deflate: too many literal/length codes", offset());

        std::uint8_t cl_lens[kClSymbols] = {};
        for (unsigned i = 0; i < hclen; ++i) {
            if (br_.cnt < 3) br_.refill();
            cl_lens[kClOrder[i]] = static_cast<std::uint8_t>(br_.take(3));
        }
        if (br_.overrun()) return truncated();
        if (Status s = build_table(cl_lens, kClSymbols, Alphabet::CodeLen, kClPrimary, cl_table_.data(),
                                   cl_table_.size());
            !s)
            return s;

        std::uint8_t lens[kLitLenSymbols + kDistSymbols] = {};
        const unsigned total = hlit + hdist;
        unsigned i = 0;
        while (i < total) {
            if (br_.cnt < kMaxClBits + 7) br_.refill();
            const Entry e = cl_table_[br_.peek(kClPrimary)];
            if (kind_of(e) == kInvalid) return Status(Errc::Malformed, "deflate: invalid code length code", offset());
            br_.drop(e.bits);
            const unsigned sym = e.value;
            unsigned repeat = 1;
            std::uint8_t value = static_cast<std::uint8_t>(sym);
            if (sym == 16) {
                if (i == 0) return Status(Errc::Malformed, "deflate: repeat with no previous length", offset());
                value = lens[i - 1];
                repeat = 3 + br_.take(2);
            } else if (sym == 17) {
                value = 0;
                repeat = 3 + br_.take(3);
            } else if (sym == 18) {
                value = 0;
                repeat = 11 + br_.take(7);
            }
            if (br_.overrun()) return truncated();
            if (repeat > total - i) return Status(Errc::Malformed, "deflate: code lengths overflow HLIT+HDIST", offset());
            for (unsigned k = 0; k < repeat; ++k) lens[i++] = value;
        }
        if (lens[kEndOfBlock] == 0) return Status(Errc::Malformed, "deflate: no end-of-block code", offset());
        if (Status s = build_table(lens, static_cast<int>(hlit), Alphabet::LitLen, kLitPrimary, lit_table_.data(),
                                   lit_table_.size());
            !s)
            return s;
        if (Status s = build_table(lens + hlit, static_cast<int>(hdist), Alphabet::Dist, kDistPrimary,
                                   dist_table_.data(), dist_table_.size());
            !s)
            return s;
        return codes(lit_table_.data(), dist_table_.data());
    }

    // Decodes the compressed data of one Huffman block. The bit reader and output cursor live in
    // locals so the compiler can keep them in registers across the byte stores.
    Status codes(const Entry* lit, const Entry* dist) {
        BitReader br = br_;
        std::uint8_t* ob = out_.data();
        std::size_t opos = opos_;
        std::size_t ocap = out_.size() - kSlack;  // logical capacity, kSlack bytes remain behind it
        const std::size_t max_out = max_out_;
        Status result;

        for (;;) {
            br.refill();
            Entry e = lookup(lit, kLitPrimary, br.buf);
            // Fast path: up to three literals per refill (3 x 15 bits <= 56). Bits past the end of
            // the input are only detected after the group; the output is discarded in that case.
            if (kind_of(e) == kLiteral && ocap - opos >= 3) {
                br.drop(e.bits);
                ob[opos++] = static_cast<std::uint8_t>(e.value);
                e = lookup(lit, kLitPrimary, br.buf);
                if (kind_of(e) == kLiteral) {
                    br.drop(e.bits);
                    ob[opos++] = static_cast<std::uint8_t>(e.value);
                    e = lookup(lit, kLitPrimary, br.buf);
                    if (kind_of(e) == kLiteral) {
                        br.drop(e.bits);
                        ob[opos++] = static_cast<std::uint8_t>(e.value);
                        if (br.overrun()) { result = truncated(); break; }
                        continue;
                    }
                }
                if (br.overrun()) { result = truncated(); break; }
                // `e` is the next symbol, decoded from bits that a refill leaves in place.
                if (br.cnt < kMaxCodeBits + 33) br.refill();
            }
            const unsigned kind = kind_of(e);
            br.drop(e.bits);
            if (kind == kLiteral) {
                if (br.overrun()) { result = truncated(); break; }
                if (opos >= ocap) {
                    if (opos >= max_out) { result = too_big(); break; }
                    opos_ = opos;
                    if (Status s = reserve(opos + 1); !s) { result = std::move(s); break; }
                    ob = out_.data();
                    ocap = out_.size() - kSlack;
                }
                ob[opos++] = static_cast<std::uint8_t>(e.value);
                continue;
            }
            if (kind == kEnd) {
                if (br.overrun()) result = truncated();
                break;
            }
            if (kind != kValue) {
                result = Status(Errc::Malformed, "deflate: invalid literal/length code", offset());
                break;
            }
            // Length extra bits (<= 5), distance code (<= 15) and distance extra bits (<= 13).
            if (br.cnt < 33) br.refill();
            const std::size_t len = e.value + br.take(extra_of(e));
            const Entry d = lookup(dist, kDistPrimary, br.buf);
            if (kind_of(d) != kValue) {
                result = Status(Errc::Malformed, "deflate: invalid distance code", offset());
                break;
            }
            br.drop(d.bits);
            const std::size_t distance = d.value + br.take(extra_of(d));
            if (br.overrun()) { result = truncated(); break; }
            if (distance > opos) {
                result = Status(Errc::Malformed, "deflate: distance before start of output", offset());
                break;
            }
            if (len > ocap - opos) {
                if (len > max_out - opos) { result = too_big(); break; }
                opos_ = opos;
                if (Status s = reserve(opos + len); !s) { result = std::move(s); break; }
                ob = out_.data();
                ocap = out_.size() - kSlack;
            }
            std::uint8_t* dst = ob + opos;
            const std::uint8_t* src = dst - distance;
            if (distance >= 8) {
                // Each 8-byte chunk reads only bytes already written; the overshoot lands in kSlack.
                for (std::size_t k = 0; k < len; k += 8) std::memcpy(dst + k, src + k, 8);
            } else if (distance == 1) {
                std::memset(dst, *src, len);
            } else {
                for (std::size_t k = 0; k < len; ++k) dst[k] = src[k];
            }
            opos += len;
        }
        br_ = br;
        opos_ = opos;
        return result;
    }

    const std::uint8_t* begin_;
    std::size_t max_out_;
    std::vector<std::uint8_t>& out_;
    std::size_t opos_ = 0;
    BitReader br_{};
    bool fixed_ready_ = false;
    std::array<Entry, kLitTableSize> lit_table_{};
    std::array<Entry, kDistTableSize> dist_table_{};
    std::array<Entry, kLitTableSize> fixed_lit_{};
    std::array<Entry, kDistTableSize> fixed_dist_{};
    std::array<Entry, kClTableSize> cl_table_{};
};

// ============================================================ deflate

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t value, unsigned bits) {
        acc_ |= static_cast<std::uint64_t>(value) << n_;
        n_ += bits;
        if (n_ >= 32) {
            std::uint8_t b[4];
            store_le32(b, static_cast<std::uint32_t>(acc_));
            out_.insert(out_.end(), b, b + 4);
            acc_ >>= 32;
            n_ -= 32;
        }
    }
    // Pads with zero bits to a byte boundary and writes out every buffered byte.
    void align() {
        while (n_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(acc_));
            acc_ >>= 8;
            n_ = n_ >= 8 ? n_ - 8 : 0;
        }
        acc_ = 0;
    }
    std::vector<std::uint8_t>& bytes() { return out_; }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t acc_ = 0;
    unsigned n_ = 0;
};

// Length-limited Huffman code lengths by package-merge (the coin collector formulation): over
// `limit` rounds, merge the symbols with the pairwise packages of the previous round; the first
// 2k-2 items of the last round select, by how often each symbol occurs in them, its code length.
// Always yields a complete code: a lone symbol gets a one-bit partner.
void limited_lengths(const std::uint32_t* freq, int n, int limit, std::uint8_t* lens) {
    std::fill(lens, lens + n, std::uint8_t{0});
    std::vector<int> syms;
    for (int i = 0; i < n; ++i)
        if (freq[i]) syms.push_back(i);
    if (syms.size() <= 1) {
        const int a = syms.empty() ? 0 : syms[0];
        lens[a] = 1;
        lens[a == 0 ? 1 : 0] = 1;
        return;
    }
    std::stable_sort(syms.begin(), syms.end(), [&](int x, int y) { return freq[x] < freq[y]; });

    struct Item {
        std::uint64_t weight;
        int sym;    // >= 0: a symbol; -1: a package of items `child` and `child + 1` one round back
        int child;
    };
    std::vector<std::vector<Item>> rounds(static_cast<std::size_t>(limit));
    for (int s : syms) rounds[0].push_back({freq[s], s, 0});
    for (int r = 1; r < limit; ++r) {
        const auto& prev = rounds[static_cast<std::size_t>(r - 1)];
        auto& cur = rounds[static_cast<std::size_t>(r)];
        const std::size_t packages = prev.size() / 2;
        std::size_t a = 0, b = 0;
        while (a < syms.size() || b < packages) {
            const bool take_symbol =
                b >= packages ||
                (a < syms.size() && freq[syms[a]] <= prev[2 * b].weight + prev[2 * b + 1].weight);
            if (take_symbol) {
                cur.push_back({freq[syms[a]], syms[a], 0});
                ++a;
            } else {
                cur.push_back({prev[2 * b].weight + prev[2 * b + 1].weight, -1, static_cast<int>(2 * b)});
                ++b;
            }
        }
    }
    struct Walk {
        const std::vector<std::vector<Item>>& rounds;
        std::uint8_t* lens;
        void visit(std::size_t r, std::size_t i) const {
            const Item& it = rounds[r][i];
            if (it.sym >= 0) {
                ++lens[it.sym];
                return;
            }
            visit(r - 1, static_cast<std::size_t>(it.child));
            visit(r - 1, static_cast<std::size_t>(it.child) + 1);
        }
    };
    const Walk walk{rounds, lens};
    const std::size_t take = 2 * syms.size() - 2;
    for (std::size_t i = 0; i < take; ++i) walk.visit(static_cast<std::size_t>(limit - 1), i);
}

struct LevelParams {
    int chain;             // candidates examined per position
    std::size_t good;      // a previous match this long quarters the search
    std::size_t nice;      // stop searching at a match this long
    std::size_t lazy_max;  // 0: greedy; else defer a match shorter than this by one byte when the
                           // next position matches longer
};
constexpr LevelParams kLevels[10] = {
    {0, 0, 0, 0},         {4, 4, 8, 0},         {8, 8, 16, 0},        {16, 8, 32, 0},
    {16, 8, 32, 8},       {32, 8, 64, 16},      {128, 8, 128, 16},    {256, 16, 192, 32},
    {1024, 32, 258, 128}, {4096, 32, 258, 258},
};

constexpr unsigned kHashBits = 15;
constexpr std::size_t kSymbolsPerBlock = 1u << 15;
constexpr std::uint32_t kMatchFlag = 0x80000000u;

inline std::uint32_t hash3(const std::uint8_t* p) noexcept {
    const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                            (static_cast<std::uint32_t>(p[2]) << 16);
    return (v * 0x9E3779B1u) >> (32 - kHashBits);
}

inline std::size_t common_prefix(const std::uint8_t* a, const std::uint8_t* b, std::size_t max) noexcept {
    std::size_t n = 0;
    while (n + 8 <= max) {
        const std::uint64_t x = load_le64(a + n) ^ load_le64(b + n);
        if (x) return n + static_cast<std::size_t>(std::countr_zero(x) / 8);
        n += 8;
    }
    while (n < max && a[n] == b[n]) ++n;
    return n;
}

class Deflater {
public:
    Deflater(const std::uint8_t* in, std::size_t n, int level, std::vector<std::uint8_t>& out)
        : in_(in), n_(n), lp_(kLevels[level]), bw_(out), head_(std::size_t{1} << kHashBits, 0),
          prev_(kWindowSize, 0) {
        syms_.reserve(kSymbolsPerBlock);
    }

    void run() {
        const bool lazy = lp_.lazy_max != 0;
        std::size_t i = 0;
        std::size_t prev_len = 0, prev_dist = 0;
        bool prev_pending = false;  // lazy mode: a literal/match decision for byte i-1 is pending
        while (i < n_) {
            std::size_t len = 0, dist = 0;
            if (!lazy || !prev_pending || prev_len < lp_.lazy_max) find(i, lazy && prev_pending ? prev_len : 0, len, dist);
            insert(i);
            if (!lazy) {
                if (len >= kMinMatch) {
                    emit_match(len, dist);
                    for (std::size_t k = 1; k < len; ++k) insert(i + k);
                    i += len;
                } else {
                    emit_literal(in_[i]);
                    ++i;
                }
            } else if (prev_pending && prev_len >= kMinMatch && len <= prev_len) {
                const std::size_t start = i - 1;
                emit_match(prev_len, prev_dist);
                for (std::size_t k = i + 1; k < start + prev_len; ++k) insert(k);
                i = start + prev_len;
                prev_pending = false;
                prev_len = 0;
            } else {
                if (prev_pending) emit_literal(in_[i - 1]);
                prev_pending = true;
                prev_len = len;
                prev_dist = dist;
                ++i;
            }
            // A pending lazy decision belongs to byte i-1, which stays out of this block.
            if (syms_.size() >= kSymbolsPerBlock) flush(false, prev_pending ? i - 1 : i);
        }
        if (prev_pending) {
            if (prev_len >= kMinMatch) emit_match(prev_len, prev_dist);  // cannot happen: len capped at n-i
            else emit_literal(in_[n_ - 1]);
        }
        flush(true, n_);
        bw_.align();
    }

    void stored_only() {
        std::size_t pos = 0;
        do {
            const std::size_t len = std::min(kMaxStored, n_ - pos);
            const bool last = pos + len == n_;
            write_stored(pos, len, last);
            pos += len;
        } while (pos < n_);
    }

private:
    void insert(std::size_t i) {
        if (i + kMinMatch > n_) return;
        const std::uint32_t h = hash3(in_ + i);
        prev_[i & (kWindowSize - 1)] = head_[h];
        head_[h] = i + 1;
    }

    // Longest match for position i that beats `beat`, within the window.
    void find(std::size_t i, std::size_t beat, std::size_t& best_len, std::size_t& best_dist) const {
        best_len = 0;
        best_dist = 0;
        if (i + kMinMatch > n_) return;
        const std::size_t max = std::min(kMaxMatch, n_ - i);
        std::size_t best = std::max<std::size_t>(beat, kMinMatch - 1);
        if (best >= max) return;
        int chain = lp_.chain;
        if (beat >= lp_.good) chain >>= 2;
        const std::size_t lowest = i > kWindowSize ? i - kWindowSize : 0;
        std::size_t cand = head_[hash3(in_ + i)];
        const std::uint8_t* cur = in_ + i;
        while (cand != 0 && chain-- > 0) {
            const std::size_t c = cand - 1;
            if (c < lowest) break;
            const std::uint8_t* m = in_ + c;
            if (m[best] == cur[best] && m[0] == cur[0]) {
                const std::size_t l = common_prefix(m, cur, max);
                if (l > best) {
                    best = l;
                    best_len = l;
                    best_dist = i - c;
                    if (l >= lp_.nice || l >= max) break;
                }
            }
            cand = prev_[c & (kWindowSize - 1)];
        }
        // A 3-byte match far back costs about as much as three literals.
        if (best_len == kMinMatch && best_dist > 8192) best_len = 0;
    }

    void emit_literal(std::uint8_t b) {
        syms_.push_back(b);
        ++lit_freq_[b];
    }
    void emit_match(std::size_t len, std::size_t dist) {
        syms_.push_back(kMatchFlag | (static_cast<std::uint32_t>(len - kMinMatch) << 16) |
                        static_cast<std::uint32_t>(dist - 1));
        ++lit_freq_[kFirstLengthSymbol + kAlpha.len_code[len]];
        ++dist_freq_[dist_code(static_cast<std::uint32_t>(dist))];
    }

    // Bits needed for the block's data with the given code lengths (end-of-block included).
    std::uint64_t data_bits(const std::uint8_t* ll, const std::uint8_t* dl) const {
        std::uint64_t bits = 0;
        for (int s = 0; s < 286; ++s) {
            std::uint64_t per = ll[s];
            if (s >= kFirstLengthSymbol) per += kAlpha.len_extra[s - kFirstLengthSymbol];
            bits += per * lit_freq_[s];
        }
        for (int s = 0; s < kDistCodes; ++s) bits += static_cast<std::uint64_t>(dl[s] + kAlpha.dist_extra[s]) * dist_freq_[s];
        return bits;
    }

    struct ClItem {
        std::uint8_t sym, extra_bits, extra_value;
    };

    // Run-length codes the concatenated code lengths with symbols 16, 17 and 18 (section 3.2.7).
    static void rle_lengths(const std::uint8_t* lens, unsigned n, std::vector<ClItem>& items) {
        unsigned i = 0;
        while (i < n) {
            const std::uint8_t v = lens[i];
            unsigned run = 1;
            while (i + run < n && lens[i + run] == v) ++run;
            i += run;
            if (v == 0) {
                while (run >= 11) {
                    const unsigned k = std::min(run, 138u);
                    items.push_back({18, 7, static_cast<std::uint8_t>(k - 11)});
                    run -= k;
                }
                if (run >= 3) {
                    items.push_back({17, 3, static_cast<std::uint8_t>(run - 3)});
                    run = 0;
                }
                for (; run > 0; --run) items.push_back({0, 0, 0});
            } else {
                items.push_back({v, 0, 0});
                --run;
                while (run >= 3) {
                    const unsigned k = std::min(run, 6u);
                    items.push_back({16, 2, static_cast<std::uint8_t>(k - 3)});
                    run -= k;
                }
                for (; run > 0; --run) items.push_back({v, 0, 0});
            }
        }
    }

    void write_stored(std::size_t pos, std::size_t len, bool last) {
        bw_.put(last ? 1u : 0u, 1);
        bw_.put(0, 2);
        bw_.align();
        auto& o = bw_.bytes();
        std::uint8_t hdr[4];
        store_le16(hdr, static_cast<std::uint16_t>(len));
        store_le16(hdr + 2, static_cast<std::uint16_t>(~len));
        o.insert(o.end(), hdr, hdr + 4);
        o.insert(o.end(), in_ + pos, in_ + pos + len);
    }

    void write_symbols(const std::uint16_t* lc, const std::uint8_t* ll, const std::uint16_t* dc, const std::uint8_t* dl) {
        for (const std::uint32_t s : syms_) {
            if (!(s & kMatchFlag)) {
                bw_.put(lc[s], ll[s]);
                continue;
            }
            const std::size_t len = ((s >> 16) & 0xFFu) + kMinMatch;
            const std::uint32_t dist = (s & 0xFFFFu) + 1;
            const int li = kAlpha.len_code[len];
            const int lsym = kFirstLengthSymbol + li;
            bw_.put(lc[lsym], ll[lsym]);
            if (kAlpha.len_extra[li]) bw_.put(static_cast<std::uint32_t>(len - kAlpha.len_base[li]), kAlpha.len_extra[li]);
            const int di = dist_code(dist);
            bw_.put(dc[di], dl[di]);
            if (kAlpha.dist_extra[di]) bw_.put(dist - kAlpha.dist_base[di], kAlpha.dist_extra[di]);
        }
        bw_.put(lc[kEndOfBlock], ll[kEndOfBlock]);
    }

    // Writes the symbols gathered for input [block_start_, end) as the cheapest block type.
    void flush(bool last, std::size_t end) {
        lit_freq_[kEndOfBlock] = 1;

        // Dynamic Huffman candidate.
        std::uint8_t dyn_ll[kLitLenSymbols] = {}, dyn_dl[kDistSymbols] = {};
        limited_lengths(lit_freq_, 286, kMaxCodeBits, dyn_ll);
        limited_lengths(dist_freq_, kDistCodes, kMaxCodeBits, dyn_dl);
        unsigned hlit = 286;
        while (hlit > 257 && dyn_ll[hlit - 1] == 0) --hlit;
        unsigned hdist = kDistCodes;
        while (hdist > 1 && dyn_dl[hdist - 1] == 0) --hdist;
        std::uint8_t all[kLitLenSymbols + kDistSymbols] = {};
        std::copy(dyn_ll, dyn_ll + hlit, all);
        std::copy(dyn_dl, dyn_dl + hdist, all + hlit);
        std::vector<ClItem> items;
        rle_lengths(all, hlit + hdist, items);
        std::uint32_t cl_freq[kClSymbols] = {};
        for (const ClItem& it : items) ++cl_freq[it.sym];
        std::uint8_t cl_lens[kClSymbols] = {};
        limited_lengths(cl_freq, kClSymbols, kMaxClBits, cl_lens);
        unsigned hclen = kClSymbols;
        while (hclen > 4 && cl_lens[kClOrder[hclen - 1]] == 0) --hclen;
        std::uint64_t dyn_bits = 3 + 14 + 3ull * hclen + data_bits(dyn_ll, dyn_dl);
        for (const ClItem& it : items) dyn_bits += cl_lens[it.sym] + it.extra_bits;

        // Fixed Huffman candidate.
        std::uint8_t fix_ll[kLitLenSymbols], fix_dl[kDistSymbols];
        for (int s = 0; s < kLitLenSymbols; ++s)
            fix_ll[s] = static_cast<std::uint8_t>(s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8);
        std::fill(fix_dl, fix_dl + kDistSymbols, std::uint8_t{5});
        const std::uint64_t fix_bits = 3 + data_bits(fix_ll, fix_dl);

        // Stored candidate (each stored block costs a header, up to 7 alignment bits and LEN/NLEN).
        const std::size_t raw = end - block_start_;
        const std::uint64_t pieces = raw == 0 ? 1 : (raw + kMaxStored - 1) / kMaxStored;
        const std::uint64_t stored_bits = 8ull * raw + pieces * (3 + 7 + 32);

        if (stored_bits <= fix_bits && stored_bits <= dyn_bits) {
            std::size_t pos = block_start_;
            do {
                const std::size_t len = std::min(kMaxStored, end - pos);
                write_stored(pos, len, last && pos + len == end);
                pos += len;
            } while (pos < end);
        } else if (fix_bits <= dyn_bits) {
            std::uint16_t lc[kLitLenSymbols], dc[kDistSymbols];
            canonical_codes(fix_ll, kLitLenSymbols, lc);
            canonical_codes(fix_dl, kDistSymbols, dc);
            bw_.put(last ? 1u : 0u, 1);
            bw_.put(1, 2);
            write_symbols(lc, fix_ll, dc, fix_dl);
        } else {
            std::uint16_t lc[kLitLenSymbols], dc[kDistSymbols], cc[kClSymbols];
            canonical_codes(dyn_ll, kLitLenSymbols, lc);
            canonical_codes(dyn_dl, kDistSymbols, dc);
            canonical_codes(cl_lens, kClSymbols, cc);
            bw_.put(last ? 1u : 0u, 1);
            bw_.put(2, 2);
            bw_.put(hlit - 257, 5);
            bw_.put(hdist - 1, 5);
            bw_.put(hclen - 4, 4);
            for (unsigned k = 0; k < hclen; ++k) bw_.put(cl_lens[kClOrder[k]], 3);
            for (const ClItem& it : items) {
                bw_.put(cc[it.sym], cl_lens[it.sym]);
                if (it.extra_bits) bw_.put(it.extra_value, it.extra_bits);
            }
            write_symbols(lc, dyn_ll, dc, dyn_dl);
        }

        syms_.clear();
        std::fill(std::begin(lit_freq_), std::end(lit_freq_), 0u);
        std::fill(std::begin(dist_freq_), std::end(dist_freq_), 0u);
        block_start_ = end;
    }

    const std::uint8_t* in_;
    std::size_t n_;
    LevelParams lp_;
    BitWriter bw_;
    std::vector<std::size_t> head_;  // hash -> most recent position + 1 (0 = none)
    std::vector<std::size_t> prev_;  // position & window mask -> previous position + 1 with that hash
    std::vector<std::uint32_t> syms_;
    std::uint32_t lit_freq_[kLitLenSymbols] = {};
    std::uint32_t dist_freq_[kDistSymbols] = {};
    std::size_t block_start_ = 0;
};

}  // namespace

Status inflate_raw(const std::uint8_t* in, std::size_t in_size, std::size_t max_output,
                   std::vector<std::uint8_t>& out, std::size_t* consumed) {
    out.clear();
    if (consumed) *consumed = 0;
    if (!in && in_size) return Status(Errc::InvalidArgument, "deflate: null input");
    static const std::uint8_t kEmpty = 0;
    if (!in) in = &kEmpty;
    std::unique_ptr<Inflater> inf;
    try {
        inf = std::make_unique<Inflater>(in, in_size, max_output, out);
    } catch (const std::bad_alloc&) {
        return Status(Errc::OutOfMemory, "deflate: cannot allocate decoder state");
    }
    Status s = inf->run(consumed);
    if (!s) out.clear();
    return s;
}

Status deflate_raw(const std::uint8_t* in, std::size_t in_size, int level, std::vector<std::uint8_t>& out) {
    out.clear();
    if (level < 0 || level > 9) return Status(Errc::InvalidArgument, "deflate: level must be 0..9");
    if (!in && in_size) return Status(Errc::InvalidArgument, "deflate: null input");
    static const std::uint8_t kEmpty = 0;
    if (!in) in = &kEmpty;
    try {
        std::uint64_t hint = 0;
        if (level == 0 ? checked_add(in_size, in_size / kMaxStored * 5 + 8, hint) : checked_add(in_size / 2, 64, hint))
            if (fits_size(hint)) out.reserve(static_cast<std::size_t>(hint));
        Deflater d(in, in_size, level, out);
        if (level == 0) d.stored_only();
        else d.run();
    } catch (const std::bad_alloc&) {
        out.clear();
        return Status(Errc::OutOfMemory, "deflate: out of memory");
    } catch (const std::length_error&) {
        out.clear();
        return Status(Errc::OutOfMemory, "deflate: out of memory");
    }
    return Status::ok();
}

}  // namespace siaara::codec
