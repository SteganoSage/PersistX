<h1 align="center">PersistX</h1>

<p align="center">
  <b>A production-grade educational storage engine built in modern C++17</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue?logo=cplusplus&logoColor=white" alt="C++17"/>
  <img src="https://img.shields.io/badge/Build-CMake_3.20+-064F8C?logo=cmake&logoColor=white" alt="CMake"/>
  <img src="https://img.shields.io/badge/ACID-Compliant-brightgreen" alt="ACID"/>
  <img src="https://img.shields.io/badge/Recovery-ARIES-orange" alt="ARIES"/>
  <img src="https://img.shields.io/badge/Index-B%2B_Tree-blueviolet" alt="B+ Tree"/>
  <img src="https://img.shields.io/badge/License-Educational-lightgrey" alt="License"/>
</p>

<p align="center">
  <i>PersistX demonstrates how real-world databases (PostgreSQL, MySQL/InnoDB, SQLite) manage persistent storage — from raw disk I/O to crash-safe ACID transactions — all in ~12,000 lines of hand-written C++.</i>
</p>

---

## ✨ Highlights

- **Full ACID Transactions** — begin, commit, and abort with strict atomicity and durability guarantees
- **ARIES Crash Recovery** — three-phase protocol (Analysis → Redo → Undo) with CLR support for crash-safe aborts
- **B+ Tree Index** — O(log N) point lookups, insertions, deletions, and range scans with leaf-linked-list traversal
- **Write-Ahead Logging** — all modifications are logged _before_ data pages are flushed; log is force-flushed on commit
- **Buffer Pool Manager** — fixed-size in-memory page cache with LRU eviction and WAL-enforced dirty page writeback
- **Slotted Page Layout** — variable-length records with stable `RID = (page_id, slot_id)` identifiers that survive compaction
- **Fuzzy Checkpointing** — non-blocking checkpoint protocol that snapshots the Active Transaction Table (ATT) and Dirty Page Table (DPT)
- **Interactive Shell** — feature-rich REPL with ANSI-colored output, box-drawn tables, B+ tree visualization, and benchmarking tools

---

## 🏗️ Architecture

PersistX follows a strict **layered architecture** with single-responsibility components and clean interface boundaries — the same design philosophy used by PostgreSQL, InnoDB, and BusTub.

```
                    ┌─────────────────────────┐
                    │   Interactive Shell      │  REPL interface
                    │   (ANSI terminal UI)     │
                    └────────────┬────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │   Transaction Manager    │  BEGIN / COMMIT / ABORT
                    │   (ATT, CLR undo chain) │  Crash-safe abort via CLRs
                    └────────────┬────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
  ┌───────────▼──────────┐ ┌────▼───────────┐ ┌────▼──────────────┐
  │   B+ Tree Index      │ │  WAL / Log     │ │  Checkpoint       │
  │   (search, insert,   │ │  Manager       │ │  Manager          │
  │    delete, range)    │ │  (append, flush)│ │  (ATT + DPT snap) │
  └───────────┬──────────┘ └────┬───────────┘ └───────────────────┘
              │                  │
              │      ┌───────────▼───────────┐
              └──────►   Buffer Manager      │  In-memory page cache
                     │   (LRU eviction,      │  WAL enforcement
                     │    pin/unpin, flush)   │
                     └───────────┬───────────┘
                                 │
                     ┌───────────▼───────────┐
                     │   Disk Manager        │  Raw page I/O
                     │   (read / write /     │  File management
                     │    allocate pages)    │
                     └───────────┬───────────┘
                                 │
                     ┌───────────▼───────────┐
                     │   persistx.db         │  Persistent storage
                     │   persistx.log        │  WAL file
                     └───────────────────────┘
```

### Component Summary

