// An in-memory B+-tree that counts node visits as logical page I/O.
//
// Nodes live in a page table indexed by NodeId, so a NodeId plays the role of
// a page number. Leaves are chained left to right for range scans.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "options.h"

namespace sabtree {

class BPlusTree {
 public:
  explicit BPlusTree(Options opts = {}) : opts_(opts) {
    assert(opts_.leaf_capacity >= 3 && opts_.inner_capacity >= 3);
    std::size_t slots = 0;
    switch (opts_.hints) {
      case HintPolicy::None: break;
      case HintPolicy::Rightmost: slots = 1; break;
      case HintPolicy::Multi: slots = std::max(1u, opts_.hint_slots); break;
    }
    hint_lo_.assign(slots, ~Key{0});
    hint_span_.assign(slots, 0);
    hint_leaf_.assign(slots, kNoNode);
    hint_stamp_.assign(slots, 0);
  }

  // Inserts key -> value. Returns false (and overwrites the value) if the key
  // was already present.
  bool insert(Key key, Value value) {
    ++stats_.inserts;
    if (root_ == kNoNode) {
      root_ = alloc(/*leaf=*/true);
      height_ = 1;
    }

    // Probe the hint cache while it is paying off. Every kHintWindow probes
    // the hit rate is checked; below one half (random keys, or more streams
    // than slots) the cache is only consulted on every kHintSample-th insert,
    // and it switches back on once a window of samples hits again.
    int slot = -1;
    const bool use_hints =
        !hint_leaf_.empty() && (hints_on_ || stats_.inserts % kHintSample == 0);
    if (use_hints) {
      ++stats_.hint_probes;
      slot = probe_hints(key);
      if (slot >= 0) ++window_hits_;
      if (++window_probes_ == kHintWindow) {
        hints_on_ = 2 * window_hits_ >= window_probes_;
        window_probes_ = window_hits_ = 0;
      }
      bool inserted;
      if (slot >= 0 && hinted_insert(slot, key, value, inserted)) return inserted;
    }

    // Descend from the root, remembering the path for split propagation.
    path_.clear();
    NodeId cur = root_;
    for (;;) {
      Node& n = read(cur);
      if (n.leaf) break;
      auto idx = child_index(n, key);
      path_.push_back({cur, idx});
      cur = n.children[idx];
    }

    Node& leaf = node(cur);
    auto pos = lower_index(leaf, key);
    if (pos < leaf.keys.size() && leaf.keys[pos] == key) {
      leaf.vals[pos] = value;
      ++stats_.page_writes;
      if (use_hints) remember_leaf(cur, slot);
      return false;
    }
    leaf.keys.insert(leaf.keys.begin() + pos, key);
    leaf.vals.insert(leaf.vals.begin() + pos, value);
    note_insert(leaf, pos);
    ++stats_.page_writes;
    ++size_;
    if (leaf.keys.size() <= opts_.leaf_capacity) {
      if (use_hints) remember_leaf(cur, slot);
      return true;
    }

    Key up;
    NodeId right = split_leaf(cur, pos, up);
    if (use_hints) remember_leaf(key < up ? cur : right, slot);

    // Push the separator up until some ancestor has room.
    while (!path_.empty()) {
      auto [pid, cidx] = path_.back();
      path_.pop_back();
      Node& p = node(pid);
      p.keys.insert(p.keys.begin() + cidx, up);
      p.children.insert(p.children.begin() + cidx + 1, right);
      note_insert(p, cidx);
      ++stats_.page_writes;
      if (p.keys.size() <= opts_.inner_capacity) return true;
      right = split_inner(pid, cidx, up);
    }

    // The root itself split: grow the tree by one level.
    NodeId new_root = alloc(/*leaf=*/false);
    Node& r = node(new_root);
    r.keys.push_back(up);
    r.children = {root_, right};
    root_ = new_root;
    ++height_;
    ++stats_.page_writes;
    return true;
  }

  std::optional<Value> find(Key key) {
    if (root_ == kNoNode) return std::nullopt;
    NodeId cur = root_;
    for (;;) {
      Node& n = read(cur);
      if (n.leaf) break;
      cur = n.children[child_index(n, key)];
    }
    const Node& leaf = node(cur);
    auto pos = lower_index(leaf, key);
    if (pos < leaf.keys.size() && leaf.keys[pos] == key) return leaf.vals[pos];
    return std::nullopt;
  }

