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
#include <cstring>
// ESP system includes
#include <driver/uart.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

// RATGDO project includes
#include "wifi.h"
#include "ratgdo.h"
#include "log.h"
#include "config.h"
#include "led.h"

// Logger tag
static const char *TAG = "ratgdo-wifi";

static const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

esp_err_t app_wifi_start(TickType_t ticks_to_wait);

#define UART_BUF_SZ (256)
#define UART_EVT_Q_SZ (8)
static QueueHandle_t uart0_queue;

#define MAX_ATTEMPTS_WIFI_CONNECTION 5
const uint16_t NETWORK_COUNT = 16;
uint8_t x_buffer[32 /* bytes per ssid */ + 64 /* bytes per password */ + 16 /* bytes overhead */];
uint8_t x_position = 0;
uint8_t dtmp[UART_BUF_SZ];

wifi_ap_record_t wifi_ap[NETWORK_COUNT];

/*
void set_error(improv::Error error);
void send_response(std::vector<uint8_t> &response);
void set_state(improv::State state);
void get_available_wifi_networks();
bool on_command_callback(improv::ImprovCommand cmd);
void on_error_callback(improv::Error err);
*/
bool connect_wifi(std::string &ssid, std::string &password);

wifi_config_t wifi_config = {};
const size_t WIFI_SSID_MAX_LEN = 31; // 802.11 point 7.3.2.1, plus NUL terminator
char wifi_ssid[WIFI_SSID_MAX_LEN + 1];
const size_t WIFI_PASS_MAX_LEN = 64; // from defn of wifi_sta_config_t
char wifi_pass[WIFI_PASS_MAX_LEN];

char macAddress[18] = "";

//static int s_retry_num = 0;
static WifiStatus wifi_status = WifiStatus::Disconnected;
//static esp_ip4_addr_t ip_info;

//static nvs_handle_t wifi_nvs_handle;

// Event handler for catching system events
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        wifi_status = WifiStatus::Pending;
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "WiFi Connected");
        // esp_netif_create_ip6_linklocal((esp_netif_t *)arg);
        // ((esp_netif_t *)arg)->ip_info->ip;
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        wifi_status = WifiStatus::Connected;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi Got IP Address");
        // ESP_LOGI(TAG, "IP: %08X", (unsigned int)event->ip_info.ip.addr);
        char ipAddr[16];
        snprintf(ipAddr, sizeof(ipAddr), IPSTR, IP2STR(&(event->ip_info.ip)));
        userConfig->set(cfg_localIP, ipAddr);
        snprintf(ipAddr, sizeof(ipAddr), IPSTR, IP2STR(&(event->ip_info.gw)));
        userConfig->set(cfg_gatewayIP, ipAddr);
        snprintf(ipAddr, sizeof(ipAddr), IPSTR, IP2STR(&(event->ip_info.netmask)));
        userConfig->set(cfg_subnetMask, ipAddr);
        esp_netif_dns_info_t dns;
        if (ESP_OK == esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns))
        {
            snprintf(ipAddr, sizeof(ipAddr), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
            userConfig->set(cfg_nameserverIP, ipAddr);
        }
        uint8_t macAddr[6];
        if (ESP_OK == esp_netif_get_mac(event->esp_netif, macAddr))
        {
            snprintf(macAddress, sizeof(macAddress), "%02x:%02x:%02x:%02x:%02x:%02x", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
        }

        ESP_LOGI(TAG, "Connected with IP Address: %s", userConfig->getLocalIP().c_str());
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6)
    {
        wifi_status = WifiStatus::Connected;
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Connected with IPv6 Address:" IPV6STR, IPV62STR(event->ip6_info.ip));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        wifi_status = WifiStatus::Pending;
        esp_wifi_connect();
    }
    else if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG,
                     "Received Wi-Fi credentials"
                     "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason =
                (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG,
                     "Provisioning failed!\n\tReason : %s"
                     "\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR)
                         ? "Wi-Fi station authentication failed"
                         : "Wi-Fi access-point not found");
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case WIFI_PROV_END:
            /* De-initialize manager once provisioning is finished */
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
    }
}
/*
static void event_handler(void *arg, esp_event_base_t event_base,                         int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGD(TAG, "wifi event station start");
        wifi_status = WifiStatus::Pending;
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&         event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGD(TAG, "wifi event station disconnected");
        if (s_retry_num < MAX_ATTEMPTS_WIFI_CONNECTION)
        {
            wifi_status = WifiStatus::Pending;
            ESP_LOGI(TAG, "retry to connect to the AP");
            esp_wifi_connect();
            s_retry_num++;
        }
        else
        {
            wifi_status = WifiStatus::Disconnected;
            ESP_LOGI(TAG, "connect to the AP fail");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        wifi_status = WifiStatus::Connected;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR "\n", IP2STR(&event->ip_info.ip));
        memcpy(&ip_info, &event->ip_info.ip, sizeof(esp_ip4_addr_t));
        s_retry_num = 0;
    }
}
*/

