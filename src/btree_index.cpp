#include "btree_index.hpp"
#include <cassert>
#include <cstring>

// ═════════════════════════════════════════════════════════════════════════════
// BTreeIndex implementation
// ═════════════════════════════════════════════════════════════════════════════

BTreeIndex::BTreeIndex(BufferManager* bm) : bm_(bm) {}

// ─── create ──────────────────────────────────────────────────────────────────
// Allocate one leaf page and make it the root. The tree starts empty.

page_id_t BTreeIndex::create() {
    page_id_t pid;
    Page* page = bm_->new_page(pid);
    assert(page != nullptr && "BTreeIndex::create — buffer pool exhausted");

    BTreeLeafPage leaf(page);
    leaf.init(pid);

    bm_->unpin_page(pid, /*is_dirty=*/true);
    root_page_id_ = pid;
    return pid;
}

// ─── load ────────────────────────────────────────────────────────────────────
// Point the index at an existing root (reopening a database file).

void BTreeIndex::load(page_id_t root_page_id) {
    root_page_id_ = root_page_id;
}

// ─── find_leaf ───────────────────────────────────────────────────────────────
// Walk from the root to the leaf that *should* contain key.
// At each internal node, use lookup_child() to pick the right subtree.
// Returns the leaf page_id WITHOUT pinning it — the caller calls fetch_page.

page_id_t BTreeIndex::find_leaf(int64_t key) {
    page_id_t cur_pid = root_page_id_;

    while (true) {
        Page* page = bm_->fetch_page(cur_pid);
        assert(page != nullptr);

        // Peek at the node type to decide how to interpret this page.
        uint8_t node_type;
        std::memcpy(&node_type, page->raw() + 4, sizeof(node_type));

        if (node_type == BTREE_NODE_LEAF) {
            // We've reached a leaf — unpin and return its page_id.
            // The caller will re-fetch it to do work.
            bm_->unpin_page(cur_pid, false);
            return cur_pid;
        }

        // Internal node: route to the correct child.
        BTreeInternalPage internal(page);
        page_id_t child_pid = internal.lookup_child(key);
        bm_->unpin_page(cur_pid, false);

        cur_pid = child_pid;
    }
}

// ─── search ──────────────────────────────────────────────────────────────────
// Point lookup: find the leaf, binary-search for the key, return its RID.

RID BTreeIndex::search(int64_t key) {
    if (root_page_id_ == INVALID_PAGE_ID) return RID{};

    page_id_t leaf_pid = find_leaf(key);

    Page* page = bm_->fetch_page(leaf_pid);
    assert(page != nullptr);

    BTreeLeafPage leaf(page);
    RID result = leaf.lookup(key);

    bm_->unpin_page(leaf_pid, false);
    return result;
}

// ─── remove ──────────────────────────────────────────────────────────────────
// Find the leaf and remove the key. No node merging (PostgreSQL policy).

bool BTreeIndex::remove(int64_t key) {
    if (root_page_id_ == INVALID_PAGE_ID) return false;

    page_id_t leaf_pid = find_leaf(key);

    Page* page = bm_->fetch_page(leaf_pid);
    assert(page != nullptr);

    BTreeLeafPage leaf(page);
    bool removed = leaf.remove(key);

    bm_->unpin_page(leaf_pid, removed);  // dirty only if we actually removed
    return removed;
}

// ─── range_scan ──────────────────────────────────────────────────────────────
// Find the leaf for begin_key, then walk the leaf-linked list collecting RIDs
// until we pass end_key (or run out of leaves).

std::vector<RID> BTreeIndex::range_scan(int64_t begin_key, int64_t end_key) {
    std::vector<RID> results;
    if (root_page_id_ == INVALID_PAGE_ID) return results;

    page_id_t cur_pid = find_leaf(begin_key);

    while (cur_pid != INVALID_PAGE_ID) {
        Page* page = bm_->fetch_page(cur_pid);
        assert(page != nullptr);

        BTreeLeafPage leaf(page);
        uint16_t n = leaf.get_num_keys();

        // Find the first entry >= begin_key on this leaf.
        uint16_t start_idx = leaf.key_index(begin_key);

        bool done = false;
        for (uint16_t i = start_idx; i < n; ++i) {
            int64_t k = leaf.get_key_at(i);
            if (k > end_key) {
                done = true;
                break;
            }
            results.push_back(leaf.get_rid_at(i));
        }

        page_id_t next_pid = leaf.get_next_leaf_id();
        bm_->unpin_page(cur_pid, false);

        if (done) break;
        cur_pid = next_pid;
    }

    return results;
}

