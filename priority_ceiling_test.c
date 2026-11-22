
#include "unit_testing.h"
#include "kernel_sched.h"

/*
 * MLFQ - Priority Ceiling Tests
 *
 * Test 1: Nested mutex acquisition and priority restoration.
 * Test 2: Priority inversion with LOW, MEDIUM and HIGH threads.
 */

/* ============================================================
 * TEST 1: NESTED PRIORITY CEILING
 * ============================================================ */

static Mutex test_mutex_a = MUTEX_INIT;
static Mutex test_mutex_b = MUTEX_INIT;

BOOT_TEST(test_nested_priority_ceiling,
          "Check priority ceiling with two nested mutexes")
{
    TCB* tcb = cur_thread();

    ASSERT(tcb != NULL);
    if (tcb == NULL)
        return -1;

    int original_priority = tcb->priority;

    /* Initial state: no mutexes held. */
    ASSERT(tcb->type == NORMAL_THREAD);
    ASSERT(tcb->held_mutexes == 0);
    ASSERT(tcb->inherited_priority == -1);

    /* Acquire the first mutex. */
    Mutex_Lock(&test_mutex_a);

    ASSERT(tcb->held_mutexes == 1);
    ASSERT(tcb->inherited_priority == MLFQ_LEVELS - 1);
    ASSERT(tcb->priority == original_priority);

    /* Acquire the second mutex. */
    Mutex_Lock(&test_mutex_b);

    ASSERT(tcb->held_mutexes == 2);
    ASSERT(tcb->inherited_priority == MLFQ_LEVELS - 1);

    /* Release B while still holding A. */
    Mutex_Unlock(&test_mutex_b);

    ASSERT(tcb->held_mutexes == 1);
    ASSERT(tcb->inherited_priority == MLFQ_LEVELS - 1);

    /* Release the last mutex. */
    Mutex_Unlock(&test_mutex_a);

    ASSERT(tcb->held_mutexes == 0);
    ASSERT(tcb->inherited_priority == -1);
    ASSERT(tcb->priority == original_priority);

    return 0;
}


/* ============================================================
 * TEST 2: PRIORITY INVERSION
 *
 * LOW    : Boot thread, base priority 1, holds a mutex.
 * MEDIUM : Independent work, base priority 2.
 * HIGH   : Tries to acquire LOW's mutex, priority 3.
 *
 * This test runs with one CPU to check scheduling order.
 * ============================================================ */

static Mutex inversion_mutex = MUTEX_INIT;

/* Shared atomic flags and observations. */
static int medium_started;
static int high_attempting;
static int high_acquired;
static int medium_steps;

#define TEST_LOAD(x) \
    __atomic_load_n(&(x), __ATOMIC_SEQ_CST)

#define TEST_SET(x, v) \
    __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

#define TEST_INC(x) \
    __atomic_fetch_add(&(x), 1, __ATOMIC_SEQ_CST)


/* MEDIUM: Execute work independently of the mutex. */
static int medium_priority_task(int argl, void* args)
{
    TCB* self = cur_thread();

    self->priority = 2;

    TEST_SET(medium_started, 1);

    /*
     * Yield once after setting priority 2, so the scheduler
     * can place this thread according to its new priority.
     */
    yield(SCHED_USER);

    /* Simulate a bounded amount of independent work. */
    for (int i = 0; i < 50; i++) {
        TEST_INC(medium_steps);
        yield(SCHED_USER);
    }

    return 0;
}


/* HIGH: Attempt to acquire the mutex already held by LOW. */
static int high_priority_task(int argl, void* args)
{
    TCB* self = cur_thread();

    self->priority = MLFQ_LEVELS - 1;

    TEST_SET(high_attempting, 1);

    /* This must wait until LOW releases the mutex. */
    Mutex_Lock(&inversion_mutex);

    TEST_SET(high_acquired, 1);

    Mutex_Unlock(&inversion_mutex);

    return 0;
}


/*
 * LOW is the thread executing this BOOT_TEST.
 * It acquires the mutex before creating MEDIUM and HIGH.
 */
