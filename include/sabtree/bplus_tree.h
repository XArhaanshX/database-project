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
  }

  // Inserts key -> value. Returns false (and overwrites the value) if the key
  // was already present.
  bool insert(Key key, Value value) {
    ++stats_.inserts;
    if (root_ == kNoNode) {
      root_ = alloc(/*leaf=*/true);
      height_ = 1;
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
      return false;
    }
    leaf.keys.insert(leaf.keys.begin() + pos, key);
    leaf.vals.insert(leaf.vals.begin() + pos, value);
    ++stats_.page_writes;
    ++size_;
    if (leaf.keys.size() <= opts_.leaf_capacity) return true;

    Key up;
    NodeId right = split_leaf(cur, pos, up);

    // Push the separator up until some ancestor has room.
    while (!path_.empty()) {
      auto [pid, cidx] = path_.back();
      path_.pop_back();
      Node& p = node(pid);
      p.keys.insert(p.keys.begin() + cidx, up);
      p.children.insert(p.children.begin() + cidx + 1, right);
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
  };

  NodeId alloc(bool leaf) {
    auto n = std::make_unique<Node>();
    n->leaf = leaf;
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

  // Number of keys that stay in the left node. `n` is the overflowing key
  // count and `pos` the slot the new key landed in.
  std::uint32_t choose_split(const Node& /*node*/, std::uint32_t /*pos*/, std::uint32_t n,
                             bool /*leaf*/) const {
    return n / 2;
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
    ++stats_.inner_splits;
    ++stats_.page_writes;
    return rid;
  }

  Options opts_;
  Stats stats_;
  std::vector<std::unique_ptr<Node>> pages_;
  std::vector<std::pair<NodeId, std::uint32_t>> path_;  // (inner node, child slot)
  NodeId root_ = kNoNode;
  std::uint32_t height_ = 0;
  std::uint64_t size_ = 0;
};

}  // namespace sabtree
