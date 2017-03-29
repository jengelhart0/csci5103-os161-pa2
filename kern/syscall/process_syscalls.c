#include <types.h>
#include <syscall.h>    // trapframe definition
#include <copyinout.h>  //has methods for copying data, does error checks unlike memcpy
#include <mips/trapframe.h>
#include <kern/errno.h> //has macros for errors, definitely needed for waitpid()
#include <proc.h> 
#include <current.h> // to get curthread
#include <kern/limits.h> //to get the __PID_MIN and __PID_MAX macros
#include <lib.h>
//defined in machine-dependent types.h, evals to signed 32-bit int for MIPS

//In Linux getpid is always successful, so errno should be set to 0 in syscall.c
//Returns the pid of the current thread's parent process
int sys_getpid(int32_t *pid) {
	spinlock_acquire(&curthread->t_proc->p_lock);
	*pid = curthread->t_proc->pid;
	spinlock_release(&curthread->t_proc->p_lock);

 	return(0);
}

int sys_waitpid(pid_t pid, int *status, int options, pid_t *retpid) {
	if(status == NULL) {
		return EFAULT;
	}
	if(options) {
		return EINVAL;
	}
	int *err;
	struct proc *child_proc;
	/* get exit code and store in status location */
	if((*status = get_exit_code(pid, err, &child_proc)) < 0) {
		return *err;
	}
	*retpid = pid;
	/* 
	 * Remove mailbox corresponding to child with pid. Note
         * this does not destroy the esn itself: proc_destroy does that
	 */
	struct esn_mailbox *cur;
	struct esn_mailbox *prev;
	/*
	 * Need the current proc's lock in case another thread is altering
	 * the mailbox node chain.
	 */
	spinlock_acquire(&curthread->t_proc->p_lock);
	cur = curthread->t_proc->child_esn_mailbox;
	/* Here and below, if a matching child_esn_mailbox node not found,
	 * this pid must not be a child
	 */
	if(!cur) {
		spinlock_release(&curthread->t_proc->p_lock);
		return ECHILD;
	}
	if(cur->pid == pid) {
		curthread->t_proc->child_esn_mailbox = cur->next;
		kfree(cur);
	} else {
		prev = cur;
		cur = cur->next;
		while(cur && cur->pid != pid) {
			prev = cur;
			cur = cur->next;
		}
		if(!cur) {
			spinlock_release(&curthread->t_proc->p_lock);
			return ECHILD;
		}
		prev->next = cur->next;
		kfree(cur);
	}
	spinlock_release(&curthread->t_proc->p_lock);
	proc_destroy(child_proc);
	return 0;
}

void sys__exit(int exitcode) {
	struct proc *proc = curthread->t_proc;
	spinlock_acquire(&proc->p_es_needed.esn_lock);

	if(!(proc->p_es_needed.needed)) {
		spinlock_release(&proc->p_es_needed.esn_lock);
		proc_remthread(curthread);
		proc_destroy(proc);
	else {
		spinlock_release(&proc->p_es_needed.esn_lock);
		struct exit_status *es = &proc->p_exit_status;
		es->exitcode = exitcode;
		V(&es->exit_sem);
	}
	thread_exit();	
}

int sys_fork(struct trapframe *tf, int32_t *retpid) {
	int result;

	struct proc *child_proc;
	/* create new child process */
	if((child_proc = proc_create_fork("[userproc]")) == NULL) {
		return ENPROC;
	}
	
	*retpid = child_proc->pid;

	/* add new mailbox and pointer to child p_es_needed */
	struct esn_mailbox *cur_mailbox;
	struct esn_mailbox *prev_mailbox = NULL;
	spinlock_acquire(&curthread->t_proc->p_lock);
	cur_mailbox = curthread->t_proc->child_esn_mailbox;
	
	while(cur_mailbox) {
		prev_mailbox = cur_mailbox;
		cur_mailbox = cur_mailbox->next_mailbox;
	}		
	cur_mailbox = kmalloc(sizeof(esn_mailbox));
	if(cur_mailbox == NULL) {
		spinlock_release(&curthread->t_proc->p_lock);
		kfree(child_proc);
		return ENOMEM;
	}
	cur_mailbox->child_pid = child_proc->pid;
	/* p_es_needed initialized to 1 in proc_create() */
	cur_mailbox->child_esn = &child_proc->p_es_needed;
	cur_mailbox->next_mailbox = NULL;	
	/* case when there was existing mailbox at begin of this func execution */
	if(prev_mailbox) {
		prev_mailbox->next = cur_mailbox;
	}	

	spinlock_release(&curthread->t_proc->p_lock);
		
	/* Copy tf to newly allocated tf to pass child. Needed to avoid
	 * corrupting child if parent gets through exception_return
	 * before child gets through enter_forked_process 
	 */
	struct trapframe *copytf = kmalloc(sizeof(struct trapframe));
	if(copytf == NULL) {
		return ENOMEM;
	}
	memmove(copytf, tf, sizeof(struct trapframe));

	/* Fork the child process into a new thread */	
	if((result = thread_fork(curthread->t_name, child_proc,
				enter_forked_process, (void *) copytf, 0))) {

		return result;
	}	
  	return 0;
}

int sys_printchar(const char *arg) {
	kprintf(arg);
  	return 0;
}

