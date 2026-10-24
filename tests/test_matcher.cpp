// TokenLIKE matcher core tests (Person A). Self-contained: no test framework, no external data.
//
// Oracle: an independent dynamic-programming LIKE matcher over decoded UTF-8 code points.
// Fixtures: a toy vocabulary containing all 256 single bytes plus multi-byte tokens that split
// UTF-8 characters and straddle pattern literals; rows are random segmentations of strings.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "tokenlike/dfa.h"
#include "tokenlike/matcher.h"
#include "tokenlike/pattern.h"

using namespace tokenlike;
using detail::BuildOptions;
using detail::MatcherAccess;

// ---------------------------------------------------------------- harness

static long g_checks = 0, g_fail = 0;

#define CHECK_MSG(cond, msg)                                                              \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_fail;                                                                     \
            if (g_fail <= 40)                                                             \
                std::printf("  FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #cond,        \
                            std::string(msg).c_str());                                    \
        }                                                                                 \
    } while (0)
#define CHECK(cond) CHECK_MSG(cond, "")
#define CHECK_THROWS(expr, Ex)                                                            \
    do {                                                                                  \
        bool thrown_ = false;                                                             \
        try { expr; } catch (const Ex&) { thrown_ = true; }                               \
        CHECK_MSG(thrown_, "expected " #Ex " from " #expr);                               \
    } while (0)

// UTF-8 samples (split so that hex escapes never swallow following characters).
static const std::string E2 = "\xC3\xA9";              // e-acute, 2 bytes
static const std::string EU = "\xE2\x82\xAC";          // euro sign, 3 bytes
static const std::string EM = "\xF0\x9F\x98\x80";      // emoji, 4 bytes

// Printable form of a byte string for failure messages.
static std::string show(const std::string& s) {
    std::string o;
    char buf[8];
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F) o += static_cast<char>(c);
        else { std::snprintf(buf, sizeof buf, "\\x%02X", c); o += buf; }
    }
    return o;
}

// ---------------------------------------------------------------- oracle

static size_t u8len(uint8_t c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

static std::vector<std::string> chars_of(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t l = u8len(static_cast<uint8_t>(s[i]));
        out.push_back(s.substr(i, l));
        i += l;
    }
    return out;
}

struct RefPat {
    std::vector<int> kind;          // 0 literal code point, 1 '_', 2 '%'
    std::vector<std::string> ch;
};

static RefPat ref_parse(const std::string& p) {
    RefPat r;
    for (const std::string& c : chars_of(p)) {
        r.kind.push_back(c == "%" ? 2 : c == "_" ? 1 : 0);
        r.ch.push_back(c);
    }
    return r;
}

static bool ref_match(const std::vector<std::string>& s, const RefPat& p) {
    size_t n = s.size(), m = p.kind.size();
    std::vector<char> prev(n + 1, 0), cur(n + 1, 0);
    prev[0] = 1;
    for (size_t j = 1; j <= m; ++j) {
        int k = p.kind[j - 1];
        cur[0] = (k == 2) ? prev[0] : 0;
        for (size_t i = 1; i <= n; ++i) {
            if (k == 2) cur[i] = prev[i] || cur[i - 1];
            else if (k == 1) cur[i] = prev[i - 1];
            else cur[i] = prev[i - 1] && s[i - 1] == p.ch[j - 1];
        }
        prev.swap(cur);
    }
    return prev[n];
}

static bool like_ref(const std::string& s, const std::string& p) { return ref_match(chars_of(s), ref_parse(p)); }

// ---------------------------------------------------------------- fixtures

static Vocab make_vocab(const std::vector<std::string>& toks) {
    Vocab v;
    v.n = static_cast<uint32_t>(toks.size());
    v.off.push_back(0);
    for (const std::string& t : toks) {
        v.bytes.insert(v.bytes.end(), t.begin(), t.end());
        v.off.push_back(static_cast<uint32_t>(v.bytes.size()));
    }
    return v;
}

static std::string tok_str(const Vocab& v, uint16_t id) { return std::string(v.token(id)); }

static std::vector<std::string> toy_tokens() {
    std::vector<std::string> t;
    for (int b = 0; b < 256; ++b) t.push_back(std::string(1, static_cast<char>(b)));
    const std::vector<std::string> multi = {
        "/static/", "/sta", "tic", "tic/", "/st", "atic", "static", "/static", ".m3u8", ".m3", "u8", "3u8",
        "m3u", "google", "goo", "gle", "oogle", "/search", "/sea", "rch", "search", "/v", "v1/", "/v1/",
        "/a", "a/", "ab", "ba", "aa", "bb", "abc", "c/", "x.js", ".js", "www.", ".com",
        E2, "\xA9" "a", "a\xC3", "\xE2\x82", "\x82\xAC", EU, "\xAC/", "\xF0\x9F", "\x98\x80",
        "\xF0\x9F\x98", "\x80" "b", E2 + EU, "/v" + E2, E2 + "/", ""};
    t.insert(t.end(), multi.begin(), multi.end());
    return t;
}

