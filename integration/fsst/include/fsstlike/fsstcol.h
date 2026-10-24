// An FSST-compressed string column on disk, plus its symbol table.
//
// Format (all integers little-endian), produced by src/encode.cpp:
//
//   <prefix>.fsst.sym   the fsst_export() buffer, verbatim (<= FSST_MAXHEADER)
//   <prefix>.fsst.cmp   compressed rows, concatenated, no padding
//   <prefix>.fsst.off   u64[rows + 1] prefix sums into .cmp, off[0] == 0
//   <prefix>.fsst.rev   u32[rows] reverse-scan safe start, see below
//   <prefix>.fsst.meta.json
//
// .rev is the metadata that makes *backward* scanning of compressed bytes
// exact. FSST codes are 0..254 and 255 is the escape marker, so a byte 0xFF in
// the stream is either a marker or an escaped literal 0xFF -- which can only be
// told apart by parsing forward. rev[i] is the offset, relative to the start of
// row i, just past the last escape sequence in that row (0 when the row has no
// escape at all). Every byte at or after that offset is a plain one-byte code,
// so a right-to-left walk over [rev[i], rowlen) is unambiguous. A reverse scan
// that has not decided by the time it reaches rev[i] falls back to a forward
// scan of that row; scan statistics count how often that happens.
#pragma once

#include "fsstlike/common.h"

#include "fsst.h"

namespace fsstlike {

struct FsstColumn {
   u64 rows = 0;
   std::vector<u8> cmp;
   std::vector<u64> off; // rows + 1
   std::vector<u32> rev; // rows, or empty when not loaded
   std::vector<u8> exported;
   fsst_decoder_t dec{};
   u32 nsymbols = 0;      // codes 0 .. nsymbols-1 are defined
   size_t max_cmp_row = 0;
   u64 raw_bytes = 0;     // uncompressed size, from the .meta.json

   const u8* sym_bytes(u32 code) const { return reinterpret_cast<const u8*>(&dec.symbol[code]); }
   u32 sym_len(u32 code) const { return dec.len[code]; }
   size_t cmp_bytes() const { return cmp.size(); }
   /// Safe upper bound on any single row's decompressed size: every code
   /// expands to at most 8 bytes and an escape pair to one.
   size_t decode_bound() const { return max_cmp_row * 8 + 8; }
};

/// Fills dec/nsymbols from an fsst_export() buffer. Throws if the buffer's
/// declared symbol count and its length histogram disagree.
void import_table(FsstColumn& c, const std::vector<u8>& exported);

FsstColumn load_fsst(const std::string& prefix, bool need_rev);

/// Structural validation, independent of any matching: offsets, code range,
/// escape well-formedness, and that .rev really is the last-escape boundary.
/// Throws on the first problem, naming the row.
void validate_fsst(const FsstColumn& c, bool check_rev);

/// Offset just past the last escape sequence in [p, p+n), or 0 if there is
/// none. Throws if the row ends in a dangling escape marker.
u32 reverse_safe_start(const u8* p, size_t n);

// ---------------------------------------------------------------------------
// Unit-level parsing, and why the .rev sidecar turns out to be unnecessary.
// ---------------------------------------------------------------------------
//
// A compressed row is a sequence of *units*, each either a one-byte code
// (0..254) or an escape pair <255, b> standing for the single literal byte b.
//
// Reverse parsing looks ambiguous: byte 255 is never a code, so a 0xFF in the
// stream is either a marker or the literal of an escape pair, and reading right
// to left you meet the literal first. That is why the first version of this
// experiment stored `.fsst.rev`. It is not actually necessary. Proof:
//
//   Lemma. The first byte of a maximal run of 0xFF always starts a unit.
//     A 0xFF is either a marker, which starts a unit, or the literal of an
//     escape pair, whose marker is the immediately preceding byte and is also
//     0xFF. The first byte of a *maximal* run has no 0xFF before it, so it
//     cannot be a literal. Hence it is a marker, and markers start units. []
//
//   Consequence. Let the last byte of the row be b.
//     * b == 0xFF. Let m be the length of the maximal 0xFF run ending at the
//       row's end. By the lemma the run starts a unit, so forward-parsing it is
//       forced: it is m/2 escape pairs <255,255>. m odd means the row ends in a
//       dangling marker and is malformed.
//     * b < 0xFF. Let r be the length of the maximal 0xFF run ending just
//       before b; by the lemma it starts a unit, so the parse of those r bytes
//       plus b is forced by the parity of r. r even => r/2 pairs <255,255> and
//       b is a code. r odd => (r-1)/2 pairs <255,255> and then <255, b>.
//
// So the last unit of a row is determined by the parity of the 0xFF run that
// precedes it, and the same argument applies to each unit in turn. No stored
// metadata is needed, and nothing in the argument depends on the data being
// valid UTF-8 or on 0xFF bytes being independent of one another -- only on the
// run being maximal. Walking a run is handled in one step, so a reverse scan
// stays linear in the bytes it touches even for a row that is entirely 0xFF.
//
// parse_units_forward and parse_units_reverse exist so that this claim is
// *tested* rather than merely argued: tests/test_unit.cpp compares them
// exhaustively over every byte string up to length 7 drawn from an alphabet
// built around 0xFF, and over random rows from the real columns.

enum class UnitKind { Symbol, Escape };

struct Unit {
   UnitKind kind;
   u8 value; // Symbol: the code. Escape: the literal byte it carries.
   bool operator==(const Unit& o) const { return kind == o.kind && value == o.value; }
};

/// Left-to-right parse. Throws on a dangling escape marker.
std::vector<Unit> parse_units_forward(const u8* p, size_t n);

/// Right-to-left parse, using no stored metadata. Returns the units in stream
/// order so the two can be compared directly. Throws on a malformed row.
std::vector<Unit> parse_units_reverse(const u8* p, size_t n);

} // namespace fsstlike
