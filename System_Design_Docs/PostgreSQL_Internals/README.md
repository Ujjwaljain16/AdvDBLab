# PostgreSQL Internal Architecture: From SQL to Durable Bytes

**Name:** Ujjwal Jain | **Roll Number:** 24bcs10173

> _LLM used to polish and structure — all research, exploration, and practical work is my own._

---

## 1. Problem Background

### Why Does PostgreSQL Even Exist?

A beginner's mental model of a database is something like: find a row, change it, write it back. That works fine for a toy project with one user. The moment you have a thousand users, a terabyte of data, and a server that can crash mid-write, you're in completely different territory.

The hard problems a production database has to solve simultaneously are:

- **Disk is brutally slow** — memory access is ~100ns, disk is ~10ms. That's 100,000× slower.
- **Memory is finite** — you cannot cache the whole database in RAM.
- **Concurrency creates conflicts** — readers and writers fighting over the same bytes.
- **Crashes happen** — power cuts, hardware faults, kernel panics. All in-memory state dies instantly.
- **Queries have thousands of possible execution paths** — you need to pick the cheapest one.

PostgreSQL's architecture exists as a principled answer to these five problems. Every design choice — from how pages get cached to how rows store transaction metadata — traces back to one of them.

The four goals PostgreSQL is constantly balancing look like this:

```
          Performance
               |
   Concurrency --- Durability
               |
           Correctness
```

Improving one usually hurts another. PostgreSQL doesn't eliminate these tensions. It manages them.

---

## 2. Architecture Overview

### The Big Picture

A SQL query passes through a pipeline of specialized components before touching storage:

```
SQL Query
    │
  Parser  →  builds a syntax tree
    │
  Analyzer / Rewriter  →  validates names, expands views
    │
  Query Planner  →  picks the cheapest execution plan
    │
  Executor
    │
    ├──── B-Tree Index ────────────┐
    │                              │
    └──────────────────────────────┤
                                   │
                            Buffer Manager
                                   │
                         Shared Buffer Cache
                                   │
                          Storage Manager
                                   │
                              Disk Files
```

### Components at a Glance

| Component | What It Solves |
|-----------|---------------|
| Shared Buffer Manager | Disk is slow → cache pages in memory |
| Clock-Sweep Replacement | Memory is limited → approximate LRU without lock overhead |
| MVCC (Heap Storage) | Reader-writer conflicts → multiple tuple versions coexist |
| Write-Ahead Log (WAL) | Random writes are slow, crashes happen → sequential log + replay |
| B-Tree Index | Full scans are expensive → fast key lookups |
| Cost-Based Optimizer | Many possible plans → pick the cheapest estimated one |

### End-to-End: What Actually Happens on `UPDATE users SET status='ACTIVE' WHERE id=42`

```
Client sends query
  → Parser builds syntax tree
  → Analyzer validates table/column existence
  → Planner selects Index Scan (id has a B-Tree)
  → Executor runs
  → B-Tree traversal finds tuple's physical location (TID)
  → Buffer Manager loads that heap page (or reads from disk)
  → MVCC visibility check passes
  → heap_update() stamps xmax on old tuple, creates new tuple with xmin
  → WAL record generated via XLogInsert()
  → Buffer marked dirty
  → COMMIT triggers XLogFlush() — WAL hits disk
  → Client gets success
  → Background Writer eventually flushes the data page
  → VACUUM later reclaims the dead old tuple
```

The philosophy embedded in this flow: **do the minimum to guarantee correctness at commit time, and push everything else to background processes.**

---

## 3. Internal Design

### 3.1 Buffer Manager — Memory ↔ Disk

**Source:** `src/backend/storage/buffer/bufmgr.c`, `freelist.c`, `buf_internals.h`

PostgreSQL manages its own page cache instead of relying on the OS filesystem cache. The reason: the OS doesn't understand database semantics — it doesn't know that a page is pinned by an active query, or that a modified page must not be evicted before its WAL record is safe.

The shared buffer pool is a fixed array of 8KB frames, each paired with a `BufferDesc` metadata struct:

```
BufferDesc fields:
  ├── BufferTag    → (tablespace OID, DB OID, relation OID, block number)
  │                  uniquely identifies which disk page is cached here
  ├── pin count    → how many backends are currently using this page
  ├── usage_count  → how recently accessed (drives replacement)
  └── dirty flag   → has this page been modified since last write?
```

**Why fixed 8KB frames?** No malloc overhead, no fragmentation. A buffer's memory address is computed directly from its index. This matters when thousands of transactions are hitting the buffer pool per second.

