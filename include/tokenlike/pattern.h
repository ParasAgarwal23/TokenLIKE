// TokenLIKE - SQL LIKE pattern representation (Person A: matcher core).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tokenlike {

enum class ElemKind : uint8_t {
    Lit,   // literal bytes (consecutive literal characters are merged)
    Any1,  // '_' : exactly one UTF-8 encoded code point
    Star   // '%' : zero or more code points
};

struct Elem {
    ElemKind kind;
    std::string bytes;  // only used by Lit
};

// Normalized pattern: no two consecutive Star, no two consecutive Lit.
// No escape character is supported: '\' is an ordinary literal byte.
struct Pattern {
    std::vector<Elem> elems;

    bool starts_with_star() const { return !elems.empty() && elems.front().kind == ElemKind::Star; }
    bool ends_with_star() const { return !elems.empty() && elems.back().kind == ElemKind::Star; }
    bool has_any1() const;
    size_t interior_stars() const;  // Star elements that are neither first nor last
};

// True if s is well-formed UTF-8 (no overlongs, no surrogates, <= U+10FFFF).
bool valid_utf8(std::string_view s);

// Parses a LIKE pattern. Throws std::invalid_argument if the pattern is not valid UTF-8.
Pattern parse_like(std::string_view pattern);

// Canonical LIKE text of a normalized pattern (for logging and tests).
std::string to_string(const Pattern& p);

}  // namespace tokenlike
