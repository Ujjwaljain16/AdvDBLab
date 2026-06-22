# Advanced DBMS Lab 7: SQL Query Parsing & Evaluation

> **Author:** Ujjwal Jain (24BCS10173)  
> **Course:** Advanced Database Management Systems  
> **Language:** C++17

```LLM was used to polish and structure the content rest all the cmnds and practical was done by me ```

This repository contains two distinct implementations for parsing and evaluating a SQL `WHERE` clause. This demonstrates the transition from a simple string parsing mechanism to robust tree-based abstract representations.

---

## Included Implementations

### 1. [Dijkstra's Shunting-Yard (RPN)](./shunting_yard/)
This approach uses the Shunting-Yard algorithm to convert the `WHERE` clause from infix notation to postfix (Reverse Polish Notation) and evaluates it using a simple stack. 
- **Pros:** Fast, lean, doesn't require complex tree structures.
- **Cons:** Flat structure; cannot carry additional metadata.

### 2. [Recursive-Descent Query Parser (AST)](./queryparsing/)
This approach builds an Abstract Syntax Tree (AST) where the precedence of operations is baked directly into the grammar rules. The tree is then walked recursively to evaluate rows.
- **Pros:** Structured, carries metadata perfectly. Matches how real DBMS query planners (like PostgreSQL) parse queries.
- **Cons:** Slightly more complex codebase; requires tree traversal overhead.

---

## Quick Start

Both folders contain their own `makefile` and specific documentation.
Navigate to either folder and run:

```bash
make          # Compiles the source
make run      # Compiles and instantly executes the driver
make clean    # Cleans up build artifacts
```
