#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "platform/platform_task.h"

static int s_ran;
static int s_woken;

static void task_fn(void *arg) {
    (void)arg;
    s_ran++;
}

static void wake_hook(void) {
    s_woken++;
}

int main(void) {
    // Without a hook, posting works as before and nothing is woken.
    assert(platform_task_post_to_ui(task_fn, NULL));
    platform_task_run_pending();
    assert(s_ran == 1);
    assert(s_woken == 0);

    // With a hook, every callback that is actually queued wakes the UI loop once.
    platform_task_set_ui_wake_hook(wake_hook);
    assert(platform_task_post_to_ui(task_fn, NULL));
    assert(s_woken == 1);
    assert(platform_task_post_to_ui(task_fn, NULL));
    assert(s_woken == 2);
    platform_task_run_pending();
    assert(s_ran == 3);

    // A post the queue rejects (full) queues nothing, so it must not wake anyone.
    int accepted = 0;
    while (platform_task_post_to_ui(task_fn, NULL)) {
        accepted++;
        assert(accepted < 100000);  // the queue is bounded
    }
    assert(s_woken == 2 + accepted);
    int woken_when_full = s_woken;
    assert(!platform_task_post_to_ui(task_fn, NULL));
    assert(s_woken == woken_when_full);
    platform_task_run_pending();
    assert(s_ran == 3 + accepted);

    // Clearing the hook stops the wake-ups.
    platform_task_set_ui_wake_hook(NULL);
    assert(platform_task_post_to_ui(task_fn, NULL));
    assert(s_woken == woken_when_full);
    platform_task_run_pending();

    puts("platform_task wake hook ok");
    return 0;
}