void wifi_task_entry(void *ctx)
{
    ESP_LOGI(TAG, "Entered WiFi task thread");
    // Initialize TCP/IP
    ESP_ERROR_CHECK(esp_netif_init());
    // Initialize the event loop
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();
    // Initialize Wi-Fi including netif with default config
    esp_netif_t *wifi_netif = esp_netif_create_default_wifi_sta();
    // Register our event handler for Wi-Fi, IP and Provisioning related events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, wifi_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler, NULL));
    ESP_LOGI(TAG, "WiFi handlers registered");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /*
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &wifi_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening wifi NVS handle. Wifi task dying.",
    esp_err_to_name(err)); return;
    }

    size_t ssid_len, pass_len;
    err = nvs_get_str(wifi_nvs_handle, "wifi_ssid", wifi_ssid, &ssid_len);  //
    TODO make sure ssid err |= nvs_get_str(wifi_nvs_handle, "wifi_pass",
    wifi_pass, &pass_len); // and pass are
                                                                            //
    nul-terminated?

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No wifi credentials stored in NVS");
    } else {
        std::string s(wifi_ssid);
        std::string p(wifi_pass);
        connect_wifi(s, p);
    }
    */
    ESP_ERROR_CHECK(app_wifi_start(portMAX_DELAY));
    /*
     std::string s("KIoT");
     std::string p("abcdefgh");
     connect_wifi(s, p);
     */

    ESP_LOGI(TAG, "WiFi initialized");
    // set up the UART for incoming ?????Improv????? bytes
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_param_config(UART_NUM_0, &uart_config);
    uart_driver_install(UART_NUM_0, UART_BUF_SZ, UART_BUF_SZ, UART_EVT_Q_SZ,
                        &uart0_queue, 0);

    ESP_LOGI(TAG, "wifi setup finished.");

    while (true)
    {
        uart_event_t event = {};

        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)portMAX_DELAY))
        {
            bzero(dtmp, UART_BUF_SZ);

            if (event.type == UART_DATA)
            {
                led->flash();
                uart_read_bytes(UART_NUM_0, dtmp, event.size, portMAX_DELAY);
                ESP_LOGI(TAG, "uart read %d bytes", event.size);

                for (size_t i = 0; i < event.size; i++)
                {
                    uint8_t b = dtmp[i];
                    ESP_LOGI(TAG, "handling byte %02X", b);
                    {
                        x_position = 0;

                        int count = uxTaskGetNumberOfTasks();
                        TaskStatus_t *tasks =
                            (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * count);
                        if (tasks != NULL)
                        {
                            uxTaskGetSystemState(tasks, count, NULL);

                            printf("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n");
                            for (size_t i = 0; i < count; i++)
                            {
                                printf("%s\t\\tt%d\t\t%d\n", (char *)tasks[i].pcTaskName, (int)tasks[i].uxBasePriority, (int)tasks[i].usStackHighWaterMark);
                            }
                            printf("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n\n");
                        }
                        vPortFree(tasks); 
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "unhandled event type %d", event.type);
            }
        }
    }

    vTaskDelete(NULL);
}

