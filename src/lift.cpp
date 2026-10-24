#include "tokenlike/dfa.h"

namespace tokenlike {

DenseTable lift(const Dfa& d, const Vocab& v, Dir dir) {
    if (v.n > 65536) throw std::invalid_argument("vocabulary has more than 65536 tokens");
    if (v.off.size() != size_t(v.n) + 1 || (v.n > 0 && v.off[v.n] > v.bytes.size()))
        throw std::invalid_argument("malformed vocabulary offsets");
    DenseTable t;
    t.S = d.S;
    t.n = v.n;
    t.t.resize(size_t(d.S) * v.n);
    const uint8_t* bytes = v.bytes.data();
    for (uint32_t tok = 0; tok < v.n; ++tok) {
        const uint8_t* b = bytes + v.off[tok];
        const uint8_t* e = bytes + v.off[tok + 1];
        for (uint32_t s0 = 0; s0 < d.S; ++s0) {
            State s = static_cast<State>(s0);
            if (dir == Dir::Forward)
                for (const uint8_t* p = b; p != e; ++p) s = d.step(s, *p);
            else
                for (const uint8_t* p = e; p != b;) s = d.step(s, *--p);
            t.t[size_t(s0) * v.n + tok] = s;
        }
    }
    return t;
}

}  // namespace tokenlike
