// Raw-string and fused decode-then-match baselines. Both dispatch on the
// pattern kind once per scan so the per-row loop calls an inlined fast path.
#include "tokenlike/baselines.h"

#include <algorithm>
#include <chrono>

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
    out.assign(rows, 0);
    const char* raw = reinterpret_cast<const char*>(c.raw.data());
    const uint64_t* ro = c.rawoff.data();
    uint64_t matches = 0;

    dispatch(plan_, [&](auto f) {
        for (uint64_t i = 0; i < rows; ++i) {
            const uint8_t m = f(std::string_view(raw + ro[i], ro[i + 1] - ro[i])) ? 1 : 0;
            out[i] = m;
            matches += m;
        }
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
    out.assign(rows, 0);
    const uint16_t* tok = c.tok.data();
    const uint64_t* off = c.off.data();
    const uint8_t* vb = padded_.data();
    const uint32_t* to = toff_.data();
    const size_t max_tok = max_tok_;
    uint64_t matches = 0, decoded = 0;

    dispatch(plan_, [&](auto f) {
        char* buf = buf_.data();
        size_t cap = buf_.size();
        for (uint64_t i = 0; i < rows; ++i) {
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
            const uint8_t m = f(std::string_view(buf, len)) ? 1 : 0;
            out[i] = m;
            matches += m;
        }
    });

    s.rows += rows;
    s.matches += matches;
    s.tokens_read += rows ? off[rows] - off[0] : 0;
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
