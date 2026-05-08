# PersistX

A production-grade educational storage engine built in modern C++17.

PersistX simulates how real-world databases (PostgreSQL, MySQL/InnoDB, RocksDB) manage persistent storage, demonstrating core database internals from the ground up.

## Architecture

```
Client API
   ↓
Access Method (B+ Tree)
   ↓
Buffer Manager
   ↓
WAL Manager
   ↓
Disk Manager
   ↓
Persistent File (persistx.db)
```

Each layer has a **single responsibility** and strict interface boundaries:

| Layer                | Responsibility                         |
| -------------------- | -------------------------------------- |
| **Page**             | Slotted page layout, record storage    |
| **Disk Manager**     | Raw page I/O, file management          |
| **Buffer Manager**   | In-memory page cache, eviction (LRU-K) |
| **WAL Manager**      | Write-ahead logging, durability        |
| **Recovery Manager** | Crash recovery via log replay          |
| **B+ Tree**          | Ordered index: key → RID               |

## Building

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

## Running Tests

```bash
./build/test_page.exe
```

## Design Principles

1. **Page-based storage** — all data in fixed 4 KB pages
2. **Separation of concerns** — strict layered architecture
3. **Write-ahead logging** — LOG → FLUSH → DATA WRITE
4. **Crash-first design** — always recoverable to consistent state
5. **Stable record identifiers** — RID = (page_id, slot_id) survives compaction

## License

This project is for educational purposes.
