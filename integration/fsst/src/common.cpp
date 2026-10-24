#include "fsstlike/common.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace fsstlike {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "every on-disk array in this experiment is little-endian and read with memcpy");

void fail(const std::string& msg) { throw std::runtime_error(msg); }

std::vector<u8> read_file(const std::string& path) {
   FILE* f = std::fopen(path.c_str(), "rb");
   if (!f) fail(path + ": cannot open file (" + std::strerror(errno) + ")");
   if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); fail(path + ": cannot seek"); }
   long size = std::ftell(f);
   if (size < 0) { std::fclose(f); fail(path + ": cannot determine size"); }
   std::rewind(f);
   std::vector<u8> buf(static_cast<size_t>(size));
   if (size > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
      std::fclose(f);
      fail(path + ": short read");
   }
   std::fclose(f);
   return buf;
}

void write_file(const std::string& path, const void* data, size_t n) {
   // Write to a sibling temporary and rename, so an interrupted run never
   // leaves a half-written column behind for the next one to load.
   std::string tmp = path + ".tmp";
   FILE* f = std::fopen(tmp.c_str(), "wb");
   if (!f) fail(tmp + ": cannot create file (" + std::strerror(errno) + ")");
   if (n > 0 && std::fwrite(data, 1, n, f) != n) { std::fclose(f); fail(tmp + ": short write"); }
   if (std::fflush(f) != 0) { std::fclose(f); fail(tmp + ": flush failed"); }
   std::fclose(f);
   if (std::rename(tmp.c_str(), path.c_str()) != 0) fail(path + ": rename failed");
}

RawColumn load_raw(const std::string& prefix, u64 max_rows) {
   RawColumn c;
   c.off = read_array<u64>(prefix + ".rawoff");
   if (c.off.empty()) fail(prefix + ".rawoff: empty, expected at least one offset");
   if (c.off[0] != 0) fail(prefix + ".rawoff: off[0] is " + std::to_string(c.off[0]) + ", expected 0");
   u64 rows = c.off.size() - 1;
   if (max_rows && max_rows < rows) {
      rows = max_rows;
      c.off.resize(rows + 1);
   }
   c.rows = rows;
   c.bytes = read_file(prefix + ".raw");
   for (u64 i = 0; i < rows; ++i)
      if (c.off[i] > c.off[i + 1])
         fail(prefix + ".rawoff: not monotonic at row " + std::to_string(i));
   if (c.off[rows] > c.bytes.size())
      fail(prefix + ".rawoff: terminal offset " + std::to_string(c.off[rows]) + " exceeds " +
           prefix + ".raw size " + std::to_string(c.bytes.size()));
   if (!max_rows && c.off[rows] != c.bytes.size())
      fail(prefix + ".rawoff: terminal offset " + std::to_string(c.off[rows]) + " disagrees with " +
           prefix + ".raw size " + std::to_string(c.bytes.size()));
   c.bytes.resize(static_cast<size_t>(c.off[rows]));
   return c;
}

bool utf8_valid(const u8* p, size_t n) {
   size_t i = 0;
   while (i < n) {
      u8 b = p[i];
      size_t need;
      u8 lo = 0x80, hi = 0xBF;
      if (b <= 0x7F) { ++i; continue; }
      else if (b >= 0xC2 && b <= 0xDF) need = 1;
      else if (b == 0xE0) { need = 2; lo = 0xA0; }
      else if (b >= 0xE1 && b <= 0xEC) need = 2;
      else if (b == 0xED) { need = 2; hi = 0x9F; }
      else if (b >= 0xEE && b <= 0xEF) need = 2;
      else if (b == 0xF0) { need = 3; lo = 0x90; }
      else if (b >= 0xF1 && b <= 0xF3) need = 3;
      else if (b == 0xF4) { need = 3; hi = 0x8F; }
      else return false; // 0x80-0xC1 as a lead byte, or 0xF5-0xFF
      if (i + need >= n) return false; // continuation bytes i+1..i+need must exist
      if (p[i + 1] < lo || p[i + 1] > hi) return false;
      for (size_t k = 2; k <= need; ++k)
         if (p[i + k] < 0x80 || p[i + k] > 0xBF) return false;
      i += need + 1;
   }
   return true;
}

} // namespace fsstlike
