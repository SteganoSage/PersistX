#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — B+ Tree Index
// ═══════════════════════════════════════════════════════════════════════════════
//
// Public API for the B+ Tree index. Owns the root page and coordinates all
// tree operations: search, insert, delete, range scan.
//
// Internally, every tree node is exactly one Page (4096 bytes), managed by
// the BufferManager. BTreeLeafPage and BTreeInternalPage (from btree_page.hpp)
// are used as "lenses" to interpret those raw pages.
//
// Pin/unpin contract:
//   Every fetch_page() call in this file has a matching unpin_page().
//   The caller of insert/search/remove/range_scan holds NO pins on return.
// ═══════════════════════════════════════════════════════════════════════════════

#include "common.hpp"
#include "buffer_manager.hpp"
#include "btree_page.hpp"
#include <vector>
#include <string>
#include <queue>

class BTreeIndex {
public:
    explicit BTreeIndex(BufferManager* bm);

    // ── tree lifecycle ────────────────────────────────────────────────────────

    // Create a brand-new empty index (single empty leaf as root).
    // Returns the root page_id. Call once before any inserts.
    page_id_t create();

    // Load an existing index whose root lives at the given page_id.
    // Call this (instead of create()) when reopening a database.
    void load(page_id_t root_page_id);

    // ── point operations ─────────────────────────────────────────────────────

    // Return the RID for key, or an invalid RID if not found.
    RID search(int64_t key);

    // Insert key → rid. Returns false if the key already exists.
    bool insert(int64_t key, const RID& rid);

    // Remove key. Returns false if the key is not found.
    // No node merging — same policy as PostgreSQL.
    bool remove(int64_t key);

    // ── range scan ───────────────────────────────────────────────────────────

    // Return all RIDs where begin_key ≤ key ≤ end_key, in key order.
    // Uses the leaf-level linked list for efficiency.
    std::vector<RID> range_scan(int64_t begin_key, int64_t end_key);

    // ── accessors ─────────────────────────────────────────────────────────────

    page_id_t get_root_page_id() const { return root_page_id_; }

    // ── introspection (for CLI visualization) ─────────────────────────────────

    struct TreeStats {
        uint32_t height;
        uint32_t internal_pages;
        uint32_t leaf_pages;
        uint64_t total_keys;
    };

    struct NodeInfo {
        page_id_t page_id;
        bool is_leaf;
        std::vector<int64_t> keys;
        std::vector<page_id_t> children;  // empty for leaves
    };

    // Compute tree statistics by traversing the entire tree.
    TreeStats get_stats();

    // Level-order traversal for visualization.
    // Returns a vector of levels; each level is a vector of NodeInfo.
    std::vector<std::vector<NodeInfo>> get_tree_layout();

private:
    BufferManager* bm_;
    page_id_t      root_page_id_{INVALID_PAGE_ID};

    // ── internal helpers ─────────────────────────────────────────────────────

    // Traverse from root to the leaf that should contain key.
    // Returns the leaf's page_id (does NOT pin it — caller must fetch_page).
    page_id_t find_leaf(int64_t key);

    // Called after a leaf becomes full during insert.
    // Splits leaf_page into two halves, links them, and returns the
    // "push-up key" (= first key of the new right leaf).
    // new_leaf_pid is set to the page_id of the newly created right leaf.
    int64_t split_leaf(Page* leaf_page, page_id_t leaf_pid,
                       page_id_t& new_leaf_pid);

    // Called after an internal node becomes full during insert_into_parent.
    // Splits node_page into two halves and returns the middle key to push up.
    // new_node_pid is set to the page_id of the newly created right node.
    int64_t split_internal(Page* node_page, page_id_t node_pid,
                           page_id_t& new_node_pid);

    // Insert (key, right_pid) into the parent of left_pid.
    // Handles the case where the parent is also full (recursive splits).
    // If left_pid IS the root, creates a new internal root.
    void insert_into_parent(page_id_t left_pid, int64_t key, page_id_t right_pid);
};