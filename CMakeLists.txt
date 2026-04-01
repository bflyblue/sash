# SPDX-License-Identifier: BSD-2-Clause
cmake_minimum_required(VERSION 3.10)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" SASH_BASE_VERSION)
string(STRIP "${SASH_BASE_VERSION}" SASH_BASE_VERSION)

project(sash VERSION ${SASH_BASE_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Feature detection
include(CheckFunctionExists)
include(CheckSymbolExists)

check_function_exists(getline HAVE_GETLINE)
check_function_exists(sigaction HAVE_SIGACTION)
check_symbol_exists(TIOCGWINSZ "sys/ioctl.h" HAVE_TIOCGWINSZ)

if(NOT HAVE_GETLINE)
    message(FATAL_ERROR "getline() is required (POSIX.1-2008)")
endif()
if(NOT HAVE_SIGACTION)
    message(FATAL_ERROR "sigaction() is required (POSIX)")
endif()
if(NOT HAVE_TIOCGWINSZ)
    message(WARNING "TIOCGWINSZ not found - terminal size detection may not work")
endif()

# Version: base from VERSION file, with git suffix for dev builds.
set(SASH_VERSION "${PROJECT_VERSION}")
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if(NOT GIT_TAG_RESULT EQUAL 0)
    # Not on an exact tag — append git info
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_SHORT_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_REV_RESULT
    )
    if(GIT_REV_RESULT EQUAL 0)
        set(SASH_VERSION "${SASH_VERSION}-${GIT_SHORT_REV}")
        execute_process(
            COMMAND git diff --quiet HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE GIT_DIRTY_RESULT
        )
        if(NOT GIT_DIRTY_RESULT EQUAL 0)
            set(SASH_VERSION "${SASH_VERSION}-dirty")
        endif()
    endif()
endif()
add_compile_definitions(SASH_VERSION="${SASH_VERSION}")

# Build
add_executable(sash sash.c ringbuf.c display.c process.c)

# Install
install(TARGETS sash DESTINATION bin)

enable_testing()
add_test(NAME integration_tests
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_tests.sh)
set_tests_properties(integration_tests PROPERTIES
    ENVIRONMENT "SASH_BIN=${CMAKE_BINARY_DIR}/sash"
)

add_executable(test_sanitize tests/test_sanitize.c)
add_test(NAME test_sanitize COMMAND test_sanitize)

add_executable(test_ringbuf tests/test_ringbuf.c)
add_test(NAME test_ringbuf COMMAND test_ringbuf)
