#include "scheduler.h"

ScheduledTask tasks[TASK_COUNT];

bool isTaskDue(ScheduledTaskId id, unsigned long nowMs) {
    return tasks[id].enabled && long(nowMs - tasks[id].nextRunMs) >= 0;
}

void scheduleTask(ScheduledTaskId id, unsigned long nextRunMs) {
    tasks[id].nextRunMs = nextRunMs;
    tasks[id].enabled = true;
}

void disableScheduledTask(ScheduledTaskId id) {
    tasks[id].enabled = false;
}
