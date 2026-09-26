#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "os_event.h"

static os_event_t s_event = OS_EVENT_INITIALIZER;

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static void *setter(void *arg) {
    (void)arg;
    struct timespec pause = {0, 50 * 1000 * 1000};
    nanosleep(&pause, NULL);
    os_event_set(&s_event);
    return NULL;
}

int main(void) {
    struct timespec t0;

    // Nothing signalled: the wait ends at the timeout and says so.
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(!os_event_wait_ms(&s_event, 60));
    assert(ms_since(&t0) >= 55);

    // A signal that arrived first is latched and consumed by exactly one wait.
    os_event_set(&s_event);
    os_event_set(&s_event);  // binary: two sets are still one signal
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(os_event_wait_ms(&s_event, 1000));
    assert(ms_since(&t0) < 500);
    assert(!os_event_wait_ms(&s_event, 20));

    // A signal from another thread ends a long wait early.
    pthread_t th;
    pthread_create(&th, NULL, setter, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(os_event_wait_ms(&s_event, 5000));
    long waited = ms_since(&t0);
    assert(waited >= 40 && waited < 1000);
    pthread_join(th, NULL);

    puts("os_event ok");
    return 0;
}
