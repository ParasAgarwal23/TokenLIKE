#!/usr/bin/env python3
"""Build the frozen TokenLIKE token vocabulary.

Source of truth is the official patched Claude token table published with
"Accelerating String-Heavy Queries with LLM Token Tables" (SGTT):

    umbra-db/token-vldb2026  ->  tokens/claude_patched.csv

That table holds exactly 65536 tokens, so every token id fits in uint16_t,
and it contains all 256 single-byte tokens, so every byte string is
encodable without an escape mechanism.

Output (little-endian):

    data/vocab.bin        uint32 n | uint32 off[n + 1] | uint8 bytes[]
    data/vocab.meta.json  provenance, hashes, token statistics

Usage:
    python scripts/encode/build_vocab.py
    python scripts/encode/build_vocab.py --out data/vocab.bin
"""

import argparse
import csv
import hashlib
import json
import os
import struct
import sys
import urllib.request

# Pinned upstream revision. Do not float this to HEAD: the vocabulary is
# frozen for the whole experiment and every encoded column references it.
UPSTREAM_REPO = "umbra-db/token-vldb2026"
UPSTREAM_COMMIT = "e202e36e2b33a4768d1122f96880b64416234175"
UPSTREAM_PATH = "tokens/claude_patched.csv"
UPSTREAM_URL = f"https://raw.githubusercontent.com/{UPSTREAM_REPO}/{UPSTREAM_COMMIT}/{UPSTREAM_PATH}"

VOCAB_ID = "claude_patched"
MAX_TOKENS = 65536

# The CSV declares a max token length of 12; assert rather than assume, since
# the matcher's lifted tables and the encoder's probe loop both depend on it.
EXPECTED_MAX_TOKEN_LEN = 12


def fetch_csv(cache_path: str) -> bytes:
    """Download the token table, caching the raw bytes next to the output."""
    if os.path.exists(cache_path):
        with open(cache_path, "rb") as f:
            return f.read()

    os.makedirs(os.path.dirname(cache_path) or ".", exist_ok=True)
    print(f"fetching {UPSTREAM_URL}")
    with urllib.request.urlopen(UPSTREAM_URL, timeout=120) as resp:
        if resp.status != 200:
            raise RuntimeError(f"upstream returned HTTP {resp.status}")
        blob = resp.read()
    with open(cache_path, "wb") as f:
        f.write(blob)
    return blob


def parse_tokens(blob: bytes) -> list[bytes]:
    """Parse the token table into a rank-ordered list of token byte strings.

    The authoritative byte source is the hex `token` column. The `text` column
    is a human-readable rendering with its own escaping and is ignored, so no
    unicode-escaping ambiguity can enter the vocabulary.
    """
    reader = csv.reader(blob.decode("utf-8").splitlines())
    header = next(reader)
    expected = ["token", "rank", "orig_rank", "length", "text"]
    if header != expected:
        raise ValueError(f"unexpected CSV header {header!r}, want {expected!r}")

    by_rank: dict[int, bytes] = {}
    for lineno, row in enumerate(reader, start=2):
        if not row:
            continue
        hex_bytes, rank_s, _orig_rank, length_s, _text = row
        rank = int(rank_s)
        token = bytes.fromhex(hex_bytes)
        if len(token) != int(length_s):
            raise ValueError(
                f"line {lineno}: declared length {length_s} != {len(token)} actual bytes"
            )
        if rank in by_rank:
            raise ValueError(f"line {lineno}: duplicate rank {rank}")
        by_rank[rank] = token

    n = len(by_rank)
    if n != MAX_TOKENS:
        raise ValueError(f"expected exactly {MAX_TOKENS} tokens, got {n}")
    missing = [r for r in range(n) if r not in by_rank]
    if missing:
        raise ValueError(f"rank range has {len(missing)} gaps, first at {missing[0]}")

    return [by_rank[r] for r in range(n)]


