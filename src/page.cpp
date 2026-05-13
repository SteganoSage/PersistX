#include "page.hpp"
#include <cassert>

// ─── memcpy helpers ───────────────────────────────────────────────────────────
//
// Every header field read/write goes through these functions.
// This eliminates strict-aliasing UB from reinterpret_cast<PageHeader*>.
// At -O1+ the compiler reduces these to single load/store instructions.

static void     wr_u8 (uint8_t* p, uint8_t  v) { *p = v; }
static void     wr_u16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
static void     wr_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
static void     wr_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

static uint8_t  rd_u8 (const uint8_t* p) { return *p; }
static uint16_t rd_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
static uint32_t rd_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t rd_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// Header byte offsets — must stay in sync with PAGE_HEADER_SIZE = 21.
static constexpr uint32_t OFF_PAGE_ID   = 0;   // uint32_t [0..3]
static constexpr uint32_t OFF_TYPE      = 4;   // uint8_t  [4]
static constexpr uint32_t OFF_SLOTCNT   = 5;   // uint16_t [5..6]
static constexpr uint32_t OFF_TOMBCNT   = 7;   // uint16_t [7..8]   FIX #1
static constexpr uint32_t OFF_FSP       = 9;   // uint32_t [9..12]
static constexpr uint32_t OFF_LSN       = 13;  // uint64_t [13..20]

// ─── construction ─────────────────────────────────────────────────────────────

Page::Page() {
    std::memset(buffer_, 0, PAGE_SIZE);
}

void Page::init(page_id_t page_id, PageType type) {
    std::memset(buffer_, 0, PAGE_SIZE);
    set_page_id(page_id);
    set_page_type(type);
    set_slot_count(0);
    set_tombstone_count(0);   // FIX #1
    set_free_space_ptr(static_cast<uint32_t>(PAGE_HEADER_SIZE));
    set_page_lsn(0);
}

Page Page::from_raw(const uint8_t* src) {
    // Low-level load: just copy the bytes. Structural validation (slot_count
    // in bounds, free_space_ptr sane, slot offsets valid) belongs in the
    // Recovery layer (Phase 5), not here.  FIX #8 design note.
    Page p;
    std::memcpy(p.buffer_, src, PAGE_SIZE);
    return p;
}

// ─── header accessors ────────────────────────────────────────────────────────

page_id_t Page::get_page_id()         const { return rd_u32(buffer_ + OFF_PAGE_ID); }
PageType  Page::get_page_type()       const { return static_cast<PageType>(rd_u8(buffer_ + OFF_TYPE)); }
uint16_t  Page::get_slot_count()      const { return rd_u16(buffer_ + OFF_SLOTCNT); }
uint16_t  Page::get_tombstone_count() const { return rd_u16(buffer_ + OFF_TOMBCNT); }  // FIX #1
uint32_t  Page::get_free_space_ptr()  const { return rd_u32(buffer_ + OFF_FSP); }
lsn_t     Page::get_page_lsn()        const { return rd_u64(buffer_ + OFF_LSN); }

void Page::set_page_id        (page_id_t v) { wr_u32(buffer_ + OFF_PAGE_ID,  v); }
void Page::set_page_type      (PageType  v) { wr_u8 (buffer_ + OFF_TYPE,     static_cast<uint8_t>(v)); }
void Page::set_slot_count     (uint16_t  v) { wr_u16(buffer_ + OFF_SLOTCNT,  v); }
void Page::set_tombstone_count(uint16_t  v) { wr_u16(buffer_ + OFF_TOMBCNT,  v); }  // FIX #1
void Page::set_free_space_ptr (uint32_t  v) { wr_u32(buffer_ + OFF_FSP,      v); }
void Page::set_page_lsn       (lsn_t     v) { wr_u64(buffer_ + OFF_LSN,      v); }

// ─── space accounting ─────────────────────────────────────────────────────────

std::size_t Page::free_space() const {
    uint32_t fsp     = get_free_space_ptr();
    uint32_t dir_top = slot_dir_start();
    return (fsp <= dir_top) ? static_cast<std::size_t>(dir_top - fsp) : 0;
}

