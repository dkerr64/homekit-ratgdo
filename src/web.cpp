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
#include <string>
#include <format>
#include <tuple>
#include <unordered_map>
#include <time.h>

// ESP system includes
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_netif_types.h>
#include <esp_wifi.h>
#include <sys/socket.h>
#include <hap.h>
// Undocumented function to get HomeKit HAP server handle
// extern "C" httpd_handle_t *hap_httpd_get_handle();
// JSON functions come as part of esp-homekit-sdk
#include "../components/esp-homekit-sdk/components/homekit/json_generator/upstream/json_generator.h"

// RATGDO project includes
#define PROGMEM // so it is no-op in webcontent.h
#include "www/build/webcontent.h"
#include "ratgdo.h"
#include "config.h"
#include "comms.h"
#include "log.h"
#include "web.h"
#include "utilities.h"
#include "wifi.h"
#include "json.h"
#include "MD5Builder.h"
#include "led.h"

// Logger tag
static const char *TAG = "ratgdo-http";

// Browser cache control, time in seconds after which browser cache invalid
// This is used for CSS, JS and IMAGE file types.  Set to 30 days !!
#define CACHE_CONTROL (60 * 60 * 24 * 30)

#ifdef ENABLE_CRASH_LOG
#include "EspSaveCrash.h"
#ifdef LOG_MSG_BUFFER
EspSaveCrash saveCrash(1408, 1024, true, &crashCallback);
#else
EspSaveCrash saveCrash(1408, 1024, true);
#endif
#endif

// Forward declare the internal URI handling functions...
esp_err_t handle_reset(httpd_req_t *req);
esp_err_t handle_reboot(httpd_req_t *req);
esp_err_t handle_status(httpd_req_t *req);
// esp_err_t handle_settings(httpd_req_t *req);
esp_err_t handle_everything(httpd_req_t *req);
esp_err_t handle_setgdo(httpd_req_t *req);
esp_err_t handle_logout(httpd_req_t *req);

esp_err_t handle_auth(httpd_req_t *req);
esp_err_t handle_subscribe(httpd_req_t *req);
esp_err_t handle_showlog(httpd_req_t *req);
esp_err_t handle_showrebootlog(httpd_req_t *req);
esp_err_t handle_crashlog(httpd_req_t *req);
esp_err_t handle_clearcrashlog(httpd_req_t *req);
#ifdef CRASH_DEBUG
esp_err_t handle_forcecrash(httpd_req_t *req);
esp_err_t handle_crash_oom(httpd_req_t *req);
void *crashptr;
char *test_str = NULL;
#endif
esp_err_t handle_checkflash(httpd_req_t *req);
esp_err_t handle_update(httpd_req_t *req);
esp_err_t handle_firmware_upload(httpd_req_t *req);
esp_err_t SSEHandler(httpd_req_t *req, uint8_t channel);
esp_err_t handle_notfound(httpd_req_t *req);
esp_err_t handle_accesspoint(httpd_req_t *req);
esp_err_t handle_setssid(httpd_req_t *req);

// Built in URI handlers
const char restEvents[] = "/rest/events/";
const std::unordered_map<std::string, std::pair<const httpd_method_t, esp_err_t (*)(httpd_req_t *)>> builtInUri = {
    {"/status.json", {HTTP_GET, handle_status}},
    {"/reset", {HTTP_POST, handle_reset}},
    {"/reboot", {HTTP_POST, handle_reboot}},
    {"/setgdo", {HTTP_POST, handle_setgdo}},
    {"/logout", {HTTP_GET, handle_logout}},
    {"/auth", {HTTP_GET, handle_auth}},
    {"/showlog", {HTTP_GET, handle_showlog}},
    {"/showrebootlog", {HTTP_GET, handle_showrebootlog}},
    {"/checkflash", {HTTP_GET, handle_checkflash}},
    {"/crashlog", {HTTP_GET, handle_crashlog}},
    {"/clearcrashlog", {HTTP_GET, handle_clearcrashlog}},
#ifdef CRASH_DEBUG
    {"/forcecrash", {HTTP_POST, handle_forcecrash}},
    {"/crashoom", {HTTP_POST, handle_crash_oom}},
#endif
    {"/rest/events/subscribe", {HTTP_GET, handle_subscribe}}};

static httpd_handle_t httpServer = NULL;

// Garage door status
extern struct GarageDoor garage_door;
// Local copy of door status
GarageDoor last_reported_garage_door;
bool last_reported_paired = false;
uint32_t lastDoorUpdateAt = 0;
GarageDoorCurrentState lastDoorState = (GarageDoorCurrentState)0xff;

// number of times the device has crashed
int crashCount = 0;

// Implement our own firmware update so can enforce MD5 check.
// Based on ESP8266HTTPUpdateServer
std::string _updaterError;
bool _authenticatedUpdate;
char firmwareMD5[36] = "";
size_t firmwareSize = 0;
bool flashCRC = true;

// Common HTTP responses
const char response400missing[] = "400: Bad Request, missing argument\n";
const char response400invalid[] = "400: Bad Request, invalid argument\n";
const char response404[] = "404: Not Found\n";
const char response503[] = "503: Service Unavailable.\n";
const char response200[] = "HTTP/1.1 200 OK\nContent-Type: text/plain\nConnection: close\n\n";

const char *http_methods[] = {"HTTP_ANY", "HTTP_GET", "HTTP_HEAD", "HTTP_POST", "HTTP_PUT", "HTTP_PATCH", "HTTP_DELETE", "HTTP_OPTIONS"};

const char softAPhttpPreamble[] = "HTTP/1.1 200 OK\nContent-Type: text/html\nCache-Control: no-cache, no-store\nConnection: close\n\n<!DOCTYPE html>";
const char softAPstyle[] = R"(<style>
.adv {
 display: none;
}
td,th {
 text-align: left;
}
th:nth-child(1n+4), td:nth-child(1n+4) {
 display: none;
 text-align: right;
}
</style>)";
const char softAPscript[] = R"(<script>
const warnTxt = 'Selecting SSID in advanced mode locks the device to a specific WiFi ' +
 'access point by its unique hardware BSSID. If that access point goes offline, or you replace ' +
 'it, then the device will NOT connect to WiFi.';
const setTxt = 'Set SSID and password, are you sure?';
function shAdv(checked) {
 Array.from(document.getElementsByClassName('adv')).forEach((elem) => {
  elem.style.display = checked ? 'table-row' : 'none';
 });
 Array.from(document.querySelectorAll('th:nth-child(1n+4), td:nth-child(1n+4)')).forEach((elem) => {
  elem.style.display = checked ? 'table-cell' : 'none';
 });
 document.getElementById('warn').innerHTML = checked ? '<p><b>WARNING: </b>' + warnTxt + '</p>' : '';
}
function confirmAdv() {
 if (document.getElementById('adv').checked) {
  return confirm('WARNING: ' + warnTxt + '\n\n' + setTxt);
 } else {
  return confirm(setTxt);
 }
}
</script>)";
const char softAPtableHead[] = R"(
<p>Select from available networks, or manually enter SSID:</p>
<form action='/setssid' method='post'>
<table>
<tr><td><input id='adv' name='advanced' type='checkbox' onclick='shAdv(this.checked)'></td><td colspan='2'>Advanced</td></tr>
<tr><th></th><th>SSID</th><th>RSSI</th><th>Chan</th><th>Hardware BSSID</th></tr>)";
const char softAPtableRow[] = R"(
<tr %s><td><input type='radio' name='net' value='%d' %s></td><td>%s</td><td>%ddBm</td><td>%d</td><td>&nbsp;&nbsp;%02x:%02x:%02x:%02x:%02x:%02x</td></tr>)";
const char softAPtableLastRow[] = R"(
<tr><td><input type='radio' name='net' value='%d'></td><td colspan='2'><input type='text' name='userSSID' placeholder='SSID' value='%s'></td></tr>)";
const char softAPtableFoot[] = R"(
</table>
<br><label for='pw'>Network password:&nbsp;</label>
<input id='pw' name='pw' type='password' placeholder='password'>
<p id='warn'></p>
<input type='submit' value='Submit' onclick='return confirmAdv();'>&nbsp;
<input type='submit' value='Rescan' formaction='/rescan'>&nbsp;
<input type='submit' value='Cancel' formaction='/reboot'
    onclick='return confirm("Reboot without changes, are you sure?");'>
