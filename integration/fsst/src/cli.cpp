// fsst_like: evaluate a SQL LIKE predicate over an FSST-compressed column.
//
//   fsst_like --mode fsst --col data/clickbench/URL_1m --pattern 'http://%' --json
//
// Modes
//   raw          byte DFA over the uncompressed column
//   raw-opt      optimised raw matcher: anchored compares, memmem, segment walk
//   decode       fsst_decompress() every row, then the byte DFA
//   fsst         lifted DFA over compressed codes (dense table)
//   fsst-compact lifted DFA, default+exception table
//   fsst-rev     lifted reverse DFA, right-to-left, using the .fsst.rev sidecar
//                (--rev-mode offset uses the 4-byte-per-row sidecar;
//                 --rev-mode bit emulates a 1-bit-per-row has-escape flag)
//   fsst-rev2    the same, with NO sidecar: each unit is resolved from the
//                parity of the 0xFF run before it (proof in fsstcol.h)
//   ref          backtracking reference matcher over raw bytes (oracle, slow)
//
// Exit codes: 0 success, 1 error, 2 this build does not support the request.
#include "fsstlike/scan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace fsstlike;

namespace {

constexpr int EXIT_UNSUPPORTED = 2;

struct Unsupported : std::runtime_error {
   explicit Unsupported(const std::string& m) : std::runtime_error(m) {}
};

struct Args {
   std::string mode = "fsst", col, col2, pattern, rowids, revMode = "offset";
   u32 runs = 1, warmup = 0, maxStates = 20000;
   bool json = false, validate = false, havePattern = false;
   int affinity = -1;
};

Args parse(int argc, char** argv) {
   Args a;
   for (int i = 1; i < argc; ++i) {
      std::string f = argv[i];
      auto next = [&]() -> std::string {
         if (i + 1 >= argc) fail(f + " needs a value");
         return argv[++i];
      };
      if (f == "--mode") a.mode = next();
      else if (f == "--col") a.col = next();
      else if (f == "--col2") a.col2 = next();
      else if (f == "--pattern") { a.pattern = next(); a.havePattern = true; }
      else if (f == "--rowids") a.rowids = next();
      else if (f == "--runs") a.runs = static_cast<u32>(std::stoul(next()));
      else if (f == "--warmup") a.warmup = static_cast<u32>(std::stoul(next()));
      else if (f == "--max-states") a.maxStates = static_cast<u32>(std::stoul(next()));
      else if (f == "--affinity") a.affinity = std::stoi(next());
      else if (f == "--rev-mode") a.revMode = next();
      else if (f == "--json") a.json = true;
      else if (f == "--validate") a.validate = true;
      else fail("unknown flag " + f + " (see the header of src/cli.cpp)");
   }
   if (a.col.empty() || !a.havePattern)
      fail("usage: fsst_like --mode M --col PREFIX [--col2 PREFIX] --pattern P [--runs N] [--rowids F] [--json]");
   if (a.runs == 0) fail("--runs must be at least 1");
   return a;
}

std::string json_escape(const std::string& s) {
   std::string out;
   for (unsigned char c : s) {
      if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
      else if (c == '\n') out += "\\n";
      else if (c == '\t') out += "\\t";
      else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
      else out += static_cast<char>(c);
   }
   return out;
}

void write_rowids(const std::string& path, const std::vector<u32>& ids) {
   write_file(path, ids.data(), ids.size() * sizeof(u32));
}

} // namespace