  // Calls fn(key, value) for every key in [lo, hi], in order. Returns the
  // number of entries visited.
  template <typename Fn>
  std::uint64_t scan(Key lo, Key hi, Fn&& fn) {
    if (root_ == kNoNode || lo > hi) return 0;
    NodeId cur = root_;
    for (;;) {
      Node& n = read(cur);
      if (n.leaf) break;
      cur = n.children[child_index(n, lo)];
    }
    std::uint64_t visited = 0;
    auto pos = lower_index(node(cur), lo);
    for (;;) {
      const Node& leaf = node(cur);
      for (; pos < leaf.keys.size(); ++pos) {
        if (leaf.keys[pos] > hi) return visited;
        fn(leaf.keys[pos], leaf.vals[pos]);
        ++visited;
      }
      if (leaf.next == kNoNode) return visited;
      cur = leaf.next;
      read(cur);
      pos = 0;
    }
  }

  // Removes a key. Nodes are never merged: a leaf that drains stays in the
  // tree with its key range, which is also what PostgreSQL does until VACUUM
  // reclaims it. Returns false if the key was absent.
  bool erase(Key key) {
    if (root_ == kNoNode) return false;
    NodeId cur = root_;
    for (;;) {
      Node& n = read(cur);
      if (n.leaf) break;
      cur = n.children[child_index(n, key)];
    }
    Node& leaf = node(cur);
    auto pos = lower_index(leaf, key);
    if (pos == leaf.keys.size() || leaf.keys[pos] != key) return false;
    leaf.keys.erase(leaf.keys.begin() + pos);
    leaf.vals.erase(leaf.vals.begin() + pos);
    leaf.last_pos = -1;  // positions shifted; forget the current run
    leaf.run = 0;
    ++stats_.page_writes;
    --size_;
    return true;
  }

  std::uint64_t size() const { return size_; }
  std::uint32_t height() const { return height_; }
  const Options& options() const { return opts_; }
  const Stats& stats() const { return stats_; }
  void reset_stats() { stats_ = Stats{}; }

  Shape shape() const {
    Shape s;
    s.height = height_;
    std::uint64_t leaf_keys = 0, inner_keys = 0;
    for (const auto& n : pages_) {
      if (n->leaf) {
        ++s.leaves;
        leaf_keys += n->keys.size();
      } else {
        ++s.inners;
        inner_keys += n->keys.size();
      }
    }
    s.entries = leaf_keys;
    if (s.leaves) s.leaf_fill = double(leaf_keys) / double(s.leaves * opts_.leaf_capacity);
    if (s.inners) s.inner_fill = double(inner_keys) / double(s.inners * opts_.inner_capacity);
    return s;
  }

  // Verifies the structural invariants. Returns an empty string if the tree
  // is well formed, otherwise a description of the first violation found.
  std::string check() const {
    if (root_ == kNoNode) return size_ == 0 ? "" : "empty tree with nonzero size";
    std::vector<NodeId> leaves;
    std::uint64_t entries = 0;
    std::string err = check_node(root_, 0, false, 0, true, 1, leaves, entries);
    if (!err.empty()) return err;
    if (entries != size_) return "entry count does not match size()";
    // The sibling chain must visit exactly the leaves in key order.
    NodeId cur = leaves.front();
    for (std::size_t i = 0; i < leaves.size(); ++i) {
      if (cur != leaves[i]) return "leaf chain out of order at leaf " + std::to_string(i);
      cur = node(cur).next;
    }
    if (cur != kNoNode) return "leaf chain continues past the last leaf";
    // Cached fences must match the leaf exactly, or a hinted insert could
    // place a key in the wrong leaf.
    for (std::size_t i = 0; i < hint_leaf_.size(); ++i) {
      if (hint_leaf_[i] == kNoNode) continue;
      const Node& n = node(hint_leaf_[i]);
      if (!n.leaf || hint_lo_[i] != n.low || hint_span_[i] != span_of(n))
        return "stale leaf hint for node " + std::to_string(hint_leaf_[i]);
    }
    return "";
  }