</form>)";

// For Server Sent Events (SSE) support
// Just reloading page causes register on new channel.  So we need a reasonable number
// to accommodate "extra" until old one is detected as disconnected.
#define SSE_MAX_CHANNELS 8
struct SSESubscription
{
    uint32_t clientIP;
    httpd_handle_t socketHD;
    int socketFD;
    TimerHandle_t heartbeatTimer;
    bool SSEconnected;
    int SSEfailCount;
    std::string clientUUID;
    bool logViewer;
};
SSESubscription subscription[SSE_MAX_CHANNELS];
// During firmware update note which subscribed client is updating
SSESubscription *firmwareUpdateSub = NULL;
uint8_t subscriptionCount = 0;

struct userContext
{
    esp_ip4_addr_t ipv4; // uint32_t version of IP address
    char ipAddrStr[16];  // Set to remote client IP addresss
    char url[24];        // URI with the query string stripped off
};
#define CTX_USER(req) ((userContext *)req->user_ctx)
#define CTX_CLIENT_IP(req) (CTX_USER(req)->ipv4.addr)
#define CTX_CLIENT_IP_STR(req) (CTX_USER(req)->ipAddrStr)
#define CTX_URL(req) (CTX_USER(req)->url)

#define JSON_BUFFER_SIZE 1280
char *json = NULL;

#define DOOR_STATE(s) (s == 0) ? "Open" : (s == 1) ? "Closed"  \
                                      : (s == 2)   ? "Opening" \
                                      : (s == 3)   ? "Closing" \
                                      : (s == 4)   ? "Stopped" \
                                                   : "Unknown"
#define LOCK_STATE(s) (s == 0) ? "Unsecured" : (s == 1) ? "Secured" \
                                           : (s == 2)   ? "Jammed"  \
                                                        : "Unknown"

void web_loop()
{
    unsigned long upTime = millis();
    START_JSON(json);
    if (garage_door.active && garage_door.current_state != lastDoorState)
    {
        ESP_LOGI(TAG, "Current Door State changing from %d to %d", lastDoorState, garage_door.current_state);
        if (enableNTP && clockSet)
        {
            if (lastDoorState == 0xff)
            {
                // initialize with saved time.
                // lastDoorUpdateAt is milliseconds relative to system reboot time.
                lastDoorUpdateAt = (userConfig->getDoorUpdateAt() != 0) ? ((userConfig->getDoorUpdateAt() - time(NULL)) * 1000) + upTime : 0;
            }
            else
            {
                // first state change after a reboot, so really is a state change.
                userConfig->set(cfg_doorUpdateAt, (int)time(NULL));
                userConfig->save();
                lastDoorUpdateAt = upTime;
            }
        }
        else
        {
            lastDoorUpdateAt = (lastDoorState == 0xff) ? 0 : upTime;
        }
        // if no NTP....  lastDoorUpdateAt = (lastDoorState == 0xff) ? 0 : upTime;
        lastDoorState = garage_door.current_state;
        // We send milliseconds relative to current time... ie updated X milliseconds ago
        // First time through, zero offset from upTime, which is when we last rebooted)
        ADD_INT(json, "lastDoorUpdateAt", (upTime - lastDoorUpdateAt));
    }
    // Conditional macros, only add if value has changed
    // ADD_BOOL_C(json, "paired", homekit_is_paired(), last_reported_paired);
    ADD_STR_C(json, "garageDoorState", DOOR_STATE(garage_door.current_state), garage_door.current_state, last_reported_garage_door.current_state);
    ADD_STR_C(json, "garageLockState", LOCK_STATE(garage_door.current_lock), garage_door.current_lock, last_reported_garage_door.current_lock);
    ADD_BOOL_C(json, "garageLightOn", garage_door.light, last_reported_garage_door.light);
    ADD_BOOL_C(json, "garageMotion", garage_door.motion, last_reported_garage_door.motion);
    ADD_BOOL_C(json, "garageObstructed", garage_door.obstructed, last_reported_garage_door.obstructed);
    if (strlen(json) > 2)
    {
        // Have we added anything to the JSON string?
        ADD_INT(json, "upTime", upTime);
        END_JSON(json);
        REMOVE_NL(json);
        SSEBroadcastState(json);
    }
    /*
    if ((CFG_GET_INT("rebootSeconds") != 0) && ((unsigned long)CFG_GET_INT("rebootSeconds") < millis() / 1000))
    {
        // Reboot the system if we have reached time...
        ESP_LOGI(TAG,"Rebooting system as %i seconds expired", userConfig->rebootSeconds);
        server.stop();
        sync_and_restart();
        return;
    }
    */
    // server.handleClient();
}

void setup_web()
{
    ESP_LOGI(TAG, "=== Starting HTTP web server ===");
    json = (char *)malloc(JSON_BUFFER_SIZE);
    ESP_LOGI(TAG, "Allocated buffer for JSON, size: %d", JSON_BUFFER_SIZE);
    last_reported_paired = false; // TODO homekit_is_paired();

    if (motionTriggers.asInt == 0)
    {
        // maybe just initialized. If we have motion sensor then set that and write back to file
        if (garage_door.has_motion_sensor)
        {
            motionTriggers.bit.motion = 1;
            userConfig->set(cfg_motionTriggers, motionTriggers.asInt);
            userConfig->save();
        }
    }
    else if (garage_door.has_motion_sensor != (bool)motionTriggers.bit.motion)
    {
        // sync up web page tracking of whether we have motion sensor or not.
        ESP_LOGI(TAG, "Motion trigger mismatch, reset to %d", (uint8_t)garage_door.has_motion_sensor);
        motionTriggers.bit.motion = (uint8_t)garage_door.has_motion_sensor;
        userConfig->set(cfg_motionTriggers, motionTriggers.asInt);
        userConfig->save();
    }
    ESP_LOGI(TAG, "Motion triggers, motion : %d, obstruction: %d, light key: %d, door key: %d, lock key: %d, asInt: %d",
             motionTriggers.bit.motion,
             motionTriggers.bit.obstruction,
             motionTriggers.bit.lightKey,
             motionTriggers.bit.doorKey,
             motionTriggers.bit.lockKey,
             motionTriggers.asInt);
    lastDoorUpdateAt = 0;
    lastDoorState = (GarageDoorCurrentState)0xff;

#ifdef ENABLE_CRASH_LOG
    crashCount = saveCrash.count();
    if (crashCount == 255)
    {
        saveCrash.clear();
        crashCount = 0;
    }
#endif

    ESP_LOGI(TAG, "Registering URI handlers");

    httpd_config_t httpConfig = HTTPD_DEFAULT_CONFIG();
    httpConfig.lru_purge_enable = true;
    httpConfig.uri_match_fn = httpd_uri_match_wildcard;
    httpConfig.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP Server on port: %d", httpConfig.server_port);
    if (httpd_start(&httpServer, &httpConfig) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start file server!");
        return;
    }

    // Register URI handlers for URIs that have built-in handlers in this source file.
    /* URI handler for getting uploaded files */
    httpd_uri_t everything = {
        .uri = "*", // Match all URIs
        .method = (httpd_method_t)HTTP_ANY,
        .handler = handle_everything,
        .user_ctx = malloc(sizeof(userContext))};
    httpd_register_uri_handler(httpServer, &everything);

    /*
        server.on("/update", HTTP_POST, handle_update, handle_firmware_upload);
        server.onNotFound(handle_everything);
        // here the list of headers to be recorded
        const char *headerkeys[] = {"If-None-Match"};
        size_t headerkeyssize = sizeof(headerkeys) / sizeof(char *);
        // ask server to track these headers
        server.collectHeaders(headerkeys, headerkeyssize);
        server.begin();
    */
    // initialize all the Server-Sent Events (SSE) slots.
    for (uint8_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        subscription[i].SSEconnected = false;
        subscription[i].clientIP = INADDR_NONE;
        subscription[i].clientUUID.clear();
    }
    return;
}

