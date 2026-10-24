"""Turn a shared library into a loadable DuckDB extension by appending DuckDB's metadata footer.

    python append_metadata.py --library tokenlike_raw.dll --out tokenlike.duckdb_extension \
        --platform windows_amd64 --duckdb-version v1.5.6 --extension-version v0.1.0 [--abi-type C_STRUCT]

Footer layout (what DuckDB's extension loader reads from the last 534 bytes of the file):
  * a WebAssembly custom-section header so the file stays a valid Wasm module on that platform:
    0x00 (custom section id), LEB128 payload length 531, name length 16, b"duckdb_signature",
    LEB128 length 512;
  * eight 32-byte NUL-padded ASCII fields, written last-field-first:
    FIELD8..FIELD6 (unused, empty), FIELD5 ABI type, FIELD4 extension version,
    FIELD3 DuckDB version (for the C_STRUCT ABI: the C extension API version), FIELD2 platform,
    FIELD1 the magic value "4";
  * 256 zero bytes reserved for a signature (unsigned extension).
"""
from __future__ import annotations

import argparse
import os
import shutil


def leb128(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def field(s: str) -> bytes:
    raw = s.encode("ascii")
    if len(raw) > 32:
        raise ValueError(f"metadata field longer than 32 bytes: {s!r}")
    return raw + b"\0" * (32 - len(raw))


def footer(platform: str, duckdb_version: str, extension_version: str, abi_type: str) -> bytes:
    name = b"duckdb_signature"
    body = b"".join(field(x) for x in ("", "", "", abi_type, extension_version, duckdb_version, platform, "4"))
    body += b"\0" * 256
    payload = bytes([len(name)]) + name + leb128(len(body)) + body
    return b"\x00" + leb128(len(payload)) + payload


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--duckdb-version", required=True)
    ap.add_argument("--extension-version", required=True)
    ap.add_argument("--abi-type", default="C_STRUCT")
    a = ap.parse_args()
    tmp = a.out + ".tmp"
    shutil.copyfile(a.library, tmp)
    with open(tmp, "ab") as f:
        f.write(footer(a.platform, a.duckdb_version, a.extension_version, a.abi_type))
    os.replace(tmp, a.out)
    print(f"wrote {a.out} ({a.platform}, C API {a.duckdb_version}, ABI {a.abi_type}, extension {a.extension_version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
