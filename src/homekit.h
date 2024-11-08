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
// none

// ESP system includes
// none

// RATGDO project includes
// none

#define DEVICE_NAME_SIZE 32
#define SERIAL_NAME_SIZE 18

extern char device_name[DEVICE_NAME_SIZE];
extern char device_name_rfc952[DEVICE_NAME_SIZE];

void homekit_task_entry(void* ctx);

void notify_homekit_target_door_state_change();
void notify_homekit_current_door_state_change();
void notify_homekit_active();
void notify_homekit_target_lock();
void notify_homekit_current_lock();
void notify_homekit_obstruction();
void notify_homekit_light();
void enable_service_homekit_motion(bool);
void notify_homekit_motion();
