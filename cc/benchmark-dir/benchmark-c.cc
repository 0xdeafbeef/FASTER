// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <deque>
#include <map>

#include "file.h"

#include "core/auto_ptr.h"
#include "core/faster-c.h"
#include "device/null_disk.h"

using namespace std::chrono_literals;
using namespace FASTER::core;

/// Basic YCSB benchmark.

enum class Op : uint8_t {
  Insert = 0,
  Read = 1,
  Upsert = 2,
  Scan = 3,
  ReadModifyWrite = 4,
};

enum class Workload {
  A_50_50 = 0,
  RMW_100 = 1,
};

static constexpr uint64_t kInitCount = 250000000;
static constexpr uint64_t kTxnCount = 1000000000;
static constexpr uint64_t kChunkSize = 3200;
static constexpr uint64_t kRefreshInterval = 64;
static constexpr uint64_t kCompletePendingInterval = 1600;

static_assert(kInitCount % kChunkSize == 0, "kInitCount % kChunkSize != 0");
static_assert(kTxnCount % kChunkSize == 0, "kTxnCount % kChunkSize != 0");
static_assert(kCompletePendingInterval % kRefreshInterval == 0,
              "kCompletePendingInterval % kRefreshInterval != 0");

static constexpr uint64_t kNanosPerSecond = 1000000000;

static constexpr uint64_t kMaxKey = 268435456;
static constexpr uint64_t kRunSeconds = 360;
static constexpr uint64_t kCheckpointSeconds = 30;

aligned_unique_ptr_t<uint64_t> init_keys_;
aligned_unique_ptr_t<uint64_t> txn_keys_;
std::atomic<uint64_t> idx_{ 0 };
std::atomic<bool> done_{ false };
std::atomic<uint64_t> total_duration_{ 0 };
std::atomic<uint64_t> total_reads_done_{ 0 };
std::atomic<uint64_t> total_writes_done_{ 0 };

inline Op ycsb_a_50_50(std::mt19937& rng) {
  if(rng() % 100 < 50) {
    return Op::Read;
  } else {
    return Op::Upsert;
  }
}

inline Op ycsb_rmw_100(std::mt19937& rng) {
  return Op::ReadModifyWrite;
}

void read_cb(void* target, const uint8_t* buffer, uint64_t length, faster_status status) {
  assert(status == Ok);
}

uint64_t rmw_cb(const uint8_t* current, uint64_t length, uint8_t* modification, uint64_t modification_length, uint8_t* dst) {
  assert(length == 1);
  assert(modification_length == 1);
  if (dst != NULL) {
    uint8_t* val = new uint8_t[1];
    val[0] = {static_cast<uint8_t>(current[0] + modification[0])};
    memcpy(dst, val, 1);
    free(val);
  }
  return 1;
}

/// Affinitize to hardware threads on the same core first, before
/// moving on to the next core.
void SetThreadAffinity(size_t core) {

  // For now, assume 36 cores. (Set this correctly for your test system.)
  constexpr size_t kCoreCount = 36;
#ifdef _WIN32
  HANDLE thread_handle = ::GetCurrentThread();
  GROUP_AFFINITY group;
  group.Group = WORD(core / kCoreCount);
  group.Mask = KAFFINITY(0x1llu << (core - kCoreCount * group.Group));
  ::SetThreadGroupAffinity(thread_handle, &group, nullptr);
#else
  // On our 28-core test system, we see CPU 0, Core 0 assigned to 0, 28;
  //                                    CPU 1, Core 0 assigned to 1, 29; etc.
  cpu_set_t mask;
  CPU_ZERO(&mask);
#ifdef NUMA
  switch(core % 4) {
  case 0:
    // 0 |-> 0
    // 4 |-> 2
    // 8 |-> 4
    core = core / 2;
    break;
  case 1:
    // 1 |-> 28
    // 5 |-> 30
    // 9 |-> 32
    core = kCoreCount + (core - 1) / 2;
    break;
  case 2:
    // 2  |-> 1
    // 6  |-> 3
    // 10 |-> 5
    core = core / 2;
    break;
  case 3:
    // 3  |-> 29
    // 7  |-> 31
    // 11 |-> 33
    core = kCoreCount + (core - 1) / 2;
    break;
  }
#else
  switch(core % 2) {
  case 0:
    // 0 |-> 0
    // 2 |-> 2
    // 4 |-> 4
    core = core;
    break;
  case 1:
    // 1 |-> 28
    // 3 |-> 30
    // 5 |-> 32
    core = (core - 1) + kCoreCount;
    break;
  }
#endif
  CPU_SET(core, &mask);

  ::sched_setaffinity(0, sizeof(mask), &mask);
#endif
}

