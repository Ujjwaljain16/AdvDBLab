# SQLite3 vs PostgreSQL — Storage Internals & Query Performance
```LLM was used to polish and structure the content rest all the cmnds and practical was done by me ```

**Name:** Ujjwal Jain \
**Roll Number:** 24bcs10173

---
## Environment Setup

Both databases were run inside Docker containers on Windows using Docker Desktop, which handles the Linux layer transparently via WSL2.

```bash
# SQLite — plain Ubuntu container with sqlite3 installed
docker run -it --name sqlite-lab ubuntu:22.04 bash
apt update && apt install -y sqlite3 wget

# PostgreSQL — official image with credentials passed as env vars
docker run -d --name pg-lab \
  -e POSTGRES_USER=labuser \
  -e POSTGRES_PASSWORD=lab123 \
  -e POSTGRES_DB=labtest \
  -p 5432:5432 \
  postgres:15
```

Sample database for SQLite: **Chinook** (a digital music store dataset with albums, tracks, invoices, customers).  
For PostgreSQL: a synthetic `users` table with **50,000 rows** generated via `generate_series`.

---

## Part 1 — SQLite3

### Dataset Overview

```sql
.tables
-- Album, Artist, Customer, Employee, Genre, Invoice, InvoiceLine, MediaType, Playlist, PlaylistTrack, Track

SELECT COUNT(*) FROM Track;     -- 3503
SELECT COUNT(*) FROM Customer;  -- 59
SELECT COUNT(*) FROM Invoice;   -- 412
SELECT COUNT(*) FROM Album;     -- 347
```

---

### Storage Internals — Pages

```sql
PRAGMA page_size;
PRAGMA page_count;
```

| Metric | Value |
|---|---|
| Page Size | 4096 bytes (4KB) |
| Page Count | 246 pages |
| Calculated Total | 4096 × 246 = **1,007,616 bytes (~984 KB)** |

The 4KB page size is not random it exactly matches the default memory page size of the Linux kernel. SQLite aligns itself with the OS intentionally, so when the kernel loads a page into memory, it maps directly to one SQLite page with no waste.

---

### Other PRAGMA Values

```sql
PRAGMA journal_mode;    -- delete
PRAGMA cache_size;      -- -2000
PRAGMA mmap_size;       -- 0
PRAGMA integrity_check; -- ok
```

`mmap_size = 0` means memory mapping is off by default. `cache_size = -2000` means SQLite will use up to 2000 × page_size = ~8MB of in-memory page cache. `journal_mode = delete` is the classic rollback journal SQLite writes changes to a separate -journal file and deletes it on commit.

---

### Timing Queries — Without mmap

Full table scan on `Track` (3503 rows), run three times:

```bash
time sqlite3 chinook.db "SELECT * FROM Track;"
```

| Run | real | user | sys |
|---|---|---|---|
| Run 1 (cold) | **37ms** | 4ms | 14ms |
| Run 2 (warm) | **16ms** | 4ms | 12ms |
| Run 3 (warm) | **18ms** | 17ms | 0ms |

The drop from 37ms to 16ms between Run 1 and Run 2 without changing anything is the OS page cache doing its job. The database file got loaded into kernel memory on the first read, so subsequent reads never touched disk. That's purely the operating system, not SQLite.

---

### Timing Queries — With mmap Enabled

```bash
time sqlite3 chinook.db "PRAGMA mmap_size=30000000; SELECT * FROM Track;"
```

| Run | real | user | sys |
|---|---|---|---|
| Run 1 | **18ms** | 9ms | 9ms |
| Run 2 | **20ms** | 11ms | 7ms |
| Run 3 | **14ms** | 0ms | 13ms |

With `mmap` on, SQLite maps the entire database file directly into the process's virtual address space. Instead of issuing `read()` syscalls, the process accesses memory addresses that the OS backs with the file — the kernel handles page faults and caching transparently.

The difference here wasn't dramatic — the Chinook database is small enough (~1MB) that it fits comfortably in the OS page cache either way. The real advantage of mmap shows up at scale: a 500MB database where repeated `read()` syscalls would accumulate meaningful overhead. On our small dataset, the times were essentially equivalent, which is itself a valid and honest observation.

