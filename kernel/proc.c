#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

struct spinlock memlock;

int nextpid = 1;
struct spinlock pid_lock;

// set the mode & tick as global variable 
int scheduling_mode = 0; // 0 -> FCFS, 1 -> MLFQ
int new_tick = 0; 



extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// priority, reset all processes. 
void boost_priority_all(void) {
  struct proc *p; 
  for (p=proc; p<&proc[NPROC]; p++){
    acquire(&p->lock); 
    if (p->state == RUNNABLE || p->state == RUNNING || p->state == SLEEPING){
      p->level = 0; 
      p->ticks_used = 0; 
      p->priority = 3; 
    } 
    release(&p->lock); 
  } 
}


// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  //initlock(&memlock, "memlock");
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}



// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock); //lock 잡기
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock); //lock 놓기
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;


  // MLFQ field initialization: 
  p->level = 0; // from the highest priority
  p->ticks_used = 0; 
  p->priority = 3; 


  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{

  if (!holding(&p->lock)) {
    printf("BUG: freeproc called without holding lock on pid %d\n", p->pid);
    panic("freeproc lock");
  }

  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;

  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;

  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;

  //release(&p->lock);
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}


// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  acquire(&memlock);

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      release (&memlock);
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }

  for (struct proc *t = proc; t < &proc[NPROC]; t++) {
    if (t->pagetable == p->pagetable) {
      t->sz = sz;
    }
  }

  release(&memlock);
  return 0;
}


//clone: 
int
clone(void (*fcn)(void*, void*), void *arg1, void *arg2, void *stack){
  int i;

  // 1.) allocate a new process struct proc 
  struct proc *np;
  struct proc *p = myproc();

  if ((np = allocproc()) == 0)
    return -1;
  
  //acquire(&np->lock);


  // 2.) Share address space 
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock); 
    return -1;
  }
  // not copying parent's address space 
  // but pointing to the same pagetable 
  
  np->sz = p->sz; // share memory size 
  np->user_stack = stack; //save stack for join() function. 


  // 3.) Copy trapframe and set context 
  *(np->trapframe) = *(p->trapframe); //copy parent's trapframe 
  np->trapframe->epc = (uint64)fcn; // start address 
  np->trapframe->sp = (uint64)stack + PGSIZE; // set to the top of user stack 
  np->trapframe->a0 = (uint64)arg1; 
  np->trapframe->a1 = (uint64)arg2;


  // 4.) copy file descriptor 
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i]){
      np->ofile[i] = filedup(p->ofile[i]); //filedup: duplicated file descriptor 
    }
  np->cwd = idup(p->cwd); 

  // Set state to RUNNABLE
  np->state = RUNNABLE;
  release(&np->lock);

  safestrcpy(np->name, p->name, sizeof(p->name));
  np->parent = p;


  // return new thread's pid 
  return np->pid;
}



// join: 
int join (void **stack){
  struct proc *p; 
  int children_, pid; 
  struct proc *curproc = myproc();

  // ensuring synchronized acceess to process table 
  acquire(&wait_lock); // join() and exit() may cause race condition. 

   for(;;){
    children_=0; // will end the loop when zombie is found, no child thread, or current process is killed. 

    // process table loop 
    for (p = proc; p < &proc[NPROC]; p++){ // linear search through proc[NPROC] array 
      // skip unrelated processes 
     if (p->parent != curproc)
        continue;

      acquire(&p->lock); 
      children_ = 1; 

      // looking for zombie thread, finished and called exit() 
      if (p->state == ZOMBIE){ 
        // copy thread's user stack pointer to caller 
        pid = p->pid; 
        //*stack = p->user_stack;
        uint64 stackaddr = (uint64)p->user_stack;

        freeproc(p); 
        release(&p->lock); 
        release(&wait_lock); 

        if (copyout(curproc->pagetable,
                    (uint64)stack,
                    (char *)&stackaddr,
                    sizeof(stackaddr)) < 0)
          return -1;
        
        return pid; 
      } 
        release(&p->lock);
    }

    //printf("[join] Found ZOMBIE pid %d, returning\n", pid);


    // if there are children but no zombie -> sleep. 
    if(!children_ || killed(curproc)){
      release(&wait_lock);
      return -1; 
    }

    sleep(curproc, &wait_lock); 

   }
}





// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  release(&wait_lock);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  //release(&p->lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;

    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);
        havekids = 1;

        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.

// FCFS, 
void
scheduler(void)
{
  // struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0; 
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    // This is FCFS scheduler. 
    if (scheduling_mode == 0)  {

      // a loop to find the earliest creation time out of all RUNNABLE process, 
      // instead of the first creation time. 

      struct proc *p; 
      struct proc *earliest = 0; // earliest creation time 

      //int found = 0;
      for(p = proc; p < &proc[NPROC]; p++) {
       acquire(&p->lock);

       if(p->state == RUNNABLE) {
         if (earliest == 0 || p->pid < earliest->pid) {

           if(earliest) release(&earliest->lock); 
            earliest = p; 
          } else {
            release(&p->lock); 
          }

        } else {
         release(&p->lock); 
        }
     }

      if(earliest) {
       earliest->state = RUNNING; 
       c->proc = earliest; 
       swtch(&c->context, &earliest->context); 
       c->proc = 0; 
       release(&earliest->lock); 
      } else {
       // no runnable process instead
       intr_on(); 
       asm volatile("wfi"); 
      }

    } else {
      // MLFQ scheduler. 
      struct proc *p; 
      struct proc *selected = 0; 

      // L0 and L1 Round Robin 
      // Loops over all processes, and 
      // select the first RUNNABLE proc in L0 -> L1 

      for (int level = 0; level < 2; level++){
        for (p=proc; p<&proc[NPROC]; p++){
          acquire(&p->lock); 
          if (p->state == RUNNABLE && p->level == level){
            selected = p; 
            break; 
          } else { 
            release(&p->lock); 
          }
        }
        if (selected) break; // process is selected, break. 
      }

      // L2, priority scheduling. 
      if (!selected) {
        int priority = -1; 

        for (p=proc; p<&proc[NPROC]; p++){
          acquire(&p->lock); 
          if(p->state == RUNNABLE && p->level == 2){
            if (selected == 0 || p->priority > priority) { // priority 
              if (selected) {
                release(&selected->lock); // release previously held lock. 
              } 
              selected = p; 
              priority = p->priority; 
            } else {
              release(&p->lock); 
            }
          } else {
            release(&p->lock); 
          }
        }
      }

      if (selected){
        selected->state = RUNNING; 
        c->proc = selected; 

        swtch (&c->context, &selected->context); 

        c->proc = 0; 
        release(&selected->lock); 
      } else {
        // No RUNNABLE process 
        intr_on(); // enable interrupt. 
        asm volatile("wfi"); // wait for the next interrupt. 
      }
    }
    
  }

}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  //int found = 0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      // Kill all threads sharing the same address space
      pagetable_t target_pagetable = p->pagetable;
      release(&p->lock);

      for (struct proc *q = proc; q < &proc[NPROC]; q++) {
        acquire(&q->lock);
        if (q->pagetable == target_pagetable) {
          q->killed = 1;
          if (q->state == SLEEPING)
            q->state = RUNNABLE;
        }
        release(&q->lock);
      }

      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