BOOT_TEST(test_priority_inversion,
          "Check priority inversion with LOW, MEDIUM and HIGH",
          .timeout = 20)
{
    TCB* low = cur_thread();

    ASSERT(low != NULL);
    if (low == NULL)
        return -1;

    ASSERT(low->type == NORMAL_THREAD);
    ASSERT(low->held_mutexes == 0);

    /* Reset observations. */
    TEST_SET(medium_started, 0);
    TEST_SET(high_attempting, 0);
    TEST_SET(high_acquired, 0);
    TEST_SET(medium_steps, 0);

    int original_priority = low->priority;

    /* --------------------------------------------------------
     * STEP 1: LOW acquires the mutex.
     * -------------------------------------------------------- */

    low->priority = 1;

    Mutex_Lock(&inversion_mutex);

    /* Priority ceiling must be active. */
    ASSERT(low->held_mutexes == 1);
    ASSERT(low->inherited_priority == MLFQ_LEVELS - 1);

    /* --------------------------------------------------------
     * STEP 2: Start MEDIUM and HIGH.
     * -------------------------------------------------------- */

    Tid_t medium = CreateThread(medium_priority_task, 0, NULL);
    Tid_t high = CreateThread(high_priority_task, 0, NULL);

    ASSERT(medium != NOTHREAD);
    ASSERT(high != NOTHREAD);

    /*
     * Allow both threads to start.
     * LOW still holds the mutex and its effective priority
     * must remain at the highest MLFQ level.
     */
    for (int i = 0; i < 10000; i++) {

        if (TEST_LOAD(medium_started) &&
            TEST_LOAD(high_attempting))
            break;

        yield(SCHED_USER);
    }

    int both_started =
        TEST_LOAD(medium_started) &&
        TEST_LOAD(high_attempting);

    ASSERT(both_started);

    /*
     * Give HIGH additional chances to reach Mutex_Lock().
     * It cannot acquire the mutex while LOW holds it.
     */
    if (both_started) {
        for (int i = 0; i < 3; i++)
            yield(SCHED_USER);
    }

    /* HIGH must not have acquired LOW's mutex yet. */
    ASSERT(TEST_LOAD(high_acquired) == 0);

    /* LOW must still have the priority ceiling. */
    ASSERT(low->held_mutexes == 1);
    ASSERT(low->inherited_priority == MLFQ_LEVELS - 1);

    /* --------------------------------------------------------
     * STEP 3: Check MEDIUM before releasing the mutex.
     * -------------------------------------------------------- */

    /*
     * With one CPU, MEDIUM (priority 2) should not progress
     * while LOW is READY with effective priority 3.
     *
     * Record the count BEFORE unlocking to avoid races
     * with the scheduling that follows the unlock.
     */
    int steps_before_unlock = TEST_LOAD(medium_steps);

    /* --------------------------------------------------------
     * STEP 4: LOW releases its mutex.
     * -------------------------------------------------------- */

    Mutex_Unlock(&inversion_mutex);

    /* Priority ceiling must now be removed. */
    ASSERT(low->held_mutexes == 0);
    ASSERT(low->inherited_priority == -1);

    /* --------------------------------------------------------
     * STEP 5: Wait for HIGH and MEDIUM to finish.
     * -------------------------------------------------------- */

    if (high != NOTHREAD)
        ASSERT(ThreadJoin(high, NULL) == 0);

    if (medium != NOTHREAD)
        ASSERT(ThreadJoin(medium, NULL) == 0);

    /* Restore the boot thread's original base priority. */
    low->priority = original_priority;

    /* --------------------------------------------------------
     * STEP 6: Final checks.
     * -------------------------------------------------------- */

    ASSERT(TEST_LOAD(high_acquired) == 1);

    /* MEDIUM must have executed its independent work. */
    ASSERT(TEST_LOAD(medium_steps) == 50);

    /*
     * This is the scheduling observation of interest:
     * MEDIUM should not progress before LOW unlocks.
     */
    ASSERT(steps_before_unlock == 0);

    return 0;
}

#undef TEST_LOAD
#undef TEST_SET
#undef TEST_INC


/* ============================================================
 * TEST SUITE
 * ============================================================ */

TEST_SUITE(priority_ceiling_tests,
           "MLFQ Priority Ceiling Tests")
{
    &test_nested_priority_ceiling,
    &test_priority_inversion,
    NULL
};


/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char** argv)
{
    return register_test(&priority_ceiling_tests) ||
           run_program(argc, argv, &priority_ceiling_tests);
}
