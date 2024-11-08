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
#include <algorithm>

// ESP system includes
#include <esp_log.h>
#include <esp_err.h>
#include <esp_mac.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>

// RATGDO project includes
#include "utilities.h"
#include "config.h"
#include "log.h"
#include "comms.h"
#include "led.h"

// Logger tag
static const char *TAG = "ratgdo-utils";

// What trigger motion...
motionTriggersUnion motionTriggers = {0, 0, 0, 0, 0, 0};
// Control booting into soft access point mode
bool softAPmode = false;
// Realm for MD5 credential hashing
const char www_realm[] = "RATGDO Login Required";
// Controls whether to log to syslog server
bool syslogEn = false;

bool clockSet = false;
bool enableNTP = false;
unsigned long lastRebootAt = 0;
int32_t savedDoorUpdateAt = 0;

bool get_tz()
{
    bool success = false;
    /*
    WiFiClient client;
    HTTPClient http;


    ESP_LOGI(TAG, "Get timezone automatically based on IP address");
    if (http.begin(client, "http://ip-api.com/csv/?fields=timezone"))
    {
        // start connection and send HTTP header
        int httpCode = http.GET();
        // httpCode will be negative on error
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
        {
            String tz = http.getString();
            tz.trim();
            userConfig->set(cfg_timeZone, tz.c_str());
            ESP_LOGI(TAG, "Automatic timezone set to: %s", userConfig->getTimeZone().c_str());
            success = true;
        }
        http.end();
    }
    */
    return success;
}

void time_is_set(timeval *tv)
{
    clockSet = true;
    ESP_LOGI(TAG, "Current time: %s", timeString());
    if (userConfig->getTimeZone().length() == 0)
    {
        // no timeZone set, try and find it automatically
        get_tz();
        // if successful this will have set the region and city, but not
        // the POSIX time zone code. That will be done by browser.
    }
    return;
}

char *timeString(time_t reqTime, bool syslog)
{
    // declare static so we don't use stack space
    static char tBuffer[32];
    static time_t tTime = 0;
    static tm tmTime;
    tBuffer[0] = 0;
    tTime = ((reqTime == 0) && clockSet) ? time(NULL) : reqTime;
    if (tTime != 0)
    {
        localtime_r(&tTime, &tmTime);
        if (syslog)
        {
            // syslog compatibe
            strftime(tBuffer, sizeof(tBuffer), "%Y-%m-%dT%H:%M:%S.000%z", &tmTime);
            // %z returns e.g. "-400" or "+1000", we need it to be "-4:00" or "+10:00"
            // thie format is REQUIRED by syslog
            int i = strlen(tBuffer);
            if (tBuffer[i - 5] == '-' || tBuffer[i - 5] == '+' ||
                tBuffer[i - 6] == '-' || tBuffer[i - 6] == '+')
            {
                tBuffer[i + 1] = 0;
                tBuffer[i] = tBuffer[i - 1];
                tBuffer[i - 1] = tBuffer[i - 2];
                tBuffer[i - 2] = ':';
            }
        }
        else
        {
            // Print format example: 27-Oct-2024 11:16:18 EDT
            strftime(tBuffer, sizeof(tBuffer), "%d-%b-%Y %H:%M:%S %Z", &tmTime);
        }
    }
    return tBuffer;
}

char *make_rfc952(char *dest, const char *src, int size)
{
    // Make device name RFC952 complient (simple, just checking for the basics)
    // RFC952 says max len of 24, [a-z][A-Z][0-9][-.] and no dash or period in last char.
    int i = 0;
    while (i <= std::min(24, size - 1) && src[i] != 0)
    {
        dest[i] = (std::isspace((unsigned char)src[i])) ? '-' : src[i];
        i++;
    }
    // remove dashes and periods from end of name
    while (i > 0 && (dest[i - 1] == '-' || dest[i - 1] == '.'))
    {
        dest[--i] = 0;
    }
    // null terminate string
    dest[std::min(i, std::min(24, size - 1))] = 0;
    return dest;
}

