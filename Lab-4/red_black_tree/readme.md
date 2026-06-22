# Advanced DBMS Lab 5: Red-Black Tree Implementation

> **Author:** Ujjwal Jain (24BCS10173)  
> **Course:** Advanced Database Management Systems  
> **Language:** C++17

This project features a complete, from-scratch implementation of a **Red-Black Tree (RBT)**. A Red-Black tree is a self-balancing binary search tree where each node holds an extra bit of data denoting its color (Red or Black). By strictly adhering to a set of coloring rules, the tree guarantees that its height remains `O(log n)`, ensuring efficient worst-case times for insertions, deletions, and lookups. This structure is famously used in `std::map`, Linux CPU schedulers, and database indexes.

---

## Table of Contents
1. [Project Files](#project-files)
2. [Compilation & Execution](#compilation--execution)
3. [Example Run](#example-run)
4. [Public Interface](#public-interface)
5. [Core Invariants](#core-invariants)
6. [Insertion Fixups](#insertion-fixups)
7. [Deletion Fixups](#deletion-fixups)
8. [Tree Rotations](#tree-rotations)
9. [The Sentinel Node Approach](#the-sentinel-node-approach)
10. [Time & Space Complexity](#time--space-complexity)

---

## Project Files

| File | Description |
|------|-------------|
| `red-black-tree.h` | The class definition, including the `Node` struct, the `Color` enum, public methods, and private helpers. |
| `red-black-tree.cc` | The core logic containing the implementations for inserting, deleting, rotating, and fixing violations. |
| `main.cc` | A driver program to demonstrate the tree's capabilities (inserting, searching, and deleting). |
| `makefile` | The build script to compile the project using `g++` with C++17 standards. |
| `readme.md` | This documentation file. |

---

## Compilation & Execution

To compile and run the driver code using the provided makefile:

```bash
make          # Generates the ./rbtree executable
make run      # Compiles and immediately executes the binary
make clean    # Deletes the generated executable
```

Manual compilation command:
```bash
g++ -std=c++17 -Wall -Wextra -O2 -o rbtree main.cc red-black-tree.cc
./rbtree
```

---

## Example Run

```text
Inserting: 50 25 75 10 30 60 80 5 15 70

Tree after inserts (level-order, format key:R/B):
  [50:B, 25:R, 75:R, 10:B, 30:B, 60:B, 80:B, 5:R, 15:R, nil, nil, nil, 70:R]

find(30)  -> found
find(70)  -> found
find(99)  -> not found

Removing 25, 60, 80 ...
Tree after removals:
  [50:B, 10:R, 75:B, 5:B, 30:B, 70:R, nil, nil, nil, 15:R]

find(25)  -> not found
find(50)  -> found
```

Visualizing the tree after insertion (using level-order representation):

```text
                  50:B
               /        \
           25:R           75:R
          /    \         /    \
       10:B   30:B    60:B   80:B
       /  \              \
     5:R  15:R           70:R
```
Notice how the invariants are strictly maintained: no Red node has a Red child, and every path from the root to a leaf has exactly three Black nodes (e.g., `50:B -> 25:R -> 10:B -> 5:R -> nil:B`).

---

## Public Interface

The `RedBlackTree` class exposes a clean and standard API:

```cpp
class RedBlackTree {
public:
    RedBlackTree();
    ~RedBlackTree();

    void insert(int key);     // Inserts a key and automatically rebalances in O(log n)
    bool find(int key) const; // Searches for a key via standard BST traversal in O(log n)
    void remove(int key);     // Deletes a key and automatically rebalances in O(log n)
    void print() const;       // Performs a Breadth-First Search to print the tree in level-order
};
```

---

## Core Invariants

To remain perfectly valid, this Red-Black Tree must never violate these five rules:
1. **Node Color:** Every node must be designated as either Red or Black.
2. **Root Property:** The root node is unconditionally Black.
3. **Leaf Property:** All `NIL` leaves are considered Black.
4. **Red-Red Restriction:** A Red node cannot be the parent of a Red node (no two Red nodes in a row).
5. **Black-Height Uniformity:** Any given path from a node to any of its descendant `NIL` leaves must contain the exact same quantity of Black nodes.

---

## Insertion Fixups

Nodes are initially inserted as **Red** nodes to prevent breaking the Black-Height rule. If the newly inserted node's parent is also Red, we face a Red-Red violation (breaking Rule 4). We repair this by propagating the fix upward using three distinct scenarios:

### Scenario 1: Uncle is Red
If both the parent and uncle are Red, we can simply recolor. The parent and uncle become **Black**, and the grandparent becomes **Red**. The violation is then pushed up to the grandparent to be checked.

```text
      G(Black)                  G(Red)    <-- Move focus here
      /      \        ==>       /    \
 P(Red)    U(Red)        P(Black)  U(Black)
   /                        /
 z(Red)                   z(Red)
```

### Scenario 2: Uncle is Black, Node is an Inner Grandchild (Zig-Zag)
If the new node is a right-child of a left-child (or vice versa), we perform a rotation on the **parent** to straighten out the line. This transitions the tree into Scenario 3.

```text
     G(Black)                    G(Black)
     /      \       ==>          /      \
 P(Red)   U(Black)            z(Red)  U(Black)
   \                           /
 z(Red)                     P(Red)
```

### Scenario 3: Uncle is Black, Node is an Outer Grandchild (Straight Line)
If the new node is a left-child of a left-child, we swap colors and rotate around the **grandparent**. The parent becomes Black, the grandparent becomes Red, and a rotation finalizes the balance.

```text
       G(Black)                  P(Black)
       /      \      ==>         /      \
    P(Red)   U(Black)         z(Red)  G(Red)
    /                                     \
 z(Red)                                 U(Black)
```
*Note: The root is unconditionally colored Black before exiting the insertion fixup.*

---

## Deletion Fixups

We use the standard CLRS (Introduction to Algorithms) strategy for deletions:
1. Standard BST deletion: If the node has two children, swap it with its in-order successor.
2. If the node that was physically removed from the tree was **Black**, it creates a "black-height deficit". We resolve this by pushing the "double-black" status upward until it is absorbed.

The fixup loop utilizes four sibling-based scenarios:

| Scenario | Trigger Condition | Resolution Action |
|----------|-------------------|-------------------|
| **1** | Sibling `w` is **Red** | Rotate around the parent and swap colors. This forces the sibling to be Black, leading into scenarios 2, 3, or 4. |
| **2** | Sibling `w` is **Black** with two **Black** children | Recolor `w` to Red and shift the double-black deficit up to the parent. |
| **3** | Sibling `w` is **Black**, near child is **Red**, far child is **Black** | Swap colors of the sibling and its near child, then rotate the sibling to set up Scenario 4. |
| **4** | Sibling `w` is **Black**, far child is **Red** | Perform a final rotation on the parent and swap colors to instantly absorb the deficit. Processing halts (`x = root`). |

---

## Tree Rotations

Rotations structurally modify the tree while maintaining strict BST ordering properties. They operate in `O(1)` time.

### Left Rotation
Moves node `y` (the right child) upward, and drops node `x` down to the left. The inner subtree `b` swaps parents.

```text
    x                  y
   / \      ==>       / \
  a   y              x   c
     / \            / \
    b   c          a   b
```

### Right Rotation
Moves node `x` (the left child) upward, and drops node `y` down to the right. The inner subtree `b` swaps parents.

```text
    y                  x
   / \      ==>       / \
  x   c              a   y
 / \                    / \
a   b                  b   c
```

---

## The Sentinel Node Approach

To prevent complex and fragile `nullptr` checking during rotations and sibling lookups, this implementation relies on a shared `m_nil` sentinel node. 

- `m_nil` represents all empty leaves in the tree.
- It is globally defined as a **Black** node upon initialization.
- Every empty `left` or `right` pointer aims at this single node.
- Because `m_nil` is an actual object, operations like reading `node->color` or `node->left` never trigger segment faults, massively simplifying the deletion edge cases.

---

## Time & Space Complexity

| Operation | Time Complexity | Auxiliary Space |
|-----------|-----------------|-----------------|
| Search (`find`) | `O(log n)` | `O(1)` |
| Insertion (`insert`) | `O(log n)` | `O(1)` (Iterative fixup) |
| Deletion (`remove`) | `O(log n)` | `O(1)` (Iterative fixup) |
| Level-Order Print | `O(n)` | `O(n)` (Queue utilized for BFS) |

Because of invariants 4 and 5, the maximum height of a Red-Black tree is capped at `2 * log₂(n + 1)`, ensuring strictly logarithmic runtime bounds for all dynamic set operations.