bool connect_wifi(std::string &ssid, std::string &password)
{
    ESP_LOGI(TAG, "Connecting to: %s", ssid.c_str());
    wifi_config_t wifi_config = {};
    wifi_config.sta = {};
    wifi_config.sta.pmf_cfg = {.capable = true, .required = false};
    strlcpy((char *)wifi_config.sta.ssid, ssid.c_str(),
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password.c_str(),
            sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // Disable power saving, considerably improves pings and we are not battery powered
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to WiFi");
    return true;
}

#define PROV_TRANSPORT_BLE "ble"

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "RATGDO_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

esp_err_t app_wifi_start(TickType_t ticks_to_wait)
{
    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {};
    /* What is the Provisioning Scheme that we want ?
     * wifi_prov_scheme_softap or wifi_prov_scheme_ble */
    config.scheme = wifi_prov_scheme_ble;

    /* Any default scheme specific event handler that you would
     * like to choose. Since our example application requires
     * neither BT nor BLE, we can choose to release the associated
     * memory once provisioning is complete, or not needed
     * (in case when device is already provisioned). Choosing
     * appropriate scheme specific event handler allows the manager
     * to take care of this automatically. This can be set to
     * WIFI_PROV_EVENT_HANDLER_NONE when using wifi_prov_scheme_softap*/
    config.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    /* Let's find out if the device is provisioned */
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned)
    {
        ESP_LOGI(TAG, "Starting provisioning");
        esp_netif_create_default_wifi_ap();

        esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
        /* What is the Device Service Name that we want
         * This translates to :
         *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
         *     - device name when scheme is wifi_prov_scheme_ble
         */
        char service_name[16];
        get_device_service_name(service_name, sizeof(service_name));

        /* What is the security level that we want (0 or 1):
         *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
         *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
         *          using X25519 key exchange and proof of possession (pop) and AES-CTR
         *          for encryption/decryption of messages.
         */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

        /* Do we want a proof-of-possession (ignored if Security 0 is selected):
         *      - this should be a string with length > 0
         *      - NULL if not used
         */
        static const char pop[] = "abcd1234";

        /* What is the service key (Wi-Fi password)
         * NULL = Open network
         * This is ignored when scheme is wifi_prov_scheme_ble
         */
        const char *service_key = NULL;

        /* This step is only useful when scheme is wifi_prov_scheme_ble. This will
         * set a custom 128 bit UUID which will be included in the BLE advertisement
         * and will correspond to the primary GATT service that provides provisioning
         * endpoints as GATT characteristics. Each GATT characteristic will be
         * formed using the primary service UUID as base, with different auto assigned
         * 12th and 13th bytes (assume counting starts from 0th byte). The client side
         * applications must identify the endpoints by reading the User Characteristic
         * Description descriptor (0x2901) for each characteristic, which contains the
         * endpoint name of the characteristic */
        uint8_t custom_service_uuid[] = {
            /* This is a random uuid. This can be modified if you want to change the BLE uuid. */
            /* 12th and 13th bit will be replaced by internal bits. */
            0xb4,
            0xdf,
            0x5a,
            0x1c,
            0x3f,
            0x6b,
            0xf4,
            0xbf,
            0xea,
            0x4a,
            0x82,
            0x03,
            0x04,
            0x90,
            0x1a,
            0x02,
        };
        esp_err_t err;
        err = wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "wifi_prov_scheme_ble_set_service_uuid failed %d", err);
            return err;
        }

        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));
        /* Print QR code for provisioning */

        ESP_LOGI(TAG, "Provisioning Started. Name : %s, POP : %s", service_name, pop);
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");
        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();
        /* Start Wi-Fi station */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, ticks_to_wait);
    return ESP_OK;
}

#ifdef IMPROV
std::vector<std::string> get_local_url()
{
    char buf[25];
    snprintf(buf, 25, "http://" IPSTR "/", IP2STR(&ip_info));

    return {// TODO
            // URL where user can finish onboarding or use device
            // Recommended to use website hosted by device
            std::string(buf)};
}

void on_error_callback(improv::Error err)
{
    ESP_LOGE(TAG, "improv error: %02X", err);
}

