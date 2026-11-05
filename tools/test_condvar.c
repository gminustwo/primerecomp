// The guest kernel's condition variables, CancelSynchronization and address arbiter against Horizon's behaviour
// (adapted from LAS Recomp's tools/test_condvar.c, MIT). Each check runs and reports by itself; the exit status is
// the number that failed.
//   clang -O2 -pthread -Iruntime tools/test_condvar.c -o build/test-condvar.exe && build/test-condvar.exe
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/kernel.c"
#include <stdarg.h>

// Unused platform / IPC entry points from the included kernel translation unit: a test that leaves the
// synchronization code under test aborts.
Module g_modules[4];
int g_nmodules;
uint32_t vi_vsync_handle(void) { abort(); }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void plat_thread_guest(int prio) { (void)prio; abort(); }
void plat_commit(uint64_t addr, uint64_t size) { abort(); }
const char *plat_symbol(uint64_t pc, uint64_t *offset) { abort(); }
void plat_print_backtrace(void) { abort(); }
void crash_handler_install(void) { abort(); }
void guest_call(Cpu *c, uint64_t addr) { abort(); }
int ipc_handle_request(Cpu *c, uint32_t handle) { abort(); }
uint32_t ipc_connect_named_port(const char *name) { abort(); }
const char *hle_name_at(uint64_t pc) { (void)pc; return NULL; }
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int failed;
static bool check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failed++;
    return ok;
}

