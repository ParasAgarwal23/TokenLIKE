# Vendored DuckDB C extension API headers

| File | Source | SHA-256 |
|---|---|---|
| `duckdb.h` | https://raw.githubusercontent.com/duckdb/duckdb/v1.5.6/src/include/duckdb.h | `e85f70155b5948c8575e59f66e60d43575cec8b252293534c9e8598eb313a293` |
| `duckdb_extension.h` | https://raw.githubusercontent.com/duckdb/duckdb/v1.5.6/src/include/duckdb_extension.h | `dfbb388b8815df5f279561694274d95119ac1eaf7be2881926dfaf606471fa9e` |
| `LICENSE` | https://raw.githubusercontent.com/duckdb/duckdb/v1.5.6/LICENSE (MIT) | `7e17fd31249fa875cb3b1c5e05c6c3e99b75509f6a2804ca176c217834de1dcb` |

Unmodified. Only these two headers are needed: the extension talks to DuckDB exclusively through
the C extension API function-pointer table (`C_STRUCT` ABI), so no DuckDB source or library is
compiled or linked. Targeted API version: `v1.5.6` (the default of these headers; stable surface only,
no `DUCKDB_EXTENSION_API_VERSION_UNSTABLE`).

The SHA-256 values are of the files as published (LF line endings), i.e. of the committed blobs.
A Windows checkout with `core.autocrlf=true` has CRLF copies whose hashes differ.