 private:
  struct Node {
    bool leaf = true;
    std::vector<Key> keys;
    std::vector<Value> vals;        // leaves only
    std::vector<NodeId> children;   // inner nodes only, keys.size() + 1 entries
    NodeId next = kNoNode;          // right sibling (leaves only)
    // Fence keys (leaves only): the leaf owns exactly [low, high). They are
    // fixed at split time, so they stay valid when keys are erased.
    Key low = 0;
    Key high = 0;
    bool high_inf = true;  // rightmost leaf: no upper bound
    // Insert-direction bookkeeping, in the spirit of InnoDB's PAGE_LAST_INSERT
    // and PAGE_N_DIRECTION: the slot of the last insert and how many inserts
    // in a row landed immediately to the right of the previous one.
    std::int32_t last_pos = -1;
    std::uint32_t run = 0;
  };

  static constexpr std::uint32_t kHintWindow = 64;
  static constexpr std::uint32_t kHintSample = 16;

  // A cached leaf owning [low, high) is stored as low and span = high-1-low,
  // so "key in range" is the single unsigned compare key - low <= span.
  static Key span_of(const Node& n) { return (n.high_inf ? ~Key{0} : n.high - 1) - n.low; }

  // Returns the slot whose leaf owns `key`, or -1. Cached leaves have
  // disjoint ranges, so at most one slot can match. Empty slots have
  // low = max key and span 0 and so can only match the max key itself.
  int probe_hints(Key key) const {
    for (std::size_t i = 0; i < hint_lo_.size(); ++i)
      if (key - hint_lo_[i] <= hint_span_[i] && hint_leaf_[i] != kNoNode)
        return static_cast<int>(i);
    return -1;
  }

  // Inserts straight into the cached leaf, reading only that page. Gives up
  // (and the caller descends normally) if the leaf is full, because a split
  // needs the path to the parent.
  bool hinted_insert(int slot, Key key, Value value, bool& inserted) {
    Node& leaf = read(hint_leaf_[slot]);
    auto pos = lower_index(leaf, key);
    if (pos < leaf.keys.size() && leaf.keys[pos] == key) {
      leaf.vals[pos] = value;
      inserted = false;
    } else {
      if (leaf.keys.size() >= opts_.leaf_capacity) return false;
      leaf.keys.insert(leaf.keys.begin() + pos, key);
      leaf.vals.insert(leaf.vals.begin() + pos, value);
      note_insert(leaf, pos);
      ++size_;
      inserted = true;
    }
    ++stats_.page_writes;
    ++stats_.hint_hits;
    hint_stamp_[slot] = ++tick_;
    return true;
  }

  // Caches the leaf a slow-path insert ended in. If the probe had matched a
  // slot (its leaf was full and has just split) that slot is reused: the
  // leaf the stream continues in replaces the one it filled. Otherwise the
  // least recently used slot is evicted. Rightmost caches only the rightmost
  // leaf, like PostgreSQL.
  void remember_leaf(NodeId id, int slot) {
    const Node& n = node(id);
    if (opts_.hints == HintPolicy::Rightmost && !n.high_inf) return;
    std::size_t victim = 0;
    if (slot >= 0) {
      victim = static_cast<std::size_t>(slot);
    } else {
      for (std::size_t i = 1; i < hint_stamp_.size(); ++i)
        if (hint_stamp_[i] < hint_stamp_[victim]) victim = i;
    }
    hint_leaf_[victim] = id;
    hint_lo_[victim] = n.low;
    hint_span_[victim] = span_of(n);
    hint_stamp_[victim] = ++tick_;
  }

  // A split shrinks the left leaf's range; keep any cached copy exact.
  void hints_after_split(NodeId left) {
    for (std::size_t i = 0; i < hint_leaf_.size(); ++i) {
      if (hint_leaf_[i] != left) continue;
      if (opts_.hints == HintPolicy::Rightmost) {
        hint_leaf_[i] = kNoNode;  // no longer the rightmost leaf
        hint_lo_[i] = ~Key{0};
        hint_span_[i] = 0;
      } else {
        hint_span_[i] = span_of(node(left));
      }
    }
  }