| Component               | File(s)                                      | Responsibility                                                                                             |
| ----------------------- | -------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| **Slotted Page**        | `page.hpp/cpp`                               | Fixed 4 KB page layout with variable-length records, tombstone-aware slot directory, and O(1) space checks |
| **Disk Manager**        | `disk_manager.hpp/cpp`                       | Thread-safe raw page I/O to a single database file                                                         |
| **Buffer Manager**      | `buffer_manager.hpp/cpp`, `replacer.hpp/cpp` | In-memory page cache with LRU eviction, pin counting, and WAL-enforced writeback                           |
| **Log Manager**         | `log_manager.hpp/cpp`, `log_record.hpp`      | Append-only WAL with in-memory buffering, group flush, and LSN-indexed record lookup                       |
| **Transaction Manager** | `transaction_manager.hpp/cpp`                | Full transaction lifecycle — BEGIN, COMMIT (force-flush), ABORT (CLR undo chain)                           |
| **Checkpoint Manager**  | `checkpoint_manager.hpp/cpp`                 | Fuzzy checkpoints — snapshots ATT and DPT to the WAL without blocking transactions                         |
| **Recovery Manager**    | `recovery_manager.hpp/cpp`                   | ARIES three-phase recovery: Analysis, Redo, Undo with CLR skip logic                                       |
| **B+ Tree Index**       | `btree_index.hpp/cpp`, `btree_page.hpp/cpp`  | Ordered index with leaf/internal page layouts, recursive split, and linked-leaf range scan                 |
| **Shell**               | `shell.hpp/cpp`, `shell_commands.cpp`        | Interactive REPL with 20+ commands, ANSI rendering, and benchmark tools                                    |
| **Terminal**            | `terminal.hpp/cpp`                           | ANSI color helpers, box-drawing, table rendering, and Windows VT100 setup                                  |

---

## 🚀 Getting Started

### Prerequisites

- **C++17** compiler (GCC 9+, Clang 10+, or MSVC 2019+)
- **CMake** 3.20 or newer
- **pthreads** (Linux/macOS) or Windows threads

### Build

```bash
# Clone the repository
git clone https://github.com/yourusername/PersistX.git
cd PersistX

# Configure and build
cmake -S . -B build -G "MinGW Makefiles"    # Windows with MinGW
# cmake -S . -B build                       # Linux / macOS
cmake --build build

# Run the interactive shell
./build/persistx.exe          # Windows
# ./build/persistx            # Linux / macOS
```

### Run Tests

PersistX ships with a comprehensive test suite covering every layer:

```bash
./build/test_page.exe              # Slotted page layout tests
./build/test_disk_manager.exe      # Disk I/O tests
./build/test_buffer_manager.exe    # Buffer pool + eviction tests
./build/test_wal.exe               # Write-ahead logging tests
./build/test_transaction.exe       # Transaction ACID tests
./build/test_checkpoint.exe        # Checkpoint protocol tests
./build/test_recovery.exe          # ARIES crash recovery tests
./build/test_btree.exe             # B+ tree index tests
./build/overall_tests.exe          # Full integration test suite
```

---

## 💻 Interactive Shell

PersistX includes a beautifully rendered interactive REPL that exposes every layer of the storage engine:

```
  ╔════════════════════════════════════════════════════════════════════╗
  ║       ◆  P E R S I S T X   S t o r a g e   E n g i n e  ◆      ║
  ║                  v0.1.0  |  ACID  |  ARIES  |  B+Tree            ║
  ╚════════════════════════════════════════════════════════════════════╝

  Type help for available commands.

  persistx>
```

### Command Reference

#### Data Operations

| Command                | Description                                     |
| ---------------------- | ----------------------------------------------- |
| `put <key> <value>`    | Insert a key-value pair into the B+ tree        |
| `get <key>`            | Look up a value by key                          |
| `del <key>`            | Delete a key-value pair                         |
| `update <key> <value>` | Update the value for an existing key            |
| `scan <begin> <end>`   | Range scan — returns all keys in `[begin, end]` |

#### Transactions

| Command  | Description                                      |
| -------- | ------------------------------------------------ |
| `begin`  | Start an explicit transaction                    |
| `commit` | Commit the current transaction (force-flush WAL) |
| `abort`  | Abort and rollback via CLR undo chain            |

#### Diagnostics

| Command     | Description                                       |
| ----------- | ------------------------------------------------- |
| `status`    | System overview — buffer pool, WAL, B+ tree stats |
| `tree`      | Visualize the B+ tree structure level by level    |
| `buffer`    | Inspect buffer pool frame table                   |
| `wal [n]`   | Show last N WAL log entries                       |
| `page <id>` | Inspect a specific page (data or index)           |

#### System

