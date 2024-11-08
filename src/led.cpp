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

// C/C++ language includes
// none

// ESP system includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <esp_log.h>
#include <driver/gpio.h>

// RATGDO project includes
#include "ratgdo.h"
#include "led.h"

// Logger tag
static const char *TAG = "ratgdo-led";

// Construct the singleton object for LED access
LED *LED::instancePtr = new LED();
LED *led = LED::getInstance();

void LEDtimerCallback(TimerHandle_t xTimer)
{
    LED *led = (LED *)pvTimerGetTimerID(xTimer);
    led->idle();
}

// Constructor for LED class
LED::LED()
{
    ESP_LOGI(TAG, "Constructor for LED class");
    resetTime = 500;
    if (UART_TX_PIN != LED_BUILTIN)
    {
        gpio_set_direction(LED_BUILTIN, GPIO_MODE_OUTPUT);
        on();
    }
    LEDtimer = xTimerCreate("LEDtimer", pdMS_TO_TICKS(resetTime), pdFALSE, this, LEDtimerCallback);
}

void LED::on()
{
    gpio_set_level(LED_BUILTIN, 0);
}

void LED::off()
{
    gpio_set_level(LED_BUILTIN, 1);
}

void LED::idle()
{
    gpio_set_level(LED_BUILTIN, idleState);
}

void LED::setIdleState(uint8_t state)
{
    // 0 = LED flashes off (idle is on)
    // 1 = LED flashes on (idle is off)
    // 3 = LED disabled (active and idle both off)
    if (state == 2)
    {
        idleState = activeState = 1;
    }
    else
    {
        idleState = state;
        activeState = (state == 1) ? 0 : 1;
    }
}

void LED::flash(unsigned long ms)
{
    if (ms > 0 && ms != resetTime)
    {
        resetTime = ms;
        xTimerChangePeriod(LEDtimer, pdMS_TO_TICKS(resetTime), 0);
    }
    gpio_set_level(LED_BUILTIN, activeState);
    xTimerReset(LEDtimer, 0);
}