  // Nodes are sized like fixed pages up front (one spare slot for the
  // overflow that triggers a split), so filling a page never reallocates.
  NodeId alloc(bool leaf) {
    auto n = std::make_unique<Node>();
    n->leaf = leaf;
    if (leaf) {
      n->keys.reserve(opts_.leaf_capacity + 1);
      n->vals.reserve(opts_.leaf_capacity + 1);
    } else {
      n->keys.reserve(opts_.inner_capacity + 1);
      n->children.reserve(opts_.inner_capacity + 2);
    }
    pages_.push_back(std::move(n));
    return static_cast<NodeId>(pages_.size() - 1);
  }

  Node& node(NodeId id) { return *pages_[id]; }
  const Node& node(NodeId id) const { return *pages_[id]; }

  // node() plus accounting: use this whenever an operation visits a page.
  Node& read(NodeId id) {
    ++stats_.page_reads;
    return *pages_[id];
  }

  // Child i of an inner node covers keys in [keys[i-1], keys[i]).
  static std::uint32_t child_index(const Node& n, Key key) {
    return static_cast<std::uint32_t>(
        std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin());
  }

  // Recursive part of check(). The subtree at `id` must hold keys in
  // [lo, hi) (hi unbounded when hi_inf) and sit at the given depth.
  std::string check_node(NodeId id, Key lo, bool lo_set, Key hi, bool hi_inf,
                         std::uint32_t depth, std::vector<NodeId>& leaves,
                         std::uint64_t& entries) const {
    const Node& n = node(id);
    const std::string where = " (node " + std::to_string(id) + ")";
    for (std::size_t i = 1; i < n.keys.size(); ++i)
      if (n.keys[i - 1] >= n.keys[i]) return "keys not strictly increasing" + where;
    for (Key k : n.keys) {
      if (lo_set && k < lo) return "key below lower bound" + where;
      if (!hi_inf && k >= hi) return "key at or above upper bound" + where;
    }
    if (n.leaf) {
      if (depth != height_) return "leaf at wrong depth" + where;
      if (n.keys.size() > opts_.leaf_capacity) return "leaf over capacity" + where;
      if (n.vals.size() != n.keys.size()) return "value count mismatch" + where;
      if (n.low != (lo_set ? lo : 0)) return "low fence mismatch" + where;
      if (n.high_inf != hi_inf || (!hi_inf && n.high != hi)) return "high fence mismatch" + where;
      leaves.push_back(id);
      entries += n.keys.size();
      return "";
    }
    if (n.keys.empty()) return "inner node without keys" + where;
    if (n.keys.size() > opts_.inner_capacity) return "inner node over capacity" + where;
    if (n.children.size() != n.keys.size() + 1) return "child count mismatch" + where;
    for (std::size_t i = 0; i < n.children.size(); ++i) {
      Key clo = i == 0 ? lo : n.keys[i - 1];
      bool clo_set = i == 0 ? lo_set : true;
      Key chi = i == n.keys.size() ? hi : n.keys[i];
      bool chi_inf = i == n.keys.size() ? hi_inf : false;
      auto err = check_node(n.children[i], clo, clo_set, chi, chi_inf, depth + 1, leaves, entries);
      if (!err.empty()) return err;
    }
    return "";
  }

  static std::uint32_t lower_index(const Node& n, Key key) {
    return static_cast<std::uint32_t>(
        std::lower_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin());
  }

  static void note_insert(Node& n, std::uint32_t pos) {
    if (n.last_pos >= 0 && pos == static_cast<std::uint32_t>(n.last_pos) + 1)
      ++n.run;
    else
      n.run = 0;
    n.last_pos = static_cast<std::int32_t>(pos);
  }

