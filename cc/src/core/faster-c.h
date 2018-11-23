// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

// C interface for the C++ code


#ifndef C_INTERFACE_FASTER_C_H_
#define C_INTERFACE_FASTER_C_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
  typedef struct faster_t faster_t;

  // Operations
  faster_t* faster_open_with_disk(const char* storage, const size_t key_space);
  void faster_upsert(faster_t* faster_t);
  char* faster_read(faster_t* faster_t, const char* key);
  void faster_destroy(faster_t* faster_t);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  /* C_INTERFACE_FASTER_C_H_ */

