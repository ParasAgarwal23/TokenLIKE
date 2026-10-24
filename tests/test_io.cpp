//---------------------------------------------------------------------------
// Tests for the vocabulary and column loaders. Owner: Person B.
//
// Covers the happy path against real generated data, plus a corruption fixture
// per validation rule: a loader that silently accepts a malformed column would
// turn every downstream mismatch into a wild goose chase, so each rule is
// proven to actually fire.
//
// Standalone on purpose, so it runs before Person C's CMake exists:
//
//   clang++ -std=c++20 -O2 -Iinclude -I<stub_include> \
//       src/vocab.cpp src/column.cpp tests/test_io.cpp -o build/test_io
//   ./build/test_io [vocab.bin] [column_prefix] [tmpdir]
//---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "tokenlike/column.h"
#include "tokenlike/vocab.h"
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
unsigned failures = 0;
unsigned checks = 0;
/// The frozen vocabulary produced by scripts/encode/build_vocab.py.
constexpr uint32_t frozenTokens = 65536;
constexpr size_t frozenPayload = 367981;
//---------------------------------------------------------------------------
void check(bool ok, const std::string& what)
// Assert a condition, recording rather than aborting so one run reports all
{
   ++checks;
   if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << what << "\n";
   }
}
//---------------------------------------------------------------------------
void expectThrow(const std::string& what, const std::function<void()>& fn)
// Assert that a malformed input is rejected, and show the message if it is
{
   ++checks;
   try {
      fn();
   } catch (const std::exception& e) {
      std::cout << "  rejected " << what << ": " << e.what() << "\n";
      return;
   }
   ++failures;
   std::cerr << "FAIL: " << what << " was accepted but should have been rejected\n";
}
//---------------------------------------------------------------------------
void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes)
// Write a fixture file
{
   std::ofstream out(path, std::ios::binary | std::ios::trunc);
   if (!out) throw std::runtime_error("cannot write fixture " + path);
   if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
//---------------------------------------------------------------------------
template <typename T>
void appendLE(std::vector<uint8_t>& out, T value)
// Append one little-endian element (the host is assumed little-endian, as is
// the rest of the pipeline; the data format is defined as little-endian)
{
   uint8_t raw[sizeof(T)];
   std::memcpy(raw, &value, sizeof(T));
   out.insert(out.end(), raw, raw + sizeof(T));
}
//---------------------------------------------------------------------------
std::vector<uint8_t> makeVocabBlob(uint32_t n, const std::vector<uint32_t>& offsets, const std::string& payload)
// Build a vocab.bin image from explicit parts, so tests can break one at a time
{
   std::vector<uint8_t> blob;
   appendLE<uint32_t>(blob, n);
   for (uint32_t off : offsets) appendLE<uint32_t>(blob, off);
   blob.insert(blob.end(), payload.begin(), payload.end());
   return blob;
}
//---------------------------------------------------------------------------
struct ColumnFixture {
   std::vector<uint16_t> tok{1, 2, 3};
   std::vector<uint64_t> off{0, 2, 3};
   std::vector<uint8_t> raw{'a', 'b', 'c', 'd'};
   std::vector<uint64_t> rawoff{0, 3, 4};

   void write(const std::string& prefix) const
   // Materialize the four column files
   {
      std::vector<uint8_t> b;
      for (uint16_t v : tok) appendLE<uint16_t>(b, v);
      writeBytes(prefix + ".tok", b);
      b.clear();
      for (uint64_t v : off) appendLE<uint64_t>(b, v);
      writeBytes(prefix + ".off", b);
      writeBytes(prefix + ".raw", raw);
      b.clear();
      for (uint64_t v : rawoff) appendLE<uint64_t>(b, v);
      writeBytes(prefix + ".rawoff", b);
   }
};
//---------------------------------------------------------------------------
bool testRealVocab(const std::string& path)
// Happy path: the frozen vocabulary produced by build_vocab.py.
// Returns whether `path` really is the frozen vocabulary, so that callers can
// skip the checks that are only meaningful against it.
{
   std::cout << "== load_vocab(" << path << ")\n";
   const Vocab v = load_vocab(path);
   check(v.n == frozenTokens, "frozen vocabulary holds 65536 tokens, got " + std::to_string(v.n));
   check(v.off.size() == v.n + 1u, "offset array holds n + 1 entries");
   check(v.off.front() == 0, "offsets start at 0");
   check(v.off.back() == v.bytes.size(), "terminal offset matches the payload length");
   check(v.bytes.size() == frozenPayload, "payload is 367981 bytes, got " + std::to_string(v.bytes.size()));

   // Every id must be addressable, and all 256 single-byte tokens must exist,
   // which is what lets greedy longest-match encoding never dead-end.
   unsigned singles = 0;
   for (uint32_t id = 0; id < v.n; ++id)
      if (v.token(static_cast<uint16_t>(id)).size() == 1) ++singles;
   check(singles == 256, "vocabulary holds 256 single-byte tokens, got " + std::to_string(singles));

   // Spot-check a known id against the segmentation observed from the encoder.
   // Vocab::token() does no bounds checking, so the id must be in range first:
   // probing it against a smaller vocabulary is a heap read past the end of
   // Vocab::off, which AddressSanitizer reports as a heap-buffer-overflow.
   constexpr uint16_t probeId = 23638;
   if (v.n > probeId)
      check(v.token(probeId) == "http", "token 23638 is \"http\", got \"" + std::string(v.token(probeId)) + "\"");
   else
      check(false, "vocabulary has only " + std::to_string(v.n) + " tokens, too few to hold id " + std::to_string(probeId));
   std::cout << "  n=" << v.n << " payload=" << v.bytes.size() << " bytes\n";
   return v.n == frozenTokens && v.bytes.size() == frozenPayload;
}
//---------------------------------------------------------------------------
void testRealColumn(const std::string& prefix, const std::string& vocabPath)
// Happy path: decode every row through the loaders and compare with .raw
{
   std::cout << "== load_column(" << prefix << ")\n";
   const Vocab v = load_vocab(vocabPath);
   const Column c = load_column(prefix);

   check(c.rows > 0, "column holds at least one row");
   check(c.off.size() == c.rows + 1, ".off holds rows + 1 entries");
   check(c.rawoff.size() == c.rows + 1, ".rawoff holds rows + 1 entries");
   check(c.off.back() == c.tok.size(), "terminal token offset matches the token count");
   check(c.rawoff.back() == c.raw.size(), "terminal raw offset matches the raw byte count");

   try {
      validate_column(c, v);
   } catch (const std::exception& e) {
      check(false, std::string("validate_column rejected the column: ") + e.what());
      std::cout << "  skipping the round-trip: the column does not match this vocabulary\n";
      return;
   }

   // This is the same guarantee roundtrip.py checks in Python; doing it here
   // too means a format bug cannot hide behind agreement between one writer
   // and one reader.
   uint64_t mismatches = 0;
   for (uint64_t row = 0; row < c.rows; ++row) {
      std::string decoded;
      for (uint64_t i = c.off[row]; i < c.off[row + 1]; ++i) decoded += v.token(c.tok[i]);
      const std::string expected(reinterpret_cast<const char*>(c.raw.data()) + c.rawoff[row], c.rawoff[row + 1] - c.rawoff[row]);
      if (decoded != expected) {
         if (++mismatches <= 3)
            std::cerr << "  row " << row << " decoded \"" << decoded << "\" != raw \"" << expected << "\"\n";
      }
   }
   check(mismatches == 0, "all " + std::to_string(c.rows) + " rows round-trip through the loaders, mismatches=" + std::to_string(mismatches));
   std::cout << "  rows=" << c.rows << " tokens=" << c.tok.size() << " raw=" << c.raw.size() << " bytes\n";
}
//---------------------------------------------------------------------------
void testVocabCorruption(const std::filesystem::path& tmp)
// One fixture per load_vocab validation rule
{
   std::cout << "== load_vocab rejects malformed input\n";
   const auto path = (tmp / "v.bin").string();

   expectThrow("a missing file", [&] { load_vocab((tmp / "does_not_exist.bin").string()); });

   writeBytes(path, {0x02, 0x00});
   expectThrow("a truncated header", [&] { load_vocab(path); });

   writeBytes(path, makeVocabBlob(0, {0}, ""));
   expectThrow("a zero-token vocabulary", [&] { load_vocab(path); });

   writeBytes(path, makeVocabBlob(70000, {0}, ""));
   expectThrow("more tokens than a uint16_t id can address", [&] { load_vocab(path); });

   // Declares 3 tokens but supplies only 2 offsets.
   writeBytes(path, makeVocabBlob(3, {0, 1}, "ab"));
   expectThrow("a truncated offset array", [&] { load_vocab(path); });

   writeBytes(path, makeVocabBlob(3, {1, 2, 3, 4}, "abcd"));
   expectThrow("offsets[0] != 0", [&] { load_vocab(path); });

   writeBytes(path, makeVocabBlob(3, {0, 3, 1, 4}, "abcd"));
   expectThrow("non-monotonic offsets", [&] { load_vocab(path); });

   writeBytes(path, makeVocabBlob(3, {0, 1, 3, 9}, "abcd"));
   expectThrow("a terminal offset past the payload", [&] { load_vocab(path); });

   // Sanity: the shape these fixtures are derived from must itself be accepted,
   // otherwise the rejections above prove nothing.
   writeBytes(path, makeVocabBlob(3, {0, 1, 3, 4}, "abcd"));
   const Vocab v = load_vocab(path);
   check(v.n == 3 && v.token(0) == "a" && v.token(1) == "bc" && v.token(2) == "d", "the well-formed control fixture loads correctly");
}
//---------------------------------------------------------------------------
void testColumnCorruption(const std::filesystem::path& tmp)
// One fixture per load_column validation rule
{
   std::cout << "== load_column rejects malformed input\n";
   const auto prefix = (tmp / "c").string();

   expectThrow("a missing column", [&] { load_column((tmp / "nope").string()); });

   // Control: the baseline fixture must load, or nothing below is meaningful.
   const ColumnFixture good;
   good.write(prefix);
   const Column c = load_column(prefix);
   check(c.rows == 2, "the well-formed control fixture reports 2 rows, got " + std::to_string(c.rows));

   {  // .tok holding a partial uint16
      ColumnFixture f = good;
      f.write(prefix);
      writeBytes(prefix + ".tok", {0x01, 0x00, 0x02});
      expectThrow(".tok with a partial element", [&] { load_column(prefix); });
   }
   {  // .off holding a partial uint64
      ColumnFixture f = good;
      f.write(prefix);
      writeBytes(prefix + ".off", {0, 0, 0, 0});
      expectThrow(".off with a partial element", [&] { load_column(prefix); });
   }
   {  // row counts disagree between the two offset arrays
      ColumnFixture f = good;
      f.rawoff = {0, 1, 3, 4};
      f.write(prefix);
      expectThrow(".off and .rawoff disagreeing on the row count", [&] { load_column(prefix); });
   }
   {  // .off does not start at zero
      ColumnFixture f = good;
      f.off = {1, 2, 3};
      f.write(prefix);
      expectThrow(".off[0] != 0", [&] { load_column(prefix); });
   }
   {  // .off decreases
      ColumnFixture f = good;
      f.off = {0, 3, 2};
      f.tok = {1, 2};
      f.write(prefix);
      expectThrow("non-monotonic .off", [&] { load_column(prefix); });
   }
   {  // .off terminates short of the token count
      ColumnFixture f = good;
      f.off = {0, 1, 2};
      f.write(prefix);
      expectThrow(".off terminating before the end of .tok", [&] { load_column(prefix); });
   }
   {  // .rawoff terminates past the raw payload
      ColumnFixture f = good;
      f.rawoff = {0, 3, 9};
      f.write(prefix);
      expectThrow(".rawoff terminating past the end of .raw", [&] { load_column(prefix); });
   }
   {  // token id outside the vocabulary
      ColumnFixture f = good;
      f.tok = {1, 2, 9999};
      f.write(prefix);
      const auto vocabPath = (tmp / "small.bin").string();
      writeBytes(vocabPath, makeVocabBlob(3, {0, 1, 3, 4}, "abcd"));
      const Vocab small = load_vocab(vocabPath);
      const Column loaded = load_column(prefix);
      expectThrow("a token id outside the vocabulary", [&] { validate_column(loaded, small); });
   }

   good.write(prefix);
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
int main(int argc, char** argv)
{
   const std::string vocabPath = argc > 1 ? argv[1] : "data/vocab.bin";
   const std::string columnPrefix = argc > 2 ? argv[2] : "data/sample/urls";
   const std::filesystem::path tmp = argc > 3 ? argv[3] : "build/test_io_tmp";

   try {
      std::filesystem::create_directories(tmp);
      // A vocabulary that loads but is not the frozen one cannot validate the
      // real column, so say so and carry on with the fixture-driven checks
      // rather than aborting the whole run.
      if (testRealVocab(vocabPath)) {
         testRealColumn(columnPrefix, vocabPath);
      } else {
         std::cout << "== skipping " << columnPrefix
                   << ": " << vocabPath << " is not the frozen vocabulary\n";
      }
      testVocabCorruption(tmp);
      testColumnCorruption(tmp);
   } catch (const std::exception& e) {
      // Reaching here means a loader rejected one of the inputs named on the
      // command line (a missing or malformed vocab.bin or column). That is a
      // clean rejection, not a crash, so report it as such and fail.
      std::cerr << "ERROR: " << e.what() << "\n";
      std::cerr << "test_io: FAILED (could not run against the given inputs)\n";
      return 1;
   }

   std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed\n";
   if (failures) {
      std::cerr << failures << " check(s) FAILED\n";
      return 1;
   }
   std::cout << "test_io: ALL PASSED\n";
   return 0;
}
//---------------------------------------------------------------------------