// Random segmentation of a string into vocabulary tokens (any segmentation is valid input).
struct Segmenter {
    std::map<std::string, uint16_t> ids;
    size_t maxlen = 1;
    int empty_id = -1;

    explicit Segmenter(const Vocab& v) {
        for (uint32_t i = 0; i < v.n; ++i) {
            std::string s = tok_str(v, static_cast<uint16_t>(i));
            if (s.empty()) { if (empty_id < 0) empty_id = static_cast<int>(i); continue; }
            ids.emplace(s, static_cast<uint16_t>(i));
            maxlen = std::max(maxlen, s.size());
        }
    }
    std::vector<uint16_t> random(const std::string& s, std::mt19937& rng) const {
        std::vector<uint16_t> out;
        size_t i = 0;
        while (i < s.size()) {
            if (empty_id >= 0 && rng() % 8 == 0) out.push_back(static_cast<uint16_t>(empty_id));
            std::vector<std::pair<size_t, uint16_t>> cand;
            for (size_t l = 1; l <= maxlen && i + l <= s.size(); ++l) {
                auto it = ids.find(s.substr(i, l));
                if (it != ids.end()) cand.push_back({l, it->second});
            }
            auto pick = cand[rng() % cand.size()];
            out.push_back(pick.second);
            i += pick.first;
        }
        if (empty_id >= 0 && rng() % 8 == 0) out.push_back(static_cast<uint16_t>(empty_id));
        return out;
    }
    std::vector<uint16_t> greedy(const std::string& s) const {
        std::vector<uint16_t> out;
        size_t i = 0;
        while (i < s.size()) {
            for (size_t l = std::min(maxlen, s.size() - i); l >= 1; --l) {
                auto it = ids.find(s.substr(i, l));
                if (it != ids.end()) { out.push_back(it->second); i += l; break; }
            }
        }
        return out;
    }
};

static Column make_column(const Vocab& v, const std::vector<std::vector<uint16_t>>& rows) {
    Column c;
    c.rows = rows.size();
    c.off.push_back(0);
    c.rawoff.push_back(0);
    for (const auto& r : rows) {
        for (uint16_t t : r) {
            c.tok.push_back(t);
            std::string_view b = v.token(t);
            c.raw.insert(c.raw.end(), b.begin(), b.end());
        }
        c.off.push_back(c.tok.size());
        c.rawoff.push_back(c.raw.size());
    }
    return c;
}

static std::string random_url(std::mt19937& rng) {
    static const std::vector<std::string> pieces = {
        "/static/", "/sta", "tic", "/", ".m3u8", ".m3", "u8", "google", "/search", "/v", "1", "a", "b", "c",
        "x", ".", "s", "/v1/", "/v" + E2 + "/", E2, EU, EM, "www.", ".com", "?q=", "/vv/", "_", "%"};
    std::string s;
    int k = static_cast<int>(rng() % 9);
    for (int i = 0; i < k; ++i) s += pieces[rng() % pieces.size()];
    return s;
}

static const std::vector<std::string>& url_patterns() {
    static const std::vector<std::string> p = {
        "/static/%", "%.m3u8", "%/v_/%", "%google%/search%", "/static/%.m3u8", "/sta%", "%", "", "_", "%_",
        "_%", "%_%", "%" + E2 + "%", E2 + "%", "%" + EU, "%" + EM + "%", "/static/", "%tic/%", "____%",
        "%a_b%", "%/v__/%", "/_%", "%u8", "%3u8", "a%", "%a", "a", "%%", "/static/%/search", "%.com%",
        "www.%.com", "%_" + E2 + "_%", "_" + EU + "%", "%" + E2 + EU, "/v_/%", "%c", "%/", "%%%a%%b%%"};
    return p;
}

// ---------------------------------------------------------------- T0 oracle self-test

static void t0_oracle() {
    struct Case { std::string s, p; bool m; };
    const std::vector<Case> cs = {
        {"", "", true}, {"a", "", false}, {"", "%", true}, {"", "_", false}, {"abc", "a%", true},
        {"abc", "%c", true}, {"abc", "%b%", true}, {"abc", "a_c", true}, {"a" + E2 + "c", "a_c", true},
        {"a" + EU + "c", "a_c", true}, {"a" + EM + "c", "a_c", true}, {"ac", "a_c", false},
        {"a" + E2 + "c", "a__c", false}, {"abc", "abc", true}, {"abcd", "abc", false}, {"abc", "%%%", true},
        {"abc", "_%_", true}, {"a", "_%_", false}, {"/static/x.js", "/static/%", true},
        {"/stat", "/static/%", false}, {"x.m3u8", "%.m3u8", true}, {".m3u8x", "%.m3u8", false},
        {"/a/v1/b", "%/v_/%", true}, {"/a/v12/b", "%/v_/%", false}, {"/a/v" + E2 + "/b", "%/v_/%", true},
        {"www.google.com/search?q", "%google%/search%", true}, {"www.google.com/maps", "%google%/search%", false},
        {"ab\\c", "ab\\c", true}, {"abc", "ab\\c", false}, {E2, "_", true}, {E2, "__", false},
        {"aXbXc", "a%b%c", true}, {"acb", "a%b%c", false}, {"abab", "%ab", true}, {"aba", "ab%ba", false},
        {"abba", "ab%ba", true}, {"abcba", "ab%ba", true}};
    for (const Case& c : cs) CHECK_MSG(like_ref(c.s, c.p) == c.m, show(c.s) + " LIKE " + show(c.p));
}