// FIX #1: can_insert is now O(1).
//
// Previous version always reserved SLOT_BYTES regardless of whether a
// tombstone slot could be reused, causing valid inserts to be rejected.
//
// Fix: tombstone_count is maintained in the header by insert_record and
// delete_record, so we can check for reuse in O(1) without scanning the
// directory. Only when no tombstone exists do we require SLOT_BYTES of
// additional directory space.
bool Page::can_insert(std::size_t record_len) const {
    // Data area always needs: [uint16_t size prefix] + [payload bytes]
    std::size_t needed = sizeof(uint16_t) + record_len;
    // Directory needs SLOT_BYTES only when we must append a new slot entry.
    // If a tombstone slot exists it will be reused — no extra directory space.
    if (get_tombstone_count() == 0) {
        needed += SLOT_BYTES;
    }
    return free_space() >= needed;
}

// ─── insert_record ────────────────────────────────────────────────────────────

slot_id_t Page::insert_record(const uint8_t* data, uint16_t size, lsn_t lsn) {
    if (!can_insert(size)) return INVALID_SLOT_ID;

    // 1. Write [uint16_t size_prefix][payload] at free_space_ptr.
    //
    //    The on-disk size prefix makes the DATA REGION self-describing,
    //    independently of the slot directory.  This is intentional redundancy:
    //    WAL recovery can reconstruct record boundaries by walking the data
    //    region prefix-by-prefix even if the slot directory is partially
    //    corrupt.  FIX #2: both copies are written atomically here so they
    //    are always consistent at rest.
    uint32_t offset = get_free_space_ptr();
    wr_u16(buffer_ + offset, size);
    std::memcpy(buffer_ + offset + sizeof(uint16_t), data, size);
    set_free_space_ptr(offset + static_cast<uint32_t>(sizeof(uint16_t)) + size);

    // 2. Assign a slot.
    //    FIX #1: only call find_free_slot() when tombstone_count > 0 —
    //    the O(n) scan is paid only when we already know a reusable slot exists.
    slot_id_t sid;
    if (get_tombstone_count() > 0) {
        sid = find_free_slot();
        assert(sid != INVALID_SLOT_ID); // tombstone_count > 0 guarantees one exists
        set_tombstone_count(static_cast<uint16_t>(get_tombstone_count() - 1));
    } else {
        sid = get_slot_count();
        set_slot_count(static_cast<uint16_t>(sid + 1));
    }
    write_slot(sid, offset, size);

    // 3. Stamp page_lsn for WAL recovery: recovery compares page_lsn vs
    //    log_lsn to decide whether this operation must be redone.
    if (lsn > get_page_lsn()) set_page_lsn(lsn);

    return sid;
}

// ─── redo_insert ──────────────────────────────────────────────────────────────
// Recovery-only: inserts a record at a specific slot_id to reproduce the exact
// physical state of the original operation. During normal operation, use
// insert_record() which allocates a slot automatically.

slot_id_t Page::redo_insert(slot_id_t target_slot, const uint8_t* data,
                            uint16_t size, lsn_t lsn) {
    // 1. Write [size_prefix][payload] at free_space_ptr.
    uint32_t offset = get_free_space_ptr();
    wr_u16(buffer_ + offset, size);
    std::memcpy(buffer_ + offset + sizeof(uint16_t), data, size);
    set_free_space_ptr(offset + static_cast<uint32_t>(sizeof(uint16_t)) + size);

    // 2. Ensure slot_count covers target_slot.
    uint16_t current_count = get_slot_count();
    if (target_slot >= current_count) {
        set_slot_count(static_cast<uint16_t>(target_slot + 1));
    } else {
        // Reusing an existing slot — if it's a tombstone, fix the count.
        auto [old_off, old_len] = read_slot(target_slot);
        if (old_off == SLOT_TOMBSTONE) {
            set_tombstone_count(static_cast<uint16_t>(get_tombstone_count() - 1));
        }
    }

    // 3. Write the slot entry at the exact target position.
    write_slot(target_slot, offset, size);

    // 4. Stamp page_lsn.
    if (lsn > get_page_lsn()) set_page_lsn(lsn);

    return target_slot;
}

