#include <algorithm>
#include <map>

#include "tokenlike/dfa.h"

namespace tokenlike {

bool Dfa::accepts(const uint8_t* p, size_t len) const {
    State s = start;
    for (size_t i = 0; i < len; ++i) s = step(s, p[i]);
    return flags[s] & kAccept;
}

Dfa determinize(const Nfa& n, uint32_t max_states) {
    max_states = std::min<uint32_t>(max_states, 65535);
    using Set = std::vector<uint32_t>;
    std::map<Set, State> ids;
    std::vector<Set> sets;
    Dfa d;

    auto intern = [&](Set&& s) -> State {
        auto it = ids.find(s);
        if (it != ids.end()) return it->second;
        if (sets.size() >= max_states)
            throw TooComplex("pattern needs more than " + std::to_string(max_states) + " DFA states");
        State id = static_cast<State>(sets.size());
        ids.emplace(s, id);
        sets.push_back(std::move(s));
        return id;
    };

    Set init = n.start;
    std::sort(init.begin(), init.end());
    init.erase(std::unique(init.begin(), init.end()), init.end());
    d.start = intern(std::move(init));

    std::vector<std::vector<uint32_t>> succ(256);
    std::vector<uint32_t> mark(n.size(), UINT32_MAX);
    for (size_t cur = 0; cur < sets.size(); ++cur) {
        for (auto& v : succ) v.clear();
        for (uint32_t q : sets[cur])
            for (const Nfa::Edge& e : n.edges[q])
                for (uint32_t b = e.lo; b <= e.hi; ++b) succ[b].push_back(e.dst);
        d.next.resize((cur + 1) * 256);
        for (uint32_t b = 0; b < 256; ++b) {
            Set s;
            uint32_t gen = static_cast<uint32_t>(cur * 256 + b);
            for (uint32_t q : succ[b])
                if (mark[q] != gen) { mark[q] = gen; s.push_back(q); }
            std::sort(s.begin(), s.end());
            State id = intern(std::move(s));
            d.next[cur * 256 + b] = id;  // sets may have grown; index cur is stable
        }
    }
    d.S = static_cast<uint32_t>(sets.size());
    d.flags.assign(d.S, 0);
    for (uint32_t s = 0; s < d.S; ++s)
        for (uint32_t q : sets[s])
            if (n.accept[q]) { d.flags[s] = kAccept; break; }
    classify(d);
    return d;
}

Dfa minimize(const Dfa& d) {
    std::vector<uint32_t> cls(d.S);
    for (uint32_t s = 0; s < d.S; ++s) cls[s] = (d.flags[s] & kAccept) ? 1 : 0;
    uint32_t count = 0;
    for (;;) {
        std::map<std::vector<uint32_t>, uint32_t> sig;
        std::vector<uint32_t> nc(d.S);
        std::vector<uint32_t> key(257);
        for (uint32_t s = 0; s < d.S; ++s) {
            key[0] = cls[s];
            for (uint32_t b = 0; b < 256; ++b) key[b + 1] = cls[d.next[size_t(s) * 256 + b]];
            auto it = sig.emplace(key, static_cast<uint32_t>(sig.size())).first;
            nc[s] = it->second;
        }
        uint32_t nc_count = static_cast<uint32_t>(sig.size());
        cls.swap(nc);
        if (nc_count == count) break;
        count = nc_count;
    }
    Dfa m;
    m.S = count;
    m.start = static_cast<State>(cls[d.start]);
    m.next.assign(size_t(count) * 256, 0);
    m.flags.assign(count, 0);
    std::vector<uint8_t> done(count, 0);
    for (uint32_t s = 0; s < d.S; ++s) {
        uint32_t c = cls[s];
        if (done[c]) continue;
        done[c] = 1;
        m.flags[c] = d.flags[s] & kAccept;
        for (uint32_t b = 0; b < 256; ++b)
            m.next[size_t(c) * 256 + b] = static_cast<State>(cls[d.next[size_t(s) * 256 + b]]);
    }
    classify(m);
    return m;
}

void classify(Dfa& d) {
    std::vector<std::vector<State>> pred(d.S);
    for (uint32_t s = 0; s < d.S; ++s)
        for (uint32_t b = 0; b < 256; ++b) {
            State t = d.next[size_t(s) * 256 + b];
            if (pred[t].empty() || pred[t].back() != s) pred[t].push_back(static_cast<State>(s));
        }
    // reach[s] = 1 if some state with (flags & kAccept) == want is reachable from s.
    auto reach = [&](uint8_t want) {
        std::vector<uint8_t> r(d.S, 0);
        std::vector<State> stack;
        for (uint32_t s = 0; s < d.S; ++s)
            if ((d.flags[s] & kAccept) == want) { r[s] = 1; stack.push_back(static_cast<State>(s)); }
        while (!stack.empty()) {
            State t = stack.back();
            stack.pop_back();
            for (State p : pred[t])
                if (!r[p]) { r[p] = 1; stack.push_back(p); }
        }
        return r;
    };
    std::vector<uint8_t> can_acc = reach(kAccept), can_rej = reach(0);
    for (uint32_t s = 0; s < d.S; ++s) {
        uint8_t f = d.flags[s] & kAccept;
        if (!can_acc[s]) f |= kDead;
        if (!can_rej[s]) f |= kUniv;
        d.flags[s] = f;
    }
}

}  // namespace tokenlike
