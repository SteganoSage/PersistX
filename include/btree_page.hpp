#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — B+ Tree Node Page Layouts
// ═══════════════════════════════════════════════════════════════════════════════
//
// Two "lens" classes that interpret a raw Page* buffer as a B+ Tree node.
// They do NOT own the Page — the BufferManager does.
//
// Leaf header (15 bytes):
//   [0..3]   page_id       uint32_t
//   [4]      node_type     uint8_t   (0 = internal, 1 = leaf)
//   [5..6]   num_keys      uint16_t
//   [7..10]  parent_id     uint32_t
//   [11..14] next_leaf_id  uint32_t  (leaf only)
//
// Internal header (11 bytes):
//   [0..3]   page_id       uint32_t
//   [4]      node_type     uint8_t
//   [5..6]   num_keys      uint16_t
//   [7..10]  parent_id     uint32_t
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "common.hpp"
#include "page.hpp"
#include <cstdint>
#include <vector>

// ─── node type tag ───────────────────────────────────────────────────────────

static constexpr uint8_t BTREE_NODE_INTERNAL = 0;
static constexpr uint8_t BTREE_NODE_LEAF     = 1;

// ─── leaf page constants ─────────────────────────────────────────────────────

static constexpr size_t BTREE_LEAF_HEADER_SIZE = 15;   // 4+1+2+4+4
static constexpr size_t BTREE_KEY_SIZE         = 8;    // int64_t
static constexpr size_t BTREE_RID_SIZE         = 6;    // page_id(4) + slot_id(2)
static constexpr size_t LEAF_ENTRY_SIZE        = 14;   // key(8) + RID(6)
static constexpr size_t LEAF_MAX_ENTRIES       =
    (PAGE_SIZE - BTREE_LEAF_HEADER_SIZE) / LEAF_ENTRY_SIZE;  // = 291

// ─── internal page constants ─────────────────────────────────────────────────
//
// Layout: child[0] | key[0] | child[1] | key[1] | ... | child[N]
//         4 bytes    8 bytes  4 bytes    8 bytes        4 bytes
//
// N keys → N+1 children.  Space = 4 + 12*N ≤ (PAGE_SIZE - header)

static constexpr size_t BTREE_INTERNAL_HEADER_SIZE = 11;   // 4+1+2+4
static constexpr size_t INTERNAL_MAX_KEYS          =
    (PAGE_SIZE - BTREE_INTERNAL_HEADER_SIZE - 4) / 12;     // = 340


// ═════════════════════════════════════════════════════════════════════════════
// BTreeLeafPage — interprets a Page* as a sorted array of (key, RID) pairs
// ═════════════════════════════════════════════════════════════════════════════

class BTreeLeafPage {
public:
    explicit BTreeLeafPage(Page* page) : page_(page) {}

    // ── initialisation ───────────────────────────────────────────────────────
    void init(page_id_t page_id);

    // ── header accessors ─────────────────────────────────────────────────────
    page_id_t get_page_id()      const;
    uint8_t   get_node_type()    const;
    uint16_t  get_num_keys()     const;
    page_id_t get_parent_id()    const;
    page_id_t get_next_leaf_id() const;

    void set_num_keys    (uint16_t  n);
    void set_parent_id   (page_id_t pid);
    void set_next_leaf_id(page_id_t pid);

    // ── entry accessors (index = 0 .. num_keys-1) ────────────────────────────
    int64_t get_key_at(uint16_t index) const;
    RID     get_rid_at(uint16_t index) const;
    void    set_key_at(uint16_t index, int64_t key);
    void    set_rid_at(uint16_t index, const RID& rid);

    // ── operations ───────────────────────────────────────────────────────────

    // Insert (key, rid) in sorted order.  Returns false if leaf is full.
    bool insert(int64_t key, const RID& rid);

    // Binary search for key.  Returns the RID or an invalid RID if not found.
    RID lookup(int64_t key) const;

    // Remove key.  Returns false if key not found.
    bool remove(int64_t key);

    // Returns the index of the first key ≥ the given key (insertion point).
    uint16_t key_index(int64_t key) const;

private:
    Page* page_;

    // Byte offset of entry[index] within the raw buffer.
    size_t entry_offset(uint16_t index) const;
};


// ═════════════════════════════════════════════════════════════════════════════
// BTreeInternalPage — interprets a Page* as keys + child pointers
// ═════════════════════════════════════════════════════════════════════════════

class BTreeInternalPage {
public:
    explicit BTreeInternalPage(Page* page) : page_(page) {}

    // ── initialisation ───────────────────────────────────────────────────────
    void init(page_id_t page_id);

    // ── header accessors ─────────────────────────────────────────────────────
    page_id_t get_page_id()   const;
    uint8_t   get_node_type() const;
    uint16_t  get_num_keys()  const;
    page_id_t get_parent_id() const;

    void set_num_keys (uint16_t  n);
    void set_parent_id(page_id_t pid);

    // ── entry accessors ──────────────────────────────────────────────────────
    //   child[0]  key[0]  child[1]  key[1]  ...  child[N]
    //   index:  0        0        1        1          N

    int64_t   get_key_at  (uint16_t index) const;   // index: 0..num_keys-1
    page_id_t get_child_at(uint16_t index) const;   // index: 0..num_keys
    void      set_key_at  (uint16_t index, int64_t key);
    void      set_child_at(uint16_t index, page_id_t pid);

    // ── operations ───────────────────────────────────────────────────────────

    // Find which child pointer to follow for a given search key.
    page_id_t lookup_child(int64_t key) const;

    // After a child split: insert (key, new_child) to the right of old_child.
    // Returns false if the node is full (caller must split this node too).
    bool insert_after(page_id_t old_child, int64_t key, page_id_t new_child);

private:
    Page* page_;

    // Byte offset of key[index] in the raw buffer.
    size_t key_offset(uint16_t index) const;

    // Byte offset of child[index] in the raw buffer.
    size_t child_offset(uint16_t index) const;
};