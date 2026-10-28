// Differential tests: every tree configuration is driven with the same
// operation streams as a std::map and must agree with it at every step, while
// check() keeps passing.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sabtree/bplus_tree.h"
#include "sabtree/workloads.h"

using namespace sabtree;

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
      ++g_failures;                                                           \
      return;                                                                 \
    }                                                                         \
  } while (0)

struct Config {
  std::string name;
  Options opts;
};

std::vector<Config> configs(std::uint32_t leaf_cap, std::uint32_t inner_cap) {
  Options base;
  base.leaf_capacity = leaf_cap;
  base.inner_capacity = inner_cap;
  Options insert_point = base;
  insert_point.split = SplitPolicy::InsertPoint;
  Options adaptive = base;
  adaptive.split = SplitPolicy::Adaptive;
  Options adaptive100 = adaptive;
  adaptive100.seq_fill = 1.0;
  Options eager = adaptive;
  eager.run_threshold = 0;  // every split is treated as part of a run
  Options rightmost = base;
  rightmost.hints = HintPolicy::Rightmost;
  Options multi = base;
  multi.hints = HintPolicy::Multi;
  multi.hint_slots = 4;
  Options one_slot = multi;
  one_slot.hint_slots = 1;
  Options ours = adaptive;
  ours.hints = HintPolicy::Multi;
  Options ours100 = ours;
  ours100.seq_fill = 1.0;
  return {{"middle", base},
          {"insert-point", insert_point},
          {"adaptive", adaptive},
          {"adaptive-f100", adaptive100},
          {"adaptive-eager", eager},
          {"rightmost-hint", rightmost},
          {"multi-hint", multi},
          {"multi-hint-1", one_slot},
          {"adaptive+multi", ours},
          {"adaptive-f100+multi", ours100}};
}

// Inserts `keys` in order, checking against a std::map every `every` steps.
void run_inserts(const Config& c, const std::vector<Key>& keys, std::size_t every) {
  BPlusTree t(c.opts);
  std::map<Key, Value> ref;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    bool fresh = ref.insert_or_assign(keys[i], i).second;
    EXPECT(t.insert(keys[i], i) == fresh, "insert return value");
    if (i % every == 0 || i + 1 == keys.size()) {
      auto err = t.check();
      EXPECT(err.empty(), err.c_str());
    }
  }
  EXPECT(t.size() == ref.size(), "size");
  for (const auto& [k, v] : ref) EXPECT(t.find(k) == v, "find after load");
  std::vector<Key> got;
  t.scan(0, ~Key{0}, [&](Key k, Value) { got.push_back(k); });
  EXPECT(got.size() == ref.size(), "full scan size");
  std::size_t i = 0;
  for (const auto& kv : ref) EXPECT(got[i++] == kv.first, "full scan order");
}

// Random mix of inserts, overwrites, erases, lookups and range scans.
void run_mixed(const Config& c, std::uint64_t seed, int ops, Key domain) {
  BPlusTree t(c.opts);
  std::map<Key, Value> ref;
  SplitMix64 rng(seed);
  for (int i = 0; i < ops; ++i) {
    Key k = rng.next() % domain;
    auto op = rng.next() % 10;
    if (op < 5) {
      bool fresh = ref.insert_or_assign(k, i).second;
      EXPECT(t.insert(k, i) == fresh, "insert return value");
    } else if (op < 7) {
      EXPECT(t.erase(k) == (ref.erase(k) == 1), "erase return value");
    } else if (op < 9) {
      auto it = ref.find(k);
      auto got = t.find(k);
      EXPECT(got.has_value() == (it != ref.end()), "find presence");
      if (got) EXPECT(*got == it->second, "find value");
    } else {
      Key hi = k + rng.next() % 64;
      std::vector<Key> got, want;
      t.scan(k, hi, [&](Key kk, Value) { got.push_back(kk); });
      for (auto it = ref.lower_bound(k); it != ref.end() && it->first <= hi; ++it)
        want.push_back(it->first);
      EXPECT(got == want, "range scan");
    }
    if (i % 997 == 0) {
      auto err = t.check();
      EXPECT(err.empty(), err.c_str());
    }
  }
  auto err = t.check();
  EXPECT(err.empty(), err.c_str());
  EXPECT(t.size() == ref.size(), "size after mixed ops");
}

void test_empty_tree() {
  BPlusTree t;
  EXPECT(t.size() == 0, "empty size");
  EXPECT(!t.find(42), "find on empty tree");
  EXPECT(!t.erase(42), "erase on empty tree");
  EXPECT(t.scan(0, 100, [](Key, Value) {}) == 0, "scan on empty tree");
  EXPECT(t.check().empty(), "empty tree invariants");
}

