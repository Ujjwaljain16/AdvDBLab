# MySQL InnoDB Storage Engine: Architecture, MVCC, and Design Trade-offs

**Name:** Ujjwal Jain | **Roll Number:** 24bcs10173

> _LLM used to polish and structure — all research, exploration, and practical work is my own._

---

## 1. Problem Background

### Why Does InnoDB Exist?

MySQL originally shipped with MyISAM — a simple, fast storage engine that had one fatal flaw: no transactions. No rollback, no crash safety, no ACID. For toy apps, fine. For a banking system processing thousands of concurrent transfers, catastrophic.

InnoDB was built to answer the question: **how do you get ACID guarantees and high concurrency without making every operation painfully slow?**

The workload it's designed for is OLTP — Online Transaction Processing. Think e-commerce checkouts, bank transfers, user authentication. Millions of small, fast, concurrent transactions hitting the same tables. The engineering constraints this creates:

- **Reads must not block writes, and writes must not block reads** — or throughput collapses
- **Commits must be fast** — a user waiting 200ms for "your order was placed" is a bad experience
- **Crashes happen** — power cut mid-transaction must not corrupt data
- **Primary-key lookups must be near-instant** — these dominate OLTP query patterns

InnoDB's answer to all four is three architectural bets:

1. **Clustered storage** — the primary key B+Tree *is* the table; the row lives in the leaf page
2. **Undo-based MVCC** — old row versions live in a separate undo log, keeping the table compact
3. **Write-Ahead Redo Logging** — sequential redo records hit disk at commit; actual page writes happen later

Every other complexity in InnoDB traces back to these three decisions.

---

## 2. Architecture Overview

### The Big Picture

```
SQL Query
    │
  MySQL Server (Parser, Optimizer, Executor)
    │
  InnoDB Engine
    │
    ├──── Buffer Pool ────────────────── Cached pages (data, index, undo)
    │
    ├──── Clustered B+Tree Storage ───── Primary index = table rows
    │
    └──── Transaction Manager
              ├── Undo Logs    → MVCC snapshots + rollback
              ├── Redo Logs    → crash recovery + durability
              └── Lock Manager → row locks + gap locks
    │
  Disk Data Files (.ibd)
```

### Components at a Glance

| Component | What It Solves |
|-----------|---------------|
| Clustered B+Tree | Primary-key lookups and range scans are O(log n) with zero extra heap fetch |
| Buffer Pool | Disk is 100,000× slower than RAM → cache hot pages |
| Undo Log | MVCC needs old row versions → store them outside the table |
| Redo Log | Commits must survive crashes → WAL before page flush |
| Lock Manager | Writers still conflict → row-level + gap locks |

### End-to-End: What Happens on `UPDATE users SET balance = balance - 100 WHERE id = 42`

```
Clustered B+Tree traversal finds row at leaf page
  → Buffer Pool loads that page (or reads from disk)
  → Transaction Manager creates undo record (old balance = 5000)
  → Row modified in-place inside buffer pool (balance = 4900)
  → DB_TRX_ID and DB_ROLL_PTR updated in row header
  → Redo record generated (describes the change)
  → Row lock acquired on id=42
  → COMMIT: redo log flushed to disk (sequential write)
  → Client receives success
  → Dirty data page written to disk later by background flusher
  → Purge thread eventually discards undo record (when no txn needs it)
```

The philosophy: **do the minimum at commit time (flush redo log), defer everything expensive (page writes, undo cleanup) to background processes.**

---

## 3. Internal Design

### 3.1 Clustered Index — The Table Is the Index

The single most important design decision in InnoDB:

> **The primary key B+Tree leaf pages store the actual row data.** There is no separate heap file.

```
Root Page
    │
Internal Pages  (key ranges + child pointers)
    │
Leaf Pages  (complete row data, ordered by primary key)
```

A query like `SELECT * FROM users WHERE id = 100` does a single B+Tree traversal and arrives directly at the row. No extra heap lookup. This is what makes primary-key lookups so fast.

Range scans also benefit — rows with adjacent primary keys are stored on adjacent pages, so `WHERE id BETWEEN 1000 AND 2000` reads pages sequentially instead of chasing random pointers.

**The flip side:** the table's physical layout is determined by the primary key. Random keys (UUIDs) mean random inserts throughout the B+Tree → frequent page splits → fragmentation → degraded range scan performance. This is why InnoDB wants short, sequential, stable primary keys. `AUTO_INCREMENT` integers are ideal. UUIDs are not.

---

### 3.2 Secondary Indexes — The Double Lookup Problem

If the clustered index stores the full row, what does a secondary index store?