void load_files(const std::string& load_filename, const std::string& run_filename) {
  constexpr size_t kFileChunkSize = 131072;

  auto chunk_guard = alloc_aligned<uint64_t>(512, kFileChunkSize);
  uint64_t* chunk = chunk_guard.get();

  FASTER::benchmark::File init_file{ load_filename };

  printf("loading keys from %s into memory...\n", load_filename.c_str());

  init_keys_ = alloc_aligned<uint64_t>(64, kInitCount * sizeof(uint64_t));
  uint64_t count = 0;

  uint64_t offset = 0;
  while(true) {
    uint64_t size = init_file.Read(chunk, kFileChunkSize, offset);
    for(uint64_t idx = 0; idx < size / 8; ++idx) {
      init_keys_.get()[count] = chunk[idx];
      ++count;
    }
    if(size == kFileChunkSize) {
      offset += kFileChunkSize;
    } else {
      break;
    }
  }
  if(kInitCount != count) {
    printf("Init file load fail!\n");
    exit(1);
  }

  printf("loaded %" PRIu64 " keys.\n", count);

  FASTER::benchmark::File txn_file{ run_filename };

  printf("loading txns from %s into memory...\n", run_filename.c_str());

  txn_keys_ = alloc_aligned<uint64_t>(64, kTxnCount * sizeof(uint64_t));

  count = 0;
  offset = 0;

  while(true) {
    uint64_t size = txn_file.Read(chunk, kFileChunkSize, offset);
    for(uint64_t idx = 0; idx < size / 8; ++idx) {
      txn_keys_.get()[count] = chunk[idx];
      ++count;
    }
    if(size == kFileChunkSize) {
      offset += kFileChunkSize;
    } else {
      break;
    }
  }
  if(kTxnCount != count) {
    printf("Txn file load fail!\n");
    exit(1);
  }
  printf("loaded %" PRIu64 " txns.\n", count);
}

void thread_setup_store(faster_t* store, size_t thread_idx) {
  SetThreadAffinity(thread_idx);

  const char* guid = faster_start_session(store);

  uint8_t value = 42;
  for(uint64_t chunk_idx = idx_.fetch_add(kChunkSize); chunk_idx < kInitCount;
      chunk_idx = idx_.fetch_add(kChunkSize)) {
    for(uint64_t idx = chunk_idx; idx < chunk_idx + kChunkSize; ++idx) {
      if(idx % kRefreshInterval == 0) {
          faster_refresh_session(store);
        if(idx % kCompletePendingInterval == 0) {
          faster_complete_pending(store, false);
        }
      }

      uint8_t* val = new uint8_t[1];
      val[0] = value;
      uint8_t* key = new uint8_t[8];
      memcpy(key, &init_keys_.get()[idx], 8);
      faster_upsert(store, key, 8, val, 1, 1);
      free(val);
      free(key);
    }
  }

  faster_complete_pending(store, true);
  faster_stop_session(store);
}

void setup_store(faster_t* store, size_t num_threads) {
  idx_ = 0;
  std::deque<std::thread> threads;
  for(size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    threads.emplace_back(&thread_setup_store, store, thread_idx);
  }
  for(auto& thread : threads) {
    thread.join();
  }

  init_keys_.reset();

  printf("Finished populating store: contains ?? elements.\n");
}


static std::atomic<int64_t> async_reads_done{ 0 };
static std::atomic<int64_t> async_writes_done{ 0 };

template <Op(*FN)(std::mt19937&)>
void thread_run_benchmark(faster_t* store, size_t thread_idx) {
  SetThreadAffinity(thread_idx);

  std::random_device rd{};
  std::mt19937 rng{ rd() };

  auto start_time = std::chrono::high_resolution_clock::now();

  uint8_t upsert_value = 0;
  int64_t reads_done = 0;
  int64_t writes_done = 0;

  const char* guid = faster_start_session(store);

  while(!done_) {
    uint64_t chunk_idx = idx_.fetch_add(kChunkSize);
    while(chunk_idx >= kTxnCount) {
      if(chunk_idx == kTxnCount) {
        idx_ = 0;
      }
      chunk_idx = idx_.fetch_add(kChunkSize);
    }
    for(uint64_t idx = chunk_idx; idx < chunk_idx + kChunkSize; ++idx) {
      if(idx % kRefreshInterval == 0) {
        faster_refresh_session(store);
        if(idx % kCompletePendingInterval == 0) {
          faster_complete_pending(store, false);
        }
      }
      switch(FN(rng)) {
      case Op::Insert:
      case Op::Upsert: {
        uint8_t* val = new uint8_t[1];
        val[0] = upsert_value;
        uint8_t* key = new uint8_t[8];
        memcpy(key, &txn_keys_.get()[idx], 8);
        faster_upsert(store, key, 8, val, 1, 1);
        free(val);
        free(key);
        ++writes_done;
        break;
      }
      case Op::Scan:
        printf("Scan currently not supported!\n");
        exit(1);
        break;
      case Op::Read: {
        uint8_t* key = new uint8_t[8];
        memcpy(key, &txn_keys_.get()[idx], 8);
        faster_read(store, key, 8, 1, read_cb, NULL);
        free(key);
        ++reads_done;
        break;
      }
      case Op::ReadModifyWrite:
        uint8_t* modification = new uint8_t[1];
        modification[0] = 0;
        uint8_t* key = new uint8_t[8];
        memcpy(key, &txn_keys_.get()[idx], 8);
        uint8_t result = faster_rmw(store, key, 8, modification, 1, 1, rmw_cb);
        free(modification);
        free(key);
        if(result == 0) {
          ++writes_done;
        }
        break;
      }
    }
  }

  faster_complete_pending(store, true);
  faster_stop_session(store);

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::nanoseconds duration = end_time - start_time;
  total_duration_ += duration.count();
  total_reads_done_ += reads_done;
  total_writes_done_ += writes_done;
  printf("Finished thread %" PRIu64 " : %" PRIu64 " reads, %" PRIu64 " writes, in %.2f seconds.\n",
         thread_idx, reads_done, writes_done, (double)duration.count() / kNanosPerSecond);
}