| Command      | Description                                  |
| ------------ | -------------------------------------------- |
| `flush`      | Flush all dirty pages to disk                |
| `checkpoint` | Trigger a fuzzy checkpoint                   |
| `fill <n>`   | Bulk insert N sequential keys (benchmarking) |
| `bench <n>`  | Benchmark N insert + lookup operations       |
| `crash`      | Simulate a crash (`_Exit(1)` — no flush!)    |
| `recover`    | Run ARIES recovery + rebuild B+ tree index   |
| `clear`      | Clear the terminal screen                    |
| `exit`       | Shut down gracefully                         |

### Example Session

```
  persistx> put 1 "Hello, World!"
  ✓ INSERT  key=1  →  RID(2,0)  [LSN=1]

  persistx> put 2 "Database Internals"
  ✓ INSERT  key=2  →  RID(2,1)  [LSN=3]

  persistx> get 1
  1 → Hello, World!

  persistx> begin
  │ BEGIN  txn#2

  persistx[txn#2]> put 3 "ACID transactions"
  ✓ INSERT  key=3  →  RID(2,2)  [LSN=5]

  persistx[txn#2]> abort
  ✗ ABORT  txn#2

  persistx> get 3
  Key 3 not found

  persistx> crash
  💥 CRASH — killing process without flushing!

  persistx> recover
  🔄 Running ARIES recovery...
  ✓ ARIES complete (2.35ms)
  🔧 Rebuilding B+ tree index...
  ✓ Index rebuilt (2 keys recovered)
  ✓ Recovery complete (4.12ms total)
```

---

## 🧠 Technical Deep Dive

### Page Layout (Slotted Pages)

Every page is exactly **4096 bytes**, aligned for direct I/O:

```
┌──────────────────────────────────────────────────────┐
│ Header (21 bytes)                                    │
│  page_id (4) │ type (1) │ slots (2) │ tombstones (2) │
│  free_ptr (4) │ page_lsn (8)                         │
├──────────────────────────────────────────────────────┤
│ Record Data (grows ↓ from top)                       │
│  ┌──────┐ ┌────────────┐ ┌──────┐                   │
│  │ Rec0 │ │   Rec1     │ │ Rec2 │ ...               │
│  └──────┘ └────────────┘ └──────┘                   │
│                    ↕ free space ↕                    │
│  [Slot2][Slot1][Slot0]  ← Slot Directory (grows ↑)  │
└──────────────────────────────────────────────────────┘
```

- **Tombstone-aware**: deleted slots are marked (not removed), enabling O(1) `can_insert()` checks and stable RIDs
- **Compaction**: live records are rewritten contiguously while preserving slot indices — RIDs remain valid

### B+ Tree Index

- **Leaf pages**: sorted array of `(int64_t key, RID)` entries — max **291 entries/leaf**
- **Internal pages**: `child[0] | key[0] | child[1] | ... | child[N]` — max **340 keys/node**
- **Leaf linked list**: leaves are connected via `next_leaf_id` for efficient range scans
- **Lens pattern**: `BTreeLeafPage` and `BTreeInternalPage` are zero-cost wrappers over raw `Page*` buffers — no extra allocation
- **No merge on delete**: same policy as PostgreSQL — deleted entries are removed without rebalancing

### Write-Ahead Logging (WAL)

Every modification follows the **LOG → FLUSH → DATA WRITE** protocol:

1. Serialize the log record (before-image + after-image) into the in-memory buffer
2. Force-flush the log to disk **before** any dirty data page can be written
3. The `BufferManager::enforce_wal()` method guarantees this invariant on every eviction

**Log record types**: `BEGIN`, `UPDATE`, `COMMIT`, `ABORT`, `TXN_END`, `CLR`, `CHECKPOINT_BEGIN`, `CHECKPOINT_END`

### ARIES Recovery

On every startup (clean or crash), PersistX runs the full ARIES protocol:

| Phase        | What It Does                                                                                |
| ------------ | ------------------------------------------------------------------------------------------- |
| **Analysis** | Scan WAL from the last checkpoint to reconstruct the ATT and DPT                            |
| **Redo**     | Replay all logged changes — idempotent thanks to LSN comparison                             |
| **Undo**     | Roll back uncommitted transactions using the `prevLSN` chain, writing CLRs for crash safety |

After ARIES, the B+ tree index is rebuilt by scanning recovered data pages — ensuring total consistency even after an unclean shutdown.

### Transaction Abort (CLR Chain)

