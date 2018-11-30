#include "c_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>


void callback(void* func) {
    printf("hej\n");
}


int main() {
  uint64_t table_size = (1L << 14);
  uint64_t log_size = 17179869184;
  const char* storage_dir = "storage_dir";
  faster_t* store = faster_open_with_disk(table_size, log_size, storage_dir);

  // Upsert
  faster_upsert(store, 1, 1000);
  faster_upsert(store, 2, 1000);
  faster_upsert(store, 3, 1000);
 
  // RMW
  uint8_t rmw = faster_rmw(store, 3, 10);
  assert(rmw == 0);

  // Read
  uint8_t res = faster_read(store, 1);
  uint8_t resTwo = faster_read(store, 2);
  uint8_t resThree = faster_read(store, 3);
  uint8_t resFour = faster_read(store, 4);

  // Status: 0 == Ok, 2 == NotFound
  assert(res == 0);
  assert(resTwo == 0);
  assert(resThree == 0);
  assert(resFour == 2);


  uint64_t size = faster_size(store);
  printf("size: %" PRIu64 "\n", size);

  // Checkpoint
  faster_checkpoint_result* checkpoint_res = faster_checkpoint(store);
  printf("checked %d\n", checkpoint_res->checked);
  printf("token %s\n", checkpoint_res->token);

  char* recover_token = checkpoint_res->token;

  faster_recover_result* recover_res = faster_recover(store, recover_token, recover_token);
  printf("rec %d\n", recover_res->status);
  printf("version: %" PRIu32"\n", recover_res->version);


  free(checkpoint_res->token);
  free(checkpoint_res);
  free(recover_res);

  // Free the resources tied to STORE
  faster_destroy(store);
  return 0;
}
