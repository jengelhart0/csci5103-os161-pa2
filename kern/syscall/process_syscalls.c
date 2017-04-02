#include <types.h>
#include <syscall.h>    // trapframe definition
#include <copyinout.h>  //has methods for copying data, does error checks unlike memcpy
#include <addrspace.h>
#include <mips/trapframe.h>
#include <kern/errno.h> //has macros for errors, definitely needed for waitpid()
#include <proc.h> 
#include <current.h> // to get curthread
#include <kern/limits.h> //to get the __PID_MIN and __PID_MAX macros
#include <limits.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <vm.h>
#include <vfs.h>
#include <uio.h>

//defined in machine-dependent types.h, evals to signed 32-bit int for MIPS

//In Linux getpid is always successful, so errno should be set to 0 in syscall.c
//Returns the pid of the current thread's parent process
int sys_getpid(int32_t *pid) {
	spinlock_acquire(&curthread->t_proc->p_lock);
	*pid = curthread->t_proc->pid;
	spinlock_release(&curthread->t_proc->p_lock);

 	return(0);
}

int sys_waitpid(pid_t pid, userptr_t status, int options, pid_t *retpid) {
	if(status == NULL) {
		return EFAULT;
	}
	if(options) {
		return EINVAL;
	}
	int err;
	struct proc *child_proc;
	/* get exit code and store in status location; destroy child */
	if((err = get_exit_code(pid, status, &child_proc)) < 0) {
		return err;
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
	if(cur->child_pid == pid) {
		curthread->t_proc->child_esn_mailbox = cur->next_mailbox;
		kfree(cur);
	} else {
		prev = cur;
		cur = cur->next_mailbox;
		while(cur && cur->child_pid != pid) {
			prev = cur;
			cur = cur->next_mailbox;
		}
		if(!cur) {
			spinlock_release(&curthread->t_proc->p_lock);
			return ECHILD;
		}
		prev->next_mailbox = cur->next_mailbox;
		kfree(cur);
	}

	spinlock_release(&curthread->t_proc->p_lock);
	proc_destroy(child_proc);
	return 0;
}

void sys__exit(int exitcode) {
	struct proc *proc = curthread->t_proc;
	/*
	 * Set all child exit_status_needed's to 0 and free mailbox chain.
	 * Child has continued access to needed while letting this process's
	 * mailbox chain be freed.
	 *
	 * Setting children needed flag to 0 is necessary here because parent
	 * might not be proc_destroy()ed before child sys__exit()s and checks
	 * its needed flag. Therefore, it must happen on parent exit. Freeing
	 * the children mailboxes here makes sense because they are only needed
	 * for the purpose of communicating to children that their exit status
	 * is no longer needed.
	 */

	struct esn_mailbox *cur;	
	struct esn_mailbox *prev;
	/* cur sets exit_status_needed pointed to by current mailbox.
	 * prev frees previous mailbox.
	 */

	cur = proc->child_esn_mailbox;
	if(cur) {
		spinlock_acquire(&cur->child_esn->esn_lock);
		cur->child_esn->needed = 0;		
		spinlock_release(&cur->child_esn->esn_lock);

		prev = cur;
		cur = cur->next_mailbox;
		kfree(prev);

		while(cur) {
			spinlock_acquire(&cur->child_esn->esn_lock);
			cur->child_esn->needed = 0;		
			spinlock_release(&cur->child_esn->esn_lock);
			
			prev = cur; 
			cur = cur->next_mailbox;
			kfree(prev);
		}
		proc->child_esn_mailbox = NULL;
	}

	spinlock_acquire(&proc->p_es_needed.esn_lock);
	if(!(proc->p_es_needed.needed)) {
		spinlock_release(&proc->p_es_needed.esn_lock);
		proc_remthread(curthread);
		proc_destroy(proc);
	} else {
		spinlock_release(&proc->p_es_needed.esn_lock);
		struct exit_status *es = &proc->p_exit_status;
		es->exitcode = exitcode;
		/* Ideally, we'd like this to happen in thread_exit, but
		 * we need to ensure this occurs before a parent is signalled.
		 * We don't want to context switch to the parent, who tries to
		 * destroy us before our thread gets removed
		 */
		proc_remthread(curthread);

		V(es->exit_sem);
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
	cur_mailbox = kmalloc(sizeof(struct esn_mailbox));
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
		prev_mailbox->next_mailbox = cur_mailbox;
	} else {
		curthread->t_proc->child_esn_mailbox = cur_mailbox;	
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

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int
sys_execv(char *progname, char **argv) {

	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result = 0;
	
	if(!(progname && argv)) {
		return EFAULT;
	}

	char *argv_buf;
	char **argvptr_buf;
	argvptr_buf = kmalloc(sizeof(char*) * NUM_MAXARGS);
	if(!argvptr_buf) {
		return result;
	}
	argv_buf = kmalloc(sizeof(ARG_MAX));
	if(!argv_buf) {
		return result;
	}

	int num_args = 0;
	result = copyin((const_userptr_t)argv,
			(void *) argvptr_buf, sizeof(userptr_t));
	if(result) {
		return result;
	}
	while(argvptr_buf[num_args]) {
		num_args++;
		result = copyin((const_userptr_t) (argv + num_args), 
				(void *) (argvptr_buf + num_args), 
				sizeof(userptr_t));
		if(result) {
			return result;
		}					
	}
	/* Allocate space to track str lengths (they will all be in one array later) */
	int *strlens = kmalloc(sizeof(int) * num_args);
	if(!strlens) {
		return ENOMEM;
	}

	size_t actual;
	int i;
	int bytescopied = 0;
	for(i = 0; i < num_args; i++) {
	
		result = copyinstr((const_userptr_t)argvptr_buf[i], 
				   (void *) (argv_buf + bytescopied), 
				   ARG_MAX, &actual);
		if(result) {
			return result;
		}
		bytescopied += actual;
		if(bytescopied > ARG_MAX) {
			return E2BIG;
		}
		strlens[i] = actual;
	}

	char *kprogname;
	kprogname = (char *) kmalloc(sizeof(PATH_MAX));
	if(!kprogname) {
		return ENOMEM;
	}
	if((result = copyinstr((const_userptr_t)progname, 
				kprogname, PATH_MAX, NULL))) {
		return result;
	}
	
	/* Open the file. */
	result = vfs_open(kprogname, O_RDONLY, 0, &v);
	if (result) {
		return result;

	}
	/* This seems appropriate but revisit if as problems */
	as_destroy(proc_getas());
	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	/* Copy out arg strings to new user stack */
	int bytesrem = bytescopied;
	for(i = num_args - 1; i >= 0; i--) {
		DEBUGASSERT(bytesrem >= 0);
		stackptr -= strlens[i];	
		result = copyoutstr((void *) &argv_buf[bytesrem - strlens[i]],
				 (userptr_t) stackptr, strlens[i], NULL);
		if(result) {
			return result;
		}
		bytesrem -= strlens[i];
		argvptr_buf[i] = (char *) stackptr;
	}
	DEBUGASSERT(bytesrem == 0);
	/* Create padding to maintain alignment needed for stackptr. */
	int totalbytes = bytescopied + sizeof(char *) * num_args;
	int overrun; 
	if((overrun = totalbytes % ALIGN_SIZE)) {
		totalbytes += (ALIGN_SIZE - overrun); 
	}
	stackptr -= totalbytes - bytescopied; 
	
	/* Make room on user stack and copyout argptrs to user address space */
	result = copyout((void *) argvptr_buf, 
			 (userptr_t) stackptr, sizeof(char *) * num_args);
	if(result) {
		return result;
	}
	
	kfree(strlens);
	kfree(argv_buf);
	kfree(argvptr_buf);

	/* Warp to user mode. */
	enter_new_process(num_args /*argc*/, (userptr_t) stackptr /*userspace 
			  addr of argv*/, NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);


	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

int sys_printchar(const char *arg) {
	kprintf(arg);
  	return 0;
}

