# excel-compatible-spreadsheet-engine

A spreadsheet calculation engine written from scratch in C++20. It parses Excel formulas, compiles them to bytecode, keeps a dependency graph between cells and recalculates either incrementally or across a thread pool. It also reads and writes .xlsx files with its own zip, deflate and XML code. There are no third-party libraries, only the standard library and CMake.

I built it to find out how a spreadsheet decides what to recompute after you change one cell, and how close an engine written from nothing can get to Excel's answers. To check the second part, I run it against the test spreadsheets of Apache POI and LibreOffice, which store the last computed value of every formula.

## Numbers

Measured on my laptop: HP OmniBook 5, Intel Core 5 120U (2 performance cores and 8 efficiency cores, 12 threads), 8 GB RAM, Windows 11, GCC 15.2 (MinGW-w64), Release build.

The benchmark workbook is 100,000 rows of a small finance model: 1,000,058 cells and 600,005 formulas (arithmetic, IF, ROUND and VLOOKUP on every row, plus SUM, AVERAGE, MAX, COUNTIF and SUMIF over the full 100,000-row columns).

| Threads | Full recalc | Speedup |
|--------:|------------:|--------:|
| 1 | 113 ms | 1.0x |
| 2 | 64 ms | 1.8x |
| 4 | 50 ms | 2.3x |
| 8 | 35 ms | 3.2x |
| 12 | 28 ms | 4.0x |

Changing one input cell dirties 9 formulas. Recalculating only those takes 3.2 ms (median of 200 random edits, p99 9.3 ms), while a full recalc after the same kind of edit takes 121 ms on one thread, so the incremental path is about 37x faster. Most of that time goes to the column aggregates, which still read 100,000 cells each.

Loading the same workbook from an 11 MB .xlsx file takes 2.5 s, and writing it takes 3.1 s.

The laptop has a 15 W chip, so these numbers move around with temperature and turbo. I ran each measurement after a short cool-down and report medians. `sheet bench --threads 1,2,4,8,12` runs the same measurements on your machine.

Conformance against the cached values in other people's test files:

| Test files | Formulas compared | Matching |
|---|---:|---:|
| Apache POI test spreadsheets (.xlsx) | 27,525 | 27,507 (99.93%) |
| LibreOffice function tests (.fods) | 25,383 | 23,658 (93.20%) |

Formulas that call a function I haven't implemented, or a volatile one like RAND or NOW, are skipped. Most of the remaining LibreOffice mismatches are places where LibreOffice intentionally behaves differently from Excel, like regular expressions in criteria, its 1899 date base and the error codes it picks for some edge cases.

## Features

- Formulas: relative, absolute and mixed references, ranges, whole rows and columns, 3D references like `Sheet1:Sheet3!A1`, defined names, structured references to tables (`Sales[Amount]`, `[@Price]`), array constants, array formulas, and the range, union and intersection operators
- 382 functions across math, statistics, text, lookup (VLOOKUP, XLOOKUP, INDEX, MATCH, OFFSET, INDIRECT), dates, finance, information and logic
- Excel behavior in the details: error propagation, implicit intersection, comparisons and rounding at 15 significant digits, General number display, TEXT format codes, 1900 and 1904 date systems
- Incremental recalculation through a dependency graph, with an interval index for ranges so a formula over `A1:A100000` is one entry instead of 100,000
- Parallel recalculation on a thread pool, cycle detection, and optional iterative calculation for circular references
- Insert and delete rows and columns with formulas rewritten the way Excel does it, including `#REF!` for deleted references
- .xlsx import and export (shared strings, shared and array formulas, tables, defined names, hidden rows, cached values), plus .fods import for the LibreOffice tests
- A command line tool with an interactive shell, batch recalc, a conformance checker and a benchmark

## How it works

Formulas. A hand-written lexer and recursive descent parser build a syntax tree, and a compiler turns it into bytecode for a small stack machine. References are stored relative to the formula's own cell, so a formula filled down a column compiles to the same program on every row. Programs are interned, which is why the 600,005 formulas in the benchmark share 11 programs.

