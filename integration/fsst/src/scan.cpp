#include "fsstlike/scan.h"

#include <cstring>

namespace fsstlike {
namespace {

/// Feeds bytes through the byte DFA, stopping at either sink. Returns the state
/// and reports how many bytes were consumed.
inline u32 run_early(const u16* delta, u32 s, const u8* p, size_t n, size_t& used) {
   size_t i = 0;
   for (; i < n; ++i) {
      s = delta[(s << 8) | p[i]];
      if (s < 2) { ++i; break; }
   }
   used = i;
   return s;
}

} // namespace

void scan_raw(const RawColumn& c, const Dfa& d, Bitmap& out, ScanStats& st) {
   out.assign(bitmap_bytes(c.rows), 0);
   const u16* delta = d.delta.data();
   const u8* acc = d.accept.data();
   u64 matches = 0, units = 0, early = 0;
   for (u64 i = 0; i < c.rows; ++i) {
      size_t n = c.len(i), used = 0;
      u32 s = run_early(delta, d.start, c.row(i), n, used);
      units += used;
      if (used < n) ++early;
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = c.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = units;
   st.early_rows = early;
}

void scan_raw_concat(const RawColumn& a, const RawColumn& b, const Dfa& d, Bitmap& out, ScanStats& st) {
   if (a.rows != b.rows)
      fail("concatenation needs row-aligned columns: " + std::to_string(a.rows) + " vs " + std::to_string(b.rows));
   out.assign(bitmap_bytes(a.rows), 0);
   const u16* delta = d.delta.data();
   const u8* acc = d.accept.data();
   u64 matches = 0, units = 0, early = 0;
   for (u64 i = 0; i < a.rows; ++i) {
      size_t total = a.len(i) + b.len(i), used = 0, u2 = 0;
      u32 s = run_early(delta, d.start, a.row(i), a.len(i), used);
      if (s >= 2) { s = run_early(delta, s, b.row(i), b.len(i), u2); used += u2; }
      units += used;
      if (used < total) ++early;
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = a.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = units;
   st.early_rows = early;
}

void scan_raw_opt(const RawColumn& c, const RawPlan& plan, Bitmap& out, ScanStats& st) {
   out.assign(bitmap_bytes(c.rows), 0);
   u64 matches = 0, bytes = 0;
   // Dispatch on the shape ONCE, outside the row loop. Leaving the switch and
   // the call inside the loop cost about 2 ns/row on anchored patterns, which
   // would have quietly handicapped the very baseline this mode exists to make
   // fair.
   const u8* lit = plan.lit.data();
   const size_t ln = plan.lit.size();
   switch (plan.shape) {
      case RawPlan::Shape::Prefix:
         for (u64 i = 0; i < c.rows; ++i) {
            size_t n = c.len(i);
            bytes += n;
            if (n >= ln && std::memcmp(c.row(i), lit, ln) == 0) { bitmap_set(out, i); ++matches; }
         }
         break;
      case RawPlan::Shape::Suffix:
         for (u64 i = 0; i < c.rows; ++i) {
            size_t n = c.len(i);
            bytes += n;
            if (n >= ln && std::memcmp(c.row(i) + n - ln, lit, ln) == 0) { bitmap_set(out, i); ++matches; }
         }
         break;
      case RawPlan::Shape::Exact:
         for (u64 i = 0; i < c.rows; ++i) {
            size_t n = c.len(i);
            bytes += n;
            if (n == ln && std::memcmp(c.row(i), lit, ln) == 0) { bitmap_set(out, i); ++matches; }
         }
         break;
      default:
         for (u64 i = 0; i < c.rows; ++i) {
            size_t n = c.len(i);
            bytes += n;
            if (raw_match(plan, c.row(i), n)) { bitmap_set(out, i); ++matches; }
         }
         break;
   }
   st.rows = c.rows;
   st.matches = matches;
   // The optimised matcher does not walk the string unit by unit, so there is
   // no honest per-row "units read" figure for it. bytes_read is the size of
   // the representation it was given, not a count of bytes inspected.
   st.units = 0;
   st.bytes_read = bytes;
}

void scan_raw_opt_concat(const RawColumn& a, const RawColumn& b, const RawPlan& plan, Bitmap& out,
                         ScanStats& st) {
   if (a.rows != b.rows)
      fail("concatenation needs row-aligned columns: " + std::to_string(a.rows) + " vs " + std::to_string(b.rows));
   out.assign(bitmap_bytes(a.rows), 0);
   std::vector<u8> buf;
   u64 matches = 0, bytes = 0;
   for (u64 i = 0; i < a.rows; ++i) {
      // The optimised matcher needs one contiguous string, so concatenation
      // costs a copy here. A real engine would pay the same price, or teach the
      // matcher about two-part inputs.
      buf.clear();
      buf.insert(buf.end(), a.row(i), a.row(i) + a.len(i));
      buf.insert(buf.end(), b.row(i), b.row(i) + b.len(i));
      bytes += buf.size();
      if (raw_match(plan, buf.data(), buf.size())) { bitmap_set(out, i); ++matches; }
   }
   st.rows = a.rows;
   st.matches = matches;
   st.units = 0;
   st.bytes_read = bytes;
}

void scan_decode(const FsstColumn& c, const Dfa& d, Bitmap& out, ScanStats& st) {
   out.assign(bitmap_bytes(c.rows), 0);
   std::vector<u8> buf(c.decode_bound());
   const u16* delta = d.delta.data();
   const u8* acc = d.accept.data();
   const u8* base = c.cmp.data();
   u64 matches = 0, units = 0, early = 0, decoded = 0;
   for (u64 i = 0; i < c.rows; ++i) {
      size_t len = static_cast<size_t>(c.off[i + 1] - c.off[i]);
      size_t n = fsst_decompress(&c.dec, len, base + c.off[i], buf.size(), buf.data());
      if (n > buf.size())
         fail("row " + std::to_string(i) + " decompresses to " + std::to_string(n) +
              " bytes, past the " + std::to_string(buf.size()) + "-byte bound");
      decoded += n;
      size_t used = 0;
      u32 s = run_early(delta, d.start, buf.data(), n, used);
      units += used;
      if (used < n) ++early;
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = c.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = c.cmp.size();
   st.early_rows = early;
   st.decoded_bytes = decoded;
}

void scan_decode_concat(const FsstColumn& a, const FsstColumn& b, const Dfa& d, Bitmap& out, ScanStats& st) {
   if (a.rows != b.rows)
      fail("concatenation needs row-aligned columns: " + std::to_string(a.rows) + " vs " + std::to_string(b.rows));
   out.assign(bitmap_bytes(a.rows), 0);
   std::vector<u8> bufA(a.decode_bound()), bufB(b.decode_bound());
   const u16* delta = d.delta.data();
   const u8* acc = d.accept.data();
   u64 matches = 0, units = 0, decoded = 0;
   for (u64 i = 0; i < a.rows; ++i) {
      size_t la = static_cast<size_t>(a.off[i + 1] - a.off[i]);
      size_t lb = static_cast<size_t>(b.off[i + 1] - b.off[i]);
      size_t na = fsst_decompress(&a.dec, la, a.cmp.data() + a.off[i], bufA.size(), bufA.data());
      size_t nb = fsst_decompress(&b.dec, lb, b.cmp.data() + b.off[i], bufB.size(), bufB.data());
      if (na > bufA.size() || nb > bufB.size()) fail("row " + std::to_string(i) + ": decode bound exceeded");
      decoded += na + nb;
      size_t used = 0, u2 = 0;
      u32 s = run_early(delta, d.start, bufA.data(), na, used);
      if (s >= 2) { s = run_early(delta, s, bufB.data(), nb, u2); used += u2; }
      units += used;
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = a.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = a.cmp.size() + b.cmp.size();
   st.decoded_bytes = decoded;
}

/// The core compressed-domain loop. One table lookup per code; escapes take the
/// byte transition for the literal they carry.
static inline u32 fsst_row(const u16* T, const u16* B, u32 s, const u8*& p, const u8* e, u64& units,
                           u64& malformed) {
   while (p < e) {
      u32 code = *p++;
      if (code != FSST_ESC) {
         s = T[(s << 8) | code];
      } else {
         if (p == e) { ++malformed; return Dfa::DEAD; }
         s = B[(s << 8) | *p++];
      }
      ++units;
      if (s < 2) break;
   }
   return s;
}

void scan_fsst(const FsstColumn& c, const Dfa& d, const LiftedDense& L, Bitmap& out, ScanStats& st) {
   if (L.n != d.n) fail("lifted table was built for a different automaton");
   out.assign(bitmap_bytes(c.rows), 0);
   const u16* T = L.sym.data();
   const u16* B = d.delta.data();
   const u8* acc = d.accept.data();
   const u8* base = c.cmp.data();
   u64 matches = 0, units = 0, bytes = 0, early = 0, malformed = 0;
   for (u64 i = 0; i < c.rows; ++i) {
      const u8* p = base + c.off[i];
      const u8* e = base + c.off[i + 1];
      const u8* p0 = p;
      u32 s = fsst_row(T, B, d.start, p, e, units, malformed);
      bytes += static_cast<u64>(p - p0);
      if (p < e) ++early;
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = c.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = bytes;
   st.early_rows = early;
   st.malformed = malformed;
}

void scan_fsst_concat(const FsstColumn& a, const LiftedDense& La, const FsstColumn& b, const LiftedDense& Lb,
                      const Dfa& d, Bitmap& out, ScanStats& st) {
   if (a.rows != b.rows)
      fail("concatenation needs row-aligned columns: " + std::to_string(a.rows) + " vs " + std::to_string(b.rows));
   if (La.n != d.n || Lb.n != d.n) fail("lifted table was built for a different automaton");
   out.assign(bitmap_bytes(a.rows), 0);
   const u16* B = d.delta.data();
   const u8* acc = d.accept.data();
   u64 matches = 0, units = 0, bytes = 0, malformed = 0;
   for (u64 i = 0; i < a.rows; ++i) {
      const u8* pa = a.cmp.data() + a.off[i];
      const u8* ea = a.cmp.data() + a.off[i + 1];
      const u8* pa0 = pa;
      u32 s = fsst_row(La.sym.data(), B, d.start, pa, ea, units, malformed);
      bytes += static_cast<u64>(pa - pa0);
      if (s >= 2) {
         const u8* pb = b.cmp.data() + b.off[i];
         const u8* eb = b.cmp.data() + b.off[i + 1];
         const u8* pb0 = pb;
         s = fsst_row(Lb.sym.data(), B, s, pb, eb, units, malformed);
         bytes += static_cast<u64>(pb - pb0);
      }
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = a.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = bytes;
   st.malformed = malformed;
}

void scan_fsst_compact(const FsstColumn& c, const Dfa& d, const LiftedCompact& L, Bitmap& out, ScanStats& st) {
   if (L.n != d.n) fail("lifted table was built for a different automaton");
   out.assign(bitmap_bytes(c.rows), 0);
   const u16* B = d.delta.data();
   const u8* acc = d.accept.data();
   const u8* base = c.cmp.data();
   u64 matches = 0, units = 0, bytes = 0, early = 0, malformed = 0;
   for (u64 i = 0; i < c.rows; ++i) {
      const u8* p = base + c.off[i];
      const u8* e = base + c.off[i + 1];
      const u8* p0 = p;
      u32 s = d.start;
      while (p < e) {
         u32 code = *p++;
         if (code != FSST_ESC) {
            s = L.at(s, static_cast<u8>(code));
         } else {
            if (p == e) { ++malformed; s = Dfa::DEAD; break; }
            s = B[(s << 8) | *p++];
         }
         ++units;
         if (s < 2) break;
      }
      bytes += static_cast<u64>(p - p0);
      if (p < e) ++early;
      if (acc[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = c.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = bytes;
   st.early_rows = early;
   st.malformed = malformed;
}

void scan_fsst_rev(const FsstColumn& c, const Dfa& rev, const LiftedDense& Lr, const Dfa& fwd,
                   const LiftedDense& Lf, Bitmap& out, ScanStats& st) {
   if (c.rev.empty()) fail("mode fsst-rev needs the .fsst.rev sidecar; re-encode the column without --no-rev");
   if (Lr.n != rev.n || Lf.n != fwd.n) fail("lifted table was built for a different automaton");
   out.assign(bitmap_bytes(c.rows), 0);
   const u16* T = Lr.sym.data();
   const u16* Tf = Lf.sym.data();
   const u16* Bf = fwd.delta.data();
   const u8* accR = rev.accept.data();
   const u8* accF = fwd.accept.data();
   const u8* base = c.cmp.data();
   const u32 nsym = c.nsymbols;
   u64 matches = 0, units = 0, bytes = 0, early = 0, fallback = 0, malformed = 0;
   for (u64 i = 0; i < c.rows; ++i) {
      const u8* b0 = base + c.off[i];
      const u8* e = base + c.off[i + 1];
      const u8* safe = b0 + c.rev[i];
      if (safe > e) fail("row " + std::to_string(i) + ": .fsst.rev is past the end of the row");
      const u8* p = e;
      u32 s = rev.start;
      bool ambiguous = false;
      while (p > safe) {
         u32 code = *--p;
         // Within [.fsst.rev, end) every byte is a plain code, so a byte that
         // is not a defined code means either corrupt data or a .fsst.rev that
         // lies. Both must take the forward path: reading on would silently
         // misinterpret an escaped literal as a symbol. This single compare is
         // what keeps the backward walk honest -- testing only for 255 is not
         // enough, because the literal byte of an escape pair is read first and
         // can itself look like a perfectly ordinary code.
         if (code >= nsym) {
            ++malformed;
            ambiguous = true;
            break;
         }
         s = T[(s << 8) | code];
         ++units;
         if (s < 2) break;
      }
      bytes += static_cast<u64>(e - p);
      if (!ambiguous && s >= 2 && p > b0) ambiguous = true; // hit the escape region undecided
      if (ambiguous) {
         ++fallback;
         const u8* q = b0;
         u32 sf = fsst_row(Tf, Bf, fwd.start, q, e, units, malformed);
         bytes += static_cast<u64>(q - b0);
         if (accF[sf]) { bitmap_set(out, i); ++matches; }
         continue;
      }
      if (p > b0) ++early;
      if (accR[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = c.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = bytes;
   st.early_rows = early;
   st.fallback_rows = fallback;
   st.malformed = malformed;
}

void scan_fsst_rev2(const FsstColumn& c, const Dfa& rev, const LiftedDense& Lr, const Dfa& fwd,
                    const LiftedDense& Lf, Bitmap& out, ScanStats& st) {
   if (Lr.n != rev.n || Lf.n != fwd.n) fail("lifted table was built for a different automaton");
   out.assign(bitmap_bytes(c.rows), 0);
   const u16* T = Lr.sym.data();   // reversed symbol bytes, reverse automaton
   const u16* B = rev.delta.data(); // single bytes, for escaped literals
   const u16* Tf = Lf.sym.data();
   const u16* Bf = fwd.delta.data();
   const u8* accR = rev.accept.data();
   const u8* accF = fwd.accept.data();
   const u8* base = c.cmp.data();
   const u32 nsym = c.nsymbols;
   u64 matches = 0, units = 0, bytes = 0, early = 0, fallback = 0, malformed = 0;

   for (u64 i = 0; i < c.rows; ++i) {
      const u8* b0 = base + c.off[i];
      const u8* e = base + c.off[i + 1];
      const u8* p = e;
      u32 s = rev.start;
      bool bad = false;

      // Each iteration consumes one maximal 0xFF run plus, when the run is
      // even-length, the code that follows it. See the proof in fsstcol.h.
      while (p > b0 && s >= 2) {
         u8 b = *(p - 1);
         // Fast path: b is not the marker and the byte before it is not the
         // marker either, so the run length is zero and b is a plain code. On
         // real data this is the overwhelming majority of units -- ClickBench
         // URL has escapes in 0.15% of units -- and it costs the same two
         // compares the sidecar version spends on its boundary test, which is
         // why dropping the sidecar does not have to cost throughput.
         if (b != FSST_ESC && (p - 1 == b0 || *(p - 2) != FSST_ESC)) {
            if (b >= nsym) { bad = true; break; }
            s = T[(s << 8) | b];
            ++units;
            --p;
            continue;
         }
         size_t pairs;         // escape pairs <255,255> inside the run
         const u8* q;          // new left boundary
         if (b == FSST_ESC) {
            q = p;
            while (q > b0 && *(q - 1) == FSST_ESC) --q;
            size_t m = static_cast<size_t>(p - q);
            if (m % 2 != 0) { bad = true; break; } // dangling marker
            pairs = m / 2;
         } else {
            q = p - 1;
            while (q > b0 && *(q - 1) == FSST_ESC) --q;
            size_t r = static_cast<size_t>((p - 1) - q);
            if (r % 2 != 0) {
               s = B[(s << 8) | b]; // <255, b>: one literal byte
               ++units;
               pairs = (r - 1) / 2;
            } else {
               if (b >= nsym) { bad = true; break; } // undefined code: corrupt
               s = T[(s << 8) | b];
               ++units;
               pairs = r / 2;
            }
         }
         for (size_t k = 0; k < pairs && s >= 2; ++k) {
            s = B[(s << 8) | 0xFF]; // escaped literal 0xFF
            ++units;
         }
         p = q;
      }
      bytes += static_cast<u64>(e - p);

      if (bad) {
         // Unreachable for data this project's encoder produced; kept so that a
         // corrupt column still gets a defined answer rather than a guess.
         ++malformed;
         ++fallback;
         const u8* f = b0;
         u32 sf = fsst_row(Tf, Bf, fwd.start, f, e, units, malformed);
         bytes += static_cast<u64>(f - b0);
         if (accF[sf]) { bitmap_set(out, i); ++matches; }
         continue;
      }
      if (p > b0) ++early;
      if (accR[s]) { bitmap_set(out, i); ++matches; }
   }
   st.rows = c.rows;
   st.matches = matches;
   st.units = units;
   st.bytes_read = bytes;
   st.early_rows = early;
   st.fallback_rows = fallback;
   st.malformed = malformed;
}

std::vector<u32> bitmap_to_rowids(const Bitmap& b, u64 rows) {
   std::vector<u32> ids;
   for (u64 i = 0; i < rows; ++i)
      if (bitmap_get(b, i)) ids.push_back(static_cast<u32>(i));
   return ids;
}

} // namespace fsstlike
