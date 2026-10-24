// SQL LIKE pattern parsing, with DuckDB's semantics.
//
// Measured facts this file encodes (reproduced in scripts/semantics.py):
//   * '%' matches zero or more *characters*.
//   * '_' matches exactly one *character* (code point), not one byte.
//   * Matching is case-sensitive.
//   * Backslash is an ordinary literal byte. DuckDB only treats it as an
//     escape when the query says ESCAPE '\', which we never write.
#pragma once

#include "fsstlike/common.h"

namespace fsstlike {

enum class ItemKind { Literal, AnyChar, Star };

struct PatternItem {
   ItemKind kind;
   std::vector<u8> bytes; // Literal only
};

struct Pattern {
   std::string text;
   std::vector<PatternItem> items;

   /// True when the pattern starts/ends with '%' (so it is not anchored there).
   bool starts_with_star() const { return !items.empty() && items.front().kind == ItemKind::Star; }
   bool ends_with_star() const { return !items.empty() && items.back().kind == ItemKind::Star; }
   bool has_any_char() const;
};

/// Throws if the pattern is not well-formed UTF-8. Consecutive '%' collapse.
Pattern parse_like(const std::string& text);

/// Byte length of the well-formed UTF-8 character starting at s[i], or 0 if
/// s[i] is not a valid lead byte or the character is truncated.
size_t utf8_char_len(const u8* s, size_t n, size_t i);

/// Reference LIKE matcher: straightforward backtracking over characters.
/// Deliberately independent of the DFA path so the tests have a second
/// opinion that is not derived from the thing under test.
bool like_reference(const u8* s, size_t slen, const Pattern& p);

} // namespace fsstlike
