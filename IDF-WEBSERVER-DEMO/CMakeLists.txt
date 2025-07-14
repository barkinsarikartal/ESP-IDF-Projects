cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{IDF_PATH}/components/spiffs/project_include.cmake)

project(IDF-WEBSERVER-DEMO)

spiffs_create_partition_image(
    spiffs  # partitions.csv'deki etiket
    ${CMAKE_CURRENT_SOURCE_DIR}/spiffs_image  # kaynak klasör
    FLASH_IN_PROJECT
)