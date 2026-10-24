// fsst_liftbench: does separating DFA construction from lifting actually pay?
//
//   fsst_liftbench --pattern '%.php' --col data/a --col data/b ... [--repeat 9] [--json]
//
// Reports, for one pattern over N symbol tables:
//   * the cost of building the byte DFA (pattern-only, done once when reused)
//   * the cost of lifting onto each table (pattern x table)
//   * what rebuilding from scratch for every table would cost
//   * peak table memory
//
// It also *verifies* the reuse: for every column it scans once with a table
// lifted from the shared automaton and once with a table lifted from a freshly
// built automaton, and compares the match bitmaps. A speedup that changed an
// answer would be worthless, so the check runs before the numbers are printed.
#include "fsstlike/scan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace fsstlike;

namespace {

u64 now_ns() {
   return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
}

u64 median(std::vector<u64> v) {
   std::sort(v.begin(), v.end());
   return v.empty() ? 0 : v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) try {
   std::string pattern;
   std::vector<std::string> cols;
   u32 repeat = 9, maxStates = 20000;
   bool json = false, reverse = false, havePattern = false;
   for (int i = 1; i < argc; ++i) {
      std::string f = argv[i];
      auto next = [&]() -> std::string {
         if (i + 1 >= argc) fail(f + " needs a value");
         return argv[++i];
      };
      if (f == "--pattern") { pattern = next(); havePattern = true; }
      else if (f == "--col") cols.push_back(next());
      else if (f == "--repeat") repeat = static_cast<u32>(std::stoul(next()));
      else if (f == "--max-states") maxStates = static_cast<u32>(std::stoul(next()));
      else if (f == "--reverse") reverse = true;
      else if (f == "--json") json = true;
      else fail("unknown flag " + f);
   }
   if (!havePattern || cols.empty())
      fail("usage: fsst_liftbench --pattern P --col PREFIX [--col PREFIX ...] [--repeat N] [--json]");

   Pattern pat = parse_like(pattern);
   std::vector<FsstColumn> loaded(cols.size());
   std::vector<const FsstColumn*> ptrs(cols.size());
   for (size_t i = 0; i < cols.size(); ++i) {
      loaded[i] = load_fsst(cols[i], false);
      ptrs[i] = &loaded[i];
   }

   // --- correctness of reuse, before any timing ---------------------------
   LiftedSet shared = lift_for(pat, reverse, maxStates, ptrs, cols);
   u64 mismatches = 0;
   for (size_t i = 0; i < cols.size(); ++i) {
      Dfa fresh = build_dfa(pat, reverse, maxStates);
      LiftedDense freshTable = lift_dense(fresh, loaded[i], reverse);
      Bitmap a, b;
      ScanStats sa, sb;
      if (reverse) {
         Dfa fwdShared = build_dfa(pat, false, maxStates);
         LiftedDense fwdTable = lift_dense(fwdShared, loaded[i], false);
         scan_fsst_rev2(loaded[i], shared.dfa, shared.tables[i], fwdShared, fwdTable, a, sa);
         scan_fsst_rev2(loaded[i], fresh, freshTable, fwdShared, fwdTable, b, sb);
      } else {
         scan_fsst(loaded[i], shared.dfa, shared.tables[i], a, sa);
         scan_fsst(loaded[i], fresh, freshTable, b, sb);
      }
      if (a != b) {
         ++mismatches;
         std::fprintf(stderr, "REUSE MISMATCH on %s: shared=%llu matches, fresh=%llu matches\n",
                      cols[i].c_str(), (unsigned long long)sa.matches, (unsigned long long)sb.matches);
      }
      if (shared.dfa.n != fresh.n)
         std::fprintf(stderr, "WARNING: shared automaton has %u states, fresh has %u\n", shared.dfa.n, fresh.n);
   }
   if (mismatches) fail(std::to_string(mismatches) + " column(s) disagreed between a reused and a fresh automaton");

   // --- timing -------------------------------------------------------------
   std::vector<u64> dfaNs;
   for (u32 r = 0; r < repeat; ++r) {
      u64 t0 = now_ns();
      Dfa d = build_dfa(pat, reverse, maxStates);
      dfaNs.push_back(now_ns() - t0);
      if (d.n == 0) fail("impossible");
   }
   u64 dfaMed = median(dfaNs);

   std::vector<u64> liftMed(cols.size());
   u64 liftSum = 0, tableBytes = 0;
   for (size_t i = 0; i < cols.size(); ++i) {
      std::vector<u64> v;
      for (u32 r = 0; r < repeat; ++r) {
         u64 t0 = now_ns();
         LiftedDense l = lift_dense(shared.dfa, loaded[i], reverse);
         v.push_back(now_ns() - t0);
         tableBytes = std::max<u64>(tableBytes, l.table_bytes());
      }
      liftMed[i] = median(v);
      liftSum += liftMed[i];
   }

   u64 reuseTotal = dfaMed + liftSum;
   u64 naiveTotal = static_cast<u64>(cols.size()) * dfaMed + liftSum;

   if (json) {
      std::printf("{\"pattern\":\"%s\",\"reverse\":%s,\"tables\":%zu,\"repeat\":%u,"
                  "\"dfa_states\":%u,\"dfa_build_ns_median\":%llu,\"lift_ns_sum\":%llu,"
                  "\"reuse_total_ns\":%llu,\"rebuild_total_ns\":%llu,\"saving\":%.4f,"
                  "\"dense_table_bytes_max\":%llu,\"reuse_mismatches\":0,\"per_table\":[",
                  pattern.c_str(), reverse ? "true" : "false", cols.size(), repeat, shared.dfa.n,
                  (unsigned long long)dfaMed, (unsigned long long)liftSum, (unsigned long long)reuseTotal,
                  (unsigned long long)naiveTotal, double(naiveTotal) / double(reuseTotal ? reuseTotal : 1),
                  (unsigned long long)tableBytes);
      for (size_t i = 0; i < cols.size(); ++i)
         std::printf("%s{\"col\":\"%s\",\"nsymbols\":%u,\"lift_ns\":%llu}", i ? "," : "", cols[i].c_str(),
                     loaded[i].nsymbols, (unsigned long long)liftMed[i]);
      std::printf("]}\n");
   } else {
      std::printf("pattern %s   %u DFA states   %zu symbol tables   median of %u runs\n", pattern.c_str(),
                  shared.dfa.n, cols.size(), repeat);
      std::printf("  build the byte DFA once        %9.1f us\n", dfaMed / 1000.0);
      for (size_t i = 0; i < cols.size(); ++i)
         std::printf("  lift onto %-28s %9.1f us  (%u symbols)\n", cols[i].c_str(), liftMed[i] / 1000.0,
                     loaded[i].nsymbols);
      std::printf("  reuse one automaton            %9.1f us\n", reuseTotal / 1000.0);
      std::printf("  rebuild for every table        %9.1f us   (%.2fx more)\n", naiveTotal / 1000.0,
                  double(naiveTotal) / double(reuseTotal ? reuseTotal : 1));
      std::printf("  reuse verified: %zu of %zu tables gave bitmaps identical to a fresh build\n",
                  cols.size(), cols.size());
   }
   return 0;
} catch (const std::exception& e) {
   std::fprintf(stderr, "fsst_liftbench: %s\n", e.what());
   return 1;
}
