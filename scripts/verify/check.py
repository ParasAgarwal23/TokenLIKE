#!/usr/bin/env python3
"""Compare Person C's CLI results against DuckDB ground truth.

For every (pattern x mode) pair: ensure the DuckDB truth exists, run the CLI,
read the row ids it emitted, and compare the two sets exactly. Any difference
is reported with enough context to find the bug -- the row id, the raw string,
the string decoded from the token ids, and the token ids with their textual
expansion -- because that is what localizing a matcher bug actually needs.

Exits non-zero if any pattern is wrong in any mode.

Usage:
    python scripts/verify/check.py patterns/core.txt --modes token,decode,raw
    python scripts/verify/check.py patterns/core.txt --cli build/tokenlike
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "encode"))
from common import (  # noqa: E402
    Pattern,
    parse_pattern_file,
    read_ids,
    read_raw_values,
    read_token_rows,
)
from ground_truth import connect, ensure_truth  # noqa: E402

DEFAULT_MODES = "token,decode,raw"
VALID_MODES = ("token", "decode", "raw")
MAX_REPORTED = 5


def run_cli(cli: str, pat: Pattern, mode: str, vocab: str, out_path: str, timeout: int):
    """Invoke the CLI for one (pattern, mode). Returns (ok, detail)."""
    argv = [
        cli,
        "--mode", mode,
        "--column", pat.column,
        "--vocab", vocab,
        "--pattern", pat.pattern,
        "--out", out_path,
    ]
    if pat.is_concat:
        argv += ["--column2", pat.column2]

    try:
        proc = subprocess.run(
            argv, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return False, f"CLI timed out after {timeout}s"
    except OSError as e:
        return False, f"cannot execute {cli}: {e}"

    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail = detail[-1] if detail else "no output"
        return False, f"CLI exited {proc.returncode}: {tail}"
    if not os.path.exists(out_path):
        return False, f"CLI exited 0 but wrote no output file ({out_path})"
    return True, ""


def describe_rows(pat: Pattern, row_ids, vocab_tokens, label: str) -> None:
    """Print the evidence needed to localize a mismatch."""
    if not row_ids:
        return
    raws = read_raw_values(pat.column)
    toks = read_token_rows(pat.column)
    raws2 = read_raw_values(pat.column2) if pat.is_concat else None

    print(f"    first {min(len(row_ids), MAX_REPORTED)} {label}:")
    for row_id in row_ids[:MAX_REPORTED]:
        raw = raws[row_id]
        decoded = b"".join(vocab_tokens[t] for t in toks[row_id])
        print(f"      row {row_id}")
        print(f"        raw     : {raw[:160]!r}")
        if decoded != raw:
            print(f"        decoded : {decoded[:160]!r}   <-- DISAGREES WITH RAW")
        if raws2 is not None:
            print(f"        column2 : {raws2[row_id][:160]!r}")
        ids = toks[row_id]
        print(f"        tokens  : {ids[:20]}")
        print(
            "        expand  : "
            + " | ".join(
                vocab_tokens[t].decode("utf-8", "replace") for t in ids[:20]
            )
        )


def check_pattern(con, pat: Pattern, modes, cli: str, vocab: str, force: bool, timeout: int, tmpdir: str, vocab_tokens):
    """Verify one pattern across all requested modes.

    Returns a dict of mode -> ('pass' | 'fail' | 'skip', detail).
    """
    expected_count, _ = ensure_truth(con, pat, force)
    expected = set(read_ids(pat.truth_path))
    results = {}

    for mode in modes:
        out_path = os.path.join(tmpdir, f"{pat.name}.{mode}.ids")
        ok, detail = run_cli(cli, pat, mode, vocab, out_path, timeout)
        if not ok:
            results[mode] = ("skip", detail)
            continue

        actual = set(read_ids(out_path))
        missing = sorted(expected - actual)   # truth says match, CLI did not
        extra = sorted(actual - expected)     # CLI says match, truth did not

        if not missing and not extra:
            results[mode] = ("pass", f"{len(actual):,} matches")
            continue

        detail = (
            f"{len(missing):,} false negatives, {len(extra):,} false positives "
            f"(expected {expected_count:,}, got {len(actual):,})"
        )
        results[mode] = ("fail", detail)
        print(f"  MISMATCH {pat.name} mode={mode}: {detail}")
        describe_rows(pat, missing, vocab_tokens, "false negatives (truth matched, CLI did not)")
        describe_rows(pat, extra, vocab_tokens, "false positives (CLI matched, truth did not)")

    return expected_count, results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pattern_file", help="e.g. patterns/core.txt")
    ap.add_argument("--modes", default=DEFAULT_MODES, help=f"comma-separated subset of {DEFAULT_MODES}")
    ap.add_argument("--cli", default=os.environ.get("TOKENLIKE_CLI", "build/tokenlike"))
    ap.add_argument("--vocab", default="data/vocab.bin")
    ap.add_argument("--force", action="store_true", help="regenerate ground truth")
    ap.add_argument("--timeout", type=int, default=300, help="per-invocation CLI timeout")
    args = ap.parse_args()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    bad = [m for m in modes if m not in VALID_MODES]
    if bad:
        ap.error(f"unknown mode(s) {bad}; valid modes are {list(VALID_MODES)}")

    patterns = parse_pattern_file(args.pattern_file)
    sys.path.insert(0, "scripts/encode")
    from encode import load_vocab  # noqa: E402

    vocab_tokens = load_vocab(args.vocab)
    con = connect()

    print(f"checking {len(patterns)} pattern(s) x {len(modes)} mode(s) from {args.pattern_file}")
    print(f"  cli    : {args.cli}" + ("" if os.path.exists(args.cli) else "   (NOT FOUND)"))
    print(f"  oracle : duckdb against the canonical parquet")
    print()

    tally = {"pass": 0, "fail": 0, "skip": 0}
    failed_patterns = []
    skip_reasons = {}
    started = time.monotonic()

    with tempfile.TemporaryDirectory(prefix="tokenlike-check-") as tmpdir:
        for pat in patterns:
            expected_count, results = check_pattern(
                con, pat, modes, args.cli, args.vocab, args.force,
                args.timeout, tmpdir, vocab_tokens,
            )
            cells = []
            for mode in modes:
                status, detail = results[mode]
                tally[status] += 1
                cells.append(f"{mode}={status.upper()}")
                if status == "fail" and pat.name not in failed_patterns:
                    failed_patterns.append(pat.name)
                if status == "skip":
                    skip_reasons.setdefault(detail, 0)
                    skip_reasons[detail] += 1
            print(f"  {pat.name:<24} {expected_count:>9,} expected   " + "  ".join(cells))

    con.close()
    elapsed = time.monotonic() - started

    print()
    print(f"{tally['pass']} passed, {tally['fail']} failed, {tally['skip']} skipped "
          f"in {elapsed:.1f}s")
    for reason, count in skip_reasons.items():
        print(f"  skipped x{count}: {reason}")

    if tally["fail"]:
        print(f"FAILED patterns: {', '.join(failed_patterns)}", file=sys.stderr)
        print(f"Total mismatching (pattern, mode) pairs: {tally['fail']}", file=sys.stderr)
        return 1
    if tally["pass"] == 0:
        print("No pattern was actually verified; nothing passed.", file=sys.stderr)
        return 2
    print("All patterns passed.")
    print("Total mismatches: 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
