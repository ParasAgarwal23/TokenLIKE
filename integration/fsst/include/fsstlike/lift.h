// Lifting a byte-level DFA onto an FSST symbol alphabet.
//
// For every DFA state s and every defined code c, precompute the state reached
// by feeding the bytes that c expands to:
//
//     sym[s][c] = delta*(s, symbol_bytes(c))
//
// Escapes are not lifted: <255, b> is one logical unit but b is a raw byte, so
// the scan loop applies the ordinary byte transition for it. Codes that the
// symbol table does not define, and code 255 itself, are mapped to the reject
// sink -- they cannot occur in valid compressed data, and sending them to DEAD
// means corrupt input can never be read as a match.
#pragma once

#include "fsstlike/dfa.h"
#include "fsstlike/fsstcol.h"

namespace fsstlike {

/// One uint16 per (state, code): 512 bytes per state. With at most 255 codes
/// this is small enough that it is usually the right answer; lift_compact()
/// exists so that claim can be measured rather than assumed.
struct LiftedDense {
   u32 n = 0;
   std::vector<u16> sym; // n * 256
   u64 build_ns = 0;
   size_t table_bytes() const { return sym.size() * sizeof(u16); }
};

/// Default target plus a sorted exception list per state, indexed by a 256-bit
/// presence bitmap with per-word cumulative counts so a lookup is O(1).
struct LiftedCompact {
   u32 n = 0;
   std::vector<u16> def;    // n
   std::vector<u64> mask;   // n * 4
   std::vector<u16> cum;    // n * 4, exceptions in words before this one
   std::vector<u32> excOff; // n + 1
   std::vector<u16> exc;
   u64 build_ns = 0;

   size_t table_bytes() const {
      return def.size() * 2 + mask.size() * 8 + cum.size() * 2 + excOff.size() * 4 + exc.size() * 2;
   }
   u16 at(u32 s, u8 c) const {
      size_t base = static_cast<size_t>(s) * 4;
      u32 w = c >> 6;
      u64 m = mask[base + w];
      u64 bit = 1ull << (c & 63);
      if (!(m & bit)) return def[s];
      return exc[excOff[s] + cum[base + w] + static_cast<u32>(__builtin_popcountll(m & (bit - 1)))];
   }
};

/// reverse_symbols feeds each symbol's bytes back to front, for the reverse
/// automaton used by suffix-anchored patterns.
LiftedDense lift_dense(const Dfa& d, const FsstColumn& c, bool reverse_symbols);
LiftedCompact lift_compact(const LiftedDense& dense);

// ---------------------------------------------------------------------------
// Reuse: one automaton, many symbol tables
// ---------------------------------------------------------------------------
//
// The two halves of building a compressed-domain matcher depend on different
// things, and the split is not incidental:
//
//   build_dfa(pattern)        depends only on the pattern. Parsing, the NFA,
//                             subset construction and Hopcroft minimisation
//                             never look at a symbol table.
//   lift_dense(dfa, column)   depends on the pattern *and* the symbol table,
//                             but only reads dfa.delta -- it does not modify
//                             the automaton.
//
// FSST calibrates a symbol table per block or per column, so a single query
// predicate typically has to be matched against many tables. Because
// lift_dense is a pure function of (dfa, table), the expensive half can be
// done once and the cheap half repeated. Measured on this machine, the DFA is
// the expensive half by an order of magnitude; `fsst_liftbench` reports the
// real numbers for a given pattern and set of columns, and also checks that
// reuse gives bit-identical results to rebuilding from scratch.
//
// LiftedSet is the safe form of that reuse: it owns the automaton so a caller
// cannot lift against a Dfa that has since been reassigned, and it records
// which table each entry was built for.

struct LiftedSet {
   Dfa dfa;
   std::vector<LiftedDense> tables;
   std::vector<std::string> table_ids; // caller's label per entry, for diagnostics
   u64 lift_ns_total = 0;

   /// Lifts the owned automaton onto one more symbol table.
   const LiftedDense& add(const FsstColumn& c, const std::string& id, bool reverse_symbols = false);
};

/// Builds the automaton once and lifts it onto every column given.
LiftedSet lift_for(const Pattern& p, bool reverse, u32 max_states,
                   const std::vector<const FsstColumn*>& columns,
                   const std::vector<std::string>& ids);

} // namespace fsstlike
