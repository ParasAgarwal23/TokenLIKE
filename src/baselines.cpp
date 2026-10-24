// Raw-string and fused decode-then-match baselines. Both dispatch on the
// pattern kind once per scan so the per-row loop calls an inlined fast path.
#include "tokenlike/baselines.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {

using Kind = LikePlan::Kind;

// Calls fn(matcher) with a per-kind callable `bool(std::string_view)`.
template <class Fn>
void dispatch(const LikePlan& p, Fn&& fn) {
    switch (p.kind()) {
        case Kind::Exact:        fn([&p](std::string_view v) { return p.match_exact(v); }); break;
        case Kind::Prefix:       fn([&p](std::string_view v) { return p.match_prefix(v); }); break;
        case Kind::Suffix:       fn([&p](std::string_view v) { return p.match_suffix(v); }); break;
        case Kind::Contains:     fn([&p](std::string_view v) { return p.match_contains(v); }); break;
        case Kind::PrefixSuffix: fn([&p](std::string_view v) { return p.match_prefix_suffix(v); }); break;
        case Kind::All:          fn([](std::string_view) { return true; }); break;
        case Kind::General:      fn([&p](std::string_view v) { return p.match_general(v); }); break;
    }
}

// Fills a bit-packed bitmap with row(i) for every row; returns the match count.
template <class RowFn>
uint64_t fill_bitmap(uint64_t rows, Bitmap& out, RowFn&& row) {
    out.assign(bitmap_bytes(rows), 0);
    uint8_t* o = out.data();
    uint64_t matches = 0, i = 0;
    for (uint64_t blk = 0; blk < rows / 8; ++blk) {
        unsigned byte = 0;
        for (unsigned k = 0; k < 8; ++k, ++i) byte |= static_cast<unsigned>(row(i)) << k;
        o[blk] = static_cast<uint8_t>(byte);
        matches += static_cast<uint64_t>(__builtin_popcount(byte));
    }
    if (i < rows) {
        unsigned byte = 0;
        for (unsigned k = 0; i < rows; ++k, ++i) byte |= static_cast<unsigned>(row(i)) << k;
        o[rows / 8] = static_cast<uint8_t>(byte);
        matches += static_cast<uint64_t>(__builtin_popcount(byte));
    }
    return matches;
}

double elapsed_us(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
        .count();
}

}  // namespace

RawMatcher RawMatcher::build(std::string_view pattern) {
    RawMatcher m;
    m.plan_ = LikePlan::compile(pattern);
    return m;
}

void RawMatcher::match(const Column& c, Bitmap& out, Stats& s) const {
    const uint64_t rows = c.rows;
    const char* raw = reinterpret_cast<const char*>(c.raw.data());
    const uint64_t* ro = c.rawoff.data();
    uint64_t matches = 0;

    dispatch(plan_, [&](auto f) {
        matches = fill_bitmap(rows, out, [&](uint64_t i) {
            return f(std::string_view(raw + ro[i], ro[i + 1] - ro[i]));
        });
    });

    s.rows += rows;
    s.matches += matches;
    s.bytes_compared += rows ? ro[rows] - ro[0] : 0;
}

DecodeMatcher DecodeMatcher::build(std::string_view pattern, const Vocab& v) {
    DecodeMatcher m;
    m.plan_ = LikePlan::compile(pattern);
    m.padded_.assign(v.bytes.begin(), v.bytes.end());
    m.padded_.resize(v.bytes.size() + 16, 0);
    m.toff_.assign(v.off.begin(), v.off.end());
    for (uint32_t t = 0; t < v.n; ++t)
        m.max_tok_ = std::max(m.max_tok_, v.off[t + 1] - v.off[t]);
    m.buf_.resize(4096);
    return m;
}

void DecodeMatcher::match(const Column& c, Bitmap& out, Stats& s) const {
    const uint64_t rows = c.rows;
    const uint16_t* tok = c.tok.data();
    const uint64_t* off = c.off.data();
    const uint8_t* vb = padded_.data();
    const uint32_t* to = toff_.data();
    const size_t max_tok = max_tok_;
    uint64_t matches = 0, decoded = 0;

    dispatch(plan_, [&](auto f) {
        char* buf = buf_.data();
        size_t cap = buf_.size();
        matches = fill_bitmap(rows, out, [&](uint64_t i) {
            const uint64_t b = off[i], e = off[i + 1];
            const size_t need = static_cast<size_t>(e - b) * max_tok + 16;
            if (need > cap) {
                buf_.resize(std::max(need, 2 * cap));
                buf = buf_.data();
                cap = buf_.size();
            }
            size_t len = 0;
            for (uint64_t j = b; j < e; ++j) {
                const uint32_t o = to[tok[j]];
                const uint32_t l = to[tok[j] + 1] - o;
                if (l <= 16)
                    std::memcpy(buf + len, vb + o, 16);  // padded: safe over-copy
                else
                    std::memcpy(buf + len, vb + o, l);
                len += l;
            }
            decoded += len;
            return f(std::string_view(buf, len));
        });
    });

    s.rows += rows;
    s.matches += matches;
    s.tokens_read += rows ? off[rows] - off[0] : 0;
    s.bytes_decoded += decoded;
    s.bytes_compared += decoded;
}

