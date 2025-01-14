set optimizer_trace_fallback to on;

--
-- UNION (also INTERSECT, EXCEPT)
--

-- Simple UNION constructs

SELECT 1 AS two UNION SELECT 2;

SELECT 1 AS one UNION SELECT 1;

SELECT 1 AS two UNION ALL SELECT 2;

SELECT 1 AS two UNION ALL SELECT 1;

SELECT 1 AS three UNION SELECT 2 UNION SELECT 3;

SELECT 1 AS two UNION SELECT 2 UNION SELECT 2;

SELECT 1 AS three UNION SELECT 2 UNION ALL SELECT 2;

SELECT 1.1 AS two UNION SELECT 2.2;

-- Mixed types

SELECT 1.1 AS two UNION SELECT 2;

SELECT 1 AS two UNION SELECT 2.2;

SELECT 1 AS one UNION SELECT 1.0::float8;

SELECT 1.1 AS two UNION ALL SELECT 2;

SELECT 1.0::float8 AS two UNION ALL SELECT 1;

SELECT 1.1 AS three UNION SELECT 2 UNION SELECT 3;

SELECT 1.1::float8 AS two UNION SELECT 2 UNION SELECT 2.0::float8 ORDER BY 1;

SELECT 1.1 AS three UNION SELECT 2 UNION ALL SELECT 2;

SELECT 1.1 AS two UNION (SELECT 2 UNION ALL SELECT 2);

--
-- Try testing from tables...
--

SELECT f1 AS five FROM FLOAT8_TBL
UNION
SELECT f1 FROM FLOAT8_TBL
ORDER BY 1;

SELECT f1 AS ten FROM FLOAT8_TBL
UNION ALL
SELECT f1 FROM FLOAT8_TBL;

SELECT f1 AS nine FROM FLOAT8_TBL
UNION
SELECT f1 FROM INT4_TBL
ORDER BY 1;

SELECT f1 AS ten FROM FLOAT8_TBL
UNION ALL
SELECT f1 FROM INT4_TBL;

SELECT f1 AS five FROM FLOAT8_TBL
  WHERE f1 BETWEEN -1e6 AND 1e6
UNION
SELECT f1 FROM INT4_TBL
  WHERE f1 BETWEEN 0 AND 1000000;

SELECT CAST(f1 AS char(4)) AS three FROM VARCHAR_TBL
UNION
SELECT f1 FROM CHAR_TBL
ORDER BY 1;

SELECT f1 AS three FROM VARCHAR_TBL
UNION
SELECT CAST(f1 AS varchar) FROM CHAR_TBL
ORDER BY 1;

SELECT f1 AS eight FROM VARCHAR_TBL
UNION ALL
SELECT f1 FROM CHAR_TBL;

SELECT f1 AS five FROM TEXT_TBL
UNION
SELECT f1 FROM VARCHAR_TBL
UNION
SELECT TRIM(TRAILING FROM f1) FROM CHAR_TBL
ORDER BY 1;

--
-- INTERSECT and EXCEPT
--

SELECT q2 FROM int8_tbl INTERSECT SELECT q1 FROM int8_tbl;

SELECT q2 FROM int8_tbl INTERSECT ALL SELECT q1 FROM int8_tbl;

SELECT q2 FROM int8_tbl EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q2 FROM int8_tbl EXCEPT ALL SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q2 FROM int8_tbl EXCEPT ALL SELECT DISTINCT q1 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl EXCEPT SELECT q2 FROM int8_tbl;

SELECT q1 FROM int8_tbl EXCEPT ALL SELECT q2 FROM int8_tbl;

SELECT q1 FROM int8_tbl EXCEPT ALL SELECT DISTINCT q2 FROM int8_tbl;

SELECT q1 FROM int8_tbl EXCEPT ALL SELECT q1 FROM int8_tbl FOR NO KEY UPDATE;

--
-- Mixed types
--

SELECT f1 FROM float8_tbl INTERSECT SELECT f1 FROM int4_tbl;

SELECT f1 FROM float8_tbl EXCEPT SELECT f1 FROM int4_tbl ORDER BY 1;

--
-- Operator precedence and (((((extra))))) parentheses
--

SELECT q1 FROM int8_tbl INTERSECT SELECT q2 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl;

