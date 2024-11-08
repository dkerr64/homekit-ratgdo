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
#include <random>

// ESP system includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <driver/uart.h>
#include <esp_system.h>
#include <esp_log.h>
#include <rom/ets_sys.h>

// RATGDO project includes
#include "config.h"
#include "ratgdo.h"
#include "homekit.h"
#include "Reader.h"
#include "secplus2.h"
#include "Packet.h"
#include "log.h"
#include "utilities.h"
#include "comms.h"
#include "web.h"
#include "led.h"
// #include "softuart.h"

#undef TAG
static const char *TAG = "ratgdo-comms";

/********************************** LOCAL STORAGE *****************************************/

struct PacketAction
{
    Packet pkt;
    bool inc_counter;
    uint32_t delay;
};

QueueHandle_t pkt_q;
extern struct GarageDoor garage_door;
extern bool status_done;

extern TimerHandle_t led_on_timer;
extern TimerHandle_t motion_timer;

// For Time-to-close control
TimerHandle_t TTCtimer = NULL;
uint8_t TTCcountdown = 0;
bool TTCwasLightOn = false;
void (*TTC_Action)(void) = NULL;

struct ForceRecover force_recover;
#define force_recover_delay 3

#define UART_BUF_SZ (256)
#define UART_EVT_Q_SZ (8)
static QueueHandle_t uart2_queue;
uint8_t uartBuf[UART_BUF_SZ];

static QueueSetHandle_t commsQueue;

/******************************* SECURITY 2.0 *********************************/

SecPlus2Reader reader;
uint32_t id_code = 0;
uint32_t rolling_code = 0;
uint32_t last_saved_code = 0;
#define MAX_CODES_WITHOUT_FLASH_WRITE 10

/******************************* SECURITY 1.0 *********************************/

static const uint8_t RX_LENGTH = 2;
typedef uint8_t RxPacket[RX_LENGTH * 4];
unsigned long last_rx;
unsigned long last_tx;

bool wallplateBooting = false;
bool wallPanelDetected = false;
DoorState doorState = DoorState::Unknown;
uint8_t lightState;
uint8_t lockState;

// keep this here incase at somepoint its needed
// it is used for emulation of wall panel
// byte secplus1States[19] = {0x35,0x35,0x35,0x35,0x33,0x33,0x53,0x53,0x38,0x3A,0x3A,0x3A,0x39,0x38,0x3A, 0x38,0x3A,0x39,0x3A};
// this is what MY 889LM exhibited when powered up (release of all buttons, and then polls)
uint8_t secplus1States[] = {0x35, 0x35, 0x33, 0x33, 0x38, 0x3A, 0x39};

// values for SECURITY+1.0 communication
enum secplus1Codes : uint8_t
{
    DoorButtonPress = 0x30,
    DoorButtonRelease = 0x31,
    LightButtonPress = 0x32,
    LightButtonRelease = 0x33,
    LockButtonPress = 0x34,
    LockButtonRelease = 0x35,

    Unkown_0x36 = 0x36,
    Unknown_0x37 = 0x37,

    DoorStatus = 0x38,
    ObstructionStatus = 0x39, // this is not proven
    LightLockStatus = 0x3A,
    Unknown = 0xFF
};

/*************************** FORWARD DECLARATIONS ******************************/

void sync();
void door_command(DoorAction action);
void send_get_status();

bool process_PacketAction(PacketAction &pkt_ac);
// bool transmitSec1(uint8_t toSend);
bool transmitSec2(PacketAction &pkt_ac);
void TTCdelayLoop(TimerHandle_t xTimer);
void manual_recovery();

/********************************** MAIN LOOP CODE *****************************************/

void comms_task_entry(void *ctx)
{
    printf("Stack 1: %d\n", uxTaskGetStackHighWaterMark(NULL));
    uint32_t baudRate = 9600;

    if (userConfig->getGDOSecurityType() == 1)
    {
        printf("Stack 1a: %d\n", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG, "=== Setting up comms for Secuirty+1.0 protocol");
        baudRate = 1200;
        wallPanelDetected = false;
        wallplateBooting = false;
        doorState = DoorState::Unknown;
        lightState = 2;
        lockState = 2;
    }
    else
    {
        printf("Stack 1b: %d\n", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG, "=== Setting up comms for Secuirty+2.0 protocol");

        // read from flash, default of 0 if file not exist
        id_code = nvRam->read("id_code");
        if (!id_code)
        {
            ESP_LOGW(TAG, "id code not found, generating new code");
            std::mt19937 rng(time(NULL));
            std::uniform_int_distribution<int> gen(0x01, 0xFFF); // uniform, unbiased
            id_code = (gen(rng) << 12) | 0x539;
            nvRam->write("id_code", id_code);
        }
        ESP_LOGI(TAG, "id code %lu (0x%02lX)", id_code, id_code);
        // read from flash, default of 0 if file not exist
        rolling_code = nvRam->read("rolling", 0);
        // last saved rolling code may be behind what the GDO thinks, so bump it up so that it will
        // always be ahead of what the GDO thinks it should be, and save it.
        rolling_code = (rolling_code != 0) ? rolling_code + MAX_CODES_WITHOUT_FLASH_WRITE : 0;
        save_rolling_code();
        ESP_LOGI(TAG, "rolling code %lu (0x%02lX)", rolling_code, rolling_code);
    }
    printf("Stack 2: %d\n", uxTaskGetStackHighWaterMark(NULL));

    // Create packet queue
    pkt_q = xQueueCreate(5, sizeof(PacketAction));
    printf("Stack 3: %d\n", uxTaskGetStackHighWaterMark(NULL));

    // Initialize our serial port
    uart_config_t uart_config = {};
    uart_config.baud_rate = baudRate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
    ESP_ERROR_CHECK(uart_set_line_inverse(UART_NUM_2, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, UART_BUF_SZ, 0, UART_EVT_Q_SZ, &uart2_queue, 0));
    //ESP_ERROR_CHECK(uart_set_mode(UART_NUM_2,UART_MODE_RS485_HALF_DUPLEX));
    //ESP_ERROR_CHECK(uart_set_loop_back(UART_NUM_2, false));

    commsQueue = xQueueCreateSet(16);
    xQueueAddToSet(uart2_queue, commsQueue);
    xQueueAddToSet(pkt_q, commsQueue);

    printf("Stack 4: %d\n", uxTaskGetStackHighWaterMark(NULL));

    if (userConfig->getGDOSecurityType() == 2)
    {
        ESP_LOGI(TAG, "Syncing rolling code counter after reboot...");
        sync();
        // Get the initial state of the door
        if (!gpio_get_level(UART_RX_PIN))
        {
            send_get_status();
        }
        force_recover.push_count = 0;
    }

    TTCtimer = xTimerCreate("TTCtimer", pdMS_TO_TICKS(500), pdTRUE, NULL, TTCdelayLoop);

    // comms loop has a while(true) so will never return
    comms_loop(userConfig->getGDOSecurityType());
    // comms loop has a while(true) so will never return
    return;
}

