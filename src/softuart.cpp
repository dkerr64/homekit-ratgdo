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

// C/C++ language includes
// none

// ESP system includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gptimer.h>
#include <driver/gpio.h>

// RATGDO project includes
#include "softuart.h"
#include "tasks.h"

#define TAG "ratgdo-softuart"

// Construct the singleton object for SoftUart access
SoftUart *SoftUart::instancePtr = new SoftUart();
SoftUart *sw_serial = SoftUart::getInstance();

typedef bool (*gptimer_alarm_cb_t)(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);

IRAM_ATTR static bool handle_tx(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg)
{
    SoftUart *uart = (SoftUart *)arg;

    switch (uart->tx_state)
    {
    case State::Start:
    {
        // set the start bit
        gpio_set_level(uart->tx_pin, uart->invert);

        // set the initial conditions for data bit transmission
        uart->tx_bit_count = 0;

        // move the state machine
        uart->tx_state = State::Data;

        break;
    }

    case State::Data:
    {
        // get the bit to be output
        bool bit = (uart->tx_byte & 0x1) ^ uart->invert;
        gpio_set_level(uart->tx_pin, bit);

        // prepare the next bit
        uart->tx_byte >>= 1;

        // track which bit we've emitted
        uart->tx_bit_count += 1;

        // if we've written 8 bits
        if (uart->tx_bit_count == 8)
        {
            // move the state machine
            uart->tx_state = State::Stop;
        }

        break;
    }

    case State::Stop:
    {
        // set the stop bit to logic high
        gpio_set_level(uart->tx_pin, !uart->invert);

        // after that we're back to idle and done
        uart->tx_state = State::Idle;

        break;
    }

    case State::Idle:
    {

        if (xQueueReceiveFromISR(uart->tx_q, &uart->tx_byte, NULL))
        {
            // there was another byte waiting to be sent
            uart->tx_state = State::Start;
        }
        else
        {
            // disarm the timer and remove the callback (hw_timer might be used for rx)
            ESP_ERROR_CHECK(gptimer_stop(uart->gptimer));

            // notify caller that transmission has finished
            xSemaphoreGiveFromISR(uart->tx_flag, NULL);
        }

        break;
    }
    }
    return true;
}

void rx_isr_handler_entry(SoftUart *uart)
{
    // STOP bits sometimes aren't preceded by a transition if the bits preceding it are all zeroes.
    // this is the number of milliseconds it takes a whole byte (1 start + 8 data + 1 stop bits) to
    // arrive, plus one. we use this as a timeout to know when to consider a byte "complete".
    //
    uint32_t byte_timeout_ms = (10000 / uart->speed) + 1;
    while (true)
    {
        ISREvent e;
        if (xQueueReceive(uart->rx_isr_q, &e, pdMS_TO_TICKS(byte_timeout_ms)))
        {
            // got a bit
            uart->process_isr(e);
        }
        else
        {
            int64_t ct = esp_timer_get_time();
            // didn't get a bit
            //
            // if we get here, we're waiting for more bits, but we've timed out waiting for them
            if (uart->rx_state != State::Idle)
            {
                e = {
                    // why can we pick any old timestamp here? well, because the state machine
                    // permits us to do so. the time since the last interrupt will get chopped into
                    // some number of bit periods. we know there has been no transition since then,
                    // so we know every bit period has the same value. even if the calculated bit
                    // periods don't match the real ones, there's no difference because there's no
                    // edge, so it doesn't matter "when" the "samples" happen. and even if there are
                    // "too many" bit periods for the remaining number of bits, we no-op the
                    // State::Idle bit periods (TODO: add an optimization to skip them entirely)
                    ct,
                    gpio_get_level(uart->rx_pin)};
                uart->process_isr(e);
            }
        }
    }
}

IRAM_ATTR void handle_rx_edge(SoftUart *uart)
{
#ifdef ISR_DEBUG
    gpio_set_level(GPIO_NUM_16, true);
#endif
    ISREvent e = {
        esp_timer_get_time(),
        gpio_get_level(uart->rx_pin)};
    xQueueSendToBackFromISR(uart->rx_isr_q, &e, NULL);
#ifdef ISR_DEBUG
    gpio_set_level(GPIO_NUM_16, false);
#endif
}

SoftUart::SoftUart()
{
    ESP_LOGI(TAG, "Constructor for SoftUart");
}

