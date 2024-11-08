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
#include <stdint.h>
#include <time.h>

// ESP system includes
#include <esp_timer.h>

// RATGDO project includes
#include "homekit_decl.h"
#include "ratgdo.h"

#ifdef NTP_CLIENT
extern bool clockSet;
extern unsigned long lastRebootAt;
extern char *timeString(time_t reqTime = 0, bool syslog = false);
extern bool enableNTP;
#define NTP_SERVER "pool.ntp.org"
#endif

#define millis() (esp_timer_get_time() / 1000UL)

// Controls whether to log to syslog server
extern bool syslogEn;

// Controls soft Access Point mode.
extern bool softAPmode;

// Password and credential management for HTTP server...
extern const char www_realm[];

//#define IP_ADDRESS_SIZE 16

// Bitset that identifies what will trigger the motion sensor
typedef struct
{
    uint8_t motion : 1;
    uint8_t obstruction : 1;
    uint8_t lightKey : 1;
    uint8_t doorKey : 1;
    uint8_t lockKey : 1;
    uint8_t undef : 3;
} motionTriggerBitset;
typedef union
{
    motionTriggerBitset bit;
    uint8_t asInt;
} motionTriggersUnion;
extern motionTriggersUnion motionTriggers;

// Function declarations
void load_all_config_settings();
void sync_and_restart();
