"""Shared helpers for the TokenLIKE DuckDB integration (integration/duckdb).

All paths derive from the repository root and can be overridden with environment variables:
  TOKENLIKE_DATA     data directory produced by scripts/encode   (default <repo>/data)
  TOKENLIKE_EXT      the built extension    (default <repo>/build/duckdb/tokenlike.duckdb_extension)
  TOKENLIKE_DUCKDB   the demo database      (default <data>/duckdb/tokenlike.duckdb)
  TOKENLIKE_CLI      the TokenLIKE CLI used as a reference (default <repo>/build/tokenlike.exe)
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes
import os
import sys
import time
from pathlib import Path

import duckdb
import numpy as np
import pyarrow as pa

MODULE = Path(__file__).resolve().parent.parent          # integration/duckdb
ROOT = MODULE.parent.parent                                # repository root
PATTERNS = ROOT / "patterns"
DATA = Path(os.environ.get("TOKENLIKE_DATA", ROOT / "data")).resolve()
EXT = Path(os.environ.get("TOKENLIKE_EXT", ROOT / "build" / "duckdb" / "tokenlike.duckdb_extension")).resolve()
VOCAB = DATA / "vocab.bin"
DB = Path(os.environ.get("TOKENLIKE_DUCKDB", DATA / "duckdb" / "tokenlike.duckdb")).resolve()
UNC_DB = DB.with_name("bench_uncompressed.duckdb")
CLI = Path(os.environ.get("TOKENLIKE_CLI", ROOT / "build" / ("tokenlike.exe" if sys.platform == "win32" else "tokenlike"))).resolve()
SCRATCH = ROOT / "build" / "duckdb" / "scratch"           # build/ is git-ignored
RESULTS = MODULE / "results"


def rel(p: Path | str) -> str:
    """Path relative to the repository root when possible (keeps local paths out of results)."""
    p = Path(p).resolve()
    try:
        return p.relative_to(ROOT).as_posix()
    except ValueError:
        return "<outside repository>/" + p.name


def connect(db: Path | str | None = DB, *, read_only: bool = False, threads: int | None = None,
            load: bool = True, vocab: Path | str | None = VOCAB) -> duckdb.DuckDBPyConnection:
    """Open DuckDB with the native TokenLIKE extension loaded and the vocabulary installed."""
    path = ":memory:" if db is None else str(db)
    con = duckdb.connect(path, read_only=read_only, config={"allow_unsigned_extensions": "true"})
    if threads is not None:
        con.execute(f"SET threads={int(threads)}")
    if load:
        if not EXT.exists():
            raise SystemExit(f"extension not built: {rel(EXT)} (see integration/duckdb/README.md: cmake --build build/duckdb)")
        con.execute(f"LOAD '{EXT.as_posix()}'")
        if vocab is not None:
            # A literal, not a ? parameter: the DuckDB Python client spends ~0.4 s on the first
            # parameterised statement of a process (measured, independent of this extension).
            con.execute(f"SELECT tokenlike_load_vocab({sql_str(Path(vocab).as_posix())})")
    return con


def token_blobs(prefix: Path | str) -> pa.Array:
    """<prefix>.tok/.off as an Arrow large_binary array, one little-endian uint16 BLOB per row.

    Zero-copy over the files' contents: row i is tok[off[i]:off[i+1]] reinterpreted as bytes.
    """
    prefix = str(prefix)
    tok = np.fromfile(prefix + ".tok", dtype="<u2")
    off = np.fromfile(prefix + ".off", dtype="<u8")
    if off.size == 0 or off[0] != 0 or off[-1] != tok.size or np.any(np.diff(off.astype(np.int64)) < 0):
        raise ValueError(f"{prefix}.off is not a valid prefix-sum over {prefix}.tok")
    boff = (off.astype(np.int64) * 2).astype("<i8")
    arr = pa.Array.from_buffers(pa.large_binary(), int(off.size - 1),
                                [None, pa.py_buffer(boff), pa.py_buffer(tok.view(np.uint8))])
    arr.validate(full=True)
    return arr


def raw_strings(prefix: Path | str) -> pa.Array:
    """<prefix>.raw/.rawoff as an Arrow large_string array (UTF-8 validated)."""
    prefix = str(prefix)
    raw = np.fromfile(prefix + ".raw", dtype=np.uint8)
    off = np.fromfile(prefix + ".rawoff", dtype="<u8").astype("<i8")
    arr = pa.Array.from_buffers(pa.large_string(), int(off.size - 1), [None, pa.py_buffer(off), pa.py_buffer(raw)])
    arr.validate(full=True)
    return arr


def read_patterns(path: Path | str) -> list[tuple[str, str, str, str | None]]:
    """Person B's pattern format: name<TAB>column_prefix<TAB>pattern[<TAB>column2_prefix]."""
    out = []
    with open(path, encoding="utf-8", newline="") as f:
        for line in f:
            line = line.rstrip("\n").rstrip("\r")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            out.append((parts[0], parts[1], parts[2], parts[3] if len(parts) > 3 and parts[3] else None))
    return out