void save_rolling_code()
{
    nvRam->write("rolling", rolling_code);
    last_saved_code = rolling_code;
}

void reset_door()
{
    rolling_code = 0; // because sync_and_reboot writes this.
    nvRam->erase("rolling");
    nvRam->erase("id_code");
    nvRam->erase("has_motion");
}

/*
void wallPlate_Emulation()
{

    if (wallPanelDetected)
        return;

    unsigned long currentMillis = millis();
    static unsigned long lastRequestMillis = 0;
    static bool emulateWallPanel = false;
    static unsigned long serialDetected = 0;
    static uint8_t stateIndex = 0;

    if (!serialDetected)
    {
        if (sw_serial->available())
        {
            serialDetected = currentMillis;
        }

        return;
    }

    // wait up to 15 seconds to look for an existing wallplate or it could be booting, so need to wait
    if (currentMillis - serialDetected < 15000 || wallplateBooting == true)
    {
        if (currentMillis - lastRequestMillis > 1000)
        {
            ESP_LOGI(TAG, "Looking for security+ 1.0 DIGITAL wall panel...");
            lastRequestMillis = currentMillis;
        }

        if (!wallPanelDetected && (doorState != DoorState::Unknown || lightState != 2))
        {
            wallPanelDetected = true;
            wallplateBooting = false;
            ESP_LOGI(TAG, "DIGITAL Wall panel detected.");
            return;
        }
    }
    else
    {
        if (!emulateWallPanel && !wallPanelDetected)
        {
            emulateWallPanel = true;
            ESP_LOGI(TAG, "No DIGITAL wall panel detected. Switching to emulation mode.");
        }

        // transmit every 250ms
        if (emulateWallPanel && (currentMillis - lastRequestMillis) > 250)
        {
            lastRequestMillis = currentMillis;

            uint8_t secplus1ToSend = uint8_t(secplus1States[stateIndex]);

            // send through queue
            PacketData data;
            data.type = PacketDataType::Status;
            data.value.cmd = secplus1ToSend;
            Packet pkt = Packet(PacketCommand::GetStatus, data, id_code);
            PacketAction pkt_ac = {pkt, true, 20}; // 20ms delay for SECURITY1.0 (which is minimum delay)
            if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
            {
                ESP_LOGE(TAG, "packet queue full");
            }

            // send direct
            // transmitSec1( secplus1ToSend);

            stateIndex++;
            if (stateIndex == sizeof(secplus1States))
            {
                stateIndex = sizeof(secplus1States) - 3;
            }
        }
    }
}

void comms_loop_sec1()
{
    static bool reading_msg = false;
    static uint16_t byte_count = 0;
    static RxPacket rx_packet;
    bool gotMessage = false;

    if (sw_serial->available())
    {
        uint8_t ser_byte;
        sw_serial->read(&ser_byte);
        last_rx = millis();

        if (!reading_msg)
        {
            // valid?
            if (ser_byte >= 0x30 && ser_byte <= 0x3A)
            {
                byte_count = 0;
                rx_packet[byte_count++] = ser_byte;
                reading_msg = true;
            }
            // is it single byte command?
            // really all commands are single byte
            // is it a button push or release? (FROM WALL PANEL)
            if (ser_byte >= 0x30 && ser_byte <= 0x37)
            {
                rx_packet[1] = 0;
                reading_msg = false;
                byte_count = 0;

                gotMessage = true;
            }
        }
        else
        {
            // save next byte
            rx_packet[byte_count++] = ser_byte;

            if (byte_count == RX_LENGTH)
            {
                reading_msg = false;
                byte_count = 0;

                gotMessage = true;
            }

            if (gotMessage == false && (millis() - last_rx) > 100)
            {
                ESP_LOGI(TAG, "RX message timeout");
                // if we have a partial packet and it's been over 100ms since last byte was read,
                // the rest is not coming (a full packet should be received in ~20ms),
                // discard it so we can read the following packet correctly
                reading_msg = false;
                byte_count = 0;
            }
        }
    }

    // got data?
    if (gotMessage)
    {
        gotMessage = false;

        // get kvp
        // button press/release have no val, just a single byte
        uint8_t key = rx_packet[0];
        uint8_t val = rx_packet[1];

        if (key == secplus1Codes::DoorButtonPress)
        {
            ESP_LOGI(TAG, "0x30 RX (door press)");
            manual_recovery();
            if (motionTriggers.bit.doorKey)
            {
                garage_door.motion_timer = millis() + 5000;
                garage_door.motion = true;
                notify_homekit_motion();
            }
        }
        // wall panel is sending out 0x31 (Door Button Release) when it starts up
        // but also on release of door button
        else if (key == secplus1Codes::DoorButtonRelease)
        {
            ESP_LOGI(TAG, "0x31 RX (door release)");

            // Possible power up of 889LM
            if ((DoorState)doorState == DoorState::Unknown)
            {
                wallplateBooting = true;
            }
        }
        else if (key == secplus1Codes::LightButtonPress)
        {
            ESP_LOGI(TAG, "0x32 RX (light press)");
            manual_recovery();
        }
        else if (key == secplus1Codes::LightButtonRelease)
        {
            ESP_LOGI(TAG, "0x33 RX (light release)");
        }

        // 2 byte status messages (0x38 - 0x3A)
        // its the byte sent out by the wallplate + the byte transmitted by the opener
        if (key == secplus1Codes::DoorStatus || key == secplus1Codes::ObstructionStatus || key == secplus1Codes::LightLockStatus)
        {

            // ESP_LOGI(TAG, "SEC1 STATUS MSG: %X%02X",key,val);

            switch (key)
            {
            // door status
            case secplus1Codes::DoorStatus:

                // ESP_LOGI(TAG, "0x38 MSG: %02X",val);

                // 0x5X = stopped
                // 0x0X = moving
                // best attempt to trap invalid values (due to collisions)
                if (((val & 0xF0) != 0x00) && ((val & 0xF0) != 0x50) && ((val & 0xF0) != 0xB0))
                {
                    ESP_LOGI(TAG, "0x38 val upper nible not 0x0 or 0x5 or 0xB: %02X", val);
                    break;
                }

                val = (val & 0x7);
                // 000 0x0 stopped
                // 001 0x1 opening
                // 010 0x2 open
                // 100 0x4 closing
                // 101 0x5 closed
                // 110 0x6 stopped

                // sec+1 doors sometimes report wrong door status
                // require two sequential matching door states
                // I have not seen this to be the case on my unit (MJS)
                static uint8_t prevDoor;
                if (prevDoor != val)
                {
                    prevDoor = val;
                    break;
                }

                switch (val)
                {
                case 0x00:
                    doorState = DoorState::Stopped;
                    break;
                case 0x01:
                    doorState = DoorState::Opening;
                    break;
                case 0x02:
                    doorState = DoorState::Open;
                    break;
                // no 0x03 known
                case 0x04:
                    doorState = DoorState::Closing;
                    break;
                case 0x05:
                    doorState = DoorState::Closed;
                    break;
                case 0x06:
                    doorState = DoorState::Stopped;
                    break;
                default:
                    doorState = DoorState::Unknown;
                    break;
                }

                // ESP_LOGI(TAG, "doorstate: %d", doorState);

                switch (doorState)
                {
                case DoorState::Open:
                    garage_door.current_state = CURR_OPEN;
                    garage_door.target_state = TGT_OPEN;
                    break;
                case DoorState::Closed:
                    garage_door.current_state = CURR_CLOSED;
                    garage_door.target_state = TGT_CLOSED;
                    break;
                case DoorState::Stopped:
                    garage_door.current_state = CURR_STOPPED;
                    garage_door.target_state = TGT_OPEN;
                    break;
                case DoorState::Opening:
                    garage_door.current_state = CURR_OPENING;
                    garage_door.target_state = TGT_OPEN;
                    break;
                case DoorState::Closing:
                    garage_door.current_state = CURR_CLOSING;
                    garage_door.target_state = TGT_CLOSED;
                    break;
                case DoorState::Unknown:
                    ESP_LOGE(TAG, "Got door state unknown");
                    break;
                }

                if ((garage_door.current_state == CURR_CLOSING) && (TTCcountdown > 0))
                {
                    // We are in a time-to-close delay timeout, cancel the timeout
                    ESP_LOGI(TAG, "Canceling time-to-close delay timer");
                    xTimerStop(TTCtimer, 100);
                    TTCcountdown = 0;
                }

                if (!garage_door.active)
                {
                    ESP_LOGI(TAG, "activating door");
                    garage_door.active = true;
                    notify_homekit_active();
                    if (garage_door.current_state == CURR_OPENING || garage_door.current_state == CURR_OPEN)
                    {
                        garage_door.target_state = TGT_OPEN;
                    }
                    else
                    {
                        garage_door.target_state = TGT_CLOSED;
                    }
                }

                static GarageDoorCurrentState gd_currentstate;
                if (garage_door.current_state != gd_currentstate)
                {
                    gd_currentstate = garage_door.current_state;

                    const char *l = "unknown door state";
                    switch (gd_currentstate)
                    {
                    case GarageDoorCurrentState::CURR_STOPPED:
                        l = "Stopped";
                        break;
                    case GarageDoorCurrentState::CURR_OPEN:
                        l = "Open";
                        break;
                    case GarageDoorCurrentState::CURR_OPENING:
                        l = "Opening";
                        break;
                    case GarageDoorCurrentState::CURR_CLOSED:
                        l = "Closed";
                        break;
                    case GarageDoorCurrentState::CURR_CLOSING:
                        l = "Closing";
                        break;
                    }
                    ESP_LOGI(TAG, "status DOOR: %s", l);

                    notify_homekit_current_door_state_change();
                }

                static GarageDoorTargetState gd_TargetState;
                if (garage_door.target_state != gd_TargetState)
                {
                    gd_TargetState = garage_door.target_state;
                    notify_homekit_target_door_state_change();
                }

                break;

            // objstruction states (not confirmed)
            case secplus1Codes::ObstructionStatus:
                // currently not using
                break;

            // light & lock
            case secplus1Codes::LightLockStatus:

                // ESP_LOGI(TAG, "0x3A MSG: %X%02X",key,val);

                // upper nibble must be 5
                if ((val & 0xF0) != 0x50)
                {
                    ESP_LOGI(TAG, "0x3A val upper nible not 5: %02X", val);
                    break;
                }

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
                lightState = bitRead(val, 2);
                lockState = !bitRead(val, 3);

                // light status
                static uint8_t lastLightState = 0xff;
                // light state change?
                if (lightState != lastLightState)
                {
                    ESP_LOGI(TAG, "status LIGHT: %s", lightState ? "On" : "Off");
                    lastLightState = lightState;

                    garage_door.light = (bool)lightState;
                    notify_homekit_light();
                    if (motionTriggers.bit.lightKey)
                    {
                        garage_door.motion_timer = millis() + 5000;
                        garage_door.motion = true;
                        notify_homekit_motion();
                    }
                }

                // lock status
                static uint8_t lastLockState = 0xff;
                // lock state change?
                if (lockState != lastLockState)
                {
                    ESP_LOGI(TAG, "status LOCK: %s", lockState ? "Secured" : "Unsecured");
                    lastLockState = lockState;

                    if (lockState)
                    {
                        garage_door.current_lock = CURR_LOCKED;
                        garage_door.target_lock = TGT_LOCKED;
                    }
                    else
                    {
                        garage_door.current_lock = CURR_UNLOCKED;
                        garage_door.target_lock = TGT_UNLOCKED;
                    }
                    notify_homekit_target_lock();
                    notify_homekit_current_lock();
                    if (motionTriggers.bit.lockKey)
                    {
                        garage_door.motion_timer = millis() + 5000;
                        garage_door.motion = true;
                        notify_homekit_motion();
                    }
                }

                break;
            }
        }
    }

    //
    // PROCESS TRANSMIT QUEUE
    //
    PacketAction pkt_ac;
    static unsigned long cmdDelay = 0;
    unsigned long now;
    bool okToSend = false;

    if (uxQueueMessagesWaiting(pkt_q) > 0)
    {

        now = millis();

        // if there is no wall panel, no need to check 200ms since last rx
        // (yes some duped code here, but its clearer)
        if (!wallPanelDetected)
        {
            // no wall panel
            okToSend = (now - last_rx > 20);        // after 20ms since last rx
            okToSend &= (now - last_tx > 20);       // after 20ms since last tx
            okToSend &= (now - last_tx > cmdDelay); // after any command delays
        }
        else
        {
            // digitial wall pnael
            okToSend = (now - last_rx > 20);        // after 20ms since last rx
            okToSend &= (now - last_rx < 200);      // before 200ms since last rx
            okToSend &= (now - last_tx > 20);       // after 20ms since last tx
            okToSend &= (now - last_tx > cmdDelay); // after any command delays
        }

        // OK to send based on above rules
        if (okToSend)
        {

            if (xQueuePeek(pkt_q, &pkt_ac, 0))
            {
                if (process_PacketAction(pkt_ac))
                {
                    // get next delay "between" transmits
                    cmdDelay = pkt_ac.delay;
                    xQueueReceive(pkt_q, &pkt_ac, 0);
                }
                else
                {
                    cmdDelay = 0;
                    ESP_LOGE(TAG, "transmit failed, will retry");
                }
            }
        }
    }

    // check for wall panel and provide emulator
    wallPlate_Emulation();
}
*/
void comms_loop_sec2()
{
    // no incoming data, check if we have command queued
    // Block until either data on the UART queue or the Packet queue.
    QueueSetMemberHandle_t commsEvent = xQueueSelectFromSet(commsQueue, portMAX_DELAY);

    if (commsEvent == pkt_q)
    {
        ESP_LOGD(TAG, "packet ready for tx");
        PacketAction pkt_ac;
        xQueueReceive(pkt_q, &pkt_ac, 0); // ignore errors
        if (!transmitSec2(pkt_ac))
        {
            ESP_LOGE(TAG, "transmit failed, will retry");
            xQueueSendToFront(pkt_q, &pkt_ac, 0); // ignore errors
        }
    }
    else
    {
        // spin on receiving data until the whole packet has arrived
        uart_event_t event = {};
        if (xQueueReceive(uart2_queue, (void *)&event, 0) && (event.type == UART_DATA))
        {
            uart_read_bytes(UART_NUM_2, uartBuf, event.size, portMAX_DELAY);
            ESP_LOGI(TAG, "UART read %d bytes", event.size);

            for (size_t i = 0; i < event.size; i++)
            {
                if (!reader.push_byte(uartBuf[i]))
                    continue;

                // We have a complete packet.
                led->flash();
                Packet pkt = Packet(reader.fetch_buf());
                pkt.print();

                switch (pkt.m_pkt_cmd)
                {
                case PacketCommand::Status:
                {
                    GarageDoorCurrentState current_state = garage_door.current_state;
                    GarageDoorTargetState target_state = garage_door.target_state;
                    switch (pkt.m_data.value.status.door)
                    {
                    case DoorState::Open:
                        current_state = CURR_OPEN;
                        target_state = TGT_OPEN;
                        break;
                    case DoorState::Closed:
                        current_state = CURR_CLOSED;
                        target_state = TGT_CLOSED;
                        break;
                    case DoorState::Stopped:
                        current_state = CURR_STOPPED;
                        target_state = TGT_OPEN;
                        break;
                    case DoorState::Opening:
                        current_state = CURR_OPENING;
                        target_state = TGT_OPEN;
                        break;
                    case DoorState::Closing:
                        current_state = CURR_CLOSING;
                        target_state = TGT_CLOSED;
                        break;
                    case DoorState::Unknown:
                        ESP_LOGE(TAG, "Got door state unknown");
                        break;
                    }

                    if ((current_state == CURR_CLOSING) && (TTCcountdown > 0))
                    {
                        // We are in a time-to-close delay timeout, cancel the timeout
                        ESP_LOGI(TAG, "Canceling time-to-close delay timer");
                        // TTCtimer.detach();
                        TTCcountdown = 0;
                    }

                    if (!garage_door.active)
                    {
                        ESP_LOGI(TAG, "activating door");
                        garage_door.active = true;
                        notify_homekit_active();
                        if (current_state == CURR_OPENING || current_state == CURR_OPEN)
                        {
                            target_state = TGT_OPEN;
                        }
                        else
                        {
                            target_state = TGT_CLOSED;
                        }
                    }

                    ESP_LOGI(TAG, "tgt %d curr %d", target_state, current_state);

                    if ((target_state != garage_door.target_state) ||
                        (current_state != garage_door.current_state))
                    {
                        garage_door.target_state = target_state;
                        garage_door.current_state = current_state;

                        notify_homekit_current_door_state_change();
                        notify_homekit_target_door_state_change();
                    }

                    if (pkt.m_data.value.status.light != garage_door.light)
                    {
                        ESP_LOGI(TAG, "Light Status %s", pkt.m_data.value.status.light ? "On" : "Off");
                        garage_door.light = pkt.m_data.value.status.light;
                        notify_homekit_light();
                    }

                    LockCurrentState current_lock;
                    LockTargetState target_lock;
                    if (pkt.m_data.value.status.lock)
                    {
                        current_lock = CURR_LOCKED;
                        target_lock = TGT_LOCKED;
                    }
                    else
                    {
                        current_lock = CURR_UNLOCKED;
                        target_lock = TGT_UNLOCKED;
                    }
                    if (current_lock != garage_door.current_lock)
                    {
                        garage_door.target_lock = target_lock;
                        garage_door.current_lock = current_lock;
                        notify_homekit_target_lock();
                        notify_homekit_current_lock();
                    }

                    status_done = true;
                    break;
                }

                case PacketCommand::Lock:
                {
                    LockTargetState lock = garage_door.target_lock;
                    switch (pkt.m_data.value.lock.lock)
                    {
                    case LockState::Off:
                        lock = TGT_UNLOCKED;
                        break;
                    case LockState::On:
                        lock = TGT_LOCKED;
                        break;
                    case LockState::Toggle:
                        if (lock == TGT_LOCKED)
                        {
                            lock = TGT_UNLOCKED;
                        }
                        else
                        {
                            lock = TGT_LOCKED;
                        }
                        break;
                    }
                    if (lock != garage_door.target_lock)
                    {
                        ESP_LOGI(TAG, "Lock Cmd %d", lock);
                        garage_door.target_lock = lock;
                        notify_homekit_target_lock();
                        if (motionTriggers.bit.lockKey)
                        {
                            garage_door.motion_timer = millis() + 5000;
                            garage_door.motion = true;
                            notify_homekit_motion();
                        }
                    }
                    // Send a get status to make sure we are in sync
                    send_get_status();
                    break;
                }

                case PacketCommand::Light:
                {
                    bool l = garage_door.light;
                    manual_recovery();
                    switch (pkt.m_data.value.light.light)
                    {
                    case LightState::Off:
                        l = false;
                        break;
                    case LightState::On:
                        l = true;
                        break;
                    case LightState::Toggle:
                    case LightState::Toggle2:
                        l = !garage_door.light;
                        break;
                    }
                    if (l != garage_door.light)
                    {
                        ESP_LOGI(TAG, "Light Cmd %s", l ? "On" : "Off");
                        garage_door.light = l;
                        notify_homekit_light();
                        if (motionTriggers.bit.lightKey)
                        {
                            garage_door.motion_timer = millis() + 5000;
                            garage_door.motion = true;
                            notify_homekit_motion();
                        }
                    }
                    // Send a get status to make sure we are in sync
                    // Should really only need to do this on a toggle,
                    // But safer to do it always
                    send_get_status();
                    break;
                }

                case PacketCommand::Motion:
                {
                    ESP_LOGI(TAG, "Motion Detected");
                    // We got a motion message, so we know we have a motion sensor
                    // If it's not yet enabled, add the service
                    if (!garage_door.has_motion_sensor)
                    {
                        ESP_LOGI(TAG, "Detected new Motion Sensor. Enabling Service");
                        garage_door.has_motion_sensor = true;
                        motionTriggers.bit.motion = 1;
                        userConfig->set(cfg_motionTriggers, motionTriggers.asInt);
                        userConfig->save();
                        // Only reboot if we had not already other motionTriggers (which would have enabled service already)
                        enable_service_homekit_motion(motionTriggers.asInt == 1);
                    }

                    /* When we get the motion detect message, notify HomeKit. Motion sensor
                        will continue to send motion messages every 5s until motion stops.
                        set a timer for 5 seconds to disable motion after the last message */
                    garage_door.motion_timer = millis() + 5000;
                    if (!garage_door.motion)
                    {
                        garage_door.motion = true;
                        notify_homekit_motion();
                    }
                    // Update status because things like light may have changed states
                    send_get_status();
                    break;
                }

                case PacketCommand::DoorAction:
                {
                    ESP_LOGI(TAG, "Door Action");
                    if (pkt.m_data.value.door_action.pressed)
                    {
                        manual_recovery();
                    }
                    if (pkt.m_data.value.door_action.pressed && motionTriggers.bit.doorKey)
                    {
                        garage_door.motion_timer = millis() + 5000;
                        garage_door.motion = true;
                        notify_homekit_motion();
                    }
                    break;
                }

                default:
                    ESP_LOGI(TAG, "Support for %s packet unimplemented. Ignoring.", PacketCommand::to_string(pkt.m_pkt_cmd));
                    break;
                }
            }
        }
    }

    // Save rolling code if we have exceeded max limit.
    if (rolling_code >= (last_saved_code + MAX_CODES_WITHOUT_FLASH_WRITE))
    {
        save_rolling_code();
    }
}

