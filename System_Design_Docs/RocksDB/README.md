# RocksDB Architecture: Understanding LSM Trees, Compaction, and the Trade-offs Behind High Write Performance
```LLM was used to polish and structure the content rest all the research exploration and practical was done by me ```

**Name:** Ujjwal Jain \
**Roll Number:** 24bcs10173

---

## 1. Problem Background — Why Were LSM Trees Created?

Traditional database systems such as B+Tree-based storage engines are designed around the idea that data on disk should always remain organized. When a record is inserted or updated, the database locates the corresponding leaf page, modifies the page, and preserves the tree structure.

```
Write Request
      |
Locate B+Tree Page
      |
Modify Existing Page
      |
Maintain Sorted Disk Structure
```

This design provides excellent read performance because data can be located through a predictable tree traversal. However, it becomes expensive for write-intensive workloads because maintaining disk organization requires frequent page updates, page splits, and random I/O.

This problem became more significant with the rise of systems generating massive continuous streams of data, including:

* Logging and monitoring platforms.
* Time-series databases.
* IoT telemetry systems.
* Real-time analytics pipelines.
* Distributed key-value stores.

These workloads prioritize high ingestion throughput over maintaining perfectly organized data at every moment.

The **Log-Structured Merge Tree (LSM Tree)** was introduced as a different architectural philosophy:

> **Do not pay the cost of organizing data during every write. Accept writes quickly, store them sequentially, and reorganize the data later in the background.**

RocksDB, originally developed by Facebook as an optimized fork of LevelDB, adopts this design philosophy. Instead of updating existing files in place, RocksDB writes new data to memory and immutable files, while background processes continuously merge and optimize the storage layout.

This shift in thinking explains every major component of RocksDB, including MemTables, SSTables, Bloom Filters, and Compaction.

---

# 2. Architecture Overview — How Data Moves Through RocksDB

At a high level, every piece of data follows a lifecycle from memory to persistent storage.

```
                Client Write
                     |
                     v
              Write Ahead Log
                     |
                     v
                 MemTable
                     |
                     v
          Immutable MemTable
                     |
                     v
              Flush to SSTable
                     |
                     v
                  Level 0
                     |
              Background Compaction
                     |
      +--------------+--------------+
      |              |              |
     L1             L2             Ln
 Larger, sorted, non-overlapping files
```

The main components are:

| Component                 | Purpose                                                                  |
| ------------------------- | ------------------------------------------------------------------------ |
| **WAL (Write Ahead Log)** | Guarantees durability before data reaches disk files                     |
| **MemTable**              | In-memory sorted structure that absorbs fast writes                      |
| **Immutable MemTable**    | Frozen MemTable waiting to be flushed to disk                            |
| **SSTable**               | Immutable sorted file containing key-value data                          |
| **Levels (L0-Ln)**        | Organize SSTables to reduce lookup cost                                  |
| **Compaction**            | Merges files, removes obsolete data, and restores efficient organization |

A key observation is that a user write never modifies an existing SSTable. RocksDB continuously creates new files and uses background compaction to maintain performance.

---

# 3. Internal Design

## 3.1 Write Path — Turning Random Writes into Sequential Operations

Consider the operation:

```text
PUT(user123, "Alice")
```

The write path is:

```
Client Write
      |
Append record to WAL
      |
Insert into MemTable
      |
Acknowledge success
```

At this point, the write is durable because the operation has been recorded in the WAL, even though the actual SSTable has not yet been created.

When the MemTable reaches its configured size limit:

```
MemTable Full
       |
Convert to Immutable MemTable
       |
Background Flush
       |
Create L0 SSTable
```

The critical insight is:

> **RocksDB does not update existing disk structures during writes. It converts many small random updates into large sequential writes.**

This makes writes extremely efficient because sequential I/O is significantly cheaper than constantly modifying scattered disk pages.

---

## 3.2 SSTables and Level Organization

An SSTable (Sorted String Table) is an immutable file storing keys in sorted order.

Immutability provides several benefits:

* No expensive in-place updates.
* Efficient sequential file creation.
* Simple crash recovery.
* Safe concurrent reads because files never change after creation.

Newly created SSTables are placed into **Level 0 (L0)**.

A challenge with L0 is that files may have overlapping key ranges.

Example:

```
Level 0

File A: A-Z
File B: M-X
File C: F-T
```

A query for a key such as `P` may need to check multiple files.

To solve this problem, RocksDB performs compaction and moves data into lower levels.

Example:

```
Level 1

File A: A-F
File B: G-M
File C: N-Z
```

Files in L1 and below generally have non-overlapping ranges, allowing RocksDB to quickly identify the file that may contain a particular key.

---

## 3.3 Read Path — The Cost of Cheap Writes

Because RocksDB spreads data across memory and multiple SSTables, reads are more complicated than in a B+Tree.

A lookup follows this order:

```
GET(user123)

        |
        v
   MemTable
        |
        v
 Immutable MemTables
        |
        v
 L0 SSTables
        |
        v
 L1 → L2 → L3 → Ln
```

The search starts from newer structures because the latest version of a key is likely to be found there.

Updates do not overwrite old values. Instead, newer versions of keys are written to newer files, while old versions are eventually removed during compaction.

Deletes are handled using **tombstones**, which are special markers indicating that a key has been removed. The actual data is physically deleted later during compaction.

