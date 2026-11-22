#include <stddef.h>
#include "unit_testing.h"

/* Two threads must never reap the same terminated child twice. */
static volatile int child_may_exit;
static volatile int waiters_ready;
static Pid_t waited_pid;

typedef struct {
    Pid_t ret;
    int status;
} WaitResult;

static WaitResult results[2];

static int held_child(int argl, void* args)
{
    while (!__atomic_load_n(&child_may_exit, __ATOMIC_ACQUIRE))
        (void)GetPid();
    Exit(67);
    return -1;
}

static int waiting_thread(int argl, void* args)
{
    WaitResult* result = args;
    int status = -500;
    __atomic_add_fetch(&waiters_ready, 1, __ATOMIC_RELEASE);
    result->ret = WaitChild(waited_pid, &status);
    result->status = status;
    return 0;
}

static int empty_child(int argl, void* args)
{
    return 23;
}

BOOT_TEST(test_waitchild_single_reaper,
          "Two concurrent WaitChild calls must reap a child only once",
          .timeout = 8)
{
    __atomic_store_n(&child_may_exit, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&waiters_ready, 0, __ATOMIC_RELEASE);
    results[0].ret = results[1].ret = NOPROC;
    results[0].status = results[1].status = -500;

    waited_pid = Exec(held_child, 0, NULL);
    ASSERT(waited_pid != NOPROC);
    if (waited_pid == NOPROC) return -1;

    Tid_t a = CreateThread(waiting_thread, 0, &results[0]);
    Tid_t b = CreateThread(waiting_thread, 0, &results[1]);
    ASSERT(a != NOTHREAD && b != NOTHREAD);
    if (a == NOTHREAD || b == NOTHREAD) return -1;

    while (__atomic_load_n(&waiters_ready, __ATOMIC_ACQUIRE) < 2)
        (void)GetPid();

    /* Give both waiters a chance to block before the child exits. */
    for (int i = 0; i < 250; ++i) (void)GetPid();
    __atomic_store_n(&child_may_exit, 1, __ATOMIC_RELEASE);

    ASSERT(ThreadJoin(a, NULL) == 0);
    ASSERT(ThreadJoin(b, NULL) == 0);

    int successes = 0, errors = 0;
    for (int i = 0; i < 2; ++i) {
        if (results[i].ret == waited_pid && results[i].status == 67)
            ++successes;
        else if (results[i].ret == NOPROC && results[i].status == -500)
            ++errors;
    }
    ASSERT_MSG(successes == 1 && errors == 1,
               "Expected 1 reaper and 1 error; got results (%d,%d), (%d,%d)",
               results[0].ret, results[0].status,
               results[1].ret, results[1].status);
    if (successes != 1 || errors != 1) return -1;

    /* Reuse a freed PCB and make sure it is still reaped normally. */
    Pid_t next_pid = Exec(empty_child, 0, NULL);
    ASSERT(next_pid != NOPROC);
    if (next_pid == NOPROC) return -1;
    int status = -1;
    ASSERT(WaitChild(next_pid, &status) == next_pid);
    ASSERT(status == 23);
    return 0;
}

TEST_SUITE(waitchild_reaper_tests, "WaitChild single-reaper regression")
{
    &test_waitchild_single_reaper,
    NULL
};

int main(int argc, char** argv)
{
    return register_test(&waitchild_reaper_tests) ||
           run_program(argc, argv, &waitchild_reaper_tests);
}
