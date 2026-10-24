// Tests for the byte-level LIKE matcher and the raw / decode baselines.
//   test_baselines                      run all tests
//   test_baselines --write-fixture DIR  write a synthetic dataset for CLI tests
#include "fixture_io.h"
#include "tokenlike/baselines.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++failures;                                        \
            if (failures <= 20) {                              \
                std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                std::fprintf(stderr, __VA_ARGS__);             \
                std::fprintf(stderr, "\n");                    \
            }                                                  \
        }                                                      \
    } while (0)

// ---- Reference matcher: textbook DP over decoded code points. -------------

std::vector<char32_t> decode_utf8(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < n && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

bool like_ref(const std::string& pattern, const std::string& text) {
    const auto p = decode_utf8(pattern);
    const auto t = decode_utf8(text);
    // dp[j] = pattern prefix of length i matches text prefix of length j
    std::vector<char> dp(t.size() + 1, 0), nx(t.size() + 1);
    dp[0] = 1;
    for (char32_t pc : p) {
        std::fill(nx.begin(), nx.end(), 0);
        if (pc == U'%') {
            char any = 0;
            for (size_t j = 0; j <= t.size(); ++j) nx[j] = any = any | dp[j];
        } else {
            for (size_t j = 1; j <= t.size(); ++j)
                nx[j] = dp[j - 1] && (pc == U'_' || pc == t[j - 1]);
        }
        dp.swap(nx);
    }
    return dp[t.size()] != 0;
}

// ---- Hand-written cases. ---------------------------------------------------

struct Case {
    const char* pattern;
    const char* text;
    bool expect;
};

const Case kCases[] = {
    {"", "", true},           {"", "a", false},          {"%", "", true},
    {"%", "abc", true},       {"%%", "", true},          {"_", "", false},
    {"_", "a", true},         {"_", "ab", false},        {"abc", "abc", true},
    {"abc", "abcd", false},   {"abc", "ab", false},      {"a%", "a", true},
    {"a%", "ba", false},      {"%a", "ba", true},        {"%a", "ab", false},
    {"%b%", "abc", true},     {"%b%", "ac", false},      {"%%b%%", "b", true},
    {"a%c", "ac", true},      {"a%c", "abc", true},      {"a%c", "a", false},
    {"a%a", "a", false},      {"a%a", "aa", true},       {"ab%ba", "aba", false},
    {"ab%ba", "abba", true},  {"%aa%a", "aaa", true},    {"%aa%a", "aa", false},
    {"%a%a%a%", "aa", false}, {"%a%a%a%", "xaxaxax", true},
    {"a_c", "abc", true},     {"a_c", "ac", false},      {"a_c", "abbc", false},
    {"%_%", "", false},       {"%_%", "x", true},        {"__", "a", false},
    // UTF-8: '_' is one code point, not one byte.
    {"_", "\xC3\xA9", true},                          // é (2 bytes)
    {"__", "\xC3\xA9", false},
    {"_", "\xE6\x97\xA5", true},                      // 日 (3 bytes)
    {"__", "\xE6\x97\xA5\xE6\x9C\xAC", true},         // 日本
    {"_", "\xF0\x9F\x98\x80", true},                  // 😀 (4 bytes)
    {"x_y", "x\xE6\x97\xA5y", true},
    {"%_", "a\xC3\xA9", true},
    {"%\xC3\xA9", "caf\xC3\xA9", true},
    {"caf_", "caf\xC3\xA9", true},
    {"%_\xC3\xA9", "\xC3\xA9", false},
    {"%_\xC3\xA9", "x\xC3\xA9", true},
    {"_%_", "\xF0\x9F\x98\x80", false},
    // URL-shaped patterns.
    {"/static/%", "/static/app.js", true},
    {"/static/%", "/stat", false},
    {"%.m3u8", "/live/index.m3u8", true},
    {"%.m3u8", "/live/index.m3u8?x", false},
    {"%/v_/%", "/api/v1/users", true},
    {"%/v_/%", "/api/v12/users", false},
    {"%/v_/%", "/v1/", true},
    {"/static/%.js", "/static/.js", true},
    {"/static/%.js", "/static.js", false},
    {"http%://%.com/%", "https://a.com/", true},
    // Backslash is an ordinary character (no ESCAPE), as in DuckDB.
    {"a\\_b", "a_b", false},
    {"a\\_b", "a\\xb", true},
};

void test_cases() {
    for (const auto& c : kCases) {
        const bool got = like_match(c.pattern, c.text);
        CHECK(got == c.expect, "like('%s','%s') = %d, want %d", c.text, c.pattern, got, c.expect);
        const bool ref = like_ref(c.pattern, c.text);
        CHECK(ref == c.expect, "like_ref('%s','%s') = %d, want %d", c.text, c.pattern, ref,
              c.expect);
    }
}

// ---- Differential fuzzing against like_ref. --------------------------------

const char* const kAlpha[] = {"a", "b", "/", ".", "\xC3\xA9", "\xE6\x97\xA5",
                              "\xF0\x9F\x98\x80"};

void test_fuzz(int iterations) {
    std::mt19937_64 rng(12345);
    auto rnd = [&](int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); };
    for (int it = 0; it < iterations; ++it) {
        std::string text, pat;
        // Every 4th text is long enough to exercise the 16-byte SIMD search.
        const int tl = it % 4 == 0 ? rnd(70) : rnd(9), pl = rnd(7);
        for (int i = 0; i < tl; ++i) text += kAlpha[rnd(rnd(2) ? 3 : 7)];
        for (int i = 0; i < pl; ++i) {
            const int r = rnd(10);
            pat += r < 2 ? "%" : r < 4 ? "_" : kAlpha[rnd(rnd(2) ? 3 : 7)];
        }
        const bool want = like_ref(pat, text);
        const bool got = like_match(pat, text);
        CHECK(got == want, "fuzz like('%s','%s') = %d, ref %d", text.c_str(), pat.c_str(), got,
              want);
    }
}

