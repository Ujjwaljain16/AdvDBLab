# PostgreSQL Internal Architecture: From SQL Queries to Durable Bytes
```LLM was used to polish and structure the content rest all the research exploration and practical was done by me ```

**Name:** Ujjwal Jain \
**Roll Number:** 24bcs10173

---

## 1. Introduction — Understanding Why PostgreSQL Is Complex

At first glance, a database appears to perform a simple task: store some data, retrieve it when asked, and update it when required.

A beginner might imagine a database update as:

```
Find row on disk
        |
        |
Modify bytes
        |
        |
Write back to disk
```

This approach works for a toy database with a single user. However, a production database faces much harder engineering problems:

* Thousands of users may access the same data simultaneously.
* Disk operations are millions of times slower than CPU operations.
* The system may crash at any moment due to power failures or hardware faults.
* Complex queries may have thousands of possible execution strategies.
* Memory is limited and cannot hold the entire database.
* Writes must be durable without making every transaction extremely slow.

Therefore, a production database is fundamentally a system that continuously balances four competing goals:

```
             Performance
                  |
                  |
      Concurrency ----- Durability
                  |
                  |
              Correctness
```

PostgreSQL's architecture is a collection of engineering decisions made to balance these competing requirements.

Rather than solving every problem with a single mechanism, PostgreSQL divides responsibilities across specialized subsystems:

| Challenge                                     | PostgreSQL Solution                                                            |
| --------------------------------------------- | ------------------------------------------------------------------------------ |
| Disk is slow                                  | Shared Buffer Manager caches pages in memory                                   |
| Memory is limited                             | Clock-sweep replacement approximates LRU without high synchronization overhead |
| Readers and writers conflict                  | MVCC allows multiple tuple versions to coexist                                 |
| Random writes are expensive                   | Write-Ahead Logging converts changes into efficient sequential logs            |
| Crashes may lose in-memory changes            | WAL replay and checkpoints recover a consistent state                          |
| Queries have multiple execution possibilities | Cost-based optimizer chooses the estimated cheapest plan                       |

A major lesson from studying PostgreSQL is that every optimization introduces a new challenge.

For example:

* Caching pages improves performance, but now PostgreSQL needs a replacement policy.
* Keeping old tuple versions improves concurrency, but now dead tuples must be cleaned by VACUUM.
* Delaying data page writes improves throughput, but now crash recovery requires WAL.
* Choosing execution plans dynamically improves performance, but requires accurate statistics.

PostgreSQL does not attempt to eliminate these trade-offs. It carefully manages them.

This philosophy explains why PostgreSQL's codebase has evolved for decades and why it remains one of the most widely used relational database systems.

---

# 2. PostgreSQL Architecture Overview

A SQL statement passes through several stages before becoming a physical operation on storage.

A simplified execution pipeline is shown below:

```
                    SQL Query
                        |
                        |
                    Parser
                        |
                        |
              Analyzer / Rewriter
                        |
                        |
              Query Planner / Optimizer
                        |
                        |
                    Executor
                        |
        +---------------+---------------+
        |                               |
        |                               |
   B-Tree Index                   Heap Storage
        |                               |
        +---------------+---------------+
                        |
                  Buffer Manager
                        |
              Shared Buffer Cache
                        |
                   Storage Manager
                        |
                     Disk Files
```

Each component solves a different problem.

---

## Parser

The parser receives the SQL text and converts it into an internal syntax tree.

Example:

```sql
SELECT name
FROM users
WHERE id = 42;
```

The parser identifies:

* The operation being requested (`SELECT`)
* Relations involved (`users`)
* Columns requested (`name`)
* Conditions (`id = 42`)

However, the parser does not understand whether the table actually exists or whether the query is efficient.

---

## Analyzer and Query Rewriter

The analyzer resolves names using PostgreSQL system catalogs.

For example, a table name like:

```
users
```

is internally mapped to a relation identifier (OID).

The analyzer also validates:

* Column names
* Data types
* Permissions
* Function signatures

The query rewriter performs transformations such as:

* Expanding views into underlying queries.
* Applying rewrite rules.

The result is a logical query tree ready for optimization.

---

## Query Planner and Optimizer

The planner answers a very important question:

> "There are many ways to execute this query. Which one is expected to be the cheapest?"

Consider a simple join:

```sql
SELECT *
FROM orders o
JOIN customers c
ON o.customer_id = c.id;
```

Possible strategies include:

* Sequential scan both tables.
* Use an index on `customer_id`.
* Perform a Nested Loop Join.
* Build a Hash Join.
* Sort both relations and use a Merge Join.

The planner generates possible execution paths and estimates their cost using statistics collected in `pg_statistic`.

The chosen plan is not guaranteed to be perfect.

It is the plan that PostgreSQL predicts to be the cheapest based on available information.

This is why outdated statistics can cause poor performance.

---

# 3. Source Code Exploration Map

Instead of treating PostgreSQL as a black box, the internal implementation was explored through its source code.

The following components were particularly studied:

| Component           | Source Location               | Important Structures / Functions                                            |
| ------------------- | ----------------------------- | --------------------------------------------------------------------------- |
| Buffer Manager      | `src/backend/storage/buffer/` | `ReadBuffer()`, `ReadBufferExtended()`, `BufferDesc`, `StrategyGetBuffer()` |
| Buffer Replacement  | `freelist.c`                  | Clock-sweep algorithm, `nextVictimBuffer`                                   |
| B-Tree Index        | `src/backend/access/nbtree/`  | `_bt_search()`, `_bt_doinsert()`, `_bt_split()`                             |
| Heap Storage & MVCC | `src/backend/access/heap/`    | `heap_update()`, `HeapTupleHeaderData`, `HeapTupleSatisfiesMVCC()`          |
| WAL                 | `src/backend/access/transam/` | `XLogInsert()`, `XLogFlush()`, checkpoint recovery                          |
| Query Optimizer     | `src/backend/optimizer/`      | `planner.c`, `costsize.c`                                                   |

Studying the source code reveals an important difference between a classroom database and a production database.

A simple educational database often focuses only on correctness:

```
Read page
     |
Modify data
     |
Write page
```

PostgreSQL must additionally consider:

* Multiple concurrent processes.
* Lock contention.
* CPU cache efficiency.
* Disk latency.
* Crash consistency.
* Long-running transactions.
* Background maintenance.

This is why the implementation contains mechanisms such as atomic state variables, lightweight locks, background writers, WAL buffering, and MVCC visibility checks.

---

# 4. End-to-End Query Lifecycle

Before diving into individual subsystems, it is useful to understand how they work together.

Consider the query:

```sql
UPDATE users
SET status = 'ACTIVE'
WHERE id = 42;
```

The lifecycle looks like this:

```
Client Query
     |
     |
Parser creates syntax tree
     |
Analyzer validates objects
     |
Planner chooses Index Scan
     |
Executor starts execution
     |
B-Tree traversal finds tuple location
     |
Buffer Manager loads required pages
     |
MVCC checks tuple visibility
     |
heap_update() creates a new tuple version
     |
Old tuple receives xmax
New tuple receives xmin
     |
WAL record generated using XLogInsert()
     |
Buffer marked dirty
     |
COMMIT triggers XLogFlush()
     |
Client receives success
     |
Background writer/checkpointer eventually writes data page
     |
VACUUM later removes obsolete tuple versions
```

This single flow demonstrates PostgreSQL's central design philosophy:

**Do the minimum amount of work required to make a transaction durable, and postpone expensive operations to background processes whenever possible.**

Examples:

* A transaction does not wait for data pages to be written to disk. It only waits for WAL to become durable.
* Old tuple versions are not immediately removed. VACUUM cleans them later.
* Dirty pages are accumulated and flushed in batches.
* Buffer replacement decisions are approximate rather than maintaining an expensive exact LRU ordering.

These decisions sacrifice additional complexity and background maintenance in exchange for significantly higher throughput and concurrency.

---

# 5. Buffer Manager — The Boundary Between Memory and Disk

The first major subsystem to study is PostgreSQL's Buffer Manager.

Source Location:

```
src/backend/storage/buffer/
src/include/storage/
```

The buffer manager is arguably one of the most critical parts of the database because it controls the movement of pages between slow persistent storage and fast main memory.

The fundamental question it solves is:

