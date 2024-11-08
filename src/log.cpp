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

// C/C++ language includes
// #include <stdint.h>
#include <cstring>
#include <algorithm>
#include <memory>
#include <filesystem>
#include <fstream>

// ESP system includes
// #include <esp_log.h>
// #include <esp_err.h>
// #include <esp_littlefs.h>

// RATGDO project includes
#include "log.h"
#include "utilities.h"
// #include "secplus2.h"
// #include "comms.h"
#include "web.h"

// Logger tag
static const char *TAG = "ratgdo-logger";

#ifdef LOG_MSG_BUFFER
vprintf_like_t systemLogFn;
SemaphoreHandle_t mutexLogger = NULL;

#define LINE_BUFFER_SIZE 256
// char *lineBuffer = NULL;
logBuffer *msgBuffer = NULL; // Buffer to save log messages as they occur

// fstream logMessageFile;

#ifdef SYSLOG
#define SYSLOG_PORT 514
#define SYSLOG_LOCAL0 16
#define SYSLOG_EMERGENCY 0
#define SYSLOG_ALERT 1
#define SYSLOG_CRIT 2
#define SYSLOG_ERROR 3
#define SYSLOG_WARN 4
#define SYSLOG_NOTICE 5
#define SYSLOG_INFO 6
#define SYSLOG_DEBUG 7
#define SYSLOG_NIL "-"
#define SYSLOG_BOM "\xEF\xBB\xBF"

WiFiUDP syslog;
void logToSyslog(char *message)
{
    if (!syslogEn || !WiFi.isConnected())
        return;

    uint8_t PRI = SYSLOG_LOCAL0 * 8;
    if (*message == '>')
        PRI += SYSLOG_INFO;
    else if (*message == '!')
        PRI += SYSLOG_ERROR;

    char *app_name;
    char *msg;

    app_name = strtok(message, "]");
    while (*app_name == ' ')
        app_name++;
    app_name = strtok(NULL, ":");
    while (*app_name == ' ')
        app_name++;
    msg = strtok(NULL, "\r\n");
    while (*msg == ' ')
        msg++;

    syslog.beginPacket(userConfig->syslogIP, SYSLOG_PORT);

    // Use RFC5424 Format
    syslog.printf("<%u>1 ", PRI); // PRI code
#if defined(NTP_CLIENT) && defined(USE_NTP_TIMESTAMP)
    syslog.print((enableNTP && clockSet) ? timeString(0, true) : SYSLOG_NIL);
#else
    syslog.print(SYSLOG_NIL); // Time - let the syslog server insert time
#endif
    syslog.print(" ");
    syslog.print(device_name_rfc952); // hostname
    syslog.print(" ");
    syslog.print(app_name);        // application name
    syslog.printf(" %d", loop_id); // process ID
    syslog.print(" " SYSLOG_NIL    // message ID
                 " " SYSLOG_NIL    // structured data
#ifdef USE_UTF8_BOM
                 " " SYSLOG_BOM); // BOM - indicates UTF-8 encoding
#else
                 " "); // No BOM
#endif
    syslog.print(msg); // message

    syslog.endPacket();
}
#endif

// Make sure that CONFIG_LOG_COLORS is not set in menuconfig.
int logToBuffer(const char *format, va_list args)
{
    // start by calling the system logger
    int rc = systemLogFn(format, args);
    // now do our thing, protected by mutex.
    // CAUTION, do not use ESP_LOGx() within this mutex
    char lineBuffer[LINE_BUFFER_SIZE];
    //xSemaphoreTake(mutexLogger, portMAX_DELAY);
    if (!msgBuffer)
    {
        // first time in we need to create the buffers
        printf("Allocating memory for logs\n");
        msgBuffer = (logBuffer *)malloc(sizeof(logBuffer));
        printf("Allocated %d bytes for message log buffer\n", sizeof(logBuffer));
        // lineBuffer = (char *)malloc(LINE_BUFFER_SIZE);
        // printf("Allocated %d bytes for line buffer\n", LINE_BUFFER_SIZE);
        //  Fill the buffer with space chars... because if we crash and dump buffer before it fills
        //  up, we want blank space not garbage! Nothing is null-terminated in this circular buffer.
        memset(msgBuffer->buffer, 0x20, sizeof(msgBuffer->buffer));
        msgBuffer->wrapped = 0;
        msgBuffer->head = 0;
    }
    //xSemaphoreGive(mutexLogger);
    // parse the format string into lineBuffer
    size_t len = vsnprintf(lineBuffer, LINE_BUFFER_SIZE, format, args);
    // copy the line into the message save buffer
    size_t available = sizeof(msgBuffer->buffer) - msgBuffer->head;
    memcpy(&msgBuffer->buffer[msgBuffer->head], lineBuffer, std::min(available, len));
    if (available < len)
    {
        // we wrapped on the available buffer space
        msgBuffer->wrapped = 1;
        msgBuffer->head = len - available;
        memcpy(msgBuffer->buffer, &lineBuffer[available], msgBuffer->head);
    }
    else
    {
        msgBuffer->head += len;
    }
    // send it to subscribed browsers
    SSEBroadcastState(lineBuffer, LOG_MESSAGE);
#ifdef SYSLOG
    logToSyslog(lineBuffer);
#endif
    return rc; // return code from the system logger
}

