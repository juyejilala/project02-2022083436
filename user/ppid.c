#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main() {
    printf("My student ID is 2022083436\n");
    printf("My pid is %d\n", getpid());
    printf("My ppid is %d\n", getppid());
    exit(0);
}
