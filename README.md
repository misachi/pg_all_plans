# pg_show_plans
Show all plans generated by the optimizer for a query, if more than one plan was considered by the optimizer

Example

Create table `bar` table with 2 columns(`id` and `descr`). There is an index on the `id` column. The table will have about 1M rows.

```
CREATE TABLE bar(id INT PRIMARY KEY, descr TEXT); -- New table
INSERT INTO bar SELECT i, 'Hello' FROM generate_series(100001, 1000000) AS i; -- populate table
```

Run `EXPLAIN` command to get the execution plan. Search for a random word in the `descr` field(this touches all the rows in the table since we are certain the search key does not exist)

```
postgres=# SELECT * FROM show_all_plans('EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM bar WHERE descr=''shoe''');
                                                   query_plans
-----------------------------------------------------------------------------------------------------------------
 -------------------------------Plan 1-------------------------------
 Gather  (cost=1000.00..10633.40 rows=1 width=8) (actual time=47.286..48.178 rows=0 loops=1)
   Workers Planned: 2
   Workers Launched: 2
   Buffers: shared hit=480 read=3945
   ->  Parallel Seq Scan on bar  (cost=0.00..9633.30 rows=1 width=8) (actual time=44.317..44.317 rows=0 loops=3)
         Filter: (descr = 'shoe'::text)
         Rows Removed by Filter: 333331
         Buffers: shared hit=480 read=3945
 Planning:
   Buffers: shared hit=46
 Planning Time: 0.237 ms
 Execution Time: 48.212 ms

 -------------------------------Plan 2-------------------------------
 Seq Scan on bar  (cost=0.00..16924.93 rows=1 width=8) (actual time=112.259..112.259 rows=0 loops=1)
   Filter: (descr = 'shoe'::text)
   Rows Removed by Filter: 999994
   Buffers: shared hit=576 read=3849
 Planning:
   Buffers: shared hit=529 read=3945
 Planning Time: 48.513 ms
 Execution Time: 112.278 ms

(24 rows)
```

Two scan plans are considered, one involves a parallel sequential scan (with 2 workers) on the table while another is a normal sequential scan. In the end, only the parallel sequential scan is preferred since it deemed to provide the cheapest path.

In some cases, there is only a single path considered when the optimizer "thinks" that no other path could be cheaper than it, when all costs are taken into account(sort order, index, limit etc). Below is one such case where only the index scan plan is considered

```
postgres=# SELECT * FROM show_all_plans('EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM bar WHERE id=1000002');
                                                 query_plans
--------------------------------------------------------------------------------------------------------------
 -------------------------------Plan 1-------------------------------
 Index Scan using bar_pkey on bar  (cost=0.42..8.44 rows=1 width=8) (actual time=0.926..0.927 rows=0 loops=1)
   Index Cond: (id = 1000002)
   Buffers: shared read=3
 Planning:
   Buffers: shared hit=39 read=20
 Planning Time: 7.003 ms
 Execution Time: 1.480 ms

(9 rows)
```