bool SoftUart::initialize(gpio_num_t rx_pin, gpio_num_t tx_pin, uint32_t speed, bool invert, bool one_wire)
{
    ESP_LOGI(TAG, "Initialize SoftUart RX pin: %d, TX pin: %d, Speed: %ld, Invert: %d, One-wire: %d", rx_pin, tx_pin, speed, invert, one_wire);
    SoftUart::rx_pin = rx_pin;
    SoftUart::tx_pin = tx_pin;
    SoftUart::speed = speed;
    SoftUart::invert = invert;
    SoftUart::one_wire = one_wire;

    if (speed == 0)
    {
        ESP_LOGE(TAG, "speed cannot be zero. panicking!");
        abort();
    }

    rx_q = xQueueCreate(BYTE_Q_BUF_SZ, sizeof(uint8_t));
    if (!rx_q)
    {
        ESP_LOGE(TAG, "could not create rx byte queue. panicking!");
        abort();
    }

    rx_isr_q = xQueueCreate(ISR_Q_BUF_SZ, sizeof(ISREvent));
    if (!rx_q)
    {
        ESP_LOGE(TAG, "could not create rx isr queue. panicking!");
        abort();
    }

    tx_q = xQueueCreate(BYTE_Q_BUF_SZ, sizeof(uint8_t));
    if (!tx_q)
    {
        ESP_LOGE(TAG, "could not create tx byte queue. panicking!");
        abort();
    }

    tx_flag = xSemaphoreCreateBinary();
    if (!tx_flag)
    {
        ESP_LOGE(TAG, "could not create tx flag. panicking!");
        abort();
    }

    // install gpio isr service
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    // Calculate bit_time
    bit_time_us = (1000000 / speed);
    if (((100000000 / speed) - (100 * bit_time_us)) > 50)
    {
        bit_time_us++;
    }
    ESP_LOGI(TAG, "bit time is %lu", bit_time_us);

    // Setup Rx
    ESP_ERROR_CHECK(gpio_set_direction(rx_pin, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(rx_pin, GPIO_PULLUP_ONLY));

    // Setup Tx
    ESP_ERROR_CHECK(gpio_set_direction(tx_pin, GPIO_MODE_OUTPUT_OD));
    ESP_ERROR_CHECK(gpio_set_pull_mode(tx_pin, GPIO_PULLUP_ONLY));

#ifdef ISR_DEBUG
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(GPIO_NUM_16, GPIO_PULLUP_ONLY);
#endif

    // IDLE high
    ESP_ERROR_CHECK(gpio_set_level(tx_pin, !invert));

    // set up the hardware timer, readying it for activation
    ESP_LOGI(TAG, "Setting up hw_timer for %luus", bit_time_us);

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1MHz, 1 tick=1us //
        .intr_priority = 0,       // NO IDEA what this should be, no documentation anywhere.
        .flags{
            .intr_shared = 0,
            .backup_before_sleep = 0,
        },
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = handle_tx,
    };

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = bit_time_us, // period = 500mss @resolution 1MHz
        .reload_count = 0,          // counter will reload with 0 on alarm event
        .flags = {
            .auto_reload_on_alarm = true, // enable auto-reload
        },
    };

    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, (void *)this));
    ESP_LOGI(TAG, "Enable timer: 0x%08X", (int)gptimer);
    ESP_ERROR_CHECK(gptimer_enable(gptimer));

    /*
        hw_timer_init(handle_tx, (void *)this);
        hw_timer_set_clkdiv(TIMER_CLKDIV_16);
        hw_timer_set_intr_type(TIMER_EDGE_INT);

        uint32_t hw_timer_load_data = ((TIMER_BASE_CLK >> hw_timer_get_clkdiv()) / 1000000) * bit_time_us;
        ESP_LOGD(TAG, "hw_timer_load_data is %lu", hw_timer_load_data);
        hw_timer_set_load_data(hw_timer_load_data);
        hw_timer_set_reload(true);
    */
    // Setup the interrupt handler to get edges
    ESP_LOGI(TAG, "setting up gpio intr for pin %d", rx_pin);
    ESP_ERROR_CHECK(gpio_set_intr_type(rx_pin, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(rx_pin, reinterpret_cast<void (*)(void *)>(handle_rx_edge), (void *)this));

    ESP_LOGI(TAG, "Create ISR handler task");
    xTaskCreate(
        reinterpret_cast<void (*)(void *)>(rx_isr_handler_entry),
        RX_ISR_TASK_NAME,
        RX_ISR_TASK_STK_SZ,
        this,
        RX_ISR_TASK_PRIO,
        &rx_task);
    return true;
};

