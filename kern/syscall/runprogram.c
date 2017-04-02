/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
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
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than runprogram() does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <limits.h>
#include <copyinout.h>

/*
 * Runprogram with args
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int
runprogram_args(char *progname, int num_args, char **argv)
{

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

	memmove((void *) argvptr_buf, (void *)argv, sizeof(char *));
	int i;
	for(i = 0; i <= num_args; i++) {
		memmove((void *) (argvptr_buf + i),
			   	 (void *) (argv + i), 
				 sizeof(char *));
	}
	/* Allocate space to track str lengths (they will all be in one array later) */
	int *strlens = kmalloc(sizeof(int) * num_args);
	if(!strlens) {
		return ENOMEM;
	}

	size_t actual;
	int bytescopied = 0;
	for(i = 0; i < num_args; i++) {
		strcpy((char *) (argv_buf + bytescopied),
					 (char *)argvptr_buf[i]);
		actual = strlen(argv_buf + bytescopied) + 1;
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
	strcpy(kprogname, progname);
	
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

	/* Load the executable */
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
			 (userptr_t) stackptr, 
			 sizeof(char *) * num_args);
	if(result) {
		return result;
	}
		
	/* Warp to user mode. */
	enter_new_process(num_args /*argc*/, (userptr_t) stackptr /*userspace 
			  addr of argv*/, NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);


	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;

}

int
runprogram(char *progname, int num_args, char **argv)
{

	int result;
	if(num_args > 1) {
		result = runprogram_args(progname, num_args, argv);
		return result;
	}
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

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

	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

