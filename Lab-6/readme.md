# Advanced DBMS Lab 6: B-Tree Implementation

> **Author:** Ujjwal Jain (24BCS10173)  
> **Course:** Advanced Database Management Systems  
> **Language:** C++17

```LLM was used to polish and structure the content rest all the cmnds and practical was done by me ```

This repository contains a complete implementation of a **B-Tree** data structure from scratch in C++. B-Trees are self-balancing search trees designed to work efficiently on secondary storage devices like magnetic disks. They are heavily utilized in modern database systems (e.g., PostgreSQL, MySQL, SQLite) and file systems (e.g., NTFS, ext4) to store and retrieve massive amounts of ordered data in heavily reduced disk I/O operations.

---

## Table of Contents
1. [Included Files](#included-files)
2. [Compilation & Execution](#compilation--execution)
3. [Features & Capabilities](#features--capabilities)
4. [B-Tree Properties Maintained](#b-tree-properties-maintained)
5. [Time & Space Complexity](#time--space-complexity)

---

## Included Files

| File | Description |
|------|-------------|
| `btree.cpp` | Core implementation file containing the `BTreeNode` and `BTree` classes, along with an interactive terminal UI driver in `main()`. |
| `makefile` | Build script configured with standard C++17 compiler flags. |
| `readme.md` | This technical documentation. |

---

## Compilation & Execution

This project includes a `makefile` for easy compilation. Open your terminal and run:

```bash
make          # Compiles the source into a binary named 'btree'
make run      # Compiles and instantly executes the interactive driver
make clean    # Removes the compiled binary
```

Alternatively, you can manually compile it:
```bash
g++ -std=c++17 -Wall -Wextra -O2 -o btree btree.cpp
./btree
```

---

## Features & Capabilities

The interactive driver allows you to define the **minimum degree (t)** of the B-Tree at startup, ensuring that you can construct trees with varying node capacities dynamically. 

Through the interface, you can perform:
1. **Insertions:** Inserts a new key while safely handling node overflows by utilizing the `splitChild` routine, strictly preserving B-Tree capacity constraints.
2. **Searches:** Efficiently navigates down the multi-way tree to locate specific keys.
3. **Traversals:** Prints out the keys strictly in ascending order by traversing the subtrees.

---

## B-Tree Properties Maintained

This robust implementation mathematically guarantees the following B-Tree structural properties at all times during its operation:
1. **Node Degree Constraints:** For a given minimum degree `t`, every node (excluding the root) possesses between `t - 1` and `2t - 1` keys.
2. **Child Constraints:** An internal node harboring `k` keys strictly holds `k + 1` children.
3. **Sorting Invariant:** All keys within a single node are maintained in strict ascending order.
4. **Perfect Balance:** All leaf nodes exist precisely at the same depth level from the root.
5. **Proactive Splitting:** If a node hits its maximum capacity (`2t - 1` keys), it is proactively split *before* a new key is inserted, gracefully increasing the height of the tree from the root when necessary.

---

## Time & Space Complexity

For a B-Tree managing `n` keys with a minimum degree `t`:

| Operation | Time Complexity | Auxiliary Space |
|-----------|-----------------|-----------------|
| Search | `O(t * log_t n)` | `O(log_t n)` (Recursion Stack) |
| Insertion | `O(t * log_t n)` | `O(log_t n)` (Recursion Stack) |
| Traversal | `O(n)` | `O(log_t n)` (Recursion Stack) |

The parameter `t` heavily minimizes the height of the tree (`h ≤ log_t((n+1)/2)`), which is explicitly why B-Trees are fundamentally optimized for databases where disk seeks are exceptionally expensive.