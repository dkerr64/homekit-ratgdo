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
#include <fstream>
#include <format>

// ESP system includes
#include <esp_littlefs.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>

// RATGDO project includes
#include "config.h"
// #include "log.h"

// Logger tag
static const char *TAG = "ratgdo-config";

// Construct the singleton tasks for user config and NVRAM access
userSettings *userSettings::instancePtr = new userSettings();
userSettings *userConfig = userSettings::getInstance();
nvRamClass *nvRamClass::instancePtr = new nvRamClass();
nvRamClass *nvRam = nvRamClass::getInstance();

/****************************************************************************
 * User settings class
 */
userSettings::userSettings()
{
    ESP_LOGI(TAG, "Constructor for user settings");
    char name[32];
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(name, sizeof(name), "Garage Door %02X%02X%02X", mac[3], mac[4], mac[5]);
    // key, {reboot, wifiChanged, value, fn to call}
    settings = {
        {cfg_deviceName, {false, false, name, NULL}}, // call fn to set global
        {cfg_wifiSettingsChanged, {true, true, false, NULL}},
        {cfg_wifiPower, {true, true, 20, NULL}},  // call fn to set reboot only if setting changed
        {cfg_wifiPhyMode, {true, true, 0, NULL}}, // call fn to set reboot only if setting changed
        {cfg_staticIP, {true, true, false, NULL}},
        {cfg_localIP, {true, true, "0.0.0.0", NULL}},
        {cfg_subnetMask, {true, true, "0.0.0.0", NULL}},
        {cfg_gatewayIP, {true, true, "0.0.0.0", NULL}},
        {cfg_nameserverIP, {true, true, "0.0.0.0", NULL}},
        {cfg_passwordRequired, {false, false, false, NULL}},
        {cfg_wwwUsername, {false, false, "admin", NULL}},
        //  Credentials are MD5 Hash... server.credentialHash(username, realm, "password");
        {cfg_wwwCredentials, {false, false, "10d3c00fa1e09696601ef113b99f8a87", NULL}},
        {cfg_GDOSecurityType, {true, false, 2, NULL}}, // call fn to reset door
        {cfg_TTCseconds, {false, false, 0, NULL}},
        {cfg_rebootSeconds, {true, true, 0, NULL}},
        {cfg_LEDidle, {false, false, 0, NULL}},       // call fn to set LED object
        {cfg_motionTriggers, {true, false, 0, NULL}}, // call fn to set reboot only if required
        {cfg_enableNTP, {true, false, false, NULL}},
        {cfg_doorUpdateAt, {false, false, 0, NULL}},
        // Will contain string of region/city and POSIX code separated by semicolon...
        // For example... "America/New_York;EST5EDT,M3.2.0,M11.1.0"
        // Current maximum string length is known to be 60 chars (+ null terminator), see JavaScript console log.
        {cfg_timeZone, {false, false, "", NULL}}, // call fn to set system time zone
        {cfg_softAPmode, {true, false, false, NULL}},
        {cfg_syslogEn, {false, false, false, NULL}},
        {cfg_syslogIP, {false, false, "0.0.0.0", NULL}},
        {cfg_syslogPort, {false, false, 514, NULL}},
    };
    configFile = "/littlefs/user_config";
    ESP_LOGI(TAG, "Default config set for: %s", getDeviceName().c_str());
}

void userSettings::toStdOut()
{
    for (const auto &it : settings)
    {

        if (holds_alternative<std::string>(it.second.value))
        {
            std::printf("%s:\t%s\n", it.first.c_str(), std::get<std::string>(it.second.value).c_str());
        }
        else if (holds_alternative<int>(it.second.value))
        {
            std::printf("%s:\t%d\n", it.first.c_str(), std::get<int>(it.second.value));
        }
        else
        {
            std::printf("%s:\t%d\n", it.first.c_str(), std::get<bool>(it.second.value));
        }
    }
}