Abort walks the undo chain backward (`prevLSN` pointers), restoring before-images:

```
UPDATE(LSN=5) ← prevLSN ← UPDATE(LSN=3) ← prevLSN ← BEGIN(LSN=1)
    │                          │
    ▼                          ▼
CLR(LSN=7, undo_next=3)   CLR(LSN=8, undo_next=1)
```

Each CLR records which step was already undone, so a crash mid-abort can safely skip completed undo steps.

---

## 📁 Project Structure

```
PersistX/
├── include/                    # Header files (public API)
│   ├── common.hpp              # Shared types, constants, RID
│   ├── page.hpp                # Slotted page layout
│   ├── disk_manager.hpp        # Raw page I/O
│   ├── replacer.hpp            # LRU eviction policy
│   ├── buffer_manager.hpp      # In-memory page cache
│   ├── log_manager.hpp         # WAL management
│   ├── log_record.hpp          # Log record serialization
│   ├── transaction.hpp         # Transaction state
│   ├── transaction_manager.hpp # Transaction lifecycle
│   ├── checkpoint_manager.hpp  # Fuzzy checkpoints
│   ├── recovery_manager.hpp    # ARIES recovery
│   ├── btree_page.hpp          # B+ tree node layouts
│   ├── btree_index.hpp         # B+ tree public API
│   ├── shell.hpp               # Interactive shell
│   └── terminal.hpp            # ANSI terminal rendering
├── src/                        # Implementation files
│   ├── main.cpp                # Entry point
│   ├── page.cpp                # Slotted page operations
│   ├── disk_manager.cpp        # File I/O
│   ├── replacer.cpp            # LRU eviction
│   ├── buffer_manager.cpp      # Buffer pool management
│   ├── log_manager.cpp         # WAL append / flush / read
│   ├── transaction_manager.cpp # BEGIN / COMMIT / ABORT + CLR
│   ├── checkpoint_manager.cpp  # Checkpoint protocol
│   ├── recovery_manager.cpp    # Analysis / Redo / Undo
│   ├── btree_page.cpp          # Leaf & internal page ops
│   ├── btree_index.cpp         # Tree search / insert / split
│   ├── shell.cpp               # REPL loop + data commands
│   ├── shell_commands.cpp      # Diagnostics & system commands
│   └── terminal.cpp            # ANSI rendering utilities
├── tests/                      # Test suite
│   ├── test_page.cpp           # Page layout correctness
│   ├── test_disk_manager.cpp   # Disk I/O + allocation
│   ├── test_buffer_manager.cpp # Eviction, pinning, WAL
│   ├── test_wal.cpp            # Log append / flush / read
│   ├── test_transaction.cpp    # ACID transaction tests
│   ├── test_checkpoint.cpp     # Checkpoint correctness
│   ├── test_recovery.cpp       # Crash recovery scenarios
│   ├── test_btree.cpp          # B+ tree operations
│   └── overall_tests.cpp       # End-to-end integration
├── CMakeLists.txt              # Build configuration
└── README.md
```

---

## 🎯 Design Principles

1. **Page-based storage** — all data lives in fixed 4 KB pages for O_DIRECT compatibility
2. **Separation of concerns** — strict layered architecture with zero upward dependencies
3. **Write-ahead logging** — LOG → FLUSH → DATA WRITE, always
4. **Crash-first design** — the system is _always_ recoverable to a consistent state
5. **Stable record identifiers** — `RID = (page_id, slot_id)` survives compaction and page reorganization
6. **Zero-copy lenses** — B+ tree pages are interpreted in-place, no deserialization overhead
7. **Thread safety** — mutex-protected critical sections with documented latch ordering: `TxnMgr → BufferMgr → LogMgr`
8. **No UB** — all wire format access uses `memcpy` instead of `reinterpret_cast` to avoid strict-aliasing violations

---

## 📚 Inspired By

- [CMU 15-445 Database Systems](https://15445.courses.cs.cmu.edu/) — BusTub project
- [_Database Internals_](https://www.databass.dev/) by Alex Petrov
- [_ARIES: A Transaction Recovery Method_](https://cs.stanford.edu/people/chr101/aries.pdf) — Mohan et al., 1992
- PostgreSQL and InnoDB source code

---

## 📄 License

This project is for **educational purposes**. Built as part of the **IEEE NITK Envision 2026** program.
