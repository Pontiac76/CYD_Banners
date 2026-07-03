#pragma once
#include <Arduino.h>

enum ScheduledTaskId
{
    TASK_NETWORK,
    TASK_SERVER_HEARTBEAT,
    TASK_SERIAL_HEARTBEAT,
    TASK_SUM_CHECK,
    TASK_MANIFEST_DOWNLOAD,
    TASK_MANIFEST_SCAN,
    TASK_RENDER_NEXT,
    TASK_FIRMWARE_OTA_CHECK,
    TASK_COUNT
};

constexpr unsigned long TASK_SERIAL_HEARTBEAT_INTERVAL_MS = 15000UL;
constexpr unsigned long TASK_SERVER_HEARTBEAT_INTERVAL_MS = 15000UL;

struct ScheduledTask
{
    unsigned long nextRunMs;
    bool enabled;
};

extern ScheduledTask tasks[TASK_COUNT];

bool isTaskDue(ScheduledTaskId id, unsigned long nowMs);
void scheduleTask(ScheduledTaskId id, unsigned long nextRunMs);
void disableScheduledTask(ScheduledTaskId id);
