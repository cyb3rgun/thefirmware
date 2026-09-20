# CYB3RGUN thefirmware: one project, several targets.
#
# The application to build is chosen with CGTARGET, which names a directory
# under targets/. That directory holds the main component for that firmware.
# Shared code lives under components/ and is available to every target.
#
#   idf.py -D CGTARGET=module -B build/module \
#          -D SDKCONFIG=sdkconfig.module \
#          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.module" \
#          build
#
# See docs/decisions.md D-002 and D-004.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CGTARGET)
    if(DEFINED ENV{CGTARGET})
        set(CGTARGET $ENV{CGTARGET})
    else()
        set(CGTARGET "module")
    endif()
endif()

if(NOT IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/targets/${CGTARGET}/main")
    message(FATAL_ERROR
        "Unknown CGTARGET '${CGTARGET}'. Expected a directory targets/${CGTARGET}/main.")
endif()

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "${CMAKE_CURRENT_LIST_DIR}/targets/${CGTARGET}"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyb3rgun-${CGTARGET})
