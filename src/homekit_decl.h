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

// Possible values for characteristic CURRENT_DOOR_STATE:
#define HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_OPEN 0
#define HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_CLOSED 1
#define HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_OPENING 2
#define HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_CLOSING 3
#define HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_STOPPED 4

#define HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_OPEN 0
#define HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_CLOSED 1

#define HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED 0
#define HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_SECURED 1
#define HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_JAMMED 2
#define HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNKNOWN 3

#define HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_UNSECURED 0
#define HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_SECURED 1

#define HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_CLEAR      0
#define HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_OBSTRUCTED 1