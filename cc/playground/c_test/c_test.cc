
#include "c_test.h"
#include "core/faster-c.h"


int main() {
  size_t key_space = (1L << 14);
  faster_t* store = faster_open_with_disk("storagez", key_space);
  faster_upsert(store);
  faster_read(store, "notakey");

  // Free the resources tied to FASTER
  faster_destroy(store);
  return 0;
}