// Targeted fuzzing of each fast-path shape (lit, lit%, %lit, %lit%, a%b) on
// long texts, so the SIMD search and short-compare code are exercised.
void test_fuzz_fast_paths(int iterations) {
    std::mt19937_64 rng(999);
    auto rnd = [&](int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); };
    auto lit = [&](int maxlen) {
        std::string s;
        for (int i = 0, n = 1 + rnd(maxlen); i < n; ++i) s += kAlpha[rnd(rnd(3) ? 3 : 7)];
        return s;
    };
    for (int it = 0; it < iterations; ++it) {
        const std::string a = lit(10), b = lit(10);
        std::string text = lit(it % 2 ? 80 : 12);
        if (rnd(2)) text += a + lit(40);  // plant an occurrence mid-text
        std::string pat;
        switch (rnd(5)) {
            case 0: pat = a; break;
            case 1: pat = a + "%"; break;
            case 2: pat = "%" + a; break;
            case 3: pat = "%" + a + "%"; break;
            default: pat = a + "%" + b; break;
        }
        if (rnd(3) == 0) text = a + text;          // help prefix/contains hit
        if (rnd(3) == 0) text = text + b;          // help suffix hit
        const bool want = like_ref(pat, text);
        const bool got = like_match(pat, text);
        CHECK(got == want, "fastpath like('%s','%s') = %d, ref %d", text.c_str(), pat.c_str(), got,
              want);
    }
}

// ---- Synthetic URL column: raw == decode == reference. ---------------------

const std::vector<std::string> kExtraTokens = {
    "http://", "https://", "www.", ".com", "/static/", "/api/", "/v1/", "/v2/", ".m3u8",
    ".html",   ".js",      "index", "live", "\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC",
    "/images/", ".png",    "?id=",  "static"};

std::vector<std::string> synth_urls(size_t n, uint64_t seed) {
    static const char* hosts[] = {"http://", "https://", "https://www.", ""};
    static const char* names[] = {"example", "cdn", "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC", "a"};
    static const char* dirs[] = {"/static/", "/api/v1/", "/api/v2/", "/live/", "/images/",
                                 "/", "/v9/x/", "/staticx/"};
    static const char* files[] = {"index.html", "app.js", "seg.m3u8", "logo.png", "", "a.m3u8?id=7",
                                  "\xF0\x9F\x98\x80.html"};
    std::mt19937_64 rng(seed);
    auto pick = [&](auto& arr) { return arr[rng() % (sizeof(arr) / sizeof(arr[0]))]; };
    std::vector<std::string> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (rng() % 50 == 0) {
            out.emplace_back();  // NULL -> empty string
            continue;
        }
        std::string u = rng() % 3 ? std::string(pick(hosts)) + pick(names) + ".com" : "";
        u += pick(dirs);
        if (rng() % 4 == 0) u += pick(dirs) + 1;
        u += pick(files);
        out.push_back(u);
    }
    return out;
}

