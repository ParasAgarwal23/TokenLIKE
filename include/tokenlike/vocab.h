#pragma once
//---------------------------------------------------------------------------
// Vocabulary loading. Owner: Person B.
//---------------------------------------------------------------------------
#include <string>

#include "tokenlike/common.h"

//---------------------------------------------------------------------------
/// Load a frozen token vocabulary from disk.
///
/// File format (little-endian), as produced by scripts/encode/build_vocab.py:
///
///     uint32 n_tokens
///     uint32 offsets[n_tokens + 1]
///     uint8  token_bytes[offsets[n_tokens]]
///
/// Token `i` occupies token_bytes[offsets[i], offsets[i + 1]).
///
/// Validates that the header is complete, that n_tokens fits the uint16_t id
/// range, that the offset array is present in full, starts at zero, is
/// monotonically non-decreasing, and that its terminal entry equals the token
/// byte payload length. Every token is therefore safe to address via
/// Vocab::token() without further bounds checking.
///
/// Throws std::runtime_error naming the path and the specific violation if the
/// file is missing, truncated, or malformed.
Vocab load_vocab(const std::string& path);
//---------------------------------------------------------------------------
