#include <stddef.h>
#include "unit_testing.h"

/* These tests run without changing any existing kernel sources. */
static Mutex wait_mutex = MUTEX_INIT;
static CondVar wait_cv = COND_INIT;
static volatile int cond_ready = 0;
static volatile int join_ready = 0;
static Tid_t target_tid = NOTHREAD;

/* Wait forever unless process-wide Exit cancels the entire process. */
static int blocked_cond_worker(int argl, void* args)
{
    Mutex_Lock(&wait_mutex);
    __atomic_store_n(&cond_ready, 1, __ATOMIC_RELEASE);
    (void)Cond_Wait(&wait_mutex, &wait_cv);
    Mutex_Unlock(&wait_mutex);
    return 19;
}

static int child_with_cond_waiter(int argl, void* args)
{
    cond_ready = 0;
    Tid_t tid = CreateThread(blocked_cond_worker, 0, NULL);
    ASSERT(tid != NOTHREAD);
    if (tid == NOTHREAD) return -1;

    while (!__atomic_load_n(&cond_ready, __ATOMIC_ACQUIRE))
        (void)GetPid();

    /* Let the waiter finish entering Cond_Wait before calling Exit. */
    for (int i = 0; i < 40; i++) (void)GetPid();

    Exit(42);
    FAIL("Exit returned unexpectedly");
    return -1;
}

BOOT_TEST(test_exit_with_cond_waiter,
          "Exit must complete with a sibling waiting in Cond_Wait",
          .timeout = 6)
{
    Pid_t pid = Exec(child_with_cond_waiter, 0, NULL);
    ASSERT(pid != NOPROC);
    if (pid == NOPROC) return -1;

    int status = -1;
    ASSERT(WaitChild(pid, &status) == pid);
    ASSERT_MSG(status == 42, "Expected process exit status 42, got %d", status);
    return 0;
}

/* This target never exits by itself, so ThreadJoin would wait forever. */
static int never_finishing_worker(int argl, void* args)
{
    for (;;) (void)GetPid();
    return -1;
}

static int blocked_join_worker(int argl, void* args)
{
    __atomic_store_n(&join_ready, 1, __ATOMIC_RELEASE);
    (void)ThreadJoin(target_tid, NULL);
    return 20;
}

static int child_with_join_waiter(int argl, void* args)
{
    join_ready = 0;
    target_tid = CreateThread(never_finishing_worker, 0, NULL);
    ASSERT(target_tid != NOTHREAD);
    if (target_tid == NOTHREAD) return -1;

    Tid_t joiner_tid = CreateThread(blocked_join_worker, 0, NULL);
    ASSERT(joiner_tid != NOTHREAD);
    if (joiner_tid == NOTHREAD) return -1;

    while (!__atomic_load_n(&join_ready, __ATOMIC_ACQUIRE))
        (void)GetPid();

    /* Give the joiner a chance to block in ThreadJoin. */
    for (int i = 0; i < 40; i++) (void)GetPid();

    Exit(43);
    FAIL("Exit returned unexpectedly");
    return -1;
}

BOOT_TEST(test_exit_with_join_waiter,
          "Exit must complete with a sibling waiting in ThreadJoin",
          .timeout = 6)
{
    Pid_t pid = Exec(child_with_join_waiter, 0, NULL);
    ASSERT(pid != NOPROC);
    if (pid == NOPROC) return -1;

    int status = -1;
    ASSERT(WaitChild(pid, &status) == pid);
    ASSERT_MSG(status == 43, "Expected process exit status 43, got %d", status);
    return 0;
}

TEST_SUITE(exit_blocked_tests, "Process-wide Exit with blocked sibling threads")
{
    &test_exit_with_cond_waiter,
    &test_exit_with_join_waiter,
    NULL
};

int main(int argc, char** argv)
{
    return register_test(&exit_blocked_tests) ||
           run_program(argc, argv, &exit_blocked_tests);
}