void comms_loop(const uint32_t secType)
{
    while (true)
    {
        if (secType == 1)
        {
            // comms_loop_sec1();
        }
        else
        {
            // vTaskDelay(pdMS_TO_TICKS(500));
            comms_loop_sec2();
        }
    }
}

/********************************** CONTROLLER CODE *****************************************/
// SECURITY+1.0
/*
bool transmitSec1(uint8_t toSend)
{
    // safety
    if (gpio_get_level(UART_RX_PIN) || sw_serial->available())
    {
        return false;
    }

    // sending a poll?
    bool poll_cmd = (toSend == 0x38) || (toSend == 0x39) || (toSend == 0x3A);
    // if not a poll command (and polls only with wall planel emulation),
    // disable disable rx (allows for cleaner tx, and no echo)
    if (!poll_cmd)
    {
        // TODO sw_serial->enableRx(false);
    }

    sw_serial->transmit(toSend);
    last_tx = millis();

    // ESP_LOGI(TAG, "SEC1 SEND BYTE: %02X",toSend);

    // re-enable rx
    if (!poll_cmd)
    {
        // TODO sw_serial->enableRx(true);
    }

    return true;
}
*/
// SECURITY+2.0
bool transmitSec2(PacketAction &pkt_ac)
{
    // inverted logic, so this pulls the bus low to assert it
    // gpio_set_level(UART_TX_PIN, true);
    // ets_delay_us(1300); // No other task can run, is that what we want?
    // gpio_set_level(UART_TX_PIN, false);
    // ets_delay_us(130); // No other task can run, is that what we want?

    // check to see if anyone else is continuing to assert the bus after we have released it
    /* TODO FIXME
    if (gpio_get_level(UART_RX_PIN))
    {
        ESP_LOGI(TAG, "Collision detected, waiting to send packet");
        return false;
    }
    */
    uint8_t buf[SECPLUS2_CODE_LEN];
    if (pkt_ac.pkt.encode(rolling_code, buf) != 0)
    {
        ESP_LOGE(TAG, "Could not encode packet");
        pkt_ac.pkt.print();
    }
    else
    {
        // sw_serial->transmit(buf, SECPLUS2_CODE_LEN);
        ESP_LOGI(TAG, "Send packet");
        uart_write_bytes(UART_NUM_2, (const char *)buf, SECPLUS2_CODE_LEN);
        // uart_wait_tx_done(UART_NUM_2, 100);
        ESP_LOGI(TAG, "Send packet done");
    }

    if (pkt_ac.inc_counter)
    {
        rolling_code = (rolling_code + 1) & 0xfffffff;
    }
    return true;
}

