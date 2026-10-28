// Benchmark driver: builds each tree variant on each insert workload and
// records logical I/O, space usage, lookup/scan cost and wall-clock time.
//
//   bench [--n N] [--reps R] [--out DIR] [--quick]
//
// Writes DIR/main.csv, DIR/streams_sweep.csv and DIR/late_sweep.csv.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sabtree/bplus_tree.h"
#include "sabtree/workloads.h"

using namespace sabtree;
using Clock = std::chrono::steady_clock;

namespace {

struct Variant {
  std::string name;
  Options opts;
};

std::vector<Variant> variants() {
  Options base;  // 256-entry nodes, middle split, no hints
  Options rightmost = base;
  rightmost.hints = HintPolicy::Rightmost;
  Options insert_point = base;
  insert_point.split = SplitPolicy::InsertPoint;
  Options existing = insert_point;
  existing.hints = HintPolicy::Rightmost;
  Options adaptive = base;
  adaptive.split = SplitPolicy::Adaptive;
  Options multi = base;
  multi.hints = HintPolicy::Multi;
  Options sa = adaptive;
  sa.hints = HintPolicy::Multi;
  Options sa100 = sa;
  sa100.seq_fill = 1.0;
  Options sa64 = sa;
  sa64.hint_slots = 64;
  return {
      {"baseline", base},           {"rightmost", rightmost},
      {"insert-point", insert_point}, {"rightmost+insert-point", existing},
      {"adaptive-split", adaptive}, {"multi-hint", multi},
      {"sa-btree", sa},             {"sa-btree-f100", sa100},
      {"sa-btree-64slots", sa64},
  };
}

struct Workload {
  std::string name;
  std::vector<Key> keys;
};

struct Result {
  double insert_ns = 0;  // median over timed reps, after one untimed warm-up
  Stats stats;
  Shape shape;
  double lookup_reads = 0, lookup_ns = 0;
  double scan_reads = 0;
};

double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

volatile std::uint64_t g_sink;  // keeps lookups from being optimised away

Result measure(const Options& opts, const std::vector<Key>& keys, int reps, bool probe) {
  Result r;
  std::vector<double> times;
  for (int rep = -1; rep < reps; ++rep) {
    BPlusTree t(opts);
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], i);
    if (rep < 0) continue;  // warm-up: first touch of fresh memory
    times.push_back(seconds_since(t0) * 1e9 / double(keys.size()));
    if (rep + 1 < reps) continue;

    r.stats = t.stats();
    r.shape = t.shape();
    if (!probe) break;

    // Point lookups of existing keys in random order.
    SplitMix64 rng(42);
    const std::size_t lookups = std::min<std::size_t>(200000, keys.size());
    std::vector<Key> probes(lookups);
    for (auto& k : probes) k = keys[rng.below(keys.size())];
    t.reset_stats();
    std::uint64_t sum = 0;
    t0 = Clock::now();
    for (Key k : probes) sum += t.find(k).value_or(0);
    r.lookup_ns = seconds_since(t0) * 1e9 / double(lookups);
    r.lookup_reads = double(t.stats().page_reads) / double(lookups);
    g_sink = sum;

    // Range scans returning 1000 consecutive entries each.
    std::vector<Key> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t scans = 2000, len = 1000;
    t.reset_stats();
    for (std::size_t i = 0; i < scans; ++i) {
      std::size_t at = rng.below(sorted.size() - len);
      t.scan(sorted[at], sorted[at + len - 1], [&](Key, Value v) { sum += v; });
    }
    r.scan_reads = double(t.stats().page_reads) / double(scans);
    g_sink = sum;
  }
  std::sort(times.begin(), times.end());
  r.insert_ns = times[times.size() / 2];
  return r;
}

const char* kHeader =
    "workload,variant,n,insert_ns,reads_per_insert,writes_per_insert,hint_hit_rate,"
    "leaf_splits,inner_splits,height,leaves,inners,leaf_fill,inner_fill,size_mib,"
    "lookup_reads,lookup_ns,scan1000_reads\n";

