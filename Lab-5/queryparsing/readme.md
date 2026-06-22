# Advanced DBMS Lab 7: Recursive-Descent Query Parser

> **Author:** Ujjwal Jain (24BCS10173)  
> **Course:** Advanced Database Management Systems  
> **Language:** C++17

```LLM was used to polish and structure the content rest all the cmnds and practical was done by me ```

This repository contains an implementation of a **Recursive-Descent Query Parser**. It takes a small `SELECT ... FROM ... WHERE ...` statement, lexes it, parses the `WHERE` clause into an **Abstract Syntax Tree (AST)**, and walks that tree once per row to filter results.

---

## Table of Contents
1. [Included Files](#included-files)
2. [Compilation & Execution](#compilation--execution)
3. [The Concept](#the-concept)
4. [Architecture Pieces](#architecture-pieces)
5. [Sample Execution & Output](#sample-execution--output)
6. [Versus Shunting-Yard](#versus-shunting-yard)

---

## Included Files

| File | Description |
|------|-------------|
| `main.cpp` | Core implementation file containing the Lexer, Parser, AST definition, Evaluator and execution driver. |
| `makefile` | Build script configured with standard C++17 compiler flags. |
| `readme.md` | This technical documentation. |

---

## Compilation & Execution

This project includes a `makefile` for easy compilation. Open your terminal and run:

```bash
make          # Compiles the source into a binary named 'main'
make run      # Compiles and instantly executes the interactive driver
make clean    # Removes the compiled binary
```

Alternatively, you can manually compile it:
```bash
g++ -std=c++17 -Wall -Wextra -O2 -o main main.cpp
./main
```

---

## The Concept

A `WHERE` clause like `marks >= 80 AND (age < 20 OR id = 5)` has precedence rules: comparisons bind tightest, then `AND`, then `OR`. Instead of carrying a precedence table around, this approach **bakes precedence into the grammar** — each rule calls the next-tighter rule, so the tree comes out shaped correctly.

**Grammar Structure:**
```text
query  := SELECT Name FROM Name WHERE expr
expr   := term  (OR  term)*      # OR loosest  -> sits highest in the tree
term   := factor (AND factor)*   # AND tighter
factor := '(' expr ')' | Name Cmp Int   # comparisons tightest -> leaves
```

---

## Architecture Pieces

1. **Lexer (`lex`)**: One pass over the string producing a flat `vector<Token>`: keywords (`SELECT`/`FROM`/`WHERE`/`AND`/`OR`), identifiers, numbers, comparison ops (`> < >= <= =`), and parentheses.
2. **Parser (`Parser`)**: Recursive descent following the grammar above. Each `OR` / `AND` becomes a `BinOp` node; each comparison becomes a `BinOp` over a `Column` and a `Number`.
3. **AST**: A single tagged `Expr` struct (`Column` / `Number` / `BinOp`) rather than a virtual class hierarchy. This ensures the evaluator is short and doesn't require complex type casting.
4. **Evaluator (`eval`)**: A recursive tree walk where `AND`/`OR` recurse on both sides, and comparisons read column values dynamically from row structs to compare with numbers.

---

## Sample Execution & Output

```
Query: SELECT name FROM students WHERE marks >= 80 AND (age < 20 OR id = 5)

WHERE as an AST (precedence baked into the tree):
AND
  >=
    marks
    80
  OR
    <
      age
      20
    =
      id
      5

Matching rows:
  Ishan
  Dev
```

The `AND` is at the root with `>=` on the left and the parenthesised `OR` on the right — exactly the precedence we wanted. `Ishan` passes because `88 >= 80` and `19 < 20`; `Dev` passes because `95 >= 80` and `id = 5`.

---

## Versus Shunting-Yard

Here is how this implementation compares to the Dijkstra's Shunting-Yard version found in the sibling directory (`../shunting_yard`):

| Feature | This Folder (`queryparsing`) | Sibling Folder (`shunting_yard`) |
|---|---|---|
| **Output of parse** | AST (tree structure) | Flat postfix list (RPN) |
| **Where precedence lives** | Order of grammar rule calls | Precedence number + operator stack |
| **Evaluator** | Recursive tree walk | One-pass integer stack |

Real query planners in large DBMS (PostgreSQL, MySQL) keep a tree structure because it can carry extra metadata (types, row estimates, index choices) that a flat RPN list simply cannot. Shunting-yard algorithms are typically leaner and remain great building blocks (e.g., an expression inside a `CASE`).