---

### Heavier Query — JOIN with Aggregation

```bash
# Without mmap
time sqlite3 chinook.db "SELECT a.Title, COUNT(t.TrackId) as TrackCount \
FROM Album a JOIN Track t ON a.AlbumId = t.AlbumId \
GROUP BY a.AlbumId ORDER BY TrackCount DESC LIMIT 20;"

# With mmap
time sqlite3 chinook.db "PRAGMA mmap_size=30000000; SELECT a.Title, COUNT(t.TrackId) as TrackCount \
FROM Album a JOIN Track t ON a.AlbumId = t.AlbumId \
GROUP BY a.AlbumId ORDER BY TrackCount DESC LIMIT 20;"
```

| Mode | real |
|---|---|
| Without mmap | **3ms** |
| With mmap | **3ms** |

For a JOIN across two small, already-cached tables, the difference was negligible. At this data size the bottleneck is CPU (sorting, grouping), not I/O so mmap contributes nothing here.

---

### SQLite Built-in Timer

```sql
.timer on
SELECT * FROM Track;
-- Run Time: real 0.017  user 0.004210  sys 0.013155

SELECT * FROM Invoice;
-- Run Time: real 0.003  user 0.000471  sys 0.002355

SELECT a.Title, COUNT(t.TrackId) FROM Album a JOIN Track t ON a.AlbumId = t.AlbumId GROUP BY a.AlbumId;
-- Run Time: real 0.003  user 0.000000  sys 0.002886
```

The higher `sys` time vs `user` time on the full Track scan shows the CPU was spending more time in kernel mode (I/O handling) than in user-space computation. For the JOIN query it flips more user time, meaning the CPU was actually computing rather than waiting on I/O.

---

### Process Architecture — `ps aux`

While a query was running in one terminal, I opened a second shell into the same container:

```bash
docker exec -it sqlite-lab bash
ps aux | grep sqlite
```

```
root  2905  0.9  0.0  6532  4608  pts/0  S+  01:07  0:02  sqlite3 chinook.db
root  2919  0.0  0.0  3472  1792  pts/1  S+  01:12  0:00  grep --color=auto sqlite
```

Just two entries: the sqlite3 process and the grep itself. There is no SQLite server, no daemon, no background process. SQLite is a C library that gets loaded directly into whatever process invokes it. The `sqlite3` CLI is just a thin shell embedding that library. This means zero network overhead, zero IPC but also means only one writer can hold the file lock at a time.

---

## Part 2 — PostgreSQL

### Dataset Setup

```sql
CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT NOT NULL,
    city TEXT,
    score FLOAT,
    created_at TIMESTAMP DEFAULT now()
);

\timing on

INSERT INTO users (name, email, city, score)
SELECT
    'User_' || i,
    'user' || i || '@example.com',
    (ARRAY['Mumbai','Delhi','Bangalore','Chennai','Hyderabad'])[1 + (i % 5)],
    random() * 100
FROM generate_series(1, 50000) AS s(i);
-- INSERT 0 50000
-- Time: 113.694 ms
```

50,000 rows in 113ms. That includes page allocation, WAL writes, primary key index maintenance, and buffer flushing — not just raw inserts.

---

### Storage Internals — Blocks

```sql
SHOW block_size;
-- 8192

SELECT relpages FROM pg_class WHERE relname = 'users';
-- 568

SELECT pg_size_pretty(pg_relation_size('users'));
-- 4544 kB

SELECT pg_size_pretty(pg_total_relation_size('users'));
-- 5696 kB
```

| Metric | Value |
|---|---|
| Block Size | 8192 bytes (8KB) |
| Block Count | 568 blocks |
| Calculated Size | 8192 × 568 = **4,653,056 bytes (4544 KB)** |
| Total with Indexes + TOAST | **5696 KB** |

