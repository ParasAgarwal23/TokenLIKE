### Physical storage, identical rows

Every byte a reader has to keep, not just the payload. `.fsst.off` is stored as `uint64` per row boundary, which is what the loader reads; the `u32 offsets` column is what the same data would cost with 4-byte offsets, which is sound for any payload under 4 GiB and is what a real engine would do. The sidecar is **no longer required** — mode `fsst-rev2` reverse-scans without it (see FINAL_REPORT.md §H) — so it is listed but excluded from the totals, with the sidecar total shown for comparison.

| column | rows | raw bytes | codes | offsets (u64) | offsets (u32) | symbol table | sidecar (optional) | total, u32 off, no sidecar | ratio, payload only | ratio, total | ratio, total + sidecar |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `clickbench/URL_1m` | 1,000,000 | 88,562,192 | 49,965,040 | 8,000,008 | 4,000,004 | 565 | 4,000,000 | 53,965,609 | 1.772x | 1.641x | 1.528x |
| `clickbench/Referer_1m` | 1,000,000 | 79,583,339 | 46,112,704 | 8,000,008 | 4,000,004 | 600 | 4,000,000 | 50,113,308 | 1.726x | 1.588x | 1.471x |
| `tpch/l_comment_1m` | 1,000,000 | 26,511,095 | 9,320,929 | 8,000,008 | 4,000,004 | 865 | 4,000,000 | 13,321,798 | 2.844x | 1.990x | 1.531x |
| `tpch/o_comment_1m` | 1,000,000 | 48,529,673 | 15,161,196 | 8,000,008 | 4,000,004 | 903 | 4,000,000 | 19,162,103 | 3.201x | 2.533x | 2.095x |
| `dbtext/urls_100k` | 100,000 | 6,227,875 | 2,668,368 | 800,008 | 400,004 | 596 | 400,000 | 3,068,968 | 2.334x | 2.029x | 1.795x |
| `clickbench/URL_100k` | 100,000 | 7,646,758 | 3,982,606 | 800,008 | 400,004 | 561 | 400,000 | 4,383,171 | 1.920x | 1.745x | 1.599x |
| `clickbench/URL_10k` | 10,000 | 760,012 | 396,374 | 80,008 | 40,004 | 541 | 40,000 | 436,919 | 1.917x | 1.739x | 1.594x |
| `clickbench/URL_1k` | 1,000 | 79,362 | 38,219 | 8,008 | 4,004 | 554 | 4,000 | 42,777 | 2.077x | 1.855x | 1.697x |

### Same rows, FSST versus the LLM token table

The token-table columns come from the original TokenLIKE pipeline over the identical canonical parquet rows. Its vocabulary (`data/vocab.bin`, 630,133 B) is **shared by every column**, so it is shown separately rather than charged to each one; FSST's symbol table is per column and is charged. Both use `u32` offsets here, so the comparison is of payload plus per-column metadata.

| column | rows | raw bytes | FSST codes + table | token ids (`uint16`) | FSST total (u32 off) | token total (u32 off) | FSST ratio | token ratio | FSST / token |
|---|---|---|---|---|---|---|---|---|---|
| `clickbench/URL_1m` | 1,000,000 | 88,562,192 | 49,965,605 | 72,272,106 | 53,965,609 | 76,272,110 | 1.641x | 1.161x | 1.413x |
| `clickbench/Referer_1m` | 1,000,000 | 79,583,339 | 46,113,304 | 65,951,752 | 50,113,308 | 69,951,756 | 1.588x | 1.138x | 1.396x |
| `tpch/l_comment_1m` | 1,000,000 | 26,511,095 | 9,321,794 | 13,167,378 | 13,321,798 | 17,167,382 | 1.990x | 1.544x | 1.289x |
| `tpch/o_comment_1m` | 1,000,000 | 48,529,673 | 15,162,099 | 22,274,900 | 19,162,103 | 26,274,904 | 2.533x | 1.847x | 1.371x |
| `dbtext/urls_100k` | 100,000 | 6,227,875 | 2,668,964 | 4,851,408 | 3,068,968 | 5,251,412 | 2.029x | 1.186x | 1.711x |

The token table's 630,133-byte vocabulary is amortised across however many columns a deployment has. Charged in full to a single 1M-row column it would add about 1.3% to that column's FSST-equivalent footprint; across the four 1M-row columns measured here it is under 0.4% each. FSST's per-column tables total 5,185 B for all 8 columns.

