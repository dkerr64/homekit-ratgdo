/****************************************************************************
 * RATGDO HomeKit for ESP32
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 *
 * Copyright (c) 2023-24 Brandon Matthews... https://github.com/thenewwazoo
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 * Contributions acknowledged from
 *
 * David A Kerr... https://github.com/dkerr64/
 * Jonathan Stroud...  https://github.com/jgstroud
 *
 */
#pragma once

// C/C++ language includes
// none

// ESP system includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <driver/gptimer.h>
#include <driver/gpio.h>

// RATGDO project includes
#include "secplus2.h"

// enable to emit an ISR edge on D0 aka GPIO_NUM_16
#define ISR_DEBUG

enum class State : uint8_t
{
    Idle,
    Start,
    Data,
    Stop,
};

struct ISREvent
{
    int64_t ticks;
    int level;
};

class SoftUart
{
private:
    bool one_wire;

    static SoftUart *instancePtr;
    SoftUart();

public:
    gpio_num_t rx_pin;
    gpio_num_t tx_pin;
    uint32_t speed;       // bits per second
    uint32_t bit_time_us; // microseconds per bit
    bool invert;

    QueueHandle_t tx_q;
    State tx_state = State::Idle;
    uint8_t tx_bit_count = 0;
    uint8_t tx_byte = 0;

    // task that receives and processes GPIO edge ISREvents
    TaskHandle_t rx_task;

    // queue to handle bit edges
    QueueHandle_t rx_isr_q;
    // queue to handle fully-received bytes
    QueueHandle_t rx_q;
    State rx_state = State::Idle;
    uint8_t rx_bit_count = 0;
    uint8_t rx_byte = 0;
    uint32_t last_isr_ticks = 0;
    bool last_isr_level = true; // default to active-high
    SemaphoreHandle_t tx_flag;
    gptimer_handle_t gptimer;

    SoftUart(const SoftUart &obj) = delete;
    static SoftUart *getInstance() { return instancePtr; }

    bool initialize(gpio_num_t rx_pin, gpio_num_t tx_pin, uint32_t speed, bool invert = false, bool one_wire = false);
    bool transmit(uint8_t bytebuf[], size_t len);
    bool transmit(uint8_t byte) { return transmit(&byte, 1); };
    bool available(void) { return uxQueueMessagesWaiting(rx_q) > 0; }
    bool read(uint8_t *out) { return xQueueReceive(rx_q, out, 0); }
    void process_isr(ISREvent &e);
    gptimer_handle_t getTimer() { return gptimer; };
};

extern SoftUart *sw_serial;

// we can buffer up to 10 complete Security+ 2.0 packets to transmit
constexpr size_t BYTE_Q_BUF_SZ = SECPLUS2_CODE_LEN * 10;

// we can store up to 5 complete Security+ 2.0 packets' worth of data w/ value 0x55
constexpr size_t ISR_Q_BUF_SZ = 10 /* bits per byte */ * SECPLUS2_CODE_LEN /* bytes per packet */ * 5 /* packets */;