Postgres uses 8KB blocks double SQLite's 4KB. The larger block size makes sense for server workloads: larger fetches per I/O operation means fewer disk reads when scanning big tables. The difference between `pg_relation_size` (4544 KB) and `pg_total_relation_size` (5696 KB) is the primary key index + TOAST overhead — about 1.1MB of infrastructure cost on top of raw data.

---

### Query Timings

```sql
\timing on

SELECT * FROM users LIMIT 100;
-- Time: 0.467 ms

SELECT city, COUNT(*), ROUND(AVG(score)::numeric, 2) FROM users GROUP BY city;
-- Time: 10.834 ms

SELECT * FROM users WHERE score > 95;
-- Time: 4.094 ms

SELECT * FROM users ORDER BY score DESC LIMIT 50;
-- Time: 3.628 ms
```

---

### EXPLAIN ANALYZE — The Query Planner in Action

#### Filter query (before index)

```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE score > 95;
```

```
Seq Scan on users  (cost=0.00..1193.00 rows=2474 width=59)
                   (actual time=0.015..4.073 rows=2507 loops=1)
  Filter: (score > '95'::double precision)
  Rows Removed by Filter: 47493
Planning Time: 0.084 ms
Execution Time: 4.187 ms
```

Postgres scanned all 50,000 rows to find the 2,507 that passed the filter. With no index on `score`, that's the only option.

#### Aggregation query

```sql
EXPLAIN ANALYZE SELECT city, COUNT(*), AVG(score) FROM users GROUP BY city;
```

```
HashAggregate  (cost=1443.00..1443.06 rows=5 width=24)
               (actual time=12.694..12.695 rows=5 loops=1)
  Group Key: city
  Batches: 1  Memory Usage: 24kB
  ->  Seq Scan on users  (actual time=0.009..2.920 rows=50000 loops=1)
Planning Time: 0.088 ms
Execution Time: 12.787 ms
```

Postgres chose a HashAggregate it read all rows in one pass and built a hash map keyed by city. Only 24KB of memory needed for 50,000 rows, because it was only storing 5 distinct city buckets.

#### Sort + Limit

```sql
EXPLAIN ANALYZE SELECT * FROM users ORDER BY score DESC LIMIT 50;
```

```
Limit  (actual time=7.415..7.419 rows=50 loops=1)
  ->  Sort  (Sort Method: top-N heapsort  Memory: 37kB)
      ->  Seq Scan on users  (actual time=0.008..3.044 rows=50000 loops=1)
Planning Time: 0.062 ms
Execution Time: 7.434 ms
```

Instead of sorting all 50,000 rows and taking the top 50, Postgres used a **top-N heapsort** maintains a heap of only 50 elements as it scans, which uses 37KB instead of megabytes.

---

### Index — Before and After

```sql
CREATE INDEX idx_users_score ON users(score);
-- Time: 44.288 ms
```

#### Selective filter — index used

```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE score > 95;
```

```
Bitmap Heap Scan on users  (actual time=0.183..0.847 rows=2507 loops=1)
  Recheck Cond: (score > '95'::double precision)
  Heap Blocks: exact=563
  ->  Bitmap Index Scan on idx_users_score  (actual time=0.132..0.133 rows=2507 loops=1)
Planning Time: 0.136 ms
Execution Time: 0.977 ms
```

Same query: **4.187ms → 0.977ms** — roughly 4x faster. Postgres first built a bitmap of matching row locations from the index, then fetched only those 563 heap blocks.

#### Broad filter — index ignored (intentionally)

```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE score > 10;
```

```
Seq Scan on users  (actual time=0.009..5.259 rows=44974 loops=1)
  Filter: (score > '10'::double precision)
  Rows Removed by Filter: 5026
Planning Time: 0.073 ms
Execution Time: 6.986 ms
```

`score > 10` matches 44,974 out of 50,000 rows (~90%). Even though the index exists, Postgres used Seq Scan. If you fetch 90% of the table, jumping between index pages and heap pages randomly causes more I/O than just reading the table sequentially. The query planner ran the math and made the right call. This was the most interesting observation of the entire lab.

---

### Memory Configuration

