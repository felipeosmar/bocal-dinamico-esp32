cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bocal-dinamico)

# Create SPIFFS partition image for web interface (www partition)
# This is flashed with the app - safe to update
spiffs_create_partition_image(www ${CMAKE_CURRENT_SOURCE_DIR}/main/www FLASH_IN_PROJECT)

# Note: 'userdata' partition is NOT included here intentionally
# It will be auto-formatted on first boot and preserves user config across updates