def sql_str(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


class Timer:
    def __enter__(self):
        self.t0 = time.perf_counter()
        return self

    def __exit__(self, *a):
        self.s = time.perf_counter() - self.t0


class _PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.wintypes.DWORD), ("PageFaultCount", ctypes.wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def memory_mb() -> dict:
    """Current and peak working set of this process (Windows only), in MB."""
    if sys.platform != "win32":
        return {}
    pmc = _PMC()
    pmc.cb = ctypes.sizeof(_PMC)
    k32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
    k32.GetCurrentProcess.restype = ctypes.wintypes.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.wintypes.HANDLE, ctypes.POINTER(_PMC), ctypes.wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = ctypes.wintypes.BOOL
    if not psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
        return {}
    return {"working_set_mb": round(pmc.WorkingSetSize / 2**20, 1),
            "peak_working_set_mb": round(pmc.PeakWorkingSetSize / 2**20, 1)}


def stats(con) -> dict:
    import json
    return json.loads(con.execute("SELECT tokenlike_stats()").fetchone()[0])


def performance_cpus() -> list[int]:
    """Logical CPUs of the highest efficiency class (P-cores on hybrid Intel CPUs); Windows only."""
    if sys.platform != "win32":
        return list(range(os.cpu_count() or 1))
    k32 = ctypes.windll.kernel32
    need = ctypes.wintypes.ULONG(0)
    k32.GetSystemCpuSetInformation(None, 0, ctypes.byref(need), None, 0)
    buf = ctypes.create_string_buffer(need.value)
    if not k32.GetSystemCpuSetInformation(buf, need, ctypes.byref(need), None, 0):
        return list(range(os.cpu_count() or 1))
    raw, off, cpus = buf.raw, 0, []
    while off < need.value:  # SYSTEM_CPU_SET_INFORMATION entries
        size = int.from_bytes(raw[off:off + 4], "little")
        cpus.append((raw[off + 18], raw[off + 14]))  # (EfficiencyClass, LogicalProcessorIndex)
        off += size
    best = max(e for e, _ in cpus)
    return sorted(lp for e, lp in cpus if e == best)


def pin_to_performance_cores() -> dict:
    """Restrict this process (and its children) to the P-cores and opt out of EcoQoS throttling."""
    cpus = performance_cpus()
    info = {"pinned_cpus": cpus}
    if sys.platform != "win32":
        return info
    k32 = ctypes.windll.kernel32
    k32.GetCurrentProcess.restype = ctypes.wintypes.HANDLE
    h = k32.GetCurrentProcess()
    mask = sum(1 << c for c in cpus)
    k32.SetProcessAffinityMask.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_size_t]
    info["affinity_ok"] = bool(k32.SetProcessAffinityMask(h, mask))

    class _PPTS(ctypes.Structure):  # PROCESS_POWER_THROTTLING_STATE
        _fields_ = [("Version", ctypes.wintypes.ULONG), ("ControlMask", ctypes.wintypes.ULONG),
                    ("StateMask", ctypes.wintypes.ULONG)]
    st = _PPTS(1, 1, 0)  # control EXECUTION_SPEED, state off = never throttle
    k32.SetProcessInformation.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, ctypes.wintypes.DWORD]
    info["power_throttling_disabled"] = bool(k32.SetProcessInformation(h, 4, ctypes.byref(st), ctypes.sizeof(st)))
    return info
