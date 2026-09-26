#pragma once

// A binary event with a timed wait: os_event_set() latches a signal, os_event_wait_ms() blocks until
// there is one (consuming it) or the timeout ends. Replaces "sleep 30-100 ms and look again" loops
// with a real blocking wait, so the CPU can stay in light sleep between events (issue #45/#46).
//
// Declare with OS_EVENT_INITIALIZER; the ESP version creates its semaphore on first use.

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef SemaphoreHandle_t os_event_t;
#define OS_EVENT_INITIALIZER NULL

static inline SemaphoreHandle_t os_event_get_(os_event_t *event) {
    // First use may race between two tasks; the loser deletes its semaphore.
    if (*event == NULL) {
        SemaphoreHandle_t created = xSemaphoreCreateBinary();
        if (created != NULL) {
            portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
            taskENTER_CRITICAL(&lock);
            bool won = (*event == NULL);
            if (won) *event = created;
            taskEXIT_CRITICAL(&lock);
            if (!won) vSemaphoreDelete(created);
        }
    }
    return *event;
}

static inline void os_event_set(os_event_t *event) {
    SemaphoreHandle_t sem = os_event_get_(event);
    if (sem != NULL) {
        (void)xSemaphoreGive(sem);  // giving an already-given binary semaphore is a no-op
    }
}

static inline bool os_event_wait_ms(os_event_t *event, uint32_t timeout_ms) {
    SemaphoreHandle_t sem = os_event_get_(event);
    if (sem == NULL) {
        return false;
    }
    return xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

#else  // host builds (tests, simulator)

#include <pthread.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool set;
} os_event_t;
#define OS_EVENT_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false }

static inline void os_event_set(os_event_t *event) {
    pthread_mutex_lock(&event->mutex);
    event->set = true;
    pthread_cond_signal(&event->cond);
    pthread_mutex_unlock(&event->mutex);
}

static inline bool os_event_wait_ms(os_event_t *event, uint32_t timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&event->mutex);
    while (!event->set) {
        if (pthread_cond_timedwait(&event->cond, &event->mutex, &deadline) != 0) {
            break;  // timed out
        }
    }
    bool signalled = event->set;
    event->set = false;
    pthread_mutex_unlock(&event->mutex);
    return signalled;
}

#endif