> "How can PostgreSQL access a multi-terabyte database efficiently when only a small fraction of that data can fit into RAM?"

A naïve approach would read every requested page directly from disk:

```
Query
  |
Disk Read
  |
Return Data
```

This would be unacceptable because disk I/O is several orders of magnitude slower than memory access.

PostgreSQL instead maintains a shared memory region called the **Shared Buffer Pool** where frequently used database pages are cached.

The architecture can be viewed as:

```
                Backend Process
                        |
                        |
                Buffer Manager
                        |
        +---------------+---------------+
        |                               |
        |                               |
     BufferDesc                  8KB Page Frame
   (Metadata)                   (Actual Data)
        |
        |
   BufferTag Hash Table
        |
        |
 Identifies Disk Pages
```

Every page stored in memory has two parts:

* A physical 8KB memory frame containing the actual table or index data.
* A `BufferDesc` structure containing metadata describing the state of that frame.

The next section explores how PostgreSQL represents buffers internally, how pages are found, and how the system decides which pages remain in memory and which pages are evicted.

---

# 5. Buffer Manager — The Boundary Between Memory and Disk (Continued)

## 5.1 Shared Buffer Architecture

The first thing that becomes apparent when exploring PostgreSQL's source code is that the database does not simply ask the operating system for pages whenever it needs data. PostgreSQL maintains its own managed cache known as the **Shared Buffer Pool**.

The relevant implementation can be found in:

```
src/backend/storage/buffer/
```

particularly:

```
bufmgr.c
buf_internals.h
freelist.c
```

A natural question arises:

> "Modern operating systems already maintain a filesystem cache. Why does PostgreSQL maintain another cache?"

The answer is control.

The operating system sees a database file as a collection of bytes. It does not understand concepts like:

* A transaction currently reading a page.
* A page that has been modified but not safely written to WAL.
* A page that must not be evicted because an active query is using it.
* A page that is frequently accessed by thousands of concurrent sessions.

PostgreSQL's buffer manager understands these database-level semantics.

A simplified memory layout looks like:

```
                 Shared Memory
                        |
                        |
        +--------------------------------+
        |                                |
        |         Shared Buffers          |
        |                                |
        |  +----------+   +----------+   |
        |  | Page 1   |   | Page 2   |   |
        |  | 8 KB     |   | 8 KB     |   |
        |  +----------+   +----------+   |
        |         ...                    |
        +--------------------------------+
                        |
                        |
               Buffer Descriptors
                        |
                        |
              Buffer Lookup Hash Table
```

Each page frame stores exactly one database block, typically 8 KB.

The fixed-size design is intentional.

A dynamic allocation model using `malloc()` for every page would create memory fragmentation and additional allocation overhead during critical execution paths.

Instead, PostgreSQL can calculate the exact memory location of a buffer frame using its index.

---

# 5.2 BufferDesc — The Metadata Behind Every Page

A database page is useless without metadata describing its current state.

This responsibility belongs to the `BufferDesc` structure, defined in:

```
src/include/storage/buf_internals.h
```

A `BufferDesc` does not contain actual table data. Instead, it acts as a control block for a buffer frame.

Conceptually:

```
                 Buffer
                    |
        +-----------+------------+
        |                        |
        v                        v
  BufferDesc                 8KB Page
  (Metadata)                (User Data)
```

Important information stored inside `BufferDesc` includes:

### 1. BufferTag

A `BufferTag` uniquely identifies the physical page loaded into a buffer.

It contains:

* Tablespace OID.
* Database OID.
* Relation (table/index) OID.
* Fork number.
* Block number.

Together, these fields answer:

> "Which exact page from disk does this memory frame represent?"

---

### 2. Reference Count (Pin Count)

A page currently being used by a backend process is **pinned**.

Example:

```
Transaction A
       |
       |
Reading Page 100
       |
       |
Increment pin count
```

A pinned page cannot be selected as a victim during buffer replacement.

Without this mechanism, PostgreSQL could accidentally evict a page while another query is still reading or modifying it.

---

### 3. Usage Count

The `usage_count` represents how recently the page has been accessed.

It is the foundation of PostgreSQL's clock-sweep replacement algorithm.

A frequently accessed page receives a higher usage count and gets additional chances before eviction.

This approximates the behavior of an LRU cache without requiring expensive maintenance of a globally ordered list.

---

### 4. Dirty State

A page becomes dirty when its contents have been modified.

For example:

```
UPDATE users
SET balance = balance - 100
WHERE id = 42;
```

The modified page in memory is marked dirty.

However, PostgreSQL does not immediately write it to disk.

This is one of the most important performance decisions in the entire architecture.

Instead:

```
Update Data
     |
     |
Mark Buffer Dirty
     |
     |
Generate WAL Record
     |
     |
Commit after WAL Flush
     |
     |
Write actual page later
```

The database converts many small random writes into larger, more efficient background writes.

---

# 5.3 Atomic Buffer State — Designing for Thousands of Concurrent Connections

A toy database might protect every buffer with a large mutex:

```
Acquire Lock
      |
Modify Metadata
      |
Release Lock
```

This approach quickly becomes a bottleneck when thousands of sessions continuously access the buffer pool.

PostgreSQL takes a much more sophisticated approach.

The `state` field inside `BufferDesc` packs multiple pieces of information together:

* Reference count.
* Usage count.
* Dirty flags.
* Validity information.
* I/O state flags.

This state can often be modified using atomic Compare-And-Swap (CAS) operations.

The engineering insight is important:

The database is not only optimizing disk I/O.

It is also optimizing CPU synchronization overhead.

A high-performance database can easily become CPU-bound due to lock contention even when storage is fast.

---

# 5.4 The Complete Page Read Lifecycle

The journey of a page begins with:

```
ReadBuffer()
ReadBufferExtended()
```

located in:

```
src/backend/storage/buffer/bufmgr.c
```

Suppose the executor needs a particular table block.

The process looks like:

```
Executor
    |
    |
Request Page
    |
    |
Create BufferTag
    |
    |
Lookup Buffer Hash Table
    |
    +-------------------+
    |                   |
 Buffer Hit         Buffer Miss
    |                   |
Increase Pin        Select Victim
Usage Count              |
    |                    |
Return Page       Load From Disk
                       |
                       |
                  Insert Into Cache
                       |
                       |
                  Return Page
```

---

## Buffer Hit

The ideal scenario.

The required page already exists in shared memory.

PostgreSQL:

* Finds the buffer through the lookup hash table.
* Increases its reference count.
* Updates usage information.
* Returns the page immediately.

No disk access occurs.

A buffer hit is usually several orders of magnitude faster than a disk read.

---

## Buffer Miss

When the page does not exist in memory, PostgreSQL must find space for it.

This requires:

```
BufferAlloc()
```

inside `bufmgr.c`.

The system must answer a difficult question:

> "Which existing page should be removed to make space for the new one?"

The answer is the clock-sweep algorithm.

---

# 5.5 Clock-Sweep Replacement — Why PostgreSQL Avoids True LRU

The replacement logic is implemented in:

```
src/backend/storage/buffer/freelist.c

StrategyGetBuffer()
```

At first, a true Least Recently Used (LRU) cache appears ideal.

Whenever a page is accessed:

```
Move Page To Front
```

Whenever space is needed:

```
Remove Oldest Page
```

The problem is synchronization.

Every buffer access would require modifying a global LRU structure.

In a production database handling thousands of transactions per second, this global lock would become a severe bottleneck.

PostgreSQL instead uses a **clock-sweep algorithm**.

Imagine all buffers arranged in a circle:

```
        Buffer Pool

             [1]
        [8]       [2]

       [7]         [3]

        [6]       [4]
             [5]

             ^
             |
        Clock Hand
```

The clock hand continuously moves through the buffer pool.

For each candidate:

### If usage_count > 0

The page receives a second chance:

```
usage_count = usage_count - 1
Move to next page
```

---

### If usage_count = 0 and pin count = 0

The page can be safely reused:

```
Evict Page
       |
Load New Disk Block
```

---

This design is a perfect example of a production engineering trade-off:

| Approach    | Advantage                      | Disadvantage                    |
| ----------- | ------------------------------ | ------------------------------- |
| True LRU    | More accurate recency tracking | High synchronization overhead   |
| Clock Sweep | Scales well under concurrency  | Less perfect eviction decisions |