// ─── insert ──────────────────────────────────────────────────────────────────
// Find the correct leaf and try to insert. If the leaf is full, split it and
// push the new separator key up into the parent via insert_into_parent().

bool BTreeIndex::insert(int64_t key, const RID& rid) {
    if (root_page_id_ == INVALID_PAGE_ID) return false;

    page_id_t leaf_pid = find_leaf(key);

    Page* page = bm_->fetch_page(leaf_pid);
    assert(page != nullptr);

    BTreeLeafPage leaf(page);

    // Duplicate key check: if the key already exists, reject.
    if (leaf.lookup(key).is_valid()) {
        bm_->unpin_page(leaf_pid, false);
        return false;
    }

    if (leaf.insert(key, rid)) {
        // Fast path: leaf had room. Done.
        bm_->unpin_page(leaf_pid, true);
        return true;
    }

    // Slow path: leaf is full — split it.
    // split_leaf creates the new right leaf, moves the upper half there,
    // fixes the next_leaf pointer chain, and gives us the push-up key.
    page_id_t new_leaf_pid;
    int64_t push_up_key = split_leaf(page, leaf_pid, new_leaf_pid);

    // After the split, decide which half the new key belongs in.
    // If key >= push_up_key, it goes into the new right leaf.
    if (key >= push_up_key) {
        Page* new_page = bm_->fetch_page(new_leaf_pid);
        assert(new_page != nullptr);
        BTreeLeafPage new_leaf(new_page);
        new_leaf.insert(key, rid);
        bm_->unpin_page(new_leaf_pid, true);
    } else {
        // Re-fetch the original (now smaller) left leaf and insert there.
        // (split_leaf already unpinned it; we need to re-fetch)
        Page* left_page = bm_->fetch_page(leaf_pid);
        assert(left_page != nullptr);
        BTreeLeafPage left_leaf(left_page);
        left_leaf.insert(key, rid);
        bm_->unpin_page(leaf_pid, true);
    }

    // Push the separator key up into the parent.
    insert_into_parent(leaf_pid, push_up_key, new_leaf_pid);
    return true;
}

// ─── split_leaf ──────────────────────────────────────────────────────────────
// Split a full leaf into two halves:
//   - Left (original page):  entries [0 .. mid-1]
//   - Right (new page):      entries [mid .. n-1]
// Links: new_right.next = old_leaf.next; old_leaf.next = new_right.
// Returns the first key of the right page (the separator to push up).
// Unpins leaf_page when done.

int64_t BTreeIndex::split_leaf(Page* leaf_page, page_id_t leaf_pid,
                                page_id_t& new_leaf_pid) {
    BTreeLeafPage left(leaf_page);
    uint16_t n   = left.get_num_keys();
    uint16_t mid = n / 2;  // right half starts here

    // Snapshot all entries before we start modifying anything.
    // (We're reading from the page we're about to overwrite.)
    struct Entry { int64_t key; RID rid; };
    std::vector<Entry> entries(n);
    for (uint16_t i = 0; i < n; ++i) {
        entries[i] = { left.get_key_at(i), left.get_rid_at(i) };
    }

    // Allocate the new right leaf.
    Page* new_page = bm_->new_page(new_leaf_pid);
    assert(new_page != nullptr);

    BTreeLeafPage right(new_page);
    right.init(new_leaf_pid);

    // Fix the linked list: new_right.next = left.next; left.next = new_right.
    right.set_next_leaf_id(left.get_next_leaf_id());
    left.set_next_leaf_id(new_leaf_pid);

    // Propagate parent_id to the new leaf.
    right.set_parent_id(left.get_parent_id());

    // Copy upper half [mid..n-1] into the right leaf.
    uint16_t right_count = 0;
    for (uint16_t i = mid; i < n; ++i) {
        right.set_key_at(right_count, entries[i].key);
        right.set_rid_at(right_count, entries[i].rid);
        ++right_count;
    }
    right.set_num_keys(right_count);

    // Shrink the left leaf to only [0..mid-1].
    left.set_num_keys(mid);

    // The push-up key is the first key of the new right leaf.
    int64_t push_up_key = entries[mid].key;

    bm_->unpin_page(leaf_pid,    true);
    bm_->unpin_page(new_leaf_pid, true);

    return push_up_key;
}

