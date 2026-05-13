// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — Overall Rigorous Test Suite
// ═══════════════════════════════════════════════════════════════════════════════
//
// Tests every layer of the storage engine end-to-end:
//   Section A  — BTreeLeafPage  (unit)
//   Section B  — BTreeInternalPage (unit)
//   Section C  — BTreeIndex basic ops (no splits)
//   Section D  — BTreeIndex leaf splits
//   Section E  — BTreeIndex internal splits
//   Section F  — BTreeIndex range scan
//   Section G  — BTreeIndex delete
//   Section H  — BTreeIndex edge cases
//   Section I  — BTreeIndex large scale / stress
//   Section J  — Page (slotted heap page) unit tests
//   Section K  — BufferManager + DiskManager integration
//   Section L  — Cross-layer integration (heap + index together)
//
// Build:
//   add_executable(overall_tests tests/overall_tests.cpp)
//   target_link_libraries(overall_tests PRIVATE persistx_lib)
// ═══════════════════════════════════════════════════════════════════════════════

#include "btree_index.hpp"
#include "btree_page.hpp"
#include "buffer_manager.hpp"
#include "disk_manager.hpp"
#include "page.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <set>
#include <map>

// ─── test infrastructure ──────────────────────────────────────────────────────

static int  g_total  = 0;
static int  g_passed = 0;
static int  g_failed = 0;
static std::string g_current_section;

static void section(const char* name) {
    g_current_section = name;
    std::cout << "\n── " << name << " ──\n";
}

static void check(bool condition, const char* test_name) {
    ++g_total;
    if (condition) {
        ++g_passed;
        std::cout << "  [PASS] " << test_name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << test_name << "  ← " << g_current_section << "\n";
    }
}

// Helper: make a RID from two integers
static RID rid(uint32_t p, uint16_t s) {
    RID r; r.page_id = p; r.slot_id = s; return r;
}

// ─── fixture: a fresh BTreeIndex backed by a temp file ───────────────────────

struct TreeFixture {
    const char*   path;
    DiskManager   dm;
    BufferManager bm;
    BTreeIndex    idx;

    explicit TreeFixture(const char* p, size_t pool = 512)
        : path(p), dm(p), bm(&dm, pool), idx(&bm) {
        idx.create();
    }
    ~TreeFixture() { std::remove(path); }
};

// ─── fixture: a standalone Page (not in a pool) ──────────────────────────────

struct PageFixture {
    Page page;
    PageFixture() { page.init(0, PageType::DATA); }
};

// ═════════════════════════════════════════════════════════════════════════════
// SECTION A — BTreeLeafPage unit tests
// ═════════════════════════════════════════════════════════════════════════════

