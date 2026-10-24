// An *optimised* raw-string LIKE matcher, for a fair baseline.
//
// Why this file exists. The first version of this experiment used one generic
// forward byte DFA as the "raw" baseline for every pattern shape. That is a
// correct matcher but not a fair one: no real system scans an entire string
// with an automaton to answer `value LIKE '%.php'`. It compares four bytes at
// the end. Measuring against the DFA therefore overstated the compressed-domain
// advantage on suffix, exact and substring patterns, which is exactly what the
// review caught. This matcher does what a real system does, so the comparison
// in FINAL_REPORT.md §J means something.
//
// It is an optimisation of the *baseline*, not a handicap: fast paths first,
// the platform's `memmem` for substring search, and a correct general fallback.
// It is checked against DuckDB on the same 2,528-case suite as every other
// mode, and against the independent backtracking reference matcher in the unit
// tests, so "optimised" never gets to mean "wrong".
//
// Shapes and how each is answered:
//
//   (empty)        n == 0
//   %              true
//   abc            n == 3 and memcmp
//   abc%           n >= 3 and memcmp at the front
//   %abc           n >= 3 and memcmp at the back
//   %abc%          memmem
//   everything else, including every pattern containing '_':
//                  the pattern is cut at '%' into segments of fixed character
//                  length; the first is anchored at the start if the pattern
//                  does not begin with '%', the last at the end if it does not
//                  end with '%', and the middle ones are matched greedily
//                  left to right. Greedy-leftmost is correct here because a
//                  '%' between two segments can absorb anything, so matching a
//                  segment earlier never costs a later one a match.
//
// Within a segment, a literal run is compared with memcmp and a '_' consumes
// exactly one well-formed UTF-8 character (1-4 bytes), so a segment match at a
// given offset is deterministic and yields one end offset. Segments that begin
// with a literal are located with `memmem` on that literal; segments that begin
// with '_' are scanned one character at a time, which is rare in practice and
// documented rather than hidden.
#pragma once

#include "fsstlike/pattern.h"

namespace fsstlike {

struct RawPlan {
   enum class Shape { Empty, AnyString, Exact, Prefix, Suffix, Contains, General };

   struct Piece {
      bool wildcard = false;   // true => exactly one UTF-8 character
      std::vector<u8> bytes;   // literal run, when !wildcard
   };
   struct Segment {
      std::vector<Piece> pieces;
      size_t literal_bytes = 0; // total bytes of the literal pieces
      size_t wildcards = 0;     // number of '_' pieces
      bool pure_literal = false;
      std::vector<u8> literal;  // set when pure_literal
      size_t min_bytes() const { return literal_bytes + wildcards; }      // '_' >= 1 byte
      size_t max_bytes() const { return literal_bytes + wildcards * 4; }  // '_' <= 4 bytes
      bool leads_with_literal() const { return !pieces.empty() && !pieces[0].wildcard; }
   };

   Shape shape = Shape::General;
   std::vector<u8> lit;             // Exact / Prefix / Suffix / Contains
   std::vector<Segment> segs;       // General
   bool anchored_start = false;
   bool anchored_end = false;

   const char* shape_name() const;
};

RawPlan compile_raw(const Pattern& p);
bool raw_match(const RawPlan& plan, const u8* s, size_t n);

} // namespace fsstlike
