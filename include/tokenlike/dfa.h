// TokenLIKE - byte automata and token transition tables (Person A: matcher core).
//
// Pipeline: Pattern -> Nfa (epsilon-free) -> [reverse] -> Dfa (subset construction)
//           -> minimize -> lift over vocab (DenseTable) -> compress (CompactTable) -> verify.
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "tokenlike/common.h"
#include "tokenlike/pattern.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace tokenlike {

using State = uint16_t;

// Upper bound on MINIMIZED DFA states. The dense table costs states * vocab.n * 2 bytes
// (128 KB per state for a 65,536-token vocabulary), so 512 states is 64 MB at build time.
constexpr uint32_t kMaxStates = 512;

// Upper bound on subset-construction states before minimization (S * 512 bytes of
// transitions). Long literal-heavy patterns with '_' produce up to ~3x redundant raw states.
constexpr uint32_t kMaxSubsetStates = 4096;

enum class Dir : uint8_t { Forward, Backward };

// Thrown when determinization exceeds the state limit.
struct TooComplex : std::runtime_error {
    explicit TooComplex(const std::string& m) : std::runtime_error(m) {}
};

// ---------------------------------------------------------------- NFA (nfa.cpp)

struct Nfa {
    struct Edge {
        uint8_t lo, hi;  // inclusive byte range
        uint32_t dst;
    };
    std::vector<std::vector<Edge>> edges;  // per state
    std::vector<uint32_t> start;           // start set
    std::vector<uint8_t> accept;           // per state, 0/1

    uint32_t size() const { return static_cast<uint32_t>(edges.size()); }
    uint32_t add_state() {
        edges.emplace_back();
        accept.push_back(0);
        return size() - 1;
    }
};

// '%' is a self-loop on any byte, '_' is the (lenient) UTF-8 code point automaton
// 00-7F | C0-DF C | E0-EF C C | F0-F7 C C C   with C = 80-BF.
Nfa build_nfa(const Pattern& p);

// Recognizes exactly the byte-reversed language of `n`.
Nfa reverse_nfa(const Nfa& n);

// ---------------------------------------------------------------- DFA (dfa.cpp)

enum StateFlag : uint8_t {
    kAccept = 1,              // accepting state
    kDead = 2,                // no accepting state reachable: reject now
    kUniv = 4,                // every reachable state accepts: accept now
    kStop = kDead | kUniv
};

struct Dfa {
    uint32_t S = 0;
    State start = 0;
    std::vector<State> next;     // S * 256
    std::vector<uint8_t> flags;  // S, StateFlag bits

    State step(State s, uint8_t b) const { return next[size_t(s) * 256 + b]; }
    bool accepts(const uint8_t* p, size_t len) const;  // byte-level run, for tests
};

// Subset construction. Throws TooComplex above max_states. Result is classified.
Dfa determinize(const Nfa& n, uint32_t max_states = kMaxSubsetStates);

// Moore partition refinement. Result is classified.
Dfa minimize(const Dfa& d);

// Recomputes kDead / kUniv from kAccept by reachability.
void classify(Dfa& d);

// ---------------------------------------------------------------- tables

// T[s * n + t]: the state reached from s after consuming token t's bytes
// (in reverse byte order for Dir::Backward).
struct DenseTable {
    uint32_t S = 0, n = 0;
    std::vector<State> t;

    State next(State s, uint16_t tok) const { return t[size_t(s) * n + tok]; }
    size_t bytes() const { return size_t(S) * n * sizeof(State); }
};

// lift.cpp. Throws std::invalid_argument if vocab.n > 65536 or vocab offsets are malformed.
DenseTable lift(const Dfa& d, const Vocab& v, Dir dir);

inline uint32_t popcount64(uint64_t x) {
#if defined(_MSC_VER)
    return static_cast<uint32_t>(__popcnt64(x));
#else
    return static_cast<uint32_t>(__builtin_popcountll(x));
#endif
}

// Default transition per state + global exception-token bitmap (65536 bits = 8 KB)
// + rank directory + token-major exception rows E[rank(t) * S + s].
struct CompactTable {
    static constexpr uint32_t kWords = 65536 / 64;

    uint32_t S = 0, n = 0, exc = 0;
    std::vector<State> def;     // S
    std::vector<uint64_t> bits; // kWords
    std::vector<uint16_t> rank; // kWords, exceptions in words [0, w)
    std::vector<State> E;       // exc * S

    State next(State s, uint16_t tok) const {
        uint64_t w = bits[tok >> 6];
        uint64_t m = uint64_t(1) << (tok & 63);
        if (!(w & m)) return def[s];
        uint32_t r = rank[tok >> 6] + popcount64(w & (m - 1));
        return E[size_t(r) * S + s];
    }
    size_t bytes() const {
        return def.size() * sizeof(State) + bits.size() * sizeof(uint64_t) +
               rank.size() * sizeof(uint16_t) + E.size() * sizeof(State);
    }
};

// compress.cpp
CompactTable compress(const DenseTable& d);
// Exhaustively compares every (state, token < n) transition. Throws std::logic_error on mismatch.
void verify(const CompactTable& c, const DenseTable& d);

// ---------------------------------------------------------------- scanning (scan.cpp)

struct ScanSpec {
    const uint8_t* flags;  // per-state StateFlag bits
    State start;
    Dir dir;
    bool early_exit;
};

// Writes a bit-packed (LSB-first) match bitmap of a.rows bits into out.
// If b is non-null the scanned string of row r is a[r] || b[r].
// Stats: rows, matches and tokens_read are accumulated.
void scan(const CompactTable& t, const ScanSpec& sp, const Column& a, const Column* b, Bitmap& out, Stats& st);
void scan(const DenseTable& t, const ScanSpec& sp, const Column& a, const Column* b, Bitmap& out, Stats& st);

// Bitmap helpers (bit-packed, LSB-first: row r is bit (r & 7) of byte r >> 3).
inline bool bitmap_get(const Bitmap& b, uint64_t r) { return (b[r >> 3] >> (r & 7)) & 1; }

}  // namespace tokenlike
