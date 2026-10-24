// tokenlike CLI: load a column, run one matching mode N times, report the
// median as ONE JSON line on stdout, and write matching row ids
// (uint32 little-endian, ascending). Diagnostics go to stderr.
#include "tokenlike/baselines.h"
#include "tokenlike/common.h"

#if defined(TOKENLIKE_HAVE_LOADERS)
#include "tokenlike/column.h"
#include "tokenlike/vocab.h"
#elif defined(TOKENLIKE_FIXTURE_LOADERS)
#include "fixture_io.h"
#endif

#if defined(TOKENLIKE_HAVE_MATCHER)
#include "tokenlike/matcher.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kExitOk = 0, kExitError = 1, kExitUnavailable = 2;

const char* kUsage =
    "usage: tokenlike --mode token|decode|raw (--pattern LIKE | --pattern-file FILE)\n"
    "                 --col PREFIX [--col2 PREFIX] [--vocab data/vocab.bin]\n"
    "                 [--runs 5] [--warmup 1] [--rowids FILE] [--stats FILE] [--pin-cpu N]\n"
    "       aliases:  --column = --col, --column2 = --col2, --out = --rowids\n";

struct Args {
    std::string mode;
    std::optional<std::string> pattern;
    std::string col;
    std::optional<std::string> col2;
    std::string vocab = "data/vocab.bin";
    int runs = 5;
    int warmup = 1;
    std::optional<std::string> rowids;
    std::optional<std::string> stats;
    std::optional<int> pin_cpu;
};

// Pin the process to one logical CPU so that hybrid (P/E-core) machines do
// not migrate the scan between core types mid-measurement.
bool pin_to_cpu(int cpu) {
#ifdef _WIN32
    if (cpu >= 64) return false;
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::vector<std::string> utf8_argv(int argc, char** argv) {
#ifdef _WIN32
    // argv on Windows is in the ANSI code page; rebuild it as UTF-8 so that
    // non-ASCII patterns survive.
    (void)argc;
    (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(w);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

std::string read_pattern_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw UsageError("cannot read pattern file " + path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

int to_int(const std::string& key, const std::string& v) {
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end || x < 0 || x > 1000000) throw UsageError("bad value for " + key);
    return static_cast<int>(x);
}

Args parse_args(const std::vector<std::string>& av) {
    Args a;
    for (size_t i = 1; i < av.size(); ++i) {
        std::string key = av[i], val;
        const auto eq = key.find('=');
        if (key == "-h" || key == "--help") throw UsageError("");
        if (key.rfind("--", 0) != 0) throw UsageError("unexpected argument " + key);
        if (eq != std::string::npos) {
            val = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else {
            if (i + 1 >= av.size()) throw UsageError("missing value for " + key);
            val = av[++i];
        }
        // --column / --column2 / --out are the names Person B's check.py uses.
        if (key == "--mode") a.mode = val;
        else if (key == "--pattern") a.pattern = val;
        else if (key == "--pattern-file") a.pattern = read_pattern_file(val);
        else if (key == "--col" || key == "--column") a.col = val;
        else if (key == "--col2" || key == "--column2") a.col2 = val;
        else if (key == "--vocab") a.vocab = val;
        else if (key == "--runs") a.runs = to_int(key, val);
        else if (key == "--warmup") a.warmup = to_int(key, val);
        else if (key == "--rowids" || key == "--out") a.rowids = val;
        else if (key == "--stats") a.stats = val;
        else if (key == "--pin-cpu") a.pin_cpu = to_int(key, val);
        else throw UsageError("unknown option " + key);
    }
    if (a.mode != "token" && a.mode != "decode" && a.mode != "raw")
        throw UsageError("--mode must be token, decode or raw");
    if (!a.pattern) throw UsageError("--pattern or --pattern-file is required");
    if (a.col.empty()) throw UsageError("--col is required");
    if (a.runs < 1) throw UsageError("--runs must be >= 1");
    return a;
}

void validate(const Column& c, const std::string& name) {
    if (c.off.size() != c.rows + 1 || c.rawoff.size() != c.rows + 1 ||
        (c.rows && (c.off.back() > c.tok.size() || c.rawoff.back() > c.raw.size())))
        throw std::runtime_error("column " + name + " violates the common.h offset layout");
    if (c.rows > 0xFFFFFFFFull) throw std::runtime_error("column " + name + " exceeds uint32 row ids");
}

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);  // UTF-8 passes through
                }
        }
    }
    return out + "\"";
}

