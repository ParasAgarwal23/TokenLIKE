#!/usr/bin/env python3
"""Verify that decoding a token column reproduces the original bytes exactly.

For every row: read its token ids, concatenate the vocabulary entries, and
compare against the row's raw bytes. The success criterion is zero differing
rows. A column that fails here is unusable, because every downstream
correctness result would be measured against corrupt data.

This also re-validates the column's structural invariants, independently of
the C++ loader, so a format bug is caught on both sides.

Exit status is 0 only if every row matches.

Usage:
    python scripts/encode/roundtrip.py data/sample/urls
    python scripts/encode/roundtrip.py data/clickbench/URL --vocab data/vocab.bin
"""

import argparse
import json
import os
import sys
from array import array

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from encode import load_vocab  # noqa: E402  (sibling script, same owner)

MAX_REPORTED = 5


def read_u64(path: str, label: str) -> array:
    size = os.path.getsize(path)
    if size % 8 != 0:
        raise ValueError(f"{path}: size {size} is not a multiple of 8")
    arr = array("Q")
    with open(path, "rb") as f:
        arr.frombytes(f.read())
    if sys.byteorder != "little":
        arr.byteswap()
    if not arr or arr[0] != 0:
        raise ValueError(f"{path}: {label}[0] must be 0")
    return arr


def read_u16(path: str) -> array:
    size = os.path.getsize(path)
    if size % 2 != 0:
        raise ValueError(f"{path}: size {size} is not a multiple of 2")
    arr = array("H")
    with open(path, "rb") as f:
        arr.frombytes(f.read())
    if sys.byteorder != "little":
        arr.byteswap()
    return arr


def check_structure(prefix: str, tok, off, rawoff, raw_size, n_vocab) -> list[str]:
    """Structural invariants, checked before any decoding is attempted."""
    errors = []
    rows = len(off) - 1
    if len(rawoff) - 1 != rows:
        errors.append(
            f"row count disagreement: .off implies {rows}, .rawoff implies {len(rawoff) - 1}"
        )
    for i in range(len(off) - 1):
        if off[i] > off[i + 1]:
            errors.append(f".off not monotonic at index {i}: {off[i]} > {off[i + 1]}")
            break
    for i in range(len(rawoff) - 1):
        if rawoff[i] > rawoff[i + 1]:
            errors.append(f".rawoff not monotonic at index {i}")
            break
    if off[-1] != len(tok):
        errors.append(f"terminal token offset {off[-1]} != token count {len(tok)}")
    if rawoff[-1] != raw_size:
        errors.append(f"terminal raw offset {rawoff[-1]} != .raw size {raw_size}")
    bad = [t for t in tok[: min(len(tok), 1 << 20)] if t >= n_vocab]
    if bad:
        errors.append(f"token id {bad[0]} is outside the vocabulary (n={n_vocab})")
    return errors


def roundtrip(prefix: str, vocab_path: str, verbose: bool) -> tuple[int, int]:
    """Return (rows_checked, mismatches)."""
    tokens = load_vocab(vocab_path)
    tok = read_u16(prefix + ".tok")
    off = read_u64(prefix + ".off", "off")
    rawoff = read_u64(prefix + ".rawoff", "rawoff")
    raw_size = os.path.getsize(prefix + ".raw")
    with open(prefix + ".raw", "rb") as f:
        raw = f.read()

    errors = check_structure(prefix, tok, off, rawoff, raw_size, len(tokens))
    if errors:
        for e in errors:
            print(f"STRUCTURE FAIL: {e}", file=sys.stderr)
        return 0, len(errors)

    rows = len(off) - 1
    mismatches = 0
    for row in range(rows):
        decoded = b"".join(tokens[t] for t in tok[off[row] : off[row + 1]])
        expected = raw[rawoff[row] : rawoff[row + 1]]
        if decoded != expected:
            mismatches += 1
            if mismatches <= MAX_REPORTED:
                print(f"MISMATCH row {row}", file=sys.stderr)
                print(f"  expected ({len(expected)}B): {expected[:120]!r}", file=sys.stderr)
                print(f"  decoded  ({len(decoded)}B): {decoded[:120]!r}", file=sys.stderr)
                ids = list(tok[off[row] : off[row + 1]])
                print(f"  token ids: {ids[:24]}", file=sys.stderr)
                print(
                    f"  tokens: {[tokens[t] for t in ids[:24]]}",
                    file=sys.stderr,
                )
        elif verbose and row < 3:
            print(f"  row {row}: {len(expected)}B <- {off[row + 1] - off[row]} tokens OK")

    return rows, mismatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("prefix", help="column prefix, e.g. data/clickbench/URL")
    ap.add_argument("--vocab", default="data/vocab.bin")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    rows, mismatches = roundtrip(args.prefix, args.vocab, args.verbose)

    print(f"round-trip {args.prefix}: {rows:,} rows checked, {mismatches} mismatches")
    if mismatches:
        print("ROUND-TRIP FAILED", file=sys.stderr)
        return 1

    # Record the verdict next to the data so nobody has to take it on trust.
    meta_path = args.prefix + ".meta.json"
    if os.path.exists(meta_path):
        with open(meta_path) as f:
            meta = json.load(f)
        meta["roundtrip"] = {"rows_checked": rows, "mismatches": 0, "status": "pass"}
        with open(meta_path, "w") as f:
            json.dump(meta, f, indent=2, sort_keys=True)
            f.write("\n")
    print("ROUND-TRIP OK (zero differences)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
