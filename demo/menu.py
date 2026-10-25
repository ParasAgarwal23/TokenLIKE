"""One entry point for every TokenLIKE demonstration.

    python demo/menu.py            # list the demos and what each needs
    python demo/menu.py 1          # run demo 1
    python demo/menu.py 1 --print  # only print its command

Each demo is an ordinary command that can also be run directly (see README).
Streamlit demos bind to 127.0.0.1 only (also set in .streamlit/config.toml).
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
PY = sys.executable
STREAMLIT = [PY, "-m", "streamlit", "run", "demo/app.py", "--server.address", "127.0.0.1"]
# One FSST build directory for both the prerequisite check and the demo's own lookup
# (integration/fsst/scripts/common.py never searches build-fsst unless told to).
FSST_BUILD = ROOT / os.environ.get("FSSTLIKE_BUILD", "build-fsst")

# (title, platform verified on, command, files that must exist)
DEMOS = [
    ("TokenLIKE replay: saved, verified benchmark results (Streamlit)", "Windows",
     STREAMLIT + ["--", "--replay", "results/final.json"], ["results/final.json"]),
    ("TokenLIKE live: run token / decode / raw now and verify against DuckDB (Streamlit)", "Windows",
     STREAMLIT, [f"build/tokenlike{EXE}", "data/vocab.bin", "data/clickbench/URL.tok"]),
    ("TokenLIKE CLI: one query, JSON report", "Windows",
     [f"build/tokenlike{EXE}", "--mode", "token", "--pattern", "%.php", "--col", "data/clickbench/URL",
      "--runs", "5", "--pin-cpu", "2"], [f"build/tokenlike{EXE}", "data/clickbench/URL.tok"]),
    ("DuckDB SQL: token_like() inside DuckDB queries (integration/duckdb)", "Windows",
     [PY, "-X", "utf8", "integration/duckdb/python/demo_sql.py"],
     ["build/duckdb/tokenlike.duckdb_extension", "data/duckdb/tokenlike.duckdb"]),
    ("DuckDB SQL: the plain-SQL judge script", "Windows",
     [PY, "-X", "utf8", "integration/duckdb/python/run_sql.py", "integration/duckdb/examples/judge_demo.sql"],
     ["build/duckdb/tokenlike.duckdb_extension", "data/duckdb/tokenlike.duckdb"]),
    ("FSST-LIKE: LIKE on FSST-compressed strings (integration/fsst)", "macOS (Person B), Windows (Person C)",
     [PY, "integration/fsst/demo_fsst.py", "--rows", "100000", "--tokenlike", str(ROOT)],
     [str(FSST_BUILD / f"fsst_like{EXE}"), str(FSST_BUILD / f"fsst_encode{EXE}"), "data/clickbench/URL.raw"]),
]


def missing(files: list[str]) -> list[str]:
    return [f for f in files if not (ROOT / f).exists()]


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--print"]
    only_print = "--print" in sys.argv[1:]
    if not args:
        print(__doc__.strip().splitlines()[0] + "\n")
        for i, (title, plat, cmd, need) in enumerate(DEMOS, 1):
            gone = missing(need)
            state = "ready" if not gone else "needs " + ", ".join(gone)
            print(f"{i}. {title}\n   verified on: {plat}; {state}\n   $ {' '.join(cmd)}\n")
        print("Build / data steps for anything marked 'needs': README.md, section 'Demonstrations'.")
        return 0
    i = int(args[0])
    if not 1 <= i <= len(DEMOS):
        sys.exit(f"no demo {i}; choose 1..{len(DEMOS)}")
    title, _, cmd, need = DEMOS[i - 1]
    print(f"$ {' '.join(cmd)}")
    if only_print:
        return 0
    gone = missing(need)
    if gone:
        sys.exit(f"cannot run '{title}': missing {', '.join(gone)} (see README 'Demonstrations')")
    if not Path(cmd[0]).is_absolute():  # Windows cannot launch a relative path with '/'
        cmd = [str(ROOT / cmd[0])] + cmd[1:]
    env = dict(os.environ, MSYS_NO_PATHCONV="1", TOKENLIKE_DIR=str(ROOT), FSSTLIKE_BUILD=str(FSST_BUILD))
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
