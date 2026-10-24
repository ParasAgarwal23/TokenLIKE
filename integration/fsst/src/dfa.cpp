#include "fsstlike/dfa.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace fsstlike {
namespace {

struct NfaEdge { u8 lo, hi; u32 to; };
struct NfaState {
   std::vector<NfaEdge> edges;
   std::vector<u32> eps;
};

struct Nfa {
   std::vector<NfaState> st;
   u32 start = 0, accept = 0;
   u32 add() { st.emplace_back(); return static_cast<u32>(st.size() - 1); }
   void edge(u32 from, u8 lo, u8 hi, u32 to) { st[from].edges.push_back({lo, hi, to}); }
};

/// The nine byte-range sequences that make up well-formed UTF-8. Keep in sync
/// with utf8_valid() in common.cpp and char_len() in pattern.cpp.
struct Range { u8 lo, hi; };
const std::vector<std::vector<Range>>& utf8_sequences() {
   static const std::vector<std::vector<Range>> seqs = {
       {{0x00, 0x7F}},
       {{0xC2, 0xDF}, {0x80, 0xBF}},
       {{0xE0, 0xE0}, {0xA0, 0xBF}, {0x80, 0xBF}},
       {{0xE1, 0xEC}, {0x80, 0xBF}, {0x80, 0xBF}},
       {{0xED, 0xED}, {0x80, 0x9F}, {0x80, 0xBF}},
       {{0xEE, 0xEF}, {0x80, 0xBF}, {0x80, 0xBF}},
       {{0xF0, 0xF0}, {0x90, 0xBF}, {0x80, 0xBF}, {0x80, 0xBF}},
       {{0xF1, 0xF3}, {0x80, 0xBF}, {0x80, 0xBF}, {0x80, 0xBF}},
       {{0xF4, 0xF4}, {0x80, 0x8F}, {0x80, 0xBF}, {0x80, 0xBF}},
   };
   return seqs;
}

/// Appends a one-character sub-automaton after `entry`; returns its exit state.
/// `reverse` feeds each byte sequence back to front, for the reverse automaton.
u32 add_any_char(Nfa& nfa, u32 entry, bool reverse) {
   u32 exit = nfa.add();
   for (const auto& seq : utf8_sequences()) {
      u32 cur = entry;
      for (size_t k = 0; k < seq.size(); ++k) {
         const Range& r = reverse ? seq[seq.size() - 1 - k] : seq[k];
         bool last = (k + 1 == seq.size());
         u32 to = last ? exit : nfa.add();
         nfa.edge(cur, r.lo, r.hi, to);
         cur = to;
      }
   }
   return exit;
}

Nfa build_nfa(const Pattern& p, bool reverse) {
   Nfa nfa;
   u32 cur = nfa.add();
   nfa.start = cur;
   size_t count = p.items.size();
   for (size_t idx = 0; idx < count; ++idx) {
      const PatternItem& it = p.items[reverse ? count - 1 - idx : idx];
      switch (it.kind) {
         case ItemKind::Star:
            nfa.edge(cur, 0x00, 0xFF, cur); // '%' is "any byte sequence"
            break;
         case ItemKind::AnyChar:
            cur = add_any_char(nfa, cur, reverse);
            break;
         case ItemKind::Literal: {
            size_t nb = it.bytes.size();
            for (size_t k = 0; k < nb; ++k) {
               u8 b = it.bytes[reverse ? nb - 1 - k : k];
               u32 to = nfa.add();
               nfa.edge(cur, b, b, to);
               cur = to;
            }
            break;
         }
      }
   }
   nfa.accept = cur;
   return nfa;
}

struct SetHash {
   size_t operator()(const std::vector<u32>& v) const {
      size_t h = 1469598103934665603ull;
      for (u32 x : v) { h ^= x; h *= 1099511628211ull; }
      return h;
   }
};

void eps_closure(const Nfa& nfa, std::vector<u32>& set, std::vector<u8>& seen) {
   std::vector<u32> stack = set;
   for (u32 s : set) seen[s] = 1;
   while (!stack.empty()) {
      u32 s = stack.back();
      stack.pop_back();
      for (u32 t : nfa.st[s].eps)
         if (!seen[t]) { seen[t] = 1; set.push_back(t); stack.push_back(t); }
   }
   for (u32 s : set) seen[s] = 0;
   std::sort(set.begin(), set.end());
}

/// Plain subset construction. `raw_delta` is nstates*256.
struct RawDfa {
   u32 n = 0, start = 0;
   std::vector<u32> delta;
   std::vector<u8> accept;
};

RawDfa subset_construct(const Nfa& nfa, u32 max_states) {
   RawDfa d;
   std::vector<u8> seen(nfa.st.size(), 0);
   std::unordered_map<std::vector<u32>, u32, SetHash> ids;
   std::vector<std::vector<u32>> sets;

   auto intern = [&](std::vector<u32> set) -> u32 {
      eps_closure(nfa, set, seen);
      auto it = ids.find(set);
      if (it != ids.end()) return it->second;
      u32 id = static_cast<u32>(sets.size());
      if (id >= max_states)
         fail("pattern needs more than " + std::to_string(max_states) +
              " DFA states; raise --max-states or simplify the pattern");
      ids.emplace(set, id);
      sets.push_back(std::move(set));
      return id;
   };

   d.start = intern({nfa.start});
   for (u32 s = 0; s < sets.size(); ++s) {
      d.delta.resize(static_cast<size_t>(s + 1) * 256, 0);
      // One pass over the set's edges per byte value would be O(256*|edges|);
      // instead bucket the edges once into 256 target lists.
      std::vector<std::vector<u32>> next(256);
      for (u32 q : sets[s])
         for (const NfaEdge& e : nfa.st[q].edges)
            for (u32 c = e.lo; c <= e.hi; ++c) next[c].push_back(e.to);
      for (u32 c = 0; c < 256; ++c) {
         std::vector<u32>& tgt = next[c];
         std::sort(tgt.begin(), tgt.end());
         tgt.erase(std::unique(tgt.begin(), tgt.end()), tgt.end());
         d.delta[(static_cast<size_t>(s) << 8) | c] = intern(std::move(tgt));
      }
   }
   d.n = static_cast<u32>(sets.size());
   d.delta.resize(static_cast<size_t>(d.n) * 256, 0);
   d.accept.assign(d.n, 0);
   for (u32 s = 0; s < d.n; ++s)
      d.accept[s] = std::binary_search(sets[s].begin(), sets[s].end(), nfa.accept) ? 1 : 0;
   return d;
}

/// Hopcroft's algorithm. Returns a block id per state and the block count.
u32 hopcroft(const RawDfa& d, std::vector<u32>& blockOf) {
   const u32 n = d.n;
   // Inverse transition relation as 256 CSR arrays packed into one buffer.
   std::vector<u32> invOff(static_cast<size_t>(256) * (n + 1), 0);
   std::vector<u32> invList(static_cast<size_t>(256) * n, 0);
   for (u32 c = 0; c < 256; ++c) {
      u32* off = &invOff[static_cast<size_t>(c) * (n + 1)];
      for (u32 s = 0; s < n; ++s) ++off[d.delta[(static_cast<size_t>(s) << 8) | c] + 1];
      for (u32 t = 0; t < n; ++t) off[t + 1] += off[t];
      std::vector<u32> cursor(off, off + n);
      u32* list = &invList[static_cast<size_t>(c) * n];
      for (u32 s = 0; s < n; ++s) list[cursor[d.delta[(static_cast<size_t>(s) << 8) | c]]++] = s;
   }

   std::vector<u32> elems(n), pos(n);
   blockOf.assign(n, 0);
   // Initial partition: non-accepting states, then accepting states.
   u32 k = 0;
   for (u32 s = 0; s < n; ++s) if (!d.accept[s]) elems[k++] = s;
   u32 split = k;
   for (u32 s = 0; s < n; ++s) if (d.accept[s]) elems[k++] = s;
   for (u32 i = 0; i < n; ++i) pos[elems[i]] = i;

   std::vector<u32> bstart, bend;
   if (split > 0) { bstart.push_back(0); bend.push_back(split); }
   if (split < n) { bstart.push_back(split); bend.push_back(n); }
   for (u32 b = 0; b < bstart.size(); ++b)
      for (u32 i = bstart[b]; i < bend[b]; ++i) blockOf[elems[i]] = b;

   std::vector<u8> inW(static_cast<size_t>(n + 1) * 256, 0);
   std::vector<std::pair<u32, u16>> work;
   auto push = [&](u32 b, u32 c) {
      if (inW[static_cast<size_t>(b) * 256 + c]) return;
      inW[static_cast<size_t>(b) * 256 + c] = 1;
      work.push_back({b, static_cast<u16>(c)});
   };
   if (bstart.size() == 2) {
      u32 smaller = (bend[0] - bstart[0] <= bend[1] - bstart[1]) ? 0 : 1;
      for (u32 c = 0; c < 256; ++c) push(smaller, c);
   }

   std::vector<u32> hitBlocks;
   std::vector<std::vector<u32>> hits(n + 1);
   while (!work.empty()) {
      auto [b, c] = work.back();
      work.pop_back();
      inW[static_cast<size_t>(b) * 256 + c] = 0;

      // X = { s : delta(s, c) is in block b }, grouped by the block of s.
      const u32* off = &invOff[static_cast<size_t>(c) * (n + 1)];
      const u32* list = &invList[static_cast<size_t>(c) * n];
      for (u32 i = bstart[b]; i < bend[b]; ++i) {
         u32 t = elems[i];
         for (u32 j = off[t]; j < off[t + 1]; ++j) {
            u32 s = list[j];
            u32 sb = blockOf[s];
            if (hits[sb].empty()) hitBlocks.push_back(sb);
            hits[sb].push_back(s);
         }
      }

      for (u32 sb : hitBlocks) {
         std::vector<u32>& m = hits[sb];
         u32 size = bend[sb] - bstart[sb];
         if (m.size() < size) {
            // Move the marked states to the front of sb's range and cut there.
            u32 front = bstart[sb];
            for (u32 s : m) {
               u32 ps = pos[s];
               u32 other = elems[front];
               elems[front] = s; pos[s] = front;
               elems[ps] = other; pos[other] = ps;
               ++front;
            }
            u32 nb = static_cast<u32>(bstart.size());
            u32 oldStart = bstart[sb]; // copy: push_back may reallocate bstart
            bstart.push_back(oldStart);
            bend.push_back(front);
            bstart[sb] = front;
            for (u32 i = bstart[nb]; i < bend[nb]; ++i) blockOf[elems[i]] = nb;
            u32 szNew = bend[nb] - bstart[nb], szOld = bend[sb] - bstart[sb];
            for (u32 cc = 0; cc < 256; ++cc) {
               if (inW[static_cast<size_t>(sb) * 256 + cc]) push(nb, cc);
               else push(szNew <= szOld ? nb : sb, cc);
            }
         }
         m.clear();
      }
      hitBlocks.clear();
   }
   return static_cast<u32>(bstart.size());
}

} // namespace

