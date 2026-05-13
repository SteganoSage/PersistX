#include "btree_index.hpp"
#include "buffer_manager.hpp"
#include "disk_manager.hpp"
#include <cassert>
#include <iostream>
#include <cstdio>

// ─── tiny helper ─────────────────────────────────────────────────────────────

static void pass(const char* name) {
    std::cout << "[PASS] " << name << "\n";
}

static RID make_rid(page_id_t p, slot_id_t s) {
    RID r; r.page_id = p; r.slot_id = s; return r;
}

// ─── fixture ─────────────────────────────────────────────────────────────────
// Each test gets a fresh disk file and a fresh BTreeIndex.
// Pool size 128 is plenty for 1000-key tests (tree stays shallow).

struct Fixture {
    const char*    path;
    DiskManager    dm;
    BufferManager  bm;
    BTreeIndex     idx;

    explicit Fixture(const char* p)
        : path(p), dm(p), bm(&dm, 512), idx(&bm) {
        idx.create();
    }
    ~Fixture() { std::remove(path); }
};

// ─── test 1: single insert + lookup ──────────────────────────────────────────

static void test_single_insert_lookup() {
    Fixture f("test_btree_1.db");
    RID r = make_rid(10, 5);
    assert(f.idx.insert(42, r));
    RID found = f.idx.search(42);
    assert(found.is_valid());
    assert(found.page_id == 10 && found.slot_id == 5);
    pass("single insert + lookup");
}

// ─── test 2: multiple inserts, all found ─────────────────────────────────────

static void test_multiple_inserts() {
    Fixture f("test_btree_2.db");
    for (int i = 0; i < 20; ++i)
        assert(f.idx.insert(i, make_rid(i, 0)));
    for (int i = 0; i < 20; ++i) {
        RID r = f.idx.search(i);
        assert(r.is_valid() && r.page_id == (page_id_t)i);
    }
    pass("multiple inserts (20 keys)");
}

// ─── test 3: missing key ─────────────────────────────────────────────────────

static void test_missing_key() {
    Fixture f("test_btree_3.db");
    f.idx.insert(1, make_rid(1, 0));
    f.idx.insert(2, make_rid(2, 0));
    RID r = f.idx.search(99);
    assert(!r.is_valid());
    pass("missing key returns invalid RID");
}

// ─── test 4: leaf split (292 keys → forces one split) ────────────────────────

static void test_leaf_split() {
    Fixture f("test_btree_4.db");
    // LEAF_MAX_ENTRIES = 291, so 292 inserts forces exactly one leaf split.
    const int N = 292;
    for (int i = 0; i < N; ++i)
        assert(f.idx.insert(i, make_rid(i, 0)));
    for (int i = 0; i < N; ++i) {
        RID r = f.idx.search(i);
        assert(r.is_valid() && r.page_id == (page_id_t)i);
    }
    pass("leaf split (292 keys)");
}

// ─── test 5: many splits (1000 keys) ─────────────────────────────────────────

static void test_many_splits() {
    Fixture f("test_btree_5.db");
    const int N = 1000;
    for (int i = 0; i < N; ++i)
        assert(f.idx.insert(i, make_rid(i, 1)));
    for (int i = 0; i < N; ++i) {
        RID r = f.idx.search(i);
        assert(r.is_valid() && r.page_id == (page_id_t)i);
    }
    pass("many splits (1000 keys)");
}

// ─── test 6: range scan ──────────────────────────────────────────────────────

static void test_range_scan() {
    Fixture f("test_btree_6.db");
    for (int i = 0; i < 100; ++i)
        f.idx.insert(i, make_rid(i, 0));
    auto results = f.idx.range_scan(25, 75);
    assert(results.size() == 51);
    for (size_t i = 0; i < results.size(); ++i)
        assert(results[i].page_id == (page_id_t)(25 + i));
    pass("range scan [25..75] → 51 results");
}

// ─── test 7: delete ──────────────────────────────────────────────────────────

static void test_delete() {
    Fixture f("test_btree_7.db");
    for (int i = 0; i < 10; ++i)
        f.idx.insert(i, make_rid(i, 0));

    // Delete the even keys.
    for (int i = 0; i < 10; i += 2)
        assert(f.idx.remove(i));

    // Even keys should be gone.
    for (int i = 0; i < 10; i += 2)
        assert(!f.idx.search(i).is_valid());

    // Odd keys should still be there.
    for (int i = 1; i < 10; i += 2)
        assert(f.idx.search(i).is_valid());

    pass("delete (remove 5 of 10 keys)");
}

// ─── test 8: duplicate rejection ─────────────────────────────────────────────

static void test_duplicate_rejection() {
    Fixture f("test_btree_8.db");
    assert( f.idx.insert(7, make_rid(1, 0)));
    assert(!f.idx.insert(7, make_rid(2, 0)));  // duplicate → false
    RID r = f.idx.search(7);
    assert(r.page_id == 1);  // original RID unchanged
    pass("duplicate rejection");
}

// ─── test 9: reverse-order inserts ───────────────────────────────────────────
// Exercises the case where keys always go to the leftmost leaf.

static void test_reverse_inserts() {
    Fixture f("test_btree_9.db");
    const int N = 300;
    for (int i = N - 1; i >= 0; --i)
        assert(f.idx.insert(i, make_rid(i, 0)));
    for (int i = 0; i < N; ++i) {
        RID r = f.idx.search(i);
        assert(r.is_valid() && r.page_id == (page_id_t)i);
    }
    pass("reverse-order inserts (300 keys)");
}

// ─── test 10: delete non-existent key ────────────────────────────────────────

static void test_delete_nonexistent() {
    Fixture f("test_btree_10.db");
    f.idx.insert(1, make_rid(1, 0));
    assert(!f.idx.remove(999));
    pass("delete non-existent key returns false");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    test_single_insert_lookup();
    test_multiple_inserts();
    test_missing_key();
    test_leaf_split();
    test_many_splits();
    test_range_scan();
    test_delete();
    test_duplicate_rejection();
    test_reverse_inserts();
    test_delete_nonexistent();

    std::cout << "\nAll B+ Tree tests passed.\n";
    return 0;
}