```sql
SHOW shared_buffers;        -- 128MB
SHOW work_mem;              -- 4MB
SHOW maintenance_work_mem;  -- 64MB
SHOW effective_cache_size;  -- 4GB
```

`shared_buffers` is Postgres's internal page cache all connections share this pool. `work_mem` is the memory budget per sort or hash operation. `maintenance_work_mem` is for heavier ops like VACUUM and CREATE INDEX. `effective_cache_size` is a planner hint — it tells Postgres how much the OS page cache is likely holding, influencing whether it prefers index scans over seq scans.

---

### Detailed Page Stats

```sql
SELECT
    relname, relpages, reltuples::bigint AS estimated_rows,
    pg_size_pretty(relpages::bigint * 8192) AS estimated_size
FROM pg_class WHERE relname = 'users';
```

```
 relname | relpages | estimated_rows | estimated_size
---------+----------+----------------+----------------
 users   |      568 |          50000 | 4544 kB
```

Postgres estimated exactly 50,000 rows — accurate because stats were fresh from the insert. In practice, stats drift as data changes, which is why autovacuum also runs ANALYZE periodically.

---

### Process Architecture — `ps aux`

```bash
docker exec -it pg-lab bash
ps aux
```

```
postgres   1   postgres                               (main postmaster)
postgres  62   postgres: checkpointer
postgres  63   postgres: background writer
postgres  65   postgres: walwriter
postgres  66   postgres: autovacuum launcher
postgres  67   postgres: logical replication launcher
root       71   psql -U labuser -d labtest            (my connection)
postgres  77   postgres: labuser labtest [local] idle
```

Six background daemons before a single query even runs. Compare this to SQLite's zero. Each daemon exists for a specific reason:

- **checkpointer** — periodically flushes dirty pages from shared_buffers to disk. Without it, a crash would require replaying the entire WAL from the last checkpoint.
- **background writer** — proactively writes dirty pages to disk ahead of checkpoints, so query execution isn't interrupted by sudden I/O bursts.
- **walwriter** — flushes the Write-Ahead Log to disk. This is what makes `COMMIT` durable. Every committed transaction is in the WAL before the client gets a response.
- **autovacuum launcher** — spawns worker processes to run VACUUM and ANALYZE on tables that need it. Without this, dead rows accumulate forever and performance degrades.
- **logical replication launcher** — manages replication slots for streaming changes to replicas.

---

### MVCC — Dead Rows in Practice

```sql
UPDATE users SET score = score + 1 WHERE city = 'Mumbai';
-- UPDATE 10000

SELECT relname, n_live_tup, n_dead_tup, last_autovacuum
FROM pg_stat_user_tables WHERE relname = 'users';
```

```
 relname | n_live_tup | n_dead_tup |        last_autovacuum
---------+------------+------------+-------------------------------
 users   |      50000 |      10000 | 2026-05-05 12:15:26.392436+00
```

After updating 10,000 rows, `n_dead_tup = 10000`. Postgres wrote 10,000 new row versions and marked the old ones dead — it never overwrites in place. This is what makes concurrent reads work without locking: a long-running SELECT sees the old version while an UPDATE writes the new one, because both exist on disk simultaneously.

```sql
VACUUM users;
SELECT n_live_tup, n_dead_tup FROM pg_stat_user_tables WHERE relname = 'users';
-- n_live_tup: 50000 | n_dead_tup: 0
```

VACUUM reclaimed all 10,000 dead row slots. If autovacuum never ran on a write-heavy table, this count would grow indefinitely — pages would fill with ghost data, scans would read useless rows, and indexes would bloat until performance collapsed.

---

## Part 3 — Comparison

