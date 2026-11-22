#include <stddef.h>
#include "unit_testing.h"

/* A pure CPU loop does not make syscalls or reach PRE_CALL checkpoints. */
static volatile unsigned long busy_count;
static int cpu_only_worker(int argl, void* args)
{
    for (;;) __atomic_add_fetch(&busy_count, 1, __ATOMIC_RELAXED);
    return 0;
}
static int cpu_only_parent(int argl, void* args)
{
    ASSERT(CreateThread(cpu_only_worker, 0, NULL) != NOTHREAD);
    for (int i = 0; i < 50; ++i) (void)GetPid();
    Exit(63);
    return -1;
}
BOOT_TEST(test_exit_cpu_only, "Exit terminates a CPU-only sibling", .timeout=5)
{
    Pid_t pid = Exec(cpu_only_parent, 0, NULL);
    ASSERT(pid != NOPROC);
    int status = -1;
    ASSERT(WaitChild(pid, &status) == pid);
    ASSERT(status == 63);
    return 0;
}

/* Two locks held by a CPU-bound sibling must be reusable after process Exit. */
static Mutex first = MUTEX_INIT;
static Mutex second = MUTEX_INIT;
static volatile int both_held;
static int two_locks_worker(int argl, void* args)
{
    Mutex_Lock(&first);
    Mutex_Lock(&second);
    __atomic_store_n(&both_held, 1, __ATOMIC_RELEASE);
    for (;;) __asm__ __volatile__("" ::: "memory");
    return 0;
}
static int two_locks_waiter(int argl, void* args)
{
    Mutex_Lock(&first);
    Mutex_Lock(&second);
    Mutex_Unlock(&second);
    Mutex_Unlock(&first);
    return 0;
}
static int two_locks_parent(int argl, void* args)
{
    ASSERT(CreateThread(two_locks_worker, 0, NULL) != NOTHREAD);
    while (!__atomic_load_n(&both_held, __ATOMIC_ACQUIRE)) (void)GetPid();
    ASSERT(CreateThread(two_locks_waiter, 0, NULL) != NOTHREAD);
    for (int i=0; i<40; ++i) (void)GetPid();
    Exit(57);
    return -1;
}
static int check_reuse(int argl, void* args)
{
    Mutex_Lock(&first);
    Mutex_Lock(&second);
    Mutex_Unlock(&second);
    Mutex_Unlock(&first);
    return 0;
}
BOOT_TEST(test_exit_multiple_mutexes, "Multiple mutexes released on process Exit", .timeout=5)
{
    __atomic_store_n(&both_held, 0, __ATOMIC_RELEASE);
    Pid_t pid = Exec(two_locks_parent,0,NULL);
    ASSERT(pid != NOPROC);
    int status = -1;
    ASSERT(WaitChild(pid,&status) == pid);
    ASSERT(status == 57);
    pid = Exec(check_reuse,0,NULL);
    ASSERT(pid != NOPROC);
    ASSERT(WaitChild(pid,&status) == pid);
    ASSERT(status == 0);
    return 0;
}

/* Single-thread Exit does not enter sys_ThreadExit; cleanup still matters. */
static Mutex single = MUTEX_INIT;
static int single_exit(int argl, void* args)
{
    Mutex_Lock(&single);
    Exit(61);
    return -1;
}
static int single_reuse(int argl, void* args)
{
    Mutex_Lock(&single);
    Mutex_Unlock(&single);
    return 0;
}
BOOT_TEST(test_exit_single_mutex, "Single-thread Exit releases its mutex", .timeout=5)
{
    Pid_t pid = Exec(single_exit,0,NULL);
    ASSERT(pid != NOPROC);
    int status=-1;
    ASSERT(WaitChild(pid,&status)==pid);
    ASSERT(status==61);
    pid=Exec(single_reuse,0,NULL);
    ASSERT(pid!=NOPROC);
    ASSERT(WaitChild(pid,&status)==pid);
    ASSERT(status==0);
    return 0;
}

TEST_SUITE(exit_extended_tests, "Extended process Exit tests") {
    &test_exit_cpu_only,
    &test_exit_multiple_mutexes,
    &test_exit_single_mutex,
    NULL
};
int main(int argc, char** argv)
{
    return register_test(&exit_extended_tests) ||
        run_program(argc, argv, &exit_extended_tests);
}
