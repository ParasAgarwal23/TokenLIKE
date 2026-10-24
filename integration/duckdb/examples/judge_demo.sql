-- TokenLIKE inside DuckDB: the minimal judge demo.
-- Run from the repository root:
--   .venv\Scripts\python -X utf8 integration\duckdb\python\run_sql.py integration\duckdb\examples\judge_demo.sql
-- (run_sql.py opens data\duckdb\tokenlike.duckdb with unsigned extensions allowed, runs each statement.)

LOAD 'build/duckdb/tokenlike.duckdb_extension';
SELECT tokenlike_load_vocab('data/vocab.bin') AS vocab_tokens;

-- The encoded column is a BLOB of little-endian uint16 token ids.
SELECT row_id, original_url, octet_length(encoded_url) // 2 AS tokens, detokenize(encoded_url) = original_url AS round_trip
FROM urls WHERE strlen(original_url) BETWEEN 30 AND 60 ORDER BY row_id LIMIT 3;

-- Native LIKE on raw text vs TokenLIKE on token ids: same answer.
SELECT COUNT(*) AS native_like FROM urls WHERE original_url LIKE '%.php';
SELECT COUNT(*) AS token_like  FROM urls WHERE token_like(encoded_url, '%.php');

-- Prefix, concatenation, and filter-then-aggregate.
SELECT COUNT(*) AS http_prefix FROM urls WHERE token_like(encoded_url, 'http://%');
SELECT COUNT(*) AS url_or_referer_yandex FROM urls WHERE token_like_concat(encoded_url, encoded_referer, '%yandex%');
SELECT traffic_source_id, COUNT(*) AS php_rows FROM urls WHERE token_like(encoded_url, '%.php') GROUP BY 1 ORDER BY 1;

-- Proof that the row sets are identical (both must be 0).
SELECT COUNT(*) AS extra   FROM (SELECT row_id FROM urls WHERE token_like(encoded_url, '%.php') EXCEPT SELECT row_id FROM urls WHERE original_url LIKE '%.php');
SELECT COUNT(*) AS missing FROM (SELECT row_id FROM urls WHERE original_url LIKE '%.php' EXCEPT SELECT row_id FROM urls WHERE token_like(encoded_url, '%.php'));

-- Filter and group on token ids, decode only the 10 output rows.
SELECT detokenize(encoded_url) AS url, n FROM (
  SELECT encoded_url, count(*) AS n FROM urls WHERE token_like(encoded_url, '%.php')
  GROUP BY encoded_url ORDER BY n DESC, encoded_url LIMIT 10);

-- What was built for the pattern, and where DuckDB runs it.
SELECT tokenlike_info('%.php') AS matcher;
EXPLAIN SELECT COUNT(*) FROM urls WHERE token_like(encoded_url, '%.php');
