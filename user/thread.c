#include "user.h"
#include "thread.h"

//#define MAX_THREADS 64  // bigger than NUM_THREAD
//static int joined[MAX_THREADS] = {0};

int thread_join() {

  void *stack = 0;
  int pid = join(&stack);
  if (pid < 0)
    return -1;
  free(stack);
  return pid;

  /*void *stack = 0;
  int pid;

  while (1) {
    pid = join(&stack);

    if (pid < 0 || stack == 0) {
      return -1;
    }

    // Check if this thread has already been joined
    int already_joined = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
      if (joined[i] == pid) {
        already_joined = 1;
        break;
      }
    }

    if (!already_joined) {
      // Mark as joined
      for (int i = 0; i < MAX_THREADS; i++) {
        if (joined[i] == 0) {
          joined[i] = pid;
          break;
        }
      }

      free(stack);  // Free thread’s stack
      return pid;   
    }

    // If already joined, just loop again for the next
  }*/
}


int thread_create(void (*start_routine)(void *, void *), void *arg1, void *arg2)
{
  void *stack = malloc(4096);  // allocate 1 page for stack

  if (stack == 0)
    return -1;

  // Call the kernel's clone syscall
  int pid = clone(start_routine, arg1, arg2, stack);

  if (pid < 0) {
    free(stack);  // cleanup if clone fails
    return -1;
  }

  return pid;
}

