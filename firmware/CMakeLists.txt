cmake_minimum_required(VERSION 3.16)

# Build only the main component and what it requires.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32dmx)