```
Secondary Index Leaf:  (indexed_column_value, primary_key_value)
```

A secondary index lookup for `WHERE email = 'alice@example.com'` runs:

```
1. Search email secondary index → find primary key = 42
2. Search clustered index with pk=42 → fetch full row
```

Two B+Tree traversals per lookup. This is called a **double lookup** (or "bookmark lookup").

**Why not store the full row in every secondary index?** Because then every UPDATE to any column would require modifying every secondary index, and the indexes would be massive. Storing only the primary key keeps secondary indexes small and updates cheap.

**Implication for schema design:**
- Large primary keys (e.g., 128-byte UUIDs) inflate every secondary index because each entry carries a copy of the PK
- **Covering indexes** — secondary indexes that include all columns a query needs — eliminate the second traversal entirely: `CREATE INDEX idx ON users(email, name, balance)` can answer `SELECT name, balance WHERE email=?` without touching the clustered index

---

### 3.3 Buffer Pool — Making Disk Rare

The Buffer Pool is InnoDB's in-memory page cache. It stores data pages, index pages, undo pages, and metadata. The goal: almost every read hits memory, not disk.

```
Request page:
  ├── Buffer Pool HIT  → return immediately (no disk I/O)
  └── Buffer Pool MISS → read from disk, insert into pool, return
```

Modified pages become **dirty** and are not immediately written to disk — this is the No-Force policy, same as PostgreSQL. Background flusher threads write dirty pages asynchronously.

InnoDB uses a modified LRU with a **young/old sublist** split. Newly loaded pages enter the "old" sublist. Only after they're accessed again do they move to the "young" (hot) sublist. This prevents full-table scans (which touch every page once) from evicting frequently-used hot pages — a real problem that naive LRU suffers from.

---

### 3.4 MVCC via Undo Logs — History Lives Outside the Table

**The core problem:** Transaction A starts. Transaction B updates a row. Transaction A then reads that row — it should see the version that existed when it started, not B's uncommitted (or even committed) change.

InnoDB's solution: keep the current row in the clustered index, and store the previous version in the undo log.

**Every InnoDB row has two hidden system columns:**

| Column | Purpose |
|--------|---------|
| `DB_TRX_ID` | Transaction ID of the last transaction to modify this row |
| `DB_ROLL_PTR` | Pointer to the undo log record containing the previous version |

When transaction B updates a row:
```
Before:  [id=42, balance=5000, DB_TRX_ID=100, DB_ROLL_PTR=nullptr]

Undo record created:  "TRX 100 had balance=5000"

After:   [id=42, balance=4900, DB_TRX_ID=200, DB_ROLL_PTR → undo record]
```

When transaction A needs the old version:
```
Read current row → DB_TRX_ID=200, which is newer than A's snapshot
  → Follow DB_ROLL_PTR to undo record
  → Find version with DB_TRX_ID=100, which is visible to A
  → Return balance=5000
```

If the row has been updated many times, A might need to walk a chain of undo records. A long-running transaction reading from a table under heavy write activity can traverse a very long undo chain — this is InnoDB's **long-running transaction penalty**, equivalent to PostgreSQL's table bloat problem.

**Undo log cleanup (Purge):** Once no active transaction can possibly need an old version, the purge thread discards that undo record. If a long-running transaction holds a snapshot open, undo records can't be purged — undo log grows, purge falls behind, performance degrades. This is why long-running transactions are a production concern in InnoDB, just as they cause VACUUM lag in PostgreSQL.

---

### 3.5 Undo Log vs. Redo Log — Backward vs. Forward

These solve completely different problems and are often confused:

| | Undo Log | Redo Log |
|-|----------|----------|
| Question | "How do I go back?" | "How do I go forward after a crash?" |
| Used for | Rollback + MVCC snapshots | Crash recovery + durability |
| Written when | Before the row is modified | After the row is modified in memory |
| Discarded when | No active txn needs that version | After the corresponding page reaches disk |

**Redo Log (Write-Ahead Log):**

```
Modify page in Buffer Pool (in memory)
  → Generate redo record (compact description of the change)
  → Place in redo log buffer
  → COMMIT: flush redo log buffer to disk (sequential I/O)
  → Return success to client
  → Data page hits disk later (background flush)
```

Commits are fast because they only wait for a sequential redo log flush, not a random data page write. After a crash, InnoDB replays redo records to bring pages to their committed state, then uses undo records to roll back any transactions that didn't commit. Both logs participate in recovery.

---

### 3.6 Concurrency Control — MVCC + Locks Together

MVCC handles read consistency — readers see a snapshot and never block writers. But concurrent writers still conflict and need coordination.