// ---------------------------------------------------------------- T1 parser

static void t1_parser() {
    Pattern p = parse_like("%%a%%");
    CHECK(p.elems.size() == 3);
    CHECK(to_string(p) == "%a%");
    CHECK(p.starts_with_star() && p.ends_with_star());
    CHECK(parse_like("").elems.empty());
    CHECK(!parse_like("").starts_with_star() && !parse_like("").ends_with_star());
    Pattern q = parse_like("a_b");
    CHECK(q.elems.size() == 3 && q.elems[0].kind == ElemKind::Lit && q.elems[1].kind == ElemKind::Any1 &&
          q.elems[2].kind == ElemKind::Lit);
    CHECK(q.has_any1());
    Pattern r = parse_like("/static/");
    CHECK(r.elems.size() == 1 && r.elems[0].bytes == "/static/");
    CHECK(!r.has_any1());
    CHECK(parse_like("%a%b%c%").interior_stars() == 2);
    CHECK(parse_like("__").elems.size() == 2);
    CHECK(to_string(parse_like("%%%")) == "%");
    CHECK(to_string(parse_like("a%%_%%b")) == "a%_%b");
    CHECK(parse_like(E2 + EU + EM).elems.size() == 1);
    CHECK(to_string(parse_like("ab\\%")) == "ab\\%");
    CHECK_THROWS(parse_like("\xC3"), std::invalid_argument);
    CHECK_THROWS(parse_like("\xC0\x80"), std::invalid_argument);
    CHECK_THROWS(parse_like("\xED\xA0\x80"), std::invalid_argument);
    CHECK_THROWS(parse_like("\xF4\x90\x80\x80"), std::invalid_argument);
    CHECK_THROWS(parse_like("\xA9"), std::invalid_argument);
    CHECK_THROWS(parse_like("\xFF"), std::invalid_argument);
    CHECK(valid_utf8(E2 + EU + EM + "abc"));
    CHECK(!valid_utf8("\xE2\x82"));
}

// ---------------------------------------------------------------- T2/T3 exhaustive DFA

static void enumerate(const std::vector<std::string>& alpha, size_t maxlen, std::vector<std::string>& out) {
    out.clear();
    out.push_back("");
    size_t begin = 0;
    for (size_t len = 1; len <= maxlen; ++len) {
        size_t end = out.size();
        for (size_t i = begin; i < end; ++i)
            for (const std::string& a : alpha) out.push_back(out[i] + a);
        begin = end;
    }
}

// Brute-force DEAD / UNIV check on a DFA.
static void check_classes(const Dfa& d, const std::string& label) {
    for (uint32_t s = 0; s < d.S; ++s) {
        std::vector<uint8_t> seen(d.S, 0);
        std::vector<uint32_t> st = {s};
        seen[s] = 1;
        bool any_acc = false, all_acc = true;
        while (!st.empty()) {
            uint32_t x = st.back();
            st.pop_back();
            bool a = d.flags[x] & kAccept;
            any_acc |= a;
            all_acc &= a;
            for (int b = 0; b < 256; ++b) {
                uint32_t y = d.next[size_t(x) * 256 + b];
                if (!seen[y]) { seen[y] = 1; st.push_back(y); }
            }
        }
        CHECK_MSG(bool(d.flags[s] & kDead) == !any_acc, label);
        CHECK_MSG(bool(d.flags[s] & kUniv) == all_acc, label);
    }
}

static void t2_exhaustive_dfa() {
    std::vector<std::string> strs, pats;
    enumerate({"a", "b", "/", E2, EU, EM}, 5, strs);
    enumerate({"a", E2, "%", "_"}, 4, pats);
    std::vector<std::vector<std::string>> schars;
    std::vector<std::string> srev;
    for (const std::string& s : strs) {
        schars.push_back(chars_of(s));
        srev.push_back(std::string(s.rbegin(), s.rend()));
    }
    long bad = 0;
    for (const std::string& pt : pats) {
        Pattern p = parse_like(pt);
        Nfa n = build_nfa(p);
        Dfa raw = determinize(n);
        Dfa fwd = minimize(raw);
        Dfa bwd = minimize(determinize(reverse_nfa(n)));
        check_classes(raw, "raw " + show(pt));
        check_classes(fwd, "fwd " + show(pt));
        check_classes(bwd, "bwd " + show(pt));
        int dead = 0, univ = 0;
        for (uint32_t s = 0; s < fwd.S; ++s) { dead += !!(fwd.flags[s] & kDead); univ += !!(fwd.flags[s] & kUniv); }
        CHECK_MSG(dead <= 1 && univ <= 1, "minimized DFA has unique sinks: " + show(pt));
        CHECK(fwd.S <= raw.S);
        RefPat rp = ref_parse(pt);
        for (size_t i = 0; i < strs.size(); ++i) {
            bool want = ref_match(schars[i], rp);
            const auto* b = reinterpret_cast<const uint8_t*>(strs[i].data());
            const auto* rb = reinterpret_cast<const uint8_t*>(srev[i].data());
            bool ok = fwd.accepts(b, strs[i].size()) == want && raw.accepts(b, strs[i].size()) == want &&
                      bwd.accepts(rb, srev[i].size()) == want;
            // Early-termination soundness on every byte prefix.
            State s = fwd.start;
            for (size_t k = 0; k <= strs[i].size(); ++k) {
                if (fwd.flags[s] & kDead) ok &= !want;
                if (fwd.flags[s] & kUniv) ok &= want;
                if (k < strs[i].size()) s = fwd.step(s, b[k]);
            }
            if (!ok && ++bad <= 10)
                std::printf("  mismatch: %s LIKE %s want %d\n", show(strs[i]).c_str(), show(pt).c_str(), want);
            CHECK(ok);
        }
    }
    std::printf("  %zu patterns x %zu strings\n", pats.size(), strs.size());
}

