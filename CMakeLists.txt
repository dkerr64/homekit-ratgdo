# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{HOMEKIT_PATH})
  set(HOMEKIT_PATH $ENV{HOMEKIT_PATH})
else()
  set(HOMEKIT_PATH ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk)
endif(DEFINED ENV{HOMEKIT_PATH})

set(EXTRA_COMPONENT_DIRS ${HOMEKIT_PATH}/components
  ${HOMEKIT_PATH}/components/homekit
  ${HOMEKIT_PATH}/components/homekit/esp_hap_apple_profiles
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/lib/ratgdo
  ${CMAKE_CURRENT_LIST_DIR}/components/esp_littlefs
  ${CMAKE_CURRENT_LIST_DIR}/components/secplus
  )

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ratgdo)
