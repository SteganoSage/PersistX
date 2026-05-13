#include "btree_page.hpp"


void BTreeLeafPage::init(page_id_t page_id) {
    std::memset(page_->raw(),0, PAGE_SIZE);

    std::memcpy(page_->raw()+0, &page_id, sizeof(page_id));

    uint8_t  node_type    = BTREE_NODE_LEAF;     // = 1
    uint16_t num_keys     = 0;
    page_id_t parent_id   = INVALID_PAGE_ID;
    page_id_t next_leaf   = INVALID_PAGE_ID;

    std::memcpy(page_->raw() + 4,  &node_type,  sizeof(node_type));
    std::memcpy(page_->raw() + 5,  &num_keys,   sizeof(num_keys));
    std::memcpy(page_->raw() + 7,  &parent_id,  sizeof(parent_id));
    std::memcpy(page_->raw() + 11, &next_leaf,  sizeof(next_leaf));

    
}

page_id_t BTreeLeafPage::get_page_id() const {
    page_id_t id;
    std::memcpy(&id, page_->raw(), sizeof(id));
    return id;
}

uint8_t BTreeLeafPage::get_node_type() const {
    uint8_t type;
    std::memcpy(&type, page_->raw() + 4, sizeof(type));
    return type;
}

uint16_t BTreeLeafPage::get_num_keys() const {
    uint16_t keys;
    std::memcpy(&keys, page_->raw() + 5, sizeof(keys));
    return keys;
}

page_id_t BTreeLeafPage::get_parent_id() const {
    page_id_t pid;
    std::memcpy(&pid, page_->raw() + 7, sizeof(pid));
    return pid;
}

RID BTreeLeafPage::get_rid_at(uint16_t index) const {
    RID rid;
    size_t off = entry_offset(index) + BTREE_KEY_SIZE;
    std::memcpy(&rid.page_id, page_->raw() + off,                      sizeof(rid.page_id));
    std::memcpy(&rid.slot_id, page_->raw() + off + sizeof(rid.page_id), sizeof(rid.slot_id));
    return rid;
}

page_id_t BTreeLeafPage::get_next_leaf_id() const {
    page_id_t nid;
    std::memcpy(&nid, page_->raw() + 11, sizeof(nid));
    return nid;
}

void BTreeLeafPage::set_num_keys(uint16_t n) {
    std::memcpy(page_->raw() + 5, &n, sizeof(n));
}

void BTreeLeafPage::set_parent_id(page_id_t pid){
    std::memcpy(page_->raw()+7,&pid,sizeof(pid));
}

void BTreeLeafPage::set_key_at(uint16_t index, int64_t key) {
    std::memcpy(page_->raw() + entry_offset(index), &key, sizeof(key));
}

void BTreeLeafPage::set_rid_at(uint16_t index, const RID& rid) {
    size_t off = entry_offset(index) + BTREE_KEY_SIZE;
    std::memcpy(page_->raw() + off,                      &rid.page_id, sizeof(rid.page_id));
    std::memcpy(page_->raw() + off + sizeof(rid.page_id), &rid.slot_id, sizeof(rid.slot_id));
}

void BTreeLeafPage::set_next_leaf_id(page_id_t nid){
    std::memcpy(page_->raw()+11,&nid,sizeof(nid));
}

size_t BTreeLeafPage::entry_offset(uint16_t index) const {
    return BTREE_LEAF_HEADER_SIZE + index * LEAF_ENTRY_SIZE;
}

// ─── key_index ───────────────────────────────────────────────────────────────
// Binary search (lower_bound): returns the index of the first key >= target.
// If all keys are smaller, returns num_keys (i.e. "insert at the end").

