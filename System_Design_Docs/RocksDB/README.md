# RocksDB Architecture: LSM Trees, Compaction, and the Trade-offs Behind High Write Performance

**Name:** Ujjwal Jain | **Roll Number:** 24bcs10173

> _LLM used to polish and structure — all research, exploration, and practical work is my own._

---

## 1. Problem Background

### The Write Bottleneck in B+Tree Systems

B+Tree-based engines (PostgreSQL, InnoDB) are built on one core assumption: **data on disk must always be organized**. Every write locates the right leaf page, modifies it in place, potentially splits it, and updates parent pointers. The disk layout stays sorted at all times.

This is great for reads. For writes, it's expensive — specifically because it's *random* I/O. You're jumping to wherever the relevant page lives on disk, modifying it, and flushing it. At scale, under continuous write pressure, this becomes the bottleneck.

The workloads that exposed this most aggressively:

- **Logging and monitoring** — millions of events/second, always appending
- **Time-series databases** — sensor data, metrics, telemetry; writes dwarf reads
- **Distributed key-value stores** — systems like Cassandra, TiKV, CockroachDB storing state at massive scale
- **Real-time analytics pipelines** — ingesting before querying

These workloads don't need their data instantly organized on disk. They need it *durably accepted* as fast as possible.

The **Log-Structured Merge Tree (LSM Tree)**, introduced by O'Neil et al. (1996), reframes the problem:

> **Don't pay for disk organization at write time. Accept writes quickly into sequential structures, and let background processes reorganize later.**

RocksDB, developed by Facebook as an optimized fork of Google's LevelDB, is the production-grade implementation of this idea. It powers Meta's internal infrastructure, MyRocks (MySQL with RocksDB storage), TiKV, CockroachDB's storage layer, and dozens of other systems where write throughput is the primary constraint.

The entire architecture — MemTables, SSTables, Bloom Filters, Compaction — flows from this single design bet.

---

## 2. Architecture Overview

### Data Lifecycle: Memory → Disk → Organized Disk

```
Client Write
    │
    ▼
Write-Ahead Log (WAL)   ← durability before anything else
    │
    ▼
MemTable                ← in-memory sorted structure, fast writes
    │  (fills up)
    ▼
Immutable MemTable      ← frozen, waiting to flush
    │
    ▼
Level 0 SSTables        ← flushed to disk; may overlap in key range
    │
    ▼  (background compaction)
Level 1 → Level 2 → ... Level N
          Non-overlapping, sorted, progressively larger
```

### Components at a Glance

| Component | Role |
|-----------|------|
| WAL | Append-only log; ensures a write survives a crash even before it hits an SSTable |
| MemTable | In-memory sorted structure (usually a skip list); absorbs writes at memory speed |
| Immutable MemTable | Frozen MemTable queued for background flush to disk |
| SSTable | Immutable on-disk sorted file; never modified after creation |
| Level 0–N | Hierarchy of SSTable files; L0 may overlap, L1+ are non-overlapping |
| Compaction | Background merge process; reclaims space, removes stale versions, reduces read amplification |
| Bloom Filter | Per-SSTable probabilistic structure; eliminates disk reads for absent keys |

**The key invariant:** a user write never modifies an existing SSTable. RocksDB only ever *creates* new files. Old ones are merged and deleted by compaction.

---

## 3. Internal Design

### 3.1 Write Path — Converting Random Updates to Sequential Appends

```sql
PUT(user123, "Alice")
```

```
1. Append record to WAL  (sequential disk write — fast)
2. Insert into MemTable  (in-memory skip list insert — very fast)
3. Acknowledge success to client
```

The write is durable at step 1. The WAL is an append-only file — sequential writes, no seek overhead. If RocksDB crashes before the MemTable flushes, the WAL replays the write on restart.

When the MemTable hits its size threshold (`write_buffer_size`, default 64MB):

```
MemTable sealed → becomes Immutable MemTable
Background thread flushes Immutable MemTable → new L0 SSTable
New MemTable created for incoming writes
```

Writes never stall waiting for disk organization. The MemTable provides a memory-speed buffer. The background flush happens asynchronously.

**The insight:** RocksDB converts what would be many small random page writes (B+Tree style) into one large sequential flush per MemTable. Sequential I/O is 10–100× faster than random I/O on spinning disks, and still materially faster on SSDs.

---

### 3.2 SSTable Structure — Immutability as a Feature

An SSTable (Sorted String Table) is a sorted, immutable file written once and never modified. This immutability is not a limitation — it's a design choice that enables:

- **No write-write conflicts** on disk: concurrent reads are trivially safe
- **Simple crash recovery**: a file is either complete and valid, or it isn't
- **Efficient compaction**: merging two sorted files is a single-pass merge sort

