// Deterministic insert-order generators used by the tests and benchmarks.
//
// Everything is driven by SplitMix64 and a hand-written Fisher-Yates shuffle
// so a given seed produces the same key order on every compiler and standard
// library (std::shuffle's output is implementation defined).
#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "options.h"

namespace sabtree {

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  // Uniform in [0, bound). The modulo bias is negligible for our bounds.
  std::uint64_t below(std::uint64_t bound) { return next() % bound; }

  double unit() { return double(next() >> 11) * 0x1.0p-53; }

 private:
  std::uint64_t state_;
};

template <typename T>
void shuffle(std::vector<T>& v, SplitMix64& rng) {
  for (std::size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[rng.below(i)]);
}

// 0, 1, 2, ...: auto-increment primary keys.
inline std::vector<Key> sequential_keys(std::size_t n) {
  std::vector<Key> keys(n);
  std::iota(keys.begin(), keys.end(), Key{0});
  return keys;
}

// n-1, n-2, ..., 0.
inline std::vector<Key> descending_keys(std::size_t n) {
  std::vector<Key> keys = sequential_keys(n);
  std::reverse(keys.begin(), keys.end());
  return keys;
}

// A uniformly random permutation of 0..n-1.
inline std::vector<Key> random_keys(std::size_t n, std::uint64_t seed) {
  std::vector<Key> keys = sequential_keys(n);
  SplitMix64 rng(seed);
  shuffle(keys, rng);
  return keys;
}

// `streams` independent ascending sequences interleaved at random, as with a
// composite key (device_id, timestamp) fed by many devices at once. Stream s
// owns the key range [s << 40, (s + 1) << 40).
inline std::vector<Key> stream_keys(std::size_t n, std::uint32_t streams, std::uint64_t seed) {
  std::vector<Key> keys(n);
  std::vector<Key> counter(streams, 0);
  SplitMix64 rng(seed);
  for (auto& k : keys) {
    auto s = rng.below(streams);
    k = (Key(s) << 40) | counter[s]++;
  }
  return keys;
}

// Ascending keys where a fraction `late` of the records arrive up to
// `max_delay` positions late, as with event timestamps from a network feed.
inline std::vector<Key> nearly_sorted_keys(std::size_t n, double late, std::uint32_t max_delay,
                                           std::uint64_t seed) {
  std::vector<std::pair<std::uint64_t, Key>> arrivals(n);
  SplitMix64 rng(seed);
  for (std::size_t i = 0; i < n; ++i) {
    std::uint64_t delay = rng.unit() < late ? 1 + rng.below(max_delay) : 0;
    arrivals[i] = {i + delay, Key(i)};
  }
  std::sort(arrivals.begin(), arrivals.end());
  std::vector<Key> keys(n);
  for (std::size_t i = 0; i < n; ++i) keys[i] = arrivals[i].second;
  return keys;
}

}  // namespace sabtree
