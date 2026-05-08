#include "disk_manager.hpp"
#include "page.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>

// Helper: fill a buffer with a repeating byte pattern based on page_id.
static void fill_pattern(uint8_t* buf, page_id_t page_id) {
    uint8_t byte = static_cast<uint8_t>(page_id * 37 + 7); // arbitrary pattern
    std::memset(buf, byte, PAGE_SIZE);
}

// Helper: verify a buffer matches the expected pattern.
static bool check_pattern(const uint8_t* buf, page_id_t page_id) {
    uint8_t byte = static_cast<uint8_t>(page_id * 37 + 7);
    for (std::size_t i = 0; i < PAGE_SIZE; ++i) {
        if (buf[i] != byte) return false;
    }
    return true;
}

// Helper: check buffer is all zeros.
static bool is_zeroed(const uint8_t* buf) {
    for (std::size_t i = 0; i < PAGE_SIZE; ++i) {
        if (buf[i] != 0) return false;
    }
    return true;
}

static const std::string TEST_DB = "storage/test_disk_manager.db";

// Clean up test file before/after tests.
static void cleanup() {
    std::filesystem::remove(TEST_DB);
}

int main() {
    cleanup();

    // ── Test 1: Basic write/read round-trip ──────────────────────────────────
    {
        DiskManager dm(TEST_DB);
        assert(dm.get_num_pages() == 0);

        page_id_t pid = dm.allocate_page();
        assert(pid == 0);
        assert(dm.get_num_pages() == 1);

        uint8_t write_buf[PAGE_SIZE];
        fill_pattern(write_buf, pid);
        assert(dm.write_page(pid, write_buf));

        uint8_t read_buf[PAGE_SIZE];
        assert(dm.read_page(pid, read_buf));
        assert(check_pattern(read_buf, pid));

        std::cout << "Test 1 PASSED: basic write/read round-trip\n";
    }

    // ── Test 2: Multiple pages written out of order ──────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);

        page_id_t p0 = dm.allocate_page(); // 0
        page_id_t p1 = dm.allocate_page(); // 1
        page_id_t p2 = dm.allocate_page(); // 2
        assert(dm.get_num_pages() == 3);

        // Write in reverse order: 2, 0, 1
        uint8_t buf[PAGE_SIZE];

        fill_pattern(buf, p2);
        assert(dm.write_page(p2, buf));

        fill_pattern(buf, p0);
        assert(dm.write_page(p0, buf));

        fill_pattern(buf, p1);
        assert(dm.write_page(p1, buf));

        // Read all back and verify
        uint8_t read_buf[PAGE_SIZE];

        assert(dm.read_page(p0, read_buf));
        assert(check_pattern(read_buf, p0));

        assert(dm.read_page(p1, read_buf));
        assert(check_pattern(read_buf, p1));

        assert(dm.read_page(p2, read_buf));
        assert(check_pattern(read_buf, p2));

        std::cout << "Test 2 PASSED: multiple pages out-of-order\n";
    }

    // ── Test 3: Read unwritten page returns zeros ────────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);

        page_id_t pid = dm.allocate_page();

        // Never wrote to pid — reading should give all zeros.
        uint8_t read_buf[PAGE_SIZE];
        assert(dm.read_page(pid, read_buf));
        assert(is_zeroed(read_buf));

        std::cout << "Test 3 PASSED: unwritten page returns zeros\n";
    }

    // ── Test 4: Data persists across DiskManager restarts ────────────────────
    {
        cleanup();

        // Phase A: write data and close
        {
            DiskManager dm(TEST_DB);
            page_id_t p0 = dm.allocate_page();
            page_id_t p1 = dm.allocate_page();

            uint8_t buf[PAGE_SIZE];

            fill_pattern(buf, p0);
            dm.write_page(p0, buf);

            fill_pattern(buf, p1);
            dm.write_page(p1, buf);

            dm.flush();
            // dm destroyed here — file closed
        }

        // Phase B: reopen and verify
        {
            DiskManager dm(TEST_DB);
            assert(dm.get_num_pages() == 2);

            uint8_t read_buf[PAGE_SIZE];

            assert(dm.read_page(0, read_buf));
            assert(check_pattern(read_buf, 0));

            assert(dm.read_page(1, read_buf));
            assert(check_pattern(read_buf, 1));
        }

        std::cout << "Test 4 PASSED: data persists across restart\n";
    }

    // ── Test 5: Page integrates with DiskManager ─────────────────────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);

        // Use a real Page object — write via DiskManager, read back.
        Page page;
        page.init(0, PageType::DATA);

        std::string record = "hello persistx";
        auto sid = page.insert_record(
            reinterpret_cast<const uint8_t*>(record.data()),
            static_cast<uint16_t>(record.size()));
        assert(sid != INVALID_SLOT_ID);

        page_id_t pid = dm.allocate_page();
        assert(dm.write_page(pid, page.raw()));

        // Read into a new Page and verify the record survived.
        uint8_t raw_buf[PAGE_SIZE];
        assert(dm.read_page(pid, raw_buf));
        Page loaded = Page::from_raw(raw_buf);

        std::vector<uint8_t> out;
        assert(loaded.read_record(sid, out));
        std::string result(out.begin(), out.end());
        assert(result == "hello persistx");

        std::cout << "Test 5 PASSED: Page round-trip through DiskManager\n";
    }

    // ── Test 6: Tombstones & slot reuse survive disk round-trip ─────────────
    {
        cleanup();
        DiskManager dm(TEST_DB);

        // Build a page with 3 records, delete the middle one.
        Page page;
        page.init(0, PageType::DATA);

        std::string r1 = "alpha";
        std::string r2 = "bravo";
        std::string r3 = "charlie";

        auto s1 = page.insert_record(reinterpret_cast<const uint8_t*>(r1.data()),
                                     static_cast<uint16_t>(r1.size()));
        auto s2 = page.insert_record(reinterpret_cast<const uint8_t*>(r2.data()),
                                     static_cast<uint16_t>(r2.size()));
        auto s3 = page.insert_record(reinterpret_cast<const uint8_t*>(r3.data()),
                                     static_cast<uint16_t>(r3.size()));

        // Delete "bravo" — creates a tombstone at slot s2.
        assert(page.delete_record(s2));
        assert(page.get_tombstone_count() == 1);

        // Write to disk and read back.
        page_id_t pid = dm.allocate_page();
        assert(dm.write_page(pid, page.raw()));

        uint8_t raw_buf[PAGE_SIZE];
        assert(dm.read_page(pid, raw_buf));
        Page loaded = Page::from_raw(raw_buf);

        // Verify tombstone survived.
        assert(loaded.get_tombstone_count() == 1);
        assert(loaded.get_slot_count() == 3);

        // s1 and s3 still readable.
        std::vector<uint8_t> out;
        assert(loaded.read_record(s1, out));
        assert(std::string(out.begin(), out.end()) == "alpha");

        assert(loaded.read_record(s3, out));
        assert(std::string(out.begin(), out.end()) == "charlie");

        // s2 is tombstoned — read must fail.
        assert(!loaded.read_record(s2, out));

        // Insert into the loaded page — should reuse slot s2.
        std::string r4 = "delta";
        auto s4 = loaded.insert_record(reinterpret_cast<const uint8_t*>(r4.data()),
                                       static_cast<uint16_t>(r4.size()));
        assert(s4 == s2); // tombstone slot reused
        assert(loaded.get_tombstone_count() == 0);

        // Write the updated page back, read again, verify.
        assert(dm.write_page(pid, loaded.raw()));
        assert(dm.read_page(pid, raw_buf));
        Page reloaded = Page::from_raw(raw_buf);

        assert(reloaded.read_record(s4, out));
        assert(std::string(out.begin(), out.end()) == "delta");

        std::cout << "Test 6 PASSED: tombstones & slot reuse survive disk round-trip\n";
    }

    cleanup();
    std::cout << "\nAll DiskManager tests passed.\n";
    return 0;
}
