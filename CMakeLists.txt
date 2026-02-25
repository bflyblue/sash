# SPDX-License-Identifier: BSD-2-Clause
cmake_minimum_required(VERSION 3.10)
project(sash VERSION 0.1.0 LANGUAGES C)

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
