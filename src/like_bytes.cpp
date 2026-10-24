// Independent byte-level LIKE matcher over UTF-8 text. Deliberately shares no
// code with the DFA-based matcher so that it can detect matcher bugs.
//
// General algorithm: the pattern is split on '%' into segments. Each segment
// has a fixed length in code points, so
//   * the first segment is matched anchored at the start,
//   * the last segment is matched anchored at the end (walking backwards),
//   * every middle segment takes its leftmost occurrence between the two.
// Leftmost is optimal: advancing a fixed number of code points from an
// earlier boundary never ends later, so it leaves the most room for the rest.
#include "tokenlike/baselines.h"

namespace {

constexpr size_t npos = static_cast<size_t>(-1);

inline size_t cp_len(unsigned char c) {
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

inline bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

LikePlan LikePlan::compile(std::string_view pattern) {
    LikePlan p;
    p.pattern_ = std::string(pattern);

    std::vector<std::string> parts(1);
    bool has_underscore = false;
    for (char ch : pattern) {
        if (ch == '%') {
            parts.emplace_back();
            p.has_pct_ = true;
        } else {
            if (ch == '_') has_underscore = true;
            parts.back().push_back(ch);
        }
    }

    const std::string& first = parts.front();
    const std::string& last = parts.back();
    std::vector<std::string> mids;
    for (size_t i = 1; i + 1 < parts.size(); ++i)
        if (!parts[i].empty()) mids.push_back(parts[i]);

    if (!has_underscore) {
        if (!p.has_pct_) {
            p.kind_ = Kind::Exact;
            p.a_ = first;
            return p;
        }
        if (mids.empty()) {
            if (first.empty() && last.empty()) {
                p.kind_ = Kind::All;
            } else if (first.empty()) {
                p.kind_ = Kind::Suffix;
                p.b_ = last;
            } else if (last.empty()) {
                p.kind_ = Kind::Prefix;
                p.a_ = first;
            } else {
                p.kind_ = Kind::PrefixSuffix;
                p.a_ = first;
                p.b_ = last;
            }
            return p;
        }
        if (mids.size() == 1 && first.empty() && last.empty()) {
            p.kind_ = Kind::Contains;
            p.a_ = mids[0];
            return p;
        }
    }

    auto to_segment = [](const std::string& text) {
        Segment seg;
        for (char ch : text) {
            if (ch == '_') {
                if (seg.empty() || !seg.back().lit.empty()) seg.push_back(Atom{});
                seg.back().any++;
            } else {
                if (seg.empty() || seg.back().lit.empty()) seg.push_back(Atom{});
                seg.back().lit.push_back(ch);
            }
        }
        return seg;
    };

    p.kind_ = Kind::General;
    p.segs_.push_back(to_segment(first));
    if (p.has_pct_) {
        for (const auto& m : mids) p.segs_.push_back(to_segment(m));
        p.segs_.push_back(to_segment(last));
    }
    return p;
}

// Match `seg` starting at `pos`; returns the end position or npos.
size_t LikePlan::seg_forward(const Segment& seg, std::string_view s, size_t pos) {
    const size_t n = s.size();
    for (const Atom& a : seg) {
        if (!a.lit.empty()) {
            const size_t k = a.lit.size();
            if (n - pos < k || !tl_eq_bytes(s.data() + pos, a.lit.data(), k)) return npos;
            pos += k;
        } else {
            for (uint32_t j = 0; j < a.any; ++j) {
                if (pos >= n) return npos;
                pos += cp_len(static_cast<unsigned char>(s[pos]));
                if (pos > n) return npos;
            }
        }
    }
    return pos;
}

// Match `seg` ending exactly at `end`; returns the start position or npos.
size_t LikePlan::seg_backward(const Segment& seg, std::string_view s, size_t end) {
    for (auto it = seg.rbegin(); it != seg.rend(); ++it) {
        const Atom& a = *it;
        if (!a.lit.empty()) {
            const size_t k = a.lit.size();
            if (end < k || !tl_eq_bytes(s.data() + end - k, a.lit.data(), k)) return npos;
            end -= k;
        } else {
            for (uint32_t j = 0; j < a.any; ++j) {
                if (end == 0) return npos;
                --end;
                while (end > 0 && is_cont(static_cast<unsigned char>(s[end]))) --end;
            }
        }
    }
    return end;
}

// Leftmost occurrence of `seg` starting at or after `from` (a code-point
// boundary) and ending at or before `limit`. Returns the start or npos.
size_t LikePlan::seg_find(const Segment& seg, std::string_view s, size_t from, size_t limit,
                          size_t* match_end) {
    if (seg.empty()) {
        *match_end = from;
        return from;
    }
    const Atom& head = seg.front();
    size_t p = from;
    while (p <= limit) {
        if (!head.lit.empty()) {
            const char* hit = find_lit(s.data() + p, limit - p, head.lit);
            if (!hit) return npos;
            p = static_cast<size_t>(hit - s.data());
        }
        const size_t e = seg_forward(seg, s, p);
        if (e != npos) {
            if (e <= limit) {
                *match_end = e;
                return p;
            }
            return npos;  // later starts cannot end earlier
        }
        if (p >= limit) return npos;
        p += head.lit.empty() ? cp_len(static_cast<unsigned char>(s[p])) : 1;
    }
    return npos;
}

bool LikePlan::match_general(std::string_view s) const {
    if (!has_pct_) return seg_forward(segs_[0], s, 0) == s.size();

    size_t pos = seg_forward(segs_.front(), s, 0);
    if (pos == npos) return false;
    const size_t tail = seg_backward(segs_.back(), s, s.size());
    if (tail == npos || tail < pos) return false;

    for (size_t i = 1; i + 1 < segs_.size(); ++i) {
        size_t e;
        if (seg_find(segs_[i], s, pos, tail, &e) == npos) return false;
        pos = e;
    }
    return true;
}

bool like_match(std::string_view pattern, std::string_view s) {
    return LikePlan::compile(pattern).match(s);
}
