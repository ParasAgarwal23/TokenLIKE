// fsst_encode: build an FSST-compressed column from a TokenLIKE raw column.
//
//   fsst_encode --raw <src prefix> --out <dst prefix> [--rows N] [--no-rev]
//
// Reads <src prefix>.raw / .rawoff (the original project's files are only ever
// read), writes <dst prefix>.{raw,rawoff,fsst.sym,fsst.cmp,fsst.off,fsst.rev,
// fsst.meta.json} inside this experiment. The raw column is copied so the
// experiment is self-contained and the raw baseline reads the same bytes the
// compressor saw.
//
// Round-trip verification of every row is not optional: the encoder decodes
// each compressed row with the upstream fsst_decompress() and compares it with
// the source bytes before any file is written.
#include "fsstlike/fsstcol.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace fsstlike;

namespace {

u64 fnv64(const void* data, size_t n) {
   const u8* p = static_cast<const u8*>(data);
   u64 h = 14695981039346656037ull;
   for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
   return h;
}

std::string hex64(u64 v) {
   char buf[17];
   std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
   return buf;
}

struct Args {
   std::string raw, out;
   u64 rows = 0;
   bool rev = true;
};

Args parse(int argc, char** argv) {
   Args a;
   for (int i = 1; i < argc; ++i) {
      std::string f = argv[i];
      auto next = [&]() -> std::string {
         if (i + 1 >= argc) fail(f + " needs a value");
         return argv[++i];
      };
      if (f == "--raw") a.raw = next();
      else if (f == "--out") a.out = next();
      else if (f == "--rows") a.rows = std::stoull(next());
      else if (f == "--no-rev") a.rev = false;
      else fail("unknown flag " + f);
   }
   if (a.raw.empty() || a.out.empty()) fail("usage: fsst_encode --raw <src prefix> --out <dst prefix> [--rows N] [--no-rev]");
   return a;
}

} // namespace

// Paths go into JSON; a Windows path contains backslashes that must be escaped.
static std::string json_path(const std::string& s) {
   std::string out;
   for (unsigned char c : s) {
      if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
      else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
      else out += static_cast<char>(c);
   }
   return out;
}