static void section_A() {
    section("A: BTreeLeafPage unit tests");

    // We need a raw page to wrap.
    Page raw;
    raw.init(7, PageType::INDEX);
    BTreeLeafPage leaf(&raw);
    leaf.init(7);

    // A1: header after init
    check(leaf.get_page_id()      == 7,            "A1 get_page_id after init");
    check(leaf.get_node_type()    == BTREE_NODE_LEAF, "A1 node_type == LEAF");
    check(leaf.get_num_keys()     == 0,            "A1 num_keys == 0");
    check(leaf.get_next_leaf_id() == INVALID_PAGE_ID, "A1 next_leaf_id == INVALID");
    check(leaf.get_parent_id()    == INVALID_PAGE_ID, "A1 parent_id == INVALID");

    // A2: single insert + lookup
    RID r1 = rid(10, 3);
    check(leaf.insert(42, r1),    "A2 insert succeeds");
    check(leaf.get_num_keys() == 1, "A2 num_keys == 1");
    RID found = leaf.lookup(42);
    check(found.page_id == 10 && found.slot_id == 3, "A2 lookup finds correct RID");

    // A3: lookup missing key
    check(!leaf.lookup(99).is_valid(), "A3 lookup missing key → invalid");

    // A4: sorted order maintained
    leaf.insert(10, rid(1, 0));
    leaf.insert(30, rid(3, 0));
    leaf.insert(20, rid(2, 0));
    // keys should be [10, 20, 30, 42]
    check(leaf.get_key_at(0) == 10 &&
          leaf.get_key_at(1) == 20 &&
          leaf.get_key_at(2) == 30 &&
          leaf.get_key_at(3) == 42, "A4 sorted order maintained");

    // A5: key_index boundary conditions
    check(leaf.key_index(5)  == 0, "A5 key_index before all keys → 0");
    check(leaf.key_index(10) == 0, "A5 key_index == first key → 0");
    check(leaf.key_index(15) == 1, "A5 key_index between 10 and 20 → 1");
    check(leaf.key_index(99) == 4, "A5 key_index after all keys → num_keys");

    // A6: remove existing key
    check(leaf.remove(20),         "A6 remove existing key → true");
    check(leaf.get_num_keys() == 3, "A6 num_keys decremented");
    check(!leaf.lookup(20).is_valid(), "A6 removed key not found");
    // surviving keys still correct
    check(leaf.get_key_at(0) == 10 &&
          leaf.get_key_at(1) == 30 &&
          leaf.get_key_at(2) == 42, "A6 remaining keys intact");

    // A7: remove non-existent key
    check(!leaf.remove(999), "A7 remove non-existent → false");
    check(leaf.get_num_keys() == 3, "A7 num_keys unchanged after failed remove");

    // A8: remove first key
    check(leaf.remove(10),          "A8 remove first key");
    check(leaf.get_key_at(0) == 30, "A8 new first key correct");

    // A9: remove last key
    check(leaf.remove(42),          "A9 remove last key");
    check(leaf.get_key_at(0) == 30 && leaf.get_num_keys() == 1,
          "A9 only middle key remains");

    // A10: negative keys
    leaf.insert(-100, rid(5, 0));
    leaf.insert(-1,   rid(6, 0));
    check(leaf.get_key_at(0) == -100 &&
          leaf.get_key_at(1) == -1,   "A10 negative keys sorted correctly");

    // A11: next_leaf_id round-trip
    leaf.set_next_leaf_id(42);
    check(leaf.get_next_leaf_id() == 42, "A11 next_leaf_id round-trip");

    // A12: parent_id round-trip
    leaf.set_parent_id(7);
    check(leaf.get_parent_id() == 7, "A12 parent_id round-trip");

    // A13: fill to capacity and verify insert returns false when full
    Page raw2;
    raw2.init(99, PageType::INDEX);
    BTreeLeafPage full(&raw2);
    full.init(99);
    bool all_ok = true;
    for (size_t i = 0; i < LEAF_MAX_ENTRIES; ++i)
        all_ok &= full.insert(static_cast<int64_t>(i), rid(0, 0));
    check(all_ok, "A13 insert up to LEAF_MAX_ENTRIES all succeed");
    check(!full.insert(9999, rid(0, 0)), "A13 insert beyond capacity returns false");
    check(full.get_num_keys() == LEAF_MAX_ENTRIES, "A13 num_keys == LEAF_MAX_ENTRIES");
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION B — BTreeInternalPage unit tests
// ═════════════════════════════════════════════════════════════════════════════

static void section_B() {
    section("B: BTreeInternalPage unit tests");

    Page raw;
    raw.init(1, PageType::INDEX);
    BTreeInternalPage node(&raw);
    node.init(1);

    // B1: header after init
    check(node.get_page_id()   == 1,               "B1 get_page_id after init");
    check(node.get_node_type() == BTREE_NODE_INTERNAL, "B1 node_type == INTERNAL");
    check(node.get_num_keys()  == 0,               "B1 num_keys == 0");
    check(node.get_parent_id() == INVALID_PAGE_ID, "B1 parent_id == INVALID");

    // B2: build a node with 3 keys manually: c0 | k0=10 | c1 | k1=20 | c2 | k2=30 | c3
    node.set_num_keys(3);
    node.set_child_at(0, 100);
    node.set_key_at  (0, 10);
    node.set_child_at(1, 101);
    node.set_key_at  (1, 20);
    node.set_child_at(2, 102);
    node.set_key_at  (2, 30);
    node.set_child_at(3, 103);

    // B3: get_key_at / get_child_at round-trip
    check(node.get_key_at(0)   == 10,  "B3 key[0] == 10");
    check(node.get_key_at(1)   == 20,  "B3 key[1] == 20");
    check(node.get_key_at(2)   == 30,  "B3 key[2] == 30");
    check(node.get_child_at(0) == 100, "B3 child[0] == 100");
    check(node.get_child_at(3) == 103, "B3 child[3] == 103");

    // B4: lookup_child routing
    check(node.lookup_child(5)  == 100, "B4 key < 10  → child[0]");
    check(node.lookup_child(10) == 101, "B4 key == 10 → child[1]");
    check(node.lookup_child(15) == 101, "B4 10 < key < 20 → child[1]");
    check(node.lookup_child(20) == 102, "B4 key == 20 → child[2]");
    check(node.lookup_child(30) == 103, "B4 key == 30 → child[3]");
    check(node.lookup_child(99) == 103, "B4 key > 30  → child[3]");

    // B5: insert_after in the middle
    // Insert key=15, new_child=200 after child[0]=100
    check(node.insert_after(100, 15, 200), "B5 insert_after returns true");
    // Now: c0=100 | k=10 | c1=200 | k=15 | ... wait — insert_after puts new
    // key AT position of old_child, shifting right. Verify routing still works.
    check(node.get_num_keys() == 4, "B5 num_keys incremented to 4");
    // child 100 still routes keys < 10
    check(node.lookup_child(5) == 100, "B5 routing below 10 still works");

    // B6: parent_id round-trip
    node.set_parent_id(55);
    check(node.get_parent_id() == 55, "B6 parent_id round-trip");

    // B7: fill internal node to capacity, verify insert_after returns false
    Page raw2;
    raw2.init(2, PageType::INDEX);
    BTreeInternalPage full(&raw2);
    full.init(2);
    full.set_num_keys(static_cast<uint16_t>(INTERNAL_MAX_KEYS));
    check(!full.insert_after(INVALID_PAGE_ID, 0, INVALID_PAGE_ID),
          "B7 insert_after on full node returns false");
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION C — BTreeIndex basic ops (single leaf, no splits)
// ═════════════════════════════════════════════════════════════════════════════

static void section_C() {
    section("C: BTreeIndex basic ops (no splits)");

    TreeFixture f("overall_C.db");

    // C1: search on empty tree
    check(!f.idx.search(1).is_valid(), "C1 search empty tree → invalid");

    // C2: remove on empty tree
    check(!f.idx.remove(1), "C2 remove from empty tree → false");

    // C3: range scan on empty tree
    check(f.idx.range_scan(0, 100).empty(), "C3 range_scan empty tree → empty");

    // C4: single insert + search
    check(f.idx.insert(5, rid(5, 0)), "C4 insert key 5");
    RID r = f.idx.search(5);
    check(r.page_id == 5, "C4 search finds key 5");

    // C5: insert multiple keys, all retrievable
    for (int i = 0; i < 20; ++i)
        f.idx.insert(i * 10, rid(i, 1));
    for (int i = 0; i < 20; ++i)
        check(f.idx.search(i * 10).page_id == (uint32_t)i,
              ("C5 search key " + std::to_string(i * 10)).c_str());

    // C6: duplicate rejection
    check(!f.idx.insert(5, rid(99, 99)), "C6 duplicate insert → false");
    check(f.idx.search(5).page_id == 5,  "C6 original RID preserved");

    // C7: search for key between existing keys
    check(!f.idx.search(7).is_valid(), "C7 search between keys → invalid");

    // C8: remove key, verify gone
    check(f.idx.remove(5),              "C8 remove key 5 → true");
    check(!f.idx.search(5).is_valid(),  "C8 removed key not found");
    check(f.idx.remove(5) == false,     "C8 double-remove → false");

    // C9: keys before/after removed key still present
    check(f.idx.search(0).page_id  == 0,  "C9 key 0 still present");
    check(f.idx.search(10).page_id == 1,  "C9 key 10 still present");
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION D — BTreeIndex leaf splits
// ═════════════════════════════════════════════════════════════════════════════

static void section_D() {
    section("D: BTreeIndex leaf splits");

    // D1: exactly LEAF_MAX_ENTRIES+1 keys forces one split
    {
        TreeFixture f("overall_D1.db");
        const int N = static_cast<int>(LEAF_MAX_ENTRIES) + 1;
        for (int i = 0; i < N; ++i)
            check(f.idx.insert(i, rid(i, 0)),
                  ("D1 insert key " + std::to_string(i)).c_str());
        bool all_found = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).page_id != (uint32_t)i) all_found = false;
        check(all_found, "D1 all keys retrievable after first leaf split");
    }

    // D2: split with keys inserted in reverse order
    {
        TreeFixture f("overall_D2.db");
        const int N = static_cast<int>(LEAF_MAX_ENTRIES) + 1;
        for (int i = N - 1; i >= 0; --i)
            f.idx.insert(i, rid(i, 0));
        bool all_found = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).page_id != (uint32_t)i) all_found = false;
        check(all_found, "D2 reverse-order keys after split all retrievable");
    }

    // D3: split — keys on both sides of the boundary
    {
        TreeFixture f("overall_D3.db");
        const int N = static_cast<int>(LEAF_MAX_ENTRIES) + 5;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i * 2, rid(i, 0));   // even keys 0,2,4,...
        // insert odd keys between existing ones
        for (int i = 0; i < 10; ++i)
            f.idx.insert(i * 2 + 1, rid(1000 + i, 0));
        // spot check
        check(f.idx.search(0).page_id    == 0,    "D3 key 0 after interleaved inserts");
        check(f.idx.search(1).page_id    == 1000,  "D3 key 1 after interleaved inserts");
        check(f.idx.search(N * 2 - 2).is_valid(), "D3 last even key still present");
    }

    // D4: range scan across split boundary
    {
        TreeFixture f("overall_D4.db");
        const int N = static_cast<int>(LEAF_MAX_ENTRIES) + 50;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i, rid(i, 0));
        auto results = f.idx.range_scan(0, N - 1);
        check((int)results.size() == N,  "D4 range scan covers all keys after split");
        bool ordered = true;
        for (int i = 0; i < N; ++i)
            if (results[i].page_id != (uint32_t)i) ordered = false;
        check(ordered, "D4 range scan returns keys in order after split");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION E — BTreeIndex internal splits