// ─── split_internal ──────────────────────────────────────────────────────────
// Split a full internal node. The layout is:
//   left:  keys[0..mid-1]  children[0..mid]
//   push:  keys[mid]               ← this key goes UP, NOT into either child
//   right: keys[mid+1..n-1]  children[mid+1..n]
//
// This is different from leaf splits! The middle key is NOT kept in either half.
// Returns the middle key to push up.
// Unpins node_page when done.

int64_t BTreeIndex::split_internal(Page* node_page, page_id_t node_pid,
                                    page_id_t& new_node_pid) {
    BTreeInternalPage left(node_page);
    uint16_t n   = left.get_num_keys();
    uint16_t mid = n / 2;

    // Snapshot keys and children.
    struct IEntry { int64_t key; page_id_t child; };
    // n keys, n+1 children
    std::vector<int64_t>   keys(n);
    std::vector<page_id_t> children(n + 1);
    for (uint16_t i = 0; i < n; ++i)   keys[i]     = left.get_key_at(i);
    for (uint16_t i = 0; i <= n; ++i)  children[i] = left.get_child_at(i);

    // The middle key goes up; it's NOT stored in either child.
    int64_t push_up_key = keys[mid];

    // Allocate the new right internal node.
    Page* new_page = bm_->new_page(new_node_pid);
    assert(new_page != nullptr);

    BTreeInternalPage right(new_page);
    right.init(new_node_pid);
    right.set_parent_id(left.get_parent_id());

    // Right node gets keys[mid+1..n-1] and children[mid+1..n].
    uint16_t right_key_count = 0;
    // First child of the right node is children[mid+1].
    right.set_child_at(0, children[mid + 1]);
    for (uint16_t i = mid + 1; i < n; ++i) {
        right.set_key_at(right_key_count, keys[i]);
        right.set_child_at(right_key_count + 1, children[i + 1]);
        ++right_key_count;
    }
    right.set_num_keys(right_key_count);

    // Left node keeps keys[0..mid-1] and children[0..mid].
    left.set_num_keys(mid);
    // children[0..mid] are already in place — no need to rewrite them.
    // But we do need to clear any stale data beyond mid (optional for safety).

    // Update parent_id of the children now owned by the right node.
    // This is important so future splits can navigate upward correctly.
    for (uint16_t i = 0; i <= right_key_count; ++i) {
        page_id_t child_pid = right.get_child_at(i);
        Page* child_page = bm_->fetch_page(child_pid);
        if (child_page != nullptr) {
            // node_type lives at byte 4; parent_id at bytes 7..10.
            // Both BTreeLeafPage and BTreeInternalPage have parent_id at [7].
            std::memcpy(child_page->raw() + 7, &new_node_pid, sizeof(new_node_pid));
            bm_->unpin_page(child_pid, true);
        }
    }

    bm_->unpin_page(node_pid,    true);
    bm_->unpin_page(new_node_pid, true);

    return push_up_key;
}

// ─── insert_into_parent ──────────────────────────────────────────────────────
// After splitting a child, insert the separator (key, right_pid) into the
// parent of left_pid. Handles three cases:
//
//   Case 1: left_pid IS the root → create a brand-new internal root.
//   Case 2: parent has room → call insert_after() and we're done.
//   Case 3: parent is full → split the parent, then recurse upward.