#ifdef ENABLE_CRASH_LOG
void crashCallback()
{
    if (msgBuffer && logMessageFile)
    {
        logMessageFile.truncate(0);
        logMessageFile.seek(0, fs::SeekSet);
        logMessageFile.println();
        logMessageFile.write(ESP.checkFlashCRC() ? "Flash CRC OK" : "Flash CRC BAD");
        logMessageFile.println();
        printMessageLog(logMessageFile);
        logMessageFile.close();
    }
    // We may not have enough memory to open the file and save the code
    // save_rolling_code();
}
#endif

void sendRebootLog(httpd_req_t *req)
{
    std::ifstream file(REBOOT_LOG_MSG_FILE);
    if (!file.is_open())
        return;

    ESP_LOGI(TAG, "Sending reboot log");
    int num = LINE_BUFFER_SIZE;
    char lineBuffer[LINE_BUFFER_SIZE];
    while (num == LINE_BUFFER_SIZE)
    {
        file.read(lineBuffer, LINE_BUFFER_SIZE);
        num = file.gcount();
        httpd_resp_send_chunk(req, lineBuffer, num);
    }
    httpd_resp_sendstr_chunk(req, "\n");
    httpd_resp_send_chunk(req, NULL, 0);

    file.close();
}

void sendCrashLog(httpd_req_t *req)
{
    std::ifstream file(CRASH_LOG_MSG_FILE);
    if (!file.is_open())
        return;

    ESP_LOGI(TAG, "Sending crash log");
    int num = LINE_BUFFER_SIZE;
    char lineBuffer[LINE_BUFFER_SIZE];
    while (num == LINE_BUFFER_SIZE)
    {
        file.read(lineBuffer, LINE_BUFFER_SIZE);
        num = file.gcount();
        httpd_resp_send_chunk(req, lineBuffer, num);
    }
    httpd_resp_sendstr_chunk(req, "\n");
    httpd_resp_send_chunk(req, NULL, 0);

    file.close();
}

void sendMessageLog(httpd_req_t *req)
{
    char lineBuffer[64];
#ifdef NTP_CLIENT
    if (enableNTP && clockSet)
    {
        snprintf(lineBuffer, sizeof(lineBuffer), "Server time (secs): %lld\n", time(NULL));
        httpd_resp_sendstr_chunk(req, lineBuffer);
    }
#endif
    snprintf(lineBuffer, sizeof(lineBuffer), "Server uptime (ms): %lld\n", millis());
    httpd_resp_sendstr_chunk(req, lineBuffer);
    snprintf(lineBuffer, sizeof(lineBuffer), "Firmware version: %s\n", AUTO_VERSION);
    httpd_resp_sendstr_chunk(req, lineBuffer);
    snprintf(lineBuffer, sizeof(lineBuffer), "Free heap: %lu\n", esp_get_free_heap_size());
    httpd_resp_sendstr_chunk(req, lineBuffer);
    snprintf(lineBuffer, sizeof(lineBuffer), "Minimum heap: %lu\n", esp_get_minimum_free_heap_size());
    httpd_resp_sendstr_chunk(req, lineBuffer);
#if defined(MMU_IRAM_HEAP)
    snprintf(lineBuffer, sizeof(lineBuffer), "IRAM heap size: %d\n", MMU_SEC_HEAP_SIZE);
    httpd_resp_sendstr_chunk(req, lineBuffer);
#endif
    httpd_resp_sendstr_chunk(req, "\n");
    if (msgBuffer)
    {
        if (msgBuffer->wrapped != 0)
        {
            httpd_resp_send_chunk(req, &msgBuffer->buffer[msgBuffer->head], sizeof(msgBuffer->buffer) - msgBuffer->head);
        }
        httpd_resp_send_chunk(req, msgBuffer->buffer, msgBuffer->head);
    }
    httpd_resp_send_chunk(req, NULL, 0);
}

void saveMessageLog()
{
    std::ofstream file(REBOOT_LOG_MSG_FILE);
    if (!file.is_open())
        return;
    ESP_LOGI(TAG, "Save reboot log");
#ifdef NTP_CLIENT
    if (enableNTP && clockSet)
    {
        file << "Server time (secs): " << time(NULL) << "\n";
    }
#endif
    file << "Server uptime (ms): " << millis() << "\n";
    file << "Firmware version: " << AUTO_VERSION << "\n";
    file << "Free heap: " << esp_get_free_heap_size() << "\n";
    file << "Minimum heap: " << esp_get_minimum_free_heap_size() << "\n";
#if defined(MMU_IRAM_HEAP)
    file << "IRAM heap size: " << MMU_SEC_HEAP_SIZE << "\n";
#endif
    file << "\n";
    if (msgBuffer)
    {
        if (msgBuffer->wrapped != 0)
        {
            file.write(&msgBuffer->buffer[msgBuffer->head], sizeof(msgBuffer->buffer) - msgBuffer->head);
        }
        file.write(msgBuffer->buffer, msgBuffer->head);
    }

    file.close();
}

#endif // LOG_MSG_BUFFER
