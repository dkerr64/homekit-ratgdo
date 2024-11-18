idf_component_register(
    SRCS "softuart.cpp" "comms.cpp" "log.cpp" "MD5Builder.cpp" "web.cpp" "utilities.cpp" "homekit.cpp" "config.cpp" "ratgdo.cpp" "wifi.cpp" "led.cpp"
    INCLUDE_DIRS "." "./include"
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/esp_littlefs/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/secplus/secplus/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/ratgdo
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/esp-homekit-sdk/components/homekit/esp_hap_apple_profiles/include
    REQUIRES driver esp_wifi wifi_provisioning nvs_flash freertos esp_hap_core esp_hap_platform esp_http_server lwip
)

add_custom_command(
    COMMENT "Creating web content header file"
    OUTPUT "www/build/webcontent.h"
    COMMAND python3 build_web_content.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_custom_target(
    run ALL
    DEPENDS "www/build/webcontent.h"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -DAUTO_VERSION=\"3.0.0\" -DNTP_CLIENT -DLOG_MSG_BUFFER)