// ---------------------------------------------------------------- T4 lifting

static void t4_lift() {
    Vocab v = make_vocab(toy_tokens());
    for (const std::string& pt : url_patterns()) {
        Nfa n = build_nfa(parse_like(pt));
        for (int dir = 0; dir < 2; ++dir) {
            Dfa d = minimize(determinize(dir ? reverse_nfa(n) : n));
            DenseTable t = lift(d, v, dir ? Dir::Backward : Dir::Forward);
            CHECK(t.S == d.S && t.n == v.n && t.bytes() == size_t(d.S) * v.n * 2);
            bool ok = true;
            for (uint32_t s = 0; s < d.S; ++s)
                for (uint32_t id = 0; id < v.n; ++id) {
                    std::string b = tok_str(v, static_cast<uint16_t>(id));
                    if (dir) b.assign(b.rbegin(), b.rend());
                    State x = static_cast<State>(s);
                    for (unsigned char c : b) x = d.step(x, c);
                    ok &= t.next(static_cast<State>(s), static_cast<uint16_t>(id)) == x;
                }
            CHECK_MSG(ok, "lift " + show(pt));
        }
    }
    Vocab bad = make_vocab({"a", "b"});
    bad.off.pop_back();
    Dfa d = determinize(build_nfa(parse_like("a%")));
    CHECK_THROWS(lift(d, bad, Dir::Forward), std::invalid_argument);
}

// ---------------------------------------------------------------- T5 segmentation invariance

static std::vector<uint8_t> unpack(const Bitmap& b, uint64_t rows) {
    std::vector<uint8_t> o(rows);
    for (uint64_t r = 0; r < rows; ++r) o[r] = bitmap_get(b, r);
    return o;
}

// Checks every scan variant of m against the oracle for the strings behind column c.
static void check_all_variants(const Matcher& m, const DenseTable& dense, const Column& c,
                               const std::vector<std::string>& strs, const std::string& pt, const std::string& label,
                               const Column* c2 = nullptr, const std::vector<std::string>* strs2 = nullptr) {
    std::vector<uint8_t> want(c.rows);
    uint64_t want_matches = 0;
    for (uint64_t r = 0; r < c.rows; ++r) {
        want[r] = like_ref(strs2 ? strs[r] + (*strs2)[r] : strs[r], pt);
        want_matches += want[r];
    }
    for (int variant = 0; variant < 4; ++variant) {
        Bitmap out;
        Stats st;
        const DenseTable* dt = (variant & 1) ? &dense : nullptr;
        bool early = !(variant & 2);
        MatcherAccess::match_with(m, dt, early, c, c2, out, st);
        CHECK(out.size() == (c.rows + 7) / 8);
        std::vector<uint8_t> got = unpack(out, c.rows);
        long bad = 0;
        for (uint64_t r = 0; r < c.rows; ++r)
            if (got[r] != want[r] && ++bad <= 3)
                std::printf("  %s variant %d: row %llu %s LIKE %s got %d want %d\n", label.c_str(), variant,
                            (unsigned long long)r,
                            show(strs2 ? strs[r] + (*strs2)[r] : strs[r]).c_str(), show(pt).c_str(), got[r], want[r]);
        CHECK_MSG(bad == 0, label);
        CHECK_MSG(st.matches == want_matches && st.rows == c.rows, label);
        if (c.rows % 8) CHECK_MSG((out.back() >> (c.rows % 8)) == 0, "padding bits zero");
    }
}

