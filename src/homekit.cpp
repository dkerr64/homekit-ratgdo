// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License

#include <arduino_homekit_server.h>
#include <ESP8266WiFi.h>

#include "ratgdo.h"
#include "config.h"
#include "comms.h"
#include "web.h"

// Logger tag
static const char *TAG = "ratgdo-homekit";

// Forward-declare setters used by characteristics
homekit_value_t current_door_state_get();
homekit_value_t target_door_state_get();
void target_door_state_set(const homekit_value_t new_value);
homekit_value_t obstruction_detected_get();
homekit_value_t active_state_get();
homekit_value_t current_lock_state_get();
homekit_value_t target_lock_state_get();
void target_lock_state_set(const homekit_value_t new_value);
homekit_value_t light_state_get();
void light_state_set(const homekit_value_t value);

/********************************** MAIN LOOP CODE *****************************************/

void homekit_loop()
{
    arduino_homekit_loop();
}

void setup_homekit()
{
    ESP_LOGI(TAG, "=== Starting HomeKit Server");
    String macAddress = WiFi.macAddress();
    snprintf(serial_number, SERIAL_NAME_SIZE, "%s", macAddress.c_str());

    current_door_state.getter = current_door_state_get;
    target_door_state.getter = target_door_state_get;
    target_door_state.setter = target_door_state_set;
    obstruction_detected.getter = obstruction_detected_get;
    active_state.getter = active_state_get;
    current_lock_state.getter = current_lock_state_get;
    target_lock_state.getter = target_lock_state_get;
    target_lock_state.setter = target_lock_state_set;
    light_state.getter = light_state_get;
    light_state.setter = light_state_set;

    garage_door.has_motion_sensor = (bool)read_int_from_file(nvram_has_motion);
    if (!garage_door.has_motion_sensor && (userConfig->getMotionTriggers() == 0))
    {
        ESP_LOGI(TAG, "Motion Sensor not detected.  Disabling Service");
        config.accessories[0]->services[3] = NULL;
    }
    if (userConfig->getGDOSecurityType() == 3)
    {
        ESP_LOGI(TAG, "Dry contact does not support light control.  Disabling Service");
        config.accessories[0]->services[2] = NULL;
    }

    // We can set current lock state to unknown as HomeKit has value for that.
    // But we can't do the same for door state as HomeKit has no value for that.
    garage_door.current_lock = CURR_UNKNOWN;
    arduino_homekit_setup(&config);
    IRAM_START
    // Doing a IRAM start/end just to log free memory
    IRAM_END("HomeKit server started");
}

/******************************** GETTERS AND SETTERS ***************************************/

homekit_value_t current_door_state_get()
{
    ESP_LOGI(TAG, "get current door state: %d", garage_door.current_state);

    return HOMEKIT_UINT8_CPP(garage_door.current_state);
}

homekit_value_t target_door_state_get()
{
    ESP_LOGI(TAG, "get target door state: %d", garage_door.target_state);

    return HOMEKIT_UINT8_CPP(garage_door.target_state);
}

void target_door_state_set(const homekit_value_t value)
{
    ESP_LOGI(TAG, "set door state: %d", value.uint8_value);

    switch (value.uint8_value)
    {
    case TGT_OPEN:
        open_door();
        break;
    case TGT_CLOSED:
        close_door();
        break;
    default:
        ERROR("invalid target door state set requested: %d", value.uint8_value);
        break;
    }
}

homekit_value_t obstruction_detected_get()
{
    ESP_LOGI(TAG, "get obstruction: %d", garage_door.obstructed);
    return HOMEKIT_BOOL_CPP(garage_door.obstructed);
}

homekit_value_t active_state_get()
{
    ESP_LOGI(TAG, "get active: %d", garage_door.active);
    return HOMEKIT_BOOL_CPP(garage_door.active);
}

homekit_value_t current_lock_state_get()
{
    ESP_LOGI(TAG, "get current lock state: %d", garage_door.current_lock);

    return HOMEKIT_UINT8_CPP(garage_door.current_lock);
}

homekit_value_t target_lock_state_get()
{
    ESP_LOGI(TAG, "get target lock state: %d", garage_door.target_lock);

    return HOMEKIT_UINT8_CPP(garage_door.target_lock);
}

void target_lock_state_set(const homekit_value_t value)
{
    ESP_LOGI(TAG, "set lock state: %d", value.uint8_value);

    set_lock(value.uint8_value);
}

/****************************************************************************
 * Garage Door Service Handler
 */
