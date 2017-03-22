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
int sys_getpid(int32_t *pid) {
	spinlock_acquire(&curthread->t_proc->p_lock);
	*pid = curthread->t_proc->pid;
	spinlock_release(&curthread->t_proc->p_lock);

 	return(0);
}

int sys_fork(struct trapframe *tf, int32_t *retpid) {
	int result;

	/* copy trapframe for child */
	struct trapframe *child_tf = kmalloc(sizeof(struct trapframe));

	if(child_tf == NULL) {
		return ENOMEM;
	}
	memmove(child_tf, tf, sizeof(struct trapframe));
	/* create new child process */
	struct proc *child_proc = proc_create("[userproc]");

	/* set retpid to be child's pid.
	 * Note this is initialized in proc_create().
	 */
	*retpid = child_proc->pid;

	/* set address space of child to a copy of parent's */
	struct addrspace **addrspace_copy; 
	if(as_copy(proc_getas(), addrspace_copy)) {
		return ENOMEM;
	}
	proc_setas(child_proc, *addrspace_copy);

	// TODO: IF USE PPID, NEED TO GETPID AND SET CHILD_PROC'S PPID TO IT OR GET RID OF PPID

	/* initialize exit status of child and exit node for parent
	 * this is how child/parent will communicate about exit statuses
	 */
	struct *exit_node ex_node;
	if((ex_node = kmalloc(sizeof(struct exit_node))) == NULL) {
		return ENOMEM;
	}
	/* note: init_exitmode also sets child's exit status to
	 * the status it sets up in ex_node
	 */
	if(result = init_exitnode(ex_node, child_proc)) {
		return result;
	}
	/* add initialized exit node to parent's child exit nodes */
	spinlock_acquire(&curproc->p_lock);
	struct *exit_node cur_node;
	cur_node = curproc->child_exitnodes;
	/* currently no children -> no exit_nodes */
	if(!cur_node) {
		curproc->child_exitnodes = ex_node;
	} else {
		/* iterate until end of nodes */
		while(cur_node->next) {
			cur_node = cur_node->next;
		}
		cur_node->next = ex_node;
	}	
	spinlock_release(&curproc->p_lock);

	/* Fork the child process into a new thread */	
	if(result = thread_fork(curthread->t_name, child_proc,
				enter_forked_process, child_tf, NULL)) {
		return result;
	}	
  	return 0;
}
