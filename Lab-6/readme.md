# Advanced DBMS Lab 6: Transaction Manager

> **Author:** Ujjwal Jain (24BCS10173)  
> **Course:** Advanced Database Management Systems  
> **Language:** C++17

This project features an in-memory database **Transaction Manager** demonstrating core database concurrency control and recovery principles from scratch.

## Core Concepts Implemented

1. **Strict Two-Phase Locking (Strict 2PL)**
   - Transactions acquire Shared (`S`) locks for reading and Exclusive (`X`) locks for writing.
   - All locks are held until the transaction either commits or aborts.
   - Ensures conflict serializability and prevents cascading aborts.

2. **Multi-Version Concurrency Control (MVCC)**
   - Row modifications do not overwrite in place. Instead, new versions are prepended to a version chain.
   - When a transaction aborts, the versions it created are discarded, effectively rolling back the database state.

3. **Deadlock Detection (Wait-for Graph)**
   - A central `LockManager` manages all lock requests.
   - When a lock cannot be granted, the system dynamically updates a **waits-for graph** (directed edges from requesting transaction to blocking transactions).
   - Before a transaction is put to sleep, a Cycle Detection algorithm (Depth First Search) traverses the graph.
   - If a cycle is detected, the requesting transaction immediately aborts to break the deadlock and allows the others to proceed.

## Compilation & Execution

```bash
g++ -std=c++17 -pthread txn_manager.cpp -o txn_manager
./txn_manager
```

## Example Run (Deadlock Resolution)
The driver code spins up two concurrent transactions:
- Txn 1 writes to Row 100, then sleeps, then writes to Row 200.
- Txn 2 writes to Row 200, then sleeps, then writes to Row 100.
This intentionally triggers a classic deadlock. The engine successfully catches it, aborts one transaction, and commits the other.