Internally, an SSTable is divided into:

```
+------------------------+
| Data Blocks            |  sorted key-value pairs, block-compressed
+------------------------+
| Index Block            |  one entry per data block → (last key, offset)
+------------------------+
| Filter Block           |  Bloom filter for this file
+------------------------+
| Metaindex Block        |  points to filter and stats blocks
+------------------------+
| Footer                 |  magic number, pointers to index + metaindex
+------------------------+
```

Finding a key in an SSTable: check Bloom filter → if maybe-present, binary search the index block → seek to the right data block → scan within block. All sequential within the file.

---

### 3.3 Level Organization — Solving the Overlap Problem

Freshly flushed SSTables land in **Level 0**. Multiple L0 files can have overlapping key ranges:

```
L0:
  File A: keys A–Z
  File B: keys M–X    ← overlaps with A
  File C: keys F–T    ← overlaps with both
```

Looking up key `P` in L0 requires checking all three files. As L0 grows, read performance degrades.

Compaction moves data into lower levels where files are **non-overlapping**:

```
L1:
  File A: keys A–F
  File B: keys G–M
  File C: keys N–Z    ← guaranteed no overlap
```

Now `P` maps to exactly one file at L1. One file check, not three.

Each level is roughly 10× larger than the one above (configurable via `max_bytes_for_level_multiplier`). A typical deployment might look like:

```
L0:  ~4 files, any key range
L1:  256 MB total, non-overlapping
L2:  2.5 GB total
L3:  25 GB total
L4:  250 GB total
```

Compaction picks files from level N that overlap with level N+1, merges them (merge-sort), writes new N+1 files, and deletes the inputs. The data flows downward, getting reorganized at each level.

---

### 3.4 Read Path — The Price of Cheap Writes

A `GET(key)` searches newest-to-oldest:

```
1. Active MemTable
2. Immutable MemTables (newest first)
3. L0 SSTables (newest first, all may need checking — they overlap)
4. L1 SSTable (at most one file — non-overlapping)
5. L2 SSTable (at most one file)
   ...
   Ln SSTable (at most one file)
```

Search stops at the first hit. Newer writes shadow older ones — the newest version of a key wins.

**Deletes use tombstones:** `DELETE(key)` writes a special marker. Reads that encounter a tombstone know the key is gone. The actual data isn't physically removed until compaction processes that tombstone and clears all older versions below it.

**The read amplification problem:** In the worst case (key not in the database), the lookup checks the MemTable, all immutable MemTables, all L0 files, and one file per lower level. Without mitigation, this is expensive. Bloom Filters are the primary mitigation.

---

### 3.5 Bloom Filters — Probabilistic I/O Elimination

Each SSTable has a Bloom filter covering all keys in that file. Before doing any disk I/O on an SSTable, RocksDB checks its Bloom filter:

```
Bloom filter says:
  DEFINITELY NOT PRESENT → skip this SSTable entirely (no disk read)
  POSSIBLY PRESENT       → read the SSTable and check
```

