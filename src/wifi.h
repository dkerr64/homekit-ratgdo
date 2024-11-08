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
#pragma once

// C/C++ language includes
#include <string>
#include <set>

// ESP system includes
// none

// RATGDO project includes
// none

//void improv_loop();

//void wifi_connect();

//void wifi_scan();

//bool connect_wifi(const std::string& ssid, const std::string& password, const uint8_t *bssid);
//bool connect_wifi(const std::string& ssid, const std::string& password);

void wifi_task_entry(void* ctx);

struct wifiNet_t
{
    std::string ssid;
    int32_t rssi;
    int32_t channel;
    uint8_t bssid[6];
};
extern std::multiset<wifiNet_t, bool (*)(wifiNet_t, wifiNet_t)> wifiNets;
//extern station_config wifiConf;

enum class WifiStatus {
    Disconnected,
    Pending,
    Connected,
};

extern char macAddress[];
