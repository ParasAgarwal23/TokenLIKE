// TokenLIKE - SQL LIKE matching directly over 16-bit token-id columns (Person A: matcher core).
//
// Semantics
//   - Case-sensitive SQL LIKE. '%' = zero or more code points, '_' = exactly one code point.
//     No escape character: '\' is literal. The pattern must be valid UTF-8 (else build() throws
//     std::invalid_argument). Rows are assumed to be valid UTF-8 once decoded.
//   - Output bitmap is bit-packed, LSB-first: row r matches iff (out[r >> 3] >> (r & 7)) & 1.
//     out is resized to ceil(rows / 8) bytes and overwritten; padding bits are zero.
//   - Stats: rows, matches, tokens_read are accumulated (+=); bytes_decoded / bytes_compared are
//     never touched (the matcher does not decode); build_us and table_kb are overwritten.
//   - Token ids must be < vocab.n. Out-of-range ids are not detected unless compiled with
//     TOKENLIKE_CHECKED (then match() throws std::out_of_range).
//   - build() throws tokenlike::TooComplex (a std::runtime_error) when the pattern needs more than
//     kMaxStates DFA states in both scan directions.
//
// ScanKind
//   Forward  : pattern does not start with '%' but ends with '%' (prefix), or starts and ends
//              with '%' (contains). Scanned left to right with early accept / reject.
//   Backward : pattern starts with '%' and does not end with '%' (suffix). Scanned right to left.
//   Anchored : pattern neither starts nor ends with '%' (exact, 'a%b'). Scanned left to right.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "tokenlike/common.h"
#include "tokenlike/dfa.h"

enum class ScanKind {
    Forward,
    Backward,
    Anchored
};

namespace tokenlike::detail {
struct MatcherAccess;
}

class Matcher {
public:
    static Matcher build(std::string_view pattern, const Vocab& v);

    void match(const Column& c, Bitmap& out, Stats& s) const;

    // Matches the concatenation a[r] || b[r]. Throws std::invalid_argument if row counts differ.
    void match_concat(const Column& a, const Column& b, Bitmap& out, Stats& s) const;

    ScanKind kind() const;

    size_t table_bytes() const;        // compact table + per-state flags
    size_t dense_table_bytes() const;  // states * vocab.n * sizeof(uint16_t)

private:
    friend struct tokenlike::detail::MatcherAccess;
    Matcher() = default;

    ScanKind kind_ = ScanKind::Forward;
    tokenlike::Dir dir_ = tokenlike::Dir::Forward;
    tokenlike::CompactTable tab_;
    std::vector<uint8_t> flags_;
    tokenlike::State start_ = 0;
    uint32_t vocab_n_ = 0;
    size_t dense_bytes_ = 0;
    double build_us_ = 0;
};

// Test and diagnostics hooks. Not part of the team interface.
namespace tokenlike::detail {

struct BuildOptions {
    int force_dir = -1;  // -1 automatic, 0 forward, 1 backward (no retry when forced)
    bool minimize = true;
    uint32_t max_states = kMaxStates;               // limit on the DFA the tables are built from
    uint32_t max_subset_states = kMaxSubsetStates;  // limit on subset construction
};

struct MatcherAccess {
    static Matcher build(std::string_view pattern, const Vocab& v, const BuildOptions& opt,
                         DenseTable* keep_dense = nullptr);
    static uint32_t states(const Matcher& m) { return m.tab_.S; }
    static Dir dir(const Matcher& m) { return m.dir_; }
    static const CompactTable& table(const Matcher& m) { return m.tab_; }
    static const std::vector<uint8_t>& flags(const Matcher& m) { return m.flags_; }
    // Runs the scan with the dense table instead of the compact one and/or without early exit.
    static void match_with(const Matcher& m, const DenseTable* dense, bool early_exit, const Column& a,
                           const Column* b, Bitmap& out, Stats& s);
};

// Throws std::invalid_argument on malformed offsets.
void validate_shape(const Column& c);
// Throws std::out_of_range if any token id is >= n.
void validate_tokens(const Column& c, uint32_t n);

}  // namespace tokenlike::detail