uint32_t getRemoteIP(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr; // esp_http_server uses IPv6 addressing
    socklen_t addr_size = sizeof(addr);
    uint32_t *ipv4;

    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0)
    {
        ESP_LOGE(TAG, "Error getting client IP");
        return (uint32_t)INADDR_NONE;
    }
    ipv4 = (uint32_t *)&addr.sin6_addr.un.u32_addr[3];
    return *ipv4;
}

esp_err_t getRemoteIPstr(httpd_req_t *req, char *ipstr, size_t size)
{
    uint32_t ipv4 = getRemoteIP(req);
    inet_ntop(AF_INET, &ipv4, ipstr, size);
    return ESP_OK;
}

esp_err_t handle_notfound(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Sending 404 Not Found for: %s with method: %s to client: %s", req->uri, http_methods[req->method], CTX_CLIENT_IP_STR(req));
    httpd_resp_send_404(req);
    return ESP_OK;
}

esp_err_t handle_auth(httpd_req_t *req)
{
    /*
    if (userConfig->wwwPWrequired && !server.authenticateDigest(userConfig->wwwUsername, userConfig->wwwCredentials))
    {
        return server.requestAuthentication(DIGEST_AUTH, www_realm);
    }
    */
    const char *resp = "Authenticated";
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

esp_err_t handle_reset(httpd_req_t *req)
{
    /*
    if (userConfig->wwwPWrequired && !server.authenticateDigest(userConfig->wwwUsername, userConfig->wwwCredentials))
    {
        return server.requestAuthentication(DIGEST_AUTH, www_realm);
    }
    */
    ESP_LOGI(TAG, "... reset requested");
    const char *resp = "Device has been un-paired from HomeKit. Rebooting...\n";
    hap_reset_homekit_data();
    httpd_resp_sendstr(req, resp);
    // Allow time to process send() before terminating web server...
    // delay(500);
    // httpd_stop(httpServer);
    // hap_reboot_accessory();
    vTaskDelay(pdMS_TO_TICKS(500));
    sync_and_restart();
    return ESP_OK;
}

esp_err_t handle_reboot(httpd_req_t *req)
{
    ESP_LOGI(TAG, "... reboot requested");
    const char *resp = "Rebooting...\n";
    httpd_resp_sendstr(req, resp);
    // Allow time to process send() before terminating web server...
    // delay(500);
    // httpd_stop(httpServer);
    // hap_reboot_accessory();
    sync_and_restart();
    return ESP_OK;
}

esp_err_t handle_checkflash(httpd_req_t *req)
{
    // flashCRC = ESP.checkFlashCRC();
    ESP_LOGI(TAG, "checkFlashCRC: %s", flashCRC ? "true" : "false");
    httpd_resp_sendstr(req, flashCRC ? "true\n" : "false\n");
    return ESP_OK;
}

esp_err_t load_page(httpd_req_t *req, const char *page)
{
    if (webcontent.count(page) == 0)
        return handle_notfound(req);

    const char *data = (char *)std::get<0>(webcontent.at(page));
    int length = std::get<1>(webcontent.at(page));
    const char *type = std::get<2>(webcontent.at(page));
    // char type[MAX_MIME_TYPE_LEN];
    // strncpy(type, typeP, MAX_MIME_TYPE_LEN);
    //  Following for browser cache control...
    const char *crc32 = std::get<3>(webcontent.at(page)).c_str();
    bool cache = false;
    char cacheHdr[24] = "no-cache, no-store";
    char matchHdr[8] = "";
    if ((CACHE_CONTROL > 0) &&
        (!strcmp(type, type_css) || !strcmp(type, type_js) || strstr(type, ("image"))))
    {
        sprintf(cacheHdr, "max-age=%i", CACHE_CONTROL);
        cache = true;
    }

    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", cacheHdr);
    if (cache)
        httpd_resp_set_hdr(req, "ETag", crc32);

    httpd_req_get_hdr_value_str(req, "If-None-Match", matchHdr, 8);

    if (strcmp(crc32, matchHdr))
    {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        if (req->method == HTTP_HEAD)
        {
            ESP_LOGI(TAG, "Client %s requesting: %s (HTTP_HEAD, type: %s)", CTX_CLIENT_IP_STR(req), page, type);
            httpd_resp_send(req, NULL, 0);
        }
        else
        {
            ESP_LOGI(TAG, "Client %s requesting: %s (HTTP_GET, type: %s, length: %i)", CTX_CLIENT_IP_STR(req), page, type, length);
            httpd_resp_send(req, (const char *)data, length);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Sending 304 not modified to client %s requesting: %s (method: %s, type: %s)", CTX_CLIENT_IP_STR(req), page, http_methods[req->method], type);
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t handle_everything(httpd_req_t *req)
{
    CTX_CLIENT_IP(req) = getRemoteIP(req);

    strncpy(CTX_URL(req), req->uri, sizeof(CTX_URL(req)));
    if (char *p = strchr(CTX_URL(req), '?'))
    {
        *p = 0; // null terminate at the query string
    }
    const char *uri = CTX_URL(req); // truncated URI at the query string;
    if (CTX_CLIENT_IP(req) == INADDR_NONE)
    {
        // Bad client IP address, don't continue.
        ESP_LOGW(TAG, "Client with bad IP address requesting: %s (method: %s), sending 404 not found.", uri, http_methods[req->method]);
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    getRemoteIPstr(req, CTX_CLIENT_IP_STR(req), sizeof(CTX_CLIENT_IP_STR(req)));

    /*
    if ((WiFi.getMode() & WIFI_AP) == WIFI_AP)
    {
        // If we are in Soft Access Point mode
        ESP_LOGI(TAG, "WiFi Soft Access Point mode");
        if (page == "/" || page == "/ap")
            return handle_accesspoint(req);
        else if (page == "/setssid" && req->method == HTTP_POST)
            return handle_setssid(req);
        else if (page == "/reboot" && req->method == HTTP_POST)
            return handle_reboot(req);
        else if (page == "/rescan" && req->method == HTTP_POST)
        {
            wifi_scan();
            return handle_accesspoint(req);
        }
        else
            return handle_notfound(req);
    }
    */

    if (builtInUri.count(uri) > 0)
    {
        // requested page matches one of our built-in handlers
        ESP_LOGI(TAG, "Client %s requesting: %s (method: %s)", CTX_CLIENT_IP_STR(req), uri, http_methods[req->method]);
        if (req->method == builtInUri.at(uri).first)
            return builtInUri.at(uri).second(req);
        else
            return handle_notfound(req);
    }
    else if ((req->method == HTTP_GET) && (!strncmp(uri, restEvents, strlen(restEvents))))
    {
        // Request for "/rest/events/" with a channel number appended
        uri += strlen(restEvents);
        unsigned int channel = atoi(uri);
        if (channel < SSE_MAX_CHANNELS)
            return SSEHandler(req, channel);
        else
            return handle_notfound(req);
    }
    else if (req->method == HTTP_GET || req->method == HTTP_HEAD)
    {
        // HTTP_GET that does not match a built-in handler
        if (!strcmp(uri, "/"))
            return load_page(req, "/index.html");
        else
            return load_page(req, uri);
    }
    // it is a HTTP_POST for unknown URI
    return handle_notfound(req);
}

esp_err_t handle_status(httpd_req_t *req)
{
    unsigned long upTime = millis();
#define paired false
#define accessoryID "Unknown"
#define clientCount 0
    // Build the JSON string
    START_JSON(json);
    ADD_INT(json, "upTime", upTime);
    ADD_STR(json, cfg_deviceName, userConfig->getDeviceName().c_str());
    ADD_STR(json, "userName", userConfig->getwwwUsername().c_str());
    ADD_BOOL(json, "paired", paired);
    ADD_STR(json, "firmwareVersion", std::string(AUTO_VERSION).c_str());
    ADD_STR(json, "accessoryID", accessoryID);
    ADD_INT(json, "clients", clientCount);
    ADD_STR(json, cfg_localIP, userConfig->getLocalIP().c_str());
    ADD_STR(json, cfg_subnetMask, userConfig->getSubnetMask().c_str());
    ADD_STR(json, cfg_gatewayIP, userConfig->getGatewayIP().c_str());
    ADD_STR(json, cfg_nameserverIP, userConfig->getNameserverIP().c_str());
    ADD_STR(json, "macAddress", macAddress);
    wifi_ap_record_t apInfo;
    if (ESP_OK == esp_wifi_sta_get_ap_info(&apInfo))
    {
        ADD_STR(json, "wifiSSID", (char *)apInfo.ssid);
        ADD_STR(json, "wifiRSSI", (std::to_string(apInfo.rssi) + " dBm, Channel " + std::to_string(apInfo.primary)).c_str());
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", apInfo.bssid[0], apInfo.bssid[1], apInfo.bssid[2], apInfo.bssid[3], apInfo.bssid[4], apInfo.bssid[5]);
        ADD_STR(json, "wifiBSSID", bssid);
    }
    ADD_BOOL(json, "lockedAP", false);
    ADD_INT(json, cfg_GDOSecurityType, userConfig->getGDOSecurityType());
    ADD_STR(json, "garageDoorState", garage_door.active ? DOOR_STATE(garage_door.current_state) : DOOR_STATE(255));
    ADD_STR(json, "garageLockState", LOCK_STATE(garage_door.current_lock));
    ADD_BOOL(json, "garageLightOn", garage_door.light);
    ADD_BOOL(json, "garageMotion", garage_door.motion);
    ADD_BOOL(json, "garageObstructed", garage_door.obstructed);
    ADD_BOOL(json, cfg_passwordRequired, userConfig->getPasswordRequired());
    ADD_INT(json, cfg_rebootSeconds, userConfig->getRebootSeconds());
    ADD_INT(json, "freeHeap", esp_get_free_heap_size());
    ADD_INT(json, "minHeap", esp_get_minimum_free_heap_size());
    ADD_INT(json, "minStack", 0);
    ADD_INT(json, "crashCount", crashCount);
    ADD_INT(json, cfg_wifiPhyMode, userConfig->getWifiPhyMode());
    ADD_INT(json, cfg_wifiPower, userConfig->getWifiPower());
    ADD_BOOL(json, cfg_staticIP, userConfig->getStaticIP());
    ADD_BOOL(json, cfg_syslogEn, userConfig->getSyslogEn());
    ADD_STR(json, cfg_syslogIP, userConfig->getSyslogIP().c_str());
    ADD_INT(json, cfg_TTCseconds, userConfig->getTTCseconds());
    ADD_INT(json, cfg_motionTriggers, motionTriggers.asInt);
    ADD_INT(json, cfg_LEDidle, led->getIdleState());
    // We send milliseconds relative to current time... ie updated X milliseconds ago
    ADD_INT(json, "lastDoorUpdateAt", (upTime - lastDoorUpdateAt));
    ADD_BOOL(json, "enableNTP", enableNTP);
    if (enableNTP)
    {
        if (clockSet)
        {
            ADD_INT(json, "serverTime", time(NULL));
        }
        ADD_STR(json, cfg_timeZone, userConfig->getTimeZone().c_str());
    }
    ADD_BOOL(json, "checkFlashCRC", flashCRC);
    END_JSON(json);

    // send JSON straight to serial port
    printf("%s\n", json);
    ESP_LOGI(TAG, "JSON length: %d", strlen(json));
    last_reported_garage_door = garage_door;

    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGI(TAG, "JSON length: %d", strlen(json));
    return ESP_OK;
}

esp_err_t handle_logout(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle logout");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
    /*
    return server.requestAuthentication(DIGEST_AUTH, www_realm);
    */
}

esp_err_t httpd_load_multipart(httpd_req_t *req, char **buf)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Content-Type") + 1;
    char *hdr = (char *)malloc(len);
    httpd_req_get_hdr_value_str(req, "Content-Type", hdr, len);
    if (strstr(hdr, "multipart/form-data") == NULL)
    {
        free(hdr);
        return ESP_FAIL;
    }
    free(hdr);                                   // release buffer allocated for content type
    *buf = (char *)malloc(req->content_len + 1); // allocate for content, released in calling fn.
    size_t off = 0;
    while (off < req->content_len)
    {
        /* Read data received in the request */
        int ret = httpd_req_recv(req, *buf + off, req->content_len - off);
        if (ret <= 0)
        {
            free(*buf);
            return (ret == 0) ? ESP_FAIL : ret;
        }
        off += ret;
    }
    return ESP_OK;
}

char *getKeyValue(char *buf, char **key, char **value)
{
    char *p = strstr(buf, "Content-Disposition:");
    if (p)
    {
        *key = strstr(p, "name=\"") + 6;    // find key at name="
        *value = strstr(p, "\r\n\r\n") + 4; // find value at two newlines
        *strstr(*key, "\"") = 0;            // terminate key string at closing quote
        p = strstr(*value, "\r\n");         // move pointer to end of value
        *p++ = 0;                           // terminate value string at newline, move pointer
    }
    return p; // pointer to point we got to in buffer
}

esp_err_t handleResetDoor(char *key, char *value)
{
    ESP_LOGI(TAG, "Request to reset door rolling codes");
    reset_door();
    return ESP_OK;
}

esp_err_t handleGarageLightOn(char *key, char *value)
{
    set_light(!strcmp(value, "1") ? true : false);
    return ESP_OK;
}

esp_err_t handleGarageDoorState(char *key, char *value)
{
    if (!strcmp(value, "1"))
        open_door();
    else
        close_door();
    return ESP_OK;
}

esp_err_t handleGarageLockState(char *key, char *value)
{
    set_lock(!strcmp(value, "1") ? 1 : 0);
    return ESP_OK;
}

esp_err_t handleCredentials(char *key, char *value)
{
    char *newUsername = strstr(value, "username");
    char *newCredentials = strstr(value, "credentials");
    if (!(newUsername && newCredentials))
        return ESP_FAIL;

    // JSON string passed in.
    // Very basic parsing, not using library functions to save memory
    // find the colon after the key string
    newUsername = strchr(newUsername, ':') + 1;
    newCredentials = strchr(newCredentials, ':') + 1;
    // for strings find the double quote
    newUsername = strchr(newUsername, '"') + 1;
    newCredentials = strchr(newCredentials, '"') + 1;
    // null terminate the strings (at closing quote).
    *strchr(newUsername, '"') = (char)0;
    *strchr(newCredentials, '"') = (char)0;
    // save values...
    ESP_LOGI(TAG, "Set user credentials: %s : %s", newUsername, newCredentials);
    userConfig->set(cfg_wwwUsername, newUsername);
    userConfig->set(cfg_wwwCredentials, newCredentials);
    return ESP_OK;
}

esp_err_t handleUpdateUnderway(char *key, char *value)
{
    firmwareSize = 0;
    firmwareUpdateSub = NULL;
    char *md5 = strstr(value, "md5");
    char *size = strstr(value, "size");
    char *uuid = strstr(value, "uuid");

    if (!(md5 && size && uuid))
        return ESP_FAIL;

    // JSON string of passed in.
    // Very basic parsing, not using library functions to save memory
    // find the colon after the key string
    md5 = strchr(md5, ':') + 1;
    size = strchr(size, ':') + 1;
    uuid = strchr(uuid, ':') + 1;
    // for strings find the double quote
    md5 = strchr(md5, '"') + 1;
    uuid = strchr(uuid, '"') + 1;
    // null terminate the strings (at closing quote).
    *strchr(md5, '"') = (char)0;
    *strchr(uuid, '"') = (char)0;
    // ESP_LOGI(TAG,"MD5: %s, UUID: %s, Size: %d", md5, uuid, atoi(size));
    // save values...
    strlcpy(firmwareMD5, md5, sizeof(firmwareMD5));
    firmwareSize = atoi(size);
    /* TODO
    for (uint8_t channel = 0; channel < SSE_MAX_CHANNELS; channel++)
    {
        if (subscription[channel].SSEconnected && subscription[channel].clientUUID == uuid && subscription[channel].client.connected())
        {
            firmwareUpdateSub = &subscription[channel];
            break;
        }
    }
    */
    return ESP_OK;
}

esp_err_t handle_setgdo(httpd_req_t *req)
{
    // Build-in handlers that do not set a configuration value, or set multiple values.
    // key, {reboot, wifiChanged, value, fn to call}
    static const std::unordered_map<std::string, configSetting> setGDOhandlers = {
        {"resetDoor", {true, false, 0, handleResetDoor}},
        {"garageLightOn", {false, false, 0, handleGarageLightOn}},
        {"garageDoorState", {false, false, 0, handleGarageDoorState}},
        {"garageLockState", {false, false, 0, handleGarageLockState}},
        {"credentials", {false, false, 0, handleCredentials}}, // parse out wwwUsername and credentials
        {"updateUnderway", {false, false, 0, handleUpdateUnderway}},
    };

    bool reboot = false;
    bool error = false;
    bool wifiChanged = false;
    bool saveSettings = false;
    char *ptr;
    char *key;
    char *value;
    configSetting actions;
    char *form; // malloc() in httpd_load_multipart, must free() here.

    esp_err_t err = httpd_load_multipart(req, &form);
    if (err)
    {
        ESP_LOGI(TAG, "Sending %s, for: %s", response400invalid, CTX_URL(req));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response400invalid);
    };

    ptr = form;
    while (!error && (ptr = getKeyValue(ptr, &key, &value)))
    {
        // TODO... add authentication for all except "timeZone"
        if (setGDOhandlers.count(key))
        {
            ESP_LOGI(TAG, "Call handler for Key: %s, Value: %s", key, value);
            actions = setGDOhandlers.at(key);
            if (actions.fn)
            {
                error = error || (actions.fn(key, value) != ESP_OK);
                reboot = reboot || actions.reboot;
                wifiChanged = wifiChanged || actions.wifiChanged;
            }
        }
        else if (userConfig->set(key, value))
        {
            ESP_LOGI(TAG, "Set configuration for Key: %s, Value: %s", key, value);
            actions = userConfig->getDetail(key);
            if (actions.fn)
            {
                error = error || (actions.fn(key, value) != ESP_OK);
                // TODO, set these in handler function called in line above
                reboot = reboot || actions.reboot;
                wifiChanged = wifiChanged || actions.wifiChanged;
            }
            else
            {
                reboot = reboot || actions.reboot;
                wifiChanged = wifiChanged || actions.wifiChanged;
            }
            saveSettings = true;
        }
        else
        {
            ESP_LOGW(TAG, "Invalid Key: %s, Value: %s (F)", key, value);
            error = true;
        }
    }
    free(form);

    ESP_LOGI(TAG, "SetGDO Complete");

    if (error)
    {
        // Simple error handling...
        ESP_LOGI(TAG, "Sending %s, for: %s", response400invalid, CTX_URL(req));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response400invalid);
    }

    if (saveSettings)
    {
        userConfig->set(cfg_wifiSettingsChanged, wifiChanged);
        userConfig->save();
    }
    if (reboot)
    {
        httpd_resp_set_type(req, type_html);
        httpd_resp_sendstr(req, "<p>Success. Reboot.</p>");
        // Some settings require reboot to take effect
        ESP_LOGI(TAG, "SetGDO Restart required");
        // Allow time to process send() before terminating web server...
        // delay(500);
        vTaskDelay(pdMS_TO_TICKS(500));
        sync_and_restart();
    }
    else
    {
        httpd_resp_set_type(req, type_html);
        httpd_resp_sendstr(req, "<p>Success.</p>");
    }
    return ESP_OK;
}

void SSEheartbeat(SSESubscription *s)
{
    if (!s)
        return;

    if (!(s->clientIP))
        return;

    char clientIPstr[16];
    inet_ntop(AF_INET, &(s->clientIP), clientIPstr, sizeof(clientIPstr));

    if (!(s->SSEconnected))
    {
        if (s->SSEfailCount++ >= 5)
        {
            // 5 heartbeats have failed... assume client will not connect
            // and free up the slot
            subscriptionCount--;
            xTimerDelete(s->heartbeatTimer, 100);
            s->clientIP = INADDR_NONE;
            s->clientUUID.clear();
            s->SSEconnected = false;
            ESP_LOGI(TAG, "Client %s timeout waiting to listen, remove SSE subscription.  Total subscribed: %d", clientIPstr, subscriptionCount);
            // no need to stop client socket because it is not live yet.
        }
        else
        {
            ESP_LOGI(TAG, "Client %s not yet listening for SSE", clientIPstr);
        }
        return;
    }

    // if (s->client.connected())
    if (s->clientIP && (s->clientIP != INADDR_NONE))
    {
        static int8_t lastRSSI = 0;
        // static int lastClientCount = 0;
        static std::string sse_resp;

        START_JSON(json);
        ADD_INT(json, "upTime", millis());
        ADD_INT(json, "freeHeap", esp_get_free_heap_size());
        ADD_INT(json, "minHeap", esp_get_minimum_free_heap_size());
        ADD_INT(json, "minStack", uxTaskGetStackHighWaterMark(NULL));
        ADD_BOOL(json, "checkFlashCRC", flashCRC);

        wifi_ap_record_t apInfo;
        if (ESP_OK == esp_wifi_sta_get_ap_info(&apInfo))
        {
            if (lastRSSI != apInfo.rssi)
            {
                lastRSSI = apInfo.rssi;
                ADD_STR(json, "wifiRSSI", (std::to_string(apInfo.rssi) + " dBm, Channel " + std::to_string(apInfo.primary)).c_str());
            }
        }
        /*
        if (arduino_homekit_get_running_server() && arduino_homekit_get_running_server()->nfds != lastClientCount)
        {
            lastClientCount = arduino_homekit_get_running_server()->nfds;
            ADD_INT(json, "clients", lastClientCount);
        }
        */
        END_JSON(json);
        REMOVE_NL(json);
        int i = strlen(json);
        json[i++] = '\n'; // end the JSON string with two newlines
        json[i++] = '\n';
        json[i++] = '\0';
        sse_resp = "event: message\nretry: 15000\ndata: ";
        sse_resp += json;
        if (httpd_socket_send(s->socketHD, s->socketFD, sse_resp.c_str(), sse_resp.length(), 0) < 0)
        {
            s->clientIP = INADDR_NONE;
            s->clientUUID.clear();
            ESP_LOGI(TAG, "SSE Heartbeat socket to client %s broken", clientIPstr);
        }
    }
    else
    {
        subscriptionCount--;
        xTimerDelete(s->heartbeatTimer, 100);
        s->clientIP = INADDR_NONE;
        s->clientUUID.clear();
        s->SSEconnected = false;
        ESP_LOGI(TAG, "Client %s not listening, remove SSE subscription. Total subscribed: %d", clientIPstr, subscriptionCount);
    }
}

void xTimerHeartbeat(TimerHandle_t xTimer)
{
    return SSEheartbeat((SSESubscription *)pvTimerGetTimerID(xTimer));
}

std::unordered_map<std::string, std::string> parseQueryString(const std::string &query)
{
    std::unordered_map<std::string, std::string> result;
    std::string key, value;
    std::string::size_type pos = 0, nextPos;

    while (pos < query.size())
    {
        nextPos = query.find('&', pos);
        if (nextPos == std::string::npos)
        {
            nextPos = query.size();
        }

        std::string pair = query.substr(pos, nextPos - pos);
        std::string::size_type eqPos = pair.find('=');

        if (eqPos != std::string::npos)
        {
            key = pair.substr(0, eqPos);
            value = pair.substr(eqPos + 1);
        }
        else
        {
            key = pair;
            value = "";
        }

        result[key] = value;
        pos = nextPos + 1;
    }
    return result;
}

esp_err_t SSEHandler(httpd_req_t *req, uint8_t channel)
{
    // httpd_resp_send(req, NULL, 0);
    ESP_LOGI(TAG, "SSE handler for channel: %d", channel);
    static char sse_resp[] = "HTTP/1.1 200 OK\nContent-Type: text/event-stream;\nConnection: keep-alive\nCache-Control: no-cache\nAccess-Control-Allow-Origin: *\n\n";
    char *p = strchr(req->uri, '?');
    if (!p)
    {
        ESP_LOGI(TAG, "Sending %s, for: %s", response400missing, CTX_URL(req));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response400missing);
    }
    // find the UUID and whether client wants to receive log messages
    std::unordered_map<std::string, std::string> params = parseQueryString(++p);
    ESP_LOGI(TAG, "SSE handler for UUID: %s", params.at("id").c_str());
    SSESubscription &s = subscription[channel];
    if (s.clientUUID != params.at("id"))
    {
        ESP_LOGI(TAG, "Client %s with IP %s tries to listen for SSE but not subscribed", params.at("id").c_str(), CTX_CLIENT_IP_STR(req));
        return handle_notfound(req);
    }
    s.socketHD = req->handle;
    s.socketFD = httpd_req_to_sockfd(req);
    httpd_socket_send(s.socketHD, s.socketFD, sse_resp, sizeof(sse_resp), 0);
    s.SSEconnected = true;
    s.SSEfailCount = 0;
    ESP_LOGI(TAG, "Start heartbeat timer for: %s", CTX_URL(req));
    s.heartbeatTimer = xTimerCreate(CTX_URL(req), pdMS_TO_TICKS(1000), pdTRUE, &s, xTimerHeartbeat);
    xTimerStart(s.heartbeatTimer, 100);
    // s.heartbeatTimer.attach_scheduled(1.0, [channel, &s] { SSEheartbeat(&s); });
    ESP_LOGI(TAG, "Client %s listening for SSE events on channel %d", CTX_CLIENT_IP_STR(req), channel);
    return ESP_OK;
}

esp_err_t handle_subscribe(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle subscribe");
    uint8_t channel;
    std::string SSEurl = restEvents;

    if (subscriptionCount == SSE_MAX_CHANNELS)
    {
        ESP_LOGI(TAG, "Client %s SSE Subscription declined, subscription count: %d", CTX_CLIENT_IP_STR(req), subscriptionCount);
        for (channel = 0; channel < SSE_MAX_CHANNELS; channel++)
        {
            ESP_LOGI(TAG, "Client %d: %s at %s", channel, subscription[channel].clientUUID.c_str(), "<TODO client IP>");
        }
        return handle_notfound(req); // We ran out of channels
    }

    if (CTX_CLIENT_IP(req) == INADDR_NONE)
    {
        ESP_LOGI(TAG, "Sending %s, for: %s as clientIP missing", response400invalid, CTX_URL(req));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response400invalid);
    }

    // check we were passed at least one query arg
    char *p = strchr(req->uri, '?');
    if (!p)
    {
        ESP_LOGI(TAG, "Sending %s, for: %s", response400invalid, CTX_URL(req));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response400invalid);
    }
    // find the UUID and whether client wants to receive log messages
    std::unordered_map<std::string, std::string> params = parseQueryString(++p);
    bool logViewer = params.count("log") > 0;
    // check if we already have a subscription for this UUID
    for (channel = 0; channel < SSE_MAX_CHANNELS; channel++)
    {
        SSESubscription &s = subscription[channel];
        if (s.clientUUID == params.at("id"))
        {
            if (s.SSEconnected)
            {
                // Already connected.  We need to close it down as client will be reconnecting
                ESP_LOGI(TAG, "SSE Subscribe - client %s with IP %s already connected on channel %d, remove subscription", params.at("id").c_str(), CTX_CLIENT_IP_STR(req), channel);
                xTimerDelete(s.heartbeatTimer, 100);
                s.socketHD = NULL;
                s.socketFD = 0;
            }
            else
            {
                // Subscribed but not connected yet, so nothing to close down.
                ESP_LOGI(TAG, "SSE Subscribe - client %s with IP %s already subscribed but not connected on channel %d", params.at("id").c_str(), CTX_CLIENT_IP_STR(req), channel);
            }
            break;
        }
    }

    if (channel == SSE_MAX_CHANNELS)
    {
        // ended loop above without finding a match, so need to allocate a free slot
        ++subscriptionCount;
        for (channel = 0; channel < SSE_MAX_CHANNELS; channel++)
            if (!subscription[channel].clientIP || subscription[channel].clientIP == INADDR_NONE)
                break;
    }

    //  clientIP; socketHD; socketFD; heartbeatTimer; SSEconnected; SSEfailCount; clientUUID; logViewer;
    subscription[channel] = {CTX_CLIENT_IP(req), req->handle, httpd_req_to_sockfd(req), NULL, false, 0, params.at("id"), logViewer};
    SSEurl += std::to_string(channel);
    ESP_LOGI(TAG, "SSE Subscription for client %s with IP %s: event bus location: %s, Total subscribed: %d", params.at("id").c_str(), CTX_CLIENT_IP_STR(req), SSEurl.c_str(), subscriptionCount);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_type(req, type_txt);
    httpd_resp_sendstr(req, SSEurl.c_str());
    return ESP_OK;
}

