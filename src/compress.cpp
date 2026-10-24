#include <algorithm>
#include <string>

#include "tokenlike/dfa.h"

namespace tokenlike {

CompactTable compress(const DenseTable& d) {
    CompactTable c;
    c.S = d.S;
    c.n = d.n;
    c.def.assign(d.S, 0);
    c.bits.assign(CompactTable::kWords, 0);
    c.rank.assign(CompactTable::kWords, 0);

    // Default transition = most frequent target of each row (ties -> lowest state id).
    std::vector<uint32_t> cnt(d.S);
    for (uint32_t s = 0; s < d.S; ++s) {
        std::fill(cnt.begin(), cnt.end(), 0);
        const State* row = d.t.data() + size_t(s) * d.n;
        for (uint32_t t = 0; t < d.n; ++t) ++cnt[row[t]];
        uint32_t best = 0;
        for (uint32_t k = 1; k < d.S; ++k)
            if (cnt[k] > cnt[best]) best = k;
        c.def[s] = static_cast<State>(best);
    }

    // A token is an exception if any state leaves it off its default.
    for (uint32_t s = 0; s < d.S; ++s) {
        const State* row = d.t.data() + size_t(s) * d.n;
        for (uint32_t t = 0; t < d.n; ++t)
            if (row[t] != c.def[s]) c.bits[t >> 6] |= uint64_t(1) << (t & 63);
    }

    uint32_t acc = 0;
    for (uint32_t w = 0; w < CompactTable::kWords; ++w) {
        c.rank[w] = static_cast<uint16_t>(acc);
        acc += popcount64(c.bits[w]);
    }
    c.exc = acc;

    c.E.resize(size_t(c.exc) * d.S);
    uint32_t r = 0;
    for (uint32_t t = 0; t < d.n; ++t) {
        if (!((c.bits[t >> 6] >> (t & 63)) & 1)) continue;
        for (uint32_t s = 0; s < d.S; ++s) c.E[size_t(r) * d.S + s] = d.t[size_t(s) * d.n + t];
        ++r;
    }
    return c;
}

void verify(const CompactTable& c, const DenseTable& d) {
    if (c.S != d.S || c.n != d.n) throw std::logic_error("compact table shape differs from dense table");
    for (uint32_t s = 0; s < d.S; ++s)
        for (uint32_t t = 0; t < d.n; ++t) {
            State want = d.t[size_t(s) * d.n + t];
            State got = c.next(static_cast<State>(s), static_cast<uint16_t>(t));
            if (got != want)
                throw std::logic_error("compact table mismatch at state " + std::to_string(s) + ", token " +
                                       std::to_string(t));
        }
}

}  // namespace tokenlike