SELECT q1 FROM int8_tbl INTERSECT (((SELECT q2 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl)));

(((SELECT q1 FROM int8_tbl INTERSECT SELECT q2 FROM int8_tbl))) UNION ALL SELECT q2 FROM int8_tbl;

SELECT q1 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl UNION ALL (((SELECT q2 FROM int8_tbl EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1))); --order none

(((SELECT q1 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl))) EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1;

--
-- Subqueries with ORDER BY & LIMIT clauses
--

-- In this syntax, ORDER BY/LIMIT apply to the result of the EXCEPT
SELECT q1,q2 FROM int8_tbl EXCEPT SELECT q2,q1 FROM int8_tbl
ORDER BY q2,q1;

-- This should fail, because q2 isn't a name of an EXCEPT output column
SELECT q1 FROM int8_tbl EXCEPT SELECT q2 FROM int8_tbl ORDER BY q2 LIMIT 1;

-- But this should work:
SELECT q1 FROM int8_tbl EXCEPT (((SELECT q2 FROM int8_tbl ORDER BY q2 LIMIT 1))) ORDER BY 1;

--
-- New syntaxes (7.1) permit new tests
--

(((((select * from int8_tbl)))));

--
-- Check behavior with empty select list (allowed since 9.4)
-- ORCA should fall back to planner when target list is empty
--

select union select;
select intersect select;
select except select;

-- check hashed implementation
set enable_hashagg = true;
set enable_sort = false;

explain (costs off)
select from generate_series(1,5) union select from generate_series(1,3);
explain (costs off)
select from generate_series(1,5) intersect select from generate_series(1,3);

select from generate_series(1,5) union select from generate_series(1,3);
select from generate_series(1,5) union all select from generate_series(1,3);
select from generate_series(1,5) intersect select from generate_series(1,3);
select from generate_series(1,5) intersect all select from generate_series(1,3);
select from generate_series(1,5) except select from generate_series(1,3);
select from generate_series(1,5) except all select from generate_series(1,3);

-- check sorted implementation
set enable_hashagg = false;
set enable_sort = true;

explain (costs off)
select from generate_series(1,5) union select from generate_series(1,3);
explain (costs off)
select from generate_series(1,5) intersect select from generate_series(1,3);

select from generate_series(1,5) union select from generate_series(1,3);
select from generate_series(1,5) union all select from generate_series(1,3);
select from generate_series(1,5) intersect select from generate_series(1,3);
select from generate_series(1,5) intersect all select from generate_series(1,3);
select from generate_series(1,5) except select from generate_series(1,3);
select from generate_series(1,5) except all select from generate_series(1,3);

reset enable_hashagg;
reset enable_sort;

--
-- Check handling of a case with unknown constants.  We don't guarantee
-- an undecorated constant will work in all cases, but historically this
-- usage has worked, so test we don't break it.
--

SELECT a.f1 FROM (SELECT 'test' AS f1 FROM varchar_tbl) a
UNION
SELECT b.f1 FROM (SELECT f1 FROM varchar_tbl) b
ORDER BY 1;

-- This should fail, but it should produce an error cursor
SELECT '3.4'::numeric UNION SELECT 'foo';

--
-- Test that expression-index constraints can be pushed down through
-- UNION or UNION ALL
--

CREATE TEMP TABLE t1 (a text, b text);
CREATE INDEX t1_ab_idx on t1 ((a || b));
CREATE TEMP TABLE t2 (ab text primary key);
INSERT INTO t1 VALUES ('a', 'b'), ('x', 'y');
INSERT INTO t2 VALUES ('ab'), ('xy');

set enable_seqscan = off;
set enable_indexscan = on;
set enable_bitmapscan = off;

explain (costs off)
 SELECT * FROM
 (SELECT a || b AS ab FROM t1
  UNION ALL
  SELECT * FROM t2) t
 WHERE ab = 'ab';

explain (costs off)
 SELECT * FROM
 (SELECT a || b AS ab FROM t1
  UNION
  SELECT * FROM t2) t
 WHERE ab = 'ab';

--
-- Test that ORDER BY for UNION ALL can be pushed down to inheritance
-- children.
--