bool process_PacketAction(PacketAction &pkt_ac)
{

    bool success = false;

    // Use LED to signal activity
    led->flash();
    /*
        if (userConfig->getGDOSecurityType() == 1)
        {
            // check which action
            switch (pkt_ac.pkt.m_data.type)
            {
            // using this type for emaulation of wall panel
            case PacketDataType::Status:
            {
                // 0x38 || 0x39 || 0x3A
                if (pkt_ac.pkt.m_data.value.cmd)
                {
                    success = transmitSec1(pkt_ac.pkt.m_data.value.cmd);
                    if (success)
                    {
                        last_tx = millis();
                        // ESP_LOGI(TAG, "sending 0x%02X query", pkt_ac.pkt.m_data.value.cmd);
                    }
                }
                break;
            }
            case PacketDataType::DoorAction:
            {
                if (pkt_ac.pkt.m_data.value.door_action.pressed == true)
                {
                    success = transmitSec1(secplus1Codes::DoorButtonPress);
                    if (success)
                    {
                        last_tx = millis();
                        ESP_LOGI(TAG, "sending DOOR button press");
                    }
                }
                else
                {
                    success = transmitSec1(secplus1Codes::DoorButtonRelease);
                    if (success)
                    {
                        last_tx = millis();
                        ESP_LOGI(TAG, "sending DOOR button release");
                    }
                }

                break;
            }

            case PacketDataType::Light:
            {
                if (pkt_ac.pkt.m_data.value.light.pressed == true)
                {
                    success = transmitSec1(secplus1Codes::LightButtonPress);
                    if (success)
                    {
                        last_tx = millis();
                        ESP_LOGI(TAG, "sending LIGHT button press");
                    }
                }
                else
                {
                    success = transmitSec1(secplus1Codes::LightButtonRelease);
                    if (success)
                    {
                        last_tx = millis();
                        ESP_LOGI(TAG, "Sending LIGHT button release");
                    }
                }

                break;
            }

            case PacketDataType::Lock:
            {
                if (pkt_ac.pkt.m_data.value.lock.pressed == true)
                {
                    success = transmitSec1(secplus1Codes::LockButtonPress);
                    if (success)
                    {
                        last_tx = millis();
                        ESP_LOGI(TAG, "sending LOCK button press");
                    }
                }
                else
                {
                    success = transmitSec1(secplus1Codes::LockButtonRelease);
                    if (success)
                    {
                        last_tx = millis();
                        ESP_LOGI(TAG, "sending LOCK button release");
                    }
                }

                break;
            }

            default:
            {
                ESP_LOGI(TAG, "pkt_ac.pkt.m_data.type=%d", (int)pkt_ac.pkt.m_data.type);

                break;
            }
            }
        }
        else
        */
    {
        success = transmitSec2(pkt_ac);
    }

    return success;
}

