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
#include <stdbool.h>

  typedef struct faster_t faster_t;
  typedef struct faster_result faster_result;
  typedef struct faster_recover_result faster_recover_result;
  typedef void (*faster_callback)(faster_result);

  struct faster_recover_result {
    uint8_t* result;
    uint32_t* version;
    const char** session_ids;
  };

  // Operations
  faster_t* faster_open_with_disk(const uint64_t table_size, const uint64_t log_size, const char* storage);
  void faster_upsert(faster_t* faster_t, const uint64_t key, const uint64_t value);
  uint8_t faster_rmw(faster_t* faster_t, const uint64_t key, const uint64_t value);
  uint8_t faster_read(faster_t* faster_t, const uint64_t key, faster_callback* _callback);
  bool faster_checkpoint(faster_t* faster_t, const char* token);
  void faster_destroy(faster_t* faster_t);
  uint64_t faster_size(faster_t* faster_t);
  void faster_complete_pending(faster_t* faster_t);
  faster_recover_result* faster_recover(faster_t* faster_t, const char* index_token, const char* hybrid_log_token);
  // TODO:
  // CheckpointIndex
  // CheckpointHybridLog
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

