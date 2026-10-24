| pattern | shape | FSST-LIKE best ns/row (mode) | TokenLIKE token ns/row | ratio | FSST units/row | TokenLIKE tokens/row |
|---|---|---|---|---|---|---|
| `http://%` | prefix | 3.09 (fsst) | 2.99 | 0.97x | 2.35 | 1.52 |
| `/static/%` | prefix, 0 matches | 1.25 (fsst) | 2.05 | 1.64x | 1.00 | 1.00 |
| `http://irr.ru%` | prefix | 3.38 (fsst) | 5.24 | 1.55x | 2.87 | 3.06 |
| `%.php` | suffix | 2.56 (fsst-rev) | 2.19 | 0.86x | 1.28 | 1.01 |
| `%.m3u8` | suffix, 0 matches | 2.39 (fsst-rev) | 2.22 | 0.93x | 1.25 | 1.00 |
| `%.html` | suffix | 2.51 (fsst-rev) | 2.26 | 0.90x | 1.27 | 1.01 |
| `http://irr.ru/index.php?showalbum/log...` | exact literal | 3.51 (fsst-rev) | 12.21 | 3.48x | 2.17 | 7.46 |
| `%yandex%` | contains | 57.92 (fsst) | 45.20 | 0.78x | 48.94 | 35.54 |
| `%/catalog/%` | contains | 57.80 (fsst) | 50.82 | 0.88x | 49.23 | 35.76 |
| `%/v_/%` | contains with '_' | 58.26 (fsst) | 71.52 | 1.23x | 49.89 | 36.13 |
| `%сезон%` | Cyrillic contains | 58.28 (fsst) | 29.46 | 0.51x | 49.81 | 36.09 |
| `http%irr%php%` | multi-% | 38.12 (fsst) | 36.18 | 0.95x | 33.36 | 25.14 |
| `http://_rr.ru/%php%` | prefix with '_' | 4.46 (fsst) | 7.85 | 1.76x | 3.76 | 4.97 |
| `%regular%` | contains (TPC-H) | 12.65 (fsst) | 12.92 | 1.02x | 13.35 | 9.86 |
| `%s` | suffix (TPC-H) | 2.71 (fsst-rev) | 2.38 | 0.88x | 1.00 | 1.00 |
