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
#include <synch.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <kern/limits.h>
#include <kern/wait.h>
#include <copyinout.h>
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc* kproc;
struct pid_list *pid_list = NULL;

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

	/* Exit status/mailbox structure fields */
	if((proc->p_exit_status.exit_sem = sem_create("exitsem", 0)) == NULL) {
		kfree(proc);
		return NULL;
	}

	/* Initialized standardly for consistency (note no real exit status < 0) */
	proc->p_exit_status.exitcode = -1;	

	/* At creation every exit status assumed needed */
	proc->p_es_needed.needed = 1;
	spinlock_init(&proc->p_es_needed.esn_lock);

	/* At creation, process has no children->no exit mailboxes needed */
	proc->child_esn_mailbox = NULL;

	/* PID allocation. PPID set in proc_create_fork() */
	if(new_pid(proc)) {
		sem_destroy(proc->p_exit_status.exit_sem);
		kfree(proc);
		return NULL; 
	}
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
		
	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	/* Exit structure clean-up */
	sem_destroy(proc->p_exit_status.exit_sem);
	spinlock_cleanup(&proc->p_es_needed.esn_lock);
	
	/*
	 * Set all child exit_status_needed's to 0 and free mailbox chain.
	 * Child has continued access to needed while letting this process's
	 * mailbox chain be freed.
	 *
	 * Note: Child can't be proc_destroyed before parent, unless through
	 * parent calling waitpid, during which child's p_es_needed would be
	 * wiped through this function (i.e., proc_destroy), not before. So
	 * we can be sure data at child_esn still exists here.
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
	}

	remove_pid(proc->pid);

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
 * Create a fresh proc for use by fork.
 *
 * It will have a copy of its parent's address space, and will have
 * an exit status set up for the parent/child to communicate through
 * for the purposes of waitpid/_exit.
 */
struct proc *
proc_create_fork(const char *name)
{
	struct proc *child_proc = proc_create(name);

	child_proc->ppid = curthread->t_proc->pid;

	/* set address space of child to a copy of parent's */
	struct addrspace **addrspace_copy;
	if((addrspace_copy = kmalloc(sizeof(struct addrspace *))) == NULL) {
		kfree(child_proc);
		return NULL;
	} 
	if(as_copy(proc_getas(), addrspace_copy)) {
		kfree(child_proc);
		return NULL;
	}
	proc_setas_other(child_proc, *addrspace_copy);
	kfree(addrspace_copy);

	/* Lock parent to set cwd and add exit status node to parent */
	spinlock_acquire(&curproc->p_lock);
	
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		child_proc->p_cwd = curproc->p_cwd;
	}

	spinlock_release(&curproc->p_lock);

	return child_proc;
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
proc_setas_other(struct proc *proc, struct addrspace *newas)
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
	if(pid_list == NULL || pid_list->knode == NULL) {
		return ENOMEM;
	}
	pid_list->size = 0;
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
	pid_list->size++;
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
		spinlock_release(&pid_list->pl_lock);
		return EINVAL;
	}
	prev->next = cur->next;
	cur->next = NULL;
	kfree(cur);
	pid_list->size--;
	spinlock_release(&pid_list->pl_lock);
	return 0;
}

int get_exit_code(pid_t pid, userptr_t status, struct proc **proc) {
	struct pid_list_node *cur;	
	struct exit_status *es;	
	int pid_found = 0;

	spinlock_acquire(&pid_list->pl_lock);
	/* first process/pid will have been initialized by proc_bootstrap */
	cur = pid_list->knode->next;
	/* search list for entry with matching pid */
	while(cur && !pid_found) {
		if(pid == cur->pid) {
			es = &cur->proc->p_exit_status;		
			pid_found = 1;
			/* reference used by waitpid to destroy process */
			*proc = cur->proc;
		} else {
			cur = cur->next;
		}
	}
	spinlock_release(&pid_list->pl_lock);
	if(cur->proc->ppid != curthread->t_proc->pid) {
		return ECHILD;
	}
	if(!pid_found) {
		return ESRCH;
	}
	/* wait on sem until child signals a set exit code */
	P(es->exit_sem);
	
	copyout(&es->exitcode, status, sizeof(int));
	return 0;
}
/*
 * Removes zombie processes. Creates a temporary array of proc * that
 * will be destroyed once all processes have been considered. Necessary
 * because proc_destroy alters the exit needed flag of any child process.
 */
void proc_exorcise(void) {
	spinlock_acquire(&pid_list->pl_lock);

	struct proc *to_destroy[pid_list->size];
	int lastidx = 0;

	struct pid_list_node *cur;
	struct proc *cur_proc;
	/* we assume kernel proc is resident since operating system running */
	cur = pid_list->knode->next;
	while(cur) {
		cur_proc = cur->proc;
		spinlock_acquire(&cur_proc->p_lock);
		if(cur_proc->p_exit_status.exitcode != -1 &&
		   cur_proc->p_es_needed.needed == 0) {
			to_destroy[lastidx++] = cur_proc;		
		} 
		spinlock_release(&cur_proc->p_lock);
		cur = cur->next;
	}
	int i;
	for(i = 0; i < lastidx; i++) {
		proc_destroy(to_destroy[i]);
	}
	spinlock_release(&pid_list->pl_lock);
}