PostgreSQL intentionally sacrifices a small amount of cache accuracy to achieve significantly better scalability.

---

# 5.6 Dirty Pages, Background Writer, and Checkpointer

A crucial realization is:

> A successful transaction does not mean its data page has already reached disk.

Consider:

```
UPDATE account
SET balance = balance - 100;
```

The sequence is:

```
Modify Page in Buffer
          |
          |
Set Dirty Bit
          |
          |
Generate WAL Record
          |
          |
WAL Flush on COMMIT
          |
          |
Return Success to Client
          |
          |
Background Process Writes Data Page Later
```

This approach is called a **No-Force policy**.

PostgreSQL does not force every modified page to disk at commit time.

---

## Background Writer

The background writer proactively writes dirty pages.

Its goal is not durability.

Its purpose is performance.

It tries to ensure that when a future page replacement occurs, PostgreSQL can quickly find clean pages instead of making a user transaction wait for a disk write.

---

## Checkpointer

The checkpointer has a different responsibility.

It creates checkpoints that establish a recovery boundary.

During a checkpoint:

* Dirty pages are gradually flushed.
* A checkpoint record is written into WAL.
* Future crash recovery can begin from this point.

---

# 5.7 The Fundamental WAL Rule

A very important correctness rule exists:

> A data page containing changes with LSN X must never reach disk before the WAL containing LSN X is durable.

Why?

Imagine the opposite:

```
Data Page Written
        |
System Crash
        |
WAL Record Lost
```

After recovery, PostgreSQL would see a data page containing changes that have no corresponding history in WAL.

The database could not guarantee consistency.

Therefore the correct order is:

```
Change Data In Memory
          |
Create WAL Record
          |
Flush WAL To Disk
          |
Allow Data Page Flush
```

This simple rule is the foundation of PostgreSQL's crash safety.

---

# 5.8 Buffer Manager Design Trade-Off Summary

| Design Decision           | Benefit                          | Cost                                   |
| ------------------------- | -------------------------------- | -------------------------------------- |
| Shared Buffer Pool        | Reduces expensive disk I/O       | Additional memory usage and complexity |
| Fixed 8KB Buffer Frames   | Fast lookup and no fragmentation | Internal fragmentation possible        |
| Atomic Buffer State       | Reduces lock contention          | More complex implementation            |
| Clock Sweep               | Scales under high concurrency    | Approximate LRU behavior               |
| Delayed Dirty Page Writes | Higher transaction throughput    | Requires WAL and recovery mechanisms   |
| Background Writer         | Reduces latency during eviction  | Additional background I/O              |
| Checkpointing             | Faster crash recovery            | Periodic write overhead                |

---

## Key Engineering Insight

The most important lesson from studying PostgreSQL's buffer manager is that the problem is not simply **"how do we cache pages?"**

The real problem is:

> "How do we share a limited amount of memory among thousands of concurrent transactions while minimizing disk I/O and avoiding synchronization bottlenecks?"

PostgreSQL's answer is not a single algorithm.

It is a collection of carefully engineered compromises: shared buffers, atomic metadata, approximate replacement policies, deferred writes, and background maintenance.

This philosophy appears repeatedly throughout PostgreSQL's architecture.

The same pattern will appear again in B-Tree indexing, MVCC, WAL, and query optimization.

---

# 6. B-Tree Index Implementation — Designing Fast Searches at Disk Scale

## 6.1 The Fundamental Problem — How Do We Find Data Without Scanning Everything?

Imagine a table containing one billion rows.

A simple query:

```sql
SELECT *
FROM users
WHERE id = 42;
```

can be executed in two ways.

### Approach 1: Sequential Scan

The database starts from the first page of the table and checks every row:

```
Page 1
 ↓
Page 2
 ↓
Page 3
 ↓
...
 ↓
Page 10,000,000
```

This approach is simple and efficient when a large percentage of the table is required.

However, finding a single row in a massive table would mean reading millions of pages from storage.

---

### Approach 2: Maintain an Index

An index creates an auxiliary data structure that answers:

> "Where is the required tuple located?"

PostgreSQL's default answer is the **B+ Tree**.

The implementation exists in:

```
src/backend/access/nbtree/
```

Important source files explored:

| File          | Responsibility                                |
| ------------- | --------------------------------------------- |
| `nbtree.c`    | High-level B-Tree access method interface     |
| `nbtsearch.c` | Search and tree traversal (`_bt_search`)      |
| `nbtinsert.c` | Insert logic (`_bt_doinsert`) and page splits |
| `nbtpage.c`   | Page management and structural operations     |

---

# 6.2 Why PostgreSQL Chooses B+ Trees

At first glance, a Binary Search Tree (BST) seems sufficient.

```
        50
       /  \
      25   75
     /      \
    10       90
```

The problem is that this design is optimized for memory, not storage.

A disk access is extremely expensive compared to CPU operations.

A deep tree means:

```
Root
 ↓
Child
 ↓
Grandchild
 ↓
...
 ↓
Many random disk reads
```

This is unacceptable for a database.

---

## The Database Perspective

A B+ Tree is designed around **pages**, not individual nodes.

A single PostgreSQL page is typically 8 KB and can store hundreds of index entries.

Example:

```
                Root Page
                     |
        +------------+-------------+
        |                          |
   Internal Page             Internal Page
        |                          |
   +----+----+                +----+----+
   |         |                |         |
 Leaf      Leaf             Leaf      Leaf
 Page      Page             Page      Page
```

Because each node stores many keys, the tree becomes extremely wide and shallow.

A B-Tree containing millions of rows may only have a height of 3–4 pages.

Therefore, a lookup often requires only a handful of page accesses.

---

# 6.3 Physical Page Layout — How PostgreSQL Stores a B-Tree

A B-Tree is not a collection of C objects connected by pointers.

It is a sequence of fixed-size disk pages.

Every B-Tree page follows a structure similar to:

```
+--------------------------------+
| Standard PostgreSQL Page Header |
+--------------------------------+
| Line Pointer Array              |
+--------------------------------+
| Index Tuples                    |
| (Keys + TIDs)                   |
+--------------------------------+
| Free Space                      |
+--------------------------------+
| BTPageOpaqueData                |
+--------------------------------+
```

The interesting part is the `BTPageOpaqueData` structure located at the end of every index page.

It stores metadata such as:

* Left sibling pointer (`btpo_prev`)
* Right sibling pointer (`btpo_next`)
* Tree level
* Page status flags

---

# 6.4 Why Are Leaf Pages Linked?

This is one of the most elegant engineering decisions inside PostgreSQL.

A beginner might imagine that searching always requires returning to the root.

Example:

```
Root
 |
Leaf A
 |
Need next value
 |
Go back to Root
 |
Find Leaf B
```

This would be inefficient.

Instead, PostgreSQL maintains a doubly linked list among leaf pages.

```
Leaf 1 <----> Leaf 2 <----> Leaf 3 <----> Leaf 4
```

This provides two major benefits.

---

## 1. Efficient Range Queries

Consider:

```sql
SELECT *
FROM orders
WHERE amount BETWEEN 100 AND 1000;
```

The search finds the first matching leaf.

After that:

```
Find first leaf
        |
        ↓
Read matching tuples
        |
        ↓
Follow right sibling
        |
        ↓
Continue until range ends
```

The tree does not need to be traversed repeatedly.

---

## 2. Concurrent Page Splits

PostgreSQL implements a variation of the **Lehman-Yao high-concurrency B-Tree algorithm**.

A major challenge occurs when a writer splits a page while another transaction is reading it.

Imagine a reader searching for:

```
Key = 500
```

It arrives at a leaf page.

At the same moment, another transaction splits the page and moves some keys to a new right sibling.

A simple B-Tree could return an incorrect result.

PostgreSQL solves this using sibling links.

The reader can detect:

> "The key I need may have moved to the page on my right."

Then it simply follows:

```
Current Leaf
      |
      |
btpo_next
      |
      ↓
Right Sibling
```

This allows high concurrency without locking the entire tree.

---

# 6.5 Search Path — Following the Tree

The main search implementation is:

```
src/backend/access/nbtree/nbtsearch.c

Function:
_bt_search()
```

A search operation looks like:

```
Start at Root
      |
Binary search inside page
      |
Choose child pointer
      |
Load child page
      |
Repeat until leaf page
      |
Return matching IndexTuple
      |
Use TID to locate heap tuple
```