**Atomic state packing:** Rather than locking every buffer descriptor individually, PostgreSQL packs multiple state bits (pin count, usage count, dirty flag, I/O state) into a single integer field that can often be updated with atomic Compare-And-Swap. This eliminates a class of lock contention entirely.

#### Page Read Lifecycle

```
Need a page?
  │
  ├── Buffer Hit:   found in hash table → pin it → return immediately (no disk I/O)
  │
  └── Buffer Miss:  not in cache → StrategyGetBuffer() picks a victim → load from disk → return
```

#### Clock-Sweep: Approximate LRU Without the Lock Tax

Implemented in `freelist.c → StrategyGetBuffer()`.

True LRU requires updating a global ordered list on every access — that's a global lock on every read. At thousands of TPS, this kills performance.

PostgreSQL uses a clock hand sweeping a circular buffer pool instead:

```
For each candidate buffer:
  if usage_count > 0:  decrement usage_count, skip (second chance)
  if usage_count == 0 and pin_count == 0:  evict this page
```

A frequently accessed page gets incremented usage_count, so it survives multiple sweeps. The cost of this approximation is occasionally evicting a page that true LRU would keep — a small accuracy loss for a massive scalability gain.

#### Dirty Pages and the No-Force Policy

When a transaction modifies a buffer, it marks it dirty and generates a WAL record. It does **not** write the page to disk. This is intentional — the **No-Force policy**. The dirty page is eventually written by:

