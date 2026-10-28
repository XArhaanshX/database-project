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

 private:
  struct Node {
    bool leaf = true;
    std::vector<Key> keys;
    std::vector<Value> vals;        // leaves only
    std::vector<NodeId> children;   // inner nodes only, keys.size() + 1 entries
    NodeId next = kNoNode;          // right sibling (leaves only)
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
