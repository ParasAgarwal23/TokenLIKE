#include "tokenlike/dfa.h"

namespace tokenlike {

namespace {

// Adds the lenient UTF-8 code point automaton from `from` to `to`.
void add_any1(Nfa& n, uint32_t from, uint32_t to) {
    n.edges[from].push_back({0x00, 0x7F, to});
    // Lead byte classes and the number of continuation bytes that follow.
    const struct { uint8_t lo, hi; int cont; } leads[] = {
        {0xC0, 0xDF, 1}, {0xE0, 0xEF, 2}, {0xF0, 0xF7, 3}};
    for (const auto& l : leads) {
        uint32_t cur = n.add_state();
        n.edges[from].push_back({l.lo, l.hi, cur});
        for (int k = 1; k < l.cont; ++k) {
            uint32_t nx = n.add_state();
            n.edges[cur].push_back({0x80, 0xBF, nx});
            cur = nx;
        }
        n.edges[cur].push_back({0x80, 0xBF, to});
    }
}

}  // namespace

Nfa build_nfa(const Pattern& p) {
    Nfa n;
    uint32_t cur = n.add_state();
    n.start.push_back(cur);
    for (const Elem& e : p.elems) {
        switch (e.kind) {
            case ElemKind::Star:
                n.edges[cur].push_back({0x00, 0xFF, cur});
                break;
            case ElemKind::Lit:
                for (char ch : e.bytes) {
                    uint8_t b = static_cast<uint8_t>(ch);
                    uint32_t nx = n.add_state();
                    n.edges[cur].push_back({b, b, nx});
                    cur = nx;
                }
                break;
            case ElemKind::Any1: {
                uint32_t end = n.add_state();
                add_any1(n, cur, end);
                cur = end;
                break;
            }
        }
    }
    n.accept[cur] = 1;
    return n;
}

Nfa reverse_nfa(const Nfa& n) {
    Nfa r;
    r.edges.resize(n.size());
    r.accept.assign(n.size(), 0);
    for (uint32_t s = 0; s < n.size(); ++s) {
        for (const Nfa::Edge& e : n.edges[s]) r.edges[e.dst].push_back({e.lo, e.hi, s});
        if (n.accept[s]) r.start.push_back(s);
    }
    for (uint32_t s : n.start) r.accept[s] = 1;
    return r;
}

}  // namespace tokenlike