Evaluation. The VM pushes values and references. Each function declares which arguments take references and which take values, which is how `SUM(A1:A10)` gets the whole range while `ABS(A1:A10)` gets an implicit intersection. Values are 16 bytes, and text and arrays are reference counted.

Dependencies. Single-cell references go into a hash table from cell to formulas. Ranges go into a per-column interval index, so finding every formula that reads a changed cell is a lookup plus a stabbing query instead of a scan.

Recalculation. An edit marks everything downstream of the changed cell as dirty, and an iterative Tarjan pass orders the dirty formulas and finds cycles at the same time. The resulting components are grouped into levels, and each level runs in parallel on the thread pool. The order is cached, so a full recalc, or an edit that dirties a big part of the workbook, skips this step until a formula changes. INDIRECT and OFFSET can read cells the graph doesn't know about, so when a formula reads a cell that is still dirty, evaluation stops, the engine computes that cell first and then retries.

Files. The xlsx code has its own zip reader and writer, inflate, a deflate compressor (LZ77 with hash chains, lazy matching and dynamic Huffman blocks), CRC-32 and a small XML pull parser.

## Building

You need CMake 3.20 or newer and a C++20 compiler. I've built it with GCC 13 and 15, Clang 18 and Visual Studio 2022.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

On Windows with MinGW I add `-G Ninja`. The two conformance tests need the POI and LibreOffice test files, which are not in this repo. This fetches them at pinned commits into `data/` (about 175 MB), and the next configure picks them up:

```
cmake -P tools/fetch_data.cmake
cmake -S . -B build
ctest --test-dir build
```

## Usage

```
sheet shell [file]                    interactive shell
sheet recalc book.xlsx --threads 4 --out out.xlsx
sheet check data/poi/test-data/spreadsheet
sheet bench --rows 100000 --threads 1,2,4,8
sheet gen big.xlsx --rows 100000
```

A short shell session:

```
Sheet1> A1 = 1200
1200    (0 recalculated in 0.003612 ms)
Sheet1> A2 = 0.05
0.05    (0 recalculated in 0.00021 ms)
Sheet1> A3 = =A1*(1+A2)
1260    (1 recalculated in 0.007928 ms)
Sheet1> insert rows 1 1
ok, 1 formula recalculated
Sheet1> A4
1260    =A2*(1+A3)
```

## Testing

There are 73 test cases in `tests/`, plus the two conformance runs.

- Unit tests for values, number formats, the parser and printer, every function family, the engine and file IO
- Differential tests on random workbooks: incremental recalc against a fresh full recalc after random edits (values, formulas, clears, row and column inserts and deletes), parallel against sequential, insert then delete as an identity, and an xlsx round trip
- Conformance against the POI and LibreOffice files, with a minimum match rate so a regression makes the test fail

CI builds with GCC and Clang on Linux (warnings as errors), on macOS and on Windows, runs the tests under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer, runs the conformance suites and checks formatting with clang-format.

## Layout

```
src/core.*         values, errors, text helpers
src/format.*       number formats and dates
src/formula.h      references and bytecode
src/parser.cpp     lexer, parser and bytecode compiler
src/printer.cpp    bytecode back to formula text
src/eval.h         evaluation context and function interface
src/vm.cpp         stack machine and argument handling
src/fn_*.cpp       the function library
src/depgraph.*     cell and range dependency index
src/workbook.*     cells, formulas, names, tables
src/recalc.cpp     ordering, cycles, sequential and parallel recalc
src/threadpool.h   thread pool
src/structure.cpp  row and column insert and delete
src/deflate.*      inflate and deflate
src/zip.*          zip archives
src/xml.*          XML pull parser
src/xlsx.*         xlsx and fods reading and writing
src/main.cpp       command line tool
src/bench.cpp      benchmark and workbook generator
tests/             unit and differential tests
tools/             script that fetches the conformance files
```

## License

MIT, see LICENSE.
