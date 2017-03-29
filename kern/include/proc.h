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

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <spinlock.h>
#include <synch.h>
#include <types.h>
#include <kern/errno.h>

struct addrspace;
struct thread;
struct vnode;

/* structure for maintaining exit status information */
struct exit_status {
  struct semaphore *exit_sem; // used to wait/signal for waitpid usage
  int exitcode;
};
/* structures for communicating exit status needs between parent/child */
struct exit_status_needed {
  int needed;
  struct spinlock esn_lock;
};

struct esn_mailbox {
  pid_t child_pid;
  struct exit_status_needed *child_esn;
  struct esn_mailbox *next_mailbox;
};

/*
 * Process structure.
 *
 * Note that we only count the number of threads in each process.
 * (And, unless you implement multithreaded user processes, this
 * number will not exceed 1 except in kproc.) If you want to know
 * exactly which threads are in the process, e.g. for debugging, add
 * an array and a sleeplock to protect it. (You can't use a spinlock
 * to protect an array because arrays need to be able to call
 * kmalloc.)
 *
 * You will most likely be adding stuff to this structure, so you may
 * find you need a sleeplock in here for other reasons as well.
 * However, note that p_addrspace must be protected by a spinlock:
 * thread_switch needs to be able to fetch the current address space
 * without sleeping.
 */
struct proc {
  char *p_name;     /* Name of this process */
  struct spinlock p_lock;   /* Lock for this structure */
  unsigned p_numthreads;    /* Number of threads in this process */

  /* VM */
  struct addrspace *p_addrspace;  /* virtual address space */

  /* VFS */
  struct vnode *p_cwd;    /* current working directory */
 
  /* Exit Status */
  struct exit_status p_exit_status;
  struct exit_status_needed p_es_needed;
  struct esn_mailbox *child_esn_mailbox;

  /* PID/PPID */
  pid_t pid, ppid;
};
/* Structures for maintaining list of pids.
 * I believe this is a better solution than using a hash table, given the relatively
 * small amount of memory SYS161 provides. If many processes are running, then there 
 * probably won't be many processes created in quick succession (otherwise we'd run out of memory), 
 * and if many processes have quit, the next new processes should be quickly assigned to a low-numbered
 * pid.
 */
struct pid_list {
  int size;
  struct pid_list_node *knode; // node for kernel process
  struct spinlock pl_lock;
};

struct pid_list_node {
  pid_t pid;
  struct proc *proc;
  struct pid_list_node *next;
};

/* List for tracking pids */
extern struct pid_list* pid_list;

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Create a fresh process for use by fork(). */
struct proc *proc_create_fork(const char *name);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);
struct addrspace *proc_setas_other(struct proc *proc, struct addrspace *newas);


// PID LIST HELPER FUNCTIONS //

/* Called from proc_bootstrap(), before kernel proc initialized */
int pid_list_init(void);

/* Add a new pid to list of all pids */
int new_pid(struct proc *proc);

/* Remove a pid from the list of all pids, returns 0 if successful*/
int remove_pid(pid_t p);

/* Helper for waitpid(). */
int get_exit_code(pid_t pid, userptr_t status, struct proc **proc);

/* Cleans zombie processes */
void proc_exorcise(void);
#endif /* _PROC_H_ */
