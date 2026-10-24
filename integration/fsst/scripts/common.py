"""Shared helpers for the FSST-LIKE experiment's Python tooling.

The DuckDB ground truth is read from the *original* TokenLIKE canonical
parquet files, which this experiment only ever opens read-only. Everything it
writes lands under this experiment's data/ directory.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # integration/fsst
REPO = ROOT.parent.parent                              # the TokenLIKE checkout
DATA = ROOT / "data"
TRUTH = DATA / "truth"


def _find_build() -> Path:
    """The CMake build tree holding fsst_like. $FSSTLIKE_BUILD wins; otherwise
    the usual places, in the order a reader would guess."""
    env = os.environ.get("FSSTLIKE_BUILD")
    cands = [Path(env)] if env else []
    cands += [REPO / "build" / "fsst", REPO / "build", ROOT / "build" / "rel"]
    for c in cands:
        if (c / "fsst_like").is_file() or (c / "fsst_like.exe").is_file():
            return c
    return cands[0] if cands else ROOT


BUILD = _find_build()
CLI = os.environ.get("FSSTLIKE_CLI", str(BUILD / "fsst_like"))
ENCODER = os.environ.get("FSSTLIKE_ENCODER", str(BUILD / "fsst_encode"))

HEX_RE = re.compile(r"^[0-9a-f]*$")


def resolve_cli(path: str = CLI) -> str:
    """Make a CLI path launchable on any platform (Windows needs the .exe and
    does not apply PATHEXT to a path containing a separator)."""
    candidates = [path]
    if os.name == "nt" and not os.path.splitext(path)[1]:
        exts = [e for e in os.environ.get("PATHEXT", ".EXE").split(os.pathsep) if e]
        candidates += [path + e.lower() for e in exts]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.abspath(c)
    import shutil

    return shutil.which(path) or path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass
class ColumnMeta:
    name: str            # e.g. "clickbench/URL_1m"
    prefix: Path         # <exp>/data/clickbench/URL_1m
    source_prefix: str   # original TokenLIKE raw prefix
    parquet: Path        # original canonical parquet for this column
    rows: int
    raw_bytes: int
    cmp_bytes: int
    meta: dict


def load_column(name: str) -> ColumnMeta:
    prefix = DATA / name
    meta_path = Path(str(prefix) + ".fsst.meta.json")
    if not meta_path.exists():
        raise SystemExit(f"{meta_path} is missing; run scripts/build_columns.sh")
    meta = json.loads(meta_path.read_text())
    src = meta["source_prefix"]
    parquet = Path(src + ".parquet")
    return ColumnMeta(name, prefix, src, parquet, meta["rows"], meta["raw_bytes"], meta["cmp_bytes"], meta)


@dataclass
class PatternSpec:
    name: str
    column: str
    pattern: str
    column2: str | None = None

    @property
    def is_concat(self) -> bool:
        return bool(self.column2)

    @property
    def truth_path(self) -> Path:
        return TRUTH / f"{self.name}.ids"


def parse_pattern_file(path: Path) -> list[PatternSpec]:
    """Tab-separated: name<TAB>column<TAB>pattern[<TAB>column2]. '#' comments.

    The pattern field is taken verbatim including spaces; only \\t and \\\\ are
    unescaped, so a pattern can contain a literal tab.
    """
    out: list[PatternSpec] = []
    seen: set[str] = set()
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) not in (3, 4):
            raise SystemExit(f"{path}:{lineno}: expected 3 or 4 tab-separated fields, got {len(parts)}")
        name, column, pattern = parts[0], parts[1], parts[2]
        pattern = pattern.replace("\\t", "\t").replace("\\\\", "\\")
        if name in seen:
            raise SystemExit(f"{path}:{lineno}: duplicate pattern name {name!r}")
        seen.add(name)
        out.append(PatternSpec(name, column, pattern, parts[3] if len(parts) == 4 else None))
    if not out:
        raise SystemExit(f"{path}: no patterns")
    return out


def write_ids(path: Path, ids) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    buf = struct.pack(f"<{len(ids)}I", *ids)
    tmp = Path(str(path) + ".tmp")
    tmp.write_bytes(buf)
    tmp.replace(path)


def read_ids(path: Path) -> list[int]:
    raw = path.read_bytes()
    if len(raw) % 4:
        raise SystemExit(f"{path}: {len(raw)} bytes is not a whole number of uint32 row ids")
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def read_raw_values(prefix: Path, wanted: list[int]) -> dict[int, bytes]:
    """Reads specific rows out of <prefix>.raw using <prefix>.rawoff."""
    off = struct.unpack_from("<%dQ" % (os.path.getsize(str(prefix) + ".rawoff") // 8),
                             Path(str(prefix) + ".rawoff").read_bytes())
    out = {}
    with open(str(prefix) + ".raw", "rb") as f:
        for r in wanted:
            if r + 1 >= len(off):
                continue
            f.seek(off[r])
            out[r] = f.read(off[r + 1] - off[r])
    return out


# ---------------------------------------------------------------------------
# DuckDB backends: the Python module when importable, otherwise the pinned CLI.
# ---------------------------------------------------------------------------

class _CliResult:
    def __init__(self, rows):
        self._rows = rows

    def fetchall(self):
        return self._rows


class _CliConnection:
    """Enough of the DBAPI surface for this experiment, backed by the DuckDB CLI.

    Parameters are inlined as decode(from_hex('..')), which is injection-proof:
    the only characters that reach the SQL text are hex digits.
    """

    def __init__(self, binary: str):
        self.binary = binary

    def execute(self, sql: str, params=None):
        if params:
            sql = _inline_params(sql, params)
        proc = subprocess.run([self.binary, "-csv", "-noheader", "-c", sql],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise SystemExit(f"duckdb CLI failed:\n{proc.stderr}\nSQL: {sql[:400]}")
        rows = []
        for line in proc.stdout.splitlines():
            if line == "":
                continue
            rows.append(tuple(line.split(",")))
        return _CliResult(rows)


def _inline_params(sql: str, params) -> str:
    out = []
    it = iter(params)
    for ch in sql:
        if ch != "?":
            out.append(ch)
            continue
        try:
            v = next(it)
        except StopIteration:
            raise SystemExit("more ? placeholders than parameters")
        if isinstance(v, (int,)):
            out.append(str(int(v)))
            continue
        hexed = (v.encode("utf-8") if isinstance(v, str) else bytes(v)).hex()
        if not HEX_RE.match(hexed):
            raise SystemExit("parameter did not hex-encode cleanly")
        out.append(f"decode(from_hex('{hexed}'))")
    try:
        next(it)
    except StopIteration:
        pass
    else:
        raise SystemExit("more parameters than ? placeholders")
    return "".join(out)


def connect_duckdb():
    try:
        import duckdb  # type: ignore

        con = duckdb.connect()
        con.execute("SET threads=1")
        return con
    except ImportError:
        pass
    for cand in [os.environ.get("DUCKDB_CLI"),
                 str(Path(os.environ.get("TOKENLIKE_DIR", "")) / "tools" / "duckdb") if os.environ.get("TOKENLIKE_DIR") else None,
                 "/Users/prahasbodanapati/Documents/Desk/semV/HASHCODE/hash/TokenLIKE/tools/duckdb",
                 "duckdb"]:
        if cand and (os.path.isfile(cand) or cand == "duckdb"):
            return _CliConnection(cand)
    raise SystemExit("no DuckDB backend: install duckdb==1.5.6 or set DUCKDB_CLI")


def backend_name(con) -> str:
    return "duckdb-cli" if isinstance(con, _CliConnection) else "duckdb-python"