void test_extreme_keys() {
  for (const auto& c : configs(3, 3)) {
    BPlusTree t(c.opts);
    const Key max = ~Key{0};
    std::vector<Key> keys = {max, 0, max - 1, 1, max / 2, max - 2, 2};
    for (Key k : keys) t.insert(k, k);
    EXPECT(t.check().empty(), "extreme keys invariants");
    for (Key k : keys) EXPECT(t.find(k) == k, "extreme key lookup");
    EXPECT(t.scan(max - 2, max, [](Key, Value) {}) == 3, "scan up to max key");
  }
}

void test_workloads() {
  const std::size_t n = 20000;
  std::vector<std::pair<std::string, std::vector<Key>>> loads = {
      {"sequential", sequential_keys(n)},
      {"descending", descending_keys(n)},
      {"random", random_keys(n, 7)},
      {"streams-4", stream_keys(n, 4, 7)},
      {"streams-64", stream_keys(n, 64, 7)},
      {"nearly-sorted", nearly_sorted_keys(n, 0.05, 500, 7)},
  };
  for (auto caps : {std::pair<std::uint32_t, std::uint32_t>{3, 3}, {4, 5}, {16, 8}, {256, 256}}) {
    for (const auto& c : configs(caps.first, caps.second)) {
      for (const auto& [name, keys] : loads) {
        int before = g_failures;
        run_inserts(c, keys, caps.first < 16 ? 101 : 2003);
        if (g_failures != before)
          std::fprintf(stderr, "  ^ config=%s caps=%u/%u workload=%s\n", c.name.c_str(),
                       caps.first, caps.second, name.c_str());
      }
    }
  }
}

void test_mixed_operations() {
  for (auto caps : {std::pair<std::uint32_t, std::uint32_t>{3, 3}, {5, 4}, {32, 16}}) {
    for (const auto& c : configs(caps.first, caps.second)) {
      for (std::uint64_t seed : {1, 2, 3}) {
        int before = g_failures;
        run_mixed(c, seed, 60000, 4000);
        if (g_failures != before)
          std::fprintf(stderr, "  ^ config=%s caps=%u/%u seed=%llu\n", c.name.c_str(),
                       caps.first, caps.second, static_cast<unsigned long long>(seed));
      }
    }
  }
}

void test_sequential_page_reads() {
  // Without any shortcut every insert walks root-to-leaf: reads == height.
  Options o;
  o.leaf_capacity = 8;
  o.inner_capacity = 8;
  BPlusTree t(o);
  for (Key k = 0; k < 1000; ++k) t.insert(k, k);
  for (Key k = 1000; k < 1100; ++k) {
    auto h = t.height();  // read before the insert: a root split may grow it
    t.reset_stats();
    t.insert(k, k);
    EXPECT(t.stats().page_reads == h, "baseline insert reads one page per level");
  }
}

void test_adaptive_split_fills_pages() {
  Options mid;
  mid.leaf_capacity = 64;
  mid.inner_capacity = 64;
  Options ada = mid;
  ada.split = SplitPolicy::Adaptive;
  ada.seq_fill = 1.0;
  BPlusTree a(mid), b(ada);
  for (Key k = 0; k < 100000; ++k) {
    a.insert(k, k);
    b.insert(k, k);
  }
  EXPECT(a.shape().leaf_fill < 0.52, "middle split leaves sequential leaves half full");
  EXPECT(b.shape().leaf_fill > 0.99, "adaptive split packs sequential leaves");
  EXPECT(b.shape().inner_fill > 0.9, "adaptive split packs inner nodes too");
  EXPECT(b.shape().leaves < 0.52 * a.shape().leaves, "about half as many leaves");

  // Random order must not trigger the append path.
  BPlusTree c(mid), d(ada);
  for (Key k : random_keys(100000, 11)) {
    c.insert(k, k);
    d.insert(k, k);
  }
  EXPECT(c.shape().leaves == d.shape().leaves, "adaptive split is inert on random keys");
}

void test_streams_keep_pages_full() {
  // Many interleaved ascending streams: the naive insert-point split strands
  // the head of the neighbouring stream, the adaptive split does not.
  Options naive;
  naive.leaf_capacity = 64;
  naive.inner_capacity = 64;
  naive.split = SplitPolicy::InsertPoint;
  Options ada = naive;
  ada.split = SplitPolicy::Adaptive;
  ada.seq_fill = 1.0;
  BPlusTree a(naive), b(ada);
  for (Key k : stream_keys(200000, 32, 5)) {
    a.insert(k, k);
    b.insert(k, k);
  }
  EXPECT(b.shape().leaf_fill > 0.95, "adaptive split keeps stream leaves full");
  EXPECT(b.shape().leaf_fill > a.shape().leaf_fill, "adaptive beats insert-point on streams");
}