void sync()
{
    // only for SECURITY2.0
    // for exposition about this process, see docs/syncing.md

    PacketData d;
    d.type = PacketDataType::NoData;
    d.value.no_data = NoData();
    Packet pkt = Packet(PacketCommand::GetOpenings, d, id_code);
    PacketAction pkt_ac = {pkt, true, 0};
    process_PacketAction(pkt_ac);

    // vTaskDelay(pdMS_TO_TICKS(100));

    pkt = Packet(PacketCommand::GetStatus, d, id_code);
    pkt_ac.pkt = pkt;
    process_PacketAction(pkt_ac);
}

void door_command(DoorAction action)
{

    PacketData data;
    data.type = PacketDataType::DoorAction;
    data.value.door_action.action = action;
    data.value.door_action.pressed = true;
    data.value.door_action.id = 1;

    Packet pkt = Packet(PacketCommand::DoorAction, data, id_code);
    PacketAction pkt_ac = {pkt, false, 250}; // 250ms delay for SECURITY1.0

    if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
    {
        ESP_LOGE(TAG, "packet queue full, dropping door command pressed pkt");
    }
    // do button release
    pkt_ac.pkt.m_data.value.door_action.pressed = false;
    pkt_ac.inc_counter = true;
    pkt_ac.delay = 40; // 40ms delay for SECURITY1.0

    if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
    {
        ESP_LOGE(TAG, "packet queue full, dropping door command release pkt");
    }
    // when observing wall panel 2 releases happen, so we do the same
    if (userConfig->getGDOSecurityType() == 1)
    {
        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping door command release pkt");
        }
    }

    send_get_status();
}
void door_command_close()
{
    door_command(DoorAction::Close);
}