  // Number of keys that stay in the left node. `n` is the overflowing key
  // count and `pos` the slot the new key landed in. A leaf needs
  // 1 <= s <= n-1; an inner node moves key s up and needs a key on each side,
  // so 1 <= s <= n-2.
  std::uint32_t choose_split(const Node& nd, std::uint32_t pos, std::uint32_t n,
                             bool leaf) const {
    const std::uint32_t hi = leaf ? n - 1 : n - 2;
    auto clamp = [hi](std::uint32_t s) { return std::min(std::max(s, 1u), hi); };
    switch (opts_.split) {
      case SplitPolicy::Middle:
        break;
      case SplitPolicy::InsertPoint:
        if (nd.run >= 1) return clamp(pos);
        break;
      case SplitPolicy::Adaptive:
        if (nd.run >= opts_.run_threshold) {
          // Keep the new key on the left, so the run keeps appending to a node
          // that holds only its own keys and whatever followed it moves out.
          // For a pure append this leaves the left node completely full.
          auto cap = leaf ? opts_.leaf_capacity : opts_.inner_capacity;
          auto limit = std::max(1u, static_cast<std::uint32_t>(opts_.seq_fill * cap));
          return clamp(std::min(pos + 1, limit));
        }
        break;
    }
    return n / 2;
  }

  // After a split, the half that received the new key inherits the run;
  // the other half starts with no history.
  static void split_history(Node& left, Node& right, std::uint32_t pos, std::uint32_t s,
                            bool leaf) {
    const std::uint32_t run = left.run;
    left.last_pos = -1;
    left.run = 0;
    right.last_pos = -1;
    right.run = 0;
    if (pos < s) {
      left.last_pos = static_cast<std::int32_t>(pos);
      left.run = run;
    } else if (leaf || pos > s) {
      right.last_pos = static_cast<std::int32_t>(pos - s - (leaf ? 0 : 1));
      right.run = run;
    }
  }

  // Splits an overflowing leaf; returns the new right sibling and stores the
  // separator (its first key) in `sep`.
  NodeId split_leaf(NodeId id, std::uint32_t pos, Key& sep) {
    auto n = static_cast<std::uint32_t>(node(id).keys.size());
    auto s = choose_split(node(id), pos, n, /*leaf=*/true);
    NodeId rid = alloc(/*leaf=*/true);
    Node& left = node(id);
    Node& right = node(rid);
    right.keys.assign(left.keys.begin() + s, left.keys.end());
    right.vals.assign(left.vals.begin() + s, left.vals.end());
    left.keys.resize(s);
    left.vals.resize(s);
    right.next = left.next;
    left.next = rid;
    sep = right.keys.front();
    right.low = sep;
    right.high = left.high;
    right.high_inf = left.high_inf;
    left.high = sep;
    left.high_inf = false;
    split_history(left, right, pos, s, /*leaf=*/true);
    hints_after_split(id);
    ++stats_.leaf_splits;
    ++stats_.page_writes;
    return rid;
  }

  // Splits an overflowing inner node. Key `s` moves up into the parent.
  NodeId split_inner(NodeId id, std::uint32_t pos, Key& up) {
    auto n = static_cast<std::uint32_t>(node(id).keys.size());
    auto s = choose_split(node(id), pos, n, /*leaf=*/false);
    NodeId rid = alloc(/*leaf=*/false);
    Node& left = node(id);
    Node& right = node(rid);
    up = left.keys[s];
    right.keys.assign(left.keys.begin() + s + 1, left.keys.end());
    right.children.assign(left.children.begin() + s + 1, left.children.end());
    left.keys.resize(s);
    left.children.resize(s + 1);
    split_history(left, right, pos, s, /*leaf=*/false);
    ++stats_.inner_splits;
    ++stats_.page_writes;
    return rid;
  }

  Options opts_;
  Stats stats_;
  std::vector<std::unique_ptr<Node>> pages_;
  std::vector<std::pair<NodeId, std::uint32_t>> path_;  // (inner node, child slot)
  // Leaf hint cache, structure-of-arrays so the probe is a tight loop.
  std::vector<Key> hint_lo_;
  std::vector<Key> hint_span_;
  std::vector<NodeId> hint_leaf_;
  std::vector<std::uint64_t> hint_stamp_;  // LRU clock values
  std::uint64_t tick_ = 0;
  bool hints_on_ = true;            // false: sampling only (see insert())
  std::uint32_t window_probes_ = 0;
  std::uint32_t window_hits_ = 0;
  NodeId root_ = kNoNode;
  std::uint32_t height_ = 0;
  std::uint64_t size_ = 0;
};

}  // namespace sabtree
