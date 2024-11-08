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
//#include "softuart.h"

void comms_task_entry(void* ctx);
void comms_loop(const uint32_t secType);

void open_door();
void close_door();

void set_lock(uint8_t value);
void set_light(bool value);

void save_rolling_code();
void reset_door();