static void t5_segmentation() {
    Vocab v = make_vocab(toy_tokens());
    Segmenter seg(v);
    std::mt19937 rng(12345);
    std::vector<std::string> strs;
    for (int i = 0; i < 600; ++i) strs.push_back(random_url(rng));
    for (const std::string& s : std::vector<std::string>{"", "/static/", "/static/a.m3u8", "/v" + E2 + "/",
                                                        "www.google.com/search", ".m3u8", "/v1/", E2, EU, EM})
        strs.push_back(s);
    for (const std::string& pt : url_patterns()) {
        for (int fd = -1; fd <= 1; ++fd) {
            BuildOptions opt;
            opt.force_dir = fd;
            DenseTable dense;
            Matcher m = MatcherAccess::build(pt, v, opt, &dense);
            for (int segmode = 0; segmode < 2; ++segmode) {
                std::vector<std::vector<uint16_t>> rows;
                for (const std::string& s : strs) rows.push_back(segmode ? seg.greedy(s) : seg.random(s, rng));
                Column c = make_column(v, rows);
                // The column really decodes to the strings.
                for (size_t r = 0; r < strs.size(); ++r)
                    CHECK(std::string(c.raw.begin() + c.rawoff[r], c.raw.begin() + c.rawoff[r + 1]) == strs[r]);
                check_all_variants(m, dense, c, strs, pt,
                                   "seg pat=" + show(pt) + " dir=" + std::to_string(fd) + " mode=" +
                                       std::to_string(segmode));
            }
        }
    }
    // Patterns from the exhaustive alphabet, random strings over a byte-splitting vocabulary.
    std::vector<std::string> pats;
    enumerate({"a", E2, "%", "_"}, 4, pats);
    std::vector<std::string> s2;
    std::vector<std::string> alpha = {"a", "b", "/", E2, EU, EM};
    for (int i = 0; i < 300; ++i) {
        std::string s;
        int k = static_cast<int>(rng() % 7);
        for (int j = 0; j < k; ++j) s += alpha[rng() % alpha.size()];
        s2.push_back(s);
    }
    for (const std::string& pt : pats) {
        BuildOptions opt;
        DenseTable dense;
        Matcher m = MatcherAccess::build(pt, v, opt, &dense);
        std::vector<std::vector<uint16_t>> rows;
        for (const std::string& s : s2) rows.push_back(seg.random(s, rng));
        check_all_variants(m, dense, make_column(v, rows), s2, pt, "alpha pat=" + show(pt));
    }
}

// ---------------------------------------------------------------- T6 kinds and early termination

static uint16_t id_of(const Vocab& v, const std::string& s) {
    for (uint32_t i = 0; i < v.n; ++i)
        if (tok_str(v, static_cast<uint16_t>(i)) == s) return static_cast<uint16_t>(i);
    std::printf("  fixture token missing: %s\n", show(s).c_str());
    std::abort();
}

static std::vector<uint16_t> ids(const Vocab& v, const std::vector<std::string>& toks) {
    std::vector<uint16_t> o;
    for (const auto& t : toks) o.push_back(id_of(v, t));
    return o;
}

static uint64_t tokens_read_for(const Matcher& m, const Vocab& v, const std::vector<std::string>& toks,
                                bool* hit = nullptr) {
    Column c = make_column(v, {ids(v, toks)});
    Bitmap out;
    Stats st;
    m.match(c, out, st);
    if (hit) *hit = bitmap_get(out, 0);
    return st.tokens_read;
}

static void t6_kinds_and_early_exit() {
    Vocab v = make_vocab(toy_tokens());
    CHECK(Matcher::build("/static/%", v).kind() == ScanKind::Forward);
    CHECK(Matcher::build("%.m3u8", v).kind() == ScanKind::Backward);
    CHECK(Matcher::build("abc", v).kind() == ScanKind::Anchored);
    CHECK(Matcher::build("", v).kind() == ScanKind::Anchored);
    CHECK(Matcher::build("a%b", v).kind() == ScanKind::Anchored);
    CHECK(Matcher::build("%", v).kind() == ScanKind::Forward);
    CHECK(Matcher::build("%x%", v).kind() == ScanKind::Forward);
    CHECK(Matcher::build("%/v_/%", v).kind() == ScanKind::Forward);

    bool hit = false;
    Matcher pre = Matcher::build("/static/%", v);
    CHECK(tokens_read_for(pre, v, {"/static/", "a", "b", "c"}, &hit) == 1 && hit);
    CHECK(tokens_read_for(pre, v, {"x", "/static/", "a", "b"}, &hit) == 1 && !hit);
    CHECK(tokens_read_for(pre, v, {"/sta", "tic", "/", "z", "z"}, &hit) == 3 && hit);
    CHECK(tokens_read_for(pre, v, {"/sta", "tic"}, &hit) == 2 && !hit);
    CHECK(tokens_read_for(pre, v, {"/st", "atic", "/static/"}, &hit) == 3 && hit);  // "/static/static/"
    CHECK(tokens_read_for(pre, v, {"/static", "x", "/", "a"}, &hit) == 2 && !hit);
    Matcher all = Matcher::build("%", v);
    CHECK(tokens_read_for(all, v, {"a", "b", "c"}, &hit) == 0 && hit);
    CHECK(tokens_read_for(all, v, {}, &hit) == 0 && hit);
    Matcher suf = Matcher::build("%.m3u8", v);
    CHECK(tokens_read_for(suf, v, {"a", "b", ".m3u8"}, &hit) == 1 && hit);
    CHECK(tokens_read_for(suf, v, {".m3u8", "a", "b", "c"}, &hit) == 1 && !hit);
    CHECK(tokens_read_for(suf, v, {"a", ".m3", "u8"}, &hit) == 2 && hit);
    CHECK(tokens_read_for(suf, v, {"m3u", "8"}, &hit) == 2 && !hit);
    Matcher ex = Matcher::build("abc", v);
    CHECK(tokens_read_for(ex, v, {"ab", "c", "d", "e"}, &hit) == 3 && !hit);
    CHECK(tokens_read_for(ex, v, {"abc"}, &hit) == 1 && hit);
    CHECK(tokens_read_for(ex, v, {"x", "abc"}, &hit) == 1 && !hit);
    Matcher con = Matcher::build("%goo%", v);
    CHECK(tokens_read_for(con, v, {"x", "goo", "y", "z"}, &hit) == 2 && hit);
    CHECK(tokens_read_for(con, v, {"x", "y"}, &hit) == 2 && !hit);
}

