// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

// C interface for the C++ code


#ifndef FASTER_C_H_
#define FASTER_C_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

  typedef struct faster_t faster_t;

  // Operations
  faster_t* faster_open_with_disk(const uint64_t table_size, const uint64_t log_size, const char* storage);
  void faster_upsert(faster_t* faster_t, const uint64_t key, const uint64_t value);
  uint8_t faster_rmw(faster_t* faster_t, const uint64_t key, const uint64_t value);
  uint8_t faster_read(faster_t* faster_t, const uint64_t key);
  void faster_destroy(faster_t* faster_t);
  uint64_t faster_size(faster_t* faster_t);
  // TODO:
  // Checkpoint
  // CheckpointIndex
  // CheckpointHybridLog
  // Recover
  // GrowIndex
  // StartSession
  // StopSession
  // ContinueSession
  // **Statistics**
  // DumpDistribution

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  /* FASTER_C_H_ */

