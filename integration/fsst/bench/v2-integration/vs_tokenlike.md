| pattern | shape | best FSST-LIKE ns/row (mode) | TokenLIKE token ns/row | ratio | FSST units/row | TokenLIKE tokens/row |
|---|---|---|---|---|---|---|
| `http://%` | prefix | 3.09 (fsst) | 3.01 | 0.97x | 2.35 | 1.52 |
| `/static/%` | prefix, 0 matches | 1.26 (fsst) | 2.14 | 1.70x | 1.00 | 1.00 |
| `http://irr.ru%` | prefix | 3.39 (fsst) | 5.27 | 1.55x | 2.87 | 3.06 |
| `%.php` | suffix | 2.15 (fsst-rev2) | 2.24 | 1.05x | 1.04 | 1.01 |
| `%.m3u8` | suffix, 0 matches | 1.91 (fsst-rev2) | 2.23 | 1.17x | 1.01 | 1.00 |
| `%.html` | suffix | 2.02 (fsst-rev2) | 2.29 | 1.13x | 1.02 | 1.01 |
| `http://irr.ru/index.php?showalbum/log...` | exact literal | 3.65 (fsst-rev) | 12.26 | 3.35x | 2.17 | 7.46 |
| `%yandex%` | contains | 58.44 (fsst) | 45.04 | 0.77x | 48.94 | 35.54 |
| `%/catalog/%` | contains | 58.10 (fsst) | 51.08 | 0.88x | 49.23 | 35.76 |
| `%/v_/%` | contains with '_' | 59.06 (fsst) | 73.43 | 1.24x | 49.89 | 36.13 |
| `%сезон%` | Cyrillic contains | 58.69 (fsst) | 31.06 | 0.53x | 49.81 | 36.09 |
| `http%irr%php%` | multi-% | 38.66 (fsst) | 36.45 | 0.94x | 33.36 | 25.14 |
| `http://_rr.ru/%php%` | prefix with '_' | 4.47 (fsst) | 8.01 | 1.79x | 3.76 | 4.97 |
| `%regular%` | contains (TPC-H) | 12.66 (fsst) | 13.13 | 1.04x | 13.35 | 9.86 |
| `%s` | suffix (TPC-H) | 2.49 (fsst-rev2) | 2.41 | 0.97x | 1.00 | 1.00 |
