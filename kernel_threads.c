#include <stdlib.h>

#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

PTCB* create_ptcb(PCB* pcb, Task task, int argl, void* args)
{
    PTCB* ptcb = malloc(sizeof(*ptcb));

    if (ptcb == NULL)
        return NULL;

    ptcb->owner_pcb = pcb;
    ptcb->tcb = NULL;

    ptcb->task = task;
    ptcb->argl = argl;
    ptcb->args = args;

    ptcb->exitval = 0;
    ptcb->exited = 0;
    ptcb->detached = 0;
    ptcb->join_waiters = 0;

    ptcb->exit_cv = COND_INIT;

    ptcb->tid = NOTHREAD;
    ptcb->joined = 0;

    rlnode_init(&ptcb->thread_node, ptcb);
    rlist_push_back(&pcb->threads_list, &ptcb->thread_node);

    pcb->live_threads++;

    return ptcb;
}

static void start_process_thread(void)
{
    PTCB* ptcb = cur_thread()->ptcb;

    int exitval = ptcb->task(ptcb->argl, ptcb->args);

    ThreadExit(exitval);
}

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
	  if (task == NULL)
      return NOTHREAD;

    PCB* pcb = CURPROC;

    PTCB* ptcb = create_ptcb(pcb, task, argl, args);

    if (ptcb == NULL)
        return NOTHREAD;

    TCB* tcb = spawn_thread(pcb, start_process_thread);

    ptcb->tcb = tcb;
    tcb->ptcb = ptcb;

    /* The PTCB remains allocated while the thread can still be joined. */
    ptcb->tid = (Tid_t)ptcb;

    Tid_t tid = ptcb->tid;

    wakeup(tcb);

    return tid;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return cur_thread()->ptcb->tid;
}

static PTCB* find_ptcb(PCB* pcb, Tid_t tid)
{
    rlnode* list = &pcb->threads_list;

    for (rlnode* node = list->next;
         node != list;
         node = node->next)
    {
        PTCB* ptcb = node->obj;

        if (ptcb->tid == tid)
            return ptcb;
    }

    return NULL;
}

void release_ptcb(PTCB* ptcb)
{
    rlist_remove(&ptcb->thread_node);
    free(ptcb);
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
    /* A thread cannot join itself or an invalid thread. */
    if (tid == NOTHREAD || tid == cur_thread()->ptcb->tid)
        return -1;

    /* Find the requested thread in the current process. */
    PTCB* ptcb = find_ptcb(CURPROC, tid);

    if (ptcb == NULL || ptcb->detached || ptcb->joined)
        return -1;

    /* Wait until the target thread terminates or is detached. */
    if (!ptcb->exited) {
        ptcb->join_waiters++;
        /* Keep the wait registration visible to process-wide cancellation. */
        cur_thread()->pending_join = ptcb;

        while (!ptcb->exited && !ptcb->detached)
            kernel_wait(&ptcb->exit_cv, SCHED_USER);

        /* Normal return from the wait owns this decrement. */
        cur_thread()->pending_join = NULL;
        ptcb->join_waiters--;
    }

    /* A detached thread cannot be joined. */
    if (ptcb->detached) {
        if (ptcb->exited && ptcb->join_waiters == 0)
            release_ptcb(ptcb);

        return -1;
    }

    /* Return the exit status, if requested. */
    if (exitval != NULL)
        *exitval = ptcb->exitval;

    /* Mark the thread as successfully joined. */
    ptcb->joined = 1;

    /* Keep the PTCB until all existing waiters have resumed. */
    if (ptcb->join_waiters == 0)
        release_ptcb(ptcb);

    return 0;
}

/**
  @brief Detach the given thread.
 */
int sys_ThreadDetach(Tid_t tid)
{
    /* Find the thread within the current process. */
    PTCB* ptcb = find_ptcb(CURPROC, tid);

    /* Invalid or already exited threads cannot be detached. */
    if (ptcb == NULL || ptcb->exited)
        return -1;

    /* Mark the thread as detached. Repeated calls are allowed. */
    ptcb->detached = 1;

    /* Wake up threads waiting in ThreadJoin. */
    kernel_broadcast(&ptcb->exit_cv);

    return 0;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
    PCB* pcb = CURPROC;
    TCB* tcb = cur_thread();
    PTCB* ptcb = tcb->ptcb;

    CHECK_CONDITION(ptcb != NULL);
    CHECK_CONDITION(!ptcb->exited);

    /* A canceled ThreadJoin never returns to decrement join_waiters. */
    if (tcb->pending_join != NULL) {
        PTCB* target = tcb->pending_join;
        tcb->pending_join = NULL;
        CHECK_CONDITION(target->join_waiters > 0);
        target->join_waiters--;
        /* Match the cleanup performed by the regular ThreadJoin path. */
        if (target->exited && target->join_waiters == 0 &&
            (target->detached || target->joined))
            release_ptcb(target);
    }

    /* Release this thread's mutexes before it leaves the process. */
    release_owned_mutexes_on_exit();

    /* Store the exit status. */
    ptcb->exitval = exitval;
    ptcb->exited = 1;

    /* One fewer active process thread. */
    pcb->live_threads--;

    /* Disconnect the TCB before the scheduler frees it. */
    if (pcb->main_thread == tcb)
        pcb->main_thread = NULL;

    ptcb->tcb = NULL;
    tcb->ptcb = NULL;

    /* Wake threads waiting in ThreadJoin. */
    kernel_broadcast(&ptcb->exit_cv);

    /* Detached threads may release their PTCB. */
    if (ptcb->detached && ptcb->join_waiters == 0)
        release_ptcb(ptcb);

    /* The last thread also finishes the process. */
    /* The last thread must also terminate its process. */
    if (pcb->live_threads == 0) {

        sys_Exit(pcb->exit_requested ? pcb->exitval : 0);

        __builtin_unreachable();
    }

    /* Permanently terminate this scheduler thread. */
    kernel_sleep(EXITED, SCHED_USER);

    __builtin_unreachable();
}