---

## Why Binary Search Inside a Page?

A single page may contain hundreds of index entries.

For example:

```
[10][25][40][55][70][90][120][150]
```

Finding the correct position using a linear scan would waste CPU cycles.

Therefore PostgreSQL performs binary search:

```
Middle key
     |
Smaller? → Left half
Larger?  → Right half
```

This reduces CPU work while the B-Tree minimizes expensive disk I/O.

---

# 6.6 The Relationship Between B-Tree and Heap Storage

A common misconception is:

> "The index stores the actual row."

In PostgreSQL's standard B-Tree implementation, this is not true.

A leaf entry contains:

```
Index Key
    +
ItemPointer (TID)
```

Example:

```
Index Entry:

id = 42
 |
 |
TID = (Block 100, Offset 5)
```

The TID points to the physical location of the tuple inside the heap.

The actual lookup is:

```
B-Tree
  |
Find key
  |
Return TID
  |
Buffer Manager loads heap page
  |
Executor checks MVCC visibility
  |
Return row
```

This separation provides flexibility but introduces an extra heap access.

This design is different from clustered storage engines such as InnoDB, where the primary index stores the entire row.

---

# 6.7 Insert Path — What Happens When a New Index Entry Arrives?

Insertion begins inside:

```
src/backend/access/nbtree/nbtinsert.c

Functions:
_bt_doinsert()
_bt_split()
```

The process is:

```
Find target leaf
        |
        |
Is there free space?
        |
     Yes | No
         |
         ↓
    Insert tuple
              |
              ↓
         Page Split
```

---

## Normal Insertion

If the page has enough free space:

```
Locate correct position
        |
Shift existing tuples
        |
Insert new IndexTuple
        |
Generate WAL record
```

This is relatively cheap.

---

# 6.8 Page Splits — The Cost of Keeping the Tree Balanced

A page split occurs when the target page has no space.

Example:

Before split:

```
[10 20 30 40 50 60 70 80]
```

A new key arrives:

```
Insert 45
```

The page cannot fit the data.

PostgreSQL creates a new page:

```
Before:

[10 20 30 40 50 60 70 80]

After:

Old Page          New Page

[10 20 30 40]    [45 50 60 70 80]
```

The operation requires:

* Allocating a new page.
* Redistributing tuples.
* Updating sibling links.
* Creating a separator key.
* Inserting the separator into the parent page.
* Writing WAL records.

If the parent is also full, the split propagates upward.

In extreme cases:

```
Leaf Split
     |
Parent Split
     |
Root Split
     |
New Tree Level
```

---

# 6.9 Why Random Inserts Are Expensive

A very practical production insight is that not all inserts are equal.

Consider two primary keys.

---

## Sequential IDs

Example:

```
1, 2, 3, 4, 5, 6...
```

Most inserts occur at the right-most leaf.

```
Root
 |
Right-most leaf
 |
Append new entries
```

The database enjoys good cache locality.

---

## Random UUIDs

Example:

```
F9A3...
12BC...
8D91...
```

Every new key may target a completely different leaf.

This causes:

* More random page reads.
* More page modifications.
* More cache misses.
* More page splits.

This is why random UUID primary keys can have significantly worse write performance in B-Tree-heavy workloads.

---

# 6.10 B-Tree Design Trade-Off Summary

| Design Decision            | Benefit                                   | Cost                                 |
| -------------------------- | ----------------------------------------- | ------------------------------------ |
| Wide B+ Tree pages         | Very small tree height and few disk reads | Page management complexity           |
| Linked leaf pages          | Fast range scans and concurrent splits    | Additional metadata maintenance      |
| Heap TID pointers          | Flexible storage organization             | Extra heap lookup required           |
| Balanced tree structure    | Predictable lookup performance            | Inserts may trigger expensive splits |
| Lehman-Yao concurrency     | Readers and writers proceed concurrently  | More complex implementation          |
| Binary search inside pages | Reduced CPU overhead                      | More complicated page logic          |

---

# Key Engineering Insight

A B-Tree is not simply a data structure for sorting keys.

It is a compromise between the realities of modern storage systems, CPU efficiency, and concurrency.

A classroom implementation might focus only on maintaining a balanced tree.

PostgreSQL's implementation must answer much harder questions:

* What happens if two users modify the same index simultaneously?
* How can a reader continue while a page is being split?
* How can range scans avoid repeatedly traversing the tree?
* How can modifications survive a crash?

The answer is a carefully engineered combination of page-oriented storage, sibling links, lightweight synchronization, and WAL-protected structural modifications.

The same design philosophy continues into PostgreSQL's next major subsystem: **MVCC and Heap Storage**, where the database solves another fundamental problem:

> "How can thousands of users read and modify the same data without constantly blocking each other?"

---

# 7. MVCC and Heap Storage — Solving Concurrency Without Locking Readers

## 7.1 The Fundamental Problem — Why Not Simply Overwrite a Row?

Imagine a very simple database implementation.

A user executes:

```sql
UPDATE users
SET balance = balance - 100
WHERE id = 42;
```

A naïve database may perform:

```
Locate Row
    |
    |
Overwrite Existing Bytes
    |
    |
Write Updated Page To Disk
```

This seems straightforward.

However, consider another transaction that started a few milliseconds earlier and is still reading the old value.

A fundamental question appears:

> "Should the reader wait for the writer to finish, or should the writer wait for every reader to complete?"

Traditional lock-based systems often suffer from this conflict:

```
Reader
  |
  |------ Waiting for writer lock
  |
Writer
```

As the number of concurrent users increases, this approach becomes a major bottleneck.

---

# 7.2 PostgreSQL's Core Philosophy — Never Destroy History Immediately

PostgreSQL takes a very different approach:

> "Do not overwrite the old version of the row. Create a new version and allow transactions to decide which version they can see."

This mechanism is called **Multi-Version Concurrency Control (MVCC).**

An UPDATE internally behaves more like:

```
Original Tuple
     |
     |
 UPDATE
     |
     |
+---------------------+
|                     |
Old Version      New Version
(xmax set)       (new xmin)
```

The old row remains available for transactions that started before the update.

New transactions observe the new version.

This allows readers and writers to proceed simultaneously without blocking each other.

The cost is that PostgreSQL temporarily stores multiple versions of the same logical row.

---

# 7.3 Heap Storage — PostgreSQL's Choice of Data Organization

The source code responsible for heap storage is primarily located in:

```
src/backend/access/heap/
src/include/access/
```

Important files explored:

| File                  | Responsibility                                       |
| --------------------- | ---------------------------------------------------- |
| `heapam.c`            | Heap table access methods, inserts, updates, deletes |
| `heapam_visibility.c` | MVCC visibility checks                               |
| `htup_details.h`      | Heap tuple header definitions                        |
| `vacuumlazy.c`        | Automatic garbage collection of dead tuples          |

Unlike some database systems that store the latest row version inside an index structure, PostgreSQL uses a **heap-based storage model**.

A heap page contains tuples without any particular logical ordering:

```
Heap Page (8KB)

+----------------+
| Page Header    |
+----------------+
| Tuple Slot 1   |
+----------------+
| Tuple Slot 2   |
+----------------+
| Tuple Slot 3   |
+----------------+
| Free Space     |
+----------------+
```

Indexes simply contain references to these heap tuples.

For example:

```
B-Tree Index

Key = 42
   |
   |
 TID
(Block 100, Offset 3)
   |
   |
Heap Page
   |
Actual Row Data
```

This separation gives PostgreSQL flexibility, but it means an index lookup usually requires an additional heap access.

---

# 7.4 Heap Tuple Header — Every Row Contains Transaction History

The most important structure in PostgreSQL MVCC is:

```
HeapTupleHeaderData
```

defined inside:

```
src/include/access/htup_details.h
```

A PostgreSQL row is not just user data.

Every tuple stores metadata describing its transactional history:

```
+-----------------------+
| xmin                  |
| xmax                  |
| ctid                  |
| infomask              |
+-----------------------+
| User Columns          |
+-----------------------+
```

---

## xmin — Who Created This Row?

`xmin` stores the transaction ID (XID) of the transaction that inserted this tuple.

Example:

```
Transaction 100 inserts:

User: Alice
Balance: 5000

Tuple:

xmin = 100
xmax = 0
```