This approach improves write speed but introduces **read amplification**, where a single lookup may need to inspect multiple files.

---

## 3.4 Bloom Filters — Avoiding Unnecessary Disk Reads

Without additional optimization, RocksDB may need to repeatedly check SSTables that do not contain the requested key.

Without Bloom Filters:

```
Does SSTable 1 contain key X?
          |
       Check file

Does SSTable 2 contain key X?
          |
       Check file

Does SSTable 3 contain key X?
          |
       Check file
```

A Bloom Filter is a probabilistic data structure that answers two questions:

* **Definitely not present**
* **Possibly present**

It guarantees no false negatives.

```
Bloom Filter
      |
      |---- No → Skip SSTable
      |
      |---- Maybe → Check SSTable
```

This significantly reduces unnecessary disk reads, especially for negative lookups.

The trade-off is that Bloom Filters require additional memory and may occasionally produce false positives.

---

## 3.5 Compaction — The Heart of RocksDB

Compaction is the mechanism that pays the cost postponed by fast writes.

Without compaction:

```
Many SSTables
      |
More overlapping files
      |
More files to search
      |
Higher read latency
```

Compaction performs:

```
Read SSTables
       |
Merge sorted keys
       |
Remove older versions
       |
Delete obsolete tombstones
       |
Generate optimized SSTables
```

The benefits are:

* Lower read amplification.
* Improved storage utilization.
* Removal of stale versions.
* Better organization of lower levels.

However, compaction is expensive because the same data may be rewritten multiple times as it moves through the levels.

---

# 4. Design Trade-offs — Understanding Amplification

The entire LSM architecture revolves around balancing three forms of amplification.

## Write Amplification

A record may be written once by the user but rewritten several times during compaction:

```
User Write
    |
MemTable
    |
L0
    |
L1
    |
L2
    |
L3
```

The same data may be copied multiple times.

---

## Read Amplification

A read may need to search:

* MemTables.
* Multiple L0 files.
* Several lower levels.

More files mean more work during lookups.

---

## Space Amplification

During compaction, old and new SSTables may temporarily coexist, increasing storage usage.

The key lesson is:

> Improving one amplification often worsens another. A storage engine cannot simultaneously minimize read, write, and space amplification.

---

# 5. Experiments and Practical Observations

RocksDB provides the `db_bench` benchmarking tool to evaluate different compaction strategies.

## Leveled Compaction

Typical observations:

Advantages:

* Lower read amplification.
* Better point lookup performance.
* More predictable read latency.

Disadvantages:

* Higher write amplification because data moves through multiple levels.

---

## Universal (Tiered) Compaction

Typical observations:

Advantages:

* Higher write throughput.
* Lower write amplification.

Disadvantages:

* More overlapping SSTables.
* Higher read amplification.
* Greater temporary storage usage.

The experiment demonstrates an important engineering principle:

> There is no universally best compaction strategy. The correct choice depends on whether the workload prioritizes writes, reads, or storage efficiency.

---

# 6. Comparison with B+Tree-Based Databases

| Aspect             | B+Tree Systems (PostgreSQL/InnoDB) | LSM Systems (RocksDB)     |
| ------------------ | ---------------------------------- | ------------------------- |
| Storage philosophy | Maintain order immediately         | Accept temporary disorder |
| Write operation    | Modify existing pages              | Append new data           |
| Disk I/O pattern   | More random writes                 | Mostly sequential writes  |
| Read path          | Simple tree traversal              | Search multiple levels    |
| Updates            | In-place/page modifications        | New versions + compaction |
| Background work    | Lower                              | Heavy compaction          |
| Best workload      | Balanced read/write                | Write-heavy ingestion     |

---

# 7. Key Learnings and Engineering Insights

The most important realization from studying RocksDB is that high write performance does not come from eliminating work. Instead, it comes from **moving expensive work away from the critical write path**.

RocksDB achieves this through a series of coordinated design choices:

* **WAL** ensures durability without waiting for disk reorganization.
* **MemTables** absorb large numbers of writes in memory.
* **Immutable SSTables** allow efficient sequential disk writes.
* **Bloom Filters** compensate for complex reads.
* **Compaction** gradually restores order and removes unnecessary data.

The central trade-off can be summarized as:

```
B+Tree:
Write → Maintain Order Immediately


RocksDB:
Write → Store Quickly → Organize Later
```

Neither approach is universally better.

B+Tree systems provide simpler reads and balanced performance, making them ideal for traditional transactional workloads.

RocksDB accepts more complicated reads and background maintenance in exchange for extremely high ingestion throughput.

---

# Conclusion

RocksDB demonstrates a fundamental lesson in database engineering:

> **The fastest systems are not the ones that avoid expensive operations, but the ones that carefully decide when those operations should occur.**

By delaying organization through the LSM-tree design, RocksDB transforms random writes into efficient sequential operations and achieves excellent write scalability.

The cost of this decision appears later in the form of compaction overhead, read complexity, and amplification trade-offs.

Understanding RocksDB ultimately reveals that database architecture is the art of choosing where to pay the unavoidable cost of managing data.

---

# References

1. RocksDB Official Documentation
2. *The Log-Structured Merge-Tree (LSM-Tree)* — Patrick O’Neil et al.
3. RocksDB `db_bench` Benchmark Tool Documentation
4. *Architecture of a Database System* — Hellerstein, Stonebraker, Hamilton