**Row-level locks:** Each `UPDATE` or `DELETE` acquires an exclusive lock on the affected row. Other transactions can modify different rows concurrently — this is the "high concurrency" advantage over table-level locking.

**Gap locks and Next-Key locks:** Consider:
```sql
SELECT * FROM users WHERE age BETWEEN 20 AND 30 FOR UPDATE;
```
Without gap locks, another transaction could insert `age=25` right after this read, causing a **phantom read** when this transaction reads again. InnoDB prevents this with **next-key locks** — locking both the matching rows *and* the gaps between them. This is how InnoDB provides Repeatable Read isolation by default.

The cost: gap locks reduce concurrency for range-heavy workloads. Two transactions updating different rows in the same index range can still block each other due to gap lock conflicts. Read Committed isolation level drops gap locks (only row locks) for better concurrency, at the cost of allowing phantoms.

---

## 4. Design Trade-Offs

### The Master Table

| Decision | What You Gain | What It Costs |
|----------|--------------|---------------|
| Clustered primary index | Instant PK lookup, zero heap fetch, great range scans | Secondary lookups = 2 B+Tree traversals; PK changes require row relocation |
| Undo-based MVCC | Table stays compact, no dead-tuple bloat | Long-running txns → long undo chains → slow old-snapshot reads |
| Redo WAL + No-Force | Fast commits (sequential write only) | Recovery complexity; crash replay needed |
| Row-level locking | High concurrency between unrelated writers | Lock tracking overhead; gap locks reduce range-write concurrency |
| Next-key locking | Repeatable Read without serializable cost | Can cause unexpected blocking in range workloads |
| Buffer Pool LRU-young/old | Scan-resistant hot page retention | More complex implementation than simple LRU |
| Secondary indexes store PK | Small indexes, cheap non-PK column updates | Large PKs inflate all secondary indexes |

### InnoDB vs. PostgreSQL — Two MVCC Philosophies

Both provide MVCC. The difference is where old row versions live:

| | InnoDB | PostgreSQL |
|-|--------|-----------|
| Current version location | Clustered index leaf page | Heap page |
| Old version location | Separate undo log | Same heap page, nearby |
| Table bloat | No (old versions leave the table) | Yes (dead tuples accumulate) |
| Old-version read cost | Walk undo chain | Read nearby heap tuple |
| Cleanup mechanism | Purge thread | VACUUM / Autovacuum |
| Long txn penalty | Undo chain grows, purge falls behind | Dead tuples accumulate, VACUUM can't reclaim |
| Primary key | Clustered (row stored in index) | Heap-based (index stores TID, extra lookup) |

Neither is universally better. InnoDB wins on primary-key access patterns and table compactness. PostgreSQL wins on simpler visibility logic and no undo-chain traversal for old snapshots. The right choice depends on workload.

---

## 5. Experiments / Observations

### Experiment 1 — Primary Key Lookup vs. Secondary Index Lookup

```sql
-- Primary key: single B+Tree traversal
EXPLAIN SELECT * FROM users WHERE id = 100;

-- Secondary index: double lookup
EXPLAIN SELECT * FROM users WHERE email = 'alice@example.com';
```

**Observed in EXPLAIN output:**

The PK query shows `type: const` or `ref` — InnoDB goes directly to the clustered index leaf. The email query shows an extra `Using index condition` or triggers a `key: email_idx` → then fetches from the clustered index. Two separate index operations are visible in the execution plan.

**What this reveals about internals:** The secondary index only contains `(email, primary_key)`. The full row must come from the clustered index. This is the double-lookup materializing in an observable execution plan.

---

### Experiment 2 — Covering Index Eliminating the Double Lookup

```sql
-- Without covering index: double lookup
EXPLAIN SELECT name, balance FROM users WHERE email = 'alice@example.com';

-- With covering index: single pass
CREATE INDEX idx_email_covering ON users(email, name, balance);
EXPLAIN SELECT name, balance FROM users WHERE email = 'alice@example.com';
```

**Observed:** The second query shows `Using index` in the Extra column — InnoDB found everything it needed in the secondary index and never touched the clustered index. The double-lookup is eliminated.

**What this reveals:** Covering indexes are InnoDB's primary tool for making secondary index access as cheap as primary key access. The engineering insight is that index design in InnoDB is about *what data lives where*, not just *which queries have indexes*.

---

### Experiment 3 — Sequential vs. UUID Primary Keys

```sql
-- Sequential (AUTO_INCREMENT)
CREATE TABLE orders_seq (id BIGINT AUTO_INCREMENT PRIMARY KEY, ...);

-- Random (UUID)
CREATE TABLE orders_uuid (id CHAR(36) PRIMARY KEY, ...);
```

