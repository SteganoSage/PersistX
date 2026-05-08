#include "page.hpp"
#include <iostream>
#include <string>
#include <cassert>

static const uint8_t* as_bytes(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

int main() {
    // ── basic insert / read ───────────────────────────────────────────────────
    Page p;
    p.init(0, PageType::DATA);

    std::string r1 = "hello";
    std::string r2 = "persistx";
    std::string r3 = "database";

    slot_id_t s1 = p.insert_record(as_bytes(r1), static_cast<uint16_t>(r1.size()));
    slot_id_t s2 = p.insert_record(as_bytes(r2), static_cast<uint16_t>(r2.size()));
    slot_id_t s3 = p.insert_record(as_bytes(r3), static_cast<uint16_t>(r3.size()));

    assert(s1 != INVALID_SLOT_ID && s2 != INVALID_SLOT_ID && s3 != INVALID_SLOT_ID);

    std::vector<uint8_t> out;
    assert(p.read_record(s2, out));
    std::cout << "read s2: " << std::string(out.begin(), out.end()) << "\n"; // persistx

    // ── FIX #1: slot reuse — tombstone_count-driven can_insert ───────────────
    std::cout << "tombstone_count before delete: " << p.get_tombstone_count() << "\n"; // 0

    p.delete_record(s1);
    std::cout << "tombstone_count after delete:  " << p.get_tombstone_count() << "\n"; // 1

    // Insert should reuse slot s1 — can_insert must not over-reject.
    std::string r4 = "reused";
    slot_id_t s4 = p.insert_record(as_bytes(r4), static_cast<uint16_t>(r4.size()));
    assert(s4 == s1); // tombstone slot was reused
    std::cout << "slot reused:  s4==" << s4 << " (same as deleted s1==" << s1 << ")\n";
    std::cout << "tombstone_count after reuse:   " << p.get_tombstone_count() << "\n"; // 0

    // ── FIX #2: size-prefix / slot-directory cross-check ─────────────────────
    // Verify a good record passes the cross-check.
    assert(p.read_record(s2, out));
    std::cout << "cross-check OK for s2\n";

    // ── FIX #3 / #4: out-of-range and overread guard ─────────────────────────
    std::vector<uint8_t> bad_out;
    assert(!p.read_record(INVALID_SLOT_ID, bad_out)); // out of range
    assert(!p.read_record(static_cast<slot_id_t>(p.get_slot_count()), bad_out)); // one past end

    // ── delete / read back ────────────────────────────────────────────────────
    p.delete_record(s3);
    assert(!p.read_record(s3, bad_out)); // tombstone
    std::cout << "s3 after delete: gone (OK)\n";

    // ── compact ───────────────────────────────────────────────────────────────
    std::size_t before = p.free_space();
    p.compact();
    std::size_t after = p.free_space();
    std::cout << "free_space before=" << before << "  after=" << after << "\n";
    std::cout << "tombstone_count after compact: " << p.get_tombstone_count() << "\n"; // 1 (s3 kept)

    // s2 and s4 must survive compaction at the same slot_id (RID stability).
    assert(p.read_record(s2, out));
    std::cout << "s2 after compact: " << std::string(out.begin(), out.end()) << "\n"; // persistx
    assert(p.read_record(s4, out));
    std::cout << "s4 after compact: " << std::string(out.begin(), out.end()) << "\n"; // reused

    // ── disk round-trip via from_raw ──────────────────────────────────────────
    Page p2 = Page::from_raw(p.raw());
    assert(p2.read_record(s2, out));
    std::cout << "round-trip s2: " << std::string(out.begin(), out.end()) << "\n"; // persistx

    // FIX #1: tombstone_count must survive the raw round-trip.
    std::cout << "round-trip tombstone_count: " << p2.get_tombstone_count() << "\n"; // 1

    std::cout << "\nAll checks passed.\n";
    return 0;
}