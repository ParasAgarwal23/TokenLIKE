#!/usr/bin/env python3
"""Encode a raw string column into uint16 token ids.

Reads the raw byte representation produced by fetch_clickbench.py / gen_tpch.py
and writes the compressed token representation:

    <prefix>.raw     uint8[total_bytes]     (input)
    <prefix>.rawoff  uint64[rows + 1]       (input)
    <prefix>.tok     uint16[total_tokens]   (output)
    <prefix>.off     uint64[rows + 1]       (output)
    <prefix>.meta.json                      (output, updated in place)

The algorithm is greedy longest-match over the frozen vocabulary, matching
SGTT's StandardEncoder::encode: at each position take the longest token that
matches, falling back to the single-byte token. Because the patched table
contains all 256 single-byte tokens, encoding never dead-ends.

Taking the raw binaries as input (rather than the parquet) keeps this script
dependency-free and makes raw/token row alignment structural: both
representations are indexed by the same row ids.

Usage:
    python scripts/encode/encode.py data/clickbench/URL
    python scripts/encode/encode.py data/clickbench/URL --vocab data/vocab.bin
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import time
from array import array

TOKEN_END = -1  # trie key holding the token id of the node's own prefix


def load_vocab(path: str) -> list[bytes]:
    """Read vocab.bin into a rank-ordered list of token byte strings."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 8:
        raise ValueError(f"{path}: too small to hold a header")
    (n,) = struct.unpack_from("<I", blob, 0)
    if n == 0 or n > 65536:
        raise ValueError(f"{path}: n_tokens={n} outside 1..65536")
    base = 4 + 4 * (n + 1)
    if len(blob) < base:
        raise ValueError(f"{path}: truncated offset array")
    offsets = struct.unpack_from(f"<{n + 1}I", blob, 4)
    if offsets[0] != 0:
        raise ValueError(f"{path}: offsets[0]={offsets[0]}, must be 0")
    if offsets[n] != len(blob) - base:
        raise ValueError(
            f"{path}: terminal offset {offsets[n]} != payload length {len(blob) - base}"
        )
    tokens = []
    for i in range(n):
        if offsets[i] > offsets[i + 1]:
            raise ValueError(f"{path}: offsets not monotonic at {i}")
        tokens.append(blob[base + offsets[i] : base + offsets[i + 1]])
    return tokens


def build_trie(tokens: list[bytes]) -> tuple[dict, int]:
    """Build a byte-keyed trie for longest-match lookup.

    Walking a trie keeps the inner loop on integer dict keys, avoiding a bytes
    slice allocation per probe. Returns the root and the longest token length.
    """
    root: dict = {}
    max_len = 0
    for tid, token in enumerate(tokens):
        node = root
        for byte in token:
            node = node.setdefault(byte, {})
        # A duplicate byte string would silently shadow a lower rank; the
        # vocabulary builder rejects duplicates, so this is a safety net.
        if TOKEN_END in node:
            raise ValueError(f"duplicate token bytes for ids {node[TOKEN_END]} and {tid}")
        node[TOKEN_END] = tid
        max_len = max(max_len, len(token))

    for byte in range(256):
        if byte not in root or TOKEN_END not in root[byte]:
            raise ValueError(f"vocabulary lacks a single-byte token for 0x{byte:02x}")
    return root, max_len


def encode_value(buf: memoryview, start: int, end: int, root: dict) -> array:
    """Greedy longest-match encode buf[start:end] into token ids."""
    out = array("H")
    append = out.append
    i = start
    while i < end:
        node = root
        best_id = -1
        best_end = i
        j = i
        # Walk as deep as the trie allows, remembering the last token seen.
        while j < end:
            node = node.get(buf[j])
            if node is None:
                break
            j += 1
            tid = node.get(TOKEN_END)
            if tid is not None:
                best_id = tid
                best_end = j
        if best_id < 0:
            # Unreachable: all 256 single bytes are in the vocabulary.
            raise AssertionError(f"no token matches byte 0x{buf[i]:02x} at offset {i}")
        append(best_id)
        i = best_end
    return out


def read_offsets(path: str, label: str) -> array:
    """Read a uint64 offset file and validate its structure."""
    size = os.path.getsize(path)
    if size % 8 != 0:
        raise ValueError(f"{path}: size {size} is not a multiple of 8")
    if size < 8:
        raise ValueError(f"{path}: must hold at least one offset")
    offsets = array("Q")
    with open(path, "rb") as f:
        offsets.frombytes(f.read())
    if sys.byteorder != "little":
        offsets.byteswap()
    if offsets[0] != 0:
        raise ValueError(f"{path}: {label}[0]={offsets[0]}, must be 0")
    for i in range(len(offsets) - 1):
        if offsets[i] > offsets[i + 1]:
            raise ValueError(f"{path}: {label} not monotonic at index {i}")
    return offsets


