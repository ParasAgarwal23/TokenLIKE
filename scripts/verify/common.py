#!/usr/bin/env python3
"""Shared helpers for the TokenLIKE correctness tooling.

Pattern file format (tab-separated, '#' starts a comment):

    name<TAB>column_prefix<TAB>pattern[<TAB>column2_prefix]

A fourth field marks a concatenation pattern, evaluated as (a || b) LIKE ...

Match-id files hold sorted little-endian uint32 row ids, which is the format
both the DuckDB ground truth and Person C's CLI emit.
"""

import csv
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
from array import array
from dataclasses import dataclass

TRUTH_ROOT = "data/truth"

# Pinned DuckDB version for both backends, so the oracle is reproducible.
DUCKDB_VERSION = "1.5.6"
HEX_RE = re.compile(r"^[0-9a-f]*$")
_INT_RE = re.compile(r"^-?\d+$")


@dataclass(frozen=True)
class Pattern:
    """One named LIKE pattern to verify."""

    name: str
    column: str
    pattern: str
    column2: str | None = None

    @property
    def is_concat(self) -> bool:
        return self.column2 is not None

    @property
    def dataset(self) -> str:
        """Dataset label, taken from the column prefix's parent directory."""
        return os.path.basename(os.path.dirname(self.column)) or "default"

    @property
    def truth_path(self) -> str:
        return os.path.join(TRUTH_ROOT, self.dataset, self.name + ".ids")

    def describe(self) -> str:
        cols = self.column if not self.is_concat else f"{self.column} || {self.column2}"
        return f"{self.name}  [{cols}]  LIKE {self.pattern!r}"


def parse_pattern_file(path: str) -> list[Pattern]:
    """Parse a pattern file, failing loudly on a malformed line."""
    patterns: list[Pattern] = []
    seen: set[str] = set()
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) not in (3, 4):
                raise ValueError(
                    f"{path}:{lineno}: expected 3 or 4 tab-separated fields, got "
                    f"{len(fields)}: {line!r}"
                )
            name, column, pattern = fields[0], fields[1], fields[2]
            column2 = fields[3] if len(fields) == 4 else None
            if not name or not column or pattern is None:
                raise ValueError(f"{path}:{lineno}: empty name or column: {line!r}")
            if name in seen:
                raise ValueError(f"{path}:{lineno}: duplicate pattern name {name!r}")
            seen.add(name)
            patterns.append(Pattern(name, column, pattern, column2))
    if not patterns:
        raise ValueError(f"{path}: no patterns found")
    return patterns


def write_ids(path: str, ids) -> None:
    """Write sorted row ids as little-endian uint32, atomically."""
    arr = array("I", sorted(ids))
    if arr and arr[-1] > 0xFFFFFFFF:
        raise ValueError(f"row id {arr[-1]} does not fit in uint32")
    if sys.byteorder != "little":
        arr.byteswap()
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        arr.tofile(f)
    os.replace(tmp, path)


def read_ids(path: str) -> array:
    """Read a little-endian uint32 match-id file."""
    size = os.path.getsize(path)
    if size % 4 != 0:
        raise ValueError(f"{path}: size {size} is not a multiple of 4 (uint32 row ids)")
    arr = array("I")
    with open(path, "rb") as f:
        arr.frombytes(f.read())
    if sys.byteorder != "little":
        arr.byteswap()
    return arr


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_raw_values(prefix: str) -> list[bytes]:
    """Read a raw column back into per-row byte strings, for debug output."""
    rawoff = array("Q")
    with open(prefix + ".rawoff", "rb") as f:
        rawoff.frombytes(f.read())
    if sys.byteorder != "little":
        rawoff.byteswap()
    with open(prefix + ".raw", "rb") as f:
        raw = f.read()
    return [raw[rawoff[i] : rawoff[i + 1]] for i in range(len(rawoff) - 1)]


def read_token_rows(prefix: str):
    """Read a token column back as (row -> list of ids), for debug output."""
    off = array("Q")
    with open(prefix + ".off", "rb") as f:
        off.frombytes(f.read())
    tok = array("H")
    with open(prefix + ".tok", "rb") as f:
        tok.frombytes(f.read())
    if sys.byteorder != "little":
        off.byteswap()
        tok.byteswap()
    return [list(tok[off[i] : off[i + 1]]) for i in range(len(off) - 1)]