std::string json_num(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", x);
    return buf;
}

std::string json_opt(const std::optional<double>& x) { return x ? json_num(*x) : "null"; }

void write_rowids(const std::string& path, const Bitmap& bm) {
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::vector<uint32_t> ids;
    for (size_t byte = 0; byte < bm.size(); ++byte)
        for (unsigned bits = bm[byte]; bits; bits &= bits - 1)  // x86/ARM: little-endian
            ids.push_back(static_cast<uint32_t>(8 * byte + __builtin_ctz(bits)));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    const size_t w = ids.empty() ? 0 : std::fwrite(ids.data(), sizeof(uint32_t), ids.size(), f);
    std::fclose(f);
    if (w != ids.size()) throw std::runtime_error("short write " + path);
}

int run(const Args& a) {
    const std::string& pattern = *a.pattern;
#if defined(TOKENLIKE_FIXTURE_LOADERS)
    std::fprintf(stderr, "[tokenlike] WARNING: built with TEST FIXTURE loaders, not Person B's\n");
#endif
#if !defined(TOKENLIKE_HAVE_MATCHER)
    if (a.mode == "token") {
        std::fprintf(stderr, "[tokenlike] token mode not built: matcher sources absent\n");
        return kExitUnavailable;
    }
#endif
    if (a.col2 && a.mode != "token") {
        std::fprintf(stderr, "[tokenlike] --col2 is not yet supported for mode %s\n",
                     a.mode.c_str());
        return kExitUnavailable;
    }

    if (a.pin_cpu && !pin_to_cpu(*a.pin_cpu))
        std::fprintf(stderr, "[tokenlike] WARNING: could not pin to CPU %d\n", *a.pin_cpu);

    // ---- Load (not timed as part of matching).
    auto t_load = Clock::now();
    Vocab vocab{};
    const bool need_vocab = a.mode != "raw";
    if (need_vocab) vocab = load_vocab(a.vocab);
    const Column col = load_column(a.col);
    validate(col, a.col);
    Column col2{};
    if (a.col2) {
        col2 = load_column(*a.col2);
        validate(col2, *a.col2);
        if (col2.rows != col.rows) throw std::runtime_error("--col and --col2 row counts differ");
    }
    std::fprintf(stderr, "[tokenlike] loaded %llu rows in %.1f ms\n",
                 static_cast<unsigned long long>(col.rows), ms_since(t_load));

    // ---- Build (timed separately).
    std::function<void(Bitmap&, Stats&)> scan;
    std::optional<double> table_kb, dense_table_kb;
    const auto t_build = Clock::now();
    std::optional<RawMatcher> raw;
    std::optional<DecodeMatcher> dec;
#if defined(TOKENLIKE_HAVE_MATCHER)
    std::optional<Matcher> tok;
#endif
    if (a.mode == "raw") {
        raw = RawMatcher::build(pattern);
        scan = [&](Bitmap& bm, Stats& st) { raw->match(col, bm, st); };
    } else if (a.mode == "decode") {
        dec = DecodeMatcher::build(pattern, vocab);
        scan = [&](Bitmap& bm, Stats& st) { dec->match(col, bm, st); };
    } else {
#if defined(TOKENLIKE_HAVE_MATCHER)
        tok = Matcher::build(pattern, vocab);
        if (a.col2)
            scan = [&](Bitmap& bm, Stats& st) { tok->match_concat(col, col2, bm, st); };
        else
            scan = [&](Bitmap& bm, Stats& st) { tok->match(col, bm, st); };
#endif
    }
    const double build_us = ms_since(t_build) * 1000.0;
#if defined(TOKENLIKE_HAVE_MATCHER)
    if (tok) {
        table_kb = tok->table_bytes() / 1024.0;
        dense_table_kb = tok->dense_table_bytes() / 1024.0;
    }
#endif

    // ---- Scan: warmup, then timed runs. Only the match call is timed.
    Bitmap bm(bitmap_bytes(col.rows)), first;
    Stats st;
    for (int i = 0; i < a.warmup; ++i) {
        st = Stats{};
        scan(bm, st);
    }
    std::vector<double> ns;
    for (int r = 0; r < a.runs; ++r) {
        st = Stats{};
        const auto t0 = Clock::now();
        scan(bm, st);
        const auto t1 = Clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        if (r == 0) first = bm;
        else if (bm != first) throw std::runtime_error("result differs between runs");
    }
    if (bm.size() != bitmap_bytes(col.rows))
        throw std::runtime_error("matcher returned a bitmap of wrong size");
    for (uint64_t r = col.rows; r < 8 * bm.size(); ++r)
        if (bitmap_bit(bm, r)) throw std::runtime_error("matcher set a padding bit");

    uint64_t matches = 0;
    for (uint8_t b : bm) matches += static_cast<uint64_t>(__builtin_popcount(b));
    if (st.matches != matches)
        std::fprintf(stderr, "[tokenlike] WARNING: Stats.matches=%llu but bitmap has %llu\n",
                     static_cast<unsigned long long>(st.matches),
                     static_cast<unsigned long long>(matches));
    if (a.rowids) write_rowids(*a.rowids, bm);

    // ---- Report.
    uint64_t text_bytes = col.rows ? col.rawoff[col.rows] - col.rawoff[0] : 0;
    if (a.col2 && col2.rows) text_bytes += col2.rawoff[col2.rows] - col2.rawoff[0];
    const double med = median(ns);
    const double rows = col.rows ? static_cast<double>(col.rows) : 1.0;
    const auto [mn, mx] = std::minmax_element(ns.begin(), ns.end());

    std::string j = "{";
    j += "\"mode\": " + json_str(a.mode);
    j += ", \"pattern\": " + json_str(pattern);
    j += ", \"col\": " + json_str(a.col);
    j += ", \"col2\": " + (a.col2 ? json_str(*a.col2) : std::string("null"));
    j += ", \"rows\": " + std::to_string(col.rows);
    j += ", \"matches\": " + std::to_string(matches);
    j += ", \"ns_per_row\": " + json_num(med / rows);
    j += ", \"gbps\": " + json_num(med > 0 ? text_bytes / med : 0.0);
    j += ", \"tokens_per_row\": " + json_num(st.tokens_read / rows);
    j += ", \"bytes_decoded_per_row\": " + json_num(st.bytes_decoded / rows);
    j += ", \"table_kb\": " + json_opt(table_kb);
    j += ", \"dense_table_kb\": " + json_opt(dense_table_kb);
    j += ", \"build_us\": " + json_num(build_us);
    j += ", \"runs\": " + std::to_string(a.runs);
    // Extra fields (not part of the frozen minimum).
    j += ", \"warmup\": " + std::to_string(a.warmup);
    j += ", \"text_bytes\": " + std::to_string(text_bytes);
    j += ", \"median_ns\": " + json_num(med);
    j += ", \"ns_per_row_min\": " + json_num(*mn / rows);
    j += ", \"ns_per_row_max\": " + json_num(*mx / rows);
    j += ", \"bytes_compared_per_row\": " + json_num(st.bytes_compared / rows);
    j += ", \"pin_cpu\": " + (a.pin_cpu ? std::to_string(*a.pin_cpu) : std::string("null"));
#if defined(TOKENLIKE_FIXTURE_LOADERS)
    j += ", \"loaders\": \"fixture\"";
#else
    j += ", \"loaders\": \"official\"";
#endif
    j += "}";
    std::fprintf(stdout, "%s\n", j.c_str());
    std::fflush(stdout);
    if (a.stats) {
        FILE* f = std::fopen(a.stats->c_str(), "wb");
        if (!f) throw std::runtime_error("cannot write " + *a.stats);
        std::fprintf(f, "%s\n", j.c_str());
        std::fclose(f);
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(utf8_argv(argc, argv)));
    } catch (const UsageError& e) {
        if (*e.what()) std::fprintf(stderr, "error: %s\n", e.what());
        std::fputs(kUsage, stderr);
        return kExitError;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kExitError;
    }
}
