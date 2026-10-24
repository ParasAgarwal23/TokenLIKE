#include "fsstlike/pattern.h"

namespace fsstlike {

bool Pattern::has_any_char() const {
   for (const auto& it : items)
      if (it.kind == ItemKind::AnyChar) return true;
   return false;
}

Pattern parse_like(const std::string& text) {
   const u8* p = reinterpret_cast<const u8*>(text.data());
   size_t n = text.size();
   if (!utf8_valid(p, n))
      fail("pattern is not valid UTF-8; DuckDB VARCHAR patterns always are, so this "
           "experiment rejects it instead of guessing byte semantics");

   Pattern out;
   out.text = text;
   for (size_t i = 0; i < n;) {
      if (p[i] == '%') {
         // Collapse runs of '%': '%%%' and '%' accept the same language, and
         // collapsing keeps the NFA (and every state count we report) honest.
         while (i < n && p[i] == '%') ++i;
         if (!out.items.empty() && out.items.back().kind == ItemKind::Star) continue;
         out.items.push_back({ItemKind::Star, {}});
      } else if (p[i] == '_') {
         ++i;
         out.items.push_back({ItemKind::AnyChar, {}});
      } else {
         std::vector<u8> lit;
         while (i < n && p[i] != '%' && p[i] != '_') lit.push_back(p[i++]);
         out.items.push_back({ItemKind::Literal, std::move(lit)});
      }
   }
   return out;
}

size_t utf8_char_len(const u8* s, size_t n, size_t i) {
   u8 b = s[i];
   size_t need;
   u8 lo = 0x80, hi = 0xBF;
   if (b <= 0x7F) return 1;
   else if (b >= 0xC2 && b <= 0xDF) need = 1;
   else if (b == 0xE0) { need = 2; lo = 0xA0; }
   else if (b >= 0xE1 && b <= 0xEC) need = 2;
   else if (b == 0xED) { need = 2; hi = 0x9F; }
   else if (b >= 0xEE && b <= 0xEF) need = 2;
   else if (b == 0xF0) { need = 3; lo = 0x90; }
   else if (b >= 0xF1 && b <= 0xF3) need = 3;
   else if (b == 0xF4) { need = 3; hi = 0x8F; }
   else return 0;
   if (i + need >= n) return 0;
   if (s[i + 1] < lo || s[i + 1] > hi) return 0;
   for (size_t k = 2; k <= need; ++k)
      if (s[i + k] < 0x80 || s[i + k] > 0xBF) return 0;
   return need + 1;
}

static bool match_from(const u8* s, size_t n, size_t si, const Pattern& p, size_t pi) {
   while (pi < p.items.size()) {
      const PatternItem& it = p.items[pi];
      if (it.kind == ItemKind::Literal) {
         if (n - si < it.bytes.size()) return false;
         for (size_t k = 0; k < it.bytes.size(); ++k)
            if (s[si + k] != it.bytes[k]) return false;
         si += it.bytes.size();
         ++pi;
      } else if (it.kind == ItemKind::AnyChar) {
         if (si >= n) return false;
         size_t cl = utf8_char_len(s, n, si);
         if (cl == 0) return false; // invalid UTF-8 in the data: '_' matches no character
         si += cl;
         ++pi;
      } else { // Star: try every character boundary at or after si
         if (pi + 1 == p.items.size()) return true;
         size_t j = si;
         for (;;) {
            if (match_from(s, n, j, p, pi + 1)) return true;
            if (j >= n) return false;
            size_t cl = utf8_char_len(s, n, j);
            j += (cl == 0) ? 1 : cl;
         }
      }
   }
   return si == n;
}

bool like_reference(const u8* s, size_t slen, const Pattern& p) {
   return match_from(s, slen, 0, p, 0);
}

} // namespace fsstlike
