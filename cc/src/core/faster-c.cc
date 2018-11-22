// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.


#include "faster.h"
#include "faster-c.h"
#include "device/file_system_disk.h"

extern "C" {

  struct fasterkv_key_t {
    void *obj;
  };

  struct fasterkv_value_t {
    void *obj;
  };

  struct fasterkv_t { 
    void *obj;
    //FasterKv<fasterkv_key_t, fasterkv_value_t, fasterkv_disk_t>* obj;
  };

  struct fasterkv_disk_t {
    FASTER::device::FileSystemDisk<
      FASTER::environment::QueueIoHandler, 1073741824L>* obj;
  };

  fasterkv_t* fasterkv_create() {
    // for now
    FasterKv<int, int, FASTER::device::FileSystemDisk<
      FASTER::environment::QueueIoHandler, 1073741824L>> *store;

    fasterkv_t* res = new fasterkv_t;
    res->obj = store;
    return res;
  }

  void fasterkv_destroy(fasterkv_t *store) {
    if (store == NULL)
      return;
    free(store);
  }

} // extern "C"