const char* const kColumnPatterns[] = {
    "/static/%", "%.m3u8", "%/v_/%", "%.html", "%static%", "https://%", "%caf_.com%",
    "%\xE6\x97\xA5%", "http%://%.com/%", "%", "", "/", "%_%_%", "/static/%.js", "%?id=_",
};

void test_column() {
    const Vocab v = fixture_vocab(kExtraTokens);
    const auto urls = synth_urls(20003, 7);  // not a multiple of 8: exercises padding
    const Column c = fixture_column(v, urls);

    // The tokenization must round-trip.
    for (size_t i = 0; i < urls.size(); ++i) {
        std::string dec;
        for (uint64_t j = c.off[i]; j < c.off[i + 1]; ++j) dec += v.token(c.tok[j]);
        CHECK(dec == urls[i], "fixture round-trip row %zu", i);
    }

    for (const char* p : kColumnPatterns) {
        Bitmap braw, bdec;
        Stats sraw, sdec;
        raw_match(p, c, braw, sraw);
        decode_match(p, v, c, bdec, sdec);
        CHECK(braw.size() == bitmap_bytes(c.rows) && bdec.size() == bitmap_bytes(c.rows),
              "bitmap size for '%s'", p);
        uint64_t want_matches = 0;
        for (size_t i = 0; i < urls.size(); ++i) {
            const bool want = like_ref(p, urls[i]);
            want_matches += want;
            CHECK(bitmap_bit(braw, i) == want, "raw '%s' row %zu ('%s')", p, i, urls[i].c_str());
            CHECK(bitmap_bit(bdec, i) == want, "decode '%s' row %zu ('%s')", p, i, urls[i].c_str());
        }
        // Padding bits past the last row must be zero.
        for (uint64_t r = c.rows; r < 8 * braw.size(); ++r)
            CHECK(!bitmap_bit(braw, r) && !bitmap_bit(bdec, r), "padding bit %llu set for '%s'",
                  static_cast<unsigned long long>(r), p);
        CHECK(sraw.matches == want_matches && sdec.matches == want_matches, "match count '%s'", p);
        CHECK(sraw.rows == c.rows && sdec.rows == c.rows, "stats rows '%s'", p);
        CHECK(sraw.tokens_read == 0 && sraw.bytes_decoded == 0, "raw stats '%s'", p);
        CHECK(sdec.tokens_read == c.tok.size(), "decode tokens_read '%s'", p);
        CHECK(sdec.bytes_decoded == c.raw.size(), "decode bytes_decoded '%s'", p);
        std::printf("  %-20s matches=%llu\n", p, static_cast<unsigned long long>(want_matches));
    }

    // A decoder reused across calls must give identical results.
    const DecodeMatcher dm = DecodeMatcher::build("%.m3u8", v);
    Bitmap b1, b2;
    Stats s1, s2;
    dm.match(c, b1, s1);
    dm.match(c, b2, s2);
    CHECK(b1 == b2 && s1.matches == s2.matches, "decode matcher reuse");

    // Empty column.
    const Column empty = fixture_column(v, {});
    Bitmap be;
    Stats se;
    raw_match("%", empty, be, se);
    CHECK(be.empty() && se.rows == 0, "empty column raw");
    decode_match("%", v, empty, be, se);
    CHECK(be.empty() && se.rows == 0, "empty column decode");
}

int write_fixture(const std::string& dir) {
    std::filesystem::create_directories(dir + "/synth");
    const Vocab v = fixture_vocab(kExtraTokens);
    write_vocab(dir + "/vocab.bin", v);
    write_column(dir + "/synth/url", fixture_column(v, synth_urls(200003, 11)));
    FILE* f = std::fopen((dir + "/patterns.txt").c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "# synthetic fixture patterns (test only)\n");
    for (const char* p : kColumnPatterns)
        if (*p) std::fprintf(f, "%s\n", p);
    std::fclose(f);
    std::printf("fixture written to %s\n", dir.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-fixture") return write_fixture(argv[2]);

    std::printf("hand cases\n");
    test_cases();
    std::printf("fuzz\n");
    test_fuzz(argc > 1 ? std::atoi(argv[1]) : 1000000);
    std::printf("fuzz fast paths\n");
    test_fuzz_fast_paths(argc > 1 ? std::atoi(argv[1]) : 300000);
    std::printf("synthetic column\n");
    test_column();

    if (failures) {
        std::fprintf(stderr, "%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("all baseline tests passed\n");
    return 0;
}