void open_door()
{
    ESP_LOGI(TAG, "open door request");

    /* TODO
        if (TTCcountdown > 0)
        {
            // We are in a time-to-close delay timeout.
            // Effect of open is to cancel the timeout (leaving door open)
            ESP_LOGI(TAG, "Canceling time-to-close delay timer");
            TTCtimer.detach();
            TTCcountdown = 0;
            // Reset light to state it was at before delay start.
            set_light(TTCwasLightOn);
        }
    */
    // safety
    if (garage_door.current_state == GarageDoorCurrentState::CURR_OPEN)
    {
        ESP_LOGI(TAG, "door already open; ignored request");
        return;
    }

    if (garage_door.current_state == GarageDoorCurrentState::CURR_CLOSING)
    {
        ESP_LOGI(TAG, "door is closing; do stop");
        door_command(DoorAction::Stop);
        return;
    }

    door_command(DoorAction::Open);
}

void TTCdelayLoop(TimerHandle_t xTimer)
{
    if (--TTCcountdown > 0)
    {
        // If light is on, turn it off.  If off, turn it on.
        set_light(!garage_door.light);
    }
    else
    {
        // End of delay period
        xTimerStop(TTCtimer, 100);
        if (TTC_Action)
            (*TTC_Action)();
    }
    return;
}