// ═════════════════════════════════════════════════════════════════════════════

static void section_E() {
    section("E: BTreeIndex internal splits");

    // E1: enough keys to force internal node splits (multiple levels)
    {
        TreeFixture f("overall_E1.db");
        // Each leaf holds 291 keys. An internal node holds 340 keys (341 children).
        // 341 * 291 = ~99,231 keys to fill one internal node's worth of leaves.
        // Use 5000 keys — enough for several internal splits.
        const int N = 5000;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i, rid(i, 0));
        bool all_found = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).page_id != (uint32_t)i) all_found = false;
        check(all_found, "E1 5000 sequential keys all retrievable");
    }

    // E2: random order inserts at scale
    {
        TreeFixture f("overall_E2.db");
        const int N = 2000;
        std::vector<int> keys(N);
        for (int i = 0; i < N; ++i) keys[i] = i;
        std::mt19937 rng(42);
        std::shuffle(keys.begin(), keys.end(), rng);
        for (int k : keys)
            f.idx.insert(k, rid(k, 0));
        bool all_found = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).page_id != (uint32_t)i) all_found = false;
        check(all_found, "E2 2000 random-order keys all retrievable");
    }

    // E3: range scan across multiple internal nodes
    {
        TreeFixture f("overall_E3.db");
        const int N = 3000;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i, rid(i, 0));
        auto results = f.idx.range_scan(500, 1500);
        check(results.size() == 1001, "E3 range scan [500..1500] = 1001 results");
        bool correct = true;
        for (size_t i = 0; i < results.size(); ++i)
            if (results[i].page_id != (uint32_t)(500 + i)) correct = false;
        check(correct, "E3 range scan results in correct order");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION F — BTreeIndex range scan edge cases