void load_all_config_settings()
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(device_name, DEVICE_NAME_SIZE, "Garage Door %02X%02X%02X", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "=== Load all config settings for %s", device_name);

    // Force reset to defaults
    // userConfig->save();
    userConfig->set(cfg_deviceName, device_name);
    if (!userConfig->load())
    {
        ESP_LOGI(TAG, "No settings saved, using factory defaults.");
        userConfig->save();
    }

    // Check we have a legal device name...
    make_rfc952(device_name_rfc952, userConfig->getDeviceName().c_str(), sizeof(device_name_rfc952));
    if (strlen(device_name_rfc952) == 0)
    {
        // cannot have a empty device name, reset to default...
        userConfig->set(cfg_deviceName, device_name);
        make_rfc952(device_name_rfc952, device_name, sizeof(device_name_rfc952));
    }
    else
    {
        // device name okay, copy it to our global
        strlcpy(device_name, userConfig->getDeviceName().c_str(), sizeof(device_name));
    }

    // Set rest of globals...
    led->setIdleState(userConfig->getLEDidle());
    motionTriggers.asInt = userConfig->getMotionTriggers();
    softAPmode = userConfig->getSoftAPmode();
    // Only enable NTP client if not in soft AP mode.
    enableNTP = !softAPmode && userConfig->getEnableNTP();
    syslogEn = userConfig->getSyslogEn();
    // Now log what we have loaded
    ESP_LOGI(TAG, "RFC952 compliant device hostname: %s", device_name_rfc952);
    ESP_LOGI(TAG, "User Configuration...");
    ESP_LOGI(TAG, "   deviceName:          %s", userConfig->getDeviceName().c_str());
    ESP_LOGI(TAG, "   wifiSettingsChanged: %s", userConfig->getWifiSettingsChanged() ? "true" : "false");
    ESP_LOGI(TAG, "   wifiPower:           %d", userConfig->getWifiPower());
    ESP_LOGI(TAG, "   wifiPhyMode:         %d", userConfig->getWifiPhyMode());
    ESP_LOGI(TAG, "   staticIP:            %s", userConfig->getStaticIP() ? "true" : "false");
    ESP_LOGI(TAG, "   localIP:             %s", userConfig->getLocalIP().c_str());
    ESP_LOGI(TAG, "   subnetMask:          %s", userConfig->getSubnetMask().c_str());
    ESP_LOGI(TAG, "   gatewayIP:           %s", userConfig->getGatewayIP().c_str());
    ESP_LOGI(TAG, "   nameserverIP:        %s", userConfig->getNameserverIP().c_str());
    ESP_LOGI(TAG, "   wwwPWrequired:       %s", userConfig->getPasswordRequired() ? "true" : "false");
    ESP_LOGI(TAG, "   wwwUsername:         %s", userConfig->getwwwUsername().c_str());
    ESP_LOGI(TAG, "   wwwCredentials:      %s", userConfig->getwwwCredentials().c_str());
    ESP_LOGI(TAG, "   GDOSecurityType:     %d", userConfig->getGDOSecurityType());
    ESP_LOGI(TAG, "   TTCseconds:          %d", userConfig->getTTCseconds());
    ESP_LOGI(TAG, "   rebootSeconds:       %d", userConfig->getRebootSeconds());
    ESP_LOGI(TAG, "   LEDidle:             %d", userConfig->getLEDidle());
    ESP_LOGI(TAG, "   motionTriggers:      %d", userConfig->getMotionTriggers());
    ESP_LOGI(TAG, "   enableNTP:           %s", userConfig->getEnableNTP() ? "true" : "false");
    ESP_LOGI(TAG, "   doorUpdateAt:        %d", userConfig->getDoorUpdateAt());
    ESP_LOGI(TAG, "   timeZone:            %s", userConfig->getTimeZone().c_str());
    ESP_LOGI(TAG, "   softAPmode:          %s", userConfig->getSoftAPmode() ? "true" : "false");
    ESP_LOGI(TAG, "   syslogEn:            %s", userConfig->getSyslogEn() ? "true" : "false");
    ESP_LOGI(TAG, "   syslogIP:            %s", userConfig->getSyslogIP().c_str());

    if (enableNTP)
    {
        std::string tz = userConfig->getTimeZone();
        size_t pos = tz.find(';');
        if (pos != std::string::npos)
        {
            // semicolon may separate continent/city from posix TZ string
            // if no semicolon then no POSIX code, so use UTC
            ESP_LOGI(TAG, "Set timezone: %s", tz.substr(pos + 1).c_str());
            setenv("TZ", tz.substr(pos + 1).c_str(), 1);
        }
        else
        {
            ESP_LOGI(TAG, "Set timezone: UTC0");
            setenv("TZ", "UTC0", 1);
        }
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
        config.sync_cb = time_is_set;
        esp_netif_sntp_init(&config);
        tzset();
        sntp_set_sync_interval(30 * 60 * 1000UL); // update every 30 minutes
    }
}

void sync_and_restart()
{
    if (softAPmode)
    {
        // reset so next reboot will be standard station mode
        userConfig->set(cfg_softAPmode, false);
        userConfig->save();
    }
    else
    {
        // In soft AP mode we never initialized garage door comms, so don't save rolling code.
        save_rolling_code();
    }
    saveMessageLog();
    esp_restart();
}