static uint32_t mutex_word, condition_word;
static KThread waiter;
static int contended;
static _Atomic int signaler_saw_mutex_wait;
static void *signal_waiter(void *unused) {
    (void)unused;
    // Wait until the waiter has released its mutex and joined the condition queue; no scheduling delay assumed.
    for (;;) {
        pthread_mutex_lock(&g_klock);
        bool ready = g_waitlist == &waiter && waiter.wait_kind == WAIT_CONDVAR;
        if (ready && contended) mutex_word = 0x222;  // (another thread holds the mutex: the waiter has to wait for it)
        pthread_mutex_unlock(&g_klock);
        if (ready) break;
        sched_yield();
    }
    svc_signal_process_wide_key((uint64_t)&condition_word, 1);
    if (contended) {
        pthread_mutex_lock(&g_klock);
        signaler_saw_mutex_wait = waiter.wait_kind == WAIT_MUTEX;
        mutex_release((uint64_t)&mutex_word);  // (the other thread lets go: ownership goes to the waiter)
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

// User space's own updates of an arbiter word (exclusive load / store, no kernel lock) while the kernel updates it
static uint32_t arbiter_word;
static volatile int arbiter_stop;
static unsigned long arbiter_adds;
static void *arbiter_adder(void *unused) {
    (void)unused;
    while (!arbiter_stop) { __atomic_fetch_add(&arbiter_word, 2, __ATOMIC_SEQ_CST); arbiter_adds++; }
    return NULL;
}

// Runs `kernel_step` (returns the change it made to the word: 0 when it made none) against the adder; true when
// the word ends as the sum of both sides' changes
static bool arbiter_race(long (*kernel_step)(void)) {
    arbiter_word = 0, arbiter_stop = 0, arbiter_adds = 0;
    pthread_t adder;
    pthread_create(&adder, NULL, arbiter_adder, NULL);
    long kernel_delta = 0;
    for (int i = 0; i < 300000; i++) kernel_delta += kernel_step();
    arbiter_stop = 1;
    pthread_join(adder, NULL);
    long lost = (long)(2 * arbiter_adds + kernel_delta) - (long)(int32_t)arbiter_word;
    if (lost) printf("      (%ld of the word's changes lost)\n", lost);
    return lost == 0;
}

static long dec_then_inc(void) {
    long d = 0;
    // decrement if less than (timeout 0: the decrement, then the timeout)
    if (svc_wait_for_address((uint64_t)&arbiter_word, ARB_DEC_WAIT_LESS, INT32_MAX, 0) == R_TIMEOUT) d--;
    // increment if equal
    int32_t v = (int32_t)__atomic_load_n(&arbiter_word, __ATOMIC_SEQ_CST);
    if (svc_signal_to_address((uint64_t)&arbiter_word, ARB_SIGNAL_INC_EQ, v, 0) == R_OK) d++;
    return d;
}

static long modify_by_waiting_count(void) {
    // signal and modify by waiting count if equal: no one waiting, count 1 -> value + 1 (the form Metroid Prime
    // Remastered calls thousands of times a minute, with WaitIfEqual)
    int32_t v = (int32_t)__atomic_load_n(&arbiter_word, __ATOMIC_SEQ_CST);
    return svc_signal_to_address((uint64_t)&arbiter_word, ARB_SIGNAL_MOD_EQ, v, 1) == R_OK ? 1 : 0;
}

int main(void) {
    waiter.handle = 0x130;
    waiter.prio = 46;
    pthread_cond_init(&waiter.cond, NULL);
    t_cur = &waiter;

    {   // A timed wait that runs out returns without the mutex, as on Horizon: the SDK takes it again itself
        // (nn::os TimedWait calls the critical section's Enter on 0xEA01; libnx's condvarWaitTimeout locks it).
        bool ok = true;
        for (int i = 0; i < 40 && ok; i++) {
            mutex_word = waiter.handle;
            uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word, (uint64_t)&condition_word, waiter.handle,
                                                   i ? 1000000 : 0);
            ok = r == R_TIMEOUT && mutex_word == 0 && g_waitlist == NULL;
            if (!ok) printf("      (result %x, mutex word %x after the timeout)\n", r, mutex_word);
            else ok = mutex_acquire_or_mark((uint64_t)&mutex_word, waiter.handle);  // (the SDK's Enter)
            mutex_word = 0;
        }
        check(ok, "a timed condition variable wait that runs out returns without the mutex (the SDK takes it again)");
    }
    for (contended = 0; contended <= 1; contended++) {  // A signalled wait returns holding the mutex
        mutex_word = waiter.handle;
        signaler_saw_mutex_wait = 0;
        pthread_t signaler;
        pthread_create(&signaler, NULL, signal_waiter, NULL);
        uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word, (uint64_t)&condition_word, waiter.handle, -1);
        pthread_join(signaler, NULL);
        bool ok = r == R_OK && mutex_word == waiter.handle && g_waitlist == NULL && (!contended || signaler_saw_mutex_wait);
        check(ok, contended ? "a signalled wait whose mutex is held gets it when it is let go"
                            : "a signalled wait returns holding the mutex");
        mutex_word = 0;
    }
    {   // A pending CancelSynchronization is for WaitSynchronization only: a condition variable wait and a polling
        // WaitSynchronization (timeout 0) leave it, and the next blocking WaitSynchronization returns cancelled.
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        uint32_t h = kernel_new_handle(ev);
        int32_t idx = -1;
        waiter.cancel = true;
        mutex_word = waiter.handle;
        uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word, (uint64_t)&condition_word, waiter.handle, 1000000);
        mutex_word = 0;
        check(r == R_TIMEOUT && waiter.cancel, "a condition variable wait leaves a pending cancel alone");
        waiter.cancel = true;
        r = svc_wait_synchronization((uint64_t)&h, 1, 0, &idx);
        check(r == R_TIMEOUT && waiter.cancel, "a polling WaitSynchronization (timeout 0) leaves a pending cancel");
        waiter.cancel = true;
        r = svc_wait_synchronization((uint64_t)&h, 1, -1, &idx);
        check(r == R_CANCELLED && !waiter.cancel, "the next blocking WaitSynchronization is cancelled by it");
        waiter.cancel = false;
    }
    check(arbiter_race(dec_then_inc),
          "address arbiter: decrement-if-less and increment-if-equal lose none of user space's changes");
    check(arbiter_race(modify_by_waiting_count),
          "address arbiter: modify-by-waiting-count-if-equal loses none of user space's changes");
    pthread_cond_destroy(&waiter.cond);
    printf("%s\n", failed ? "FAILED" : "all passed");
    return failed;
}