namespace {

void check_aligned(const Column& a, const Column& b) {
    if (a.rows != b.rows)
        throw std::invalid_argument("match_concat: columns have different row counts");
}

// Grows `buf` to at least `need` bytes; returns its data pointer.
inline char* reserve(std::vector<char>& buf, size_t need) {
    if (need > buf.size()) buf.resize(std::max(need, 2 * buf.size()));
    return buf.data();
}

}  // namespace

void RawMatcher::match_concat(const Column& a, const Column& b, Bitmap& out, Stats& s) const {
    check_aligned(a, b);
    const uint64_t rows = a.rows;
    const char* ra = reinterpret_cast<const char*>(a.raw.data());
    const char* rb = reinterpret_cast<const char*>(b.raw.data());
    const uint64_t* oa = a.rawoff.data();
    const uint64_t* ob = b.rawoff.data();
    if (buf_.empty()) buf_.resize(4096);
    uint64_t matches = 0;

    dispatch(plan_, [&](auto f) {
        matches = fill_bitmap(rows, out, [&](uint64_t i) {
            const size_t la = oa[i + 1] - oa[i], lb = ob[i + 1] - ob[i];
            char* buf = reserve(buf_, la + lb);
            std::memcpy(buf, ra + oa[i], la);
            std::memcpy(buf + la, rb + ob[i], lb);
            return f(std::string_view(buf, la + lb));
        });
    });

    const uint64_t bytes = rows ? (oa[rows] - oa[0]) + (ob[rows] - ob[0]) : 0;
    s.rows += rows;
    s.matches += matches;
    s.bytes_compared += bytes;
}

void DecodeMatcher::match_concat(const Column& a, const Column& b, Bitmap& out,
                                 Stats& s) const {
    check_aligned(a, b);
    const uint64_t rows = a.rows;
    const uint8_t* vb = padded_.data();
    const uint32_t* to = toff_.data();
    const size_t max_tok = max_tok_;
    uint64_t matches = 0, decoded = 0;

    // Appends the decoded tokens [tb, te) of `tok` at buf + len; returns the new length.
    auto decode = [&](char* buf, size_t len, const uint16_t* tok, uint64_t tb, uint64_t te) {
        for (uint64_t j = tb; j < te; ++j) {
            const uint32_t o = to[tok[j]];
            const uint32_t l = to[tok[j] + 1] - o;
            if (l <= 16)
                std::memcpy(buf + len, vb + o, 16);  // padded: safe over-copy
            else
                std::memcpy(buf + len, vb + o, l);
            len += l;
        }
        return len;
    };

    dispatch(plan_, [&](auto f) {
        matches = fill_bitmap(rows, out, [&](uint64_t i) {
            const uint64_t a0 = a.off[i], a1 = a.off[i + 1], b0 = b.off[i], b1 = b.off[i + 1];
            char* buf = reserve(buf_, static_cast<size_t>((a1 - a0) + (b1 - b0)) * max_tok + 16);
            size_t len = decode(buf, 0, a.tok.data(), a0, a1);
            len = decode(buf, len, b.tok.data(), b0, b1);
            decoded += len;
            return f(std::string_view(buf, len));
        });
    });

    s.rows += rows;
    s.matches += matches;
    s.tokens_read += rows ? (a.off[rows] - a.off[0]) + (b.off[rows] - b.off[0]) : 0;
    s.bytes_decoded += decoded;
    s.bytes_compared += decoded;
}

void raw_match(std::string_view pattern, const Column& c, Bitmap& out, Stats& s) {
    const auto t0 = std::chrono::steady_clock::now();
    const RawMatcher m = RawMatcher::build(pattern);
    s.build_us = elapsed_us(t0);
    m.match(c, out, s);
}

void decode_match(std::string_view pattern, const Vocab& v, const Column& c, Bitmap& out,
                  Stats& s) {
    const auto t0 = std::chrono::steady_clock::now();
    const DecodeMatcher m = DecodeMatcher::build(pattern, v);
    s.build_us = elapsed_us(t0);
    m.match(c, out, s);
}

void raw_match_concat(std::string_view pattern, const Column& a, const Column& b, Bitmap& out,
                      Stats& s) {
    const auto t0 = std::chrono::steady_clock::now();
    const RawMatcher m = RawMatcher::build(pattern);
    s.build_us = elapsed_us(t0);
    m.match_concat(a, b, out, s);
}

void decode_match_concat(std::string_view pattern, const Vocab& v, const Column& a,
                         const Column& b, Bitmap& out, Stats& s) {
    const auto t0 = std::chrono::steady_clock::now();
    const DecodeMatcher m = DecodeMatcher::build(pattern, v);
    s.build_us = elapsed_us(t0);
    m.match_concat(a, b, out, s);
}