| Metric | SQLite3 | PostgreSQL |
|---|---|---|
| Page / Block Size | 4096 bytes (4KB) | 8192 bytes (8KB) |
| Page Count | 246 pages | 568 pages |
| Dataset | 3,503 rows (Track table) | 50,000 rows (users table) |
| Table Size on Disk | ~984 KB (full DB) | 4544 KB (relation) / 5696 KB (total) |
| SELECT * — cold run | 37ms | — |
| SELECT * — warm run | 16–18ms | 0.47ms (LIMIT 100) |
| Full table filter (no index) | ~3ms | 4.187ms |
| Full table filter (with index) | N/A | 0.977ms |
| JOIN + GROUP BY | 3ms | 12.787ms |
| Index creation time | N/A | 44.288ms |
| INSERT 50k rows | N/A | 113.694ms |
| Background processes | 0 | 6 daemons |
| Dead rows after UPDATE | No MVCC | 10,000 dead tuples |
| Dead rows after VACUUM | N/A | 0 |
| Memory mapping | `PRAGMA mmap_size` (manual) | `shared_buffers` (automatic, 128MB) |
| Max concurrent writers | 1 (file-level lock) | Many (row-level MVCC) |
| Architecture | In-process library | Client-server (separate process) |

---

## Analysis

### Why the page size difference makes sense

SQLite's 4KB page aligns with the OS memory page built for embedded environments where memory is tight and alignment with the kernel's unit of work matters most. Postgres's 8KB block is a deliberate trade-off for server workloads: larger blocks mean fewer I/O operations per table scan, and when you have 128MB of shared_buffers anyway, the extra memory cost per block is irrelevant.

### mmap: when it helps and when it doesn't

The honest result from the mmap experiment: on a ~1MB database, it barely made a difference. The OS page cache already keeps the file warm after the first read, with or without mmap. Where mmap would actually matter is at scale a database too large to fit in the OS page cache, accessed frequently enough that individual read() syscalls accumulate into real overhead. For our dataset, both approaches hit the same ceiling: the file fit in memory either way. That's a real finding, not a failure of the experiment.

### The index decision Postgres made correctly

The score > 10 query matching 90% of the table was deliberately ignored by the index and that is the right call. Random I/O (jumping between index leaf pages and heap pages) is slower than sequential I/O (reading the table block by block) when the result set is large. Postgres's query planner estimated the cost of both plans using table statistics and chose the cheaper one. This kind of cost-based reasoning doesn't exist at the same level in SQLite — it has a simpler rule-based planner.

### MVCC overhead is the price of concurrency

10,000 dead rows from a single UPDATE is a lot of ghost data on disk. But that is the cost of MVCC: readers never block writers and writers never block readers, because old versions stay alive until no active transaction needs them anymore. SQLite avoids this overhead entirely but the trade-off is one writer at a time, always. For a personal app: totally fine. For any backend with concurrent writes: not viable.

### Architecture is the root of everything

Every performance characteristic measured in this lab traces back to one fact SQLite is a library, Postgres is a server. SQLite runs inside your process no network, no IPC, no daemon overhead, but also no concurrency and no isolation between the database engine and the application. Postgres runs as its own process ecosystem every connection crosses a process boundary, six daemons run continuously in the background, but in return you get MVCC, crash recovery via WAL, a cost-based query planner, and the ability to serve hundreds of clients simultaneously without any of them knowing the others exist.

---

## Commands Reference

```bash
# SQLite
sqlite3 chinook.db
.tables
PRAGMA page_size;
PRAGMA page_count;
PRAGMA mmap_size;
PRAGMA mmap_size=30000000;
PRAGMA journal_mode;
PRAGMA cache_size;
PRAGMA integrity_check;
.timer on
time sqlite3 chinook.db "SELECT * FROM Track;"
ps aux | grep sqlite

# PostgreSQL
docker exec -it pg-lab psql -U labuser -d labtest
SHOW block_size;
SELECT relpages FROM pg_class WHERE relname = 'users';
SELECT pg_size_pretty(pg_relation_size('users'));
SELECT pg_size_pretty(pg_total_relation_size('users'));
\timing on
EXPLAIN ANALYZE SELECT ...;
CREATE INDEX idx_users_score ON users(score);
SHOW shared_buffers;
SHOW work_mem;
SELECT n_live_tup, n_dead_tup FROM pg_stat_user_tables WHERE relname = 'users';
VACUUM users;
ps aux
```