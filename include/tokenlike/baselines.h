// Person C baselines: an independent byte-level LIKE matcher (no DFA) and the
// raw-string / fused decode-then-match scans used as TokenLIKE's competitors.
#pragma once

#include "tokenlike/common.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// Byte equality for short runtime lengths, inlined (library memcmp calls
// dominate for the 2-20 byte literals typical of URL patterns).
inline bool tl_eq_bytes(const char* a, const char* b, size_t n) {
    while (n >= 8) {
        uint64_t x, y;
        std::memcpy(&x, a, 8);
        std::memcpy(&y, b, 8);
        if (x != y) return false;
        a += 8, b += 8, n -= 8;
    }
    if (n >= 4) {
        uint32_t x0, y0, x1, y1;
        std::memcpy(&x0, a, 4), std::memcpy(&y0, b, 4);
        std::memcpy(&x1, a + n - 4, 4), std::memcpy(&y1, b + n - 4, 4);
        return x0 == y0 && x1 == y1;
    }
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// Compiled LIKE pattern. Semantics as documented in common.h.
class LikePlan {
public:
    enum class Kind {
        Exact,         // 'lit'        (no wildcards)
        Prefix,        // 'lit%'
        Suffix,        // '%lit'
        Contains,      // '%lit%'
        PrefixSuffix,  // 'lit1%lit2'
        All,           // '%', '%%', ...
        General        // anything containing '_' or more than one inner segment
    };

    static LikePlan compile(std::string_view pattern);

    Kind kind() const { return kind_; }
    const std::string& pattern() const { return pattern_; }

    bool match(std::string_view s) const {
        switch (kind_) {
            case Kind::Exact:        return match_exact(s);
            case Kind::Prefix:       return match_prefix(s);
            case Kind::Suffix:       return match_suffix(s);
            case Kind::Contains:     return match_contains(s);
            case Kind::PrefixSuffix: return match_prefix_suffix(s);
            case Kind::All:          return true;
            case Kind::General:      return match_general(s);
        }
        return false;
    }

    // Per-kind fast paths; callers may dispatch once outside a row loop.
    bool match_exact(std::string_view s) const {
        return s.size() == a_.size() && tl_eq_bytes(s.data(), a_.data(), a_.size());
    }
    bool match_prefix(std::string_view s) const {
        return s.size() >= a_.size() && tl_eq_bytes(s.data(), a_.data(), a_.size());
    }
    bool match_suffix(std::string_view s) const {
        return s.size() >= b_.size() &&
               tl_eq_bytes(s.data() + s.size() - b_.size(), b_.data(), b_.size());
    }
    bool match_prefix_suffix(std::string_view s) const {
        return s.size() >= a_.size() + b_.size() &&
               tl_eq_bytes(s.data(), a_.data(), a_.size()) &&
               tl_eq_bytes(s.data() + s.size() - b_.size(), b_.data(), b_.size());
    }
    bool match_contains(std::string_view s) const {
        return find_lit(s.data(), s.size(), a_) != nullptr;
    }
    bool match_general(std::string_view s) const;

private:
    // A segment is the text between two '%': literal runs and '_' counts.
    struct Atom {
        std::string lit;     // non-empty for a literal atom
        uint32_t any = 0;    // number of '_' for a wildcard atom (lit empty)
    };
    using Segment = std::vector<Atom>;

    // Leftmost occurrence of `lit` in p[0..n). Filters candidates on the first
    // and last literal byte (16 positions at a time with SSE2), then verifies.
    // Never reads outside p[0..n).
    static const char* find_lit(const char* p, size_t n, const std::string& lit) {
        const size_t k = lit.size();
        if (k == 0) return p;
        if (n < k) return nullptr;
        const char* L = lit.data();
        const char c0 = L[0], c1 = L[k - 1];
        size_t i = 0;
#if defined(__SSE2__)
        const __m128i f = _mm_set1_epi8(c0), l = _mm_set1_epi8(c1);
        for (; i + k - 1 + 16 <= n; i += 16) {
            const __m128i bf = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
            const __m128i bl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + k - 1));
            unsigned mask = static_cast<unsigned>(
                _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(f, bf), _mm_cmpeq_epi8(l, bl))));
            while (mask) {
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
                if (k <= 2 || tl_eq_bytes(p + i + bit + 1, L + 1, k - 2)) return p + i + bit;
                mask &= mask - 1;
            }
        }
#endif
        for (; i + k <= n; ++i)
            if (p[i] == c0 && p[i + k - 1] == c1 && (k <= 2 || tl_eq_bytes(p + i + 1, L + 1, k - 2)))
                return p + i;
        return nullptr;
    }

    static size_t seg_forward(const Segment& seg, std::string_view s, size_t pos);
    static size_t seg_backward(const Segment& seg, std::string_view s, size_t end);
    static size_t seg_find(const Segment& seg, std::string_view s, size_t from, size_t limit,
                           size_t* match_end);

    Kind kind_ = Kind::Exact;
    std::string pattern_;
    std::string a_, b_;              // literals for the fast paths
    bool has_pct_ = false;           // pattern contains at least one '%'
    std::vector<Segment> segs_;      // General: segments split on '%'
};

bool like_match(std::string_view pattern, std::string_view s);

// Raw-string baseline: LIKE over the original UTF-8 bytes (Column.raw).
class RawMatcher {
public:
    static RawMatcher build(std::string_view pattern);
    void match(const Column& c, Bitmap& out, Stats& s) const;
    const LikePlan& plan() const { return plan_; }

private:
    LikePlan plan_;
};

// Fused decode-then-match baseline: decode each row's tokens into a reusable
// buffer, then run the same LikePlan on the decoded text.
class DecodeMatcher {
public:
    static DecodeMatcher build(std::string_view pattern, const Vocab& v);
    void match(const Column& c, Bitmap& out, Stats& s) const;
    const LikePlan& plan() const { return plan_; }

private:
    LikePlan plan_;
    std::vector<uint8_t> padded_;   // vocab bytes + 16 bytes slack for wide copies
    std::vector<uint32_t> toff_;    // token start offsets (n + 1)
    uint32_t max_tok_ = 0;
    mutable std::vector<char> buf_; // reused across rows and calls
};

// One-shot convenience wrappers (build + match). Build time lands in s.build_us.
void raw_match(std::string_view pattern, const Column& c, Bitmap& out, Stats& s);
void decode_match(std::string_view pattern, const Vocab& v, const Column& c, Bitmap& out,
                  Stats& s);