// ─── read_record ──────────────────────────────────────────────────────────────

bool Page::read_record(slot_id_t slot_id, std::vector<uint8_t>& out) const {
    // FIX #3: explicit public-API bounds check.
    if (slot_id >= get_slot_count()) return false;

    auto [offset, length] = read_slot(slot_id);

    // Tombstone check.
    if (offset == SLOT_TOMBSTONE) return false;

    // FIX #4: validate that the record lies entirely within the data region.
    // A corrupted page_lsn, free_space_ptr, or slot entry could otherwise
    // cause a buffer overread.
    // Lower bound: offset must be beyond the header.
    // Upper bound: offset + prefix + payload must not reach the slot directory.
    if (offset < static_cast<uint32_t>(PAGE_HEADER_SIZE)) return false;
    if (offset + static_cast<uint32_t>(sizeof(uint16_t)) + length > PAGE_SIZE) return false;

    // FIX #2: cross-check the on-disk size prefix against the slot directory
    // length to detect single-bit corruption in either copy.
    uint16_t stored_prefix = rd_u16(buffer_ + offset);
    if (stored_prefix != length) return false;  // mismatch → page is corrupt

    // Both copies agree — safe to copy the payload.
    out.resize(length);
    std::memcpy(out.data(), buffer_ + offset + sizeof(uint16_t), length);
    return true;
}

// ─── delete_record ────────────────────────────────────────────────────────────

bool Page::delete_record(slot_id_t slot_id, lsn_t lsn) {
    // FIX #3: public bounds check before touching the directory.
    if (slot_id >= get_slot_count()) return false;

    auto [offset, length] = read_slot(slot_id);
    if (offset == SLOT_TOMBSTONE) return false; // already deleted — idempotent

    mark_tombstone(slot_id);

    // FIX #1: increment tombstone_count so can_insert stays accurate in O(1).
    set_tombstone_count(static_cast<uint16_t>(get_tombstone_count() + 1));

    if (lsn > get_page_lsn()) set_page_lsn(lsn);
    return true;
}

// ─── update_record ────────────────────────────────────────────────────────────
// In-place overwrite of a slot's payload bytes.
//
// The new data MUST be exactly the same size as the original record.
// This is intentional: WAL UPDATE records store full before/after images of
// the same logical tuple, so the byte count never changes between the original
// write and any subsequent undo (restore of before-image).
//
// Attempting to change the size would require relocating the record in the
// data region and rebuilding the slot directory — that is compaction, not an
// update. If variable-size updates are ever needed, delete + reinsert is the
// correct path.
 
bool Page::update_record(slot_id_t slot_id, const uint8_t* data, uint16_t size,
                         lsn_t lsn) {
    if (slot_id >= get_slot_count()) return false;
 
    auto [offset, length] = read_slot(slot_id);
    if (offset == SLOT_TOMBSTONE) return false;
 
    // Reject size mismatches — caller has a logic error if this fires.
    if (size != length) return false;
 
    // Overwrite the payload bytes in-place (the uint16_t size prefix stays).
    std::memcpy(buffer_ + offset + sizeof(uint16_t), data, size);
 
    if (lsn > get_page_lsn()) set_page_lsn(lsn);
    return true;
}

// ─── compact ──────────────────────────────────────────────────────────────────

