//---------------------------------------------------------------------------
// Vocabulary loading. Owner: Person B.
//---------------------------------------------------------------------------
#include "tokenlike/vocab.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
/// Maximum vocabulary size: token ids must fit in uint16_t.
constexpr uint64_t maxTokens = 65536;
//---------------------------------------------------------------------------
[[noreturn]] void fail(const std::string& path, const std::string& reason)
// Report a malformed vocabulary file
{
   throw std::runtime_error("load_vocab(" + path + "): " + reason);
}
//---------------------------------------------------------------------------
std::vector<char> readWholeFile(const std::string& path)
// Read a file into memory, or throw naming the path
{
   std::ifstream in(path, std::ios::binary | std::ios::ate);
   if (!in) fail(path, "cannot open file");

   const auto end = in.tellg();
   if (end < 0) fail(path, "cannot determine file size (is it a directory?)");
   in.seekg(0, std::ios::beg);

   std::vector<char> blob(static_cast<size_t>(end));
   if (!blob.empty() && !in.read(blob.data(), end))
      fail(path, "short read of " + std::to_string(blob.size()) + " bytes");
   return blob;
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
Vocab load_vocab(const std::string& path)
// Load a frozen token vocabulary from disk
{
   const auto blob = readWholeFile(path);

   // Header: uint32 n_tokens.
   if (blob.size() < sizeof(uint32_t))
      fail(path, "file is " + std::to_string(blob.size()) + " bytes, too small for a header");

   Vocab v;
   std::memcpy(&v.n, blob.data(), sizeof(uint32_t));

   if (v.n == 0) fail(path, "header declares 0 tokens");
   if (static_cast<uint64_t>(v.n) > maxTokens)
      fail(path, "header declares " + std::to_string(v.n) + " tokens, which exceeds the " + std::to_string(maxTokens) + " addressable by a uint16_t id");

   // Offsets: uint32[n + 1].
   const uint64_t offsetBytes = (static_cast<uint64_t>(v.n) + 1) * sizeof(uint32_t);
   const uint64_t headerBytes = sizeof(uint32_t) + offsetBytes;
   if (blob.size() < headerBytes)
      fail(path, "truncated offset array: need " + std::to_string(headerBytes) + " bytes for " + std::to_string(v.n) + " tokens, file has " + std::to_string(blob.size()));

   v.off.resize(v.n + 1);
   std::memcpy(v.off.data(), blob.data() + sizeof(uint32_t), offsetBytes);

   if (v.off.front() != 0)
      fail(path, "offsets[0] is " + std::to_string(v.off.front()) + ", must be 0");

   for (uint32_t i = 0; i < v.n; ++i)
      if (v.off[i] > v.off[i + 1])
         fail(path, "offsets not monotonic at index " + std::to_string(i) + ": " + std::to_string(v.off[i]) + " > " + std::to_string(v.off[i + 1]));

   // Payload: uint8[offsets[n]].
   const uint64_t payloadBytes = blob.size() - headerBytes;
   if (v.off.back() != payloadBytes)
      fail(path, "terminal offset is " + std::to_string(v.off.back()) + " but the token byte payload is " + std::to_string(payloadBytes) + " bytes");

   v.bytes.resize(payloadBytes);
   if (payloadBytes) std::memcpy(v.bytes.data(), blob.data() + headerBytes, payloadBytes);

   return v;
}
//---------------------------------------------------------------------------
