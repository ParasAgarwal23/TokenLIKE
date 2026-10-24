// Deterministic and seeded-random tests for the FSST-LIKE experiment.
//
// Everything here compresses with the real upstream encoder in-process, so the
// tests need no datasets and no files except the handful of deliberately
// corrupt ones they write themselves.
//
// The oracle is like_reference() from src/pattern.cpp -- a plain backtracking
// matcher over characters, written independently of the DFA. A test that only
// compared the lifted scan against the byte DFA would be comparing the thing
// under test with itself.
#include "fsstlike/scan.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

using namespace fsstlike;

namespace {

int checks = 0, failures = 0;
std::string currentCase;

void check(bool ok, const std::string& what) {
   ++checks;
   if (!ok) {
      ++failures;
      std::printf("  FAIL %s%s\n", currentCase.empty() ? "" : (currentCase + ": ").c_str(), what.c_str());
   }
}

std::string show(const std::string& s) {
   std::string out;
   for (unsigned char c : s) {
      if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
      else { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", c); out += b; }
   }
   return out;
}

/// An FSST column built in memory, with the same invariants load_fsst() checks.
struct MemColumn {
   FsstColumn col;
   std::vector<std::string> values;
   u64 split_chars = 0; // UTF-8 characters cut by a symbol boundary
};

MemColumn compress(const std::vector<std::string>& values) {
   MemColumn m;
   m.values = values;
   std::vector<const u8*> ptrs(values.size());
   std::vector<size_t> lens(values.size());
   size_t vol = 0;
   for (size_t i = 0; i < values.size(); ++i) {
      ptrs[i] = reinterpret_cast<const u8*>(values[i].data());
      lens[i] = values[i].size();
      vol += lens[i];
   }
   fsst_encoder_t* enc = fsst_create(values.size(), lens.data(), ptrs.data(), 0);
   if (!enc) fail("fsst_create returned null");
   std::vector<u8> exported(FSST_MAXHEADER);
   exported.resize(fsst_export(enc, exported.data()));

   std::vector<u8> obuf(7 + 2 * vol + 16);
   std::vector<size_t> lenOut(values.size());
   std::vector<u8*> strOut(values.size());
   std::vector<u8> cmp;
   std::vector<u64> off(values.size() + 1, 0);
   size_t done = 0;
   while (done < values.size()) {
      size_t got = fsst_compress(enc, values.size() - done, lens.data() + done, ptrs.data() + done, obuf.size(),
                                 obuf.data(), lenOut.data(), strOut.data());
      if (got == 0) fail("fsst_compress made no progress");
      for (size_t k = 0; k < got; ++k) {
         cmp.insert(cmp.end(), strOut[k], strOut[k] + lenOut[k]);
         off[done + k + 1] = cmp.size();
      }
      done += got;
   }
   fsst_destroy(enc);

   import_table(m.col, exported);
   m.col.rows = values.size();
   m.col.cmp = std::move(cmp);
   m.col.off = std::move(off);
   m.col.rev.resize(values.size());
   m.col.raw_bytes = vol;
   for (u64 i = 0; i < m.col.rows; ++i) {
      const u8* p = m.col.cmp.data() + m.col.off[i];
      size_t n = static_cast<size_t>(m.col.off[i + 1] - m.col.off[i]);
      m.col.max_cmp_row = std::max(m.col.max_cmp_row, n);
      m.col.rev[i] = reverse_safe_start(p, n);
      // Count characters that a symbol boundary cuts in half: proof that the
      // byte-level automaton is really being asked to stop mid-character.
      size_t at = 0;
      const std::string& v = values[i];
      for (size_t j = 0; j < n;) {
         size_t grow;
         if (p[j] == FSST_ESC) { grow = 1; j += 2; }
         else { grow = m.col.sym_len(p[j]); ++j; }
         at += grow;
         if (at < v.size() && (static_cast<u8>(v[at]) & 0xC0) == 0x80) ++m.split_chars;
      }
   }
   return m;
}

RawColumn as_raw(const std::vector<std::string>& values) {
   RawColumn r;
   r.rows = values.size();
   r.off.assign(values.size() + 1, 0);
   for (size_t i = 0; i < values.size(); ++i) {
      r.bytes.insert(r.bytes.end(), values[i].begin(), values[i].end());
      r.off[i + 1] = r.bytes.size();
   }
   return r;
}

/// Runs every mode against the reference matcher for one pattern.
void compare_all_modes(const MemColumn& m, const RawColumn& raw, const std::string& patternText) {
   Pattern pat = parse_like(patternText);
   Bitmap expect(bitmap_bytes(m.col.rows), 0);
   for (u64 i = 0; i < m.col.rows; ++i)
      if (like_reference(reinterpret_cast<const u8*>(m.values[i].data()), m.values[i].size(), pat))
         bitmap_set(expect, i);

   Dfa fwd = build_dfa(pat, false, 20000);
   Dfa rev = build_dfa(pat, true, 20000);
   LiftedDense dense = lift_dense(fwd, m.col, false);
   LiftedDense denseRev = lift_dense(rev, m.col, true);
   LiftedCompact compact = lift_compact(dense);
   RawPlan plan = compile_raw(pat);

   struct Run { const char* name; Bitmap bits; };
   Run runs[7] = {{"raw", {}},          {"raw-opt", {}},      {"decode", {}},
                  {"fsst", {}},         {"fsst-compact", {}}, {"fsst-rev", {}},
                  {"fsst-rev2", {}}};
   ScanStats st;
   scan_raw(raw, fwd, runs[0].bits, st);
   scan_raw_opt(raw, plan, runs[1].bits, st);
   scan_decode(m.col, fwd, runs[2].bits, st);
   scan_fsst(m.col, fwd, dense, runs[3].bits, st);
   scan_fsst_compact(m.col, fwd, compact, runs[4].bits, st);
   scan_fsst_rev(m.col, rev, denseRev, fwd, dense, runs[5].bits, st);
   scan_fsst_rev2(m.col, rev, denseRev, fwd, dense, runs[6].bits, st);

   for (const Run& r : runs) {
      if (r.bits == expect) { ++checks; continue; }
      ++checks;
      ++failures;
      std::printf("  FAIL mode %s disagrees with the reference on pattern '%s'\n", r.name, show(patternText).c_str());
      int shown = 0;
      for (u64 i = 0; i < m.col.rows && shown < 3; ++i)
         if (bitmap_get(r.bits, i) != bitmap_get(expect, i)) {
            std::printf("       row %llu value '%s': reference=%d %s=%d\n", (unsigned long long)i,
                        show(m.values[i]).c_str(), (int)bitmap_get(expect, i), r.name, (int)bitmap_get(r.bits, i));
            ++shown;
         }
   }

   // Lifted tables must agree with running the symbol's bytes through the DFA.
   for (u32 s = 0; s < fwd.n; ++s)
      for (u32 c = 0; c < m.col.nsymbols; ++c) {
         u32 want = run_bytes(fwd, s, m.col.sym_bytes(c), m.col.sym_len(c));
         if (dense.sym[(static_cast<size_t>(s) << 8) | c] != want ||
             compact.at(s, static_cast<u8>(c)) != want) {
            check(false, "lifted transition (" + std::to_string(s) + "," + std::to_string(c) + ") is wrong");
            return;
         }
      }
   ++checks;
}

const std::vector<std::string>& edge_values() {
   static const std::vector<std::string> v = {
       "",                          // empty string
       " ",                         // single space
       "h", "ht", "htt", "http", "http:", "http://",
       "http://example.com/index.html",
       "http://example.com/index.php",
       "http://example.com/a/b/c/d/e/f/g.m3u8",
       "/static/js/app.js",
       "%literal-percent%", "_literal-underscore_", "back\\slash",
       "100%", "a_b", "__",
       "\xD1\x81\xD0\xB5\xD0\xB7\xD0\xBE\xD0\xBD",             // cyrillic 'сезон'
       "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",                 // japanese '日本語'
       "\xF0\x9F\x8E\x89\xF0\x9F\x8E\x88",                     // emoji
       "na\xC3\xAFve-caf\xC3\xA9.html",                        // latin-1 supplement
       "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9",     // many 2-byte chars
       "a\xF0\x9F\x8E\x89" "b\xF0\x9F\x8E\x89" "c",            // 4-byte chars between ASCII
       std::string(300, 'x'),                                  // long, highly compressible
       std::string(1, '\x01') + std::string(1, '\x7F'),        // control bytes
       "\xC2\x80\xC2\x81\xC2\xBF",                             // low 2-byte chars
       "tab\there", "new\nline", "quote\"inside", "apos'inside",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",                     // one repeated symbol
       "zq\x01\x02\x03\x04\x05\x06\x07zq",                     // rare bytes -> escapes
       "\xEF\xBF\xBD",                                         // U+FFFD replacement char
       "\xF4\x8F\xBF\xBF",                                     // U+10FFFF, highest code point
   };
   return v;
}

const std::vector<std::string>& edge_patterns() {
   static const std::vector<std::string> p = {
       "", "%", "%%", "_", "__", "%_", "_%", "%_%",
       "h", "http", "http%", "http://%", "%http%", "%.html", "%.php", "%.m3u8",
       "/static/%", "%/a/b/%", "http://example.com/index.html",
       "%example%", "%e%a%", "%_.html", "%.htm_", "h_tp%", "h__p://%",
       "100%%", "%\\%", "a_b", "%literal-percent%",
       "\xD1\x81%", "%\xD0\xBD", "_\xD0\xB5%", "___", "_____",
       "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "_\xE6\x9C\xAC_",
       "\xF0\x9F\x8E\x89%", "_\xF0\x9F\x8E\x89%", "a_b_c", "%\xF0\x9F\x8E\x89_",
       "na_ve-caf_.html", "%AAAA%", "%\x01%", "%zq%", "%\x07%",
       std::string(200, 'x') + "%", "%" + std::string(50, 'x') + "%",
   };
   return p;
}

void test_pattern_parsing() {
   currentCase = "pattern parsing";
   check(parse_like("").items.empty(), "empty pattern has no items");
   check(parse_like("%%%%").items.size() == 1, "runs of %% collapse to one Star");
   check(parse_like("a%b").items.size() == 3, "'a%b' is Literal Star Literal");
   check(parse_like("_").items.size() == 1 && parse_like("_").items[0].kind == ItemKind::AnyChar,
         "'_' is one AnyChar");
   check(parse_like("a\\b").items.size() == 1, "backslash is an ordinary literal byte");
   check(parse_like("\xF0\x9F\x8E\x89").items[0].bytes.size() == 4, "a 4-byte character stays one literal run");
   bool threw = false;
   try { parse_like("\xFF\xFE"); } catch (const std::exception&) { threw = true; }
   check(threw, "an invalid UTF-8 pattern is rejected");
   threw = false;
   try { parse_like("a\xC3"); } catch (const std::exception&) { threw = true; }
   check(threw, "a truncated UTF-8 character in the pattern is rejected");
   currentCase.clear();
}

void test_dfa_limits() {
   currentCase = "DFA limits";
   bool threw = false;
   try { build_dfa(parse_like(std::string(100, 'a') + "%"), false, 8); } catch (const std::exception&) { threw = true; }
   check(threw, "a pattern over --max-states is rejected rather than truncated");
   Dfa d = build_dfa(parse_like("%"), false, 20000);
   check(d.accept[d.start] != 0, "'%' accepts from its start state");
   check(d.start == Dfa::ASINK, "'%' collapses to the accept sink");
   Dfa e = build_dfa(parse_like(""), false, 20000);
   check(e.accept[e.start] != 0, "the empty pattern accepts the empty string");
   check(e.step(e.start, 'a') == Dfa::DEAD, "the empty pattern rejects any byte");
   currentCase.clear();
}

void test_reverse_safe() {
   currentCase = "reverse-safe boundary";
   // Hand-built streams: 255 is the escape marker, everything else is a code.
   struct Case { std::vector<u8> bytes; u32 want; };
   std::vector<Case> cases = {
       {{}, 0},
       {{1, 2, 3}, 0},
       {{255, 7}, 2},
       {{1, 255, 7, 2, 3}, 3},
       {{255, 255}, 2},                 // escaped literal 0xFF
       {{255, 255, 255, 255}, 4},       // two escaped 0xFF bytes
       {{1, 255, 255, 2}, 3},
   };
   for (const Case& c : cases)
      check(reverse_safe_start(c.bytes.data(), c.bytes.size()) == c.want,
            "reverse_safe_start expected " + std::to_string(c.want));
   bool threw = false;
   try { std::vector<u8> b = {1, 255}; reverse_safe_start(b.data(), b.size()); } catch (const std::exception&) { threw = true; }
   check(threw, "a dangling escape marker is rejected");
   currentCase.clear();
}

void test_robustness() {
   currentCase = "robustness";
   MemColumn m = compress(edge_values());

   // A symbol table that is too short, too long, or not an FSST export at all.
   for (std::vector<u8> bad : std::vector<std::vector<u8>>{{}, {1, 2, 3}, std::vector<u8>(16, 0),
                                                           std::vector<u8>(FSST_MAXHEADER + 1, 0)}) {
      FsstColumn c;
      bool threw = false;
      try { import_table(c, bad); } catch (const std::exception&) { threw = true; }
      check(threw, "a malformed symbol table of " + std::to_string(bad.size()) + " bytes is rejected");
   }
   {
      // Valid header, but the hidden symbol count disagrees with the histogram.
      std::vector<u8> bad = m.col.exported;
      bad[1] = static_cast<u8>(bad[1] ^ 0x01);
      FsstColumn c;
      bool threw = false;
      try { import_table(c, bad); } catch (const std::exception&) { threw = true; }
      check(threw, "a symbol table whose declared count disagrees with its histogram is rejected");
   }

   // A code the table never defines.
   {
      FsstColumn c = m.col;
      bool placed = false;
      for (u64 i = 0; i < c.rows && !placed; ++i)
         if (c.off[i + 1] > c.off[i] && c.cmp[c.off[i]] != FSST_ESC) {
            c.cmp[c.off[i]] = static_cast<u8>(c.nsymbols); // first undefined code
            placed = true;
         }
      check(placed, "found a row to corrupt with an undefined code");
      bool threw = false;
      try { validate_fsst(c, false); } catch (const std::exception&) { threw = true; }
      check(threw, "an out-of-range code is rejected");
      // The scan must stay in bounds and never report it as a match.
      Pattern pat = parse_like("%");
      Dfa d = build_dfa(pat, false, 20000);
      LiftedDense L = lift_dense(d, c, false);
      Bitmap bits;
      ScanStats st;
      scan_fsst(c, d, L, bits, st);
      check(bits.size() == bitmap_bytes(c.rows), "scanning corrupt data still produces a full bitmap");
   }

   // A row that ends in a dangling escape marker: truncation at the worst spot.
   {
      FsstColumn c = m.col;
      u64 victim = UINT64_MAX;
      for (u64 i = 0; i < c.rows; ++i)
         if (c.off[i + 1] - c.off[i] >= 2) { victim = i; break; }
      check(victim != UINT64_MAX, "found a row long enough to truncate");
      c.cmp[c.off[victim + 1] - 1] = FSST_ESC;
      bool threw = false;
      try { validate_fsst(c, false); } catch (const std::exception&) { threw = true; }
      check(threw, "a dangling escape marker at the end of a row is rejected");
      Pattern pat = parse_like("%x%");
      Dfa d = build_dfa(pat, false, 20000);
      LiftedDense L = lift_dense(d, c, false);
      Bitmap bits;
      ScanStats st;
      scan_fsst(c, d, L, bits, st);
      check(st.malformed >= 1, "the scan counts the dangling escape instead of reading past the row");
   }

   // A correct .rev, with a pattern whose reverse automaton cannot decide from
   // the right: every row is walked to its start, so rows with escapes must
   // take the forward fallback.
   {
      Pattern pat = parse_like("zq%");
      Dfa fwd = build_dfa(pat, false, 20000), rv = build_dfa(pat, true, 20000);
      LiftedDense Lf = lift_dense(fwd, m.col, false), Lr = lift_dense(rv, m.col, true);
      Bitmap bits, want;
      ScanStats st, st2;
      scan_fsst_rev(m.col, rv, Lr, fwd, Lf, bits, st);
      scan_fsst(m.col, fwd, Lf, want, st2);
      check(bits == want, "fsst-rev agrees with fsst on a pattern that forces whole-row reverse scans");
      check(st.fallback_rows >= 1, "rows with escapes take the forward fallback (" +
                                       std::to_string(st.fallback_rows) + ")");
      check(st.malformed == 0, "a correct .fsst.rev produces no malformed reports");
   }

   // A .rev that lies: zeroed, so the reverse walk runs into escape bytes it
   // was told were not there. It must notice rather than misread them.
   {
      FsstColumn c = m.col;
      for (u64 i = 0; i < c.rows; ++i) c.rev[i] = 0;
      Pattern pat = parse_like("zq%");
      Dfa fwd = build_dfa(pat, false, 20000), rv = build_dfa(pat, true, 20000);
      LiftedDense Lf = lift_dense(fwd, c, false), Lr = lift_dense(rv, c, true);
      Bitmap bits, want;
      ScanStats st, st2;
      scan_fsst_rev(c, rv, Lr, fwd, Lf, bits, st);
      scan_fsst(m.col, fwd, Lf, want, st2);
      check(st.malformed >= 1, "the reverse scan notices the escape it was told was not there (" +
                                   std::to_string(st.malformed) + ")");
      check(st.fallback_rows >= 1, "and falls back to the forward scan for those rows");
      check(bits == want, "and still gives the right answer");
   }

   // A .rev that points past the end of its row must be rejected outright.
   {
      FsstColumn c = m.col;
      c.rev[c.rows - 1] = static_cast<u32>(c.cmp.size() + 1);
      Pattern pat = parse_like("%a%");
      Dfa fwd = build_dfa(pat, false, 20000), rv = build_dfa(pat, true, 20000);
      LiftedDense Lf = lift_dense(fwd, c, false), Lr = lift_dense(rv, c, true);
      Bitmap bits;
      ScanStats st;
      bool threw = false;
      try { scan_fsst_rev(c, rv, Lr, fwd, Lf, bits, st); } catch (const std::exception&) { threw = true; }
      check(threw, "a .fsst.rev entry past the end of its row is rejected");
   }

   // Offsets: load_fsst is the only path that reads them, so write real files.
   {
      std::string dir = "build/badcol";
      std::string prefix = dir + "/bad";
      std::filesystem::create_directories(dir);  // portable (was `mkdir -p`, POSIX only)
      write_file(prefix + ".fsst.sym", m.col.exported.data(), m.col.exported.size());
      write_file(prefix + ".fsst.cmp", m.col.cmp.data(), m.col.cmp.size());
      std::string meta = "{\"rows\": " + std::to_string(m.col.rows) + ", \"raw_bytes\": 1}\n";
      write_file(prefix + ".fsst.meta.json", meta.data(), meta.size());

      std::vector<u64> off = m.col.off;
      off[1] = off[0] == 0 ? UINT64_MAX : 0; // non-monotonic
      write_file(prefix + ".fsst.off", off.data(), off.size() * sizeof(u64));
      bool threw = false;
      try { load_fsst(prefix, false); } catch (const std::exception&) { threw = true; }
      check(threw, "non-monotonic offsets are rejected");

      off = m.col.off;
      off[0] = 1;
      write_file(prefix + ".fsst.off", off.data(), off.size() * sizeof(u64));
      threw = false;
      try { load_fsst(prefix, false); } catch (const std::exception&) { threw = true; }
      check(threw, "off[0] != 0 is rejected");

      off = m.col.off;
      off.back() -= 1; // terminal offset disagrees with the payload
      write_file(prefix + ".fsst.off", off.data(), off.size() * sizeof(u64));
      threw = false;
      try { load_fsst(prefix, false); } catch (const std::exception&) { threw = true; }
      check(threw, "a terminal offset that disagrees with .fsst.cmp is rejected");

      // A size that is not a whole number of u64s.
      std::vector<u8> ragged(m.col.off.size() * 8 - 3, 0);
      write_file(prefix + ".fsst.off", ragged.data(), ragged.size());
      threw = false;
      try { load_fsst(prefix, false); } catch (const std::exception&) { threw = true; }
      check(threw, "an offset file whose size is not a multiple of 8 is rejected");

      write_file(prefix + ".fsst.off", m.col.off.data(), m.col.off.size() * sizeof(u64));
      meta = "{\"rows\": 7, \"raw_bytes\": 1}\n";
      write_file(prefix + ".fsst.meta.json", meta.data(), meta.size());
      threw = false;
      try { load_fsst(prefix, false); } catch (const std::exception&) { threw = true; }
      check(threw, "a row count in meta.json that disagrees with the offsets is rejected");

      threw = false;
      try { load_fsst(dir + "/missing", false); } catch (const std::exception&) { threw = true; }
      check(threw, "a missing column is rejected");
   }
   currentCase.clear();
}

void test_deterministic_matrix() {
   currentCase = "deterministic matrix";
   MemColumn m = compress(edge_values());
   RawColumn raw = as_raw(edge_values());
   std::printf("  fixture: %zu values x %zu patterns x 7 modes, %u symbols\n", edge_values().size(),
               edge_patterns().size(), m.col.nsymbols);
   check(m.split_chars > 0,
         "at least one UTF-8 character is split by a symbol boundary (" + std::to_string(m.split_chars) + ")");
   u64 escapes = 0;
   for (u64 i = 0; i < m.col.rows; ++i) escapes += (m.col.rev[i] != 0);
   check(escapes > 0, "the fixture produces escape sequences (" + std::to_string(escapes) + " rows)");
   validate_fsst(m.col, true);
   ++checks;
   for (const std::string& p : edge_patterns()) compare_all_modes(m, raw, p);
   currentCase.clear();
}

void test_compressibility_extremes() {
   currentCase = "compressibility extremes";
   // Very high compression: one long repeated string.
   std::vector<std::string> high(200, std::string(64, 'q'));
   MemColumn hm = compress(high);
   check(hm.col.cmp.size() * 4 < hm.col.raw_bytes, "a repetitive column compresses at least 4x");
   compare_all_modes(hm, as_raw(high), "%qqqq%");
   compare_all_modes(hm, as_raw(high), "q%");

   // Very low compression: random ASCII. Note FSST can hand each of the 127
   // distinct bytes its own single-byte code, so this produces almost no
   // escapes -- the thing to assert is the poor ratio, not escapes.
   std::mt19937 rng(12345);
   std::vector<std::string> low;
   for (int i = 0; i < 200; ++i) {
      std::string s;
      for (int k = 0; k < 24; ++k) s += static_cast<char>(1 + (rng() % 127));
      low.push_back(s);
   }
   MemColumn lm = compress(low);
   check(lm.col.cmp.size() * 10 > lm.col.raw_bytes * 9, "random ASCII barely compresses");
   compare_all_modes(lm, as_raw(low), "%a%");
   compare_all_modes(lm, as_raw(low), "_%");

   // Escape-heavy: long repeated phrases eat the 255 symbol slots, so the rare
   // bytes mixed in have no code left and must be escaped.
   std::vector<std::string> starved;
   for (int i = 0; i < 400; ++i) {
      std::string s = "https://www.example.com/catalog/section-";
      s += static_cast<char>('a' + (i % 26));
      s += "/page/index.html?ref=frontpage&lang=en";
      starved.push_back(s);
   }
   for (int i = 1; i < 127; ++i) starved.push_back(std::string(3, static_cast<char>(i)));
   MemColumn sm = compress(starved);
   u64 esc = 0;
   for (u64 i = 0; i < sm.col.rows; ++i) {
      const u8* p = sm.col.cmp.data() + sm.col.off[i];
      size_t n = static_cast<size_t>(sm.col.off[i + 1] - sm.col.off[i]);
      for (size_t j = 0; j < n;) { if (p[j] == FSST_ESC) { ++esc; j += 2; } else ++j; }
   }
   check(esc > 0, "a table starved of slots escapes the rare bytes (" + std::to_string(esc) + ")");
   compare_all_modes(sm, as_raw(starved), "%index.html%");
   compare_all_modes(sm, as_raw(starved), "https://%");
   compare_all_modes(sm, as_raw(starved), "%en");
   compare_all_modes(sm, as_raw(starved), "%_a_%");
   currentCase.clear();
}

/// Appends one random well-formed UTF-8 character from a mixed distribution.
void random_char(std::mt19937& rng, std::string& out) {
   u32 pick = rng() % 100;
   if (pick < 55) out += static_cast<char>(0x20 + rng() % 0x5F);                 // ASCII printable
   else if (pick < 70) out += static_cast<char>("/.:-_abc"[rng() % 8]);           // URL-ish, compressible
   else if (pick < 85) { u32 cp = 0x80 + rng() % 0x700; out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
   else if (pick < 95) { u32 cp = 0x1000 + rng() % 0x1000; out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
   else { u32 cp = 0x10000 + rng() % 0x1000; out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

/// Exhaustively checks that reverse unit parsing, which uses no sidecar, agrees
/// with forward parsing. This is the test behind the claim in fsstcol.h; if the
/// parity argument were wrong, this is where it would show.
void test_unit_parse_equivalence() {
   currentCase = "reverse unit parsing (no sidecar)";
   u64 checked = 0, malformed = 0;
   std::vector<u8> buf;
   auto one = [&](const std::vector<u8>& row) {
      std::vector<Unit> f, r;
      bool fBad = false, rBad = false;
      try { f = parse_units_forward(row.data(), row.size()); } catch (const std::exception&) { fBad = true; }
      try { r = parse_units_reverse(row.data(), row.size()); } catch (const std::exception&) { rBad = true; }
      ++checked;
      if (fBad || rBad) {
         ++malformed;
         if (fBad != rBad) {
            std::string hex;
            for (u8 b : row) { char t[4]; std::snprintf(t, sizeof(t), "%02x", b); hex += t; }
            check(false, "forward and reverse disagree on whether " + hex + " is malformed (fwd bad=" +
                             std::to_string(fBad) + ", rev bad=" + std::to_string(rBad) + ")");
         }
         return;
      }
      if (f != r) {
         std::string hex;
         for (u8 b : row) { char t[4]; std::snprintf(t, sizeof(t), "%02x", b); hex += t; }
         check(false, "reverse parse of " + hex + " produced " + std::to_string(r.size()) +
                          " units, forward produced " + std::to_string(f.size()));
      }
   };

   // Every string over an alphabet built around the escape marker, up to
   // length 10: 3^0 + ... + 3^10 rows.
   const u8 alpha[3] = {0x00, 0x05, 0xFF};
   for (size_t len = 0; len <= 10; ++len) {
      std::vector<size_t> idx(len, 0);
      for (;;) {
         buf.clear();
         for (size_t k = 0; k < len; ++k) buf.push_back(alpha[idx[k]]);
         one(buf);
         size_t k = 0;
         for (; k < len; ++k) {
            if (++idx[k] < 3) break;
            idx[k] = 0;
         }
         if (k == len) break;
      }
   }
   // Runs of nothing but the escape marker, both parities, up to 24 bytes.
   for (size_t len = 0; len <= 24; ++len) one(std::vector<u8>(len, 0xFF));
   // Seeded random rows over the full byte range.
   std::mt19937 rng(77);
   for (int i = 0; i < 20000; ++i) {
      buf.clear();
      size_t len = rng() % 24;
      for (size_t k = 0; k < len; ++k)
         buf.push_back(static_cast<u8>((rng() % 4 == 0) ? 0xFF : (rng() & 0xFF)));
      one(buf);
   }
   check(malformed > 0, "the corpus contains malformed rows, so the agreement is not vacuous (" +
                            std::to_string(malformed) + " of " + std::to_string(checked) + ")");
   std::printf("  reverse unit parsing: %llu rows checked, %llu of them malformed, both parsers agreed\n",
               (unsigned long long)checked, (unsigned long long)malformed);
   currentCase.clear();
}

/// Builds columns out of *arbitrary* valid unit sequences, including escape
/// literals of 0xFF and long escape runs, which the real encoder would never
/// emit. Every FSST mode must agree with scanning the expanded bytes directly.
/// Patterns here avoid '_' because the expanded bytes are not valid UTF-8 and
/// '_' is only defined on characters; the UTF-8 cases are covered elsewhere.
void test_adversarial_escape_columns(u32 seed) {
   currentCase = "adversarial escape streams";
   std::mt19937 rng(seed);
   MemColumn seedCol = compress(edge_values()); // borrow a real symbol table
   const u32 nsym = seedCol.col.nsymbols;

   std::vector<std::string> expanded;
   std::vector<u8> cmp;
   std::vector<u64> off{0};
   u64 escapeUnits = 0, ffLiterals = 0;
   for (int row = 0; row < 400; ++row) {
      std::string text;
      size_t units = rng() % 14;
      for (size_t u = 0; u < units; ++u) {
         // Deliberately escape-heavy, and deliberately fond of 0xFF.
         if (rng() % 3 == 0) {
            u8 lit = (rng() % 2 == 0) ? 0xFF : static_cast<u8>(rng() & 0xFF);
            cmp.push_back(FSST_ESC);
            cmp.push_back(lit);
            text.push_back(static_cast<char>(lit));
            ++escapeUnits;
            if (lit == 0xFF) ++ffLiterals;
         } else {
            u8 code = static_cast<u8>(rng() % nsym);
            cmp.push_back(code);
            const u8* sb = seedCol.col.sym_bytes(code);
            for (u32 k = 0; k < seedCol.col.sym_len(code); ++k) text.push_back(static_cast<char>(sb[k]));
         }
      }
      off.push_back(cmp.size());
      expanded.push_back(text);
   }
   check(escapeUnits > 0 && ffLiterals > 0,
         "the corpus really does contain escaped 0xFF literals (" + std::to_string(ffLiterals) + " of " +
             std::to_string(escapeUnits) + " escapes)");

   FsstColumn col;
   import_table(col, seedCol.col.exported);
   col.rows = expanded.size();
   col.cmp = cmp;
   col.off = off;
   col.rev.resize(col.rows);
   for (u64 i = 0; i < col.rows; ++i) {
      size_t n = static_cast<size_t>(off[i + 1] - off[i]);
      col.max_cmp_row = std::max(col.max_cmp_row, n);
      col.rev[i] = reverse_safe_start(col.cmp.data() + off[i], n);
   }
   col.raw_bytes = 0;
   for (const std::string& v : expanded) col.raw_bytes += v.size();
   validate_fsst(col, true);
   ++checks;

   RawColumn rawExpanded = as_raw(expanded);
   const char* pats[] = {"%", "", "h%", "%l", "%ttp%", "%AAAA%", "a%b", "%.%",
                         "%\xff%", "\xff%", "%\xff", "%\xff\xff%"};
   for (const char* ptext : pats) {
      std::string pt(ptext);
      if (!utf8_valid(reinterpret_cast<const u8*>(pt.data()), pt.size())) continue;
      Pattern pat = parse_like(pt);
      Dfa fwd = build_dfa(pat, false, 20000), rev = build_dfa(pat, true, 20000);
      LiftedDense dense = lift_dense(fwd, col, false), denseRev = lift_dense(rev, col, true);
      Bitmap bRaw, bFsst, bDecode, bRev, bRev2;
      ScanStats st;
      scan_raw(rawExpanded, fwd, bRaw, st);
      scan_decode(col, fwd, bDecode, st);
      scan_fsst(col, fwd, dense, bFsst, st);
      scan_fsst_rev(col, rev, denseRev, fwd, dense, bRev, st);
      scan_fsst_rev2(col, rev, denseRev, fwd, dense, bRev2, st);
      ScanStats st2;
      scan_fsst_rev2(col, rev, denseRev, fwd, dense, bRev2, st2);
      check(bDecode == bRaw, "decode matches the expanded bytes for '" + show(pt) + "'");
      check(bFsst == bRaw, "forward lifted scan matches for '" + show(pt) + "'");
      check(bRev == bRaw, "sidecar reverse scan matches for '" + show(pt) + "'");
      check(bRev2 == bRaw, "sidecar-free reverse scan matches for '" + show(pt) + "'");
      check(st2.fallback_rows == 0, "the sidecar-free scan needed no fallback for '" + show(pt) + "'");
   }
   currentCase.clear();
}

/// One byte DFA, many symbol tables: the reuse interface must give bit-identical
/// answers to rebuilding the automaton for each table.
void test_lift_reuse() {
   currentCase = "lift reuse across symbol tables";
   // Three corpora with deliberately different symbol tables.
   std::vector<std::vector<std::string>> corpora;
   corpora.push_back(edge_values());
   {
      std::vector<std::string> v;
      for (int i = 0; i < 300; ++i)
         v.push_back("https://www.example.com/catalog/" + std::to_string(i) + "/index.html");
      corpora.push_back(v);
   }
   {
      std::vector<std::string> v;
      for (int i = 0; i < 300; ++i)
         v.push_back("the quick brown fox jumps over the lazy dog number " + std::to_string(i));
      corpora.push_back(v);
   }
   std::vector<MemColumn> cols;
   std::vector<const FsstColumn*> ptrs;
   std::vector<std::string> ids;
   for (size_t i = 0; i < corpora.size(); ++i) {
      cols.push_back(compress(corpora[i]));
      ids.push_back("corpus" + std::to_string(i));
   }
   for (MemColumn& c : cols) ptrs.push_back(&c.col);
   std::vector<u32> nsyms;
   for (MemColumn& c : cols) nsyms.push_back(c.col.nsymbols);
   std::printf("  lift reuse: symbol tables of %u, %u and %u symbols\n", nsyms[0], nsyms[1], nsyms[2]);
   check(nsyms[0] != nsyms[1] || nsyms[1] != nsyms[2],
         "the three corpora really produced different symbol tables (" + std::to_string(nsyms[0]) + ", " +
             std::to_string(nsyms[1]) + ", " + std::to_string(nsyms[2]) + ")");

   for (const char* ptext : {"%index.html", "https://%", "%quick%", "%o_%", "%"}) {
      Pattern pat = parse_like(ptext);
      LiftedSet set = lift_for(pat, false, 20000, ptrs, ids);
      check(set.tables.size() == cols.size(), "lift_for produced one table per column");
      for (size_t i = 0; i < cols.size(); ++i) {
         Dfa fresh = build_dfa(pat, false, 20000);
         LiftedDense freshTable = lift_dense(fresh, cols[i].col, false);
         check(fresh.n == set.dfa.n, "a fresh automaton has the same state count as the reused one");
         Bitmap a, b;
         ScanStats sa, sb;
         scan_fsst(cols[i].col, set.dfa, set.tables[i], a, sa);
         scan_fsst(cols[i].col, fresh, freshTable, b, sb);
         check(a == b, std::string("reused automaton agrees with a fresh one on ") + ids[i] +
                           " for '" + ptext + "'");
         // And the lifted tables themselves must be identical, not just the answers.
         check(set.tables[i].sym == freshTable.sym,
               std::string("the lifted table is identical to a freshly lifted one on ") + ids[i]);
      }
   }
   currentCase.clear();
}

/// The optimised raw matcher takes shape-specific fast paths, so check that it
/// classifies shapes as intended and still agrees with the reference.
void test_raw_opt_shapes() {
   currentCase = "optimised raw baseline";
   struct Case { const char* pattern; const char* shape; };
   const Case cases[] = {
       {"", "empty"},        {"%", "any"},          {"%%", "any"},
       {"abc", "exact"},     {"abc%", "prefix"},    {"%abc", "suffix"},
       {"%abc%", "contains"},{"a%b", "general"},    {"_", "general"},
       {"%a_b%", "general"}, {"a%b%c", "general"},  {"%.php", "suffix"},
       {"http://%", "prefix"},
   };
   for (const Case& c : cases) {
      RawPlan plan = compile_raw(parse_like(c.pattern));
      check(std::string(plan.shape_name()) == c.shape,
            std::string("'") + c.pattern + "' compiles to shape " + c.shape + ", got " + plan.shape_name());
   }
   // Agreement with the reference on the edge fixture, every pattern.
   std::vector<std::string> vals = edge_values();
   for (const std::string& pt : edge_patterns()) {
      Pattern pat = parse_like(pt);
      RawPlan plan = compile_raw(pat);
      for (const std::string& v : vals) {
         const u8* s = reinterpret_cast<const u8*>(v.data());
         bool want = like_reference(s, v.size(), pat);
         bool got = raw_match(plan, s, v.size());
         if (want != got) {
            check(false, "raw-opt disagrees with the reference on value '" + show(v) + "' pattern '" +
                             show(pt) + "' (reference=" + std::to_string(want) + ")");
            break;
         }
      }
      ++checks;
   }
   currentCase.clear();
}

void test_random(u32 seed, int corpora, int patternsPer) {
   currentCase = "random seed " + std::to_string(seed);
   std::mt19937 rng(seed);
   for (int round = 0; round < corpora; ++round) {
      std::vector<std::string> values;
      size_t rows = 40 + rng() % 120;
      for (size_t i = 0; i < rows; ++i) {
         std::string s;
         size_t chars = rng() % 40;
         for (size_t k = 0; k < chars; ++k) random_char(rng, s);
         values.push_back(s);
      }
      MemColumn m = compress(values);
      RawColumn raw = as_raw(values);
      validate_fsst(m.col, true);
      ++checks;

      for (int p = 0; p < patternsPer; ++p) {
         // Build a pattern out of substrings of real values, so it actually
         // matches sometimes, mixed with wildcards at character boundaries.
         std::string pat;
         int parts = 1 + static_cast<int>(rng() % 3);
         if (rng() % 2) pat += '%';
         for (int k = 0; k < parts; ++k) {
            const std::string& v = values[rng() % values.size()];
            if (!v.empty()) {
               // Cut on character boundaries so the pattern stays valid UTF-8.
               std::vector<size_t> bounds{0};
               for (size_t j = 0; j < v.size();) {
                  u8 b = static_cast<u8>(v[j]);
                  size_t w = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                  j += w;
                  bounds.push_back(std::min(j, v.size()));
               }
               size_t a = bounds[rng() % bounds.size()], b = bounds[rng() % bounds.size()];
               if (a > b) std::swap(a, b);
               pat += v.substr(a, b - a);
            }
            u32 w = rng() % 10;
            if (w < 4) pat += '%';
            else if (w < 6) pat += '_';
         }
         if (rng() % 3 == 0) pat += '%';
         // Patterns are UTF-8 by construction, but a cut could still land badly
         // if a value was not valid; skip rather than assert.
         if (!utf8_valid(reinterpret_cast<const u8*>(pat.data()), pat.size())) continue;
         compare_all_modes(m, raw, pat);
      }
   }
   currentCase.clear();
}

} // namespace

int main(int argc, char** argv) try {
   bool quick = (argc > 1 && std::strcmp(argv[1], "--quick") == 0);
   std::printf("test_unit: FSST-LIKE experiment\n");
   test_pattern_parsing();
   test_dfa_limits();
   test_reverse_safe();
   test_unit_parse_equivalence();
   test_raw_opt_shapes();
   test_robustness();
   test_deterministic_matrix();
   test_compressibility_extremes();
   test_adversarial_escape_columns(4242);
   test_lift_reuse();
   test_random(1, quick ? 2 : 12, quick ? 10 : 30);
   test_random(20260525, quick ? 2 : 12, quick ? 10 : 30);
   std::printf("test_unit: %d checks, %d failures\n", checks, failures);
   if (failures) { std::printf("test_unit: FAILED\n"); return 1; }
   std::printf("test_unit: PASSED\n");
   return 0;
} catch (const std::exception& e) {
   std::printf("test_unit: ERROR: %s\n", e.what());
   std::printf("test_unit: FAILED (could not run)\n");
   return 1;
}