esp_err_t handle_crashlog(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Request to display crash log...");
#ifdef LOG_MSG_BUFFER
    if (crashCount > 0)
    {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
        httpd_resp_set_type(req, type_txt);
        sendCrashLog(req);
    }
    else
    {
        httpd_resp_send(req, NULL, 0);
    }
#else
    httpd_resp_send(req, NULL, 0);
#endif
    return ESP_OK;
}

esp_err_t handle_showlog(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle showlog");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_type(req, type_txt);
#ifdef LOG_MSG_BUFFER
    sendMessageLog(req);
#else
    httpd_resp_send(req, NULL, 0);
#endif
    return ESP_OK;
}

esp_err_t handle_showrebootlog(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle showrebootlog");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_type(req, type_txt);
#ifdef LOG_MSG_BUFFER
    sendRebootLog(req);
#else
    httpd_resp_send(req, NULL, 0);
#endif
    return ESP_OK;
}

esp_err_t handle_clearcrashlog(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle clearcrashlog");
    /*
    if (userConfig->wwwPWrequired && !server.authenticateDigest(userConfig->wwwUsername, userConfig->wwwCredentials))
    {
        return server.requestAuthentication(DIGEST_AUTH, www_realm);
    }
    */
    // saveCrash.clear();
    crashCount = 0;
    httpd_resp_set_type(req, type_txt);
    httpd_resp_sendstr(req, "Crash log cleared\n");
    return ESP_OK;
}

