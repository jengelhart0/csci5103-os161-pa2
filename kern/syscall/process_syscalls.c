#include <types.h>

#include <syscall.h>    // trapframe definition
#include <copyinout.h>  //has methods for copying data, does error checks unlike memcpy
#include <kern/errno.h> //has macros for errors, definitely needed for waitpid()
#include <process_syscalls.h> 
#include <proc.h> 
#include <current.h> // to get curthread
#include <kern/limits.h> //to get the __PID_MIN and __PID_MAX macros

//defined in machine-dependent types.h, evals to signed 32-bit int for MIPS

//In Linux getpid is always successful, so errno should be set to 0 in syscall.c
//Returns the pid of the current thread's parent process
pid_t sys_getpid(void) {
  return curthread->t_proc->pid;
}

pid_t sys_fork() {
  kprintf("Forked.\n");
  return 1;
}