int main(int argc, char** argv) try {
   Args args = parse(argc, argv);
   RawColumn src = load_raw(args.raw, args.rows);
   if (src.rows == 0) fail(args.raw + ": no rows");

   std::vector<const u8*> ptrs(src.rows);
   std::vector<size_t> lens(src.rows);
   size_t maxRaw = 0;
   for (u64 i = 0; i < src.rows; ++i) {
      ptrs[i] = src.row(i);
      lens[i] = src.len(i);
      if (lens[i] > maxRaw) maxRaw = lens[i];
   }

   auto t0 = std::chrono::steady_clock::now();
   fsst_encoder_t* enc = fsst_create(src.rows, lens.data(), ptrs.data(), 0);
   if (!enc) fail("fsst_create returned null");
   auto t1 = std::chrono::steady_clock::now();

   std::vector<u8> exported(FSST_MAXHEADER);
   u32 symLen = fsst_export(enc, exported.data());
   exported.resize(symLen);

   // Compress in chunks. fsst_compress may compress fewer strings than asked
   // when the output buffer fills, so advance by whatever it reports.
   std::vector<u8> cmp;
   cmp.reserve(src.bytes.size());
   std::vector<u64> off(src.rows + 1, 0);
   const u64 chunk = 1 << 16;
   std::vector<size_t> lenOut(chunk);
   std::vector<u8*> strOut(chunk);
   std::vector<u8> obuf;
   u64 done = 0;
   while (done < src.rows) {
      u64 want = std::min(chunk, src.rows - done);
      size_t vol = 0;
      for (u64 k = 0; k < want; ++k) vol += lens[done + k];
      obuf.assign(7 + 2 * vol + 16, 0);
      size_t got = fsst_compress(enc, want, lens.data() + done, ptrs.data() + done, obuf.size(), obuf.data(),
                                 lenOut.data(), strOut.data());
      if (got == 0) fail("fsst_compress made no progress at row " + std::to_string(done));
      for (size_t k = 0; k < got; ++k) {
         cmp.insert(cmp.end(), strOut[k], strOut[k] + lenOut[k]);
         off[done + k + 1] = cmp.size();
      }
      done += got;
   }
   auto t2 = std::chrono::steady_clock::now();

   // Independent decode path: import the exported table from scratch rather
   // than reusing the encoder's own decoder, so a bad export is caught here.
   FsstColumn col;
   import_table(col, exported);
   col.rows = src.rows;
   col.off = off;
   col.cmp = cmp;
   for (u64 i = 0; i < src.rows; ++i)
      col.max_cmp_row = std::max(col.max_cmp_row, static_cast<size_t>(off[i + 1] - off[i]));
   validate_fsst(col, false);

   std::vector<u8> buf(col.decode_bound());
   u64 mismatches = 0;
   for (u64 i = 0; i < src.rows; ++i) {
      size_t len = static_cast<size_t>(off[i + 1] - off[i]);
      size_t n = fsst_decompress(&col.dec, len, cmp.data() + off[i], buf.size(), buf.data());
      if (n != lens[i] || (n && std::memcmp(buf.data(), ptrs[i], n) != 0)) {
         if (mismatches < 5)
            std::fprintf(stderr, "roundtrip mismatch at row %llu: %zu bytes in, %zu bytes out\n",
                         static_cast<unsigned long long>(i), lens[i], n);
         ++mismatches;
      }
   }
   if (mismatches) fail(std::to_string(mismatches) + " rows failed the FSST round-trip; nothing was written");

   // Unit census and the reverse-scan boundary.
   std::vector<u32> rev(src.rows, 0);
   u64 symUnits = 0, escUnits = 0, rowsWithEsc = 0;
   for (u64 i = 0; i < src.rows; ++i) {
      const u8* p = cmp.data() + off[i];
      size_t n = static_cast<size_t>(off[i + 1] - off[i]);
      size_t j = 0, last = 0;
      while (j < n) {
         if (p[j] == FSST_ESC) { ++escUnits; j += 2; last = j; }
         else { ++symUnits; ++j; }
      }
      rev[i] = static_cast<u32>(last);
      if (last) ++rowsWithEsc;
   }

   write_file(args.out + ".fsst.sym", exported.data(), exported.size());
   write_file(args.out + ".fsst.cmp", cmp.data(), cmp.size());
   write_file(args.out + ".fsst.off", off.data(), off.size() * sizeof(u64));
   if (args.rev) write_file(args.out + ".fsst.rev", rev.data(), rev.size() * sizeof(u32));
   // Copy the raw column alongside, so every mode in this experiment reads from
   // the experiment directory only.
   write_file(args.out + ".raw", src.bytes.data(), src.bytes.size());
   {
      std::vector<u64> rawoff(src.off.begin(), src.off.begin() + static_cast<long>(src.rows) + 1);
      write_file(args.out + ".rawoff", rawoff.data(), rawoff.size() * sizeof(u64));
   }

   double createMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
   double compressMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
   char meta[4096];
   std::snprintf(meta, sizeof(meta),
                 "{\n"
                 "  \"column\": \"%s\",\n"
                 "  \"source_prefix\": \"%s\",\n"
                 "  \"rows\": %llu,\n"
                 "  \"raw_bytes\": %llu,\n"
                 "  \"cmp_bytes\": %llu,\n"
                 "  \"sym_bytes\": %llu,\n"
                 "  \"rev_bytes\": %llu,\n"
                 "  \"ratio\": %.6f,\n"
                 "  \"nsymbols\": %u,\n"
                 "  \"symbol_units\": %llu,\n"
                 "  \"escape_units\": %llu,\n"
                 "  \"rows_with_escape\": %llu,\n"
                 "  \"max_cmp_row\": %llu,\n"
                 "  \"max_raw_row\": %llu,\n"
                 "  \"roundtrip_mismatches\": 0,\n"
                 "  \"fsst_create_ms\": %.3f,\n"
                 "  \"fsst_compress_ms\": %.3f,\n"
                 "  \"fnv64_cmp\": \"%s\",\n"
                 "  \"fnv64_off\": \"%s\",\n"
                 "  \"fnv64_sym\": \"%s\"\n"
                 "}\n",
                 json_path(args.out).c_str(), json_path(args.raw).c_str(), static_cast<unsigned long long>(src.rows),
                 static_cast<unsigned long long>(src.bytes.size()), static_cast<unsigned long long>(cmp.size()),
                 static_cast<unsigned long long>(exported.size()),
                 static_cast<unsigned long long>(args.rev ? rev.size() * sizeof(u32) : 0),
                 cmp.empty() ? 0.0 : double(src.bytes.size()) / double(cmp.size()), col.nsymbols,
                 static_cast<unsigned long long>(symUnits), static_cast<unsigned long long>(escUnits),
                 static_cast<unsigned long long>(rowsWithEsc), static_cast<unsigned long long>(col.max_cmp_row),
                 static_cast<unsigned long long>(maxRaw), createMs, compressMs,
                 hex64(fnv64(cmp.data(), cmp.size())).c_str(),
                 hex64(fnv64(off.data(), off.size() * sizeof(u64))).c_str(),
                 hex64(fnv64(exported.data(), exported.size())).c_str());
   write_file(args.out + ".fsst.meta.json", meta, std::strlen(meta));
   std::fputs(meta, stdout);

   fsst_destroy(enc);
   return 0;
} catch (const std::exception& e) {
   std::fprintf(stderr, "fsst_encode: %s\n", e.what());
   return 1;
}