template <Op(*FN)(std::mt19937&)>
void run_benchmark(faster_t* store, size_t num_threads) {
  idx_ = 0;
  total_duration_ = 0;
  total_reads_done_ = 0;
  total_writes_done_ = 0;
  done_ = false;
  std::deque<std::thread> threads;
  for(size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    threads.emplace_back(&thread_run_benchmark<FN>, store, thread_idx);
  }

  static std::atomic<uint64_t> num_checkpoints;
  num_checkpoints = 0;

  if(kCheckpointSeconds == 0) {
    std::this_thread::sleep_for(std::chrono::seconds(kRunSeconds));
  } else {
    auto callback = [](Status result, uint64_t persistent_serial_num) {
      if(result != Status::Ok) {
        printf("Thread %" PRIu32 " reports checkpoint failed.\n",
               Thread::id());
      } else {
        ++num_checkpoints;
      }
    };

    auto start_time = std::chrono::high_resolution_clock::now();
    auto last_checkpoint_time = start_time;
    auto current_time = start_time;

    uint64_t checkpoint_num = 0;

    while(current_time - start_time < std::chrono::seconds(kRunSeconds)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      current_time = std::chrono::high_resolution_clock::now();
      if(current_time - last_checkpoint_time >= std::chrono::seconds(kCheckpointSeconds)) {
        Guid token;
        faster_checkpoint_result* result = faster_checkpoint(store);
        if(result->checked) {
          printf("Starting checkpoint %" PRIu64 ".\n", checkpoint_num);
          ++checkpoint_num;
        } else {
          printf("Failed to start checkpoint.\n");
        }
        last_checkpoint_time = current_time;
        free(result->token);
        free(result);
      }
    }

    done_ = true;
  }

  for(auto& thread : threads) {
    thread.join();
  }

  printf("Finished benchmark: %" PRIu64 " thread checkpoints completed;  %.2f ops/second/thread\n",
         num_checkpoints.load(),
         ((double)total_reads_done_ + (double)total_writes_done_) / ((double)total_duration_ /
             kNanosPerSecond));
}

void run(Workload workload, size_t num_threads) {
  // FASTER store has a hash table with approx. kInitCount / 2 entries, a log of size 16 GB,
  // and a null device (it's in-memory only).
  size_t init_size = next_power_of_two(kInitCount / 2);
  faster_t* store = faster_open_with_disk(init_size, 17179869184, "storage");

  printf("Populating the store...\n");

  setup_store(store, num_threads);
  faster_dump_distribution(store);

  printf("Running benchmark on %" PRIu64 " threads...\n", num_threads);
  switch(workload) {
  case Workload::A_50_50:
    run_benchmark<ycsb_a_50_50>(store, num_threads);
    break;
  case Workload::RMW_100:
    run_benchmark<ycsb_rmw_100>(store, num_threads);
    break;
  default:
    printf("Unknown workload!\n");
    exit(1);
  }

  faster_destroy(store);
}

int main(int argc, char* argv[]) {
  constexpr size_t kNumArgs = 4;
  if(argc != kNumArgs + 1) {
    printf("Usage: benchmark.exe <workload> <# threads> <load_filename> <run_filename>\n");
    exit(0);
  }

  std::map<std::string, Workload> workloadMap;
  workloadMap["ycsb_a_50_50"] = Workload::A_50_50;
  workloadMap["ycsb_rmw_100"] = Workload::RMW_100;
  auto find_workload = workloadMap.find(argv[1]);
  Workload workload;
  if (find_workload != workloadMap.end()) {
    workload = find_workload->second;
  } else {
    printf("Unknown workload!\n");
    exit(1);
  }
  size_t num_threads = ::atol(argv[2]);
  std::string load_filename{ argv[3] };
  std::string run_filename{ argv[4] };

  load_files(load_filename, run_filename);

  run(workload, num_threads);

  return 0;
}