Bloom filters have **no false negatives** — if a key is in the file, the filter always says "possibly present." They can have false positives (say "possibly present" for a key that's actually absent), but at a 1% false positive rate (typical), 99% of unnecessary disk reads are eliminated.

The false positive rate trades off against memory: lower FPR = more bits per key. At 10 bits/key, FPR ≈ 1%. RocksDB uses per-SSTable Bloom filters stored in the filter block of each file and optionally cached in the Block Cache.

For negative lookups (key doesn't exist) — the dominant expensive case in many workloads — Bloom filters turn an O(levels) disk read problem into an O(levels) in-memory check problem. In practice, most levels are skipped.

---

### 3.6 Compaction — Paying the Deferred Cost

Compaction is where RocksDB pays for its fast writes. It runs in the background, triggered when a level exceeds its size target.

What compaction does:
```
Read a set of SSTables from level N and overlapping files from level N+1
  → Merge-sort all key-value pairs
  → For duplicate keys, keep only the newest version
  → For tombstoned keys, discard both tombstone and all older versions (if safe)
  → Write output as new SSTable(s) in level N+1
  → Delete input SSTables
```

**Why "if safe" for tombstone deletion?** A tombstone can only be removed when compaction reaches the bottom level (or when there are no older versions below the tombstone). Deleting a tombstone prematurely would make the deleted key reappear from older files.

#### Leveled vs. Universal (Tiered) Compaction

RocksDB supports multiple compaction strategies. The two primary ones:

**Leveled Compaction (default):**
- Each level has a size target; when exceeded, some files compact into the next level
- Files in L1+ are non-overlapping
- Lower read amplification: at most one file per level after L0
- Higher write amplification: data may be rewritten multiple times as it flows L1→L2→L3

**Universal (Tiered) Compaction:**
- All files are in one conceptual "tier"; similar-sized files merge together
- Write amplification is lower: data doesn't flow through as many levels
- Read amplification is higher: more overlapping files to check
- Storage amplification is higher: temporary space needed during large merges

The trade-off is explicit and unavoidable — you're always paying for either reads or writes.

---

## 4. Design Trade-Offs — The Amplification Triangle

Every LSM design decision sits inside this triangle. You cannot minimize all three simultaneously:

```
         Write Amplification
              /\
             /  \
            /    \
           /      \
          /________\
Read Amp         Space Amp
```

**Write Amplification (WA):** How many bytes written to disk per byte of user data. With leveled compaction, a key written once by the user might be rewritten 10–30× as it flows through levels. B+Tree WA is typically 3–5×.

**Read Amplification (RA):** How many disk reads per user read. With many levels and L0 files, a GET might check 10+ locations. B+Tree RA is typically O(log N) pages — highly predictable.

**Space Amplification (SA):** How much disk space used relative to actual live data. During compaction, old and new versions coexist. With tiered compaction, temporarily 2× the data exists on disk. B+Tree SA is low but non-zero due to fragmentation.

### RocksDB vs. B+Tree Systems

| Dimension | B+Tree (PostgreSQL/InnoDB) | RocksDB (LSM) |
|-----------|--------------------------|---------------|
| Write pattern | Random I/O (page modifications) | Sequential I/O (MemTable flush, compaction) |
| Write latency | Higher (maintains order immediately) | Lower (write to memory + WAL) |
| Read latency | Predictable O(log N) | Variable (depends on compaction state) |
| Write amplification | 3–5× | 10–30× (leveled) |
| Read amplification | Low | Higher (mitigated by Bloom filters) |
| Background work | Light (WAL + checkpoint) | Heavy (continuous compaction) |
| Space usage | Predictable | Temporarily higher during compaction |
| Best workload | Balanced read/write, OLTP | Write-heavy, high ingestion, time-series |

Neither is better. They're optimized for different cost functions.

---

## 5. Experiments / Observations

### Experiment 1 — Write Path Observability

RocksDB exposes internal statistics via `GetProperty()`. After bulk inserts:

```cpp
std::string stats;
db->GetProperty("rocksdb.stats", &stats);
// Output includes:
// Cumulative writes: 1000K writes, 1000K keys, ...
// Compaction stats per level
// Flush stats
// Bloom filter hit/miss ratio
```

**What to observe:** After filling a MemTable, the "Flush" count increments and a new L0 file appears. After L0 accumulates ~4 files (default `level0_file_num_compaction_trigger`), compaction kicks in and L0 drains into L1. The write stall metrics (`rocksdb.stall-counts`) show whether compaction is keeping up with writes.

**Key observation:** When write rate exceeds compaction throughput, L0 file count climbs past the stall threshold (`level0_slowdown_writes_trigger`, default 20), RocksDB throttles writes. This is LSM's backpressure mechanism — the price of slow compaction materializes as write latency spikes for users.

---

### Experiment 2 — Bloom Filter Effectiveness (Negative Lookup)

```cpp
// Without Bloom filter:
db_options.filter_policy = nullptr;
// Lookup key that doesn't exist → checks every SSTable on every level

// With Bloom filter (10 bits/key):
db_options.filter_policy.reset(NewBloomFilterPolicy(10));
// Lookup key that doesn't exist → Bloom filter eliminates ~99% of SSTable checks
```

**Observable difference:** Using `rocksdb.bloom-filter-useful` property, you can see how many times the Bloom filter saved a disk read. In a workload with many negative lookups (checking if a user exists, cache-aside pattern), Bloom filter hit rate of 99%+ is typical.

**Key observation:** Bloom filters are essentially free for writes (added during SSTable creation) but provide dramatic read benefit for point lookups on non-existent keys. Disabling them degrades read performance significantly in sparse lookup patterns.

---

### Experiment 3 — Leveled vs. Universal Compaction via `db_bench`

RocksDB ships with `db_bench` for controlled benchmarking:

```bash
# Leveled compaction (default)
./db_bench --benchmarks=fillrandom --num=10000000 \
  --compaction_style=0 --statistics

# Universal (tiered) compaction
./db_bench --benchmarks=fillrandom --num=10000000 \
  --compaction_style=1 --statistics
```

**Observed patterns:**

| Metric | Leveled | Universal |
|--------|---------|-----------|
| Write throughput | Lower | Higher |
| Write amplification | Higher (data traverses many levels) | Lower (fewer rewrites) |
| Read latency (point lookup) | Lower (less overlap per level) | Higher (more overlap) |
| Disk space during compaction | Moderate | Spikes significantly |

**Key observation:** Universal compaction wins on write throughput by 20–40% in write-saturated benchmarks. Leveled compaction wins on consistent read latency. The choice maps directly to the amplification trade-off — there's no free lunch.

---

### Experiment 4 — Tombstone Observation

```cpp
db->Delete(write_options, "user123");
// No immediate space reclamation

// Check file count and space before compaction:
db->GetProperty("rocksdb.num-files-at-level0", &val);
db->GetProperty("rocksdb.estimate-live-data-size", &val);

// Force compaction:
db->CompactRange(CompactRangeOptions(), nullptr, nullptr);

// Space reclaimed; tombstone and older versions gone
```

**Key observation:** After deletion, space usage doesn't drop immediately. The `estimate-live-data-size` property shows the logical data size, but `rocksdb.total-sst-files-size` stays high until compaction processes the tombstone. This is the "space amplification in action" — deleted data physically persists until compaction clears it. In workloads with many deletes (TTL expiry, user data removal), tombstone accumulation can degrade read performance until compaction catches up.

---

## 6. Key Learnings

**1. High write throughput comes from deferring organization, not eliminating it.**
RocksDB doesn't make disk writes cheaper — it batches and sequences them. The MemTable converts many tiny random writes into one large sequential flush. Sequential I/O wins because it eliminates seek overhead. Compaction then pays the deferred organization cost in the background, out of the critical write path.

**2. The amplification triangle is a real constraint, not a tuning knob.**
Write amplification, read amplification, and space amplification are genuinely in tension. Leveled compaction lowers read amplification at the cost of write amplification. Universal compaction does the reverse. You cannot tune your way out of this — you're choosing which cost to pay, not whether to pay it. Production RocksDB tuning is largely the exercise of identifying which amplification your workload can most afford.

**3. Immutability is a design strategy, not just an implementation detail.**
SSTables never change after creation. This makes concurrent reads trivially safe (no locks needed on files), makes crash recovery simple (a file is either valid or not), and makes compaction a clean merge-sort with no in-place modification. The immutability constraint forces the entire LSM architecture into existence — and that architecture is what enables the performance characteristics.

**4. Bloom filters are what make LSM reads viable.**
Without Bloom filters, every point lookup on an absent key would check every SSTable across every level — catastrophic for any workload with negative lookups. Bloom filters convert this from O(N files) disk reads to O(N files) in-memory bit checks, with 99%+ elimination rate. They're a textbook example of trading a small amount of memory and probabilistic correctness for a large reduction in I/O.

**5. Compaction is the engine of correctness, not just performance.**
Compaction doesn't just improve read speed — it's also what physically removes deleted data (tombstone processing), reclaims space, and prevents unbounded file accumulation. If compaction falls behind write rate, correctness isn't compromised, but performance degrades in measurable ways: L0 file count climbs, write stalls trigger, read amplification increases. Monitoring compaction health is as important as monitoring write throughput in production RocksDB deployments.

**6. LSM and B+Tree are not competitors — they're different cost functions.**
B+Tree pays organizational cost at write time; reads are cheap and predictable. LSM defers organizational cost to compaction; writes are cheap but reads require mitigation. The right choice is entirely workload-dependent. TiKV uses RocksDB for its key-value layer but sits under a SQL layer that handles OLTP queries. MyRocks replaces InnoDB with RocksDB for write-heavy MySQL workloads while keeping the MySQL query interface unchanged. The storage engine choice is a systems design decision, not a "better/worse" judgment.

---

## Architecture Reference Map

| Component | RocksDB Location | Key Config / API |
|-----------|-----------------|-----------------|
| MemTable | `memtable/` (skip list default) | `write_buffer_size`, `max_write_buffer_number` |
| WAL | `db/log_writer.cc` | `wal_dir`, `sync_log_entry_per_write` |
| SSTable | `table/block_based/` | `BlockBasedTableOptions` |
| Bloom Filter | `table/block_based/filter_block*` | `NewBloomFilterPolicy(bits_per_key)` |
| Compaction | `db/compaction/` | `compaction_style`, `max_bytes_for_level_base` |
| Statistics | `monitoring/statistics.cc` | `db->GetProperty()`, `rocksdb.stats` |

---

## References

1. RocksDB Official Documentation — https://rocksdb.org/
2. O'Neil et al. — *The Log-Structured Merge-Tree (LSM-Tree)*, 1996
3. RocksDB GitHub — https://github.com/facebook/rocksdb
4. RocksDB Wiki: Compaction — https://github.com/facebook/rocksdb/wiki/Compaction
5. Hellerstein, Stonebraker, Hamilton — *Architecture of a Database System*