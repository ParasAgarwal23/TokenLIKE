#include "fsstlike/lift.h"

#include <chrono>

namespace fsstlike {

LiftedDense lift_dense(const Dfa& d, const FsstColumn& c, bool reverse_symbols) {
   auto t0 = std::chrono::steady_clock::now();
   LiftedDense L;
   L.n = d.n;
   L.sym.assign(static_cast<size_t>(d.n) * 256, Dfa::DEAD);
   for (u32 code = 0; code < c.nsymbols; ++code) {
      u32 len = c.sym_len(code);
      u8 bytes[8];
      const u8* src = c.sym_bytes(code);
      for (u32 k = 0; k < len; ++k) bytes[k] = reverse_symbols ? src[len - 1 - k] : src[k];
      for (u32 s = 0; s < d.n; ++s)
         L.sym[(static_cast<size_t>(s) << 8) | code] = static_cast<u16>(run_bytes(d, s, bytes, len));
   }
   L.build_ns = static_cast<u64>(
       std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
   return L;
}

LiftedCompact lift_compact(const LiftedDense& dense) {
   auto t0 = std::chrono::steady_clock::now();
   LiftedCompact L;
   L.n = dense.n;
   L.def.assign(dense.n, 0);
   L.mask.assign(static_cast<size_t>(dense.n) * 4, 0);
   L.cum.assign(static_cast<size_t>(dense.n) * 4, 0);
   L.excOff.assign(dense.n + 1, 0);
   std::vector<u32> count(dense.n, 0);
   for (u32 s = 0; s < dense.n; ++s) {
      const u16* row = &dense.sym[static_cast<size_t>(s) << 8];
      // Most frequent target becomes the default.
      u32 bestCount = 0;
      u16 best = row[0];
      for (u32 c = 0; c < 256; ++c) ++count[row[c]];
      for (u32 c = 0; c < 256; ++c)
         if (count[row[c]] > bestCount) { bestCount = count[row[c]]; best = row[c]; }
      for (u32 c = 0; c < 256; ++c) count[row[c]] = 0;
      L.def[s] = best;
      u32 exc = 0;
      for (u32 c = 0; c < 256; ++c) {
         if (row[c] == best) continue;
         L.mask[static_cast<size_t>(s) * 4 + (c >> 6)] |= 1ull << (c & 63);
         ++exc;
      }
      u32 run = 0;
      for (u32 w = 0; w < 4; ++w) {
         L.cum[static_cast<size_t>(s) * 4 + w] = static_cast<u16>(run);
         run += static_cast<u32>(__builtin_popcountll(L.mask[static_cast<size_t>(s) * 4 + w]));
      }
      L.excOff[s + 1] = L.excOff[s] + exc;
   }
   L.exc.resize(L.excOff[dense.n]);
   for (u32 s = 0; s < dense.n; ++s) {
      const u16* row = &dense.sym[static_cast<size_t>(s) << 8];
      u32 idx = L.excOff[s];
      for (u32 c = 0; c < 256; ++c)
         if (row[c] != L.def[s]) L.exc[idx++] = row[c];
   }
   L.build_ns = static_cast<u64>(
       std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
   return L;
}

const LiftedDense& LiftedSet::add(const FsstColumn& c, const std::string& id, bool reverse_symbols) {
   LiftedDense l = lift_dense(dfa, c, reverse_symbols);
   lift_ns_total += l.build_ns;
   tables.push_back(std::move(l));
   table_ids.push_back(id);
   return tables.back();
}

LiftedSet lift_for(const Pattern& p, bool reverse, u32 max_states,
                   const std::vector<const FsstColumn*>& columns,
                   const std::vector<std::string>& ids) {
   if (columns.size() != ids.size())
      fail("lift_for: " + std::to_string(columns.size()) + " columns but " + std::to_string(ids.size()) +
           " ids");
   LiftedSet set;
   set.dfa = build_dfa(p, reverse, max_states);
   set.tables.reserve(columns.size());
   for (size_t i = 0; i < columns.size(); ++i) set.add(*columns[i], ids[i], reverse);
   return set;
}

} // namespace fsstlike