void test_fill_limit_absorbs_late_keys() {
  // With 2% of keys arriving late, pages packed to 100% get split again by
  // the stragglers; a 90% limit leaves room for them.
  Options full;
  full.leaf_capacity = 64;
  full.inner_capacity = 64;
  full.split = SplitPolicy::Adaptive;
  full.seq_fill = 1.0;
  Options slack = full;
  slack.seq_fill = 0.9;
  BPlusTree a(full), b(slack);
  for (Key k : nearly_sorted_keys(100000, 0.02, 200, 9)) {
    a.insert(k, k);
    b.insert(k, k);
  }
  EXPECT(b.shape().leaf_fill > 0.85, "90% fill limit keeps leaves dense");
  EXPECT(b.shape().leaf_fill > a.shape().leaf_fill + 0.2, "slack beats full packing");
}

void test_random_keys_bypass_hints() {
  Options o;
  o.hints = HintPolicy::Multi;
  BPlusTree t(o);
  auto keys = random_keys(1000000, 4);
  for (std::size_t i = 0; i < 500000; ++i) t.insert(keys[i], i);
  t.reset_stats();
  for (std::size_t i = 500000; i < keys.size(); ++i) t.insert(keys[i], i);
  // Once the tree is large, probes keep missing and the cache backs off to
  // sampling, so few inserts pay for a probe.
  EXPECT(t.stats().hint_probes < 0.2 * 500000, "hint probes back off on random keys");
  EXPECT(t.check().empty(), "invariants after bypass");
}

void test_hints_skip_descent() {
  Options o;
  o.leaf_capacity = 64;
  o.inner_capacity = 64;
  o.split = SplitPolicy::Adaptive;
  BPlusTree plain(o);
  o.hints = HintPolicy::Rightmost;
  BPlusTree rightmost(o);
  o.hints = HintPolicy::Multi;
  o.hint_slots = 16;
  BPlusTree multi(o);

  for (Key k = 0; k < 100000; ++k) {
    plain.insert(k, k);
    rightmost.insert(k, k);
    multi.insert(k, k);
  }
  const double n = 100000;
  EXPECT(plain.stats().page_reads / n > 2.5, "plain inserts descend the whole tree");
  EXPECT(rightmost.stats().page_reads / n < 1.1, "rightmost hint serves appends");
  EXPECT(multi.stats().page_reads / n < 1.1, "multi hint serves appends");

  // Eight interleaved streams: one rightmost slot cannot cover them, 16 can.
  BPlusTree r8(rightmost.options()), m8(multi.options());
  for (Key k : stream_keys(100000, 8, 3)) {
    r8.insert(k, k);
    m8.insert(k, k);
  }
  EXPECT(m8.stats().hint_hits > 0.9 * n, "multi hint covers 8 streams");
  EXPECT(r8.stats().hint_hits < 0.2 * n, "rightmost hint only covers the last stream");
  EXPECT(m8.check().empty(), "multi-hint tree invariants");
}

void test_hint_survives_overwrites_and_erases() {
  Options o;
  o.leaf_capacity = 4;
  o.inner_capacity = 4;
  o.split = SplitPolicy::Adaptive;
  o.hints = HintPolicy::Multi;
  o.hint_slots = 2;
  BPlusTree t(o);
  std::map<Key, Value> ref;
  for (Key k = 0; k < 2000; ++k) {
    t.insert(k * 2, k);
    ref[k * 2] = k;
    if (k % 3 == 0) {
      t.insert(k, 7);  // overwrite or fill a gap behind the hinted leaf
      ref[k] = 7;
    }
    if (k % 5 == 0) {
      t.erase(k);
      ref.erase(k);
    }
  }
  auto err = t.check();
  EXPECT(err.empty(), err.c_str());
  for (const auto& [k, v] : ref) EXPECT(t.find(k) == v, "lookup after hinted writes");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"empty_tree", test_empty_tree},
      {"extreme_keys", test_extreme_keys},
      {"workloads", test_workloads},
      {"mixed_operations", test_mixed_operations},
      {"sequential_page_reads", test_sequential_page_reads},
      {"adaptive_split_fills_pages", test_adaptive_split_fills_pages},
      {"streams_keep_pages_full", test_streams_keep_pages_full},
      {"fill_limit_absorbs_late_keys", test_fill_limit_absorbs_late_keys},
      {"random_keys_bypass_hints", test_random_keys_bypass_hints},
      {"hints_skip_descent", test_hints_skip_descent},
      {"hint_survives_overwrites_and_erases", test_hint_survives_overwrites_and_erases},
  };
  for (const auto& [name, fn] : tests) {
    int before = g_failures;
    fn();
    std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
  }
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("all tests passed\n");
  return EXIT_SUCCESS;
}