void close_door()
{
    ESP_LOGI(TAG, "close door request");

    // safety
    if (garage_door.current_state == GarageDoorCurrentState::CURR_CLOSED)
    {
        ESP_LOGI(TAG, "door already closed; ignored request");
        return;
    }

    if (garage_door.current_state == GarageDoorCurrentState::CURR_OPENING)
    {
        ESP_LOGI(TAG, "door already opening; do stop");
        door_command(DoorAction::Stop);
        return;
    }

    if (userConfig->getTTCseconds() == 0)
    {
        door_command(DoorAction::Close);
    }
    else
    {
        if (TTCcountdown > 0)
        {
            // We are in a time-to-close delay timeout.
            // Effect of second click is to cancel the timeout and close immediately
            ESP_LOGI(TAG, "Canceling time-to-close delay timer");
            xTimerStop(TTCtimer, 100);
            TTCcountdown = 0;
            door_command(DoorAction::Close);
        }
        else
        {
            ESP_LOGI(TAG, "Delay door close by %d seconds", userConfig->getTTCseconds());
            // Call delay loop every 0.5 seconds to flash light.
            TTCcountdown = userConfig->getTTCseconds() * 2;
            // Remember whether light was on or off
            TTCwasLightOn = garage_door.light;
            TTC_Action = &door_command_close;
            xTimerStart(TTCtimer, 100);
        }
    }
}

