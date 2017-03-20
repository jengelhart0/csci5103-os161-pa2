#include <types.h>

#include <syscall.h>    // trapframe definition
#include <copyinout.h>  //has methods for copying data, does error checks unlike memcpy
#include <kern/errno.h> //has macros for errors, definitely needed for waitpid()
#include <process_syscalls.h> 
#include <proc.h> 
#include <current.h> // to get curthread
#include <kern/limits.h> //to get the __PID_MIN and __PID_MAX macros
#include <thread.h> //to get definition of thread struct

//defined in machine-dependent types.h, evals to signed 32-bit int for MIPS

//In Linux getpid is always successful, so errno should be set to 0 in syscall.c
//Returns the pid of the current thread's parent process
pid_t sys_getpid(void) {
  return curthread->t_proc->pid;
}

pid_t sys_fork(struct trapframe* tf, int32_t* retval) {
  //arguments only here for now to prevent warning
  (void)*retval;
  //struct thread* child_thread;

  struct trapframe* child_tf = kmalloc(sizeof(struct trapframe));
  if(child_tf == NULL) {
    *retval = -1;
    return ENOMEM;
  }
  *child_tf = *tf; 
  //TODO: As far as I can tell we *have no* file system...so I don't
  // know if there's no address space needed or what exactly

  /*TODO: finish all of this...I honestly can't tell whether from here in 
   this should do everything by hand and then call thread_fork(); or call 
   enter_forked_process() and then have that call mips_usermode() 
   (which is in arch/mips/locore/trap.c and mentions doing this). 
   I think either would be fine...and I don't know how either would ultimately work

   Either way, have to make sure the return value is consistant with posix/the man pages, 
   but *retval is set to be the child pid for the parent, and 0 within the child.
  */ 

  return 0;
}
