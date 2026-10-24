// Shared types for the FSST-LIKE experiment.
//
// Isolated experiment: nothing here is shared with, or linked against, the
// original TokenLIKE repository. The row-id and bitmap conventions deliberately
// match TokenLIKE's so the two systems' outputs can be compared byte for byte.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsstlike {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

/// Every failure in this experiment is a std::runtime_error naming the file or
/// the offending index. Callers (CLI, tests) turn them into exit codes.
[[noreturn]] void fail(const std::string& msg);

/// Bit-packed match set, lowest bit first: row r matches iff
/// (b[r >> 3] >> (r & 7)) & 1. Same convention as TokenLIKE's Bitmap.
using Bitmap = std::vector<u8>;
inline size_t bitmap_bytes(u64 rows) { return static_cast<size_t>((rows + 7) / 8); }
inline void bitmap_set(Bitmap& b, u64 r) { b[r >> 3] |= static_cast<u8>(1u << (r & 7)); }
inline bool bitmap_get(const Bitmap& b, u64 r) { return (b[r >> 3] >> (r & 7)) & 1; }

std::vector<u8> read_file(const std::string& path);
void write_file(const std::string& path, const void* data, size_t n);

/// Reads a file as an array of fixed-width little-endian elements. Rejects a
/// size that is not a whole multiple of sizeof(T). Host is assumed
/// little-endian; a static_assert in common.cpp enforces that.
template <class T>
std::vector<T> read_array(const std::string& path) {
   std::vector<u8> raw = read_file(path);
   if (raw.size() % sizeof(T) != 0)
      fail(path + ": size " + std::to_string(raw.size()) + " is not a multiple of " + std::to_string(sizeof(T)));
   std::vector<T> out(raw.size() / sizeof(T));
   if (!out.empty()) __builtin_memcpy(out.data(), raw.data(), raw.size());
   return out;
}

/// An uncompressed string column: TokenLIKE's <prefix>.raw / <prefix>.rawoff
/// pair. We only ever read these files.
struct RawColumn {
   u64 rows = 0;
   std::vector<u8> bytes;
   std::vector<u64> off; // rows + 1 prefix sums, off[0] == 0

   const u8* row(u64 i) const { return bytes.data() + off[i]; }
   size_t len(u64 i) const { return static_cast<size_t>(off[i + 1] - off[i]); }
};

/// max_rows == 0 means "all rows".
RawColumn load_raw(const std::string& prefix, u64 max_rows = 0);

/// True iff [p, p+n) is well-formed UTF-8 (strict: no surrogates, no
/// overlongs, no code points above U+10FFFF).
bool utf8_valid(const u8* p, size_t n);

} // namespace fsstlike