// ---------------------------------------------------------------- T7 compact table

static void t7_compact() {
    // Full 65536-token vocabulary with pattern-relevant tokens on word boundaries.
    std::vector<std::string> toks(65536);
    for (size_t i = 0; i < toks.size(); ++i) toks[i] = "z" + std::to_string(i);
    toks[0] = "/static/";
    toks[63] = "/s";
    toks[64] = "tatic/";
    toks[127] = "/";
    toks[128] = "static/x";
    toks[4095] = ".m3u8";
    toks[65535] = "/static/abc";
    Vocab v = make_vocab(toks);
    for (const std::string& pt : {std::string("/static/%"), std::string("%.m3u8"), std::string("%tic%"),
                                  std::string("/s_atic/%")}) {
        DenseTable dense;
        Matcher m = MatcherAccess::build(pt, v, {}, &dense);
        const CompactTable& t = MatcherAccess::table(m);
        uint32_t S = MatcherAccess::states(m);
        CHECK(m.dense_table_bytes() == size_t(S) * 65536 * 2);
        CHECK(m.table_bytes() == size_t(S) * 2 + 8192 + 2048 + size_t(t.exc) * S * 2 + S);
        CHECK(m.table_bytes() < m.dense_table_bytes());
        // Every transition, including ids at word boundaries, equals the dense table.
        bool ok = true;
        for (uint32_t s = 0; s < S; ++s)
            for (uint32_t id = 0; id < 65536; ++id)
                ok &= t.next(static_cast<State>(s), static_cast<uint16_t>(id)) ==
                      dense.next(static_cast<State>(s), static_cast<uint16_t>(id));
        CHECK_MSG(ok, "compact == dense: " + pt);
        std::vector<std::vector<uint16_t>> rows = {{0},     {63, 64},     {63, 64, 65535}, {65535}, {127, 128},
                                                   {1, 4095}, {4095, 0}, {}, {65535, 4095}, {63, 128}};
        Column c = make_column(v, rows);
        std::vector<std::string> strs;
        for (auto& r : rows) {
            std::string s;
            for (uint16_t id : r) s += toks[id];
            strs.push_back(s);
        }
        check_all_variants(m, dense, c, strs, pt, "full-vocab " + pt);
        std::printf("  %-12s S=%u exceptions=%u table=%zu B dense=%zu B\n", pt.c_str(), S, t.exc, m.table_bytes(),
                    m.dense_table_bytes());
    }
    toks.push_back("overflow");
    Vocab big = make_vocab(toks);
    CHECK_THROWS(Matcher::build("a%", big), std::invalid_argument);

    // A tampered compact table must be rejected by verify().
    Vocab tv = make_vocab(toy_tokens());
    Dfa d = minimize(determinize(build_nfa(parse_like("/static/%"))));
    DenseTable dense = lift(d, tv, Dir::Forward);
    CompactTable ct = compress(dense);
    verify(ct, dense);
    CHECK(ct.exc > 0);
    ct.E[0] = static_cast<State>(ct.E[0] + 1);
    CHECK_THROWS(verify(ct, dense), std::logic_error);
}

// ---------------------------------------------------------------- T8 edge cases

