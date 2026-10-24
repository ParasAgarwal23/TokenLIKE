// TEST FIXTURES ONLY. See fixture_io.h.
#include "fixture_io.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

template <class T>
void write_vec(const std::string& path, const std::vector<T>& v) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    if (!v.empty()) std::fwrite(v.data(), sizeof(T), v.size(), f);
    std::fclose(f);
}

template <class T>
std::vector<T> read_vec(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + path);
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<T> v(static_cast<size_t>(n) / sizeof(T));
    if (!v.empty() && std::fread(v.data(), sizeof(T), v.size(), f) != v.size()) {
        std::fclose(f);
        throw std::runtime_error("short read " + path);
    }
    std::fclose(f);
    return v;
}

}  // namespace

Vocab fixture_vocab(const std::vector<std::string>& extra_tokens) {
    Vocab v;
    v.off.push_back(0);
    for (int b = 0; b < 256; ++b) {
        v.bytes.push_back(static_cast<uint8_t>(b));
        v.off.push_back(static_cast<uint32_t>(v.bytes.size()));
    }
    for (const auto& t : extra_tokens) {
        v.bytes.insert(v.bytes.end(), t.begin(), t.end());
        v.off.push_back(static_cast<uint32_t>(v.bytes.size()));
    }
    v.n = static_cast<uint32_t>(v.off.size() - 1);
    return v;
}

Column fixture_column(const Vocab& v, const std::vector<std::string>& rows) {
    Column c;
    c.rows = rows.size();
    c.off.push_back(0);
    c.rawoff.push_back(0);
    for (const auto& r : rows) {
        size_t i = 0;
        while (i < r.size()) {
            uint16_t best = static_cast<uint8_t>(r[i]);
            size_t best_len = 1;
            for (uint32_t t = 256; t < v.n; ++t) {
                const auto tk = v.token(static_cast<uint16_t>(t));
                if (tk.size() > best_len && r.compare(i, tk.size(), tk) == 0) {
                    best = static_cast<uint16_t>(t);
                    best_len = tk.size();
                }
            }
            c.tok.push_back(best);
            i += best_len;
        }
        c.off.push_back(c.tok.size());
        c.raw.insert(c.raw.end(), r.begin(), r.end());
        c.rawoff.push_back(c.raw.size());
    }
    return c;
}

void write_vocab(const std::string& path, const Vocab& v) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fwrite(&v.n, sizeof(v.n), 1, f);
    std::fwrite(v.off.data(), sizeof(uint32_t), v.off.size(), f);
    if (!v.bytes.empty()) std::fwrite(v.bytes.data(), 1, v.bytes.size(), f);
    std::fclose(f);
}

void write_column(const std::string& prefix, const Column& c) {
    write_vec(prefix + ".tok", c.tok);
    write_vec(prefix + ".off", c.off);
    write_vec(prefix + ".raw", c.raw);
    write_vec(prefix + ".rawoff", c.rawoff);
    FILE* f = std::fopen((prefix + ".meta.json").c_str(), "wb");
    if (f) {
        std::fprintf(f, "{\"rows\": %llu, \"fixture\": true}\n",
                     static_cast<unsigned long long>(c.rows));
        std::fclose(f);
    }
}

// Only compiled when Person B's loaders are absent, so the symbols never clash.
#ifndef TOKENLIKE_HAVE_LOADERS
Vocab load_vocab(const std::string& path) {
    const auto all = read_vec<uint8_t>(path);
    Vocab v;
    if (all.size() < 4) throw std::runtime_error("bad vocab " + path);
    std::memcpy(&v.n, all.data(), 4);
    const size_t off_bytes = (static_cast<size_t>(v.n) + 1) * 4;
    if (all.size() < 4 + off_bytes) throw std::runtime_error("bad vocab " + path);
    v.off.resize(v.n + 1);
    std::memcpy(v.off.data(), all.data() + 4, off_bytes);
    v.bytes.assign(all.begin() + 4 + off_bytes, all.end());
    if (v.off.back() != v.bytes.size()) throw std::runtime_error("bad vocab " + path);
    return v;
}

Column load_column(const std::string& prefix) {
    Column c;
    c.tok = read_vec<uint16_t>(prefix + ".tok");
    c.off = read_vec<uint64_t>(prefix + ".off");
    c.raw = read_vec<uint8_t>(prefix + ".raw");
    c.rawoff = read_vec<uint64_t>(prefix + ".rawoff");
    if (c.off.empty() || c.off.size() != c.rawoff.size())
        throw std::runtime_error("inconsistent column " + prefix);
    c.rows = c.off.size() - 1;
    return c;
}
#endif