#ifdef CRASH_DEBUG
esp_err_t handle_crash_oom(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Attempting to use up all memory");
    server.send(200, type_txt, ("Attempting to use up all memory\n"));
    delay(1000);
    for (int i = 0; i < 30; i++)
    {
        crashptr = malloc(1024);
    }
}

esp_err_t handle_forcecrash(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Attempting to null ptr deref");
    server.send(200, type_txt, ("Attempting to null ptr deref\n"));
    delay(1000);
    ESP_LOGI(TAG, "Result: %s", test_str);
}
#endif

void SSEBroadcastState(const char *data, BroadcastType type)
{
    // Flash LED to signal activity
    // led.flash(FLASH_MS);

    // if nothing subscribed, then return
    if (subscriptionCount == 0)
        return;

    static std::string sse_resp;
    for (uint8_t i = 0; i < SSE_MAX_CHANNELS; i++)
    {
        SSESubscription *s = &subscription[i];
        if (s->SSEconnected && s->clientIP && (s->clientIP != INADDR_NONE))
        {
            char clientIPstr[16];
            inet_ntop(AF_INET, &(s->clientIP), clientIPstr, sizeof(clientIPstr));
            if (type == LOG_MESSAGE)
            {
                if (s->logViewer)
                {
                    // s->client.printf(("event: logger\ndata: %s\n\n"), data);
                    sse_resp = std::format("event: logger\ndata: {}\n\n", data);
                    if (httpd_socket_send(s->socketHD, s->socketFD, sse_resp.c_str(), sse_resp.length(), 0) < 0)
                    {
                        s->clientIP = INADDR_NONE;
                        s->clientUUID.clear();
                        ESP_LOGI(TAG, "SSE socket to client %s broken", clientIPstr);
                    }
                }
            }
            else if (type == RATGDO_STATUS)
            {
                ESP_LOGI(TAG, "SSE send to client %s on channel %d, data: %s", clientIPstr, i, data);
                sse_resp = std::format("event: message\ndata: {}\n\n", data);
                if (httpd_socket_send(s->socketHD, s->socketFD, sse_resp.c_str(), sse_resp.length(), 0) < 0)
                {
                    s->clientIP = INADDR_NONE;
                    s->clientUUID.clear();
                    ESP_LOGI(TAG, "SSE socket to client %s broken", clientIPstr);
                }
            }
        }
    }
}