Insert 1 million rows into both, observe:

- `orders_seq`: inserts are consistently fast; B+Tree grows right-ward; page splits rare
- `orders_uuid`: insert throughput degrades as table grows; SHOW ENGINE INNODB STATUS shows increasing page splits; secondary indexes are 16× larger per entry (36 bytes vs 8 bytes for BIGINT)

**What this reveals:** The primary key isn't just a logical identifier in InnoDB — it determines physical storage layout. UUID PKs scatter inserts across the entire B+Tree, causing random I/O patterns that defeat the Buffer Pool (each insert is likely a cache miss for the target leaf page). Sequential keys keep inserts at the right-most leaf — mostly in cache, few splits.

---

### Experiment 4 — MVCC Snapshot Observation

```sql
-- Session A
START TRANSACTION;
SELECT balance FROM users WHERE id = 42;  -- sees 5000

-- Session B (concurrent)
UPDATE users SET balance = 4000 WHERE id = 42;
COMMIT;

-- Session A (still running)
SELECT balance FROM users WHERE id = 42;  -- still sees 5000 (snapshot isolation)
COMMIT;
```

**What this reveals:** Session A's second read returns 5000 even though the committed value on disk is now 4000. InnoDB served the old value by following the `DB_ROLL_PTR` from the current row back to the undo record. This is undo-based MVCC in action — no blocking, no dirty read, consistent snapshot maintained entirely through hidden row metadata.

---

## 6. Key Learnings

**1. The primary key is a physical layout decision, not just an identifier.**
In InnoDB, choosing a primary key decides where rows live on disk, how many page splits happen, how large secondary indexes are, and how well the Buffer Pool absorbs writes. Auto-increment integers are not a default for laziness — they're correct for InnoDB's architecture.

**2. MVCC can be implemented at least two different ways with fundamentally different trade-offs.**
InnoDB keeps the table compact and externalizes history (undo log). PostgreSQL keeps history in the table and cleans it up (VACUUM). Both provide consistent snapshots. The difference shows up in table bloat, old-snapshot read cost, and the nature of background maintenance work.

**3. Undo and redo logs are not redundant — they're orthogonal.**
Undo answers "what was the previous state?" (rollback + MVCC). Redo answers "what changes must survive a crash?" (durability + recovery). A robust transactional engine needs both. They're used together during crash recovery: redo brings the database forward to the last committed state, then undo rolls back any transactions that hadn't committed when the crash occurred.

**4. MVCC solves read-write conflicts but not write-write conflicts.**
Readers see old versions without blocking. Writers modifying the *same* row must still serialize. InnoDB's gap locks add a further constraint: writers to the same index *range* can block each other even on different rows. Isolation level selection (Read Committed vs Repeatable Read) is a concurrency vs. consistency trade-off with real throughput implications.

**5. High performance is about deferring expensive work, not eliminating it.**
InnoDB commits quickly by flushing only the redo log. The actual data page write, undo log purge, and B+Tree maintenance happen asynchronously. This pattern — do the minimum for correctness at commit time, defer everything else — appears identically in PostgreSQL's WAL design, and is a general principle of high-performance database engineering.

**6. Long-running transactions are a systems problem, not just an application concern.**
In InnoDB, a transaction holding an old snapshot prevents undo record purge. In PostgreSQL, it prevents VACUUM from reclaiming dead tuples. In both systems, a single forgotten `COMMIT` can silently degrade performance across the entire database. This is why connection poolers, query timeouts, and transaction monitoring are production necessities.

---

## Source Code / Architecture Reference Map

| Component | InnoDB Location | Key Structures |
|-----------|----------------|----------------|
| Clustered Index | `storage/innobase/btr/` | B+Tree traversal, page splits |
| Buffer Pool | `storage/innobase/buf/` | `buf_pool_t`, LRU young/old sublists |
| Undo Log | `storage/innobase/trx/trx0undo.cc` | `trx_undo_t`, undo record chains |
| Redo Log | `storage/innobase/log/` | `log_t`, WAL write + flush |
| MVCC Visibility | `storage/innobase/read/read0read.cc` | Snapshot creation, `ReadView` |
| Lock Manager | `storage/innobase/lock/` | Row locks, gap locks, next-key locks |

---

## References

1. MySQL 8.0 Reference Manual — InnoDB Storage Engine — https://dev.mysql.com/doc/
2. Silberschatz, Korth, Sudarshan — *Database System Concepts*
3. Hellerstein, Stonebraker, Hamilton — *Architecture of a Database System*
4. MySQL Source Code — https://github.com/mysql/mysql-server
5. InnoDB Technical Papers and Internals Documentation