# Project B — CSV Mini Database and Query Engine

A compact CSV database engine in **C++17** that ingests CSV data, converts it into
typed in-memory tables, and executes SQL-like queries with filtering, projection,
aggregation, GROUP BY, ORDER BY and join support. It demonstrates core database
concepts — CSV parsing, schema inference, indexing, query parsing/execution and
their performance trade-offs — in a lightweight, extensible form.

The repository ships with the classic **Iris dataset**
([UCI ML Repository](https://archive.ics.uci.edu/ml/datasets/iris) /
[Kaggle: uciml/iris](https://www.kaggle.com/datasets/uciml/iris)) in
`data/Iris.csv` (150 rows) plus a small lookup table `data/SpeciesInfo.csv`
used to demonstrate joins.

## Building

No external dependencies — the CSV parser is written from scratch (handles quoted
fields, `""` escapes, embedded newlines, CRLF). Everything is header-only except
the two entry points, so a single compiler invocation suffices:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -o minidb src/main.cpp        # the CLI
g++ -std=c++17 -O2 -Wall -Wextra -o run_tests tests/test_main.cpp  # the tests
```

or use the provided `Makefile` (`make`, `make test`, `make bench`) or
`CMakeLists.txt` (`cmake -B build && cmake --build build`).
On Windows/MinGW add `-static` so the executables don't depend on which
runtime DLLs happen to be in `PATH`.

## Usage

```sh
./minidb data/Iris.csv data/SpeciesInfo.csv          # load CSVs, start the REPL
./minidb data/Iris.csv -q "SELECT COUNT(*) FROM Iris"
./minidb data/Iris.csv data/SpeciesInfo.csv -f queries/demo.sql
./minidb --bench                                     # index benchmark (see below)
```

Each CSV becomes a table named after the file (`data/Iris.csv` → `Iris`); use
`name=path` to pick the name explicitly. Inside the REPL, queries end with `;`
and meta commands start with `.`:

```
.tables               list loaded tables
.schema <table>       show columns, inferred types, row count, indexes
.index <table> <col>  build a hash index on a column
.load <path> [name]   load another CSV
.timer on             print execution time and the query plan
.help / .quit
```

Example session:

```
minidb> SELECT Species, COUNT(*) AS n, AVG(PetalLengthCm) AS avg_petal
   ...> FROM Iris GROUP BY Species ORDER BY avg_petal DESC;
Species          n   avg_petal
---------------  --  ---------
Iris-virginica   50  5.552
Iris-versicolor  50  4.26
Iris-setosa      50  1.464
(3 rows)

minidb> SELECT i.Id, s.CommonName FROM Iris i
   ...> JOIN SpeciesInfo s ON i.Species = s.Species WHERE i.PetalLengthCm > 6.5;
Id   CommonName
---  -------------
118  Virginia iris
119  Virginia iris
123  Virginia iris
106  Virginia iris
(4 rows)
```

## Query language

```
query     := SELECT items FROM tableRef [join] [WHERE expr]
             [GROUP BY colRef ("," colRef)*]
             [ORDER BY colRef [ASC|DESC] ("," ...)*]
             [LIMIT number] [";"]
items     := "*" | item ("," item)*
item      := agg "(" ("*" | colRef) ")" [[AS] alias] | colRef [[AS] alias]
agg       := COUNT | SUM | AVG | MIN | MAX
join      := [INNER] JOIN tableRef ON colRef "=" colRef
expr      := andExpr (OR andExpr)*
andExpr   := notExpr (AND notExpr)*
notExpr   := NOT notExpr | "(" expr ")" | operand cmpOp operand
operand   := colRef | number | 'string' | TRUE | FALSE
cmpOp     := = | != | <> | < | <= | > | >=
colRef    := ident ["." ident]          -- optional table/alias qualifier
```

Keywords are case-insensitive; strings use single quotes with `''` as escape;
`--` starts a line comment. NULL (an empty CSV field) follows SQL semantics:
comparisons with NULL never match and aggregates skip NULLs.

## Architecture

| Module | Responsibility |
|---|---|
| [src/csv.hpp](src/csv.hpp) | Single-pass state-machine CSV parser (RFC 4180-style) |
| [src/value.hpp](src/value.hpp) | Dynamically typed cell value (`null/bool/int/float/string`) and comparison semantics |
| [src/table.hpp](src/table.hpp) | Typed tables, column type inference, hash indexes |
| [src/query.hpp](src/query.hpp) | Lexer, AST and recursive-descent parser for the grammar above |
| [src/executor.hpp](src/executor.hpp) | Catalog (`Database`) and execution pipeline |
| [src/main.cpp](src/main.cpp) | CLI: REPL, script runner, benchmark |
| [tests/test_main.cpp](tests/test_main.cpp) | 164-check unit test suite |

**Execution pipeline** (`Database::run`):

1. **FROM / JOIN** — produce source rows. Joins use a *hash join*: build a hash
   table on the right side's join key (reusing a user-built index if present),
   probe with each left row — `O(L + R)` instead of the naive nested loop's
   `O(L × R)`.
2. **WHERE** — the predicate tree is compiled once into closures (columns are
   resolved per query, not per row). If the predicate contains an AND-ed
   `column = literal` on an indexed column, the engine fetches candidates from
   the index and re-checks the full predicate, so an index can change the plan
   but never the result.
3. **GROUP BY / aggregates** — single-pass online accumulation per group
   (COUNT/SUM/AVG/MIN/MAX), with the SQL rule that bare columns must appear in
   GROUP BY.
4. **ORDER BY** — stable multi-key sort (before projection for plain queries so
   non-selected columns can be sort keys; over output columns when aggregating
   so aliases like `ORDER BY avg_petal` work).
5. **LIMIT / projection** — build the final result set.

With `.timer on` (or `-f` script mode) the engine prints what it actually did:

```
-- plan: index lookup on Iris.Species (50 of 150 rows)
-- time: 0.05 ms
```

## Schema inference

Column types are inferred by scanning all values with precedence
**int > float > bool > string**; empty fields are NULL and don't constrain the
type. For Iris this yields `Id:int`, the four measurements `float`,
`Species:string` — so `WHERE SepalLengthCm > 7` is a *numeric* comparison, not
a string one. Comparing incompatible types (e.g. `Species > 5`) is a query
error rather than a silent mismatch.

## Indexing and performance trade-offs

The engine implements a **hash index**: `value → list of row ids`. Build one
with `.index Iris Species` and equality predicates on that column switch from a
full scan to a hash lookup. `./minidb --bench` measures the trade-off on a
synthetic 1,000,000-row table (numbers from one run on this machine):

| | full scan | indexed | speedup |
|---|---|---|---|
| point query (`id = 500000`, 1 match) | 79.5 ms | 0.010 ms | ~8000× |
| low-selectivity (`species = …`, ⅓ of rows match) | 99.3 ms | 60.2 ms | 1.65× |

with a one-time index build cost of ~860 ms (id) — i.e. the index pays for
itself after about **11 point queries**, but barely helps the low-selectivity
query because most of the time goes into materializing the many matching rows.

Trade-offs demonstrated / discussed:

- **Index build cost vs lookup speed** — an index is an O(n) pass plus extra
  memory (every value's canonical string + row-id lists); it only wins if the
  column is queried repeatedly with selective predicates.
- **Selectivity matters** — a hash lookup that returns 333 k candidate rows
  saves only the predicate evaluation, not the per-row result handling.
- **Hash vs sorted index** — a hash index supports only `=`; range predicates
  (`<`, `>`, `BETWEEN`) would need a sorted structure (B-tree, sorted column)
  with O(log n) lookups — slower for pure equality, but far more versatile.
- **Hash join vs nested loop** — the hash join costs O(L + R) time and O(R)
  memory; a nested loop needs no memory but O(L × R) time. For 150 × 3 rows
  both are instant; at a million rows the nested loop is hopeless.
- **Row store vs column store** — rows are stored row-major
  (`vector<vector<Value>>`), which is simple and good for `SELECT *`, but a
  columnar layout would be more cache-friendly for aggregations that touch a
  single column.
- **Type inference up front** — parsing every field once at load time costs
  ingestion latency but makes every later comparison a cheap typed operation
  instead of repeated string parsing.

## Error handling

- malformed CSV (unterminated quotes, junk after a closing quote, rows with
  the wrong field count, empty file) → descriptive `CSV error: …`
- invalid queries (syntax errors, unknown tables/columns, ambiguous columns,
  non-numeric `SUM`/`AVG`, ungrouped bare columns, incomparable types) →
  descriptive `query error: …`
- the REPL reports errors and keeps running.

## Tests

```sh
make test          # or: g++ -std=c++17 -O2 -o run_tests tests/test_main.cpp && ./run_tests
```

164 checks covering: the CSV parser (quoting, escapes, embedded newlines, CRLF,
blank lines, malformed input), type inference, schema of the real Iris file,
filtering/projection/ORDER BY/LIMIT, aggregation and GROUP BY (validated
against known Iris statistics, e.g. `AVG(SepalLengthCm) = 5.8433`), joins,
aliases, error cases, and that **indexed and non-indexed execution return
identical results**. Run from the repository root (the tests load `data/Iris.csv`).

## Repository layout

```
├── src/                  engine + CLI sources (header-only modules)
├── tests/test_main.cpp   unit tests
├── data/Iris.csv         Iris dataset (UCI / Kaggle uciml/iris)
├── data/SpeciesInfo.csv  lookup table for join demos
├── queries/demo.sql      sample query script
├── Makefile / CMakeLists.txt
```

## Possible extensions

Subqueries, `LEFT JOIN`, `DISTINCT`, sorted indexes for range predicates,
arithmetic expressions in SELECT, columnar storage, and spill-to-disk for
tables larger than memory.
