// TokenLIKE shared structures. FROZEN: changes require agreement of A, B and C.
//
// Conventions (all owners rely on these):
//   * Offset arrays are prefix sums with a leading 0:
//       Vocab.off.size()     == n + 1      token id t = bytes[off[t] .. off[t+1])
//       Column.off.size()    == rows + 1   row i tokens = tok[off[i] .. off[i+1])
//       Column.rawoff.size() == rows + 1   row i text   = raw[rawoff[i] .. rawoff[i+1])
//   * Vocabulary size n <= 65536 (ids are uint16).
//   * Text is valid UTF-8. NULL values are stored as empty strings.
//   * Bitmap is BIT-PACKED, LSB-first: row r matches iff (b[r >> 3] >> (r & 7)) & 1.
//     size() == ceil(rows / 8); padding bits are zero. The callee of a match
//     function resizes and overwrites it.
//   * Stats: the callee ACCUMULATES (+=) rows, matches, tokens_read,
//     bytes_decoded and bytes_compared; it may overwrite build_us / table_kb.
//     Pass a zeroed Stats to measure one call.
//     Counters are accumulated per row (never per byte in an inner loop) so
//     that statistics collection does not distort timing.
//   * LIKE semantics: case-sensitive, '%' = any sequence of characters,
//     '_' = exactly one Unicode code point, no ESCAPE character
//     (verified to match DuckDB's LIKE).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

struct Vocab {
    uint32_t n = 0;
    std::vector<uint32_t> off;
    std::vector<uint8_t> bytes;

    std::string_view token(uint16_t id) const {
        return std::string_view(reinterpret_cast<const char*>(bytes.data()) + off[id],
                                off[id + 1] - off[id]);
    }
};

struct Column {
    uint64_t rows = 0;
    std::vector<uint16_t> tok;
    std::vector<uint64_t> off;
    std::vector<uint8_t> raw;
    std::vector<uint64_t> rawoff;
};

using Bitmap = std::vector<uint8_t>;

inline size_t bitmap_bytes(uint64_t rows) { return static_cast<size_t>((rows + 7) / 8); }
inline bool bitmap_bit(const Bitmap& b, uint64_t r) { return (b[r >> 3] >> (r & 7)) & 1; }

struct Stats {
    uint64_t rows = 0;
    uint64_t matches = 0;
    uint64_t tokens_read = 0;
    uint64_t bytes_decoded = 0;
    uint64_t bytes_compared = 0;

    double build_us = 0;
    double table_kb = 0;
};
