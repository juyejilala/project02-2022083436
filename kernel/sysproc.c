#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

extern int scheduling_mode; 
extern int new_tick; 
extern struct proc proc[NPROC]; 

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_getppid(void)
{
  return myproc()->parent->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_getlev(void){
  if (scheduling_mode == 0 ) { // FCFS -> 99. 
    return 99; 
    }
    return myproc()->level; // MLFQ -> queue level. 
}

uint64
sys_setpriority(void){
  int pid; 
  int priority_new; 

  argint(0, &pid); 
  argint(1, &priority_new); //pass in. 

  if (priority_new < 0 || priority_new > 3){
    return -2; //if the priority value is not between 0 and 3 
  }

  for (struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock); 
    if (p->pid == pid) {
      p->priority = priority_new; 
      release(&p->lock); 
      return 0; // success 
    }
    release(&p->lock); 
  }
  return -1; // no process with the given pid exists 
}

uint64
sys_mlfqmode(void) {
  if (scheduling_mode == 1) {
    // system is already in MLFQ, print. 
    printf("Error: already in MLFQ mode\n"); 
    return -1; 
  }

  for (struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock); 
    if (p->state == RUNNABLE || p->state == SLEEPING){
      p->level = 0; 
      p->ticks_used = 0; 
      p->priority = 3; 
    }
    release(&p->lock); 
  }

  scheduling_mode = 1; 
  new_tick = 0; 

  return 0; 
}

uint64 
sys_fcfsmode(void){
  if (scheduling_mode == 0){
    // system is already in FCFS, print. 
    printf("Error: already in FCFS mode\n"); 
    return -1; 
    }

    for(struct proc *p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock); 
      if (p->state == RUNNABLE || p->state == SLEEPING){
        p->level = -1; 
        p->ticks_used = -1; 
         p->priority = -1; 
      }
    release(&p->lock); 
  }

  scheduling_mode = 0; 
  new_tick = 0; 

  return 0; 
  
}

uint64
sys_yield(void){
  yield(); 
  return 0; 
}


uint64
sys_clone(void){
    void (*fcn)(void *, void *);
    void *arg1, *arg2, *stack;

    // Fetch the function pointer, arguments, and stack address from user space
    argaddr(0, (uint64*)&fcn); // Fetch the function pointer
    argaddr(1, (uint64*)&arg1);// Fetch arg1
    argaddr(2, (uint64*)&arg2);// Fetch arg2
    argaddr(3, (uint64*)&stack); // Fetch stack address

    return clone(fcn, arg1, arg2, stack);  // Call real clone
}

uint64
sys_join(void) {
  void *stack;
  return join(&stack);
}
