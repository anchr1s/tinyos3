#include <stddef.h>
#include "unit_testing.h"

/* A process thread that keeps running until its process is terminated. */
static volatile int worker_running = 0;

static int persistent_worker(int argl, void* args)
{
    __atomic_store_n(&worker_running, 1, __ATOMIC_RELEASE);

    for (;;) {
        /* Repeated system calls give the kernel opportunities to schedule. */
        (void)GetPid();
    }
    return -1;
}

static int child_process(int argl, void* args)
{
    Tid_t tid = CreateThread(persistent_worker, 0, NULL);
    ASSERT(tid != NOTHREAD);
    if (tid == NOTHREAD)
        return -1;

    /* Ensure the sibling really started before requesting process exit. */
    while (!__atomic_load_n(&worker_running, __ATOMIC_ACQUIRE))
        (void)GetPid();

    Exit(42);
    FAIL("Exit returned unexpectedly");
    return -1;
}

BOOT_TEST(test_exit_terminates_running_sibling,
          "Exit must terminate all threads in its process",
          .timeout = 6)
{
    worker_running = 0;
    Pid_t pid = Exec(child_process, 0, NULL);
    ASSERT(pid != NOPROC);
    if (pid == NOPROC)
        return -1;

    int status = -1;
    ASSERT(WaitChild(pid, &status) == pid);
    ASSERT_MSG(status == 42, "Expected exit status 42, got %d", status);
    return 0;
}

TEST_SUITE(exit_all_threads_tests, "Process-wide Exit regression tests")
{
    &test_exit_terminates_running_sibling,
    NULL
};

int main(int argc, char** argv)
{
    return register_test(&exit_all_threads_tests) ||
           run_program(argc, argv, &exit_all_threads_tests);
}