// Implement our own firmware update so can enforce MD5 check.
// Based on ESP8266HTTPUpdateServer
void _setUpdaterError()
{
    /*
    StreamString str;
    Update.printError(str);
    _updaterError = str.c_str();
    ESP_LOGI(TAG, "Update error: %s", str.c_str());
    */
}

bool check_flash_md5(uint32_t flashAddr, uint32_t size, const char *expectedMD5)
{
    return true;
    /*
    MD5Builder md5 = MD5Builder();
    uint8_t buffer[128];
    uint32_t pos = 0;

    md5.begin();
    while (pos < size)
    {
        size_t read_size = ((size - pos) > sizeof(buffer)) ? sizeof(buffer) : size - pos;
        ESP.flashRead(flashAddr + pos, buffer, read_size);
        md5.add(buffer, read_size);
        pos += sizeof(buffer);
    }
    md5.calculate();
    ESP_LOGI(TAG, "Flash MD5: %s", md5.toString().c_str());
    return (strcmp(md5.toString().c_str(), expectedMD5) == 0);
    */
}

esp_err_t handle_update(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle update");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
    /*
    bool verify = !strcmp(server.arg("action").c_str(), "verify");

    server.sendHeader(F("Access-Control-Allow-Headers"), "*");
    server.sendHeader(F("Access-Control-Allow-Origin"), "*");
    if (userConfig->wwwPWrequired && !server.authenticateDigest(userConfig->wwwUsername, userConfig->wwwCredentials))
    {
        ESP_LOGI(TAG, "In handle_update request authentication");
        return server.requestAuthentication(DIGEST_AUTH, www_realm);
    }

    server.client().setNoDelay(true);
    if (!verify && Update.hasError())
    {
        // Error logged in _setUpdaterError
        eboot_command_clear();
        RERROR("Firmware upload error. Aborting update, not rebooting");
        server.send(400, "text/plain", _updaterError);
        return;
    }
    else
    {
        ESP_LOGI(TAG, "Received MD5: %s", Update.md5String().c_str());
        struct eboot_command ebootCmd;
        eboot_command_read(&ebootCmd);
        // ESP_LOGI(TAG,"eboot_command: 0x%08X 0x%08X [0x%08X 0x%08X 0x%08X (%d)]", ebootCmd.magic, ebootCmd.action, ebootCmd.args[0], ebootCmd.args[1], ebootCmd.args[2], ebootCmd.args[2]);
        if (!check_flash_md5(ebootCmd.args[0], firmwareSize, firmwareMD5))
        {
            // MD5 of flash does not match expected MD5
            eboot_command_clear();
            RERROR("Flash MD5 does not match expected MD5. Aborting update, not rebooting");
            server.send(400, "text/plain", "Flash MD5 does not match expected MD5.");
            return;
        }
    }

    if (server.args() > 0)
    {
        // Don't reboot, user/client must explicity request reboot.
        server.send(200, type_txt, ("Upload Success.\n"));
    }
    else
    {
        // Legacy... no query string args, so automatically reboot...
        server.send(200, type_txt, ("Upload Success. Rebooting...\n"));
        // Allow time to process send() before terminating web server...
        delay(500);
        server.stop();
        sync_and_restart();
    }
    */
}

