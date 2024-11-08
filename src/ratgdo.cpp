
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
#include <time.h>

// ESP system includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <driver/uart.h>
#include <nvs_flash.h>

// RATGDO project includes
#include "utilities.h"
#include "ratgdo.h"
#include "wifi.h"
#include "tasks.h"
#include "config.h"
#include "homekit.h"
#include "comms.h"
#include "web.h"
#include "log.h"

// Logger tag
static const char *TAG = "ratgdo";

time_t now = 0;
tm timeInfo;
/********************************* FWD DECLARATIONS *****************************************/

void setup_pins();
void IRAM_ATTR isr_obstruction();
void service_timer_loop();

/********************************* RUNTIME STORAGE *****************************************/

struct obstruction_sensor_t
{
    unsigned int low_count = 0;    // count obstruction low pulses
    unsigned long last_asleep = 0; // count time between high pulses from the obst ISR
} obstruction_sensor;


extern bool flashCRC;

struct GarageDoor garage_door;

bool status_done = false;
unsigned long status_timeout;

/********************************** MAIN LOOP CODE *****************************************/

extern "C" void setup_ratgdo(void)
{
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, 115200));
    ESP_LOGI(TAG, "RATGDO main app starting");

    #ifdef LOG_MSG_BUFFER
    // Intercept all log messages so we can send to syslog and browser
    mutexLogger = xSemaphoreCreateMutex();
    systemLogFn = esp_log_set_vprintf(&logToBuffer);
    #endif

    load_all_config_settings();

    // core setup
    ESP_ERROR_CHECK(nvs_flash_init());
    // esp_set_cpu_freq(ESP_CPU_FREQ_160M); // returns void
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(wifi_task_entry, WIFI_TASK_NAME, WIFI_TASK_STK_SZ, NULL, WIFI_TASK_PRIO, NULL);

    // setup_pins();

    xTaskCreate(comms_task_entry, COMMS_TASK_NAME, COMMS_TASK_STK_SZ, NULL, COMMS_TASK_PRIO, NULL);

    xTaskCreate(homekit_task_entry, HOMEKIT_TASK_NAME, HOMEKIT_TASK_STK_SZ, NULL, HOMEKIT_TASK_PRIO, NULL);

    setup_web();

    ESP_LOGI(TAG, "RATGDO setup completed");
    // ESP_LOGI(TAG, "Starting RATGDO Homekit version %s", "esptest"); // TODO
    ESP_LOGI(TAG, "%s", IDF_VER);
}