def write_array(path: str, arr: array) -> None:
    """Write an array little-endian, atomically."""
    tmp = path + ".tmp"
    if sys.byteorder != "little":
        arr = arr[:]
        arr.byteswap()
    with open(tmp, "wb") as f:
        arr.tofile(f)
    os.replace(tmp, path)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def encode_column(prefix: str, vocab_path: str, progress_every: int) -> dict:
    raw_path, rawoff_path = prefix + ".raw", prefix + ".rawoff"
    for path in (raw_path, rawoff_path, vocab_path):
        if not os.path.exists(path):
            raise FileNotFoundError(path)

    tokens = load_vocab(vocab_path)
    root, max_token_len = build_trie(tokens)

    rawoff = read_offsets(rawoff_path, "rawoff")
    rows = len(rawoff) - 1
    raw_size = os.path.getsize(raw_path)
    if rawoff[rows] != raw_size:
        raise ValueError(
            f"{rawoff_path}: terminal offset {rawoff[rows]} != {raw_path} size {raw_size}"
        )

    with open(raw_path, "rb") as f:
        raw = memoryview(f.read())

    tok = array("H")
    off = array("Q", [0])
    # Distinct values repeat heavily in URL columns; encoding each distinct
    # value once and replaying the result is a large win and cannot change the
    # output, since encoding is a pure function of the bytes.
    cache: dict[bytes, bytes] = {}
    hits = 0
    started = time.monotonic()

    for row in range(rows):
        start, end = rawoff[row], rawoff[row + 1]
        key = bytes(raw[start:end])
        cached = cache.get(key)
        if cached is None:
            cached = encode_value(raw, start, end, root).tobytes()
            cache[key] = cached
        else:
            hits += 1
        tok.frombytes(cached)
        off.append(len(tok))
        if progress_every and row and row % progress_every == 0:
            rate = row / max(time.monotonic() - started, 1e-9)
            print(f"  {row}/{rows} rows ({rate:,.0f} rows/s)", flush=True)

    elapsed = time.monotonic() - started
    # tobytes()/frombytes() both use native order, so `tok` is in native order
    # here; write_array() converts to little-endian on output.
    if off[rows] != len(tok):
        raise AssertionError("token offsets disagree with token count")

    write_array(prefix + ".tok", tok)
    write_array(prefix + ".off", off)

    total_tokens = len(tok)
    stats = {
        "rows": rows,
        "raw_bytes": raw_size,
        "total_tokens": total_tokens,
        "distinct_values": len(cache),
        "cache_hit_rate": round(hits / rows, 6) if rows else 0.0,
        "bytes_per_token": round(raw_size / total_tokens, 4) if total_tokens else 0.0,
        "compression_ratio": round(raw_size / (2 * total_tokens), 4) if total_tokens else 0.0,
        "max_token_len": max_token_len,
        "encode_seconds": round(elapsed, 3),
        "encode_rows_per_sec": round(rows / elapsed) if elapsed > 0 else 0,
        "tok_sha256": sha256_file(prefix + ".tok"),
        "off_sha256": sha256_file(prefix + ".off"),
        "vocab": vocab_path,
        "vocab_sha256": sha256_file(vocab_path),
        "n_vocab_tokens": len(tokens),
    }
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("prefix", help="column prefix, e.g. data/clickbench/URL")
    ap.add_argument("--vocab", default="data/vocab.bin")
    ap.add_argument(
        "--progress-every",
        type=int,
        default=250_000,
        help="print progress every N rows (0 to disable)",
    )
    args = ap.parse_args()

    stats = encode_column(args.prefix, args.vocab, args.progress_every)

    meta_path = args.prefix + ".meta.json"
    meta = {}
    if os.path.exists(meta_path):
        with open(meta_path) as f:
            meta = json.load(f)
    meta["encoding"] = stats
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")

    print(f"encoded {args.prefix}")
    print(
        f"  rows={stats['rows']:,} raw_bytes={stats['raw_bytes']:,} "
        f"tokens={stats['total_tokens']:,}"
    )
    print(
        f"  distinct={stats['distinct_values']:,} "
        f"cache_hits={stats['cache_hit_rate']:.1%} "
        f"in {stats['encode_seconds']}s ({stats['encode_rows_per_sec']:,} rows/s)"
    )
    print(
        f"  bytes/token={stats['bytes_per_token']} "
        f"compression_ratio={stats['compression_ratio']}x (vs 2 bytes per token id)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