void BTreeIndex::insert_into_parent(page_id_t left_pid, int64_t key,
                                     page_id_t right_pid) {
    // Case 1: The left node is the root. Create a new root above it.
    if (left_pid == root_page_id_) {
        page_id_t new_root_pid;
        Page* new_root_page = bm_->new_page(new_root_pid);
        assert(new_root_page != nullptr);

        BTreeInternalPage new_root(new_root_page);
        new_root.init(new_root_pid);
        new_root.set_num_keys(1);
        new_root.set_child_at(0, left_pid);
        new_root.set_key_at(0, key);
        new_root.set_child_at(1, right_pid);

        // Update parent_id of both children.
        Page* left_page = bm_->fetch_page(left_pid);
        if (left_page) {
            std::memcpy(left_page->raw() + 7, &new_root_pid, sizeof(new_root_pid));
            bm_->unpin_page(left_pid, true);
        }
        Page* right_page = bm_->fetch_page(right_pid);
        if (right_page) {
            std::memcpy(right_page->raw() + 7, &new_root_pid, sizeof(new_root_pid));
            bm_->unpin_page(right_pid, true);
        }

        bm_->unpin_page(new_root_pid, true);
        root_page_id_ = new_root_pid;
        return;
    }

    // Find the parent of left_pid by reading left's parent_id from its header.
    Page* left_page = bm_->fetch_page(left_pid);
    assert(left_page != nullptr);
    page_id_t parent_pid;
    std::memcpy(&parent_pid, left_page->raw() + 7, sizeof(parent_pid));
    bm_->unpin_page(left_pid, false);

    // Case 2: Parent has room — just insert and we're done.
    Page* parent_page = bm_->fetch_page(parent_pid);
    assert(parent_page != nullptr);

    BTreeInternalPage parent(parent_page);

    if (parent.insert_after(left_pid, key, right_pid)) {
        // Update parent_id of right_pid.
        Page* right_page = bm_->fetch_page(right_pid);
        if (right_page) {
            std::memcpy(right_page->raw() + 7, &parent_pid, sizeof(parent_pid));
            bm_->unpin_page(right_pid, true);
        }
        bm_->unpin_page(parent_pid, true);
        return;
    }

    // Case 3: Parent is full — split the parent, then recurse upward.
    // insert_after() returned false, so we need to split before inserting.
    // Strategy: insert the new (key, right_pid) into a temporary oversized
    // snapshot, then split that snapshot into two real nodes.

    // --- Snapshot the parent's current state ---
    uint16_t n = parent.get_num_keys();
    std::vector<int64_t>   par_keys(n);
    std::vector<page_id_t> par_children(n + 1);
    for (uint16_t i = 0; i < n; ++i)   par_keys[i]     = parent.get_key_at(i);
    for (uint16_t i = 0; i <= n; ++i)  par_children[i] = parent.get_child_at(i);

    // Find insertion position for left_pid in the children array.
    uint16_t insert_pos = 0;
    while (insert_pos <= n && par_children[insert_pos] != left_pid)
        ++insert_pos;

    // Build the oversized arrays with the new entry inserted.
    std::vector<int64_t>   new_keys(n + 1);
    std::vector<page_id_t> new_children(n + 2);

    // Copy everything up to insert_pos.
    for (uint16_t i = 0; i < insert_pos; ++i) {
        new_keys[i]         = par_keys[i];
        new_children[i]     = par_children[i];
    }
    new_children[insert_pos] = left_pid;  // already there, but explicit
    new_keys[insert_pos]     = key;
    new_children[insert_pos + 1] = right_pid;

    // Copy the rest, shifted right by one.
    for (uint16_t i = insert_pos; i < n; ++i) {
        new_keys[i + 1]         = par_keys[i];
        new_children[i + 2]     = par_children[i + 1];
    }

    // n+1 keys, n+2 children total.
    uint16_t total_keys = n + 1;
    uint16_t mid        = total_keys / 2;          // middle key goes UP
    int64_t  parent_push_key = new_keys[mid];

    // Overwrite left (original parent) with the left half.
    parent.set_num_keys(mid);
    for (uint16_t i = 0; i < mid; ++i) {
        parent.set_key_at(i, new_keys[i]);
        parent.set_child_at(i, new_children[i]);
    }
    parent.set_child_at(mid, new_children[mid]);

    bm_->unpin_page(parent_pid, true);

    // Allocate the right sibling for the split parent.
    page_id_t new_parent_pid;
    Page* new_parent_page = bm_->new_page(new_parent_pid);
    assert(new_parent_page != nullptr);

    BTreeInternalPage new_parent(new_parent_page);
    new_parent.init(new_parent_pid);

    uint16_t right_key_count = total_keys - mid - 1;  // keys after the middle
    new_parent.set_num_keys(right_key_count);
    new_parent.set_child_at(0, new_children[mid + 1]);
    for (uint16_t i = 0; i < right_key_count; ++i) {
        new_parent.set_key_at(i, new_keys[mid + 1 + i]);
        new_parent.set_child_at(i + 1, new_children[mid + 2 + i]);
    }

    // Update parent_id for all children of the new right internal node.
    for (uint16_t i = 0; i <= right_key_count; ++i) {
        page_id_t child_pid = new_parent.get_child_at(i);
        Page* child_page = bm_->fetch_page(child_pid);
        if (child_page != nullptr) {
            std::memcpy(child_page->raw() + 7, &new_parent_pid, sizeof(new_parent_pid));
            bm_->unpin_page(child_pid, true);
        }
    }

    bm_->unpin_page(new_parent_pid, true);

    // Recurse: push parent_push_key further up the tree.
    insert_into_parent(parent_pid, parent_push_key, new_parent_pid);
}

