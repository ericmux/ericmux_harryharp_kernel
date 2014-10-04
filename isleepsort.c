/* itest1.c

   Spawn two single thread.
*/

#include "minithread.h"

#include <stdio.h>
#include <stdlib.h>


static int a[10] = {7,4,9,0,1,3,2,5,6,8}; 

int
sleepsort(int* arg) {
  minithread_sleep_with_timout(1000*(*arg));
  printf("%d\n", *arg);
  return 0;
}

int initialize_threads(int *arg) {

  int i;
  for(i = 0; i < 10; i++){
	   minithread_fork(sleepsort, &a[i]);
   }

	return 0;
}

int
main(void) {
  minithread_system_initialize(initialize_threads, NULL);
  return 0;
}