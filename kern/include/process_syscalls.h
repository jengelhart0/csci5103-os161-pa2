#ifndef PROCESS_SYSCALLS_H_
#define PROCESS_SYSCALLS_H_

#include <types.h>

pid_t sys_getpid(void);
//not implemented yet, these are the signatures from userland/include/unistd.h 
pid_t sys_fork(void);
// pid_t waitpid(pid_t pid, int *returncode, int flags);
// int execv(const char *prog, char *const *args);
//__DEAD void sys_exit(int code);

#endif