CREATE TEMP TABLE t1c (b text, a text);
ALTER TABLE t1c INHERIT t1;
CREATE TEMP TABLE t2c (primary key (ab)) INHERITS (t2);
INSERT INTO t1c VALUES ('v', 'w'), ('c', 'd'), ('m', 'n'), ('e', 'f');
INSERT INTO t2c VALUES ('vw'), ('cd'), ('mn'), ('ef');
CREATE INDEX t1c_ab_idx on t1c ((a || b));

set enable_seqscan = on;
set enable_indexonlyscan = off;

-- NOTE: GPDB planner chooses a seqscan rather than indexscan for t1.
-- This is a side-effect of disabling pull-up for simple union all in GPDB.
explain (costs off)
  SELECT * FROM
  (SELECT a || b AS ab FROM t1
   UNION ALL
   SELECT ab FROM t2) t
  ORDER BY 1 LIMIT 8;

  SELECT * FROM
  (SELECT a || b AS ab FROM t1
   UNION ALL
   SELECT ab FROM t2) t
  ORDER BY 1 LIMIT 8;

reset enable_seqscan;
reset enable_indexscan;
reset enable_bitmapscan;

-- This simpler variant of the above test has been observed to fail differently

create table events (event_id int primary key);
create table other_events (event_id int primary key);
create table events_child () inherits (events);

explain (costs off)
select event_id
 from (select event_id from events
       union all
       select event_id from other_events) ss
 order by event_id;

drop table events_child, events, other_events;

reset enable_indexonlyscan;

-- Test constraint exclusion of UNION ALL subqueries
explain (costs off)
 SELECT * FROM
  (SELECT 1 AS t, * FROM tenk1 a
   UNION ALL
   SELECT 2 AS t, * FROM tenk1 b) c
 WHERE t = 2;

-- Test that we push quals into UNION sub-selects only when it's safe
explain (costs off)
SELECT * FROM
  (SELECT 1 AS t, 2 AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4;

SELECT * FROM
  (SELECT 1 AS t, 2 AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4;

explain (costs off)
SELECT * FROM
  (SELECT 1 AS t, generate_series(1,10) AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4
ORDER BY x;

SELECT * FROM
  (SELECT 1 AS t, generate_series(1,10) AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4
ORDER BY x;

explain (costs off)
SELECT * FROM
  (SELECT 1 AS t, (random()*3)::int AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x > 3;

SELECT * FROM
  (SELECT 1 AS t, (random()*3)::int AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x > 3;

-- Test proper handling of parameterized appendrel paths when the
-- potential join qual is expensive
create function expensivefunc(int) returns int
language plpgsql immutable strict cost 10000
as $$begin return $1; end$$;

create temp table t3 as select generate_series(-1000,1000) as x;
create index t3i on t3 (expensivefunc(x));
analyze t3;

explain (costs off)
select * from
  (select * from t3 a union all select * from t3 b) ss
  join int4_tbl on f1 = expensivefunc(x);
select * from
  (select * from t3 a union all select * from t3 b) ss
  join int4_tbl on f1 = expensivefunc(x);

reset optimizer_trace_fallback;

drop table t3;
drop function expensivefunc(int);

-------------------------------------------------------------------------------
--Test case to check parallel union all with 'json' type 1st column in project list
-------------------------------------------------------------------------------
set optimizer_parallel_union to on;
drop table if exists my_table;
create table my_table ( id serial  primary key, json_data json);
insert into my_table (json_data) values ('{"name": "Name1", "age": 10}');
insert into my_table (json_data) values ('{"name": "Name2", "age": 20}');
insert into my_table (json_data) values ('{"name": "Name3", "age": 30}');
insert into my_table (json_data) values ('{"name": "Name4", "age": 40}');

explain select json_data from my_table  where json_data->>'age' = '30' union all select json_data from my_table where json_data->>'age' = '40' ;
select json_data from my_table  where json_data->>'age' = '30' union all select json_data from my_table where json_data->>'age' = '40' ;

explain select json_data,id from my_table  where json_data->>'age' = '30' union all select json_data,id from my_table where json_data->>'age' = '40' ;
select json_data,id from my_table  where json_data->>'age' = '30' union all select json_data,id from my_table where json_data->>'age' = '40' ;

set optimizer_parallel_union to off;
drop table if exists my_table;
