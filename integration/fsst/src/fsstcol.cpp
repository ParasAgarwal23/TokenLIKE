#include "fsstlike/fsstcol.h"

#include <cstring>

namespace fsstlike {

void import_table(FsstColumn& c, const std::vector<u8>& exported) {
   if (exported.size() < 17)
      fail("symbol table is " + std::to_string(exported.size()) + " bytes, too short for the 17-byte header");
   if (exported.size() > FSST_MAXHEADER)
      fail("symbol table is " + std::to_string(exported.size()) + " bytes, above FSST_MAXHEADER");
   c.exported = exported;
   u32 used = fsst_import(&c.dec, exported.data());
   if (used == 0) fail("fsst_import rejected the symbol table (version mismatch)");
   if (used != exported.size())
      fail("fsst_import consumed " + std::to_string(used) + " of " + std::to_string(exported.size()) + " bytes");

   // fsst_export hides nSymbols in byte 1 of the version word; the length
   // histogram at bytes 9..16 says the same thing a second way. Cross-check,
   // because everything downstream relies on "codes >= nsymbols never occur".
   u64 version = 0;
   std::memcpy(&version, exported.data(), 8);
   u32 declared = static_cast<u32>((version >> 8) & 0xFF);
   u32 histo = 0;
   for (u32 i = 0; i < 8; ++i) histo += exported[9 + i];
   if (declared != histo)
      fail("symbol table disagrees with itself: version says " + std::to_string(declared) +
           " symbols, length histogram says " + std::to_string(histo));
   c.nsymbols = declared;
   if (c.nsymbols > 255) fail("symbol table declares " + std::to_string(c.nsymbols) + " symbols, above 255");
   for (u32 i = 0; i < c.nsymbols; ++i)
      if (c.dec.len[i] < 1 || c.dec.len[i] > 8)
         fail("symbol " + std::to_string(i) + " has length " + std::to_string(c.dec.len[i]));
}

u32 reverse_safe_start(const u8* p, size_t n) {
   size_t i = 0, last = 0;
   while (i < n) {
      if (p[i] == FSST_ESC) {
         if (i + 1 >= n) fail("row ends with a dangling escape marker at byte " + std::to_string(i));
         i += 2;
         last = i;
      } else {
         ++i;
      }
   }
   return static_cast<u32>(last);
}

static u64 read_u64_field(const std::string& json, const std::string& key, bool required) {
   size_t k = json.find("\"" + key + "\"");
   if (k == std::string::npos) {
      if (required) fail("meta.json has no \"" + key + "\" field");
      return 0;
   }
   size_t colon = json.find(':', k);
   if (colon == std::string::npos) fail("meta.json field \"" + key + "\" has no value");
   size_t i = colon + 1;
   while (i < json.size() && (json[i] == ' ' || json[i] == '\n')) ++i;
   u64 v = 0;
   bool any = false;
   while (i < json.size() && json[i] >= '0' && json[i] <= '9') { v = v * 10 + u64(json[i++] - '0'); any = true; }
   if (!any) fail("meta.json field \"" + key + "\" is not a non-negative integer");
   return v;
}

FsstColumn load_fsst(const std::string& prefix, bool need_rev) {
   FsstColumn c;
   import_table(c, read_file(prefix + ".fsst.sym"));
   c.off = read_array<u64>(prefix + ".fsst.off");
   if (c.off.empty()) fail(prefix + ".fsst.off: empty, expected at least one offset");
   c.rows = c.off.size() - 1;
   c.cmp = read_file(prefix + ".fsst.cmp");
   if (need_rev) {
      c.rev = read_array<u32>(prefix + ".fsst.rev");
      if (c.rev.size() != c.rows)
         fail(prefix + ".fsst.rev: " + std::to_string(c.rev.size()) + " entries for " +
              std::to_string(c.rows) + " rows");
   }
   if (c.off[0] != 0) fail(prefix + ".fsst.off: off[0] is " + std::to_string(c.off[0]) + ", expected 0");
   for (u64 i = 0; i < c.rows; ++i) {
      if (c.off[i] > c.off[i + 1]) fail(prefix + ".fsst.off: not monotonic at row " + std::to_string(i));
      size_t len = static_cast<size_t>(c.off[i + 1] - c.off[i]);
      if (len > c.max_cmp_row) c.max_cmp_row = len;
   }
   if (c.off[c.rows] != c.cmp.size())
      fail(prefix + ".fsst.off: terminal offset " + std::to_string(c.off[c.rows]) + " disagrees with " +
           prefix + ".fsst.cmp size " + std::to_string(c.cmp.size()));

   std::vector<u8> meta = read_file(prefix + ".fsst.meta.json");
   std::string text(reinterpret_cast<const char*>(meta.data()), meta.size());
   u64 metaRows = read_u64_field(text, "rows", true);
   if (metaRows != c.rows)
      fail(prefix + ".fsst.meta.json: says " + std::to_string(metaRows) + " rows, offsets say " +
           std::to_string(c.rows));
   c.raw_bytes = read_u64_field(text, "raw_bytes", true);
   return c;
}

void validate_fsst(const FsstColumn& c, bool check_rev) {
   for (u64 i = 0; i < c.rows; ++i) {
      const u8* p = c.cmp.data() + c.off[i];
      size_t n = static_cast<size_t>(c.off[i + 1] - c.off[i]);
      size_t j = 0, last = 0;
      while (j < n) {
         u8 b = p[j];
         if (b == FSST_ESC) {
            if (j + 1 >= n)
               fail("row " + std::to_string(i) + " ends with a dangling escape marker");
            j += 2;
            last = j;
         } else {
            if (b >= c.nsymbols)
               fail("row " + std::to_string(i) + " byte " + std::to_string(j) + " is code " +
                    std::to_string(static_cast<u32>(b)) + ", but the table defines only " +
                    std::to_string(c.nsymbols) + " symbols");
            ++j;
         }
      }
      if (check_rev && !c.rev.empty() && c.rev[i] != last)
         fail("row " + std::to_string(i) + ": .fsst.rev says " + std::to_string(c.rev[i]) +
              " but the last escape ends at " + std::to_string(last));
   }
}

std::vector<Unit> parse_units_forward(const u8* p, size_t n) {
   std::vector<Unit> out;
   size_t i = 0;
   while (i < n) {
      if (p[i] == FSST_ESC) {
         if (i + 1 >= n) fail("dangling escape marker at byte " + std::to_string(i));
         out.push_back({UnitKind::Escape, p[i + 1]});
         i += 2;
      } else {
         out.push_back({UnitKind::Symbol, p[i]});
         ++i;
      }
   }
   return out;
}

std::vector<Unit> parse_units_reverse(const u8* p, size_t n) {
   // See the proof in fsstcol.h: the parity of the maximal 0xFF run preceding a
   // byte decides whether that byte is a code or an escaped literal.
   std::vector<Unit> rev;
   size_t end = n;
   while (end > 0) {
      u8 b = p[end - 1];
      if (b == FSST_ESC) {
         size_t q = end;
         while (q > 0 && p[q - 1] == FSST_ESC) --q;
         size_t m = end - q;
         if (m % 2 != 0)
            fail("row ends in a dangling escape marker (odd 0xFF run of " + std::to_string(m) + ")");
         for (size_t k = 0; k < m / 2; ++k) rev.push_back({UnitKind::Escape, 0xFF});
         end = q;
      } else {
         size_t q = end - 1;
         while (q > 0 && p[q - 1] == FSST_ESC) --q;
         size_t r = (end - 1) - q;
         if (r % 2 != 0) {
            rev.push_back({UnitKind::Escape, b});
            for (size_t k = 0; k < (r - 1) / 2; ++k) rev.push_back({UnitKind::Escape, 0xFF});
         } else {
            rev.push_back({UnitKind::Symbol, b});
            for (size_t k = 0; k < r / 2; ++k) rev.push_back({UnitKind::Escape, 0xFF});
         }
         end = q;
      }
   }
   std::vector<Unit> out(rev.rbegin(), rev.rend());
   return out;
}

} // namespace fsstlike
