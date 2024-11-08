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

#define COMMS_TASK_NAME   ("comms")
#define COMMS_TASK_PRIO   (1)
#define COMMS_TASK_STK_SZ (8 * 1024)

#define HOMEKIT_TASK_NAME   ("homekit")
#define HOMEKIT_TASK_PRIO   (2)
#define HOMEKIT_TASK_STK_SZ (4 * 1024)

#define WIFI_TASK_NAME   ("wifi")
#define WIFI_TASK_PRIO   (1)
#define WIFI_TASK_STK_SZ (8 * 1024)

#define RX_ISR_TASK_NAME ("rx_isr")
#define RX_ISR_TASK_PRIO (8)
#define RX_ISR_TASK_STK_SZ (3 * 1024)