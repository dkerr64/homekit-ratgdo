/****************************************************************************
 * RATGDO HomeKit for ESP32
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 *
 * Copyright (c) 2023-24 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// C/C++ language includes
#include <stdint.h>

// ESP system includes
#include <freertos/timers.h>
#include <esp_timer.h>

// RATGDO project includes
// none

class LED
{
private:
    uint8_t activeState = 0;     // 0 == LED on, 1 == LED off
    uint8_t idleState = 1;       // opposite of active
    unsigned long resetTime = 0; // Stores time when LED should return to idle state
    TimerHandle_t LEDtimer = NULL;
    static LED *instancePtr;
    LED();

public:
    LED(const LED &obj) = delete;
    static LED *getInstance() { return instancePtr; }

    void on();
    void off();
    void idle();
    void flash(unsigned long ms = 0);
    void setIdleState(uint8_t state);
    uint8_t getIdleState() { return idleState; };
};

extern LED *led;