def validate(tokens: list[bytes]) -> dict:
    """Check the invariants the rest of the pipeline relies on."""
    lengths = [len(t) for t in tokens]
    max_len = max(lengths)
    if max_len != EXPECTED_MAX_TOKEN_LEN:
        raise ValueError(
            f"max token length is {max_len}, expected {EXPECTED_MAX_TOKEN_LEN}; "
            "the encoder probe window and the matcher's lifted tables assume this"
        )
    if min(lengths) < 1:
        raise ValueError("vocabulary contains an empty token")

    # Every byte must be representable as a single token, otherwise greedy
    # longest-match encoding can dead-end on arbitrary input.
    singles = {t[0] for t in tokens if len(t) == 1}
    if len(singles) != 256:
        raise ValueError(f"only {len(singles)}/256 single-byte tokens present")

    if len(set(tokens)) != len(tokens):
        raise ValueError("vocabulary contains duplicate token byte strings")

    histogram: dict[int, int] = {}
    for length in lengths:
        histogram[length] = histogram.get(length, 0) + 1

    return {
        "n_tokens": len(tokens),
        "total_token_bytes": sum(lengths),
        "min_token_len": min(lengths),
        "max_token_len": max_len,
        "single_byte_tokens": len(singles),
        "length_histogram": {str(k): histogram[k] for k in sorted(histogram)},
    }


def write_vocab(tokens: list[bytes], out_path: str) -> dict:
    """Serialize to the frozen vocab.bin format and return file statistics."""
    offsets = [0]
    for token in tokens:
        offsets.append(offsets[-1] + len(token))
    payload = b"".join(tokens)
    if offsets[-1] != len(payload):
        raise AssertionError("offset accumulation disagrees with payload length")

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    tmp_path = out_path + ".tmp"
    with open(tmp_path, "wb") as f:
        f.write(struct.pack("<I", len(tokens)))
        f.write(struct.pack(f"<{len(offsets)}I", *offsets))
        f.write(payload)
    os.replace(tmp_path, out_path)

    size = os.path.getsize(out_path)
    expected_size = 4 + 4 * (len(tokens) + 1) + len(payload)
    if size != expected_size:
        raise AssertionError(f"wrote {size} bytes, expected {expected_size}")

    with open(out_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    return {"vocab_bin_bytes": size, "vocab_bin_sha256": digest}


def verify_readback(out_path: str, tokens: list[bytes]) -> None:
    """Re-read the file the way the C++ loader will and compare every token."""
    with open(out_path, "rb") as f:
        blob = f.read()
    (n,) = struct.unpack_from("<I", blob, 0)
    if n != len(tokens):
        raise AssertionError(f"header says {n} tokens, built {len(tokens)}")
    offsets = struct.unpack_from(f"<{n + 1}I", blob, 4)
    base = 4 + 4 * (n + 1)
    if offsets[0] != 0:
        raise AssertionError("offsets[0] must be 0")
    if offsets[n] != len(blob) - base:
        raise AssertionError("terminal offset does not match payload length")
    for i in range(n):
        if offsets[i] > offsets[i + 1]:
            raise AssertionError(f"offsets not monotonic at {i}")
        if blob[base + offsets[i] : base + offsets[i + 1]] != tokens[i]:
            raise AssertionError(f"token {i} does not round-trip")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="data/vocab.bin", help="output vocab.bin path")
    ap.add_argument(
        "--cache",
        default="data/download/claude_patched.csv",
        help="where to cache the upstream CSV",
    )
    args = ap.parse_args()

    blob = fetch_csv(args.cache)
    csv_sha = hashlib.sha256(blob).hexdigest()
    tokens = parse_tokens(blob)
    stats = validate(tokens)
    stats.update(write_vocab(tokens, args.out))
    verify_readback(args.out, tokens)

    meta = {
        "vocab_id": VOCAB_ID,
        "source": {
            "repo": UPSTREAM_REPO,
            "commit": UPSTREAM_COMMIT,
            "path": UPSTREAM_PATH,
            "url": UPSTREAM_URL,
            "csv_bytes": len(blob),
            "csv_sha256": csv_sha,
        },
        "encoder": {
            "algorithm": "greedy-longest-match",
            "note": (
                "Reimplementation of SGTT StandardEncoder::encode, which probes "
                "token lengths from longest to shortest and falls back to the "
                "single-byte token. No BPE merges, no pre-tokenization regex."
            ),
        },
        "format": "uint32 n | uint32 off[n+1] | uint8 bytes[]  (little-endian)",
        **stats,
    }
    meta_path = os.path.splitext(args.out)[0] + ".meta.json"
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")

    print(f"wrote {args.out} ({stats['vocab_bin_bytes']} bytes)")
    print(f"wrote {meta_path}")
    print(
        f"  tokens={stats['n_tokens']} "
        f"token_bytes={stats['total_token_bytes']} "
        f"max_len={stats['max_token_len']} "
        f"single_byte={stats['single_byte_tokens']}"
    )
    print(f"  sha256={stats['vocab_bin_sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
