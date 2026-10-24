#include "tokenlike/dfa.h"

namespace tokenlike {

namespace {

// Runs tokens [b, e) of `tok` from state s, in the given direction.
// Returns the reached state; `read` is incremented by the number of tokens looked up.
template <class Table, bool Early, bool Backward>
inline State run(const Table& t, const uint8_t* flags, State s, const uint16_t* tok, uint64_t b, uint64_t e,
                 uint64_t& read) {
    if (Early && (flags[s] & kStop)) return s;
    if (!Backward) {
        uint64_t i = b;
        while (i < e) {
            s = t.next(s, tok[i++]);
            if (Early && (flags[s] & kStop)) break;
        }
        read += i - b;
    } else {
        uint64_t i = e;
        while (i > b) {
            s = t.next(s, tok[--i]);
            if (Early && (flags[s] & kStop)) break;
        }
        read += e - i;
    }
    return s;
}

template <class Table, bool Early, bool Backward, bool Concat>
void scan_impl(const Table& t, const ScanSpec& sp, const Column& a, const Column* bc, Bitmap& out, Stats& st) {
    const uint64_t rows = a.rows;
    out.assign((rows + 7) / 8, 0);
    const uint16_t* ta = a.tok.data();
    const uint64_t* oa = a.off.data();
    const uint16_t* tb = Concat ? bc->tok.data() : nullptr;
    const uint64_t* ob = Concat ? bc->off.data() : nullptr;
    uint64_t read = 0, matches = 0;
    for (uint64_t r = 0; r < rows; ++r) {
        State s = sp.start;
        if (!Concat) {
            s = run<Table, Early, Backward>(t, sp.flags, s, ta, oa[r], oa[r + 1], read);
        } else if (!Backward) {
            s = run<Table, Early, false>(t, sp.flags, s, ta, oa[r], oa[r + 1], read);
            s = run<Table, Early, false>(t, sp.flags, s, tb, ob[r], ob[r + 1], read);
        } else {
            s = run<Table, Early, true>(t, sp.flags, s, tb, ob[r], ob[r + 1], read);
            s = run<Table, Early, true>(t, sp.flags, s, ta, oa[r], oa[r + 1], read);
        }
        uint8_t hit = sp.flags[s] & kAccept;
        out[r >> 3] |= static_cast<uint8_t>(hit << (r & 7));
        matches += hit;
    }
    st.rows += rows;
    st.matches += matches;
    st.tokens_read += read;
}

template <class Table>
void dispatch(const Table& t, const ScanSpec& sp, const Column& a, const Column* b, Bitmap& out, Stats& st) {
    const bool bw = sp.dir == Dir::Backward;
    if (sp.early_exit) {
        if (b) bw ? scan_impl<Table, true, true, true>(t, sp, a, b, out, st)
               : scan_impl<Table, true, false, true>(t, sp, a, b, out, st);
        else bw ? scan_impl<Table, true, true, false>(t, sp, a, b, out, st)
                : scan_impl<Table, true, false, false>(t, sp, a, b, out, st);
    } else {
        if (b) bw ? scan_impl<Table, false, true, true>(t, sp, a, b, out, st)
               : scan_impl<Table, false, false, true>(t, sp, a, b, out, st);
        else bw ? scan_impl<Table, false, true, false>(t, sp, a, b, out, st)
                : scan_impl<Table, false, false, false>(t, sp, a, b, out, st);
    }
}

}  // namespace

void scan(const CompactTable& t, const ScanSpec& sp, const Column& a, const Column* b, Bitmap& out, Stats& st) {
    dispatch(t, sp, a, b, out, st);
}

void scan(const DenseTable& t, const ScanSpec& sp, const Column& a, const Column* b, Bitmap& out, Stats& st) {
    dispatch(t, sp, a, b, out, st);
}

}  // namespace tokenlike