It means:

> "Transaction 100 created this version of the row."

---

## xmax — Who Removed This Row?

When a row is deleted or updated, PostgreSQL does not immediately remove it.

Instead:

```
Before UPDATE

xmin = 100
xmax = 0
Balance = 5000
```

Transaction 200 performs:

```sql
UPDATE users
SET balance = 4900;
```

The old tuple becomes:

```
xmin = 100
xmax = 200
Balance = 5000
```

Meaning:

> "This version was invalidated by transaction 200."

A new tuple is created:

```
xmin = 200
xmax = 0
Balance = 4900
```

Now two physical versions of the same logical row exist.

---

# 7.5 The ctid Pointer — Connecting Tuple Versions

Every tuple also contains a `ctid`.

Initially:

```
Tuple A

ctid → Tuple A
```

After an UPDATE:

```
Old Tuple
    |
    |
  ctid
    |
    v
New Tuple
```

This creates a version chain.

The database can follow this chain to find the latest version when required.

This mechanism is especially important for optimizations like HOT updates.

---

# 7.6 Visibility Rules — How Does PostgreSQL Decide Which Version To Read?

Having multiple versions creates a new problem:

> "If several copies of the same row exist, which one should a query return?"

The answer is a **snapshot**.

When a transaction begins, PostgreSQL captures a view of the database:

```
Snapshot

Visible Transactions:
- Committed before my start

Invisible Transactions:
- Started after me
- Still running
- Aborted
```

During tuple access, PostgreSQL executes visibility logic in:

```
HeapTupleSatisfiesMVCC()
```

located in:

```
src/backend/access/heap/heapam_visibility.c
```

The algorithm asks:

### 1. Is xmin visible?

Questions:

* Did the inserting transaction commit?
* Was it committed before my snapshot?
* Is it my own transaction?

If not, ignore this tuple.

---

### 2. Is xmax visible?

Questions:

* Has another transaction deleted this tuple?
* Was that deletion committed before my snapshot?

If yes, the tuple is no longer visible.

---

The result is extremely powerful.

Two transactions can read the same table and observe different versions of reality.

Example:

```
Time ---->

T1 starts
|
| Reads Balance = 5000
|
T2 updates Balance = 4900
|
T2 commits
|
T1 still sees 5000
```

The reader experiences a consistent snapshot.

---

# 7.7 Why Readers Do Not Block Writers

The major advantage of PostgreSQL's MVCC model is:

```
Reader
  |
  |
Read old tuple version


Writer
  |
  |
Create new tuple version
```

They operate on different physical copies.

Therefore:

* Readers usually do not acquire heavyweight locks.
* Writers can continue updating rows.
* Long-running analytical queries do not stop transactional workloads.

This is one of the reasons PostgreSQL scales effectively for mixed workloads.

---

# 7.8 The Hidden Cost — Dead Tuples and Table Bloat

PostgreSQL's design trades concurrency for storage overhead.

Every UPDATE leaves behind an obsolete version:

```
Before:

Tuple A

After many updates:

Tuple A (dead)
Tuple B (dead)
Tuple C (dead)
Tuple D (live)
```

A table that logically contains one million rows may physically contain many more old versions.

Problems:

* Larger table size.
* More pages to scan.
* Larger indexes.
* More disk I/O.

The system needs garbage collection.

---

# 7.9 VACUUM — Cleaning Up PostgreSQL's History

The philosophy of MVCC is:

> "Keep history until no transaction can possibly need it."

Once PostgreSQL determines a tuple is invisible to every active transaction, VACUUM can reclaim its space.

The cleanup process:

```
Dead Tuple
     |
     |
VACUUM detects it
     |
     |
Remove tuple
     |
     |
Return space for reuse
```

Important components:

* **Autovacuum** automatically runs in the background.
* **Visibility Map** helps VACUUM skip pages where every tuple is visible.
* **Free Space Map** tracks pages with reusable space.

Without VACUUM, PostgreSQL would continuously grow.

---

# 7.10 HOT Updates — Avoiding Unnecessary Index Writes

A normal UPDATE often requires updating indexes.

Example:

```
Index

id = 42
   |
   |
Old TID
```

A new tuple location requires a new index entry.

This causes write amplification.

PostgreSQL introduces an optimization called **Heap-Only Tuple (HOT) update.**

Conditions:

* Indexed columns are not modified.
* The new tuple fits on the same heap page.

Instead of creating a new index entry:

```
Index
  |
Old Tuple
  |
ctid
  |
New Tuple
```

The index continues pointing to the old location.

The executor follows the tuple chain to find the newest visible version.

This significantly reduces index maintenance overhead in write-heavy workloads.

---

# 7.11 Comparison with Undo-Based MVCC Systems

Different databases answer the same question differently:

> "Where should the old version of the row live?"

## PostgreSQL

```
Heap Table

Old Version
      |
New Version
      |
VACUUM cleans later
```

Advantages:

* Readers are simple.
* No undo chain traversal.
* Snapshot visibility is straightforward.

Costs:

* Dead tuples accumulate.
* Requires background cleanup.

---

## Undo-Based Systems (Example: InnoDB)

```
Current Row
     |
Undo Log
     |
Older Versions
```

Advantages:

* Tables remain compact.
* Less table bloat.

Costs:

* Readers may need to traverse undo chains.
* Additional undo infrastructure is required.

Neither design is universally better.

They represent different engineering trade-offs.

---

# 7.12 MVCC Design Trade-Off Summary

| Design Decision               | Benefit                                     | Cost                                 |
| ----------------------------- | ------------------------------------------- | ------------------------------------ |
| Store multiple tuple versions | Readers and writers do not block each other | Extra storage consumption            |
| Heap-based MVCC               | Simple visibility model                     | Requires VACUUM                      |
| xmin/xmax in every tuple      | Fast visibility checks                      | Additional tuple metadata overhead   |
| Snapshot isolation            | Consistent reads                            | Old versions must be retained        |
| HOT updates                   | Reduces index writes                        | Works only under specific conditions |
| Autovacuum                    | Automatic cleanup                           | Consumes background CPU and I/O      |

---

# Key Engineering Insight

The biggest lesson from PostgreSQL's MVCC implementation is that **concurrency is not free**.

A simple database could overwrite rows immediately, but it would force readers and writers to fight over the same bytes.

PostgreSQL chooses the opposite philosophy:

> **Never destroy information immediately. Keep multiple versions, allow transactions to independently observe the correct version, and clean up old history asynchronously.**

The price is additional storage, VACUUM processes, and more complex tuple management.

The reward is one of PostgreSQL's greatest strengths:

**High concurrency with consistent, non-blocking reads.**

---
# 8. Write-Ahead Logging (WAL) — Achieving Durability Without Making Every Write Slow

## 8.1 The Fundamental Problem — What Happens If The Database Crashes?

Consider a simple transaction:

```sql
UPDATE accounts
SET balance = balance - 100
WHERE id = 42;
```

A beginner might assume the database performs:

```
Locate Page
     |
Modify Page In Memory
     |
Write Updated Page To Disk
     |
Return COMMIT Success
```

This guarantees durability, but it introduces a serious performance problem.

Every transaction would need to wait for a random disk write before returning success.

On a busy system processing thousands of transactions per second, this would severely limit throughput.

---

Now imagine the opposite approach:

```
Modify Page In Memory
        |
Return COMMIT Success
        |
Write To Disk Later
```

Performance improves dramatically.

However, another problem appears.

```
Modify Memory
       |
System Crash
       |
Memory Lost
```

The transaction was reported as successful, but its data never reached permanent storage.

This violates the **Durability** property of ACID.

Therefore PostgreSQL needs to answer a difficult engineering question:

> "How can a transaction commit quickly while still guaranteeing that its changes survive a crash?"

The answer is **Write-Ahead Logging (WAL).**

---

# 8.2 PostgreSQL's Core Idea — Log The Change Before Writing The Data

The fundamental rule of WAL is:

> **No modified data page is allowed to reach disk until the WAL record describing that modification has been safely persisted.**

This is commonly called the **WAL Rule**.

Instead of immediately writing the entire 8 KB page, PostgreSQL records a much smaller description of the change.

Example:

```
Old Data Page:
Balance = 5000

Transaction:
Decrease balance by 100

WAL Record:
"Page X, Row Y changed from 5000 to 4900"
```