void notify_homekit_target_door_state_change(GarageDoorTargetState state)
{
    garage_door.target_state = state;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->target;
    e.value.u = (uint8_t)garage_door.target_state;
    queueSendHelper(door->event_q, e, "target door");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&target_door_state, HOMEKIT_UINT8_CPP(garage_door.target_state));
#endif
}

void notify_homekit_current_door_state_change(GarageDoorCurrentState state)
{
    garage_door.current_state = state;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->current;
    e.value.u = (uint8_t)garage_door.current_state;
    queueSendHelper(door->event_q, e, "current door");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&current_door_state, HOMEKIT_UINT8_CPP(garage_door.current_state));

#define doorOpening() // Noop on ESP8266
#define doorClosing() // Noop on ESP8266
#endif
    // Set target door state to match.
    switch (state)
    {
    case CURR_OPENING:
        // #ifdef ESP32
        doorOpening(); // Fall through...
                       // #endif
    case CURR_OPEN:
        notify_homekit_target_door_state_change(TGT_OPEN);
        break;

    case CURR_CLOSING:
        // #ifdef ESP32
        doorClosing(); // Fall through...
                       // #endif
    case CURR_CLOSED:
        notify_homekit_target_door_state_change(TGT_CLOSED);
        break;

    default:
        // Ignore other states.
        break;
    }
}

#ifndef ESP32
void notify_homekit_active()
{
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&active_state, HOMEKIT_BOOL_CPP(true));
}

homekit_value_t light_state_get()
{
    ESP_LOGI(TAG, "get light state: %s", garage_door.light ? "On" : "Off");

    return HOMEKIT_BOOL_CPP(garage_door.light);
}

void light_state_set(const homekit_value_t value)
{
    ESP_LOGI(TAG, "set light: %s", value.bool_value ? "On" : "Off");

    set_light(value.bool_value);
}
#endif

void notify_homekit_target_lock(LockTargetState state)
{
    garage_door.target_lock = state;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->lockTarget;
    e.value.u = (uint8_t)garage_door.target_lock;
    queueSendHelper(door->event_q, e, "target lock");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&target_lock_state, HOMEKIT_UINT8_CPP(garage_door.target_lock));
#endif
}

void notify_homekit_current_lock(LockCurrentState state)
{
    garage_door.current_lock = state;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->lockCurrent;
    e.value.u = (uint8_t)garage_door.current_lock;
    queueSendHelper(door->event_q, e, "current lock");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&current_lock_state, HOMEKIT_UINT8_CPP(garage_door.current_lock));
#endif
}

void notify_homekit_obstruction(bool state)
{
    garage_door.obstructed = state;
#ifdef ESP32
    if (!isPaired)
        return;

    GDOEvent e;
    e.c = door->obstruction;
    e.value.b = garage_door.obstructed;
    queueSendHelper(door->event_q, e, "obstruction");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&obstruction_detected, HOMEKIT_BOOL_CPP(garage_door.obstructed));
#endif
}

/****************************************************************************
 * Light Service Handler
 */
void notify_homekit_light(bool state)
{
    garage_door.light = state;
#ifdef ESP32
    if (!isPaired || !light)
        return;

    GDOEvent e;
    e.value.b = garage_door.light;
    queueSendHelper(light->event_q, e, "light");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&light_state, HOMEKIT_BOOL_CPP(garage_door.light));
#endif
}

/****************************************************************************
 * Motion Service Handler
 */
void enable_service_homekit_motion(bool reboot)
{
#ifdef ESP32
    // only create if not already created
    if (!garage_door.has_motion_sensor)
    {
        nvRam->write(nvram_has_motion, 1);
        garage_door.has_motion_sensor = true;
        createMotionAccessories();
        if (reboot)
        {
            sync_and_restart();
        }
    }
#else
    write_int_to_file(nvram_has_motion, 1);
    if (reboot)
    {
        sync_and_restart();
    }
#endif
}

void notify_homekit_motion(bool state)
{
    garage_door.motion = state;
    garage_door.motion_timer = (!state) ? 0 : _millis() + MOTION_TIMER_DURATION;
#ifdef ESP32
    if (!isPaired || !motion)
        return;

    GDOEvent e;
    e.value.b = garage_door.motion;
    queueSendHelper(motion->event_q, e, "motion");
#else
    if (!arduino_homekit_get_running_server())
        return;

    homekit_characteristic_notify(&motion_detected, HOMEKIT_BOOL_CPP(garage_door.motion));
#endif
}
