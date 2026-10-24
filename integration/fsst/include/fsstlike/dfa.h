// Byte-level DFA for a SQL LIKE pattern.
//
// Pipeline: pattern -> Thompson NFA over byte ranges -> subset construction ->
// Hopcroft minimisation -> canonical renumbering.
//
// Why a *byte* DFA when LIKE is defined on characters: FSST symbols are
// arbitrary 1-8 byte sequences and routinely cut a multi-byte UTF-8 character
// in half, so the automaton has to be able to stop between any two bytes.
// Character semantics are therefore pushed into the automaton itself:
//
//   '_'  expands to a nine-branch sub-automaton accepting exactly one
//        well-formed UTF-8 character (1-4 bytes, no surrogates, no overlongs).
//   '%'  expands to a self-loop over all 256 bytes.
//
// '%' as "any bytes" is exact for well-formed UTF-8 input, not an
// approximation: UTF-8 is self-synchronising (lead bytes 0x00-0x7F, 0xC2-0xF4
// and continuation bytes 0x80-0xBF are disjoint), so a valid UTF-8 literal can
// only ever align on a character boundary, and the '_' sub-automaton cannot
// start on a continuation byte. Both are checked against DuckDB on real
// non-ASCII data by scripts/check.py.
#pragma once

#include "fsstlike/pattern.h"

namespace fsstlike {

struct Dfa {
   /// State 0 is always the reject sink and state 1 the accept sink, whether or
   /// not the pattern actually reaches them. That makes "s < 2" the single
   /// early-exit test in every scan loop, with accept[s] still giving the
   /// verdict: accept[0] == 0, accept[1] == 1.
   static constexpr u16 DEAD = 0;
   static constexpr u16 ASINK = 1;

   u32 n = 0;      // number of states, including slots 0 and 1
   u32 start = 0;
   std::vector<u16> delta;  // n * 256, row-major: delta[(s << 8) | c]
   std::vector<u8> accept;  // n
   bool dead_reachable = false;
   bool asink_reachable = false;
   // Three different counts, kept separate because conflating them was a real
   // reporting bug: a table that showed "5 -> 7" looked like minimisation had
   // *added* states, when in fact 5 was the subset-construction count and 7 was
   // the final count including the two reserved sink slots.
   u32 states_subset = 0;   // after subset construction, before minimisation
   u32 states_minimal = 0;  // Hopcroft blocks: the true minimal automaton
   // n (above) is states_minimal with the dead and accept-sink classes mapped
   // onto the reserved slots 0 and 1, which are always allocated even when the
   // pattern never reaches them. So n == states_minimal + (2 - classes merged).
   u64 build_ns = 0;

   u16 step(u32 s, u8 c) const { return delta[(s << 8) | c]; }
   size_t table_bytes() const { return delta.size() * sizeof(u16) + accept.size(); }
};

/// reverse == true builds the automaton for the reversed pattern, for scanning
/// a string from its last byte to its first.
/// Throws if the minimised automaton would exceed max_states.
Dfa build_dfa(const Pattern& p, bool reverse, u32 max_states = 20000);

/// Feeds n bytes through the DFA starting from state s. Used both by the raw
/// baseline and to precompute lifted symbol transitions.
inline u32 run_bytes(const Dfa& d, u32 s, const u8* p, size_t n) {
   const u16* t = d.delta.data();
   for (size_t i = 0; i < n; ++i) s = t[(s << 8) | p[i]];
   return s;
}

} // namespace fsstlike