The actual internal WAL format is more sophisticated, but conceptually it represents enough information for PostgreSQL to reconstruct the database state during recovery.

This design is extremely important because writing sequential log records is much faster than performing many random page writes.

---

# 8.3 WAL Architecture and Source Code Exploration

The WAL implementation is primarily located in:

```
src/backend/access/transam/
```

Important files explored:

| Source File    | Responsibility                            |
| -------------- | ----------------------------------------- |
| `xlog.c`       | WAL management, checkpoints, and recovery |
| `xloginsert.c` | Creation and insertion of WAL records     |
| `xact.c`       | Transaction commit and abort processing   |
| `xlogreader.c` | Reading WAL records during recovery       |

A key design observation from the source code is that PostgreSQL separates:

* Generating a WAL record.
* Placing it into WAL buffers.
* Flushing WAL to persistent storage.
* Writing modified data pages later.

This separation is what enables high transaction throughput.

---

# 8.4 The Life of an UPDATE — From Memory Change to Durable Commit

Consider:

```sql
UPDATE users
SET status = 'ACTIVE'
WHERE id = 42;
```

The complete lifecycle is:

```
Executor modifies heap page
          |
          |
Page in Shared Buffer becomes dirty
          |
          |
Create WAL Record using XLogInsert()
          |
          |
Place WAL Record in WAL Buffer
          |
          |
Transaction COMMIT
          |
          |
XLogFlush() forces WAL to disk
          |
          |
Client receives success
          |
          |
Data page remains only in memory
          |
          |
Background Writer / Checkpointer
writes page later
```

This design is called a **No-Force Buffer Management Policy**.

A transaction is not forced to write every dirty page to disk at commit time.

Instead, the database only forces the WAL.

---

# 8.5 Why PostgreSQL Uses WAL Buffers

Writing every WAL record directly to disk would also be expensive.

Imagine thousands of transactions continuously generating small log records:

```
Transaction A → 200 bytes
Transaction B → 100 bytes
Transaction C → 300 bytes
```

Performing a disk operation for every small record would create unnecessary overhead.

PostgreSQL therefore maintains **WAL buffers** in shared memory.

The flow is:

```
Transaction
     |
Generate WAL Record
     |
WAL Buffer
     |
Sequential WAL File on Disk
```

Multiple records can be grouped together before a physical disk flush.

---

# 8.6 Group Commit — One Disk Flush For Many Transactions

One of PostgreSQL's most important performance optimizations is **Group Commit**.

Consider three transactions arriving at nearly the same time:

```
Transaction A
Transaction B
Transaction C
```

A naïve implementation would perform:

```
A → fsync()
B → fsync()
C → fsync()
```

Three expensive disk synchronization operations.

Instead PostgreSQL can batch them:

```
Transaction A
        \
Transaction B ----> Single WAL Flush
        /
Transaction C
```

All transactions whose WAL records are included in that flush can safely commit.

The result:

* Lower disk overhead.
* Higher transaction throughput.
* Better scalability under heavy workloads.

This is a classic example of PostgreSQL delaying expensive work and processing it in batches.

---

# 8.7 Log Sequence Numbers (LSN) — Tracking Database History

Every WAL record is associated with a **Log Sequence Number (LSN).**

Conceptually:

```
WAL Timeline

LSN 100
   |
LSN 101
   |
LSN 102
   |
LSN 103
```

The LSN acts like a position in the database's history.

Data pages also store the LSN of the latest change applied to them.

This enables PostgreSQL to answer an important question during recovery:

> "Has this page already been updated with this WAL record, or does the change still need to be replayed?"

---

# 8.8 Checkpoints — Limiting Recovery Time

Imagine a database that has been running for one year.

Without checkpoints, crash recovery would require:

```
Start from the beginning of WAL
             |
Replay every operation
             |
Reach current state
```

This would be extremely slow.

PostgreSQL periodically creates checkpoints.

A checkpoint means:

```
Current Database State
           |
Flush necessary dirty pages
           |
Write Checkpoint Record into WAL
```

After a crash, PostgreSQL can start recovery from the latest checkpoint instead of replaying the entire history.

---

# 8.9 Crash Recovery — Rebuilding The Correct State

Suppose the database crashes:

```
Power Failure
      |
Memory Lost
      |
Restart PostgreSQL
```

PostgreSQL performs recovery:

```
Locate Last Checkpoint
          |
Read WAL Records After Checkpoint
          |
Replay Missing Changes
          |
Restore Consistent Database State
```

Because the WAL was safely written before acknowledging commits, PostgreSQL can reconstruct every committed transaction.

---

# 8.10 The STEAL and NO-FORCE Design Choice

A very important database architecture decision is PostgreSQL's buffer management policy.

### STEAL Policy

PostgreSQL may write a dirty page to disk before the transaction that modified it commits.

This improves memory management because buffers can be reused.

However, it requires WAL because the page may contain uncommitted changes.

---

### NO-FORCE Policy

PostgreSQL does not force all dirty pages to disk during commit.

Advantages:

* Very fast commits.
* High transaction throughput.

Cost:

* Recovery must replay WAL after crashes.

---

The combination is:

```
STEAL + NO-FORCE
        |
        |
Requires WAL-Based Recovery
```

Most modern high-performance databases use this architecture.

---

# 8.11 WAL Design Trade-Off Summary

| Design Decision       | Benefit                                   | Cost                                      |
| --------------------- | ----------------------------------------- | ----------------------------------------- |
| Sequential WAL writes | Much faster than random page writes       | Additional storage overhead               |
| WAL before data pages | Guarantees crash recovery                 | Extra logging work                        |
| WAL buffers           | Reduces small I/O operations              | Additional memory usage                   |
| Group commit          | Higher throughput                         | Slightly more complex commit coordination |
| Checkpoints           | Faster crash recovery                     | Background I/O overhead                   |
| STEAL + NO-FORCE      | Better performance and memory utilization | Requires recovery logic                   |

---

# Key Engineering Insight

The biggest lesson from studying PostgreSQL's WAL implementation is that **durability does not mean writing everything to disk immediately.**

A simple database might say:

```
Transaction commits
        |
Write entire data page
        |
Return success
```

PostgreSQL chooses a more sophisticated approach:

```
Transaction changes data
          |
Generate WAL record
          |
Flush small sequential WAL entry
          |
Return success immediately
          |
Write actual data pages later
```

The database intentionally separates **logical durability** from **physical page persistence**.

This is a recurring design pattern throughout PostgreSQL:

* MVCC delays tuple cleanup using VACUUM.
* The buffer manager delays page writes.
* WAL delays data page persistence while preserving correctness.

PostgreSQL accepts additional complexity and background maintenance in exchange for high throughput, strong durability guarantees, and scalability.

---

# 9. Query Planner and Statistics — How PostgreSQL Chooses the Cheapest Way to Execute a Query

## 9.1 The Fundamental Problem — There Is Never Just One Way to Execute a Query

When a developer writes a SQL statement, they describe **what data they need**, not **how the database should retrieve it**.

For example:

```sql
SELECT c.name, o.amount
FROM customers c
JOIN orders o
ON c.id = o.customer_id
WHERE c.country = 'India';
```

A human sees a single query.

PostgreSQL sees many possible execution strategies.

For example:

```
Option 1:
Scan customers
      |
For every customer
      |
Search matching orders

Option 2:
Scan orders
      |
Find corresponding customers

Option 3:
Build a hash table
      |
Perform Hash Join

Option 4:
Use indexes and perform index scans
```

Each strategy has a different cost depending on:

* Table size.
* Number of matching rows.
* Available indexes.
* Data distribution.
* Available memory.
* Disk I/O cost.

The fundamental question PostgreSQL must answer is:

> **"Out of all possible execution strategies, which one is expected to be the cheapest?"**

The component responsible for this decision is the **Query Planner and Optimizer**.

---

# 9.2 Source Code Exploration

The PostgreSQL planner is one of the most sophisticated parts of the database.

Relevant source locations:

```
src/backend/optimizer/
```

Important files explored:

| Source File  | Responsibility                                                   |
| ------------ | ---------------------------------------------------------------- |
| `planner.c`  | Entry point for query planning and generation of execution plans |
| `path/`      | Creation of alternative execution paths                          |
| `costsize.c` | Cost estimation for scans, joins, and sorting operations         |
| `clauses.c`  | Analysis and simplification of query expressions                 |
| `plan/`      | Conversion of chosen paths into executable plans                 |

