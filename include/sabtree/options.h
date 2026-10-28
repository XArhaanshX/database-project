// Configuration knobs and counters shared by the tree, the tests and the
// benchmark driver.
#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace sabtree {

using Key = std::uint64_t;
using Value = std::uint64_t;
using NodeId = std::uint32_t;

inline constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();

// Where an overflowing node is cut in two.
enum class SplitPolicy {
  Middle,       // textbook 50/50 split
  InsertPoint,  // if the last two inserts were adjacent, cut at the new key
                // and start the right node with it (InnoDB-style heuristic)
  Adaptive,     // cut just after the new key once an ascending run of
                // `run_threshold` adjacent inserts has been seen (ours)
};

// Which leaves an insert may jump to directly, skipping the descent.
enum class HintPolicy {
  None,       // always descend from the root
  Rightmost,  // cache only the rightmost leaf (PostgreSQL's nbtree fastpath)
  Multi,      // LRU cache of recently written leaves with their fence keys (ours)
};

struct Options {
  // Maximum number of keys a node may hold. 256 entries of 8-byte keys and
  // 8-byte payloads is roughly one 4 KiB page.
  std::uint32_t leaf_capacity = 256;
  std::uint32_t inner_capacity = 256;

  SplitPolicy split = SplitPolicy::Middle;

  // Adaptive split: number of consecutive "insert right after the previous
  // insert" events a node must see before it is treated as append-only.
  std::uint32_t run_threshold = 3;
  // Adaptive split: the left node keeps at most this fraction of capacity,
  // leaving slack for late, slightly out-of-order keys. 0.9 matches
  // PostgreSQL's default B-tree fillfactor; 1.0 packs append-only data fully.
  double seq_fill = 0.9;

  HintPolicy hints = HintPolicy::None;
  // Multi hints: number of cached leaves. Rightmost always uses one.
  std::uint32_t hint_slots = 16;
};

// Logical I/O counters. A "page read" is one node visited by an operation;
// a "page write" is one node created or modified.
struct Stats {
  std::uint64_t inserts = 0;
  std::uint64_t page_reads = 0;
  std::uint64_t page_writes = 0;
  std::uint64_t leaf_splits = 0;
  std::uint64_t inner_splits = 0;
  std::uint64_t hint_probes = 0;  // inserts that consulted the leaf hint cache
  std::uint64_t hint_hits = 0;    // inserts served by the leaf hint cache
};

// Space usage of a built tree.
struct Shape {
  std::uint32_t height = 0;
  std::uint64_t leaves = 0;
  std::uint64_t inners = 0;
  std::uint64_t entries = 0;
  double leaf_fill = 0.0;   // mean leaf occupancy as a fraction of capacity
  double inner_fill = 0.0;  // mean inner-node occupancy (keys / capacity)
};

}  // namespace sabtree
