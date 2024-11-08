/****************************************************************************
 * RATGDO HomeKit for ESP32
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 * 
 * Copyright (c) 2023-24 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 * 
 * Contributions acknowledged from
 * Brandon Matthews... https://github.com/thenewwazoo
 * Jonathan Stroud...  https://github.com/jgstroud
 * 
 */
#pragma once

// C/C++ language includes

// ESP system includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_netif_types.h>


// RATGDO project includes
// none

#ifdef LOG_MSG_BUFFER

#define CRASH_LOG_MSG_FILE "/littlefs/crash_log"
#define REBOOT_LOG_MSG_FILE "/littlefs/reboot_log"

#define LOG_BUFFER_SIZE 8192

extern int logToBuffer(const char *format, va_list args);
extern vprintf_like_t systemLogFn;
extern SemaphoreHandle_t mutexLogger;

typedef struct logBuffer
{
    uint16_t wrapped;                 // two bytes
    uint16_t head;                    // two bytes
    char buffer[LOG_BUFFER_SIZE - 4]; // sized so whole struct is LOG_BUFFER_SIZE bytes
} logBuffer;

#include <esp_http_server.h>

extern void sendRebootLog(httpd_req_t *req);
extern void sendCrashLog(httpd_req_t *req);
extern void sendMessageLog(httpd_req_t *req);
extern void saveMessageLog();
/*
void printSavedLog(File file, Print &outDevice = Serial);
void printSavedLog(Print &outDevice = Serial);
void printMessageLog(Print &outDevice = Serial);
*/

#ifdef ENABLE_CRASH_LOG
void crashCallback();
#endif

#endif // LOG_MSG_BUFFER