Unlike a simple database that executes queries using a fixed algorithm, PostgreSQL explores multiple possibilities before choosing a plan.

---

# 9.3 Query Planning Pipeline

A SQL query goes through several stages before execution:

```
SQL Query
    |
Parser
    |
Query Tree
    |
Generate Candidate Paths
    |
Estimate Cost of Each Path
    |
Choose Cheapest Path
    |
Execution Plan
    |
Executor
```

This is known as **cost-based query optimization**.

The important idea is that PostgreSQL does not attempt to find the mathematically perfect plan.

That would require evaluating an enormous number of possibilities.

Instead, it uses heuristics and cost models to find a plan that is expected to perform well.

---

# 9.4 Cost Model — How PostgreSQL Estimates Work

PostgreSQL assigns an estimated cost to every possible operation.

The cost represents a relative measure of resources such as:

* Disk reads.
* CPU operations.
* Memory usage.
* Random versus sequential access.

For example:

```
Sequential Scan:
Cheap startup cost
High total cost for large tables

Index Scan:
Higher startup cost
Low cost when retrieving few rows
```

The planner compares these costs and chooses the lowest estimated path.

A very important observation is:

> PostgreSQL does not know the future. It makes decisions using statistics collected from past observations.

Therefore, the quality of the chosen plan depends heavily on the quality of the statistics.

---

# 9.5 The Role of pg_statistic and ANALYZE

The system catalog responsible for storing planner statistics is:

```
pg_statistic
```

These statistics are collected by the `ANALYZE` command and automatically maintained by PostgreSQL's autovacuum system.

Examples of information stored include:

### Number of Distinct Values

Example:

```
Country Column:

India
India
India
USA
Germany
```

The planner learns that some values are very common while others are rare.

This affects whether an index is useful.

---

### Most Common Values (MCV)

The planner stores frequently occurring values.

For example:

```
country:
India -> 80%
USA -> 15%
Other -> 5%
```

A query searching for `country = 'India'` may return millions of rows.

In this case, an index might be slower than simply scanning the table.

---

### Histograms

Histograms help estimate range queries.

Example:

```sql
WHERE salary > 100000
```

PostgreSQL uses histogram information to estimate how many rows are likely to satisfy the condition.

---

### Correlation Statistics

The planner also tracks whether the physical order of rows is correlated with an index.

A highly correlated index can make sequential page accesses more efficient.

---

# 9.6 Choosing Between Sequential Scan and Index Scan

One of the most common planner decisions is:

```
Should I read everything?
                OR
Should I use an index?
```

## Sequential Scan

```
Table Pages

Page 1
  |
Page 2
  |
Page 3
  |
...
```

Advantages:

* Very efficient when a large percentage of rows are required.
* Sequential disk access is fast.
* No index traversal overhead.

Disadvantages:

* Expensive for selective queries.

---

## Index Scan

```
B-Tree
   |
Find matching TID
   |
Fetch heap tuple
```

Advantages:

* Excellent for retrieving a small number of rows.
* Avoids scanning unrelated data.

Disadvantages:

* Additional index traversal.
* Random heap page accesses.

---

The surprising lesson is:

> An index is not always faster.

A query returning 90% of a table may perform better using a sequential scan.

This is why PostgreSQL relies on cost estimation rather than blindly using indexes.

---

# 9.7 Join Algorithm Selection

Joins are another major planning decision.

PostgreSQL can choose among multiple algorithms.

---

## Nested Loop Join

Concept:

```
For each row in A
        |
Search matching rows in B
```

Advantages:

* Simple.
* Works well when the outer table is small and indexes exist.

Disadvantages:

* Very expensive for large datasets.

---

## Hash Join

Concept:

```
Build Hash Table
       |
Probe Using Second Table
```

Advantages:

* Very efficient for large equality joins.

Disadvantages:

* Requires memory to store the hash table.

---

## Merge Join

Concept:

```
Sorted Input A
       +
Sorted Input B
       |
Merge Together
```

Advantages:

* Efficient for already sorted data.
* Good for large ordered datasets.

Disadvantages:

* Sorting may be expensive.

---

# 9.8 Practical Experiment Using EXPLAIN ANALYZE

To understand planner decisions, PostgreSQL provides:

```sql
EXPLAIN ANALYZE
SELECT c.name, o.amount
FROM customers c
JOIN orders o
ON c.id = o.customer_id
WHERE c.country = 'India';
```

A simplified output might look like:

```
Hash Join
  Cost: 200..500
  Actual Time: 10ms

  Seq Scan customers
  Index Scan orders
```

The output reveals two extremely important pieces of information.

---

## Estimated Statistics

Example:

```
Estimated Rows: 1000
```

This value is calculated by the planner using `pg_statistic`.

---

## Actual Execution Statistics

Example:

```
Actual Rows: 100000
```

This value is measured during real execution.

---

The difference between these numbers is very important.

A large mismatch indicates poor cardinality estimation.

Example:

```
Estimated:
100 rows

Actual:
1,000,000 rows
```

The planner may choose a nested loop because it expects a small dataset.

However, the actual query may become extremely slow.

---

# 9.9 Why Bad Statistics Cause Bad Performance

Consider a situation:

```
Planner believes:
Only 100 rows match

Reality:
10 million rows match
```

The planner may choose:

```
Index Scan
+
Nested Loop
```

which becomes extremely expensive.

A better choice could have been:

```
Sequential Scan
+
Hash Join
```

The database did not make a wrong decision.

It made a reasonable decision based on incorrect information.

This demonstrates an important engineering principle:

> A cost-based optimizer is only as good as the statistics it receives.

---

# 9.10 Comparison with a Simple Database

A toy database might execute every query using a fixed strategy:

```
SELECT
  |
Full Table Scan
  |
Return Rows
```

The implementation is simple.

However, performance becomes unpredictable as data grows.

PostgreSQL accepts the complexity of maintaining:

* Statistics catalogs.
* Cost models.
* Multiple join algorithms.
* Alternative execution paths.

The benefit is that the database can adapt to different workloads automatically.

---

# 9.11 Query Planner Design Trade-Off Summary

| Design Decision               | Benefit                                       | Cost                                     |
| ----------------------------- | --------------------------------------------- | ---------------------------------------- |
| Cost-based optimization       | Finds efficient plans for different workloads | Complex planner implementation           |
| Multiple join strategies      | Adapts to varying data sizes                  | Increased optimization time              |
| Statistics-driven estimates   | Better decision making                        | Requires ANALYZE and maintenance         |
| Index selection based on cost | Avoids unnecessary index usage                | Decisions can fail with stale statistics |
| Complex optimization pipeline | High performance at scale                     | Significant engineering complexity       |

---

# Key Engineering Insight

The most important lesson from studying PostgreSQL's query planner is that **SQL is a declarative language**.

The user says:

> "Give me this data."

The database decides:

> "This is the most efficient way I know to retrieve it."

The planner acts as the intelligence layer of PostgreSQL.

It converts a high-level query into a physical execution strategy by balancing:

* CPU cost.
* Memory usage.
* Disk I/O.
* Data distribution.
* Available indexes.

The quality of this decision directly determines whether a query executes in milliseconds or minutes.

---

# Connecting Back to the Larger PostgreSQL Architecture

By this point, we have seen how every subsystem works together.

```
SQL Query
    |
Parser and Planner decide execution strategy
    |
B-Tree indexes locate possible tuples
    |
Buffer Manager loads required pages
    |
MVCC checks which tuple versions are visible
    |
Executor returns results or modifies data
    |
WAL guarantees durability
    |
Background processes clean and maintain storage
```

PostgreSQL's architecture demonstrates a recurring engineering theme:

> **High performance is achieved not by making every operation immediately complete, but by carefully dividing work among specialized components, delaying expensive operations when safe, and maintaining enough metadata to make intelligent decisions later.**

---

# 10. Practical Observation — Understanding PostgreSQL Through `EXPLAIN ANALYZE`

Database internals become much more meaningful when we observe how PostgreSQL applies these concepts during real query execution.

For experimentation, consider the following multi-table join:

```sql
EXPLAIN ANALYZE
SELECT
    c.name,
    o.order_id,
    o.amount
FROM customers c
JOIN orders o
    ON c.customer_id = o.customer_id
WHERE c.country = 'India';
```

