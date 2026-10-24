#!/usr/bin/env python3
"""Run TokenLIKE and both baselines over a pattern list; record reproducible results.

    python scripts/bench/run_bench.py patterns/core.txt --runs 5 --out results/final.json

For every pattern each mode (token, decode, raw) runs in its own CLI process.
The mode order rotates per pattern to spread thermal/turbo effects. Results
are cross-checked (identical match counts and row-id files across modes) and,
when available, verified against DuckDB with Person B's checker.
Only the standard library is used.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODES = ["token", "decode", "raw"]
DEFAULT_CHECKER = "scripts/verify/check.py"
EXIT_UNAVAILABLE = 2


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def read_patterns(path: Path) -> list[str]:
    pats = []
    for line in path.read_bytes().decode("utf-8").split("\n"):
        line = line.rstrip("\r")
        if line and not line.startswith("#"):
            pats.append(line)
    return pats


def find_cli(build_dir: Path) -> Path:
    for name in ("tokenlike.exe", "tokenlike"):
        p = build_dir / name
        if p.is_file():
            return p
    sys.exit(f"error: CLI not found in {build_dir}; build it first (see README)")


def git(*args: str) -> str | None:
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def cpu_name() -> str:
    if sys.platform == "win32":
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                 r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        except OSError:
            pass
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def cmake_cache(build_dir: Path) -> dict:
    out = {}
    try:
        for line in (build_dir / "CMakeCache.txt").read_text(errors="replace").splitlines():
            for key in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_BUILD_TYPE",
                        "CMAKE_CXX_FLAGS_RELEASE"):
                if line.startswith(key + ":"):
                    out[key] = line.split("=", 1)[1]
    except OSError:
        pass
    comp = out.get("CMAKE_CXX_COMPILER")
    if comp:
        try:
            out["compiler_version"] = subprocess.run(
                [comp, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
        except (OSError, IndexError):
            pass
    return out


def sha256(path: Path) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def data_fingerprint(col: str, vocab: str) -> dict:
    fp = {}
    for suffix in (".meta.json", ".tok", ".off", ".raw", ".rawoff"):
        p = ROOT / (col + suffix)
        fp[suffix] = {"bytes": p.stat().st_size if p.exists() else None}
    fp[".meta.json"]["sha256"] = sha256(ROOT / (col + ".meta.json"))
    fp["vocab"] = {"path": vocab, "sha256": sha256(ROOT / vocab)}
    return fp


def stale_binary(cli: Path) -> bool:
    newest = max((p.stat().st_mtime for d in ("src", "include") for p in (ROOT / d).rglob("*")
                  if p.is_file()), default=0)
    return cli.stat().st_mtime < newest


def run_cli(cli: Path, args, mode: str, pattern: str, rowids: Path) -> dict:
    cmd = [str(cli), "--mode", mode, "--pattern", pattern, "--col", args.col,
           "--vocab", args.vocab, "--runs", str(args.runs), "--warmup", str(args.warmup),
           "--rowids", str(rowids)]
    if args.col2:
        cmd += ["--col2", args.col2]
    if args.pin_cpu >= 0:
        cmd += ["--pin-cpu", str(args.pin_cpu)]
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    stderr = p.stderr.decode("utf-8", "replace").strip()
    if p.returncode == EXIT_UNAVAILABLE:
        return {"status": "unavailable", "stderr": stderr}
    if p.returncode != 0:
        return {"status": "error", "exit": p.returncode, "stderr": stderr}
    lines = [l for l in p.stdout.decode("utf-8").splitlines() if l.strip()]
    if len(lines) != 1:
        return {"status": "error", "stderr": f"expected 1 JSON line, got {len(lines)}"}
    res = json.loads(lines[0])
    res["status"] = "ok"
    res["rowids_sha256"] = sha256(rowids)
    return res


def run_checker(template: str, args, pattern: str, rowids: Path) -> dict:
    cmd = [tok.format(col=args.col, pattern=pattern, rowids=str(rowids), vocab=args.vocab)
           for tok in shlex.split(template, posix=True)]
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=args.timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"verified": "error", "detail": str(e)}
    out = p.stdout.decode("utf-8", "replace").strip()
    return {"verified": "pass" if p.returncode == 0 else "fail", "exit": p.returncode,
            "detail": out[-2000:]}


def fmt(x, spec=".2f") -> str:
    return "-" if x is None else format(x, spec)


def print_table(results: list[dict]) -> None:
    hdr = ["pattern", "matches", "token ns/r", "decode ns/r", "raw ns/r", "dec/tok",
           "tok/row T", "tok/row D", "table KB", "dense KB", "agree", "verified"]
    rows = []
    for r in results:
        m = r["modes"]
        ns = {k: m.get(k, {}).get("ns_per_row") for k in MODES}
        speed = ns["decode"] / ns["token"] if ns["token"] and ns["decode"] else None
        anyok = next((v for v in m.values() if v.get("status") == "ok"), {})
        rows.append([r["pattern"], str(anyok.get("matches", "-")), fmt(ns["token"]),
                     fmt(ns["decode"]), fmt(ns["raw"]), fmt(speed, ".2f") + ("x" if speed else ""),
                     fmt(m.get("token", {}).get("tokens_per_row")),
                     fmt(m.get("decode", {}).get("tokens_per_row")),
                     fmt(m.get("token", {}).get("table_kb"), ".1f"),
                     fmt(m.get("token", {}).get("dense_table_kb"), ".1f"),
                     "yes" if r["agree"] else "NO", r["verified"]])
    widths = [max(len(h), *(len(row[i]) for row in rows)) for i, h in enumerate(hdr)]
    line = lambda cells: "  ".join(c.ljust(w) for c, w in zip(cells, widths))
    print(line(hdr))
    print(line(["-" * w for w in widths]))
    for row in rows:
        print(line(row))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):  # Windows consoles default to cp1252
        stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("patterns", type=Path)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--out", type=Path, default=Path("results/final.json"))
    ap.add_argument("--col", default="data/clickbench/url")
    ap.add_argument("--col2", default=None)
    ap.add_argument("--vocab", default="data/vocab.bin")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--modes", default=",".join(MODES))
    ap.add_argument("--rowids-dir", type=Path, default=Path("results/rowids"))
    ap.add_argument("--checker", default=None,
                    help="checker command template with {col} {pattern} {rowids} {vocab}; "
                         f"default: python {DEFAULT_CHECKER} ... if that file exists")
    ap.add_argument("--no-check", action="store_true", help="skip DuckDB verification")
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--pin-cpu", type=int, default=0,
                    help="logical CPU to pin every run to (-1 = no pinning)")
    args = ap.parse_args()

    if args.runs < 5:
        log(f"warning: --runs {args.runs} < 5; not acceptable for final results")
    modes = [m for m in args.modes.split(",") if m]
    cli = find_cli(args.build)
    patterns = read_patterns(args.patterns)

    status = git("status", "--porcelain", "--untracked-files=no")
    dirty = bool(status) if status is not None else None
    if dirty and not args.allow_dirty:
        sys.exit("error: working tree has uncommitted changes; commit or pass --allow-dirty")
    if stale_binary(cli):
        log("warning: CLI binary is older than sources; rebuild before final measurements")

    checker = args.checker
    if checker is None and not args.no_check and (ROOT / DEFAULT_CHECKER).exists():
        checker = (f'"{sys.executable}" {DEFAULT_CHECKER} '
                   "--col {col} --pattern {pattern} --rowids {rowids}")
    if checker is None and not args.no_check:
        log(f"warning: no checker ({DEFAULT_CHECKER} missing); results will be 'not_run'")

    meta = {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "git_commit": git("rev-parse", "HEAD"),
        "git_branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "git_dirty": dirty,
        "host": platform.node(),
        "os": platform.platform(),
        "cpu": cpu_name(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
        "build": cmake_cache(args.build),
        "cli": str(cli.relative_to(ROOT)) if cli.is_relative_to(ROOT) else str(cli),
        "patterns_file": str(args.patterns),
        "patterns_sha256": sha256(args.patterns),
        "col": args.col, "col2": args.col2, "runs": args.runs, "warmup": args.warmup,
        "pin_cpu": args.pin_cpu,
        "data": data_fingerprint(args.col, args.vocab),
        "checker": checker,
    }

    rowdir = ROOT / args.rowids_dir
    results = []
    for i, pat in enumerate(patterns):
        order = modes[i % len(modes):] + modes[:i % len(modes)]
        log(f"[{i + 1}/{len(patterns)}] {pat!r}  order={order}")
        res = {"pattern": pat, "modes": {}}
        for mode in order:
            res["modes"][mode] = run_cli(cli, args, mode, pat, rowdir / f"{i:03d}_{mode}.bin")
            r = res["modes"][mode]
            log(f"    {mode:6s} {r['status']}" + (f"  matches={r['matches']} "
                f"ns/row={r['ns_per_row']:.3f}" if r["status"] == "ok" else
                f"  {r.get('stderr', '')[-200:]}"))
        ok = {m: r for m, r in res["modes"].items() if r["status"] == "ok"}
        res["agree"] = (len({r["matches"] for r in ok.values()}) <= 1 and
                        len({r["rowids_sha256"] for r in ok.values()}) <= 1)
        res["modes_ok"] = sorted(ok)

        # Verify token output if present, else the first available mode.
        target = "token" if "token" in ok else (sorted(ok)[0] if ok else None)
        if not res["agree"]:
            res["verified"] = "fail"
            res["check"] = {"detail": "modes disagree"}
        elif target is None:
            res["verified"] = "not_run"
        elif checker:
            res["check"] = run_checker(checker, args, pat, rowdir / f"{i:03d}_{target}.bin")
            res["check"]["mode_checked"] = target
            res["verified"] = res["check"]["verified"]
        else:
            res["verified"] = "not_run"
        results.append(res)

    out = ROOT / args.out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"meta": meta, "results": results}, indent=2, ensure_ascii=False)
                   + "\n", encoding="utf-8")

    print_table(results)
    print(f"\nwritten {args.out}  (commit {str(meta['git_commit'])[:10]}, "
          f"dirty={dirty}, cpu={meta['cpu']})")
    bad = [r["pattern"] for r in results if not r["agree"] or r["verified"] in ("fail", "error")]
    if bad:
        print(f"FAILED patterns: {bad}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
