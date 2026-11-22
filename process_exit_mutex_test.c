#include <stddef.h>
#include "unit_testing.h"
static Mutex lock = MUTEX_INIT;
static volatile int holder_has_lock = 0;
static volatile int waiter_started = 0;
static int holder(int argl, void* args)
{
    Mutex_Lock(&lock);
    __atomic_store_n(&holder_has_lock, 1, __ATOMIC_RELEASE);
    for (;;) (void)GetPid();
    return 0;
}
static int waiter(int argl, void* args)
{
    __atomic_store_n(&waiter_started, 1, __ATOMIC_RELEASE);
    Mutex_Lock(&lock);
    Mutex_Unlock(&lock);
    return 0;
}
static int child(int argl, void* args)
{
    Tid_t a = CreateThread(holder, 0, NULL);
    ASSERT(a != NOTHREAD);
    while (!__atomic_load_n(&holder_has_lock, __ATOMIC_ACQUIRE)) (void)GetPid();
    Tid_t b = CreateThread(waiter, 0, NULL);
    ASSERT(b != NOTHREAD);
    while (!__atomic_load_n(&waiter_started, __ATOMIC_ACQUIRE)) (void)GetPid();
    for (int i = 0; i < 40; ++i) (void)GetPid();
    Exit(57);
    return -1;
}
BOOT_TEST(test_exit_while_mutex_held, "Exit while sibling owns a mutex", .timeout = 4)
{
    holder_has_lock = 0;
    waiter_started = 0;
    Pid_t pid = Exec(child, 0, NULL);
    ASSERT(pid != NOPROC);
    int status = -1;
    ASSERT(WaitChild(pid, &status) == pid);
    ASSERT(status == 57);
    return 0;
}
TEST_SUITE(exit_mutex_tests, "Exit mutex cancellation") {
    &test_exit_while_mutex_held, NULL
};
int main(int argc, char** argv)
{
    return register_test(&exit_mutex_tests) || run_program(argc, argv, &exit_mutex_tests);
}
