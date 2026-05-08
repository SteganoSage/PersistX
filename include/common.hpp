#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — Common Types & Constants
// ═══════════════════════════════════════════════════════════════════════════════
//
// Shared definitions used across every layer of the storage engine.
// This header has ZERO dependencies on any PersistX component — it is the
// root of the include DAG.
//
// RULE: if a type or constant is needed by more than one component, it lives
//       here.  Page-specific layout details (slot format, header offsets)
//       stay in page.hpp.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>

// ─── page geometry ───────────────────────────────────────────────────────────

/// Every I/O operation works on fixed-size pages.
inline constexpr std::size_t PAGE_SIZE = 4096;

// ─── fundamental id types ────────────────────────────────────────────────────

using page_id_t  = uint32_t;
using slot_id_t  = uint16_t;
using frame_id_t = uint32_t;
using lsn_t      = uint64_t;

// ─── sentinel values ─────────────────────────────────────────────────────────

inline constexpr page_id_t  INVALID_PAGE_ID  = UINT32_MAX;
inline constexpr slot_id_t  INVALID_SLOT_ID  = UINT16_MAX;
inline constexpr frame_id_t INVALID_FRAME_ID = UINT32_MAX;
inline constexpr lsn_t      INVALID_LSN      = UINT64_MAX;

// ─── page type tag ───────────────────────────────────────────────────────────

enum class PageType : uint8_t {
    META  = 0,
    DATA  = 1,
    INDEX = 2,
    WAL   = 3,
};

// ─── record identifier ──────────────────────────────────────────────────────
//
// RID = (page_id, slot_id) — the stable handle to a record.
// Must remain valid across compaction and page reorganisation.

struct RID {
    page_id_t page_id{INVALID_PAGE_ID};
    slot_id_t slot_id{INVALID_SLOT_ID};

    bool operator==(const RID& o) const {
        return page_id == o.page_id && slot_id == o.slot_id;
    }
    bool operator!=(const RID& o) const { return !(*this == o); }

    /// True if the RID points to a valid location.
    bool is_valid() const {
        return page_id != INVALID_PAGE_ID && slot_id != INVALID_SLOT_ID;
    }
};