bool SoftUart::transmit(uint8_t bytebuf[], size_t len)
{
    ESP_LOGD(TAG, "sending %d bytes", len);
    if (tx_state == State::Idle)
    {

        // move the state machine
        tx_state = State::Start;

        if (one_wire)
        {
            // TODO - test this disable reception
            gpio_set_intr_type(this->rx_pin, GPIO_INTR_DISABLE);
        }
        // wake us up in one bit width and start sending bits. this results in a one-bit-width
        // delay before starting but makes the state machine more elegant
        // hw_timer_enable(true);
        ESP_ERROR_CHECK(gptimer_start(gptimer));
        // save the byte to be transmitted
        tx_byte = bytebuf[0];
        // queue the rest, skipping the first byte
        for (size_t i = 1; i < len; i++)
        {
            xQueueSendToBack(tx_q, &bytebuf[i], 0);
        }
        ESP_LOGI(TAG, "queued bytes, starting transmission");
    }
    else
    {
        ESP_LOGE(TAG, "invalid state at tx start %d. abandoning tx", (uint8_t)tx_state);
        return false;
    }

    bool ret = true;

    // now block and wait for the tx flag to get set
    if (!xSemaphoreTake(tx_flag, pdMS_TO_TICKS(500)))
    {
        ESP_LOGE(TAG, "transmission of %d bytes never succeeded", len);
        ret = false;
    }

    // re-enable reception
    gpio_set_intr_type(rx_pin, GPIO_INTR_ANYEDGE);

    return ret;
}

void SoftUart::process_isr(ISREvent &e)
{
    int64_t ticks = e.ticks - last_isr_ticks;

    // calculate how many bit periods it's been since the last edge
    uint32_t bits = ticks / bit_time_us;
    if ((ticks % bit_time_us) > (bit_time_us / 2))
    {
        bits += 1;
    }

    // inspect each bit period, moving the state machine accordingly
    while (bits)
    {

        switch (rx_state)
        {

        case State::Idle:
        {
            if (e.level == last_isr_level)
            {
                // nothing has changed since the last interrupt, which means this is a
                // timeout when we were already idle. we can just break out of it
                // instead of continuing to loop for no reason
                bits = 1; // decremented to zero at the end of the loop
                break;
            }

            // if this is the last bit period before a low, then we're entering the
            // Start state
            if ((e.level ^ invert) == false && bits == 1)
            {
                rx_state = State::Start;
                rx_bit_count = 0;
            }
            break;
        }

        case State::Start:
        {
            if (last_isr_level ^ invert)
            {
                // start cannot be a logic HIGH. how did we even get here?
                rx_state = State::Idle;
                rx_bit_count = 0;
            }
            else
            {
                // this bit period carries no value so we move the state machine and
                // that's it
                rx_state = State::Data;
            }
            break;
        }

        case State::Data:
        {
            // make room for the incoming bit
            rx_byte >>= 1;

            // take the prior level as the value for this bit period which occurred
            // before the edge (i.e. level change)
            if (last_isr_level ^ invert)
            {
                rx_byte |= 0x80;
            }
            rx_bit_count += 1;

            // if that was the 8th bit, we're done with data bits and the next is a STOP
            // bit
            if (rx_bit_count == 8)
            {
                rx_state = State::Stop;
            }
            break;
        }

        case State::Stop:
        {
            // if the value during this bit period was logic-high, there's
            // presumably no framing error and we should keep the byte
            if (last_isr_level ^ invert)
            {
                ESP_LOGI(TAG, "byte complete %02X", rx_byte);
                xQueueSendToBack(rx_q, &rx_byte, 0);
                rx_byte = 0;
                rx_bit_count = 0;
            }

            // if this is the last bit period before the detected edge, then we're
            // entering the Start state
            if (bits == 1)
            {
                rx_state = State::Start;
            }
            else
            {
                // if we still have more bit periods to go, the we idle for a bit
                rx_state = State::Idle;
            }

            break;
        }

        } // switch (rx_state)

        // consume the bit period
        bits -= 1;
    } // while (bits)

    last_isr_ticks = e.ticks;
    last_isr_level = e.level;
}
