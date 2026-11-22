#include <stddef.h>
#include "unit_testing.h"

/* First explicit process Exit must keep its value. */
static int first_exit_worker(int argl, void* args)
{
    Exit(41);
    return -1;
}

static int test_child(int argl, void* args)
{
    Tid_t tid = CreateThread(first_exit_worker, 0, NULL);
    ASSERT(tid != NOTHREAD);
    if (tid == NOTHREAD) return -1;

    int value = -1;
    ASSERT(ThreadJoin(tid, &value) == 0);
    ASSERT(value == 41);

    Exit(99);
    return -1;
}

BOOT_TEST(test_exit_status_preserved,
          "First Exit status must not be overwritten by later Exit")
{
    Pid_t pid = Exec(test_child, 0, NULL);
    ASSERT(pid != NOPROC);
    if (pid == NOPROC) return -1;

    int status = -1;
    ASSERT(WaitChild(pid, &status) == pid);
    ASSERT_MSG(status == 41,
               "Expected first Exit value 41, got %d", status);
    return 0;
}

TEST_SUITE(exit_status_tests, "Process Exit status tests")
{
    &test_exit_status_preserved,
    NULL
};

int main(int argc, char** argv)
{
    return register_test(&exit_status_tests) ||
           run_program(argc, argv, &exit_status_tests);
}