static void t8_edges() {
    Vocab v = make_vocab(toy_tokens());
    uint16_t empty = id_of(v, "");
    Matcher m = Matcher::build("", v);
    Column c = make_column(v, {{}, {empty}, {empty, empty}, {id_of(v, "a")}, {empty, id_of(v, "a")}});
    Bitmap out;
    Stats st;
    m.match(c, out, st);
    CHECK(out.size() == 1 && out[0] == 0x07);
    CHECK(st.matches == 3 && st.rows == 5);
    CHECK(st.bytes_decoded == 0 && st.bytes_compared == 0);
    CHECK(st.table_kb > 0);

    // Stats accumulate across calls; build_us / table_kb are set.
    m.match(c, out, st);
    CHECK(st.rows == 10 && st.matches == 6);
    CHECK(st.build_us > 0);

    // Zero rows.
    Column z;
    z.off = {0};
    z.rawoff = {0};
    Bitmap zo = {0xFF};
    Stats zs;
    Matcher::build("%a", v).match(z, zo, zs);
    CHECK(zo.empty() && zs.rows == 0);

    // Malformed columns.
    Column bad = c;
    bad.off.pop_back();
    CHECK_THROWS(m.match(bad, out, st), std::invalid_argument);
    Column bad2 = c;
    bad2.off.back() = bad2.tok.size() + 1;
    CHECK_THROWS(m.match(bad2, out, st), std::invalid_argument);

    // Invalid ids are detected by the checked-mode validator.
    Column inv = make_column(v, {{1, 2}});
    inv.tok[1] = static_cast<uint16_t>(v.n);
    CHECK_THROWS(detail::validate_tokens(inv, v.n), std::out_of_range);
    detail::validate_tokens(c, v.n);
#ifdef TOKENLIKE_CHECKED
    CHECK_THROWS(m.match(inv, out, st), std::out_of_range);
    CHECK_THROWS(m.match_concat(c, inv, out, st), std::invalid_argument);  // row count differs first
#endif

    // A multibyte character split over single-byte tokens, and '_' over a split 4-byte char.
    Matcher eu = Matcher::build("%" + EU, v);
    Column ec = make_column(v, {{id_of(v, "a"), 0xE2, 0x82, 0xAC},
                                {id_of(v, "a"), id_of(v, "\xE2\x82"), 0xAC},
                                {id_of(v, "a\xC3"), 0xA9}});
    eu.match(ec, out, st);
    CHECK(bitmap_get(out, 0) && bitmap_get(out, 1) && !bitmap_get(out, 2));
    Matcher one = Matcher::build("a_b", v);
    Column oc = make_column(v, {{id_of(v, "a"), id_of(v, "\xF0\x9F"), id_of(v, "\x98\x80"), id_of(v, "b")},
                                {id_of(v, "a"), id_of(v, "\xF0\x9F\x98"), id_of(v, "\x80" "b")},
                                {id_of(v, "a\xC3"), id_of(v, "\xA9" "a"), id_of(v, "b")}});
    one.match(oc, out, st);
    CHECK(bitmap_get(out, 0) && bitmap_get(out, 1) && !bitmap_get(out, 2));

    // Row counts not a multiple of 8, larger than one byte.
    Matcher pa = Matcher::build("a%", v);
    std::vector<std::vector<uint16_t>> rows;
    for (int i = 0; i < 13; ++i) rows.push_back({static_cast<uint16_t>(i % 2 ? 'a' : 'b')});
    pa.match(make_column(v, rows), out, st);
    CHECK(out.size() == 2 && out[0] == 0xAA && out[1] == 0x0A);

    // Invalid pattern.
    CHECK_THROWS(Matcher::build("\xC3", v), std::invalid_argument);

    // '_' at every UTF-8 length-class boundary, fed as single-byte tokens.
    std::vector<std::string> cps;
    for (int b = 0; b < 0x80; ++b) cps.push_back(std::string(1, static_cast<char>(b)));
    for (const char* s : {"\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xEF\xBF\xBF",
                          "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"})
        cps.push_back(s);
    std::vector<std::string> strs;
    std::vector<std::vector<uint16_t>> brows;
    for (const std::string& cp : cps)
        for (const std::string& s : {cp, cp + cp, "x" + cp, cp + "x"}) {
            strs.push_back(s);
            std::vector<uint16_t> r;
            for (unsigned char ch : s) r.push_back(ch);
            brows.push_back(r);
        }
    Column bc = make_column(v, brows);
    for (const std::string& pt : {std::string("_"), std::string("__"), std::string("%_"), std::string("_%"),
                                  std::string("x_"), std::string("_x"), std::string("%_x"), std::string("x_%")})
        for (int fd = 0; fd <= 1; ++fd) {
            BuildOptions opt;
            opt.force_dir = fd;
            DenseTable dense;
            Matcher bm = MatcherAccess::build(pt, v, opt, &dense);
            check_all_variants(bm, dense, bc, strs, pt, "boundary " + pt + " dir=" + std::to_string(fd));
        }
}

// ---------------------------------------------------------------- T9 concatenation

static void t9_concat() {
    Vocab v = make_vocab(toy_tokens());
    Segmenter seg(v);
    std::mt19937 rng(777);
    std::vector<std::string> a, b;
    for (int i = 0; i < 400; ++i) {
        a.push_back(rng() % 6 ? random_url(rng) : "");
        b.push_back(rng() % 6 ? random_url(rng) : "");
    }
    std::vector<std::vector<uint16_t>> ra, rb;
    for (size_t i = 0; i < a.size(); ++i) { ra.push_back(seg.random(a[i], rng)); rb.push_back(seg.random(b[i], rng)); }
    Column ca = make_column(v, ra), cb = make_column(v, rb);
    for (const std::string& pt : url_patterns())
        for (int fd = -1; fd <= 1; ++fd) {
            BuildOptions opt;
            opt.force_dir = fd;
            DenseTable dense;
            Matcher m = MatcherAccess::build(pt, v, opt, &dense);
            check_all_variants(m, dense, ca, a, pt, "concat pat=" + show(pt) + " dir=" + std::to_string(fd), &cb, &b);
        }
    Matcher m = Matcher::build("%a%", v);
    Column shorter = make_column(v, {{1}});
    Bitmap out;
    Stats st;
    CHECK_THROWS(m.match_concat(ca, shorter, out, st), std::invalid_argument);
    m.match_concat(ca, cb, out, st);
    CHECK(st.rows == ca.rows);
}