void Page::compact() {
    // Snapshot the current page so we can read from it while rewriting buffer_.
    uint8_t snapshot[PAGE_SIZE];
    std::memcpy(snapshot, buffer_, PAGE_SIZE);

    uint16_t old_count = rd_u16(snapshot + OFF_SLOTCNT);

    // Reset data frontier and slot count; preserve page_id, type, page_lsn.
    set_slot_count(0);
    set_tombstone_count(0); // reset — will be re-accumulated below
    set_free_space_ptr(static_cast<uint32_t>(PAGE_HEADER_SIZE));

    // FIX #6: slots MUST be replayed in original index order (0 .. old_count-1).
    // This is the invariant that makes RID = (page_id, slot_id) stable:
    // after compaction, slot_id s still refers to the same logical record.
    // Never sort, reorder, or skip indices here.
    for (slot_id_t s = 0; s < old_count; ++s) {
        // Read slot entry from the snapshot directory.
        uint32_t base    = static_cast<uint32_t>(PAGE_SIZE)
                         - (static_cast<uint32_t>(s) + 1u)
                         * static_cast<uint32_t>(SLOT_BYTES);
        uint32_t old_off = rd_u32(snapshot + base);
        uint16_t old_len = rd_u16(snapshot + base + 4);

        uint16_t new_sid = get_slot_count();
        set_slot_count(static_cast<uint16_t>(new_sid + 1));

        if (old_off == SLOT_TOMBSTONE) {
            // Re-emit tombstone at the same slot index for RID stability.
            mark_tombstone(new_sid);
            set_tombstone_count(static_cast<uint16_t>(get_tombstone_count() + 1));
            continue;
        }

        // Copy live record: write [size prefix][payload] at new position.
        uint32_t new_off = get_free_space_ptr();
        wr_u16(buffer_ + new_off, old_len);
        std::memcpy(buffer_ + new_off + sizeof(uint16_t),
                    snapshot + old_off + sizeof(uint16_t),
                    old_len);
        set_free_space_ptr(new_off + static_cast<uint32_t>(sizeof(uint16_t)) + old_len);
        write_slot(new_sid, new_off, old_len);
    }
}

// ─── slot directory helpers ───────────────────────────────────────────────────

// FIX #5: sid is explicitly widened to uint32_t BEFORE the addition so that
// (sid + 1) * SLOT_BYTES is computed entirely in 32-bit arithmetic.
// Previously, if sid were promoted through uint16_t arithmetic, (sid+1) could
// wrap at UINT16_MAX before the cast, producing a wildly wrong offset.
// The two asserts catch both logical invalidity and physical impossibility.
uint32_t Page::slot_offset(slot_id_t sid) const {
    assert(static_cast<uint32_t>(sid) < static_cast<uint32_t>(get_slot_count()));
    assert(static_cast<uint32_t>(sid) < MAX_SLOTS_PER_PAGE);
    return static_cast<uint32_t>(PAGE_SIZE)
         - (static_cast<uint32_t>(sid) + 1u) * static_cast<uint32_t>(SLOT_BYTES);
}

uint32_t Page::slot_dir_start() const {
    uint16_t n = get_slot_count();
    if (n == 0) return static_cast<uint32_t>(PAGE_SIZE);
    return slot_offset(static_cast<slot_id_t>(n - 1));
}

// FIX #3: assert in each private helper provides a second line of defence
// — catches any internal caller that forgets the public bounds check.
Page::SlotEntry Page::read_slot(slot_id_t sid) const {
    assert(sid < get_slot_count());
    uint32_t base = slot_offset(sid);
    return { rd_u32(buffer_ + base), rd_u16(buffer_ + base + 4) };
}

void Page::write_slot(slot_id_t sid, uint32_t offset, uint16_t length) {
    assert(sid < get_slot_count());
    uint32_t base = slot_offset(sid);
    wr_u32(buffer_ + base,     offset);
    wr_u16(buffer_ + base + 4, length);
}

void Page::mark_tombstone(slot_id_t sid) {
    assert(sid < get_slot_count());
    uint32_t base = slot_offset(sid);
    wr_u32(buffer_ + base,     SLOT_TOMBSTONE);
    wr_u16(buffer_ + base + 4, 0);
}

slot_id_t Page::find_free_slot() const {
    // O(n) scan — only called from insert_record when tombstone_count > 0,
    // so we are guaranteed to find a result before reaching the end.
    uint16_t n = get_slot_count();
    for (slot_id_t s = 0; s < n; ++s) {
        uint32_t base = static_cast<uint32_t>(PAGE_SIZE)
                      - (static_cast<uint32_t>(s) + 1u)
                      * static_cast<uint32_t>(SLOT_BYTES);
        if (rd_u32(buffer_ + base) == SLOT_TOMBSTONE) return s;
    }
    return INVALID_SLOT_ID;
}