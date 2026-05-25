# Lab 4: SQLite B-Tree Structure Deciphered

This document details the analysis of the `my_database.db` SQLite file, using a hex dump to identify and decipher the internal B-Tree structures used by SQLite.

## 1. File Structure Overview

SQLite databases are broken down into pages. By default, our database uses a 4096-byte (4KB) page size.
- **Page 1 (0x0000 - 0x0FFF):** Contains the 100-byte database header and the root B-Tree page (which holds the `sqlite_schema` table).
- **Page 2 (0x1000 - 0x1FFF):** Contains the B-Tree leaf page for our user-defined `sample` table.

## 2. Deciphering the Hex Dump

### The Database Header
The first 100 bytes (0x00 - 0x63) of the database form the global header:
```text
00000000: 53 51 4c 69 74 65 20 66 6f 72 6d 61 74 20 33 00   SQLite format 3.
00000010: 10 00 01 01 00 ...
```
- `53 51 4c 69 74 65 20 66 6f 72 6d 61 74 20 33 00` corresponds to "SQLite format 3\0".
- `10 00` indicates the page size is 4096 bytes (0x1000).

### Page 2: Table Leaf B-Tree Header
The second page starts at memory offset `0x1000`. Let's look at the beginning of this page to see the B-Tree node header:
```text
00001000: 0d 00 00 00 03 0f e2 00 0f f6 0f ee 0f e2 00 00
```
This is a **Table B-Tree Leaf Node**:
- **0x0D** (at `0x1000`): The B-Tree page type flag. `0x0D` (13) indicates a leaf table b-tree page.
- **0x00 00** (at `0x1001`): Offset to the first freeblock on the page (0 means no freeblocks).
- **0x00 03** (at `0x1003`): Number of cells (rows) on this page. There are exactly 3 rows in our `sample` table.
- **0x0F E2** (at `0x1005`): Offset to the start of the cell content area. `0x0FE2` + `0x1000` = `0x1FE2`.
- **0x00** (at `0x1007`): Fragmented free bytes count.

### Cell Pointer Array
Immediately following the 8-byte B-tree header (starting at `0x1008`), we have the Cell Pointer Array. It contains 2-byte offsets indicating where each of the 3 cells (rows) starts relative to the beginning of the page:
- **0x0F F6**: Cell 1 offset (Absolute offset = `0x1FF6`)
- **0x0F EE**: Cell 2 offset (Absolute offset = `0x1FEE`)
- **0x0F E2**: Cell 3 offset (Absolute offset = `0x1FE2`)

### Inspecting Cell Content (Payload)
Let's jump to offset `0x1FF6` to decode the first cell. This corresponds to the end of the hex dump:
```text
00001ff0: 03 00 13 42 6f 62 08 01 03 00 17 41 6c 69 63 65
```
At offset `0x1FF6`, we see: `08 01 03 00 17 41 6c 69 63 65`

Decoding this byte-by-byte (using SQLite variable-length integers/varints):
1. **0x08**: Total Payload Size (8 bytes of data follow).
2. **0x01**: RowID (Primary Key). This is row ID 1.
3. **0x03**: Header Size. The record header is 3 bytes long.
4. **0x00**: Serial Type Code for Column 1 (`id`). `0` indicates a NULL value or that it's the RowID alias.
5. **0x17**: Serial Type Code for Column 2 (`name`). `0x17` in decimal is 23. For text columns, the length is `(SerialType - 13) / 2`. Here, `(23 - 13) / 2 = 5`. This means a 5-byte string follows.
6. **41 6c 69 63 65**: The actual text payload. These are the ASCII values for `"Alice"`.

Similarly, for the other cells:
- At `0x1FEE`: `06 02 03 00 13 42 6f 62` -> Payload Size 6, RowID 2, String Length `(19-13)/2 = 3` bytes -> "Bob".
- At `0x1FE2`: `0a 03 03 00 1b 43 68 61 72 6c 69 65` -> Payload Size 10, RowID 3, String Length `(27-13)/2 = 7` bytes -> "Charlie".

## Summary
The SQLite `my_database.db` structure clearly demonstrates the B-Tree layout:
- **Root pages** contain schemas.
- **Leaf pages** (type `0x0D`) house the actual table data.
- **Node Headers** explicitly track cell counts and free space.
- **Cell Pointer Arrays** index records stored at the end of the page to maximize contiguous space.
- **Cells** store data using a packed varint format and serial types to optimize byte consumption.