bool on_command_callback(improv::ImprovCommand cmd)
{
    switch (cmd.command)
    {
    case improv::Command::GET_CURRENT_STATE:
    {
        ESP_LOGD(TAG, "improv cmd GET_CURRENT_STATE");
        if ((wifi_status == WifiStatus::Connected))
        {
            std::vector<std::string> local_url = get_local_url();
            ESP_LOGD(TAG, "wifi is connected, returning local url %s",
                     local_url[0].c_str());
            set_state(improv::State::STATE_PROVISIONED);
            std::vector<uint8_t> data = improv::build_rpc_response(
                improv::GET_CURRENT_STATE, local_url, false);
            send_response(data);
        }
        else
        {
            set_state(improv::State::STATE_AUTHORIZED);
        }

        break;
    }

    case improv::Command::WIFI_SETTINGS:
    {
        ESP_LOGD(TAG, "improv cmd WIFI_SETTINGS");
        if (cmd.ssid.length() == 0)
        {
            set_error(improv::Error::ERROR_INVALID_RPC);
            break;
        }

        set_state(improv::STATE_PROVISIONING);

        if (connect_wifi(cmd.ssid, cmd.password))
        {
            ESP_LOGD(TAG, "connect_wifi returned true");

            set_state(improv::STATE_PROVISIONED);
            std::vector<uint8_t> data = improv::build_rpc_response(
                improv::WIFI_SETTINGS, get_local_url(), false);
            send_response(data);
        }
        else
        {
            ESP_LOGD(TAG, "connect_wifi did not return true");

            set_state(improv::STATE_STOPPED);
            set_error(improv::Error::ERROR_UNABLE_TO_CONNECT);
        }

        break;
    }

    case improv::Command::GET_DEVICE_INFO:
    {
        ESP_LOGD(TAG, "improv cmd GET_DEVICE_INFO");

        std::vector<std::string> infos = {DEVICE_NAME, AUTO_VERSION, CHIP_FAMILY,
                                          MODEL_NAME};
        std::vector<uint8_t> data =
            improv::build_rpc_response(improv::GET_DEVICE_INFO, infos, false);
        send_response(data);
        break;
    }

    case improv::Command::GET_WIFI_NETWORKS:
    {
        ESP_LOGD(TAG, "improv cmd GET_WIFI_NETWORKS");

        get_available_wifi_networks();
        break;
    }

    default:
    {
        set_error(improv::ERROR_UNKNOWN_RPC);
        return false;
    }
    }

    return true;
}

void get_available_wifi_networks()
{
    esp_err_t err = esp_wifi_scan_start(NULL, 1000);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to start wifi scan: %s", esp_err_to_name(err));
        return;
    }

    uint16_t network_count = NETWORK_COUNT;
    err = esp_wifi_scan_get_ap_records(&network_count, wifi_ap);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to get wifi scan results: %s", esp_err_to_name(err));
        return;
    }

    // TODO re-introduce feature that sorted by RSSI networks with the same SSID,
    // and discarded all but the strongest.

    for (uint16_t i = 0; i < network_count; ++i)
    {
        std::vector<uint8_t> data = improv::build_rpc_response(
            improv::GET_WIFI_NETWORKS,
            {std::string((const char *)wifi_ap[i].ssid),
             std::to_string(wifi_ap[i].rssi),
             std::string(wifi_ap[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES")},
            false);
        send_response(data);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // final response
    std::vector<uint8_t> data = improv::build_rpc_response(
        improv::GET_WIFI_NETWORKS, std::vector<std::string>{}, false);
    send_response(data);
}
enum State : uint8_t
{
    STATE_STOPPED = 0x00,
    STATE_AWAITING_AUTHORIZATION = 0x01,
    STATE_AUTHORIZED = 0x02,
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED = 0x04,
};

void set_state(improv::State state)
{
    ESP_LOGD(TAG, "setting improv state to %d", state);

    std::vector<char> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(11);
    data[6] = improv::IMPROV_SERIAL_VERSION;
    data[7] = improv::TYPE_CURRENT_STATE;
    data[8] = 1;
    data[9] = state;

    uint8_t checksum = 0x00;
    for (char d : data)
        checksum += d;
    data[10] = checksum;

    uart_write_bytes(UART_NUM_0, data.data(), data.size());
}

void send_response(std::vector<uint8_t> &response)
{
    std::vector<char> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(9);
    data[6] = improv::IMPROV_SERIAL_VERSION;
    data[7] = improv::TYPE_RPC_RESPONSE;
    data[8] = response.size();
    data.insert(data.end(), response.begin(), response.end());

    char checksum = 0x00;
    for (char d : data)
        checksum += d;
    data.push_back(checksum);

    uart_write_bytes(UART_NUM_0, data.data(), data.size());
}

void set_error(improv::Error error)
{
    ESP_LOGW(TAG, "improv returning error %d", error);

    std::vector<char> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(11);
    data[6] = improv::IMPROV_SERIAL_VERSION;
    data[7] = improv::TYPE_ERROR_STATE;
    data[8] = 1;
    data[9] = error;

    char checksum = 0x00;
    for (char d : data)
        checksum += d;
    data[10] = checksum;

    uart_write_bytes(UART_NUM_0, data.data(), data.size());
}
#endif