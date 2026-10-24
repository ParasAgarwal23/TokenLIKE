#pragma once
//---------------------------------------------------------------------------
// Column loading. Owner: Person B.
//---------------------------------------------------------------------------
#include <string>

#include "tokenlike/common.h"

//---------------------------------------------------------------------------
/// Load an encoded string column given its path prefix.
///
/// Reads four little-endian binary files (e.g. prefix "data/clickbench/URL"):
///
///     <prefix>.tok     uint16[total_tokens]
///     <prefix>.off     uint64[rows + 1]
///     <prefix>.raw     uint8[total_bytes]
///     <prefix>.rawoff  uint64[rows + 1]
///
/// Row `i` occupies tokens [off[i], off[i + 1]) and raw bytes
/// [rawoff[i], rawoff[i + 1]). Both representations are indexed by the same
/// row id, so the token column and the raw column are row-aligned.
///
/// Validates that every file exists, that each size is a whole multiple of its
/// element width, that .off and .rawoff agree on the row count, that both are
/// monotonically non-decreasing and start at zero, and that their terminal
/// entries equal the token count and the raw byte length respectively.
///
/// Throws std::runtime_error naming the file and the specific violation (with
/// the offending index, where applicable) on any malformed input.
Column load_column(const std::string& prefix);
//---------------------------------------------------------------------------
/// Check that every token id in `c` is addressable in `v`.
///
/// Kept separate from load_column() because the frozen loader signature takes
/// no vocabulary. Call it after loading both when you want the stronger
/// guarantee; it is O(total_tokens).
///
/// Throws std::runtime_error naming the first offending row and token id.
void validate_column(const Column& c, const Vocab& v);
//---------------------------------------------------------------------------
