#include "tokenlike/matcher.h"

#include <algorithm>
#include <chrono>
#include <string>

#include "tokenlike/pattern.h"

namespace tokenlike::detail {

namespace {

Dfa build_dfa(const Pattern& p, Dir dir, const BuildOptions& opt) {
    Nfa n = build_nfa(p);
    if (dir == Dir::Backward) n = reverse_nfa(n);
    Dfa d = determinize(n, std::max(opt.max_subset_states, opt.max_states));
    if (opt.minimize) d = minimize(d);
    if (d.S > opt.max_states)
        throw TooComplex("pattern needs " + std::to_string(d.S) + " DFA states (limit " +
                         std::to_string(opt.max_states) + ")");
    return d;
}

ScanKind kind_for(const Pattern& p, Dir dir) {
    if (dir == Dir::Backward) return ScanKind::Backward;
    if (!p.starts_with_star() && !p.ends_with_star()) return ScanKind::Anchored;
    return ScanKind::Forward;
}

}  // namespace

Matcher MatcherAccess::build(std::string_view pattern, const Vocab& v, const BuildOptions& opt,
                             DenseTable* keep_dense) {
    auto t0 = std::chrono::steady_clock::now();
    Pattern p = parse_like(pattern);

    Dir pref = (p.starts_with_star() && !p.ends_with_star()) ? Dir::Backward : Dir::Forward;
    if (opt.force_dir >= 0) pref = opt.force_dir ? Dir::Backward : Dir::Forward;

    Dir dir = pref;
    Dfa d;
    try {
        d = build_dfa(p, dir, opt);
    } catch (const TooComplex&) {
        if (opt.force_dir >= 0) throw;
        dir = (pref == Dir::Forward) ? Dir::Backward : Dir::Forward;
        d = build_dfa(p, dir, opt);
    }

    DenseTable dense = lift(d, v, dir);
    CompactTable tab = compress(dense);
    verify(tab, dense);

    Matcher m;
    m.kind_ = kind_for(p, dir);
    m.dir_ = dir;
    m.flags_ = d.flags;
    m.start_ = d.start;
    m.vocab_n_ = v.n;
    m.dense_bytes_ = dense.bytes();
    m.tab_ = std::move(tab);
    if (keep_dense) *keep_dense = std::move(dense);
    m.build_us_ = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return m;
}

void MatcherAccess::match_with(const Matcher& m, const DenseTable* dense, bool early_exit, const Column& a,
                               const Column* b, Bitmap& out, Stats& s) {
    validate_shape(a);
    if (b) {
        validate_shape(*b);
        if (b->rows != a.rows) throw std::invalid_argument("match_concat: columns have different row counts");
    }
#ifdef TOKENLIKE_CHECKED
    validate_tokens(a, m.vocab_n_);
    if (b) validate_tokens(*b, m.vocab_n_);
#endif
    ScanSpec sp{m.flags_.data(), m.start_, m.dir_, early_exit};
    if (dense)
        scan(*dense, sp, a, b, out, s);
    else
        scan(m.tab_, sp, a, b, out, s);
    s.build_us = m.build_us_;
    s.table_kb = static_cast<double>(m.table_bytes()) / 1024.0;
}

void validate_shape(const Column& c) {
    if (c.off.size() != c.rows + 1) throw std::invalid_argument("column offsets must have rows + 1 entries");
    if (c.off[c.rows] > c.tok.size()) throw std::invalid_argument("column offsets exceed token count");
}

void validate_tokens(const Column& c, uint32_t n) {
    for (size_t i = 0; i < c.tok.size(); ++i)
        if (c.tok[i] >= n)
            throw std::out_of_range("token id " + std::to_string(c.tok[i]) + " at position " + std::to_string(i) +
                                    " is outside the vocabulary");
}

}  // namespace tokenlike::detail

Matcher Matcher::build(std::string_view pattern, const Vocab& v) {
    return tokenlike::detail::MatcherAccess::build(pattern, v, {});
}

void Matcher::match(const Column& c, Bitmap& out, Stats& s) const {
    tokenlike::detail::MatcherAccess::match_with(*this, nullptr, true, c, nullptr, out, s);
}

void Matcher::match_concat(const Column& a, const Column& b, Bitmap& out, Stats& s) const {
    tokenlike::detail::MatcherAccess::match_with(*this, nullptr, true, a, &b, out, s);
}

ScanKind Matcher::kind() const { return kind_; }

size_t Matcher::table_bytes() const { return tab_.bytes() + flags_.size(); }

size_t Matcher::dense_table_bytes() const { return dense_bytes_; }
