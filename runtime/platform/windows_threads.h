// Windows game threads reserve room for the recompiled code's deep recursion without committing it. winpthreads
// passes pthread_attr_setstacksize's size to Windows as the stack's commit size: 256 MB of committed memory per game
// thread (13 at the title screen: 3.3 of the 6.65 GB the process had committed), counted against the system's
// commit limit (RAM + page file) though almost none of it is ever touched. Reserved, the stack grows as it's used.
// The CRT's _beginthreadex keeps its per-thread setup and cleanup. As LAS Recomp's 517a19f.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { void *(*entry)(void *); void *argument; } GuestThreadStart;

static unsigned __stdcall guest_thread_trampoline(void *argument) {
    GuestThreadStart start = *(GuestThreadStart *)argument;
    free(argument);
    start.entry(start.argument);
    return 0;
}

// a thread running entry(argument) with `reserve` bytes of stack reserved: its handle, 0 on failure
static uintptr_t guest_thread_create(void *(*entry)(void *), void *argument, unsigned reserve) {
    GuestThreadStart *start = malloc(sizeof *start);
    if (!start) return 0;
    *start = (GuestThreadStart){entry, argument};
    uintptr_t handle = _beginthreadex(NULL, reserve, guest_thread_trampoline, start, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!handle) free(start);
    return handle;
}

static bool guest_thread_join(uintptr_t handle) {
    if (WaitForSingleObject((HANDLE)handle, INFINITE) != WAIT_OBJECT_0) return false;
    return CloseHandle((HANDLE)handle) != 0;
}
#endif