esp_err_t handle_firmware_upload(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle firmware upload");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
    /*
    // handler for the file upload, gets the sketch bytes, and writes
    // them through the Update object
    static size_t uploadProgress;
    static unsigned int nextPrintPercent;
    HTTPUpload &upload = server.upload();
    static bool verify = false;
    static size_t size = 0;
    static const char *md5 = NULL;

    if (upload.status == UPLOAD_FILE_START)
    {
        _updaterError.clear();

        _authenticatedUpdate = !userConfig->wwwPWrequired || server.authenticateDigest(userConfig->wwwUsername, userConfig->wwwCredentials);
        if (!_authenticatedUpdate)
        {
            ESP_LOGI(TAG, "Unauthenticated Update");
            return;
        }
        ESP_LOGI(TAG, "Update: %s", upload.filename.c_str());
        verify = !strcmp(server.arg("action").c_str(), "verify");
        size = atoi(server.arg("size").c_str());
        md5 = server.arg("md5").c_str();

        // We are updating.  If size and MD5 provided, save them
        firmwareSize = size;
        if (strlen(md5) > 0)
            strlcpy(firmwareMD5, md5, sizeof(firmwareMD5));

        uint32_t maxSketchSpace = ESP.getFreeSketchSpace();
        ESP_LOGI(TAG, "Available space for upload: %lu", maxSketchSpace);
        ESP_LOGI(TAG, "Firmware size: %s", (firmwareSize > 0) ? std::to_string(firmwareSize).c_str() : "Unknown");
        ESP_LOGI(TAG, "Flash chip speed %d MHz", ESP.getFlashChipSpeed() / 1000000);
        // struct eboot_command ebootCmd;
        // eboot_command_read(&ebootCmd);
        // ESP_LOGI(TAG,"eboot_command: 0x%08X 0x%08X [0x%08X 0x%08X 0x%08X (%d)]", ebootCmd.magic, ebootCmd.action, ebootCmd.args[0], ebootCmd.args[1], ebootCmd.args[2], ebootCmd.args[2]);
        if (!verify)
        {
            // Close HomeKit server so we don't have to handle HomeKit network traffic during update
            // Only if not verifying as either will have been shutdown on immediately prior upload, or we
            // just want to verify without disrupting operation of the HomeKit service.
            arduino_homekit_close();
        }
        if (!verify && !Update.begin((firmwareSize > 0) ? firmwareSize : maxSketchSpace, U_FLASH))
        {
            _setUpdaterError();
        }
        else if (strlen(firmwareMD5) > 0)
        {
            // uncomment for testing...
            // char firmwareMD5[] = "675cbfa11d83a792293fdc3beb199cXX";
            ESP_LOGI(TAG, "Expected MD5: %s", firmwareMD5);
            Update.setMD5(firmwareMD5);
            if (firmwareSize > 0)
            {
                uploadProgress = 0;
                nextPrintPercent = 10;
                ESP_LOGI(TAG, "%s progress: 00%%", verify ? "Verify" : "Update");
            }
        }
    }
    else if (_authenticatedUpdate && upload.status == UPLOAD_FILE_WRITE && !_updaterError.length())
    {
        // Progress dot dot dot
        Serial.printf(".");
        if (firmwareSize > 0)
        {
            uploadProgress += upload.currentSize;
            unsigned int uploadPercent = (uploadProgress * 100) / firmwareSize;
            if (uploadPercent >= nextPrintPercent)
            {
                Serial.printf("\n"); // newline after the dot dot dots
                ESP_LOGI(TAG, "%s progress: %i%%", verify ? "Verify" : "Update", uploadPercent);
                SSEheartbeat(firmwareUpdateSub); // keep SSE connection alive.
                nextPrintPercent += 10;
                // Report percentage to browser client if it is listening
                if (firmwareUpdateSub && firmwareUpdateSub->client.connected())
                {
                    START_JSON(json);
                    ADD_INT(json, "uploadPercent", uploadPercent);
                    END_JSON(json);
                    REMOVE_NL(json);
                    firmwareUpdateSub->client.printf(("event: uploadStatus\ndata: %s\n\n"), json);
                }
            }
        }
        if (!verify)
        {
            // Don't write if verifying... we will just check MD5 of the flash at the end.
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                _setUpdaterError();
        }
    }
    else if (_authenticatedUpdate && upload.status == UPLOAD_FILE_END && !_updaterError.length())
    {
        Serial.printf("\n"); // newline after last of the dot dot dots
        if (!verify)
        {
            if (Update.end(true))
            {
                ESP_LOGI(TAG, "Upload size: %zu", upload.totalSize);
            }
            else
            {
                _setUpdaterError();
            }
        }
    }
    else if (_authenticatedUpdate && upload.status == UPLOAD_FILE_ABORTED)
    {
        if (!verify)
            Update.end();
        ESP_LOGI(TAG, "%s was aborted", verify ? "Verify" : "Update");
    }
    esp_yield();
    */
}

