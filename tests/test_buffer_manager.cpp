#include "persistx/buffer_manager.hpp"
#include "persistx/page.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>

static const std::string TEST_DB = "storage/test_buffer_manager.db";

static void cleanup() {
    std::filesystem::remove(TEST_DB);
}

int main() {
    cleanup();

    // ── Test 1: Fetch a new page ─────────────────────────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        BufferManager bm(&dm, 4);

        page_id_t pid;
        Page* page = bm.new_page(pid);
        assert(page != nullptr);
        assert(pid == 0);

        // Insert a record into the page.
        std::string rec = "hello buffer";
        auto sid = page->insert_record(
            reinterpret_cast<const uint8_t*>(rec.data()),
            static_cast<uint16_t>(rec.size()));
        assert(sid != INVALID_SLOT_ID);

        // Unpin as dirty.
        assert(bm.unpin_page(pid, true));

        std::cout << "Test 1 PASSED: new_page + insert + unpin\n";
    }

    // ── Test 2: Fetch page back from cache ───────────────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        BufferManager bm(&dm, 4);

        page_id_t pid;
        Page* page = bm.new_page(pid);
        std::string rec = "cached data";
        page->insert_record(
            reinterpret_cast<const uint8_t*>(rec.data()),
            static_cast<uint16_t>(rec.size()));
        bm.unpin_page(pid, true);

        // Fetch the same page again — should be a cache hit.
        Page* fetched = bm.fetch_page(pid);
        assert(fetched != nullptr);

        std::vector<uint8_t> out;
        assert(fetched->read_record(0, out));
        assert(std::string(out.begin(), out.end()) == "cached data");

        bm.unpin_page(pid, false);
        std::cout << "Test 2 PASSED: cache hit\n";
    }

    // ── Test 3: Eviction under pressure ──────────────────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        BufferManager bm(&dm, 2); // only 2 frames!

        // Fill both frames.
        page_id_t p0, p1;
        Page* pg0 = bm.new_page(p0);
        assert(pg0 != nullptr);
        std::string r0 = "page zero";
        pg0->insert_record(reinterpret_cast<const uint8_t*>(r0.data()),
                           static_cast<uint16_t>(r0.size()));
        bm.unpin_page(p0, true);

        Page* pg1 = bm.new_page(p1);
        assert(pg1 != nullptr);
        std::string r1 = "page one";
        pg1->insert_record(reinterpret_cast<const uint8_t*>(r1.data()),
                           static_cast<uint16_t>(r1.size()));
        bm.unpin_page(p1, true);

        // Both frames full, both unpinned. Fetching a new page should evict.
        page_id_t p2;
        Page* pg2 = bm.new_page(p2);
        assert(pg2 != nullptr);
        bm.unpin_page(p2, false);

        // The evicted page (p0, LRU) should still be readable from disk.
        Page* evicted = bm.fetch_page(p0);
        assert(evicted != nullptr);
        std::vector<uint8_t> out;
        assert(evicted->read_record(0, out));
        assert(std::string(out.begin(), out.end()) == "page zero");
        bm.unpin_page(p0, false);

        std::cout << "Test 3 PASSED: eviction + reload from disk\n";
    }

    // ── Test 4: Pinned page cannot be evicted ────────────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        BufferManager bm(&dm, 1); // only 1 frame!

        page_id_t p0;
        Page* pg0 = bm.new_page(p0);
        assert(pg0 != nullptr);
        // Don't unpin — page stays pinned.

        // Try to get another page — should fail (can't evict pinned page).
        page_id_t p1;
        Page* pg1 = bm.new_page(p1);
        assert(pg1 == nullptr);

        // Now unpin p0.
        bm.unpin_page(p0, false);

        // Retry — should succeed now.
        pg1 = bm.new_page(p1);
        assert(pg1 != nullptr);
        bm.unpin_page(p1, false);

        std::cout << "Test 4 PASSED: pinned page blocks eviction\n";
    }

    // ── Test 5: Dirty page flushed before eviction ───────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        BufferManager bm(&dm, 1);

        page_id_t p0;
        Page* pg0 = bm.new_page(p0);
        std::string rec = "must survive";
        pg0->insert_record(reinterpret_cast<const uint8_t*>(rec.data()),
                           static_cast<uint16_t>(rec.size()));
        bm.unpin_page(p0, true); // dirty

        // Allocate another page — evicts p0, should flush it first.
        page_id_t p1;
        bm.new_page(p1);
        bm.unpin_page(p1, false);

        // Fetch p0 back — it should have been flushed to disk before eviction.
        Page* reloaded = bm.fetch_page(p0);
        assert(reloaded != nullptr);
        std::vector<uint8_t> out;
        assert(reloaded->read_record(0, out));
        assert(std::string(out.begin(), out.end()) == "must survive");
        bm.unpin_page(p0, false);

        std::cout << "Test 5 PASSED: dirty page flushed before eviction\n";
    }

    // ── Test 6: delete_page returns frame to free list ───────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        BufferManager bm(&dm, 2);

        page_id_t p0, p1;
        bm.new_page(p0);
        bm.unpin_page(p0, false);
        bm.new_page(p1);
        bm.unpin_page(p1, false);

        // Delete p0 — its frame should go back to free list.
        assert(bm.delete_page(p0));

        // Now we can create a new page without eviction.
        page_id_t p2;
        Page* pg2 = bm.new_page(p2);
        assert(pg2 != nullptr);
        bm.unpin_page(p2, false);

        std::cout << "Test 6 PASSED: delete_page frees frame\n";
    }

    // ── Test 7: flush_all_pages persists everything ──────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);

        {
            BufferManager bm(&dm, 4);
            for (int i = 0; i < 3; ++i) {
                page_id_t pid;
                Page* p = bm.new_page(pid);
                std::string rec = "record_" + std::to_string(i);
                p->insert_record(reinterpret_cast<const uint8_t*>(rec.data()),
                                 static_cast<uint16_t>(rec.size()));
                bm.unpin_page(pid, true);
            }
            bm.flush_all_pages();
            // bm destroyed here
        }

        // Reopen and verify all pages survived.
        {
            BufferManager bm2(&dm, 4);
            for (int i = 0; i < 3; ++i) {
                Page* p = bm2.fetch_page(static_cast<page_id_t>(i));
                assert(p != nullptr);
                std::vector<uint8_t> out;
                assert(p->read_record(0, out));
                std::string expected = "record_" + std::to_string(i);
                assert(std::string(out.begin(), out.end()) == expected);
                bm2.unpin_page(static_cast<page_id_t>(i), false);
            }
        }

        std::cout << "Test 7 PASSED: flush_all_pages persists data\n";
    }

    cleanup();
    std::cout << "\nAll BufferManager tests passed.\n";
    return 0;
}
