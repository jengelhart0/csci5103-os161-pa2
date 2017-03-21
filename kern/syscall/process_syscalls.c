#include <types.h>

#include <syscall.h>    // trapframe definition
#include <copyinout.h>  //has methods for copying data, does error checks unlike memcpy
#include <kern/errno.h> //has macros for errors, definitely needed for waitpid()
#include <proc.h> 
#include <current.h> // to get curthread
#include <kern/limits.h> //to get the __PID_MIN and __PID_MAX macros
#include <thread.h> //to get definition of thread struct

//defined in machine-dependent types.h, evals to signed 32-bit int for MIPS

//In Linux getpid is always successful, so errno should be set to 0 in syscall.c
//Returns the pid of the current thread's parent process
int sys_getpid(pid_t *pid) {
	spinlock_acquire(&curthread->t_proc->p_lock);
	*pid = curthread->t_proc->pid;
	spinlock_release(&curthread->t_proc->p_lock);

 	return(0);
}

int sys_fork(struct trapframe *tf, pid_t *retpid) {

	int result;
	// copy trapframe for child
	struct trapframe *child_tf = kmalloc(sizeof(struct trapframe));
	// TODO: verify whether p_name is significant here?	
	if(child_tf == NULL) {
		*retval = -1;
		return ENOMEM;
	}
	memmove(child_tf, tf, sizeof(struct trapframe));
	// create new child process
	struct proc *child_proc = proc_create(curthread->t_proc->p_name);
	proc_setas(child_proc, proc_getas());
	
	if(result = thread_fork(curthread->t_name, child_proc,
				enter_forked_process, child_tf, NULL))
		return result;

	// set retpid to be child's pid. Note this is initialized in proc_create().
	sys_getpid(retpid);

  	return 0;
}