esp_err_t handle_accesspoint(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle accesspoint");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
    /*
    bool connected = WiFi.isConnected();
    std::string previousSSID = "";
    bool match = false;
    if (connected)
    {
        previousSSID = WiFi.SSID();
    }
    ESP_LOGI(TAG, "Number of WiFi networks: %d", wifiNets.size());
    std::string currentSSID = "";
    WiFiClient client = server.client();

    client.print(softAPhttpPreamble);
    client.print("<html><head>");
    client.print(softAPstyle);
    client.print(softAPscript);
    client.print("</head><body style='font-family: monospace'");
    client.print(softAPtableHead);
    int i = 0;
    for (wifiNet_t net : wifiNets)
    {
        bool hide = true;
        bool matchSSID = (previousSSID == net.ssid);
        if (matchSSID)
            match = true;
        if (currentSSID != net.ssid)
        {
            currentSSID = net.ssid;
            hide = false;
        }
        else
        {
            matchSSID = false;
        }
        client.printf(softAPtableRow, (hide) ? "class='adv'" : "", i, (matchSSID) ? "checked='checked'" : "",
                      net.ssid.c_str(), net.rssi, net.channel,
                      net.bssid[0], net.bssid[1], net.bssid[2], net.bssid[3], net.bssid[4], net.bssid[5]);
        i++;
    }
    // user entered value
    client.printf(softAPtableLastRow, i, (!match) ? previousSSID.c_str() : "");
    client.print(softAPtableFoot);
    client.print("</body></html>");
    client.flush();
    client.stop();
    return;
    */
}

esp_err_t handle_setssid(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handle setssid");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
    /*
    if (server.args() < 3)
    {
        ESP_LOGI(TAG, "Sending %s, for: %s as invalid number of args", response400invalid, server.uri().c_str());
        server.send(400, type_txt, response400invalid);
        return;
    }

    const unsigned int net = atoi(server.arg("net").c_str());
    const char *pw = server.arg("pw").c_str();
    const char *userSSID = server.arg("userSSID").c_str();
    const char *ssid = userSSID;
    bool advanced = server.arg("advanced") == "on";
    wifiNet_t wifiNet;

    if (net < wifiNets.size())
    {
        // User selected network from within scanned range
        wifiNet = *std::next(wifiNets.begin(), net);
        ssid = wifiNet.ssid.c_str();
    }
    else
    {
        // Outside scanned range, do not allow locking to access point
        advanced = false;
    }

    if (advanced)
    {
        ESP_LOGI(TAG, "Requested WiFi SSID: %s (%d) at AP: %02x:%02x:%02x:%02x:%02x:%02x",
                 ssid, net, wifiNet.bssid[0], wifiNet.bssid[1], wifiNet.bssid[2], wifiNet.bssid[3], wifiNet.bssid[4], wifiNet.bssid[5]);
        snprintf(json, JSON_BUFFER_SIZE, ("Setting SSID to: %s locked to Access Point: %02x:%02x:%02x:%02x:%02x:%02x\nRATGDO rebooting.\nPlease wait 30 seconds and connect to RATGDO on new network."),
                 ssid, wifiNet.bssid[0], wifiNet.bssid[1], wifiNet.bssid[2], wifiNet.bssid[3], wifiNet.bssid[4], wifiNet.bssid[5]);
    }
    else
    {
        ESP_LOGI(TAG, "Requested WiFi SSID: %s (%d)", ssid);
        snprintf(json, JSON_BUFFER_SIZE, ("Setting SSID to: %s\nRATGDO rebooting.\nPlease wait 30 seconds and connect to RATGDO on new network."), ssid);
    }
    server.client().setNoDelay(true);
    server.send(200, type_txt, json);
    delay(500);
    server.stop();

    const bool connected = WiFi.isConnected();
    std::string previousSSID;
    std::string previousPSK;
    std::string previousBSSID;
    if (connected)
    {
        previousSSID = WiFi.SSID();
        previousPSK = WiFi.psk();
        previousBSSID = WiFi.BSSIDstr();
        ESP_LOGI(TAG, "Current SSID: %s / BSSID:%s", previousSSID.c_str(), previousBSSID.c_str());
        WiFi.disconnect();
    }

    if (connect_wifi(ssid, pw, (advanced) ? wifiNet.bssid : NULL))
    {
        ESP_LOGI(TAG, "WiFi Successfully connects to SSID: %s", ssid);
        // We should reset WiFi if changing networks or were not currently connected.
        if (!connected || previousBSSID != ssid)
        {
            userConfig->staticIP = false;
            userConfig->wifiPower = 20;
            userConfig->wifiPhyMode = 0;
#ifdef NTP_CLIENT
            userConfig->timeZone[0] = 0;
#endif
        }
    }
    else
    {
        ESP_LOGI(TAG, "WiFi Failed to connect to SSID: %s", ssid);
        if (connected)
        {
            ESP_LOGI(TAG, "Resetting WiFi to previous SSID: %s, removing any Access Point BSSID lock", previousSSID.c_str());
            connect_wifi(previousSSID.c_str(), previousPSK.c_str());
        }
        else
        {
            // We were not connected, and we failed to connext to new SSID,
            // so best to reset any wifi settings.
            userConfig->staticIP = false;
            userConfig->wifiPower = 20;
            userConfig->wifiPhyMode = 0;
#ifdef NTP_CLIENT
            userConfig->timeZone[0] = 0;
#endif
        }
    }
    sync_and_restart();
    return;
    */
}