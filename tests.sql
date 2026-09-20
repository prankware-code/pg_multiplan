DROP EXTENSION pg_multiplan;
CREATE EXTENSION pg_multiplan;

SET enable_seqscan = off;
SET pg_multiplan.enable = on;
SET pg_multiplan.enable_caching = on;
EXPLAIN ANALYZE SELECT * FROM a WHERE b > 100;
SET enable_seqscan = on;
EXPLAIN ANALYZE SELECT * FROM a WHERE b > 100;
SET pg_multiplan.enable_caching = off;
EXPLAIN ANALYZE SELECT * FROM a WHERE b > 100;
SELECT pg_multiplan_list();
SELECT pg_multiplan_select(...);