A possible execution plan may look like:

```
Hash Join
  Hash Cond: (o.customer_id = c.customer_id)

  -> Seq Scan on orders
       Actual rows: 100000

  -> Hash
       -> Index Scan on customers
            Filter: country = 'India'
```

---

## Understanding the Planner's Decision

At first glance, a developer may ask:

> "Why did PostgreSQL perform a sequential scan on a huge table? Why did it not use every available index?"

The answer lies in the cost-based optimizer.

The planner evaluates several alternatives:

```
Option 1:
Index Scan orders
+
Nested Loop Join

Option 2:
Sequential Scan orders
+
Hash Join

Option 3:
Merge Join with sorting
```

Using information stored in `pg_statistic`, PostgreSQL estimates:

* Number of rows expected from each operation.
* Cost of reading pages.
* CPU required for comparisons.
* Memory needed for hash tables or sorting.

The plan with the lowest estimated cost is selected.

This demonstrates an important principle:

> The existence of an index does not guarantee that PostgreSQL will use it.

For large result sets, sequential access can be cheaper than performing millions of random index lookups.

---

## Estimated Rows vs Actual Rows

One of the most valuable parts of `EXPLAIN ANALYZE` is comparing:

```
Estimated rows:
1000

Actual rows:
120000
```

A large mismatch indicates poor cardinality estimation.

This usually happens because:

* Statistics are outdated.
* Data distribution has changed.
* The planner has incomplete information.

Incorrect estimates can cause PostgreSQL to choose inefficient plans.

For example:

```
Estimated:
100 rows

Planner chooses:
Nested Loop Join

Actual:
10 million rows
```

The chosen strategy may become extremely expensive.

This is why `ANALYZE` and `pg_statistic` are critical parts of PostgreSQL's architecture.

The optimizer is only as intelligent as the information it receives.

---

# 11. End-to-End PostgreSQL Lifecycle

After studying individual subsystems, the most important realization is that PostgreSQL is not a collection of independent components.

Every subsystem participates in a single coordinated workflow.

---

## Read Query Lifecycle

A `SELECT` query follows this path:

```
Client Query
      |
      v
Parser & Analyzer
      |
      v
Query Planner
      |
      v
Choose Index Scan / Sequential Scan / Join Strategy
      |
      v
Executor
      |
      v
B-Tree Index Traversal (if required)
      |
      v
Buffer Manager checks Shared Buffers
      |
      +----------------+
      |                |
 Buffer Hit       Buffer Miss
      |                |
      |          Read Page From Disk
      |                |
      +----------------+
               |
               v
MVCC Visibility Check
               |
               v
Return Correct Tuple Version
```

---

## Write Query Lifecycle

An `UPDATE` operation is more complex:

```
Client UPDATE
       |
       v
Planner chooses access path
       |
       v
B-Tree locates tuple
       |
       v
Buffer Manager loads heap page
       |
       v
Old tuple receives xmax
       |
       v
New tuple created with xmin
       |
       v
Page marked dirty
       |
       v
WAL record generated
       |
       v
WAL flushed on COMMIT
       |
       v
Transaction acknowledged
       |
       v
Data page written later
       |
       v
VACUUM eventually removes obsolete tuples
```

This flow demonstrates PostgreSQL's central philosophy:

> Do the minimum amount of work required to guarantee correctness immediately, and postpone expensive maintenance operations whenever possible.

---

# 12. Major PostgreSQL Design Trade-Offs

The most important lesson from studying PostgreSQL is that every optimization introduces a new engineering challenge.

| Problem                           | PostgreSQL Design Choice       | Advantage                 | Cost                                                |
| --------------------------------- | ------------------------------ | ------------------------- | --------------------------------------------------- |
| Disk access is slow               | Shared Buffer Pool             | Faster data access        | Requires memory management and replacement policies |
| Exact LRU does not scale          | Clock-sweep algorithm          | Reduced lock contention   | Less accurate eviction                              |
| Readers and writers conflict      | MVCC tuple versioning          | Non-blocking reads        | Dead tuples and VACUUM overhead                     |
| Random data writes are expensive  | WAL with delayed page flushing | Fast transaction commits  | Additional logging and recovery complexity          |
| Millions of possible query plans  | Cost-based optimizer           | Adaptive query execution  | Depends on accurate statistics                      |
| Frequent index updates are costly | HOT updates                    | Reduces index maintenance | Works only in specific situations                   |
| Crash recovery must be fast       | Checkpoints                    | Reduces recovery time     | Background I/O overhead                             |

---

# 13. What Makes PostgreSQL Different From a Simple Database?

A toy database often follows a straightforward approach:

```
Read page
    |
Modify data
    |
Write page
```

This is easy to implement, but it fails under real-world conditions:

* Readers block writers.
* Every transaction waits for disk I/O.
* A crash can leave data inconsistent.
* Query performance degrades as data grows.
* Concurrent access creates contention.

PostgreSQL accepts significantly more internal complexity to solve these problems.

Instead of immediate actions, it introduces controlled layers of abstraction:

```
Need fast reads?
        |
 Shared Buffer Cache

Need concurrent transactions?
        |
        MVCC

Need fast commits?
        |
        WAL

Need efficient lookups?
        |
       B-Tree

Need intelligent execution?
        |
 Cost-Based Planner

Need long-term storage health?
        |
      VACUUM
```

Each component solves a specific bottleneck while introducing its own trade-offs.

---

# 14. Key Learnings and Engineering Insights

The biggest takeaway from this exploration is that PostgreSQL's complexity is not accidental.

Every component exists because a simpler design eventually fails at scale.

Some of the most important lessons learned from studying PostgreSQL internals are:

1. **Caching is not only about memory; it is also about concurrency.**
   PostgreSQL's buffer manager must coordinate thousands of sessions while minimizing lock contention.

2. **Concurrency requires keeping history.**
   MVCC achieves non-blocking reads by maintaining multiple tuple versions and cleaning them later with VACUUM.

3. **Durability does not require writing everything immediately.**
   WAL transforms random data writes into efficient sequential logging.

4. **A good database does not execute queries; it chooses how to execute them.**
   PostgreSQL's planner converts declarative SQL into optimized physical operations using statistics.

5. **The fastest algorithm is not always the most scalable.**
   PostgreSQL intentionally uses approximations like clock-sweep instead of perfect LRU to avoid contention.

6. **Production systems are collections of trade-offs.**
   Improving one aspect of the system usually introduces new challenges elsewhere.

---

# 15. Conclusion

Studying PostgreSQL internals reveals that a modern database is much more than a data storage engine.

It is a carefully engineered system balancing four fundamental challenges:

```
Performance
     |
Concurrency ---- Correctness
     |
Durability
```

The Buffer Manager minimizes expensive disk access.

B-Tree indexes provide efficient data lookup.

MVCC enables concurrent transactions without forcing readers and writers to wait.

WAL guarantees that committed changes survive failures.

The Query Planner transforms declarative SQL into efficient execution strategies.

Together, these components demonstrate PostgreSQL's core engineering philosophy:

> **A high-performance database is not built by making every operation faster. It is built by deciding which work must happen immediately, which work can be delayed safely, and how those decisions affect correctness, scalability, and performance.**

---

# References

1. PostgreSQL Official Documentation
   https://www.postgresql.org/docs/

2. PostgreSQL Source Code Repository
   https://github.com/postgres/postgres

3. PostgreSQL Source Files Explored:

   * `src/backend/storage/buffer/`
   * `src/backend/access/nbtree/`
   * `src/backend/access/heap/`
   * `src/backend/access/transam/`
   * `src/backend/optimizer/`

4. "Architecture of a Database System"
   Joseph M. Hellerstein, Michael Stonebraker, and James Hamilton

---

**Final Reflection**

This exploration showed that PostgreSQL is not simply a database that stores rows and executes SQL statements. It is a collection of decades of engineering decisions designed around real-world constraints such as slow storage, limited memory, concurrent users, and system failures.

Understanding PostgreSQL source code transformed many theoretical DBMS concepts such as buffers, indexes, transactions, and recovery into practical engineering mechanisms. The most valuable insight gained is that building a production-grade database is fundamentally the art of managing trade-offs rather than finding perfect solutions.

---
