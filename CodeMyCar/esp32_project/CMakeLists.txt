# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=12 HEAD
    OUTPUT_VARIABLE CMCAR_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT CMCAR_GIT_SHA)
    set(CMCAR_GIT_SHA "unknown")
endif()
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} status --porcelain --untracked-files=no -- .
    OUTPUT_VARIABLE CMCAR_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(CMCAR_GIT_STATUS)
    set(PROJECT_VER "${CMCAR_GIT_SHA}-dirty")
else()
    set(PROJECT_VER "${CMCAR_GIT_SHA}")
endif()

set(EXTRA_COMPONENT_DIRS components) 
project(esp32_project)