int main(int argc, char** argv) try {
   Args args = parse(argc, argv);
   bool concat = !args.col2.empty();
   bool needRaw = (args.mode == "raw" || args.mode == "ref" || args.mode == "raw-opt");
   bool needFsst = !needRaw;
   bool reverse = (args.mode == "fsst-rev" || args.mode == "fsst-rev2");
   bool sidecar = (args.mode == "fsst-rev");

   if (args.mode != "raw" && args.mode != "raw-opt" && args.mode != "ref" && args.mode != "decode" &&
       args.mode != "fsst" && args.mode != "fsst-compact" && args.mode != "fsst-rev" &&
       args.mode != "fsst-rev2")
      throw Unsupported("unknown mode '" + args.mode + "'");
   if (concat && (args.mode == "fsst-compact" || args.mode == "fsst-rev" || args.mode == "fsst-rev2"))
      throw Unsupported("--col2 is not implemented for mode " + args.mode +
                        "; concatenation is supported in modes raw, raw-opt, ref, decode and fsst");
   if (args.revMode == "bit" && args.mode != "fsst-rev")
      throw Unsupported("--rev-mode bit only applies to mode fsst-rev; fsst-rev2 stores no sidecar");

   Pattern pat = parse_like(args.pattern);

   RawColumn rawA, rawB;
   FsstColumn fsA, fsB;
   if (needRaw) {
      rawA = load_raw(args.col);
      if (concat) rawB = load_raw(args.col2);
   }
   if (needFsst) {
      fsA = load_fsst(args.col, sidecar);
      if (concat) fsB = load_fsst(args.col2, sidecar);
      if (args.revMode == "bit") {
         // Emulate the cheaper sidecar: one bit per row saying "this row has an
         // escape somewhere", instead of four bytes saying where. Setting the
         // safe boundary to the end of the row makes the reverse scan give up
         // immediately and take the forward path, which is exactly what a
         // 1-bit scheme must do. Lets the two designs be measured, not argued.
         for (u64 i = 0; i < fsA.rows; ++i)
            fsA.rev[i] = fsA.rev[i] ? static_cast<u32>(fsA.off[i + 1] - fsA.off[i]) : 0;
      } else if (args.revMode != "offset") {
         throw Unsupported("--rev-mode must be 'offset' (the .fsst.rev sidecar) or 'bit'");
      }
      if (args.validate) {
         validate_fsst(fsA, reverse);
         if (concat) validate_fsst(fsB, reverse);
      }
   }
   u64 rows = needRaw ? rawA.rows : fsA.rows;

   Dfa fwd, rev;
   LiftedDense denseA, denseB, denseRevA, denseFwdA;
   LiftedCompact compactA;
   RawPlan plan;
   u64 liftNs = 0;
   size_t tableBytes = 0;
   if (args.mode == "raw-opt") {
      plan = compile_raw(pat);
   } else if (args.mode != "ref") {
      fwd = build_dfa(pat, false, args.maxStates);
      tableBytes = fwd.table_bytes();
      if (args.mode == "fsst" || args.mode == "fsst-compact" || reverse) {
         denseA = lift_dense(fwd, fsA, false);
         liftNs += denseA.build_ns;
         if (concat) { denseB = lift_dense(fwd, fsB, false); liftNs += denseB.build_ns; }
         if (args.mode == "fsst-compact") {
            compactA = lift_compact(denseA);
            liftNs += compactA.build_ns;
            tableBytes += compactA.table_bytes();
         } else {
            tableBytes += denseA.table_bytes() + (concat ? denseB.table_bytes() : 0);
         }
         if (reverse) {
            rev = build_dfa(pat, true, args.maxStates);
            denseRevA = lift_dense(rev, fsA, true);
            denseFwdA = denseA;
            liftNs += rev.build_ns + denseRevA.build_ns;
            tableBytes += rev.table_bytes() + denseRevA.table_bytes();
         }
      }
   }

   Bitmap bits;
   ScanStats st;
   auto once = [&]() {
      st = ScanStats{};
      if (args.mode == "ref") {
         bits.assign(bitmap_bytes(rows), 0);
         std::vector<u8> tmp;
         u64 matches = 0, units = 0;
         for (u64 i = 0; i < rows; ++i) {
            const u8* s = rawA.row(i);
            size_t n = rawA.len(i);
            if (concat) {
               tmp.assign(s, s + n);
               tmp.insert(tmp.end(), rawB.row(i), rawB.row(i) + rawB.len(i));
               s = tmp.data();
               n = tmp.size();
            }
            units += n;
            if (like_reference(s, n, pat)) { bitmap_set(bits, i); ++matches; }
         }
         st.rows = rows;
         st.matches = matches;
         st.units = units;
         st.bytes_read = units;
      } else if (args.mode == "raw") {
         if (concat) scan_raw_concat(rawA, rawB, fwd, bits, st);
         else scan_raw(rawA, fwd, bits, st);
      } else if (args.mode == "raw-opt") {
         if (concat) scan_raw_opt_concat(rawA, rawB, plan, bits, st);
         else scan_raw_opt(rawA, plan, bits, st);
      } else if (args.mode == "decode") {
         if (concat) scan_decode_concat(fsA, fsB, fwd, bits, st);
         else scan_decode(fsA, fwd, bits, st);
      } else if (args.mode == "fsst") {
         if (concat) scan_fsst_concat(fsA, denseA, fsB, denseB, fwd, bits, st);
         else scan_fsst(fsA, fwd, denseA, bits, st);
      } else if (args.mode == "fsst-compact") {
         scan_fsst_compact(fsA, fwd, compactA, bits, st);
      } else if (args.mode == "fsst-rev") {
         scan_fsst_rev(fsA, rev, denseRevA, fwd, denseFwdA, bits, st);
      } else {
         scan_fsst_rev2(fsA, rev, denseRevA, fwd, denseFwdA, bits, st);
      }
   };

   for (u32 r = 0; r < args.warmup; ++r) once();
   u64 bestNs = UINT64_MAX, sumNs = 0;
   for (u32 r = 0; r < args.runs; ++r) {
      auto t0 = std::chrono::steady_clock::now();
      once();
      u64 ns = static_cast<u64>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
      bestNs = std::min(bestNs, ns);
      sumNs += ns;
   }

   std::vector<u32> ids = bitmap_to_rowids(bits, rows);
   if (!args.rowids.empty()) write_rowids(args.rowids, ids);

   u64 rawBytes = needRaw ? rawA.bytes.size() : fsA.raw_bytes;
   u64 cmpBytes = needFsst ? fsA.cmp_bytes() : 0;
   if (concat) {
      rawBytes += needRaw ? rawB.bytes.size() : fsB.raw_bytes;
      if (needFsst) cmpBytes += fsB.cmp_bytes();
   }
   double nsPerRow = rows ? double(bestNs) / double(rows) : 0.0;

   if (args.json) {
      std::printf(
          "{\"mode\":\"%s\",\"col\":\"%s\",\"col2\":\"%s\",\"pattern\":\"%s\",\"rows\":%llu,"
          "\"matches\":%llu,\"best_ns\":%llu,\"mean_ns\":%llu,\"ns_per_row\":%.4f,\"runs\":%u,"
          "\"units\":%llu,\"units_per_row\":%.4f,\"bytes_read\":%llu,\"bytes_per_row\":%.4f,"
          "\"early_rows\":%llu,\"fallback_rows\":%llu,\"malformed\":%llu,\"decoded_bytes\":%llu,"
          "\"raw_bytes\":%llu,\"cmp_bytes\":%llu,\"ratio\":%.4f,\"nsymbols\":%u,"
          "\"dfa_states\":%u,\"dfa_states_subset\":%u,\"dfa_states_minimal\":%u,\"dfa_build_ns\":%llu,\"lift_build_ns\":%llu,"
          "\"table_bytes\":%llu,\"dead_reachable\":%s,\"asink_reachable\":%s,\"has_underscore\":%s,"
          "\"affinity\":\"%s\",\"rev_mode\":\"%s\",\"raw_shape\":\"%s\",\"rev_dfa_states\":%u,\"sidecar_bytes\":%llu,\"fsst_version\":\"e638d4c\"}\n",
          args.mode.c_str(), json_escape(args.col).c_str(), json_escape(args.col2).c_str(),
          json_escape(args.pattern).c_str(), (unsigned long long)rows, (unsigned long long)st.matches,
          (unsigned long long)bestNs, (unsigned long long)(sumNs / args.runs), nsPerRow, args.runs,
          (unsigned long long)st.units, rows ? double(st.units) / double(rows) : 0.0,
          (unsigned long long)st.bytes_read, rows ? double(st.bytes_read) / double(rows) : 0.0,
          (unsigned long long)st.early_rows, (unsigned long long)st.fallback_rows,
          (unsigned long long)st.malformed, (unsigned long long)st.decoded_bytes, (unsigned long long)rawBytes,
          (unsigned long long)cmpBytes, cmpBytes ? double(rawBytes) / double(cmpBytes) : 0.0,
          needFsst ? fsA.nsymbols : 0, fwd.n, fwd.states_subset, fwd.states_minimal,
          (unsigned long long)fwd.build_ns,
          (unsigned long long)liftNs, (unsigned long long)tableBytes, fwd.dead_reachable ? "true" : "false",
          fwd.asink_reachable ? "true" : "false", pat.has_any_char() ? "true" : "false",
          args.affinity >= 0 ? "requested-but-unsupported-on-darwin" : "not-requested",
          args.revMode.c_str(), args.mode == "raw-opt" ? plan.shape_name() : "n/a", rev.n,
          (unsigned long long)(sidecar ? fsA.rev.size() * sizeof(u32) : 0));
   } else {
      std::printf("mode=%s rows=%llu matches=%llu %.3f ns/row  units/row=%.2f  states=%u canonical / %u minimal / %u subset\n",
                  args.mode.c_str(), (unsigned long long)rows, (unsigned long long)st.matches, nsPerRow,
                  rows ? double(st.units) / double(rows) : 0.0, fwd.n, fwd.states_minimal,
                  fwd.states_subset);
   }
   return 0;
} catch (const Unsupported& e) {
   std::fprintf(stderr, "fsst_like: unsupported: %s\n", e.what());
   return EXIT_UNSUPPORTED;
} catch (const std::exception& e) {
   std::fprintf(stderr, "fsst_like: %s\n", e.what());
   return 1;
}
