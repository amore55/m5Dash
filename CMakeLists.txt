# Tab5 Desk Dashboard — top level ESP-IDF project
#
# Target: M5Stack Tab5 (ESP32-P4 application processor, ESP32-C6 radio coprocessor)
# Pinned toolchain: ESP-IDF v5.4.4  (CI is exact; the manifest accepts any 5.4.x)
#                   See docs/IMPLEMENTATION_PLAN.md §2.1 for why 5.4.4 and not 5.4.2.

cmake_minimum_required(VERSION 3.16)

# `plugins/` holds one ESP-IDF component per dashboard page. `components/` is picked up
# automatically by ESP-IDF; `plugins/` has to be declared.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/plugins")

# ---------------------------------------------------------------------------------------
# Version and git SHA.
#
# Precedence:
#   1. -DDASH_VERSION=... / -DDASH_GIT_SHA=...        (release workflow passes these)
#   2. $ENV{DASH_VERSION} / $ENV{DASH_GIT_SHA}        (CI convenience)
#   3. `git describe` / `git rev-parse`               (local development)
#   4. "0.0.0-dev" / "unknown"                        (tarball with no git)
#
# The version string is embedded in the firmware, shown on the boot screen and the Settings
# page, and compared against the OTA manifest. It must be a plain semantic version for a
# release build, because SemVer::parse() rejects anything else.
# ---------------------------------------------------------------------------------------
if(NOT DASH_VERSION AND DEFINED ENV{DASH_VERSION})
    set(DASH_VERSION "$ENV{DASH_VERSION}")
endif()
if(NOT DASH_GIT_SHA AND DEFINED ENV{DASH_GIT_SHA})
    set(DASH_GIT_SHA "$ENV{DASH_GIT_SHA}")
endif()

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
    if(NOT DASH_VERSION)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
            OUTPUT_VARIABLE _dash_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_dash_tag MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+.*)$")
            set(DASH_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(NOT DASH_GIT_SHA)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
            OUTPUT_VARIABLE DASH_GIT_SHA
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
endif()

if(NOT DASH_VERSION)
    set(DASH_VERSION "0.0.0-dev")
endif()
if(NOT DASH_GIT_SHA)
    set(DASH_GIT_SHA "unknown")
endif()

# ESP-IDF stamps this into the application descriptor, so `esptool`/`otatool` and
# `esp_app_get_description()` agree with what the UI shows.
set(PROJECT_VER "${DASH_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Must be set before project() so every component sees it.
idf_build_set_property(COMPILE_DEFINITIONS "DASH_APP_VERSION=\"${DASH_VERSION}\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "DASH_GIT_SHA=\"${DASH_GIT_SHA}\"" APPEND)

project(tab5-desk-dashboard)

message(STATUS "Tab5 Desk Dashboard version ${DASH_VERSION} (${DASH_GIT_SHA})")
