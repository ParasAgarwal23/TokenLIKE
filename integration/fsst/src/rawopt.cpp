#include "fsstlike/rawopt.h"

#include <cstring>

namespace fsstlike {
namespace {

/// memmem is in <string.h> on macOS and the BSDs, and on glibc with
/// _GNU_SOURCE (which libstdc++ defines). The fallback keeps this portable to
/// anything else without changing behaviour.
const u8* find_bytes(const u8* hay, size_t hn, const u8* needle, size_t nn) {
   if (nn == 0) return hay;
   if (nn > hn) return nullptr;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(_GNU_SOURCE)
   return static_cast<const u8*>(memmem(hay, hn, needle, nn));
#else
   const u8* limit = hay + (hn - nn);
   for (const u8* p = hay;; ++p) {
      p = static_cast<const u8*>(std::memchr(p, needle[0], static_cast<size_t>(limit - p) + 1));
      if (!p) return nullptr;
      if (std::memcmp(p, needle, nn) == 0) return p;
      if (p == limit) return nullptr;
   }
#endif
}

/// Matches one segment starting exactly at s+pos. Returns the end offset, or
/// SIZE_MAX on failure. Deterministic: each '_' consumes the one character
/// that starts where it is placed.
size_t match_segment_at(const RawPlan::Segment& seg, const u8* s, size_t n, size_t pos) {
   for (const RawPlan::Piece& pc : seg.pieces) {
      if (pc.wildcard) {
         if (pos >= n) return SIZE_MAX;
         size_t cl = utf8_char_len(s, n, pos);
         if (cl == 0) return SIZE_MAX; // '_' matches no ill-formed sequence
         pos += cl;
      } else {
         if (n - pos < pc.bytes.size()) return SIZE_MAX;
         if (std::memcmp(s + pos, pc.bytes.data(), pc.bytes.size()) != 0) return SIZE_MAX;
         pos += pc.bytes.size();
      }
   }
   return pos;
}

/// Smallest offset >= from at which the segment matches. Writes the end offset.
size_t find_segment(const RawPlan::Segment& seg, const u8* s, size_t n, size_t from, size_t& end) {
   if (seg.leads_with_literal()) {
      // Locate the leading literal with memmem, then verify the rest.
      const std::vector<u8>& lead = seg.pieces[0].bytes;
      size_t at = from;
      while (at <= n) {
         const u8* hit = find_bytes(s + at, n - at, lead.data(), lead.size());
         if (!hit) return SIZE_MAX;
         size_t p = static_cast<size_t>(hit - s);
         size_t e = match_segment_at(seg, s, n, p);
         if (e != SIZE_MAX) { end = e; return p; }
         at = p + 1;
      }
      return SIZE_MAX;
   }
   // Leading '_': step one character at a time. Rare; correctness first.
   for (size_t p = from; p <= n;) {
      size_t e = match_segment_at(seg, s, n, p);
      if (e != SIZE_MAX) { end = e; return p; }
      if (p == n) break;
      size_t cl = utf8_char_len(s, n, p);
      p += (cl == 0) ? 1 : cl;
   }
   return SIZE_MAX;
}

} // namespace

const char* RawPlan::shape_name() const {
   switch (shape) {
      case Shape::Empty: return "empty";
      case Shape::AnyString: return "any";
      case Shape::Exact: return "exact";
      case Shape::Prefix: return "prefix";
      case Shape::Suffix: return "suffix";
      case Shape::Contains: return "contains";
      default: return "general";
   }
}

RawPlan compile_raw(const Pattern& p) {
   RawPlan plan;
   plan.anchored_start = !p.starts_with_star();
   plan.anchored_end = !p.ends_with_star();

   RawPlan::Segment cur;
   auto flush = [&]() {
      if (cur.pieces.empty()) return;
      cur.pure_literal = (cur.wildcards == 0);
      if (cur.pure_literal)
         for (const RawPlan::Piece& pc : cur.pieces)
            cur.literal.insert(cur.literal.end(), pc.bytes.begin(), pc.bytes.end());
      plan.segs.push_back(std::move(cur));
      cur = RawPlan::Segment{};
   };
   for (const PatternItem& it : p.items) {
      if (it.kind == ItemKind::Star) {
         flush();
      } else if (it.kind == ItemKind::AnyChar) {
         cur.pieces.push_back({true, {}});
         ++cur.wildcards;
      } else {
         cur.pieces.push_back({false, it.bytes});
         cur.literal_bytes += it.bytes.size();
      }
   }
   flush();

   if (p.items.empty()) {
      plan.shape = RawPlan::Shape::Empty;
   } else if (plan.segs.empty()) {
      plan.shape = RawPlan::Shape::AnyString; // the pattern is nothing but '%'
   } else if (plan.segs.size() == 1 && plan.segs[0].pure_literal) {
      plan.lit = plan.segs[0].literal;
      if (plan.anchored_start && plan.anchored_end) plan.shape = RawPlan::Shape::Exact;
      else if (plan.anchored_start) plan.shape = RawPlan::Shape::Prefix;
      else if (plan.anchored_end) plan.shape = RawPlan::Shape::Suffix;
      else plan.shape = RawPlan::Shape::Contains;
   } else {
      plan.shape = RawPlan::Shape::General;
   }
   return plan;
}

bool raw_match(const RawPlan& plan, const u8* s, size_t n) {
   switch (plan.shape) {
      case RawPlan::Shape::Empty:
         return n == 0;
      case RawPlan::Shape::AnyString:
         return true;
      case RawPlan::Shape::Exact:
         return n == plan.lit.size() && std::memcmp(s, plan.lit.data(), n) == 0;
      case RawPlan::Shape::Prefix:
         return n >= plan.lit.size() && std::memcmp(s, plan.lit.data(), plan.lit.size()) == 0;
      case RawPlan::Shape::Suffix:
         return n >= plan.lit.size() &&
                std::memcmp(s + n - plan.lit.size(), plan.lit.data(), plan.lit.size()) == 0;
      case RawPlan::Shape::Contains:
         return find_bytes(s, n, plan.lit.data(), plan.lit.size()) != nullptr;
      case RawPlan::Shape::General:
         break;
   }

   const size_t nsegs = plan.segs.size();
   size_t pos = 0;
   size_t first = 0, last = nsegs;

   if (plan.anchored_start) {
      size_t e = match_segment_at(plan.segs[0], s, n, 0);
      if (e == SIZE_MAX) return false;
      pos = e;
      first = 1;
      if (nsegs == 1) return plan.anchored_end ? (pos == n) : true;
   }

   if (plan.anchored_end) {
      // The tail segment must end exactly at n. Its byte length varies with
      // '_', so try the few offsets that are arithmetically possible.
      const RawPlan::Segment& tail = plan.segs[nsegs - 1];
      --last;
      if (last < first) return pos == n; // the anchored head was also the tail
      // Middle segments first, so the tail gets as much room as possible.
      for (size_t i = first; i < last; ++i) {
         size_t end = 0;
         size_t at = find_segment(plan.segs[i], s, n, pos, end);
         if (at == SIZE_MAX) return false;
         pos = end;
      }
      if (n < tail.min_bytes()) return false;
      size_t lo = (n >= tail.max_bytes()) ? n - tail.max_bytes() : 0;
      size_t hi = n - tail.min_bytes();
      if (lo < pos) lo = pos;
      for (size_t q = lo; q <= hi; ++q)
         if (match_segment_at(tail, s, n, q) == n) return true;
      return false;
   }

   for (size_t i = first; i < last; ++i) {
      size_t end = 0;
      size_t at = find_segment(plan.segs[i], s, n, pos, end);
      if (at == SIZE_MAX) return false;
      pos = end;
   }
   return true;
}

} // namespace fsstlike
