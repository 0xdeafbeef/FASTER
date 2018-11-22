// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

// C interface for the C++ code


#ifndef C_INTERFACE_FASTER_C_H_
#define C_INTERFACE_FASTER_C_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

  typedef struct fasterkv_t fasterkv_t;
  typedef struct fasterkv_disk_t fasterkv_disk_t;
  typedef struct fasterkv_key_t fasterkv_key_t;
  typedef struct fasterkv_value_t fasterkv_value_t;

  // Operations
  fasterkv_t* fasterkv_create();
  void fasterkv_destroy(fasterkv_t *store);

#ifdef __cplusplus
}  // extern "C"
#endif


#endif  /* C_INTERFACE_FASTER_C_H_ */

