cmake_minimum_required(VERSION 3.16)

# components/ は ESP-IDF が自動で拾う（純 C コア）。
# components_port/ が ESP-IDF 依存のポート層。
set(EXTRA_COMPONENT_DIRS "components_port")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyberdeck)