// ─── get_stats ───────────────────────────────────────────────────────────────
// BFS traversal to count height, internal/leaf pages, and total keys.

BTreeIndex::TreeStats BTreeIndex::get_stats() {
    TreeStats stats{0, 0, 0, 0};
    if (root_page_id_ == INVALID_PAGE_ID) return stats;

    std::queue<page_id_t> q;
    q.push(root_page_id_);

    while (!q.empty()) {
        size_t level_size = q.size();
        ++stats.height;

        for (size_t i = 0; i < level_size; ++i) {
            page_id_t pid = q.front();
            q.pop();

            Page* page = bm_->fetch_page(pid);
            if (!page) continue;

            uint8_t node_type;
            std::memcpy(&node_type, page->raw() + 4, sizeof(node_type));

            uint16_t num_keys;
            std::memcpy(&num_keys, page->raw() + 5, sizeof(num_keys));

            stats.total_keys += num_keys;

            if (node_type == BTREE_NODE_LEAF) {
                ++stats.leaf_pages;
                bm_->unpin_page(pid, false);
            } else {
                ++stats.internal_pages;
                BTreeInternalPage internal(page);
                for (uint16_t c = 0; c <= num_keys; ++c) {
                    q.push(internal.get_child_at(c));
                }
                bm_->unpin_page(pid, false);
            }
        }
    }

    return stats;
}

// ─── get_tree_layout ─────────────────────────────────────────────────────────
// BFS that returns a snapshot of every node, level by level.

std::vector<std::vector<BTreeIndex::NodeInfo>> BTreeIndex::get_tree_layout() {
    std::vector<std::vector<NodeInfo>> levels;
    if (root_page_id_ == INVALID_PAGE_ID) return levels;

    std::queue<page_id_t> q;
    q.push(root_page_id_);

    while (!q.empty()) {
        size_t level_size = q.size();
        std::vector<NodeInfo> level;

        for (size_t i = 0; i < level_size; ++i) {
            page_id_t pid = q.front();
            q.pop();

            Page* page = bm_->fetch_page(pid);
            if (!page) continue;

            uint8_t node_type;
            std::memcpy(&node_type, page->raw() + 4, sizeof(node_type));

            uint16_t num_keys;
            std::memcpy(&num_keys, page->raw() + 5, sizeof(num_keys));

            NodeInfo info;
            info.page_id = pid;
            info.is_leaf = (node_type == BTREE_NODE_LEAF);

            if (info.is_leaf) {
                BTreeLeafPage leaf(page);
                for (uint16_t k = 0; k < num_keys; ++k) {
                    info.keys.push_back(leaf.get_key_at(k));
                }
            } else {
                BTreeInternalPage internal(page);
                for (uint16_t k = 0; k < num_keys; ++k) {
                    info.keys.push_back(internal.get_key_at(k));
                }
                for (uint16_t c = 0; c <= num_keys; ++c) {
                    page_id_t child = internal.get_child_at(c);
                    info.children.push_back(child);
                    q.push(child);
                }
            }

            bm_->unpin_page(pid, false);
            level.push_back(std::move(info));
        }

        levels.push_back(std::move(level));
    }

    return levels;
}