- **Background Writer**: proactively flushes dirty pages so future evictions are fast (clean pages don't need a write before eviction).
- **Checkpointer**: periodically flushes all dirty pages and writes a checkpoint record to WAL, establishing a recovery boundary.

**The WAL Rule (never violate this):** A data page containing changes at LSN X must never reach disk before the WAL record at LSN X is durable. Violating this means a crash could leave disk in a state that WAL can't explain.

---

### 3.2 B-Tree Index — Fast Lookups at Disk Scale

**Source:** `src/backend/access/nbtree/` — `nbtsearch.c`, `nbtinsert.c`, `nbtpage.c`

#### Why Not a Binary Search Tree?

A BST is optimized for memory. A lookup on a million-node BST might traverse 20 levels — 20 random disk reads. At 10ms each, that's 200ms for one lookup. Unacceptable.

A B+ Tree is designed around disk pages. Each node is one 8KB page and stores hundreds of entries. A B-Tree over millions of rows is only 3–4 levels deep. A lookup is 3–4 page reads.

#### Physical Page Layout

Every B-Tree page ends with `BTPageOpaqueData`:

```
+--------------------------------+
| Standard Page Header           |
| Line Pointer Array             |
| Index Tuples (key + TID)       |
| Free Space                     |
| BTPageOpaqueData               |
|   btpo_prev  ← left sibling   |
|   btpo_next  → right sibling  |
|   level, flags                 |
+--------------------------------+
```

#### Why Leaf Pages Form a Doubly-Linked List

This is one of the most elegant decisions in the design. Range queries like `WHERE amount BETWEEN 100 AND 1000` only need to find the first matching leaf, then follow `btpo_next` pointers — no tree re-traversal. It also enables the **Lehman-Yao concurrent B-Tree algorithm**: if a page splits while a reader is on it, the reader detects the split and follows the sibling link to find any keys that moved. No tree-wide lock required.

#### Search: `_bt_search()`

```
Start at root
  → binary search inside page (hundreds of keys per page)
  → follow child pointer to next level
  → repeat until leaf
  → return IndexTuple containing (key, TID)
  → Buffer Manager loads heap page at TID
  → MVCC visibility check
  → return row
```

#### Inserts and Page Splits: `_bt_doinsert()`, `_bt_split()`

Normal insert: find leaf, check free space, shift entries, insert. Cheap.

Split insert: page is full → allocate new page → redistribute entries → update sibling links → push separator key up to parent → write WAL records. If the parent is also full, split propagates upward. In extreme cases, the root splits and the tree gains a level.

**Sequential vs. Random Inserts:** Sequential primary keys (1, 2, 3...) always hit the right-most leaf — excellent cache locality, few splits. Random UUIDs scatter across all leaves — more cache misses, more splits, significantly worse write throughput. This is a real production concern.

---

### 3.3 MVCC and Heap Storage — Concurrency Without Locking Readers

**Source:** `src/backend/access/heap/heapam.c`, `heapam_visibility.c`, `htup_details.h`

#### The Core Idea

Instead of overwriting a row and making readers wait, PostgreSQL keeps the old version alive and creates a new one. Each transaction sees only the version that was current when it started.

```
Before UPDATE:  [xmin=100, xmax=0,   balance=5000]
After  UPDATE:  [xmin=100, xmax=200, balance=5000]  ← old, dead to newer txns
                [xmin=200, xmax=0,   balance=4900]  ← new, live
```

#### HeapTupleHeaderData — Every Row Carries Its History

Every tuple has a header containing:

- `xmin` — XID of the transaction that inserted this version
- `xmax` — XID of the transaction that deleted/updated this version (0 = still live)
- `ctid`  — physical location of the newest version (forms a version chain)
- `infomask` — hint bits for fast visibility checks

#### Visibility: `HeapTupleSatisfiesMVCC()`

When a transaction reads a tuple, it checks its snapshot (captured at transaction start):

1. Is xmin committed and before my snapshot start? If not, I can't see this tuple.
2. Is xmax set, committed, and before my snapshot start? If yes, this version is gone for me.

Two transactions can read the same table and see legitimately different data. A long-running analytics query doesn't block OLTP writers. Writers don't block readers. This is PostgreSQL's biggest concurrency win.

#### The Dead Tuple Problem and VACUUM

Every UPDATE leaves a dead old version on the heap page. A table with heavy writes accumulates "bloat" — physically more pages than logically necessary, larger sequential scans, wasted disk.

**VACUUM** reclaims dead tuples once no active transaction can still need them. **Autovacuum** runs this automatically. The **Visibility Map** tells VACUUM which pages are entirely clean (all-visible), so it can skip them. The **Free Space Map** tracks pages with reusable space for future inserts.

#### HOT Updates — Avoiding Index Churn

Normally, an UPDATE must insert a new index entry (the TID changed). On a write-heavy workload with many updates to non-indexed columns, this is expensive.

**Heap-Only Tuple (HOT)** optimization: if the updated column is not indexed, and the new tuple fits on the same heap page, no new index entry is written. The index still points to the old slot; the old tuple's `ctid` chains to the new one. The executor follows the chain. Significant write amplification reduction in the right workload.

---

### 3.4 Write-Ahead Logging (WAL) — Durability Without Paying Per-Write

**Source:** `src/backend/access/transam/xlog.c`, `xloginsert.c`, `xact.c`

#### The Dilemma

Writing the full 8KB modified page to disk on every commit is too slow (random I/O, one page per transaction). Skipping the write risks data loss on crash. WAL threads the needle.

**The WAL Rule:** Record the *intent* of every change sequentially before the change is considered durable. Disk writes are sequential, which is orders of magnitude faster than random.

#### A Commit's Journey

```
Executor modifies heap page in shared buffer
  → XLogInsert() creates WAL record (compact description of the change)
  → WAL record placed in WAL buffers (shared memory)
  → COMMIT
  → XLogFlush() forces WAL buffers to disk (sequential write)
  → Client receives success
  → Actual heap page stays dirty in memory until Background Writer / Checkpointer
```

The transaction only waited for a sequential WAL write, not a random data page write. This is the STEAL + NO-FORCE buffer policy:

- **STEAL**: dirty pages can be written to disk before their transaction commits (enables buffer reuse)
- **NO-FORCE**: dirty pages don't have to be written at commit time (enables high throughput)

Both require WAL for correctness.

#### Group Commit

If transactions A, B, C all commit at nearly the same moment, PostgreSQL batches their WAL records and does a single `fsync()` instead of three. All three transactions complete safely. This is why high-concurrency workloads see much better throughput than sequential single-transaction benchmarks.

#### Log Sequence Numbers (LSNs) and Checkpoints

Every WAL record has an LSN — a monotonically increasing position in the WAL stream. Data pages store the LSN of the last change applied to them. During recovery, PostgreSQL compares page LSNs to WAL LSNs to skip already-applied changes (idempotent replay).

Without checkpoints, recovery from a crash would mean replaying every WAL record ever written. A **checkpoint** periodically flushes dirty pages and writes a checkpoint record. Recovery starts from the latest checkpoint, not the beginning of time.

#### Crash Recovery

```
System restarts after crash
  → Find last checkpoint record in WAL
  → Replay all WAL records after that checkpoint
  → Reach a consistent committed state
  → Open for business
```

Every committed transaction had its WAL flushed before the client got a success response. Nothing committed is lost.

---

### 3.5 Query Planner — Choosing How to Execute, Not Just What

**Source:** `src/backend/optimizer/planner.c`, `costsize.c`

SQL is declarative. The user says *what* data they want; the planner decides *how* to get it. For a join query, valid strategies include:

- Seq scan table A + Nested Loop Join
- Index scan table B + Hash Join
- Sort both tables + Merge Join
- Various orderings, index combinations, and sub-plan choices

The search space is exponential. PostgreSQL uses a **cost-based optimizer** — it assigns estimated costs to candidate plans and picks the cheapest one. Costs model disk I/O, CPU, and memory relative to each other.

#### Statistics: `pg_statistic` and `ANALYZE`

The planner's cost estimates are only as good as its statistics. `ANALYZE` (run manually or by autovacuum) collects:

- **Row counts and page counts** — how big is this table?
- **Most Common Values (MCV)** — top values and their frequencies (`India → 80% of rows`)
- **Histograms** — distribution of values for range estimates
- **Null fraction** — how many rows have NULL?
- **Correlation** — how well does physical row order match index order?

A query on `WHERE country = 'India'` returning 80% of a large table will probably get a sequential scan — the planner knows from MCV that an index would cause millions of random heap fetches, which is worse.

#### Join Algorithm Selection

| Algorithm | When It Wins |
|-----------|-------------|
| Nested Loop | Small outer table + index on inner table |
| Hash Join | Large equality join, enough memory for hash table |
| Merge Join | Both inputs already sorted, large datasets |

The planner estimates costs for all applicable algorithms and picks the winner. The chosen plan becomes a tree of physical operations that the Executor walks.

#### The Danger of Stale Statistics

```
Planner believes: ~100 rows match
Planner chooses: Nested Loop (cheap for small datasets)
Reality: 10,000,000 rows match
Result: catastrophic performance
```

The planner made a *rational* decision with *wrong information*. Running `ANALYZE` after large data changes and monitoring `EXPLAIN ANALYZE` for row count mismatches is essential production practice.

---

## 4. Design Trade-Offs

### The Master Table

| Problem | PostgreSQL Choice | What You Gain | What It Costs |
|---------|------------------|---------------|---------------|
| Disk is slow | Shared Buffer Pool | Eliminates most disk I/O | Memory overhead, replacement complexity |
| Exact LRU is lock-heavy | Clock-Sweep | Scales to thousands of connections | Occasionally evicts a "wrong" page |
| Reader-writer conflicts | MVCC tuple versioning | Non-blocking reads | Dead tuple bloat, VACUUM overhead |
| Random writes are slow | WAL + delayed page flush | Fast commits, crash safety | Extra logging, recovery infrastructure |
| All queries differ | Cost-based optimizer | Adaptive plan selection | Requires fresh statistics, complex planner |
| Index writes are expensive | HOT updates | Reduces index maintenance | Only works when conditions align |
| Long recovery after crash | Checkpoints | Bounded recovery time | Periodic background I/O overhead |

### PostgreSQL vs. Undo-Based MVCC (e.g., InnoDB)

PostgreSQL stores old tuple versions directly in the heap alongside new versions. InnoDB stores only the current version in the clustered index and keeps old versions in a separate undo log.

| | PostgreSQL (Append-to-Heap) | InnoDB (Undo Log) |
|-|---------------------------|-------------------|
| Reader path | Read heap tuple directly | May follow undo chain |
| Table bloat | Yes, requires VACUUM | Less bloat |
| Long-running txn impact | Old versions stay in heap | Undo log grows |
| Implementation | Simpler visibility logic | More complex undo management |

Neither is strictly better — they're engineering choices with different performance profiles for different workloads.

### Clock-Sweep vs. True LRU

| | True LRU | Clock-Sweep |
|-|----------|-------------|
| Eviction accuracy | Perfect | Approximate |
| Synchronization cost | Global lock on every access | Mostly lock-free |
| Scalability | Poor at high concurrency | Excellent |

PostgreSQL deliberately accepts the approximation. At 10,000 TPS, the performance difference of LRU's global lock would dwarf any benefit from perfect eviction decisions.

---

## 5. Experiments / Observations

### Using `EXPLAIN ANALYZE` to See Internals Live

```sql
EXPLAIN ANALYZE
SELECT c.name, o.order_id, o.amount
FROM customers c
JOIN orders o ON c.customer_id = o.customer_id
WHERE c.country = 'India';
```

A realistic output:

```
Hash Join  (cost=200..500 rows=1000)  (actual rows=120000 time=15ms)
  Hash Cond: (o.customer_id = c.customer_id)
  ->  Seq Scan on orders  (actual rows=500000)
  ->  Hash
        ->  Index Scan on customers using customers_country_idx
              Filter: (country = 'India')
              actual rows=120000
```

**What this reveals about internals:**

1. **Planner chose Seq Scan on orders** — even with indexes available, the planner estimated that the fraction of orders rows needed was high enough that sequential access beats random index lookups.

2. **Index Scan on customers** — `country = 'India'` is selective enough (relative to `orders`) for an index to be worth the random heap fetches.

3. **Hash Join over Nested Loop** — the hash table fits in `work_mem`; Hash Join wins over Nested Loop when both sides are large.

4. **`rows=1000` estimated vs `rows=120000` actual** — a 120× mismatch. This is stale statistics. The planner made rational decisions with wrong numbers. Running `ANALYZE customers` would fix this. With accurate statistics, the planner might reconsider the join strategy entirely.

**Key observation:** The existence of an index does not mean PostgreSQL uses it. The planner is cost-aware, not index-obligated. After manually running `ANALYZE` and re-running the query, the estimated row counts tightened significantly and the planner reconsidered the join order.

### Source Code Observation: Clock-Sweep in Action

Reading `freelist.c → StrategyGetBuffer()` makes the algorithm concrete. The `nextVictimBuffer` global clock hand advances in a simple loop. When `usage_count` hits zero and `pin_count` is zero, that buffer is selected. The tight loop and absence of a global LRU list are immediately visible in the code — it's optimized for minimal synchronization overhead, not maximum cache accuracy.

---

## 6. Key Learnings

**1. Caching is a concurrency problem, not just a memory problem.**
PostgreSQL's buffer manager doesn't just cache pages — it coordinates thousands of concurrent sessions accessing the same cache without serializing on a global lock. Clock-sweep exists because of this constraint, not despite it.

**2. Concurrency requires keeping history.**
MVCC is the right mental model for understanding why dead tuples exist, why VACUUM is not optional, and why long-running transactions can cause table bloat. The old version of a row stays alive as long as any active transaction might need it. This is a deliberate trade-off, not a bug.

**3. Durability ≠ synchronous writes.**
WAL separates *logical durability* (the change is in a durable sequential log) from *physical page persistence* (the 8KB page has reached disk). The client gets a success response after WAL flush, not after page flush. Understanding this distinction demystifies checkpoint behavior and crash recovery.

**4. A good optimizer is only as good as its statistics.**
`EXPLAIN ANALYZE` regularly shows estimated vs. actual row count mismatches. The planner can't know the future — it knows what `ANALYZE` last measured. This makes `pg_statistic` maintenance a correctness issue, not just a performance tuning concern.

**5. Every optimization is a new problem.**
Adding a buffer cache requires a replacement policy. The replacement policy introduces approximate eviction. Approximate eviction sometimes requires manual cache hints. Keeping tuple versions requires cleanup. Cleanup requires statistics on what's visible. Every solution in PostgreSQL's architecture is the answer to a problem introduced by a previous solution. The complexity is load-bearing.

**6. The unit of production database design is the trade-off.**
PostgreSQL doesn't find optimal solutions. It finds the best-known solutions given constraints — memory limits, disk latency characteristics, CPU cache behavior, lock contention profiles, and expected workload patterns. Studying it is less about learning algorithms and more about learning *why* those algorithms were chosen over their alternatives.

---

## Source Code Map

| Subsystem | Location | Key Files / Functions |
|-----------|----------|-----------------------|
| Buffer Manager | `src/backend/storage/buffer/` | `bufmgr.c`, `freelist.c`, `buf_internals.h` |
| Clock-Sweep | `freelist.c` | `StrategyGetBuffer()`, `nextVictimBuffer` |
| B-Tree Index | `src/backend/access/nbtree/` | `_bt_search()`, `_bt_doinsert()`, `_bt_split()` |
| MVCC / Heap | `src/backend/access/heap/` | `heap_update()`, `HeapTupleHeaderData`, `HeapTupleSatisfiesMVCC()` |
| WAL | `src/backend/access/transam/` | `XLogInsert()`, `XLogFlush()`, `xlog.c` |
| Query Optimizer | `src/backend/optimizer/` | `planner.c`, `costsize.c` |

---

## References

1. PostgreSQL Official Documentation — https://www.postgresql.org/docs/
2. PostgreSQL Source Code — https://github.com/postgres/postgres
3. Hellerstein, Stonebraker, Hamilton — *Architecture of a Database System*