void write_row(std::FILE* f, const std::string& workload, const std::string& variant,
               std::size_t n, const Result& r) {
  const double dn = double(n);
  const double pages = double(r.shape.leaves + r.shape.inners);
  std::fprintf(f, "%s,%s,%zu,%.1f,%.4f,%.4f,%.4f,%llu,%llu,%u,%llu,%llu,%.4f,%.4f,%.2f,%.3f,%.1f,%.2f\n",
               workload.c_str(), variant.c_str(), n, r.insert_ns,
               double(r.stats.page_reads) / dn, double(r.stats.page_writes) / dn,
               double(r.stats.hint_hits) / dn,
               static_cast<unsigned long long>(r.stats.leaf_splits),
               static_cast<unsigned long long>(r.stats.inner_splits), r.shape.height,
               static_cast<unsigned long long>(r.shape.leaves),
               static_cast<unsigned long long>(r.shape.inners), r.shape.leaf_fill,
               r.shape.inner_fill, pages * 4096.0 / (1024.0 * 1024.0), r.lookup_reads,
               r.lookup_ns, r.scan_reads);
  std::fflush(f);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t n = 2'000'000;
  int reps = 5;
  std::string out = "results";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--n") && i + 1 < argc) {
      n = std::strtoull(argv[++i], nullptr, 10);
    } else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
      out = argv[++i];
    } else if (!std::strcmp(argv[i], "--quick")) {
      n = 200'000;
      reps = 1;
    } else {
      std::fprintf(stderr, "usage: %s [--n N] [--reps R] [--out DIR] [--quick]\n", argv[0]);
      return 2;
    }
  }

  const std::uint64_t seed = 2026;
  const std::vector<Workload> workloads = {
      {"sequential", sequential_keys(n)},
      {"random", random_keys(n, seed)},
      {"streams-8", stream_keys(n, 8, seed)},
      {"streams-64", stream_keys(n, 64, seed)},
      {"nearly-sorted", nearly_sorted_keys(n, 0.05, 1000, seed)},
  };
  const auto vs = variants();

  // 1. Every variant on every workload.
  std::FILE* f = std::fopen((out + "/main.csv").c_str(), "w");
  if (!f) {
    std::perror(out.c_str());
    return 1;
  }
  std::fputs(kHeader, f);
  std::printf("%-14s %-24s %9s %8s %7s %7s %9s\n", "workload", "variant", "ns/ins", "rd/ins",
              "fill", "MiB", "scan-rd");
  for (const auto& w : workloads) {
    for (const auto& v : vs) {
      if (v.name == "sa-btree-64slots") continue;  // sweep only
      Result r = measure(v.opts, w.keys, reps, /*probe=*/true);
      write_row(f, w.name, v.name, n, r);
      std::printf("%-14s %-24s %9.1f %8.3f %7.3f %7.1f %9.1f\n", w.name.c_str(),
                  v.name.c_str(), r.insert_ns, double(r.stats.page_reads) / double(n),
                  r.shape.leaf_fill, double(r.shape.leaves + r.shape.inners) * 4096.0 / 1048576.0,
                  r.scan_reads);
    }
  }
  std::fclose(f);

  // 2. Number of interleaved streams vs. a fixed 16-slot hint cache.
  f = std::fopen((out + "/streams_sweep.csv").c_str(), "w");
  std::fputs(kHeader, f);
  for (std::uint32_t streams : {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 1024u}) {
    auto keys = stream_keys(n, streams, seed);
    for (const auto& v : vs) {
      if (v.name != "baseline" && v.name != "rightmost+insert-point" &&
          v.name != "adaptive-split" && v.name != "sa-btree" && v.name != "sa-btree-64slots")
        continue;
      write_row(f, "streams-" + std::to_string(streams), v.name, n,
                measure(v.opts, keys, std::min(reps, 3), /*probe=*/false));
    }
  }
  std::fclose(f);

  // 3. Share of late records vs. the adaptive split's fill limit.
  f = std::fopen((out + "/late_sweep.csv").c_str(), "w");
  std::fputs(kHeader, f);
  for (double late : {0.0, 0.01, 0.02, 0.05, 0.10, 0.20}) {
    auto keys = nearly_sorted_keys(n, late, 1000, seed);
    char name[32];
    std::snprintf(name, sizeof name, "late-%.2f", late);
    for (const auto& v : vs) {
      if (v.name != "baseline" && v.name != "rightmost+insert-point" && v.name != "sa-btree" &&
          v.name != "sa-btree-f100")
        continue;
      write_row(f, name, v.name, n, measure(v.opts, keys, std::min(reps, 3), /*probe=*/false));
    }
  }
  std::fclose(f);
  std::printf("wrote %s/{main,streams_sweep,late_sweep}.csv\n", out.c_str());
  return 0;
}