void send_get_status()
{
    // only used with SECURITY2.0
    if (userConfig->getGDOSecurityType() == 2)
    {
        ESP_LOGI(TAG, "Send Get Status");
        PacketData d;
        d.type = PacketDataType::NoData;
        d.value.no_data = NoData();
        Packet pkt = Packet(PacketCommand::GetStatus, d, id_code);
        PacketAction pkt_ac = {pkt, true, 0};
        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping get status pkt");
        }
    }
}

void set_lock(uint8_t value)
{
    PacketData data;
    data.type = PacketDataType::Lock;
    if (value)
    {
        data.value.lock.lock = LockState::On;
        garage_door.target_lock = TGT_LOCKED;
    }
    else
    {
        data.value.lock.lock = LockState::Off;
        garage_door.target_lock = TGT_UNLOCKED;
    }

    // safety
    if (data.value.lock.lock == LockState::On && garage_door.current_lock == LockCurrentState::CURR_LOCKED)
    {
        ESP_LOGI(TAG, "Lock already Locked");
        return;
    }
    if (data.value.lock.lock == LockState::Off && garage_door.current_lock == LockCurrentState::CURR_UNLOCKED)
    {
        ESP_LOGI(TAG, "Lock already Unlocked");
        return;
    }

    // SECUIRTY1.0
    if (userConfig->getGDOSecurityType() == 1)
    {

        // this emulates the "look" button press+release
        // - PRESS (0x34)
        // - DELAY 3000ms
        // - RELEASE (0x35)
        // - DELAY 40ms
        // - RELEASE (0x35)
        // - DELAY 40ms

        data.value.lock.pressed = true;
        Packet pkt = Packet(PacketCommand::Lock, data, id_code);
        PacketAction pkt_ac = {pkt, true, 3000}; // 3000ms delay for SECURITY1.0

        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping lock pkt");
        }

        // button release
        pkt_ac.pkt.m_data.value.lock.pressed = false;
        pkt_ac.delay = 40; // 40ms delay for SECURITY1.0
                           // observed the wall plate does 2 releases, so we will too
        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping lock pkt");
        }
        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping lock pkt");
        }
    }
    // SECURITY2.0
    else
    {
        Packet pkt = Packet(PacketCommand::Lock, data, id_code);
        PacketAction pkt_ac = {pkt, true, 0};

        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping lock pkt");
        }
        send_get_status();
    }
}

void set_light(bool value)
{
    PacketData data;
    data.type = PacketDataType::Light;
    if (value)
    {
        data.value.light.light = LightState::On;
    }
    else
    {
        data.value.light.light = LightState::Off;
    }

    // safety
    if (data.value.light.light == LightState::On && garage_door.light == true)
    {
        ESP_LOGI(TAG, "Light already On");
        return;
    }
    if (data.value.light.light == LightState::Off && garage_door.light == false)
    {
        ESP_LOGI(TAG, "Light already Off");
        return;
    }

    // SECUIRTY+1.0
    if (userConfig->getGDOSecurityType() == 1)
    {
        // this emulates the "light" button press+release
        // - PRESS (0x32)
        // - DELAY 250ms
        // - RELEASE (0x33)
        // - DELAY 40ms
        // - RELEASE (0x33)
        // - DELAY 40ms
        data.value.light.pressed = true;

        Packet pkt = Packet(PacketCommand::Light, data, id_code);
        PacketAction pkt_ac = {pkt, true, 250}; // 250ms delay for SECURITY1.0

        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping light pkt");
        }
        // button release
        pkt_ac.pkt.m_data.value.light.pressed = false;
        pkt_ac.delay = 40; // 40ms delay for SECURITY1.0
        // observed the wall plate does 2 releases, so we will too
        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping light pkt");
        }
        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping light pkt");
        }
    }
    // SECURITY+2.0
    else
    {
        Packet pkt = Packet(PacketCommand::Light, data, id_code);
        PacketAction pkt_ac = {pkt, true, 0};

        if (xQueueSendToBack(pkt_q, &pkt_ac, 0) == errQUEUE_FULL)
        {
            ESP_LOGE(TAG, "packet queue full, dropping light pkt");
        }
        send_get_status();
    }
}

void manual_recovery()
{
    // Increment counter every time button is pushed.  If we hit 5 in 3 seconds,
    // go to WiFi recovery mode
    if (force_recover.push_count++ == 0)
    {
        ESP_LOGI(TAG, "Push count start");
        force_recover.timeout = millis() + 3000;
    }
    else if (millis() > force_recover.timeout)
    {
        ESP_LOGI(TAG, "Push count reset");
        force_recover.push_count = 0;
    }
    ESP_LOGI(TAG, "Push count %d", force_recover.push_count);

    if (force_recover.push_count >= 5)
    {
        ESP_LOGI(TAG, "Request to boot into soft access point mode in %ds", force_recover_delay);
        userConfig->set(cfg_softAPmode, true);
        userConfig->save();
        // Call delay loop every 0.5 seconds to flash light.
        TTCcountdown = force_recover_delay * 2;
        // Remember whether light was on or off
        TTCwasLightOn = garage_door.light;
        TTC_Action = &sync_and_restart;
        xTimerStart(TTCtimer, 100);
    }
}
