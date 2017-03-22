/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <kern/limits.h>
#include <kern/wait.h>
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc* kproc;
struct pid_list_node* pid_list = NULL;

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	int error;
	if(error = new_pid(&proc))
		return error;
	return proc;
}
/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}
	
	int error;
	if(error = remove_pid(proc->pid))
		return error;

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	if(pid_list_init()) {
		panic("pid_list initialization failed\n");
	}
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}
/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);

	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}
/* 
 * Change the address space of the passed process. Otherwise identical to other 
 * proc_setas().
 */
struct addrspace *
proc_setas(struct proc *proc, struct addrspace *newas)
{
	struct addrspace *oldas;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}
/* called in proc_bootstrap to set up pid list before kproc created */
int pid_list_init(void) {
	/* create list and pid = __PID_MIN-1
	 * (the first process created is the kernel process, and limits.h defines
	 * __PID_MIN to correspond to the first user process)
	 */
	pid_list = kmalloc(sizeof(struct pid_list));
	if(pid_list == NULL || pid_list->head == NULL) {
		return ENOMEM;
	}
	spinlock_init(&pid_list->pl_lock);
	//if list was NULL and could not alloc, must be out of memory
	return 0;
}
/* generates a new pid, adds to pid list, associates with passed process */
int new_pid(struct proc *process) {
	spinlock_acquire(&pid_list->pl_lock);
	// if this is the first process, initialize 
	if(pid_list->knode == NULL) {
		struct pid_list_node *kn;
		pid_list->knode = kmalloc(sizeof(struct pid_list_node));
		kn = pid_list->knode;
		if(kn == NULL) {
			spinlock_release(&pid_list->pl_lock);
			return ENOMEM;
		}
		kn->next = NULL;
		kn->pid = __PID_MIN-1;
		// associate process and pid
		kn->proc = process;
		process->pid = __PID_MIN-1;
	} else {
		struct pid_list_node* prev =  pid_list->knode;
		struct pid_list_node* cur = pid_list->knode->next;
		int next_expected_pid = pid_list->knode->pid + 1;
		while(cur != NULL && cur->pid == next_expected_pid) {
			prev = prev->next;
			cur = cur->next;
			next_expected_pid++;
		}
		//When we get here, prev == the highest numbered process before a gap is reached
		if(prev->pid == __PID_MAX) {
			//error for proc table being full
			spinlock_release(&pid_list->pl_lock);
			return ENPROC;
		} 
		cur = kmalloc(sizeof(struct pid_list_node));
		//if n was NULL and could not alloc, must be out of memory
		if(cur == NULL) {
			spinlock_release(&pid_list->pl_lock);
			return ENOMEM; 
		}
		cur->pid = prev->pid +1;
		cur->next = prev->next;
		prev->next = cur;
		// associate process and pid
		cur->proc = process;
		process->pid = cur->pid;	
	}
	spinlock_release(&pid_list->pl_lock);
	return 0;
}

// returns 0 if successful, error if not
int remove_pid(pid_t p) {
	spinlock_acquire(&pid_list->pl_lock);
	if(p < __PID_MIN || p > __PID_MAX || pid_list->knode == NULL) {
		panic("Tried to remove an invalid PID");
	}
	struct pid_list_node* prev = pid_list->knode;
	struct pid_list_node* cur = pid_list->knode->next;
	while(cur != NULL && cur->pid != p) {
		prev = cur;
		cur = cur->next;
	}
	if(cur==NULL) {
		//pid was not in list
		return EINVAL;
	}
	prev->next = cur->next;
	cur->next = NULL;
	kfree(cur);
	spinlock_release(&pid_list->pl_lock);
	return 0;
}

/* initializes fields of allocated exit_node and set child exit_status* */
int init_exitnode(struct *exit_node en, struct proc *child) {
	/* set exit node pid to be child's pid */
	KASSERT(child->pid != NULL);

	en->pid = child->pid;

	/* initialize semaphore of exit status */
	if((en->es.exit_sem = sem_create("exitsem", 0)) == NULL) {
		return ENOMEM;
	}
	/* initialize spinlock of exit status's code */
	if((en->es.code_lock = kmalloc(sizeof(struct spinlock))) == NULL) {
		return ENOMEM;
	}
	spinlock_init(en->es.code_lock);
	/* indicate exitcode is unset */
	en->es.exitcode = EUNSET;
	
	en->next = NULL;
	
	child->exitstatus_ptr = &en->es;

	return 0;
}

/* destroys resources in exit_node */
int destroy_exitnode(struct *exit_node) {
	KASSERT(exit_node != NULL);

	sem_destroy(en->es.exit_sem);
	
	spinlock_cleanup(en->es.code_lock);
	kfree(en->es.code_lock);
	return 0;
}

