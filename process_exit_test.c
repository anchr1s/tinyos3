#include "unit_testing.h"
#include "kernel_sched.h"

/* A sibling that remains alive until process-wide Exit() terminates it. */
static int lingering_thread(int argl, void* args)
{
    (void)argl;
    (void)args;

    while (1)
        yield(SCHED_USER);

    return 0; /* Unreachable unless the scheduler violates the loop. */
}

/* The original process thread exits while its sibling is still alive. */
static int exiting_child(int argl, void* args)
{
    (void)argl;
    (void)args;

    if (CreateThread(lingering_thread, 0, NULL) == NOTHREAD)
        return -1;

    Exit(17);
    return -1; /* Exit() should not return. */
}

BOOT_TEST(test_process_exit_terminates_siblings,
          "Exit terminates all threads in the process",
          .timeout = 3)
{
    int status = -1;
    Pid_t child = Exec(exiting_child, 0, NULL);

    ASSERT(child != NOPROC);
    if (child == NOPROC)
        return -1;

    ASSERT(WaitChild(child, &status) == child);
    ASSERT(status == 17);

    return 0;
}

TEST_SUITE(process_exit_tests,
           "Process-wide Exit tests")
{
    &test_process_exit_terminates_siblings,
    NULL
};

int main(int argc, char** argv)
{
    return register_test(&process_exit_tests) ||
           run_program(argc, argv, &process_exit_tests);
}