uint16_t BTreeLeafPage::key_index(int64_t key) const {
    uint16_t lo = 0;
    uint16_t hi = get_num_keys();

    while (lo < hi) {
        uint16_t mid = (lo + hi) / 2;
        if (get_key_at(mid) < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// ─── insert ──────────────────────────────────────────────────────────────────
// Insert (key, rid) in sorted order.  Returns false if the leaf is full.
// The caller (BTreeIndex) is responsible for splitting when this returns false.

bool BTreeLeafPage::insert(int64_t key, const RID& rid) {
    uint16_t n = get_num_keys();
    if (n >= LEAF_MAX_ENTRIES) return false;   // full — caller must split

    uint16_t pos = key_index(key);

    // Shift entries [pos..n-1] one slot to the right to make room.
    for (int i = static_cast<int>(n) - 1; i >= static_cast<int>(pos); --i) {
        set_key_at(i + 1, get_key_at(i));
        set_rid_at(i + 1, get_rid_at(i));
    }

    // Write the new entry at the insertion point.
    set_key_at(pos, key);
    set_rid_at(pos, rid);
    set_num_keys(n + 1);
    return true;
}

// ─── lookup ──────────────────────────────────────────────────────────────────
// Binary search for an exact key match.  Returns the RID, or an invalid RID
// (page_id == INVALID_PAGE_ID) if not found.

RID BTreeLeafPage::lookup(int64_t key) const {
    uint16_t pos = key_index(key);
    if (pos < get_num_keys() && get_key_at(pos) == key)
        return get_rid_at(pos);
    return RID{};   // not found — default RID is invalid
}

// ─── remove ──────────────────────────────────────────────────────────────────
// Find the key and shift everything after it one slot to the left.
// Returns false if the key doesn't exist.

bool BTreeLeafPage::remove(int64_t key) {
    uint16_t pos = key_index(key);
    uint16_t n   = get_num_keys();

    if (pos >= n || get_key_at(pos) != key)
        return false;   // key not found

    // Shift entries [pos+1..n-1] one slot to the left.
    for (uint16_t i = pos; i < n - 1; ++i) {
        set_key_at(i, get_key_at(i + 1));
        set_rid_at(i, get_rid_at(i + 1));
    }

    set_num_keys(n - 1);
    return true;
}


// ═════════════════════════════════════════════════════════════════════════════
// BTreeInternalPage implementation
// ═════════════════════════════════════════════════════════════════════════════
//
// Layout after the 11-byte header:
//   child[0] (4B) | key[0] (8B) | child[1] (4B) | key[1] (8B) | ... | child[N] (4B)
//
// child_offset(i) = HEADER + i * 12          (every 12 bytes: 4B child + 8B key)
// key_offset(i)   = HEADER + i * 12 + 4      (key sits 4 bytes after its child)

// ─── init ────────────────────────────────────────────────────────────────────

void BTreeInternalPage::init(page_id_t page_id) {
    std::memset(page_->raw(), 0, PAGE_SIZE);

    uint8_t   node_type = BTREE_NODE_INTERNAL;
    uint16_t  num_keys  = 0;
    page_id_t parent_id = INVALID_PAGE_ID;

    std::memcpy(page_->raw() + 0,  &page_id,   sizeof(page_id));
    std::memcpy(page_->raw() + 4,  &node_type,  sizeof(node_type));
    std::memcpy(page_->raw() + 5,  &num_keys,   sizeof(num_keys));
    std::memcpy(page_->raw() + 7,  &parent_id,  sizeof(parent_id));
}

// ─── header accessors ────────────────────────────────────────────────────────

page_id_t BTreeInternalPage::get_page_id() const {
    page_id_t val;
    std::memcpy(&val, page_->raw() + 0, sizeof(val));
    return val;
}

uint8_t BTreeInternalPage::get_node_type() const {
    uint8_t val;
    std::memcpy(&val, page_->raw() + 4, sizeof(val));
    return val;
}

uint16_t BTreeInternalPage::get_num_keys() const {
    uint16_t val;
    std::memcpy(&val, page_->raw() + 5, sizeof(val));
    return val;
}

page_id_t BTreeInternalPage::get_parent_id() const {
    page_id_t val;
    std::memcpy(&val, page_->raw() + 7, sizeof(val));
    return val;
}

void BTreeInternalPage::set_num_keys(uint16_t n) {
    std::memcpy(page_->raw() + 5, &n, sizeof(n));
}

void BTreeInternalPage::set_parent_id(page_id_t pid) {
    std::memcpy(page_->raw() + 7, &pid, sizeof(pid));
}

// ─── offset helpers ──────────────────────────────────────────────────────────
//
// The data region is laid out as:
//   child[0]  key[0]  child[1]  key[1]  ...  child[N]
//   4 bytes   8 bytes  4 bytes   8 bytes      4 bytes
//
// Each (child, key) pair occupies 12 bytes.
// child[i] is at offset: HEADER + i * 12
// key[i]   is at offset: HEADER + i * 12 + 4

size_t BTreeInternalPage::child_offset(uint16_t index) const {
    return BTREE_INTERNAL_HEADER_SIZE + static_cast<size_t>(index) * 12;
}

size_t BTreeInternalPage::key_offset(uint16_t index) const {
    return BTREE_INTERNAL_HEADER_SIZE + static_cast<size_t>(index) * 12 + 4;
}

// ─── entry accessors ─────────────────────────────────────────────────────────

int64_t BTreeInternalPage::get_key_at(uint16_t index) const {
    int64_t val;
    std::memcpy(&val, page_->raw() + key_offset(index), sizeof(val));
    return val;
}
int64_t BTreeLeafPage::get_key_at(uint16_t index) const {
    int64_t key;
    std::memcpy(&key, page_->raw() + entry_offset(index), sizeof(key));
    return key;
}

page_id_t BTreeInternalPage::get_child_at(uint16_t index) const {
    page_id_t val;
    std::memcpy(&val, page_->raw() + child_offset(index), sizeof(val));
    return val;
}

void BTreeInternalPage::set_key_at(uint16_t index, int64_t key) {
    std::memcpy(page_->raw() + key_offset(index), &key, sizeof(key));
}

void BTreeInternalPage::set_child_at(uint16_t index, page_id_t pid) {
    std::memcpy(page_->raw() + child_offset(index), &pid, sizeof(pid));
}

// ─── lookup_child ────────────────────────────────────────────────────────────
// Find which child pointer to follow for a given search key.
//
//   keys:     [10]  [20]  [30]
//   children: [c0]  [c1]  [c2]  [c3]
//
//   key < 10         → child[0]
//   10 ≤ key < 20    → child[1]
//   20 ≤ key < 30    → child[2]
//   key ≥ 30         → child[3]

page_id_t BTreeInternalPage::lookup_child(int64_t key) const {
    uint16_t n = get_num_keys();

    // Linear scan (small enough with 340 keys; could binary-search too)
    uint16_t i = 0;
    while (i < n && key >= get_key_at(i))
        ++i;

    return get_child_at(i);
}

// ─── insert_after ────────────────────────────────────────────────────────────
// After a child split: insert a new (key, new_child) to the right of old_child.
//
// Before:  ... child[i]=old_child  key[i] ...
// After:   ... child[i]=old_child  key[i]=NEW_KEY  child[i+1]=new_child ...
//
// Returns false if the node is full (caller must split this node too).

bool BTreeInternalPage::insert_after(page_id_t old_child, int64_t key, page_id_t new_child) {
    uint16_t n = get_num_keys();
    if (n >= INTERNAL_MAX_KEYS) return false;   // full

    // Find the index of old_child.
    uint16_t i = 0;
    while (i <= n && get_child_at(i) != old_child)
        ++i;

    // Shift keys[i..n-1] and children[i+1..n] to the right.
    for (int j = static_cast<int>(n) - 1; j >= static_cast<int>(i); --j) {
        set_key_at(j + 1, get_key_at(j));
        set_child_at(j + 2, get_child_at(j + 1));
    }

    // Insert the new key and child.
    set_key_at(i, key);
    set_child_at(i + 1, new_child);
    set_num_keys(n + 1);
    return true;
}