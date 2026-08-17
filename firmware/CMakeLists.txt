# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Firmware version reported over BLE ("VER=..." on command 3). Bump on release
# so an OTA update can be confirmed from the app.
set(PROJECT_VER "1.8.0")
project(FilterTrackv3)
