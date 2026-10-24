// The matching modes compared by this experiment.
//
//   raw          byte DFA over the uncompressed strings
//   raw-opt      optimised raw matcher: anchored compares, memmem, segment walk
//   decode       fsst_decompress() each row in full, then the byte DFA
//   fsst         lifted DFA over the compressed codes, dense table, forward
//   fsst-compact as fsst, with the default+exception table
//   fsst-rev     lifted *reverse* DFA, right to left, using the 4-byte-per-row
//                .fsst.rev sidecar, with a forward fallback for rows whose
//                escapes make the tail ambiguous (see fsstcol.h on .rev)
//   fsst-rev2    the same reverse scan with **no sidecar at all**, resolving
//                each unit from the parity of the 0xFF run before it. Proof in
//                fsstcol.h; exhaustively tested against the forward parse.
//
// All five produce a bit-packed match bitmap and must agree exactly.
#pragma once

#include "fsstlike/dfa.h"
#include "fsstlike/lift.h"
#include "fsstlike/rawopt.h"

namespace fsstlike {

struct ScanStats {
   u64 rows = 0;
   u64 matches = 0;
   u64 units = 0;         // codes+escape-pairs (fsst modes) or bytes (raw/decode)
   u64 bytes_read = 0;    // bytes of the scanned representation actually touched
   u64 early_rows = 0;    // rows decided before their last unit
   u64 fallback_rows = 0; // fsst-rev only
   u64 malformed = 0;
   u64 decoded_bytes = 0; // decode mode only
};

void scan_raw(const RawColumn& c, const Dfa& d, Bitmap& out, ScanStats& st);
void scan_raw_concat(const RawColumn& a, const RawColumn& b, const Dfa& d, Bitmap& out, ScanStats& st);

void scan_raw_opt(const RawColumn& c, const RawPlan& plan, Bitmap& out, ScanStats& st);
void scan_raw_opt_concat(const RawColumn& a, const RawColumn& b, const RawPlan& plan, Bitmap& out,
                         ScanStats& st);

void scan_decode(const FsstColumn& c, const Dfa& d, Bitmap& out, ScanStats& st);
void scan_decode_concat(const FsstColumn& a, const FsstColumn& b, const Dfa& d, Bitmap& out, ScanStats& st);

void scan_fsst(const FsstColumn& c, const Dfa& d, const LiftedDense& L, Bitmap& out, ScanStats& st);
void scan_fsst_concat(const FsstColumn& a, const LiftedDense& La, const FsstColumn& b, const LiftedDense& Lb,
                      const Dfa& d, Bitmap& out, ScanStats& st);
void scan_fsst_compact(const FsstColumn& c, const Dfa& d, const LiftedCompact& L, Bitmap& out, ScanStats& st);

/// fwd/Lf are used only for the fallback rows.
void scan_fsst_rev(const FsstColumn& c, const Dfa& rev, const LiftedDense& Lr, const Dfa& fwd,
                   const LiftedDense& Lf, Bitmap& out, ScanStats& st);

/// Sidecar-free reverse scan. fwd/Lf are used only for rows that turn out to be
/// malformed, which cannot happen for data this project's encoder wrote.
void scan_fsst_rev2(const FsstColumn& c, const Dfa& rev, const LiftedDense& Lr, const Dfa& fwd,
                    const LiftedDense& Lf, Bitmap& out, ScanStats& st);

/// Row ids of the set bits, for --rowids.
std::vector<u32> bitmap_to_rowids(const Bitmap& b, u64 rows);

} // namespace fsstlike