// ---------------------------------------------------------------- T10 state limit

static void t10_limits() {
    Vocab v = make_vocab(toy_tokens());
    Segmenter seg(v);
    std::mt19937 rng(99);
    // Forward needs to remember 'a' positions in a 10-character window (exponential); backward is small.
    std::string pt = "%a__________%";
    BuildOptions fwd;
    fwd.force_dir = 0;
    CHECK_THROWS(MatcherAccess::build(pt, v, fwd), TooComplex);
    DenseTable dense;
    Matcher m = MatcherAccess::build(pt, v, {}, &dense);
    CHECK(m.kind() == ScanKind::Backward);
    std::vector<std::string> strs;
    std::vector<std::vector<uint16_t>> rows;
    for (int i = 0; i < 300; ++i) {
        std::string s;
        int k = static_cast<int>(rng() % 16);
        for (int j = 0; j < k; ++j) s += (rng() % 3 == 0) ? "a" : (rng() % 2 ? E2 : "b");
        strs.push_back(s);
        rows.push_back(seg.random(s, rng));
    }
    check_all_variants(m, dense, make_column(v, rows), strs, pt, "retry " + pt);
    // Exponential in both directions -> clean failure.
    CHECK_THROWS(Matcher::build("%a__________b%", v), TooComplex);
    CHECK_THROWS(Matcher::build("%a__________b%", v), std::runtime_error);
}

// ---------------------------------------------------------------- T11 synthetic scale

static void t11_scale() {
    std::mt19937 rng(2024);
    std::vector<std::string> toks;
    for (int b = 0; b < 256; ++b) toks.push_back(std::string(1, static_cast<char>(b)));
    const std::string alpha = "abcdefghijklmnopqrstuvwxyz0123456789/._-?=&";
    for (const char* s : {"/static/", ".m3u8", "google", "/search", "/v"}) toks.push_back(s);
    while (toks.size() < 50257) {
        std::string s;
        int len = 2 + static_cast<int>(rng() % 9);
        for (int i = 0; i < len; ++i) s += alpha[rng() % alpha.size()];
        toks.push_back(s);
    }
    Vocab v = make_vocab(toks);
    Segmenter seg(v);
    std::vector<std::string> strs;
    std::vector<std::vector<uint16_t>> rows;
    for (int i = 0; i < 3000; ++i) {
        std::string s = random_url(rng);
        strs.push_back(s);
        rows.push_back(seg.greedy(s));
    }
    Column c = make_column(v, rows);
    for (const std::string& pt :
         {std::string("/static/%"), std::string("%.m3u8"), std::string("%/v_/%"), std::string("%google%/search%")}) {
        DenseTable dense;
        Matcher m = MatcherAccess::build(pt, v, {}, &dense);
        Bitmap out;
        Stats st;
        m.match(c, out, st);
        check_all_variants(m, dense, c, strs, pt, "scale " + pt);
        std::printf("  %-18s S=%-3u exc=%-5u table=%7.1f KB dense=%8.1f KB build=%8.0f us tokens_read=%llu/%zu\n",
                    pt.c_str(), MatcherAccess::states(m), MatcherAccess::table(m).exc, m.table_bytes() / 1024.0,
                    m.dense_table_bytes() / 1024.0, st.build_us, (unsigned long long)st.tokens_read, c.tok.size());
    }
}

// ---------------------------------------------------------------- main

int main() {
    struct Suite { const char* name; void (*fn)(); };
    const Suite suites[] = {
        {"T0 oracle", t0_oracle},          {"T1 parser", t1_parser},
        {"T2 exhaustive DFA", t2_exhaustive_dfa}, {"T4 lifting", t4_lift},
        {"T5 segmentation", t5_segmentation},     {"T6 kinds/early exit", t6_kinds_and_early_exit},
        {"T7 compact table", t7_compact},
        {"T8 edge cases", t8_edges},               {"T9 concatenation", t9_concat},
        {"T10 state limit", t10_limits},           {"T11 synthetic scale", t11_scale}};
    for (const Suite& s : suites) {
        long f0 = g_fail, c0 = g_checks;
        auto t0 = std::chrono::steady_clock::now();
        try {
            s.fn();
        } catch (const std::exception& e) {
            ++g_fail;
            std::printf("  EXCEPTION: %s\n", e.what());
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] %s: %ld checks, %ld failures (%.0f ms)\n", g_fail == f0 ? "PASS" : "FAIL", s.name,
                    g_checks - c0, g_fail - f0, ms);
    }
    std::printf("%s: %ld checks, %ld failures\n", g_fail ? "FAILED" : "ALL PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
