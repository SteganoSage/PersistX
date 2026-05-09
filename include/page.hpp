#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — Slotted Page Layout
// ═══════════════════════════════════════════════════════════════════════════════

#include "common.hpp"
#include <cstring>
#include <vector>

// ─── page-specific constants ─────────────────────────────────────────────────

// Slot entry: uint32_t offset + uint16_t length = 6 bytes.
// offset == SLOT_TOMBSTONE marks a deleted slot.
inline constexpr uint32_t    SLOT_TOMBSTONE      = UINT32_MAX;
inline constexpr std::size_t SLOT_BYTES          = 6;

// Physical cap: how many slots can ever fit in one page.
// FIX #5: used to guard slot_offset() against impossible sid values.
inline constexpr uint32_t MAX_SLOTS_PER_PAGE =
    (PAGE_SIZE - 21u /*PAGE_HEADER_SIZE*/) / static_cast<uint32_t>(SLOT_BYTES);

// ─── PageHeader wire layout (21 bytes, manually serialised) ──────────────────
//
//   [0..3]   page_id          uint32_t
//   [4]      type             uint8_t
//   [5..6]   slot_count       uint16_t  — total allocated slots (incl. tombstones)
//   [7..8]   tombstone_count  uint16_t  — FIX #1: maintained for O(1) can_insert
//   [9..12]  free_space_ptr   uint32_t
//   [13..20] page_lsn         uint64_t
//
// All fields are accessed ONLY through memcpy helpers in page.cpp.
// Never use reinterpret_cast<PageHeader*>(buffer_) — that is strict-aliasing UB.
//
inline constexpr std::size_t PAGE_HEADER_SIZE = 4 + 1 + 2 + 2 + 4 + 8; // = 21

// ─── Page ────────────────────────────────────────────────────────────────────

class Page {
public:
    // ── construction ─────────────────────────────────────────────────────────

    Page();

    // Initialise a brand-new page: zero the buffer, write the header.
    void init(page_id_t page_id, PageType type);

    // Wrap a raw buffer read from disk (memcpy into buffer_).
    // Structural validation is the responsibility of the Recovery layer (Phase 5),
    // not this low-level constructor.
    static Page from_raw(const uint8_t* src);

    // ── header accessors (memcpy-safe, no aliasing UB) ───────────────────────

    page_id_t get_page_id()         const;
    PageType  get_page_type()       const;
    uint16_t  get_slot_count()      const;
    uint16_t  get_tombstone_count() const;  // FIX #1
    uint32_t  get_free_space_ptr()  const;
    lsn_t     get_page_lsn()        const;

    void set_page_id        (page_id_t v);
    void set_page_type      (PageType  v);
    void set_slot_count     (uint16_t  v);
    void set_tombstone_count(uint16_t  v);  // FIX #1
    void set_free_space_ptr (uint32_t  v);
    void set_page_lsn       (lsn_t     v);

    // ── space accounting ─────────────────────────────────────────────────────

    // Bytes between the data frontier and the slot directory.
    std::size_t free_space() const;

    // FIX #1: correctly accounts for slot reuse in O(1) using tombstone_count.
    // When a tombstone slot exists, no SLOT_BYTES are needed in the directory.
    bool can_insert(std::size_t record_len) const;

    // ── core record operations ────────────────────────────────────────────────

    // Insert record. Returns assigned slot_id or INVALID_SLOT_ID if full.
    // lsn: WAL LSN that authorises this write — stamped onto page_lsn.
    [[nodiscard]] slot_id_t insert_record(const uint8_t* data,
                                          uint16_t       size,
                                          lsn_t          lsn = 0);

    // Read record into `out`. Size comes from the page — caller supplies none.
    // FIX #4: validates offset + length stays within PAGE_SIZE bounds.
    // FIX #2: cross-checks on-disk size prefix against slot directory length.
    // Returns false if slot is out of range, tombstone, or page is corrupt.
    bool read_record(slot_id_t slot_id, std::vector<uint8_t>& out) const;

    // Delete: marks slot tombstone, increments tombstone_count. No compact.
    bool delete_record(slot_id_t slot_id, lsn_t lsn = 0);
    bool update_record(slot_id_t slot_id, const uint8_t* data, uint16_t size, lsn_t lsn = 0);

    // ── compaction ───────────────────────────────────────────────────────────

    // Rewrite live records contiguously, rebuild slot directory.
    // INVARIANT (FIX #6): slots are ALWAYS re-emitted in original index order
    // (s = 0..old_count-1) so RID = (page_id, slot_id) stays stable.
    // Tombstone slots are preserved at the same index for the same reason.
    // tombstone_count is reset to 0 after compaction.
    void compact();

    // ── raw buffer access (DiskManager / BufferManager only) ─────────────────

    // FIX #7: both const and mutable overloads — const required for writePage.
    const uint8_t* raw() const { return buffer_; }
    uint8_t*       raw()       { return buffer_; }

private:
    // ── slot directory helpers ────────────────────────────────────────────────

    // FIX #5: sid is widened to uint32_t before arithmetic so (sid+1)*SLOT_BYTES
    // can never overflow a uint16_t mid-expression.
    // Both asserts guard logical validity (sid < slot_count) and physical
    // capacity (sid < MAX_SLOTS_PER_PAGE).
    uint32_t  slot_offset   (slot_id_t sid) const;
    uint32_t  slot_dir_start()              const;

    struct SlotEntry { uint32_t offset; uint16_t length; };

    // FIX #3: assert(sid < get_slot_count()) in every private helper
    // catches callers that skip the public bounds check.
    SlotEntry read_slot    (slot_id_t sid) const;
    void      write_slot   (slot_id_t sid, uint32_t offset, uint16_t length);
    void      mark_tombstone(slot_id_t sid);

    // Scans directory for the lowest-index tombstone. O(n) — only called
    // from insert_record when tombstone_count > 0 is already confirmed.
    slot_id_t find_free_slot() const;

    // ── storage ──────────────────────────────────────────────────────────────

    // uint8_t (not char): defined signedness, safe for bitwise ops,
    // safe to pass directly to read()/write() syscalls.
    // alignas(4096): enables O_DIRECT I/O without an extra alignment copy.
    alignas(4096) uint8_t buffer_[PAGE_SIZE];
};

static_assert(sizeof(Page) == PAGE_SIZE,
    "Page must be exactly PAGE_SIZE bytes for direct disk I/O");
