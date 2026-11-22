
#include "tinyos.h"
#include "kernel_sys.h"
#include "kernel_cc.h"
#include "kernel_proc.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

/*
	Define all the syscalls 
 */


/* An exit checkpoint runs only after taking the kernel semaphore.
   cur_thread() may be NULL during early kernel initialization. */
#define PRE_CALL \
kernel_lock();\
if (cur_thread() != NULL && cur_thread()->ptcb != NULL && \
    CURPROC->exit_requested) { \
    sys_ThreadExit(0); \
}



#define POST_CALL \
kernel_unlock();\


/* with return */
#define SYSCALL(NAME, RET, SIG, ARGS)\
RET NAME SIG \
{\
	RET __ret;\
	PRE_CALL\
	__ret = sys_##NAME ARGS;\
	POST_CALL\
	return __ret;\
}\

/* without return */
#define SYSCALLV(NAME, SIG, ARGS)\
void NAME SIG \
{\
	PRE_CALL\
	sys_##NAME ARGS;\
	POST_CALL\
}\


SYSCALLS

