### Compression on real data (upstream FSST, one symbol table per column)

| column | rows | original B | codes B | symbol table B | .rev B | ratio (codes) | ratio (codes+table+.rev) | symbols | escape units | % escape units | rows with escape |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `clickbench/URL_1k` | 1,000 | 79,362 | 38,219 | 554 | 4,000 | 2.077x | 1.855x | 236 | 72 | 0.19% | 41 |
| `clickbench/URL_10k` | 10,000 | 760,012 | 396,374 | 541 | 40,000 | 1.917x | 1.739x | 255 | 256 | 0.06% | 210 |
| `clickbench/URL_100k` | 100,000 | 7,646,758 | 3,982,606 | 561 | 400,000 | 1.920x | 1.745x | 252 | 6,868 | 0.17% | 5,315 |
| `clickbench/URL_1m` | 1,000,000 | 88,562,192 | 49,965,040 | 565 | 4,000,000 | 1.772x | 1.641x | 255 | 74,457 | 0.15% | 43,769 |
| `clickbench/Referer_1m` | 1,000,000 | 79,583,339 | 46,112,704 | 600 | 4,000,000 | 1.726x | 1.588x | 255 | 153,570 | 0.33% | 68,971 |
| `tpch/l_comment_1m` | 1,000,000 | 26,511,095 | 9,320,929 | 865 | 4,000,000 | 2.844x | 1.990x | 212 | 0 | 0.00% | 0 |
| `tpch/o_comment_1m` | 1,000,000 | 48,529,673 | 15,161,196 | 903 | 4,000,000 | 3.201x | 2.533x | 216 | 0 | 0.00% | 0 |
| `dbtext/urls_100k` | 100,000 | 6,227,875 | 2,668,368 | 596 | 400,000 | 2.334x | 2.029x | 253 | 16,606 | 0.63% | 2,231 |
| `fuzz/values` | 20,016 | 1,762,863 | 988,670 | 559 | 80,064 | 1.783x | 1.649x | 255 | 1,546 | 0.16% | 1,081 |
| `sample/urls_1k` | 1,000 | 62,455 | 28,265 | 570 | 4,000 | 2.210x | 1.902x | 210 | 316 | 1.13% | 35 |
| `edge/values` | 51 | 669 | 500 | 123 | 204 | 1.338x | 0.809x | 92 | 8 | 1.63% | 4 |

### Dense versus compact lifted tables (clickbench/URL_1m)

The DFA's own 256-wide byte table is in both figures, since the escape path needs it.

| pattern | states | dense ns/row | compact ns/row | dense total table B | compact total table B | compact/dense size | compact/dense time |
|---|---|---|---|---|---|---|---|
| `http://%` (prefix) | 9 | 3.09 | 6.61 | 9,225 | 5,065 | 0.55x | 2.14x |
| `%.php` (suffix) | 7 | 59.03 | 119.26 | 7,175 | 3,957 | 0.55x | 2.02x |
| `%yandex%` (contains) | 8 | 58.57 | 117.12 | 8,200 | 4,552 | 0.56x | 2.00x |
| `%/v_/%` (underscore) | 14 | 59.05 | 135.07 | 14,350 | 8,504 | 0.59x | 2.29x |
| `http%irr%php%` (complex) | 12 | 38.76 | 93.84 | 12,300 | 6,872 | 0.56x | 2.42x |
| `http://irr.ru/index.php?showalbum/log...` (long literal) | 66 | 8.59 | 30.57 | 67,650 | 37,086 | 0.55x | 3.56x |

### Cost of making the backward scan exact (clickbench/URL_1m)

The 4-byte-per-row `.fsst.rev` sidecar is 4,000,000 B, 8.0% on top of the 49,965,040 B of codes. A 1-bit-per-row has-escape flag would be 125,000 B, 0.25%, but then every one of the 43,769 rows with an escape (4.4%) has to be scanned forward instead.

| pattern | shape | 4-byte sidecar ns/row | 1-bit flag ns/row | **no sidecar** ns/row | sidecar fallbacks | 1-bit fallbacks | no-sidecar fallbacks | sidecar units/row | 1-bit units/row | no-sidecar units/row |
|---|---|---|---|---|---|---|---|---|---|---|
| `%.php` | suffix | 2.56 | 8.65 | 2.09 | 3,596 | 43,769 | 0 | 1.28 | 5.46 | 1.04 |
| `%.html` | suffix | 2.51 | 8.78 | 2.03 | 3,596 | 43,769 | 0 | 1.27 | 5.45 | 1.02 |
| `%.m3u8` | suffix | 2.39 | 8.73 | 1.91 | 3,596 | 43,769 | 0 | 1.25 | 5.43 | 1.01 |
| `http://irr.ru/index.php?showalbum/log...` | exact | 3.50 | 3.99 | 4.00 | 3,596 | 43,769 | 0 | 2.17 | 2.35 | 2.15 |
| `%yandex%` | contains | 61.75 | 59.73 | 74.49 | 43,285 | 43,769 | 0 | 50.01 | 48.57 | 48.59 |

### Compression cost (upstream FSST, single-threaded)

| column | rows | fsst_create ms | fsst_compress ms | MB/s compressed |
|---|---|---|---|---|
| `clickbench/URL_100k` | 100,000 | 1 | 13 | 581 |
| `clickbench/URL_1m` | 1,000,000 | 1 | 160 | 553 |
| `clickbench/Referer_1m` | 1,000,000 | 1 | 141 | 565 |
| `tpch/l_comment_1m` | 1,000,000 | 1 | 88 | 302 |
| `tpch/o_comment_1m` | 1,000,000 | 1 | 93 | 521 |
| `dbtext/urls_100k` | 100,000 | 1 | 12 | 522 |

### Peak resident set size, whole process (clickbench/URL_1m unless noted)

| mode | min MiB | median MiB | max MiB | what it has to hold |
|---|---|---|---|---|
| raw | 43.1 | 101.6 | 105.5 | 88 MB raw column + offsets |
| raw-opt | 43.0 | 101.5 | 105.4 |  |
| decode | 26.7 | 64.8 | 68.7 | 50 MB codes + offsets + one decode buffer |
| fsst | 26.6 | 64.8 | 68.7 | 50 MB codes + offsets + lifted table |
| fsst-compact | 26.7 | 64.8 | 68.7 | 50 MB codes + offsets + compact table |
| fsst-rev | 30.5 | 68.9 | 72.6 | 50 MB codes + offsets + .rev + two lifted tables |
| fsst-rev2 | 26.7 | 65.0 | 68.8 |  |