# ---------------------------------------------------------------------------
# DuckDB access
#
# Two backends, same tiny surface. The Python module is preferred because it
# takes genuine bound parameters. The pinned CLI is the fallback, because
# installing the wheel can be slow or impossible on a bad network, and the
# correctness gate must not depend on that.
#
# The CLI backend never interpolates a value into SQL as text. Each '?' becomes
# decode(from_hex('<hex>')), where the hex comes from the value's UTF-8 bytes
# and is asserted to be hex digits only -- a string that cannot carry a quote,
# a comment or a statement separator, so there is nothing to inject with.
# ---------------------------------------------------------------------------


class _CliResult:
    """Result of one CLI query."""

    def __init__(self, rows):
        self._rows = rows

    def fetchall(self):
        return self._rows

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchmany(self, _size):
        raise NotImplementedError(
            "the DuckDB CLI backend does not support streaming fetchmany; "
            "install the duckdb Python module for the data-preparation scripts"
        )


class _CliConnection:
    """Minimal DuckDB connection backed by the pinned CLI binary.

    Values come back parsed from CSV, so types are recovered heuristically:
    integers, booleans and otherwise strings. That covers every query the
    oracle issues (row ids, counts, LIKE verdicts, version strings); it is not
    a general-purpose driver.
    """

    backend = "cli"

    def __init__(self, binary: str):
        self.binary = binary

    def execute(self, sql: str, params=None):
        sql = _inline_params(sql, params or [])
        proc = subprocess.run(
            [self.binary, "-noheader", "-csv", "-c", sql],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"duckdb CLI failed ({proc.returncode}): "
                f"{(proc.stderr or proc.stdout).strip()}"
            )
        rows = []
        for fields in csv.reader(io.StringIO(proc.stdout)):
            if not fields:
                continue
            rows.append(tuple(_coerce(f) for f in fields))
        return _CliResult(rows)

    def close(self):
        pass


def _coerce(field: str):
    if _INT_RE.match(field):
        return int(field)
    if field == "true":
        return True
    if field == "false":
        return False
    return field


def _inline_params(sql: str, params) -> str:
    """Replace each '?' with a hex-decoded literal.

    The SQL issued by this project contains no '?' inside string literals, so a
    positional split is sufficient and is asserted against the parameter count.
    """
    parts = sql.split("?")
    if len(parts) - 1 != len(params):
        raise ValueError(
            f"SQL has {len(parts) - 1} placeholder(s) but {len(params)} parameter(s) were given"
        )
    out = [parts[0]]
    for value, tail in zip(params, parts[1:]):
        if value is None:
            out.append("NULL")
        elif isinstance(value, bool):
            out.append("true" if value else "false")
        elif isinstance(value, int):
            out.append(str(value))
        else:
            encoded = str(value).encode("utf-8").hex()
            if not HEX_RE.match(encoded):
                raise AssertionError(f"hex encoding produced unexpected text: {encoded!r}")
            out.append(f"decode(from_hex('{encoded}'))")
        out.append(tail)
    return "".join(out)


def find_duckdb_cli() -> str | None:
    """Locate a DuckDB CLI: explicit override, repo-local tools/, then PATH."""
    override = os.environ.get("TOKENLIKE_DUCKDB")
    if override and os.path.exists(override):
        return override
    local = os.path.join("tools", "duckdb")
    if os.path.exists(local):
        return local
    return shutil.which("duckdb")


def connect_duckdb(prefer_cli: bool = False):
    """Open a DuckDB connection, preferring the Python module.

    Raises SystemExit with installation guidance if neither backend is present.
    """
    if not prefer_cli:
        try:
            import duckdb
        except ImportError:
            pass
        else:
            con = duckdb.connect()
            con.execute("SET threads=1")
            return con

    cli = find_duckdb_cli()
    if cli:
        return _CliConnection(cli)

    raise SystemExit(
        "No DuckDB backend found. Either:\n"
        f"  .venv/bin/python -m pip install 'duckdb=={DUCKDB_VERSION}'\n"
        "or fetch the pinned CLI (works when the PyPI CDN is slow):\n"
        f"  curl -L -o tools/duckdb.zip https://github.com/duckdb/duckdb/releases/download/"
        f"v{DUCKDB_VERSION}/duckdb_cli-osx-universal.zip \\\n"
        "    && unzip -o tools/duckdb.zip -d tools/ && chmod +x tools/duckdb\n"
        "or point TOKENLIKE_DUCKDB at an existing duckdb binary."
    )


def backend_name(con) -> str:
    """Report which DuckDB backend a connection uses.

    The Python module's connection object does not accept new attributes, so
    the backend is identified by type rather than tagged onto the connection.
    """
    return "cli" if isinstance(con, _CliConnection) else "python-module"