// ═════════════════════════════════════════════════════════════════════════════

static void section_F() {
    section("F: BTreeIndex range scan edge cases");

    TreeFixture f("overall_F.db");
    for (int i = 0; i < 100; ++i)
        f.idx.insert(i, rid(i, 0));

    // F1: range with single result
    auto r1 = f.idx.range_scan(50, 50);
    check(r1.size() == 1 && r1[0].page_id == 50, "F1 range [50..50] → 1 result");

    // F2: range below all keys
    auto r2 = f.idx.range_scan(-100, -1);
    check(r2.empty(), "F2 range below all keys → empty");

    // F3: range above all keys
    auto r3 = f.idx.range_scan(200, 300);
    check(r3.empty(), "F3 range above all keys → empty");

    // F4: range covering all keys
    auto r4 = f.idx.range_scan(0, 99);
    check(r4.size() == 100, "F4 range [0..99] → 100 results");

    // F5: range starting before first key
    auto r5 = f.idx.range_scan(-10, 5);
    check(r5.size() == 6, "F5 range [-10..5] → 6 results (0..5)");

    // F6: range ending after last key
    auto r6 = f.idx.range_scan(95, 200);
    check(r6.size() == 5, "F6 range [95..200] → 5 results (95..99)");

    // F7: inverted range (begin > end) → empty
    auto r7 = f.idx.range_scan(50, 10);
    check(r7.empty(), "F7 inverted range → empty");

    // F8: range scan on empty tree
    TreeFixture empty("overall_F8.db");
    auto r8 = empty.idx.range_scan(0, 100);
    check(r8.empty(), "F8 range scan on empty tree → empty");
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION G — BTreeIndex delete edge cases
// ═════════════════════════════════════════════════════════════════════════════

static void section_G() {
    section("G: BTreeIndex delete edge cases");

    // G1: delete from single-entry tree
    {
        TreeFixture f("overall_G1.db");
        f.idx.insert(1, rid(1, 0));
        check(f.idx.remove(1),            "G1 delete only key → true");
        check(!f.idx.search(1).is_valid(), "G1 deleted key not found");
        check(!f.idx.remove(1),           "G1 double-delete → false");
    }

    // G2: delete first and last key in a multi-key leaf
    {
        TreeFixture f("overall_G2.db");
        for (int i = 1; i <= 10; ++i) f.idx.insert(i, rid(i, 0));
        f.idx.remove(1);
        f.idx.remove(10);
        check(!f.idx.search(1).is_valid(),  "G2 first key deleted");
        check(!f.idx.search(10).is_valid(), "G2 last key deleted");
        check(f.idx.search(5).page_id == 5, "G2 middle key still present");
    }

    // G3: delete all keys one by one
    {
        TreeFixture f("overall_G3.db");
        const int N = 10;
        for (int i = 0; i < N; ++i) f.idx.insert(i, rid(i, 0));
        for (int i = 0; i < N; ++i) f.idx.remove(i);
        bool all_gone = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).is_valid()) all_gone = false;
        check(all_gone, "G3 all keys deleted one by one");
    }

    // G4: delete across split boundary
    {
        TreeFixture f("overall_G4.db");
        const int N = static_cast<int>(LEAF_MAX_ENTRIES) + 50;
        for (int i = 0; i < N; ++i) f.idx.insert(i, rid(i, 0));
        // Delete the keys around the split point (approx LEAF_MAX_ENTRIES/2)
        int mid = static_cast<int>(LEAF_MAX_ENTRIES) / 2;
        for (int i = mid - 5; i <= mid + 5; ++i) f.idx.remove(i);
        bool deleted_gone = true;
        for (int i = mid - 5; i <= mid + 5; ++i)
            if (f.idx.search(i).is_valid()) deleted_gone = false;
        check(deleted_gone, "G4 keys at split boundary deleted");
        check(f.idx.search(0).page_id == 0,       "G4 key 0 still present");
        check(f.idx.search(N - 1).page_id == N - 1, "G4 last key still present");
    }

    // G5: insert after delete (reuse of key slot)
    {
        TreeFixture f("overall_G5.db");
        f.idx.insert(42, rid(1, 0));
        f.idx.remove(42);
        check(f.idx.insert(42, rid(2, 0)),        "G5 re-insert after delete succeeds");
        check(f.idx.search(42).page_id == 2,      "G5 re-inserted RID is new value");
    }

    // G6: delete non-existent key returns false
    {
        TreeFixture f("overall_G6.db");
        f.idx.insert(1, rid(1, 0));
        check(!f.idx.remove(999), "G6 delete non-existent → false");
        check(f.idx.search(1).page_id == 1, "G6 existing key unaffected");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION H — BTreeIndex edge cases & boundary conditions
// ═════════════════════════════════════════════════════════════════════════════

static void section_H() {
    section("H: BTreeIndex edge cases");

    // H1: INT64 extremes
    {
        TreeFixture f("overall_H1.db");
        int64_t min_key = INT64_MIN;
        int64_t max_key = INT64_MAX;
        f.idx.insert(min_key, rid(1, 0));
        f.idx.insert(max_key, rid(2, 0));
        f.idx.insert(0,       rid(3, 0));
        check(f.idx.search(min_key).page_id == 1, "H1 INT64_MIN stored/retrieved");
        check(f.idx.search(max_key).page_id == 2, "H1 INT64_MAX stored/retrieved");
        check(f.idx.search(0).page_id       == 3, "H1 key 0 stored/retrieved");
    }

    // H2: negative keys at scale
    {
        TreeFixture f("overall_H2.db");
        for (int i = -200; i <= 200; ++i)
            f.idx.insert(i, rid(i + 500, 0));
        bool all_found = true;
        for (int i = -200; i <= 200; ++i)
            if (f.idx.search(i).page_id != (uint32_t)(i + 500)) all_found = false;
        check(all_found, "H2 negative and positive keys at scale");
        auto results = f.idx.range_scan(-10, 10);
        check(results.size() == 21, "H2 range scan across zero");
    }

    // H3: duplicate insert after split
    {
        TreeFixture f("overall_H3.db");
        const int N = static_cast<int>(LEAF_MAX_ENTRIES) + 10;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i, rid(i, 0));
        // Try to insert a duplicate of a key that is now in the RIGHT leaf
        int dup_key = N - 1;
        check(!f.idx.insert(dup_key, rid(999, 0)),
              "H3 duplicate after split rejected");
        check(f.idx.search(dup_key).page_id != 999,
              "H3 original RID preserved after duplicate rejection");
    }

    // H4: sequential insert then sequential delete then re-insert
    {
        TreeFixture f("overall_H4.db");
        for (int i = 0; i < 50; ++i) f.idx.insert(i, rid(i, 0));
        for (int i = 0; i < 50; ++i) f.idx.remove(i);
        for (int i = 0; i < 50; ++i) f.idx.insert(i, rid(i + 100, 0));
        bool all_new = true;
        for (int i = 0; i < 50; ++i)
            if (f.idx.search(i).page_id != (uint32_t)(i + 100)) all_new = false;
        check(all_new, "H4 re-insert after full delete has new RIDs");
    }

    // H5: interleaved insert and delete
    {
        TreeFixture f("overall_H5.db");
        for (int i = 0; i < 100; ++i) f.idx.insert(i, rid(i, 0));
        for (int i = 0; i < 100; i += 3) f.idx.remove(i);
        for (int i = 0; i < 100; ++i) {
            bool expected_valid = (i % 3 != 0);
            bool actual_valid   = f.idx.search(i).is_valid();
            if (expected_valid != actual_valid) {
                check(false, ("H5 key " + std::to_string(i) + " validity wrong").c_str());
                goto h5_done;
            }
        }
        check(true, "H5 interleaved insert/delete: all validities correct");
        h5_done:;
    }

    // H6: load() — index survives a reopen
    {
        const char* path = "overall_H6.db";
        page_id_t root_pid;
        {
            DiskManager dm(path);
            BufferManager bm(&dm, 64);
            BTreeIndex idx(&bm);
            root_pid = idx.create();
            for (int i = 0; i < 50; ++i) idx.insert(i, rid(i, 0));
            bm.flush_all_pages();
        }
        // Reopen
        {
            DiskManager dm(path);
            BufferManager bm(&dm, 64);
            BTreeIndex idx(&bm);
            idx.load(root_pid);
            bool all_found = true;
            for (int i = 0; i < 50; ++i)
                if (idx.search(i).page_id != (uint32_t)i) all_found = false;
            check(all_found, "H6 index survives flush + reopen");
        }
        std::remove(path);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION I — Stress tests
// ═════════════════════════════════════════════════════════════════════════════

static void section_I() {
    section("I: Stress tests");

    // I1: 10 000 sequential inserts, all searchable
    {
        TreeFixture f("overall_I1.db", 1024);
        const int N = 10000;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i, rid(i, 0));
        bool ok = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).page_id != (uint32_t)i) ok = false;
        check(ok, "I1 10 000 sequential keys all searchable");
    }

    // I2: 5000 random inserts, all searchable
    {
        TreeFixture f("overall_I2.db", 1024);
        const int N = 5000;
        std::vector<int> keys(N);
        for (int i = 0; i < N; ++i) keys[i] = i;
        std::mt19937 rng(1337);
        std::shuffle(keys.begin(), keys.end(), rng);
        for (int k : keys) f.idx.insert(k, rid(k, 0));
        bool ok = true;
        for (int i = 0; i < N; ++i)
            if (f.idx.search(i).page_id != (uint32_t)i) ok = false;
        check(ok, "I2 5000 random-order keys all searchable");
    }

    // I3: large range scan correctness
    {
        TreeFixture f("overall_I3.db", 1024);
        const int N = 3000;
        for (int i = 0; i < N; ++i)
            f.idx.insert(i, rid(i, 0));
        auto results = f.idx.range_scan(0, N - 1);
        bool ok = (results.size() == (size_t)N);
        for (int i = 0; i < N && ok; ++i)
            if (results[i].page_id != (uint32_t)i) ok = false;
        check(ok, "I3 large range scan (3000 keys) correct and ordered");
    }

    // I4: mixed insert/delete/search under load
    {
        TreeFixture f("overall_I4.db", 1024);
        std::map<int, bool> alive;
        const int N = 1000;
        for (int i = 0; i < N; ++i) {
            f.idx.insert(i, rid(i, 0));
            alive[i] = true;
        }
        std::mt19937 rng(2024);
        for (int round = 0; round < 500; ++round) {
            int k = rng() % N;
            if (alive[k]) {
                f.idx.remove(k);
                alive[k] = false;
            } else {
                f.idx.insert(k, rid(k, 0));
                alive[k] = true;
            }
        }
        bool ok = true;
        for (int i = 0; i < N; ++i) {
            bool found = f.idx.search(i).is_valid();
            if (found != alive[i]) ok = false;
        }
        check(ok, "I4 mixed insert/delete/search under load");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION J — Page (slotted heap page) unit tests
// ═════════════════════════════════════════════════════════════════════════════

static void section_J() {
    section("J: Page (slotted heap page) unit tests");

    // J1: fresh page header
    {
        Page p;
        p.init(3, PageType::DATA);
        check(p.get_page_id()    == 3,           "J1 page_id == 3");
        check(p.get_page_type()  == PageType::DATA, "J1 type == DATA");
        check(p.get_slot_count() == 0,           "J1 slot_count == 0");
        check(p.get_page_lsn()   == 0,           "J1 page_lsn == 0");
    }

    // J2: insert and read back
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t data[] = {1, 2, 3, 4, 5};
        slot_id_t sid = p.insert_record(data, 5);
        check(sid != INVALID_SLOT_ID, "J2 insert returns valid slot_id");
        std::vector<uint8_t> out;
        check(p.read_record(sid, out), "J2 read_record returns true");
        check(out.size() == 5 && std::memcmp(out.data(), data, 5) == 0,
              "J2 read_record returns correct data");
    }

    // J3: multiple inserts, correct slot IDs
    {
        Page p;
        p.init(0, PageType::DATA);
        for (int i = 0; i < 10; ++i) {
            uint8_t d = static_cast<uint8_t>(i);
            slot_id_t sid = p.insert_record(&d, 1);
            check(sid == static_cast<slot_id_t>(i),
                  ("J3 slot_id == " + std::to_string(i)).c_str());
        }
    }

    // J4: delete marks tombstone
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t d = 42;
        slot_id_t sid = p.insert_record(&d, 1);
        check(p.delete_record(sid),        "J4 delete_record returns true");
        std::vector<uint8_t> out;
        check(!p.read_record(sid, out),    "J4 read_record on tombstone → false");
        check(!p.delete_record(sid),       "J4 double-delete → false");
    }

    // J5: tombstone slot is reused by next insert
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t d = 1;
        slot_id_t sid0 = p.insert_record(&d, 1);
        p.delete_record(sid0);
        slot_id_t sid1 = p.insert_record(&d, 1);
        check(sid1 == sid0, "J5 tombstone slot reused");
        check(p.get_slot_count() == 1, "J5 slot_count stays 1");
    }

    // J6: update_record
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t orig[] = {10, 20, 30};
        slot_id_t sid = p.insert_record(orig, 3);
        uint8_t newval[] = {40, 50, 60};
        check(p.update_record(sid, newval, 3), "J6 update_record returns true");
        std::vector<uint8_t> out;
        p.read_record(sid, out);
        check(out[0] == 40 && out[1] == 50 && out[2] == 60,
              "J6 updated data correct");
    }

    // J7: update wrong size rejected
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t d[] = {1, 2, 3};
        slot_id_t sid = p.insert_record(d, 3);
        uint8_t big[] = {1, 2, 3, 4};
        check(!p.update_record(sid, big, 4), "J7 update with wrong size → false");
    }

    // J8: read_record out of range
    {
        Page p;
        p.init(0, PageType::DATA);
        std::vector<uint8_t> out;
        check(!p.read_record(0, out),  "J8 read slot 0 on empty page → false");
        check(!p.read_record(99, out), "J8 read slot 99 on empty page → false");
    }

    // J9: can_insert respects capacity
    {
        Page p;
        p.init(0, PageType::DATA);
        // Page has ~4075 bytes of usable space. Fill it up.
        std::vector<uint8_t> big(200, 0xAB);
        int count = 0;
        while (p.can_insert(200)) {
            p.insert_record(big.data(), 200);
            ++count;
        }
        check(count > 0, "J9 inserted multiple large records");
        check(!p.can_insert(200), "J9 page full: can_insert returns false");
    }

    // J10: compact removes holes, RIDs stable
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t a = 1, b = 2, c = 3;
        slot_id_t s0 = p.insert_record(&a, 1);
        slot_id_t s1 = p.insert_record(&b, 1);
        slot_id_t s2 = p.insert_record(&c, 1);
        p.delete_record(s1);   // create a hole
        p.compact();
        // s0 and s2 must still be readable with correct data
        std::vector<uint8_t> out;
        p.read_record(s0, out);
        check(out[0] == 1, "J10 slot 0 data intact after compact");
        p.read_record(s2, out);
        check(out[0] == 3, "J10 slot 2 data intact after compact");
        // s1 is still a tombstone (RID stability)
        check(!p.read_record(s1, out), "J10 deleted slot still tombstone after compact");
    }

    // J11: page_lsn is updated on insert
    {
        Page p;
        p.init(0, PageType::DATA);
        uint8_t d = 1;
        p.insert_record(&d, 1, /*lsn=*/100);
        check(p.get_page_lsn() == 100, "J11 page_lsn updated on insert");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION K — BufferManager + DiskManager integration
// ═════════════════════════════════════════════════════════════════════════════

static void section_K() {
    section("K: BufferManager + DiskManager integration");

    const char* path = "overall_K.db";

    // K1: new_page gives a valid pinned page
    {
        DiskManager dm(path);
        BufferManager bm(&dm, 16);
        page_id_t pid;
        Page* p = bm.new_page(pid);
        check(p != nullptr,          "K1 new_page returns non-null");
        check(pid != INVALID_PAGE_ID,"K1 new_page assigns valid pid");
        bm.unpin_page(pid, false);
    }

    // K2: fetch_page after new_page returns same frame (cache hit)
    {
        DiskManager dm(path);
        BufferManager bm(&dm, 16);
        page_id_t pid;
        bm.new_page(pid);
        bm.unpin_page(pid, true);
        Page* p = bm.fetch_page(pid);
        check(p != nullptr, "K2 fetch_page after new_page returns non-null");
        bm.unpin_page(pid, false);
    }

    // K3: data persists across eviction and re-fetch
    {
        DiskManager dm(path);
        // Pool of 2 — forces eviction when we allocate a third page.
        BufferManager bm(&dm, 2);

        page_id_t pid0, pid1, pid2;
        Page* p0 = bm.new_page(pid0);
        p0->init(pid0, PageType::DATA);
        uint8_t sentinel[] = {0xDE, 0xAD, 0xBE, 0xEF};
        p0->insert_record(sentinel, 4);
        bm.unpin_page(pid0, true);

        // Allocate two more pages to push pid0 out of the pool.
        Page* p1 = bm.new_page(pid1);
        bm.unpin_page(pid1, false);
        Page* p2 = bm.new_page(pid2);
        bm.unpin_page(pid2, false);

        // Re-fetch pid0 — it should have been evicted to disk and reloaded.
        Page* reloaded = bm.fetch_page(pid0);
        check(reloaded != nullptr, "K3 re-fetch after eviction non-null");
        std::vector<uint8_t> out;
        bool ok = reloaded->read_record(0, out);
        check(ok && out.size() == 4 &&
              out[0] == 0xDE && out[1] == 0xAD, "K3 data survives eviction");
        bm.unpin_page(pid0, false);
    }

    // K4: flush_all_pages persists data
    {
        DiskManager dm(path);
        BufferManager bm(&dm, 8);
        page_id_t pid;
        Page* p = bm.new_page(pid);
        p->init(pid, PageType::DATA);
        uint8_t d[] = {0xCA, 0xFE};
        p->insert_record(d, 2);
        bm.unpin_page(pid, true);
        bm.flush_all_pages();

        // Fresh BufferManager, same DiskManager (same file) — re-fetch.
        BufferManager bm2(&dm, 8);
        Page* p2 = bm2.fetch_page(pid);
        check(p2 != nullptr, "K4 fetch after flush non-null");
        std::vector<uint8_t> out;
        p2->read_record(0, out);
        check(out.size() == 2 && out[0] == 0xCA && out[1] == 0xFE,
              "K4 data persists after flush_all_pages");
        bm2.unpin_page(pid, false);
    }

    // K5: pinned page is not evicted
    {
        DiskManager dm(path);
        BufferManager bm(&dm, 2);
        page_id_t pid0, pid1, pid2;
        Page* p0 = bm.new_page(pid0);
        // Keep p0 pinned (don't unpin it).
        Page* p1 = bm.new_page(pid1);
        bm.unpin_page(pid1, false);
        // Third new_page must evict pid1 (pid0 is pinned), not pid0.
        Page* p2 = bm.new_page(pid2);
        check(p2 != nullptr, "K5 new_page evicts unpinned frame");
        bm.unpin_page(pid0, false);
        bm.unpin_page(pid2, false);
    }

    std::remove(path);
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION L — Cross-layer integration (heap page + B+ Tree together)
// ═════════════════════════════════════════════════════════════════════════════

static void section_L() {
    section("L: Cross-layer integration (heap + index)");

    const char* heap_path  = "overall_L_heap.db";
    const char* index_path = "overall_L_idx.db";

    // L1: Insert records into heap pages, index them, retrieve via index.
    {
        DiskManager heap_dm(heap_path);
        BufferManager heap_bm(&heap_dm, 64);

        // Allocate a heap page and insert 20 records.
        page_id_t heap_pid;
        Page* heap_page = heap_bm.new_page(heap_pid);
        heap_page->init(heap_pid, PageType::DATA);

        std::vector<RID> rids;
        for (int i = 0; i < 20; ++i) {
            uint8_t payload[8];
            std::memcpy(payload, &i, sizeof(i));
            slot_id_t slot = heap_page->insert_record(payload, 8);
            rids.push_back(rid(heap_pid, slot));
        }
        heap_bm.unpin_page(heap_pid, true);
        heap_bm.flush_all_pages();

        // Build a B+ Tree index: key = record value (i), value = RID.
        DiskManager idx_dm(index_path);
        BufferManager idx_bm(&idx_dm, 64);
        BTreeIndex idx(&idx_bm);
        idx.create();
        for (int i = 0; i < 20; ++i)
            idx.insert(i, rids[i]);

        // Look up key 10 in the index, then fetch the actual record from the heap.
        RID found_rid = idx.search(10);
        check(found_rid.is_valid(), "L1 index lookup returns valid RID");

        Page* heap_page2 = heap_bm.fetch_page(found_rid.page_id);
        check(heap_page2 != nullptr, "L1 heap page fetch succeeds");
        std::vector<uint8_t> record_data;
        heap_page2->read_record(found_rid.slot_id, record_data);
        int stored_val = 0;
        std::memcpy(&stored_val, record_data.data(), sizeof(stored_val));
        check(stored_val == 10, "L1 heap record value matches index key");
        heap_bm.unpin_page(found_rid.page_id, false);
    }

    // L2: range scan over index, fetch all heap records, verify values.
    {
        DiskManager heap_dm2(heap_path);
        BufferManager heap_bm2(&heap_dm2, 64);

        page_id_t heap_pid;
        Page* hp = heap_bm2.new_page(heap_pid);
        hp->init(heap_pid, PageType::DATA);
        for (int i = 0; i < 50; ++i) {
            uint8_t payload[4];
            std::memcpy(payload, &i, 4);
            hp->insert_record(payload, 4);
        }
        heap_bm2.unpin_page(heap_pid, true);

        DiskManager idx_dm2(index_path);
        BufferManager idx_bm2(&idx_dm2, 64);
        BTreeIndex idx2(&idx_bm2);
        idx2.create();
        for (int i = 0; i < 50; ++i)
            idx2.insert(i, rid(heap_pid, static_cast<slot_id_t>(i)));

        auto scan_results = idx2.range_scan(10, 20);
        check(scan_results.size() == 11, "L2 range scan returns 11 RIDs");

        bool all_correct = true;
        for (int j = 0; j < 11; ++j) {
            Page* rp = heap_bm2.fetch_page(scan_results[j].page_id);
            std::vector<uint8_t> out;
            rp->read_record(scan_results[j].slot_id, out);
            int val = 0;
            std::memcpy(&val, out.data(), 4);
            if (val != 10 + j) all_correct = false;
            heap_bm2.unpin_page(scan_results[j].page_id, false);
        }
        check(all_correct, "L2 heap records fetched via range scan are correct");
    }

    std::remove(heap_path);
    std::remove(index_path);
}

// ═════════════════════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "═══════════════════════════════════════════════\n";
    std::cout << "  PersistX — Overall Rigorous Test Suite\n";
    std::cout << "═══════════════════════════════════════════════\n";

    section_A();
    section_B();
    section_C();
    section_D();
    section_E();
    section_F();
    section_G();
    section_H();
    section_I();
    section_J();
    section_K();
    section_L();

    std::cout << "\n═══════════════════════════════════════════════\n";
    std::cout << "  Results: " << g_passed << " / " << g_total << " passed";
    if (g_failed > 0)
        std::cout << "  (" << g_failed << " FAILED)";
    std::cout << "\n═══════════════════════════════════════════════\n";

    return g_failed == 0 ? 0 : 1;
}