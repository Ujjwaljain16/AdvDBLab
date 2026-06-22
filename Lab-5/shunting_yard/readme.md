# Advanced DBMS Lab 7: Dijkstra's Shunting-Yard (SQL WHERE Clause)

> **Author:** Ujjwal Jain (24BCS10173)  
> **Course:** Advanced Database Management Systems  
> **Language:** C++17

```LLM was used to polish and structure the content rest all the cmnds and practical was done by me ```

This repository contains an implementation of **Dijkstra's Shunting-Yard algorithm** used to convert a SQL `WHERE` clause from normal infix notation to postfix (Reverse Polish Notation, RPN), and then evaluates the postfix expression to filter rows in a database table.

---

## Table of Contents
1. [Included Files](#included-files)
2. [Compilation & Execution](#compilation--execution)
3. [The Concept](#the-concept)
4. [The Algorithm](#the-algorithm)
5. [Worked Example](#worked-example)
6. [Evaluating](#evaluating)
7. [Comparison with Parser](#comparison-with-parser)

---

## Included Files

| File | Description |
|------|-------------|
| `main.cpp` | Core implementation file containing the tokenizer, infix-to-postfix converter, and an evaluation driver testing it against student records. |
| `makefile` | Build script configured with standard C++17 compiler flags. |
| `readme.md` | This technical documentation. |
| `../queryparsing/` | Contains the alternative recursive-descent parser approach. |

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

### Why postfix?

Reading infix correctly needs precedence rules and parentheses: `2 + 3 * 4` is `14`, not `20`, because `*` binds tighter than `+`. A `WHERE` clause has the same issue — `marks >= 80 AND age < 20 OR id = 5` only means the right thing if you know comparisons bind tighter than `AND`, and `AND` tighter than `OR`.

Postfix removes the ambiguity. The token order already encodes what to do first, so evaluation is one left-to-right pass with a single stack — no parens, no precedence table, no recursion at eval time.

### Precedence Table

| Operator | Precedence |
|----------|------------|
| `>` `<` `>=` `<=` `=` | 3 |
| `AND` | 2 |
| `OR`  | 1 |

Higher number = binds tighter. All operators are left-associative, so the shunting-yard pop condition is `precedence(top) >= precedence(incoming)`.

---

## The Algorithm

The algorithm uses two containers — an output list and an operator stack. For each token:

- **Operand (column or number):** append to output
- **Operator `o`:** while the stack top is an operator with precedence `>= o`, pop it to output; then push `o`
- `(`: push
- `)`: pop to output until `(`, discard the `(`
- **End:** pop everything left to output

---

## Worked Example

Input: `marks >= 80 AND (age < 20 OR id = 5)`

| Token | Output so far | Stack |
|-------|---------------|-------|
| marks | marks | |
| >= | marks | >= |
| 80 | marks 80 | >= |
| AND | marks 80 >= | AND |
| ( | marks 80 >= | AND ( |
| age | marks 80 >= age | AND ( |
| < | marks 80 >= age | AND ( < |
| 20 | marks 80 >= age 20 | AND ( < |
| OR | marks 80 >= age 20 < | AND ( OR |
| id | marks 80 >= age 20 < id | AND ( OR |
| = | marks 80 >= age 20 < id | AND ( OR = |
| 5 | marks 80 >= age 20 < id 5 | AND ( OR = |
| ) | marks 80 >= age 20 < id 5 = OR | AND |
| end | marks 80 >= age 20 < id 5 = OR AND | |

**Final RPN:** `marks 80 >= age 20 < id 5 = OR AND`

---

## Evaluating

Walk the postfix with one int stack. Operands push their value (numbers as-is, columns looked up from the row). Each operator pops two values and pushes the result; comparison results are `0`/`1` and feed straight into `AND`/`OR`. The last value on the stack is the answer for that row.

**Sample Output:**
```
Infix WHERE : marks >= 80 AND (age < 20 OR id = 5)
Postfix     : marks 80 >= age 20 < id 5 = OR AND

Matching rows:
  Ishan (id=1, age=19, marks=88)
  Dev (id=5, age=21, marks=95)
```

`Ishan` passes (`88 >= 80` and `19 < 20`); `Dev` passes (`95 >= 80` and `id = 5`). The same two rows the AST parser in `../queryparsing/` returns.

---

## Comparison with Parser

Same problem, different shape: the parser builds a tree and recurses over it; shunting-yard flattens to RPN and evaluates with a stack. 

The parser's tree can carry metadata a flat list can't, which is why most real planners use one; the shunting-yard is lighter and handy for standalone expression evaluation.