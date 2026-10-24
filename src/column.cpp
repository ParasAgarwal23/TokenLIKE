//---------------------------------------------------------------------------
// Column loading. Owner: Person B.
//---------------------------------------------------------------------------
#include "tokenlike/column.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
[[noreturn]] void fail(const std::string& prefix, const std::string& reason)
// Report a malformed column
{
   throw std::runtime_error("load_column(" + prefix + "): " + reason);
}
//---------------------------------------------------------------------------
template <typename T>
std::vector<T> readElements(const std::string& prefix, const std::string& path, const char* what)
// Read a whole file as an array of T, validating that it holds whole elements
{
   std::ifstream in(path, std::ios::binary | std::ios::ate);
   if (!in) fail(prefix, std::string("cannot open ") + what + " file " + path);

   const auto end = in.tellg();
   if (end < 0) fail(prefix, std::string("cannot determine size of ") + path + " (is it a directory?)");
   const auto size = static_cast<uint64_t>(end);

   if (size % sizeof(T) != 0)
      fail(prefix, path + " is " + std::to_string(size) + " bytes, not a whole multiple of the " + std::to_string(sizeof(T)) + "-byte element width");

   in.seekg(0, std::ios::beg);
   std::vector<T> out(size / sizeof(T));
   if (size && !in.read(reinterpret_cast<char*>(out.data()), end))
      fail(prefix, "short read of " + std::to_string(size) + " bytes from " + path);
   return out;
}
//---------------------------------------------------------------------------
void checkOffsets(const std::string& prefix, const std::vector<uint64_t>& off, const char* what, uint64_t payloadSize, const char* payloadWhat)
// Validate one offset array against the payload it indexes
{
   if (off.empty())
      fail(prefix, std::string(what) + " is empty; it must hold at least the terminating offset");
   if (off.front() != 0)
      fail(prefix, std::string(what) + "[0] is " + std::to_string(off.front()) + ", must be 0");

   for (size_t i = 0; i + 1 < off.size(); ++i)
      if (off[i] > off[i + 1])
         fail(prefix, std::string(what) + " not monotonic at row " + std::to_string(i) + ": " + std::to_string(off[i]) + " > " + std::to_string(off[i + 1]));

   if (off.back() != payloadSize)
      fail(prefix, std::string(what) + " terminates at " + std::to_string(off.back()) + " but " + payloadWhat + " holds " + std::to_string(payloadSize) + " elements");
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
Column load_column(const std::string& prefix)
// Load an encoded string column given its path prefix
{
   Column c;
   c.tok = readElements<uint16_t>(prefix, prefix + ".tok", "token");
   c.off = readElements<uint64_t>(prefix, prefix + ".off", "token offset");
   c.raw = readElements<uint8_t>(prefix, prefix + ".raw", "raw byte");
   c.rawoff = readElements<uint64_t>(prefix, prefix + ".rawoff", "raw offset");

   // Both offset arrays carry rows + 1 entries and must agree on the row count,
   // otherwise the token and raw representations are not row-aligned and no
   // downstream comparison between them means anything.
   if (c.off.empty()) fail(prefix, ".off is empty; it must hold at least the terminating offset");
   if (c.rawoff.empty()) fail(prefix, ".rawoff is empty; it must hold at least the terminating offset");
   if (c.off.size() != c.rawoff.size())
      fail(prefix, ".off implies " + std::to_string(c.off.size() - 1) + " rows but .rawoff implies " + std::to_string(c.rawoff.size() - 1) + " rows");

   c.rows = c.off.size() - 1;

   checkOffsets(prefix, c.off, ".off", c.tok.size(), ".tok");
   checkOffsets(prefix, c.rawoff, ".rawoff", c.raw.size(), ".raw");

   return c;
}
//---------------------------------------------------------------------------
void validate_column(const Column& c, const Vocab& v)
// Check that every token id in the column is addressable in the vocabulary
{
   for (uint64_t row = 0; row < c.rows; ++row) {
      for (uint64_t i = c.off[row]; i < c.off[row + 1]; ++i) {
         if (static_cast<uint32_t>(c.tok[i]) >= v.n)
            throw std::runtime_error("validate_column: row " + std::to_string(row) + " token slot " + std::to_string(i) + " holds id " + std::to_string(c.tok[i]) + ", which is outside the vocabulary of " + std::to_string(v.n) + " tokens");
      }
   }
}
//---------------------------------------------------------------------------