void userSettings::toFile(std::ofstream &stream)
{
    for (const auto &it : settings)
    {
        if (holds_alternative<std::string>(it.second.value))
        {
            stream << std::format("{};{}\n", it.first.c_str(), std::get<std::string>(it.second.value).c_str());
        }
        else if (holds_alternative<int>(it.second.value))
        {
            stream << std::format("{};{}\n", it.first.c_str(), std::get<int>(it.second.value));
        }
        else
        {
            stream << std::format("{};{}\n", it.first.c_str(), std::get<bool>(it.second.value));
        }
    }
}

void userSettings::save()
{
    ESP_LOGI(TAG, "Writing user configuration to file: %s", configFile.c_str());
    std::ofstream file(configFile);
    if (!file.is_open())
    {
        ESP_LOGW(TAG, "File open error");
        return;
    }
    toFile(file);
    file.close();
}

bool userSettings::load()
{
    ESP_LOGI(TAG, "Read user configuration from file: %s", configFile.c_str());
    std::ifstream file(configFile);

    if (!file.is_open())
    {
        ESP_LOGW(TAG, "File does not exist");
        return false;
    }

    std::string line;
    while (getline(file, line))
    {
        const char *key = line.c_str();
        // ESP_LOGI(TAG, "Read line from %s file: %s", configFile.c_str(), key);
        char *value = (char *)key;
        while (*value && (*value != ';'))
            value++;
        if (*value)
            *value++ = 0;
        set(key, (std::string)value);
    }

    file.close();
    return true;
}

std::variant<bool, int, std::string> userSettings::get(const std::string &key)
{
    return settings[key].value;
}

configSetting userSettings::getDetail(const std::string &key)
{
    return settings[key];
}

bool userSettings::set(const std::string &key, const bool value)
{
    if (!settings.count(key))
        return false; // invalid key
    if (holds_alternative<bool>(settings[key].value))
    {
        settings[key].value = value;
        return true;
    }
    return false;
}

bool userSettings::set(const std::string &key, const int value)
{
    if (!settings.count(key))
        return false; // invalid key
    if (holds_alternative<int>(settings[key].value))
    {
        settings[key].value = value;
        return true;
    }
    else if (holds_alternative<bool>(settings[key].value))
    {
        settings[key].value = (value != 0);
        return true;
    }
    return false;
}

bool userSettings::set(const std::string &key, const std::string &value)
{
    if (!settings.count(key))
        return false; // invalid key
    if (holds_alternative<std::string>(settings[key].value))
    {
        settings[key].value = value;
        return true;
    }
    else if (holds_alternative<bool>(settings[key].value))
    {
        settings[key].value = (value == "true") || (atoi(value.c_str()) != 0);
        return true;
    }
    else if (holds_alternative<int>(settings[key].value))
    {
        settings[key].value = stoi(value);
        return true;
    }
    return false;
}

bool userSettings::set(const std::string &key, const char *value)
{
    return set(key, std::string(value));
}

/****************************************************************************
 * NVRAM class
 */
nvRamClass::nvRamClass()
{
    ESP_LOGI(TAG, "Constructor for NVRAM class");
    // Initialize non volatile ram
    // We use this sparingly, most settings are saved in file system initialized below.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    err = nvs_open("ratgdo", NVS_READWRITE, &nvHandle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        nvHandle = 0;
    }
}

int32_t nvRamClass::read(const std::string &key, int32_t dflt)
{
    int32_t value = dflt;
    esp_err_t err = nvs_get_i32(nvHandle, key.c_str(), &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "NVRAM get error for: %s (%s)", key.c_str(), esp_err_to_name(err));
    }
    return value;
}

bool nvRamClass::write(const std::string &key, int32_t value)
{
    esp_err_t err = nvs_set_i32(nvHandle, key.c_str(), value);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVRAM set error for: %s (%s)", key.c_str(), esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
    return true;
}

bool nvRamClass::erase(const std::string &key)
{
    esp_err_t err = nvs_erase_key(nvHandle, key.c_str());
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVRAM erase error for: %s (%s)", key.c_str(), esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
    return true;
}