Dfa build_dfa(const Pattern& p, bool reverse, u32 max_states) {
   auto t0 = std::chrono::steady_clock::now();
   Nfa nfa = build_nfa(p, reverse);
   RawDfa raw = subset_construct(nfa, max_states);

   std::vector<u32> blockOf;
   u32 nb = hopcroft(raw, blockOf);

   // Collapse the minimised DFA onto block ids.
   std::vector<u32> rep(nb, UINT32_MAX);
   for (u32 s = 0; s < raw.n; ++s)
      if (rep[blockOf[s]] == UINT32_MAX) rep[blockOf[s]] = s;
   std::vector<u32> mdelta(static_cast<size_t>(nb) * 256);
   std::vector<u8> macc(nb);
   for (u32 b = 0; b < nb; ++b) {
      macc[b] = raw.accept[rep[b]];
      for (u32 c = 0; c < 256; ++c)
         mdelta[(static_cast<size_t>(b) << 8) | c] = blockOf[raw.delta[(static_cast<size_t>(rep[b]) << 8) | c]];
   }

   // Dead states: no path to an accepting state. Reverse-reach from accepts.
   std::vector<u8> live(nb, 0);
   {
      std::vector<std::vector<u32>> pred(nb);
      for (u32 b = 0; b < nb; ++b)
         for (u32 c = 0; c < 256; ++c)
            pred[mdelta[(static_cast<size_t>(b) << 8) | c]].push_back(b);
      std::vector<u32> stack;
      for (u32 b = 0; b < nb; ++b) if (macc[b]) { live[b] = 1; stack.push_back(b); }
      while (!stack.empty()) {
         u32 b = stack.back(); stack.pop_back();
         for (u32 q : pred[b]) if (!live[q]) { live[q] = 1; stack.push_back(q); }
      }
   }

   // Accept sinks: accepting, and every successor is also an accept sink.
   // Greatest fixpoint, so start optimistic and remove.
   std::vector<u8> sink(nb, 0);
   for (u32 b = 0; b < nb; ++b) sink[b] = macc[b];
   for (bool changed = true; changed;) {
      changed = false;
      for (u32 b = 0; b < nb; ++b) {
         if (!sink[b]) continue;
         for (u32 c = 0; c < 256; ++c)
            if (!sink[mdelta[(static_cast<size_t>(b) << 8) | c]]) { sink[b] = 0; changed = true; break; }
      }
   }

   // Canonical numbering: 0 = reject sink, 1 = accept sink, rest in order.
   Dfa out;
   std::vector<u32> map(nb, UINT32_MAX);
   u32 next = 2;
   for (u32 b = 0; b < nb; ++b) {
      if (!live[b]) { map[b] = Dfa::DEAD; out.dead_reachable = true; }
      else if (sink[b]) { map[b] = Dfa::ASINK; out.asink_reachable = true; }
      else map[b] = next++;
   }
   if (next > 65535)
      fail("minimised DFA has " + std::to_string(next) + " states, which does not fit the uint16 "
           "state ids this experiment uses");
   out.n = next;
   out.start = map[blockOf[raw.start]];
   out.delta.assign(static_cast<size_t>(out.n) * 256, 0);
   out.accept.assign(out.n, 0);
   out.accept[Dfa::ASINK] = 1;
   for (u32 c = 0; c < 256; ++c) {
      out.delta[(static_cast<size_t>(Dfa::DEAD) << 8) | c] = Dfa::DEAD;
      out.delta[(static_cast<size_t>(Dfa::ASINK) << 8) | c] = Dfa::ASINK;
   }
   for (u32 b = 0; b < nb; ++b) {
      u32 s = map[b];
      if (s < 2) continue;
      out.accept[s] = macc[b];
      for (u32 c = 0; c < 256; ++c)
         out.delta[(static_cast<size_t>(s) << 8) | c] =
             static_cast<u16>(map[mdelta[(static_cast<size_t>(b) << 8) | c]]);
   }
   out.states_subset = raw.n;
   out.states_minimal = nb;
   out.build_ns = static_cast<u64>(
       std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
   return out;
}

} // namespace fsstlike
